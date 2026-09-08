// src/steamaudio/SceneBuilder.h
//
// Converts engine geometry into Steam Audio scenes.
//
//   * Static world geometry (BSP brushes, displacements, static props) becomes
//     one or more IPLStaticMesh objects in the main IPLScene.
//   * Dynamic geometry (brush entities, moving props) becomes an
//     IPLInstancedMesh backed by a per-model sub-scene; only the transform is
//     updated as the entity moves.
//
// Materials are resolved by Source $surfaceprop name through MaterialLibrary.
//
// Thread-safety: all methods are called from the simulation thread only.
// Steam Audio requires iplSceneCommit to happen while no simulation is
// running; SimulationThread guarantees this by serializing scene edits and
// simulator runs on the same thread.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "PhononApi.h"
#include "steamaudio/Backend.h"
#include "util/Math.h"

namespace sa {

class PhononContext;
class SurfacePropDatabase;

// Geometry already converted into Steam Audio coordinates (meters, Y-up).
struct MeshData {
    std::vector<IPLVector3> vertices;
    std::vector<IPLTriangle> triangles;
    std::vector<IPLint32> materialIndices; // one per triangle, indexes `materials`
    std::vector<IPLMaterial> materials;

    bool Empty() const { return triangles.empty(); }
    void Clear();
    // Appends another mesh, remapping indices.
    void Append(const MeshData& other);
    // Adds a triangle, welding nothing (callers dedupe vertices beforehand).
    void AddTriangle(const IPLVector3& a, const IPLVector3& b, const IPLVector3& c, IPLint32 material);
    // Ensures `material` exists in `materials`, returning its index.
    IPLint32 AddMaterial(const IPLMaterial& material);
    // Removes degenerate triangles and unreferenced vertices.
    void Compact();
};

// Acoustic material database keyed by Source surfaceprop / texture name.
class MaterialLibrary {
public:
    MaterialLibrary();

    // Resolves a Source $surfaceprop (e.g. "concrete", "metalgrate") to an
    // acoustic material: exact name, alias, then the surfaceproperties.txt
    // inheritance chain / gamematerial family (when a database is attached),
    // then name heuristics. Unknown names fall back to a generic material.
    const IPLMaterial& FromSurfaceProp(const std::string& surfaceProp) const;
    // Same resolution, returning the library key that matched ("metal",
    // "wood_hollow", ...) or "generic".
    std::string ResolveName(const std::string& surfaceProp) const;

    // Heuristic material lookup from a texture path (e.g. "concrete/concretefloor001a").
    const IPLMaterial& FromTextureName(const std::string& textureName) const;
    std::string NameFromTexture(const std::string& textureName) const;

    // True when `lowerName` is a base material or alias of this library.
    bool Knows(const std::string& lowerName) const;

    // Attaches the game's surfaceproperties database (may be null).
    void SetSurfaceProps(std::shared_ptr<const SurfacePropDatabase> database) { m_surfaceProps = std::move(database); }
    const SurfacePropDatabase* SurfaceProps() const { return m_surfaceProps.get(); }

    // Loads user overrides: {"surfaceprops": {"name": {"absorption":[..3],"scattering":x,"transmission":[..3]}},
    //                        "aliases": {"metalvent": "metal"}}
    bool LoadOverrides(const std::string& jsonPath);

    const IPLMaterial& Generic() const { return m_generic; }

private:
    void AddBase(const char* name, const IPLMaterial& material);
    void AddAlias(const char* alias, const char* target);

    const IPLMaterial& ByName(const std::string& name) const;

    std::unordered_map<std::string, IPLMaterial> m_materials;
    std::unordered_map<std::string, std::string> m_aliases;
    std::shared_ptr<const SurfacePropDatabase> m_surfaceProps;
    IPLMaterial m_generic{};
};

class SceneBuilder {
public:
    using DynamicId = uint32_t;
    static constexpr DynamicId kInvalidDynamic = 0;

    SceneBuilder() = default;
    ~SceneBuilder();
    SceneBuilder(const SceneBuilder&) = delete;
    SceneBuilder& operator=(const SceneBuilder&) = delete;

    bool Initialize(PhononContext& context, const BackendDevices& devices);
    void Shutdown();

    bool IsValid() const { return m_scene != nullptr; }
    IPLScene Scene() const { return m_scene; }
    IPLSceneType SceneType() const { return m_sceneSettings.type; }

    // Static geometry. Each call adds a mesh; ClearStatic removes them all.
    // Returns false if Steam Audio rejected the mesh.
    bool AddStaticMesh(const MeshData& mesh, const char* debugName);
    void ClearStatic();
    size_t StaticTriangleCount() const { return m_staticTriangles; }

    // Dynamic geometry. `localMesh` is in the entity's model space (already in
    // SA units/axes). The returned id is used for transform updates/removal.
    DynamicId AddDynamic(const MeshData& localMesh, const IPLMatrix4x4& transform, const char* debugName);
    void UpdateDynamic(DynamicId id, const IPLMatrix4x4& transform);
    void RemoveDynamic(DynamicId id);
    void ClearDynamic();
    size_t DynamicCount() const { return m_dynamic.size(); }

    // Applies pending edits. Must be called before the next simulation run
    // and never concurrently with one.
    void Commit();
    bool HasPendingChanges() const { return m_dirty; }

    // Debug dump (Steam Audio writes <base>.obj/.mtl).
    void SaveObj(const std::string& baseName) const;

private:
    struct DynamicEntry {
        IPLScene subScene = nullptr;
        IPLStaticMesh subMesh = nullptr;
        IPLInstancedMesh instance = nullptr;
        std::string name;
    };

    bool CreateStaticMeshIn(IPLScene scene, const MeshData& mesh, IPLStaticMesh& out, const char* debugName);

    PhononContext* m_context = nullptr;
    IPLSceneSettings m_sceneSettings{};
    IPLScene m_scene = nullptr;
    std::vector<IPLStaticMesh> m_staticMeshes;
    size_t m_staticTriangles = 0;
    std::unordered_map<DynamicId, DynamicEntry> m_dynamic;
    DynamicId m_nextDynamic = 1;
    bool m_dirty = false;
};

} // namespace sa
