// src/steamaudio/SurfaceProps.h
//
// Source $surfaceprop resolution:
//
//   * SurfacePropDatabase holds the entries of scripts/surfaceproperties*.txt
//     (as listed by scripts/surfaceproperties_manifest.txt): each surfaceprop
//     may inherit from a `base` entry and carries a `gamematerial` letter
//     ('C' concrete, 'M' metal, 'W' wood, ...). Canonicalize() walks that
//     inheritance chain to the first name the acoustic material library knows
//     and finally falls back to the gamematerial family, so addon-specific
//     surfaceprops ("my_mod_rusty_steel" { base "metal" }) map onto sensible
//     acoustic materials.
//   * VmtSurfaceProp() extracts $surfaceprop from a parsed VMT, including
//     "Patch" materials that include another VMT and insert/replace keys.
//
// Both are pure functions over bytes: file access stays with the caller.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace sa {

struct KvDocument;

struct SurfacePropEntry {
    std::string name; // lower-case
    std::string base; // lower-case, empty for roots
    char gameMaterial = 0;
    float density = 0.f;
    float thickness = 0.f;
};

class SurfacePropDatabase {
public:
    // Parses one surfaceproperties script; entries override earlier ones with
    // the same name (the engine loads later manifest files on top of earlier
    // ones). Returns the number of entries added.
    size_t AddScript(const char* data, size_t size, std::string* error = nullptr);
    size_t AddScript(const std::string& text, std::string* error = nullptr)
    {
        return AddScript(text.data(), text.size(), error);
    }

    // Parses scripts/surfaceproperties_manifest.txt into its "file" entries.
    static std::vector<std::string> ParseManifest(const char* data, size_t size);

    const SurfacePropEntry* Find(const std::string& name) const;
    size_t Size() const { return m_entries.size(); }
    bool Empty() const { return m_entries.empty(); }

    using KnownFn = std::function<bool(const std::string& lowerName)>;

    // Returns the first name along `name` -> base -> base... for which
    // `known` is true; if none, the gamematerial family name (see
    // GameMaterialName) if known; otherwise an empty string.
    std::string Canonicalize(const std::string& name, const KnownFn& known) const;

    // Gamematerial letter -> family surfaceprop name ("concrete", "metal", ...);
    // nullptr for unknown letters.
    static const char* GameMaterialName(char letter);

private:
    std::unordered_map<std::string, SurfacePropEntry> m_entries;
};

// $surfaceprop from a VMT document (case-insensitive key, any shader name).
// For "Patch" materials the value is taken from the insert/replace block when
// present, otherwise `patchInclude` receives the included material path
// ("materials/foo/bar.vmt") so the caller can resolve it.
std::string VmtSurfaceProp(const KvDocument& vmt, std::string* patchInclude);

// Normalizes a texture reference to "materials/<path>.vmt" (lower-case,
// forward slashes). Returns an empty string for unusable input.
std::string TextureToVmtPath(const std::string& texture);

} // namespace sa
