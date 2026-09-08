// src/steamaudio/StaticPropResolver.cpp
#include "steamaudio/StaticPropResolver.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>

#include "steamaudio/PhyModel.h"
#include "util/Logging.h"

namespace sa {
namespace {

constexpr uint8_t kSolidNone = 0;
constexpr size_t kMaxMissingReported = 16;

std::string NormalizeModelPath(const std::string& model)
{
    std::string s;
    s.reserve(model.size());
    for (char c : model)
        s.push_back(c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    while (!s.empty() && s.front() == '/')
        s.erase(s.begin());
    return s;
}

std::string SwapExtension(const std::string& path, const char* extension)
{
    const size_t dot = path.find_last_of('.');
    const size_t slash = path.find_last_of('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return path + extension;
    return path.substr(0, dot) + extension;
}

bool ReadOptional(const GameFileReader& reader, const std::string& path, std::vector<uint8_t>& out, std::string& error)
{
    out.clear();
    error.clear();
    return reader && reader(path, out, error) && !out.empty();
}

bool BoxFromMdl(const MdlInfo& mdl, const StaticPropOptions& options, PropModelGeometry& out)
{
    if (!mdl.hasHull)
        return false;
    const Vec3 extent = mdl.hullMax - mdl.hullMin;
    if (extent.x < options.minBoxExtentUnits && extent.y < options.minBoxExtentUnits &&
        extent.z < options.minBoxExtentUnits)
        return false;
    AppendBox(mdl.hullMin, mdl.hullMax, out.vertices, out.triangles, 0);
    return true;
}

} // namespace

Vec3 TransformPropVertex(const Vec3& v, const Transform& t, float uniformScale)
{
    const float s = uniformScale > 1e-4f ? uniformScale : 1.f;
    // Model +X = forward, +Y = left (-right), +Z = up.
    return t.origin + t.forward * (v.x * s) - t.right * (v.y * s) + t.up * (v.z * s);
}

bool LoadPropModelGeometry(const std::string& modelPath, const GameFileReader& reader,
                           const StaticPropOptions& options, PropModelGeometry& out)
{
    out = PropModelGeometry{};
    const std::string mdlPath = NormalizeModelPath(modelPath);
    if (mdlPath.empty()) {
        out.error = "empty model path";
        return false;
    }
    const std::string phyPath = SwapExtension(mdlPath, ".phy");

    std::vector<uint8_t> bytes;
    std::string error;
    PhyModel phy;
    bool havePhy = false;
    if (ReadOptional(reader, phyPath, bytes, error)) {
        havePhy = ParsePhy(bytes.data(), bytes.size(), phy);
        if (!havePhy)
            out.error = phyPath + ": " + phy.error;
    } else {
        out.error = phyPath + ": " + (error.empty() ? "not found" : error);
    }

    MdlInfo mdl;
    bool haveMdl = false;
    const bool needMdl = !havePhy || phy.solids.empty() || phy.solids[0].surfaceProp.empty();
    if (needMdl && ReadOptional(reader, mdlPath, bytes, error))
        haveMdl = ParseMdlInfo(bytes.data(), bytes.size(), mdl);

    std::string surfaceProp;
    if (havePhy) {
        for (const PhySolid& s : phy.solids)
            if (!s.surfaceProp.empty()) {
                surfaceProp = s.surfaceProp;
                break;
            }
    }
    if (surfaceProp.empty() && haveMdl)
        surfaceProp = mdl.surfaceProp;
    out.materialNames.push_back(surfaceProp);

    if (havePhy && phy.TriangleCount() <= options.maxModelTriangles) {
        for (const std::string& m : phy.materialTable)
            out.materialNames.push_back(m);
        for (const PhySolid& solid : phy.solids) {
            const uint32_t base = static_cast<uint32_t>(out.vertices.size());
            out.vertices.insert(out.vertices.end(), solid.vertices.begin(), solid.vertices.end());
            for (PhyTriangle tri : solid.triangles) {
                for (uint32_t& i : tri.indices)
                    i += base;
                if (tri.materialIndex >= out.materialNames.size())
                    tri.materialIndex = 0;
                out.triangles.push_back(tri);
            }
        }
        out.fromCollision = !out.triangles.empty();
        out.valid = out.fromCollision;
        if (out.valid)
            return true;
    } else if (havePhy) {
        out.error = phyPath + ": " + std::to_string(phy.TriangleCount()) + " triangles exceed per-model cap";
    }

    if (!options.boxFallback)
        return false;
    if (!haveMdl && !needMdl && ReadOptional(reader, mdlPath, bytes, error))
        haveMdl = ParseMdlInfo(bytes.data(), bytes.size(), mdl);
    if (!haveMdl) {
        if (out.error.empty())
            out.error = mdlPath + ": " + (error.empty() ? "not found" : error);
        return false;
    }
    out.vertices.clear();
    out.triangles.clear();
    out.materialNames.resize(1);
    if (!BoxFromMdl(mdl, options, out)) {
        out.error = mdlPath + ": hull box empty";
        return false;
    }
    out.fromCollision = false;
    out.valid = true;
    return true;
}

void ResolveStaticProps(const std::vector<BspStaticProp>& props, const GameFileReader& reader,
                        const CoordinateConverter& converter, const MaterialLibrary& materials,
                        const StaticPropOptions& options, MeshData& out, StaticPropStats& stats)
{
    stats = StaticPropStats{};
    if (!options.enabled || props.empty())
        return;

    struct CachedModel {
        PropModelGeometry geometry;
        std::vector<IPLint32> materialIndices; // per materialNames entry, into `out.materials`
    };
    std::unordered_map<std::string, CachedModel> cache;

    for (const BspStaticProp& prop : props) {
        ++stats.placements;
        if (prop.solid == kSolidNone && !options.includeNonSolid) {
            ++stats.skippedNonSolid;
            continue;
        }
        const std::string key = NormalizeModelPath(prop.model);
        auto it = cache.find(key);
        if (it == cache.end()) {
            CachedModel model;
            ++stats.distinctModels;
            if (!LoadPropModelGeometry(key, reader, options, model.geometry)) {
                ++stats.missingModels;
                if (stats.missing.size() < kMaxMissingReported)
                    stats.missing.push_back(model.geometry.error);
            } else {
                for (const std::string& name : model.geometry.materialNames) {
                    const IPLMaterial& m = name.empty() ? materials.Generic() : materials.FromSurfaceProp(name);
                    model.materialIndices.push_back(out.AddMaterial(m));
                }
            }
            it = cache.emplace(key, std::move(model)).first;
        }
        const CachedModel& model = it->second;
        if (!model.geometry.valid)
            continue;
        if (stats.triangles + model.geometry.triangles.size() > options.maxTriangles) {
            SA_LOGW("[props] static prop triangle budget (%zu) reached; remaining props skipped",
                    options.maxTriangles);
            break;
        }

        const Transform t = Transform::FromAngles(prop.origin, prop.angles);
        const IPLint32 vertexBase = static_cast<IPLint32>(out.vertices.size());
        out.vertices.reserve(out.vertices.size() + model.geometry.vertices.size());
        for (const Vec3& v : model.geometry.vertices) {
            const Vec3 world = TransformPropVertex(v, t, prop.uniformScale);
            out.vertices.push_back(converter.PositionToSA(world).ToIPL());
        }
        for (const PhyTriangle& tri : model.geometry.triangles) {
            IPLTriangle ipl{};
            for (int k = 0; k < 3; ++k)
                ipl.indices[k] = vertexBase + static_cast<IPLint32>(tri.indices[k]);
            out.triangles.push_back(ipl);
            const size_t mat = tri.materialIndex < model.materialIndices.size() ? tri.materialIndex : 0;
            out.materialIndices.push_back(model.materialIndices.empty() ? 0 : model.materialIndices[mat]);
        }
        stats.triangles += model.geometry.triangles.size();
        ++stats.instanced;
        if (model.geometry.fromCollision)
            ++stats.fromPhy;
        else
            ++stats.fromBox;
    }
}

} // namespace sa
