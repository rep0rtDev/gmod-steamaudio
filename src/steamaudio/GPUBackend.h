// src/steamaudio/GPUBackend.h
//
// OpenCL-based backend: enumerates OpenCL GPUs through Steam Audio, reserves
// compute units, and creates Radeon Rays (ray tracing) and TrueAudio Next
// (convolution) devices. Only available in 64-bit builds; on 32-bit the
// Initialize() call always fails so AudioEngine falls back to the CPU backend.
#pragma once

#include <vector>

#include "Backend.h"

namespace sa {

struct GpuDeviceInfo {
    int32_t index = 0;
    IPLOpenCLDeviceType type = IPL_OPENCLDEVICETYPE_GPU;
    int32_t numConvolutionCUs = 0;
    int32_t numIRUpdateCUs = 0;
    int32_t granularity = 0;
    float perfScore = 0.f;
};

class GPUBackend final : public IBackend {
public:
    GPUBackend() = default;
    ~GPUBackend() override;

    bool Initialize(PhononContext& context, const StaticConfig& config, std::string& errorOut) override;
    void Shutdown() override;
    const BackendDevices& Devices() const override { return m_devices; }
    const char* Name() const override { return "gpu"; }

    // Enumerates candidate OpenCL devices without creating any. Returns an
    // empty list when OpenCL is not available on this platform/build.
    static std::vector<GpuDeviceInfo> Enumerate(PhononContext& context, const StaticConfig& config);

    static constexpr bool IsSupportedBuild()
    {
        return sizeof(void*) == 8;
    }

private:
    BackendDevices m_devices;
    IPLOpenCLDeviceList m_deviceList = nullptr;
};

} // namespace sa
