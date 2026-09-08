// src/mixing/FallbackMixer.cpp
#include "FallbackMixer.h"

#include <algorithm>
#include <cmath>

#include "Dsp.h"
#include "SoundSource.h"
#include "util/Math.h"

namespace sa {

namespace {

constexpr float kPi = 3.14159265358979f;

// Smoothing factor per frame for gains/filters (~10 ms at 512 samples / 44.1 kHz).
float SmoothingAlpha(int32_t frameSize, int32_t sampleRate)
{
    const float frameSeconds = static_cast<float>(frameSize) / static_cast<float>(std::max(1, sampleRate));
    return 1.f - std::exp(-frameSeconds / 0.010f);
}

// One-pole low-pass coefficient for a cutoff frequency.
float LowpassCoefficient(float cutoffHz, int32_t sampleRate)
{
    const float x = std::exp(-2.f * kPi * cutoffHz / static_cast<float>(std::max(1, sampleRate)));
    return 1.f - x;
}

} // namespace

void FallbackMixer::Initialize(int32_t frameSize, int32_t sampleRate)
{
    m_frameSize = frameSize;
    m_sampleRate = sampleRate;
    m_left.assign(static_cast<size_t>(frameSize), 0.f);
    m_right.assign(static_cast<size_t>(frameSize), 0.f);
    m_mono.assign(static_cast<size_t>(frameSize), 0.f);
}

void FallbackMixer::Shutdown()
{
    m_left.clear();
    m_right.clear();
    m_mono.clear();
    m_frameSize = 0;
}

void FallbackMixer::BeginFrame(const RuntimeConfig& cfg, const ListenerState& listener, const RoomSend& roomSend)
{
    m_cfg = cfg;
    m_roomSend = roomSend;
    m_listener = listener;
    std::fill(m_left.begin(), m_left.end(), 0.f);
    std::fill(m_right.begin(), m_right.end(), 0.f);
}

void FallbackMixer::RenderSource(SoundSource& source, const float* const* input, uint32_t inputChannels, float gain)
{
    if (!input || inputChannels == 0 || m_frameSize <= 0)
        return;
    const size_t frames = static_cast<size_t>(m_frameSize);
    SoundSource::RenderState& rs = source.Render();
    const SourceParams params = source.GetParams();
    rs.lastParams = params;
    rs.roomSent = false;
    auto& fb = rs.fallback;

    const bool spatialize = !params.listenerRelative && params.spatialize != 0 && params.positionValid != 0 && m_listener.valid &&
                            (inputChannels == 1 || m_cfg.spatializeStereo);

    float targetL = params.engineGainL * params.gain;
    float targetR = params.engineGainR * params.gain;
    float targetLowpass = 1.f;
    float distMeters = 0.f;
    float distUnits = 0.f;

    if (spatialize) {
        CoordinateConverter converter;
        converter.SetUnitsPerMeter(m_cfg.unitsPerMeter);
        const Vec3 srcSA = converter.PositionToSA(params.position);
        const Vec3 origin{m_listener.frame.origin.x, m_listener.frame.origin.y, m_listener.frame.origin.z};
        const Vec3 rel = srcSA - origin;
        distMeters = rel.Length();
        distUnits = converter.LengthToSource(distMeters);
        const float distanceGain = params.engineGainValid && m_cfg.occlusion && params.occlusion
                                       ? params.engineDirectGain
                                       : SourceDistanceGain(params.distMult, distUnits, m_cfg.distanceGainMin, m_cfg.distanceGainMax);

        const Vec3 right{m_listener.frame.right.x, m_listener.frame.right.y, m_listener.frame.right.z};
        const Vec3 ahead{m_listener.frame.ahead.x, m_listener.frame.ahead.y, m_listener.frame.ahead.z};
        float x = 0.f;
        float y = 1.f;
        if (distMeters > 1e-3f) {
            const Vec3 dir = rel * (1.f / distMeters);
            x = Clamp(dir.Dot(right), -1.f, 1.f);
            y = Clamp(dir.Dot(ahead), -1.f, 1.f);
        }
        // Constant-power pan: x in [-1,1] -> angle in [0, pi/2].
        const float angle = (x + 1.f) * 0.25f * kPi;
        targetL = std::cos(angle) * distanceGain * params.gain;
        targetR = std::sin(angle) * distanceGain * params.gain;
        // Sources behind the listener lose highs (crude head shadow).
        const float behind = Clamp(-y, 0.f, 1.f);
        const float cutoff = Lerp(20000.f, 3500.f, behind);
        targetLowpass = cutoff >= 19000.f ? 1.f : LowpassCoefficient(cutoff, m_sampleRate);
    }
    RenderDiag diag;
    diag.mode = spatialize ? RenderDiag::kFallbackMixer : RenderDiag::kPassthrough;
    diag.distanceMeters = distMeters;
    source.PublishRenderDiag(diag);

    const float alpha = SmoothingAlpha(m_frameSize, m_sampleRate);
    if (!fb.primed) {
        fb.gainL = targetL;
        fb.gainR = targetR;
        fb.lowpassCoef = targetLowpass;
        fb.primed = true;
    }
    const float startL = fb.gainL;
    const float startR = fb.gainR;
    fb.gainL += (targetL - fb.gainL) * alpha;
    fb.gainR += (targetR - fb.gainR) * alpha;
    fb.lowpassCoef += (targetLowpass - fb.lowpassCoef) * alpha;

    if (spatialize) {
        // Downmix to mono, filter, pan.
        if (inputChannels == 1) {
            std::copy(input[0], input[0] + frames, m_mono.begin());
        } else {
            const float scale = 1.f / static_cast<float>(inputChannels);
            for (size_t i = 0; i < frames; ++i) {
                float acc = 0.f;
                for (uint32_t c = 0; c < inputChannels; ++c)
                    acc += input[c][i];
                m_mono[i] = acc * scale;
            }
        }
        if (m_cfg.doppler && m_cfg.dopplerScale > 0.f) {
            const float speed = std::max(m_cfg.speedOfSound, 1.f);
            rs.doppler.Process(m_mono.data(), frames,
                               distMeters / speed * m_cfg.dopplerScale * static_cast<float>(m_sampleRate),
                               static_cast<float>(m_sampleRate));
        } else if (rs.doppler.Primed()) {
            rs.doppler.Reset();
        }
        if (fb.lowpassCoef < 0.999f) {
            float state = fb.lowpassState;
            const float k = fb.lowpassCoef;
            for (size_t i = 0; i < frames; ++i) {
                state += (m_mono[i] - state) * k;
                m_mono[i] = state;
            }
            fb.lowpassState = state;
        } else {
            fb.lowpassState = m_mono[frames - 1];
        }
        for (size_t i = 0; i < frames; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(frames);
            const float gl = Lerp(startL, fb.gainL, t) * Lerp(rs.fadeStart, gain, t);
            const float gr = Lerp(startR, fb.gainR, t) * Lerp(rs.fadeStart, gain, t);
            m_left[i] += m_mono[i] * gl;
            m_right[i] += m_mono[i] * gr;
        }
        if (m_roomSend.Enabled() && (!m_cfg.physicalAcoustics || m_roomSend.always)) {
            const float mix = RoomMixForSource(*m_roomSend.preset, distUnits, DistMultToSoundLevel(params.distMult));
            // Approximate the attenuated direct level with the panned power sum.
            const float level = std::sqrt(fb.gainL * fb.gainL + fb.gainR * fb.gainR);
            if (mix > 0.f && level > 0.f) {
                dsp::MixInto(m_roomSend.bus, m_mono.data(), frames, gain * level * mix * params.reverbGain);
                rs.roomSent = true;
            }
        }
    } else {
        const float* l = input[0];
        const float* r = inputChannels > 1 ? input[1] : input[0];
        for (size_t i = 0; i < frames; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(frames);
            m_left[i] += l[i] * Lerp(startL, fb.gainL, t) * Lerp(rs.fadeStart, gain, t);
            m_right[i] += r[i] * Lerp(startR, fb.gainR, t) * Lerp(rs.fadeStart, gain, t);
        }
    }
}

void FallbackMixer::EndFrame()
{
    m_lastPeak = std::max(dsp::Peak(m_left.data(), m_left.size()), dsp::Peak(m_right.data(), m_right.size()));
}

} // namespace sa
