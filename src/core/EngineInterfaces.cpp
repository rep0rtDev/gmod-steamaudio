// src/core/EngineInterfaces.cpp
#include "core/EngineInterfaces.h"

#include <algorithm>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "core/RttiLocator.h"
#include "util/Logging.h"

namespace sa {

namespace {

#if defined(_WIN64)
constexpr const char* kArchKey = "x64";
#else
constexpr const char* kArchKey = "x86";
#endif

PatternRule ParseRule(const JsonValue& v)
{
    PatternRule rule;
    if (!v.IsObject())
        return rule;
    rule.pattern = v["pattern"].AsString("");
    rule.offset = v["offset"].AsInt(0);
    rule.length = v["length"].AsInt(0);
    rule.mode = v["mode"].AsString("absolute");
    rule.deref = v["deref"].AsInt(0);
    rule.section = v["section"].AsString(".text");
    return rule;
}

// Selects `node[arch]` when present, otherwise `node` itself.
const JsonValue& ArchNode(const JsonValue& node)
{
    if (node.IsObject() && node.Has(kArchKey))
        return node[kArchKey];
    return node;
}

void ParseSlots(const JsonValue& v, src::AudioDeviceVTable& slots)
{
    if (!v.IsObject())
        return;
    struct Entry {
        const char* key;
        int32_t* field;
    };
    const Entry entries[] = {
        {"isActive", &slots.isActive},               {"init", &slots.init},
        {"shutdown", &slots.shutdown},               {"pause", &slots.pause},
        {"unPause", &slots.unPause},                 {"mixDryVolume", &slots.mixDryVolume},
        {"should3DMix", &slots.should3DMix},         {"stopAllSounds", &slots.stopAllSounds},
        {"paintBegin", &slots.paintBegin},           {"paintEnd", &slots.paintEnd},
        {"spatializeChannel", &slots.spatializeChannel}, {"applyDSPEffects", &slots.applyDSPEffects},
        {"getOutputPosition", &slots.getOutputPosition}, {"clearBuffer", &slots.clearBuffer},
        {"updateListener", &slots.updateListener},   {"mixBegin", &slots.mixBegin},
        {"mixUpsample", &slots.mixUpsample},         {"mix8Mono", &slots.mix8Mono},
        {"mix8Stereo", &slots.mix8Stereo},           {"mix16Mono", &slots.mix16Mono},
        {"mix16Stereo", &slots.mix16Stereo},         {"channelReset", &slots.channelReset},
        {"transferSamples", &slots.transferSamples}, {"deviceName", &slots.deviceName},
        {"deviceChannels", &slots.deviceChannels},   {"deviceSampleBits", &slots.deviceSampleBits},
        {"deviceSampleBytes", &slots.deviceSampleBytes}, {"deviceDmaSpeed", &slots.deviceDmaSpeed},
        {"deviceSampleCount", &slots.deviceSampleCount}, {"isSurround", &slots.isSurround},
        {"isSurroundCenter", &slots.isSurroundCenter}, {"isHeadphone", &slots.isHeadphone},
        {"slotCount", &slots.slotCount},
    };
    for (const Entry& e : entries)
        if (v.Has(e.key))
            *e.field = v[e.key].AsInt(*e.field);
}

void ParseChannelLayout(const JsonValue& v, ChannelLayout& layout)
{
    if (!v.IsObject())
        return;
    layout.origin = v["origin"].AsInt(layout.origin);
    layout.guid = v["guid"].AsInt(layout.guid);
    layout.distMult = v["dist_mult"].AsInt(layout.distMult);
    layout.fvolume = v["fvolume"].AsInt(layout.fvolume);
    layout.masterVol = v["master_vol"].AsInt(layout.masterVol);
    layout.stride = v["stride"].AsInt(layout.stride);
}

} // namespace

// ---------------------------------------------------------------------------
// SignatureConfig
// ---------------------------------------------------------------------------
SignatureConfig SignatureConfig::Defaults()
{
    return SignatureConfig{};
}

bool SignatureConfig::Load(const std::string& path, SignatureConfig& out, std::string& error)
{
    out = Defaults();
    JsonParseResult parsed = ParseJsonFile(path);
    if (!parsed.ok) {
        if (parsed.error.find("open") != std::string::npos || parsed.error.find("read") != std::string::npos) {
            SA_LOGW("[sig] %s not found or unreadable (%s); using built-in defaults", path.c_str(),
                    parsed.error.c_str());
            return true;
        }
        error = "signature file parse error at " + std::to_string(parsed.line) + ":" +
                std::to_string(parsed.column) + ": " + parsed.error;
        return false;
    }
    const JsonValue& root = parsed.value;
    if (!root.IsObject()) {
        error = "signature file root must be an object";
        return false;
    }
    out.engineModule = root["engine_module"].AsString(out.engineModule);
    out.clientModule = root["client_module"].AsString(out.clientModule);
    out.bassModule = root["bass_module"].AsString(out.bassModule);
    out.engineSoundInterface = root["engine_sound_interface"].AsString(out.engineSoundInterface);
    out.allowUnvalidatedDevice = root["allow_unvalidated_device"].AsBool(false);
    out.disableMixerHooks = root["disable_mixer_hooks"].AsBool(false);
    out.disableBassHooks = root["disable_bass_hooks"].AsBool(false);
    out.disableEmitSoundHook = root["disable_emitsound_hook"].AsBool(false);

    const JsonValue& dev = root["audio_device"];
    if (dev.IsObject()) {
        if (dev["rtti_classes"].IsArray()) {
            out.audioDeviceClasses.clear();
            for (const JsonValue& c : dev["rtti_classes"].AsArray())
                if (c.IsString())
                    out.audioDeviceClasses.push_back(c.AsString());
        }
        ParseSlots(ArchNode(dev["vtable_slots"]), out.slots);
        out.expectedSlotCount = ArchNode(dev["expected_slot_count"]).AsInt(out.expectedSlotCount);
        out.audioDevicePointer = ParseRule(ArchNode(dev["g_AudioDevice"]));
    }
    out.paintBuffer = ParseRule(ArchNode(root["paint_buffer"]));
    ParseChannelLayout(ArchNode(root["channel_layout"]), out.channelLayout);
    out.fileSystem.Parse(root["filesystem"]);
    out.fileSystem.ParseSlots(ArchNode(root["filesystem"]["slots"]));
    out.entityList.Parse(root["entity_list"]);
    out.entityList.ParseSlots(ArchNode(root["entity_list"]["slots"]));
    return true;
}

// ---------------------------------------------------------------------------
// EngineInterfaces
// ---------------------------------------------------------------------------
bool EngineInterfaces::Initialize(const SignatureConfig& config, std::string& error)
{
    m_config = config;
    m_engine = GetLoadedModuleImage(config.engineModule);
    if (!m_engine) {
        error = "engine module '" + config.engineModule + "' is not loaded in this process";
        return false;
    }
    m_dedicated = GetLoadedModuleImage("dedicated.dll").has_value();
    SA_LOGI("[engine] %s at %p (%zu bytes, %zu sections)%s", m_engine->name.c_str(),
            reinterpret_cast<void*>(m_engine->base), m_engine->size, m_engine->sections.size(),
            m_dedicated ? " [dedicated server]" : "");

    if (!ResolveEngineSound(error))
        SA_LOGW("[engine] %s", error.c_str());
    error.clear();

    RefreshAudioDevice();

    if (!m_config.paintBuffer.Empty()) {
        SignatureScanner scanner(*m_engine);
        if (auto addr = ResolvePattern(m_config.paintBuffer, scanner)) {
            m_paintBuffer = reinterpret_cast<src::portable_samplepair_t*>(*addr);
            SA_LOGI("[engine] paint buffer resolved at %p", static_cast<void*>(m_paintBuffer));
        } else {
            SA_LOGW("[engine] paint buffer pattern did not resolve; engine output mode unavailable");
        }
    }
    return true;
}

void EngineInterfaces::Shutdown()
{
    m_engineSound = nullptr;
    m_audioDevice = AudioDeviceInfo{};
    m_paintBuffer = nullptr;
    m_engine.reset();
}

bool EngineInterfaces::ResolveEngineSound(std::string& error)
{
#ifdef _WIN32
    HMODULE module = GetModuleHandleA(m_config.engineModule.c_str());
    if (!module) {
        error = "GetModuleHandle(" + m_config.engineModule + ") failed";
        return false;
    }
    auto factory = reinterpret_cast<src::CreateInterfaceFn>(
        reinterpret_cast<void*>(GetProcAddress(module, "CreateInterface")));
    if (!factory) {
        error = "CreateInterface export missing in " + m_config.engineModule;
        return false;
    }
    int rc = 0;
    void* iface = factory(m_config.engineSoundInterface.c_str(), &rc);
    if (!iface) {
        error = "CreateInterface(" + m_config.engineSoundInterface + ") returned null";
        return false;
    }
    m_engineSound = static_cast<src::IEngineSound*>(iface);
    SA_LOGI("[engine] %s = %p", m_config.engineSoundInterface.c_str(), iface);
    return true;
#else
    error = "IEngineSound resolution is only implemented for Windows builds";
    return false;
#endif
}

std::optional<uintptr_t> EngineInterfaces::ResolvePattern(const PatternRule& rule,
                                                          const SignatureScanner& scanner) const
{
    if (rule.Empty())
        return std::nullopt;
    auto pattern = Pattern::Parse(rule.pattern);
    if (!pattern) {
        SA_LOGE("[sig] invalid pattern '%s'", rule.pattern.c_str());
        return std::nullopt;
    }
    auto match = scanner.FindUnique(*pattern, rule.section);
    if (!match) {
        const auto all = scanner.FindAll(*pattern, rule.section, 2);
        SA_LOGW("[sig] pattern '%s' matched %zu times (need exactly 1)", rule.pattern.c_str(), all.size());
        return std::nullopt;
    }
    uintptr_t address = *match;
    if (rule.mode == "relative") {
        auto resolved = SignatureScanner::ResolveRelative(address, static_cast<size_t>(rule.offset),
                                                          static_cast<size_t>(rule.length));
        if (!resolved)
            return std::nullopt;
        address = *resolved;
    } else if (rule.mode == "absolute") {
        auto ptr = SignatureScanner::ReadPointer(address + static_cast<uintptr_t>(rule.offset));
        if (!ptr)
            return std::nullopt;
        address = *ptr;
    } else if (rule.mode == "direct") {
        address += static_cast<uintptr_t>(rule.offset);
    } else {
        SA_LOGE("[sig] unknown pattern mode '%s'", rule.mode.c_str());
        return std::nullopt;
    }
    for (int32_t i = 0; i < rule.deref; ++i) {
        auto ptr = SignatureScanner::ReadPointer(address);
        if (!ptr)
            return std::nullopt;
        address = *ptr;
    }
    return address;
}

bool EngineInterfaces::ResolveAudioDeviceByRtti(AudioDeviceInfo& out) const
{
    RttiLocator rtti(*m_engine);
    for (const std::string& cls : m_config.audioDeviceClasses) {
        auto vt = rtti.FindPrimaryVTable(cls);
        if (!vt)
            continue;
        out.className = cls;
        out.vtable = reinterpret_cast<void**>(vt->vtable);
        out.slotCount = vt->slotCount;
        out.resolvedBy = "rtti";
        // A live object of that class is what the engine is currently using;
        // the null device class is also compiled in, so prefer classes with an
        // instance.
        const auto pointers = rtti.FindObjectPointers(vt->vtable);
        if (!pointers.empty()) {
            auto obj = SignatureScanner::ReadPointer(pointers.front());
            out.object = obj ? reinterpret_cast<void*>(*obj) : nullptr;
            out.objectSlot = pointers.front();
            SA_LOGI("[engine] IAudioDevice: %s vtable %p (%zu slots), object %p via %zu global pointer(s)",
                    cls.c_str(), static_cast<void*>(out.vtable), out.slotCount, out.object, pointers.size());
            return true;
        }
        SA_LOGD("[engine] IAudioDevice candidate %s has no live instance", cls.c_str());
    }
    // Fall back to a vtable without a live instance, if any class matched.
    if (out.vtable) {
        SA_LOGW("[engine] IAudioDevice: using %s vtable %p without a located instance", out.className.c_str(),
                static_cast<void*>(out.vtable));
        return true;
    }
    return false;
}

bool EngineInterfaces::ResolveAudioDeviceByPattern(AudioDeviceInfo& out) const
{
    if (m_config.audioDevicePointer.Empty())
        return false;
    SignatureScanner scanner(*m_engine);
    PatternRule addressRule = m_config.audioDevicePointer;
    addressRule.deref = std::max(0, addressRule.deref - 1);
    auto address = ResolvePattern(addressRule, scanner);
    if (!address)
        return false;
    // `address` is &g_AudioDevice (after the configured dereferences).
    auto object = SignatureScanner::ReadPointer(*address);
    if (!object || *object == 0)
        return false;
    auto vtable = SignatureScanner::ReadPointer(*object);
    if (!vtable || !m_engine->IsReadOnlyDataAddress(*vtable))
        return false;
    auto info = scanner.ExpandVTableFromSlot(*vtable);
    if (!info)
        return false;
    out.object = reinterpret_cast<void*>(*object);
    out.objectSlot = *address;
    out.vtable = reinterpret_cast<void**>(*vtable);
    out.slotCount = info->slotCount;
    out.resolvedBy = "pattern";
    RttiLocator rtti(*m_engine);
    out.className = rtti.ClassNameOfObject(out.object);
    SA_LOGI("[engine] IAudioDevice via pattern: object %p vtable %p (%zu slots) class '%s'", out.object,
            static_cast<void*>(out.vtable), out.slotCount, out.className.c_str());
    return true;
}

bool EngineInterfaces::RefreshAudioDevice()
{
    if (!m_engine)
        return false;
    AudioDeviceInfo info;
    bool found = ResolveAudioDeviceByPattern(info);
    if (!found || !info.object)
        found = ResolveAudioDeviceByRtti(info) || found;
    if (!found) {
        SA_LOGW("[engine] IAudioDevice implementation not found (no RTTI match, no pattern)");
        return false;
    }
    std::string why;
    info.validated = ValidateAudioDevice(info, why);
    if (!info.validated)
        SA_LOGW("[engine] IAudioDevice validation failed: %s", why.c_str());

    const bool changed = info.vtable != m_audioDevice.vtable || info.object != m_audioDevice.object;
    m_audioDevice = info;
    return changed;
}

bool EngineInterfaces::AudioDeviceVTableChanged()
{
    if (!m_engine || m_audioDevice.objectSlot == 0 || !m_audioDevice.vtable)
        return false;
    auto object = SignatureScanner::ReadPointer(m_audioDevice.objectSlot);
    if (!object || *object == 0)
        return false; // device torn down / being recreated; wait for the new one
    void* obj = reinterpret_cast<void*>(*object);
    if (obj == m_audioDevice.object)
        return false;
    auto vtable = SignatureScanner::ReadPointer(*object);
    if (!vtable)
        return false;
    if (reinterpret_cast<void**>(*vtable) == m_audioDevice.vtable) {
        SA_LOGI("[engine] IAudioDevice object recreated (%p -> %p), same vtable", m_audioDevice.object, obj);
        m_audioDevice.object = obj;
        return false;
    }
    SA_LOGW("[engine] IAudioDevice vtable changed (%p -> %p)", static_cast<void*>(m_audioDevice.vtable),
            reinterpret_cast<void*>(*vtable));
    return true;
}

bool EngineInterfaces::ValidateAudioDevice(AudioDeviceInfo& info, std::string& why) const
{
    const src::AudioDeviceVTable& s = m_config.slots;
    if (!info.vtable || info.slotCount == 0) {
        why = "no vtable";
        return false;
    }
    const int32_t needed = std::max({s.mix16Stereo, s.transferSamples, s.deviceDmaSpeed, s.deviceChannels,
                                     s.deviceSampleBits, s.paintBegin, s.spatializeChannel, s.updateListener});
    if (static_cast<size_t>(needed) >= info.slotCount) {
        why = "vtable has " + std::to_string(info.slotCount) + " slots, configured layout needs " +
              std::to_string(needed + 1);
        return false;
    }
    if (m_config.expectedSlotCount > 0 && static_cast<int32_t>(info.slotCount) != m_config.expectedSlotCount)
        SA_LOGW("[engine] IAudioDevice vtable has %zu slots, expected %d; layout may differ", info.slotCount,
                m_config.expectedSlotCount);
    for (int32_t i = 0; i < static_cast<int32_t>(info.slotCount); ++i) {
        if (!m_engine->IsCodeAddress(reinterpret_cast<uintptr_t>(info.vtable[i]))) {
            why = "slot " + std::to_string(i) + " does not point into engine code";
            return false;
        }
    }
    if (!info.object) {
        why = "no live object to probe (vtable only)";
        return m_config.allowUnvalidatedDevice;
    }
    if (!IsMemoryReadable(reinterpret_cast<uintptr_t>(info.object), sizeof(void*))) {
        why = "object pointer unreadable";
        return false;
    }
    const int32_t getters[] = {s.deviceDmaSpeed, s.deviceChannels, s.deviceSampleBits, s.deviceSampleBytes, s.deviceName};
    for (int32_t slot : getters) {
        if (slot < 0 || static_cast<size_t>(slot) >= info.slotCount) {
            why = "device getter slot is outside the vtable";
            return false;
        }
    }
    // Probe the pure getters. They have no side effects in every known
    // IAudioDevice implementation.
    const int32_t rate = CallVirtual<int32_t>(info.object, s.deviceDmaSpeed);
    const int32_t channels = CallVirtual<int32_t>(info.object, s.deviceChannels);
    const int32_t bits = CallVirtual<int32_t>(info.object, s.deviceSampleBits);
    const int32_t bytes = CallVirtual<int32_t>(info.object, s.deviceSampleBytes);
    const bool rateOk = rate == 11025 || rate == 22050 || rate == 44100 || rate == 48000 || rate == 96000;
    const bool channelsOk = channels >= 1 && channels <= 8;
    const bool bitsOk = (bits == 8 || bits == 16 || bits == 24 || bits == 32) && bytes == bits / 8;
    if (!rateOk || !channelsOk || !bitsOk) {
        why = "device getters returned rate=" + std::to_string(rate) + " channels=" + std::to_string(channels) +
              " bits=" + std::to_string(bits) + " bytes=" + std::to_string(bytes) +
              " (slot layout mismatch; override audio_device.vtable_slots in the signature file)";
        return false;
    }
    info.sampleRate = rate;
    info.channels = channels;
    info.sampleBits = bits;
    const char* name = CallVirtual<const char*>(info.object, s.deviceName);
    if (name && IsMemoryReadable(reinterpret_cast<uintptr_t>(name), 1)) {
        size_t len = 0;
        while (len < 128 && IsMemoryReadable(reinterpret_cast<uintptr_t>(name) + len, 1) && name[len] != '\0')
            ++len;
        info.deviceName.assign(name, len);
    }
    SA_LOGI("[engine] IAudioDevice validated: '%s' %d Hz, %d ch, %d bit", info.deviceName.c_str(), rate, channels,
            bits);
    return true;
}

} // namespace sa
