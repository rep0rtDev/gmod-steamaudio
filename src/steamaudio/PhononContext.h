// src/steamaudio/PhononContext.h
//
// Owns the IPLContext and the global IPLAudioSettings. Everything else in the
// Steam Audio layer takes a reference to a PhononContext.
//
// Thread-safety: the context handle is immutable after Initialize(); Steam
// Audio's context is internally thread-safe. Initialize/Shutdown are called
// from the game thread only.
#pragma once

#include <string>
#include <vector>

#include "PhononApi.h"
#include "steamaudio/Config.h"

namespace sa {

class PhononContext {
public:
    PhononContext() = default;
    ~PhononContext();

    PhononContext(const PhononContext&) = delete;
    PhononContext& operator=(const PhononContext&) = delete;

    // Loads phonon.dll (if not yet loaded), creates the IPLContext and stores
    // the audio settings. `libraryDirectories` are probed for phonon.dll.
    bool Initialize(const StaticConfig& config, const std::vector<std::string>& libraryDirectories,
                    std::string& errorOut);
    void Shutdown();

    bool IsValid() const { return m_context != nullptr; }
    IPLContext Handle() const { return m_context; }
    const IPLAudioSettings& AudioSettings() const { return m_audioSettings; }
    IPLAudioSettings* MutableAudioSettings() { return &m_audioSettings; }
    int32_t SampleRate() const { return m_audioSettings.samplingRate; }
    int32_t FrameSize() const { return m_audioSettings.frameSize; }

    // RAII helper for IPLAudioBuffer.
    class AudioBuffer {
    public:
        AudioBuffer() = default;
        ~AudioBuffer() { Free(); }
        AudioBuffer(const AudioBuffer&) = delete;
        AudioBuffer& operator=(const AudioBuffer&) = delete;

        bool Allocate(const PhononContext& ctx, int32_t channels, int32_t samples);
        void Free();
        void Clear();
        IPLAudioBuffer* Get() { return &m_buffer; }
        const IPLAudioBuffer* Get() const { return &m_buffer; }
        int32_t Channels() const { return m_buffer.numChannels; }
        int32_t Samples() const { return m_buffer.numSamples; }
        float* Channel(int32_t c) { return m_buffer.data[c]; }
        bool IsValid() const { return m_buffer.data != nullptr; }

    private:
        IPLContext m_context = nullptr;
        IPLAudioBuffer m_buffer{};
    };

private:
    IPLContext m_context = nullptr;
    IPLAudioSettings m_audioSettings{};
};

const char* IplErrorToString(IPLerror error);

} // namespace sa
