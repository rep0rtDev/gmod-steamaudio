// src/core/SoundOverrides.h
//
// Per-sound parameter overrides for sounds the *engine* plays (EmitSound,
// sound.Play, networked sounds, soundscapes...). The engine only tells us
// entity/channel/volume/origin about a channel; addons need a way to say
// "this shot is directional", "this radio ignores occlusion", "everything
// under ambient/* gets a longer reverb" without owning the sound itself.
//
// Overrides are layered from least to most specific and applied on top of the
// parameters derived from the engine metadata in EngineHooks:
//
//   name rules (glob on the sample path, sorted by priority, later wins)
//     < per-entity override (every sound emitted by entity N)
//       < per-sound override (engine GUID; also what a "next sound from
//         entity N" expectation turns into once that sound shows up)
//
// Identity: the engine GUID (SndInfo_t::m_nGuid) is unique per started sound
// and is what Lua receives from steamaudio.GetLastSoundGuid() right after an
// EmitSound call. Sample names come from the IEngineSound::EmitSound hook
// (client-started sounds) or from resolving SndInfo_t::m_filenameHandle
// through IFileSystem (networked sounds; needs the configured slot).
//
// Thread-safety: all methods lock an internal mutex; expected callers are the
// game thread (Lua, PollActiveSounds) and the EmitSound hook. Never called
// from the audio or simulation threads.
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "mixing/SoundSource.h"
#include "util/Math.h"

namespace sa {

struct SoundOverride {
    enum Field : uint32_t {
        kGain = 1u << 0,          // multiplier on the engine volume
        kDistMult = 1u << 1,      // replaces the channel's dist_mult (soundlevel)
        kRadius = 1u << 2,
        kDipoleWeight = 1u << 3,
        kDipolePower = 1u << 4,
        kAirAbsorption = 1u << 5,
        kReverbGain = 1u << 6,
        kForward = 1u << 7,
        kPosition = 1u << 8,
        kSpatialize = 1u << 9,
        kOcclusion = 1u << 10,
        kTransmission = 1u << 11,
        kReflections = 1u << 12,
        kPathing = 1u << 13,
    };

    uint32_t fields = 0;
    float gain = 1.f;
    float distMult = 0.f;
    float radiusMeters = 0.f;
    float dipoleWeight = 0.f;
    float dipolePower = 1.f;
    float airAbsorptionScale = 1.f;
    float reverbGain = 1.f;
    Vec3 forward{1.f, 0.f, 0.f};
    Vec3 position{};
    uint8_t spatialize = 1;
    uint8_t occlusion = 1;
    uint8_t transmission = 1;
    uint8_t reflections = 1;
    uint8_t pathing = 1;

    bool Empty() const { return fields == 0; }
    bool Has(Field f) const { return (fields & f) != 0; }
    // Fields set in `other` replace ours.
    void Merge(const SoundOverride& other);
    void Apply(SourceParams& p) const;
};

struct SoundRule {
    uint32_t id = 0;
    std::string pattern;          // lower-case glob, matched against the normalized sample path
    int32_t channel = kAnyChannel;
    int32_t priority = 0;
    SoundOverride override;

    static constexpr int32_t kAnyChannel = INT32_MIN;
};

// Case-insensitive glob: '*' matches any run (including '/'), '?' one char.
bool GlobMatch(const std::string& pattern, const std::string& text);

// Lower-case, forward slashes, sound-char prefixes ('*', '#', '@', '>', '<',
// '^', ')', '(', '}', '$', '!', '?') stripped, leading "sound/" removed.
std::string NormalizeSoundName(const char* sample);
// Rule patterns: same path normalization, wildcards preserved.
std::string NormalizeSoundPattern(const char* pattern);

class SoundOverrideTable {
public:
    struct ActiveSoundName {
        int32_t guid = 0;
        std::string name;
    };

    struct Stats {
        size_t guidEntries = 0;
        size_t namedGuids = 0;
        size_t entityOverrides = 0;
        size_t pending = 0;
        size_t rules = 0;
        uint64_t rulesMatched = 0;
        uint64_t pendingConsumed = 0;
        uint64_t pendingExpired = 0;
    };

    SoundOverrideTable() = default;

    // ---- Lua-facing ---------------------------------------------------------
    void SetForGuid(int32_t guid, const SoundOverride& ov);
    bool ClearGuid(int32_t guid);
    void SetForEntity(int32_t entity, const SoundOverride& ov);
    bool ClearEntity(int32_t entity);
    // The next sound seen from `entity` (on `channel`, or any when
    // SoundRule::kAnyChannel) takes `ov`. Expires after `ttlSeconds`.
    void ExpectSound(int32_t entity, int32_t channel, const SoundOverride& ov, float ttlSeconds);
    uint32_t AddRule(const std::string& pattern, int32_t channel, int32_t priority, const SoundOverride& ov);
    bool RemoveRule(uint32_t id);
    void ClearRules();
    std::vector<SoundRule> Rules() const;
    void ClearAll();
    bool NameForGuid(int32_t guid, std::string& out) const;
    bool OverrideForGuid(int32_t guid, SoundOverride& out) const;
    Stats GetStats() const;

    // ---- Engine-facing ------------------------------------------------------
    // From the EmitSound hook: the engine just started `guid` for `sample`.
    void NoteEmitted(int32_t guid, const char* sample, int32_t entity, int32_t channel, double now);
    // Once per poll, before Observe()/Resolve(); expires stale expectations.
    void BeginPoll(double now);
    // For every sound in the engine's active list. `name` may be null/empty
    // when unknown. Consumes a matching expectation the first time a GUID is
    // seen and (re)evaluates the name rules when the rule set changed.
    void Observe(int32_t guid, int32_t entity, int32_t channel, const char* name);
    // Combined override for a sound (rules < entity < guid). Returns false
    // when nothing applies. `out` is left untouched in that case.
    bool Resolve(int32_t guid, int32_t entity, SoundOverride& out) const;
    // Forgets GUIDs not seen for kGuidRetentionSeconds.
    void EndPoll();

    static constexpr size_t kMaxGuidEntries = 2048;
    static constexpr size_t kMaxPending = 256;
    static constexpr size_t kMaxRules = 512;
    static constexpr double kGuidRetentionSeconds = 5.0;

private:
    struct GuidEntry {
        std::string name;
        SoundOverride override;        // per-GUID (Lua SetForGuid + consumed expectations)
        SoundOverride ruleOverride;    // merged matching rules
        uint32_t rulesVersion = 0;     // m_rulesVersion the ruleOverride was computed for
        int32_t entity = -1;
        int32_t channel = 0;
        double lastSeen = 0.0;
        bool observed = false;         // seen in an active-sound poll at least once
        bool hasOverride = false;
    };
    struct Pending {
        int32_t entity = -1;
        int32_t channel = SoundRule::kAnyChannel;
        SoundOverride override;
        double expires = 0.0;
    };

    GuidEntry& Entry(int32_t guid);
    void EvaluateRules(GuidEntry& e);
    void ConsumePending(int32_t guid, GuidEntry& e);
    void Trim();

    mutable std::mutex m_mutex;
    std::unordered_map<int32_t, GuidEntry> m_guids;
    std::unordered_map<int32_t, SoundOverride> m_entities;
    std::vector<Pending> m_pending;
    std::vector<SoundRule> m_rules;      // kept sorted by (priority, id)
    uint32_t m_nextRuleId = 1;
    uint32_t m_rulesVersion = 1;
    double m_now = 0.0;
    uint64_t m_rulesMatched = 0;
    uint64_t m_pendingConsumed = 0;
    uint64_t m_pendingExpired = 0;
};

} // namespace sa
