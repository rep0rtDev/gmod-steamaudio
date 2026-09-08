// src/mixing/SoundSource.cpp
#include "SoundSource.h"

#include <algorithm>
#include <cmath>

#include "util/Logging.h"

namespace sa {

SoundSource::SoundSource(uint32_t id, SourceKind kind, uint32_t inputChannels, uint32_t engineSampleRate,
                         uint32_t streamSampleRate, size_t ringCapacitySamples)
    : m_id(id), m_kind(kind), m_inputChannels(std::min<uint32_t>(std::max<uint32_t>(inputChannels, 1), kMaxInputChannels)),
      m_engineSampleRate(engineSampleRate), m_streamSampleRate(streamSampleRate ? streamSampleRate : engineSampleRate)
{
    if (kind == SourceKind::EngineChannel) {
        for (uint32_t c = 0; c < m_inputChannels; ++c)
            m_timedRings.push_back(std::make_unique<TimedSampleRing>(ringCapacitySamples));
    } else {
        for (uint32_t c = 0; c < m_inputChannels; ++c)
            m_streamRings.push_back(std::make_unique<SpscSampleRing>(ringCapacitySamples));
        // Keep ~100 ms of stream queued to absorb producer jitter.
        m_streamTargetFrames = std::max<size_t>(m_streamSampleRate / 10, 1024);
        m_streamTargetFrames = std::min(m_streamTargetFrames, m_streamRings[0]->Capacity() / 2);
        m_resampleScratch.resize(4096);
        for (auto& rs : m_resamplers)
            rs.Prepare(8192);
    }
    m_render.streamRatio = static_cast<double>(m_streamSampleRate) / static_cast<double>(m_engineSampleRate);
    m_render.doppler.Prepare(
        static_cast<size_t>(kMaxPropagationDelaySeconds * static_cast<float>(m_engineSampleRate)), kMaxRenderBlock);
}

SoundSource::~SoundSource() = default;

void SoundSource::ResetTimedRings(uint64_t clock)
{
    for (auto& ring : m_timedRings)
        ring->Reset(clock);
}

size_t SoundSource::WriteInterleaved(const float* interleaved, size_t frames)
{
    if (m_streamRings.empty() || frames == 0)
        return 0;
    size_t writable = SIZE_MAX;
    for (auto& ring : m_streamRings)
        writable = std::min(writable, ring->WritableCount());
    const size_t n = std::min(frames, writable);
    if (n == 0) {
        SA_LOG_RT(Overrun);
        return 0;
    }
    if (m_inputChannels == 1 && dsp::AllFinite(interleaved, n) && dsp::Peak(interleaved, n) <= 1.f) {
        m_streamRings[0]->Write(interleaved, n);
    } else {
        // Deinterleave through a small stack buffer in chunks.
        float chunk[256];
        size_t done = 0;
        while (done < n) {
            const size_t count = std::min<size_t>(256, n - done);
            for (uint32_t c = 0; c < m_inputChannels; ++c) {
                for (size_t i = 0; i < count; ++i) {
                    const float sample = interleaved[(done + i) * m_inputChannels + c];
                    chunk[i] = std::isfinite(sample) ? std::clamp(sample, -1.f, 1.f) : 0.f;
                }
                m_streamRings[c]->Write(chunk, count);
            }
            done += count;
        }
    }
    return n;
}

size_t SoundSource::QueuedStreamFrames() const
{
    if (m_streamRings.empty())
        return 0;
    size_t queued = SIZE_MAX;
    for (const auto& ring : m_streamRings)
        queued = std::min(queued, ring->ReadableCount());
    return queued;
}

size_t SoundSource::ReadStreamFrames(float* const* out, size_t frames)
{
    if (m_streamRings.empty() || frames == 0)
        return 0;

    const size_t queued = QueuedStreamFrames();
    const bool eos = EndOfStream();

    // Wait until the producer has primed the ring (unless it already ended).
    if (!m_streamPrimed) {
        if (queued < m_streamTargetFrames / 2 && !eos) {
            for (uint32_t c = 0; c < m_inputChannels; ++c)
                dsp::Zero(out[c], frames);
            return 0;
        }
        m_streamPrimed = true;
    }

    // Drift correction: nudge the resampling ratio so the queue converges on
    // the target fill level. +-0.5% maximum deviation keeps pitch shifts
    // inaudible.
    const float scale = StreamRateScale();
    const double pitch = m_kind == SourceKind::Procedural ? Clamp(GetParams().pitch, 0.05f, 20.f) : 1.f;
    const double nominal = static_cast<double>(m_streamSampleRate) / static_cast<double>(m_engineSampleRate) *
                           static_cast<double>(std::isfinite(scale) ? std::clamp(scale, 0.05f, 20.f) : 1.f) * pitch;
    if (!eos && m_streamTargetFrames > 0) {
        const double error = (static_cast<double>(queued) - static_cast<double>(m_streamTargetFrames)) /
                             static_cast<double>(m_streamTargetFrames);
        const double correction = std::clamp(error * 0.01, -0.005, 0.005);
        m_render.streamRatio += ((nominal * (1.0 + correction)) - m_render.streamRatio) * 0.1;
    } else {
        m_render.streamRatio = nominal;
    }
    const double ratio = m_render.streamRatio;

    size_t produced = 0;
    for (uint32_t c = 0; c < m_inputChannels; ++c) {
        SpscSampleRing& ring = *m_streamRings[c];
        dsp::Resampler& rs = m_resamplers[c];
        size_t n = 0;
        size_t remainingInput = queued;
        while (n < frames) {
            size_t need = rs.InputNeeded(frames - n, ratio);
            need = std::min({need, remainingInput, rs.FreeSpace(), m_resampleScratch.size()});
            if (need) {
                const size_t got = ring.Read(m_resampleScratch.data(), need);
                rs.Push(m_resampleScratch.data(), got);
                remainingInput -= got;
            }
            const size_t got = rs.Pull(out[c] + n, frames - n, ratio);
            n += got;
            if (got == 0 && need == 0)
                break;
        }
        if (n < frames)
            dsp::Zero(out[c] + n, frames - n);
        produced = std::max(produced, n);
    }
    if (produced < frames && !eos)
        SA_LOG_RT(Underrun);
    return produced;
}

} // namespace sa
