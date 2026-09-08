// src/mixing/FallbackMixer.h
//
// Minimal stereo mixer used when Steam Audio is unavailable (phonon.dll
// missing, context creation failed, or `snd_sa_enabled 0`). It reproduces the
// engine's distance rolloff, constant-power azimuth panning and a gentle
// low-pass for sources behind the listener so the game stays audible with
// sensible spatial cues. Runs entirely on the audio thread; no allocations
// after Initialize().
#pragma once

#include <cstdint>
#include <vector>

#include "mixing/RoomDsp.h"
#include "steamaudio/Config.h"
#include "steamaudio/Simulator.h"

namespace sa {

class SoundSource;

class FallbackMixer {
public:
    void Initialize(int32_t frameSize, int32_t sampleRate);
    void Shutdown();

    void BeginFrame(const RuntimeConfig& cfg, const ListenerState& listener, const RoomSend& roomSend = RoomSend{});
    void RenderSource(SoundSource& source, const float* const* input, uint32_t inputChannels, float gain);
    void EndFrame();

    const float* MasterLeft() const { return m_left.data(); }
    const float* MasterRight() const { return m_right.data(); }
    int32_t FrameSize() const { return m_frameSize; }
    float LastPeak() const { return m_lastPeak; }

private:
    int32_t m_frameSize = 0;
    int32_t m_sampleRate = 0;
    std::vector<float> m_left;
    std::vector<float> m_right;
    std::vector<float> m_mono;
    RuntimeConfig m_cfg;
    RoomSend m_roomSend;
    ListenerState m_listener;
    float m_lastPeak = 0.f;
};

} // namespace sa
