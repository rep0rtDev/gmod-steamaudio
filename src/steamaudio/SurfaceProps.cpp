// src/steamaudio/SurfaceProps.cpp
#include "steamaudio/SurfaceProps.h"

#include <cctype>
#include <cstdlib>

#include "util/KeyValues.h"

namespace sa {

namespace {

constexpr size_t kMaxChain = 16;

float ToFloat(const std::string& s, float fallback)
{
    if (s.empty())
        return fallback;
    char* end = nullptr;
    const float v = std::strtof(s.c_str(), &end);
    return end && end != s.c_str() ? v : fallback;
}

std::string NormalizeSlashes(std::string s)
{
    for (char& c : s)
        if (c == '\\')
            c = '/';
    return s;
}

} // namespace

size_t SurfacePropDatabase::AddScript(const char* data, size_t size, std::string* error)
{
    KvDocument doc;
    if (!ParseKeyValues(data, size, doc)) {
        if (error)
            *error = doc.error;
        return 0;
    }
    size_t added = 0;
    for (const KvNode& root : doc.roots) {
        if (!root.isBlock || root.key.empty())
            continue;
        SurfacePropEntry e;
        e.name = KvLower(root.key);
        e.base = KvLower(root.Get("base", std::string()));
        const std::string& gm = root.Get("gamematerial", std::string());
        if (!gm.empty())
            e.gameMaterial = static_cast<char>(std::toupper(static_cast<unsigned char>(gm[0])));
        e.density = ToFloat(root.Get("density", std::string()), 0.f);
        e.thickness = ToFloat(root.Get("thickness", std::string()), 0.f);
        if (e.base == e.name)
            e.base.clear();
        m_entries[e.name] = std::move(e);
        ++added;
    }
    if (added == 0 && error)
        *error = "no surfaceprop blocks";
    return added;
}

std::vector<std::string> SurfacePropDatabase::ParseManifest(const char* data, size_t size)
{
    std::vector<std::string> files;
    KvDocument doc;
    if (!ParseKeyValues(data, size, doc))
        return files;
    for (const KvNode& root : doc.roots) {
        if (!root.isBlock)
            continue;
        for (const KvNode& child : root.children) {
            if (child.isBlock || !KvKeyEquals(child.key, "file") || child.value.empty())
                continue;
            std::string file = KvLower(NormalizeSlashes(child.value));
            if (file.find("manifest") != std::string::npos)
                continue;
            bool seen = false;
            for (const std::string& f : files)
                seen = seen || f == file;
            if (!seen)
                files.push_back(std::move(file));
        }
    }
    return files;
}

const SurfacePropEntry* SurfacePropDatabase::Find(const std::string& name) const
{
    auto it = m_entries.find(KvLower(name));
    return it == m_entries.end() ? nullptr : &it->second;
}

const char* SurfacePropDatabase::GameMaterialName(char letter)
{
    // decals.h CHAR_TEX_* (Source SDK 2013), plus the CS:GO additions.
    switch (static_cast<char>(std::toupper(static_cast<unsigned char>(letter)))) {
    case 'A': return "antlion";
    case 'B': return "bloodyflesh";
    case 'C': return "concrete";
    case 'D': return "dirt";
    case 'E': return "eggshell";
    case 'F': return "flesh";
    case 'G': return "metalgrate";
    case 'H': return "alienflesh";
    case 'I': return "default";
    case 'J': return "grass";
    case 'K': return "snow";
    case 'L': return "plastic";
    case 'M': return "metal";
    case 'N': return "sand";
    case 'O': return "foliage";
    case 'P': return "computer";
    case 'Q': return "asphalt";
    case 'R': return "brick";
    case 'S': return "water";
    case 'T': return "tile";
    case 'U': return "cardboard";
    case 'V': return "metalvent";
    case 'W': return "wood";
    case 'X': return "glass";
    case 'Y': return "glass";
    case 'Z': return "default";
    default: return nullptr;
    }
}

std::string SurfacePropDatabase::Canonicalize(const std::string& name, const KnownFn& known) const
{
    std::string current = KvLower(name);
    char gameMaterial = 0;
    for (size_t step = 0; step < kMaxChain && !current.empty(); ++step) {
        if (known && known(current))
            return current;
        const SurfacePropEntry* entry = Find(current);
        if (!entry)
            break;
        if (gameMaterial == 0)
            gameMaterial = entry->gameMaterial;
        if (entry->base.empty() || entry->base == current)
            break;
        current = entry->base;
    }
    if (gameMaterial == 0) {
        if (const SurfacePropEntry* entry = Find(name))
            gameMaterial = entry->gameMaterial;
    }
    if (gameMaterial != 0) {
        if (const char* family = GameMaterialName(gameMaterial)) {
            if (!known || known(family))
                return family;
        }
    }
    return std::string();
}

std::string VmtSurfaceProp(const KvDocument& vmt, std::string* patchInclude)
{
    if (patchInclude)
        patchInclude->clear();
    for (const KvNode& shader : vmt.roots) {
        if (!shader.isBlock)
            continue;
        if (KvKeyEquals(shader.key, "patch")) {
            for (const char* section : {"replace", "insert"}) {
                if (const KvNode* block = shader.Find(section)) {
                    const std::string& v = block->Get("$surfaceprop", std::string());
                    if (!v.empty())
                        return KvLower(v);
                }
            }
            if (patchInclude)
                *patchInclude = KvLower(NormalizeSlashes(shader.Get("include", std::string())));
            return std::string();
        }
        const std::string& direct = shader.Get("$surfaceprop", std::string());
        if (!direct.empty())
            return KvLower(direct);
        // Shader fallback blocks (">=DX90", "LightmappedGeneric_DX8", ...).
        for (const KvNode& child : shader.children) {
            if (!child.isBlock)
                continue;
            const std::string& nested = child.Get("$surfaceprop", std::string());
            if (!nested.empty())
                return KvLower(nested);
        }
    }
    return std::string();
}

std::string TextureToVmtPath(const std::string& texture)
{
    std::string path = KvLower(NormalizeSlashes(texture));
    while (!path.empty() && (path[0] == '/' || path[0] == ' '))
        path.erase(path.begin());
    while (!path.empty() && (path.back() == ' ' || path.back() == '/'))
        path.pop_back();
    if (path.empty() || path.find("..") != std::string::npos)
        return std::string();
    if (path.rfind("materials/", 0) != 0)
        path.insert(0, "materials/");
    if (path.size() >= 4 && path.compare(path.size() - 4, 4, ".vmt") == 0)
        return path;
    if (path.size() >= 4 && path.compare(path.size() - 4, 4, ".vtf") == 0)
        path.erase(path.size() - 4);
    return path + ".vmt";
}

} // namespace sa
