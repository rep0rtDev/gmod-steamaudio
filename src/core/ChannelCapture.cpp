// src/core/ChannelCapture.cpp
#include "core/ChannelCapture.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>

#include "mixing/Dsp.h"
#include "util/Logging.h"
#include "util/SignatureScanner.h"

namespace sa {

// ---------------------------------------------------------------------------
// OffsetVoter
// ---------------------------------------------------------------------------
void OffsetVoter::Vote(int32_t offset)
{
    if (offset < 0 || offset >= kMaxOffset || (offset & 3) != 0)
        return;
    ++m_votes[static_cast<size_t>(offset / 4)];
    ++m_total;
}

int32_t OffsetVoter::Winner(uint32_t minVotes, float minShare) const
{
    if (m_total < minVotes)
        return -1;
    uint32_t best = 0;
    for (uint32_t v : m_votes)
        best = std::max(best, v);
    if (best == 0)
        return -1;
    // Contiguous channel arrays make `offset + k*stride` candidates collect
    // nearly as many votes as the true offset; the smallest strong candidate
    // is the right one.
    const uint32_t threshold = static_cast<uint32_t>(std::ceil(static_cast<float>(best) * minShare));
    for (size_t i = 0; i < m_votes.size(); ++i) {
        if (m_votes[i] >= threshold)
            return static_cast<int32_t>(i * 4);
    }
    return -1;
}

void OffsetVoter::Reset()
{
    m_votes.fill(0);
    m_total = 0;
}

// ---------------------------------------------------------------------------
// ChannelCapture
// ---------------------------------------------------------------------------
ChannelCapture::ChannelCapture()
{
    for (auto& slot : m_slots)
        slot = std::make_unique<CaptureSlot>();
    m_freeSlots.reserve(kSlots);
}

ChannelCapture::~ChannelCapture() = default;

void ChannelCapture::Initialize(const std::vector<SoundSourcePtr>& sources, uint32_t dmaRate)
{
    m_dmaRate = dmaRate == 0 ? 44100 : dmaRate;
    m_scratch.assign(static_cast<size_t>(src::kPaintBufferSize) * 4, 0.f);
    m_hash.fill(HashEntry{});
    m_freeSlots.clear();
    m_stagedSlots.clear();
    m_stagedSlots.reserve(kSlots);
    m_iterationSamples = 0;
    for (uint32_t i = 0; i < kSlots; ++i) {
        CaptureSlot& slot = *m_slots[i];
        slot.source = i < sources.size() ? sources[i] : nullptr;
        slot.enginePtr.store(0, std::memory_order_relaxed);
        slot.generation.store(0, std::memory_order_relaxed);
        slot.lastClock.store(0, std::memory_order_relaxed);
        slot.everMixed.store(false, std::memory_order_relaxed);
        slot.engineGainKnown.store(false, std::memory_order_relaxed);
        ResetSlotMeters(slot);
        slot.hashIndex = UINT32_MAX;
        slot.idleSince = 0;
        slot.stageL.assign(static_cast<size_t>(src::kPaintBufferSize), 0.f);
        slot.stageR.assign(static_cast<size_t>(src::kPaintBufferSize), 0.f);
        ResetStage(slot);
        slot.groupDiv = 1;
        slot.holdL = slot.holdR = 0.f;
        if (slot.source)
            slot.source->ResetTimedRings(0);
        m_freeSlots.push_back(kSlots - 1 - i);
    }
    m_paintClock.store(0, std::memory_order_relaxed);
    m_frontier.store(0, std::memory_order_relaxed);
    m_clockInitialized = false;
    m_lastPaintedTime = 0;
    m_spatializeHead = 0;
    m_spatialize.fill(SpatializeRecord{});
    m_sourceRevision.store(0, std::memory_order_relaxed);
    m_layout = ChannelLayout{};
    m_layoutStable = false;
    m_inferenceRounds = 0;
    m_originVoter.Reset();
    m_guidVoter.Reset();
    m_fvolumeVoter.Reset();
    m_distMultVoter.Reset();
    m_strideVoter.Reset();
    if (m_overrides.origin >= 0)
        m_layout.origin = m_overrides.origin;
    if (m_overrides.guid >= 0)
        m_layout.guid = m_overrides.guid;
    if (m_overrides.distMult >= 0)
        m_layout.distMult = m_overrides.distMult;
    if (m_overrides.fvolume >= 0)
        m_layout.fvolume = m_overrides.fvolume;
    if (m_overrides.masterVol >= 0)
        m_layout.masterVol = m_overrides.masterVol;
    if (m_overrides.stride > 0)
        m_layout.stride = m_overrides.stride;
    m_layoutSnapshot.Store(m_layout);
    m_soundClock.store(0, std::memory_order_relaxed);
    m_mixAhead.store(0, std::memory_order_relaxed);
}

void ChannelCapture::Shutdown()
{
    for (auto& slot : m_slots) {
        slot->enginePtr.store(0, std::memory_order_release);
        slot->source.reset();
    }
}

void ChannelCapture::SetLayoutOverrides(const ChannelLayout& overrides)
{
    m_overrides = overrides;
    if (overrides.origin >= 0) m_layout.origin = overrides.origin;
    if (overrides.guid >= 0) m_layout.guid = overrides.guid;
    if (overrides.distMult >= 0) m_layout.distMult = overrides.distMult;
    if (overrides.fvolume >= 0) m_layout.fvolume = overrides.fvolume;
    if (overrides.masterVol >= 0) m_layout.masterVol = overrides.masterVol;
    if (overrides.stride > 0) m_layout.stride = overrides.stride;
    m_layoutSnapshot.Store(m_layout);
}

// ---- Clock ----------------------------------------------------------------------
void ChannelCapture::OnPaintBegin(int32_t soundtime, int32_t paintedtime, int32_t endtime)
{
    uint64_t next = 0;
    const bool first = !m_clockInitialized;
    if (first) {
        m_clockInitialized = true;
        // Start the 64-bit clock well above zero so "late" arithmetic never underflows.
        next = uint64_t(1) << 32;
    } else {
        const int32_t delta = paintedtime - m_lastPaintedTime; // wrap-safe
        const uint64_t clock = m_paintClock.load(std::memory_order_relaxed);
        if (delta >= 0) {
            next = clock + static_cast<uint64_t>(delta);
        } else {
            // The engine rewound its paint time (sound system reset). Keep our
            // clock monotonic by continuing from the frontier; rings and the
            // render clock stay valid.
            next = m_frontier.load(std::memory_order_relaxed);
            ++m_stats.clockRebases;
        }
    }
    m_lastPaintedTime = paintedtime;

    // Publish the play-cursor clock before the frontier so a render thread
    // that sees the first frontier also sees a valid sound clock.
    const int32_t painted = paintedtime - soundtime; // samples painted but not yet played
    if (painted >= 0 && static_cast<uint64_t>(painted) <= next)
        m_soundClock.store(next - static_cast<uint64_t>(painted), std::memory_order_release);
    const int32_t ahead = endtime - soundtime;
    if (ahead > 0 && ahead <= static_cast<int32_t>(m_dmaRate) * 2)
        m_mixAhead.store(static_cast<uint32_t>(ahead), std::memory_order_release);

    m_paintClock.store(next, std::memory_order_release);
    if (first) {
        for (auto& slot : m_slots)
            if (slot->source)
                slot->source->ResetTimedRings(next);
        m_frontier.store(next, std::memory_order_release);
    }
}

void ChannelCapture::OnMixBegin(int32_t sampleCount)
{
    m_iterationSamples = sampleCount;
    ++m_stats.paintIterations;
}

void ChannelCapture::OnTransferSamples(int32_t end)
{
    if (!m_clockInitialized)
        return;
    const int32_t delta = end - m_lastPaintedTime;
    // The painted-time delta is the iteration length at the DMA rate; MixBegin
    // announced the same number and serves as fallback for a stale `end`.
    FlushStages(delta > 0 ? delta : m_iterationSamples);
    m_iterationSamples = 0;
    m_lastPaintedTime = end;
    const uint64_t clock = m_paintClock.load(std::memory_order_relaxed);
    const uint64_t next = delta >= 0 ? clock + static_cast<uint64_t>(delta) : clock - static_cast<uint64_t>(-delta);
    m_paintClock.store(next, std::memory_order_release);
    if (next > m_frontier.load(std::memory_order_relaxed))
        m_frontier.store(next, std::memory_order_release);

    // Mark every bound channel's ring frontier so the audio thread can tell
    // "not mixed this iteration" (silence) from "not yet painted".
    for (uint32_t i = 0; i < kSlots; ++i) {
        CaptureSlot& slot = *m_slots[i];
        if (slot.enginePtr.load(std::memory_order_relaxed) == 0 || !slot.source)
            continue;
        for (uint32_t c = 0; c < 2; ++c)
            if (TimedSampleRing* ring = slot.source->TimedRing(c))
                ring->AdvanceFrontier(next);
        // Channels that stop being mixed are released after a grace period.
        if (slot.lastClock.load(std::memory_order_relaxed) + kReuseIdleSamples < next) {
            if (slot.idleSince == 0)
                slot.idleSince = next;
            else if (next - slot.idleSince > kReuseIdleSamples * 4)
                ReleaseSlot(i);
        }
    }
}

void ChannelCapture::OnSpatialize(int32_t* volume, int32_t masterVol, const src::Vector& dir, float gain)
{
    SpatializeRecord& rec = m_spatialize[m_spatializeHead];
    m_spatializeHead = (m_spatializeHead + 1) % static_cast<uint32_t>(m_spatialize.size());
    rec.volumePtr = reinterpret_cast<uintptr_t>(volume);
    rec.masterVol = masterVol;
    rec.gain = gain;
    rec.dir = dir;
    RecordFvolumeAnchor(rec.volumePtr);

    // With a known fvolume offset we can attribute the engine gain to a slot
    // immediately; otherwise CaptureMix matches by pointer window.
    const ChannelLayout layout = Layout();
    if (layout.fvolume >= 0) {
        const uintptr_t ch = rec.volumePtr - static_cast<uintptr_t>(layout.fvolume);
        const uint32_t idx = static_cast<uint32_t>(FindSlotByPointer(ch));
        if (idx != UINT32_MAX) {
            CaptureSlot& slot = *m_slots[idx];
            slot.engineMasterVol.store(static_cast<uint32_t>(std::max(0, masterVol)), std::memory_order_relaxed);
            const bool known = std::isfinite(gain) && gain >= 0.f;
            slot.engineGain.store(known ? std::min(gain, 4.f) : 1.f, std::memory_order_relaxed);
            slot.engineGainKnown.store(known, std::memory_order_release);
            slot.engineDirX.store(dir.x, std::memory_order_relaxed);
            slot.engineDirY.store(dir.y, std::memory_order_relaxed);
            slot.engineDirZ.store(dir.z, std::memory_order_relaxed);
        }
    }
}

void ChannelCapture::RecordFvolumeAnchor(uintptr_t volumePtr)
{
    if (Layout().fvolume >= 0)
        return;
    m_fvolumeAnchors.TryPush(volumePtr);
}

void ChannelCapture::OnChannelReset(int32_t entnum, int32_t channelIndex)
{
    (void)entnum;
    (void)channelIndex;
}

// ---- Slot management (mixer thread) -------------------------------------------------
uint32_t ChannelCapture::LookupOrBind(uintptr_t ch)
{
    if (ch == 0)
        return UINT32_MAX;
    const uint32_t mask = kHashSize - 1;
    uint32_t h = static_cast<uint32_t>((ch >> 4) * 2654435761u) & mask;
    for (uint32_t probe = 0; probe < kHashSize; ++probe) {
        HashEntry& e = m_hash[h];
        if (e.key == ch)
            return e.slot;
        if (e.key == 0)
            break;
        h = (h + 1) & mask;
    }
    return AllocateSlot(ch);
}

uint32_t ChannelCapture::AllocateSlot(uintptr_t ch)
{
    if (m_freeSlots.empty()) {
        // Evict the least recently mixed slot.
        uint32_t victim = UINT32_MAX;
        uint64_t oldest = UINT64_MAX;
        for (uint32_t i = 0; i < kSlots; ++i) {
            const uint64_t last = m_slots[i]->lastClock.load(std::memory_order_relaxed);
            if (last < oldest) {
                oldest = last;
                victim = i;
            }
        }
        if (victim == UINT32_MAX)
            return UINT32_MAX;
        ++m_stats.slotOverflow;
        ReleaseSlot(victim);
    }
    const uint32_t idx = m_freeSlots.back();
    m_freeSlots.pop_back();

    const uint32_t mask = kHashSize - 1;
    uint32_t h = static_cast<uint32_t>((ch >> 4) * 2654435761u) & mask;
    for (uint32_t probe = 0; probe < kHashSize; ++probe) {
        HashEntry& e = m_hash[h];
        if (e.key == 0) {
            e.key = ch;
            e.slot = idx;
            break;
        }
        h = (h + 1) & mask;
    }
    CaptureSlot& slot = *m_slots[idx];
    slot.hashIndex = h;
    slot.idleSince = 0;
    BeginNewSound(idx, ch, m_paintClock.load(std::memory_order_relaxed));
    return idx;
}

void ChannelCapture::ReleaseSlot(uint32_t idx)
{
    CaptureSlot& slot = *m_slots[idx];
    const uintptr_t ch = slot.enginePtr.load(std::memory_order_relaxed);
    if (ch == 0)
        return;
    ResetStage(slot);
    // Remove from the hash with backward-shift deletion to keep probing intact.
    const uint32_t mask = kHashSize - 1;
    uint32_t hole = slot.hashIndex;
    if (hole < kHashSize && m_hash[hole].key == ch) {
        m_hash[hole] = HashEntry{};
        uint32_t next = (hole + 1) & mask;
        while (m_hash[next].key != 0) {
            const uintptr_t key = m_hash[next].key;
            const uint32_t home = static_cast<uint32_t>((key >> 4) * 2654435761u) & mask;
            // Move the entry back if its home is not within (hole, next].
            const bool between = hole <= next ? (home > hole && home <= next) : (home > hole || home <= next);
            if (!between) {
                m_hash[hole] = m_hash[next];
                m_slots[m_hash[hole].slot]->hashIndex = hole;
                m_hash[next] = HashEntry{};
                hole = next;
            }
            next = (next + 1) & mask;
        }
    }
    slot.hashIndex = UINT32_MAX;
    slot.enginePtr.store(0, std::memory_order_release);
    slot.idleSince = 0;
    if (slot.source)
        slot.source->RequestStop();
    m_freeSlots.push_back(idx);
}

void ChannelCapture::BeginNewSound(uint32_t idx, uintptr_t ch, uint64_t clock)
{
    CaptureSlot& slot = *m_slots[idx];
    slot.lastClock.store(clock, std::memory_order_relaxed);
    slot.everMixed.store(false, std::memory_order_relaxed);
    ResetSlotMeters(slot);
    slot.engineGain.store(1.f, std::memory_order_relaxed);
    slot.engineGainKnown.store(false, std::memory_order_relaxed);
    slot.engineMasterVol.store(255, std::memory_order_relaxed);
    slot.enginePtr.store(ch, std::memory_order_release);
    slot.generation.fetch_add(1, std::memory_order_acq_rel);
    slot.groupDiv = 1;
    slot.groupKnown = false;
    slot.lastGuid = 0;
    ReadGuid(ch, slot.lastGuid);
    slot.holdL = slot.holdR = 0.f;
    if (Layout().distMult < 0)
        m_distMultProbes.TryPush(ch);
}

void ChannelCapture::ResetSlotMeters(CaptureSlot& slot)
{
    slot.capturedLevel.store(0.f, std::memory_order_relaxed);
    slot.blocksWritten.store(0, std::memory_order_relaxed);
    slot.blocksLate.store(0, std::memory_order_relaxed);
    slot.blocksDuplicate.store(0, std::memory_order_relaxed);
}

void ChannelCapture::ResetStage(CaptureSlot& slot)
{
    slot.stageEnd = 0;
    slot.stageStereo = false;
    slot.stageRateScaleFix = 0;
}

// ---- Mixing --------------------------------------------------------------------------
void ChannelCapture::CaptureMix(src::channel_t* chPtr, const void* pData, bool is16, bool stereo,
                                int32_t outputOffset, int32_t inputOffset, int32_t rateScaleFix, int32_t outCount)
{
    ++m_stats.mixCalls;
    if (!pData || outCount <= 0 || rateScaleFix <= 0 || !m_clockInitialized)
        return;
    const uintptr_t ch = reinterpret_cast<uintptr_t>(chPtr);
    const uint32_t idx = LookupOrBind(ch);
    if (idx == UINT32_MAX)
        return;
    CaptureSlot& slot = *m_slots[idx];
    if (!slot.source)
        return;

    const uint64_t clock = m_paintClock.load(std::memory_order_relaxed);

    // A channel that went silent for a while and is mixed again is a new sound
    // occupying the same channel_t (the engine recycles them).
    const uint64_t last = slot.lastClock.load(std::memory_order_relaxed);
    int32_t guid = 0;
    const bool changedGuid = ReadGuid(ch, guid) && guid != 0 && slot.lastGuid != 0 && guid != slot.lastGuid;
    if (guid != 0)
        slot.lastGuid = guid;
    if (slot.everMixed.load(std::memory_order_relaxed) && (clock > last + kReuseIdleSamples || changedGuid)) {
        slot.everMixed.store(false, std::memory_order_release);
        slot.generation.fetch_add(1, std::memory_order_acq_rel);
        ResetSlotMeters(slot);
        slot.engineGain.store(1.f, std::memory_order_relaxed);
        slot.engineGainKnown.store(false, std::memory_order_relaxed);
        slot.groupDiv = 1;
        slot.groupKnown = false;
        slot.holdL = slot.holdR = 0.f;
        if (Layout().distMult < 0)
            m_distMultProbes.TryPush(ch);
    }
    slot.idleSince = 0;

    // The engine mixes a channel into exactly one paint buffer per iteration;
    // a second block covering an already staged range is the same PCM routed
    // to another buffer (facing / facing-away, speaker) and must not be summed.
    const int32_t offset = std::max(0, outputOffset);
    if (slot.stageEnd > 0 && offset < slot.stageEnd) {
        ++m_stats.duplicateBlocks;
        slot.blocksDuplicate.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const int32_t stageCapacity = static_cast<int32_t>(slot.stageL.size());
    if (offset >= stageCapacity)
        return;
    const int32_t staged = std::min(outCount, stageCapacity - offset);

    // Attribute the most recent SpatializeChannel record whose volume pointer
    // lies inside this channel_t (window = 1 KiB) when the fvolume offset is
    // still unknown.
    const ChannelLayout layout = Layout();
    const uint32_t recordCount = static_cast<uint32_t>(m_spatialize.size());
    for (uint32_t age = 0; age < recordCount; ++age) {
        const SpatializeRecord& rec = m_spatialize[(m_spatializeHead + recordCount - 1 - age) % recordCount];
        const bool matches = layout.fvolume >= 0 ? rec.volumePtr == ch + static_cast<uintptr_t>(layout.fvolume)
                                                : rec.volumePtr >= ch && rec.volumePtr - ch < 1024;
        if (!matches)
            continue;
        const bool known = std::isfinite(rec.gain) && rec.gain >= 0.f;
        slot.engineMasterVol.store(static_cast<uint32_t>(std::max(0, rec.masterVol)), std::memory_order_relaxed);
        slot.engineGain.store(known ? std::min(rec.gain, 4.f) : 1.f, std::memory_order_relaxed);
        slot.engineGainKnown.store(known, std::memory_order_release);
        slot.engineDirX.store(rec.dir.x, std::memory_order_relaxed);
        slot.engineDirY.store(rec.dir.y, std::memory_order_relaxed);
        slot.engineDirZ.store(rec.dir.z, std::memory_order_relaxed);
        break;
    }

    const size_t count = static_cast<size_t>(staged);
    if (slot.stageEnd == 0) {
        slot.stageStereo = stereo;
        slot.stageRateScaleFix = rateScaleFix;
    } else if (stereo && !slot.stageStereo) {
        std::fill(slot.stageR.begin(), slot.stageR.begin() + slot.stageEnd, 0.f);
        slot.stageStereo = true;
    }
    if (offset > slot.stageEnd) {
        std::fill(slot.stageL.begin() + slot.stageEnd, slot.stageL.begin() + offset, 0.f);
        std::fill(slot.stageR.begin() + slot.stageEnd, slot.stageR.begin() + offset, 0.f);
    }
    if (slot.stageEnd == 0)
        m_stagedSlots.push_back(idx);
    slot.stageEnd = offset + staged;
    float* outL = slot.stageL.data() + offset;
    float* outR = slot.stageR.data() + offset;
    if (!stereo && slot.stageStereo)
        std::fill(outR, outR + count, 0.f);

    // Replicates the engine's fixed-point stepping so our output aligns
    // sample-exactly with what the engine would have mixed; the last input
    // index the engine touches bounds our interpolation reads.
    const int64_t step = static_cast<int64_t>(rateScaleFix);
    int64_t frac = static_cast<int64_t>(inputOffset) & src::kFixMask;
    int64_t index = 0;
    // Pre-compute the final index to know how far we may read.
    int64_t finalIndex = 0;
    {
        int64_t f = frac;
        int64_t i = 0;
        for (size_t n = 0; n + 1 < count; ++n) {
            f += step;
            i += f >> src::kFixShift;
            f &= src::kFixMask;
        }
        finalIndex = i;
    }
    const bool interpolate = step != src::kFixScale;
    const float scale16 = dsp::kInt16Scale;
    const float scale8 = dsp::kInt8Scale;

    for (size_t n = 0; n < count; ++n) {
        const int64_t i0 = index;
        const int64_t i1 = (interpolate && i0 < finalIndex) ? i0 + 1 : i0;
        const float t = interpolate ? static_cast<float>(frac) / static_cast<float>(src::kFixScale) : 0.f;
        if (is16) {
            const int16_t* s = static_cast<const int16_t*>(pData);
            if (stereo) {
                const float l0 = s[i0 * 2] * scale16, l1 = s[i1 * 2] * scale16;
                const float r0 = s[i0 * 2 + 1] * scale16, r1 = s[i1 * 2 + 1] * scale16;
                outL[n] = l0 + (l1 - l0) * t;
                outR[n] = r0 + (r1 - r0) * t;
            } else {
                const float m0 = s[i0] * scale16, m1 = s[i1] * scale16;
                outL[n] = m0 + (m1 - m0) * t;
            }
        } else {
            const int8_t* s = static_cast<const int8_t*>(pData);
            if (stereo) {
                const float l0 = s[i0 * 2] * scale8, l1 = s[i1 * 2] * scale8;
                const float r0 = s[i0 * 2 + 1] * scale8, r1 = s[i1 * 2 + 1] * scale8;
                outL[n] = l0 + (l1 - l0) * t;
                outR[n] = r0 + (r1 - r0) * t;
            } else {
                const float m0 = s[i0] * scale8, m1 = s[i1] * scale8;
                outL[n] = m0 + (m1 - m0) * t;
            }
        }
        frac += step;
        index += frac >> src::kFixShift;
        frac &= src::kFixMask;
    }
    // Committed to the ring by FlushStages once the iteration's rate group is known.
}

namespace {

// Upsamples `count` samples by an integer factor with linear interpolation.
// `hold` is the last sample of the previous block (continuity across
// iterations) and is updated to the last sample of this one.
void UpsampleLinear(const float* in, int32_t count, uint32_t div, float& hold, float* out)
{
    if (div <= 1) {
        std::copy(in, in + count, out);
        if (count > 0)
            hold = in[count - 1];
        return;
    }
    const float invDiv = 1.f / static_cast<float>(div);
    const int32_t total = count * static_cast<int32_t>(div);
    for (int32_t n = 0; n < total; ++n) {
        // Output sample centres map to input position (n + 0.5) / div - 0.5.
        const float x = (static_cast<float>(n) + 0.5f) * invDiv - 0.5f;
        const int32_t i0 = static_cast<int32_t>(std::floor(x));
        const float t = x - static_cast<float>(i0);
        const float s0 = i0 < 0 ? hold : in[i0];
        const int32_t i1 = std::min(i0 + 1, count - 1);
        const float s1 = i1 < 0 ? hold : in[i1];
        out[n] = s0 + (s1 - s0) * t;
    }
    hold = in[count - 1];
}

} // namespace

void ChannelCapture::FlushStages(int32_t iterationSamples)
{
    const uint64_t clock = m_paintClock.load(std::memory_order_relaxed);
    const int32_t n = std::max(0, iterationSamples);
    for (uint32_t idx : m_stagedSlots) {
        CaptureSlot& slot = *m_slots[idx];
        if (slot.stageEnd <= 0 || !slot.source || slot.enginePtr.load(std::memory_order_relaxed) == 0) {
            ResetStage(slot);
            continue;
        }
        const int32_t staged = slot.stageEnd;

        // Full blocks identify the rate group exactly (the engine mixes
        // (end - paintedtime) / (DMA / groupRate) samples per channel);
        // anything shorter is a sound ending or starving and keeps the
        // channel's last known group.
        uint32_t div = slot.groupDiv;
        if (n > 0 && staged == n) {
            div = 1;
            ++m_stats.blocks44k;
        } else if (n > 0 && staged == n / 2 && (!slot.groupKnown || div == 2)) {
            div = 2;
            ++m_stats.blocks22k;
        } else if (n > 0 && staged == n / 4 && (!slot.groupKnown || div == 4)) {
            div = 4;
            ++m_stats.blocks11k;
        } else {
            ++m_stats.blocksPartial;
            if (n > 0 && staged * static_cast<int32_t>(div) > n)
                div = 1;
        }
        slot.groupDiv = div;
        if (n > 0 && staged * static_cast<int32_t>(div) == n)
            slot.groupKnown = true;

        const size_t produced = static_cast<size_t>(staged) * div;
        if (m_scratch.size() < produced * 2)
            m_scratch.resize(produced * 2);
        float* outL = m_scratch.data();
        float* outR = m_scratch.data() + produced;
        UpsampleLinear(slot.stageL.data(), staged, div, slot.holdL, outL);
        if (slot.stageStereo)
            UpsampleLinear(slot.stageR.data(), staged, div, slot.holdR, outR);

        SoundSource& source = *slot.source;
        bool ok = true;
        if (TimedSampleRing* ring = source.TimedRing(0))
            ok = ring->Accumulate(clock, outL, produced) && ok;
        if (slot.stageStereo) {
            if (TimedSampleRing* ring = source.TimedRing(1))
                ok = ring->Accumulate(clock, outR, produced) && ok;
        }
        if (!ok) {
            ++m_stats.lateBlocks;
            slot.blocksLate.fetch_add(1, std::memory_order_relaxed);
        }
        slot.blocksWritten.fetch_add(1, std::memory_order_relaxed);
        {
            double e = 0.0;
            for (size_t i = 0; i < produced; ++i)
                e += static_cast<double>(outL[i]) * outL[i];
            if (slot.stageStereo) {
                for (size_t i = 0; i < produced; ++i)
                    e += static_cast<double>(outR[i]) * outR[i];
                e *= 0.5;
            }
            const float meanSquare = static_cast<float>(e / static_cast<double>(produced));
            const float prev = slot.capturedLevel.load(std::memory_order_relaxed);
            slot.capturedLevel.store(prev + 0.25f * (meanSquare - prev), std::memory_order_relaxed);
        }

        const uint64_t groupRate = static_cast<uint64_t>(m_dmaRate) / div;
        slot.inputChannels.store(slot.stageStereo ? 2u : 1u, std::memory_order_relaxed);
        slot.inputRate.store(
            static_cast<uint32_t>((groupRate * static_cast<uint64_t>(std::max(0, slot.stageRateScaleFix))) >> src::kFixShift),
            std::memory_order_relaxed);
        slot.lastClock.store(clock + produced, std::memory_order_relaxed);
        SourceParams captured;
        const uintptr_t channel = slot.enginePtr.load(std::memory_order_acquire);
        captured.positionValid = ReadOrigin(channel, captured.position) ? 1 : 0;
        captured.spatialize = captured.positionValid;
        captured.gain = static_cast<float>(std::min<uint32_t>(255, slot.engineMasterVol.load())) / 255.f;
        captured.engineGainValid = slot.engineGainKnown.load(std::memory_order_acquire) ? 1 : 0;
        captured.engineDirectGain = slot.engineGain.load(std::memory_order_relaxed);
        if (!ReadDistMult(channel, captured.distMult))
            captured.distMult = SoundLevelToDistMult(static_cast<float>(src::kSndlvlNorm));
        const bool firstBlock = !slot.everMixed.load(std::memory_order_relaxed);
        source.SetCapturedParams(captured, slot.generation.load(std::memory_order_acquire), slot.lastGuid);
        slot.everMixed.store(true, std::memory_order_release);
        if (firstBlock)
            m_sourceRevision.fetch_add(1, std::memory_order_release);
        source.TouchActivity(clock + produced);
        m_stats.samplesCaptured += produced;
        ResetStage(slot);
    }
    m_stagedSlots.clear();
}

// ---- Game-thread helpers -------------------------------------------------------------
int32_t ChannelCapture::FindSlotByPointer(uintptr_t ch) const
{
    for (uint32_t i = 0; i < kSlots; ++i)
        if (m_slots[i]->enginePtr.load(std::memory_order_acquire) == ch)
            return static_cast<int32_t>(i);
    return -1;
}

void ChannelCapture::VoteOriginAnchor(uintptr_t originFieldAddress)
{
    if (m_layout.origin >= 0)
        return;
    for (uint32_t i = 0; i < kSlots; ++i) {
        const uintptr_t ch = m_slots[i]->enginePtr.load(std::memory_order_acquire);
        if (ch == 0 || originFieldAddress < ch)
            continue;
        const uintptr_t diff = originFieldAddress - ch;
        if (diff < static_cast<uintptr_t>(OffsetVoter::kMaxOffset))
            m_originVoter.Vote(static_cast<int32_t>(diff));
    }
}

void ChannelCapture::VoteGuidAnchor(int32_t guid, uintptr_t originFieldAddress)
{
    if (m_layout.guid >= 0 || m_layout.origin < 0 || guid == 0)
        return;
    const uintptr_t ch = originFieldAddress - static_cast<uintptr_t>(m_layout.origin);
    if (FindSlotByPointer(ch) < 0)
        return;
    const size_t window = static_cast<size_t>(m_layout.stride > 0 ? m_layout.stride : OffsetVoter::kMaxOffset);
    if (!IsMemoryReadable(ch, window))
        return;
    for (size_t off = 0; off + sizeof(int32_t) <= window; off += 4) {
        int32_t value;
        std::memcpy(&value, reinterpret_cast<const void*>(ch + off), sizeof(value));
        if (value == guid)
            m_guidVoter.Vote(static_cast<int32_t>(off));
    }
}

bool ChannelCapture::IsCanonicalDistMult(float v)
{
    // SNDLVL_TO_DIST_MULT(sndlvl) = (pow(10, 60/20) / pow(10, sndlvl/20)) / 36
    // for integer sound levels 20..180. dist_mult == 0 means "no attenuation".
    if (!(v > 0.f) || !std::isfinite(v))
        return false;
    // Invert: sndlvl = 60 - 20*log10(v*36)
    const float sndlvl = 60.f - 20.f * std::log10(v * 36.f);
    if (sndlvl < 20.f || sndlvl > 180.f)
        return false;
    const float nearest = std::round(sndlvl);
    const float expected = std::pow(10.f, (60.f - nearest) / 20.f) / 36.f;
    return std::fabs(expected - v) <= expected * 1e-4f;
}

void ChannelCapture::VoteDistMultCandidates(uintptr_t ch)
{
    const size_t window = static_cast<size_t>(m_layout.stride > 0 ? m_layout.stride : OffsetVoter::kMaxOffset);
    if (!IsMemoryReadable(ch, window))
        return;
    for (size_t off = 0; off + sizeof(float) <= window; off += 4) {
        float value;
        std::memcpy(&value, reinterpret_cast<const void*>(ch + off), sizeof(value));
        if (IsCanonicalDistMult(value))
            m_distMultVoter.Vote(static_cast<int32_t>(off));
    }
}

void ChannelCapture::RunLayoutInference()
{
    ++m_inferenceRounds;

    // Stride: gcd of differences between bound channel pointers.
    if (m_layout.stride <= 0) {
        uintptr_t ptrs[kSlots];
        size_t n = 0;
        for (uint32_t i = 0; i < kSlots; ++i) {
            const uintptr_t p = m_slots[i]->enginePtr.load(std::memory_order_acquire);
            if (p)
                ptrs[n++] = p;
        }
        if (n >= 2) {
            std::sort(ptrs, ptrs + n);
            uintptr_t g = 0;
            for (size_t i = 1; i < n; ++i)
                g = std::gcd(g, ptrs[i] - ptrs[i - 1]);
            if (g >= 64 && g <= static_cast<uintptr_t>(OffsetVoter::kMaxOffset))
                m_strideVoter.Vote(static_cast<int32_t>(g));
            const int32_t stride = m_strideVoter.Winner(8, 0.9f);
            if (stride > 0) {
                m_layout.stride = stride;
                SA_LOGI("[capture] inferred sizeof(channel_t) = %d", stride);
            }
        }
    }

    // fvolume anchors from SpatializeChannel.
    uintptr_t anchor;
    while (m_fvolumeAnchors.TryPop(anchor)) {
        if (m_layout.fvolume >= 0)
            continue;
        for (uint32_t i = 0; i < kSlots; ++i) {
            const uintptr_t ch = m_slots[i]->enginePtr.load(std::memory_order_acquire);
            if (ch == 0 || anchor < ch)
                continue;
            const uintptr_t diff = anchor - ch;
            if (diff < static_cast<uintptr_t>(OffsetVoter::kMaxOffset))
                m_fvolumeVoter.Vote(static_cast<int32_t>(diff));
        }
    }

    // dist_mult probes: freshly started channels.
    uintptr_t probe;
    while (m_distMultProbes.TryPop(probe)) {
        if (m_layout.distMult >= 0)
            continue;
        if (FindSlotByPointer(probe) >= 0)
            VoteDistMultCandidates(probe);
    }

    const float share = 0.8f;
    if (m_layout.origin < 0) {
        const int32_t w = m_originVoter.Winner(6, share);
        if (w >= 0 && (m_layout.stride <= 0 || w < m_layout.stride)) {
            m_layout.origin = w;
            SA_LOGI("[capture] inferred channel_t::origin offset = %d", w);
        }
    }
    if (m_layout.fvolume < 0) {
        const int32_t w = m_fvolumeVoter.Winner(6, share);
        if (w >= 0 && (m_layout.stride <= 0 || w < m_layout.stride)) {
            m_layout.fvolume = w;
            SA_LOGI("[capture] inferred channel_t::fvolume offset = %d", w);
        }
    }
    if (m_layout.guid < 0) {
        const int32_t w = m_guidVoter.Winner(6, share);
        if (w >= 0 && (m_layout.stride <= 0 || w < m_layout.stride)) {
            m_layout.guid = w;
            SA_LOGI("[capture] inferred channel_t::guid offset = %d", w);
        }
    }
    if (m_layout.distMult < 0) {
        const int32_t w = m_distMultVoter.Winner(6, share);
        if (w >= 0 && (m_layout.stride <= 0 || w < m_layout.stride)) {
            m_layout.distMult = w;
            SA_LOGI("[capture] inferred channel_t::dist_mult offset = %d", w);
        }
    }
    m_layoutStable = m_layout.origin >= 0 && m_layout.guid >= 0 && m_layout.distMult >= 0;
    m_layoutSnapshot.Store(m_layout);
}

bool ChannelCapture::ReadOrigin(uintptr_t ch, Vec3& out) const
{
    const ChannelLayout layout = Layout();
    if (layout.origin < 0 || ch == 0)
        return false;
    const uintptr_t addr = ch + static_cast<uintptr_t>(layout.origin);
    if (!IsMemoryReadable(addr, sizeof(float) * 3))
        return false;
    float v[3];
    std::memcpy(v, reinterpret_cast<const void*>(addr), sizeof(v));
    if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2]))
        return false;
    out = Vec3{v[0], v[1], v[2]};
    return true;
}

bool ChannelCapture::ReadGuid(uintptr_t ch, int32_t& out) const
{
    const ChannelLayout layout = Layout();
    if (layout.guid < 0 || ch == 0)
        return false;
    const uintptr_t addr = ch + static_cast<uintptr_t>(layout.guid);
    if (!IsMemoryReadable(addr, sizeof(int32_t)))
        return false;
    std::memcpy(&out, reinterpret_cast<const void*>(addr), sizeof(out));
    return true;
}

bool ChannelCapture::ReadDistMult(uintptr_t ch, float& out) const
{
    const ChannelLayout layout = Layout();
    if (layout.distMult < 0 || ch == 0)
        return false;
    const uintptr_t addr = ch + static_cast<uintptr_t>(layout.distMult);
    if (!IsMemoryReadable(addr, sizeof(float)))
        return false;
    float v;
    std::memcpy(&v, reinterpret_cast<const void*>(addr), sizeof(v));
    if (!std::isfinite(v) || v < 0.f || v > 10.f)
        return false;
    out = v;
    return true;
}

} // namespace sa
