// src/steamaudio/HRTFRenderer.cpp
#include "HRTFRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "mixing/Dsp.h"
#include "mixing/SoundSource.h"
#include "steamaudio/OcclusionShaping.h"
#include "util/Logging.h"
#include "util/Math.h"

namespace sa {

struct HRTFRenderer::PooledEffects {
    EffectSet set;
    bool reflectionTailPending = false;
    bool reflectionApplied = false; // iplReflectionEffectApply ran since the last acquire
    bool inUse = false;
};

namespace {

IPLSpeakerLayout StereoLayout()
{
    IPLSpeakerLayout layout{};
    layout.type = IPL_SPEAKERLAYOUTTYPE_STEREO;
    layout.numSpeakers = 2;
    layout.speakers = nullptr;
    return layout;
}

int32_t AmbisonicChannels(int32_t order)
{
    return (order + 1) * (order + 1);
}

void MakeMono(const float* const* input, uint32_t channels, float* out, int32_t frames)
{
    if (channels == 0 || !input || !input[0]) {
        std::memset(out, 0, sizeof(float) * static_cast<size_t>(frames));
        return;
    }
    if (channels == 1) {
        std::memcpy(out, input[0], sizeof(float) * static_cast<size_t>(frames));
        return;
    }
    const float scale = 1.f / static_cast<float>(channels);
    for (int32_t i = 0; i < frames; ++i) {
        float acc = 0.f;
        for (uint32_t c = 0; c < channels; ++c)
            acc += input[c][i];
        out[i] = acc * scale;
    }
}

} // namespace

HRTFRenderer::~HRTFRenderer()
{
    Shutdown();
}

bool HRTFRenderer::Initialize(PhononContext& context, const BackendDevices& devices, const StaticConfig& config,
                              std::string& errorOut)
{
    Shutdown();
    if (!context.IsValid()) {
        errorOut = "context invalid";
        return false;
    }
    m_context = &context;
    m_frameSize = context.FrameSize();
    m_sampleRate = context.SampleRate();
    m_hrtfEnabled = config.hrtfEnabled;
    m_fallback.Initialize(m_frameSize, m_sampleRate);
    m_maxOrder = Clamp(config.maxAmbisonicOrder, 0, 3);
    m_ambisonicChannels = AmbisonicChannels(m_maxOrder);
    m_maxIrDuration = std::max(0.1f, config.maxIrDuration);
    m_reflectionType = devices.reflectionType;
    m_tanDevice = devices.tan;

    if (!CreateHrtf(config, errorOut))
        return false;

    IPLAudioSettings* audio = context.MutableAudioSettings();

    m_reflectionSettings = IPLReflectionEffectSettings{};
    m_reflectionSettings.type = m_reflectionType;
    m_reflectionSettings.irSize = static_cast<IPLint32>(std::lround(m_maxIrDuration * static_cast<float>(m_sampleRate)));
    m_reflectionSettings.numChannels = m_ambisonicChannels;

    m_useMixer = (m_reflectionType == IPL_REFLECTIONEFFECTTYPE_TAN ||
                  m_reflectionType == IPL_REFLECTIONEFFECTTYPE_CONVOLUTION);
    if (m_useMixer) {
        const IPLerror err = iplReflectionMixerCreate(context.Handle(), audio, &m_reflectionSettings, &m_reflectionMixer);
        if (err != IPL_STATUS_SUCCESS || !m_reflectionMixer) {
            SA_LOGW("iplReflectionMixerCreate failed: %s; rendering reflections per source", IplErrorToString(err));
            m_reflectionMixer = nullptr;
            m_useMixer = false;
            if (m_reflectionType == IPL_REFLECTIONEFFECTTYPE_TAN) {
                // TAN requires a mixer; degrade to CPU convolution.
                m_reflectionType = IPL_REFLECTIONEFFECTTYPE_CONVOLUTION;
                m_reflectionSettings.type = m_reflectionType;
                m_tanDevice = nullptr;
            }
        }
    }

    IPLAmbisonicsDecodeEffectSettings decodeSettings{};
    decodeSettings.speakerLayout = StereoLayout();
    decodeSettings.hrtf = m_hrtf;
    decodeSettings.maxOrder = m_maxOrder;
    IPLerror err = iplAmbisonicsDecodeEffectCreate(context.Handle(), audio, &decodeSettings, &m_ambisonicsDecode);
    if (err != IPL_STATUS_SUCCESS || !m_ambisonicsDecode) {
        errorOut = std::string("iplAmbisonicsDecodeEffectCreate failed: ") + IplErrorToString(err);
        Shutdown();
        return false;
    }

    const bool buffersOk = m_monoIn.Allocate(context, 1, m_frameSize) && m_monoDirect.Allocate(context, 1, m_frameSize) &&
                           m_stereoScratch.Allocate(context, 2, m_frameSize) &&
                           m_ambisonicScratch.Allocate(context, m_ambisonicChannels, m_frameSize) &&
                           m_ambisonicBus.Allocate(context, m_ambisonicChannels, m_frameSize) &&
                           m_master.Allocate(context, 2, m_frameSize);
    if (!buffersOk) {
        errorOut = "failed to allocate render buffers";
        Shutdown();
        return false;
    }
    m_pathShScratch.assign(static_cast<size_t>(m_ambisonicChannels), 0.f);

    const int32_t poolSize = std::max(1, config.maxSources);
    m_pool.reserve(static_cast<size_t>(poolSize));
    m_freeEffects.reserve(static_cast<size_t>(poolSize));
    m_drainingEffects.reserve(static_cast<size_t>(poolSize));
    for (int32_t i = 0; i < poolSize; ++i) {
        auto* pooled = new PooledEffects();
        if (!CreateEffectSet(*pooled)) {
            DestroyEffectSet(*pooled);
            delete pooled;
            if (m_pool.empty()) {
                errorOut = "failed to create any effect set";
                Shutdown();
                return false;
            }
            SA_LOGW("Effect pool truncated to %zu sets", m_pool.size());
            break;
        }
        m_pool.push_back(pooled);
        ReturnEffects(pooled);
    }

    SA_LOGI("HRTF renderer ready: %zu effect sets, reflections=%d mixer=%s order=%d irSize=%d", m_pool.size(),
            static_cast<int>(m_reflectionType), m_useMixer ? "yes" : "no", m_maxOrder, m_reflectionSettings.irSize);
    return true;
}

void HRTFRenderer::Shutdown()
{
    for (PooledEffects* pooled : m_pool) {
        DestroyEffectSet(*pooled);
        delete pooled;
    }
    m_pool.clear();
    m_freeEffects.clear();
    m_freeCount.store(0, std::memory_order_relaxed);
    m_drainingEffects.clear();
    m_fallback.Shutdown();

    if (m_ambisonicsDecode)
        iplAmbisonicsDecodeEffectRelease(&m_ambisonicsDecode);
    if (m_reflectionMixer)
        iplReflectionMixerRelease(&m_reflectionMixer);
    if (m_hrtf)
        iplHRTFRelease(&m_hrtf);
    m_ambisonicsDecode = nullptr;
    m_reflectionMixer = nullptr;
    m_hrtf = nullptr;

    m_monoIn.Free();
    m_monoDirect.Free();
    m_stereoScratch.Free();
    m_ambisonicScratch.Free();
    m_ambisonicBus.Free();
    m_master.Free();
    m_context = nullptr;
}

bool HRTFRenderer::CreateHrtf(const StaticConfig& config, std::string& errorOut)
{
    IPLHRTFSettings settings{};
    settings.type = IPL_HRTFTYPE_DEFAULT;
    settings.volume = DbToLinear(config.hrtfVolumeDb);
    settings.normType = IPL_HRTFNORMTYPE_NONE;

    if (!config.sofaFile.empty()) {
        settings.type = IPL_HRTFTYPE_SOFA;
        settings.sofaFileName = config.sofaFile.c_str();
        const IPLerror err = iplHRTFCreate(m_context->Handle(), m_context->MutableAudioSettings(), &settings, &m_hrtf);
        if (err == IPL_STATUS_SUCCESS && m_hrtf) {
            SA_LOGI("Loaded SOFA HRTF %s", config.sofaFile.c_str());
            return true;
        }
        SA_LOGW("Failed to load SOFA HRTF %s (%s); using default", config.sofaFile.c_str(), IplErrorToString(err));
        m_hrtf = nullptr;
        settings.type = IPL_HRTFTYPE_DEFAULT;
        settings.sofaFileName = nullptr;
    }

    const IPLerror err = iplHRTFCreate(m_context->Handle(), m_context->MutableAudioSettings(), &settings, &m_hrtf);
    if (err != IPL_STATUS_SUCCESS || !m_hrtf) {
        errorOut = std::string("iplHRTFCreate failed: ") + IplErrorToString(err);
        m_hrtf = nullptr;
        return false;
    }
    return true;
}

bool HRTFRenderer::CreateEffectSet(PooledEffects& pooled)
{
    EffectSet& set = pooled.set;
    IPLContext ctx = m_context->Handle();
    IPLAudioSettings* audio = m_context->MutableAudioSettings();

    IPLDirectEffectSettings direct{};
    direct.numChannels = 1;
    if (iplDirectEffectCreate(ctx, audio, &direct, &set.direct) != IPL_STATUS_SUCCESS)
        return false;

    IPLBinauralEffectSettings binaural{};
    binaural.hrtf = m_hrtf;
    if (iplBinauralEffectCreate(ctx, audio, &binaural, &set.binaural) != IPL_STATUS_SUCCESS)
        return false;

    IPLPanningEffectSettings panning{};
    panning.speakerLayout = StereoLayout();
    if (iplPanningEffectCreate(ctx, audio, &panning, &set.panning) != IPL_STATUS_SUCCESS)
        return false;

    IPLReflectionEffectSettings reflection = m_reflectionSettings;
    if (iplReflectionEffectCreate(ctx, audio, &reflection, &set.reflection) != IPL_STATUS_SUCCESS) {
        SA_LOGW("iplReflectionEffectCreate failed; source will render without reflections");
        set.reflection = nullptr;
    }

    IPLPathEffectSettings path{};
    path.maxOrder = m_maxOrder;
    path.spatialize = IPL_TRUE;
    path.speakerLayout = StereoLayout();
    path.hrtf = m_hrtf;
    if (iplPathEffectCreate(ctx, audio, &path, &set.path) != IPL_STATUS_SUCCESS) {
        SA_LOGW("iplPathEffectCreate failed; source will render without pathing");
        set.path = nullptr;
    }

    set.ambisonicsDecode = nullptr;
    set.ambisonicsEncode = nullptr;
    set.valid = true;
    return true;
}

void HRTFRenderer::DestroyEffectSet(PooledEffects& pooled)
{
    EffectSet& set = pooled.set;
    if (set.direct)
        iplDirectEffectRelease(&set.direct);
    if (set.binaural)
        iplBinauralEffectRelease(&set.binaural);
    if (set.panning)
        iplPanningEffectRelease(&set.panning);
    if (set.reflection)
        iplReflectionEffectRelease(&set.reflection);
    if (set.path)
        iplPathEffectRelease(&set.path);
    if (set.ambisonicsDecode)
        iplAmbisonicsDecodeEffectRelease(&set.ambisonicsDecode);
    if (set.ambisonicsEncode)
        iplAmbisonicsEncodeEffectRelease(&set.ambisonicsEncode);
    set = EffectSet{};
}

void HRTFRenderer::ReturnEffects(PooledEffects* set)
{
    m_freeEffects.push_back(set);
    m_freeCount.fetch_add(1, std::memory_order_relaxed);
}

EffectSet* HRTFRenderer::AcquireEffects()
{
    PooledEffects* pooled = nullptr;
    if (!m_freeEffects.empty()) {
        pooled = m_freeEffects.back();
        m_freeEffects.pop_back();
        m_freeCount.fetch_sub(1, std::memory_order_relaxed);
    } else if (!m_drainingEffects.empty()) {
        // Cut the oldest reflection tail short rather than rendering the new
        // sound without spatialization.
        pooled = m_drainingEffects.front();
        m_drainingEffects.front() = m_drainingEffects.back();
        m_drainingEffects.pop_back();
        m_stats.tailsCut.fetch_add(1, std::memory_order_relaxed);
    } else {
        SA_LOG_RT(QueueFull);
        m_stats.poolExhausted.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    pooled->inUse = true;
    pooled->reflectionTailPending = false;
    pooled->reflectionApplied = false;
    pooled->set.Reset();
    return &pooled->set;
}

void HRTFRenderer::ReleaseEffects(EffectSet* set)
{
    if (!set)
        return;
    for (PooledEffects* pooled : m_pool) {
        if (&pooled->set != set)
            continue;
        pooled->inUse = false;
        if (pooled->set.reflection && pooled->reflectionApplied) {
            pooled->reflectionTailPending = true;
            m_drainingEffects.push_back(pooled);
        } else {
            ReturnEffects(pooled);
        }
        return;
    }
}

// ---------------------------------------------------------------------------
// Frame rendering
// ---------------------------------------------------------------------------

void HRTFRenderer::BeginFrame(const RuntimeConfig& cfg, const ListenerState& listener, const RoomSend& roomSend)
{
    m_cfg = cfg;
    m_fallback.BeginFrame(cfg, listener, roomSend);
    m_roomSend = roomSend;
    m_listener = listener;
    if (listener.valid) {
        m_listenerFrame = listener.frame;
    } else {
        m_listenerFrame = IPLCoordinateSpace3{};
        m_listenerFrame.ahead = IPLVector3{0.f, 0.f, -1.f};
        m_listenerFrame.up = IPLVector3{0.f, 1.f, 0.f};
        m_listenerFrame.right = IPLVector3{1.f, 0.f, 0.f};
        m_listenerFrame.origin = IPLVector3{0.f, 0.f, 0.f};
    }
    m_master.Clear();
    m_ambisonicBus.Clear();
    m_ambisonicBusUsed = false;
    m_mixerUsed = false;
    m_frameDirectEnergy = 0.0;
    m_frameReflectionEnergy = 0.0;
    m_frameNonSpatialEnergy = 0.0;
}

IPLVector3 HRTFRenderer::DirectionTo(const IPLVector3& sourcePosition) const
{
    return iplCalculateRelativeDirection(m_context->Handle(), sourcePosition, m_listenerFrame.origin,
                                         m_listenerFrame.ahead, m_listenerFrame.up);
}

void HRTFRenderer::AddToMaster(const IPLAudioBuffer& stereo, float gain, double& energyMeter)
{
    float* l = m_master.Channel(0);
    float* r = m_master.Channel(1);
    const float* sl = stereo.data[0];
    const float* sr = stereo.data[stereo.numChannels > 1 ? 1 : 0];
    dsp::MixInto(l, sl, static_cast<size_t>(m_frameSize), gain);
    dsp::MixInto(r, sr, static_cast<size_t>(m_frameSize), gain);
    double e = 0.0;
    for (int32_t i = 0; i < m_frameSize; ++i)
        e += static_cast<double>(sl[i]) * sl[i] + static_cast<double>(sr[i]) * sr[i];
    energyMeter += e * static_cast<double>(gain) * gain;
}

void HRTFRenderer::UpdateBalance()
{
    const double frames = static_cast<double>(std::max<int32_t>(m_frameSize, 1));
    double l = 0.0;
    double r = 0.0;
    const float* ml = m_master.Channel(0);
    const float* mr = m_master.Channel(1);
    for (int32_t i = 0; i < m_frameSize; ++i) {
        l += static_cast<double>(ml[i]) * ml[i];
        r += static_cast<double>(mr[i]) * mr[i];
    }
    // ~1/8 per 512-frame block: a snapshot reflects roughly the last 100 ms.
    auto smooth = [](std::atomic<float>& avg, double meanSquare) {
        const float prev = avg.load(std::memory_order_relaxed);
        avg.store(prev + 0.125f * (static_cast<float>(meanSquare) - prev), std::memory_order_relaxed);
    };
    smooth(m_balance.masterL, l / frames);
    smooth(m_balance.masterR, r / frames);
    smooth(m_balance.direct, m_frameDirectEnergy / (2.0 * frames));
    smooth(m_balance.reflections, m_frameReflectionEnergy / (2.0 * frames));
    smooth(m_balance.nonSpatial, m_frameNonSpatialEnergy / (2.0 * frames));
}

MixBalance HRTFRenderer::Balance() const
{
    MixBalance b;
    b.masterL = m_balance.masterL.load(std::memory_order_relaxed);
    b.masterR = m_balance.masterR.load(std::memory_order_relaxed);
    b.direct = m_balance.direct.load(std::memory_order_relaxed);
    b.reflections = m_balance.reflections.load(std::memory_order_relaxed);
    b.nonSpatial = m_balance.nonSpatial.load(std::memory_order_relaxed);
    return b;
}

void HRTFRenderer::ApplyPropagationDelay(dsp::PropagationDelay& delay, float* mono, float distMeters) const
{
    if (!m_cfg.doppler || m_cfg.dopplerScale <= 0.f) {
        if (delay.Primed())
            delay.Reset();
        return;
    }
    const float speed = std::max(m_cfg.speedOfSound, 1.f);
    const float delaySamples = distMeters / speed * m_cfg.dopplerScale * static_cast<float>(m_sampleRate);
    delay.Process(mono, static_cast<size_t>(m_frameSize), delaySamples, static_cast<float>(m_sampleRate));
}

void HRTFRenderer::RenderNonSpatial(const float* const* input, uint32_t inputChannels, float gain, float gainL,
                                    float gainR, float startGain, RenderDiag& diag)
{
    if (!input || inputChannels == 0)
        return;
    const float* l = input[0];
    const float* r = inputChannels > 1 ? input[1] : input[0];
    double in = 0.0;
    double e = 0.0;
    for (int32_t i = 0; i < m_frameSize; ++i) {
        const float g = Lerp(startGain, gain, static_cast<float>(i + 1) / m_frameSize);
        const float left = l[i] * g * gainL;
        const float right = r[i] * g * gainR;
        m_master.Channel(0)[i] += left;
        m_master.Channel(1)[i] += right;
        in += 0.5 * (static_cast<double>(l[i]) * l[i] + static_cast<double>(r[i]) * r[i]);
        e += static_cast<double>(left) * left + static_cast<double>(right) * right;
    }
    m_frameNonSpatialEnergy += e;
    const double frames = static_cast<double>(std::max<int32_t>(m_frameSize, 1));
    diag.inputLevel = static_cast<float>(in / frames);
    diag.outputLevel = static_cast<float>(e / (2.0 * frames));
    diag.gain = gain * 0.5f * (gainL + gainR);
}

float HRTFRenderer::MeanSquare(const float* samples) const
{
    double e = 0.0;
    for (int32_t i = 0; i < m_frameSize; ++i)
        e += static_cast<double>(samples[i]) * samples[i];
    return static_cast<float>(e / static_cast<double>(std::max<int32_t>(m_frameSize, 1)));
}

void HRTFRenderer::RenderSource(SoundSource& source, const float* const* input, uint32_t inputChannels, float gain)
{
    if (!m_context || !input)
        return;
    SoundSource::RenderState& rs = source.Render();
    const SourceParams params = source.GetParams();
    rs.lastParams = params;
    rs.roomSent = false;

    const bool spatialize = params.spatialize != 0 && params.positionValid != 0 && m_listener.valid &&
                            (params.listenerRelative || inputChannels == 1 || m_cfg.spatializeStereo);
    RenderDiag diag;
    diag.framesRendered = rs.framesRendered;
    diag.framesNoData = rs.framesNoData;
    const auto publish = [&rs, &source](RenderDiag& d) {
        rs.peakOutputLevel = std::max(rs.peakOutputLevel, d.outputLevel);
        rs.minOcclusionRaw = std::min(rs.minOcclusionRaw, d.occlusionRaw);
        d.peakOutputLevel = rs.peakOutputLevel;
        d.minOcclusionRaw = rs.minOcclusionRaw;
        d.framesReflections = rs.framesReflections;
        d.framesPathing = rs.framesPathing;
        source.PublishRenderDiag(d);
    };
    if (!spatialize && rs.effects) {
        ReleaseEffects(rs.effects);
        rs.effects = nullptr;
    }
    if (spatialize && !rs.effects)
        rs.effects = AcquireEffects();
    if (spatialize && (!rs.effects || !rs.effects->valid)) {
        m_fallback.RenderSource(source, input, inputChannels, gain);
        rs.currentGain = gain * params.gain;
        rs.gainInitialized = true;
        diag = source.LoadRenderDiag();
        diag.mode = RenderDiag::kNoEffects;
        publish(diag);
        return;
    }
    gain *= params.gain;
    const float startGain = rs.gainInitialized ? rs.currentGain : gain;
    rs.currentGain = gain;
    rs.gainInitialized = true;
    if (!spatialize) {
        diag.mode = RenderDiag::kPassthrough;
        RenderNonSpatial(input, inputChannels, gain, params.engineGainL, params.engineGainR, startGain, diag);
        publish(diag);
        return;
    }
    EffectSet& fx = *rs.effects;

    IPLSimulationOutputs outputs{};
    bool haveOutputs = false;
    const IPLSource simSource = source.AudioSimulationSource();
    const IPLSimulationFlags readyFlags = source.SimulationReadyFlags();
    if (simSource && (readyFlags & IPL_SIMULATIONFLAGS_DIRECT)) {
        iplSourceGetOutputs(simSource, readyFlags, &outputs);
        haveOutputs = true;
    }
    rs.outputsValid = haveOutputs;
    if (haveOutputs)
        rs.outputs = outputs;

    CoordinateConverter converter;
    converter.SetUnitsPerMeter(m_cfg.unitsPerMeter);
    const IPLVector3 sourcePos = params.listenerRelative ? m_listenerFrame.origin
                                                       : converter.PositionToSA(params.position).ToIPL();
    const IPLVector3 direction = params.listenerRelative ? IPLVector3{0.f, 0.f, -1.f} : DirectionTo(sourcePos);
    const float distMeters = Vec3::Distance(Vec3{sourcePos.x, sourcePos.y, sourcePos.z},
                                            Vec3{m_listenerFrame.origin.x, m_listenerFrame.origin.y, m_listenerFrame.origin.z});
    const float distUnits = converter.LengthToSource(distMeters);
    diag.direction = Vec3::FromIPL(direction);
    diag.distanceMeters = distMeters;
    diag.simOutputs = haveOutputs ? 1 : 0;
    if (haveOutputs) {
        diag.distanceAttenuation = outputs.direct.distanceAttenuation;
        diag.occlusion = outputs.direct.occlusion;
        diag.occlusionRaw = outputs.direct.occlusion;
    }

    // 1. Mono input (+ propagation delay / Doppler).
    MakeMono(input, inputChannels, m_monoIn.Channel(0), m_frameSize);
    diag.inputLevel = MeanSquare(m_monoIn.Channel(0));
    ApplyPropagationDelay(rs.doppler, m_monoIn.Channel(0), distMeters);
    dsp::ApplyGainRamp(m_monoIn.Channel(0), static_cast<size_t>(m_frameSize), startGain, gain);
    const float sourceGain = gain;
    gain = 1.f;

    // 2. Direct path. The simulator reports the per-effect values but leaves
    // `flags` to the caller: it selects which of them the effect applies.
    IPLDirectEffectParams directParams{};
    if (haveOutputs) {
        directParams = outputs.direct;
        IPLDirectEffectFlags flags = static_cast<IPLDirectEffectFlags>(IPL_DIRECTEFFECTFLAGS_APPLYAIRABSORPTION |
                                                                       IPL_DIRECTEFFECTFLAGS_APPLYDIRECTIVITY);
        if (params.distMult > 0.f)
            flags = static_cast<IPLDirectEffectFlags>(flags | IPL_DIRECTEFFECTFLAGS_APPLYDISTANCEATTENUATION);
        if (m_cfg.occlusion && params.occlusion) {
            flags = static_cast<IPLDirectEffectFlags>(flags | IPL_DIRECTEFFECTFLAGS_APPLYOCCLUSION);
            if (m_cfg.transmission && params.transmission)
                flags = static_cast<IPLDirectEffectFlags>(flags | IPL_DIRECTEFFECTFLAGS_APPLYTRANSMISSION);
        }
        directParams.flags = flags;
        directParams.transmissionType = IPL_TRANSMISSIONTYPE_FREQDEPENDENT;
        ShapeOcclusion(directParams, flags, m_cfg);
        diag.occlusion = directParams.occlusion;
    } else {
        // Simulation has not produced results yet: approximate the engine's own
        // distance rolloff so the first frames are not rendered at full volume.
        directParams.flags = IPL_DIRECTEFFECTFLAGS_APPLYDISTANCEATTENUATION;
        directParams.distanceAttenuation =
            SourceDistanceGain(params.distMult, distUnits, m_cfg.distanceGainMin, m_cfg.distanceGainMax);
        directParams.transmissionType = IPL_TRANSMISSIONTYPE_FREQINDEPENDENT;
    }
    directParams.distanceAttenuation =
        SourceDistanceGain(params.distMult, distUnits, m_cfg.distanceGainMin, m_cfg.distanceGainMax);
    if (params.listenerRelative) {
        directParams.flags = static_cast<IPLDirectEffectFlags>(0);
        directParams.distanceAttenuation = 1.f;
    }
    diag.distanceAttenuation = directParams.distanceAttenuation;
    iplDirectEffectApply(fx.direct, &directParams, m_monoIn.Get(), m_monoDirect.Get());

    // 3. Spatialization (HRTF or panning) to stereo scratch.
    const bool useHrtf = m_hrtfEnabled && m_cfg.hrtf && m_hrtf != nullptr;
    diag.mode = useHrtf ? RenderDiag::kHrtf : RenderDiag::kPanning;
    if (!haveOutputs)
        diag.distanceAttenuation = directParams.distanceAttenuation;
    if (params.listenerRelative) {
        diag.mode = RenderDiag::kListenerRelative;
        RenderNonSpatial(input, inputChannels, sourceGain * m_cfg.directGain, 1.f, 1.f,
                         startGain * m_cfg.directGain, diag);
    } else {
        if (useHrtf) {
            IPLBinauralEffectParams bp{};
            bp.direction = direction;
            bp.interpolation = m_cfg.hrtfInterpolation == 0 ? IPL_HRTFINTERPOLATION_NEAREST : IPL_HRTFINTERPOLATION_BILINEAR;
            bp.spatialBlend = 1.f;
            bp.hrtf = m_hrtf;
            bp.peakDelays = nullptr;
            iplBinauralEffectApply(fx.binaural, &bp, m_monoDirect.Get(), m_stereoScratch.Get());
        } else {
            IPLPanningEffectParams pp{};
            pp.direction = direction;
            iplPanningEffectApply(fx.panning, &pp, m_monoDirect.Get(), m_stereoScratch.Get());
        }
        const double directBefore = m_frameDirectEnergy;
        AddToMaster(*m_stereoScratch.Get(), gain * m_cfg.directGain, m_frameDirectEnergy);
        diag.gain = sourceGain * m_cfg.directGain;
        diag.outputLevel = static_cast<float>((m_frameDirectEnergy - directBefore) /
                                              (2.0 * static_cast<double>(std::max<int32_t>(m_frameSize, 1))));
    }

    // Room reverb send (Source dsp_room replacement). Fed with the attenuated/
    // occluded direct signal so distant or blocked sources excite the room
    // less, while the mix ratio itself grows with distance like the engine's.
    const bool renderReflections = haveOutputs && m_cfg.reflections && params.reflections && fx.reflection &&
                                   (readyFlags & IPL_SIMULATIONFLAGS_REFLECTIONS);
    if (m_roomSend.Enabled() && (m_roomSend.always || !renderReflections)) {
        const float mix = RoomMixForSource(*m_roomSend.preset, distUnits, DistMultToSoundLevel(params.distMult));
        if (mix > 0.f) {
            dsp::MixInto(m_roomSend.bus, m_monoDirect.Channel(0), static_cast<size_t>(m_frameSize),
                         gain * mix * params.reverbGain);
            rs.roomSent = true;
        }
    }

    if (!haveOutputs) {
        publish(diag);
        return;
    }

    // 4. Reflections (fed with the dry signal; the IR carries the attenuation).
    if (renderReflections) {
        ++rs.framesReflections;
        // The direct buffer has already been consumed; reuse it as a gained dry copy
        // so the reflections follow the source volume.
        float* wet = m_monoDirect.Channel(0);
        std::memcpy(wet, m_monoIn.Channel(0), sizeof(float) * static_cast<size_t>(m_frameSize));
        dsp::ApplyGain(wet, static_cast<size_t>(m_frameSize), gain * m_cfg.reverbGain * params.reverbGain);

        IPLReflectionEffectParams rp = outputs.reflections;
        rp.type = m_reflectionType;
        rp.numChannels = AmbisonicChannels(Clamp(m_cfg.ambisonicOrder, 0, m_maxOrder));
        rp.irSize = static_cast<IPLint32>(std::lround(Clamp(m_cfg.irDuration, 0.1f, m_maxIrDuration) *
                                                      static_cast<float>(m_sampleRate)));
        rp.tanDevice = m_tanDevice;
        MarkReflectionApplied(fx);
        if (m_useMixer && m_reflectionMixer) {
            iplReflectionEffectApply(fx.reflection, &rp, m_monoDirect.Get(), m_ambisonicScratch.Get(), m_reflectionMixer);
            m_mixerUsed = true;
        } else {
            m_ambisonicScratch.Clear();
            iplReflectionEffectApply(fx.reflection, &rp, m_monoDirect.Get(), m_ambisonicScratch.Get(), nullptr);
            iplAudioBufferMix(m_context->Handle(), m_ambisonicScratch.Get(), m_ambisonicBus.Get());
            m_ambisonicBusUsed = true;
        }
    }

    // 5. Pathing (spatialized directly to stereo by the path effect).
    if (m_cfg.pathing && params.pathing && fx.path && (readyFlags & IPL_SIMULATIONFLAGS_PATHING) &&
        outputs.pathing.shCoeffs != nullptr) {
        IPLPathEffectParams pathParams = outputs.pathing;
        pathParams.order = Clamp(pathParams.order, 0, m_maxOrder);
        pathParams.binaural = useHrtf ? IPL_TRUE : IPL_FALSE;
        pathParams.hrtf = m_hrtf;
        pathParams.listener = m_listenerFrame;
        iplPathEffectApply(fx.path, &pathParams, m_monoIn.Get(), m_stereoScratch.Get());
        AddToMaster(*m_stereoScratch.Get(), gain * m_cfg.pathingGain, m_frameDirectEnergy);
        ++rs.framesPathing;
    }
    publish(diag);
}

void HRTFRenderer::MarkReflectionApplied(const EffectSet& set)
{
    for (PooledEffects* pooled : m_pool) {
        if (&pooled->set == &set) {
            pooled->reflectionApplied = true;
            return;
        }
    }
}

void HRTFRenderer::RenderReflectionTails()
{
    for (size_t i = 0; i < m_drainingEffects.size();) {
        PooledEffects* pooled = m_drainingEffects[i];
        IPLAudioEffectState state = IPL_AUDIOEFFECTSTATE_TAILCOMPLETE;
        if (pooled->set.reflection) {
            if (m_useMixer && m_reflectionMixer) {
                state = iplReflectionEffectGetTail(pooled->set.reflection, m_ambisonicScratch.Get(), m_reflectionMixer);
                m_mixerUsed = true;
            } else {
                m_ambisonicScratch.Clear();
                state = iplReflectionEffectGetTail(pooled->set.reflection, m_ambisonicScratch.Get(), nullptr);
                iplAudioBufferMix(m_context->Handle(), m_ambisonicScratch.Get(), m_ambisonicBus.Get());
                m_ambisonicBusUsed = true;
            }
        }
        if (state == IPL_AUDIOEFFECTSTATE_TAILCOMPLETE || pooled->inUse) {
            pooled->reflectionTailPending = false;
            if (!pooled->inUse)
                ReturnEffects(pooled);
            m_drainingEffects[i] = m_drainingEffects.back();
            m_drainingEffects.pop_back();
        } else {
            ++i;
        }
    }
}

void HRTFRenderer::EndFrame()
{
    if (!m_context)
        return;
    RenderReflectionTails();

    const int32_t order = Clamp(m_cfg.ambisonicOrder, 0, m_maxOrder);
    const bool useHrtf = m_hrtfEnabled && m_cfg.hrtf && m_hrtf != nullptr;

    if (m_mixerUsed && m_reflectionMixer) {
        IPLReflectionEffectParams rp{};
        rp.type = m_reflectionType;
        rp.numChannels = AmbisonicChannels(order);
        rp.irSize = static_cast<IPLint32>(std::lround(Clamp(m_cfg.irDuration, 0.1f, m_maxIrDuration) *
                                                      static_cast<float>(m_sampleRate)));
        rp.tanDevice = m_tanDevice;
        m_ambisonicScratch.Clear();
        iplReflectionMixerApply(m_reflectionMixer, &rp, m_ambisonicScratch.Get());
        iplAudioBufferMix(m_context->Handle(), m_ambisonicScratch.Get(), m_ambisonicBus.Get());
        m_ambisonicBusUsed = true;
    }

    if (m_ambisonicBusUsed && m_ambisonicsDecode) {
        IPLAmbisonicsDecodeEffectParams dp{};
        dp.order = order;
        dp.hrtf = m_hrtf;
        dp.orientation = m_listenerFrame;
        dp.binaural = useHrtf ? IPL_TRUE : IPL_FALSE;
        iplAmbisonicsDecodeEffectApply(m_ambisonicsDecode, &dp, m_ambisonicBus.Get(), m_stereoScratch.Get());
        AddToMaster(*m_stereoScratch.Get(), 1.f, m_frameReflectionEnergy);
    }
    m_fallback.EndFrame();
    dsp::MixInto(m_master.Channel(0), m_fallback.MasterLeft(), static_cast<size_t>(m_frameSize));
    dsp::MixInto(m_master.Channel(1), m_fallback.MasterRight(), static_cast<size_t>(m_frameSize));
    UpdateBalance();

    float peak = 0.f;
    for (int32_t c = 0; c < 2; ++c) {
        const float* d = m_master.Channel(c);
        for (int32_t i = 0; i < m_frameSize; ++i)
            peak = std::max(peak, std::fabs(d[i]));
    }
    m_lastPeak = peak;
}

} // namespace sa
