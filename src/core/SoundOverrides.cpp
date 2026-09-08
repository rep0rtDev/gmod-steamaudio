// src/core/SoundOverrides.cpp
#include "core/SoundOverrides.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace sa {

// ---------------------------------------------------------------------------
// SoundOverride
// ---------------------------------------------------------------------------
void SoundOverride::Merge(const SoundOverride& o)
{
    if (o.Has(kGain))
        gain = o.gain;
    if (o.Has(kDistMult))
        distMult = o.distMult;
    if (o.Has(kRadius))
        radiusMeters = o.radiusMeters;
    if (o.Has(kDipoleWeight))
        dipoleWeight = o.dipoleWeight;
    if (o.Has(kDipolePower))
        dipolePower = o.dipolePower;
    if (o.Has(kAirAbsorption))
        airAbsorptionScale = o.airAbsorptionScale;
    if (o.Has(kReverbGain))
        reverbGain = o.reverbGain;
    if (o.Has(kForward))
        forward = o.forward;
    if (o.Has(kPosition))
        position = o.position;
    if (o.Has(kSpatialize))
        spatialize = o.spatialize;
    if (o.Has(kOcclusion))
        occlusion = o.occlusion;
    if (o.Has(kTransmission))
        transmission = o.transmission;
    if (o.Has(kReflections))
        reflections = o.reflections;
    if (o.Has(kPathing))
        pathing = o.pathing;
    fields |= o.fields;
}

void SoundOverride::Apply(SourceParams& p) const
{
    if (Has(kGain))
        p.gain = Clamp(p.gain * gain, 0.f, 4.f);
    if (Has(kDistMult))
        p.distMult = std::max(0.f, distMult);
    if (Has(kRadius))
        p.radiusMeters = radiusMeters;
    if (Has(kDipoleWeight))
        p.dipoleWeight = dipoleWeight;
    if (Has(kDipolePower))
        p.dipolePower = dipolePower;
    if (Has(kAirAbsorption))
        p.airAbsorptionScale = airAbsorptionScale;
    if (Has(kReverbGain))
        p.reverbGain = reverbGain;
    if (Has(kForward))
        p.forward = forward;
    if (Has(kPosition)) {
        p.position = position;
        p.positionValid = 1;
    }
    if (Has(kSpatialize))
        p.spatialize = spatialize;
    if (Has(kOcclusion))
        p.occlusion = occlusion;
    if (Has(kTransmission))
        p.transmission = transmission;
    if (Has(kReflections))
        p.reflections = reflections;
    if (Has(kPathing))
        p.pathing = pathing;
    if (!p.spatialize) {
        p.occlusion = 0;
        p.transmission = 0;
        p.reflections = 0;
        p.pathing = 0;
    }
}

// ---------------------------------------------------------------------------
// Name helpers
// ---------------------------------------------------------------------------
bool GlobMatch(const std::string& pattern, const std::string& text)
{
    const size_t pn = pattern.size(), tn = text.size();
    size_t p = 0, t = 0;
    size_t starP = std::string::npos, starT = 0;
    auto lower = [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
    while (t < tn) {
        if (p < pn && pattern[p] == '*') {
            starP = p++;
            starT = t;
            continue;
        }
        if (p < pn && (pattern[p] == '?' || lower(pattern[p]) == lower(text[t]))) {
            ++p;
            ++t;
            continue;
        }
        if (starP != std::string::npos) {
            p = starP + 1;
            t = ++starT;
            continue;
        }
        return false;
    }
    while (p < pn && pattern[p] == '*')
        ++p;
    return p == pn;
}

namespace {

std::string NormalizePath(const char* s)
{
    std::string out;
    out.reserve(std::strlen(s));
    for (; *s; ++s) {
        char c = *s;
        if (c == '\\')
            c = '/';
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    while (!out.empty() && out.front() == '/')
        out.erase(out.begin());
    if (out.compare(0, 6, "sound/") == 0)
        out.erase(0, 6);
    return out;
}

} // namespace

std::string NormalizeSoundName(const char* sample)
{
    if (!sample)
        return std::string();
    // Sound characters (CHAR_STREAM '*', CHAR_DRYMIX '#', ...) prefix the path.
    while (*sample && std::strchr("*#@><^)(}$!?", *sample))
        ++sample;
    return NormalizePath(sample);
}

std::string NormalizeSoundPattern(const char* pattern)
{
    // Same as NormalizeSoundName but '*' / '?' are wildcards here, so no
    // sound-character stripping.
    return pattern ? NormalizePath(pattern) : std::string();
}

// ---------------------------------------------------------------------------
// SoundOverrideTable: Lua-facing
// ---------------------------------------------------------------------------
void SoundOverrideTable::SetForGuid(int32_t guid, const SoundOverride& ov)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    GuidEntry& e = Entry(guid);
    e.override.Merge(ov);
    e.hasOverride = !e.override.Empty();
    e.lastSeen = m_now;
}

bool SoundOverrideTable::ClearGuid(int32_t guid)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_guids.find(guid);
    if (it == m_guids.end() || !it->second.hasOverride)
        return false;
    it->second.override = SoundOverride{};
    it->second.hasOverride = false;
    return true;
}

void SoundOverrideTable::SetForEntity(int32_t entity, const SoundOverride& ov)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (ov.Empty()) {
        m_entities.erase(entity);
        return;
    }
    m_entities[entity].Merge(ov);
}

bool SoundOverrideTable::ClearEntity(int32_t entity)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_entities.erase(entity) > 0;
}

void SoundOverrideTable::ExpectSound(int32_t entity, int32_t channel, const SoundOverride& ov, float ttlSeconds)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (ov.Empty())
        return;
    if (m_pending.size() >= kMaxPending)
        m_pending.erase(m_pending.begin());
    Pending p;
    p.entity = entity;
    p.channel = channel;
    p.override = ov;
    p.expires = m_now + static_cast<double>(std::isfinite(ttlSeconds) ? Clamp(ttlSeconds, 0.05f, 30.f) : 1.f);
    m_pending.push_back(p);
}

uint32_t SoundOverrideTable::AddRule(const std::string& pattern, int32_t channel, int32_t priority,
                                     const SoundOverride& ov)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (ov.Empty() || pattern.empty() || m_rules.size() >= kMaxRules)
        return 0;
    SoundRule r;
    r.id = m_nextRuleId++;
    r.pattern = NormalizeSoundPattern(pattern.c_str());
    if (r.pattern.empty())
        return 0;
    r.channel = channel;
    r.priority = priority;
    r.override = ov;
    auto pos = std::upper_bound(m_rules.begin(), m_rules.end(), r, [](const SoundRule& a, const SoundRule& b) {
        return a.priority < b.priority || (a.priority == b.priority && a.id < b.id);
    });
    m_rules.insert(pos, r);
    ++m_rulesVersion;
    return r.id;
}

bool SoundOverrideTable::RemoveRule(uint32_t id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = std::find_if(m_rules.begin(), m_rules.end(), [id](const SoundRule& r) { return r.id == id; });
    if (it == m_rules.end())
        return false;
    m_rules.erase(it);
    ++m_rulesVersion;
    return true;
}

void SoundOverrideTable::ClearRules()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_rules.empty())
        return;
    m_rules.clear();
    ++m_rulesVersion;
}

std::vector<SoundRule> SoundOverrideTable::Rules() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_rules;
}

void SoundOverrideTable::ClearAll()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& kv : m_guids) {
        kv.second.override = SoundOverride{};
        kv.second.hasOverride = false;
    }
    m_entities.clear();
    m_pending.clear();
    m_rules.clear();
    ++m_rulesVersion;
}

bool SoundOverrideTable::NameForGuid(int32_t guid, std::string& out) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_guids.find(guid);
    if (it == m_guids.end() || it->second.name.empty())
        return false;
    out = it->second.name;
    return true;
}

bool SoundOverrideTable::OverrideForGuid(int32_t guid, SoundOverride& out) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_guids.find(guid);
    if (it == m_guids.end() || !it->second.hasOverride)
        return false;
    out = it->second.override;
    return true;
}

SoundOverrideTable::Stats SoundOverrideTable::GetStats() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    Stats s;
    s.guidEntries = m_guids.size();
    for (const auto& kv : m_guids)
        if (!kv.second.name.empty())
            ++s.namedGuids;
    s.entityOverrides = m_entities.size();
    s.pending = m_pending.size();
    s.rules = m_rules.size();
    s.rulesMatched = m_rulesMatched;
    s.pendingConsumed = m_pendingConsumed;
    s.pendingExpired = m_pendingExpired;
    return s;
}

// ---------------------------------------------------------------------------
// SoundOverrideTable: engine-facing
// ---------------------------------------------------------------------------
SoundOverrideTable::GuidEntry& SoundOverrideTable::Entry(int32_t guid)
{
    auto it = m_guids.find(guid);
    if (it == m_guids.end()) {
        if (m_guids.size() >= kMaxGuidEntries)
            Trim();
        it = m_guids.emplace(guid, GuidEntry{}).first;
        it->second.lastSeen = m_now;
    }
    return it->second;
}

void SoundOverrideTable::Trim()
{
    // Drop the least recently seen half. Rare (needs >2048 distinct GUIDs
    // within the retention window), so the linear pass is fine.
    std::vector<std::pair<double, int32_t>> order;
    order.reserve(m_guids.size());
    for (const auto& kv : m_guids)
        order.emplace_back(kv.second.lastSeen, kv.first);
    std::sort(order.begin(), order.end());
    for (size_t i = 0; i < order.size() / 2; ++i)
        m_guids.erase(order[i].second);
}

void SoundOverrideTable::EvaluateRules(GuidEntry& e)
{
    e.rulesVersion = m_rulesVersion;
    e.ruleOverride = SoundOverride{};
    if (m_rules.empty())
        return;
    for (const SoundRule& r : m_rules) {
        if (r.channel != SoundRule::kAnyChannel && r.channel != e.channel)
            continue;
        const bool match = e.name.empty() ? r.pattern == "*" : GlobMatch(r.pattern, e.name);
        if (!match)
            continue;
        e.ruleOverride.Merge(r.override);
        ++m_rulesMatched;
    }
}

void SoundOverrideTable::ConsumePending(int32_t guid, GuidEntry& e)
{
    (void)guid;
    for (auto it = m_pending.begin(); it != m_pending.end(); ++it) {
        if (it->entity != e.entity)
            continue;
        if (it->channel != SoundRule::kAnyChannel && it->channel != e.channel)
            continue;
        e.override.Merge(it->override);
        e.hasOverride = !e.override.Empty();
        m_pending.erase(it);
        ++m_pendingConsumed;
        return;
    }
}

void SoundOverrideTable::NoteEmitted(int32_t guid, const char* sample, int32_t entity, int32_t channel, double now)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (guid == 0)
        return;
    m_now = std::max(m_now, now);
    GuidEntry& e = Entry(guid);
    e.lastSeen = m_now;
    e.entity = entity;
    e.channel = channel;
    std::string name = NormalizeSoundName(sample);
    if (!name.empty() && name != e.name) {
        e.name = std::move(name);
        e.rulesVersion = 0; // re-evaluate with the name
    }
}

void SoundOverrideTable::BeginPoll(double now)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_now = std::max(m_now, now);
    // Expire before observing so a stale expectation cannot attach to a
    // sound that starts after its window closed.
    for (auto it = m_pending.begin(); it != m_pending.end();) {
        if (it->expires <= m_now) {
            it = m_pending.erase(it);
            ++m_pendingExpired;
        } else {
            ++it;
        }
    }
}

void SoundOverrideTable::Observe(int32_t guid, int32_t entity, int32_t channel, const char* name)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (guid == 0)
        return;
    GuidEntry& e = Entry(guid);
    e.lastSeen = m_now;
    e.entity = entity;
    e.channel = channel;
    if (name && *name && e.name.empty()) {
        e.name = NormalizeSoundName(name);
        e.rulesVersion = 0;
    }
    if (!e.observed) {
        e.observed = true;
        ConsumePending(guid, e);
    }
    if (e.rulesVersion != m_rulesVersion)
        EvaluateRules(e);
}

bool SoundOverrideTable::Resolve(int32_t guid, int32_t entity, SoundOverride& out) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    SoundOverride combined;
    auto g = m_guids.find(guid);
    if (g != m_guids.end())
        combined.Merge(g->second.ruleOverride);
    auto ent = m_entities.find(entity);
    if (ent != m_entities.end())
        combined.Merge(ent->second);
    if (g != m_guids.end() && g->second.hasOverride)
        combined.Merge(g->second.override);
    if (combined.Empty())
        return false;
    out = combined;
    return true;
}

void SoundOverrideTable::EndPoll()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto it = m_guids.begin(); it != m_guids.end();) {
        if (m_now - it->second.lastSeen > kGuidRetentionSeconds)
            it = m_guids.erase(it);
        else
            ++it;
    }
}

} // namespace sa
