// src/steamaudio/CPUFallbackBackend.h
//
// CPU-only backend: Embree ray tracing when available (64-bit) or the Steam
// Audio built-in ray tracer, with CPU convolution/parametric/hybrid reverb.
// Initialize() never fails while the Steam Audio context exists, which makes
// this the guaranteed fallback for any GPU initialization failure.
#pragma once

#include "Backend.h"

namespace sa {

class CPUFallbackBackend final : public IBackend {
public:
    CPUFallbackBackend() = default;
    ~CPUFallbackBackend() override;

    bool Initialize(PhononContext& context, const StaticConfig& config, std::string& errorOut) override;
    void Shutdown() override;
    const BackendDevices& Devices() const override { return m_devices; }
    const char* Name() const override { return "cpu"; }

private:
    BackendDevices m_devices;
};

} // namespace sa
