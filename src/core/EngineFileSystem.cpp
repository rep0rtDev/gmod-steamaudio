// src/core/EngineFileSystem.cpp
#include "core/EngineFileSystem.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "core/EngineInterfaces.h"
#include "core/SourceInterfaces.h"
#include "util/Logging.h"
#include "util/SignatureScanner.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace sa {

namespace {

constexpr int32_t kSeekHead = 0;
constexpr int32_t kSeekTail = 2;
constexpr uint32_t kMaxProbeSize = 1u << 20;

bool ContainsNoCase(const std::vector<uint8_t>& hay, const std::string& needle)
{
    if (needle.empty() || hay.size() < needle.size())
        return false;
    auto lower = [](uint8_t c) { return static_cast<char>(std::tolower(c)); };
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        size_t k = 0;
        while (k < needle.size() && lower(hay[i + k]) == lower(static_cast<uint8_t>(needle[k])))
            ++k;
        if (k == needle.size())
            return true;
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// FileSystemSlots / FileSystemConfig
// ---------------------------------------------------------------------------
FileSystemSlots FileSystemSlots::Sdk2013(int32_t thisOffset, int32_t firstSlot, bool reversedSizeOverloads)
{
    // IBaseFileSystem declaration order: Read, Write, Open, Close, Seek, Tell,
    // Size(FileHandle_t), Size(const char*), Flush, Precache, FileExists.
    FileSystemSlots s;
    s.thisOffset = thisOffset;
    const int32_t b = firstSlot;
    s.read = b + 0;
    s.open = b + 2;
    s.close = b + 3;
    s.seek = b + 4;
    s.tell = b + 5;
    s.sizeHandle = reversedSizeOverloads ? b + 7 : b + 6;
    s.sizeName = reversedSizeOverloads ? b + 6 : b + 7;
    s.fileExists = b + 10;
    return s;
}

void FileSystemConfig::Parse(const JsonValue& node)
{
    if (!node.IsObject())
        return;
    module = node["module"].AsString(module);
    interfaceVersion = node["interface"].AsString(interfaceVersion);
    pathId = node["path_id"].AsString(pathId);
    probeFile = node["probe_file"].AsString(probeFile);
    probeToken = node["probe_token"].AsString(probeToken);
    disabled = node["disabled"].AsBool(disabled);
    if (node["gma_directories"].IsArray()) {
        gmaDirectories.clear();
        for (const JsonValue& d : node["gma_directories"].AsArray())
            if (d.IsString())
                gmaDirectories.push_back(d.AsString());
    }
}

void FileSystemConfig::ParseSlots(const JsonValue& s)
{
    if (s.IsObject()) {
        slots.thisOffset = s["this_offset"].AsInt(slots.thisOffset);
        slots.read = s["read"].AsInt(slots.read);
        slots.open = s["open"].AsInt(slots.open);
        slots.close = s["close"].AsInt(slots.close);
        slots.seek = s["seek"].AsInt(slots.seek);
        slots.tell = s["tell"].AsInt(slots.tell);
        slots.sizeHandle = s["size_handle"].AsInt(slots.sizeHandle);
        slots.sizeName = s["size_name"].AsInt(slots.sizeName);
        slots.fileExists = s["file_exists"].AsInt(slots.fileExists);
        slots.fileNameString = s["filename_string"].AsInt(slots.fileNameString);
    }
}

// ---------------------------------------------------------------------------
// Raw virtual calls
// ---------------------------------------------------------------------------
void* EngineFileSystem::SubObject(const FileSystemSlots& slots) const
{
    return static_cast<void*>(static_cast<void**>(m_object) + slots.thisOffset);
}

EngineFileSystem::Handle EngineFileSystem::Open(const FileSystemSlots& slots, const char* path,
                                               const char* options) const
{
    return CallVirtual<Handle>(SubObject(slots), slots.open, path, options, m_config.pathId.c_str());
}

void EngineFileSystem::Close(const FileSystemSlots& slots, Handle h) const
{
    CallVirtual<void>(SubObject(slots), slots.close, h);
}

int32_t EngineFileSystem::Read(const FileSystemSlots& slots, Handle h, void* out, int32_t size) const
{
    return CallVirtual<int32_t>(SubObject(slots), slots.read, out, size, h);
}

void EngineFileSystem::Seek(const FileSystemSlots& slots, Handle h, int32_t pos, int32_t whence) const
{
    CallVirtual<void>(SubObject(slots), slots.seek, h, pos, whence);
}

uint32_t EngineFileSystem::Tell(const FileSystemSlots& slots, Handle h) const
{
    return CallVirtual<uint32_t>(SubObject(slots), slots.tell, h);
}

uint32_t EngineFileSystem::Size(const FileSystemSlots& slots, Handle h) const
{
    return CallVirtual<uint32_t>(SubObject(slots), slots.sizeHandle, h);
}

uint32_t EngineFileSystem::Size(const FileSystemSlots& slots, const char* path) const
{
    return CallVirtual<uint32_t>(SubObject(slots), slots.sizeName, path, m_config.pathId.c_str());
}

bool EngineFileSystem::Exists(const FileSystemSlots& slots, const char* path) const
{
    return CallVirtual<bool>(SubObject(slots), slots.fileExists, path, m_config.pathId.c_str());
}

// ---------------------------------------------------------------------------
// Layout sanity
// ---------------------------------------------------------------------------
bool EngineFileSystem::SlotsPlausible(const FileSystemSlots& slots, std::string& why) const
{
    if (!slots.Complete() || slots.thisOffset < 0 || slots.thisOffset > 4) {
        why = "incomplete slot table";
        return false;
    }
    const uintptr_t object = reinterpret_cast<uintptr_t>(m_object);
    const uintptr_t vptrAddress = object + static_cast<uintptr_t>(slots.thisOffset) * sizeof(void*);
    if (!IsMemoryReadable(vptrAddress, sizeof(void*))) {
        why = "object too small for this_offset " + std::to_string(slots.thisOffset);
        return false;
    }
    const uintptr_t vtable = *reinterpret_cast<const uintptr_t*>(vptrAddress);
    if (m_moduleSize && (vtable < m_moduleBase || vtable >= m_moduleBase + m_moduleSize)) {
        why = "vtable at this_offset " + std::to_string(slots.thisOffset) + " is outside " + m_config.module;
        return false;
    }
    const int32_t indices[] = {slots.read,       slots.open,     slots.close,      slots.seek,
                               slots.tell,       slots.sizeHandle, slots.sizeName, slots.fileExists};
    int32_t maxSlot = 0;
    for (int32_t i : indices)
        maxSlot = std::max(maxSlot, i);
    if (maxSlot > 256) {
        why = "slot index " + std::to_string(maxSlot) + " implausible";
        return false;
    }
    if (!IsMemoryReadable(vtable, static_cast<size_t>(maxSlot + 1) * sizeof(void*))) {
        why = "vtable shorter than slot " + std::to_string(maxSlot);
        return false;
    }
    const auto* entries = reinterpret_cast<const uintptr_t*>(vtable);
    for (int32_t i : indices) {
        const uintptr_t fn = entries[i];
        if (fn == 0 || (m_moduleSize && (fn < m_moduleBase || fn >= m_moduleBase + m_moduleSize))) {
            why = "slot " + std::to_string(i) + " does not point into " + m_config.module;
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------
bool EngineFileSystem::Probe(const FileSystemSlots& slots, std::string& why) const
{
    const char* file = m_config.probeFile.c_str();
    const Handle h = Open(slots, file, "rb");
    if (!h) {
        why = "Open(" + m_config.probeFile + ") returned null";
        return false;
    }
    const uint32_t size = Size(slots, h);
    if (size == 0 || size > kMaxProbeSize) {
        Close(slots, h);
        why = "Size(handle) implausible: " + std::to_string(size);
        return false;
    }
    std::vector<uint8_t> buf(size);
    const int32_t got = Read(slots, h, buf.data(), static_cast<int32_t>(size));
    if (got != static_cast<int32_t>(size)) {
        Close(slots, h);
        why = "Read returned " + std::to_string(got) + " of " + std::to_string(size);
        return false;
    }
    if (Tell(slots, h) != size) {
        Close(slots, h);
        why = "Tell after full read != size";
        return false;
    }
    Seek(slots, h, 0, kSeekHead);
    if (Tell(slots, h) != 0) {
        Close(slots, h);
        why = "Tell after Seek(0) != 0";
        return false;
    }
    Seek(slots, h, 0, kSeekTail);
    if (Tell(slots, h) != size) {
        Close(slots, h);
        why = "Tell after Seek(tail) != size";
        return false;
    }
    Close(slots, h);
    if (!ContainsNoCase(buf, m_config.probeToken)) {
        why = "probe token '" + m_config.probeToken + "' not found in " + m_config.probeFile;
        return false;
    }
    if (Size(slots, file) != size) {
        why = "Size(name) != Size(handle)";
        return false;
    }
    if (!Exists(slots, file)) {
        why = "FileExists(" + m_config.probeFile + ") == false";
        return false;
    }
    if (Exists(slots, "steamaudio_probe_nonexistent_7f3a.bin")) {
        why = "FileExists(nonexistent) == true";
        return false;
    }
    return true;
}

bool EngineFileSystem::ProbeGuarded(const FileSystemSlots& slots, std::string& why) const
{
#if defined(_MSC_VER)
    __try {
        return Probe(slots, why);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        why = "access violation while probing slot layout";
        return false;
    }
#else
    return Probe(slots, why);
#endif
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
bool EngineFileSystem::Initialize(const FileSystemConfig& config, std::string& error)
{
    Shutdown();
    m_config = config;
    if (config.disabled) {
        error = "disabled by configuration";
        return false;
    }
#ifdef _WIN32
    HMODULE module = GetModuleHandleA(config.module.c_str());
    if (!module) {
        error = "GetModuleHandle(" + config.module + ") failed";
        return false;
    }
    if (const std::optional<ModuleImage> image = GetLoadedModuleImage(config.module)) {
        m_moduleBase = image->base;
        m_moduleSize = image->size;
    }
    auto factory = reinterpret_cast<src::CreateInterfaceFn>(
        reinterpret_cast<void*>(GetProcAddress(module, "CreateInterface")));
    if (!factory) {
        error = "CreateInterface export missing in " + config.module;
        return false;
    }
    int rc = 0;
    m_object = factory(config.interfaceVersion.c_str(), &rc);
    if (!m_object) {
        error = "CreateInterface(" + config.interfaceVersion + ") returned null";
        return false;
    }

    struct Candidate {
        FileSystemSlots slots;
        const char* name;
    };
    std::vector<Candidate> candidates;
    if (config.slots.Complete())
        candidates.push_back({config.slots, "configured"});
    else {
        // Secondary vtable (IBaseFileSystem sub-object) is the textbook
        // layout; the primary-vtable variants cover engines that flattened
        // the hierarchy (IAppSystem has 5 or 9 virtuals depending on branch).
        candidates.push_back({FileSystemSlots::Sdk2013(1, 0, true), "ibasefilesystem/msvc-reversed-size"});
        candidates.push_back({FileSystemSlots::Sdk2013(1, 0, false), "ibasefilesystem"});
        candidates.push_back({FileSystemSlots::Sdk2013(0, 5, true), "flat/appsystem5/msvc-reversed-size"});
        candidates.push_back({FileSystemSlots::Sdk2013(0, 5, false), "flat/appsystem5"});
        candidates.push_back({FileSystemSlots::Sdk2013(0, 9, true), "flat/appsystem9/msvc-reversed-size"});
        candidates.push_back({FileSystemSlots::Sdk2013(0, 9, false), "flat/appsystem9"});
    }
    std::string failures;
    for (const Candidate& c : candidates) {
        std::string why;
        if (!SlotsPlausible(c.slots, why)) {
            failures += std::string(failures.empty() ? "" : "; ") + c.name + ": " + why;
            continue;
        }
        if (ProbeGuarded(c.slots, why)) {
            m_slots = c.slots;
            m_slots.fileNameString = config.slots.fileNameString;
            m_stringSlotState = StringSlot::Unvalidated;
            m_validated = true;
            m_description = std::string(config.interfaceVersion) + " (" + c.name +
                            ", this+" + std::to_string(m_slots.thisOffset) + " read=" + std::to_string(m_slots.read) +
                            " open=" + std::to_string(m_slots.open) + " exists=" + std::to_string(m_slots.fileExists) +
                            ")";
            SA_LOGI("[fs] %s validated: %s", config.module.c_str(), m_description.c_str());
            return true;
        }
        failures += std::string(failures.empty() ? "" : "; ") + c.name + ": " + why;
    }
    error = "no IFileSystem slot layout validated (" + failures + ")";
    m_object = nullptr;
    return false;
#else
    error = "engine file system access is only implemented for Windows builds";
    return false;
#endif
}

void EngineFileSystem::Shutdown()
{
    m_object = nullptr;
    m_moduleBase = 0;
    m_moduleSize = 0;
    m_validated = false;
    m_slots = FileSystemSlots{};
    m_stringSlotState = StringSlot::Unvalidated;
    m_description.clear();
}

// ---------------------------------------------------------------------------
// FileNameHandle_t -> path
// ---------------------------------------------------------------------------
bool EngineFileSystem::CallString(const void* handle, char* buf, int32_t len) const
{
    // bool IFileSystem::String(const FileNameHandle_t& handle, char* buf, int buflen)
    // FileNameHandle_t is a void*; the reference is a pointer to it.
    return CallVirtual<bool>(m_object, m_slots.fileNameString, &handle, buf, len);
}

bool EngineFileSystem::CallStringGuarded(const void* handle, char* buf, int32_t len) const
{
#if defined(_MSC_VER)
    __try {
        return CallString(handle, buf, len);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    return CallString(handle, buf, len);
#endif
}

namespace {

bool LooksLikeSoundPath(const char* s, size_t len)
{
    if (len < 5 || len >= 256)
        return false;
    for (size_t i = 0; i < len; ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x20 || c >= 0x7f)
            return false;
    }
    const std::string lower = [&] {
        std::string t(s, len);
        for (char& c : t)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return t;
    }();
    const char* exts[] = {".wav", ".mp3", ".ogg", ".flac"};
    for (const char* e : exts) {
        const size_t n = std::strlen(e);
        if (lower.size() > n && lower.compare(lower.size() - n, n, e) == 0)
            return true;
    }
    return false;
}

} // namespace

bool EngineFileSystem::FileNameFromHandle(const void* handle, std::string& out)
{
    out.clear();
    if (!FileNameResolverAvailable() || !handle)
        return false;
    if (m_stringSlotState == StringSlot::Unvalidated) {
        const uintptr_t vtable = *reinterpret_cast<const uintptr_t*>(m_object);
        const int32_t slot = m_slots.fileNameString;
        if (slot > 256 || !IsMemoryReadable(vtable, static_cast<size_t>(slot + 1) * sizeof(void*))) {
            m_stringSlotState = StringSlot::Broken;
            SA_LOGW("[fs] filename_string slot %d unreadable; sound names from handles disabled", slot);
            return false;
        }
        const uintptr_t fn = reinterpret_cast<const uintptr_t*>(vtable)[slot];
        if (fn == 0 || (m_moduleSize && (fn < m_moduleBase || fn >= m_moduleBase + m_moduleSize))) {
            m_stringSlotState = StringSlot::Broken;
            SA_LOGW("[fs] filename_string slot %d does not point into %s; disabled", slot, m_config.module.c_str());
            return false;
        }
    }
    char buf[512];
    std::memset(buf, 0, sizeof(buf));
    const bool ok = CallStringGuarded(handle, buf, static_cast<int32_t>(sizeof(buf) - 1));
    buf[sizeof(buf) - 1] = '\0';
    const size_t len = std::strlen(buf);
    if (!ok || !LooksLikeSoundPath(buf, len)) {
        if (m_stringSlotState == StringSlot::Unvalidated) {
            m_stringSlotState = StringSlot::Broken;
            SA_LOGW("[fs] IFileSystem::String slot %d did not yield a sound path (ok=%d, '%.*s'); disabled",
                    m_slots.fileNameString, ok ? 1 : 0, static_cast<int>(std::min<size_t>(len, 64)), buf);
        }
        return false;
    }
    if (m_stringSlotState == StringSlot::Unvalidated) {
        m_stringSlotState = StringSlot::Validated;
        SA_LOGI("[fs] IFileSystem::String slot %d validated ('%s')", m_slots.fileNameString, buf);
    }
    out.assign(buf, len);
    return true;
}

const char* EngineFileSystem::FileNameResolverStatus() const
{
    if (!Available())
        return "filesystem unavailable";
    if (m_slots.fileNameString < 0)
        return "not configured (filesystem.slots.filename_string)";
    switch (m_stringSlotState) {
    case StringSlot::Unvalidated:
        return "configured, awaiting first sound";
    case StringSlot::Validated:
        return "validated";
    case StringSlot::Broken:
        return "failed validation; disabled";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------
bool EngineFileSystem::FileExists(const std::string& path) const
{
    return Available() && Exists(m_slots, path.c_str());
}

uint32_t EngineFileSystem::FileSize(const std::string& path) const
{
    if (!Available() || !Exists(m_slots, path.c_str()))
        return 0;
    return Size(m_slots, path.c_str());
}

bool EngineFileSystem::ReadRange(const std::string& path, size_t offset, size_t length, std::vector<uint8_t>& out,
                                 std::string& error) const
{
    out.clear();
    if (!Available()) {
        error = "engine file system unavailable";
        return false;
    }
    const Handle h = Open(m_slots, path.c_str(), "rb");
    if (!h) {
        error = path + " not found in search path " + m_config.pathId;
        return false;
    }
    const uint32_t size = Size(m_slots, h);
    if (offset > size) {
        Close(m_slots, h);
        error = "offset past end of " + path;
        return false;
    }
    const size_t want = std::min(length, static_cast<size_t>(size - offset));
    out.resize(want);
    if (offset)
        Seek(m_slots, h, static_cast<int32_t>(offset), kSeekHead);
    size_t done = 0;
    while (done < want) {
        const size_t chunk = std::min<size_t>(want - done, 1u << 24);
        const int32_t got = Read(m_slots, h, out.data() + done, static_cast<int32_t>(chunk));
        if (got <= 0)
            break;
        done += static_cast<size_t>(got);
    }
    Close(m_slots, h);
    if (done != want) {
        error = "short read on " + path + " (" + std::to_string(done) + " of " + std::to_string(want) + ")";
        out.clear();
        return false;
    }
    return true;
}

bool EngineFileSystem::ReadFile(const std::string& path, std::vector<uint8_t>& out, std::string& error,
                                size_t maxSize) const
{
    out.clear();
    if (!Available()) {
        error = "engine file system unavailable";
        return false;
    }
    if (!Exists(m_slots, path.c_str())) {
        error = path + " not found in search path " + m_config.pathId;
        return false;
    }
    const uint32_t size = Size(m_slots, path.c_str());
    if (size == 0) {
        error = path + " is empty";
        return false;
    }
    if (size > maxSize) {
        error = path + " is too large (" + std::to_string(size) + " bytes)";
        return false;
    }
    return ReadRange(path, 0, size, out, error);
}

} // namespace sa
