// src/core/SourceInterfaces.h
//
// Minimal, ABI-compatible declarations of the Source engine interfaces this
// module talks to. They mirror the Source SDK 2013 (Multiplayer) public
// headers so the module builds without the SDK checked out; when the SDK is
// available the layouts must match `public/engine/IEngineSound.h`,
// `public/tier1/utlvector.h` and `public/mathlib/vector.h`.
//
// Only virtual-call ABI matters here (slot order, argument order, calling
// convention). Slot indices for the *private* engine mixer interface
// (IAudioDevice) are not part of the public SDK; they default to the 2013
// layout and can be overridden through config/steamaudio_signatures.json.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace sa {
namespace src {

struct Vector {
    float x = 0.f, y = 0.f, z = 0.f;
};

struct QAngle {
    float x = 0.f, y = 0.f, z = 0.f;
};

using FileNameHandle_t = void*;

// soundlevel_t values (const.h). Attenuation is derived from them with
// SNDLVL_TO_DIST_MULT.
enum : int32_t {
    kSndlvlNone = 0,
    kSndlvlIdle = 60,
    kSndlvlTalking = 70,
    kSndlvlNorm = 75,
    kSndlvlGunfire = 140,
    kSndlvlMax = 180,
};

// Sound flags (IEngineSound.h / const.h).
enum : int32_t {
    kSndNoFlags = 0,
    kSndChangeVol = 1 << 0,
    kSndChangePitch = 1 << 1,
    kSndStop = 1 << 2,
    kSndSpawning = 1 << 3,
    kSndDelay = 1 << 4,
    kSndStopLooping = 1 << 5,
    kSndSpeaker = 1 << 6,
    kSndShouldPause = 1 << 7,
    kSndIgnorePhonemes = 1 << 8,
    kSndIgnoreName = 1 << 9,
    kSndDoNotOverwriteExistingOnChannel = 1 << 10,
};

// Entity index used for local (UI / non-positional) sounds.
constexpr int32_t kSoundFromLocalPlayer = -1;
constexpr int32_t kSoundFromWorld = 0;
constexpr int32_t kSoundFromUI = -2;

// public/engine/IEngineSound.h
struct SndInfo_t {
    int32_t m_nGuid;
    FileNameHandle_t m_filenameHandle;
    int32_t m_nSoundSource;
    int32_t m_nChannel;
    int32_t m_nSpeakerEntity;
    float m_flVolume;
    float m_flLastSpatializedVolume;
    float m_flRadius;
    int32_t m_nPitch;
    Vector* m_pOrigin;
    Vector* m_pDirection;
    bool m_bUpdatePositions;
    bool m_bIsSentence;
    bool m_bDryMix;
    bool m_bSpeaker;
    bool m_bSpecialDSP;
    bool m_bFromServer;
};

// public/tier1/utlmemory.h + utlvector.h, laid out so the engine can read
// and append to it. The buffer is marked external (grow size -1) so the
// engine never reallocates it with its own allocator; capacity is sized well
// above MAX_CHANNELS (128).
template <typename T>
class UtlVectorView {
public:
    static constexpr int32_t kExternalBufferMarker = -1;

    explicit UtlVectorView(int32_t capacity)
        : m_pMemory(static_cast<T*>(std::calloc(static_cast<size_t>(capacity), sizeof(T)))),
          m_nAllocationCount(capacity), m_nGrowSize(kExternalBufferMarker), m_Size(0), m_pElements(m_pMemory)
    {
    }
    ~UtlVectorView() { std::free(m_pMemory); }
    UtlVectorView(const UtlVectorView&) = delete;
    UtlVectorView& operator=(const UtlVectorView&) = delete;

    int32_t Count() const { return m_Size; }
    int32_t Capacity() const { return m_nAllocationCount; }
    const T& operator[](int32_t i) const { return m_pMemory[i]; }
    void Clear() { m_Size = 0; }
    bool Valid() const { return m_pMemory != nullptr && m_Size >= 0 && m_Size <= m_nAllocationCount; }

private:
    // CUtlMemory<T>
    T* m_pMemory;
    int32_t m_nAllocationCount;
    int32_t m_nGrowSize;
    // CUtlVector<T>
    int32_t m_Size;
    T* m_pElements;
};

class IRecipientFilter;

// Client-side IEngineSound, interface version "IEngineSoundClient003".
class IEngineSound {
public:
    virtual bool PrecacheSound(const char* pSample, bool bPreload = false, bool bIsUISound = false) = 0;
    virtual bool IsSoundPrecached(const char* pSample) = 0;
    virtual void PrefetchSound(const char* pSample) = 0;
    virtual float GetSoundDuration(const char* pSample) = 0;
    virtual void EmitSound(IRecipientFilter& filter, int iEntIndex, int iChannel, const char* pSample,
                           float flVolume, float flAttenuation, int iFlags = 0, int iPitch = 100,
                           int iSpecialDSP = 0, const Vector* pOrigin = nullptr, const Vector* pDirection = nullptr,
                           void* pUtlVecOrigins = nullptr, bool bUpdatePositions = true, float soundtime = 0.0f,
                           int speakerentity = -1) = 0;
    virtual void EmitSound(IRecipientFilter& filter, int iEntIndex, int iChannel, const char* pSample,
                           float flVolume, int iSoundlevel, int iFlags = 0, int iPitch = 100, int iSpecialDSP = 0,
                           const Vector* pOrigin = nullptr, const Vector* pDirection = nullptr,
                           void* pUtlVecOrigins = nullptr, bool bUpdatePositions = true, float soundtime = 0.0f,
                           int speakerentity = -1) = 0;
    virtual void EmitSentenceByIndex(IRecipientFilter& filter, int iEntIndex, int iChannel, int iSentenceIndex,
                                     float flVolume, int iSoundlevel, int iFlags = 0, int iPitch = 100,
                                     int iSpecialDSP = 0, const Vector* pOrigin = nullptr,
                                     const Vector* pDirection = nullptr, void* pUtlVecOrigins = nullptr,
                                     bool bUpdatePositions = true, float soundtime = 0.0f,
                                     int speakerentity = -1) = 0;
    virtual void StopSound(int iEntIndex, int iChannel, const char* pSample) = 0;
    virtual void StopAllSounds(bool bClearBuffers) = 0;
    virtual void SetRoomType(IRecipientFilter& filter, int roomType) = 0;
    virtual void SetPlayerDSP(IRecipientFilter& filter, int dspType, bool fastReset) = 0;
    virtual void EmitAmbientSound(const char* pSample, float flVolume, int iPitch = 100, int flags = 0,
                                  float soundtime = 0.0f) = 0;
    virtual float GetDistGainFromSoundLevel(int soundlevel, float dist) = 0;
    virtual int GetGuidForLastSoundEmitted() = 0;
    virtual bool IsSoundStillPlaying(int guid) = 0;
    virtual void StopSoundByGuid(int guid) = 0;
    virtual void SetVolumeByGuid(int guid, float fvol) = 0;
    // Retrieves a list of all active sounds. `sndlist` is a CUtlVector<SndInfo_t>&.
    virtual void GetActiveSounds(UtlVectorView<SndInfo_t>& sndlist) = 0;
    virtual void PrecacheSentenceGroup(const char* pGroupName) = 0;
    virtual void NotifyBeginMoviePlayback() = 0;
    virtual void NotifyEndMoviePlayback() = 0;
};

constexpr const char* kEngineSoundInterfaceVersion = "IEngineSoundClient003";

using CreateInterfaceFn = void* (*)(const char* name, int* returnCode);

// channel_t is private to the engine; we only ever hold opaque pointers and
// infer the field offsets we need at runtime (see ChannelLayout).
struct channel_t;

// portable_samplepair_t: the engine paint buffer element (snd_mix_buf.h).
struct portable_samplepair_t {
    int32_t left;
    int32_t right;
};

// Fixed-point sample position format used by the mixers (snd_mix.cpp).
constexpr int32_t kFixShift = 28;
constexpr int64_t kFixScale = int64_t(1) << kFixShift;
constexpr int64_t kFixMask = kFixScale - 1;

// Default IAudioDevice virtual slot indices (engine/audio/private/snd_device.h,
// Source 2013). Overridable through the signature config.
struct AudioDeviceVTable {
    int32_t destructor = 0;
    int32_t isActive = 1;
    int32_t init = 2;
    int32_t shutdown = 3;
    int32_t pause = 4;
    int32_t unPause = 5;
    int32_t mixDryVolume = 6;
    int32_t should3DMix = 7;
    int32_t stopAllSounds = 8;
    int32_t paintBegin = 9;
    int32_t paintEnd = 10;
    int32_t spatializeChannel = 11;
    int32_t applyDSPEffects = 12;
    int32_t getOutputPosition = 13;
    int32_t clearBuffer = 14;
    int32_t updateListener = 15;
    int32_t mixBegin = 16;
    int32_t mixUpsample = 17;
    int32_t mix8Mono = 18;
    int32_t mix8Stereo = 19;
    int32_t mix16Mono = 20;
    int32_t mix16Stereo = 21;
    int32_t channelReset = 22;
    int32_t transferSamples = 23;
    int32_t deviceName = 24;
    int32_t deviceChannels = 25;
    int32_t deviceSampleBits = 26;
    int32_t deviceSampleBytes = 27;
    int32_t deviceDmaSpeed = 28;
    int32_t deviceSampleCount = 29;
    int32_t isSurround = 30;
    int32_t isSurroundCenter = 31;
    int32_t isHeadphone = 32;
    int32_t slotCount = 33;
};

constexpr int32_t kMaxEngineChannels = 128;   // MAX_CHANNELS
constexpr int32_t kChanVolumes = 12;          // CCHANVOLUMES
constexpr int32_t kPaintBufferSize = 512;     // PAINTBUFFER_SIZE (samples per paint iteration)

} // namespace src
} // namespace sa
