// src/mixing/BassBridge.cpp
#include "mixing/BassBridge.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "mixing/Dsp.h"
#include "util/Logging.h"
#include "util/Math.h"

namespace sa {

std::atomic<BassBridge*> BassBridge::s_instance{nullptr};

namespace {

std::atomic<uint32_t> g_bassReaders{0};

struct BassInvocation {
    BassInvocation() { g_bassReaders.fetch_add(1, std::memory_order_seq_cst); }
    ~BassInvocation() { g_bassReaders.fetch_sub(1, std::memory_order_seq_cst); }
};

// Lowest possible priority => our DSP runs after every DSP GMod installs.
constexpr int32_t kDspPriority = INT_MIN + 1;

const char* ActiveStateName(bass::DWORD state)
{
    switch (state) {
    case bass::kActiveStopped:
        return "stopped";
    case bass::kActivePlaying:
        return "playing";
    case bass::kActiveStalled:
        return "stalled";
    case bass::kActivePaused:
        return "paused";
    default:
        return "?";
    }
}

} // namespace

BassBridge::BassBridge() = default;

BassBridge::~BassBridge()
{
    Shutdown();
}

bool BassBridge::Initialize(uint32_t engineSampleRate, const BassBridgeOptions& options, ProceduralSourceHooks hooks,
                            std::string& error)
{
    Shutdown();
    if (s_instance) {
        error = "BassBridge already active";
        return false;
    }
    m_engineSampleRate = engineSampleRate ? engineSampleRate : 44100;
    m_options = options;
    m_hooks = std::move(hooks);
    m_initialized = true;
    s_instance = this;
    if (!m_hooks.allocateId || !m_hooks.registerSource || !m_hooks.unregisterSource) {
        error = "BassBridge: missing source hooks";
        return false;
    }
    if (!Detour::Supported()) {
        error = "BassBridge: function detours unavailable on this platform";
        return false;
    }
    return TryAttach(error);
}

void BassBridge::Shutdown()
{
    if (!m_initialized)
        return;
    RemoveDetours();
    std::vector<ChannelPtr> channels;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& kv : m_channels)
            channels.push_back(kv.second);
        m_channels.clear();
        for (auto& ch : m_pendingUnregister)
            channels.push_back(ch);
        m_pendingUnregister.clear();
    }
    for (const ChannelPtr& ch : channels)
        RetireChannel(*ch, false);
    m_attached.store(false, std::memory_order_release);
    m_stats.channels.store(0, std::memory_order_relaxed);
    m_stats.spatialChannels.store(0, std::memory_order_relaxed);
    m_initialized = false;
    if (s_instance == this)
        s_instance = nullptr;
}

bool BassBridge::TryAttach(std::string& error)
{
    if (!m_initialized)
        return false;
    if (m_attached.load(std::memory_order_acquire))
        return true;
    if (!IsModuleLoaded(m_options.moduleName.c_str())) {
        error = m_options.moduleName + " is not loaded";
        return false;
    }
    if (!ResolveApi(error))
        return false;
    s_instance.store(this, std::memory_order_seq_cst);
    if (!InstallDetours(error))
        return false;
    m_attached.store(true, std::memory_order_release);
    SA_LOGI("BASS bridge attached to %s (BASS %u.%u.%u.%u, %zu detours)", m_options.moduleName.c_str(),
            (m_bassVersion >> 24) & 0xff, (m_bassVersion >> 16) & 0xff, (m_bassVersion >> 8) & 0xff,
            m_bassVersion & 0xff, m_detours.size());
    return true;
}

bool BassBridge::ResolveApi(std::string& error)
{
    const char* module = m_options.moduleName.c_str();
    auto resolve = [&](const char* name, auto& slot) {
        using Fn = std::remove_reference_t<decltype(slot)>;
        slot = reinterpret_cast<Fn>(FindLoadedExport(module, name));
        if (!slot) {
            if (error.empty())
                error = std::string("missing BASS export: ") + name;
            return false;
        }
        return true;
    };
    error.clear();
    bool ok = true;
    ok &= resolve("BASS_StreamCreateFile", m_api.streamCreateFile);
    ok &= resolve("BASS_StreamCreateURL", m_api.streamCreateURL);
    ok &= resolve("BASS_StreamCreateFileUser", m_api.streamCreateFileUser);
    ok &= resolve("BASS_StreamCreate", m_api.streamCreate);
    ok &= resolve("BASS_StreamFree", m_api.streamFree);
    ok &= resolve("BASS_ChannelSetDSP", m_api.channelSetDSP);
    ok &= resolve("BASS_ChannelRemoveDSP", m_api.channelRemoveDSP);
    ok &= resolve("BASS_ChannelGetInfo", m_api.channelGetInfo);
    ok &= resolve("BASS_ChannelIsActive", m_api.channelIsActive);
    ok &= resolve("BASS_ChannelPlay", m_api.channelPlay);
    ok &= resolve("BASS_ChannelSet3DPosition", m_api.channelSet3DPosition);
    ok &= resolve("BASS_ChannelSet3DAttributes", m_api.channelSet3DAttributes);
    ok &= resolve("BASS_ChannelSetAttribute", m_api.channelSetAttribute);
    ok &= resolve("BASS_ChannelGetAttribute", m_api.channelGetAttribute);
    ok &= resolve("BASS_ErrorGetCode", m_api.errorGetCode);
    ok &= resolve("BASS_GetVersion", m_api.getVersion);
    if (!ok)
        return false;
    m_bassVersion = m_api.getVersion();
    if ((m_bassVersion & 0xffff0000u) != bass::kVersion24) {
        error = "unexpected BASS version 0x" + std::to_string(m_bassVersion);
        SA_LOGW("BASS version 0x%08x is not 2.4.x; bridge disabled", m_bassVersion);
        return false;
    }
    return true;
}

bool BassBridge::InstallDetours(std::string& error)
{
    m_detours.clear();
    m_detours.reserve(9);
    auto install = [&](void* target, void* detour, auto& original, const char* name) {
        Detour d;
        void* orig = nullptr;
        if (!d.Install(target, detour, &orig, name, error, false))
            return false;
        original = reinterpret_cast<std::remove_reference_t<decltype(original)>>(orig);
        if (!d.Enable(error))
            return false;
        m_detours.push_back(std::move(d));
        return true;
    };
    const bool ok =
        install(reinterpret_cast<void*>(m_api.streamCreateFile), reinterpret_cast<void*>(&HookStreamCreateFile),
                m_orig.streamCreateFile, "BASS_StreamCreateFile") &&
        install(reinterpret_cast<void*>(m_api.streamCreateURL), reinterpret_cast<void*>(&HookStreamCreateURL),
                m_orig.streamCreateURL, "BASS_StreamCreateURL") &&
        install(reinterpret_cast<void*>(m_api.streamCreateFileUser),
                reinterpret_cast<void*>(&HookStreamCreateFileUser), m_orig.streamCreateFileUser,
                "BASS_StreamCreateFileUser") &&
        install(reinterpret_cast<void*>(m_api.streamCreate), reinterpret_cast<void*>(&HookStreamCreate),
                m_orig.streamCreate, "BASS_StreamCreate") &&
        install(reinterpret_cast<void*>(m_api.streamFree), reinterpret_cast<void*>(&HookStreamFree),
                m_orig.streamFree, "BASS_StreamFree") &&
        install(reinterpret_cast<void*>(m_api.channelPlay), reinterpret_cast<void*>(&HookChannelPlay),
                m_orig.channelPlay, "BASS_ChannelPlay") &&
        install(reinterpret_cast<void*>(m_api.channelSet3DPosition),
                reinterpret_cast<void*>(&HookChannelSet3DPosition), m_orig.channelSet3DPosition,
                "BASS_ChannelSet3DPosition") &&
        install(reinterpret_cast<void*>(m_api.channelSet3DAttributes),
                reinterpret_cast<void*>(&HookChannelSet3DAttributes), m_orig.channelSet3DAttributes,
                "BASS_ChannelSet3DAttributes") &&
        install(reinterpret_cast<void*>(m_api.channelSetAttribute),
                reinterpret_cast<void*>(&HookChannelSetAttribute), m_orig.channelSetAttribute,
                "BASS_ChannelSetAttribute");
    if (!ok) {
        RemoveDetours();
        return false;
    }
    m_stats.hooksInstalled.store(static_cast<uint32_t>(m_detours.size()), std::memory_order_relaxed);
    return true;
}

void BassBridge::RemoveDetours()
{
    for (Detour& detour : m_detours)
        detour.Disable();
    s_instance.store(nullptr, std::memory_order_seq_cst);
    while (g_bassReaders.load(std::memory_order_seq_cst) != 0)
        std::this_thread::yield();
    m_detours.clear(); // Detour dtor disables + removes
    m_orig = Originals{};
    m_stats.hooksInstalled.store(0, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Channel table
// ---------------------------------------------------------------------------
BassBridge::ChannelPtr BassBridge::FindChannel(bass::HSTREAM handle) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_channels.find(handle);
    return it == m_channels.end() ? nullptr : it->second;
}

SoundSourcePtr BassBridge::FindSource(bass::HSTREAM handle) const
{
    ChannelPtr ch = FindChannel(handle);
    if (!ch)
        return nullptr;
    std::lock_guard<std::mutex> lock(ch->lock);
    return ch->source;
}

size_t BassBridge::ChannelCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_channels.size();
}

bool BassBridge::SetSourceOverrides(bass::HSTREAM handle, const SourceParams& params)
{
    ChannelPtr ch = FindChannel(handle);
    if (!ch)
        return false;
    std::lock_guard<std::mutex> lock(ch->lock);
    // Keep the BASS-derived fields authoritative; take the policy flags.
    ch->params.occlusion = params.occlusion;
    ch->params.transmission = params.transmission;
    ch->params.reflections = params.reflections;
    ch->params.pathing = params.pathing;
    ch->params.reverbGain = params.reverbGain;
    ch->params.airAbsorptionScale = params.airAbsorptionScale;
    ch->params.radiusMeters = params.radiusMeters;
    ch->params.dipoleWeight = params.dipoleWeight;
    ch->params.dipolePower = params.dipolePower;
    ch->params.entityIndex = params.entityIndex;
    ch->paramsDirty = true;
    return true;
}

std::vector<std::string> BassBridge::DescribeChannels() const
{
    std::vector<ChannelPtr> channels;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& entry : m_channels)
            channels.push_back(entry.second);
    }
    std::vector<std::string> lines;
    char text[768];
    std::snprintf(text, sizeof(text), "BASS streams: %zu; captured %llu frames, dropped %llu, skipped %llu blocks",
                  channels.size(), static_cast<unsigned long long>(m_stats.capturedFrames.load()),
                  static_cast<unsigned long long>(m_stats.droppedFrames.load()),
                  static_cast<unsigned long long>(m_stats.skippedBlocks.load()));
    lines.emplace_back(text);
    const auto db = [](float energy) { return 10.0 * std::log10(std::max(1e-12, static_cast<double>(energy))); };
    for (const auto& channel : channels) {
        const bass::DWORD state = m_api.channelIsActive ? m_api.channelIsActive(channel->handle) : bass::kActiveStopped;
        std::lock_guard<std::mutex> lock(channel->lock);
        if (!channel->source)
            continue;
        const SourceParams p = channel->source->GetParams();
        const RenderDiag d = channel->source->LoadRenderDiag();
        std::snprintf(text, sizeof(text),
            "  handle %u source %u %s %s registered=%d; raw BASS (%.0f %.0f %.0f) -> Source (%.0f %.0f %.0f); "
            "%u ch @%u Hz queued %.1f ms; gain %.3f dist %.1f m att %.3f occl %.3f; "
            "captured %.1f dB input %.1f dB output %.1f dB",
            channel->handle, channel->source->Id(), ActiveStateName(state), p.spatialize ? "3d" : "2d",
            channel->registered ? 1 : 0, static_cast<double>(channel->bassPos.x), static_cast<double>(channel->bassPos.y),
            static_cast<double>(channel->bassPos.z), static_cast<double>(p.position.x), static_cast<double>(p.position.y),
            static_cast<double>(p.position.z), channel->info.chans, channel->info.freq,
            1000.0 * channel->source->QueuedStreamFrames() / std::max(1u, channel->info.freq),
            static_cast<double>(p.gain), static_cast<double>(d.distanceMeters), static_cast<double>(d.distanceAttenuation),
            static_cast<double>(d.occlusion), db(channel->capturedLevel.load()), db(d.inputLevel), db(d.outputLevel));
        lines.emplace_back(text);
    }
    return lines;
}

SoundSourcePtr BassBridge::MakeSource(Channel& ch)
{
    const uint32_t id = m_hooks.allocateId();
    if (id == 0)
        return nullptr;
    const uint32_t channels = Clamp<uint32_t>(ch.info.chans, 1, static_cast<uint32_t>(SoundSource::kMaxInputChannels));
    const uint32_t rate = Clamp<uint32_t>(ch.info.freq, 8000, 192000);
    const size_t capacity =
        NextPowerOfTwo(static_cast<size_t>(static_cast<float>(rate) * Clamp(m_options.ringSeconds, 0.1f, 5.f)));
    auto source =
        std::make_shared<SoundSource>(id, SourceKind::BassStream, channels, m_engineSampleRate, rate, capacity);
    source->SetName(ch.name);
    return source;
}

void BassBridge::OnStreamCreated(bass::HSTREAM handle, bass::DWORD flags, const char* origin)
{
    if (!handle || (flags & bass::kStreamDecode))
        return; // decode-only streams never reach the BASS mixer (GMod uses them for FFT-only channels)
    bass::ChannelInfo info{};
    if (!m_api.channelGetInfo(handle, &info)) {
        SA_LOGW("BASS_ChannelGetInfo failed for new stream %u", handle);
        return;
    }
    auto ch = std::make_shared<Channel>();
    ch->handle = handle;
    ch->info = info;
    ch->isFloat = (info.flags & bass::kSampleFloat) != 0;
    ch->is8Bit = !ch->isFloat && (info.flags & bass::kSample8Bits) != 0;
    ch->bytesPerSample = ch->isFloat ? 4 : (ch->is8Bit ? 1 : 2);
    ch->is3D = (info.flags & bass::kSample3D) != 0;
    ch->name = std::string("bass:") + origin + "#" + std::to_string(handle);
    if (info.filename && info.filename[0])
        ch->name += std::string(" ") + info.filename;
    ch->scratch.resize(8192 * 2);

    SourceParams p;
    p.channel = kChanStatic;
    p.entityIndex = -1;
    p.spatialize = ch->is3D ? 1 : 0;
    p.positionValid = 0;
    p.distMult = SoundLevelToDistMult(m_options.defaultSoundLevelDb);
    p.fromServer = 0;
    ch->params = p;
    ch->source = MakeSource(*ch);
    if (!ch->source)
        return;
    ch->source->SetParams(p);

    // Attach the capture DSP before the table insert so the first block is
    // never processed without a channel record.
    ch->dsp = m_api.channelSetDSP(handle, &BassBridge::DspCallback, ch.get(), kDspPriority);
    if (!ch->dsp) {
        SA_LOGW("BASS_ChannelSetDSP failed for stream %u (error %d)", handle, m_api.errorGetCode());
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto existing = m_channels.find(handle);
        if (existing != m_channels.end()) {
            // Handle reuse without an observed free: retire the stale record.
            m_pendingUnregister.push_back(existing->second);
            m_channels.erase(existing);
        }
        m_channels.emplace(handle, ch);
        m_stats.channels.store(static_cast<uint32_t>(m_channels.size()), std::memory_order_relaxed);
    }
    SA_LOGD("BASS stream %u captured: %u Hz, %u ch, %s%s (%s)", handle, info.freq, info.chans,
            ch->isFloat ? "float" : (ch->is8Bit ? "8-bit" : "16-bit"), ch->is3D ? ", 3D" : "", ch->name.c_str());
}

void BassBridge::OnStreamFreed(bass::HSTREAM handle)
{
    const bool interrupted = m_api.channelIsActive(handle) != bass::kActiveStopped;
    ChannelPtr ch;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_channels.find(handle);
        if (it == m_channels.end())
            return;
        ch = it->second;
        m_channels.erase(it);
        m_stats.channels.store(static_cast<uint32_t>(m_channels.size()), std::memory_order_relaxed);
        // Detach the DSP while the handle is still valid; the source itself
        // is retired on the game thread (SPSC queues are game-thread only).
        if (ch->dsp && m_api.channelRemoveDSP) {
            m_api.channelRemoveDSP(handle, ch->dsp);
            ch->dsp = 0;
        }
        m_pendingUnregister.push_back(ch);
    }
    std::lock_guard<std::mutex> lock(ch->lock);
    if (ch->source) {
        ch->source->SetEndOfStream();
        if (interrupted)
            ch->source->RequestStop();
    }
}

void BassBridge::OnChannelPlay(bass::HSTREAM handle, bool restart)
{
    ChannelPtr ch = FindChannel(handle);
    if (!ch)
        return;
    std::lock_guard<std::mutex> lock(ch->lock);
    const bool dead = !ch->source || ch->source->EndOfStream() || ch->source->RenderFinished() ||
                      ch->source->StopRequested();
    if (!dead && !restart)
        return;
    if (!dead && restart) {
        // Same stream restarted from the beginning: drop what is queued.
        ch->source->RequestStop();
    }
    SoundSourcePtr fresh = MakeSource(*ch);
    if (!fresh)
        return;
    fresh->SetParams(ch->params);
    if (ch->source && ch->registered) {
        std::lock_guard<std::mutex> tableLock(m_mutex);
        auto stale = std::make_shared<Channel>();
        stale->source = ch->source;
        stale->registered = true;
        m_pendingUnregister.push_back(stale);
    }
    ch->source = std::move(fresh);
    ch->registered = false;
    ch->registrationRejected = false;
    ch->paramsDirty = true;
}

Vec3 BassBridge::ToSourcePosition(const bass::Vector3D& v) const
{
    return m_options.ToSource(v);
}

void BassBridge::OnPosition(bass::HSTREAM handle, const bass::Vector3D* pos, const bass::Vector3D* orient)
{
    ChannelPtr ch = FindChannel(handle);
    if (!ch)
        return;
    std::lock_guard<std::mutex> lock(ch->lock);
    if (pos) {
        ch->bassPos = *pos;
        ch->params.position = ToSourcePosition(*pos);
        ch->params.positionValid = 1;
    }
    if (orient) {
        ch->bassOrient = *orient;
        Vec3 fwd = ToSourcePosition(*orient);
        if (fwd.LengthSq() > 1e-6f)
            ch->params.forward = fwd.Normalized();
    }
    ch->paramsDirty = true;
}

void BassBridge::On3DAttributes(bass::HSTREAM handle, int32_t mode, float minDist, float maxDist, int32_t inAngle,
                                int32_t outAngle, float outVolume)
{
    ChannelPtr ch = FindChannel(handle);
    if (!ch)
        return;
    std::lock_guard<std::mutex> lock(ch->lock);
    (void)maxDist;
    if (mode == bass::k3DModeOff) {
        ch->params.spatialize = 0;
    } else if (mode == bass::k3DModeNormal || mode == bass::k3DModeRelative) {
        ch->params.spatialize = ch->is3D ? 1 : 0;
    }
    // BASS inverse-distance rolloff: gain = min / dist for dist > min. Source's
    // model is gain = 1 / (dist * distMult), so distMult = 1 / min.
    if (minDist > 1.f) {
        ch->params.distMult = 1.f / minDist;
        ch->fadeSet = true;
    } else if (minDist >= 0.f && !ch->fadeSet) {
        ch->params.distMult = SoundLevelToDistMult(m_options.defaultSoundLevelDb);
    }
    if (inAngle >= 0 && outAngle >= 0 && outAngle >= inAngle && outAngle < 360) {
        // Cone => dipole approximation: narrower cones and quieter backsides
        // increase the dipole weight; a 360-degree cone is omnidirectional.
        const float coneFrac = static_cast<float>(outAngle) / 360.f;
        const float backAtten = Clamp01(outVolume < 0.f ? 1.f : outVolume);
        ch->params.dipoleWeight = Clamp01((1.f - coneFrac) * (1.f - backAtten));
        ch->params.dipolePower = 1.f + 3.f * (1.f - static_cast<float>(std::max(inAngle, 1)) / 360.f);
    }
    ch->paramsDirty = true;
}

void BassBridge::OnVolume(bass::HSTREAM handle, float volume)
{
    ChannelPtr ch = FindChannel(handle);
    if (!ch)
        return;
    std::lock_guard<std::mutex> lock(ch->lock);
    ch->volume = std::max(0.f, volume);
    ch->params.gain = ch->volume;
    ch->paramsDirty = true;
}

void BassBridge::RetireChannel(Channel& ch, bool eraseFromTable)
{
    if (eraseFromTable) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_channels.find(ch.handle);
        if (it != m_channels.end() && it->second.get() == &ch)
            m_channels.erase(it);
        m_stats.channels.store(static_cast<uint32_t>(m_channels.size()), std::memory_order_relaxed);
    }
    if (ch.dsp && m_api.channelRemoveDSP) {
        m_api.channelRemoveDSP(ch.handle, ch.dsp);
        ch.dsp = 0;
    }
    SoundSourcePtr source;
    bool registered = false;
    {
        std::lock_guard<std::mutex> lock(ch.lock);
        source = std::move(ch.source);
        registered = ch.registered;
        ch.registered = false;
    }
    if (source) {
        source->SetEndOfStream();
        if (!eraseFromTable) {
            source->RequestStop();
            if (registered && m_hooks.unregisterSource)
                m_hooks.unregisterSource(source);
        }
    }
}

void BassBridge::PublishParams(Channel& ch, const RuntimeConfig& cfg)
{
    SourceParams p = ch.params;
    if (!cfg.bassSpatial || !ch.is3D)
        p.spatialize = 0;
    if (p.spatialize && !p.positionValid)
        p.spatialize = 0; // no position yet => 2D until SetPos is called
    ch.source->SetParams(p);
    ch.paramsDirty = false;
}

// ---------------------------------------------------------------------------
// Game thread tick
// ---------------------------------------------------------------------------
void BassBridge::Tick(const RuntimeConfig& cfg)
{
    if (!m_initialized)
        return;
    m_unitsPerMeter = cfg.unitsPerMeter;
    if (!m_attached.load(std::memory_order_acquire)) {
        std::string error;
        TryAttach(error);
        return;
    }

    // 1. Sources whose channel died.
    std::vector<ChannelPtr> dead;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        dead.swap(m_pendingUnregister);
    }
    for (const ChannelPtr& ch : dead) {
        SoundSourcePtr source;
        bool registered = false;
        {
            std::lock_guard<std::mutex> lock(ch->lock);
            if (ch->source && !ch->registered && !ch->registrationRejected)
                PublishParams(*ch, cfg);
            source = std::move(ch->source);
            registered = ch->registered;
            ch->registered = false;
        }
        if (source) {
            source->SetEndOfStream();
            if (registered && source->StopRequested())
                m_hooks.unregisterSource(source);
            else if (!registered && !source->StopRequested() && !ch->registrationRejected && source->QueuedStreamFrames() > 0)
                m_hooks.registerSource(source);
        }
    }

    // 2. Live channels.
    std::vector<ChannelPtr> channels;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        channels.reserve(m_channels.size());
        for (auto& kv : m_channels)
            channels.push_back(kv.second);
    }
    uint32_t spatial = 0;
    for (const ChannelPtr& chp : channels) {
        Channel& ch = *chp;
        const bass::DWORD state = m_api.channelIsActive(ch.handle);
        if (state == bass::kActiveStopped && m_api.errorGetCode() == bass::kErrorHandle) {
            SA_LOGD("BASS stream %u vanished without BASS_StreamFree; retiring", ch.handle);
            RetireChannel(ch, true);
            continue;
        }

        float freq = 0.f;
        const bool haveFrequency = m_api.channelGetAttribute(ch.handle, bass::kAttribFreq, &freq) && freq > 0.f;
        std::unique_lock<std::mutex> lock(ch.lock);
        if (!ch.source || ch.registrationRejected)
            continue;
        if (state == bass::kActiveStopped) {
            // Finished or Stop()ped: let the ring drain, then the audio thread finishes it.
            ch.source->SetEndOfStream();
        }
        if (haveFrequency && ch.info.freq > 0)
            ch.source->SetStreamRateScale(freq / static_cast<float>(ch.info.freq));
        PublishParams(ch, cfg);
        if (!ch.registered) {
            if (!m_hooks.registerSource(ch.source)) {
                ch.registrationRejected = true;
                SA_LOGW("BASS stream %u: source registration refused; retaining BASS playback", ch.handle);
                continue;
            }
            ch.registered = true;
        }
        if (ch.source->GetParams().spatialize)
            ++spatial;
        (void)ActiveStateName(state);
    }
    m_stats.spatialChannels.store(spatial, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// BASS DSP thread
// ---------------------------------------------------------------------------
void SA_BASSCALL BassBridge::DspCallback(bass::HDSP handle, bass::DWORD channel, void* buffer, bass::DWORD length,
                                         void* user)
{
    (void)handle;
    (void)channel;
    BassInvocation invocation;
    BassBridge* self = s_instance.load(std::memory_order_seq_cst);
    Channel* ch = static_cast<Channel*>(user);
    if (!self || !ch || !buffer || length == 0)
        return;
    self->ProcessBlock(*ch, buffer, length);
}

void BassBridge::ProcessBlock(Channel& ch, void* buffer, bass::DWORD lengthBytes)
{
    if (!m_capture.load(std::memory_order_acquire))
        return; // passthrough: BASS plays normally
    const auto mute = [&] {
        if (m_options.muteBassOutput)
            std::memset(buffer, 0, static_cast<size_t>(lengthBytes));
    };
    std::unique_lock<std::mutex> lock(ch.lock, std::try_to_lock);
    if (!lock.owns_lock()) {
        m_stats.skippedBlocks.fetch_add(1, std::memory_order_relaxed);
        mute();
        return;
    }
    if (ch.registrationRejected)
        return;
    SoundSource* source = ch.source.get();
    if (!source || source->RenderFinished() || source->EndOfStream()) {
        mute();
        return;
    }
    const uint32_t chans = std::max<uint32_t>(1, ch.info.chans);
    const size_t frameBytes = static_cast<size_t>(ch.bytesPerSample) * chans;
    const size_t frames = lengthBytes / frameBytes;
    if (frames == 0)
        return;
    if (source && !source->EndOfStream()) {
        const uint32_t outChans = source->InputChannels();
        const size_t needed = frames * outChans;
        if (ch.scratch.size() < needed)
            ch.scratch.resize(needed); // rare: BASS grew its block size (not on the engine audio thread)
        float* out = ch.scratch.data();

        // Convert to float, folding channels beyond stereo into L/R.
        for (size_t f = 0; f < frames; ++f) {
            float l = 0.f, r = 0.f;
            for (uint32_t c = 0; c < chans; ++c) {
                float s;
                const size_t idx = f * chans + c;
                if (ch.isFloat)
                    s = static_cast<const float*>(buffer)[idx];
                else if (ch.is8Bit)
                    s = static_cast<float>(static_cast<const uint8_t*>(buffer)[idx] - 128) * (1.f / 128.f);
                else
                    s = static_cast<float>(static_cast<const int16_t*>(buffer)[idx]) * (1.f / 32768.f);
                if (chans == 1) {
                    l = s;
                    r = s;
                } else if ((c & 1u) == 0) {
                    l += s;
                } else {
                    r += s;
                }
            }
            if (chans > 2) {
                const float norm = 2.f / static_cast<float>(chans);
                l *= norm;
                r *= norm;
            }
            if (outChans == 1) {
                out[f] = chans == 1 ? l : 0.5f * (l + r);
            } else {
                out[f * 2] = l;
                out[f * 2 + 1] = r;
            }
        }
        double energy = 0.0;
        for (size_t i = 0; i < needed; ++i)
            energy += static_cast<double>(out[i]) * out[i];
        const float previous = ch.capturedLevel.load(std::memory_order_relaxed);
        ch.capturedLevel.store(previous + 0.25f * (static_cast<float>(energy / needed) - previous), std::memory_order_relaxed);
        const size_t written = source->WriteInterleaved(out, frames);
        m_stats.capturedFrames.fetch_add(written, std::memory_order_relaxed);
        if (written < frames)
            m_stats.droppedFrames.fetch_add(frames - written, std::memory_order_relaxed);
        source->TouchActivity(ch.lastBlockStamp.fetch_add(1, std::memory_order_relaxed));
    }
    mute();
}

// ---------------------------------------------------------------------------
// Detours
// ---------------------------------------------------------------------------
bass::HSTREAM SA_BASSCALL BassBridge::HookStreamCreateFile(bass::BOOL mem, const void* file, bass::QWORD offset,
                                                           bass::QWORD length, bass::DWORD flags)
{
    BassInvocation invocation;
    BassBridge* self = s_instance.load(std::memory_order_seq_cst);
    if (!self)
        return 0;
    const bass::HSTREAM h = self->m_orig.streamCreateFile(mem, file, offset, length, flags);
    if (h)
        self->OnStreamCreated(h, flags, "file");
    return h;
}

bass::HSTREAM SA_BASSCALL BassBridge::HookStreamCreateURL(const char* url, bass::DWORD offset, bass::DWORD flags,
                                                          bass::DownloadProc proc, void* user)
{
    BassInvocation invocation;
    BassBridge* self = s_instance.load(std::memory_order_seq_cst);
    if (!self)
        return 0;
    const bass::HSTREAM h = self->m_orig.streamCreateURL(url, offset, flags, proc, user);
    if (h)
        self->OnStreamCreated(h, flags, "url");
    return h;
}

bass::HSTREAM SA_BASSCALL BassBridge::HookStreamCreateFileUser(bass::DWORD system, bass::DWORD flags,
                                                               const bass::FileProcs* procs, void* user)
{
    BassInvocation invocation;
    BassBridge* self = s_instance.load(std::memory_order_seq_cst);
    if (!self)
        return 0;
    const bass::HSTREAM h = self->m_orig.streamCreateFileUser(system, flags, procs, user);
    if (h)
        self->OnStreamCreated(h, flags, "user");
    return h;
}

bass::HSTREAM SA_BASSCALL BassBridge::HookStreamCreate(bass::DWORD freq, bass::DWORD chans, bass::DWORD flags,
                                                       bass::StreamProc proc, void* user)
{
    BassInvocation invocation;
    BassBridge* self = s_instance.load(std::memory_order_seq_cst);
    if (!self)
        return 0;
    const bass::HSTREAM h = self->m_orig.streamCreate(freq, chans, flags, proc, user);
    if (h)
        self->OnStreamCreated(h, flags, "proc");
    return h;
}

bass::BOOL SA_BASSCALL BassBridge::HookStreamFree(bass::HSTREAM handle)
{
    BassInvocation invocation;
    BassBridge* self = s_instance.load(std::memory_order_seq_cst);
    if (!self)
        return 0;
    self->OnStreamFreed(handle);
    return self->m_orig.streamFree(handle);
}

bass::BOOL SA_BASSCALL BassBridge::HookChannelPlay(bass::DWORD handle, bass::BOOL restart)
{
    BassInvocation invocation;
    BassBridge* self = s_instance.load(std::memory_order_seq_cst);
    if (!self)
        return 0;
    const bass::BOOL ok = self->m_orig.channelPlay(handle, restart);
    if (ok)
        self->OnChannelPlay(handle, restart != 0);
    return ok;
}

bass::BOOL SA_BASSCALL BassBridge::HookChannelSet3DPosition(bass::DWORD handle, const bass::Vector3D* pos,
                                                            const bass::Vector3D* orient, const bass::Vector3D* vel)
{
    BassInvocation invocation;
    BassBridge* self = s_instance.load(std::memory_order_seq_cst);
    if (!self)
        return 0;
    const bass::BOOL ok = self->m_orig.channelSet3DPosition(handle, pos, orient, vel);
    if (ok)
        self->OnPosition(handle, pos, orient);
    return ok;
}

bass::BOOL SA_BASSCALL BassBridge::HookChannelSet3DAttributes(bass::DWORD handle, int32_t mode, float min, float max,
                                                              int32_t iangle, int32_t oangle, float outvol)
{
    BassInvocation invocation;
    BassBridge* self = s_instance.load(std::memory_order_seq_cst);
    if (!self)
        return 0;
    const bass::BOOL ok = self->m_orig.channelSet3DAttributes(handle, mode, min, max, iangle, oangle, outvol);
    if (ok)
        self->On3DAttributes(handle, mode, min, max, iangle, oangle, outvol);
    return ok;
}

bass::BOOL SA_BASSCALL BassBridge::HookChannelSetAttribute(bass::DWORD handle, bass::DWORD attrib, float value)
{
    BassInvocation invocation;
    BassBridge* self = s_instance.load(std::memory_order_seq_cst);
    if (!self)
        return 0;
    const bass::BOOL ok = self->m_orig.channelSetAttribute(handle, attrib, value);
    if (ok && attrib == bass::kAttribVol)
        self->OnVolume(handle, value);
    return ok;
}

} // namespace sa
