// src/steamaudio/SceneBuilder.cpp
#include "SceneBuilder.h"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "PhononContext.h"
#include "steamaudio/SurfaceProps.h"
#include "util/Json.h"
#include "util/Logging.h"

namespace sa {

// ---------------------------------------------------------------------------
// MeshData
// ---------------------------------------------------------------------------

void MeshData::Clear()
{
    vertices.clear();
    triangles.clear();
    materialIndices.clear();
    materials.clear();
}

IPLint32 MeshData::AddMaterial(const IPLMaterial& material)
{
    for (size_t i = 0; i < materials.size(); ++i) {
        const IPLMaterial& m = materials[i];
        bool same = m.scattering == material.scattering;
        for (int b = 0; same && b < IPL_NUM_BANDS; ++b)
            same = m.absorption[b] == material.absorption[b] && m.transmission[b] == material.transmission[b];
        if (same)
            return static_cast<IPLint32>(i);
    }
    materials.push_back(material);
    return static_cast<IPLint32>(materials.size() - 1);
}

void MeshData::AddTriangle(const IPLVector3& a, const IPLVector3& b, const IPLVector3& c, IPLint32 material)
{
    const IPLint32 base = static_cast<IPLint32>(vertices.size());
    vertices.push_back(a);
    vertices.push_back(b);
    vertices.push_back(c);
    IPLTriangle tri{};
    tri.indices[0] = base;
    tri.indices[1] = base + 1;
    tri.indices[2] = base + 2;
    triangles.push_back(tri);
    materialIndices.push_back(material);
}

void MeshData::Append(const MeshData& other)
{
    const IPLint32 vertexBase = static_cast<IPLint32>(vertices.size());
    vertices.insert(vertices.end(), other.vertices.begin(), other.vertices.end());

    std::vector<IPLint32> materialRemap(other.materials.size());
    for (size_t i = 0; i < other.materials.size(); ++i)
        materialRemap[i] = AddMaterial(other.materials[i]);

    triangles.reserve(triangles.size() + other.triangles.size());
    materialIndices.reserve(materialIndices.size() + other.triangles.size());
    for (size_t i = 0; i < other.triangles.size(); ++i) {
        IPLTriangle tri = other.triangles[i];
        for (int k = 0; k < 3; ++k)
            tri.indices[k] += vertexBase;
        triangles.push_back(tri);
        const IPLint32 mat = i < other.materialIndices.size() ? other.materialIndices[i] : 0;
        materialIndices.push_back(mat >= 0 && static_cast<size_t>(mat) < materialRemap.size() ? materialRemap[mat] : 0);
    }
}

void MeshData::Compact()
{
    if (materials.empty()) {
        IPLMaterial generic{};
        for (int b = 0; b < IPL_NUM_BANDS; ++b) {
            generic.absorption[b] = 0.10f;
            generic.transmission[b] = 0.05f;
        }
        generic.scattering = 0.05f;
        materials.push_back(generic);
    }
    materialIndices.resize(triangles.size(), 0);

    std::vector<IPLTriangle> keptTriangles;
    std::vector<IPLint32> keptMaterials;
    keptTriangles.reserve(triangles.size());
    keptMaterials.reserve(triangles.size());
    const IPLint32 vertexCount = static_cast<IPLint32>(vertices.size());
    for (size_t i = 0; i < triangles.size(); ++i) {
        const IPLTriangle& t = triangles[i];
        bool valid = true;
        for (int k = 0; k < 3 && valid; ++k)
            valid = t.indices[k] >= 0 && t.indices[k] < vertexCount;
        if (!valid)
            continue;
        const IPLVector3& a = vertices[t.indices[0]];
        const IPLVector3& b = vertices[t.indices[1]];
        const IPLVector3& c = vertices[t.indices[2]];
        const Vec3 ab = Vec3::FromIPL(b) - Vec3::FromIPL(a);
        const Vec3 ac = Vec3::FromIPL(c) - Vec3::FromIPL(a);
        const float area = ab.Cross(ac).LengthSq();
        if (!std::isfinite(area) || area < 1e-12f)
            continue;
        keptTriangles.push_back(t);
        IPLint32 mat = materialIndices[i];
        if (mat < 0 || static_cast<size_t>(mat) >= materials.size())
            mat = 0;
        keptMaterials.push_back(mat);
    }

    std::vector<IPLint32> remap(vertices.size(), -1);
    std::vector<IPLVector3> newVertices;
    newVertices.reserve(vertices.size());
    for (IPLTriangle& t : keptTriangles) {
        for (int k = 0; k < 3; ++k) {
            IPLint32& idx = t.indices[k];
            if (remap[idx] < 0) {
                remap[idx] = static_cast<IPLint32>(newVertices.size());
                newVertices.push_back(vertices[idx]);
            }
            idx = remap[idx];
        }
    }
    vertices.swap(newVertices);
    triangles.swap(keptTriangles);
    materialIndices.swap(keptMaterials);
}

// ---------------------------------------------------------------------------
// MaterialLibrary
// ---------------------------------------------------------------------------

namespace {

IPLMaterial MakeMaterial(float lowAbs, float midAbs, float highAbs, float scattering, float lowTrans,
                         float midTrans, float highTrans)
{
    IPLMaterial m{};
    // With IPL_NUM_BANDS == 3 the bands are low/mid/high. With octave bands
    // (11) interpolate the three anchors across the range.
    for (int b = 0; b < IPL_NUM_BANDS; ++b) {
        const float t = IPL_NUM_BANDS > 1 ? static_cast<float>(b) / static_cast<float>(IPL_NUM_BANDS - 1) : 0.f;
        float abs, trans;
        if (t < 0.5f) {
            abs = Lerp(lowAbs, midAbs, t * 2.f);
            trans = Lerp(lowTrans, midTrans, t * 2.f);
        } else {
            abs = Lerp(midAbs, highAbs, (t - 0.5f) * 2.f);
            trans = Lerp(midTrans, highTrans, (t - 0.5f) * 2.f);
        }
        m.absorption[b] = Clamp(abs, 0.f, 1.f);
        m.transmission[b] = Clamp(trans, 0.f, 1.f);
    }
    m.scattering = Clamp(scattering, 0.f, 1.f);
    return m;
}

std::string ToLower(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool Contains(const std::string& haystack, const char* needle)
{
    return haystack.find(needle) != std::string::npos;
}

} // namespace

MaterialLibrary::MaterialLibrary()
{
    m_generic = MakeMaterial(0.10f, 0.20f, 0.30f, 0.05f, 0.100f, 0.050f, 0.030f);

    // Values from the Steam Audio material table (phonon.h documentation).
    AddBase("generic", m_generic);
    AddBase("brick", MakeMaterial(0.03f, 0.04f, 0.07f, 0.05f, 0.015f, 0.015f, 0.015f));
    AddBase("concrete", MakeMaterial(0.05f, 0.07f, 0.08f, 0.05f, 0.015f, 0.002f, 0.001f));
    AddBase("ceramic", MakeMaterial(0.01f, 0.02f, 0.02f, 0.05f, 0.060f, 0.044f, 0.011f));
    AddBase("gravel", MakeMaterial(0.60f, 0.70f, 0.80f, 0.05f, 0.031f, 0.012f, 0.008f));
    AddBase("carpet", MakeMaterial(0.24f, 0.69f, 0.73f, 0.05f, 0.020f, 0.005f, 0.003f));
    AddBase("glass", MakeMaterial(0.06f, 0.03f, 0.02f, 0.05f, 0.060f, 0.044f, 0.011f));
    AddBase("plaster", MakeMaterial(0.12f, 0.06f, 0.04f, 0.05f, 0.056f, 0.056f, 0.004f));
    AddBase("wood", MakeMaterial(0.11f, 0.07f, 0.06f, 0.05f, 0.070f, 0.014f, 0.005f));
    AddBase("metal", MakeMaterial(0.20f, 0.07f, 0.06f, 0.05f, 0.200f, 0.025f, 0.010f));
    AddBase("rock", MakeMaterial(0.13f, 0.20f, 0.24f, 0.05f, 0.015f, 0.002f, 0.001f));
    // Source-specific additions.
    AddBase("dirt", MakeMaterial(0.15f, 0.25f, 0.40f, 0.30f, 0.010f, 0.002f, 0.001f));
    AddBase("grass", MakeMaterial(0.11f, 0.26f, 0.60f, 0.40f, 0.010f, 0.002f, 0.001f));
    AddBase("sand", MakeMaterial(0.20f, 0.40f, 0.60f, 0.30f, 0.010f, 0.002f, 0.001f));
    AddBase("snow", MakeMaterial(0.45f, 0.75f, 0.90f, 0.20f, 0.020f, 0.005f, 0.002f));
    AddBase("water", MakeMaterial(0.01f, 0.01f, 0.02f, 0.05f, 0.150f, 0.050f, 0.010f));
    AddBase("tile", MakeMaterial(0.01f, 0.02f, 0.02f, 0.05f, 0.030f, 0.010f, 0.005f));
    AddBase("plastic", MakeMaterial(0.10f, 0.08f, 0.06f, 0.05f, 0.200f, 0.100f, 0.050f));
    AddBase("rubber", MakeMaterial(0.10f, 0.15f, 0.20f, 0.05f, 0.100f, 0.030f, 0.010f));
    AddBase("cardboard", MakeMaterial(0.20f, 0.30f, 0.40f, 0.05f, 0.300f, 0.200f, 0.100f));
    AddBase("paper", MakeMaterial(0.15f, 0.30f, 0.45f, 0.05f, 0.400f, 0.300f, 0.200f));
    AddBase("flesh", MakeMaterial(0.30f, 0.50f, 0.70f, 0.10f, 0.100f, 0.050f, 0.020f));
    AddBase("foliage", MakeMaterial(0.20f, 0.45f, 0.70f, 0.60f, 0.500f, 0.300f, 0.150f));
    AddBase("chainlink", MakeMaterial(0.10f, 0.10f, 0.10f, 0.40f, 0.800f, 0.800f, 0.700f));
    AddBase("metalgrate", MakeMaterial(0.10f, 0.10f, 0.10f, 0.40f, 0.700f, 0.700f, 0.600f));
    AddBase("wood_hollow", MakeMaterial(0.15f, 0.10f, 0.08f, 0.05f, 0.150f, 0.050f, 0.020f));
    AddBase("metal_thin", MakeMaterial(0.15f, 0.08f, 0.06f, 0.05f, 0.300f, 0.080f, 0.030f));

    // Source surfaceprop aliases (from surfaceproperties.txt families).
    AddAlias("default", "generic");
    AddAlias("solidmetal", "metal");
    AddAlias("metal_box", "metal_thin");
    AddAlias("metalvent", "metal_thin");
    AddAlias("metalpanel", "metal_thin");
    AddAlias("metal_barrel", "metal_thin");
    AddAlias("metal_bouncy", "metal");
    AddAlias("metal_sand_barrel", "metal");
    AddAlias("metal_seafloorcar", "metal");
    AddAlias("metalvehicle", "metal");
    AddAlias("floating_metal_barrel", "metal_thin");
    AddAlias("weapon", "metal");
    AddAlias("grenade", "metal");
    AddAlias("canister", "metal_thin");
    AddAlias("chain", "chainlink");
    AddAlias("roller", "metal");
    AddAlias("slidingdoor", "metal");
    AddAlias("computer", "plastic");
    AddAlias("wood_crate", "wood_hollow");
    AddAlias("wood_plank", "wood");
    AddAlias("wood_solid", "wood");
    AddAlias("wood_furniture", "wood");
    AddAlias("wood_panel", "wood_hollow");
    AddAlias("wood_lowdensity", "wood_hollow");
    AddAlias("wood_box", "wood_hollow");
    AddAlias("boulder", "rock");
    AddAlias("concrete_block", "concrete");
    AddAlias("stone", "rock");
    AddAlias("rubbertire", "rubber");
    AddAlias("jeeptire", "rubber");
    AddAlias("slime", "water");
    AddAlias("wade", "water");
    AddAlias("puddle", "water");
    AddAlias("mud", "dirt");
    AddAlias("quicksand", "sand");
    AddAlias("glassbottle", "glass");
    AddAlias("combine_glass", "glass");
    AddAlias("pottery", "ceramic");
    AddAlias("porcelain", "ceramic");
    AddAlias("bloodyflesh", "flesh");
    AddAlias("alienflesh", "flesh");
    AddAlias("zombieflesh", "flesh");
    AddAlias("antlion", "flesh");
    AddAlias("armorflesh", "metal");
    AddAlias("player", "flesh");
    AddAlias("player_control_clip", "generic");
    AddAlias("no_decal", "generic");
    AddAlias("item", "plastic");
    AddAlias("plastic_barrel", "plastic");
    AddAlias("plastic_box", "plastic");
    AddAlias("popcan", "metal_thin");
    AddAlias("paintcan", "metal_thin");
    AddAlias("papercup", "paper");
    AddAlias("cardboard", "cardboard");
    AddAlias("upholstery", "carpet");
    AddAlias("clay", "ceramic");
    AddAlias("asphalt", "concrete");
    AddAlias("tile", "tile");
    AddAlias("ice", "glass");
    AddAlias("wet", "concrete");
    AddAlias("ladder", "metal");
    AddAlias("woodladder", "wood");
    AddAlias("mudslipperyslime", "dirt");
    AddAlias("gravel", "gravel");
    AddAlias("carpet", "carpet");
}

void MaterialLibrary::AddBase(const char* name, const IPLMaterial& material)
{
    m_materials[name] = material;
}

void MaterialLibrary::AddAlias(const char* alias, const char* target)
{
    m_aliases[alias] = target;
}

bool MaterialLibrary::Knows(const std::string& lowerName) const
{
    if (m_materials.count(lowerName))
        return true;
    auto alias = m_aliases.find(lowerName);
    return alias != m_aliases.end() && m_materials.count(alias->second) != 0;
}

const IPLMaterial& MaterialLibrary::ByName(const std::string& name) const
{
    auto it = m_materials.find(name);
    return it == m_materials.end() ? m_generic : it->second;
}

std::string MaterialLibrary::ResolveName(const std::string& surfaceProp) const
{
    const std::string key = ToLower(surfaceProp);
    if (key.empty())
        return "generic";
    auto direct = [this](const std::string& k) -> std::string {
        if (m_materials.count(k))
            return k;
        auto alias = m_aliases.find(k);
        if (alias != m_aliases.end() && m_materials.count(alias->second))
            return alias->second;
        return std::string();
    };
    std::string name = direct(key);
    if (!name.empty())
        return name;
    if (m_surfaceProps) {
        const std::string canonical =
            m_surfaceProps->Canonicalize(key, [this](const std::string& k) { return Knows(k); });
        if (!canonical.empty()) {
            name = direct(canonical);
            if (!name.empty())
                return name;
        }
    }
    // Prefix/substring heuristics for the long tail of surfaceprops.
    return NameFromTexture(key);
}

const IPLMaterial& MaterialLibrary::FromSurfaceProp(const std::string& surfaceProp) const
{
    return ByName(ResolveName(surfaceProp));
}

const IPLMaterial& MaterialLibrary::FromTextureName(const std::string& textureName) const
{
    return ByName(NameFromTexture(textureName));
}

std::string MaterialLibrary::NameFromTexture(const std::string& textureName) const
{
    const std::string n = ToLower(textureName);
    static const struct {
        const char* needle;
        const char* material;
    } kHints[] = {
        {"grate", "metalgrate"},  {"chainlink", "chainlink"}, {"fence", "chainlink"}, {"metal", "metal"},
        {"steel", "metal"},       {"iron", "metal"},          {"concrete", "concrete"}, {"cement", "concrete"},
        {"asphalt", "concrete"},  {"road", "concrete"},       {"brick", "brick"},     {"stone", "rock"},
        {"rock", "rock"},         {"cliff", "rock"},          {"wood", "wood"},       {"plank", "wood"},
        {"crate", "wood_hollow"}, {"glass", "glass"},         {"window", "glass"},    {"carpet", "carpet"},
        {"rug", "carpet"},        {"cloth", "carpet"},        {"fabric", "carpet"},   {"plaster", "plaster"},
        {"drywall", "plaster"},   {"wall", "plaster"},        {"ceiling", "plaster"}, {"tile", "tile"},
        {"ceramic", "ceramic"},   {"marble", "tile"},         {"dirt", "dirt"},       {"mud", "dirt"},
        {"ground", "dirt"},       {"grass", "grass"},         {"foliage", "foliage"}, {"leaf", "foliage"},
        {"tree", "wood"},         {"sand", "sand"},           {"gravel", "gravel"},   {"snow", "snow"},
        {"ice", "glass"},         {"water", "water"},         {"plastic", "plastic"}, {"rubber", "rubber"},
        {"cardboard", "cardboard"}, {"paper", "paper"},       {"flesh", "flesh"},
    };
    for (const auto& hint : kHints) {
        if (Contains(n, hint.needle) && m_materials.count(hint.material))
            return hint.material;
    }
    return "generic";
}

bool MaterialLibrary::LoadOverrides(const std::string& jsonPath)
{
    const JsonParseResult parsed = ParseJsonFile(jsonPath);
    if (!parsed.ok) {
        SA_LOGW("Material overrides '%s' not loaded: %s", jsonPath.c_str(), parsed.error.c_str());
        return false;
    }
    const JsonValue& root = parsed.value;
    if (root.Has("surfaceprops") && root["surfaceprops"].IsObject()) {
        for (const auto& kv : root["surfaceprops"].AsObject()) {
            const JsonValue& def = kv.second;
            IPLMaterial m = m_generic;
            const JsonValue& abs = def["absorption"];
            const JsonValue& trans = def["transmission"];
            if (abs.IsArray() && abs.Size() >= 3 && trans.IsArray() && trans.Size() >= 3) {
                m = MakeMaterial(static_cast<float>(abs.At(0).AsNumber()), static_cast<float>(abs.At(1).AsNumber()),
                                 static_cast<float>(abs.At(2).AsNumber()),
                                 static_cast<float>(def["scattering"].AsNumber(0.05)),
                                 static_cast<float>(trans.At(0).AsNumber()),
                                 static_cast<float>(trans.At(1).AsNumber()),
                                 static_cast<float>(trans.At(2).AsNumber()));
            } else if (def.Has("scattering")) {
                m.scattering = Clamp(static_cast<float>(def["scattering"].AsNumber(0.05)), 0.f, 1.f);
            }
            m_materials[ToLower(kv.first)] = m;
        }
    }
    if (root.Has("aliases") && root["aliases"].IsObject()) {
        for (const auto& kv : root["aliases"].AsObject())
            m_aliases[ToLower(kv.first)] = ToLower(kv.second.AsString(""));
    }
    SA_LOGI("Loaded material overrides from %s (%zu materials, %zu aliases)", jsonPath.c_str(),
            m_materials.size(), m_aliases.size());
    return true;
}

// ---------------------------------------------------------------------------
// SceneBuilder
// ---------------------------------------------------------------------------

SceneBuilder::~SceneBuilder()
{
    Shutdown();
}

bool SceneBuilder::Initialize(PhononContext& context, const BackendDevices& devices)
{
    Shutdown();
    if (!context.IsValid())
        return false;
    m_context = &context;

    m_sceneSettings = IPLSceneSettings{};
    m_sceneSettings.type = devices.sceneType;
    m_sceneSettings.embreeDevice = devices.embree;
    m_sceneSettings.radeonRaysDevice = devices.radeonRays;

    const IPLerror err = iplSceneCreate(context.Handle(), &m_sceneSettings, &m_scene);
    if (err != IPL_STATUS_SUCCESS || !m_scene) {
        SA_LOGE("iplSceneCreate(type=%d) failed: %s", static_cast<int>(m_sceneSettings.type), IplErrorToString(err));
        m_scene = nullptr;
        // Retry with the built-in ray tracer.
        if (m_sceneSettings.type != IPL_SCENETYPE_DEFAULT) {
            m_sceneSettings.type = IPL_SCENETYPE_DEFAULT;
            m_sceneSettings.embreeDevice = nullptr;
            m_sceneSettings.radeonRaysDevice = nullptr;
            const IPLerror retry = iplSceneCreate(context.Handle(), &m_sceneSettings, &m_scene);
            if (retry != IPL_STATUS_SUCCESS || !m_scene) {
                SA_LOGE("iplSceneCreate(default) failed: %s", IplErrorToString(retry));
                m_scene = nullptr;
                return false;
            }
            SA_LOGW("Scene falls back to the built-in ray tracer");
        } else {
            return false;
        }
    }
    m_dirty = false;
    return true;
}

void SceneBuilder::Shutdown()
{
    ClearDynamic();
    ClearStatic();
    Commit();
    if (m_scene) {
        iplSceneRelease(&m_scene);
        m_scene = nullptr;
    }
    ReleaseRetiredMeshes();
    m_context = nullptr;
    m_dirty = false;
}

bool SceneBuilder::CreateStaticMeshIn(IPLScene scene, const MeshData& mesh, IPLStaticMesh& out, const char* debugName)
{
    if (mesh.Empty() || mesh.materials.empty()) {
        SA_LOGW("Static mesh '%s' has no triangles/materials; skipped", debugName ? debugName : "?");
        return false;
    }
    IPLStaticMeshSettings settings{};
    settings.numVertices = static_cast<IPLint32>(mesh.vertices.size());
    settings.numTriangles = static_cast<IPLint32>(mesh.triangles.size());
    settings.numMaterials = static_cast<IPLint32>(mesh.materials.size());
    settings.vertices = const_cast<IPLVector3*>(mesh.vertices.data());
    settings.triangles = const_cast<IPLTriangle*>(mesh.triangles.data());
    settings.materialIndices = const_cast<IPLint32*>(mesh.materialIndices.data());
    settings.materials = const_cast<IPLMaterial*>(mesh.materials.data());

    const IPLerror err = iplStaticMeshCreate(scene, &settings, &out);
    if (err != IPL_STATUS_SUCCESS || !out) {
        SA_LOGE("iplStaticMeshCreate('%s', %d tris) failed: %s", debugName ? debugName : "?", settings.numTriangles,
                IplErrorToString(err));
        out = nullptr;
        return false;
    }
    return true;
}

bool SceneBuilder::RebuildFlattenedDynamic(DynamicEntry& entry)
{
    MeshData transformed = *entry.flattenedLocalMesh;
    const auto& m = entry.transform.elements;
    for (auto& vertex : transformed.vertices) {
        const IPLVector3 local = vertex;
        vertex = {m[0][0] * local.x + m[0][1] * local.y + m[0][2] * local.z + m[0][3],
                  m[1][0] * local.x + m[1][1] * local.y + m[1][2] * local.z + m[1][3],
                  m[2][0] * local.x + m[2][1] * local.y + m[2][2] * local.z + m[2][3]};
        if (!Vec3::FromIPL(vertex).IsFinite())
            return false;
    }
    IPLStaticMesh replacement = nullptr;
    if (!CreateStaticMeshIn(m_scene, transformed, replacement, entry.name.c_str()))
        return false;
    if (entry.subMesh) {
        iplStaticMeshRemove(entry.subMesh, m_scene);
        m_retiredMeshes.push_back(entry.subMesh);
    }
    entry.subMesh = replacement;
    iplStaticMeshAdd(entry.subMesh, m_scene);
    entry.transformDirty = false;
    return true;
}

void SceneBuilder::ReleaseRetiredMeshes()
{
    for (IPLStaticMesh& mesh : m_retiredMeshes)
        iplStaticMeshRelease(&mesh);
    m_retiredMeshes.clear();
}

bool SceneBuilder::AddStaticMesh(const MeshData& mesh, const char* debugName)
{
    if (!m_scene)
        return false;
    IPLStaticMesh handle = nullptr;
    if (!CreateStaticMeshIn(m_scene, mesh, handle, debugName))
        return false;
    iplStaticMeshAdd(handle, m_scene);
    m_staticMeshes.push_back(handle);
    m_staticTriangles += mesh.triangles.size();
    m_dirty = true;
    SA_LOGI("Static mesh '%s': %zu vertices, %zu triangles, %zu materials", debugName ? debugName : "?",
            mesh.vertices.size(), mesh.triangles.size(), mesh.materials.size());
    return true;
}

void SceneBuilder::ClearStatic()
{
    for (IPLStaticMesh& mesh : m_staticMeshes) {
        if (m_scene) {
            iplStaticMeshRemove(mesh, m_scene);
            m_retiredMeshes.push_back(mesh);
            mesh = nullptr;
        } else {
            iplStaticMeshRelease(&mesh);
        }
    }
    if (!m_staticMeshes.empty())
        m_dirty = true;
    m_staticMeshes.clear();
    m_staticTriangles = 0;
}

SceneBuilder::DynamicId SceneBuilder::AddDynamic(const MeshData& localMesh, const IPLMatrix4x4& transform,
                                                 const char* debugName)
{
    if (!m_scene || !m_context)
        return kInvalidDynamic;

    DynamicEntry entry;
    entry.name = debugName ? debugName : "";
    if (m_sceneSettings.type == IPL_SCENETYPE_RADEONRAYS) {
        entry.flattenedLocalMesh = std::make_unique<MeshData>(localMesh);
        entry.transform = transform;
        if (!RebuildFlattenedDynamic(entry))
            return kInvalidDynamic;
        const DynamicId id = m_nextDynamic++;
        if (m_nextDynamic == kInvalidDynamic) ++m_nextDynamic;
        m_dynamic.emplace(id, std::move(entry));
        m_dirty = true;
        return id;
    }

    IPLerror err = iplSceneCreate(m_context->Handle(), &m_sceneSettings, &entry.subScene);
    if (err != IPL_STATUS_SUCCESS || !entry.subScene) {
        SA_LOGE("iplSceneCreate(sub-scene '%s') failed: %s", entry.name.c_str(), IplErrorToString(err));
        return kInvalidDynamic;
    }
    if (!CreateStaticMeshIn(entry.subScene, localMesh, entry.subMesh, debugName)) {
        iplSceneRelease(&entry.subScene);
        return kInvalidDynamic;
    }
    iplStaticMeshAdd(entry.subMesh, entry.subScene);
    iplSceneCommit(entry.subScene);

    IPLInstancedMeshSettings settings{};
    settings.subScene = entry.subScene;
    settings.transform = transform;
    err = iplInstancedMeshCreate(m_scene, &settings, &entry.instance);
    if (err != IPL_STATUS_SUCCESS || !entry.instance) {
        SA_LOGE("iplInstancedMeshCreate('%s') failed: %s", entry.name.c_str(), IplErrorToString(err));
        iplStaticMeshRemove(entry.subMesh, entry.subScene);
        iplStaticMeshRelease(&entry.subMesh);
        iplSceneRelease(&entry.subScene);
        return kInvalidDynamic;
    }
    iplInstancedMeshAdd(entry.instance, m_scene);

    const DynamicId id = m_nextDynamic++;
    if (m_nextDynamic == kInvalidDynamic)
        ++m_nextDynamic;
    m_dynamic.emplace(id, std::move(entry));
    m_dirty = true;
    return id;
}

void SceneBuilder::UpdateDynamic(DynamicId id, const IPLMatrix4x4& transform)
{
    auto it = m_dynamic.find(id);
    if (it == m_dynamic.end() || !m_scene)
        return;
    if (it->second.flattenedLocalMesh) {
        it->second.transform = transform;
        it->second.transformDirty = true;
    } else {
        iplInstancedMeshUpdateTransform(it->second.instance, m_scene, transform);
    }
    m_dirty = true;
}

void SceneBuilder::RemoveDynamic(DynamicId id)
{
    auto it = m_dynamic.find(id);
    if (it == m_dynamic.end())
        return;
    DynamicEntry& e = it->second;
    if (m_scene && e.instance)
        iplInstancedMeshRemove(e.instance, m_scene);
    if (e.instance)
        iplInstancedMeshRelease(&e.instance);
    if (e.subMesh) {
        if (e.flattenedLocalMesh && m_scene) {
            iplStaticMeshRemove(e.subMesh, m_scene);
            m_retiredMeshes.push_back(e.subMesh);
            e.subMesh = nullptr;
        } else {
            if (e.subScene)
                iplStaticMeshRemove(e.subMesh, e.subScene);
            iplStaticMeshRelease(&e.subMesh);
        }
    }
    if (e.subScene)
        iplSceneRelease(&e.subScene);
    m_dynamic.erase(it);
    m_dirty = true;
}

void SceneBuilder::ClearDynamic()
{
    std::vector<DynamicId> ids;
    ids.reserve(m_dynamic.size());
    for (const auto& kv : m_dynamic)
        ids.push_back(kv.first);
    for (DynamicId id : ids)
        RemoveDynamic(id);
}

void SceneBuilder::Commit()
{
    if (!m_scene || !m_dirty)
        return;
    for (auto& item : m_dynamic) {
        auto& entry = item.second;
        if (entry.flattenedLocalMesh && entry.transformDirty && !RebuildFlattenedDynamic(entry)) {
            SA_LOGW("Could not update flattened dynamic mesh '%s'; retaining previous geometry", entry.name.c_str());
            entry.transformDirty = false;
        }
    }
    iplSceneCommit(m_scene);
    ReleaseRetiredMeshes();
    m_dirty = false;
}

void SceneBuilder::SaveObj(const std::string& baseName) const
{
    if (!m_scene)
        return;
    iplSceneSaveOBJ(m_scene, const_cast<IPLstring>(baseName.c_str()));
}

} // namespace sa
