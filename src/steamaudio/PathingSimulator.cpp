// src/steamaudio/PathingSimulator.cpp
#include "PathingSimulator.h"

#include <algorithm>

#include "PhononContext.h"
#include "Simulator.h"
#include "mixing/SoundSource.h"
#include "util/Logging.h"

namespace sa {

namespace {

void IPLCALL PathProgress(IPLfloat32 progress, void* userData)
{
    auto* flag = static_cast<std::atomic<float>*>(userData);
    if (flag)
        flag->store(progress, std::memory_order_relaxed);
}

} // namespace

bool PathingSimulator::Initialize(PhononContext& context, Simulator& simulator, const StaticConfig& config)
{
    Shutdown();
    if (!context.IsValid() || !simulator.IsValid())
        return false;
    m_context = &context;
    m_simulator = &simulator;
    m_static = config;
    m_enabled = simulator.SupportsPathing();

    m_identifier = IPLBakedDataIdentifier{};
    m_identifier.type = IPL_BAKEDDATATYPE_PATHING;
    m_identifier.variation = IPL_BAKEDDATAVARIATION_DYNAMIC;
    m_identifier.endpointInfluence.center = IPLVector3{0.f, 0.f, 0.f};
    m_identifier.endpointInfluence.radius = 0.f;
    return true;
}

void PathingSimulator::Shutdown()
{
    m_context = nullptr;
    m_simulator = nullptr;
    m_enabled = false;
}

ReflectionSimulator::ExtraBakeStep PathingSimulator::MakeBakeStep(const RuntimeConfig& cfg)
{
    if (!m_enabled || !m_context || !m_simulator)
        return {};
    PhononContext* ctx = m_context;
    const IPLBakedDataIdentifier identifier = m_identifier;
    const int32_t threads = std::max(1, m_simulator->Settings().numThreads);
    const RuntimeConfig snapshot = cfg;

    return [ctx, identifier, threads, snapshot](IPLScene scene, IPLProbeBatch batch, std::atomic<bool>& cancel,
                                                 IPLProgressCallback callback, void* userData) {
        if (!ctx || !ctx->IsValid() || !batch || !scene || cancel.load(std::memory_order_acquire))
            return;
        IPLPathBakeParams params{};
        params.scene = scene;
        params.probeBatch = batch;
        params.identifier = identifier;
        params.numSamples = std::max(1, std::min(snapshot.pathingVisSamples, 32));
        params.radius = std::max(0.f, snapshot.pathingVisRadius);
        params.threshold = std::max(0.f, std::min(snapshot.pathingVisThreshold, 1.f));
        params.visRange = std::max(1.f, snapshot.pathingVisRange);
        params.pathRange = std::max(params.visRange, snapshot.pathingRange);
        params.numThreads = threads;
        std::atomic<float> progress{0.f};
        SA_LOGI("Baking pathing: samples=%d radius=%.2f threshold=%.2f visRange=%.0f pathRange=%.0f",
                params.numSamples, params.radius, params.threshold, params.visRange, params.pathRange);
        iplPathBakerBake(ctx->Handle(), &params, callback ? callback : &PathProgress,
                        callback ? userData : static_cast<void*>(&progress));
    };
}

void PathingSimulator::FillSharedInputs(const RuntimeConfig& cfg, IPLSimulationSharedInputs& shared) const
{
    shared.pathingVisCallback = nullptr;
    shared.pathingUserData = nullptr;
    (void)cfg;
}

void PathingSimulator::FillSourceInputs(const RuntimeConfig& cfg, const SourceParams& params, IPLProbeBatch probes,
                                        IPLSimulationInputs& inputs) const
{
    inputs.pathingProbes = (m_enabled && params.pathing && cfg.pathing) ? probes : nullptr;
    inputs.visRadius = std::max(0.f, cfg.pathingVisRadius);
    inputs.visThreshold = std::max(0.f, std::min(cfg.pathingVisThreshold, 1.f));
    inputs.visRange = std::max(1.f, cfg.pathingVisRange);
    inputs.pathingOrder = std::max(0, std::min(cfg.ambisonicOrder, m_static.maxAmbisonicOrder));
    inputs.enableValidation = cfg.physicalAcoustics || cfg.pathingValidation ? IPL_TRUE : IPL_FALSE;
    inputs.findAlternatePaths = cfg.pathingAlternatePaths ? IPL_TRUE : IPL_FALSE;
}

} // namespace sa
