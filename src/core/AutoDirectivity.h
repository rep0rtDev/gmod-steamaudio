// src/core/AutoDirectivity.h
//
// Heuristic directivity for engine sounds (C16). Source gives no radiation
// pattern per sound, so weapon and voice channels are treated as dipoles
// facing along the emitting entity. Pure functions; the hook layer supplies
// the facing and applies explicit per-sound overrides afterwards so they win.
#pragma once

#include <cmath>
#include <cstdint>

#include "mixing/SoundSource.h"
#include "steamaudio/Config.h"
#include "util/Math.h"

namespace sa {

struct DirectivityHint {
    float weight = 0.f; // 0 = omnidirectional (no directivity implied)
    float power = 1.f;
    bool Directional() const { return weight > 0.f; }
};

// Maps a sound's channel/sentence metadata to a dipole hint. Channels the
// heuristic does not know stay omnidirectional.
inline DirectivityHint InferDirectivity(const RuntimeConfig& cfg, int32_t channel, bool sentence)
{
    DirectivityHint hint;
    if (!cfg.autoDirectivity)
        return hint;
    if (channel == kChanWeapon) {
        hint.weight = Clamp(cfg.weaponDipoleWeight, 0.f, 1.f);
        hint.power = Clamp(cfg.weaponDipolePower, 0.f, 8.f);
    } else if (channel == kChanVoice || sentence || (channel >= kChanVoiceBase && channel < kChanUserBase)) {
        hint.weight = Clamp(cfg.voiceDipoleWeight, 0.f, 1.f);
        hint.power = Clamp(cfg.voiceDipolePower, 0.f, 8.f);
    }
    return hint;
}

inline bool ValidForward(const Vec3& forward)
{
    return std::isfinite(forward.x) && std::isfinite(forward.y) && std::isfinite(forward.z) &&
           forward.LengthSq() > 1e-6f;
}

// Writes the dipole into `p` when the hint is directional and the facing is
// usable; otherwise leaves `p` omnidirectional. Returns whether it applied.
inline bool ApplyDirectivity(SourceParams& p, const DirectivityHint& hint, bool haveForward, const Vec3& forward)
{
    p.dipoleWeight = 0.f;
    p.dipolePower = 1.f;
    if (!hint.Directional() || !haveForward || !ValidForward(forward))
        return false;
    p.forward = forward.Normalized();
    p.dipoleWeight = hint.weight;
    p.dipolePower = hint.power;
    return true;
}

} // namespace sa
