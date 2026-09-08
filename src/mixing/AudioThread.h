// src/mixing/AudioThread.h
//
// Dedicated mixing thread. Every frame it:
//   1. picks the render clock (engine paint samples),
//   2. reads captured engine PCM (TimedSampleRing) and stream PCM (SPSC ring)
//      for every active source,
//   3. renders through HRTFRenderer (Steam Audio) or FallbackMixer,
//   4. applies master gain + limiter,
//   5. writes stereo output to (a) the native-output SPSC ring pulled by the
//      device thread, or (b) timed rings pulled by the engine's TransferSamples
//      hook (engine output mode).
//
// Pacing: native mode is paced by the output ring's free space (device clock);
// engine mode by the captured frontier (engine clock). The thread waits on a
// condition variable with a short timeout; consumers notify after pulling.
//
// Thread-safety: Start/Stop/Add*/Remove* from the game thread; everything in
// Run() is the audio thread; PullStereo/FillPaintBuffer from device / engine
// mixer threads through lock-free rings only.
#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "core/ChannelCapture.h"
#include "core/EngineHooks.h"
#include "mixing/FallbackMixer.h"
#include "mixing/LockFreeQueue.h"
#include "mixing/RoomDsp.h"
#include "mixing/SoundSource.h"
#include "platform/PlatformAudioOutput.h"
#include "steamaudio/Config.h"
#include "steamaudio/HRTFRenderer.h"

namespace sa {

enum class AudioOutputPath : int32_t {
    Native = 0, // device thread pulls from the SPSC ring
    Engine = 1, // engine TransferSamples hook pulls from timed rings
};

struct AudioThreadSetup {
    int32_t sampleRate = 44100;
    int32_t frameSize = 512;
    int32_t latencyMs = 40;
    float fadeSeconds = 0.008f;
    int32_t engineLeadMs = 0;             // native path: 0 = follow the engine play cursor
    AudioOutputPath path = AudioOutputPath::Native;
    HRTFRenderer* renderer = nullptr;     // null => fallback mixer
    FallbackMixer* fallback = nullptr;    // required
    ChannelCapture* capture = nullptr;    // null => no engine sources
    EngineHooks* hooks = nullptr;         // listener provider (may be null)
    size_t maxStreamSources = 256;
};

struct AudioThreadStats {
    std::atomic<uint64_t> framesRendered{0};
    std::atomic<uint64_t> outputUnderruns{0};
    std::atomic<uint64_t> engineStarves{0};   // frames held because the engine had not painted them yet
    std::atomic<uint64_t> clockResyncs{0};
    std::atomic<uint64_t> nonFiniteFrames{0}; // master frames replaced by silence (NaN/Inf)
    std::atomic<uint32_t> activeSources{0};
    std::atomic<uint32_t> spatializedSources{0};
    std::atomic<float> peak{0.f};
    std::atomic<uint32_t> lastRenderMicros{0};
    std::atomic<uint32_t> maxRenderMicros{0};
    std::atomic<bool> priorityElevated{false};
    std::atomic<bool> usingFallback{false};
    std::atomic<int32_t> roomPreset{0};        // effective dsp_room replacement preset (0 = off)
    std::atomic<uint32_t> roomSends{0};        // sources fed to the room reverb last frame
    std::atomic<float> playerLowpassHz{0.f};   // active dsp_player / underwater lowpass (0 = bypass)
};

// Rare clock events recorded on the audio thread (fixed-size, wait-free) and
// drained by the game thread into the log.
struct ClockEvent {
    enum class Kind : uint8_t { Anchor, HoldBegin, HoldEnd, Resync, NonFinite } kind = Kind::Anchor;
    uint64_t frame = 0;        // frames rendered so far
    int64_t backlog = 0;       // frontier - render clock (samples)
    int64_t lag = 0;           // target - render clock (samples)
    uint32_t heldFrames = 0;   // HoldEnd: frames spent holding
    uint32_t activeSources = 0;
};

// Snapshot taken when a source stops rendering (audio thread, wait-free);
// the game thread keeps the most recent ones for `snd_sa_sounds`.
struct SourceEndEvent {
    uint64_t frame = 0;          // frames rendered so far when the source ended
    int32_t entity = -1;
    Vec3 position{};
    uint8_t spatialize = 0;
    RenderDiag diag;             // last published diagnostics (lifetime counters included)
};

class AudioThread final : public IOutputSource, public IEngineOutputSink {
public:
    AudioThread();
    ~AudioThread() override;
    AudioThread(const AudioThread&) = delete;
    AudioThread& operator=(const AudioThread&) = delete;

    bool Start(const AudioThreadSetup& setup);
    void Stop();
    bool Running() const { return m_running.load(std::memory_order_acquire); }

    // Game thread.
    void SetRuntimeConfig(const RuntimeConfig& cfg) { m_runtime.Store(cfg); }
    void SetExternalListener(const ListenerState& listener) { m_externalListener.Store(listener); }
    void SetEnvironment(const EnvironmentState& state) { m_environmentState.Store(state); }
    void SetDeviceLatencyFrames(uint32_t frames) { m_deviceLatencyFrames.store(frames, std::memory_order_release); }
    bool AddStreamSource(const SoundSourcePtr& source);   // false when the command queue is full
    bool RemoveStreamSource(uint32_t id);
    // Sources the audio thread has finished with (game thread drains & destroys).
    ReleaseQueue& Released() { return m_released; }

    const AudioThreadStats& Stats() const { return m_stats; }
    // Game thread: pops the next recorded clock event.
    bool PopClockEvent(ClockEvent& out) { return m_clockEvents.TryPop(out); }
    bool PopSourceEnd(SourceEndEvent& out) { return m_sourceEnds.TryPop(out); }
    const std::atomic<uint64_t>& FrameCounter() const { return m_stats.framesRendered; }
    uint64_t RenderClock() const { return m_renderClockPublished.load(std::memory_order_acquire); }
    const AudioThreadSetup& Setup() const { return m_setup; }

    // IOutputSource (device thread).
    void PullStereo(float* interleaved, size_t frames) override;
    // IEngineOutputSink (engine mixer thread).
    void FillPaintBuffer(uint64_t clock, src::portable_samplepair_t* out, size_t frames) override;

private:
    struct Command {
        enum class Type : uint8_t { Add, Remove } type = Type::Add;
        SoundSourcePtr source;
        uint32_t id = 0;
    };
    struct SlotRender {
        uint32_t generation = 0;
        bool active = false;
    };

    void Run();
    void DrainCommands();
    bool WaitForWork();
    void RenderFrame();
    // Native path: picks the render clock for this frame. Returns false when
    // the engine has not painted this frame yet (hold: render without engine
    // data and do not advance).
    bool ScheduleNativeClock(uint64_t frontier);
    void RecordClockEvent(ClockEvent::Kind kind, int64_t backlog, int64_t lag);
    void RenderEngineSlots(uint64_t clock, bool haveEngineData);
    void RenderStreamSources();
    void BeginSource(SoundSource& source);
    void EndSource(SoundSource& source);
    float SourceFrameGain(SoundSource& source, bool hasData, bool ending, bool& finished);
    void FinishFrame();
    void PublishOutput(const float* left, const float* right);
    ListenerState CurrentListener() const;
    void ApplyLimiter(float* interleaved, size_t frames);

    AudioThreadSetup m_setup;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::mutex m_wakeMutex;
    std::condition_variable m_wake;

    SeqLock<RuntimeConfig> m_runtime;
    SeqLock<ListenerState> m_externalListener;
    SeqLock<EnvironmentState> m_environmentState;
    RuntimeConfig m_cfg;                 // audio thread copy
    EnvironmentProcessor m_environment;  // audio thread

    SpscQueue<Command> m_commands{256};
    ReleaseQueue m_released{256};
    std::vector<SoundSourcePtr> m_streams;     // audio thread
    std::array<SlotRender, ChannelCapture::kSlots> m_slotRender{};

    // Output rings.
    std::unique_ptr<SpscSampleRing> m_outRing;     // interleaved stereo (native)
    std::unique_ptr<TimedSampleRing> m_outLeft;    // engine path
    std::unique_ptr<TimedSampleRing> m_outRight;
    size_t m_latencyFrames = 0;

    // Scratch (audio thread).
    std::vector<float> m_inL;
    std::vector<float> m_inR;
    std::vector<float> m_interleaved;
    std::vector<float> m_silence;
    std::vector<float> m_masterL;   // post-environment master
    std::vector<float> m_masterR;

    uint64_t m_renderClock = 0;
    std::atomic<bool> m_clockAnchored{false};
    size_t m_engineLeadFrames = 0;   // 0 = follow the engine play cursor
    size_t m_maxLagFrames = 0;
    uint32_t m_heldFrames = 0;       // consecutive frames held so far (0 = not holding)
    SpscQueue<ClockEvent> m_clockEvents{64};
    SpscQueue<SourceEndEvent> m_sourceEnds{128};
    std::atomic<uint64_t> m_renderClockPublished{0};
    std::atomic<uint32_t> m_deviceLatencyFrames{0};
    float m_limiterGain = 1.f;
    bool m_useRenderer = false;

    AudioThreadStats m_stats;
};

} // namespace sa
