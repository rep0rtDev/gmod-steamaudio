// src/mixing/Dsp.h
//
// Small real-time-safe DSP helpers: format conversion, interleaving, gain
// ramps, soft clipping, and a fractional-ratio resampler (4-point cubic
// Hermite). None of these functions allocate.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sa {
namespace dsp {

constexpr float kInt16Scale = 1.f / 32768.f;
constexpr float kInt8Scale = 1.f / 128.f;

inline void Int16ToFloat(const int16_t* in, float* out, size_t count)
{
    for (size_t i = 0; i < count; ++i)
        out[i] = static_cast<float>(in[i]) * kInt16Scale;
}

inline void Int16ToFloatStrided(const int16_t* in, size_t stride, float* out, size_t count)
{
    for (size_t i = 0; i < count; ++i)
        out[i] = static_cast<float>(in[i * stride]) * kInt16Scale;
}

// Source's 8-bit PCM is signed (it converts unsigned wav bytes on load).
inline void Int8ToFloat(const int8_t* in, float* out, size_t count)
{
    for (size_t i = 0; i < count; ++i)
        out[i] = static_cast<float>(in[i]) * kInt8Scale;
}

inline void Int8ToFloatStrided(const int8_t* in, size_t stride, float* out, size_t count)
{
    for (size_t i = 0; i < count; ++i)
        out[i] = static_cast<float>(in[i * stride]) * kInt8Scale;
}

inline int16_t FloatToInt16(float v)
{
    const float scaled = v * 32767.f;
    if (scaled >= 32767.f)
        return 32767;
    if (scaled <= -32768.f)
        return -32768;
    return static_cast<int16_t>(std::lrint(scaled));
}

inline void FloatToInt16(const float* in, int16_t* out, size_t count)
{
    for (size_t i = 0; i < count; ++i)
        out[i] = FloatToInt16(in[i]);
}

inline void Interleave2(const float* left, const float* right, float* out, size_t frames)
{
    for (size_t i = 0; i < frames; ++i) {
        out[i * 2] = left[i];
        out[i * 2 + 1] = right[i];
    }
}

inline void Deinterleave(const float* in, float* const* out, size_t channels, size_t frames)
{
    for (size_t f = 0; f < frames; ++f)
        for (size_t c = 0; c < channels; ++c)
            out[c][f] = in[f * channels + c];
}

inline void Interleave(const float* const* in, float* out, size_t channels, size_t frames)
{
    for (size_t f = 0; f < frames; ++f)
        for (size_t c = 0; c < channels; ++c)
            out[f * channels + c] = in[c][f];
}

inline void ApplyGain(float* buffer, size_t count, float gain)
{
    for (size_t i = 0; i < count; ++i)
        buffer[i] *= gain;
}

inline void MixInto(float* dst, const float* src, size_t count, float gain = 1.f)
{
    for (size_t i = 0; i < count; ++i)
        dst[i] += src[i] * gain;
}

// Linear gain ramp from `from` to `to` across the buffer.
inline void ApplyGainRamp(float* buffer, size_t count, float from, float to)
{
    if (count == 0)
        return;
    const float step = (to - from) / static_cast<float>(count);
    float g = from;
    for (size_t i = 0; i < count; ++i) {
        buffer[i] *= g;
        g += step;
    }
}

// tanh-style soft clipper keeping |x| < 1.
inline float SoftClip(float x)
{
    if (x > 1.5f)
        return 1.f;
    if (x < -1.5f)
        return -1.f;
    if (x > -0.5f && x < 0.5f)
        return x;
    // Smooth cubic knee between 0.5 and 1.5.
    const float sign = x < 0.f ? -1.f : 1.f;
    const float a = std::fabs(x);
    const float t = (a - 0.5f);           // 0..1
    const float y = 0.5f + t - t * t * 0.5f; // reaches 1.0 at a = 1.5
    return sign * std::min(y, 1.f);
}

inline void SoftClipBuffer(float* buffer, size_t count)
{
    for (size_t i = 0; i < count; ++i)
        buffer[i] = SoftClip(buffer[i]);
}

inline void Zero(float* buffer, size_t count)
{
    std::fill(buffer, buffer + count, 0.f);
}

inline float Peak(const float* buffer, size_t count)
{
    float peak = 0.f;
    for (size_t i = 0; i < count; ++i)
        peak = std::max(peak, std::fabs(buffer[i]));
    return peak;
}

// True when every sample is a finite number (no NaN/Inf).
inline bool AllFinite(const float* buffer, size_t count)
{
    for (size_t i = 0; i < count; ++i)
        if (!std::isfinite(buffer[i]))
            return false;
    return true;
}

// One-pole smoother for parameter changes (per-sample coefficient).
struct Smoother {
    float value = 0.f;
    float coeff = 0.001f;

    void SetTimeConstant(float seconds, float sampleRate)
    {
        coeff = seconds > 0.f ? 1.f - std::exp(-1.f / (seconds * sampleRate)) : 1.f;
    }
    float Next(float target)
    {
        value += (target - value) * coeff;
        return value;
    }
    void Snap(float v) { value = v; }
};

// 4-point cubic Hermite interpolation between y1 and y2 at fraction t.
inline float Hermite(float y0, float y1, float y2, float y3, float t)
{
    const float c0 = y1;
    const float c1 = 0.5f * (y2 - y0);
    const float c2 = y0 - 2.5f * y1 + 2.f * y2 - 0.5f * y3;
    const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * t + c2) * t + c1) * t + c0;
}

// Streaming fractional resampler. Input is appended to an internal FIFO
// (allocated once by Prepare); output is pulled at a given ratio (input
// samples per output sample). Unconsumed input is retained so no samples are
// ever dropped at block boundaries.
class Resampler {
public:
    // Allocates the input FIFO. Not real-time safe; call once at setup.
    void Prepare(size_t maxBufferedInput)
    {
        m_buffer.assign(maxBufferedInput + 8, 0.f);
        Reset();
    }

    void Reset()
    {
        // Start with 3 zero samples of history so the first output has a
        // full interpolation window.
        m_size = std::min<size_t>(3, m_buffer.size());
        std::fill(m_buffer.begin(), m_buffer.begin() + static_cast<std::ptrdiff_t>(m_size), 0.f);
        m_pos = 0.0;
    }

    size_t Buffered() const { return m_size; }
    size_t FreeSpace() const { return m_buffer.size() - m_size; }

    // Appends input; returns the number of samples accepted.
    size_t Push(const float* in, size_t count)
    {
        const size_t n = std::min(count, FreeSpace());
        if (n)
            std::copy(in, in + n, m_buffer.begin() + static_cast<std::ptrdiff_t>(m_size));
        m_size += n;
        return n;
    }

    // Number of input samples required (beyond what is buffered) to produce
    // `outCount` outputs at `ratio`.
    size_t InputNeeded(size_t outCount, double ratio) const
    {
        if (outCount == 0)
            return 0;
        // Last output index k = outCount-1 needs floor(pos + k*ratio) + 3 < size.
        const double last = m_pos + static_cast<double>(outCount - 1) * ratio;
        const size_t needSize = static_cast<size_t>(last) + 4;
        return needSize > m_size ? needSize - m_size : 0;
    }

    // Produces up to `outCount` samples. Returns how many were produced.
    size_t Pull(float* out, size_t outCount, double ratio)
    {
        size_t produced = 0;
        while (produced < outCount) {
            const auto ipos = static_cast<size_t>(m_pos);
            if (ipos + 3 >= m_size)
                break;
            const float t = static_cast<float>(m_pos - static_cast<double>(ipos));
            out[produced++] = Hermite(m_buffer[ipos], m_buffer[ipos + 1], m_buffer[ipos + 2], m_buffer[ipos + 3], t);
            m_pos += ratio;
        }
        // Discard fully consumed prefix, keeping the window start.
        const auto keepFrom = std::min(static_cast<size_t>(m_pos), m_size);
        if (keepFrom > 0) {
            std::copy(m_buffer.begin() + static_cast<std::ptrdiff_t>(keepFrom),
                      m_buffer.begin() + static_cast<std::ptrdiff_t>(m_size), m_buffer.begin());
            m_size -= keepFrom;
            m_pos -= static_cast<double>(keepFrom);
        }
        return produced;
    }

private:
    std::vector<float> m_buffer;
    size_t m_size = 0;
    double m_pos = 0.0;
};

// Variable fractional delay line modelling sound propagation time. Sweeping
// the delay as a source approaches or recedes produces the Doppler shift
// naturally (the read head moves faster or slower than the write head).
// The delay ramps linearly across each block and its rate of change is
// bounded so a sudden position jump cannot produce an extreme pitch glide;
// jumps beyond `kSnapThreshold` seconds snap instead.
class PropagationDelay {
public:
    static constexpr float kMaxRatePerSample = 0.5f;   // |d(delay)/dt| <= 0.5 -> pitch within [0.5x, 1.5x]
    static constexpr float kSnapThreshold = 0.25f;     // seconds

    // Allocates the line. Not real-time safe; call once at setup.
    void Prepare(size_t maxDelaySamples, size_t maxBlockSamples)
    {
        size_t n = 16;
        while (n < maxDelaySamples + maxBlockSamples + 8)
            n <<= 1;
        m_buffer.assign(n, 0.f);
        m_mask = n - 1;
        m_maxDelay = static_cast<float>(maxDelaySamples);
        Reset();
    }

    void Reset()
    {
        std::fill(m_buffer.begin(), m_buffer.end(), 0.f);
        // Start one full wrap in so `write - delay` never goes negative.
        m_write = m_buffer.size();
        m_delay = 0.f;
        m_primed = false;
    }

    bool IsPrepared() const { return !m_buffer.empty(); }
    bool Primed() const { return m_primed; }
    float CurrentDelay() const { return m_delay; }

    // Processes `frames` samples in place, moving the delay from its current
    // value towards `targetDelaySamples` over the block. `sampleRate` is
    // used to convert the snap threshold.
    void Process(float* buffer, size_t frames, float targetDelaySamples, float sampleRate)
    {
        if (m_buffer.empty() || frames == 0 || frames + static_cast<size_t>(m_maxDelay) + 8 > m_buffer.size())
            return; // block larger than prepared for: leave the audio untouched
        if (!std::isfinite(targetDelaySamples))
            targetDelaySamples = m_delay;
        const float target = std::min(std::max(targetDelaySamples, 0.f), m_maxDelay);
        if (!m_primed || std::fabs(target - m_delay) > kSnapThreshold * sampleRate) {
            m_delay = target;
            m_primed = true;
        }
        const float maxStep = kMaxRatePerSample * static_cast<float>(frames);
        const float end = m_delay + std::min(std::max(target - m_delay, -maxStep), maxStep);
        const float step = (end - m_delay) / static_cast<float>(frames);
        float delay = m_delay;
        for (size_t i = 0; i < frames; ++i) {
            m_buffer[m_write & m_mask] = buffer[i];
            const float readPos = static_cast<float>(m_write) - delay;
            const auto whole = static_cast<size_t>(std::floor(readPos));
            const float frac = readPos - static_cast<float>(whole);
            // Hermite around the read position (guarded by the +8 slack in Prepare).
            const float y0 = m_buffer[(whole - 1) & m_mask];
            const float y1 = m_buffer[whole & m_mask];
            const float y2 = m_buffer[(whole + 1) & m_mask];
            const float y3 = m_buffer[(whole + 2) & m_mask];
            buffer[i] = Hermite(y0, y1, y2, y3, frac);
            // Keep the write index in [n, 2n): masked indices are unchanged
            // and the float conversion above never loses integer precision.
            if (++m_write == 2 * m_buffer.size())
                m_write = m_buffer.size();
            delay += step;
        }
        m_delay = end;
    }

private:
    std::vector<float> m_buffer;
    size_t m_mask = 0;
    size_t m_write = 0;
    float m_maxDelay = 0.f;
    float m_delay = 0.f;
    bool m_primed = false;
};

// Second-order Butterworth low-pass (transposed direct form II). Coefficients
// are recomputed only when the cutoff changes.
class BiquadLowpass {
public:
    void SetCutoff(float cutoffHz, float sampleRate)
    {
        const float nyquist = sampleRate * 0.5f;
        cutoffHz = std::min(std::max(cutoffHz, 10.f), nyquist * 0.99f);
        if (cutoffHz == m_cutoff && sampleRate == m_sampleRate)
            return;
        m_cutoff = cutoffHz;
        m_sampleRate = sampleRate;
        const float w0 = 2.f * 3.14159265358979f * cutoffHz / sampleRate;
        const float cosw = std::cos(w0);
        const float alpha = std::sin(w0) / (2.f * 0.70710678f);
        const float a0 = 1.f + alpha;
        m_b0 = (1.f - cosw) * 0.5f / a0;
        m_b1 = (1.f - cosw) / a0;
        m_b2 = m_b0;
        m_a1 = -2.f * cosw / a0;
        m_a2 = (1.f - alpha) / a0;
    }

    float Cutoff() const { return m_cutoff; }
    void Reset() { m_z1 = m_z2 = 0.f; }

    float Next(float x)
    {
        const float y = m_b0 * x + m_z1;
        m_z1 = m_b1 * x - m_a1 * y + m_z2;
        m_z2 = m_b2 * x - m_a2 * y;
        return y;
    }

    void Process(float* buffer, size_t count)
    {
        for (size_t i = 0; i < count; ++i)
            buffer[i] = Next(buffer[i]);
    }

private:
    float m_cutoff = 0.f;
    float m_sampleRate = 0.f;
    float m_b0 = 1.f, m_b1 = 0.f, m_b2 = 0.f, m_a1 = 0.f, m_a2 = 0.f;
    float m_z1 = 0.f, m_z2 = 0.f;
};

} // namespace dsp
} // namespace sa
