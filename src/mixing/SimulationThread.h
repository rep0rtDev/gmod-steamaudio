// src/mixing/SimulationThread.h
//
// Dedicated Steam Audio simulation thread. It owns every call into the
// IPLSimulator / IPLScene objects after initialization:
//
//   * applies geometry edits (static map geometry, static props, dynamic
//     brush entities) posted by the game thread and commits the scene,
//   * publishes listener + per-source simulation inputs (direct, reflections,
//     pathing) from the SourceParams snapshots,
//   * runs direct simulation every tick, reflections and pathing on their own
//     (slower) intervals,
//   * drives probe generation / reverb + pathing baking and attaches baked
//     data when ready,
//   * creates / destroys IPLSource handles for engine channels and stream
//     sources within the simulator's source budget.
//
// Thread-safety:
//   game thread  -> sim thread : bounded SpscQueue<Command> with a short wake mutex
//                                (shared_ptr payloads move large meshes without copying)
//   any thread   -> sim thread : SeqLock<RuntimeConfig>, SeqLock<ListenerState>
//   sim thread   -> audio      : retained IPLSource published in SoundSource
//   sim thread   -> game       : ReleaseQueue for retired stream sources, atomics for stats
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/ChannelCapture.h"
#include "core/EngineHooks.h"
#include "mixing/LockFreeQueue.h"
#include "mixing/SoundSource.h"
#include "platform/PlatformAudioOutput.h"
#include "steamaudio/BspGeometry.h"
#include "steamaudio/Config.h"
#include "steamaudio/PathingSimulator.h"
#include "steamaudio/ReflectionSimulator.h"
#include "steamaudio/SceneBuilder.h"
#include "steamaudio/Simulator.h"

namespace sa {

class PhononContext;

struct SimulationThreadSetup {
    PhononContext* context = nullptr;             // required
    Simulator* simulator = nullptr;               // required
    ReflectionSimulator* reflections = nullptr;   // optional
    PathingSimulator* pathing = nullptr;          // optional
    SceneBuilder* scene = nullptr;                // optional (no geometry => free-field simulation)
    ChannelCapture* capture = nullptr;            // engine channel sources (optional)
    EngineHooks* hooks = nullptr;                 // listener provider (optional)
    const std::atomic<uint64_t>* audioFrameCounter = nullptr;
    std::string cacheDirectory;                   // where baked probe batches are stored
    size_t maxStreamSources = 256;
};

struct SimulationThreadStats {
    std::atomic<uint64_t> ticks{0};
    std::atomic<uint64_t> directRuns{0};
    std::atomic<uint64_t> reflectionRuns{0};
    std::atomic<uint64_t> pathingRuns{0};
    std::atomic<uint64_t> sceneCommits{0};
    std::atomic<uint32_t> simulatedSources{0};
    std::atomic<uint32_t> lastDirectMicros{0};
    std::atomic<uint32_t> lastReflectionMicros{0};
    std::atomic<uint32_t> lastPathingMicros{0};
    std::atomic<uint32_t> lastTickMicros{0};
    std::atomic<uint32_t> lastCommandMicros{0};
    std::atomic<uint32_t> maxCommandMicros{0};
    std::atomic<uint32_t> lastSceneCommitMicros{0};
    std::atomic<uint32_t> maxSceneCommitMicros{0};
    std::atomic<uint32_t> maxTickMicros{0};
    std::atomic<uint32_t> staticTriangles{0};
    std::atomic<uint32_t> dynamicMeshes{0};
    std::atomic<float> bakeProgress{0.f};
    std::atomic<bool> bakeRunning{false};
    std::atomic<bool> bakedDataAttached{false};
    std::atomic<bool> listenerValid{false};
    std::atomic<uint32_t> sourceBudgetRejects{0};
};

// Dynamic geometry handle used by the game thread. Brush models from the BSP
// use their model index; other dynamic geometry gets ids from AllocDynamicId.
using DynamicGeometryId = uint32_t;

class SimulationThread {
public:
    SimulationThread();
    ~SimulationThread();
    SimulationThread(const SimulationThread&) = delete;
    SimulationThread& operator=(const SimulationThread&) = delete;

    bool Start(const SimulationThreadSetup& setup);
    void Stop();
    bool Running() const { return m_running.load(std::memory_order_acquire); }

    // ---- Any thread -------------------------------------------------------------
    void SetRuntimeConfig(const RuntimeConfig& cfg) { m_runtime.Store(cfg); }
    void SetExternalListener(const ListenerState& listener) { m_externalListener.Store(listener); }

    // ---- Game thread: sources ---------------------------------------------------
    bool AddStreamSource(const SoundSourcePtr& source);
    bool RemoveStreamSource(uint32_t id);
    ReleaseQueue& Released() { return m_released; }

    // ---- Game thread: geometry ----------------------------------------------------
    // Replaces all static geometry with the map's world mesh and registers the
    // BSP brush models as dynamic instances at their spawn transforms.
    bool SetMapGeometry(std::shared_ptr<const BspGeometry> geometry, const std::string& mapName);
    // Adds static prop collision geometry (already in SA space) to the static scene.
    bool AddStaticGeometry(std::shared_ptr<const MeshData> mesh, const std::string& debugName);
    // Dynamic geometry that is not a BSP brush model (e.g. physics props).
    DynamicGeometryId AllocDynamicId();
    bool AddDynamicGeometry(DynamicGeometryId id, std::shared_ptr<const MeshData> localMesh,
                            const Transform& transform, const std::string& debugName);
    bool UpdateDynamicGeometry(DynamicGeometryId id, const Transform& transform);
    // Brush model N ("*N") of the current map.
    bool UpdateBrushModel(int32_t modelIndex, const Vec3& origin, const Vec3& angles);
    bool RemoveDynamicGeometry(DynamicGeometryId id);
    bool ClearGeometry();
    // Probe/bake control. Baking uses the cache file derived from the map name.
    bool RequestBake(bool forceRebake);
    bool CancelBake();

    const SimulationThreadStats& Stats() const { return m_stats; }
    static uint64_t BakeFingerprint(const BspGeometry& geometry, const RuntimeConfig& cfg,
                                    const IPLSimulationSettings& settings);

private:
    struct Command {
        enum class Type : uint8_t {
            AddSource,
            RemoveSource,
            SetMap,
            AddStatic,
            AddDynamic,
            UpdateDynamic,
            RemoveDynamic,
            ClearGeometry,
            Bake,
            CancelBake,
        };
        Type type = Type::AddSource;
        uint32_t id = 0;
        int32_t modelIndex = -1;
        bool flag = false;
        Transform transform;
        SoundSourcePtr source;
        std::shared_ptr<const BspGeometry> geometry;
        std::shared_ptr<const MeshData> mesh;
        std::string name;
    };

    struct TrackedSource {
        SoundSourcePtr source;
        bool isEngineSlot = false;
        uint32_t slotIndex = 0;
    };

    struct DynamicInstance {
        SceneBuilder::DynamicId sceneId = SceneBuilder::kInvalidDynamic;
        Vec3 lastOrigin{};
        Vec3 lastAngles{};
        bool isBrushModel = false;
    };

    void Run();
    void Tick();
    bool QueueCommand(Command&& cmd);
    void DrainCommands();
    void HandleCommand(Command& cmd);
    void ApplyMapGeometry(const BspGeometry& geometry);
    void DestroyAllSources(bool force);
    ListenerState CurrentListener() const;
    bool UpdateSourceInputs(SoundSource& source, const SourceParams& params, const ListenerState& listener,
                            bool runReflections, bool runPathing);
    bool EnsureSimulationSource(SoundSource& source, uint32_t generation);
    void RetireSimulationSource(SoundSource& source);
    void EvictIdleSources(size_t needed);
    void StartBakeForCurrentMap(bool force);
    std::string CacheFileForMap(const std::string& mapName) const;
    void PublishStats();

    static float IPLCALL DistanceAttenuationCallback(IPLfloat32 distance, void* userData);

    SimulationThreadSetup m_setup;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::mutex m_wakeMutex;
    std::condition_variable m_wake;
    std::atomic<uint32_t> m_pendingCommands{0};

    SeqLock<RuntimeConfig> m_runtime;
    SeqLock<ListenerState> m_externalListener;
    RuntimeConfig m_cfg; // sim thread copy

    SpscQueue<Command> m_commands{1024};
    ReleaseQueue m_released{256};
    std::atomic<uint32_t> m_nextDynamicId{1u << 16}; // ids below are reserved for BSP brush models

    // Sim thread state.
    std::vector<TrackedSource> m_streams;
    std::array<uint32_t, ChannelCapture::kSlots> m_slotGeneration{};
    std::unordered_map<DynamicGeometryId, DynamicInstance> m_dynamic;
    std::string m_mapName;
    std::shared_ptr<const BspGeometry> m_mapGeometry;
    bool m_sceneDirty = false;
    bool m_bakeRequested = false;
    std::atomic<bool> m_cancelBakeRequested{false};
    bool m_bakeForce = false;
    bool m_bakeAttempted = false;
    uint64_t m_tick = 0;
    uint64_t m_lastReflectionsTick = 0;
    uint64_t m_lastPathingTick = 0;
    std::chrono::steady_clock::time_point m_lastReflectionsTime{};
    std::chrono::steady_clock::time_point m_lastPathingTime{};
    std::chrono::steady_clock::time_point m_nextGeometryCommit{};
    IPLSimulationSharedInputs m_shared{};
    SimulationThreadStats m_stats;
};

} // namespace sa
