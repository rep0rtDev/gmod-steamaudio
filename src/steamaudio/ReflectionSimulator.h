// src/steamaudio/ReflectionSimulator.h
//
// Reflection / reverb simulation:
//   * fills the reflection part of IPLSimulationSharedInputs (rays, bounces,
//     IR duration, Ambisonic order) and per-source IPLSimulationInputs,
//   * generates probes over the static scene and bakes listener-centric
//     reverb (IPL_BAKEDDATAVARIATION_REVERB) on a background thread so
//     real-time reflections can fall back to baked data when the ray budget
//     is exhausted,
//   * caches baked probe batches on disk per map.
//
// Thread-safety: Update*/Configure*/Poll* run on the simulation thread. The
// bake worker thread only touches the Steam Audio baking API and the atomic
// progress/cancel flags.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "PhononApi.h"
#include "steamaudio/Config.h"
#include "steamaudio/Backend.h"
#include "mixing/LockFreeQueue.h"

namespace sa {

class PhononContext;
class Simulator;
class SceneBuilder;
struct BspGeometry;
struct SourceParams;
struct ProcessMemoryInfo;

enum class BakePhase : uint8_t {
    Idle, Preparing, Probes, Reflections, AwaitingPathing, Pathing, Saving, Ready, Cancelled, Failed
};

const char* BakePhaseName(BakePhase phase);

struct BakeStatus {
    uint64_t id = 0;
    BakePhase phase = BakePhase::Idle;
    IPLSceneType sceneType = IPL_SCENETYPE_DEFAULT;
    int32_t probes = 0;
    int32_t completedProbes = 0;
    int32_t pass = 1;
    float progress = 0.f;
    double startedAt = 0.0;
    double phaseStartedAt = 0.0;
    double updatedAt = 0.0;
    double finishedAt = 0.0;
    double elapsedSeconds = 0.0;
    double remainingSeconds = -1.0;
    bool active = false;
    bool cacheSaved = false;
    uint64_t pathingEstimatedBytes = 0;
    uint64_t pathingBudgetBytes = 0;
    uint64_t pathingGrowthBytes = 0;
    bool pathingAllowed = false;
    bool memoryLimited = false;
    char detail[192]{};
};

class ReflectionSimulator {
public:
    // Optional additional bake step executed on the bake thread after the
    // reverb bake (used by PathingSimulator for pathing data).
    using ExtraBakeStep = std::function<void(IPLScene scene, IPLProbeBatch batch, std::atomic<bool>& cancel,
                                              IPLProgressCallback progress, void* userData)>;

    using MemoryQuery = ProcessMemoryInfo (*)();
    explicit ReflectionSimulator(MemoryQuery memoryQuery = nullptr) : m_memoryQuery(memoryQuery) {}
    ~ReflectionSimulator();
    ReflectionSimulator(const ReflectionSimulator&) = delete;
    ReflectionSimulator& operator=(const ReflectionSimulator&) = delete;

    bool Initialize(PhononContext& context, Simulator& simulator, const StaticConfig& config,
                    const BackendDevices& devices = BackendDevices{});
    BakeStatus Status() const;
    static double EstimateRemaining(double elapsedSeconds, float progress);
    static uint64_t EstimatePathingMemory(uint32_t probes);
    static uint64_t PathingMemoryBudget(int32_t limitMiB, const ProcessMemoryInfo& memory);
    static bool PathingMemoryExceeded(uint64_t baseline, uint64_t budget, const ProcessMemoryInfo& memory);
    void SetPathingMemoryLimit(int32_t limitMiB) { m_pathingMemoryLimitMiB.store(limitMiB, std::memory_order_release); }
    void Shutdown();

    // Shared inputs for iplSimulatorRunReflections. `listener` is in SA space.
    void FillSharedInputs(const RuntimeConfig& cfg, IPLSimulationSharedInputs& shared) const;

    // Per-source reflection settings.
    void FillSourceInputs(const RuntimeConfig& cfg, const SourceParams& params, IPLSimulationInputs& inputs) const;

    // ---- Probes / baking ----------------------------------------------------------
    // Starts probe generation + reverb bake for `scene` (already committed).
    // `cacheFile` is where the baked batch is saved/loaded. Returns false if a
    // bake is already running or the scene is null.
    bool StartBake(std::shared_ptr<const BspGeometry> geometry, const RuntimeConfig& cfg,
                   const std::string& cacheFile, ExtraBakeStep extraStep);
    // Attempts to load a previously baked batch for the map. Returns true on success.
    bool LoadCachedBake(const std::string& cacheFile);
    void CancelBake();
    bool ContinuePathing(bool enabled);
    bool BakeInProgress() const { return m_bakeRunning.load(std::memory_order_acquire); }
    float BakeProgress() const { return m_bakeProgress.load(std::memory_order_relaxed); }
    // Called on the simulation thread each tick: when a bake finished, commits
    // the batch and attaches it to the simulator. Returns true when a new batch
    // was attached.
    bool PollBake();
    // Removes the probe batch from the simulator and releases it.
    void ClearProbes();

    bool HasBakedData() const { return m_probeBatch != nullptr && m_batchAttached; }
    IPLProbeBatch ProbeBatch() const { return m_probeBatch; }
    int32_t ProbeCount() const { return m_probeCount; }
    bool PathingBaked() const { return m_pathingBaked; }
    const IPLBakedDataIdentifier& ReverbIdentifier() const { return m_reverbIdentifier; }

private:
    struct BakeJob {
        std::shared_ptr<const BspGeometry> geometry;
        BackendDevices devices;
        std::unique_ptr<IBackend> backendOwner;
        std::shared_ptr<SceneBuilder> sceneOwner;
        IPLScene scene = nullptr;
        IPLProbeBatch batch = nullptr;
        RuntimeConfig cfg;
        std::string cacheFile;
        ExtraBakeStep extraStep;
        bool succeeded = false;
        bool pathingBaked = false;
        int32_t probeCount = 0;
    };

    static void IPLCALL ProgressCallback(IPLfloat32 progress, void* userData);
    void BakeThreadMain(std::shared_ptr<BakeJob> job);
    bool RunPathingStep(BakeJob& job);
    ProcessMemoryInfo QueryMemory() const;
    void SetPhase(BakePhase phase, const char* detail = "");
    void FinishBake(bool succeeded, bool saved, const char* detail = "");
    bool SaveBatch(IPLProbeBatch batch, const std::string& file) const;
    void ReleaseBatch(IPLProbeBatch& batch);

    MemoryQuery m_memoryQuery = nullptr;
    PhononContext* m_context = nullptr;
    Simulator* m_simulator = nullptr;
    StaticConfig m_static;
    BackendDevices m_devices;
    mutable std::mutex m_progressMutex;
    BakeStatus m_progressState;
    SeqLock<BakeStatus> m_status;
    uint64_t m_nextBakeId = 0;

    IPLProbeBatch m_probeBatch = nullptr;
    bool m_batchAttached = false;
    bool m_pathingBaked = false;
    int32_t m_probeCount = 0;
    IPLBakedDataIdentifier m_reverbIdentifier{};

    std::thread m_bakeThread;
    std::shared_ptr<BakeJob> m_activeJob;
    std::atomic<bool> m_bakeRunning{false};
    std::atomic<bool> m_bakeDone{false};
    std::atomic<bool> m_bakeCancel{false};
    std::atomic<float> m_bakeProgress{0.f};
    std::atomic<int> m_pathingDecision{0};
    std::atomic<int32_t> m_pathingMemoryLimitMiB{1024};
    std::condition_variable m_decisionWake;
    std::mutex m_decisionMutex;
};

} // namespace sa
