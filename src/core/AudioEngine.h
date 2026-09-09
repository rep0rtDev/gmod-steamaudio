// src/core/AudioEngine.h
//
// Top-level orchestrator. Owns every subsystem and sequences their
// lifecycle:
//
//   Initialize:  config files -> engine interfaces (dedicated-server check,
//                IEngineSound, IAudioDevice) -> Steam Audio context ->
//                backend (GPU, then CPU fallback) -> HRTF / simulator /
//                reflections / pathing / scene -> capture + mixer hooks ->
//                output device -> audio + simulation threads -> BASS and
//                procedural bridges.
//   Tick:        game-thread housekeeping (active-sound polling, bridge
//                ticks, device-loss recovery, finished-source disposal).
//   Map load:    BSP -> scene geometry -> optional bake.
//   Shutdown:    reverse order, always safe to call.
//
// Every public method is game-thread only. Cross-thread state lives inside
// the subsystems (SeqLocks, SPSC queues, atomics).
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/ChannelCapture.h"
#include "core/ClientEntityList.h"
#include "core/DynamicOccluders.h"
#include "core/EngineHooks.h"
#include "core/EngineFileSystem.h"
#include "core/EngineInterfaces.h"
#include "util/GmaArchive.h"
#include "mixing/AudioThread.h"
#include "mixing/BassBridge.h"
#include "mixing/FallbackMixer.h"
#include "mixing/ProceduralAudioBridge.h"
#include "mixing/SimulationThread.h"
#include "platform/PlatformAudioOutput.h"
#include "steamaudio/Backend.h"
#include "steamaudio/BspGeometry.h"
#include "steamaudio/Config.h"
#include "steamaudio/HRTFRenderer.h"
#include "steamaudio/PathingSimulator.h"
#include "steamaudio/PhononContext.h"
#include "steamaudio/ReflectionSimulator.h"
#include "steamaudio/SceneBuilder.h"
#include "steamaudio/Simulator.h"
#include "util/Math.h"

namespace sa {

struct EnginePaths {
    std::string moduleDirectory;  // where the gmcl_*.dll lives (lua/bin)
    std::string gameDirectory;    // <steam>/GarrysMod/garrysmod
    std::string configDirectory;  // garrysmod/data/steamaudio (json configs)
    std::string cacheDirectory;   // garrysmod/data/steamaudio/cache (bakes)
    std::string logFile;          // garrysmod/data/steamaudio/steamaudio.log
};

enum class EngineState : int32_t {
    Uninitialized = 0,
    Inactive = 1,      // dedicated server / disabled: no audio work at all
    Fallback = 2,      // Steam Audio unavailable, stereo fallback mixer active
    Active = 3,        // Steam Audio rendering
    Passthrough = 4,   // hooks installed, engine mixes on its own (snd_sa_enabled 0)
};

// Snapshot for the Lua status surface (copied out on the game thread).
struct EngineStatus {
    EngineState state = EngineState::Uninitialized;
    std::string stateReason;
    std::string backend;
    std::string sceneType;
    std::string outputPath;
    std::string outputDevice;
    std::string audioDeviceClass;
    std::string mapName;
    int32_t sampleRate = 0;
    int32_t frameSize = 0;
    bool hrtf = false;
    bool mixerHooked = false;
    bool listenerFromEngine = false;
    bool bassAttached = false;
    bool bakeRunning = false;
    float bakeProgress = 0.f;
    bool bakedDataLoaded = false;
    bool geometryPending = false;
    uint64_t audioFrames = 0;
    uint64_t audioUnderruns = 0;
    uint64_t engineStarves = 0;
    uint64_t clockResyncs = 0;
    uint64_t clockRebases = 0;
    uint64_t nonFiniteFrames = 0;
    uint64_t engineLeadSamples = 0;  // paint frontier - render clock (native path)
    uint32_t activeSources = 0;
    uint32_t spatializedSources = 0;
    uint32_t renderMicros = 0;
    uint32_t maxRenderMicros = 0;
    float peak = 0.f;
    int32_t roomPreset = 0;          // effective dsp_room replacement preset (0 = off)
    uint32_t roomSends = 0;          // sources routed to the room reverb last frame
    float playerLowpassHz = 0.f;     // active dsp_player / underwater lowpass (0 = bypass)
    uint64_t simulationTicks = 0;
    uint64_t simulationDirectRuns = 0;
    uint64_t simulationReflectionRuns = 0;
    uint64_t simulationPathingRuns = 0;
    uint32_t simulationSources = 0;
    uint32_t simulationMicros = 0;
    uint32_t simulationTickMicros = 0;
    uint32_t maxSimulationTickMicros = 0;
    uint32_t simulationCommandMicros = 0;
    uint32_t maxSimulationCommandMicros = 0;
    uint32_t sceneCommitMicros = 0;
    uint32_t maxSceneCommitMicros = 0;
    uint64_t sceneCommits = 0;
    uint32_t gameTickMicros = 0;
    uint32_t maxGameTickMicros = 0;
    uint32_t entitySnapshotMicros = 0;
    uint32_t maxEntitySnapshotMicros = 0;
    uint32_t occluderUpdateMicros = 0;
    uint32_t maxOccluderUpdateMicros = 0;
    size_t staticTriangles = 0;
    size_t dynamicMeshes = 0;
    size_t staticProps = 0;
    size_t staticPropTriangles = 0;
    size_t staticPropsMissing = 0;
    size_t surfacePropEntries = 0;
    size_t vmtLookups = 0;
    size_t vmtResolved = 0;
    uint32_t bassChannels = 0;
    size_t proceduralStreams = 0;
    uint64_t hookCalls = 0;
    bool audioPriorityElevated = false;
    // Engine channel capture (mixer hooks).
    uint64_t captureMixCalls = 0;
    uint64_t capturePaintIterations = 0;
    uint64_t captureBlocks44k = 0;
    uint64_t captureBlocks22k = 0;
    uint64_t captureBlocks11k = 0;
    uint64_t captureBlocksPartial = 0;
    uint64_t captureDuplicateBlocks = 0;
    uint64_t captureLateBlocks = 0;
    uint64_t captureSlotOverflow = 0;
    // Per-sound overrides (C22/E23).
    bool emitSoundHooked = false;
    uint64_t emitSoundCalls = 0;
    uint64_t namesFromHandles = 0;
    uint64_t autoDirectivity = 0;    // engine sounds given an inferred dipole this session
    uint64_t emitterHullPushes = 0;  // emitter positions moved out of their own occluder hull
    std::string soundNameResolver; // EngineFileSystem::FileNameResolverStatus()
    SoundOverrideTable::Stats overrides;
    // Dynamic occluders (B9/B10).
    std::string entitySource;      // "native", "lua", "off"
    std::string entityListState;   // ClientEntityList::StateName() + description / error
    ClientEntityList::Stats entityList;
    DynamicOccluders::Stats occluders;
};

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // Reads the configs, probes the engine and brings up the pipeline.
    // Returns false only for hard failures; degraded modes (fallback mixer,
    // passthrough) return true and are reported via Status().
    // `overrides` runs after the JSON configs are read (and again on every
    // Restart()) so convar values can be layered on top.
    using ConfigOverrideFn = std::function<void(RuntimeConfig&, StaticConfig&)>;
    bool Initialize(const EnginePaths& paths, std::string& error, ConfigOverrideFn overrides = {});
    void Shutdown();
    bool Initialized() const { return m_state != EngineState::Uninitialized; }
    EngineState State() const { return m_state; }

    // Once per client frame.
    void Tick(float frameTime);

    // ---- Configuration ------------------------------------------------------
    const StaticConfig& Static() const { return m_config.fixed; }
    const RuntimeConfig& Runtime() const { return m_config.runtime; }
    // Applies a new runtime config to every subsystem.
    void SetRuntimeConfig(const RuntimeConfig& cfg);
    // Static changes require a re-init: stored and applied on the next
    // Restart(). Returns true when a restart is now pending.
    bool SetStaticConfig(const StaticConfig& cfg);
    // Flags that a static convar changed; the new value is picked up by the
    // override callback on Restart().
    void MarkRestartPending() { m_restartRequested = true; }
    bool RestartPending() const { return m_restartRequested || m_pendingStaticValid; }
    // Full teardown + init with the stored configs (snd_sa_restart).
    bool Restart(std::string& error);

    // ---- Listener --------------------------------------------------------------
    // From Lua (render view setup), used when the engine hook cannot supply
    // the listener and as a fallback when the hook stops updating.
    void SetListener(const Vec3& position, const Vec3& forward, const Vec3& right, const Vec3& up);
    void SetLocalPlayer(int32_t entityIndex) { m_localPlayer = entityIndex; }
    // Source DSP state (dsp_room / dsp_player / dsp_water convars + water
    // level) read by Lua each frame; drives the module-side room/underwater
    // replacement on the audio thread.
    void SetEnvironment(const EnvironmentState& state) { m_audio.SetEnvironment(state); }

    // ---- Map / geometry -------------------------------------------------------
    // Locates maps/<name>.bsp through the engine file system (mounted Workshop
    // content included), then <game>/maps + download/maps on disk, then the
    // .gma archives in the addon directories, and builds the scene.
    bool LoadMap(const std::string& mapName, std::string& error);
    // Same, from BSP bytes supplied by Lua (last-resort path).
    bool LoadMapFromMemory(const std::string& mapName, const uint8_t* data, size_t size, std::string& error);
    // Reads a game-relative file ("models/x.mdl") the way the game resolves it:
    // engine file system first, then the loose game directory, then GMAs.
    bool ReadGameFile(const std::string& relativePath, std::vector<uint8_t>& out, std::string& error,
                      std::string* resolvedBy = nullptr, bool fallbackOnEngineMiss = true);
    bool GameFileExists(const std::string& relativePath);
    const EngineFileSystem& FileSystem() const { return m_fileSystem; }
    const std::string& FileSystemStatus() const { return m_fileSystemStatus; }
    void UnloadMap();
    const std::string& MapName() const { return m_mapName; }
    // Brush entity "*N" moved (func_movelinear, doors...).
    bool UpdateBrushModel(int32_t modelIndex, const Vec3& origin, const Vec3& angles);

    // ---- Dynamic occluders -------------------------------------------------------
    // Entities become occluders through DynamicOccluders. The snapshot comes
    // from the native IClientEntityList walk when its layout validated for
    // this map; otherwise Lua supplies one per update via Begin/Push/End
    // (a batch is consumed by the next Tick and ignored while native is active).
    bool NativeEntitiesActive() const { return m_nativeEntitiesActive; }
    void BeginLuaEntities();
    void PushLuaEntity(const EntitySnapshot& entity);
    void EndLuaEntities();
    const DynamicOccluders& Occluders() const { return m_occluders; }
    const ClientEntityList& EntityList() const { return m_entityList; }
    // Arbitrary dynamic meshes (Lua-provided triangles, Source units, local space).
    DynamicGeometryId AddDynamicMesh(const std::vector<Vec3>& triangles, const std::string& material,
                                     const Vec3& origin, const Vec3& angles, const std::string& name);
    bool UpdateDynamicMesh(DynamicGeometryId id, const Vec3& origin, const Vec3& angles);
    bool RemoveDynamicMesh(DynamicGeometryId id);
    bool RequestBake(bool force);
    bool CancelBake();
    BakeStatus BakeState() const { return m_reflections.Status(); }
    bool ContinueBakePathing(bool enabled) { return m_reflections.ContinuePathing(enabled); }

    // ---- Materials ---------------------------------------------------------------
    struct SurfacePropInfo {
        std::string texture;      // normalized texture name
        std::string vmtPath;      // materials/<texture>.vmt
        std::string surfaceProp;  // $surfaceprop as written in the VMT chain (lower-case), "" if none
        std::string material;     // acoustic material key the library chose
        std::string source;       // "vmt", "heuristic"
        std::string base;         // surfaceproperties.txt base chain start, if any
        char gameMaterial = 0;
    };
    // Diagnostic: how a texture maps onto an acoustic material.
    SurfacePropInfo DescribeTexture(const std::string& texture);
    const MaterialLibrary& Materials() const { return m_materials; }

    // ---- Bridges ----------------------------------------------------------------
    ProceduralAudioBridge& Procedural() { return m_procedural; }
    BassBridge& Bass() { return m_bass; }

    // ---- Engine sounds ------------------------------------------------------------
    // Per-sound override table (game thread). Always usable; only has an
    // effect while the mixer hooks are installed.
    SoundOverrideTable& SoundOverrides() { return m_hooks.Overrides(); }
    // Active engine sounds from the last poll (metadata + what we did with them).
    const std::vector<ActiveSoundInfo>& ActiveEngineSounds() const { return m_hooks.ActiveSounds(); }
    // IEngineSound::GetGuidForLastSoundEmitted(); 0 when unavailable.
    int32_t LastEmittedSoundGuid() const;
    bool EngineSoundAvailable() const;
    bool IsSoundStillPlaying(int32_t guid) const;

    // ---- Status -----------------------------------------------------------------
    EngineStatus Status() const;
    // Human-readable dump of the listener, every active engine sound and every
    // bound capture slot with what the renderer did with it (snd_sa_sounds).
    std::vector<std::string> DescribeSounds() const;
    const EnginePaths& Paths() const { return m_paths; }
    const std::string& BackendDescription() const { return m_backendDescription; }

private:
    // Native-output adapter: resamples the AudioThread's engine-rate stereo
    // stream to the device rate when WASAPI refuses our preferred format.
    class RateAdaptedSource final : public IOutputSource {
    public:
        void Configure(IOutputSource& inner, int32_t innerRate, int32_t deviceRate, size_t maxFrames);
        void PullStereo(float* interleaved, size_t frames) override;

    private:
        IOutputSource* m_inner = nullptr;
        double m_ratio = 1.0;
        std::vector<float> m_scratch;
        std::vector<float> m_deinterleaved[2];
        dsp::Resampler m_resamplers[2];
        std::vector<float> m_outScratch[2];
    };

    bool LoadConfigs(std::string& error);
    bool InitializeEngineSide(std::string& error);
    bool InitializeSteamAudio(std::string& error);
    bool InitializeOutput(std::string& error);
    bool StartThreads(std::string& error);
    void InitializeBridges();
    void StopThreads();
    void ShutdownSteamAudio();
    void ShutdownEngineSide();
    bool OpenNativeOutput(std::string& error);
    void CloseNativeOutput();
    void RecoverOutputDevice();
    void DrainReleasedSources();
    void DrainClockEvents();
    void DrainSourceEnds();
    void UpdateExternalListener();
    void PropagateRuntimeConfig();
    void SetState(EngineState state, const std::string& reason);
    bool ApplyBsp(const std::string& mapName, const std::vector<uint8_t>& bytes, std::string& error);
    void ResolveStaticPropGeometry(BspGeometry& geometry, const CoordinateConverter& converter);
    std::string ResolveSurfaceProp(const std::string& texture);
    void EnsureSurfacePropDatabase();
    std::vector<SoundSourcePtr> CreateEngineSlotSources(int32_t sampleRate) const;
    ProceduralSourceHooks MakeSourceHooks();
    std::vector<std::string> PhononSearchPaths() const;
    bool ReadFile(const std::string& path, std::vector<uint8_t>& out) const;

    EnginePaths m_paths;
    EngineConfig m_config;
    ConfigOverrideFn m_configOverrides;
    StaticConfig m_pendingStatic;
    bool m_pendingStaticValid = false;
    bool m_restartRequested = false;
    EngineState m_state = EngineState::Uninitialized;
    std::string m_stateReason;
    std::string m_backendDescription;

    // Engine side
    SignatureConfig m_signatures;
    EngineInterfaces m_interfaces;
    EngineFileSystem m_fileSystem;
    std::string m_fileSystemStatus;   // human-readable: validated layout or why it is unavailable
    GmaLocator m_gma;
    ChannelCapture m_capture;
    EngineHooks m_hooks;
    std::vector<SoundSourcePtr> m_slotSources;
    bool m_hooksInstalled = false;
    double m_gameClock = 0.0; // accumulated Tick() time, drives override expiry

    // Steam Audio side
    PhononContext m_context;
    std::unique_ptr<IBackend> m_backend;
    BackendDevices m_devices;
    HRTFRenderer m_renderer;
    Simulator m_simulator;
    ReflectionSimulator m_reflections;
    PathingSimulator m_pathing;
    SceneBuilder m_scene;
    MaterialLibrary m_materials;
    FallbackMixer m_fallback;
    bool m_steamAudioReady = false;

    // Output
    std::unique_ptr<IPlatformAudioOutput> m_output;
    RateAdaptedSource m_rateAdapter;
    AudioOutputPath m_outputPath = AudioOutputPath::Engine;
    OutputFormat m_outputFormat;
    float m_deviceRetryTimer = 0.f;
    uint32_t m_deviceRetries = 0;

    // Threads
    AudioThread m_audio;
    SimulationThread m_simulation;

    // Bridges
    ProceduralAudioBridge m_procedural;
    BassBridge m_bass;
    std::atomic<uint32_t> m_nextSourceId{ChannelCapture::kSlots + 1};

    // Map
    std::string m_mapName;
    std::shared_ptr<const BspGeometry> m_geometry;
    bool m_geometryPending = false;
    std::vector<uint8_t> m_bspScratch;

    // Materials: surfaceproperties database (loaded once) and the per-texture
    // $surfaceprop cache (texture -> lower-case surfaceprop, "" when unknown).
    bool m_surfacePropsLoaded = false;
    std::unordered_map<std::string, std::string> m_vmtSurfaceProps;
    size_t m_vmtLookups = 0;
    size_t m_vmtResolved = 0;

    // Listener
    EngineListener m_externalListener;
    bool m_externalListenerValid = false;
    int32_t m_localPlayer = -1;
    float m_flushTimer = 0.f;
    uint32_t m_clockHoldLogs = 0;

    // Recently ended sources (snd_sa_sounds history), newest last.
    static constexpr size_t kRecentSounds = 12;
    std::array<SourceEndEvent, kRecentSounds> m_recentSounds{};
    uint64_t m_recentSoundsNext = 0;

    // Dynamic occluders
    DynamicOccluderOptions OccluderOptions() const;
    void UpdateDynamicOccluders(float frameTime);
    void ResetDynamicOccluders();
    bool ListenerPosition(Vec3& out) const;
    ClientEntityList m_entityList;
    DynamicOccluders m_occluders;
    std::vector<EntitySnapshot> m_entityScratch;
    std::vector<EntitySnapshot> m_luaEntities;      // batch being built by Lua
    std::vector<EntitySnapshot> m_luaEntitiesReady; // last complete Lua batch
    bool m_luaBatchOpen = false;
    bool m_luaBatchReady = false;
    bool m_nativeEntitiesActive = false;
    float m_occluderTimer = 0.f;
    uint32_t m_gameTickMicros = 0;
    uint32_t m_maxGameTickMicros = 0;
    uint32_t m_entitySnapshotMicros = 0;
    uint32_t m_maxEntitySnapshotMicros = 0;
    uint32_t m_occluderUpdateMicros = 0;
    uint32_t m_maxOccluderUpdateMicros = 0;
};

} // namespace sa
