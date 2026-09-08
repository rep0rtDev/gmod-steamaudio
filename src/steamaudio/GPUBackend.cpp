// src/steamaudio/GPUBackend.cpp
#include "GPUBackend.h"

#include <algorithm>

#include "PhononContext.h"
#include "util/Logging.h"

namespace sa {

IPLReflectionEffectType ResolveReflectionType(ReflectionTypePreference preference, bool tanAvailable)
{
    switch (preference) {
    case ReflectionTypePreference::Convolution: return IPL_REFLECTIONEFFECTTYPE_CONVOLUTION;
    case ReflectionTypePreference::Parametric: return IPL_REFLECTIONEFFECTTYPE_PARAMETRIC;
    case ReflectionTypePreference::Hybrid: return IPL_REFLECTIONEFFECTTYPE_HYBRID;
    case ReflectionTypePreference::TrueAudioNext:
        return tanAvailable ? IPL_REFLECTIONEFFECTTYPE_TAN : IPL_REFLECTIONEFFECTTYPE_HYBRID;
    }
    return IPL_REFLECTIONEFFECTTYPE_HYBRID;
}

namespace {

IPLerror CreateGpuDeviceList(PhononContext& context, const StaticConfig& config,
                             const phonon::GpuRuntime& runtime, IPLOpenCLDeviceList& list)
{
    IPLOpenCLDeviceSettings settings{};
    settings.type = IPL_OPENCLDEVICETYPE_GPU;
    if (runtime.gpuUtilities && (config.gpuComputeUnits > 0 || (config.enableTan && runtime.trueAudioNext))) {
        settings.numCUsToReserve = std::max(0, config.gpuComputeUnits);
        settings.fractionCUsForIRUpdate = std::clamp(config.gpuIrUpdateFraction, 0.f, 1.f);
        settings.requiresTAN = config.enableTan && runtime.trueAudioNext ? IPL_TRUE : IPL_FALSE;
        const IPLerror error = iplOpenCLDeviceListCreate(context.Handle(), &settings, &list);
        if (error == IPL_STATUS_SUCCESS && list && iplOpenCLDeviceListGetNumDevices(list) > 0)
            return error;
        if (list) iplOpenCLDeviceListRelease(&list);
        SA_LOGI("No reserved/TAN GPU available; trying standard OpenCL without AMD CU reservation");
    }
    settings.numCUsToReserve = 0;
    settings.fractionCUsForIRUpdate = 0.f;
    settings.requiresTAN = IPL_FALSE;
    const IPLerror error = iplOpenCLDeviceListCreate(context.Handle(), &settings, &list);
    if (error != IPL_STATUS_SUCCESS && list) iplOpenCLDeviceListRelease(&list);
    return error;
}

}

GPUBackend::~GPUBackend()
{
    Shutdown();
}

std::vector<GpuDeviceInfo> GPUBackend::Enumerate(PhononContext& context, const StaticConfig& config)
{
    std::vector<GpuDeviceInfo> result;
    if (!IsSupportedBuild() || !context.IsValid())
        return result;

    const phonon::GpuRuntime runtime = phonon::PreloadGpuRuntime();
    if (!runtime.openCL)
        return result;

    IPLOpenCLDeviceList list = nullptr;
    const IPLerror err = CreateGpuDeviceList(context, config, runtime, list);
    if (err != IPL_STATUS_SUCCESS || !list)
        return result;

    const IPLint32 count = iplOpenCLDeviceListGetNumDevices(list);
    for (IPLint32 i = 0; i < count; ++i) {
        IPLOpenCLDeviceDesc desc{};
        iplOpenCLDeviceListGetDeviceDesc(list, i, &desc);
        GpuDeviceInfo info;
        info.index = i;
        info.type = desc.type;
        info.numConvolutionCUs = desc.numConvolutionCUs;
        info.numIRUpdateCUs = desc.numIRUpdateCUs;
        info.granularity = desc.granularity;
        info.perfScore = desc.perfScore;
        result.push_back(info);
    }
    iplOpenCLDeviceListRelease(&list);
    return result;
}

bool GPUBackend::Initialize(PhononContext& context, const StaticConfig& config, std::string& errorOut)
{
    Shutdown();

    if (!IsSupportedBuild()) {
        errorOut = "GPU acceleration (OpenCL/Radeon Rays/TrueAudio Next) requires the 64-bit build";
        return false;
    }
    if (!context.IsValid()) {
        errorOut = "Steam Audio context not initialized";
        return false;
    }

    const phonon::GpuRuntime runtime = phonon::PreloadGpuRuntime();
    if (!runtime.openCL) {
        errorOut = "OpenCL runtime not found; install the GPU driver's OpenCL runtime";
        return false;
    }
    const bool tanAllowed = config.enableTan && runtime.gpuUtilities && runtime.trueAudioNext;
    if (config.enableTan && !runtime.trueAudioNext)
        SA_LOGW("TrueAudioNext.dll not found next to phonon.dll; TAN convolution disabled");

    IPLerror err = CreateGpuDeviceList(context, config, runtime, m_deviceList);
    if (err != IPL_STATUS_SUCCESS || !m_deviceList) {
        errorOut = std::string("iplOpenCLDeviceListCreate failed: ") + IplErrorToString(err);
        m_deviceList = nullptr;
        return false;
    }

    const IPLint32 count = iplOpenCLDeviceListGetNumDevices(m_deviceList);
    if (count <= 0) {
        errorOut = "no OpenCL GPU devices satisfy the requirements";
        Shutdown();
        return false;
    }

    // Pick the device with the best performance score.
    IPLint32 best = 0;
    IPLOpenCLDeviceDesc bestDesc{};
    for (IPLint32 i = 0; i < count; ++i) {
        IPLOpenCLDeviceDesc desc{};
        iplOpenCLDeviceListGetDeviceDesc(m_deviceList, i, &desc);
        SA_LOGI("OpenCL device %d: %s (%s), type=%d convCUs=%d irCUs=%d granularity=%d perf=%.2f", i,
                desc.deviceName ? desc.deviceName : "unknown", desc.deviceVendor ? desc.deviceVendor : "unknown",
                static_cast<int>(desc.type), desc.numConvolutionCUs, desc.numIRUpdateCUs, desc.granularity,
                desc.perfScore);
        if (i == 0 || desc.perfScore > bestDesc.perfScore) {
            best = i;
            bestDesc = desc;
        }
    }

    err = iplOpenCLDeviceCreate(context.Handle(), m_deviceList, best, &m_devices.openCL);
    if (err != IPL_STATUS_SUCCESS || !m_devices.openCL) {
        errorOut = std::string("iplOpenCLDeviceCreate failed: ") + IplErrorToString(err);
        Shutdown();
        return false;
    }

    // Radeon Rays for ray tracing (optional; fall back to Embree/default if it fails).
    bool wantRadeonRays = config.sceneType == SceneTypePreference::Auto ||
                          config.sceneType == SceneTypePreference::RadeonRays;
    if (wantRadeonRays) {
        IPLRadeonRaysDeviceSettings rrSettings{};
        err = iplRadeonRaysDeviceCreate(m_devices.openCL, &rrSettings, &m_devices.radeonRays);
        if (err != IPL_STATUS_SUCCESS || !m_devices.radeonRays) {
            SA_LOGW("iplRadeonRaysDeviceCreate failed (%s); using CPU ray tracing with GPU convolution",
                    IplErrorToString(err));
            m_devices.radeonRays = nullptr;
        }
    }

    if (m_devices.radeonRays) {
        m_devices.sceneType = IPL_SCENETYPE_RADEONRAYS;
    } else if (config.enableEmbree && config.sceneType != SceneTypePreference::Default) {
        IPLEmbreeDeviceSettings embreeSettings{};
        err = iplEmbreeDeviceCreate(context.Handle(), &embreeSettings, &m_devices.embree);
        if (err == IPL_STATUS_SUCCESS && m_devices.embree) {
            m_devices.sceneType = IPL_SCENETYPE_EMBREE;
        } else {
            m_devices.embree = nullptr;
            m_devices.sceneType = IPL_SCENETYPE_DEFAULT;
        }
    } else {
        m_devices.sceneType = IPL_SCENETYPE_DEFAULT;
    }

    // TrueAudio Next for convolution.
    if (tanAllowed && bestDesc.numConvolutionCUs > 0) {
        IPLTrueAudioNextDeviceSettings tanSettings{};
        tanSettings.frameSize = config.frameSize;
        tanSettings.irSize = static_cast<IPLint32>(config.maxIrDuration * static_cast<float>(config.sampleRate));
        tanSettings.order = config.maxAmbisonicOrder;
        tanSettings.maxSources = config.maxSources;
        err = iplTrueAudioNextDeviceCreate(m_devices.openCL, &tanSettings, &m_devices.tan);
        if (err != IPL_STATUS_SUCCESS || !m_devices.tan) {
            SA_LOGW("iplTrueAudioNextDeviceCreate failed (%s); using CPU convolution", IplErrorToString(err));
            m_devices.tan = nullptr;
        }
    }

    m_devices.reflectionType = ResolveReflectionType(config.reflectionType, m_devices.tan != nullptr);
    if (m_devices.tan && config.reflectionType == ReflectionTypePreference::Hybrid) {
        // Hybrid rendering is CPU-only; when TAN is available and the user did not
        // explicitly ask for a CPU convolution type, prefer the GPU convolver.
        m_devices.reflectionType = IPL_REFLECTIONEFFECTTYPE_TAN;
    }

    m_devices.gpuAccelerated = (m_devices.radeonRays != nullptr) || (m_devices.tan != nullptr);
    if (!m_devices.gpuAccelerated) {
        errorOut = "OpenCL device created but neither Radeon Rays nor TrueAudio Next initialized";
        Shutdown();
        return false;
    }

    m_devices.description = "OpenCL GPU #" + std::to_string(best) + " (perf " + std::to_string(bestDesc.perfScore) +
                            ", " + std::to_string(bestDesc.numConvolutionCUs) + " conv CUs, " +
                            std::to_string(bestDesc.numIRUpdateCUs) + " IR CUs); rays=" +
                            (m_devices.sceneType == IPL_SCENETYPE_RADEONRAYS ? "RadeonRays"
                             : m_devices.sceneType == IPL_SCENETYPE_EMBREE     ? "Embree"
                                                                                : "Default") +
                            "; convolution=" + (m_devices.tan ? "TrueAudioNext" : "CPU");
    SA_LOGI("GPU backend ready: %s", m_devices.description.c_str());
    return true;
}

void GPUBackend::Shutdown()
{
    if (m_devices.tan)
        iplTrueAudioNextDeviceRelease(&m_devices.tan);
    if (m_devices.radeonRays)
        iplRadeonRaysDeviceRelease(&m_devices.radeonRays);
    if (m_devices.embree)
        iplEmbreeDeviceRelease(&m_devices.embree);
    if (m_devices.openCL)
        iplOpenCLDeviceRelease(&m_devices.openCL);
    if (m_deviceList)
        iplOpenCLDeviceListRelease(&m_deviceList);
    m_devices = BackendDevices{};
    m_deviceList = nullptr;
}

} // namespace sa
