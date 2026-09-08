// src/mixing/ProceduralAudioBridge.h
//
// Live PCM sources for procedurally generated audio. Lua (or other in-process
// generators) push interleaved sample blocks into an SPSC ring; the audio
// thread pulls, resamples and spatializes them like any other source, so
// synthesized engine noises, dynamic music layers or generated tones go
// through the full Steam Audio pipeline instead of being pre-rendered to a
// WAV (which is what `sound.Generate` does).
//
// Thread-safety: all methods are called on the game thread. Sample writes go
// straight into the source's lock-free ring (game thread = producer, audio
// thread = consumer).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "SoundSource.h"

namespace sa {

struct ProceduralSourceHooks {
    // Allocates a unique source id.
    std::function<uint32_t()> allocateId;
    // Registers the source with the simulation/audio threads. Returns false if refused.
    std::function<bool(const SoundSourcePtr&)> registerSource;
    // Tells the threads to drop the source (they release their references asynchronously).
    std::function<void(const SoundSourcePtr&)> unregisterSource;
};

class ProceduralAudioBridge {
public:
    static constexpr uint32_t kMinSampleRate = 8000;
    static constexpr uint32_t kMaxSampleRate = 192000;

    void Initialize(uint32_t engineSampleRate, ProceduralSourceHooks hooks);
    void Shutdown();

    // Creates a stream. `bufferSeconds` sizes the ring (clamped to [0.1, 10]).
    // Returns 0 on failure.
    uint32_t CreateStream(uint32_t sampleRate, uint32_t channels, float bufferSeconds, const std::string& name,
                          const SourceParams& initialParams);
    bool Exists(uint32_t id) const { return m_streams.count(id) != 0; }
    SoundSourcePtr Find(uint32_t id) const;

    // Writers return the number of frames accepted (may be fewer than offered).
    size_t WriteFloat(uint32_t id, const float* interleaved, size_t frames);
    size_t WriteInt16(uint32_t id, const int16_t* interleaved, size_t frames);
    // Frames the producer should push now to reach the target fill level.
    size_t FramesNeeded(uint32_t id) const;
    size_t FramesQueued(uint32_t id) const;

    bool SetParams(uint32_t id, const SourceParams& params);
    // Marks the end of the stream; the source finishes when the ring drains.
    bool Finish(uint32_t id);
    // Stops immediately (fade-out on the audio thread).
    bool Destroy(uint32_t id);
    void DestroyAll();

    // Drops bookkeeping for streams whose rendering has finished.
    void Tick();
    size_t Count() const { return m_streams.size(); }

private:
    struct Stream {
        SoundSourcePtr source;
        bool finished = false;
    };

    uint32_t m_engineSampleRate = 44100;
    ProceduralSourceHooks m_hooks;
    std::unordered_map<uint32_t, Stream> m_streams;
    std::vector<float> m_convertScratch;
};

} // namespace sa
