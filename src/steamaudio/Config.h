// src/steamaudio/Config.h
//
// All tunables exposed through console variables. `EngineConfig` is owned by
// the game thread; a copy is handed to each subsystem at initialization and
// live-updatable fields are re-published through `SeqLock<RuntimeConfig>`.
#pragma once

#include <cstdint>
#include <string>

namespace sa {

enum class BackendPreference : int32_t {
    Auto = 0, // GPU when available, otherwise CPU
    Cpu = 1,
    Gpu = 2,
};

enum class SceneTypePreference : int32_t {
    Auto = 0,
    Default = 1,    // Steam Audio built-in ray tracer
    Embree = 2,     // 64-bit only
    RadeonRays = 3, // 64-bit + OpenCL GPU only
};

enum class ReflectionTypePreference : int32_t {
    Convolution = 0,
    Parametric = 1,
    Hybrid = 2,
    TrueAudioNext = 3,
};

enum class OutputMode : int32_t {
    Auto = 0,   // engine paint buffer when the mixer is hooked, native WASAPI otherwise
    Native = 1, // WASAPI only
    Engine = 2, // write back into the engine's paint buffer (engine device does the output)
};

// Fields that require a full re-initialization of Steam Audio when changed.
struct StaticConfig {
    int32_t sampleRate = 44100;
    int32_t frameSize = 512;
    int32_t maxSources = 64;
    int32_t simulationThreads = 0; // 0 = hardware_concurrency - 2, min 1
    int32_t maxRays = 4096;
    int32_t maxOcclusionSamples = 32;
    int32_t numDiffuseSamples = 32;
    float maxIrDuration = 1.5f;
    int32_t maxAmbisonicOrder = 1;
    BackendPreference backend = BackendPreference::Auto;
    SceneTypePreference sceneType = SceneTypePreference::Auto;
    ReflectionTypePreference reflectionType = ReflectionTypePreference::Hybrid;
    int32_t gpuComputeUnits = 8;        // OpenCL CUs to reserve (0 = whole GPU)
    float gpuIrUpdateFraction = 0.5f;   // fraction of CUs for IR update
    bool hrtfEnabled = true;
    std::string sofaFile;               // optional custom HRTF (SOFA)
    float hrtfVolumeDb = 0.f;
    OutputMode outputMode = OutputMode::Auto;
    std::string outputDeviceId;         // WASAPI endpoint id, empty = default
    int32_t outputLatencyMs = 40;
    // Native output: how far behind the engine's paint frontier to render.
    // 0 = follow the engine's own play cursor (soundtime); > 0 = fixed ms.
    int32_t engineLeadMs = 0;
    bool validationLayer = false;
    bool enableEmbree = true;
    bool enableTan = true;
};

// Fields that can change at any time without re-initialization.
struct RuntimeConfig {
    bool enabled = true;
    bool physicalAcoustics = true;
    bool hrtf = true;
    int32_t hrtfInterpolation = 1;    // 0 nearest, 1 bilinear
    bool reflections = true;
    bool pathing = true;
    bool occlusion = true;
    bool transmission = true;
    int32_t occlusionType = 1;        // 0 raycast, 1 volumetric
    int32_t occlusionSamples = 16;
    int32_t transmissionRays = 1;
    int32_t numRays = 2048;
    int32_t numBounces = 8;
    float irDuration = 1.0f;
    int32_t ambisonicOrder = 1;
    float irradianceMinDistance = 1.0f;
    float hybridTransitionTime = 0.5f;
    float hybridOverlapPercent = 0.25f;
    float reverbGain = 1.0f;
    float directGain = 1.0f;
    float pathingGain = 1.0f;
    float masterVolume = 1.0f;
    float engineVolume = 1.0f;        // mirrors the engine "volume" convar
    float voiceScale = 1.0f;          // mirrors "voice_scale"
    bool voiceSpatial = true;
    bool spatializeStereo = true;    // spatialize stereo engine sounds (downmixed) instead of 2D passthrough
    bool bassSpatial = true;          // spatialize 3D IGModAudioChannels
    float unitsPerMeter = 52.4934f;
    float simulationIntervalMs = 100.f;
    float reflectionsIntervalMs = 250.f;
    float pathingIntervalMs = 250.f;
    float sourceRadius = 0.5f;        // meters, for volumetric occlusion
    // Occlusion shaping. Volumetric occlusion of a sphere around a point
    // emitter that sits on or inside geometry (impact sounds at a wall, an
    // NPC's own hull) reports partial visibility although the emitter is in
    // plain view; visibility at or above `occlusionFullVisibility` counts as
    // unoccluded, below `occlusionZeroVisibility` as fully occluded. A fully
    // occluded source still leaks `occlusionMinGain` of its low band through
    // the wall (less for the mid/high bands) so it stays audible and muffled
    // instead of vanishing.
    float occlusionFullVisibility = 0.6f;
    float occlusionZeroVisibility = 0.1f;
    float occlusionMinGain = 0.3f;
    // Emitters attached to an entity that is itself an occluder are moved out
    // of that entity's collision bounds toward the listener by the occlusion
    // radius plus this margin (Source units) so the entity does not occlude
    // its own sounds.
    float emitterHullMarginUnits = 4.f;
    float airAbsorptionScale = 1.0f;
    float distanceGainMin = 0.01f;    // Source snd_gain_min
    float distanceGainMax = 1.0f;     // Source snd_gain_max
    bool useBakedReverb = true;
    bool bakeOnMapLoad = true;
    float probeSpacing = 4.0f;        // meters
    float probeHeight = 1.5f;         // meters above floor
    int32_t bakeRays = 8192;
    int32_t bakeBounces = 16;
    float bakeDuration = 1.5f;
    bool dynamicGeometry = true;      // moving brush entities (doors, elevators, func_movelinear)
    bool dynamicProps = true;         // physics/dynamic props, ragdolls, vehicles as occluders
    bool dynamicPlayers = true;       // other players as hull-box occluders
    bool nativeEntityList = true;     // walk IClientEntityList natively (falls back to Lua when unvalidated)
    int32_t dynamicMaxOccluders = 256;
    float dynamicRangeUnits = 3000.f; // only entities within this distance of the listener (0 = all)
    float dynamicMinExtentUnits = 8.f;
    float dynamicUpdateIntervalMs = 100.f;
    float dynamicScanBudgetMs = 2.f;
    float dynamicModelBudgetMs = 2.f;
    bool staticProps = true;          // instance static prop collision (.phy) / hull boxes into the scene
    bool staticPropBoxFallback = true; // approximate props without a usable .phy by their .mdl hull box
    bool vmtSurfaceProps = true;      // read $surfaceprop from materials/*.vmt for world faces
    bool surfacePropScripts = true;   // load scripts/surfaceproperties*.txt for surfaceprop inheritance
    // Source DSP replacement (dsp_room / dsp_player / dsp_water).
    int32_t roomDspMode = 1;          // 0 off, 1 sources without Steam Audio reflections, 2 all sources
    int32_t roomDspPreset = -1;       // >= 0 forces a room preset instead of following dsp_room
    float roomDspGain = 1.f;
    bool playerDsp = true;            // dsp_player lowpass/muffle presets (flashbang, facing away)
    bool underwaterDsp = true;        // lowpass + dsp_water reverb while the listener is submerged
    float underwaterCutoffHz = 900.f;
    float underwaterGain = 0.8f;
    // Propagation delay / Doppler.
    bool doppler = false;
    float dopplerScale = 1.f;         // scales the propagation delay (0 = no delay, 1 = physical)
    float speedOfSound = 343.f;       // m/s
    // Automatic directivity for engine sounds without explicit dipole settings.
    bool autoDirectivity = true;
    float weaponDipoleWeight = 0.25f;  // CHAN_WEAPON sounds face the owner's aim direction
    float weaponDipolePower = 1.f;
    float voiceDipoleWeight = 0.15f;   // CHAN_VOICE / voice chat face the speaker
    float voiceDipolePower = 1.f;
    int32_t pathingMemoryLimitMiB = 1024;
    int32_t pathingVisSamples = 4;
    float pathingVisRadius = 1.0f;    // meters
    float pathingVisThreshold = 0.1f;
    float pathingVisRange = 50.0f;    // meters
    float pathingRange = 200.0f;      // meters
    bool pathingValidation = false;
    bool pathingAlternatePaths = false;
    bool debugDraw = false;
    int32_t logLevel = 1;
};

struct EngineConfig {
    StaticConfig fixed;
    RuntimeConfig runtime;
};

} // namespace sa
