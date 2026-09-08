// tests/TestSoundOverrides.cpp
#include <string>
#include <vector>

#include "TestFramework.h"
#include "core/SoundOverrides.h"
#include "mixing/SoundSource.h"

using namespace sa;

namespace {

SoundOverride GainOnly(float gain)
{
    SoundOverride ov;
    ov.fields = SoundOverride::kGain;
    ov.gain = gain;
    return ov;
}

SoundOverride ReverbOnly(float reverb)
{
    SoundOverride ov;
    ov.fields = SoundOverride::kReverbGain;
    ov.reverbGain = reverb;
    return ov;
}

SourceParams EngineParams()
{
    SourceParams p;
    p.gain = 0.5f;
    p.distMult = 0.8f;
    p.radiusMeters = 0.3f;
    p.position = Vec3{1.f, 2.f, 3.f};
    p.positionValid = 1;
    p.spatialize = 1;
    p.occlusion = 1;
    p.transmission = 1;
    p.reflections = 1;
    p.pathing = 1;
    return p;
}

// One "frame": poll observes the given (guid, entity, channel, name) tuples.
struct Seen {
    int32_t guid;
    int32_t entity;
    int32_t channel;
    const char* name;
};

void Poll(SoundOverrideTable& t, double now, const std::vector<Seen>& sounds)
{
    t.BeginPoll(now);
    for (const Seen& s : sounds)
        t.Observe(s.guid, s.entity, s.channel, s.name);
    t.EndPoll();
}

} // namespace

// ---------------------------------------------------------------------------
// SoundOverride value semantics
// ---------------------------------------------------------------------------

SA_TEST(SoundOverride_PartialMergeKeepsEngineValues)
{
    SourceParams p = EngineParams();
    ReverbOnly(0.25f).Apply(p);
    SA_CHECK_NEAR(p.reverbGain, 0.25f, 1e-6);
    // Everything else untouched.
    SA_CHECK_NEAR(p.gain, 0.5f, 1e-6);
    SA_CHECK_NEAR(p.distMult, 0.8f, 1e-6);
    SA_CHECK_NEAR(p.radiusMeters, 0.3f, 1e-6);
    SA_CHECK_EQ(p.positionValid, 1);
    SA_CHECK_NEAR(p.position.x, 1.f, 1e-6);
    SA_CHECK_EQ(p.spatialize, 1);
    SA_CHECK_EQ(p.occlusion, 1);
}

SA_TEST(SoundOverride_GainIsMultiplierAndClamped)
{
    SourceParams p = EngineParams(); // gain 0.5
    GainOnly(2.f).Apply(p);
    SA_CHECK_NEAR(p.gain, 1.f, 1e-6);
    GainOnly(100.f).Apply(p);
    SA_CHECK_NEAR(p.gain, 4.f, 1e-6);
    GainOnly(0.f).Apply(p);
    SA_CHECK_NEAR(p.gain, 0.f, 1e-6);
}

SA_TEST(SoundOverride_PositionMarksValid)
{
    SourceParams p;
    SA_CHECK_EQ(p.positionValid, 0);
    SoundOverride ov;
    ov.fields = SoundOverride::kPosition;
    ov.position = Vec3{10.f, 20.f, 30.f};
    ov.Apply(p);
    SA_CHECK_EQ(p.positionValid, 1);
    SA_CHECK_NEAR(p.position.y, 20.f, 1e-6);
}

SA_TEST(SoundOverride_SpatializeOffDisablesSimulationFlags)
{
    SourceParams p = EngineParams();
    SoundOverride ov;
    ov.fields = SoundOverride::kSpatialize;
    ov.spatialize = 0;
    ov.Apply(p);
    SA_CHECK_EQ(p.spatialize, 0);
    SA_CHECK_EQ(p.occlusion, 0);
    SA_CHECK_EQ(p.transmission, 0);
    SA_CHECK_EQ(p.reflections, 0);
    SA_CHECK_EQ(p.pathing, 0);

    // Flags can be toggled individually while spatialized.
    p = EngineParams();
    ov = SoundOverride{};
    ov.fields = SoundOverride::kReflections | SoundOverride::kPathing;
    ov.reflections = 0;
    ov.pathing = 0;
    ov.Apply(p);
    SA_CHECK_EQ(p.spatialize, 1);
    SA_CHECK_EQ(p.occlusion, 1);
    SA_CHECK_EQ(p.reflections, 0);
    SA_CHECK_EQ(p.pathing, 0);
}

SA_TEST(SoundOverride_MergeReplacesOnlyPresentFields)
{
    SoundOverride a = GainOnly(0.5f);
    a.fields |= SoundOverride::kRadius;
    a.radiusMeters = 1.f;
    SoundOverride b = GainOnly(2.f);
    a.Merge(b);
    SA_CHECK(a.Has(SoundOverride::kGain));
    SA_CHECK(a.Has(SoundOverride::kRadius));
    SA_CHECK_NEAR(a.gain, 2.f, 1e-6);
    SA_CHECK_NEAR(a.radiusMeters, 1.f, 1e-6);
    SA_CHECK(SoundOverride{}.Empty());
    SA_CHECK(!a.Empty());
}

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

SA_TEST(SoundOverride_NormalizeSoundName)
{
    SA_CHECK_STREQ(NormalizeSoundName("Weapons\\AR2\\Fire1.WAV"), "weapons/ar2/fire1.wav");
    SA_CHECK_STREQ(NormalizeSoundName("*#ambient/loop.wav"), "ambient/loop.wav");
    SA_CHECK_STREQ(NormalizeSoundName(")weapons/x.wav"), "weapons/x.wav");
    SA_CHECK_STREQ(NormalizeSoundName("/sound/npc/zombie/moan.wav"), "npc/zombie/moan.wav");
    SA_CHECK_STREQ(NormalizeSoundName("sound/Music/hl2_song1.mp3"), "music/hl2_song1.mp3");
    SA_CHECK_STREQ(NormalizeSoundName(nullptr), "");
    SA_CHECK_STREQ(NormalizeSoundName(""), "");
    // Patterns keep their wildcards.
    SA_CHECK_STREQ(NormalizeSoundPattern("*.MP3"), "*.mp3");
    SA_CHECK_STREQ(NormalizeSoundPattern("sound\\Weapons\\*"), "weapons/*");
    SA_CHECK_STREQ(NormalizeSoundPattern("?"), "?");
}

SA_TEST(SoundOverride_GlobMatch)
{
    SA_CHECK(GlobMatch("weapons/*", "weapons/ar2/fire1.wav"));
    SA_CHECK(GlobMatch("*.mp3", "music/hl2_song1.mp3"));
    SA_CHECK(!GlobMatch("*.mp3", "music/hl2_song1.wav"));
    SA_CHECK(GlobMatch("weapons/ar2/fire?.wav", "weapons/ar2/fire1.wav"));
    SA_CHECK(!GlobMatch("weapons/ar2/fire?.wav", "weapons/ar2/fire12.wav"));
    SA_CHECK(GlobMatch("*", "anything"));
    SA_CHECK(GlobMatch("*", ""));
    SA_CHECK(GlobMatch("WEAPONS/*", "weapons/x.wav"));
    SA_CHECK(GlobMatch("*zombie*", "npc/zombie/moan.wav"));
    SA_CHECK(!GlobMatch("npc/zombie", "npc/zombie/moan.wav"));
    SA_CHECK(!GlobMatch("", "x"));
    SA_CHECK(GlobMatch("", ""));
}

// ---------------------------------------------------------------------------
// Table: matching keys and precedence
// ---------------------------------------------------------------------------

SA_TEST(SoundOverrideTable_NoMatchLeavesParamsAlone)
{
    SoundOverrideTable t;
    Poll(t, 0.0, {{100, 5, 1, "weapons/x.wav"}});
    SoundOverride ov;
    SA_CHECK(!t.Resolve(100, 5, ov));
    SA_CHECK(!t.Resolve(0, -1, ov));
    SA_CHECK(!t.Resolve(999, 5, ov));
}

SA_TEST(SoundOverrideTable_GuidExactMatch)
{
    SoundOverrideTable t;
    t.SetForGuid(100, GainOnly(2.f));
    Poll(t, 0.0, {{100, 5, 1, nullptr}, {101, 5, 1, nullptr}});
    SoundOverride ov;
    SA_CHECK(t.Resolve(100, 5, ov));
    SA_CHECK_NEAR(ov.gain, 2.f, 1e-6);
    SA_CHECK(!t.Resolve(101, 5, ov)); // same entity/channel, different sound
    // Query surface.
    SoundOverride q;
    SA_CHECK(t.OverrideForGuid(100, q));
    SA_CHECK(q.Has(SoundOverride::kGain));
    SA_CHECK(!t.OverrideForGuid(101, q));
}

SA_TEST(SoundOverrideTable_GuidUpdateLayersAndClearRemoves)
{
    SoundOverrideTable t;
    t.SetForGuid(7, GainOnly(0.5f));
    t.SetForGuid(7, ReverbOnly(0.f)); // later registration layers on top
    SoundOverride ov;
    SA_CHECK(t.Resolve(7, -1, ov));
    SA_CHECK(ov.Has(SoundOverride::kGain));
    SA_CHECK(ov.Has(SoundOverride::kReverbGain));
    t.SetForGuid(7, GainOnly(3.f)); // replaces the gain, keeps the reverb
    SA_CHECK(t.Resolve(7, -1, ov));
    SA_CHECK_NEAR(ov.gain, 3.f, 1e-6);
    SA_CHECK_NEAR(ov.reverbGain, 0.f, 1e-6);

    SA_CHECK(t.ClearGuid(7));
    SA_CHECK(!t.ClearGuid(7));
    SA_CHECK(!t.Resolve(7, -1, ov));
}

SA_TEST(SoundOverrideTable_EntityFallback)
{
    SoundOverrideTable t;
    t.SetForEntity(42, ReverbOnly(0.f));
    Poll(t, 0.0, {{1, 42, 1, nullptr}, {2, 42, 6, nullptr}, {3, 43, 1, nullptr}});
    SoundOverride ov;
    SA_CHECK(t.Resolve(1, 42, ov));
    SA_CHECK(t.Resolve(2, 42, ov));
    SA_CHECK(!t.Resolve(3, 43, ov));
    // Unknown GUID (layout-inferred slot with no active-list match) still
    // gets the entity override.
    SA_CHECK(t.Resolve(0, 42, ov));
    SA_CHECK(t.ClearEntity(42));
    SA_CHECK(!t.ClearEntity(42));
    SA_CHECK(!t.Resolve(1, 42, ov));
    // Empty override on SetForEntity clears too.
    t.SetForEntity(42, ReverbOnly(0.f));
    t.SetForEntity(42, SoundOverride{});
    SA_CHECK(!t.Resolve(1, 42, ov));
}

SA_TEST(SoundOverrideTable_NameRulesMatchByGlobAndChannel)
{
    SoundOverrideTable t;
    const uint32_t weapons = t.AddRule("weapons/*", SoundRule::kAnyChannel, 0, GainOnly(0.5f));
    const uint32_t music = t.AddRule("*.mp3", SoundRule::kAnyChannel, 0, ReverbOnly(0.f));
    const uint32_t voice = t.AddRule("*", 2 /* CHAN_VOICE */, 0, ReverbOnly(0.5f));
    SA_CHECK(weapons != 0 && music != 0 && voice != 0);
    SA_CHECK_EQ(t.AddRule("", SoundRule::kAnyChannel, 0, GainOnly(1.f)), 0u);
    SA_CHECK_EQ(t.AddRule("x", SoundRule::kAnyChannel, 0, SoundOverride{}), 0u);

    Poll(t, 0.0,
         {{1, 5, 1, "weapons/ar2/fire1.wav"},
          {2, 5, 1, "music/hl2_song1.mp3"},
          {3, 9, 2, "vo/npc/male01/hi01.wav"},
          {4, 9, 1, "vo/npc/male01/hi02.wav"},
          {5, 9, 1, nullptr}});
    SoundOverride ov;
    SA_CHECK(t.Resolve(1, 5, ov));
    SA_CHECK(ov.Has(SoundOverride::kGain) && !ov.Has(SoundOverride::kReverbGain));
    SA_CHECK(t.Resolve(2, 5, ov));
    SA_CHECK(!ov.Has(SoundOverride::kGain) && ov.Has(SoundOverride::kReverbGain));
    SA_CHECK(t.Resolve(3, 9, ov)); // channel-restricted catch-all
    SA_CHECK_NEAR(ov.reverbGain, 0.5f, 1e-6);
    SA_CHECK(!t.Resolve(4, 9, ov)); // CHAN_WEAPON, not matched by the voice rule
    SA_CHECK(!t.Resolve(5, 9, ov)); // nameless sound: only a bare "*" would match

    std::vector<SoundRule> rules = t.Rules();
    SA_CHECK_EQ(rules.size(), 3u);
    SA_CHECK(t.RemoveRule(weapons));
    SA_CHECK(!t.RemoveRule(weapons));
    Poll(t, 0.1, {{1, 5, 1, "weapons/ar2/fire1.wav"}});
    SA_CHECK(!t.Resolve(1, 5, ov)); // re-evaluated after the rule set changed
    t.ClearRules();
    SA_CHECK(t.Rules().empty());
}

SA_TEST(SoundOverrideTable_NamelessSoundsMatchBareStar)
{
    SoundOverrideTable t;
    t.AddRule("*", SoundRule::kAnyChannel, 0, ReverbOnly(0.f));
    Poll(t, 0.0, {{1, 5, 1, nullptr}});
    SoundOverride ov;
    SA_CHECK(t.Resolve(1, 5, ov));
}

SA_TEST(SoundOverrideTable_RulePriorityOrdering)
{
    SoundOverrideTable t;
    // Registered first but lower priority: loses to the later, higher one.
    t.AddRule("weapons/*", SoundRule::kAnyChannel, 0, GainOnly(0.1f));
    t.AddRule("weapons/ar2/*", SoundRule::kAnyChannel, 10, GainOnly(0.9f));
    // Same priority: later registration wins.
    t.AddRule("*.wav", SoundRule::kAnyChannel, 10, ReverbOnly(0.2f));
    t.AddRule("*.wav", SoundRule::kAnyChannel, 10, ReverbOnly(0.7f));
    Poll(t, 0.0, {{1, 5, 1, "weapons/ar2/fire1.wav"}});
    SoundOverride ov;
    SA_CHECK(t.Resolve(1, 5, ov));
    SA_CHECK_NEAR(ov.gain, 0.9f, 1e-6);
    SA_CHECK_NEAR(ov.reverbGain, 0.7f, 1e-6);
}

SA_TEST(SoundOverrideTable_SpecificBeatsBroad)
{
    SoundOverrideTable t;
    t.AddRule("weapons/*", SoundRule::kAnyChannel, 100, GainOnly(0.1f));
    t.SetForEntity(5, GainOnly(0.5f));
    Poll(t, 0.0, {{1, 5, 1, "weapons/x.wav"}, {2, 5, 1, "weapons/y.wav"}});
    SoundOverride ov;
    // Entity beats rule regardless of rule priority.
    SA_CHECK(t.Resolve(1, 5, ov));
    SA_CHECK_NEAR(ov.gain, 0.5f, 1e-6);
    // GUID beats entity.
    t.SetForGuid(1, GainOnly(0.9f));
    SA_CHECK(t.Resolve(1, 5, ov));
    SA_CHECK_NEAR(ov.gain, 0.9f, 1e-6);
    SA_CHECK(t.Resolve(2, 5, ov));
    SA_CHECK_NEAR(ov.gain, 0.5f, 1e-6);
    // Fields not covered by the specific layer fall through to the broad one.
    t.SetForGuid(2, ReverbOnly(0.f));
    SA_CHECK(t.Resolve(2, 5, ov));
    SA_CHECK_NEAR(ov.gain, 0.5f, 1e-6);
    SA_CHECK_NEAR(ov.reverbGain, 0.f, 1e-6);
}

// ---------------------------------------------------------------------------
// Table: pending "next sound" expectations
// ---------------------------------------------------------------------------

SA_TEST(SoundOverrideTable_ExpectSoundConsumedByFirstNewSound)
{
    SoundOverrideTable t;
    Poll(t, 0.0, {{1, 5, 1, nullptr}}); // already playing: must not consume
    t.ExpectSound(5, SoundRule::kAnyChannel, GainOnly(2.f), 1.f);
    SA_CHECK_EQ(t.GetStats().pending, 1u);
    Poll(t, 0.1, {{1, 5, 1, nullptr}, {2, 5, 1, nullptr}, {3, 5, 1, nullptr}});
    SoundOverride ov;
    SA_CHECK(!t.Resolve(1, 5, ov));
    SA_CHECK(t.Resolve(2, 5, ov));
    SA_CHECK(!t.Resolve(3, 5, ov)); // consumed exactly once
    SA_CHECK_EQ(t.GetStats().pending, 0u);
    SA_CHECK_EQ(t.GetStats().pendingConsumed, 1u);
    // Sticks to the GUID afterwards.
    Poll(t, 0.2, {{2, 5, 1, nullptr}});
    SA_CHECK(t.Resolve(2, 5, ov));
}

SA_TEST(SoundOverrideTable_ExpectSoundChannelFilterAndExpiry)
{
    SoundOverrideTable t;
    Poll(t, 0.0, {});
    t.ExpectSound(5, 6 /* CHAN_STATIC */, GainOnly(2.f), 0.5f);
    Poll(t, 0.1, {{1, 5, 1, nullptr}}); // wrong channel
    SoundOverride ov;
    SA_CHECK(!t.Resolve(1, 5, ov));
    SA_CHECK_EQ(t.GetStats().pending, 1u);
    Poll(t, 0.7, {{2, 5, 6, nullptr}}); // expired before it could match
    SA_CHECK(!t.Resolve(2, 5, ov));
    SA_CHECK_EQ(t.GetStats().pending, 0u);
    SA_CHECK_EQ(t.GetStats().pendingExpired, 1u);
    // Empty override is ignored.
    t.ExpectSound(5, 6, SoundOverride{}, 1.f);
    SA_CHECK_EQ(t.GetStats().pending, 0u);
}

SA_TEST(SoundOverrideTable_ExpectSoundEntityFilter)
{
    SoundOverrideTable t;
    Poll(t, 0.0, {});
    t.ExpectSound(5, SoundRule::kAnyChannel, GainOnly(2.f), 1.f);
    t.ExpectSound(6, SoundRule::kAnyChannel, ReverbOnly(0.f), 1.f);
    Poll(t, 0.1, {{10, 6, 1, nullptr}, {11, 5, 1, nullptr}});
    SoundOverride ov;
    SA_CHECK(t.Resolve(10, 6, ov));
    SA_CHECK(ov.Has(SoundOverride::kReverbGain) && !ov.Has(SoundOverride::kGain));
    SA_CHECK(t.Resolve(11, 5, ov));
    SA_CHECK(ov.Has(SoundOverride::kGain) && !ov.Has(SoundOverride::kReverbGain));
}

// ---------------------------------------------------------------------------
// Table: lifetime
// ---------------------------------------------------------------------------

SA_TEST(SoundOverrideTable_GuidEntriesExpireAfterRetention)
{
    SoundOverrideTable t;
    t.SetForGuid(1, GainOnly(2.f));
    Poll(t, 0.0, {{1, 5, 1, "weapons/x.wav"}});
    SA_CHECK_EQ(t.GetStats().guidEntries, 1u);
    Poll(t, 3.0, {}); // sound stopped; still within retention
    SoundOverride ov;
    SA_CHECK(t.Resolve(1, 5, ov));
    Poll(t, 6.0, {});
    SA_CHECK(!t.Resolve(1, 5, ov));
    SA_CHECK_EQ(t.GetStats().guidEntries, 0u);
    std::string name;
    SA_CHECK(!t.NameForGuid(1, name));
}

SA_TEST(SoundOverrideTable_ClearAllDropsEverythingButKeepsNames)
{
    SoundOverrideTable t;
    t.AddRule("*", SoundRule::kAnyChannel, 0, GainOnly(0.5f));
    t.SetForEntity(5, GainOnly(0.5f));
    t.SetForGuid(1, GainOnly(0.5f));
    t.ExpectSound(5, SoundRule::kAnyChannel, GainOnly(0.5f), 1.f);
    Poll(t, 0.0, {{1, 5, 1, "weapons/x.wav"}});
    t.ClearAll();
    const SoundOverrideTable::Stats s = t.GetStats();
    SA_CHECK_EQ(s.rules, 0u);
    SA_CHECK_EQ(s.entityOverrides, 0u);
    SA_CHECK_EQ(s.pending, 0u);
    SoundOverride ov;
    Poll(t, 0.1, {{1, 5, 1, nullptr}});
    SA_CHECK(!t.Resolve(1, 5, ov));
    std::string name;
    SA_CHECK(t.NameForGuid(1, name));
    SA_CHECK_STREQ(name, "weapons/x.wav");
}

SA_TEST(SoundOverrideTable_NoteEmittedSuppliesNameAndEntity)
{
    SoundOverrideTable t;
    t.AddRule("npc/zombie/*", SoundRule::kAnyChannel, 0, ReverbOnly(0.f));
    // EmitSound hook fires before the poll sees the sound.
    t.NoteEmitted(50, "NPC\\Zombie\\moan1.wav", 12, 2, 0.0);
    std::string name;
    SA_CHECK(t.NameForGuid(50, name));
    SA_CHECK_STREQ(name, "npc/zombie/moan1.wav");
    // Poll without a name (GetActiveSounds has no path) keeps the hooked one
    // and applies the rule.
    Poll(t, 0.05, {{50, 12, 2, nullptr}});
    SoundOverride ov;
    SA_CHECK(t.Resolve(50, 12, ov));
    SA_CHECK_EQ(t.GetStats().namedGuids, 1u);
    // guid 0 (engine failed to start the sound) is ignored.
    t.NoteEmitted(0, "x.wav", 1, 1, 0.1);
    SA_CHECK_EQ(t.GetStats().guidEntries, 1u);
}

SA_TEST(SoundOverrideTable_ChannelReuseDoesNotLeakOverrides)
{
    // Two different sounds played back-to-back from the same entity/channel:
    // the per-GUID override of the first must not reach the second.
    SoundOverrideTable t;
    Poll(t, 0.0, {{1, 5, 1, "a.wav"}});
    t.SetForGuid(1, GainOnly(0.1f));
    Poll(t, 0.1, {{2, 5, 1, "b.wav"}});
    SoundOverride ov;
    SA_CHECK(!t.Resolve(2, 5, ov));
    SA_CHECK(t.Resolve(1, 5, ov));
}

SA_TEST(SoundOverrideTable_BoundedStorage)
{
    SoundOverrideTable t;
    for (int i = 0; i < 600; ++i)
        t.AddRule("r*", SoundRule::kAnyChannel, 0, GainOnly(1.f));
    SA_CHECK_EQ(t.GetStats().rules, static_cast<size_t>(SoundOverrideTable::kMaxRules));
    for (int i = 0; i < 300; ++i)
        t.ExpectSound(i, SoundRule::kAnyChannel, GainOnly(1.f), 30.f);
    SA_CHECK_EQ(t.GetStats().pending, static_cast<size_t>(SoundOverrideTable::kMaxPending));
    std::vector<Seen> many;
    for (int i = 1; i <= 3000; ++i)
        many.push_back({i, 1, 1, nullptr});
    Poll(t, 0.0, many);
    SA_CHECK(t.GetStats().guidEntries <= SoundOverrideTable::kMaxGuidEntries);
    // Recent sounds survive the trim.
    SoundOverride ov;
    t.SetForGuid(3000, GainOnly(2.f));
    SA_CHECK(t.Resolve(3000, 1, ov));
}
