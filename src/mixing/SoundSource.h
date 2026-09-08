// src/mixing/SoundSource.h
//
// Per-source state shared between the game thread (parameter updates, PCM
// capture from the engine mixer hook), the BASS DSP thread (PCM capture for
// IGModAudioChannel streams), the simulation thread (IPLSource inputs) and the
// audio thread (rendering).
//
// Ownership: SoundSource objects are reference counted. The game thread owns
// the registry; the simulation and audio threads keep their own shared_ptr
// copies which are returned through `ReleaseQueue`s so destruction always
// happens on the game thread.
//
// Thread-safety summary (see field comments):
//   params        SeqLock: written by game thread, read by sim + audio threads
//   ring buffers  SPSC: one producer (engine hook / BASS / Lua), consumer = audio thread
//   simulation    accessed only by the simulation thread (IPLSource handle)
//   render        accessed only by the audio thread (effects, per-source state)
//   atomics       shared control flags
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "steamaudio/PhononApi.h"

#include "Dsp.h"
#include "LockFreeQueue.h"
#include "util/Math.h"

namespace sa {

enum class SourceKind : uint8_t {
    EngineChannel = 0, // Source engine channel_t (world/weapon/voice/ambient/UI/Lua EmitSound)
    BassStream = 1,    // IGModAudioChannel (sound.PlayFile / sound.PlayURL) via BASS
    Procedural = 2,    // Lua-fed live PCM stream (steamaudio.CreateStream)
};

// Source engine channel enumeration (const.h) - used for policy decisions.
enum SourceChannel : int32_t {
    kChanReplace = -1,
    kChanAuto = 0,
    kChanWeapon = 1,
    kChanVoice = 2,
    kChanItem = 3,
    kChanBody = 4,
    kChanStream = 5,
    kChanStatic = 6,
    kChanVoiceBase = 7,
    kChanUserBase = 136,
};

// Trivially copyable snapshot published by the game thread through a SeqLock.
struct SourceParams {
    Vec3 position{};              // Source world units
    Vec3 forward{1.f, 0.f, 0.f};  // directivity axis, Source space
    float gain = 1.f;             // linear, non-spatial: channel volume * master volume
    float engineGainL = 1.f;      // last spatialized per-speaker gains reported by the engine mixer
    float engineGainR = 1.f;
    float distMult = 0.f;         // Source attenuation multiplier (0 = no distance attenuation)
    float radiusMeters = 0.f;     // volumetric occlusion radius
    float dipoleWeight = 0.f;     // directivity: 0 = omni, 1 = full dipole
    float dipolePower = 1.f;
    float airAbsorptionScale = 1.f;
    float reverbGain = 1.f;
    float pitch = 1.f;
    int32_t entityIndex = -1;
    int32_t channel = kChanAuto;
    uint8_t spatialize = 1;       // 0 => 2D passthrough (UI, music, unspatialized stereo)
    uint8_t occlusion = 1;
    uint8_t transmission = 1;
    uint8_t reflections = 1;
    uint8_t pathing = 1;
    uint8_t fromServer = 0;
    uint8_t positionValid = 0;    // 0 while the position is unknown (falls back to engine gains)
    uint8_t listenerRelative = 0;
};

// What the audio thread last did with a source (published for diagnostics,
// see steamaudio.SoundLines / snd_sa_sounds).
struct RenderDiag {
    enum Mode : uint8_t {
        kIdle = 0,        // not rendered this frame
        kPassthrough = 1, // 2D: params say non-spatial or position unknown
        kNoEffects = 2,   // wanted spatial but the effect pool was exhausted
        kHrtf = 3,
        kPanning = 4,
        kFallbackMixer = 5,
        kListenerRelative = 6,
    };
    Vec3 direction{};              // listener-relative unit vector (Steam Audio axes: +x right, -z ahead)
    float distanceMeters = 0.f;
    float distanceAttenuation = 1.f;
    float occlusion = 1.f;         // occlusion applied by the direct effect (after shaping)
    float occlusionRaw = 1.f;      // visibility reported by the simulator
    float inputLevel = 0.f;        // mean square of the (mono) input this frame
    float outputLevel = 0.f;       // mean square of what was added to the master (per channel)
    float peakOutputLevel = 0.f;   // largest outputLevel since the source began
    float minOcclusionRaw = 1.f;   // lowest simulator visibility since the source began
    float gain = 0.f;              // overall gain applied (fade * channel volume * config)
    uint32_t framesRendered = 0;   // frames handed to the renderer since the source began
    uint32_t framesNoData = 0;     // frames without captured PCM (silence fed / skipped)
    uint32_t framesReflections = 0; // frames that fed the reflection effect
    uint32_t framesPathing = 0;    // frames that fed the path effect
    uint8_t mode = kIdle;
    uint8_t simOutputs = 0;        // direct simulation results were available
    uint8_t reserved[2]{};
};

// Effect objects used to render one source on the audio thread. Pooled and
// pre-allocated (see EffectPool) so the audio thread never allocates.
struct EffectSet {
    IPLDirectEffect direct = nullptr;
    IPLBinauralEffect binaural = nullptr;
    IPLPanningEffect panning = nullptr;
    IPLReflectionEffect reflection = nullptr;
    IPLPathEffect path = nullptr;
    IPLAmbisonicsDecodeEffect ambisonicsDecode = nullptr;
    IPLAmbisonicsEncodeEffect ambisonicsEncode = nullptr;
    bool valid = false;

    void Reset() const
    {
        if (direct)
            iplDirectEffectReset(direct);
        if (binaural)
            iplBinauralEffectReset(binaural);
        if (panning)
            iplPanningEffectReset(panning);
        if (reflection)
            iplReflectionEffectReset(reflection);
        if (path)
            iplPathEffectReset(path);
        if (ambisonicsDecode)
            iplAmbisonicsDecodeEffectReset(ambisonicsDecode);
        if (ambisonicsEncode)
            iplAmbisonicsEncodeEffectReset(ambisonicsEncode);
    }
};

class SoundSource {
public:
    static constexpr size_t kMaxInputChannels = 2;
    // Longest modelled propagation delay (~120 m at 343 m/s); farther sources
    // keep this delay and lose the Doppler shift.
    static constexpr float kMaxPropagationDelaySeconds = 0.35f;
    static constexpr size_t kMaxRenderBlock = 4096;

    // `engineSampleRate` is the mixing rate (engine DMA rate, 44100).
    // For streamed kinds `streamSampleRate` is the producer's native rate.
    SoundSource(uint32_t id, SourceKind kind, uint32_t inputChannels, uint32_t engineSampleRate,
                uint32_t streamSampleRate, size_t ringCapacitySamples);
    ~SoundSource();

    SoundSource(const SoundSource&) = delete;
    SoundSource& operator=(const SoundSource&) = delete;

    uint32_t Id() const { return m_id; }
    SourceKind Kind() const { return m_kind; }
    uint32_t InputChannels() const { return m_inputChannels; }
    uint32_t StreamSampleRate() const { return m_streamSampleRate; }
    uint32_t EngineSampleRate() const { return m_engineSampleRate; }

    // ---- Parameters (game thread writes, others read) ----------------------
    void SetParams(const SourceParams& p)
    {
        const ParamSnapshot captured = m_captureParams.Load();
        m_params.Store(ParamSnapshot{SanitizeParams(p), captured.generation, captured.guid});
    }
    void SetEngineParams(const SourceParams& p, int32_t guid)
    {
        m_params.Store(ParamSnapshot{SanitizeParams(p), m_captureParams.Load().generation, guid});
    }
    void SetCapturedParams(const SourceParams& p, uint32_t generation, int32_t guid = 0)
    {
        m_captureParams.Store(ParamSnapshot{SanitizeParams(p), generation, guid});
    }
    SourceParams GetParams() const
    {
        const ParamSnapshot game = m_params.Load();
        if (m_kind != SourceKind::EngineChannel)
            return game.params;
        const ParamSnapshot captured = m_captureParams.Load();
        const bool same = game.guid != 0 && captured.guid != 0 ? game.guid == captured.guid
                                                              : game.generation == captured.generation;
        SourceParams p = same ? game.params : captured.params;
        if (!p.positionValid && captured.params.positionValid) {
            p.position = captured.params.position;
            p.positionValid = 1;
        }
        return p;
    }

    // ---- Lifetime flags ------------------------------------------------------
    // Set by the game thread when the source should stop; the audio thread
    // fades out and then reports `RenderFinished`.
    void RequestStop() { m_stopRequested.store(true, std::memory_order_release); }
    bool StopRequested() const { return m_stopRequested.load(std::memory_order_acquire); }
    void MarkRenderFinished() { m_renderFinished.store(true, std::memory_order_release); }
    bool RenderFinished() const { return m_renderFinished.load(std::memory_order_acquire); }

    // Last time (engine clock or wall clock, see AudioEngine) data was written.
    void TouchActivity(uint64_t stamp) { m_lastActivity.store(stamp, std::memory_order_relaxed); }
    uint64_t LastActivity() const { return m_lastActivity.load(std::memory_order_relaxed); }

    // ---- Engine channel capture (TimedSampleRing) ------------------------------
    // Valid only for SourceKind::EngineChannel.
    TimedSampleRing* TimedRing(uint32_t channel)
    {
        return channel < m_timedRings.size() ? m_timedRings[channel].get() : nullptr;
    }
    void ResetTimedRings(uint64_t clock);

    // ---- Streamed capture (SpscSampleRing + resampler) ------------------------
    // Valid for BassStream / Procedural. Interleaved input is split per channel.
    SpscSampleRing* StreamRing(uint32_t channel)
    {
        return channel < m_streamRings.size() ? m_streamRings[channel].get() : nullptr;
    }
    // Producer helper: writes interleaved samples, returns frames written.
    size_t WriteInterleaved(const float* interleaved, size_t frames);
    // Consumer helper (audio thread): pulls `frames` frames at the engine
    // rate into deinterleaved buffers, resampling and drift-correcting.
    // Returns the number of frames actually produced (rest is zero-filled).
    size_t ReadStreamFrames(float* const* out, size_t frames);

    // Marks that the producer has finished (no more data will be written);
    // the audio thread finishes when the ring drains.
    void SetEndOfStream() { m_endOfStream.store(true, std::memory_order_release); }
    bool EndOfStream() const { return m_endOfStream.load(std::memory_order_acquire); }
    size_t QueuedStreamFrames() const;
    // Preferred number of frames the producer should keep queued.
    size_t StreamTargetFrames() const { return m_streamTargetFrames; }
    // Playback-rate multiplier applied on top of the stream/engine rate ratio
    // (producer delivers `scale` times more frames per second than nominal).
    void SetStreamRateScale(float scale) { m_streamRateScale.store(scale, std::memory_order_relaxed); }
    float StreamRateScale() const { return m_streamRateScale.load(std::memory_order_relaxed); }

    // ---- Simulation-thread state --------------------------------------------------
    struct SimulationState {
        IPLSource source = nullptr;
        bool added = false;
        IPLSimulationFlags flags = static_cast<IPLSimulationFlags>(0);
        uint64_t lastReflectionsRun = 0;
        uint64_t lastPathingRun = 0;
        uint64_t lastActive = 0;      // sim tick when the source was last seen active
        uint32_t generation = 0;      // engine slot generation the IPLSource belongs to
        Vec3 lastPosition{};
        bool everSimulated = false;
        // Parameters of the Source-engine distance model evaluated by the
        // IPLDistanceAttenuationCallback (userData points at this struct).
        struct DistanceModel {
            float distMult = 0.f;
            float unitsPerMeter = kDefaultUnitsPerMeter;
            float gainMin = 0.01f;
            float gainMax = 1.f;
        } distance;
    };
    SimulationState& Sim() { return m_sim; }

    // Simulation results are read by the audio thread directly through
    // iplSourceGetOutputs (Steam Audio double-buffers them internally). The
    // simulation thread publishes a retained IPLSource handle for that purpose
    // and only releases it after the audio thread can no longer observe it
    // (see SimulationThread's deferred release list).
    void PublishSimulationSource(IPLSource source) { m_audioSimSource.store(source, std::memory_order_release); }
    IPLSource AudioSimulationSource() const { return m_audioSimSource.load(std::memory_order_acquire); }
    void MarkSimulationReady(IPLSimulationFlags flags)
    {
        m_simReadyFlags.store(static_cast<uint32_t>(flags), std::memory_order_release);
    }
    IPLSimulationFlags SimulationReadyFlags() const
    {
        return static_cast<IPLSimulationFlags>(m_simReadyFlags.load(std::memory_order_acquire));
    }

    // ---- Audio-thread state -------------------------------------------------------
    struct RenderState {
        EffectSet* effects = nullptr;
        float currentGain = 0.f;      // smoothed overall gain
        bool gainInitialized = false;
        float fadeGain = 0.f;         // 0..1 fade-in/out envelope
        float fadeStart = 1.f;
        bool fadingOut = false;
        uint64_t silentFrames = 0;    // consecutive frames with no data
        bool everHadData = false;
        uint32_t framesRendered = 0;
        uint32_t framesNoData = 0;
        uint32_t framesReflections = 0;
        uint32_t framesPathing = 0;
        float peakOutputLevel = 0.f;
        float minOcclusionRaw = 1.f;
        IPLSimulationOutputs outputs{};
        bool outputsValid = false;
        SourceParams lastParams{};
        double streamRatio = 1.0;      // drift-corrected resample ratio
        dsp::PropagationDelay doppler; // propagation delay line (mono, after downmix)
        bool roomSent = false;         // sent to the room reverb this frame (stats)
        struct {
            float gainL = 0.f;         // smoothed per-speaker gains
            float gainR = 0.f;
            float lowpassState = 0.f;  // one-pole filter memory
            float lowpassCoef = 1.f;   // smoothed cutoff coefficient (1 = bypass)
            bool primed = false;
        } fallback;
    };
    RenderState& Render() { return m_render; }
    void PublishRenderDiag(const RenderDiag& d) { m_renderDiag.Store(d); } // audio thread
    RenderDiag LoadRenderDiag() const { return m_renderDiag.Load(); }

    // Debug name (engine sound name / BASS file / stream label).
    void SetName(const std::string& name) { m_name = name; } // game thread only
    const std::string& Name() const { return m_name; }

private:
    const uint32_t m_id;
    const SourceKind m_kind;
    const uint32_t m_inputChannels;
    const uint32_t m_engineSampleRate;
    const uint32_t m_streamSampleRate;
    std::string m_name;

    static SourceParams SanitizeParams(SourceParams p)
    {
        if (p.positionValid && (!p.position.IsFinite() || p.position.LengthSq() > 1e12f)) {
            p.positionValid = 0;
            p.gain = 0.f;
        }
        if (!p.forward.IsFinite() || !std::isfinite(p.forward.LengthSq()) || p.forward.LengthSq() < 1e-6f)
            p.forward = {1.f, 0.f, 0.f};
        p.gain = Clamp(p.gain, 0.f, 4.f);
        p.engineGainL = Clamp(p.engineGainL, 0.f, 8.f);
        p.engineGainR = Clamp(p.engineGainR, 0.f, 8.f);
        p.distMult = Clamp(p.distMult, 0.f, 1e6f);
        p.radiusMeters = Clamp(p.radiusMeters, 0.f, 10.f);
        p.dipoleWeight = Clamp(p.dipoleWeight, 0.f, 1.f);
        p.dipolePower = Clamp(p.dipolePower, 0.f, 8.f);
        p.airAbsorptionScale = Clamp(p.airAbsorptionScale, 0.f, 4.f);
        p.reverbGain = Clamp(p.reverbGain, 0.f, 4.f);
        p.pitch = Clamp(p.pitch, 0.05f, 20.f);
        return p;
    }

    struct ParamSnapshot {
        SourceParams params;
        uint32_t generation = 0;
        int32_t guid = 0;
    };
    SeqLock<ParamSnapshot> m_params;
    SeqLock<ParamSnapshot> m_captureParams;
    SeqLock<RenderDiag> m_renderDiag;
    std::atomic<IPLSource> m_audioSimSource{nullptr};
    std::atomic<uint32_t> m_simReadyFlags{0};
    std::atomic<bool> m_stopRequested{false};
    std::atomic<bool> m_renderFinished{false};
    std::atomic<bool> m_endOfStream{false};
    std::atomic<float> m_streamRateScale{1.f};
    std::atomic<uint64_t> m_lastActivity{0};

    std::vector<std::unique_ptr<TimedSampleRing>> m_timedRings;
    std::vector<std::unique_ptr<SpscSampleRing>> m_streamRings;
    std::array<dsp::Resampler, kMaxInputChannels> m_resamplers;
    std::vector<float> m_resampleScratch; // audio thread only
    size_t m_streamTargetFrames = 0;
    bool m_streamPrimed = false;          // audio thread only

    SimulationState m_sim;
    RenderState m_render;
};

using SoundSourcePtr = std::shared_ptr<SoundSource>;

// Queue used by the sim/audio threads to hand released sources back to the
// game thread for destruction.
using ReleaseQueue = SpscQueue<SoundSourcePtr>;

} // namespace sa
