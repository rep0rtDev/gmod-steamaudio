// src/platform/PlatformAudioOutput.h
//
// Native output device abstraction. The device thread pulls interleaved
// stereo float frames from the owner through IOutputSource; the owner
// (AudioThread) renders into a lock-free ring that the pull reads from.
//
// Implementations: WASAPI (Windows, shared-mode event-driven), Null (no
// device; consumes frames on a timer so the pipeline keeps flowing, used for
// dedicated servers / headless tests).
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace sa {

class IOutputSource {
public:
    virtual ~IOutputSource() = default;
    // Fills `frames` interleaved stereo float frames. Must be real-time safe.
    virtual void PullStereo(float* interleaved, size_t frames) = 0;
};

struct OutputRequest {
    int32_t sampleRate = 48000;   // preferred; the device may impose its own
    int32_t frameSize = 512;      // preferred period in frames
    std::string deviceId;         // empty = default endpoint
};

struct OutputFormat {
    int32_t sampleRate = 0;
    int32_t channels = 0;
    int32_t periodFrames = 0;
    int32_t bufferFrames = 0;
    std::string deviceName;
};

class IPlatformAudioOutput {
public:
    virtual ~IPlatformAudioOutput() = default;

    virtual bool Start(const OutputRequest& request, IOutputSource& source, OutputFormat& actual,
                       std::string& error) = 0;
    virtual void Stop() = 0;
    virtual bool IsRunning() const = 0;
    // True when the device was lost / the default endpoint changed and the
    // owner should Stop() + Start() again.
    virtual bool NeedsRestart() const = 0;
    virtual const OutputFormat& Format() const = 0;
    virtual uint64_t FramesDelivered() const = 0;
    virtual uint64_t Underruns() const = 0;
    virtual const char* BackendName() const = 0;
};

std::unique_ptr<IPlatformAudioOutput> CreateNullAudioOutput();
#ifdef _WIN32
std::unique_ptr<IPlatformAudioOutput> CreateWasapiAudioOutput();
#endif
// Best native backend for this platform (WASAPI on Windows, Null elsewhere).
std::unique_ptr<IPlatformAudioOutput> CreateNativeAudioOutput();

// Requests real-time-ish scheduling for the calling thread. Returns true when
// the OS honoured at least one of the requests (documented per platform).
bool ElevateCurrentThreadToAudioPriority(const char* threadName);
void SetCurrentThreadName(const char* name);

struct ProcessMemoryInfo {
    uint64_t physicalTotal = 0;
    uint64_t physicalAvailable = 0;
    uint64_t commitAvailable = 0;
    uint64_t virtualAvailable = UINT64_MAX;
    uint64_t privateBytes = 0;
    bool valid = false;
};

ProcessMemoryInfo QueryProcessMemory();

} // namespace sa
