// src/steamaudio/HRTFRenderer.h
//
// Audio-thread rendering pipeline for one output frame:
//
//   per source:  input PCM -> [downmix] -> direct effect (attenuation, air
//                absorption, directivity, occlusion, transmission) -> binaural
//                (HRTF) or panning -> master
//                dry input -> reflection effect -> reflection mixer / ambisonic bus
//                dry input -> path effect -> master (binaural) or ambisonic bus
//   per frame:   reflection mixer + ambisonic bus -> ambisonics decode (HRTF)
//                -> master
//
// All effect objects and buffers are pre-allocated in Initialize(); nothing
// on the render path allocates. The renderer is used exclusively by the audio
// thread once initialized.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "PhononApi.h"
#include "PhononContext.h"
#include "mixing/Dsp.h"
#include "mixing/FallbackMixer.h"
#include "mixing/RoomDsp.h"
#include "steamaudio/Backend.h"
#include "steamaudio/Config.h"
#include "steamaudio/Simulator.h"

namespace sa {

class SoundSource;
struct EffectSet;
struct RenderDiag;

// Smoothed mean-square levels of what reached the stereo master (metering).
struct MixBalance {
    float masterL = 0.f;
    float masterR = 0.f;
    float direct = 0.f;      // HRTF/panned direct paths (plus pathing)
    float reflections = 0.f; // decoded ambisonic bus
    float nonSpatial = 0.f;  // 2D passthrough
};

class HRTFRenderer {
public:
    HRTFRenderer() = default;
    ~HRTFRenderer();
    HRTFRenderer(const HRTFRenderer&) = delete;
    HRTFRenderer& operator=(const HRTFRenderer&) = delete;

    bool Initialize(PhononContext& context, const BackendDevices& devices, const StaticConfig& config,
                    std::string& errorOut);
    void Shutdown();
    bool IsValid() const { return m_hrtf != nullptr; }
    IPLHRTF Hrtf() const { return m_hrtf; }
    IPLReflectionEffectType ReflectionType() const { return m_reflectionType; }

    // Effect pool (audio thread).
    EffectSet* AcquireEffects();
    // Returns the set to the pool; reflection tails keep ringing out through
    // the mixer until complete.
    void ReleaseEffects(EffectSet* set);
    size_t FreeEffectSets() const { return m_freeCount.load(std::memory_order_relaxed); }
    // Reflection tails cut short to serve a new source / sources rendered 2D
    // because no effect set was available (any thread).
    uint64_t TailsCut() const { return m_stats.tailsCut.load(std::memory_order_relaxed); }
    uint64_t PoolExhausted() const { return m_stats.poolExhausted.load(std::memory_order_relaxed); }

    // Frame lifecycle (audio thread).
    void BeginFrame(const RuntimeConfig& cfg, const ListenerState& listener, const RoomSend& roomSend = RoomSend{});
    // `input` holds `inputChannels` de-interleaved channels of FrameSize samples.
    void RenderSource(SoundSource& source, const float* const* input, uint32_t inputChannels, float gain);
    void EndFrame();

    // Rendered stereo master for the current frame (valid after EndFrame).
    const float* MasterLeft() const { return m_master.Channels() >= 2 ? m_master.Get()->data[0] : nullptr; }
    const float* MasterRight() const { return m_master.Channels() >= 2 ? m_master.Get()->data[1] : nullptr; }
    int32_t FrameSize() const { return m_frameSize; }

    // Peak of the last frame (for debugging/metering).
    float LastPeak() const { return m_lastPeak; }
    MixBalance Balance() const;

private:
    struct PooledEffects;

    bool CreateHrtf(const StaticConfig& config, std::string& errorOut);
    bool CreateEffectSet(PooledEffects& set);
    void DestroyEffectSet(PooledEffects& set);
    void ReturnEffects(PooledEffects* set);
    void RenderNonSpatial(const float* const* input, uint32_t inputChannels, float gain, float gainL, float gainR,
                          float startGain, RenderDiag& diag);
    float MeanSquare(const float* samples) const;
    void ApplyPropagationDelay(dsp::PropagationDelay& delay, float* mono, float distMeters) const;
    void AddToMaster(const IPLAudioBuffer& stereo, float gain, double& energyMeter);
    void UpdateBalance();
    void MarkReflectionApplied(const EffectSet& set);
    void RenderReflectionTails();
    IPLVector3 DirectionTo(const IPLVector3& sourcePosition) const;

    PhononContext* m_context = nullptr;
    FallbackMixer m_fallback;
    bool m_hrtfEnabled = true;
    IPLHRTF m_hrtf = nullptr;
    IPLReflectionMixer m_reflectionMixer = nullptr;
    IPLAmbisonicsDecodeEffect m_ambisonicsDecode = nullptr;
    IPLReflectionEffectSettings m_reflectionSettings{};
    IPLReflectionEffectType m_reflectionType = IPL_REFLECTIONEFFECTTYPE_CONVOLUTION;
    IPLTrueAudioNextDevice m_tanDevice = nullptr;
    bool m_useMixer = false;
    int32_t m_frameSize = 0;
    int32_t m_sampleRate = 0;
    int32_t m_maxOrder = 1;
    int32_t m_ambisonicChannels = 4;
    float m_maxIrDuration = 1.f;

    std::vector<PooledEffects*> m_pool;
    std::vector<PooledEffects*> m_freeEffects;
    std::atomic<size_t> m_freeCount{0};
    std::vector<PooledEffects*> m_drainingEffects;

    PhononContext::AudioBuffer m_monoIn;
    PhononContext::AudioBuffer m_monoDirect;
    PhononContext::AudioBuffer m_stereoScratch;
    PhononContext::AudioBuffer m_ambisonicScratch;
    PhononContext::AudioBuffer m_ambisonicBus;
    PhononContext::AudioBuffer m_master;
    std::vector<float> m_pathShScratch;

    RuntimeConfig m_cfg;
    RoomSend m_roomSend;
    ListenerState m_listener;
    IPLCoordinateSpace3 m_listenerFrame{};
    bool m_ambisonicBusUsed = false;
    bool m_mixerUsed = false;
    float m_lastPeak = 0.f;
    double m_frameDirectEnergy = 0.0;
    double m_frameReflectionEnergy = 0.0;
    double m_frameNonSpatialEnergy = 0.0;
    struct {
        std::atomic<float> masterL{0.f};
        std::atomic<float> masterR{0.f};
        std::atomic<float> direct{0.f};
        std::atomic<float> reflections{0.f};
        std::atomic<float> nonSpatial{0.f};
    } m_balance;
    struct {
        std::atomic<uint64_t> tailsCut{0};
        std::atomic<uint64_t> poolExhausted{0};
    } m_stats;
};

} // namespace sa
