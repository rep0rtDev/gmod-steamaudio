// src/steamaudio/Backend.h
//
// Common description of the compute backend chosen at initialization: which
// ray tracer drives the scene and which convolution engine renders reflections.
// Exactly one backend (GPUBackend or CPUFallbackBackend) is active at a time.
#pragma once

#include <string>

#include "PhononApi.h"
#include "steamaudio/Config.h"

namespace sa {

class PhononContext;

struct BackendDevices {
    IPLSceneType sceneType = IPL_SCENETYPE_DEFAULT;
    IPLReflectionEffectType reflectionType = IPL_REFLECTIONEFFECTTYPE_CONVOLUTION;
    IPLEmbreeDevice embree = nullptr;
    IPLOpenCLDevice openCL = nullptr;
    IPLRadeonRaysDevice radeonRays = nullptr;
    IPLTrueAudioNextDevice tan = nullptr;
    bool gpuAccelerated = false;
    std::string description;
};

class IBackend {
public:
    virtual ~IBackend() = default;

    // Creates the devices. Returns false when this backend cannot run on the
    // current machine/build; `errorOut` explains why.
    virtual bool Initialize(PhononContext& context, const StaticConfig& config, std::string& errorOut) = 0;
    virtual void Shutdown() = 0;
    virtual const BackendDevices& Devices() const = 0;
    virtual const char* Name() const = 0;
};

// Maps the user's reflection type preference to the IPL enum, honoring
// what the backend can actually render.
IPLReflectionEffectType ResolveReflectionType(ReflectionTypePreference preference, bool tanAvailable);

} // namespace sa
