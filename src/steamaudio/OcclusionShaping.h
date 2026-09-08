// src/steamaudio/OcclusionShaping.h
//
// Turns the simulator's raw occlusion (visible fraction of the source
// sphere, 1 = unobstructed) into the values the direct effect applies.
//
//   * Visibility knee: point emitters that sit on or inside geometry (an
//     impact sound at a wall, an NPC's own hull) are only partly visible to
//     volumetric occlusion although nothing stands between them and the
//     listener. Visibility at or above `occlusionFullVisibility` is treated
//     as unoccluded, at or below `occlusionZeroVisibility` as fully occluded,
//     linear in between.
//   * Leak floor: the occluded part of the signal passes through the
//     material's transmission coefficients, which for concrete are ~-40 dB and
//     make a source behind a wall vanish. `occlusionMinGain` raises the
//     low-band transmission to at least that value (mid/high bands to a
//     fraction of it) so blocked sources stay audible and muffled. Without
//     transmission the same floor is applied to the occlusion gain itself.
#pragma once

#include <algorithm>

#include "steamaudio/Config.h"
#include "steamaudio/PhononApi.h"

namespace sa {

// Per-band multipliers of `occlusionMinGain` (low, mid, high).
constexpr float kOcclusionLeakBandWeights[3] = {1.f, 0.6f, 0.35f};

inline float ShapeOcclusionVisibility(float raw, const RuntimeConfig& cfg)
{
    const float zero = std::clamp(cfg.occlusionZeroVisibility, 0.f, 0.95f);
    const float full = std::clamp(cfg.occlusionFullVisibility, zero + 0.05f, 1.f);
    const float v = std::clamp(raw, 0.f, 1.f);
    if (v >= full)
        return 1.f;
    if (v <= zero)
        return 0.f;
    return (v - zero) / (full - zero);
}

// Rewrites `params.occlusion` / `params.transmission` in place. `flags` are
// the direct-effect apply flags that will be used with `params`.
inline void ShapeOcclusion(IPLDirectEffectParams& params, IPLDirectEffectFlags flags, const RuntimeConfig& cfg)
{
    if (!(flags & IPL_DIRECTEFFECTFLAGS_APPLYOCCLUSION))
        return;
    const float visibility = ShapeOcclusionVisibility(params.occlusion, cfg);
    const float floor = std::clamp(cfg.occlusionMinGain, 0.f, 1.f);
    if (flags & IPL_DIRECTEFFECTFLAGS_APPLYTRANSMISSION) {
        params.occlusion = visibility;
        for (int b = 0; b < 3; ++b)
            params.transmission[b] = std::max(params.transmission[b], floor * kOcclusionLeakBandWeights[b]);
    } else {
        params.occlusion = floor + (1.f - floor) * visibility;
    }
}

} // namespace sa
