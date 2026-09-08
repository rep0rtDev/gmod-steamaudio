// tests/TestAudioThreadClock.cpp
//
// Native-output scheduling: the audio thread must never read engine samples
// the mixer has not painted yet. When the engine stops painting (a frame
// hitch) the output holds instead of running past the frontier, and when the
// engine resumes with a jumped paint time the render clock skips the empty
// gap instead of playing it as silence.
#include "TestFramework.h"
#include "core/ChannelCapture.h"
#include "core/SourceInterfaces.h"
#include "mixing/AudioThread.h"
#include "mixing/FallbackMixer.h"
#include "mixing/SoundSource.h"

#include <chrono>
#include <cmath>
#include <memory>
#include <thread>
#include <vector>

using namespace sa;

namespace {

constexpr int32_t kRate = 44100;
constexpr int32_t kFrame = 512;
constexpr int32_t kMixAhead = 4410; // snd_mixahead 0.1 at 44.1 kHz
constexpr int32_t kGameFrame = 735; // 1/60 s
constexpr int32_t kRampBase = 1000;
constexpr int32_t kRampPeriod = 12000;

// Forward distance from ramp index a to b (the ramp wraps every kRampPeriod).
int64_t RampDistance(int64_t from, int64_t to)
{
    return ((to - from) % kRampPeriod + kRampPeriod) % kRampPeriod;
}

struct Rig {
    ChannelCapture capture;
    std::vector<SoundSourcePtr> sources;
    FallbackMixer fallback;
    AudioThread audio;
    int channel = 0;
    int32_t soundtime = 0;
    int32_t painted = 0;
    int64_t sampleIndex = 0; // ramp position of the next painted sample
    std::vector<int16_t> pcm;

    explicit Rig(HRTFRenderer* renderer = nullptr)
    {
        for (uint32_t i = 0; i < 4; ++i)
            sources.push_back(std::make_shared<SoundSource>(i + 1, SourceKind::EngineChannel, 2, kRate, kRate, 1 << 17));
        capture.Initialize(sources, kRate);
        fallback.Initialize(kFrame, kRate);
        AudioThreadSetup setup;
        setup.sampleRate = kRate;
        setup.frameSize = kFrame;
        setup.latencyMs = 40;
        setup.fadeSeconds = 0.f;
        setup.path = AudioOutputPath::Native;
        setup.fallback = &fallback;
        setup.renderer = renderer;
        setup.capture = &capture;
        SA_CHECK(audio.Start(setup));
        // The thread fills the ring headroom right away (before any engine
        // paint exists); wait for that so later renders are driven by Pull().
        WaitRendered(ExpectedRendered(0));
    }

    // Frames the thread must have rendered after the device pulled `pulledFrames`:
    // ring = 4096 frames, pre-filled with 2048, refilled while >= 2 frames are free.
    static uint64_t ExpectedRendered(size_t pulledFrames) { return pulledFrames / kFrame; }

    void WaitRendered(uint64_t frames)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (audio.Stats().framesRendered.load() < frames && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    ~Rig() { audio.Stop(); }

    static int16_t RampValue(int64_t n) { return static_cast<int16_t>(kRampBase + n % kRampPeriod); }
    static int64_t RampIndex(float sample) { return std::lround(sample * 32768.f) - kRampBase; }

    // One S_Update: the DMA cursor advanced by `advance`, paint up to soundtime + mixahead.
    void Update(int32_t advance)
    {
        soundtime += advance;
        if (painted < soundtime)
            painted = soundtime; // S_Update_ "overflow" clamp
        const int32_t end = soundtime + kMixAhead;
        capture.OnPaintBegin(soundtime, painted, end);
        while (painted < end) {
            capture.OnMixBegin(kFrame);
            pcm.resize(static_cast<size_t>(kFrame));
            for (int32_t i = 0; i < kFrame; ++i)
                pcm[static_cast<size_t>(i)] = RampValue(sampleIndex + i);
            sampleIndex += kFrame;
            capture.CaptureMix(static_cast<src::channel_t*>(static_cast<void*>(&channel)), pcm.data(), true, false,
                               0, 0, static_cast<int32_t>(src::kFixScale), kFrame);
            painted += kFrame;
            capture.OnTransferSamples(painted);
        }
    }

    // Pulls `frames` like the device would and waits until the audio thread
    // has refilled the ring (so the next pull observes freshly rendered audio).
    std::vector<float> Pull(size_t frames)
    {
        std::vector<float> out(frames * 2, 0.f);
        audio.PullStereo(out.data(), frames);
        pulled += frames;
        WaitRendered(ExpectedRendered(pulled));
        return out;
    }

    size_t pulled = 0;
};

// Returns the ramp index of the first non-silent left sample, or -1.
int64_t FirstRampIndex(const std::vector<float>& buf)
{
    for (size_t i = 0; i < buf.size(); i += 2)
        if (std::fabs(buf[i]) > 1e-6f)
            return Rig::RampIndex(buf[i]);
    return -1;
}

// Checks the left channel is a contiguous ramp; returns the last ramp index or -1 when silent.
int64_t CheckContiguous(const std::vector<float>& buf, int64_t expectNext)
{
    int64_t last = -1;
    for (size_t i = 0; i < buf.size(); i += 2) {
        if (std::fabs(buf[i]) <= 1e-6f)
            continue;
        const int64_t idx = Rig::RampIndex(buf[i]);
        if (expectNext >= 0)
            SA_CHECK_EQ(idx, expectNext % kRampPeriod);
        expectNext = idx + 1;
        last = idx;
    }
    return last;
}

} // namespace

SA_TEST(AudioThreadClock_HoldsOnEngineHitchThenSkipsGap)
{
    Rig rig;
    const size_t period = 480;

    // Engine runs normally for ~0.5 s of game time.
    for (int i = 0; i < 30; ++i)
        rig.Update(kGameFrame);

    // Device pulls until audio appears, then keeps the engine running in
    // lockstep with device consumption.
    int64_t expectNext = -1;
    int64_t lastHeard = -1;
    for (int i = 0; i < 200 && lastHeard < 0; ++i) {
        const std::vector<float> out = rig.Pull(period);
        lastHeard = CheckContiguous(out, expectNext);
        if (lastHeard >= 0)
            expectNext = lastHeard + 1;
        rig.Update(static_cast<int32_t>(period));
    }
    SA_CHECK(lastHeard >= 0);
    for (int i = 0; i < 40; ++i) {
        const std::vector<float> out = rig.Pull(period);
        const int64_t last = CheckContiguous(out, expectNext);
        SA_CHECK(last >= 0);
        expectNext = last + 1;
        rig.Update(static_cast<int32_t>(period));
    }
    SA_CHECK_EQ(rig.audio.Stats().engineStarves.load(), 0u);
    SA_CHECK_EQ(rig.audio.Stats().clockResyncs.load(), 0u);
    const uint64_t frontierBeforeHitch = rig.capture.PaintedFrontier();

    // Engine hitch: no S_Update for ~0.4 s while the device keeps pulling.
    int64_t lastBeforeGap = -1;
    bool sawSilence = false;
    for (int i = 0; i < 40; ++i) {
        const std::vector<float> out = rig.Pull(period);
        const int64_t last = CheckContiguous(out, expectNext);
        if (last >= 0) {
            SA_CHECK(!sawSilence); // once held, stays held until the engine resumes
            expectNext = last + 1;
            lastBeforeGap = last;
        } else {
            sawSilence = true;
        }
    }
    SA_CHECK(sawSilence);
    SA_CHECK(rig.audio.Stats().engineStarves.load() > 0u);
    SA_CHECK_EQ(rig.audio.Stats().clockResyncs.load(), 0u);
    // Everything the engine painted before the hitch was heard, nothing beyond it.
    SA_CHECK(rig.audio.RenderClock() <= frontierBeforeHitch);
    SA_CHECK(rig.audio.RenderClock() + kFrame >= frontierBeforeHitch);
    // (only the trailing partial frame, which never became renderable, is unplayed)
    SA_CHECK(RampDistance(lastBeforeGap, rig.sampleIndex) <= static_cast<int64_t>(kFrame));

    // Engine resumes: the DMA cursor jumped past the painted range, so the
    // engine clamps paintedtime to soundtime and paints fresh audio there.
    rig.Update(static_cast<int32_t>(period * 40));
    int64_t firstAfterGap = -1;
    for (int i = 0; i < 100 && firstAfterGap < 0; ++i) {
        const std::vector<float> out = rig.Pull(period);
        firstAfterGap = FirstRampIndex(out);
        if (firstAfterGap >= 0)
            expectNext = CheckContiguous(out, firstAfterGap) + 1;
        rig.Update(static_cast<int32_t>(period));
    }
    SA_CHECK(firstAfterGap >= 0);
    // The empty gap was skipped rather than played as silence; what was lost is
    // the partial frame before the hitch plus the resync landing one frame past
    // the DMA cursor (which itself moved one device period during the resume).
    const int64_t dropped = RampDistance(lastBeforeGap + 1, firstAfterGap);
    const int64_t outputLead = ((kRate * 40 / 1000 + kFrame - 1) / kFrame) * kFrame;
    SA_CHECK(dropped <= kFrame * 2 + static_cast<int64_t>(period) + outputLead);
    SA_CHECK_EQ(rig.audio.Stats().clockResyncs.load(), 1u);

    for (int i = 0; i < 20; ++i) {
        const std::vector<float> out = rig.Pull(period);
        const int64_t last = CheckContiguous(out, expectNext);
        SA_CHECK(last >= 0);
        expectNext = last + 1;
        rig.Update(static_cast<int32_t>(period));
    }
}

SA_TEST(AudioThreadClock_EngineHitchDoesNotRetireLiveEffects)
{
    const char* dir = std::getenv("SA_PHONON_DIR");
    if (!dir || !*dir)
        throw satest::Skipped{"SA_PHONON_DIR not set"};
    PhononContext ctx;
    StaticConfig fixed;
    fixed.maxSources = 1;
    std::string error;
    SA_CHECK(ctx.Initialize(fixed, {dir}, error));
    HRTFRenderer renderer;
    SA_CHECK(renderer.Initialize(ctx, BackendDevices{}, fixed, error));
    Rig rig(&renderer);
    ListenerState listener;
    listener.valid = true;
    listener.frame = CoordinateConverter{}.FrameToSA({}, {1, 0, 0}, {0, 0, 1});
    rig.audio.SetExternalListener(listener);
    rig.Update(kGameFrame);
    SourceParams params;
    params.positionValid = 1;
    params.position = {100.f, 0.f, 0.f};
    rig.sources[0]->SetParams(params);
    rig.Pull(kFrame);
    SA_CHECK_EQ(renderer.FreeEffectSets(), size_t(0));
    for (int i = 0; i < 24; ++i)
        rig.Pull(kFrame);
    SA_CHECK(rig.audio.Stats().engineStarves.load() > 0);
    SA_CHECK_EQ(renderer.FreeEffectSets(), size_t(0));
}

SA_TEST(AudioThread_StreamDrainsPropagationDelayAfterEof)
{
    FallbackMixer fallback;
    fallback.Initialize(kFrame, kRate);
    AudioThread audio;
    RuntimeConfig cfg;
    cfg.doppler = true;
    cfg.roomDspMode = 0;
    audio.SetRuntimeConfig(cfg);
    ListenerState listener;
    listener.valid = true;
    listener.frame = CoordinateConverter{}.FrameToSA({}, {1, 0, 0}, {0, 0, 1});
    audio.SetExternalListener(listener);
    auto source = std::make_shared<SoundSource>(200, SourceKind::Procedural, 1, kRate, kRate, 8192);
    SourceParams params;
    params.position = {100.f * cfg.unitsPerMeter, 0, 0};
    params.positionValid = 1;
    source->SetParams(params);
    std::vector<float> input(2205, 0.2f), output(kFrame * 2);
    source->WriteInterleaved(input.data(), input.size());
    source->SetEndOfStream();
    AudioThreadSetup setup;
    setup.path = AudioOutputPath::Native;
    setup.fallback = &fallback;
    SA_CHECK(audio.Start(setup));
    SA_CHECK(audio.AddStreamSource(source));
    double energy = 0.0;
    for (int n = 0; n < 120; ++n) {
        audio.PullStereo(output.data(), kFrame);
        for (float sample : output)
            energy += static_cast<double>(sample) * sample;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    audio.Stop();
    SA_CHECK(source->RenderFinished());
    SA_CHECK(energy > 20.0);
}

SA_TEST(ChannelCaptureClock_SoundClockFollowsDmaCursor)
{
    std::vector<SoundSourcePtr> sources;
    ChannelCapture capture;
    capture.Initialize(sources, kRate);
    capture.OnPaintBegin(0, 0, kMixAhead);
    const uint64_t start = capture.PaintedFrontier();
    SA_CHECK_EQ(capture.SoundClock(), start);

    // Painted 4410 ahead of the cursor: sound clock trails the paint clock by that much.
    capture.OnMixBegin(kMixAhead);
    capture.OnTransferSamples(kMixAhead);
    capture.OnPaintBegin(735, kMixAhead, 735 + kMixAhead);
    SA_CHECK_EQ(capture.PaintedFrontier(), start + kMixAhead);
    SA_CHECK_EQ(capture.SoundClock(), start + kMixAhead - (kMixAhead - 735));
    SA_CHECK_EQ(capture.MixAheadSamples(), static_cast<uint32_t>(kMixAhead));
    SA_CHECK_EQ(capture.Stats().clockRebases.load(), 0u);
}

SA_TEST(ChannelCaptureClock_PaintTimeRewindRebasesMonotonically)
{
    std::vector<SoundSourcePtr> sources;
    ChannelCapture capture;
    capture.Initialize(sources, kRate);
    capture.OnPaintBegin(0, 0, kMixAhead);
    capture.OnMixBegin(kMixAhead);
    capture.OnTransferSamples(kMixAhead);
    const uint64_t frontier = capture.PaintedFrontier();

    // Sound system reset: paintedtime restarts from zero.
    capture.OnPaintBegin(0, 0, kMixAhead);
    SA_CHECK_EQ(capture.Stats().clockRebases.load(), 1u);
    SA_CHECK_EQ(capture.PaintedFrontier(), frontier);
    capture.OnMixBegin(kFrame);
    capture.OnTransferSamples(kFrame);
    SA_CHECK_EQ(capture.PaintedFrontier(), frontier + kFrame);
}
