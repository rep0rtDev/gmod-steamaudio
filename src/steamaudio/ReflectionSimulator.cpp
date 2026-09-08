// src/steamaudio/ReflectionSimulator.cpp
#include "ReflectionSimulator.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "PhononContext.h"
#include "GPUBackend.h"
#include "Simulator.h"
#include "steamaudio/BspGeometry.h"
#include "mixing/SoundSource.h"
#include "platform/PlatformAudioOutput.h"
#include "util/Logging.h"

namespace sa {

namespace {

// Bounding box of the scene is not exposed by Steam Audio; probes are generated
// with a transform covering a large volume around the origin. Source maps fit
// inside +-16384 units (~312 m), so a 700 m cube centered at the origin covers
// every map while UNIFORMFLOOR placement only keeps probes above walkable floors.
IPLMatrix4x4 ProbeVolumeTransform(float extentMeters)
{
    IPLMatrix4x4 m{};
    // Row-major affine: scale by 2*extent and translate by -extent so the unit
    // cube [0,1]^3 maps to [-extent, extent]^3.
    m.elements[0][0] = extentMeters * 2.f;
    m.elements[1][1] = extentMeters * 2.f;
    m.elements[2][2] = extentMeters * 2.f;
    m.elements[0][3] = -extentMeters;
    m.elements[1][3] = -extentMeters;
    m.elements[2][3] = -extentMeters;
    m.elements[3][3] = 1.f;
    return m;
}

} // namespace

ReflectionSimulator::~ReflectionSimulator()
{
    Shutdown();
}

bool ReflectionSimulator::Initialize(PhononContext& context, Simulator& simulator, const StaticConfig& config,
                                      const BackendDevices& devices)
{
    Shutdown();
    if (!context.IsValid() || !simulator.IsValid())
        return false;
    m_context = &context;
    m_simulator = &simulator;
    m_static = config;
    m_devices = devices;
    {
        std::lock_guard<std::mutex> lock(m_progressMutex);
        m_progressState = BakeStatus{};
        m_status.Store(m_progressState);
    }

    m_reverbIdentifier = IPLBakedDataIdentifier{};
    m_reverbIdentifier.type = IPL_BAKEDDATATYPE_REFLECTIONS;
    m_reverbIdentifier.variation = IPL_BAKEDDATAVARIATION_REVERB;
    m_reverbIdentifier.endpointInfluence.center = IPLVector3{0.f, 0.f, 0.f};
    m_reverbIdentifier.endpointInfluence.radius = 0.f;
    return true;
}

void ReflectionSimulator::Shutdown()
{
    CancelBake();
    if (m_bakeThread.joinable())
        m_bakeThread.join();
    if (m_activeJob) {
        ReleaseBatch(m_activeJob->batch);
        m_activeJob.reset();
    }
    ClearProbes();
    m_context = nullptr;
    m_simulator = nullptr;
}

void ReflectionSimulator::FillSharedInputs(const RuntimeConfig& cfg, IPLSimulationSharedInputs& shared) const
{
    shared.numRays = std::max(32, std::min(cfg.numRays, m_static.maxRays));
    shared.numBounces = std::max(1, std::min(cfg.numBounces, 64));
    shared.duration = std::max(0.1f, std::min(cfg.irDuration, m_static.maxIrDuration));
    shared.order = std::max(0, std::min(cfg.ambisonicOrder, m_static.maxAmbisonicOrder));
    shared.irradianceMinDistance = std::max(0.1f, cfg.irradianceMinDistance);
}

void ReflectionSimulator::FillSourceInputs(const RuntimeConfig& cfg, const SourceParams& params,
                                           IPLSimulationInputs& inputs) const
{
    (void)params;
    for (int b = 0; b < IPL_NUM_BANDS; ++b)
        inputs.reverbScale[b] = 1.f;
    inputs.hybridReverbTransitionTime = std::max(0.05f, cfg.hybridTransitionTime);
    inputs.hybridReverbOverlapPercent = std::max(0.f, std::min(cfg.hybridOverlapPercent, 1.f));
    if (cfg.useBakedReverb && HasBakedData()) {
        inputs.baked = IPL_TRUE;
        inputs.bakedDataIdentifier = m_reverbIdentifier;
    } else {
        inputs.baked = IPL_FALSE;
        inputs.bakedDataIdentifier = IPLBakedDataIdentifier{};
    }
}

// ---------------------------------------------------------------------------
// Baking
// ---------------------------------------------------------------------------

const char* BakePhaseName(BakePhase phase)
{
    switch (phase) {
    case BakePhase::Preparing: return "preparing";
    case BakePhase::Probes: return "probes";
    case BakePhase::Reflections: return "reflections";
    case BakePhase::AwaitingPathing: return "awaiting_pathing";
    case BakePhase::Pathing: return "pathing";
    case BakePhase::Saving: return "saving";
    case BakePhase::Ready: return "ready";
    case BakePhase::Cancelled: return "cancelled";
    case BakePhase::Failed: return "failed";
    default: return "idle";
    }
}

static double BakeTime()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

ProcessMemoryInfo ReflectionSimulator::QueryMemory() const
{
    return m_memoryQuery ? m_memoryQuery() : QueryProcessMemory();
}

uint64_t ReflectionSimulator::EstimatePathingMemory(uint32_t probes)
{
    if (probes == 0)
        return 0;
    if (probes > 32768)
        return UINT64_MAX;
    return 64ull * 1024 * 1024 + static_cast<uint64_t>(probes) * probes * 128;
}

uint64_t ReflectionSimulator::PathingMemoryBudget(int32_t limitMiB, const ProcessMemoryInfo& memory)
{
    if (!memory.valid || limitMiB <= 0)
        return 0;
    constexpr uint64_t mib = 1024 * 1024;
    const uint64_t reserve = std::max(512 * mib, memory.physicalTotal / 4);
    const auto remaining = [](uint64_t available, uint64_t keep) { return available > keep ? available - keep : 0; };
    return std::min({static_cast<uint64_t>(std::min(limitMiB, 16384)) * mib,
                     remaining(memory.physicalAvailable, reserve), remaining(memory.commitAvailable, reserve),
                     remaining(memory.virtualAvailable, 128 * mib)});
}

bool ReflectionSimulator::PathingMemoryExceeded(uint64_t baseline, uint64_t budget, const ProcessMemoryInfo& memory)
{
    return budget == 0 || PathingMemoryBudget(16384, memory) == 0 ||
           (memory.privateBytes > baseline && memory.privateBytes - baseline > budget);
}

double ReflectionSimulator::EstimateRemaining(double elapsedSeconds, float progress)
{
    if (!std::isfinite(elapsedSeconds) || !std::isfinite(progress) || elapsedSeconds < 0.5 || progress <= 0.f)
        return -1.0;
    return progress >= 1.f ? 0.0 : elapsedSeconds * (1.0 - progress) / progress;
}

BakeStatus ReflectionSimulator::Status() const
{
    BakeStatus status = m_status.Load();
    if (status.phase == BakePhase::AwaitingPathing) {
        status.pathingEstimatedBytes = EstimatePathingMemory(static_cast<uint32_t>(std::max(0, status.probes)));
        status.pathingBudgetBytes = PathingMemoryBudget(m_pathingMemoryLimitMiB.load(std::memory_order_acquire), QueryMemory());
        status.pathingAllowed = status.probes > 0 && status.pathingEstimatedBytes <= status.pathingBudgetBytes;
    }
    if (status.id != 0) {
        const double now = status.active ? BakeTime() : status.finishedAt;
        status.elapsedSeconds = std::max(0.0, now - status.startedAt);
        if (status.active && status.remainingSeconds > 0.0)
            status.remainingSeconds = std::max(1.0, status.remainingSeconds - (now - status.updatedAt));
    }
    return status;
}

void ReflectionSimulator::SetPhase(BakePhase phase, const char* detail)
{
    std::lock_guard<std::mutex> lock(m_progressMutex);
    m_progressState.phase = phase;
    m_progressState.phaseStartedAt = m_progressState.updatedAt = BakeTime();
    m_progressState.progress = 0.f;
    m_progressState.remainingSeconds = -1.0;
    m_progressState.pass = 1;
    std::snprintf(m_progressState.detail, sizeof(m_progressState.detail), "%s", detail);
    m_bakeProgress.store(0.f, std::memory_order_relaxed);
    m_status.Store(m_progressState);
}

void ReflectionSimulator::FinishBake(bool succeeded, bool saved, const char* detail)
{
    SetPhase(succeeded ? BakePhase::Ready : m_bakeCancel.load(std::memory_order_acquire)
                                              ? BakePhase::Cancelled : BakePhase::Failed, detail);
    {
        std::lock_guard<std::mutex> lock(m_progressMutex);
        m_progressState.active = false;
        m_progressState.progress = succeeded ? 1.f : m_progressState.progress;
        m_progressState.finishedAt = m_progressState.updatedAt = BakeTime();
        m_progressState.cacheSaved = saved;
        m_progressState.remainingSeconds = -1.0;
        m_status.Store(m_progressState);
    }
    m_bakeDone.store(true, std::memory_order_release);
    m_bakeRunning.store(false, std::memory_order_release);
}

void IPLCALL ReflectionSimulator::ProgressCallback(IPLfloat32 progress, void* userData)
{
    auto* self = static_cast<ReflectionSimulator*>(userData);
    if (!self || !std::isfinite(progress))
        return;
    std::lock_guard<std::mutex> lock(self->m_progressMutex);
    BakeStatus& status = self->m_progressState;
    const double now = BakeTime();
    progress = std::clamp(progress, 0.f, 1.f);
    if (status.phase == BakePhase::Pathing && progress + 0.001f < status.progress) {
        ++status.pass;
        status.phaseStartedAt = now;
    }
    status.progress = progress;
    status.updatedAt = now;
    status.remainingSeconds = EstimateRemaining(now - status.phaseStartedAt, progress);
    if (status.phase == BakePhase::Reflections)
        status.completedProbes = std::min(status.probes, static_cast<int32_t>(std::lround(progress * status.probes)));
    self->m_bakeProgress.store(progress, std::memory_order_relaxed);
    self->m_status.Store(status);
}

bool ReflectionSimulator::StartBake(std::shared_ptr<const BspGeometry> geometry, const RuntimeConfig& cfg,
                                    const std::string& cacheFile, ExtraBakeStep extraStep)
{
    if (!m_context || !m_simulator || !geometry || geometry->world.Empty())
        return false;
    if (m_bakeRunning.load(std::memory_order_acquire))
        return false;
    if (m_bakeThread.joinable())
        m_bakeThread.join();
    if (m_activeJob) {
        ReleaseBatch(m_activeJob->batch);
        m_activeJob.reset();
    }

    auto job = std::make_shared<BakeJob>();
    job->geometry = std::move(geometry);
    job->devices = m_devices;
    job->cfg = cfg;
    SetPathingMemoryLimit(cfg.pathingMemoryLimitMiB);
    job->cacheFile = cacheFile;
    job->extraStep = std::move(extraStep);
    {
        std::lock_guard<std::mutex> lock(m_progressMutex);
        m_progressState = BakeStatus{};
        m_progressState.id = ++m_nextBakeId;
        m_progressState.active = true;
        m_progressState.startedAt = BakeTime();
        m_progressState.sceneType = job->devices.sceneType;
        m_status.Store(m_progressState);
    }
    SetPhase(BakePhase::Preparing);
    m_pathingDecision.store(0, std::memory_order_release);
    m_bakeCancel.store(false, std::memory_order_release);
    m_bakeDone.store(false, std::memory_order_release);
    m_bakeProgress.store(0.f, std::memory_order_relaxed);
    m_bakeRunning.store(true, std::memory_order_release);
    m_activeJob = job;
    try {
        m_bakeThread = std::thread(&ReflectionSimulator::BakeThreadMain, this, job);
    } catch (const std::exception& error) {
        FinishBake(false, false, error.what());
        return false;
    }
    return true;
}

bool ReflectionSimulator::RunPathingStep(BakeJob& job)
{
    const ProcessMemoryInfo initial = QueryMemory();
    const uint64_t estimate = EstimatePathingMemory(static_cast<uint32_t>(std::max(0, job.probeCount)));
    const uint64_t systemBudget = PathingMemoryBudget(16384, initial);
    const uint64_t budget = PathingMemoryBudget(m_pathingMemoryLimitMiB.load(std::memory_order_acquire), initial);
    const bool allowed = job.probeCount > 0 && estimate <= budget;
    {
        std::lock_guard<std::mutex> lock(m_progressMutex);
        m_progressState.pathingEstimatedBytes = estimate;
        m_progressState.pathingBudgetBytes = budget;
        m_progressState.pathingGrowthBytes = 0;
        m_progressState.pathingAllowed = allowed;
        m_progressState.memoryLimited = !allowed;
        m_status.Store(m_progressState);
    }
    if (!allowed) {
        SA_LOGW("Pathing blocked by memory guard: estimate %.0f MiB, budget %.0f MiB",
                static_cast<double>(estimate) / 1048576.0, static_cast<double>(budget) / 1048576.0);
        return false;
    }
    SetPhase(BakePhase::Pathing);
    struct Watchdog {
        std::atomic<bool> done{false};
        std::thread thread;
        ~Watchdog()
        {
            done.store(true, std::memory_order_release);
            if (thread.joinable()) thread.join();
        }
    } watchdog;
    try {
        watchdog.thread = std::thread([&, this] {
            while (!watchdog.done.load(std::memory_order_acquire)) {
                if (m_bakeCancel.load(std::memory_order_acquire)) {
                    iplPathBakerCancelBake(m_context->Handle());
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    continue;
                }
                const ProcessMemoryInfo memory = QueryMemory();
                const uint64_t limit = std::min(systemBudget, static_cast<uint64_t>(
                    std::clamp(m_pathingMemoryLimitMiB.load(std::memory_order_acquire), 0, 16384)) * 1048576);
                const uint64_t growth = memory.privateBytes > initial.privateBytes ? memory.privateBytes - initial.privateBytes : 0;
                const bool exceeded = limit < estimate || PathingMemoryExceeded(initial.privateBytes, limit, memory);
                {
                    std::lock_guard<std::mutex> lock(m_progressMutex);
                    m_progressState.pathingBudgetBytes = limit;
                    m_progressState.pathingGrowthBytes = std::max(m_progressState.pathingGrowthBytes, growth);
                    if (exceeded) {
                        m_progressState.memoryLimited = true;
                        std::snprintf(m_progressState.detail, sizeof(m_progressState.detail),
                                      "Memory guard requested cancellation; reflection cache is preserved");
                    }
                    m_status.Store(m_progressState);
                }
                if (exceeded) {
                    m_bakeCancel.store(true, std::memory_order_release);
                    iplPathBakerCancelBake(m_context->Handle());
                    SA_LOGW("Pathing cancelled by memory guard: process growth %.0f MiB, budget %.0f MiB",
                            static_cast<double>(growth) / 1048576.0, static_cast<double>(limit) / 1048576.0);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        });
        job.extraStep(job.scene, job.batch, m_bakeCancel, &ReflectionSimulator::ProgressCallback, this);
    } catch (const std::exception& error) {
        SA_LOGE("Pathing failed; retaining reflections: %s", error.what());
        return false;
    }
    return !m_bakeCancel.load(std::memory_order_acquire);
}

void ReflectionSimulator::BakeThreadMain(std::shared_ptr<BakeJob> job)
try {
    PhononContext* ctx = m_context;
    if (!ctx || !ctx->IsValid()) {
        FinishBake(false, false, "Steam Audio context unavailable");
        return;
    }
    if (job->devices.sceneType == IPL_SCENETYPE_RADEONRAYS) {
        SetPhase(BakePhase::Preparing, "Creating an isolated GPU bake device");
        StaticConfig bakeConfig = m_static;
        bakeConfig.sceneType = SceneTypePreference::RadeonRays;
        bakeConfig.enableTan = false;
        bakeConfig.gpuComputeUnits = 0;
        auto backend = std::make_unique<GPUBackend>();
        std::string error;
        const bool ready = backend->Initialize(*ctx, bakeConfig, error);
        const BackendDevices& devices = backend->Devices();
        if (!ready || devices.sceneType != IPL_SCENETYPE_RADEONRAYS || !devices.openCL || !devices.radeonRays ||
            devices.openCL == job->devices.openCL || devices.radeonRays == job->devices.radeonRays) {
            if (error.empty()) error = "GPU bake device is not isolated from the live scene";
            SA_LOGE("Cannot initialize GPU bake resources: %s", error.c_str());
            FinishBake(false, false, error.c_str());
            return;
        }
        job->devices = devices;
        job->backendOwner = std::move(backend);
        SA_LOGI("Baking uses isolated Radeon Rays/OpenCL resources");
    }
    job->sceneOwner = std::make_shared<SceneBuilder>();
    if (!job->sceneOwner->Initialize(*ctx, job->devices) ||
        !job->sceneOwner->AddStaticMesh(job->geometry->world, "bake_world") ||
        (!job->geometry->staticPropMesh.Empty() &&
         !job->sceneOwner->AddStaticMesh(job->geometry->staticPropMesh, "bake_props"))) {
        FinishBake(false, false, "Could not create the bake scene");
        return;
    }
    CoordinateConverter converter;
    converter.SetUnitsPerMeter(job->cfg.unitsPerMeter);
    for (const BspBrushModel& brush : job->geometry->brushModels) {
        if (m_bakeCancel.load(std::memory_order_acquire)) {
            FinishBake(false, false);
            return;
        }
        if (brush.solid && !brush.mesh.Empty() &&
            job->sceneOwner->AddDynamic(brush.mesh,
                converter.TransformToSA(Transform::FromAngles(brush.spawnOrigin, {})), "bake_brush") ==
                SceneBuilder::kInvalidDynamic) {
            FinishBake(false, false, "Could not create bake brush geometry");
            return;
        }
    }
    job->sceneOwner->Commit();
    job->scene = job->sceneOwner->Scene();
    {
        std::lock_guard<std::mutex> lock(m_progressMutex);
        m_progressState.sceneType = job->sceneOwner->SceneType();
        m_status.Store(m_progressState);
    }
    if (iplProbeBatchCreate(ctx->Handle(), &job->batch) != IPL_STATUS_SUCCESS || !job->batch) {
        FinishBake(false, false, "Could not create the probe batch");
        return;
    }
    SetPhase(BakePhase::Probes);

    // 1. Probe generation over the whole map volume.
    IPLProbeArray probeArray = nullptr;
    IPLerror err = iplProbeArrayCreate(ctx->Handle(), &probeArray);
    if (err == IPL_STATUS_SUCCESS && probeArray) {
        IPLProbeGenerationParams gen{};
        gen.type = IPL_PROBEGENERATIONTYPE_UNIFORMFLOOR;
        gen.spacing = std::max(0.5f, job->cfg.probeSpacing);
        gen.height = std::max(0.2f, job->cfg.probeHeight);
        gen.transform = ProbeVolumeTransform(350.f);
        for (int axis = 0; axis < 3; ++axis)
            gen.transform.elements[axis][3] += 0.5f * gen.transform.elements[axis][axis];
        iplProbeArrayGenerateProbes(probeArray, job->scene, &gen);
        job->probeCount = iplProbeArrayGetNumProbes(probeArray);
        SA_LOGI("Generated %d probes (spacing %.1f m, height %.1f m)", job->probeCount, gen.spacing, gen.height);
        if (job->probeCount > 0)
            iplProbeBatchAddProbeArray(job->batch, probeArray);
        iplProbeArrayRelease(&probeArray);
    } else {
        SA_LOGE("iplProbeArrayCreate failed: %s", IplErrorToString(err));
    }

    if (job->probeCount <= 0 || m_bakeCancel.load(std::memory_order_acquire)) {
        FinishBake(false, false, job->probeCount <= 0 ? "No probes generated" : "");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_progressMutex);
        m_progressState.probes = job->probeCount;
        m_status.Store(m_progressState);
    }
    iplProbeBatchCommit(job->batch);
    SetPhase(BakePhase::Reflections);

    // 2. Reverb bake.
    IPLReflectionsBakeParams bake{};
    bake.scene = job->scene;
    bake.probeBatch = job->batch;
    bake.sceneType = job->sceneOwner->SceneType();
    bake.identifier = m_reverbIdentifier;
    const IPLReflectionEffectType reflType =
        m_simulator ? m_simulator->Settings().reflectionType : IPL_REFLECTIONEFFECTTYPE_HYBRID;
    IPLReflectionsBakeFlags flags = static_cast<IPLReflectionsBakeFlags>(0);
    if (reflType == IPL_REFLECTIONEFFECTTYPE_CONVOLUTION || reflType == IPL_REFLECTIONEFFECTTYPE_TAN ||
        reflType == IPL_REFLECTIONEFFECTTYPE_HYBRID)
        flags = static_cast<IPLReflectionsBakeFlags>(flags | IPL_REFLECTIONSBAKEFLAGS_BAKECONVOLUTION);
    if (reflType == IPL_REFLECTIONEFFECTTYPE_PARAMETRIC || reflType == IPL_REFLECTIONEFFECTTYPE_HYBRID)
        flags = static_cast<IPLReflectionsBakeFlags>(flags | IPL_REFLECTIONSBAKEFLAGS_BAKEPARAMETRIC);
    bake.bakeFlags = flags;
    bake.numRays = std::max(256, job->cfg.bakeRays);
    bake.numDiffuseSamples = std::max(8, m_static.numDiffuseSamples);
    bake.numBounces = std::max(1, job->cfg.bakeBounces);
    bake.simulatedDuration = std::max(0.2f, job->cfg.bakeDuration);
    bake.savedDuration = std::min(bake.simulatedDuration, m_static.maxIrDuration);
    bake.order = std::max(0, std::min(job->cfg.ambisonicOrder, m_static.maxAmbisonicOrder));
    bake.numThreads = m_simulator ? std::max(1, m_simulator->Settings().numThreads) : 1;
    bake.rayBatchSize = 16;
    bake.irradianceMinDistance = std::max(0.1f, job->cfg.irradianceMinDistance);
    bake.bakeBatchSize = 1;
    bake.openCLDevice = bake.sceneType == IPL_SCENETYPE_RADEONRAYS ? job->devices.openCL : nullptr;
    bake.radeonRaysDevice = bake.sceneType == IPL_SCENETYPE_RADEONRAYS ? job->devices.radeonRays : nullptr;

    SA_LOGI("Baking reverb: %d probes, %d rays, %d bounces, %.2fs, scene=%d, threads=%d", job->probeCount,
            bake.numRays, bake.numBounces, bake.simulatedDuration, static_cast<int>(bake.sceneType), bake.numThreads);
    iplReflectionsBakerBake(ctx->Handle(), &bake, &ReflectionSimulator::ProgressCallback, this);

    if (m_bakeCancel.load(std::memory_order_acquire)) {
        SA_LOGI("Reverb bake cancelled");
        FinishBake(false, false);
        return;
    }
    iplProbeBatchCommit(job->batch);
    IPLBakedDataIdentifier reverb = m_reverbIdentifier;
    job->succeeded = iplProbeBatchGetDataSize(job->batch, &reverb) > 0;
    if (!job->succeeded) {
        FinishBake(false, false, "Steam Audio produced no reflection data");
        return;
    }
    SetPhase(BakePhase::Saving);
    bool saved = !job->cacheFile.empty() && SaveBatch(job->batch, job->cacheFile);
    {
        std::lock_guard<std::mutex> lock(m_progressMutex);
        m_progressState.cacheSaved = saved;
        m_status.Store(m_progressState);
    }

    // 3. Optional pathing bake into the same batch.
    if (job->extraStep) {
        SetPhase(BakePhase::AwaitingPathing);
        {
            std::unique_lock<std::mutex> lock(m_decisionMutex);
            m_decisionWake.wait(lock, [this] {
                return m_pathingDecision.load(std::memory_order_acquire) != 0 ||
                       m_bakeCancel.load(std::memory_order_acquire);
            });
        }
        if (!m_bakeCancel.load(std::memory_order_acquire) && m_pathingDecision.load(std::memory_order_acquire) > 0) {
            const bool pathingSucceeded = RunPathingStep(*job);
            IPLBakedDataIdentifier pathing{};
            pathing.type = IPL_BAKEDDATATYPE_PATHING;
            pathing.variation = IPL_BAKEDDATAVARIATION_DYNAMIC;
            job->pathingBaked = pathingSucceeded && !m_bakeCancel.load(std::memory_order_acquire) &&
                                iplProbeBatchGetDataSize(job->batch, &pathing) > 0;
            if (job->pathingBaked) {
                SetPhase(BakePhase::Saving);
                iplProbeBatchCommit(job->batch);
                saved = !job->cacheFile.empty() && SaveBatch(job->batch, job->cacheFile);
            } else {
                iplProbeBatchRemoveData(job->batch, &pathing);
                iplProbeBatchCommit(job->batch);
            }
        }
    }
    FinishBake(true, saved, Status().memoryLimited ? "Reflection bake ready; pathing stopped by memory guard" :
                           job->extraStep && !job->pathingBaked ? "Reflection bake ready; pathing skipped" : "");
} catch (const std::exception& error) {
    SA_LOGE("Bake failed: %s", error.what());
    FinishBake(false, false, error.what());
}

bool ReflectionSimulator::PollBake()
{
    if (!m_activeJob || !m_bakeDone.load(std::memory_order_acquire))
        return false;
    if (m_bakeThread.joinable())
        m_bakeThread.join();

    std::shared_ptr<BakeJob> job = std::move(m_activeJob);
    m_activeJob.reset();
    m_bakeDone.store(false, std::memory_order_release);

    if (!job->succeeded) {
        ReleaseBatch(job->batch);
        return false;
    }

    ClearProbes();
    m_probeBatch = job->batch;
    job->batch = nullptr;
    m_probeCount = job->probeCount;
    m_pathingBaked = job->pathingBaked;
    if (m_simulator && m_simulator->IsValid()) {
        m_simulator->AddProbeBatch(m_probeBatch);
        m_batchAttached = true;
    }
    SA_LOGI("Baked probe batch attached (%d probes, pathing=%s)", m_probeCount, m_pathingBaked ? "yes" : "no");
    return true;
}

void ReflectionSimulator::CancelBake()
{
    if (!m_bakeRunning.load(std::memory_order_acquire))
        return;
    {
        std::lock_guard<std::mutex> lock(m_decisionMutex);
        m_bakeCancel.store(true, std::memory_order_release);
    }
    m_decisionWake.notify_all();
    if (m_context && m_context->IsValid()) {
        iplReflectionsBakerCancelBake(m_context->Handle());
        iplPathBakerCancelBake(m_context->Handle());
    }
}

bool ReflectionSimulator::ContinuePathing(bool enabled)
{
    const BakeStatus status = Status();
    if (!BakeInProgress() || status.phase != BakePhase::AwaitingPathing || (enabled && !status.pathingAllowed))
        return false;
    {
        std::lock_guard<std::mutex> lock(m_decisionMutex);
        m_pathingDecision.store(enabled ? 1 : -1, std::memory_order_release);
    }
    m_decisionWake.notify_all();
    return true;
}

void ReflectionSimulator::ClearProbes()
{
    if (m_probeBatch) {
        if (m_batchAttached && m_simulator && m_simulator->IsValid())
            m_simulator->RemoveProbeBatch(m_probeBatch);
        ReleaseBatch(m_probeBatch);
    }
    m_batchAttached = false;
    m_pathingBaked = false;
    m_probeCount = 0;
}

void ReflectionSimulator::ReleaseBatch(IPLProbeBatch& batch)
{
    if (batch)
        iplProbeBatchRelease(&batch);
    batch = nullptr;
}

bool ReflectionSimulator::SaveBatch(IPLProbeBatch batch, const std::string& file) const
{
    if (!m_context || !batch)
        return false;
    IPLSerializedObjectSettings settings{};
    IPLSerializedObject serialized = nullptr;
    IPLerror err = iplSerializedObjectCreate(m_context->Handle(), &settings, &serialized);
    if (err != IPL_STATUS_SUCCESS || !serialized) {
        SA_LOGW("iplSerializedObjectCreate failed: %s", IplErrorToString(err));
        return false;
    }
    iplProbeBatchSave(batch, serialized);
    const IPLsize size = iplSerializedObjectGetSize(serialized);
    const IPLbyte* data = iplSerializedObjectGetData(serialized);
    bool ok = false;
    const std::string temporary = file + ".tmp";
    if (size > 0 && data) {
        FILE* f = std::fopen(temporary.c_str(), "wb");
        if (f) {
            ok = std::fwrite(data, 1, size, f) == size;
            const int closed = std::fclose(f);
            ok = ok && closed == 0;
        }
    }
    iplSerializedObjectRelease(&serialized);
    if (ok) {
#ifdef _WIN32
        ok = MoveFileExW(std::filesystem::u8path(temporary).c_str(), std::filesystem::u8path(file).c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
        ok = std::rename(temporary.c_str(), file.c_str()) == 0;
#endif
    }
    if (!ok)
        std::remove(temporary.c_str());
    if (ok)
        SA_LOGI("Saved probe batch to %s (%zu bytes)", file.c_str(), static_cast<size_t>(size));
    else
        SA_LOGW("Could not save probe batch to %s", file.c_str());
    return ok;
}

bool ReflectionSimulator::LoadCachedBake(const std::string& cacheFile)
{
    if (!m_context || !m_simulator || cacheFile.empty())
        return false;
    FILE* f = std::fopen(cacheFile.c_str(), "rb");
    if (!f)
        return false;
    std::vector<IPLbyte> bytes;
    std::fseek(f, 0, SEEK_END);
    const long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (len > 0 && static_cast<uint64_t>(len) <= (uint64_t(256) << 20)) {
        bytes.resize(static_cast<size_t>(len));
        if (std::fread(bytes.data(), 1, bytes.size(), f) != bytes.size())
            bytes.clear();
    }
    std::fclose(f);
    if (bytes.empty())
        return false;

    IPLSerializedObjectSettings settings{};
    settings.data = bytes.data();
    settings.size = bytes.size();
    IPLSerializedObject serialized = nullptr;
    IPLerror err = iplSerializedObjectCreate(m_context->Handle(), &settings, &serialized);
    if (err != IPL_STATUS_SUCCESS || !serialized)
        return false;

    IPLProbeBatch batch = nullptr;
    err = iplProbeBatchLoad(m_context->Handle(), serialized, &batch);
    iplSerializedObjectRelease(&serialized);
    if (err != IPL_STATUS_SUCCESS || !batch) {
        SA_LOGW("iplProbeBatchLoad(%s) failed: %s", cacheFile.c_str(), IplErrorToString(err));
        return false;
    }
    iplProbeBatchCommit(batch);
    IPLBakedDataIdentifier reverb = m_reverbIdentifier;
    if (iplProbeBatchGetNumProbes(batch) <= 0 || iplProbeBatchGetDataSize(batch, &reverb) == 0) {
        ReleaseBatch(batch);
        return false;
    }

    ClearProbes();
    m_probeBatch = batch;
    m_probeCount = iplProbeBatchGetNumProbes(batch);
    // A cached batch produced by this module always contains the pathing layer
    // when pathing was enabled at bake time; the simulator ignores it otherwise.
    IPLBakedDataIdentifier pathing{};
    pathing.type = IPL_BAKEDDATATYPE_PATHING;
    pathing.variation = IPL_BAKEDDATAVARIATION_DYNAMIC;
    m_pathingBaked = iplProbeBatchGetDataSize(m_probeBatch, &pathing) > 0;
    m_simulator->AddProbeBatch(m_probeBatch);
    m_batchAttached = true;
    {
        std::lock_guard<std::mutex> lock(m_progressMutex);
        m_progressState = BakeStatus{};
        m_progressState.id = ++m_nextBakeId;
        m_progressState.phase = BakePhase::Ready;
        m_progressState.progress = 1.f;
        m_progressState.probes = m_progressState.completedProbes = m_probeCount;
        m_progressState.startedAt = m_progressState.finishedAt = m_progressState.updatedAt = BakeTime();
        m_progressState.cacheSaved = true;
        m_progressState.sceneType = m_devices.sceneType;
        std::snprintf(m_progressState.detail, sizeof(m_progressState.detail), "%s", "Loaded from cache");
        m_status.Store(m_progressState);
    }
    SA_LOGI("Loaded cached probe batch %s (%d probes)", cacheFile.c_str(), m_probeCount);
    return true;
}

} // namespace sa
