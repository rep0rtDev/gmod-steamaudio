// src/steamaudio/Simulator.cpp
#include "Simulator.h"

#include <algorithm>
#include <thread>

#include "PhononContext.h"
#include "mixing/SoundSource.h"
#include "util/Logging.h"

namespace sa {

Simulator::~Simulator()
{
    Shutdown();
}

bool Simulator::Initialize(PhononContext& context, const BackendDevices& devices, const StaticConfig& config,
                           std::string& errorOut)
{
    Shutdown();
    if (!context.IsValid()) {
        errorOut = "Steam Audio context not initialized";
        return false;
    }
    m_context = &context;

    int32_t threads = config.simulationThreads;
    if (threads <= 0) {
        const unsigned hw = std::thread::hardware_concurrency();
        threads = hw > 3 ? static_cast<int32_t>(hw) - 2 : 1;
    }
    threads = std::max(1, std::min(threads, 32));

    m_flags = static_cast<IPLSimulationFlags>(IPL_SIMULATIONFLAGS_DIRECT | IPL_SIMULATIONFLAGS_REFLECTIONS |
                                              IPL_SIMULATIONFLAGS_PATHING);

    m_settings = IPLSimulationSettings{};
    m_settings.flags = m_flags;
    m_settings.sceneType = devices.sceneType;
    m_settings.reflectionType = devices.reflectionType;
    m_settings.maxNumOcclusionSamples = std::max(1, config.maxOcclusionSamples);
    m_settings.maxNumRays = std::max(64, config.maxRays);
    m_settings.numDiffuseSamples = std::max(1, config.numDiffuseSamples);
    m_settings.maxDuration = std::max(0.1f, config.maxIrDuration);
    m_settings.maxOrder = std::max(0, std::min(config.maxAmbisonicOrder, 3));
    m_settings.maxNumSources = std::max(1, config.maxSources);
    m_settings.numThreads = threads;
    m_settings.rayBatchSize = 16;
    m_settings.numVisSamples = 4;
    m_settings.samplingRate = context.SampleRate();
    m_settings.frameSize = context.FrameSize();
    m_settings.openCLDevice = devices.openCL;
    m_settings.radeonRaysDevice = devices.radeonRays;
    m_settings.tanDevice = devices.tan;

    IPLerror err = iplSimulatorCreate(context.Handle(), &m_settings, &m_simulator);
    if (err != IPL_STATUS_SUCCESS || !m_simulator) {
        SA_LOGW("iplSimulatorCreate(full) failed: %s; retrying without pathing", IplErrorToString(err));
        m_simulator = nullptr;
        m_flags = static_cast<IPLSimulationFlags>(IPL_SIMULATIONFLAGS_DIRECT | IPL_SIMULATIONFLAGS_REFLECTIONS);
        m_settings.flags = m_flags;
        err = iplSimulatorCreate(context.Handle(), &m_settings, &m_simulator);
    }
    if (err != IPL_STATUS_SUCCESS || !m_simulator) {
        SA_LOGW("iplSimulatorCreate(direct+reflections) failed: %s; retrying direct only", IplErrorToString(err));
        m_simulator = nullptr;
        m_flags = IPL_SIMULATIONFLAGS_DIRECT;
        m_settings.flags = m_flags;
        m_settings.openCLDevice = nullptr;
        m_settings.radeonRaysDevice = nullptr;
        m_settings.tanDevice = nullptr;
        err = iplSimulatorCreate(context.Handle(), &m_settings, &m_simulator);
    }
    if (err != IPL_STATUS_SUCCESS || !m_simulator) {
        errorOut = std::string("iplSimulatorCreate failed: ") + IplErrorToString(err);
        m_simulator = nullptr;
        return false;
    }

    SA_LOGI("Simulator created: flags=%d scene=%d reflections=%d rays=%d threads=%d maxSources=%d duration=%.2f order=%d",
            static_cast<int>(m_flags), static_cast<int>(m_settings.sceneType),
            static_cast<int>(m_settings.reflectionType), m_settings.maxNumRays, m_settings.numThreads,
            m_settings.maxNumSources, m_settings.maxDuration, m_settings.maxOrder);
    return true;
}

void Simulator::Shutdown()
{
    ProcessDeferredReleases(true);
    if (m_simulator) {
        iplSimulatorRelease(&m_simulator);
        m_simulator = nullptr;
    }
    m_scene = nullptr;
    m_context = nullptr;
    m_sourceCount = 0;
}

void Simulator::SetScene(IPLScene scene)
{
    if (!m_simulator)
        return;
    m_scene = scene;
    iplSimulatorSetScene(m_simulator, scene);
}

void Simulator::AddProbeBatch(IPLProbeBatch batch)
{
    if (m_simulator && batch)
        iplSimulatorAddProbeBatch(m_simulator, batch);
}

void Simulator::RemoveProbeBatch(IPLProbeBatch batch)
{
    if (m_simulator && batch)
        iplSimulatorRemoveProbeBatch(m_simulator, batch);
}

void Simulator::SetSharedInputs(IPLSimulationFlags flags, const IPLSimulationSharedInputs& inputs)
{
    if (!m_simulator)
        return;
    const IPLSimulationFlags masked = static_cast<IPLSimulationFlags>(flags & m_flags);
    if (!masked)
        return;
    IPLSimulationSharedInputs copy = inputs;
    iplSimulatorSetSharedInputs(m_simulator, masked, &copy);
}

void Simulator::Commit()
{
    if (m_simulator)
        iplSimulatorCommit(m_simulator);
}

void Simulator::RunDirect()
{
    if (m_simulator && (m_flags & IPL_SIMULATIONFLAGS_DIRECT))
        iplSimulatorRunDirect(m_simulator);
}

void Simulator::RunReflections()
{
    if (m_simulator && (m_flags & IPL_SIMULATIONFLAGS_REFLECTIONS))
        iplSimulatorRunReflections(m_simulator);
}

void Simulator::RunPathing()
{
    if (m_simulator && (m_flags & IPL_SIMULATIONFLAGS_PATHING))
        iplSimulatorRunPathing(m_simulator);
}

bool Simulator::CreateSource(SoundSource& source)
{
    if (!m_simulator)
        return false;
    SoundSource::SimulationState& sim = source.Sim();
    if (sim.source)
        return true;
    if (m_sourceCount >= static_cast<size_t>(m_settings.maxNumSources)) {
        SA_LOG_RT(QueueFull);
        return false;
    }
    IPLSourceSettings settings{};
    settings.flags = m_flags;
    const IPLerror err = iplSourceCreate(m_simulator, &settings, &sim.source);
    if (err != IPL_STATUS_SUCCESS || !sim.source) {
        SA_LOGE("iplSourceCreate failed for source %u: %s", source.Id(), IplErrorToString(err));
        sim.source = nullptr;
        return false;
    }
    iplSourceAdd(sim.source, m_simulator);
    sim.added = true;
    sim.flags = m_flags;
    ++m_sourceCount;
    // Second reference for the audio thread.
    iplSourceRetain(sim.source);
    source.MarkSimulationReady(static_cast<IPLSimulationFlags>(0));
    source.PublishSimulationSource(sim.source);
    return true;
}

void Simulator::DestroySource(SoundSource& source)
{
    SoundSource::SimulationState& sim = source.Sim();
    if (!sim.source)
        return;
    if (sim.added && m_simulator)
        iplSourceRemove(sim.source, m_simulator);
    source.PublishSimulationSource(nullptr);
    source.MarkSimulationReady(static_cast<IPLSimulationFlags>(0));
    // The audio thread's reference is released once it can no longer observe the handle.
    const uint64_t frame = m_audioFrameCounter ? m_audioFrameCounter->load(std::memory_order_acquire) : 0;
    m_deferred.push_back(DeferredRelease{sim.source, frame});
    iplSourceRelease(&sim.source);
    sim.source = nullptr;
    sim.added = false;
    sim.everSimulated = false;
    if (m_sourceCount)
        --m_sourceCount;
}

void Simulator::ProcessDeferredReleases(bool force)
{
    if (m_deferred.empty())
        return;
    const uint64_t now = m_audioFrameCounter ? m_audioFrameCounter->load(std::memory_order_acquire) : UINT64_MAX;
    for (size_t i = 0; i < m_deferred.size();) {
        DeferredRelease& entry = m_deferred[i];
        // Two frames guarantee any in-flight RenderSource that loaded the old
        // handle has completed.
        if (force || !m_audioFrameCounter || now >= entry.retiredAtFrame + 2) {
            IPLSource handle = entry.source;
            iplSourceRelease(&handle);
            m_deferred[i] = m_deferred.back();
            m_deferred.pop_back();
        } else {
            ++i;
        }
    }
}

} // namespace sa
