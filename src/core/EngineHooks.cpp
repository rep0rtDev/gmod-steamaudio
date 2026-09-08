// src/core/EngineHooks.cpp
#include "core/EngineHooks.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstring>

#include "core/AutoDirectivity.h"
#include "mixing/Dsp.h"
#include "util/Logging.h"
#include "util/SignatureScanner.h"

// Member-function ABI for the hook trampolines. On 32-bit Windows the engine
// uses __thiscall (this in ECX); a __fastcall free function receives ECX/EDX
// as its first two arguments, so we take a dummy EDX parameter. Everywhere
// else (x64 Windows) `this` is simply the first argument.
#if defined(_WIN32) && !defined(_WIN64)
#define SA_HOOKCALL __fastcall
#define SA_THIS void *self, void * /*edx*/
#define SA_ORIG_CC __thiscall
#else
#define SA_HOOKCALL
#define SA_THIS void* self
#define SA_ORIG_CC
#endif

namespace sa {

namespace {

std::atomic<EngineHooks*> g_hooks{nullptr};
std::atomic<uint32_t> g_hookReaders{0};

struct HookInvocation {
    HookInvocation() { g_hookReaders.fetch_add(1, std::memory_order_seq_cst); }
    ~HookInvocation() { g_hookReaders.fetch_sub(1, std::memory_order_seq_cst); }
};

using PaintBeginFn = int(SA_ORIG_CC*)(void*, float, int, int);
using PaintEndFn = void(SA_ORIG_CC*)(void*);
using SpatializeFn = void(SA_ORIG_CC*)(void*, int*, int, const src::Vector&, float, float);
using ApplyDspFn = void(SA_ORIG_CC*)(void*, int, src::portable_samplepair_t*, src::portable_samplepair_t*,
                                     src::portable_samplepair_t*, int);
using UpdateListenerFn = void(SA_ORIG_CC*)(void*, const src::Vector&, const src::Vector&, const src::Vector&,
                                           const src::Vector&);
using MixBeginFn = void(SA_ORIG_CC*)(void*, int);
using Mix8Fn = void(SA_ORIG_CC*)(void*, src::channel_t*, char*, int, int, int, int, int);
using Mix16Fn = void(SA_ORIG_CC*)(void*, src::channel_t*, short*, int, int, int, int, int);
using ChannelResetFn = void(SA_ORIG_CC*)(void*, int, int, float);
using TransferFn = void(SA_ORIG_CC*)(void*, int);
using StopAllFn = void(SA_ORIG_CC*)(void*);
// Both IEngineSound::EmitSound overloads share this stack layout (the 6th
// argument is a float attenuation in one and an int soundlevel in the other;
// it is forwarded bit-exact as an int). Hooking both slots with the same
// layout-agnostic trampoline makes the hook independent of the compiler's
// overload ordering in the vtable (MSVC reverses adjacent overloads).
using EmitSoundFn = void(SA_ORIG_CC*)(void*, void* filter, int entity, int channel, const char* sample,
                                      float volume, int32_t attenuationOrLevel, int flags, int pitch,
                                      int specialDsp, const src::Vector* origin, const src::Vector* direction,
                                      void* utlVecOrigins, bool updatePositions, float soundTime,
                                      int speakerEntity);

// IEngineSound vtable: PrecacheSound, IsSoundPrecached, PrefetchSound,
// GetSoundDuration, EmitSound x2, EmitSentenceByIndex, ...
constexpr size_t kEmitSoundSlotFirst = 4;
constexpr size_t kEmitSoundSlotSecond = 5;
constexpr size_t kEngineSoundSlotCount = 21;

inline Vec3 ToVec3(const src::Vector& v)
{
    return Vec3{v.x, v.y, v.z};
}

} // namespace

// Static trampolines; they fetch the singleton and forward.
struct HookTrampolines {
    static int SA_HOOKCALL PaintBegin(SA_THIS, float mixAheadTime, int soundtime, int paintedtime)
    {
        HookInvocation invocation;
        EngineHooks* h = g_hooks.load(std::memory_order_seq_cst);
        auto orig = h ? h->m_vmt.Original<PaintBeginFn>(static_cast<size_t>(h->m_slots.paintBegin)) : nullptr;
        const int endtime = orig ? orig(self, mixAheadTime, soundtime, paintedtime) : 0;
        if (h) {
            h->m_hookCalls.fetch_add(1, std::memory_order_relaxed);
            if (!h->Passthrough() && h->m_capture)
                h->m_capture->OnPaintBegin(soundtime, paintedtime, orig ? endtime : paintedtime);
        }
        return endtime;
    }

    static void SA_HOOKCALL SpatializeChannel(SA_THIS, int* volume, int masterVol, const src::Vector& sourceDir,
                                              float gain, float mono)
    {
        HookInvocation invocation;
        EngineHooks* h = g_hooks.load(std::memory_order_seq_cst);
        auto orig = h ? h->m_vmt.Original<SpatializeFn>(static_cast<size_t>(h->m_slots.spatializeChannel)) : nullptr;
        if (orig)
            orig(self, volume, masterVol, sourceDir, gain, mono);
        if (!h || h->Passthrough() || !h->m_capture || !volume)
            return;
        h->m_capture->OnSpatialize(volume, masterVol, sourceDir, gain);
        // Defeat the engine's distance/occlusion culling and panning: every
        // speaker slot gets the unattenuated master volume so the channel is
        // always handed to the mixer, where we take over.
        const int v = std::max(1, std::min(255, masterVol));
        for (int i = 0; i < src::kChanVolumes / 2; ++i)
            volume[i] = v;
    }

    static void SA_HOOKCALL ApplyDSPEffects(SA_THIS, int idsp, src::portable_samplepair_t* front,
                                            src::portable_samplepair_t* rear, src::portable_samplepair_t* center,
                                            int sampleCount)
    {
        HookInvocation invocation;
        EngineHooks* h = g_hooks.load(std::memory_order_seq_cst);
        auto orig = h ? h->m_vmt.Original<ApplyDspFn>(static_cast<size_t>(h->m_slots.applyDSPEffects)) : nullptr;
        if (!h || h->Passthrough()) {
            if (orig)
                orig(self, idsp, front, rear, center, sampleCount);
            return;
        }
        // Engine DSP (room reverb presets, underwater filter) is replaced by
        // Steam Audio's simulation: skip it entirely.
    }

    static void SA_HOOKCALL UpdateListener(SA_THIS, const src::Vector& position, const src::Vector& forward,
                                           const src::Vector& right, const src::Vector& up)
    {
        HookInvocation invocation;
        EngineHooks* h = g_hooks.load(std::memory_order_seq_cst);
        auto orig = h ? h->m_vmt.Original<UpdateListenerFn>(static_cast<size_t>(h->m_slots.updateListener)) : nullptr;
        if (orig)
            orig(self, position, forward, right, up);
        if (!h)
            return;
        EngineListener l;
        l.position = ToVec3(position);
        l.forward = ToVec3(forward);
        l.right = ToVec3(right);
        l.up = ToVec3(up);
        l.stamp = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        l.valid = 1;
        if (l.position.IsFinite() && l.forward.IsFinite() && l.right.IsFinite() && l.up.IsFinite() &&
            l.forward.LengthSq() > 0.5f) {
            h->m_listener.Store(l);
            h->m_listenerFromEngine.store(true, std::memory_order_relaxed);
        }
    }

    static void SA_HOOKCALL MixBegin(SA_THIS, int sampleCount)
    {
        HookInvocation invocation;
        EngineHooks* h = g_hooks.load(std::memory_order_seq_cst);
        auto orig = h ? h->m_vmt.Original<MixBeginFn>(static_cast<size_t>(h->m_slots.mixBegin)) : nullptr;
        if (orig)
            orig(self, sampleCount);
        if (h && !h->Passthrough() && h->m_capture)
            h->m_capture->OnMixBegin(sampleCount);
    }

    template <bool Is16, bool Stereo>
    static void MixCommon(EngineHooks* h, src::channel_t* ch, const void* data, int outputOffset, int inputOffset,
                          int rateScaleFix, int outCount)
    {
        h->m_capture->CaptureMix(ch, data, Is16, Stereo, outputOffset, inputOffset, rateScaleFix, outCount);
    }

    static void SA_HOOKCALL Mix8Mono(SA_THIS, src::channel_t* ch, char* data, int outputOffset, int inputOffset,
                                     int rateScaleFix, int outCount, int timecompress)
    {
        HookInvocation invocation;
        EngineHooks* h = g_hooks.load(std::memory_order_seq_cst);
        if (!h || h->Passthrough() || !h->m_capture) {
            auto orig = h ? h->m_vmt.Original<Mix8Fn>(static_cast<size_t>(h->m_slots.mix8Mono)) : nullptr;
            if (orig)
                orig(self, ch, data, outputOffset, inputOffset, rateScaleFix, outCount, timecompress);
            return;
        }
        MixCommon<false, false>(h, ch, data, outputOffset, inputOffset, rateScaleFix, outCount);
    }

    static void SA_HOOKCALL Mix8Stereo(SA_THIS, src::channel_t* ch, char* data, int outputOffset, int inputOffset,
                                       int rateScaleFix, int outCount, int timecompress)
    {
        HookInvocation invocation;
        EngineHooks* h = g_hooks.load(std::memory_order_seq_cst);
        if (!h || h->Passthrough() || !h->m_capture) {
            auto orig = h ? h->m_vmt.Original<Mix8Fn>(static_cast<size_t>(h->m_slots.mix8Stereo)) : nullptr;
            if (orig)
                orig(self, ch, data, outputOffset, inputOffset, rateScaleFix, outCount, timecompress);
            return;
        }
        MixCommon<false, true>(h, ch, data, outputOffset, inputOffset, rateScaleFix, outCount);
    }

    static void SA_HOOKCALL Mix16Mono(SA_THIS, src::channel_t* ch, short* data, int outputOffset, int inputOffset,
                                      int rateScaleFix, int outCount, int timecompress)
    {
        HookInvocation invocation;
        EngineHooks* h = g_hooks.load(std::memory_order_seq_cst);
        if (!h || h->Passthrough() || !h->m_capture) {
            auto orig = h ? h->m_vmt.Original<Mix16Fn>(static_cast<size_t>(h->m_slots.mix16Mono)) : nullptr;
            if (orig)
                orig(self, ch, data, outputOffset, inputOffset, rateScaleFix, outCount, timecompress);
            return;
        }
        MixCommon<true, false>(h, ch, data, outputOffset, inputOffset, rateScaleFix, outCount);
    }

    static void SA_HOOKCALL Mix16Stereo(SA_THIS, src::channel_t* ch, short* data, int outputOffset, int inputOffset,
                                        int rateScaleFix, int outCount, int timecompress)
    {
        HookInvocation invocation;
        EngineHooks* h = g_hooks.load(std::memory_order_seq_cst);
        if (!h || h->Passthrough() || !h->m_capture) {
            auto orig = h ? h->m_vmt.Original<Mix16Fn>(static_cast<size_t>(h->m_slots.mix16Stereo)) : nullptr;
            if (orig)
                orig(self, ch, data, outputOffset, inputOffset, rateScaleFix, outCount, timecompress);
            return;
        }
        MixCommon<true, true>(h, ch, data, outputOffset, inputOffset, rateScaleFix, outCount);
    }

    static void SA_HOOKCALL ChannelReset(SA_THIS, int entnum, int channelIndex, float distanceMod)
    {
        HookInvocation invocation;
        EngineHooks* h = g_hooks.load(std::memory_order_seq_cst);
        auto orig = h ? h->m_vmt.Original<ChannelResetFn>(static_cast<size_t>(h->m_slots.channelReset)) : nullptr;
        if (orig)
            orig(self, entnum, channelIndex, distanceMod);
        if (h && h->m_capture)
            h->m_capture->OnChannelReset(entnum, channelIndex);
    }

    static void SA_HOOKCALL TransferSamples(SA_THIS, int end)
    {
        HookInvocation invocation;
        EngineHooks* h = g_hooks.load(std::memory_order_seq_cst);
        auto orig = h ? h->m_vmt.Original<TransferFn>(static_cast<size_t>(h->m_slots.transferSamples)) : nullptr;
        if (h && !h->Passthrough() && h->m_capture) {
            const uint64_t start = h->m_capture->PaintClock();
            h->m_capture->OnTransferSamples(end);
            const uint64_t stop = h->m_capture->PaintClock();
            IEngineOutputSink* sink = h->m_outputSink.load(std::memory_order_acquire);
            if (h->m_engineOutput.load(std::memory_order_acquire) && sink && h->m_paintBuffer && stop > start) {
                const size_t frames = std::min<size_t>(static_cast<size_t>(stop - start),
                                                       static_cast<size_t>(src::kPaintBufferSize));
                sink->FillPaintBuffer(start, h->m_paintBuffer, frames);
            }
        }
        if (orig)
            orig(self, end);
    }

    static void SA_HOOKCALL StopAllSounds(SA_THIS)
    {
        HookInvocation invocation;
        EngineHooks* h = g_hooks.load(std::memory_order_seq_cst);
        auto orig = h ? h->m_vmt.Original<StopAllFn>(static_cast<size_t>(h->m_slots.stopAllSounds)) : nullptr;
        if (orig)
            orig(self);
    }

    static void EmitSoundCommon(size_t slot, void* self, void* filter, int entity, int channel, const char* sample,
                                float volume, int32_t attenuationOrLevel, int flags, int pitch, int specialDsp,
                                const src::Vector* origin, const src::Vector* direction, void* utlVecOrigins,
                                bool updatePositions, float soundTime, int speakerEntity)
    {
        HookInvocation invocation;
        EngineHooks* h = g_hooks.load(std::memory_order_seq_cst);
        auto orig = h ? h->m_soundVmt.Original<EmitSoundFn>(slot) : nullptr;
        if (!orig)
            return;
        orig(self, filter, entity, channel, sample, volume, attenuationOrLevel, flags, pitch, specialDsp, origin,
             direction, utlVecOrigins, updatePositions, soundTime, speakerEntity);
        h->OnSoundEmitted(self, entity, channel, sample);
    }

    static void SA_HOOKCALL EmitSoundFirst(SA_THIS, void* filter, int entity, int channel, const char* sample,
                                          float volume, int32_t attenuationOrLevel, int flags, int pitch,
                                          int specialDsp, const src::Vector* origin, const src::Vector* direction,
                                          void* utlVecOrigins, bool updatePositions, float soundTime,
                                          int speakerEntity)
    {
        EmitSoundCommon(kEmitSoundSlotFirst, self, filter, entity, channel, sample, volume, attenuationOrLevel,
                        flags, pitch, specialDsp, origin, direction, utlVecOrigins, updatePositions, soundTime,
                        speakerEntity);
    }

    static void SA_HOOKCALL EmitSoundSecond(SA_THIS, void* filter, int entity, int channel, const char* sample,
                                           float volume, int32_t attenuationOrLevel, int flags, int pitch,
                                           int specialDsp, const src::Vector* origin, const src::Vector* direction,
                                           void* utlVecOrigins, bool updatePositions, float soundTime,
                                           int speakerEntity)
    {
        EmitSoundCommon(kEmitSoundSlotSecond, self, filter, entity, channel, sample, volume, attenuationOrLevel,
                        flags, pitch, specialDsp, origin, direction, utlVecOrigins, updatePositions, soundTime,
                        speakerEntity);
    }
};

// ---------------------------------------------------------------------------
EngineHooks::EngineHooks() = default;

EngineHooks::~EngineHooks()
{
    Uninstall();
}

bool EngineHooks::Install(EngineInterfaces& interfaces, ChannelCapture& capture, std::string& error)
{
    Uninstall();
    m_interfaces = &interfaces;
    m_capture = &capture;
    m_slots = interfaces.Config().slots;
    m_paintBuffer = interfaces.PaintBuffer();

    const AudioDeviceInfo& dev = interfaces.AudioDevice();
    if (!dev.vtable) {
        error = "IAudioDevice vtable not resolved";
        return false;
    }
    if (!dev.validated && !interfaces.Config().allowUnvalidatedDevice) {
        error = "IAudioDevice layout not validated; refusing to hook (set allow_unvalidated_device to force)";
        return false;
    }
    if (interfaces.Config().disableMixerHooks) {
        error = "mixer hooks disabled by configuration";
        return false;
    }
    if (!m_vmt.Attach(dev.vtable, dev.slotCount)) {
        error = "failed to attach to IAudioDevice vtable";
        return false;
    }

    g_hooks.store(this, std::memory_order_release);

    struct HookDef {
        int32_t slot;
        void* fn;
        const char* name;
    };
    const HookDef defs[] = {
        {m_slots.paintBegin, reinterpret_cast<void*>(&HookTrampolines::PaintBegin), "PaintBegin"},
        {m_slots.spatializeChannel, reinterpret_cast<void*>(&HookTrampolines::SpatializeChannel), "SpatializeChannel"},
        {m_slots.applyDSPEffects, reinterpret_cast<void*>(&HookTrampolines::ApplyDSPEffects), "ApplyDSPEffects"},
        {m_slots.updateListener, reinterpret_cast<void*>(&HookTrampolines::UpdateListener), "UpdateListener"},
        {m_slots.mixBegin, reinterpret_cast<void*>(&HookTrampolines::MixBegin), "MixBegin"},
        {m_slots.mix8Mono, reinterpret_cast<void*>(&HookTrampolines::Mix8Mono), "Mix8Mono"},
        {m_slots.mix8Stereo, reinterpret_cast<void*>(&HookTrampolines::Mix8Stereo), "Mix8Stereo"},
        {m_slots.mix16Mono, reinterpret_cast<void*>(&HookTrampolines::Mix16Mono), "Mix16Mono"},
        {m_slots.mix16Stereo, reinterpret_cast<void*>(&HookTrampolines::Mix16Stereo), "Mix16Stereo"},
        {m_slots.channelReset, reinterpret_cast<void*>(&HookTrampolines::ChannelReset), "ChannelReset"},
        {m_slots.transferSamples, reinterpret_cast<void*>(&HookTrampolines::TransferSamples), "TransferSamples"},
        {m_slots.stopAllSounds, reinterpret_cast<void*>(&HookTrampolines::StopAllSounds), "StopAllSounds"},
    };
    for (const HookDef& d : defs) {
        if (d.slot < 0 || static_cast<size_t>(d.slot) >= dev.slotCount) {
            error = std::string("slot for ") + d.name + " is out of range";
            m_installed = true;
            Uninstall();
            return false;
        }
        if (!m_vmt.Hook(static_cast<size_t>(d.slot), d.fn)) {
            error = std::string("failed to hook IAudioDevice::") + d.name;
            m_installed = true;
            Uninstall();
            return false;
        }
    }
    m_installed = true;
    for (SlotTrack& t : m_slotTrack)
        t = SlotTrack{};
    SA_LOGI("[hooks] IAudioDevice hooks installed on %s (%zu slots)%s", dev.className.c_str(), dev.slotCount,
            m_paintBuffer ? ", engine output available" : "");
    InstallEmitSoundHook();
    return true;
}

void EngineHooks::InstallEmitSoundHook()
{
    m_emitSoundHooked = false;
    if (!m_interfaces || m_interfaces->Config().disableEmitSoundHook)
        return;
    src::IEngineSound* snd = m_interfaces->EngineSound();
    if (!snd || m_interfaces->IsDedicatedServer()) {
        SA_LOGW("[hooks] IEngineSound unavailable; per-sound rules will only see names from the file system");
        return;
    }
    const uintptr_t object = reinterpret_cast<uintptr_t>(snd);
    if (!IsMemoryReadable(object, sizeof(void*)))
        return;
    void** vtable = *reinterpret_cast<void***>(snd);
    if (!IsMemoryReadable(reinterpret_cast<uintptr_t>(vtable), kEngineSoundSlotCount * sizeof(void*))) {
        SA_LOGW("[hooks] IEngineSound vtable unreadable; EmitSound hook skipped");
        return;
    }
    for (size_t i = 0; i < kEngineSoundSlotCount; ++i) {
        if (!vtable[i]) {
            SA_LOGW("[hooks] IEngineSound vtable slot %zu is null; EmitSound hook skipped", i);
            return;
        }
    }
    if (!m_soundVmt.Attach(vtable, kEngineSoundSlotCount))
        return;
    if (!m_soundVmt.Hook(kEmitSoundSlotFirst, reinterpret_cast<void*>(&HookTrampolines::EmitSoundFirst)) ||
        !m_soundVmt.Hook(kEmitSoundSlotSecond, reinterpret_cast<void*>(&HookTrampolines::EmitSoundSecond))) {
        m_soundVmt.Detach();
        SA_LOGW("[hooks] failed to hook IEngineSound::EmitSound");
        return;
    }
    m_emitSoundHooked = true;
    SA_LOGI("[hooks] IEngineSound::EmitSound hooked (slots %zu/%zu, pass-through)", kEmitSoundSlotFirst,
            kEmitSoundSlotSecond);
}

void EngineHooks::UninstallEmitSoundHook()
{
    if (m_soundVmt.IsAttached())
        m_soundVmt.Detach();
    m_emitSoundHooked = false;
}

void EngineHooks::OnSoundEmitted(void* engineSound, int32_t entity, int32_t channel, const char* sample)
{
    m_emitSoundCalls.fetch_add(1, std::memory_order_relaxed);
    if (!m_interfaces || Passthrough())
        return;
    src::IEngineSound* snd = m_interfaces->EngineSound();
    if (!snd || static_cast<void*>(snd) != engineSound)
        return;
    const int32_t guid = snd->GetGuidForLastSoundEmitted();
    if (guid == 0 || guid == m_lastEmittedGuid)
        return; // failed start (precache miss / too many channels) or a repeat
    m_lastEmittedGuid = guid;
    m_overrides.NoteEmitted(guid, sample, entity, channel, m_now);
}

void EngineHooks::Uninstall()
{
    if (!m_installed) {
        g_hooks.store(nullptr, std::memory_order_release);
        return;
    }
    // Restore original behaviour first so the mixer immediately stops
    // entering our capture path, then drop the singleton.
    SetPassthrough(true);
    m_soundVmt.UnhookAll();
    m_vmt.UnhookAll();
    g_hooks.store(nullptr, std::memory_order_seq_cst);
    while (g_hookReaders.load(std::memory_order_seq_cst) != 0)
        std::this_thread::yield();
    UninstallEmitSoundHook();
    m_vmt.Detach();
    m_installed = false;
    m_capture = nullptr;
    m_interfaces = nullptr;
    SA_LOGI("[hooks] IAudioDevice hooks removed");
}

void EngineHooks::SetExternalListener(const Vec3& position, const Vec3& forward, const Vec3& right, const Vec3& up)
{
    const uint64_t now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    if (m_listenerFromEngine.load(std::memory_order_relaxed) && now - m_listener.Load().stamp < 250000)
        return; // the engine hook is authoritative once it has reported
    EngineListener l;
    l.position = position;
    l.forward = forward;
    l.right = right;
    l.up = up;
    l.stamp = now;
    l.valid = position.IsFinite() && forward.IsFinite() && right.IsFinite() && up.IsFinite();
    m_externalListener.Store(l);
}

float EngineHooks::PanFromDirection(const Vec3& dir, bool right)
{
    // Engine passes the source direction in listener space (x = right).
    const float x = std::max(-1.f, std::min(1.f, dir.x));
    const float angle = (x + 1.f) * 0.25f * 3.14159265f; // 0..pi/2
    return right ? std::sin(angle) : std::cos(angle);
}

void EngineHooks::PollActiveSounds(const RuntimeConfig& cfg, int32_t localPlayer, double now)
{
    if (!m_capture)
        return;
    m_now = std::max(m_now, now);
    m_overrides.BeginPoll(m_now);
    src::IEngineSound* snd = m_interfaces ? m_interfaces->EngineSound() : nullptr;
    m_activeSounds.clear();
    if (snd && !m_interfaces->IsDedicatedServer()) {
        m_sndList.Clear();
        snd->GetActiveSounds(m_sndList);
        if (m_sndList.Valid()) {
            const int32_t n = std::min(m_sndList.Count(), m_sndList.Capacity());
            m_activeSounds.reserve(static_cast<size_t>(n));
            for (int32_t i = 0; i < n; ++i) {
                const src::SndInfo_t& s = m_sndList[i];
                ActiveSoundInfo info;
                info.guid = s.m_nGuid;
                info.entity = s.m_nSoundSource;
                info.channel = s.m_nChannel;
                info.volume = std::isfinite(s.m_flVolume) ? std::max(0.f, std::min(1.f, s.m_flVolume)) : 1.f;
                info.pitch = s.m_nPitch;
                info.fromServer = s.m_bFromServer;
                info.dryMix = s.m_bDryMix;
                info.speaker = s.m_bSpeaker;
                info.sentence = s.m_bIsSentence;
                info.updatePositions = s.m_bUpdatePositions;
                info.originPtr = reinterpret_cast<uintptr_t>(s.m_pOrigin);
                info.fileHandle = reinterpret_cast<uintptr_t>(s.m_filenameHandle);
                if (s.m_pOrigin && IsMemoryReadable(info.originPtr, sizeof(src::Vector))) {
                    info.origin = ToVec3(*s.m_pOrigin);
                    info.hasOrigin = std::isfinite(info.origin.x) && std::isfinite(info.origin.y) &&
                                     std::isfinite(info.origin.z);
                }
                m_activeSounds.push_back(std::move(info));
            }
        }
    }

    // Sound names: from the EmitSound hook (client-started sounds) or, for
    // networked sounds, from the file name handle when a resolver is wired.
    for (ActiveSoundInfo& info : m_activeSounds) {
        if (info.guid == 0)
            continue;
        if (!m_overrides.NameForGuid(info.guid, info.name)) {
            info.name.clear();
            std::string raw;
            if (m_nameResolver && info.fileHandle != 0 &&
                m_nameResolver(reinterpret_cast<const void*>(info.fileHandle), raw)) {
                info.name = NormalizeSoundName(raw.c_str());
                ++m_namesFromHandles;
            }
        }
        m_overrides.Observe(info.guid, info.entity, info.channel, info.name.empty() ? nullptr : info.name.c_str());
    }

    // Layout inference from the public metadata.
    for (const ActiveSoundInfo& info : m_activeSounds) {
        if (info.originPtr == 0)
            continue;
        m_capture->VoteOriginAnchor(info.originPtr);
        m_capture->VoteGuidAnchor(info.guid, info.originPtr);
    }
    m_capture->RunLayoutInference();

    // Join active sounds to capture slots through the origin pointer.
    const ChannelLayout& layout = m_capture->Layout();
    if (layout.HaveOrigin()) {
        for (ActiveSoundInfo& info : m_activeSounds) {
            if (info.originPtr == 0)
                continue;
            const uintptr_t ch = info.originPtr - static_cast<uintptr_t>(layout.origin);
            info.slot = m_capture->FindSlotByPointer(ch);
        }
    }

    // Update every bound slot.
    for (uint32_t i = 0; i < m_capture->SlotCount(); ++i) {
        const CaptureSlot& slot = m_capture->Slot(i);
        const uintptr_t ch = slot.enginePtr.load(std::memory_order_acquire);
        if (ch == 0 || !slot.source)
            continue;
        ActiveSoundInfo* match = nullptr;
        for (ActiveSoundInfo& info : m_activeSounds) {
            if (info.slot == static_cast<int32_t>(i)) {
                match = &info;
                break;
            }
        }
        ApplyParamsForSlot(i, match, cfg, localPlayer);
    }
    m_overrides.EndPoll();
}

bool IsLocalWeaponSound(const ActiveSoundInfo& info, int32_t localPlayer)
{
    return info.channel == kChanWeapon &&
           (info.entity == src::kSoundFromLocalPlayer || (localPlayer > 0 && info.entity == localPlayer));
}

float EngineChannelGain(const ActiveSoundInfo& info, int32_t localPlayer)
{
    return Clamp(info.volume, 0.f, 1.f) * (IsLocalWeaponSound(info, localPlayer) ? DbToLinear(2.f) : 1.f);
}

void EngineHooks::ApplyParamsForSlot(uint32_t slotIndex, ActiveSoundInfo* info, const RuntimeConfig& cfg,
                                     int32_t localPlayer)
{
    CaptureSlot& slot = m_capture->Slot(slotIndex);
    SoundSource& source = *slot.source;
    SlotTrack& track = m_slotTrack[slotIndex];
    const uint32_t generation = slot.generation.load(std::memory_order_acquire);
    const uintptr_t ch = slot.enginePtr.load(std::memory_order_acquire);

    SourceParams p = source.GetParams();
    if (generation != track.generation) {
        // New sound on this channel: start from defaults.
        p = SourceParams{};
        track.generation = generation;
        track.guid = 0;
        track.hadInfo = false;
    }

    // Engine-side distance/occlusion gain and panning (used when no position is
    // known). The channel volume itself comes from the sound info (p.gain).
    const float engineGain = std::max(0.f, std::min(4.f, slot.engineGain.load(std::memory_order_relaxed)));
    const Vec3 dir{slot.engineDirX.load(std::memory_order_relaxed), slot.engineDirY.load(std::memory_order_relaxed),
                   slot.engineDirZ.load(std::memory_order_relaxed)};
    const EngineListener listener = GetListener();
    const Vec3 relative{listener.valid ? dir.Dot(listener.right.Normalized()) : 0.f, 0.f, 0.f};

    // Guid tracking through the inferred layout: a changed guid on the same
    // channel_t means a new sound even if the mixer never paused.
    int32_t guid = 0;
    if (m_capture->ReadGuid(ch, guid) && guid != 0) {
        if (track.guid != 0 && guid != track.guid) {
            p = SourceParams{};
            track.hadInfo = false;
        }
        track.guid = guid;
    }
    p.engineGainL = engineGain * PanFromDirection(relative, false) * 1.41421356f;
    p.engineGainR = engineGain * PanFromDirection(relative, true) * 1.41421356f;
    p.engineDirectGain = engineGain;
    p.engineGainValid = slot.engineGainKnown.load(std::memory_order_acquire) ? 1 : 0;

    // Per-sound overrides (Lua). Resolved up front because an override that
    // supplies a position (or forces spatialization) changes how the
    // engine-derived flags below are computed; the value merge happens last.
    const int32_t overrideGuid = info ? info->guid : track.guid;
    const int32_t overrideEntity = info ? info->entity : p.entityIndex;
    SoundOverride ov;
    const bool overridden = m_overrides.Resolve(overrideGuid, overrideEntity, ov);
    const bool ovPosition = overridden && ov.Has(SoundOverride::kPosition);
    const bool ovSpatialize = overridden && ov.Has(SoundOverride::kSpatialize);

    bool spatialize = false;
    if (info) {
        track.hadInfo = true;
        p.entityIndex = info->entity;
        p.channel = info->channel;
        p.gain = EngineChannelGain(*info, localPlayer);
        p.listenerRelative = IsLocalWeaponSound(*info, localPlayer) && !ovPosition ? 1 : 0;
        p.pitch = std::max(1, info->pitch) / 100.f;
        p.fromServer = info->fromServer ? 1 : 0;
        if (info->hasOrigin) {
            p.position = info->origin;
            p.positionValid = 1;
        }
        const bool isVoice = info->channel >= kChanVoiceBase && info->channel < kChanUserBase;
        const bool isLocalEntity = info->entity == src::kSoundFromLocalPlayer ||
                                   (localPlayer > 0 && info->entity == localPlayer);
        const bool isUi = info->entity == src::kSoundFromUI || (info->dryMix && info->entity <= 0);
        const bool hasOrigin = info->hasOrigin || ovPosition;
        const bool zeroOrigin = !ovPosition && info->origin.LengthSq() == 0.f && info->entity <= 0 &&
                                !info->updatePositions;
        spatialize = !isUi;
        if (isLocalEntity && info->channel != kChanWeapon && info->updatePositions && !ovPosition)
            spatialize = false; // follows the listener: engine plays it "in the head"
        if (isVoice && (!cfg.voiceSpatial || isLocalEntity))
            spatialize = false;
        if (!hasOrigin || zeroOrigin)
            spatialize = false;
        p.radiusMeters = cfg.sourceRadius;
    } else if (!track.hadInfo) {
        // No metadata yet (layout not inferred or list not available): play
        // through the engine's own gains, unspatialized, so nothing is lost.
        // An override that supplies a position is enough to spatialize.
        p.positionValid = m_capture->ReadOrigin(ch, p.position) ? 1 : 0;
        p.gain = static_cast<float>(std::min<uint32_t>(255, slot.engineMasterVol.load(std::memory_order_relaxed))) / 255.f;
        p.radiusMeters = cfg.sourceRadius;
        spatialize = ovPosition || p.positionValid;
    } else {
        spatialize = p.spatialize != 0;
    }
    if (p.listenerRelative && listener.valid) {
        p.position = listener.position;
        p.positionValid = 1;
        p.engineGainL = p.engineGainR = 1.f;
        spatialize = true;
    }
    if (ovSpatialize)
        spatialize = ov.spatialize != 0;
    p.spatialize = spatialize ? 1 : 0;
    p.occlusion = spatialize && !p.listenerRelative && cfg.occlusion ? 1 : 0;
    p.transmission = spatialize && !p.listenerRelative && cfg.transmission ? 1 : 0;
    p.reflections = spatialize && cfg.reflections ? 1 : 0;
    p.pathing = spatialize && !p.listenerRelative && cfg.pathing ? 1 : 0;

    // Distance attenuation multiplier from the private layout when available,
    // else the engine default for SNDLVL_NORM.
    float distMult = 0.f;
    if (m_capture->ReadDistMult(ch, distMult)) {
        p.distMult = distMult;
    } else if (p.spatialize) {
        p.distMult = SoundLevelToDistMult(static_cast<float>(src::kSndlvlNorm));
    }
    if (p.listenerRelative)
        p.distMult = 0.f;
    p.airAbsorptionScale = 1.f;
    p.reverbGain = 1.f;

    // Automatic directivity: weapon and voice channels radiate along the
    // emitting entity's facing (its snapshot angles; the listener's own view
    // for the local player). Unknown facing => omnidirectional; explicit
    // overrides win below.
    const bool wasDirectional = p.dipoleWeight > 0.f;
    DirectivityHint hint;
    Vec3 forward;
    bool haveForward = false;
    if (spatialize && !p.listenerRelative && info && info->entity > 0) {
        hint = InferDirectivity(cfg, info->channel, info->sentence);
        if (hint.Directional()) {
            if (localPlayer > 0 && info->entity == localPlayer) {
                const EngineListener l = GetListener();
                haveForward = l.valid;
                forward = l.forward;
            } else if (m_forwardResolver) {
                haveForward = m_forwardResolver(info->entity, forward);
            }
        }
    }
    if (ApplyDirectivity(p, hint, haveForward, forward) && !wasDirectional)
        ++m_autoDirectivity;

    // An emitter inside its own entity's occluder hull (NPC voice/footsteps
    // at the hull center) is moved out toward the listener so the hull does
    // not occlude it.
    if (spatialize && p.positionValid && info && info->entity > 0 && !ovPosition && m_hullResolver &&
        !(localPlayer > 0 && info->entity == localPlayer)) {
        const EngineListener l = GetListener();
        if (l.valid) {
            const float margin = p.radiusMeters * cfg.unitsPerMeter + std::max(0.f, cfg.emitterHullMarginUnits);
            if (m_hullResolver(info->entity, l.position, margin, p.position))
                ++m_hullPushes;
        }
    }

    if (overridden)
        ov.Apply(p);
    if (info) {
        info->spatialized = p.spatialize != 0;
        info->overridden = overridden;
    }

    // The engine already applied its own volume/master through master_vol; we
    // apply channel volume ourselves, and the engine "volume" convar through
    // cfg.engineVolume at the master stage.
    source.SetEngineParams(p, overrideGuid);
}

} // namespace sa
