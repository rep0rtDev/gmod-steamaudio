// src/steamaudio/CPUFallbackBackend.cpp
#include "CPUFallbackBackend.h"

#include "PhononContext.h"
#include "util/Logging.h"

namespace sa {

CPUFallbackBackend::~CPUFallbackBackend()
{
    Shutdown();
}

bool CPUFallbackBackend::Initialize(PhononContext& context, const StaticConfig& config, std::string& errorOut)
{
    Shutdown();
    if (!context.IsValid()) {
        errorOut = "Steam Audio context not initialized";
        return false;
    }

    const bool tryEmbree = config.enableEmbree && sizeof(void*) == 8 &&
                           (config.sceneType == SceneTypePreference::Auto ||
                            config.sceneType == SceneTypePreference::Embree ||
                            config.sceneType == SceneTypePreference::RadeonRays);
    if (tryEmbree) {
        IPLEmbreeDeviceSettings embreeSettings{};
        const IPLerror err = iplEmbreeDeviceCreate(context.Handle(), &embreeSettings, &m_devices.embree);
        if (err == IPL_STATUS_SUCCESS && m_devices.embree) {
            m_devices.sceneType = IPL_SCENETYPE_EMBREE;
        } else {
            SA_LOGW("iplEmbreeDeviceCreate failed (%s); using built-in ray tracer", IplErrorToString(err));
            m_devices.embree = nullptr;
            m_devices.sceneType = IPL_SCENETYPE_DEFAULT;
        }
    } else {
        m_devices.sceneType = IPL_SCENETYPE_DEFAULT;
    }

    m_devices.reflectionType = ResolveReflectionType(config.reflectionType, false);
    m_devices.gpuAccelerated = false;
    m_devices.description = std::string("CPU; rays=") +
                            (m_devices.sceneType == IPL_SCENETYPE_EMBREE ? "Embree" : "Default") +
                            "; reflections=" +
                            (m_devices.reflectionType == IPL_REFLECTIONEFFECTTYPE_CONVOLUTION  ? "Convolution"
                             : m_devices.reflectionType == IPL_REFLECTIONEFFECTTYPE_PARAMETRIC ? "Parametric"
                                                                                               : "Hybrid");
    SA_LOGI("CPU backend ready: %s", m_devices.description.c_str());
    return true;
}

void CPUFallbackBackend::Shutdown()
{
    if (m_devices.embree)
        iplEmbreeDeviceRelease(&m_devices.embree);
    m_devices = BackendDevices{};
}

} // namespace sa
