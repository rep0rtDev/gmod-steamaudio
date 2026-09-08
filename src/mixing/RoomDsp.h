// src/mixing/RoomDsp.h
//
// Replacement for the Source engine's `dsp_room` / `dsp_player` / `dsp_water`
// processing, which is bypassed once the mixer is hooked.
//
//   * `RoomPreset` maps the legacy room preset numbers (0-29 and the 100+
//     automatic-DSP templates from dsp_presets.txt) onto the parameters of a
//     small feedback-delay-network reverb (`RoomReverb`). The per-source
//     wet mix follows the engine's model: it grows with source distance
//     between `mixMin` and `mixMax`, and quiet sounds (soundlevel < dbMin)
//     get `mixDrop` less.
//   * `PlayerDspPreset` maps `dsp_player` presets (facing-away lowpass,
//     explosion/shock/flashbang muffles) onto a master-bus lowpass with the
//     preset's duration and fade.
//   * `EnvironmentProcessor` owns the master-bus stages: two `RoomReverb`
//     instances cross-faded on preset changes, the underwater lowpass and the
//     player-DSP lowpass. It runs on the audio thread and never allocates
//     after Initialize().
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "mixing/Dsp.h"

namespace sa {

// Source: SND_GetDspMix() scales the mix by distance up to this many units.
constexpr float kDspMixDistanceUnits = 1400.f;

struct RoomPreset {
    int32_t id = 0;
    const char* name = "off";
    float size = 1.f;         // delay-line length scale (1.0 ~ medium room)
    float decaySeconds = 0.f; // RT60; 0 disables the reverb
    float dampingHz = 5000.f; // high-frequency decay cutoff inside the feedback loop
    float preDelayMs = 0.f;
    float wet = 0.f;          // preset output gain
    float mixMin = 0.2f;      // dsp_presets.txt mixrng
    float mixMax = 0.7f;
    float dbMin = 80.f;       // soundlevels below this get less mix
    float mixDrop = 0.5f;

    bool Active() const { return decaySeconds > 0.f && wet > 0.f; }
};

// Returns the preset for a `dsp_room` value. Ids without a room definition
// (system/player presets, out of range) resolve to preset 0 (off).
const RoomPreset& FindRoomPreset(int32_t id);

// Wet mix for one source (Source's SND_GetDspMix model). `distanceUnits` is
// the listener distance in game units, `soundLevelDb` the source soundlevel
// (DistMultToSoundLevel(); 0 = attenuation-less, never dropped).
float RoomMixForSource(const RoomPreset& preset, float distanceUnits, float soundLevelDb);

struct PlayerDspPreset {
    int32_t id = 0;
    float cutoffHz = 0.f;        // 0 = no lowpass
    float gain = 1.f;
    float durationSeconds = 0.f; // 0 = until changed
    float fadeSeconds = 0.f;     // fade-out time after duration (>0 linear, <0 exponential)
    bool Active() const { return cutoffHz > 0.f; }
};

const PlayerDspPreset& FindPlayerDspPreset(int32_t id);

// Compact 8-line feedback delay network with Householder mixing and a
// one-pole damping filter per line, followed by two all-pass diffusers per
// output channel. Mono in, stereo out.
class RoomReverb {
public:
    static constexpr size_t kLines = 8;
    static constexpr float kMaxPreDelayMs = 250.f;
    static constexpr float kMaxSize = 4.f;

    void Prepare(int32_t sampleRate, int32_t frameSize);
    void Reset();
    void SetPreset(const RoomPreset& preset);
    const RoomPreset& Preset() const { return m_preset; }

    // Adds the reverberated `input` (frameSize samples) scaled by `gain` into
    // `left`/`right`.
    void Process(const float* input, float* left, float* right, size_t frames, float gain);

    // True while the tail is still audible.
    bool Ringing() const { return m_ringing; }

private:
    struct Line {
        std::vector<float> buffer;
        size_t length = 1;
        size_t write = 0;
        float feedback = 0.f;
        float damp = 0.f;
        float dampState = 0.f;
    };
    struct Allpass {
        std::vector<float> buffer;
        size_t length = 1;
        size_t write = 0;
    };

    void ConfigureLengths();
    float AllpassNext(Allpass& ap, float x, float g);

    int32_t m_sampleRate = 0;
    int32_t m_frameSize = 0;
    RoomPreset m_preset;
    std::array<Line, kLines> m_lines;
    std::array<Allpass, 2> m_allpassL;
    std::array<Allpass, 2> m_allpassR;
    std::vector<float> m_preDelay;
    size_t m_preDelayLength = 1;
    size_t m_preDelayWrite = 0;
    std::vector<float> m_scratch;
    float m_wet = 0.f;
    bool m_ringing = false;
    uint32_t m_silentFrames = 0;
};

// Environment state published by the game thread (Lua reads the engine
// convars `dsp_room`, `dsp_player`, `dsp_water` and the listener's water
// contents); consumed on the audio thread.
struct EnvironmentState {
    int32_t roomPreset = 0;      // dsp_room
    int32_t playerPreset = 0;    // dsp_player
    int32_t waterPreset = 14;    // dsp_water (room preset used while underwater)
    uint8_t underwater = 0;
    uint8_t valid = 0;
};

// Per-frame room-reverb send target handed to the renderers.
struct RoomSend {
    float* bus = nullptr;              // null = no room reverb this frame
    const RoomPreset* preset = nullptr;
    bool always = false;               // send even when Steam Audio reflections rendered the source
    bool Enabled() const { return bus != nullptr && preset != nullptr && preset->Active(); }
};

struct EnvironmentConfig {
    int32_t roomMode = 1;         // 0 off, 1 only for sources without Steam Audio reflections, 2 always
    int32_t roomOverride = -1;    // >= 0 forces a room preset regardless of dsp_room
    float roomGain = 1.f;
    bool playerDsp = true;
    bool underwater = true;
    float underwaterCutoffHz = 900.f;
    float underwaterGain = 0.8f;
};

class EnvironmentProcessor {
public:
    void Initialize(int32_t sampleRate, int32_t frameSize);
    void Shutdown();
    bool IsInitialized() const { return m_frameSize > 0; }

    // Game/audio thread boundary is handled by the caller; both setters are
    // called on the audio thread with the latest published values.
    void SetConfig(const EnvironmentConfig& cfg) { m_cfg = cfg; }
    void SetState(const EnvironmentState& state) { m_state = state; }

    // Call at the start of a frame: selects the effective room preset and
    // clears the send bus. Returns the preset that sources should send to
    // (inactive preset -> no sends needed).
    const RoomPreset& BeginFrame();
    // Whether every spatialized source should send to the room reverb
    // (mode 2, or underwater) rather than only those without Steam Audio
    // reflections.
    bool SendAlways() const;
    bool RoomEnabled() const;
    float* SendBus() { return m_sendBus.data(); }

    // Renders the reverb from the send bus into the master, then applies the
    // player-DSP / underwater lowpass in place.
    void EndFrame(float* left, float* right);

    int32_t EffectiveRoomPreset() const { return m_effectiveRoom; }
    // Effective master lowpass cutoff (underwater or dsp_player), 0 when bypassed.
    float PlayerLowpassHz() const { return m_masterCutoff; }
    bool Ringing() const { return m_reverb[0].Ringing() || m_reverb[1].Ringing(); }

private:
    void UpdatePlayerDsp();

    int32_t m_sampleRate = 0;
    int32_t m_frameSize = 0;
    EnvironmentConfig m_cfg;
    EnvironmentState m_state;
    std::vector<float> m_sendBus;
    std::array<RoomReverb, 2> m_reverb;
    size_t m_activeReverb = 0;
    int32_t m_effectiveRoom = 0;
    float m_crossfade = 1.f; // 1 = fully on active reverb
    dsp::BiquadLowpass m_lowpassL;
    dsp::BiquadLowpass m_lowpassR;
    dsp::Smoother m_cutoffSmoother;
    dsp::Smoother m_gainSmoother;
    bool m_lowpassActive = false;
    // dsp_player timing
    int32_t m_playerPresetSeen = 0;
    float m_playerElapsed = 0.f;
    float m_playerCutoff = 0.f;
    float m_playerGain = 1.f;
    float m_masterCutoff = 0.f;
};

} // namespace sa
