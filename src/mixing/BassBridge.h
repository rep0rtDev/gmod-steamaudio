// src/mixing/BassBridge.h
//
// Bridges IGModAudioChannel streams (sound.PlayFile / sound.PlayURL) into the
// Steam Audio pipeline. Garry's Mod plays those through un4seen BASS, which
// owns its own output device and never touches the Source mixer, so the
// engine-side channel hooks cannot see them.
//
// Strategy:
//   * detour the BASS stream constructors (BASS_StreamCreateFile/URL/FileUser/
//     BASS_StreamCreate). Every new playback stream gets a lowest-priority
//     BASS DSP callback attached (runs after any DSP GMod installs itself);
//   * the DSP callback (BASS mixer thread) converts the decoded block to
//     float, pushes it into a SoundSource stream ring consumed by our audio
//     thread, and then zeroes the block so BASS's own device outputs silence;
//   * detoured BASS_ChannelSet3DPosition / Set3DAttributes / SetAttribute(VOL)
//     / ChannelPlay / StreamFree keep the SoundSource parameters and lifetime
//     in sync with what the Lua side does to the IGModAudioChannel;
//   * Tick() (game thread) registers new sources with the simulation/audio
//     threads, publishes parameter changes and retires dead handles.
//
// The bridge is inert (all API calls no-ops) when bass.dll is not loaded in
// the process, when detours are unsupported, or when disabled by config.
//
// Thread-safety:
//   m_mutex          guards the channel table (hook threads + game thread)
//   Channel::lock    guards the SoundSource swap between game thread and the
//                    BASS DSP thread (DSP uses try_lock and skips a block when
//                    contended - never blocks the BASS mixer)
//   SoundSource ring SPSC: producer BASS DSP thread, consumer audio thread
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/Detour.h"
#include "mixing/BassAbi.h"
#include "mixing/ProceduralAudioBridge.h"
#include "mixing/SoundSource.h"
#include "steamaudio/Config.h"

namespace sa {

struct BassBridgeOptions {
    std::string moduleName = "bass.dll";
    // How BASS_3DVECTOR from IGModAudioChannel:SetPos maps to Source units:
    //   0 => components are already Source world coordinates (x,y,z)
    //   1 => BASS left-handed (x right, y up, z forward) => Source (z, -x, y)
    int32_t positionSpace = 2;
    // Distance model applied when the Lua side never called Set3DFadeDistance
    // (BASS default min distance of 1 unit would be inaudible past a metre).
    float defaultSoundLevelDb = 75.f;
    float ringSeconds = 0.5f;
    bool muteBassOutput = true;

    Vec3 ToSource(const bass::Vector3D& v) const
    {
        if (positionSpace == 1)
            return {v.z, -v.x, v.y};
        if (positionSpace == 2)
            return {v.x, -v.y, v.z};
        return {v.x, v.y, v.z};
    }
};

struct BassBridgeStats {
    std::atomic<uint32_t> channels{0};
    std::atomic<uint32_t> spatialChannels{0};
    std::atomic<uint64_t> capturedFrames{0};
    std::atomic<uint64_t> droppedFrames{0};
    std::atomic<uint64_t> skippedBlocks{0};
    std::atomic<uint32_t> hooksInstalled{0};
};

class BassBridge {
public:
    BassBridge();
    ~BassBridge();
    BassBridge(const BassBridge&) = delete;
    BassBridge& operator=(const BassBridge&) = delete;

    // Resolves bass exports and installs detours. Returns false when BASS is
    // not (yet) loaded or detours are unavailable; TryAttach() may be called
    // later (Tick does so automatically) to retry.
    bool Initialize(uint32_t engineSampleRate, const BassBridgeOptions& options, ProceduralSourceHooks hooks,
                    std::string& error);
    void Shutdown();

    bool TryAttach(std::string& error);
    bool Attached() const { return m_attached.load(std::memory_order_acquire); }

    // Game thread, once per frame.
    void Tick(const RuntimeConfig& cfg);

    void SetCaptureEnabled(bool enabled) { m_capture.store(enabled, std::memory_order_release); }
    bool CaptureEnabled() const { return m_capture.load(std::memory_order_acquire); }

    // Lua-facing helpers: look up the SoundSource for a BASS handle.
    SoundSourcePtr FindSource(bass::HSTREAM handle) const;
    bool SetSourceOverrides(bass::HSTREAM handle, const SourceParams& params);

    const BassBridgeStats& Stats() const { return m_stats; }
    uint32_t BassVersion() const { return m_bassVersion; }
    size_t ChannelCount() const;
    std::vector<std::string> DescribeChannels() const;

private:
    friend struct BassBridgeTestAccess;

    struct Channel {
        bass::HSTREAM handle = 0;
        bass::HDSP dsp = 0;
        bass::ChannelInfo info{};
        uint32_t bytesPerSample = 2;
        bool isFloat = false;
        bool is8Bit = false;
        bool is3D = false;
        std::string name;

        std::mutex lock;
        SoundSourcePtr source;      // active source (nullptr while retired)
        bool registered = false;    // registered with the sim/audio threads
        bool paramsDirty = true;
        bool registrationRejected = false;
        bool fadeSet = false;       // Set3DFadeDistance was called
        SourceParams params;
        bass::Vector3D bassPos{};
        bass::Vector3D bassOrient{};
        float volume = 1.f;
        std::vector<float> scratch; // DSP thread only
        std::atomic<uint64_t> lastBlockStamp{0};
        std::atomic<float> capturedLevel{0.f};
    };
    using ChannelPtr = std::shared_ptr<Channel>;

    struct Api {
        bass::StreamCreateFileFn streamCreateFile = nullptr;
        bass::StreamCreateURLFn streamCreateURL = nullptr;
        bass::StreamCreateFileUserFn streamCreateFileUser = nullptr;
        bass::StreamCreateFn streamCreate = nullptr;
        bass::StreamFreeFn streamFree = nullptr;
        bass::ChannelSetDSPFn channelSetDSP = nullptr;
        bass::ChannelRemoveDSPFn channelRemoveDSP = nullptr;
        bass::ChannelGetInfoFn channelGetInfo = nullptr;
        bass::ChannelIsActiveFn channelIsActive = nullptr;
        bass::ChannelPlayFn channelPlay = nullptr;
        bass::ChannelSet3DPositionFn channelSet3DPosition = nullptr;
        bass::ChannelSet3DAttributesFn channelSet3DAttributes = nullptr;
        bass::ChannelSetAttributeFn channelSetAttribute = nullptr;
        bass::ChannelGetAttributeFn channelGetAttribute = nullptr;
        bass::ErrorGetCodeFn errorGetCode = nullptr;
        bass::GetVersionFn getVersion = nullptr;
    };

    // Trampolines (original functions) filled by Detour::Install.
    struct Originals {
        bass::StreamCreateFileFn streamCreateFile = nullptr;
        bass::StreamCreateURLFn streamCreateURL = nullptr;
        bass::StreamCreateFileUserFn streamCreateFileUser = nullptr;
        bass::StreamCreateFn streamCreate = nullptr;
        bass::StreamFreeFn streamFree = nullptr;
        bass::ChannelPlayFn channelPlay = nullptr;
        bass::ChannelSet3DPositionFn channelSet3DPosition = nullptr;
        bass::ChannelSet3DAttributesFn channelSet3DAttributes = nullptr;
        bass::ChannelSetAttributeFn channelSetAttribute = nullptr;
    };

    bool ResolveApi(std::string& error);
    bool InstallDetours(std::string& error);
    void RemoveDetours();

    void OnStreamCreated(bass::HSTREAM handle, bass::DWORD flags, const char* origin);
    void OnStreamFreed(bass::HSTREAM handle);
    void OnChannelPlay(bass::HSTREAM handle, bool restart);
    void OnPosition(bass::HSTREAM handle, const bass::Vector3D* pos, const bass::Vector3D* orient);
    void On3DAttributes(bass::HSTREAM handle, int32_t mode, float minDist, float maxDist, int32_t inAngle,
                        int32_t outAngle, float outVolume);
    void OnVolume(bass::HSTREAM handle, float volume);

    ChannelPtr FindChannel(bass::HSTREAM handle) const;
    SoundSourcePtr MakeSource(Channel& ch);
    void RetireChannel(Channel& ch, bool eraseFromTable);
    void PublishParams(Channel& ch, const RuntimeConfig& cfg);
    Vec3 ToSourcePosition(const bass::Vector3D& v) const;

    // BASS callbacks.
    static void SA_BASSCALL DspCallback(bass::HDSP handle, bass::DWORD channel, void* buffer, bass::DWORD length,
                                        void* user);
    void ProcessBlock(Channel& ch, void* buffer, bass::DWORD lengthBytes);

    static bass::HSTREAM SA_BASSCALL HookStreamCreateFile(bass::BOOL mem, const void* file, bass::QWORD offset,
                                                          bass::QWORD length, bass::DWORD flags);
    static bass::HSTREAM SA_BASSCALL HookStreamCreateURL(const char* url, bass::DWORD offset, bass::DWORD flags,
                                                         bass::DownloadProc proc, void* user);
    static bass::HSTREAM SA_BASSCALL HookStreamCreateFileUser(bass::DWORD system, bass::DWORD flags,
                                                              const bass::FileProcs* procs, void* user);
    static bass::HSTREAM SA_BASSCALL HookStreamCreate(bass::DWORD freq, bass::DWORD chans, bass::DWORD flags,
                                                      bass::StreamProc proc, void* user);
    static bass::BOOL SA_BASSCALL HookStreamFree(bass::HSTREAM handle);
    static bass::BOOL SA_BASSCALL HookChannelPlay(bass::DWORD handle, bass::BOOL restart);
    static bass::BOOL SA_BASSCALL HookChannelSet3DPosition(bass::DWORD handle, const bass::Vector3D* pos,
                                                           const bass::Vector3D* orient, const bass::Vector3D* vel);
    static bass::BOOL SA_BASSCALL HookChannelSet3DAttributes(bass::DWORD handle, int32_t mode, float min, float max,
                                                             int32_t iangle, int32_t oangle, float outvol);
    static bass::BOOL SA_BASSCALL HookChannelSetAttribute(bass::DWORD handle, bass::DWORD attrib, float value);

    static std::atomic<BassBridge*> s_instance; // set for the lifetime of Initialize..Shutdown

    BassBridgeOptions m_options;
    ProceduralSourceHooks m_hooks;
    Api m_api;
    Originals m_orig;
    std::vector<Detour> m_detours;
    uint32_t m_engineSampleRate = 44100;
    uint32_t m_bassVersion = 0;
    std::atomic<bool> m_attached{false};
    std::atomic<bool> m_capture{true};
    bool m_initialized = false;
    float m_unitsPerMeter = kDefaultUnitsPerMeter;

    mutable std::mutex m_mutex;
    std::unordered_map<bass::HSTREAM, ChannelPtr> m_channels;
    std::vector<ChannelPtr> m_pendingUnregister; // sources whose channel died (game thread drains)
    BassBridgeStats m_stats;
};

} // namespace sa
