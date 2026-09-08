// src/steamaudio/PhononApi.h
//
// Runtime binding of the Steam Audio C API. phonon.dll is loaded with
// LoadLibrary at module initialization (searching lua/bin, garrysmod/bin and
// the engine bin directories) instead of being import-linked, so a missing or
// incompatible phonon.dll degrades gracefully into the fallback stereo mixer
// instead of preventing the GMod module from loading at all.
//
// After including this header, every `iplXxx` symbol used by the project
// resolves to the corresponding function pointer in `sa::phonon::Api()`, so
// call sites read exactly like static Steam Audio code.
#pragma once

#include "phonon.h"
#include "steamaudio/PathCacheApi.h"

#include <string>
#include <vector>

// X-macro list of every Steam Audio entry point used by this project.
#define SA_PHONON_FUNCTIONS(X)                                                                                  \
    X(iplContextCreate)                                                                                         \
    X(iplContextRelease)                                                                                        \
    X(iplEmbreeDeviceCreate)                                                                                    \
    X(iplEmbreeDeviceRelease)                                                                                   \
    X(iplOpenCLDeviceListCreate)                                                                                \
    X(iplOpenCLDeviceListRelease)                                                                               \
    X(iplOpenCLDeviceListGetNumDevices)                                                                         \
    X(iplOpenCLDeviceListGetDeviceDesc)                                                                         \
    X(iplOpenCLDeviceCreate)                                                                                    \
    X(iplOpenCLDeviceRelease)                                                                                   \
    X(iplRadeonRaysDeviceCreate)                                                                                \
    X(iplRadeonRaysDeviceRelease)                                                                               \
    X(iplTrueAudioNextDeviceCreate)                                                                             \
    X(iplTrueAudioNextDeviceRelease)                                                                            \
    X(iplSceneCreate)                                                                                           \
    X(iplSceneRelease)                                                                                          \
    X(iplSceneCommit)                                                                                           \
    X(iplSceneSaveOBJ)                                                                                          \
    X(iplStaticMeshCreate)                                                                                      \
    X(iplStaticMeshRelease)                                                                                     \
    X(iplStaticMeshAdd)                                                                                         \
    X(iplStaticMeshRemove)                                                                                      \
    X(iplInstancedMeshCreate)                                                                                   \
    X(iplInstancedMeshRelease)                                                                                  \
    X(iplInstancedMeshAdd)                                                                                      \
    X(iplInstancedMeshRemove)                                                                                   \
    X(iplInstancedMeshUpdateTransform)                                                                          \
    X(iplAudioBufferAllocate)                                                                                   \
    X(iplAudioBufferFree)                                                                                       \
    X(iplAudioBufferMix)                                                                                        \
    X(iplAudioBufferDownmix)                                                                                    \
    X(iplHRTFCreate)                                                                                            \
    X(iplHRTFRelease)                                                                                           \
    X(iplPanningEffectCreate)                                                                                   \
    X(iplPanningEffectRelease)                                                                                  \
    X(iplPanningEffectReset)                                                                                    \
    X(iplPanningEffectApply)                                                                                    \
    X(iplBinauralEffectCreate)                                                                                  \
    X(iplBinauralEffectRelease)                                                                                 \
    X(iplBinauralEffectReset)                                                                                   \
    X(iplBinauralEffectApply)                                                                                   \
    X(iplAmbisonicsEncodeEffectCreate)                                                                          \
    X(iplAmbisonicsEncodeEffectRelease)                                                                         \
    X(iplAmbisonicsEncodeEffectReset)                                                                           \
    X(iplAmbisonicsEncodeEffectApply)                                                                           \
    X(iplAmbisonicsDecodeEffectCreate)                                                                          \
    X(iplAmbisonicsDecodeEffectRelease)                                                                         \
    X(iplAmbisonicsDecodeEffectReset)                                                                           \
    X(iplAmbisonicsDecodeEffectApply)                                                                           \
    X(iplDirectEffectCreate)                                                                                    \
    X(iplDirectEffectRelease)                                                                                   \
    X(iplDirectEffectReset)                                                                                     \
    X(iplDirectEffectApply)                                                                                     \
    X(iplReflectionEffectCreate)                                                                                \
    X(iplReflectionEffectRelease)                                                                               \
    X(iplReflectionEffectReset)                                                                                 \
    X(iplReflectionEffectApply)                                                                                 \
    X(iplReflectionMixerCreate)                                                                                 \
    X(iplReflectionMixerRelease)                                                                                \
    X(iplReflectionMixerReset)                                                                                  \
    X(iplReflectionMixerApply)                                                                                  \
    X(iplPathEffectCreate)                                                                                      \
    X(iplPathEffectRelease)                                                                                     \
    X(iplPathEffectReset)                                                                                       \
    X(iplPathEffectApply)                                                                                       \
    X(iplProbeArrayCreate)                                                                                      \
    X(iplProbeArrayRelease)                                                                                     \
    X(iplProbeArrayGenerateProbes)                                                                              \
    X(iplProbeArrayGetNumProbes)                                                                                \
    X(iplProbeBatchCreate)                                                                                      \
    X(iplProbeBatchRelease)                                                                                     \
    X(iplProbeBatchAddProbeArray)                                                                               \
    X(iplProbeBatchCommit)                                                                                      \
    X(iplProbeBatchGetNumProbes)                                                                                \
    X(iplProbeBatchSave)                                                                                        \
    X(iplProbeBatchLoad)                                                                                        \
    X(iplSerializedObjectCreate)                                                                                \
    X(iplSerializedObjectRelease)                                                                               \
    X(iplSerializedObjectGetSize)                                                                               \
    X(iplSerializedObjectGetData)                                                                               \
    X(iplReflectionsBakerBake)                                                                                  \
    X(iplReflectionsBakerCancelBake)                                                                            \
    X(iplPathBakerBake)                                                                                         \
    X(iplPathBakerCancelBake)                                                                                   \
    X(iplSimulatorCreate)                                                                                       \
    X(iplSimulatorRelease)                                                                                      \
    X(iplSimulatorSetScene)                                                                                     \
    X(iplSimulatorAddProbeBatch)                                                                                \
    X(iplSimulatorRemoveProbeBatch)                                                                             \
    X(iplSimulatorSetSharedInputs)                                                                              \
    X(iplSimulatorCommit)                                                                                       \
    X(iplSimulatorRunDirect)                                                                                    \
    X(iplSimulatorRunReflections)                                                                               \
    X(iplSimulatorRunPathing)                                                                                   \
    X(iplSourceCreate)                                                                                          \
    X(iplSourceRelease)                                                                                         \
    X(iplSourceAdd)                                                                                             \
    X(iplSourceRemove)                                                                                          \
    X(iplSourceSetInputs)                                                                                       \
    X(iplSourceGetOutputs)                                                                                      \
    X(iplCalculateRelativeDirection)                                                                            \
    X(iplDistanceAttenuationCalculate) \
    X(iplSourceRetain) \
    X(iplReflectionEffectGetTail) \
    X(iplReflectionEffectGetTailSize) \
    X(iplBinauralEffectGetTail) \
    X(iplPathEffectGetTail) \
    X(iplAmbisonicsDecodeEffectGetTail) \
    X(iplAudioBufferInterleave) \
    X(iplAudioBufferDeinterleave) \
    X(iplStaticMeshSave) \
    X(iplStaticMeshLoad) \
    X(iplAirAbsorptionCalculate) \
    X(iplDirectivityCalculate) \
    X(iplProbeBatchGetDataSize) \
    X(iplProbeBatchRemoveData) \
    X(iplProbeBatchAddProbe) \
    X(iplProbeArrayGetProbe)

namespace sa {
namespace phonon {

struct ApiTable {
#define SA_PHONON_DECLARE(name) decltype(&::name) name = nullptr;
    SA_PHONON_FUNCTIONS(SA_PHONON_DECLARE)
#undef SA_PHONON_DECLARE
    PathCacheApi pathCache;
};

// Loads phonon.dll. `searchDirectories` are tried in order; the plain library
// name is tried last (standard search path). Returns true when every function
// in SA_PHONON_FUNCTIONS resolved. Safe to call repeatedly.
bool Load(const std::vector<std::string>& searchDirectories, std::string& errorOut);

// Unloads the library. All Steam Audio objects must have been released.
void Unload();

bool IsLoaded();

// Path of the library that was loaded (empty when not loaded).
const std::string& LoadedPath();

// phonon.dll delay-loads OpenCL.dll, GPUUtilities.dll and TrueAudioNext.dll.
// The MSVC delay-load helper raises a fatal SEH exception (0xC06D007E) when
// one of them is missing, so the GPU entry points must never be called
// before these have been pinned into the process. Modules are loaded from the
// phonon.dll directory first, then via the default search path.
struct GpuRuntime {
    bool openCL = false;
    bool gpuUtilities = false;
    bool trueAudioNext = false;
    std::string missing;
};
GpuRuntime PreloadGpuRuntime();

// Function table. Every entry is non-null once IsLoaded() is true.
const ApiTable& Api();

} // namespace phonon
} // namespace sa

// Redirect every entry point to the runtime-bound table. Placed after the
// declarations in phonon.h so the prototypes keep their real signatures.
// PhononApi.cpp defines SA_PHONON_NO_MACROS to access the table members.
#ifndef SA_PHONON_NO_MACROS
#define iplContextCreate (::sa::phonon::Api().iplContextCreate)
#define iplContextRelease (::sa::phonon::Api().iplContextRelease)
#define iplEmbreeDeviceCreate (::sa::phonon::Api().iplEmbreeDeviceCreate)
#define iplEmbreeDeviceRelease (::sa::phonon::Api().iplEmbreeDeviceRelease)
#define iplOpenCLDeviceListCreate (::sa::phonon::Api().iplOpenCLDeviceListCreate)
#define iplOpenCLDeviceListRelease (::sa::phonon::Api().iplOpenCLDeviceListRelease)
#define iplOpenCLDeviceListGetNumDevices (::sa::phonon::Api().iplOpenCLDeviceListGetNumDevices)
#define iplOpenCLDeviceListGetDeviceDesc (::sa::phonon::Api().iplOpenCLDeviceListGetDeviceDesc)
#define iplOpenCLDeviceCreate (::sa::phonon::Api().iplOpenCLDeviceCreate)
#define iplOpenCLDeviceRelease (::sa::phonon::Api().iplOpenCLDeviceRelease)
#define iplRadeonRaysDeviceCreate (::sa::phonon::Api().iplRadeonRaysDeviceCreate)
#define iplRadeonRaysDeviceRelease (::sa::phonon::Api().iplRadeonRaysDeviceRelease)
#define iplTrueAudioNextDeviceCreate (::sa::phonon::Api().iplTrueAudioNextDeviceCreate)
#define iplTrueAudioNextDeviceRelease (::sa::phonon::Api().iplTrueAudioNextDeviceRelease)
#define iplSceneCreate (::sa::phonon::Api().iplSceneCreate)
#define iplSceneRelease (::sa::phonon::Api().iplSceneRelease)
#define iplSceneCommit (::sa::phonon::Api().iplSceneCommit)
#define iplSceneSaveOBJ (::sa::phonon::Api().iplSceneSaveOBJ)
#define iplStaticMeshCreate (::sa::phonon::Api().iplStaticMeshCreate)
#define iplStaticMeshRelease (::sa::phonon::Api().iplStaticMeshRelease)
#define iplStaticMeshAdd (::sa::phonon::Api().iplStaticMeshAdd)
#define iplStaticMeshRemove (::sa::phonon::Api().iplStaticMeshRemove)
#define iplInstancedMeshCreate (::sa::phonon::Api().iplInstancedMeshCreate)
#define iplInstancedMeshRelease (::sa::phonon::Api().iplInstancedMeshRelease)
#define iplInstancedMeshAdd (::sa::phonon::Api().iplInstancedMeshAdd)
#define iplInstancedMeshRemove (::sa::phonon::Api().iplInstancedMeshRemove)
#define iplInstancedMeshUpdateTransform (::sa::phonon::Api().iplInstancedMeshUpdateTransform)
#define iplAudioBufferAllocate (::sa::phonon::Api().iplAudioBufferAllocate)
#define iplAudioBufferFree (::sa::phonon::Api().iplAudioBufferFree)
#define iplAudioBufferMix (::sa::phonon::Api().iplAudioBufferMix)
#define iplAudioBufferDownmix (::sa::phonon::Api().iplAudioBufferDownmix)
#define iplHRTFCreate (::sa::phonon::Api().iplHRTFCreate)
#define iplHRTFRelease (::sa::phonon::Api().iplHRTFRelease)
#define iplPanningEffectCreate (::sa::phonon::Api().iplPanningEffectCreate)
#define iplPanningEffectRelease (::sa::phonon::Api().iplPanningEffectRelease)
#define iplPanningEffectReset (::sa::phonon::Api().iplPanningEffectReset)
#define iplPanningEffectApply (::sa::phonon::Api().iplPanningEffectApply)
#define iplBinauralEffectCreate (::sa::phonon::Api().iplBinauralEffectCreate)
#define iplBinauralEffectRelease (::sa::phonon::Api().iplBinauralEffectRelease)
#define iplBinauralEffectReset (::sa::phonon::Api().iplBinauralEffectReset)
#define iplBinauralEffectApply (::sa::phonon::Api().iplBinauralEffectApply)
#define iplAmbisonicsEncodeEffectCreate (::sa::phonon::Api().iplAmbisonicsEncodeEffectCreate)
#define iplAmbisonicsEncodeEffectRelease (::sa::phonon::Api().iplAmbisonicsEncodeEffectRelease)
#define iplAmbisonicsEncodeEffectReset (::sa::phonon::Api().iplAmbisonicsEncodeEffectReset)
#define iplAmbisonicsEncodeEffectApply (::sa::phonon::Api().iplAmbisonicsEncodeEffectApply)
#define iplAmbisonicsDecodeEffectCreate (::sa::phonon::Api().iplAmbisonicsDecodeEffectCreate)
#define iplAmbisonicsDecodeEffectRelease (::sa::phonon::Api().iplAmbisonicsDecodeEffectRelease)
#define iplAmbisonicsDecodeEffectReset (::sa::phonon::Api().iplAmbisonicsDecodeEffectReset)
#define iplAmbisonicsDecodeEffectApply (::sa::phonon::Api().iplAmbisonicsDecodeEffectApply)
#define iplDirectEffectCreate (::sa::phonon::Api().iplDirectEffectCreate)
#define iplDirectEffectRelease (::sa::phonon::Api().iplDirectEffectRelease)
#define iplDirectEffectReset (::sa::phonon::Api().iplDirectEffectReset)
#define iplDirectEffectApply (::sa::phonon::Api().iplDirectEffectApply)
#define iplReflectionEffectCreate (::sa::phonon::Api().iplReflectionEffectCreate)
#define iplReflectionEffectRelease (::sa::phonon::Api().iplReflectionEffectRelease)
#define iplReflectionEffectReset (::sa::phonon::Api().iplReflectionEffectReset)
#define iplReflectionEffectApply (::sa::phonon::Api().iplReflectionEffectApply)
#define iplReflectionMixerCreate (::sa::phonon::Api().iplReflectionMixerCreate)
#define iplReflectionMixerRelease (::sa::phonon::Api().iplReflectionMixerRelease)
#define iplReflectionMixerReset (::sa::phonon::Api().iplReflectionMixerReset)
#define iplReflectionMixerApply (::sa::phonon::Api().iplReflectionMixerApply)
#define iplPathEffectCreate (::sa::phonon::Api().iplPathEffectCreate)
#define iplPathEffectRelease (::sa::phonon::Api().iplPathEffectRelease)
#define iplPathEffectReset (::sa::phonon::Api().iplPathEffectReset)
#define iplPathEffectApply (::sa::phonon::Api().iplPathEffectApply)
#define iplProbeArrayCreate (::sa::phonon::Api().iplProbeArrayCreate)
#define iplProbeArrayRelease (::sa::phonon::Api().iplProbeArrayRelease)
#define iplProbeArrayGenerateProbes (::sa::phonon::Api().iplProbeArrayGenerateProbes)
#define iplProbeArrayGetNumProbes (::sa::phonon::Api().iplProbeArrayGetNumProbes)
#define iplProbeBatchCreate (::sa::phonon::Api().iplProbeBatchCreate)
#define iplProbeBatchRelease (::sa::phonon::Api().iplProbeBatchRelease)
#define iplProbeBatchAddProbeArray (::sa::phonon::Api().iplProbeBatchAddProbeArray)
#define iplProbeBatchAddProbe (::sa::phonon::Api().iplProbeBatchAddProbe)
#define iplProbeArrayGetProbe (::sa::phonon::Api().iplProbeArrayGetProbe)
#define iplProbeBatchCommit (::sa::phonon::Api().iplProbeBatchCommit)
#define iplProbeBatchGetNumProbes (::sa::phonon::Api().iplProbeBatchGetNumProbes)
#define iplProbeBatchSave (::sa::phonon::Api().iplProbeBatchSave)
#define iplProbeBatchLoad (::sa::phonon::Api().iplProbeBatchLoad)
#define iplSerializedObjectCreate (::sa::phonon::Api().iplSerializedObjectCreate)
#define iplSerializedObjectRelease (::sa::phonon::Api().iplSerializedObjectRelease)
#define iplSerializedObjectGetSize (::sa::phonon::Api().iplSerializedObjectGetSize)
#define iplSerializedObjectGetData (::sa::phonon::Api().iplSerializedObjectGetData)
#define iplReflectionsBakerBake (::sa::phonon::Api().iplReflectionsBakerBake)
#define iplReflectionsBakerCancelBake (::sa::phonon::Api().iplReflectionsBakerCancelBake)
#define iplPathBakerBake (::sa::phonon::Api().iplPathBakerBake)
#define iplPathBakerCancelBake (::sa::phonon::Api().iplPathBakerCancelBake)
#define iplSimulatorCreate (::sa::phonon::Api().iplSimulatorCreate)
#define iplSimulatorRelease (::sa::phonon::Api().iplSimulatorRelease)
#define iplSimulatorSetScene (::sa::phonon::Api().iplSimulatorSetScene)
#define iplSimulatorAddProbeBatch (::sa::phonon::Api().iplSimulatorAddProbeBatch)
#define iplSimulatorRemoveProbeBatch (::sa::phonon::Api().iplSimulatorRemoveProbeBatch)
#define iplSimulatorSetSharedInputs (::sa::phonon::Api().iplSimulatorSetSharedInputs)
#define iplSimulatorCommit (::sa::phonon::Api().iplSimulatorCommit)
#define iplSimulatorRunDirect (::sa::phonon::Api().iplSimulatorRunDirect)
#define iplSimulatorRunReflections (::sa::phonon::Api().iplSimulatorRunReflections)
#define iplSimulatorRunPathing (::sa::phonon::Api().iplSimulatorRunPathing)
#define iplSourceCreate (::sa::phonon::Api().iplSourceCreate)
#define iplSourceRelease (::sa::phonon::Api().iplSourceRelease)
#define iplSourceAdd (::sa::phonon::Api().iplSourceAdd)
#define iplSourceRemove (::sa::phonon::Api().iplSourceRemove)
#define iplSourceSetInputs (::sa::phonon::Api().iplSourceSetInputs)
#define iplSourceGetOutputs (::sa::phonon::Api().iplSourceGetOutputs)
#define iplCalculateRelativeDirection (::sa::phonon::Api().iplCalculateRelativeDirection)
#define iplDistanceAttenuationCalculate (::sa::phonon::Api().iplDistanceAttenuationCalculate)
#define iplSourceRetain (::sa::phonon::Api().iplSourceRetain)
#define iplReflectionEffectGetTail (::sa::phonon::Api().iplReflectionEffectGetTail)
#define iplReflectionEffectGetTailSize (::sa::phonon::Api().iplReflectionEffectGetTailSize)
#define iplBinauralEffectGetTail (::sa::phonon::Api().iplBinauralEffectGetTail)
#define iplPathEffectGetTail (::sa::phonon::Api().iplPathEffectGetTail)
#define iplAmbisonicsDecodeEffectGetTail (::sa::phonon::Api().iplAmbisonicsDecodeEffectGetTail)
#define iplAudioBufferInterleave (::sa::phonon::Api().iplAudioBufferInterleave)
#define iplAudioBufferDeinterleave (::sa::phonon::Api().iplAudioBufferDeinterleave)
#define iplStaticMeshSave (::sa::phonon::Api().iplStaticMeshSave)
#define iplStaticMeshLoad (::sa::phonon::Api().iplStaticMeshLoad)
#define iplAirAbsorptionCalculate (::sa::phonon::Api().iplAirAbsorptionCalculate)
#define iplDirectivityCalculate (::sa::phonon::Api().iplDirectivityCalculate)
#define iplProbeBatchGetDataSize (::sa::phonon::Api().iplProbeBatchGetDataSize)
#define iplProbeBatchRemoveData (::sa::phonon::Api().iplProbeBatchRemoveData)
#endif // SA_PHONON_NO_MACROS
