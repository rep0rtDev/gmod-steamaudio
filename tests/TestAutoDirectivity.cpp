// tests/TestAutoDirectivity.cpp
#include <cmath>
#include <vector>

#include "TestFramework.h"
#include "core/AutoDirectivity.h"
#include "core/DynamicOccluders.h"
#include "core/SoundOverrides.h"
#include "core/EngineHooks.h"

using namespace sa;

namespace {

struct NullSink final : public IDynamicGeometrySink {
    DynamicGeometryId AddMesh(std::shared_ptr<const MeshData>, const Transform&, const std::string&) override
    {
        return 0;
    }
    bool UpdateMesh(DynamicGeometryId, const Transform&) override { return false; }
    bool RemoveMesh(DynamicGeometryId) override { return false; }
    bool UpdateBrushModel(int32_t, const Vec3&, const Vec3&) override { return false; }
};

EntitySnapshot Ent(int32_t index, Vec3 angles, bool dormant = false)
{
    EntitySnapshot e;
    e.index = index;
    e.serial = 1u;
    e.model = "models/weapons/w_pistol.mdl";
    e.mins = {-4.f, -4.f, -4.f};
    e.maxs = {4.f, 4.f, 4.f};
    e.angles = angles;
    e.dormant = dormant;
    return e;
}

} // namespace

SA_TEST(EngineSoundGain_PreservesVoiceVolumeAndLocalWeaponBoost)
{
    ActiveSoundInfo sound;
    sound.entity = 5;
    sound.channel = kChanVoiceBase;
    sound.volume = 0.4f;
    SA_CHECK_NEAR(EngineChannelGain(sound, 1), 0.4f, 1e-6);
    SA_CHECK(!IsLocalWeaponSound(sound, 1));
    sound.channel = kChanWeapon;
    SA_CHECK_NEAR(EngineChannelGain(sound, 1), 0.4f, 1e-6);
    sound.entity = 1;
    SA_CHECK(IsLocalWeaponSound(sound, 1));
    SA_CHECK_NEAR(EngineChannelGain(sound, 1), 0.4f * DbToLinear(2.f), 1e-6);
    sound.entity = src::kSoundFromLocalPlayer;
    SA_CHECK(IsLocalWeaponSound(sound, 1));
}

SA_TEST(AutoDirectivity_DefaultsHaveNoSilentAngles)
{
    RuntimeConfig cfg;
    for (int32_t channel : {kChanWeapon, kChanVoice}) {
        const DirectivityHint hint = InferDirectivity(cfg, channel, false);
        for (int i = -100; i <= 100; ++i) {
            const float cosine = static_cast<float>(i) / 100.f;
            const float gain = std::pow(std::fabs(1.f - hint.weight + hint.weight * cosine), hint.power);
            SA_CHECK(gain >= 0.49f);
        }
    }
}

SA_TEST(AutoDirectivity_ChannelHeuristics)
{
    RuntimeConfig cfg;
    cfg.weaponDipoleWeight = 0.6f;
    cfg.weaponDipolePower = 2.f;
    cfg.voiceDipoleWeight = 0.4f;
    cfg.voiceDipolePower = 1.f;

    DirectivityHint w = InferDirectivity(cfg, kChanWeapon, false);
    SA_CHECK(w.Directional());
    SA_CHECK_NEAR(w.weight, 0.6f, 1e-6);
    SA_CHECK_NEAR(w.power, 2.f, 1e-6);

    const int32_t voiceChannels[] = {kChanVoice, kChanVoiceBase, kChanVoiceBase + 40, kChanUserBase - 1};
    for (int32_t ch : voiceChannels) {
        DirectivityHint v = InferDirectivity(cfg, ch, false);
        SA_CHECK(v.Directional());
        SA_CHECK_NEAR(v.weight, 0.4f, 1e-6);
        SA_CHECK_NEAR(v.power, 1.f, 1e-6);
    }
    // Sentences are speech whatever channel they were emitted on.
    SA_CHECK_NEAR(InferDirectivity(cfg, kChanStatic, true).weight, 0.4f, 1e-6);

    // Everything else radiates omnidirectionally.
    const int32_t omniChannels[] = {kChanAuto,   kChanItem,     kChanBody,         kChanStream,
                                    kChanStatic, kChanUserBase, kChanUserBase + 5, kChanReplace};
    for (int32_t ch : omniChannels) {
        SA_CHECK(!InferDirectivity(cfg, ch, false).Directional());
    }

    // Master switch and zero weights disable the heuristic.
    cfg.autoDirectivity = false;
    SA_CHECK(!InferDirectivity(cfg, kChanWeapon, false).Directional());
    cfg.autoDirectivity = true;
    cfg.weaponDipoleWeight = 0.f;
    SA_CHECK(!InferDirectivity(cfg, kChanWeapon, false).Directional());
    // Out-of-range values are clamped into Steam Audio's ranges.
    cfg.weaponDipoleWeight = 7.f;
    cfg.weaponDipolePower = 100.f;
    w = InferDirectivity(cfg, kChanWeapon, false);
    SA_CHECK_NEAR(w.weight, 1.f, 1e-6);
    SA_CHECK_NEAR(w.power, 8.f, 1e-6);
}

SA_TEST(AutoDirectivity_RequiresUsableFacing)
{
    DirectivityHint hint;
    hint.weight = 0.6f;
    hint.power = 2.f;

    SourceParams p;
    p.dipoleWeight = 0.9f; // stale value from a previous frame
    // Unknown facing: omnidirectional, sound still fully playable.
    SA_CHECK(!ApplyDirectivity(p, hint, false, Vec3{1.f, 0.f, 0.f}));
    SA_CHECK_NEAR(p.dipoleWeight, 0.f, 1e-6);
    SA_CHECK_NEAR(p.dipolePower, 1.f, 1e-6);
    SA_CHECK_EQ(p.spatialize, 1);

    // Degenerate / NaN facing is rejected.
    SA_CHECK(!ApplyDirectivity(p, hint, true, Vec3{0.f, 0.f, 0.f}));
    SA_CHECK(!ApplyDirectivity(p, hint, true, Vec3{std::nanf(""), 0.f, 0.f}));
    SA_CHECK_NEAR(p.dipoleWeight, 0.f, 1e-6);

    // Valid facing is normalized and the dipole applied.
    SA_CHECK(ApplyDirectivity(p, hint, true, Vec3{0.f, 5.f, 0.f}));
    SA_CHECK_NEAR(p.forward.x, 0.f, 1e-6);
    SA_CHECK_NEAR(p.forward.y, 1.f, 1e-6);
    SA_CHECK_NEAR(p.forward.z, 0.f, 1e-6);
    SA_CHECK_NEAR(p.dipoleWeight, 0.6f, 1e-6);
    SA_CHECK_NEAR(p.dipolePower, 2.f, 1e-6);

    // Omnidirectional hint never writes a dipole even with a facing.
    DirectivityHint omni;
    SA_CHECK(!ApplyDirectivity(p, omni, true, Vec3{1.f, 0.f, 0.f}));
    SA_CHECK_NEAR(p.dipoleWeight, 0.f, 1e-6);
}

SA_TEST(AutoDirectivity_ExplicitOverridesWin)
{
    // Same order as EngineHooks::ApplyParamsForSlot: heuristic first, then
    // the Lua override merges on top.
    RuntimeConfig cfg;
    SourceParams p;
    p.channel = kChanWeapon;
    SA_CHECK(ApplyDirectivity(p, InferDirectivity(cfg, kChanWeapon, false), true, Vec3{1.f, 0.f, 0.f}));
    SA_CHECK_NEAR(p.dipoleWeight, cfg.weaponDipoleWeight, 1e-6);

    SoundOverride ov;
    ov.fields = SoundOverride::kDipoleWeight | SoundOverride::kDipolePower | SoundOverride::kForward;
    ov.dipoleWeight = 0.1f;
    ov.dipolePower = 4.f;
    ov.forward = Vec3{0.f, 0.f, -1.f};
    ov.Apply(p);
    SA_CHECK_NEAR(p.dipoleWeight, 0.1f, 1e-6);
    SA_CHECK_NEAR(p.dipolePower, 4.f, 1e-6);
    SA_CHECK_NEAR(p.forward.z, -1.f, 1e-6);

    // An override that only pins the facing keeps the heuristic's weight.
    SourceParams q;
    SA_CHECK(ApplyDirectivity(q, InferDirectivity(cfg, kChanVoice, false), true, Vec3{1.f, 0.f, 0.f}));
    SoundOverride onlyForward;
    onlyForward.fields = SoundOverride::kForward;
    onlyForward.forward = Vec3{0.f, 1.f, 0.f};
    onlyForward.Apply(q);
    SA_CHECK_NEAR(q.dipoleWeight, cfg.voiceDipoleWeight, 1e-6);
    SA_CHECK_NEAR(q.forward.y, 1.f, 1e-6);

    // Lua can force a weapon back to omni.
    SoundOverride omni;
    omni.fields = SoundOverride::kDipoleWeight;
    omni.dipoleWeight = 0.f;
    omni.Apply(p);
    SA_CHECK_NEAR(p.dipoleWeight, 0.f, 1e-6);
}

SA_TEST(AutoDirectivity_EntityFacingFromOccluderSnapshot)
{
    DynamicOccluders occ;
    DynamicOccluderOptions opts;
    opts.enabled = false; // the orientation cache must work even with geometry off
    occ.Configure(opts);
    NullSink sink;

    std::vector<EntitySnapshot> ents = {Ent(12, {0.f, 90.f, 0.f}), Ent(13, {0.f, 0.f, 0.f}, true),
                                        Ent(14, {std::nanf(""), 0.f, 0.f})};
    occ.Update(ents, {}, false, sink);

    Vec3 angles;
    SA_CHECK(occ.EntityAngles(12, angles));
    SA_CHECK_NEAR(angles.y, 90.f, 1e-6);
    SA_CHECK(!occ.EntityAngles(13, angles)); // dormant: stale on the client
    SA_CHECK(!occ.EntityAngles(14, angles)); // non-finite
    SA_CHECK(!occ.EntityAngles(99, angles)); // unknown

    // Yaw 90 faces +Y in Source space, the same convention EngineHooks uses
    // through Transform::FromAngles.
    const Vec3 fwd = Transform::FromAngles(Vec3{}, angles = Vec3{0.f, 90.f, 0.f}).forward;
    SA_CHECK_NEAR(fwd.x, 0.f, 1e-5);
    SA_CHECK_NEAR(fwd.y, 1.f, 1e-5);

    // Each snapshot replaces the cache: a vanished entity no longer resolves.
    ents.erase(ents.begin());
    occ.Update(ents, {}, false, sink);
    SA_CHECK(!occ.EntityAngles(12, angles));
    occ.Clear(sink);
    SA_CHECK(!occ.EntityAngles(13, angles));
}
