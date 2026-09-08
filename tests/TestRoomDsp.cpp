// tests/TestRoomDsp.cpp
#include "TestFramework.h"
#include "mixing/Dsp.h"
#include "mixing/RoomDsp.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace sa;

namespace {

constexpr int32_t kRate = 48000;
constexpr int32_t kFrame = 256;

float Rms(const std::vector<float>& v)
{
    double acc = 0.0;
    for (float s : v)
        acc += static_cast<double>(s) * s;
    return v.empty() ? 0.f : static_cast<float>(std::sqrt(acc / static_cast<double>(v.size())));
}

void FillSine(std::vector<float>& v, float hz, float& phase)
{
    for (float& s : v) {
        s = std::sin(phase);
        phase += 2.f * 3.14159265f * hz / static_cast<float>(kRate);
        if (phase > 2.f * 3.14159265f)
            phase -= 2.f * 3.14159265f;
    }
}

// Runs `blocks` frames through the processor with the given master signal
// (also sent to the room bus with `send` gain) and returns the RMS of the
// last block's left channel.
float RunFrames(EnvironmentProcessor& env, int blocks, float toneHz, float send, float* lastGain = nullptr)
{
    std::vector<float> left(kFrame), right(kFrame);
    float phase = 0.f;
    float rms = 0.f;
    for (int b = 0; b < blocks; ++b) {
        const RoomPreset& preset = env.BeginFrame();
        FillSine(left, toneHz, phase);
        right = left;
        if (preset.Active() && send > 0.f)
            dsp::MixInto(env.SendBus(), left.data(), left.size(), send);
        env.EndFrame(left.data(), right.data());
        rms = Rms(left);
        if (lastGain)
            *lastGain = rms / 0.7071f;
    }
    return rms;
}

} // namespace

// ---------------------------------------------------------------------------
// dsp_room preset table

SA_TEST(RoomDsp_ShortFramesDoNotDiscardDelayedImpulse)
{
    for (int frame : {64, 128, 512}) {
        RoomReverb room;
        room.Prepare(44100, frame);
        room.SetPreset(FindRoomPreset(7));
        std::vector<float> in(frame, 0.f), left(frame), right(frame);
        in[0] = 1.f;
        float peak = 0.f;
        for (int n = 0; n < 44100 / frame; ++n) {
            std::fill(left.begin(), left.end(), 0.f);
            std::fill(right.begin(), right.end(), 0.f);
            room.Process(in.data(), left.data(), right.data(), in.size(), 1.f);
            peak = std::max(peak, dsp::Peak(left.data(), left.size()));
            in[0] = 0.f;
        }
        SA_CHECK(peak > 0.001f);
    }
}

SA_TEST(RoomDsp_LegacyPresetsAreMapped)
{
    // Every dsp_room id Source ships (1..29) must resolve to itself with an
    // audible reverb; 0 is "off".
    SA_CHECK_EQ(FindRoomPreset(0).id, 0);
    SA_CHECK(!FindRoomPreset(0).Active());
    for (int32_t id = 1; id <= 29; ++id) {
        const RoomPreset& p = FindRoomPreset(id);
        SA_CHECK_EQ(p.id, id);
        SA_CHECK(p.Active());
        SA_CHECK(p.decaySeconds > 0.f && p.decaySeconds < 10.f);
        SA_CHECK(p.wet > 0.f && p.wet <= 1.f);
    }
}

SA_TEST(RoomDsp_AutomaticTemplatesAreMapped)
{
    // The adsp_* templates the engine picks on maps without env_sound fields.
    for (int32_t id = 100; id <= 133; ++id) {
        const RoomPreset& p = FindRoomPreset(id);
        SA_CHECK_EQ(p.id, id);
        SA_CHECK(p.Active());
    }
}

SA_TEST(RoomDsp_UnknownAndPlayerIdsResolveToOff)
{
    // Player/system presets (30..40, 134..139) and out-of-range values must
    // not be guessed into a room.
    for (int32_t id : {30, 31, 35, 40, 41, 99, 134, 139, 140, 500, -1, -1000}) {
        const RoomPreset& p = FindRoomPreset(id);
        SA_CHECK_EQ(p.id, 0);
        SA_CHECK(!p.Active());
    }
}

SA_TEST(RoomDsp_PlayerPresetsAreMapped)
{
    SA_CHECK(!FindPlayerDspPreset(0).Active());
    for (int32_t id : {30, 31, 32, 35, 40, 134, 135, 136}) {
        const PlayerDspPreset& p = FindPlayerDspPreset(id);
        SA_CHECK_EQ(p.id, id);
        SA_CHECK(p.Active());
        SA_CHECK(p.cutoffHz >= 100.f && p.cutoffHz < 20000.f);
    }
    SA_CHECK_EQ(FindPlayerDspPreset(1).id, 0); // room ids are not player presets
    SA_CHECK_EQ(FindPlayerDspPreset(999).id, 0);
}

SA_TEST(RoomDsp_MixFollowsSourceDistanceModel)
{
    const RoomPreset& p = FindRoomPreset(1);
    // Near sources get the min mix, far sources the max, monotonic between.
    const float near = RoomMixForSource(p, 0.f, 0.f);
    const float mid = RoomMixForSource(p, 500.f, 0.f);
    const float far = RoomMixForSource(p, 100000.f, 0.f);
    SA_CHECK_NEAR(near, p.mixMin, 1e-4);
    SA_CHECK_NEAR(far, p.mixMax, 1e-4);
    SA_CHECK(near < mid && mid < far);
    // Quiet sounds (soundlevel below dbMin) get less reverb; 0 (attenuation-less)
    // is never dropped.
    const float quiet = RoomMixForSource(p, 500.f, 60.f);
    SA_CHECK(quiet < mid);
    SA_CHECK_NEAR(RoomMixForSource(p, 500.f, 0.f), mid, 1e-6);
    SA_CHECK_NEAR(RoomMixForSource(FindRoomPreset(0), 500.f, 0.f), 0.f, 1e-6);
}

// ---------------------------------------------------------------------------
// EnvironmentProcessor

SA_TEST(EnvironmentProcessor_InvalidStateIsBypass)
{
    EnvironmentProcessor env;
    env.Initialize(kRate, kFrame);
    env.SetConfig(EnvironmentConfig{});
    env.SetState(EnvironmentState{}); // valid = 0
    std::vector<float> left(kFrame, 0.5f), right(kFrame, -0.25f);
    const RoomPreset& p = env.BeginFrame();
    SA_CHECK(!p.Active());
    SA_CHECK(!env.RoomEnabled());
    env.EndFrame(left.data(), right.data());
    for (size_t i = 0; i < left.size(); ++i) {
        SA_CHECK_NEAR(left[i], 0.5f, 1e-5);
        SA_CHECK_NEAR(right[i], -0.25f, 1e-5);
    }
}

SA_TEST(EnvironmentProcessor_RoomSelectionPriority)
{
    EnvironmentProcessor env;
    env.Initialize(kRate, kFrame);
    EnvironmentConfig cfg;
    EnvironmentState st;
    st.valid = 1;
    st.roomPreset = 7;
    st.waterPreset = 15;
    env.SetConfig(cfg);
    env.SetState(st);
    SA_CHECK_EQ(env.BeginFrame().id, 7);
    SA_CHECK_EQ(env.EffectiveRoomPreset(), 7);
    SA_CHECK(!env.SendAlways());

    // Explicit override beats dsp_room.
    cfg.roomOverride = 23;
    env.SetConfig(cfg);
    SA_CHECK_EQ(env.BeginFrame().id, 23);

    // Underwater beats the override and forces every source onto the bus.
    st.underwater = 1;
    env.SetState(st);
    SA_CHECK_EQ(env.BeginFrame().id, 15);
    SA_CHECK(env.SendAlways());

    // Mode 2 sends always even in air.
    st.underwater = 0;
    cfg.roomOverride = -1;
    cfg.roomMode = 2;
    env.SetConfig(cfg);
    env.SetState(st);
    SA_CHECK_EQ(env.BeginFrame().id, 7);
    SA_CHECK(env.SendAlways());

    // Mode 0 disables the room entirely.
    cfg.roomMode = 0;
    env.SetConfig(cfg);
    SA_CHECK_EQ(env.BeginFrame().id, 0);
    SA_CHECK(!env.RoomEnabled());

    // Unknown ids from the game degrade to off instead of guessing.
    cfg.roomMode = 1;
    st.roomPreset = 37;
    env.SetConfig(cfg);
    env.SetState(st);
    SA_CHECK_EQ(env.BeginFrame().id, 0);
}

SA_TEST(EnvironmentProcessor_RoomReverbAddsTailAndDecays)
{
    EnvironmentProcessor env;
    env.Initialize(kRate, kFrame);
    env.SetConfig(EnvironmentConfig{});
    EnvironmentState st;
    st.valid = 1;
    st.roomPreset = 25; // cavern_large, long decay
    env.SetState(st);

    // Feed a burst, then silence: the reverb must ring and then decay below
    // an audible threshold within a bounded time (no runaway feedback).
    std::vector<float> left(kFrame), right(kFrame);
    float phase = 0.f;
    for (int b = 0; b < 40; ++b) {
        env.BeginFrame();
        FillSine(left, 440.f, phase);
        right = left;
        dsp::MixInto(env.SendBus(), left.data(), left.size(), 0.5f);
        env.EndFrame(left.data(), right.data());
    }
    float ringing = 0.f;
    float peakTail = 0.f;
    int blocksToSilence = -1;
    for (int b = 0; b < 3000; ++b) { // 16 s
        env.BeginFrame();
        std::fill(left.begin(), left.end(), 0.f);
        std::fill(right.begin(), right.end(), 0.f);
        env.EndFrame(left.data(), right.data());
        const float rms = Rms(left);
        if (b == 0)
            ringing = rms;
        for (float s : left) {
            SA_CHECK(std::isfinite(s));
            peakTail = std::max(peakTail, std::fabs(s));
        }
        if (rms < 1e-5f && blocksToSilence < 0)
            blocksToSilence = b;
    }
    SA_CHECK(ringing > 1e-3f);       // there was a tail right after the burst
    SA_CHECK(peakTail < 2.f);        // bounded
    SA_CHECK(blocksToSilence >= 0);  // and it died out
    SA_CHECK(!env.Ringing());
}

SA_TEST(EnvironmentProcessor_RoomChangeCrossfadesWithoutClicks)
{
    EnvironmentProcessor env;
    env.Initialize(kRate, kFrame);
    env.SetConfig(EnvironmentConfig{});
    EnvironmentState st;
    st.valid = 1;
    st.roomPreset = 3;
    env.SetState(st);
    RunFrames(env, 60, 300.f, 0.5f);
    st.roomPreset = 22;
    env.SetState(st);
    // Sample-to-sample steps in the output over the switch must stay small
    // relative to the tone's own slope (no discontinuity from swapping reverbs).
    std::vector<float> left(kFrame), right(kFrame);
    float phase = 0.f;
    float prev = 0.f;
    float maxStep = 0.f;
    for (int b = 0; b < 120; ++b) {
        env.BeginFrame();
        FillSine(left, 300.f, phase);
        right = left;
        dsp::MixInto(env.SendBus(), left.data(), left.size(), 0.5f);
        env.EndFrame(left.data(), right.data());
        for (float s : left) {
            if (b > 0)
                maxStep = std::max(maxStep, std::fabs(s - prev));
            prev = s;
        }
    }
    SA_CHECK_EQ(env.EffectiveRoomPreset(), 22);
    // A 300 Hz tone at 48 kHz steps at most ~0.04 per sample; leave headroom
    // for the reverb tail but reject full-scale jumps.
    SA_CHECK(maxStep < 0.3f);
}

SA_TEST(EnvironmentProcessor_UnderwaterLowpassAndGainAreBounded)
{
    EnvironmentProcessor env;
    env.Initialize(kRate, kFrame);
    EnvironmentConfig cfg;
    cfg.roomMode = 0; // isolate the filter
    cfg.underwaterCutoffHz = 900.f;
    cfg.underwaterGain = 0.8f;
    env.SetConfig(cfg);
    EnvironmentState st;
    st.valid = 1;
    st.underwater = 1;
    env.SetState(st);

    // Let the smoothers settle, then measure a low and a high tone.
    RunFrames(env, 100, 200.f, 0.f);
    const float low = RunFrames(env, 40, 200.f, 0.f);
    const float high = RunFrames(env, 40, 6000.f, 0.f);
    const float dry = 0.7071f;
    SA_CHECK(low > dry * 0.8f * 0.85f && low < dry * 0.8f * 1.05f); // passband ~ gain 0.8
    SA_CHECK(high < low * 0.1f);                                    // 6 kHz well above 900 Hz cutoff
    SA_CHECK_NEAR(env.PlayerLowpassHz(), 900.f, 1.f);

    // Surfacing restores the dry signal.
    st.underwater = 0;
    env.SetState(st);
    RunFrames(env, 200, 6000.f, 0.f);
    const float restored = RunFrames(env, 20, 6000.f, 0.f);
    SA_CHECK_NEAR(restored, dry, 0.03);
}

SA_TEST(EnvironmentProcessor_UnderwaterDisabledIsTransparent)
{
    EnvironmentProcessor env;
    env.Initialize(kRate, kFrame);
    EnvironmentConfig cfg;
    cfg.roomMode = 0;
    cfg.underwater = false;
    env.SetConfig(cfg);
    EnvironmentState st;
    st.valid = 1;
    st.underwater = 1;
    env.SetState(st);
    RunFrames(env, 100, 6000.f, 0.f);
    SA_CHECK_NEAR(RunFrames(env, 10, 6000.f, 0.f), 0.7071f, 0.02);
    SA_CHECK(!env.SendAlways());
}

SA_TEST(EnvironmentProcessor_PlayerDspTimesOut)
{
    EnvironmentProcessor env;
    env.Initialize(kRate, kFrame);
    EnvironmentConfig cfg;
    cfg.roomMode = 0;
    env.SetConfig(cfg);
    EnvironmentState st;
    st.valid = 1;
    st.playerPreset = 35; // shock muffle, 1.6 s then exponential fade
    env.SetState(st);
    RunFrames(env, 60, 6000.f, 0.f); // 0.32 s: muffled
    const float muffled = RunFrames(env, 10, 6000.f, 0.f);
    SA_CHECK(muffled < 0.3f);
    RunFrames(env, 1500, 6000.f, 0.f); // +8 s: preset expired even though dsp_player still reads 35
    const float expired = RunFrames(env, 10, 6000.f, 0.f);
    SA_CHECK_NEAR(expired, 0.7071f, 0.03);
    SA_CHECK_NEAR(env.PlayerLowpassHz(), 0.f, 1e-3);

    // A permanent preset (40) stays until changed.
    st.playerPreset = 40;
    env.SetState(st);
    RunFrames(env, 1500, 6000.f, 0.f);
    SA_CHECK(RunFrames(env, 10, 6000.f, 0.f) < 0.3f);
    st.playerPreset = 0;
    env.SetState(st);
    RunFrames(env, 300, 6000.f, 0.f);
    SA_CHECK_NEAR(RunFrames(env, 10, 6000.f, 0.f), 0.7071f, 0.03);
}

// ---------------------------------------------------------------------------
// Doppler / propagation delay

SA_TEST(PropagationDelay_StaticDistanceIsPureDelay)
{
    dsp::PropagationDelay d;
    d.Prepare(48000 * 35 / 100, 512);
    // Impulse at sample 10, delay 100 samples -> impulse at 110.
    std::vector<float> buf(512, 0.f);
    buf[10] = 1.f;
    d.Process(buf.data(), buf.size(), 100.f, 48000.f);
    SA_CHECK(d.Primed());
    SA_CHECK_NEAR(d.CurrentDelay(), 100.f, 1e-4);
    size_t peak = 0;
    for (size_t i = 1; i < buf.size(); ++i)
        if (std::fabs(buf[i]) > std::fabs(buf[peak]))
            peak = i;
    SA_CHECK_EQ(peak, static_cast<size_t>(110));
    SA_CHECK_NEAR(buf[110], 1.f, 1e-3);
    for (size_t i = 0; i < 100; ++i)
        SA_CHECK_NEAR(buf[i], 0.f, 1e-6);
}

SA_TEST(PropagationDelay_ApproachingSourceRaisesPitch)
{
    // A source closing at 34.3 m/s (10% of c) must shift a tone up ~10%.
    dsp::PropagationDelay d;
    d.Prepare(48000 * 35 / 100, 512);
    const float fs = 48000.f, c = 343.f, tone = 1000.f;
    float dist = 100.f;
    float phase = 0.f;
    std::vector<float> block(512);
    std::vector<float> out;
    // Prime at the start distance, then approach.
    for (int b = 0; b < 200; ++b) {
        for (float& s : block) {
            s = std::sin(phase);
            phase += 2.f * 3.14159265f * tone / fs;
        }
        d.Process(block.data(), block.size(), dist / c * fs, fs);
        if (b >= 20)
            dist -= 34.3f * 512.f / fs;
        if (b >= 60 && b < 180)
            out.insert(out.end(), block.begin(), block.end());
    }
    // Count zero crossings over the steady approach window.
    size_t crossings = 0;
    for (size_t i = 1; i < out.size(); ++i)
        if ((out[i - 1] < 0.f) != (out[i] < 0.f))
            ++crossings;
    const float measuredHz = static_cast<float>(crossings) * fs / (2.f * static_cast<float>(out.size()));
    SA_CHECK_NEAR(measuredHz, tone * (1.f + 34.3f / c), 15.f);
    for (float s : out)
        SA_CHECK(std::fabs(s) <= 1.05f);
}

SA_TEST(PropagationDelay_RecedingSourceLowersPitch)
{
    dsp::PropagationDelay d;
    d.Prepare(48000 * 35 / 100, 512);
    const float fs = 48000.f, c = 343.f, tone = 1000.f;
    float dist = 5.f;
    float phase = 0.f;
    std::vector<float> block(512);
    std::vector<float> out;
    for (int b = 0; b < 200; ++b) {
        for (float& s : block) {
            s = std::sin(phase);
            phase += 2.f * 3.14159265f * tone / fs;
        }
        d.Process(block.data(), block.size(), dist / c * fs, fs);
        if (b >= 20)
            dist += 34.3f * 512.f / fs;
        if (b >= 60 && b < 180)
            out.insert(out.end(), block.begin(), block.end());
    }
    size_t crossings = 0;
    for (size_t i = 1; i < out.size(); ++i)
        if ((out[i - 1] < 0.f) != (out[i] < 0.f))
            ++crossings;
    const float measuredHz = static_cast<float>(crossings) * fs / (2.f * static_cast<float>(out.size()));
    SA_CHECK_NEAR(measuredHz, tone * (1.f - 34.3f / c), 15.f);
}

SA_TEST(PropagationDelay_RateIsSlewLimitedAndTeleportSnaps)
{
    dsp::PropagationDelay d;
    d.Prepare(48000 * 35 / 100, 256);
    std::vector<float> buf(256, 0.f);
    d.Process(buf.data(), buf.size(), 1000.f, 48000.f);
    SA_CHECK_NEAR(d.CurrentDelay(), 1000.f, 1e-3);
    // A moderate jump (< 0.25 s) is slewed at most 0.5 samples/sample.
    d.Process(buf.data(), buf.size(), 5000.f, 48000.f);
    SA_CHECK_NEAR(d.CurrentDelay(), 1000.f + 0.5f * 256.f, 1e-2);
    // A teleport (>= 0.25 s of delay change) snaps immediately.
    d.Process(buf.data(), buf.size(), 15000.f, 48000.f);
    SA_CHECK_NEAR(d.CurrentDelay(), 15000.f, 1e-2);
    // Targets are clamped to the prepared maximum (16800 here) and approached
    // at the slew rate; negatives clamp to zero.
    for (int i = 0; i < 20; ++i)
        d.Process(buf.data(), buf.size(), 1e9f, 48000.f);
    SA_CHECK_NEAR(d.CurrentDelay(), 16800.f, 1e-2);
    d.Reset();
    d.Process(buf.data(), buf.size(), 100.f, 48000.f);
    d.Process(buf.data(), buf.size(), -50.f, 48000.f);
    SA_CHECK_NEAR(d.CurrentDelay(), 0.f, 1e-4);
    // Reset drops the primed state so the next block snaps.
    d.Reset();
    SA_CHECK(!d.Primed());
    d.Process(buf.data(), buf.size(), 42.f, 48000.f);
    SA_CHECK_NEAR(d.CurrentDelay(), 42.f, 1e-4);
}

SA_TEST(PropagationDelay_OversizedBlockAndNaNAreHarmless)
{
    dsp::PropagationDelay d;
    d.Prepare(100, 64);
    std::vector<float> big(100000, 0.25f);
    d.Process(big.data(), big.size(), 50.f, 48000.f);
    for (float s : big)
        SA_CHECK_NEAR(s, 0.25f, 1e-6);
    std::vector<float> buf(64, 0.f);
    d.Process(buf.data(), buf.size(), 30.f, 48000.f);
    d.Process(buf.data(), buf.size(), std::nanf(""), 48000.f);
    SA_CHECK_NEAR(d.CurrentDelay(), 30.f, 1e-4);
    for (float s : buf)
        SA_CHECK(std::isfinite(s));
}
