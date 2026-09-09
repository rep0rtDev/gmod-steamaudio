// src/mixing/SimulationThread.cpp
#include "mixing/SimulationThread.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "steamaudio/PhononContext.h"
#include "util/Logging.h"
#include "util/Math.h"

namespace sa {

namespace {

// Steam Audio's default exponential air absorption coefficients (per band);
// scaled by the per-source / global air absorption factor.
constexpr float kDefaultAirAbsorption[IPL_NUM_BANDS] = {0.0002f, 0.0017f, 0.0182f};

// Idle engine slots keep their IPLSource this many ticks before it is retired.
constexpr uint64_t kIdleTicksBeforeRetire = 20;

uint32_t MicrosSince(std::chrono::steady_clock::time_point start)
{
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start);
    return static_cast<uint32_t>(std::min<int64_t>(us.count(), UINT32_MAX));
}

bool HasDynamicChanged(const Vec3& a, const Vec3& b)
{
    return (a - b).LengthSq() > 1e-6f;
}

} // namespace

SimulationThread::SimulationThread() = default;

SimulationThread::~SimulationThread()
{
    Stop();
}

bool SimulationThread::Start(const SimulationThreadSetup& setup)
{
    Stop();
    if (!setup.context || !setup.simulator || !setup.simulator->IsValid()) {
        SA_LOGE("[sim] invalid thread setup");
        return false;
    }
    m_setup = setup;
    m_setup.simulator->SetAudioFrameCounter(setup.audioFrameCounter);
    if (m_setup.scene && m_setup.scene->IsValid())
        m_setup.simulator->SetScene(m_setup.scene->Scene());
    m_streams.clear();
    m_streams.reserve(setup.maxStreamSources);
    m_slotGeneration.fill(UINT32_MAX);
    m_dynamic.clear();
    m_mapName.clear();
    m_mapGeometry.reset();
    m_sceneDirty = true;
    m_bakeRequested = false;
    m_cancelBakeRequested.store(false, std::memory_order_relaxed);
    m_bakeForce = false;
    m_bakeAttempted = false;
    m_tick = 0;
    m_lastReflectionsTime = std::chrono::steady_clock::time_point{};
    m_lastPathingTime = std::chrono::steady_clock::time_point{};
    m_shared = IPLSimulationSharedInputs{};
    m_cfg = m_runtime.Load();
    m_stats.lastTickMicros.store(0, std::memory_order_relaxed);
    m_stats.maxTickMicros.store(0, std::memory_order_relaxed);
    m_stats.lastCommandMicros.store(0, std::memory_order_relaxed);
    m_stats.maxCommandMicros.store(0, std::memory_order_relaxed);
    m_stats.lastSceneCommitMicros.store(0, std::memory_order_relaxed);
    m_stats.maxSceneCommitMicros.store(0, std::memory_order_relaxed);

    m_running.store(true, std::memory_order_release);
    m_thread = std::thread([this] { Run(); });
    SA_LOGI("[sim] thread started (flags=0x%x, maxSources=%d)", static_cast<unsigned>(m_setup.simulator->Flags()),
            m_setup.simulator->Settings().maxNumSources);
    return true;
}

void SimulationThread::Stop()
{
    if (m_running.exchange(false, std::memory_order_acq_rel)) {
        {
            std::lock_guard<std::mutex> lock(m_wakeMutex);
        }
        m_wake.notify_all();
    }
    if (m_thread.joinable())
        m_thread.join();
}

// ---------------------------------------------------------------------------
// Game-thread API
// ---------------------------------------------------------------------------
bool SimulationThread::AddStreamSource(const SoundSourcePtr& source)
{
    if (!source)
        return false;
    Command cmd;
    cmd.type = Command::Type::AddSource;
    cmd.source = source;
    cmd.id = source->Id();
    if (!m_commands.TryPush(std::move(cmd)))
        return false;
    m_pendingCommands.fetch_add(1, std::memory_order_release);
    m_wake.notify_one();
    return true;
}

bool SimulationThread::RemoveStreamSource(uint32_t id)
{
    Command cmd;
    cmd.type = Command::Type::RemoveSource;
    cmd.id = id;
    if (!m_commands.TryPush(std::move(cmd)))
        return false;
    m_pendingCommands.fetch_add(1, std::memory_order_release);
    m_wake.notify_one();
    return true;
}

bool SimulationThread::SetMapGeometry(std::shared_ptr<const BspGeometry> geometry, const std::string& mapName)
{
    Command cmd;
    cmd.type = Command::Type::SetMap;
    cmd.geometry = std::move(geometry);
    cmd.name = mapName;
    if (!m_commands.TryPush(std::move(cmd)))
        return false;
    m_pendingCommands.fetch_add(1, std::memory_order_release);
    m_wake.notify_one();
    return true;
}

bool SimulationThread::AddStaticGeometry(std::shared_ptr<const MeshData> mesh, const std::string& debugName)
{
    if (!mesh || mesh->Empty())
        return false;
    Command cmd;
    cmd.type = Command::Type::AddStatic;
    cmd.mesh = std::move(mesh);
    cmd.name = debugName;
    if (!m_commands.TryPush(std::move(cmd)))
        return false;
    m_pendingCommands.fetch_add(1, std::memory_order_release);
    m_wake.notify_one();
    return true;
}

DynamicGeometryId SimulationThread::AllocDynamicId()
{
    return m_nextDynamicId.fetch_add(1, std::memory_order_relaxed);
}

bool SimulationThread::AddDynamicGeometry(DynamicGeometryId id, std::shared_ptr<const MeshData> localMesh,
                                          const Transform& transform, const std::string& debugName)
{
    if (!localMesh || localMesh->Empty() || id == 0)
        return false;
    Command cmd;
    cmd.type = Command::Type::AddDynamic;
    cmd.id = id;
    cmd.mesh = std::move(localMesh);
    cmd.transform = transform;
    cmd.name = debugName;
    if (!m_commands.TryPush(std::move(cmd)))
        return false;
    m_pendingCommands.fetch_add(1, std::memory_order_release);
    m_wake.notify_one();
    return true;
}

bool SimulationThread::UpdateDynamicGeometry(DynamicGeometryId id, const Transform& transform)
{
    Command cmd;
    cmd.type = Command::Type::UpdateDynamic;
    cmd.id = id;
    cmd.transform = transform;
    if (!m_commands.TryPush(std::move(cmd)))
        return false;
    m_pendingCommands.fetch_add(1, std::memory_order_release);
    return true; // transform updates are frequent; the periodic tick picks them up
}

bool SimulationThread::UpdateBrushModel(int32_t modelIndex, const Vec3& origin, const Vec3& angles)
{
    if (modelIndex <= 0)
        return false;
    Command cmd;
    cmd.type = Command::Type::UpdateDynamic;
    cmd.id = static_cast<DynamicGeometryId>(modelIndex);
    cmd.modelIndex = modelIndex;
    cmd.transform = Transform::FromAngles(origin, angles);
    if (!m_commands.TryPush(std::move(cmd)))
        return false;
    m_pendingCommands.fetch_add(1, std::memory_order_release);
    return true;
}

bool SimulationThread::RemoveDynamicGeometry(DynamicGeometryId id)
{
    Command cmd;
    cmd.type = Command::Type::RemoveDynamic;
    cmd.id = id;
    if (!m_commands.TryPush(std::move(cmd)))
        return false;
    m_pendingCommands.fetch_add(1, std::memory_order_release);
    m_wake.notify_one();
    return true;
}

bool SimulationThread::ClearGeometry()
{
    Command cmd;
    cmd.type = Command::Type::ClearGeometry;
    if (!m_commands.TryPush(std::move(cmd)))
        return false;
    m_pendingCommands.fetch_add(1, std::memory_order_release);
    m_wake.notify_one();
    return true;
}

bool SimulationThread::RequestBake(bool forceRebake)
{
    Command cmd;
    cmd.type = Command::Type::Bake;
    cmd.flag = forceRebake;
    if (!m_commands.TryPush(std::move(cmd)))
        return false;
    m_pendingCommands.fetch_add(1, std::memory_order_release);
    m_wake.notify_one();
    return true;
}

bool SimulationThread::CancelBake()
{
    if (!Running())
        return false;
    m_cancelBakeRequested.store(true, std::memory_order_release);
    if (m_setup.reflections)
        m_setup.reflections->CancelBake();
    m_wake.notify_one();
    return true;
}

// ---------------------------------------------------------------------------
// Simulation thread
// ---------------------------------------------------------------------------
void SimulationThread::Run()
{
    SetCurrentThreadName("sa-simulation");
    auto nextTick = std::chrono::steady_clock::now();
    uint64_t sourceRevision = 0;
    while (m_running.load(std::memory_order_acquire)) {
        const auto tickStart = std::chrono::steady_clock::now();
        const uint64_t revision = m_setup.capture ? m_setup.capture->SourceRevision() : 0;
        const bool urgent = revision != sourceRevision || m_pendingCommands.load(std::memory_order_acquire) != 0 ||
                            m_cancelBakeRequested.load(std::memory_order_acquire);
        if (tickStart >= nextTick || urgent) {
            sourceRevision = revision;
            Tick();
            const uint32_t elapsed = MicrosSince(tickStart);
            m_stats.lastTickMicros.store(elapsed, std::memory_order_relaxed);
            uint32_t prevMax = m_stats.maxTickMicros.load(std::memory_order_relaxed);
            while (elapsed > prevMax &&
                   !m_stats.maxTickMicros.compare_exchange_weak(prevMax, elapsed, std::memory_order_relaxed)) {
            }
            nextTick = tickStart + std::chrono::microseconds(
                static_cast<int64_t>(std::max(5.f, m_cfg.simulationIntervalMs) * 1000.f));
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(
            nextTick - std::chrono::steady_clock::now());
        const auto sleepFor = remaining.count() <= 0 ? std::chrono::microseconds(1000)
                              : m_setup.capture ? std::min(remaining, std::chrono::microseconds(5000)) : remaining;
        std::unique_lock<std::mutex> lock(m_wakeMutex);
        m_wake.wait_for(lock, sleepFor, [this, sourceRevision] {
            return !m_running.load(std::memory_order_acquire) ||
                   m_pendingCommands.load(std::memory_order_acquire) != 0 ||
                   m_cancelBakeRequested.load(std::memory_order_acquire) ||
                   (m_setup.capture && m_setup.capture->SourceRevision() != sourceRevision);
        });
    }

    // Teardown on the simulation thread so no Steam Audio call races a Run*().
    if (m_setup.reflections) {
        m_setup.reflections->CancelBake();
        while (m_setup.reflections->BakeInProgress())
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        m_setup.reflections->PollBake();
    }
    DrainCommands();
    DestroyAllSources(true);
    for (TrackedSource& t : m_streams) {
        if (t.source)
            m_released.TryPush(std::move(t.source));
    }
    m_streams.clear();
    if (m_setup.scene) {
        m_setup.scene->ClearDynamic();
        m_setup.scene->Commit();
    }
    m_dynamic.clear();
}

void SimulationThread::Tick()
{
    ++m_tick;
    m_stats.ticks.fetch_add(1, std::memory_order_relaxed);
    m_cfg = m_runtime.Load();
    const auto idleCutoff = std::chrono::steady_clock::now() - std::chrono::microseconds(
        static_cast<int64_t>(kIdleTicksBeforeRetire * std::max(5.f, m_cfg.simulationIntervalMs) * 1000.f));
    const auto commandsStart = std::chrono::steady_clock::now();
    DrainCommands();
    const uint32_t commandsMicros = MicrosSince(commandsStart);
    m_stats.lastCommandMicros.store(commandsMicros, std::memory_order_relaxed);
    m_stats.maxCommandMicros.store(std::max(commandsMicros, m_stats.maxCommandMicros.load(std::memory_order_relaxed)),
                                    std::memory_order_relaxed);
    if (m_cancelBakeRequested.exchange(false, std::memory_order_acq_rel)) {
        m_bakeRequested = false;
        if (m_setup.reflections)
            m_setup.reflections->CancelBake();
    }

    Simulator& sim = *m_setup.simulator;

    // 1. Baked data.
    if (m_setup.reflections) {
        if (m_setup.reflections->PollBake())
            m_stats.bakedDataAttached.store(m_setup.reflections->HasBakedData(), std::memory_order_relaxed);
        m_stats.bakeRunning.store(m_setup.reflections->BakeInProgress(), std::memory_order_relaxed);
        m_stats.bakeProgress.store(m_setup.reflections->BakeProgress(), std::memory_order_relaxed);
    }

    // 2. Geometry.
    const bool geometryChanged = m_sceneDirty && m_setup.scene && m_setup.scene->IsValid() &&
                                 m_setup.scene->HasPendingChanges();
    m_sceneDirty = false;
    if (geometryChanged) {
        const auto commitStart = std::chrono::steady_clock::now();
        m_setup.scene->Commit();
        const uint32_t commitMicros = MicrosSince(commitStart);
        m_stats.lastSceneCommitMicros.store(commitMicros, std::memory_order_relaxed);
        m_stats.maxSceneCommitMicros.store(
            std::max(commitMicros, m_stats.maxSceneCommitMicros.load(std::memory_order_relaxed)),
            std::memory_order_relaxed);
        m_stats.sceneCommits.fetch_add(1, std::memory_order_relaxed);
        m_stats.staticTriangles.store(static_cast<uint32_t>(m_setup.scene->StaticTriangleCount()),
                                      std::memory_order_relaxed);
        m_stats.dynamicMeshes.store(static_cast<uint32_t>(m_setup.scene->DynamicCount()), std::memory_order_relaxed);
    }
    if (m_bakeRequested && m_setup.reflections && m_setup.scene && m_setup.scene->StaticTriangleCount() > 0 &&
        !m_setup.reflections->BakeInProgress()) {
        StartBakeForCurrentMap(m_bakeForce);
        m_bakeRequested = false;
    }

    // 3. Listener.
    const ListenerState listener = CurrentListener();
    m_stats.listenerValid.store(listener.valid, std::memory_order_relaxed);
    if (!m_cfg.enabled || !listener.valid) {
        // Keep handles alive (sounds continue through the fallback gains) but
        // retire slots that were released by the engine.
        if (m_setup.capture) {
            for (uint32_t i = 0; i < ChannelCapture::kSlots; ++i) {
                CaptureSlot& slot = m_setup.capture->Slot(i);
                if (slot.source && slot.enginePtr.load(std::memory_order_acquire) == 0 && slot.source->Sim().source &&
                    slot.source->Sim().lastActiveTime < idleCutoff)
                    RetireSimulationSource(*slot.source);
            }
        }
        sim.ProcessDeferredReleases(false);
        PublishStats();
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const bool reflectionsCapable = sim.SupportsReflections() && m_cfg.reflections && m_setup.reflections;
    const bool pathingCapable = sim.SupportsPathing() && m_cfg.pathing && m_setup.pathing &&
                                m_setup.pathing->Enabled() && m_setup.reflections &&
                                m_setup.reflections->PathingBaked();
    const auto reflInterval = std::chrono::microseconds(
        static_cast<int64_t>(std::max(20.f, m_cfg.reflectionsIntervalMs) * 1000.f));
    const auto pathInterval = std::chrono::microseconds(
        static_cast<int64_t>(std::max(20.f, m_cfg.pathingIntervalMs) * 1000.f));
    const bool refreshGeometry = m_cfg.physicalAcoustics && geometryChanged;
    bool runReflections = reflectionsCapable && ((now - m_lastReflectionsTime) >= reflInterval || refreshGeometry);
    bool runPathing = pathingCapable && ((now - m_lastPathingTime) >= pathInterval || refreshGeometry);

    // 4. Shared inputs.
    m_shared.listener = listener.frame;
    IPLSimulationFlags sharedFlags = IPL_SIMULATIONFLAGS_DIRECT;
    if (reflectionsCapable) {
        m_setup.reflections->FillSharedInputs(m_cfg, m_shared);
        sharedFlags = static_cast<IPLSimulationFlags>(sharedFlags | IPL_SIMULATIONFLAGS_REFLECTIONS);
    }
    if (pathingCapable) {
        m_setup.pathing->FillSharedInputs(m_cfg, m_shared);
        sharedFlags = static_cast<IPLSimulationFlags>(sharedFlags | IPL_SIMULATIONFLAGS_PATHING);
    }
    sim.SetSharedInputs(sharedFlags, m_shared);

    // 5. Per-source inputs.
    uint32_t simulated = 0;
    std::vector<SoundSource*> simulatedSources;
    simulatedSources.reserve(static_cast<size_t>(sim.Settings().maxNumSources));

    if (m_setup.capture) {
        for (uint32_t i = 0; i < ChannelCapture::kSlots; ++i) {
            CaptureSlot& slot = m_setup.capture->Slot(i);
            if (!slot.source)
                continue;
            SoundSource& source = *slot.source;
            const bool bound = slot.enginePtr.load(std::memory_order_acquire) != 0;
            const uint32_t generation = slot.generation.load(std::memory_order_acquire);
            if (!bound) {
                if (source.Sim().source && source.Sim().lastActiveTime < idleCutoff)
                    RetireSimulationSource(source);
                continue;
            }
            if (!slot.everMixed.load(std::memory_order_acquire))
                continue;
            const SourceParams params = source.GetParams();
            if (!params.spatialize || !params.positionValid) {
                if (source.Sim().source)
                    RetireSimulationSource(source);
                continue;
            }
            if (source.Sim().source && source.Sim().generation != generation)
                RetireSimulationSource(source);
            if (!EnsureSimulationSource(source, generation))
                continue;
            runReflections = runReflections || (reflectionsCapable && params.reflections &&
                                               !(source.Sim().flags & IPL_SIMULATIONFLAGS_REFLECTIONS));
            runPathing = runPathing || (pathingCapable && params.pathing &&
                                       !(source.Sim().flags & IPL_SIMULATIONFLAGS_PATHING));
            if (UpdateSourceInputs(source, params, listener, runReflections, runPathing)) {
                ++simulated;
                simulatedSources.push_back(&source);
            }
        }
    }

    for (TrackedSource& t : m_streams) {
        SoundSource& source = *t.source;
        const SourceParams params = source.GetParams();
        const bool finished = source.RenderFinished();
        if (finished || !params.spatialize || !params.positionValid) {
            if (source.Sim().source)
                RetireSimulationSource(source);
            continue;
        }
        if (!EnsureSimulationSource(source, 0))
            continue;
        runReflections = runReflections || (reflectionsCapable && params.reflections &&
                                           !(source.Sim().flags & IPL_SIMULATIONFLAGS_REFLECTIONS));
        runPathing = runPathing || (pathingCapable && params.pathing &&
                                   !(source.Sim().flags & IPL_SIMULATIONFLAGS_PATHING));
        if (UpdateSourceInputs(source, params, listener, runReflections, runPathing)) {
            ++simulated;
            simulatedSources.push_back(&source);
        }
    }

    // Sources created this tick must be committed before Run*().
    sim.Commit();

    // 6. Run.
    {
        const auto t0 = std::chrono::steady_clock::now();
        sim.RunDirect();
        m_stats.lastDirectMicros.store(MicrosSince(t0), std::memory_order_relaxed);
        m_stats.directRuns.fetch_add(1, std::memory_order_relaxed);
    }
    for (SoundSource* source : simulatedSources) {
        auto& state = source->Sim();
        state.flags = static_cast<IPLSimulationFlags>((state.flags | IPL_SIMULATIONFLAGS_DIRECT) & sim.Flags());
        state.everSimulated = true;
        source->MarkSimulationReady(state.flags);
    }
    if (runReflections) {
        const auto t0 = std::chrono::steady_clock::now();
        sim.RunReflections();
        m_lastReflectionsTime = now;
        m_stats.lastReflectionMicros.store(MicrosSince(t0), std::memory_order_relaxed);
        m_stats.reflectionRuns.fetch_add(1, std::memory_order_relaxed);
    }
    if (runPathing) {
        const auto t0 = std::chrono::steady_clock::now();
        sim.RunPathing();
        m_lastPathingTime = now;
        m_stats.lastPathingMicros.store(MicrosSince(t0), std::memory_order_relaxed);
        m_stats.pathingRuns.fetch_add(1, std::memory_order_relaxed);
    }

    // 7. Publish readiness: the audio thread reads outputs only for flags that
    //    have produced at least one result for this handle.
    for (SoundSource* source : simulatedSources) {
        SoundSource::SimulationState& s = source->Sim();
        IPLSimulationFlags ready = static_cast<IPLSimulationFlags>(s.flags | IPL_SIMULATIONFLAGS_DIRECT);
        if (runReflections && reflectionsCapable && source->GetParams().reflections) {
            ready = static_cast<IPLSimulationFlags>(ready | IPL_SIMULATIONFLAGS_REFLECTIONS);
            s.lastReflectionsRun = m_tick;
        }
        if (runPathing && pathingCapable && source->GetParams().pathing) {
            ready = static_cast<IPLSimulationFlags>(ready | IPL_SIMULATIONFLAGS_PATHING);
            s.lastPathingRun = m_tick;
        }
        s.flags = static_cast<IPLSimulationFlags>(ready & sim.Flags());
        s.everSimulated = true;
        source->MarkSimulationReady(s.flags);
    }

    sim.ProcessDeferredReleases(false);
    m_stats.simulatedSources.store(simulated, std::memory_order_relaxed);
    PublishStats();
}

void SimulationThread::PublishStats()
{
    if (m_setup.scene) {
        m_stats.staticTriangles.store(static_cast<uint32_t>(m_setup.scene->StaticTriangleCount()),
                                      std::memory_order_relaxed);
        m_stats.dynamicMeshes.store(static_cast<uint32_t>(m_setup.scene->DynamicCount()), std::memory_order_relaxed);
    }
}

ListenerState SimulationThread::CurrentListener() const
{
    if (m_setup.hooks) {
        const EngineListener l = m_setup.hooks->GetListener();
        if (l.valid) {
            ListenerState state;
            CoordinateConverter converter;
            converter.SetUnitsPerMeter(m_cfg.unitsPerMeter);
            state.frame = converter.FrameToSA(l.position, l.forward, l.up);
            state.valid = true;
            return state;
        }
    }
    return m_externalListener.Load();
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------
void SimulationThread::DrainCommands()
{
    Command cmd;
    while (m_commands.TryPop(cmd)) {
        m_pendingCommands.fetch_sub(1, std::memory_order_acq_rel);
        HandleCommand(cmd);
        cmd = Command{};
    }
}

void SimulationThread::HandleCommand(Command& cmd)
{
    switch (cmd.type) {
    case Command::Type::AddSource: {
        if (!cmd.source)
            break;
        for (const TrackedSource& t : m_streams) {
            if (t.source && t.source->Id() == cmd.id)
                return;
        }
        if (m_streams.size() >= m_setup.maxStreamSources) {
            SA_LOGW("[sim] stream source limit (%zu) reached; %u not simulated", m_setup.maxStreamSources, cmd.id);
            m_released.TryPush(std::move(cmd.source));
            break;
        }
        TrackedSource t;
        t.source = std::move(cmd.source);
        m_streams.push_back(std::move(t));
        break;
    }
    case Command::Type::RemoveSource: {
        for (size_t i = 0; i < m_streams.size(); ++i) {
            if (m_streams[i].source && m_streams[i].source->Id() == cmd.id) {
                RetireSimulationSource(*m_streams[i].source);
                if (!m_released.TryPush(std::move(m_streams[i].source)))
                    SA_LOGW("[sim] release queue full; source %u freed on simulation thread", cmd.id);
                m_streams.erase(m_streams.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
        }
        break;
    }
    case Command::Type::SetMap: {
        if (!m_setup.scene || !m_setup.scene->IsValid()) {
            m_mapName = cmd.name;
            break;
        }
        if (m_setup.reflections) {
            m_setup.reflections->CancelBake();
            while (m_setup.reflections->BakeInProgress())
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            m_setup.reflections->PollBake();
            m_setup.reflections->ClearProbes();
            m_stats.bakedDataAttached.store(false, std::memory_order_relaxed);
        }
        m_setup.scene->ClearDynamic();
        m_setup.scene->ClearStatic();
        m_dynamic.clear();
        m_mapName = cmd.name;
        m_mapGeometry = cmd.geometry;
        m_bakeAttempted = false;
        if (cmd.geometry)
            ApplyMapGeometry(*cmd.geometry);
        m_sceneDirty = true;
        if (m_setup.reflections && m_cfg.useBakedReverb) {
            const std::string cache = CacheFileForMap(m_mapName);
            const bool loaded = m_setup.reflections->LoadCachedBake(cache);
            if (!loaded) {
                m_setup.reflections->ClearProbes();
                if (m_cfg.bakeOnMapLoad) {
                    m_bakeRequested = true;
                    m_bakeForce = false;
                }
            } else {
                m_stats.bakedDataAttached.store(m_setup.reflections->HasBakedData(), std::memory_order_relaxed);
                SA_LOGI("[sim] loaded baked probes for %s from %s", m_mapName.c_str(), cache.c_str());
            }
        }
        break;
    }
    case Command::Type::AddStatic: {
        if (!m_setup.scene || !m_setup.scene->IsValid() || !cmd.mesh)
            break;
        if (m_setup.scene->AddStaticMesh(*cmd.mesh, cmd.name.c_str()))
            m_sceneDirty = true;
        break;
    }
    case Command::Type::AddDynamic: {
        if (!m_setup.scene || !m_setup.scene->IsValid() || !cmd.mesh)
            break;
        auto it = m_dynamic.find(cmd.id);
        if (it != m_dynamic.end()) {
            m_setup.scene->RemoveDynamic(it->second.sceneId);
            m_dynamic.erase(it);
        }
        CoordinateConverter converter;
        converter.SetUnitsPerMeter(m_cfg.unitsPerMeter);
        DynamicInstance inst;
        inst.sceneId = m_setup.scene->AddDynamic(*cmd.mesh, converter.TransformToSA(cmd.transform), cmd.name.c_str());
        inst.lastOrigin = cmd.transform.origin;
        inst.isBrushModel = false;
        if (inst.sceneId != SceneBuilder::kInvalidDynamic) {
            m_dynamic.emplace(cmd.id, inst);
            m_sceneDirty = true;
        }
        break;
    }
    case Command::Type::UpdateDynamic: {
        auto it = m_dynamic.find(cmd.id);
        if (it == m_dynamic.end() || !m_setup.scene)
            break;
        DynamicInstance& inst = it->second;
        const Vec3 anglesProxy = cmd.transform.forward + cmd.transform.up * 3.f; // cheap orientation fingerprint
        if (!HasDynamicChanged(inst.lastOrigin, cmd.transform.origin) && !HasDynamicChanged(inst.lastAngles, anglesProxy))
            break;
        CoordinateConverter converter;
        converter.SetUnitsPerMeter(m_cfg.unitsPerMeter);
        m_setup.scene->UpdateDynamic(inst.sceneId, converter.TransformToSA(cmd.transform));
        inst.lastOrigin = cmd.transform.origin;
        inst.lastAngles = anglesProxy;
        m_sceneDirty = true;
        break;
    }
    case Command::Type::RemoveDynamic: {
        auto it = m_dynamic.find(cmd.id);
        if (it == m_dynamic.end() || !m_setup.scene)
            break;
        m_setup.scene->RemoveDynamic(it->second.sceneId);
        m_dynamic.erase(it);
        m_sceneDirty = true;
        break;
    }
    case Command::Type::ClearGeometry: {
        if (m_setup.reflections) {
            m_setup.reflections->CancelBake();
            while (m_setup.reflections->BakeInProgress())
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            m_setup.reflections->PollBake();
            m_setup.reflections->ClearProbes();
            m_stats.bakedDataAttached.store(false, std::memory_order_relaxed);
        }
        if (m_setup.scene) {
            m_setup.scene->ClearDynamic();
            m_setup.scene->ClearStatic();
        }
        m_dynamic.clear();
        m_mapName.clear();
        m_mapGeometry.reset();
        m_bakeRequested = false;
        m_sceneDirty = true;
        break;
    }
    case Command::Type::Bake:
        m_bakeRequested = true;
        m_bakeForce = cmd.flag;
        break;
    case Command::Type::CancelBake:
        m_bakeRequested = false;
        if (m_setup.reflections)
            m_setup.reflections->CancelBake();
        break;
    }
}

void SimulationThread::ApplyMapGeometry(const BspGeometry& geometry)
{
    if (!geometry.world.Empty()) {
        if (m_setup.scene->AddStaticMesh(geometry.world, geometry.mapName.c_str()))
            SA_LOGI("[sim] world geometry: %zu triangles, %zu vertices", geometry.world.triangles.size(),
                    geometry.world.vertices.size());
    }
    if (!geometry.staticPropMesh.Empty()) {
        if (m_setup.scene->AddStaticMesh(geometry.staticPropMesh, "static_props"))
            SA_LOGI("[sim] static props: %zu instanced, %zu triangles (%zu models missing)",
                    geometry.staticPropsInstanced, geometry.staticPropMesh.triangles.size(),
                    geometry.staticPropsMissing);
    }
    if (!m_cfg.dynamicGeometry) {
        // Bake brush entities into the static scene at their spawn transform.
        CoordinateConverter converter;
        converter.SetUnitsPerMeter(m_cfg.unitsPerMeter);
        for (const BspBrushModel& bm : geometry.brushModels) {
            if (!bm.solid || bm.mesh.Empty())
                continue;
            MeshData world = bm.mesh;
            const Vec3 offset = converter.PositionToSA(bm.spawnOrigin);
            for (IPLVector3& v : world.vertices) {
                v.x += offset.x;
                v.y += offset.y;
                v.z += offset.z;
            }
            m_setup.scene->AddStaticMesh(world, bm.classname.c_str());
        }
        return;
    }
    CoordinateConverter converter;
    converter.SetUnitsPerMeter(m_cfg.unitsPerMeter);
    size_t added = 0;
    for (const BspBrushModel& bm : geometry.brushModels) {
        if (!bm.solid || bm.mesh.Empty() || bm.modelIndex <= 0)
            continue;
        const Transform t = Transform::FromAngles(bm.spawnOrigin, Vec3{});
        DynamicInstance inst;
        inst.sceneId = m_setup.scene->AddDynamic(bm.mesh, converter.TransformToSA(t), bm.classname.c_str());
        inst.lastOrigin = bm.spawnOrigin;
        inst.lastAngles = t.forward + t.up * 3.f;
        inst.isBrushModel = true;
        if (inst.sceneId != SceneBuilder::kInvalidDynamic) {
            m_dynamic[static_cast<DynamicGeometryId>(bm.modelIndex)] = inst;
            ++added;
        }
    }
    SA_LOGI("[sim] %zu brush models registered as dynamic geometry", added);
}

// ---------------------------------------------------------------------------
// Sources
// ---------------------------------------------------------------------------
float IPLCALL SimulationThread::DistanceAttenuationCallback(IPLfloat32 distance, void* userData)
{
    const auto* model = static_cast<const SoundSource::SimulationState::DistanceModel*>(userData);
    if (!model)
        return 1.f;
    return SourceDistanceGain(model->distMult, distance * model->unitsPerMeter, model->gainMin, model->gainMax);
}

bool SimulationThread::EnsureSimulationSource(SoundSource& source, uint32_t generation)
{
    SoundSource::SimulationState& s = source.Sim();
    if (s.source) {
        s.lastActive = m_tick;
        s.lastActiveTime = std::chrono::steady_clock::now();
        return true;
    }
    Simulator& sim = *m_setup.simulator;
    if (sim.SourceCount() >= static_cast<size_t>(sim.Settings().maxNumSources))
        EvictIdleSources(1);
    if (sim.SourceCount() >= static_cast<size_t>(sim.Settings().maxNumSources)) {
        m_stats.sourceBudgetRejects.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!sim.CreateSource(source))
        return false;
    s.generation = generation;
    s.lastActive = m_tick;
    s.lastActiveTime = std::chrono::steady_clock::now();
    s.flags = static_cast<IPLSimulationFlags>(0);
    s.everSimulated = false;
    s.lastReflectionsRun = 0;
    s.lastPathingRun = 0;
    return true;
}

void SimulationThread::RetireSimulationSource(SoundSource& source)
{
    if (!source.Sim().source)
        return;
    m_setup.simulator->DestroySource(source);
    source.Sim().flags = static_cast<IPLSimulationFlags>(0);
}

void SimulationThread::EvictIdleSources(size_t needed)
{
    // Retire the least recently active engine-slot sources that are currently unbound.
    size_t freed = 0;
    if (m_setup.capture) {
        std::vector<SoundSource*> candidates;
        for (uint32_t i = 0; i < ChannelCapture::kSlots; ++i) {
            CaptureSlot& slot = m_setup.capture->Slot(i);
            if (slot.source && slot.source->Sim().source && slot.enginePtr.load(std::memory_order_acquire) == 0)
                candidates.push_back(slot.source.get());
        }
        std::sort(candidates.begin(), candidates.end(),
                  [](SoundSource* a, SoundSource* b) { return a->Sim().lastActive < b->Sim().lastActive; });
        for (SoundSource* c : candidates) {
            if (freed >= needed)
                break;
            RetireSimulationSource(*c);
            ++freed;
        }
    }
    if (freed < needed) {
        for (TrackedSource& t : m_streams) {
            if (freed >= needed)
                break;
            if (t.source && t.source->Sim().source && t.source->RenderFinished()) {
                RetireSimulationSource(*t.source);
                ++freed;
            }
        }
    }
}

bool SimulationThread::UpdateSourceInputs(SoundSource& source, const SourceParams& params, const ListenerState& listener,
                                          bool runReflections, bool runPathing)
{
    SoundSource::SimulationState& s = source.Sim();
    if (!s.source)
        return false;
    const Simulator& sim = *m_setup.simulator;
    CoordinateConverter converter;
    converter.SetUnitsPerMeter(m_cfg.unitsPerMeter);

    IPLSimulationInputs in{};
    IPLSimulationFlags flags = IPL_SIMULATIONFLAGS_DIRECT;
    const bool wantReflections = (runReflections || (s.flags & IPL_SIMULATIONFLAGS_REFLECTIONS)) &&
                                 sim.SupportsReflections() && m_cfg.reflections && params.reflections;
    const bool wantPathing = (runPathing || (s.flags & IPL_SIMULATIONFLAGS_PATHING)) && sim.SupportsPathing() &&
                             m_cfg.pathing && params.pathing && m_setup.pathing && m_setup.reflections &&
                             m_setup.reflections->PathingBaked();
    if (wantReflections)
        flags = static_cast<IPLSimulationFlags>(flags | IPL_SIMULATIONFLAGS_REFLECTIONS);
    if (wantPathing)
        flags = static_cast<IPLSimulationFlags>(flags | IPL_SIMULATIONFLAGS_PATHING);
    in.flags = flags;

    // Direct.
    IPLDirectSimulationFlags direct = static_cast<IPLDirectSimulationFlags>(
        IPL_DIRECTSIMULATIONFLAGS_DISTANCEATTENUATION | IPL_DIRECTSIMULATIONFLAGS_AIRABSORPTION);
    if (params.dipoleWeight > 0.f)
        direct = static_cast<IPLDirectSimulationFlags>(direct | IPL_DIRECTSIMULATIONFLAGS_DIRECTIVITY);
    const bool occlusion = m_cfg.occlusion && params.occlusion && sim.Scene() != nullptr;
    if (occlusion) {
        direct = static_cast<IPLDirectSimulationFlags>(direct | IPL_DIRECTSIMULATIONFLAGS_OCCLUSION);
        if (m_cfg.transmission && params.transmission)
            direct = static_cast<IPLDirectSimulationFlags>(direct | IPL_DIRECTSIMULATIONFLAGS_TRANSMISSION);
    }
    in.directFlags = direct;

    const Vec3 up{0.f, 0.f, 1.f};
    Vec3 forward = params.forward;
    if (forward.LengthSq() < 1e-6f)
        forward = Vec3{1.f, 0.f, 0.f};
    in.source = params.listenerRelative ? listener.frame : converter.FrameToSA(params.position, forward, up);

    // Distance attenuation: Source's dist_mult curve (snd_gain_min/max clamps).
    SoundSource::SimulationState::DistanceModel& dm = s.distance;
    const bool distanceDirty = std::fabs(dm.distMult - params.distMult) > 1e-7f ||
                               std::fabs(dm.unitsPerMeter - m_cfg.unitsPerMeter) > 1e-4f ||
                               std::fabs(dm.gainMin - m_cfg.distanceGainMin) > 1e-6f ||
                               std::fabs(dm.gainMax - m_cfg.distanceGainMax) > 1e-6f || !s.everSimulated;
    dm.distMult = params.distMult;
    dm.unitsPerMeter = m_cfg.unitsPerMeter;
    dm.gainMin = m_cfg.distanceGainMin;
    dm.gainMax = m_cfg.distanceGainMax;
    in.distanceAttenuationModel.type = IPL_DISTANCEATTENUATIONTYPE_CALLBACK;
    in.distanceAttenuationModel.minDistance = 1.f;
    in.distanceAttenuationModel.callback = &SimulationThread::DistanceAttenuationCallback;
    in.distanceAttenuationModel.userData = &dm;
    in.distanceAttenuationModel.dirty = distanceDirty ? IPL_TRUE : IPL_FALSE;

    // Air absorption.
    const float airScale = std::max(0.f, params.airAbsorptionScale * m_cfg.airAbsorptionScale);
    if (std::fabs(airScale - 1.f) < 1e-3f) {
        in.airAbsorptionModel.type = IPL_AIRABSORPTIONTYPE_DEFAULT;
    } else {
        in.airAbsorptionModel.type = IPL_AIRABSORPTIONTYPE_EXPONENTIAL;
        for (int b = 0; b < IPL_NUM_BANDS; ++b)
            in.airAbsorptionModel.coefficients[b] = kDefaultAirAbsorption[b] * airScale;
    }
    in.airAbsorptionModel.callback = nullptr;
    in.airAbsorptionModel.userData = nullptr;
    in.airAbsorptionModel.dirty = IPL_FALSE;

    // Directivity.
    in.directivity.dipoleWeight = Clamp01(params.dipoleWeight);
    in.directivity.dipolePower = std::max(0.f, params.dipolePower);
    in.directivity.callback = nullptr;
    in.directivity.userData = nullptr;

    // Occlusion / transmission.
    in.occlusionType = m_cfg.occlusionType == 0 ? IPL_OCCLUSIONTYPE_RAYCAST : IPL_OCCLUSIONTYPE_VOLUMETRIC;
    in.occlusionRadius = params.radiusMeters > 0.f ? params.radiusMeters : std::max(0.05f, m_cfg.sourceRadius);
    in.numOcclusionSamples = Clamp(m_cfg.occlusionSamples, 1, std::max(1, sim.Settings().maxNumOcclusionSamples));
    in.numTransmissionRays = Clamp(m_cfg.transmissionRays, 1, 16);

    // Reflections.
    if (wantReflections)
        m_setup.reflections->FillSourceInputs(m_cfg, params, in);

    // Pathing.
    if (wantPathing)
        m_setup.pathing->FillSourceInputs(m_cfg, params, m_setup.reflections->ProbeBatch(), in);

    in.deviationModel = nullptr;

    iplSourceSetInputs(s.source, flags, &in);
    s.lastPosition = params.position;
    s.lastActive = m_tick;
    (void)listener;
    return true;
}

void SimulationThread::DestroyAllSources(bool force)
{
    if (m_setup.capture) {
        for (uint32_t i = 0; i < ChannelCapture::kSlots; ++i) {
            CaptureSlot& slot = m_setup.capture->Slot(i);
            if (slot.source)
                RetireSimulationSource(*slot.source);
        }
    }
    for (TrackedSource& t : m_streams) {
        if (t.source)
            RetireSimulationSource(*t.source);
    }
    m_setup.simulator->ProcessDeferredReleases(force);
}

// ---------------------------------------------------------------------------
// Baking
// ---------------------------------------------------------------------------
uint64_t SimulationThread::BakeFingerprint(const BspGeometry& geometry, const RuntimeConfig& cfg,
                                            const IPLSimulationSettings& settings)
{
    uint64_t hash = 14695981039346656037ull;
    const auto bytes = [&hash](const void* data, size_t size) {
        const auto* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < size; ++i)
            hash = (hash ^ p[i]) * 1099511628211ull;
    };
    const auto value = [&bytes](const auto& v) { bytes(&v, sizeof(v)); };
    const auto array = [&bytes, &value](const auto& items) {
        const uint64_t count = items.size();
        value(count);
        if (count)
            bytes(items.data(), items.size() * sizeof(items[0]));
    };
    const auto mesh = [&array](const MeshData& m) {
        array(m.vertices);
        array(m.triangles);
        array(m.materialIndices);
        array(m.materials);
    };
    const uint32_t version = STEAMAUDIO_VERSION;
    value(version);
    mesh(geometry.world);
    mesh(geometry.staticPropMesh);
    for (const BspBrushModel& brush : geometry.brushModels) {
        value(brush.modelIndex);
        value(brush.spawnOrigin);
        value(brush.solid);
        mesh(brush.mesh);
    }
    value(cfg.unitsPerMeter);
    value(cfg.probeSpacing);
    value(cfg.probeHeight);
    value(cfg.bakeRays);
    value(cfg.bakeBounces);
    value(cfg.bakeDuration);
    value(cfg.ambisonicOrder);
    value(cfg.irradianceMinDistance);
    value(cfg.pathing);
    value(cfg.pathingVisSamples);
    value(cfg.pathingVisRadius);
    value(cfg.pathingVisThreshold);
    value(cfg.pathingVisRange);
    value(cfg.pathingRange);
    value(settings.numDiffuseSamples);
    value(settings.maxDuration);
    value(settings.maxOrder);
    value(settings.reflectionType);
    value(settings.samplingRate);
    value(settings.frameSize);
    return hash;
}

std::string SimulationThread::CacheFileForMap(const std::string& mapName) const
{
    if (mapName.empty())
        return {};
    std::string safe;
    safe.reserve(mapName.size());
    for (char c : mapName) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        safe.push_back(ok ? c : '_');
    }
    std::string dir = m_setup.cacheDirectory;
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\')
        dir.push_back('/');
    if (!m_mapGeometry || !m_setup.simulator)
        return {};
    return dir + safe + "-v2-" + std::to_string(BakeFingerprint(*m_mapGeometry, m_cfg, m_setup.simulator->Settings())) +
           ".probes";
}

void SimulationThread::StartBakeForCurrentMap(bool force)
{
    if (!m_setup.reflections || !m_setup.scene || !m_setup.scene->IsValid() || !m_setup.simulator->Scene())
        return;
    if (m_bakeAttempted && !force)
        return;
    const std::string cache = CacheFileForMap(m_mapName);
    if (!force && m_setup.reflections->HasBakedData())
        return;
    ReflectionSimulator::ExtraBakeStep pathingStep;
    if (m_setup.pathing && m_cfg.pathing)
        pathingStep = m_setup.pathing->MakeBakeStep(m_cfg);
    if (m_setup.reflections->StartBake(m_mapGeometry, m_cfg, cache, std::move(pathingStep))) {
        m_bakeAttempted = true;
        SA_LOGI("[sim] bake started for %s -> %s", m_mapName.c_str(), cache.c_str());
    } else {
        SA_LOGW("[sim] bake could not be started for %s", m_mapName.c_str());
    }
}

} // namespace sa
