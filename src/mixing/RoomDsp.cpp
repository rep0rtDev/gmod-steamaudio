// src/mixing/RoomDsp.cpp
#include "mixing/RoomDsp.h"

#include <algorithm>
#include <cmath>

#include "util/Math.h"

namespace sa {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kBypassCutoffHz = 20000.f;
constexpr float kCrossfadeSeconds = 0.5f;
constexpr float kLowpassSmoothSeconds = 0.05f;
constexpr uint32_t kSilentFramesToIdle = 24;

// Parameters derived from the RVA/DLY/DFR processor chains in Source's
// dsp_presets.txt (size ~ delay spread, feedback ~ decay, cutoff ~ damping).
// Values are tuned by ear to sound close to the engine's presets rather than
// being a literal port of its processors.
//   id  name             size  decay damp   pre   wet   mixMin mixMax dbMin drop
const RoomPreset kRoomPresets[] = {
    {0, "off", 1.0f, 0.0f, 5000.f, 0.f, 0.0f, 0.2f, 0.7f, 80.f, 0.5f},
    {1, "generic", 1.0f, 0.8f, 5000.f, 10.f, 0.25f, 0.2f, 0.7f, 80.f, 0.5f},
    {2, "metal_small", 0.8f, 1.2f, 4000.f, 5.f, 0.30f, 0.2f, 0.7f, 80.f, 0.5f},
    {3, "metal_medium", 1.0f, 1.8f, 4000.f, 8.f, 0.35f, 0.2f, 0.7f, 80.f, 0.5f},
    {4, "metal_large", 1.3f, 2.6f, 4000.f, 12.f, 0.40f, 0.2f, 0.7f, 80.f, 0.5f},
    {5, "tunnel_small", 0.7f, 1.5f, 6000.f, 5.f, 0.30f, 0.2f, 0.7f, 80.f, 0.5f},
    {6, "tunnel_medium", 1.2f, 1.8f, 5000.f, 15.f, 0.35f, 0.2f, 0.7f, 80.f, 0.5f},
    {7, "tunnel_large", 1.5f, 2.6f, 4000.f, 25.f, 0.40f, 0.2f, 0.7f, 80.f, 0.5f},
    {8, "chamber_small", 0.7f, 1.0f, 5000.f, 5.f, 0.35f, 0.2f, 0.7f, 80.f, 0.5f},
    {9, "chamber_medium", 0.8f, 1.2f, 6000.f, 8.f, 0.35f, 0.2f, 0.7f, 80.f, 0.5f},
    {10, "chamber_large", 1.0f, 1.5f, 6000.f, 12.f, 0.40f, 0.2f, 0.7f, 80.f, 0.5f},
    {11, "bright_small", 0.7f, 0.9f, 8000.f, 5.f, 0.30f, 0.2f, 0.7f, 80.f, 0.5f},
    {12, "bright_medium", 0.8f, 1.1f, 8000.f, 8.f, 0.30f, 0.2f, 0.7f, 80.f, 0.5f},
    {13, "bright_large", 1.0f, 1.4f, 9000.f, 12.f, 0.35f, 0.2f, 0.7f, 80.f, 0.5f},
    {14, "water_1", 1.0f, 0.9f, 1800.f, 10.f, 0.50f, 0.2f, 0.7f, 80.f, 0.5f},
    {15, "water_2", 0.8f, 1.2f, 1000.f, 10.f, 0.50f, 0.2f, 0.7f, 80.f, 0.5f},
    {16, "water_3", 1.0f, 1.8f, 1000.f, 15.f, 0.50f, 0.2f, 0.7f, 80.f, 0.5f},
    {17, "concrete_small", 0.7f, 1.1f, 4000.f, 5.f, 0.35f, 0.2f, 0.7f, 80.f, 0.5f},
    {18, "concrete_medium", 0.8f, 1.3f, 3500.f, 8.f, 0.35f, 0.2f, 0.7f, 80.f, 0.5f},
    {19, "concrete_large", 1.0f, 1.6f, 3000.f, 12.f, 0.40f, 0.2f, 0.7f, 80.f, 0.5f},
    {20, "outside_small", 3.0f, 0.9f, 2000.f, 60.f, 0.30f, 0.2f, 0.7f, 80.f, 0.5f},
    {21, "outside_medium", 3.5f, 1.2f, 1500.f, 90.f, 0.30f, 0.2f, 0.7f, 80.f, 0.5f},
    {22, "outside_large", 4.0f, 1.6f, 1000.f, 150.f, 0.30f, 0.2f, 0.7f, 80.f, 0.5f},
    {23, "cavern_small", 1.8f, 2.5f, 2000.f, 40.f, 0.40f, 0.2f, 0.7f, 80.f, 0.5f},
    {24, "cavern_medium", 2.2f, 3.5f, 2000.f, 60.f, 0.40f, 0.2f, 0.7f, 80.f, 0.5f},
    {25, "cavern_large", 2.6f, 4.5f, 1800.f, 80.f, 0.45f, 0.2f, 0.7f, 80.f, 0.5f},
    {26, "weird_1", 4.0f, 2.0f, 1500.f, 120.f, 0.35f, 0.2f, 0.7f, 80.f, 0.5f},
    {27, "weird_2", 4.0f, 2.0f, 1500.f, 120.f, 0.35f, 0.2f, 0.7f, 80.f, 0.5f},
    {28, "weird_3", 4.0f, 2.0f, 1500.f, 120.f, 0.35f, 0.2f, 0.7f, 80.f, 0.5f},
    {29, "weird_4", 4.0f, 2.0f, 1500.f, 120.f, 0.35f, 0.2f, 0.7f, 80.f, 0.5f},
    // Automatic-DSP templates (adsp_*_min): "empty/diffuse small bright" ..
    // "huge dull" pairs for each space shape.
    {100, "auto_generic_a", 1.0f, 1.4f, 4000.f, 8.f, 0.30f, 0.2f, 0.7f, 80.f, 0.5f},
    {101, "auto_generic_b", 2.5f, 1.0f, 6000.f, 30.f, 0.25f, 0.2f, 0.7f, 80.f, 0.5f},
    {102, "room_empty_small", 0.7f, 0.6f, 6000.f, 5.f, 0.30f, 0.3f, 0.8f, 80.f, 0.5f},
    {103, "room_empty_huge", 2.0f, 2.4f, 1800.f, 20.f, 0.40f, 0.32f, 0.8f, 80.f, 0.5f},
    {104, "room_diffuse_small", 0.7f, 0.7f, 5000.f, 5.f, 0.35f, 0.63f, 0.8f, 80.f, 0.5f},
    {105, "room_diffuse_huge", 2.0f, 2.6f, 1600.f, 20.f, 0.40f, 0.32f, 0.8f, 80.f, 0.5f},
    {106, "duct_empty_small", 0.9f, 1.4f, 6000.f, 3.f, 0.35f, 0.4f, 0.9f, 80.f, 0.5f},
    {107, "duct_empty_huge", 1.6f, 2.4f, 2000.f, 8.f, 0.40f, 0.4f, 0.9f, 80.f, 0.5f},
    {108, "duct_diffuse_small", 0.9f, 1.4f, 6000.f, 3.f, 0.35f, 0.4f, 0.9f, 80.f, 0.5f},
    {109, "duct_diffuse_huge", 1.6f, 2.4f, 2000.f, 8.f, 0.40f, 0.4f, 0.9f, 80.f, 0.5f},
    {110, "hall_empty_small", 1.0f, 1.2f, 6000.f, 10.f, 0.35f, 0.3f, 0.8f, 80.f, 0.5f},
    {111, "hall_empty_huge", 2.4f, 3.0f, 4000.f, 30.f, 0.40f, 0.3f, 0.8f, 80.f, 0.5f},
    {112, "hall_diffuse_small", 1.0f, 1.2f, 6000.f, 10.f, 0.35f, 0.3f, 0.8f, 80.f, 0.5f},
    {113, "hall_diffuse_huge", 2.4f, 3.0f, 4000.f, 30.f, 0.40f, 0.3f, 0.8f, 80.f, 0.5f},
    {114, "tunnel_empty_small", 1.0f, 1.8f, 6000.f, 10.f, 0.35f, 0.4f, 0.9f, 80.f, 0.7f},
    {115, "tunnel_empty_huge", 2.6f, 3.5f, 4000.f, 40.f, 0.40f, 0.4f, 0.9f, 80.f, 0.7f},
    {116, "tunnel_diffuse_small", 1.0f, 1.8f, 6000.f, 10.f, 0.35f, 0.4f, 0.9f, 80.f, 0.7f},
    {117, "tunnel_diffuse_huge", 2.6f, 3.5f, 4000.f, 40.f, 0.40f, 0.4f, 0.9f, 80.f, 0.7f},
    {118, "street_empty_small", 3.0f, 1.0f, 4000.f, 50.f, 0.25f, 0.3f, 0.8f, 75.f, 0.3f},
    {119, "street_empty_huge", 4.0f, 1.4f, 1000.f, 100.f, 0.25f, 0.3f, 0.8f, 75.f, 0.3f},
    {120, "street_diffuse_small", 3.0f, 1.0f, 4000.f, 50.f, 0.25f, 0.3f, 0.8f, 75.f, 0.3f},
    {121, "street_diffuse_huge", 4.0f, 1.4f, 1000.f, 100.f, 0.25f, 0.3f, 0.8f, 75.f, 0.3f},
    {122, "alley_empty_small", 2.0f, 1.0f, 5000.f, 30.f, 0.30f, 0.32f, 0.8f, 60.f, 0.3f},
    {123, "alley_empty_huge", 3.0f, 1.4f, 3000.f, 60.f, 0.30f, 0.32f, 0.8f, 60.f, 0.3f},
    {124, "alley_diffuse_small", 2.0f, 1.0f, 5000.f, 30.f, 0.30f, 0.32f, 0.8f, 60.f, 0.3f},
    {125, "alley_diffuse_huge", 3.0f, 1.4f, 3000.f, 60.f, 0.30f, 0.32f, 0.8f, 60.f, 0.3f},
    {126, "courtyard_empty_small", 3.0f, 0.8f, 4000.f, 40.f, 0.30f, 0.3f, 0.8f, 80.f, 0.6f},
    {127, "courtyard_empty_huge", 4.0f, 1.2f, 900.f, 80.f, 0.30f, 0.3f, 0.8f, 80.f, 0.6f},
    {128, "courtyard_diffuse_small", 3.0f, 0.8f, 4000.f, 40.f, 0.30f, 0.3f, 0.8f, 80.f, 0.6f},
    {129, "courtyard_diffuse_huge", 4.0f, 1.0f, 900.f, 80.f, 0.30f, 0.3f, 0.8f, 80.f, 0.6f},
    {130, "openspace_empty_small", 4.0f, 1.0f, 3000.f, 120.f, 0.20f, 0.3f, 0.6f, 95.f, 0.1f},
    {131, "openspace_empty_huge", 4.0f, 1.6f, 900.f, 200.f, 0.20f, 0.3f, 0.6f, 95.f, 0.1f},
    {132, "openspace_diffuse_small", 4.0f, 1.0f, 3000.f, 120.f, 0.20f, 0.2f, 0.7f, 90.f, 0.1f},
    {133, "openspace_diffuse_huge", 4.0f, 1.6f, 900.f, 200.f, 0.20f, 0.2f, 0.7f, 90.f, 0.1f},
};

// dsp_player presets: lowpass cutoffs approximating the FLT/DFR/LFO chains,
// durations/fades straight from dsp_presets.txt.
const PlayerDspPreset kPlayerPresets[] = {
    {0, 0.f, 1.f, 0.f, 0.f},
    {30, 3000.f, 1.f, 0.f, 0.f},   // facing away
    {31, 1000.f, 1.f, 0.f, 0.f},   // facing away + delay
    {32, 1000.f, 0.9f, 1.6f, -1.f}, // explosion ring 1
    {33, 1000.f, 0.9f, 1.6f, -1.f}, // explosion ring 2
    {34, 1000.f, 0.9f, 1.6f, -1.f}, // explosion ring 3
    {35, 2000.f, 0.8f, 1.6f, -1.f}, // shock muffle 1
    {36, 2000.f, 0.8f, 1.6f, -1.f}, // shock muffle 2
    {37, 2000.f, 0.8f, 1.6f, -1.f}, // shock muffle 3
    {39, 1200.f, 0.9f, 1.0f, -1.f}, // shock muffle (short)
    {40, 1500.f, 1.f, 0.f, 0.f},   // lowpass (permanent)
    {134, 800.f, 0.8f, 1.6f, -1.f}, // flashbang muffle long
    {135, 800.f, 0.8f, 0.2f, -1.f}, // flashbang muffle medium
    {136, 800.f, 0.8f, 0.1f, -1.f}, // flashbang muffle short
    {137, 900.f, 0.9f, 1.0f, -1.f},
    {138, 900.f, 0.9f, 1.5f, -1.f},
    {139, 900.f, 0.9f, 3.0f, -1.f},
};

// Line lengths (ms at size 1.0), mutually inharmonic.
constexpr float kLineMs[RoomReverb::kLines] = {29.7f, 37.1f, 41.1f, 43.7f, 53.3f, 61.3f, 71.9f, 83.1f};
constexpr float kAllpassMsL[2] = {5.1f, 1.7f};
constexpr float kAllpassMsR[2] = {4.7f, 2.1f};
constexpr float kAllpassGain = 0.5f;
constexpr float kOutputScale = 0.35f;

size_t MsToSamples(float ms, int32_t sampleRate)
{
    return std::max<size_t>(1, static_cast<size_t>(ms * 0.001f * static_cast<float>(sampleRate) + 0.5f));
}

} // namespace

const RoomPreset& FindRoomPreset(int32_t id)
{
    for (const RoomPreset& p : kRoomPresets)
        if (p.id == id)
            return p;
    return kRoomPresets[0];
}

float RoomMixForSource(const RoomPreset& preset, float distanceUnits, float soundLevelDb)
{
    if (!preset.Active())
        return 0.f;
    const float t = Clamp01(distanceUnits / kDspMixDistanceUnits);
    float mix = Lerp(preset.mixMin, preset.mixMax, t);
    if (soundLevelDb > 0.f && soundLevelDb < preset.dbMin)
        mix *= 1.f - Clamp01(preset.mixDrop);
    return Clamp01(mix);
}

const PlayerDspPreset& FindPlayerDspPreset(int32_t id)
{
    for (const PlayerDspPreset& p : kPlayerPresets)
        if (p.id == id)
            return p;
    return kPlayerPresets[0];
}

// ---------------------------------------------------------------------------
// RoomReverb

void RoomReverb::Prepare(int32_t sampleRate, int32_t frameSize)
{
    m_sampleRate = sampleRate;
    m_frameSize = frameSize;
    for (size_t i = 0; i < kLines; ++i)
        m_lines[i].buffer.assign(MsToSamples(kLineMs[i] * kMaxSize, sampleRate) + 1, 0.f);
    for (size_t i = 0; i < 2; ++i) {
        m_allpassL[i].buffer.assign(MsToSamples(kAllpassMsL[i], sampleRate) + 1, 0.f);
        m_allpassL[i].length = MsToSamples(kAllpassMsL[i], sampleRate);
        m_allpassR[i].buffer.assign(MsToSamples(kAllpassMsR[i], sampleRate) + 1, 0.f);
        m_allpassR[i].length = MsToSamples(kAllpassMsR[i], sampleRate);
    }
    m_preDelay.assign(MsToSamples(kMaxPreDelayMs, sampleRate) + 1, 0.f);
    m_scratch.assign(static_cast<size_t>(std::max(frameSize, 1)), 0.f);
    Reset();
    SetPreset(FindRoomPreset(0));
}

void RoomReverb::Reset()
{
    for (Line& l : m_lines) {
        std::fill(l.buffer.begin(), l.buffer.end(), 0.f);
        l.write = 0;
        l.dampState = 0.f;
    }
    for (Allpass& a : m_allpassL) {
        std::fill(a.buffer.begin(), a.buffer.end(), 0.f);
        a.write = 0;
    }
    for (Allpass& a : m_allpassR) {
        std::fill(a.buffer.begin(), a.buffer.end(), 0.f);
        a.write = 0;
    }
    std::fill(m_preDelay.begin(), m_preDelay.end(), 0.f);
    m_preDelayWrite = 0;
    m_ringing = false;
    m_silentFrames = 0;
}

void RoomReverb::SetPreset(const RoomPreset& preset)
{
    m_preset = preset;
    m_preset.size = Clamp(m_preset.size, 0.25f, kMaxSize);
    m_preset.preDelayMs = Clamp(m_preset.preDelayMs, 0.f, kMaxPreDelayMs);
    m_wet = m_preset.Active() ? m_preset.wet : 0.f;
    ConfigureLengths();
}

void RoomReverb::ConfigureLengths()
{
    if (m_sampleRate <= 0)
        return;
    const float fs = static_cast<float>(m_sampleRate);
    const float damp = 1.f - std::exp(-2.f * kPi * Clamp(m_preset.dampingHz, 100.f, fs * 0.45f) / fs);
    for (size_t i = 0; i < kLines; ++i) {
        Line& l = m_lines[i];
        l.length = std::min(MsToSamples(kLineMs[i] * m_preset.size, m_sampleRate), l.buffer.size() - 1);
        // g = 10^(-3 * L / (T60 * fs)) gives -60 dB after T60 seconds.
        l.feedback = m_preset.decaySeconds > 0.f
                         ? std::pow(10.f, -3.f * static_cast<float>(l.length) / (m_preset.decaySeconds * fs))
                         : 0.f;
        l.feedback = std::min(l.feedback, 0.9995f);
        l.damp = damp;
    }
    m_preDelayLength = std::max<size_t>(1, std::min(MsToSamples(m_preset.preDelayMs, m_sampleRate), m_preDelay.size() - 1));
}

float RoomReverb::AllpassNext(Allpass& ap, float x, float g)
{
    const size_t read = (ap.write + ap.buffer.size() - ap.length) % ap.buffer.size();
    const float delayed = ap.buffer[read];
    const float out = delayed - g * x;
    ap.buffer[ap.write] = x + g * out;
    if (++ap.write == ap.buffer.size())
        ap.write = 0;
    return out;
}

void RoomReverb::Process(const float* input, float* left, float* right, size_t frames, float gain)
{
    if (m_sampleRate <= 0 || frames == 0 || !input)
        return;
    const float inPeak = dsp::Peak(input, frames) * gain;
    if (inPeak < 1e-6f) {
        if (!m_ringing)
            return;
    } else {
        m_ringing = true;
        m_silentFrames = 0;
    }
    if (m_wet <= 0.f) {
        m_ringing = false;
        return;
    }

    constexpr float kHouseholder = 2.f / static_cast<float>(kLines);
    const float wet = m_wet * gain * kOutputScale;
    float outPeak = 0.f;
    float reads[kLines];
    for (size_t n = 0; n < frames; ++n) {
        // Pre-delay.
        const size_t preRead = (m_preDelayWrite + m_preDelay.size() - m_preDelayLength) % m_preDelay.size();
        const float x = m_preDelay[preRead];
        m_preDelay[m_preDelayWrite] = input[n];
        if (++m_preDelayWrite == m_preDelay.size())
            m_preDelayWrite = 0;

        // Read + damp + feedback scale.
        float sum = 0.f;
        for (size_t i = 0; i < kLines; ++i) {
            Line& l = m_lines[i];
            const size_t read = (l.write + l.buffer.size() - l.length) % l.buffer.size();
            l.dampState += (l.buffer[read] - l.dampState) * l.damp;
            reads[i] = l.dampState * l.feedback;
            sum += reads[i];
        }
        // Householder feedback matrix: v_i = x + r_i - (2/N) * sum(r).
        const float mixed = kHouseholder * sum;
        float outL = 0.f;
        float outR = 0.f;
        for (size_t i = 0; i < kLines; ++i) {
            Line& l = m_lines[i];
            l.buffer[l.write] = x + reads[i] - mixed;
            if (++l.write == l.buffer.size())
                l.write = 0;
            if (i & 1)
                outR += reads[i];
            else
                outL += reads[i];
        }
        outL = AllpassNext(m_allpassL[1], AllpassNext(m_allpassL[0], outL, kAllpassGain), kAllpassGain);
        outR = AllpassNext(m_allpassR[1], AllpassNext(m_allpassR[0], outR, kAllpassGain), kAllpassGain);
        left[n] += outL * wet;
        right[n] += outR * wet;
        outPeak = std::max(outPeak, std::max(std::fabs(outL), std::fabs(outR)));
    }

    if (inPeak < 1e-6f && outPeak * wet < 1e-5f) {
        const size_t quietSamples = static_cast<size_t>(++m_silentFrames) * frames;
        const size_t pendingDelay = m_preDelayLength + m_lines.back().length +
                                    m_allpassL[0].length + m_allpassL[1].length;
        if (quietSamples >= std::max(pendingDelay, static_cast<size_t>(kSilentFramesToIdle) * 512))
            m_ringing = false;
    } else {
        m_silentFrames = 0;
    }
}

// ---------------------------------------------------------------------------
// EnvironmentProcessor

void EnvironmentProcessor::Initialize(int32_t sampleRate, int32_t frameSize)
{
    m_sampleRate = sampleRate;
    m_frameSize = frameSize;
    m_sendBus.assign(static_cast<size_t>(std::max(frameSize, 1)), 0.f);
    for (RoomReverb& r : m_reverb)
        r.Prepare(sampleRate, frameSize);
    m_activeReverb = 0;
    m_effectiveRoom = 0;
    m_crossfade = 1.f;
    const float frameRate = static_cast<float>(sampleRate) / static_cast<float>(std::max(frameSize, 1));
    m_cutoffSmoother.SetTimeConstant(kLowpassSmoothSeconds, frameRate);
    m_cutoffSmoother.Snap(kBypassCutoffHz);
    m_gainSmoother.SetTimeConstant(kLowpassSmoothSeconds, frameRate);
    m_gainSmoother.Snap(1.f);
    m_lowpassL.Reset();
    m_lowpassR.Reset();
    m_lowpassActive = false;
    m_playerPresetSeen = 0;
    m_playerElapsed = 0.f;
    m_playerCutoff = 0.f;
    m_playerGain = 1.f;
    m_masterCutoff = 0.f;
}

void EnvironmentProcessor::Shutdown()
{
    m_sendBus.clear();
    m_frameSize = 0;
    m_sampleRate = 0;
    m_masterCutoff = 0.f;
    m_effectiveRoom = 0;
}

bool EnvironmentProcessor::RoomEnabled() const
{
    return IsInitialized() && m_cfg.roomMode != 0 && m_reverb[m_activeReverb].Preset().Active();
}

bool EnvironmentProcessor::SendAlways() const
{
    return m_cfg.roomMode >= 2 || (m_cfg.underwater && m_state.underwater != 0);
}

const RoomPreset& EnvironmentProcessor::BeginFrame()
{
    static const RoomPreset kOff{};
    if (!IsInitialized())
        return kOff;
    std::fill(m_sendBus.begin(), m_sendBus.end(), 0.f);

    int32_t desired = 0;
    if (m_cfg.roomMode != 0) {
        if (m_cfg.underwater && m_state.underwater != 0 && m_state.valid != 0)
            desired = m_state.waterPreset;
        else if (m_cfg.roomOverride >= 0)
            desired = m_cfg.roomOverride;
        else if (m_state.valid != 0)
            desired = m_state.roomPreset;
    }
    // Ids without a room definition fall back to off.
    desired = FindRoomPreset(desired).id;

    if (desired != m_effectiveRoom) {
        const size_t next = m_activeReverb ^ 1u;
        m_reverb[next].Reset();
        m_reverb[next].SetPreset(FindRoomPreset(desired));
        m_activeReverb = next;
        m_crossfade = 0.f;
        m_effectiveRoom = desired;
    }
    return m_reverb[m_activeReverb].Preset();
}

void EnvironmentProcessor::UpdatePlayerDsp()
{
    const float dt = static_cast<float>(m_frameSize) / static_cast<float>(std::max(m_sampleRate, 1));
    const int32_t id = (m_cfg.playerDsp && m_state.valid != 0) ? m_state.playerPreset : 0;
    if (id != m_playerPresetSeen) {
        m_playerPresetSeen = id;
        m_playerElapsed = 0.f;
    }
    const PlayerDspPreset& p = FindPlayerDspPreset(id);
    if (!p.Active()) {
        m_playerCutoff = 0.f;
        m_playerGain = 1.f;
        return;
    }
    float weight = 1.f;
    if (p.durationSeconds > 0.f) {
        m_playerElapsed += dt;
        if (m_playerElapsed >= p.durationSeconds) {
            const float fade = std::fabs(p.fadeSeconds);
            if (fade <= 0.f) {
                weight = 0.f;
            } else {
                const float t = (m_playerElapsed - p.durationSeconds) / fade;
                weight = t >= 1.f ? 0.f : (p.fadeSeconds < 0.f ? std::exp(-4.f * t) * (1.f - t) : 1.f - t);
            }
        }
    }
    if (weight <= 0.f) {
        m_playerCutoff = 0.f;
        m_playerGain = 1.f;
        return;
    }
    // Interpolate the cutoff in the log domain towards bypass as the effect fades.
    m_playerCutoff = p.cutoffHz * std::pow(kBypassCutoffHz / p.cutoffHz, 1.f - weight);
    m_playerGain = Lerp(1.f, p.gain, weight);
}

void EnvironmentProcessor::EndFrame(float* left, float* right)
{
    if (!IsInitialized() || !left || !right)
        return;
    const size_t frames = static_cast<size_t>(m_frameSize);
    const float fs = static_cast<float>(m_sampleRate);

    // Room reverb with cross-fade between the outgoing and incoming preset.
    if (m_cfg.roomMode != 0) {
        m_crossfade = std::min(1.f, m_crossfade + static_cast<float>(frames) / (kCrossfadeSeconds * fs));
        const float roomGain = std::max(m_cfg.roomGain, 0.f);
        RoomReverb& active = m_reverb[m_activeReverb];
        RoomReverb& fading = m_reverb[m_activeReverb ^ 1u];
        active.Process(m_sendBus.data(), left, right, frames, roomGain * m_crossfade);
        if (m_crossfade < 1.f || fading.Ringing())
            fading.Process(m_sendBus.data(), left, right, frames, roomGain * (1.f - m_crossfade));
    }

    // Master lowpass: underwater takes precedence over dsp_player.
    UpdatePlayerDsp();
    float targetCutoff = kBypassCutoffHz;
    float targetGain = 1.f;
    if (m_cfg.underwater && m_state.underwater != 0 && m_state.valid != 0) {
        targetCutoff = Clamp(m_cfg.underwaterCutoffHz, 100.f, kBypassCutoffHz);
        targetGain = Clamp(m_cfg.underwaterGain, 0.f, 1.f);
    } else if (m_playerCutoff > 0.f) {
        targetCutoff = Clamp(m_playerCutoff, 100.f, kBypassCutoffHz);
        targetGain = Clamp(m_playerGain, 0.f, 1.f);
    }
    const float prevGain = m_gainSmoother.value;
    const float cutoff = m_cutoffSmoother.Next(targetCutoff);
    const float gainNow = m_gainSmoother.Next(targetGain);
    const bool lowpass = cutoff < kBypassCutoffHz * 0.9f;
    m_masterCutoff = lowpass ? cutoff : 0.f;
    if (lowpass) {
        if (!m_lowpassActive) {
            m_lowpassL.Reset();
            m_lowpassR.Reset();
            m_lowpassActive = true;
        }
        m_lowpassL.SetCutoff(cutoff, fs);
        m_lowpassR.SetCutoff(cutoff, fs);
        m_lowpassL.Process(left, frames);
        m_lowpassR.Process(right, frames);
    } else {
        m_lowpassActive = false;
    }
    if (prevGain != 1.f || gainNow != 1.f) {
        dsp::ApplyGainRamp(left, frames, prevGain, gainNow);
        dsp::ApplyGainRamp(right, frames, prevGain, gainNow);
    }
}

} // namespace sa
