// src/steamaudio/Simulator.h
//
// Owner of the single IPLSimulator (Steam Audio 4.x runs direct, reflections
// and pathing simulation through one simulator object) and of the IPLSource
// handles attached to it. ReflectionSimulator and PathingSimulator configure
// their parts of the shared/per-source inputs through this class.
//
// Thread-safety: everything here runs on the simulation thread. The audio
// thread only touches the retained IPLSource handle published into
// SoundSource (iplSourceGetOutputs is safe to call concurrently with the
// simulation). Handles retired from the audio thread are released only after
// the audio frame counter has advanced past the retirement frame.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "PhononApi.h"
#include "steamaudio/Backend.h"
#include "steamaudio/Config.h"

namespace sa {

class PhononContext;
class SoundSource;

struct ListenerState {
    IPLCoordinateSpace3 frame{}; // Steam Audio space
    bool valid = false;
};

class Simulator {
public:
    Simulator() = default;
    ~Simulator();
    Simulator(const Simulator&) = delete;
    Simulator& operator=(const Simulator&) = delete;

    bool Initialize(PhononContext& context, const BackendDevices& devices, const StaticConfig& config,
                    std::string& errorOut);
    void Shutdown();

    bool IsValid() const { return m_simulator != nullptr; }
    IPLSimulator Handle() const { return m_simulator; }
    IPLSimulationFlags Flags() const { return m_flags; }
    const IPLSimulationSettings& Settings() const { return m_settings; }
    bool SupportsReflections() const { return (m_flags & IPL_SIMULATIONFLAGS_REFLECTIONS) != 0; }
    bool SupportsPathing() const { return (m_flags & IPL_SIMULATIONFLAGS_PATHING) != 0; }

    void SetScene(IPLScene scene);
    IPLScene Scene() const { return m_scene; }

    void AddProbeBatch(IPLProbeBatch batch);
    void RemoveProbeBatch(IPLProbeBatch batch);

    // Shared (listener) inputs for the given simulation types.
    void SetSharedInputs(IPLSimulationFlags flags, const IPLSimulationSharedInputs& inputs);

    // Applies pending scene/source/probe changes. Must not run concurrently
    // with Run*().
    void Commit();

    void RunDirect();
    void RunReflections();
    void RunPathing();

    // Creates the IPLSource for `source` (all simulator flags) and adds it.
    bool CreateSource(SoundSource& source);
    void DestroySource(SoundSource& source);
    size_t SourceCount() const { return m_sourceCount; }

    // Counter incremented by the audio thread once per rendered frame; used to
    // delay releasing handles the audio thread may still be reading.
    void SetAudioFrameCounter(const std::atomic<uint64_t>* counter) { m_audioFrameCounter = counter; }
    // Releases retired handles that are provably unobservable. `force` releases
    // everything (audio thread stopped).
    void ProcessDeferredReleases(bool force);
    size_t PendingReleases() const { return m_deferred.size(); }

private:
    struct DeferredRelease {
        IPLSource source;
        uint64_t retiredAtFrame;
    };

    PhononContext* m_context = nullptr;
    IPLSimulator m_simulator = nullptr;
    IPLSimulationSettings m_settings{};
    IPLSimulationFlags m_flags = static_cast<IPLSimulationFlags>(0);
    IPLScene m_scene = nullptr;
    size_t m_sourceCount = 0;
    const std::atomic<uint64_t>* m_audioFrameCounter = nullptr;
    std::vector<DeferredRelease> m_deferred;
};

} // namespace sa
