// src/core/EngineHooks.h
//
// Installs the engine-side hooks:
//
//   IAudioDevice vtable (engine.dll, resolved by EngineInterfaces):
//     PaintBegin / MixBegin / TransferSamples  -> paint clock bookkeeping
//     SpatializeChannel                        -> capture engine gain, defeat
//                                                 engine culling/panning
//     Mix8Mono/Mix8Stereo/Mix16Mono/Mix16Stereo-> PCM capture (engine paint
//                                                 buffer is left untouched)
//     ApplyDSPEffects                          -> suppressed (engine DSP off)
//     UpdateListener                           -> listener pose
//     TransferSamples (engine output mode)     -> our stereo mix written into
//                                                 the paint buffer
//
//   IEngineSound (public) is polled every game frame for the active sound
//   list which carries entity/channel/volume/origin metadata that is joined to
//   captured channels through ChannelCapture's layout inference. Its two
//   EmitSound overloads are additionally VMT-hooked (pass-through) to learn
//   the sample name of every client-started sound for the per-sound override
//   rules (SoundOverrides.h); networked sounds get their name through the
//   optional IFileSystem::String resolver instead.
//
// All hooks honour a passthrough switch: when set, the original engine
// behaviour is restored live (used by snd_sa_enabled 0 and during teardown).
//
// Thread-safety: Install/Uninstall/PollActiveSounds on the game thread; the
// static hook trampolines run on the engine mixer thread; GetListener may be
// called from any thread (SeqLock).
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/ChannelCapture.h"
#include "core/EngineInterfaces.h"
#include "core/SoundOverrides.h"
#include "core/VmtHook.h"
#include "mixing/LockFreeQueue.h"
#include "mixing/SoundSource.h"
#include "steamaudio/Config.h"
#include "util/Math.h"

namespace sa {

// Listener pose as reported by the engine (Source units/axes).
struct EngineListener {
    Vec3 position{};
    Vec3 forward{1.f, 0.f, 0.f};
    Vec3 right{0.f, -1.f, 0.f};
    Vec3 up{0.f, 0.f, 1.f};
    uint64_t stamp = 0; // paint clock when received
    uint8_t valid = 0;
};

// Sink for the "engine" output mode: fills the engine paint buffer with our
// rendered stereo mix. Runs on the mixer thread.
class IEngineOutputSink {
public:
    virtual ~IEngineOutputSink() = default;
    // Writes `frames` interleaved int32 sample pairs (engine paint format,
    // 16-bit range) for absolute clock `clock`. Missing data must be zeroed.
    virtual void FillPaintBuffer(uint64_t clock, src::portable_samplepair_t* out, size_t frames) = 0;
};

struct ActiveSoundInfo {
    int32_t guid = 0;
    int32_t entity = -1;
    int32_t channel = 0;
    float volume = 1.f;
    int32_t pitch = 100;
    Vec3 origin{};
    bool hasOrigin = false;
    bool fromServer = false;
    bool dryMix = false;
    bool speaker = false;
    bool sentence = false;
    bool updatePositions = false;
    uintptr_t originPtr = 0;
    uintptr_t fileHandle = 0; // SndInfo_t::m_filenameHandle (opaque)
    int32_t slot = -1;     // capture slot bound to this sound (-1 = not yet mixed)
    bool spatialized = false; // what we ended up doing with it (bound slots only)
    bool overridden = false;  // a per-sound override applied
    std::string name;      // normalized sample path when known, else empty
};

bool IsLocalWeaponSound(const ActiveSoundInfo& info, int32_t localPlayer);
float EngineChannelGain(const ActiveSoundInfo& info, int32_t localPlayer);

// Resolves SndInfo_t::m_filenameHandle to a path (EngineFileSystem::FileNameFromHandle).
using SoundNameResolver = std::function<bool(const void* handle, std::string& out)>;
// Resolves an entity index to its facing direction (Source units, unit
// length). Returns false when unknown; used for automatic directivity.
using EntityForwardResolver = std::function<bool(int32_t entity, Vec3& forward)>;
// Moves an emitter position out of its own entity's occluder hull toward the
// listener; `margin` is the clearance in Source units. Returns true when moved.
using EmitterHullResolver = std::function<bool(int32_t entity, const Vec3& listener, float margin, Vec3& position)>;

class EngineHooks {
public:
    EngineHooks();
    ~EngineHooks();
    EngineHooks(const EngineHooks&) = delete;
    EngineHooks& operator=(const EngineHooks&) = delete;

    // Installs the mixer hooks. Returns false (and leaves the engine untouched)
    // when the device could not be validated.
    bool Install(EngineInterfaces& interfaces, ChannelCapture& capture, std::string& error);
    void Uninstall();
    bool Installed() const { return m_installed; }

    // Engine output mode plumbing; sink may be null (native output).
    void SetOutputSink(IEngineOutputSink* sink) { m_outputSink.store(sink, std::memory_order_release); }
    void SetEngineOutputEnabled(bool enabled) { m_engineOutput.store(enabled, std::memory_order_release); }
    bool EngineOutputPossible() const { return m_paintBuffer != nullptr; }

    // Passthrough: the engine mixes/outputs on its own, our capture is idle.
    void SetPassthrough(bool passthrough) { m_passthrough.store(passthrough, std::memory_order_release); }
    bool Passthrough() const { return m_passthrough.load(std::memory_order_acquire); }

    EngineListener GetListener() const
    {
        const EngineListener native = m_listener.Load();
        const EngineListener external = m_externalListener.Load();
        return external.valid && external.stamp > native.stamp ? external : native;
    }
    // Listener provided by Lua (render.GetViewSetup) when the engine hook is unavailable.
    void SetExternalListener(const Vec3& position, const Vec3& forward, const Vec3& right, const Vec3& up);

    // Game thread: polls IEngineSound::GetActiveSounds and updates the
    // SourceParams of every bound capture slot. `localPlayer` is the local
    // player's entity index (-1 unknown); `now` is a monotonic clock in
    // seconds used for override expiry.
    void PollActiveSounds(const RuntimeConfig& cfg, int32_t localPlayer, double now);
    const std::vector<ActiveSoundInfo>& ActiveSounds() const { return m_activeSounds; }

    // Per-sound overrides (Lua-facing table). Game thread.
    SoundOverrideTable& Overrides() { return m_overrides; }
    const SoundOverrideTable& Overrides() const { return m_overrides; }
    void SetSoundNameResolver(SoundNameResolver resolver) { m_nameResolver = std::move(resolver); }
    void SetEntityForwardResolver(EntityForwardResolver resolver) { m_forwardResolver = std::move(resolver); }
    void SetEmitterHullResolver(EmitterHullResolver resolver) { m_hullResolver = std::move(resolver); }

    // Stats for the Lua debug surface.
    uint64_t AutoDirectivityApplied() const { return m_autoDirectivity; }
    uint64_t EmitterHullPushes() const { return m_hullPushes; }
    uint64_t HookCalls() const { return m_hookCalls.load(std::memory_order_relaxed); }
    bool ListenerFromEngine() const
    {
        const EngineListener native = m_listener.Load();
        const EngineListener external = m_externalListener.Load();
        return native.valid && (!external.valid || native.stamp >= external.stamp);
    }
    bool EmitSoundHooked() const { return m_emitSoundHooked; }
    uint64_t EmitSoundCalls() const { return m_emitSoundCalls.load(std::memory_order_relaxed); }
    uint64_t NamesFromHandles() const { return m_namesFromHandles; }

private:
    friend struct HookTrampolines;

    void ApplyParamsForSlot(uint32_t slotIndex, ActiveSoundInfo* info, const RuntimeConfig& cfg,
                            int32_t localPlayer);
    static float PanFromDirection(const Vec3& dir, bool right);
    void InstallEmitSoundHook();
    void UninstallEmitSoundHook();
    // Called from the EmitSound trampolines after the engine started the sound.
    void OnSoundEmitted(void* engineSound, int32_t entity, int32_t channel, const char* sample);

    EngineInterfaces* m_interfaces = nullptr;
    ChannelCapture* m_capture = nullptr;
    VmtHook m_vmt;
    src::AudioDeviceVTable m_slots;
    bool m_installed = false;
    src::portable_samplepair_t* m_paintBuffer = nullptr;

    // IEngineSound::EmitSound (pass-through, name capture).
    VmtHook m_soundVmt;
    bool m_emitSoundHooked = false;
    std::atomic<uint64_t> m_emitSoundCalls{0};
    int32_t m_lastEmittedGuid = 0;

    std::atomic<bool> m_passthrough{false};
    std::atomic<bool> m_engineOutput{false};
    std::atomic<IEngineOutputSink*> m_outputSink{nullptr};
    std::atomic<uint64_t> m_hookCalls{0};
    std::atomic<bool> m_listenerFromEngine{false};
    SeqLock<EngineListener> m_listener;
    SeqLock<EngineListener> m_externalListener;

    // Game-thread state.
    SoundOverrideTable m_overrides;
    SoundNameResolver m_nameResolver;
    EntityForwardResolver m_forwardResolver;
    EmitterHullResolver m_hullResolver;
    uint64_t m_namesFromHandles = 0;
    uint64_t m_autoDirectivity = 0;
    uint64_t m_hullPushes = 0;
    double m_now = 0.0;
    std::vector<ActiveSoundInfo> m_activeSounds;
    struct SlotTrack {
        uint32_t generation = 0;
        int32_t guid = 0;
        bool hadInfo = false;
    };
    std::array<SlotTrack, ChannelCapture::kSlots> m_slotTrack{};
    src::UtlVectorView<src::SndInfo_t> m_sndList{512};
};

} // namespace sa
