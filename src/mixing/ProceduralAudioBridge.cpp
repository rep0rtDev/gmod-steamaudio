// src/mixing/ProceduralAudioBridge.cpp
#include "ProceduralAudioBridge.h"

#include <algorithm>

#include "Dsp.h"
#include "util/Logging.h"
#include "util/Math.h"

namespace sa {

void ProceduralAudioBridge::Initialize(uint32_t engineSampleRate, ProceduralSourceHooks hooks)
{
    Shutdown();
    m_engineSampleRate = engineSampleRate ? engineSampleRate : 44100;
    m_hooks = std::move(hooks);
    m_convertScratch.resize(8192);
}

void ProceduralAudioBridge::Shutdown()
{
    DestroyAll();
    m_hooks = ProceduralSourceHooks{};
}

uint32_t ProceduralAudioBridge::CreateStream(uint32_t sampleRate, uint32_t channels, float bufferSeconds,
                                             const std::string& name, const SourceParams& initialParams)
{
    if (!m_hooks.allocateId || !m_hooks.registerSource)
        return 0;
    if (sampleRate < kMinSampleRate || sampleRate > kMaxSampleRate) {
        SA_LOGW("CreateStream: unsupported sample rate %u", sampleRate);
        return 0;
    }
    channels = Clamp<uint32_t>(channels, 1, static_cast<uint32_t>(SoundSource::kMaxInputChannels));
    bufferSeconds = Clamp(bufferSeconds, 0.1f, 10.f);
    const size_t capacity = NextPowerOfTwo(static_cast<size_t>(static_cast<float>(sampleRate) * bufferSeconds));

    const uint32_t id = m_hooks.allocateId();
    if (id == 0)
        return 0;
    auto source = std::make_shared<SoundSource>(id, SourceKind::Procedural, channels, m_engineSampleRate, sampleRate,
                                                capacity);
    source->SetName(name.empty() ? std::string("procedural#") + std::to_string(id) : name);
    source->SetParams(initialParams);
    if (!m_hooks.registerSource(source)) {
        SA_LOGW("CreateStream: registration refused for %s", source->Name().c_str());
        return 0;
    }
    Stream stream;
    stream.source = std::move(source);
    m_streams.emplace(id, std::move(stream));
    SA_LOGD("Procedural stream %u created (%u Hz, %u ch, %.1fs buffer)", id, sampleRate, channels, bufferSeconds);
    return id;
}

SoundSourcePtr ProceduralAudioBridge::Find(uint32_t id) const
{
    auto it = m_streams.find(id);
    return it == m_streams.end() ? nullptr : it->second.source;
}

size_t ProceduralAudioBridge::WriteFloat(uint32_t id, const float* interleaved, size_t frames)
{
    auto it = m_streams.find(id);
    if (it == m_streams.end() || it->second.finished || !interleaved || frames == 0)
        return 0;
    SoundSource& src = *it->second.source;
    const size_t written = src.WriteInterleaved(interleaved, frames);
    if (written)
        src.TouchActivity(0);
    return written;
}

size_t ProceduralAudioBridge::WriteInt16(uint32_t id, const int16_t* interleaved, size_t frames)
{
    auto it = m_streams.find(id);
    if (it == m_streams.end() || it->second.finished || !interleaved || frames == 0)
        return 0;
    SoundSource& src = *it->second.source;
    const uint32_t channels = src.InputChannels();
    size_t total = 0;
    while (total < frames) {
        const size_t chunkFrames = std::min(frames - total, m_convertScratch.size() / channels);
        dsp::Int16ToFloat(interleaved + total * channels, m_convertScratch.data(), chunkFrames * channels);
        const size_t written = src.WriteInterleaved(m_convertScratch.data(), chunkFrames);
        total += written;
        if (written < chunkFrames)
            break;
    }
    if (total)
        src.TouchActivity(0);
    return total;
}

size_t ProceduralAudioBridge::FramesNeeded(uint32_t id) const
{
    auto it = m_streams.find(id);
    if (it == m_streams.end() || it->second.finished)
        return 0;
    const SoundSource& src = *it->second.source;
    const size_t queued = src.QueuedStreamFrames();
    const size_t target = src.StreamTargetFrames() * 2;
    return queued >= target ? 0 : target - queued;
}

size_t ProceduralAudioBridge::FramesQueued(uint32_t id) const
{
    auto it = m_streams.find(id);
    return it == m_streams.end() ? 0 : it->second.source->QueuedStreamFrames();
}

bool ProceduralAudioBridge::SetParams(uint32_t id, const SourceParams& params)
{
    auto it = m_streams.find(id);
    if (it == m_streams.end())
        return false;
    it->second.source->SetParams(params);
    return true;
}

bool ProceduralAudioBridge::Finish(uint32_t id)
{
    auto it = m_streams.find(id);
    if (it == m_streams.end())
        return false;
    it->second.finished = true;
    it->second.source->SetEndOfStream();
    return true;
}

bool ProceduralAudioBridge::Destroy(uint32_t id)
{
    auto it = m_streams.find(id);
    if (it == m_streams.end())
        return false;
    it->second.source->RequestStop();
    if (m_hooks.unregisterSource)
        m_hooks.unregisterSource(it->second.source);
    m_streams.erase(it);
    return true;
}

void ProceduralAudioBridge::DestroyAll()
{
    for (auto& kv : m_streams) {
        kv.second.source->RequestStop();
        if (m_hooks.unregisterSource)
            m_hooks.unregisterSource(kv.second.source);
    }
    m_streams.clear();
}

void ProceduralAudioBridge::Tick()
{
    for (auto it = m_streams.begin(); it != m_streams.end();) {
        if (it->second.source->RenderFinished()) {
            if (m_hooks.unregisterSource)
                m_hooks.unregisterSource(it->second.source);
            it = m_streams.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace sa
