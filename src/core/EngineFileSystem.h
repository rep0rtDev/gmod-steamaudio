// src/core/EngineFileSystem.h
//
// Read-only access to the engine's virtual file system (IFileSystem from
// filesystem_stdio.dll). This is the only way to see files the way the game
// does: mounted Workshop addons (.gma), legacy addons/, download/, VPKs and
// gamemode content are all resolved through the "GAME" search path.
//
// IFileSystem is a public interface (`IFileSystem : IAppSystem,
// IBaseFileSystem`). With both MSVC and GCC the IBaseFileSystem methods live
// in a *secondary* vtable whose vptr sits one pointer into the object, and
// MSVC additionally emits the two adjacent `Size` overloads in reverse
// declaration order. Because branches differ, the sub-object offset and slot
// indices are configurable (config/steamaudio_signatures.json, "filesystem")
// and every candidate layout is (a) checked to point at code inside the
// filesystem module and (b) validated against a file that is guaranteed to
// exist (gameinfo.txt) before it is used. When validation fails the caller
// falls back to plain disk access / GMA scanning / Lua file.Read.
//
// Thread-safety: game thread only (IFileSystem itself is thread-safe, but
// this wrapper's state is not synchronized).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "util/Json.h"

namespace sa {

// Vtable slot indices of the IBaseFileSystem methods we call, relative to the
// vtable found `thisOffset` pointers into the IFileSystem object.
struct FileSystemSlots {
    int32_t thisOffset = 1;  // 1 => secondary (IBaseFileSystem) vtable, 0 => primary
    int32_t read = -1;       // int Read(void* out, int size, FileHandle_t)
    int32_t open = -1;       // FileHandle_t Open(const char* name, const char* options, const char* pathID)
    int32_t close = -1;      // void Close(FileHandle_t)
    int32_t seek = -1;       // void Seek(FileHandle_t, int pos, FileSystemSeek_t)
    int32_t tell = -1;       // unsigned Tell(FileHandle_t)
    int32_t sizeHandle = -1; // unsigned Size(FileHandle_t)
    int32_t sizeName = -1;   // unsigned Size(const char* name, const char* pathID)
    int32_t fileExists = -1; // bool FileExists(const char* name, const char* pathID)
    // IFileSystem::String(const FileNameHandle_t&, char* buf, int buflen) in
    // the *primary* vtable (this+0). Not part of the probed layout: it is only
    // used when configured and is validated lazily on the first handle that
    // produces a readable sound path. -1 = disabled.
    int32_t fileNameString = -1;

    bool Complete() const
    {
        return read >= 0 && open >= 0 && close >= 0 && seek >= 0 && tell >= 0 && sizeHandle >= 0 &&
               sizeName >= 0 && fileExists >= 0;
    }
    // Source SDK 2013 IBaseFileSystem declaration order starting at
    // `firstSlot` of the vtable selected by `thisOffset`. MSVC emits adjacent
    // overloads in reverse declaration order, which `reversedSizeOverloads`
    // models.
    static FileSystemSlots Sdk2013(int32_t thisOffset, int32_t firstSlot, bool reversedSizeOverloads);
};

struct FileSystemConfig {
    std::string module = "filesystem_stdio.dll";
    std::string interfaceVersion = "VFileSystem022";
    std::string pathId = "GAME";
    FileSystemSlots slots;          // all -1 => probe the built-in candidate layouts
    std::string probeFile = "gameinfo.txt";
    std::string probeToken = "GameInfo"; // must appear (case-insensitively) in the probe file
    bool disabled = false;
    // Directories (relative to the game directory unless absolute) scanned for
    // .gma archives when the engine file system is unavailable.
    std::vector<std::string> gmaDirectories = {"addons", "cache/workshop", "cache"};

    void Parse(const JsonValue& node);
    // `slots` may be the flat object or the per-architecture one (caller selects).
    void ParseSlots(const JsonValue& slotsNode);
};

class EngineFileSystem {
public:
    EngineFileSystem() = default;
    ~EngineFileSystem() = default;
    EngineFileSystem(const EngineFileSystem&) = delete;
    EngineFileSystem& operator=(const EngineFileSystem&) = delete;

    // Resolves the interface and validates the slot layout. Returns false
    // (with `error`) when the engine FS cannot be used; the object then stays
    // in the unavailable state and every query returns false.
    bool Initialize(const FileSystemConfig& config, std::string& error);
    void Shutdown();

    bool Available() const { return m_object != nullptr && m_validated; }
    const FileSystemSlots& Slots() const { return m_slots; }
    const std::string& Description() const { return m_description; }

    bool FileExists(const std::string& path) const;
    // Size of the file or 0 when missing.
    uint32_t FileSize(const std::string& path) const;
    // Reads the entire file. `maxSize` guards against absurd sizes.
    bool ReadFile(const std::string& path, std::vector<uint8_t>& out, std::string& error,
                  size_t maxSize = size_t(1) << 31) const;
    // Reads at most `length` bytes starting at `offset`.
    bool ReadRange(const std::string& path, size_t offset, size_t length, std::vector<uint8_t>& out,
                   std::string& error) const;

    // Resolves a FileNameHandle_t (SndInfo_t::m_filenameHandle) to the
    // game-relative path through IFileSystem::String when the slot is
    // configured. The slot is validated on the first call (the result must
    // look like a sound path); a failure disables it for the session.
    bool FileNameFromHandle(const void* handle, std::string& out);
    bool FileNameResolverAvailable() const
    {
        return Available() && m_slots.fileNameString >= 0 && m_stringSlotState != StringSlot::Broken;
    }
    const char* FileNameResolverStatus() const;

private:
    enum class StringSlot { Unvalidated, Validated, Broken };
    bool CallString(const void* handle, char* buf, int32_t len) const;
    bool CallStringGuarded(const void* handle, char* buf, int32_t len) const;
    StringSlot m_stringSlotState = StringSlot::Unvalidated;
    using Handle = void*;

    bool Probe(const FileSystemSlots& slots, std::string& why) const;
    bool ProbeGuarded(const FileSystemSlots& slots, std::string& why) const;
    // Rejects layouts whose vptr/slots do not point into the module image.
    bool SlotsPlausible(const FileSystemSlots& slots, std::string& why) const;
    void* SubObject(const FileSystemSlots& slots) const;

    Handle Open(const FileSystemSlots& slots, const char* path, const char* options) const;
    void Close(const FileSystemSlots& slots, Handle h) const;
    int32_t Read(const FileSystemSlots& slots, Handle h, void* out, int32_t size) const;
    void Seek(const FileSystemSlots& slots, Handle h, int32_t pos, int32_t whence) const;
    uint32_t Tell(const FileSystemSlots& slots, Handle h) const;
    uint32_t Size(const FileSystemSlots& slots, Handle h) const;
    uint32_t Size(const FileSystemSlots& slots, const char* path) const;
    bool Exists(const FileSystemSlots& slots, const char* path) const;

    FileSystemConfig m_config;
    void* m_object = nullptr;
    uintptr_t m_moduleBase = 0;
    size_t m_moduleSize = 0;
    FileSystemSlots m_slots;
    bool m_validated = false;
    std::string m_description;
};

} // namespace sa
