// src/steamaudio/PathingSimulator.h
//
// Sound propagation paths around geometry. Steam Audio pathing needs a baked
// probe visibility graph (IPL_BAKEDDATATYPE_PATHING); this class provides the
// bake step (run on ReflectionSimulator's bake thread against the shared probe
// batch) and fills the pathing-related shared/per-source simulation inputs.
//
// Thread-safety: simulation thread only, except `BakeStep` which runs on the
// bake worker thread.
#pragma once

#include <atomic>

#include "PhononApi.h"
#include "steamaudio/Config.h"
#include "steamaudio/ReflectionSimulator.h"

namespace sa {

class PhononContext;
class Simulator;
struct SourceParams;

class PathingSimulator {
public:
    bool Initialize(PhononContext& context, Simulator& simulator, const StaticConfig& config);
    void Shutdown();

    // Returns a bake step suitable for ReflectionSimulator::StartBake, or an
    // empty functor when pathing is unsupported by the simulator.
    ReflectionSimulator::ExtraBakeStep MakeBakeStep(const RuntimeConfig& cfg);

    void FillSharedInputs(const RuntimeConfig& cfg, IPLSimulationSharedInputs& shared) const;
    // `probes` is the batch containing baked pathing data (may be null => pathing disabled).
    void FillSourceInputs(const RuntimeConfig& cfg, const SourceParams& params, IPLProbeBatch probes,
                          IPLSimulationInputs& inputs) const;

    const IPLBakedDataIdentifier& Identifier() const { return m_identifier; }
    bool Enabled() const { return m_enabled; }

private:
    PhononContext* m_context = nullptr;
    Simulator* m_simulator = nullptr;
    StaticConfig m_static;
    IPLBakedDataIdentifier m_identifier{};
    bool m_enabled = false;
};

} // namespace sa
