// src/core/EngineInterfaces.h
//
// Resolves everything we need from the running engine process:
//   * engine.dll's CreateInterface  -> IEngineSound (public interface)
//   * the IAudioDevice implementation vtable, found through RTTI class names
//     (CAudioDirectSound, ...) or, failing that, through byte patterns from
//     config/steamaudio_signatures.json that locate the `g_AudioDevice` global
//   * optionally the engine paint buffer (pattern only), used by the "engine"
//     output mode
//   * channel_t layout overrides and IAudioDevice slot overrides from the config
//
// Every resolved address is validated before use (see ValidateAudioDevice),
// and the module refuses to install mixer hooks when validation fails.
//
// Thread-safety: game thread only.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/ChannelCapture.h"
#include "core/ClientEntityList.h"
#include "core/EngineFileSystem.h"
#include "core/SourceInterfaces.h"
#include "util/Json.h"
#include "util/SignatureScanner.h"

namespace sa {

// One pattern-based address resolution rule from the signature file.
struct PatternRule {
    std::string pattern;            // IDA-style "48 8B 05 ? ? ? ?"
    int32_t offset = 0;             // byte offset of the displacement/immediate within the match
    int32_t length = 0;             // instruction length (relative mode)
    std::string mode = "absolute";  // "absolute" | "relative" | "direct" (match address itself)
    int32_t deref = 0;              // number of pointer dereferences to apply to the result
    std::string section = ".text";

    bool Empty() const { return pattern.empty(); }
};

struct SignatureConfig {
    std::string engineModule = "engine.dll";
    std::string clientModule = "client.dll";
    std::string bassModule = "bass.dll";
    std::string engineSoundInterface = src::kEngineSoundInterfaceVersion;
    std::vector<std::string> audioDeviceClasses = {"CAudioDirectSound", "CAudioXAudio2", "CAudioWasapi",
                                                   "CAudioDeviceWave", "CAudioMMSystem", "CAudioDeviceNull"};
    src::AudioDeviceVTable slots;
    int32_t expectedSlotCount = 33;
    PatternRule audioDevicePointer;   // resolves &g_AudioDevice (deref once => object)
    PatternRule paintBuffer;          // resolves the paint buffer array
    ChannelLayout channelLayout;      // -1 fields => infer at runtime
    FileSystemConfig fileSystem;      // IFileSystem access (Workshop / mounted content)
    EntityListConfig entityList;      // IClientEntityList walk (dynamic occluders)
    bool allowUnvalidatedDevice = false;
    bool disableMixerHooks = false;
    bool disableBassHooks = false;
    bool disableEmitSoundHook = false; // IEngineSound::EmitSound vtable hook (sample names for per-sound rules)

    static SignatureConfig Defaults();
    // Loads the JSON file; missing file => defaults, malformed => error.
    static bool Load(const std::string& path, SignatureConfig& out, std::string& error);
};

struct AudioDeviceInfo {
    void* object = nullptr;       // IAudioDevice* (may be null when only the vtable was found)
    void** vtable = nullptr;      // slot 0
    size_t slotCount = 0;
    uintptr_t objectSlot = 0;     // &g_AudioDevice (global holding `object`), 0 if unknown
    std::string className;
    std::string deviceName;
    int32_t sampleRate = 0;
    int32_t channels = 0;
    int32_t sampleBits = 0;
    bool validated = false;
    std::string resolvedBy;       // "rtti" | "pattern"
};

class EngineInterfaces {
public:
    EngineInterfaces() = default;

    bool Initialize(const SignatureConfig& config, std::string& error);
    void Shutdown();

    const SignatureConfig& Config() const { return m_config; }
    const std::optional<ModuleImage>& EngineImage() const { return m_engine; }
    src::IEngineSound* EngineSound() const { return m_engineSound; }
    const AudioDeviceInfo& AudioDevice() const { return m_audioDevice; }
    src::portable_samplepair_t* PaintBuffer() const { return m_paintBuffer; }
    bool IsDedicatedServer() const { return m_dedicated; }
    bool HaveEngineModule() const { return m_engine.has_value(); }

    // Re-resolves the audio device (it is recreated when the engine restarts
    // its sound system, e.g. snd_restart). Returns true when it changed.
    bool RefreshAudioDevice();

    // Cheap per-frame check: re-reads the global that holds the device pointer
    // and reports whether the engine now uses an object with a *different*
    // vtable (a new device class). Same-class recreation (snd_restart) keeps
    // in-place VMT patches valid and only updates the cached object pointer.
    bool AudioDeviceVTableChanged();

    // Calls IAudioDevice::DeviceDmaSpeed etc. on the object through the
    // configured slots and checks that the answers are plausible.
    bool ValidateAudioDevice(AudioDeviceInfo& info, std::string& why) const;

private:
    bool ResolveEngineSound(std::string& error);
    bool ResolveAudioDeviceByRtti(AudioDeviceInfo& out) const;
    bool ResolveAudioDeviceByPattern(AudioDeviceInfo& out) const;
    std::optional<uintptr_t> ResolvePattern(const PatternRule& rule, const SignatureScanner& scanner) const;

    SignatureConfig m_config;
    std::optional<ModuleImage> m_engine;
    src::IEngineSound* m_engineSound = nullptr;
    AudioDeviceInfo m_audioDevice;
    src::portable_samplepair_t* m_paintBuffer = nullptr;
    bool m_dedicated = false;
};

// Calls virtual slot `slot` of `object` using the platform's member-function
// calling convention.
template <typename Ret, typename... Args>
Ret CallVirtual(void* object, int32_t slot, Args... args)
{
#if defined(_WIN32) && !defined(_WIN64)
    using Fn = Ret(__thiscall*)(void*, Args...);
#else
    using Fn = Ret (*)(void*, Args...);
#endif
    void** vtable = *reinterpret_cast<void***>(object);
    return reinterpret_cast<Fn>(vtable[slot])(object, args...);
}

} // namespace sa
