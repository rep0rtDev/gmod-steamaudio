// src/core/ChannelCapture.h
//
// Mixer-thread side of the engine integration. The engine's IAudioDevice
// mixing virtuals (Mix8Mono/Mix8Stereo/Mix16Mono/Mix16Stereo) are redirected
// here. Instead of accumulating into the engine paint buffer we resample the
// channel's PCM exactly like the engine would (28-bit fixed-point step) and
// store it into the per-channel TimedSampleRing at the absolute paint clock,
// where the audio thread picks it up.
//
// The engine mixes one paint iteration (MixBegin .. TransferSamples, up to
// PAINTBUFFER_SIZE samples at the DMA rate) in three rate groups: sources at
// 11025 Hz are mixed into an 11 kHz paint buffer, then that buffer is
// upsampled 2x, 22050 Hz sources are added, upsampled again, and finally the
// 44100 Hz sources are added. The Mix* calls of the low-rate groups therefore
// deliver N/4 or N/2 output samples for an iteration of N DMA samples, with
// rateScaleFix relative to the group rate. The device interface does not say
// which group a call belongs to, so a channel's blocks are staged per
// iteration and classified at TransferSamples by their total length against
// the iteration length (partial blocks, e.g. a sound ending, keep the group
// last seen on that channel), then upsampled to the DMA rate.
//
// The engine's channel_t layout is private. We never dereference it by fixed
// offsets: each channel_t* is mapped to one preallocated slot, and field
// offsets that we do need (origin, guid, dist_mult, fvolume) are inferred at
// runtime by ChannelLayout from anchors the public API gives us
// (SndInfo_t::m_pOrigin, SndInfo_t::m_nGuid, SpatializeChannel's volume
// pointer, the canonical SNDLVL_TO_DIST_MULT value set). The config file can
// pin any of them explicitly.
//
// Thread-safety:
//   * All Capture*/OnPaintBegin/OnTransfer/OnSpatialize calls happen on the
//     engine mixer thread (main thread, or the snd_mix_async worker) and are
//     serialized by the engine. They are the single producer of every ring.
//   * The game thread reads the published per-slot pointers/generations
//     through atomics (Slot::enginePtr, Slot::generation).
//   * The audio thread consumes the rings.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/SourceInterfaces.h"
#include "mixing/LockFreeQueue.h"
#include "mixing/SoundSource.h"

namespace sa {

// Runtime-inferred / configured channel_t field offsets. -1 = unknown.
struct ChannelLayout {
    int32_t origin = -1;    // Vector origin
    int32_t guid = -1;      // int guid
    int32_t distMult = -1;  // float dist_mult
    int32_t fvolume = -1;   // int fvolume[CCHANVOLUMES]
    int32_t masterVol = -1; // int master_vol
    int32_t stride = 0;     // sizeof(channel_t), 0 = unknown

    bool HaveOrigin() const { return origin >= 0; }
    bool HaveGuid() const { return guid >= 0; }
    bool HaveDistMult() const { return distMult >= 0; }
};

// Accumulates evidence for one offset and reports the winner once it has a
// clear majority over a minimum number of observations.
class OffsetVoter {
public:
    static constexpr int32_t kMaxOffset = 2048;
    void Vote(int32_t offset);
    // Returns the winning offset or -1 when the evidence is insufficient.
    int32_t Winner(uint32_t minVotes, float minShare) const;
    void Reset();
    uint32_t Total() const { return m_total; }

private:
    std::array<uint32_t, kMaxOffset / 4> m_votes{}; // offsets are 4-byte aligned
    uint32_t m_total = 0;
};

// Per-slot mixer-side bookkeeping. One slot per engine channel_t.
struct CaptureSlot {
    std::atomic<uintptr_t> enginePtr{0};     // channel_t* currently bound (0 = free)
    std::atomic<uint32_t> generation{0};     // bumped whenever a new sound occupies the channel
    std::atomic<uint64_t> lastClock{0};      // last paint clock written
    std::atomic<uint32_t> inputChannels{1};  // 1 or 2 (last mixer format seen)
    std::atomic<uint32_t> inputRate{0};      // derived from rateScaleFix
    std::atomic<uint32_t> engineMasterVol{255};
    std::atomic<float> engineGain{1.f};      // engine's own distance/obscured gain (fallback path)
    std::atomic<bool> engineGainKnown{false};
    std::atomic<float> engineDirX{0.f};
    std::atomic<float> engineDirY{0.f};
    std::atomic<float> engineDirZ{0.f};
    std::atomic<bool> everMixed{false};
    std::atomic<float> capturedLevel{0.f};   // smoothed mean square of the PCM committed to the ring
    std::atomic<uint32_t> blocksWritten{0};  // ring blocks committed since the sound began
    std::atomic<uint32_t> blocksLate{0};     // blocks (partly) behind the audio thread's read cursor
    std::atomic<uint32_t> blocksDuplicate{0}; // second mix of the same range within one iteration
    SoundSourcePtr source;                   // set once at init, never changes
    uint64_t idleSince = 0;                  // mixer-thread private
    uint32_t hashIndex = UINT32_MAX;         // mixer-thread private
    int32_t lastGuid = 0;
    bool groupKnown = false;

    // Per-iteration staging at the engine's group rate (mixer-thread private).
    std::vector<float> stageL;
    std::vector<float> stageR;
    int32_t stageEnd = 0;                    // samples staged this iteration, 0 = nothing
    bool stageStereo = false;
    int32_t stageRateScaleFix = 0;
    uint32_t groupDiv = 1;                   // DMA rate / group rate of the last full block (1, 2, 4)
    float holdL = 0.f;                       // last staged sample, interpolation history across iterations
    float holdR = 0.f;
};

struct ChannelCaptureStats {
    std::atomic<uint64_t> mixCalls{0};
    std::atomic<uint64_t> samplesCaptured{0};
    std::atomic<uint64_t> paintIterations{0};
    std::atomic<uint64_t> slotOverflow{0};
    std::atomic<uint64_t> lateBlocks{0};
    std::atomic<uint64_t> duplicateBlocks{0}; // same channel mixed twice into one iteration (skipped)
    std::atomic<uint64_t> blocks44k{0};       // iteration blocks per rate group
    std::atomic<uint64_t> blocks22k{0};
    std::atomic<uint64_t> blocks11k{0};
    std::atomic<uint64_t> blocksPartial{0};   // shorter than the iteration; group taken from history
    std::atomic<uint64_t> clockRebases{0};    // engine paint time rewound; clock continued from the frontier
};

class ChannelCapture {
public:
    static constexpr uint32_t kSlots = static_cast<uint32_t>(src::kMaxEngineChannels);
    static constexpr uint32_t kHashSize = 512; // power of two, > 2 * kSlots
    // Channel reused by a new sound if it went idle for this long (paint clock samples).
    static constexpr uint64_t kReuseIdleSamples = 44100 / 4;

    ChannelCapture();
    ~ChannelCapture();
    ChannelCapture(const ChannelCapture&) = delete;
    ChannelCapture& operator=(const ChannelCapture&) = delete;

    // `sources` must contain exactly kSlots EngineChannel sources.
    void Initialize(const std::vector<SoundSourcePtr>& sources, uint32_t dmaRate);
    void Shutdown();

    // ---- Mixer thread ---------------------------------------------------------------
    // `endtime` is the engine's mix target for this update (PaintBegin's
    // return value); pass `paintedtime` when unknown.
    void OnPaintBegin(int32_t soundtime, int32_t paintedtime, int32_t endtime);
    void OnMixBegin(int32_t sampleCount);
    void OnTransferSamples(int32_t end);
    void OnSpatialize(int32_t* volume, int32_t masterVol, const src::Vector& dir, float gain);
    // Mixes one block. `is16` / `stereo` describe pData's format.
    void CaptureMix(src::channel_t* ch, const void* pData, bool is16, bool stereo, int32_t outputOffset,
                    int32_t inputOffset, int32_t rateScaleFix, int32_t outCount);
    // Called by the engine when a channel is (re)initialized (ChannelReset).
    void OnChannelReset(int32_t entnum, int32_t channelIndex);

    // Absolute paint clock (samples at the DMA rate) of the start of the
    // current paint iteration; updated by the mixer thread.
    uint64_t PaintClock() const { return m_paintClock.load(std::memory_order_acquire); }
    // Highest clock the engine has painted up to.
    uint64_t PaintedFrontier() const { return m_frontier.load(std::memory_order_acquire); }
    // Paint clock the engine's device was playing at the last PaintBegin
    // (paintedtime - soundtime behind the paint clock); 0 until known.
    uint64_t SoundClock() const { return m_soundClock.load(std::memory_order_acquire); }
    // Samples the engine keeps painted ahead of its play cursor (snd_mixahead).
    uint32_t MixAheadSamples() const { return m_mixAhead.load(std::memory_order_acquire); }
    uint32_t DmaRate() const { return m_dmaRate; }
    uint64_t SourceRevision() const { return m_sourceRevision.load(std::memory_order_acquire); }

    // ---- Game thread ----------------------------------------------------------------
    CaptureSlot& Slot(uint32_t index) { return *m_slots[index]; }
    const CaptureSlot& Slot(uint32_t index) const { return *m_slots[index]; }
    uint32_t SlotCount() const { return kSlots; }
    // Finds the slot bound to `ch` (game-thread lookup, linear over 128 entries).
    int32_t FindSlotByPointer(uintptr_t ch) const;

    // Layout inference (game thread). Anchors are absolute addresses of fields
    // inside some channel_t; `channelPtrs` are the currently bound channel_t*s.
    void VoteOriginAnchor(uintptr_t originFieldAddress);
    void VoteGuidAnchor(int32_t guid, uintptr_t originFieldAddress);
    void RunLayoutInference();
    ChannelLayout Layout() const { return m_layoutSnapshot.Load(); }
    void SetLayoutOverrides(const ChannelLayout& overrides);
    bool LayoutStable() const { return m_layoutStable; }

    // Reads fields through the inferred layout (returns false when unknown/unreadable).
    bool ReadOrigin(uintptr_t ch, Vec3& out) const;
    bool ReadGuid(uintptr_t ch, int32_t& out) const;
    bool ReadDistMult(uintptr_t ch, float& out) const;

    const ChannelCaptureStats& Stats() const { return m_stats; }

private:
    struct HashEntry {
        uintptr_t key = 0;
        uint32_t slot = UINT32_MAX;
    };

    uint32_t LookupOrBind(uintptr_t ch);
    uint32_t AllocateSlot(uintptr_t ch);
    void ReleaseSlot(uint32_t slot);
    void BeginNewSound(uint32_t slot, uintptr_t ch, uint64_t clock);
    // Commits every slot's staged blocks for the iteration that ends at `end`.
    void FlushStages(int32_t iterationSamples);
    void ResetStage(CaptureSlot& slot);
    static void ResetSlotMeters(CaptureSlot& slot);
    void RecordFvolumeAnchor(uintptr_t volumePtr);
    void VoteDistMultCandidates(uintptr_t ch);
    static bool IsCanonicalDistMult(float v);

    std::array<std::unique_ptr<CaptureSlot>, kSlots> m_slots;
    std::array<HashEntry, kHashSize> m_hash;           // mixer-thread private
    std::vector<float> m_scratch;                      // mixer-thread private
    std::vector<uint32_t> m_freeSlots;                 // mixer-thread private
    struct SpatializeRecord {
        uintptr_t volumePtr = 0;
        int32_t masterVol = 0;
        float gain = 0.f;
        src::Vector dir{};
    };
    std::array<SpatializeRecord, kSlots * 2> m_spatialize; // mixer-thread private ring
    uint32_t m_spatializeHead = 0;
    uint32_t m_dmaRate = 44100;
    int32_t m_iterationSamples = 0;                    // from MixBegin; mixer-thread private
    std::vector<uint32_t> m_stagedSlots;               // slots with stageEnd > 0; mixer-thread private

    alignas(64) std::atomic<uint64_t> m_paintClock{0};
    alignas(64) std::atomic<uint64_t> m_frontier{0};
    alignas(64) std::atomic<uint64_t> m_soundClock{0};
    std::atomic<uint64_t> m_sourceRevision{0};
    std::atomic<uint32_t> m_mixAhead{0};
    int32_t m_lastPaintedTime = 0;
    bool m_clockInitialized = false;

    // Layout inference state. Anchors recorded by the mixer thread are pushed
    // through an SPSC queue to the game thread which owns the voters.
    SpscQueue<uintptr_t> m_fvolumeAnchors{256};
    SpscQueue<uintptr_t> m_distMultProbes{256};
    OffsetVoter m_originVoter;
    OffsetVoter m_guidVoter;
    OffsetVoter m_fvolumeVoter;
    OffsetVoter m_distMultVoter;
    OffsetVoter m_strideVoter;
    ChannelLayout m_layout;
    SeqLock<ChannelLayout> m_layoutSnapshot;
    ChannelLayout m_overrides;
    bool m_layoutStable = false;
    uint32_t m_inferenceRounds = 0;

    ChannelCaptureStats m_stats;
};

} // namespace sa
