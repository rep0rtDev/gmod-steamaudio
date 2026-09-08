// src/steamaudio/PhononContext.cpp
#include "PhononContext.h"

#include <cstdlib>
#include <cstring>

#include "util/Logging.h"

namespace sa {

namespace {

void IPLCALL PhononLogCallback(IPLLogLevel level, const char* message)
{
    // Steam Audio messages end with a newline; trim it.
    std::string text = message ? message : "";
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        text.pop_back();
    switch (level) {
    case IPL_LOGLEVEL_ERROR: SA_LOGE("[phonon] %s", text.c_str()); break;
    case IPL_LOGLEVEL_WARNING: SA_LOGW("[phonon] %s", text.c_str()); break;
    case IPL_LOGLEVEL_DEBUG: SA_LOGD("[phonon] %s", text.c_str()); break;
    case IPL_LOGLEVEL_INFO:
    default: SA_LOGI("[phonon] %s", text.c_str()); break;
    }
}

void* IPLCALL PhononAllocate(IPLsize size, IPLsize alignment)
{
#ifdef _WIN32
    return _aligned_malloc(size, alignment);
#else
    void* ptr = nullptr;
    if (posix_memalign(&ptr, alignment < sizeof(void*) ? sizeof(void*) : alignment, size) != 0)
        return nullptr;
    return ptr;
#endif
}

void IPLCALL PhononFree(void* memory)
{
#ifdef _WIN32
    _aligned_free(memory);
#else
    std::free(memory);
#endif
}

} // namespace

const char* IplErrorToString(IPLerror error)
{
    switch (error) {
    case IPL_STATUS_SUCCESS: return "success";
    case IPL_STATUS_FAILURE: return "unspecified failure";
    case IPL_STATUS_OUTOFMEMORY: return "out of memory";
    case IPL_STATUS_INITIALIZATION: return "initialization failure";
    default: return "unknown error";
    }
}

PhononContext::~PhononContext()
{
    Shutdown();
}

bool PhononContext::Initialize(const StaticConfig& config, const std::vector<std::string>& libraryDirectories,
                               std::string& errorOut)
{
    if (m_context)
        return true;

    if (!phonon::Load(libraryDirectories, errorOut))
        return false;

    IPLContextSettings settings{};
    settings.version = STEAMAUDIO_VERSION;
    settings.logCallback = PhononLogCallback;
    settings.allocateCallback = PhononAllocate;
    settings.freeCallback = PhononFree;
    settings.simdLevel = IPL_SIMDLEVEL_AVX2;
    settings.flags = config.validationLayer ? IPL_CONTEXTFLAGS_VALIDATION : static_cast<IPLContextFlags>(0);

    IPLContext context = nullptr;
    const IPLerror err = iplContextCreate(&settings, &context);
    if (err != IPL_STATUS_SUCCESS || !context) {
        errorOut = std::string("iplContextCreate failed: ") + IplErrorToString(err);
        return false;
    }
    m_context = context;

    m_audioSettings.samplingRate = config.sampleRate;
    m_audioSettings.frameSize = config.frameSize;

    SA_LOGI("Steam Audio context created (API %d.%d.%d, rate %d Hz, frame %d)", STEAMAUDIO_VERSION_MAJOR,
            STEAMAUDIO_VERSION_MINOR, STEAMAUDIO_VERSION_PATCH, m_audioSettings.samplingRate,
            m_audioSettings.frameSize);
    return true;
}

void PhononContext::Shutdown()
{
    if (m_context) {
        iplContextRelease(&m_context);
        m_context = nullptr;
    }
}

bool PhononContext::AudioBuffer::Allocate(const PhononContext& ctx, int32_t channels, int32_t samples)
{
    Free();
    if (!ctx.IsValid())
        return false;
    const IPLerror err = iplAudioBufferAllocate(ctx.Handle(), channels, samples, &m_buffer);
    if (err != IPL_STATUS_SUCCESS) {
        SA_LOGE("iplAudioBufferAllocate(%d ch, %d samples) failed: %s", channels, samples, IplErrorToString(err));
        m_buffer = IPLAudioBuffer{};
        return false;
    }
    m_context = ctx.Handle();
    Clear();
    return true;
}

void PhononContext::AudioBuffer::Free()
{
    if (m_context && m_buffer.data) {
        iplAudioBufferFree(m_context, &m_buffer);
    }
    m_buffer = IPLAudioBuffer{};
    m_context = nullptr;
}

void PhononContext::AudioBuffer::Clear()
{
    if (!m_buffer.data)
        return;
    for (int32_t c = 0; c < m_buffer.numChannels; ++c)
        std::memset(m_buffer.data[c], 0, sizeof(float) * static_cast<size_t>(m_buffer.numSamples));
}

} // namespace sa
