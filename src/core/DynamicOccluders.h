// src/core/DynamicOccluders.h
//
// Turns a per-tick snapshot of the client's entities into dynamic acoustic
// geometry (B9/B10):
//
//   * brush entities ("*N" models: doors, elevators, func_movelinear...) are
//     already part of the scene as instanced brush models; they only need
//     their transform pushed when they move;
//   * physics/dynamic props, ragdolls and vehicles get the model's collision
//     mesh (.phy) or, failing that, its hull box, loaded once per model and
//     instanced per entity;
//   * other players are approximated by their collision hull box (their .phy
//     is a ragdoll and does not match the standing pose).
//
// The snapshot comes either from the native IClientEntityList walk
// (ClientEntityList) or from Lua (steamaudio.UpdateEntities). Both produce
// EntitySnapshot records so the tracker does not care about the source.
//
// Budgeting: only the closest `maxOccluders` candidates within `rangeUnits`
// of the listener are instanced; props smaller than `minExtentUnits` are
// skipped; at most `modelLoadsPerUpdate` collision models are read per
// update so a burst of new props cannot stall the game thread; an entity
// whose bounds contain the listener (the vehicle you sit in, your own
// player) is never an occluder.
//
// Thread-safety: game thread only. Geometry reaches the simulation thread
// through IDynamicGeometrySink (SimulationThread command queue).
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "steamaudio/SceneBuilder.h"
#include "steamaudio/StaticPropResolver.h"
#include "util/Math.h"

namespace sa {

using DynamicGeometryId = uint32_t;

// Source SOLID_* values (const.h).
constexpr int32_t kSolidNone = 0;
constexpr int32_t kSolidBsp = 1;
constexpr int32_t kSolidBBox = 2;
constexpr int32_t kSolidVPhysics = 6;

struct EntitySnapshot {
    int32_t index = -1;         // entity index (1..MAX_EDICTS); 0 = world (ignored)
    uint32_t serial = 0;        // handle serial / generation; 0 when unknown
    std::string model;          // "models/x.mdl", "*12" (brush model) or ""
    Vec3 origin{};              // collision origin, Source units
    Vec3 angles{};              // collision angles (pitch, yaw, roll), degrees
    Vec3 mins{};                // collision OBB in entity space (zero when unknown)
    Vec3 maxs{};
    int32_t solid = kSolidVPhysics;
    bool player = false;
    bool dormant = false;
};

struct DynamicOccluderOptions {
    bool enabled = true;            // snd_sa_dynamic_geometry
    bool brushModels = true;        // push transforms of moving brush entities
    bool props = true;              // snd_sa_dynamic_props
    bool players = true;            // snd_sa_dynamic_players
    bool boxFallback = true;        // hull box when the .phy is missing/unusable
    int32_t maxOccluders = 256;     // snd_sa_dynamic_max (props + players)
    float rangeUnits = 3000.f;      // snd_sa_dynamic_range; <= 0 => unlimited
    float minExtentUnits = 8.f;     // snd_sa_dynamic_min_size (largest OBB side)
    int32_t modelLoadsPerUpdate = 2;
    float modelLoadBudgetMs = 0.f;
    size_t maxModelCache = 512;
    size_t maxModelTriangles = 16384; // larger collision meshes are box-approximated
    float positionEpsilonUnits = 0.5f;
    float angleEpsilonDegrees = 0.25f;
    float listenerMarginUnits = 8.f; // bounds inflation for the "listener inside" test
    int32_t missedUpdatesBeforeRemove = 2;
    float unitsPerMeter = kDefaultUnitsPerMeter;
};

// Where the geometry goes. Implemented by AudioEngine over SimulationThread
// and by the tests with a recording fake.
class IDynamicGeometrySink {
public:
    virtual ~IDynamicGeometrySink() = default;
    // `localMesh` is in Steam Audio space (model space converted with
    // CoordinateConverter::PositionToSA); `transform` is in Source space.
    // Returns 0 on failure.
    virtual DynamicGeometryId AddMesh(std::shared_ptr<const MeshData> localMesh, const Transform& transform,
                                      const std::string& debugName) = 0;
    virtual bool UpdateMesh(DynamicGeometryId id, const Transform& transform) = 0;
    virtual bool RemoveMesh(DynamicGeometryId id) = 0;
    virtual bool UpdateBrushModel(int32_t modelIndex, const Vec3& origin, const Vec3& angles) = 0;
};

class DynamicOccluders {
public:
    struct Stats {
        size_t tracked = 0;          // entities with geometry in the scene
        size_t props = 0;
        size_t players = 0;
        size_t brushModels = 0;      // brush entities seen in the last update
        size_t modelCache = 0;       // distinct models loaded
        size_t modelsMissing = 0;    // models with neither .phy nor .mdl
        size_t triangles = 0;        // triangles instanced (sum over tracked)
        size_t skippedRange = 0;     // last update
        size_t skippedSmall = 0;
        size_t skippedLimit = 0;
        size_t skippedInside = 0;
        size_t pendingLoads = 0;     // candidates waiting for their model to load
        uint64_t updates = 0;
        uint64_t added = 0;
        uint64_t removed = 0;
        uint64_t transformUpdates = 0;
        uint64_t brushUpdates = 0;
        uint64_t modelLoads = 0;
        uint64_t modelBudgetDeferrals = 0;
        uint32_t lastModelLoadMicros = 0;
        uint32_t maxModelLoadMicros = 0;
        uint32_t lastFileReadMicros = 0;
        uint32_t maxFileReadMicros = 0;
    };

    struct TrackedInfo {
        int32_t index = -1;
        std::string model;
        DynamicGeometryId id = 0;
        bool player = false;
        bool fromCollision = false;
        size_t triangles = 0;
        Vec3 origin{};
    };

    DynamicOccluders();

    void Configure(const DynamicOccluderOptions& options);
    const DynamicOccluderOptions& Options() const { return m_options; }
    // Material library for .phy/.mdl $surfaceprop resolution (may be null =>
    // generic material).
    void SetMaterials(const MaterialLibrary* materials) { m_materials = materials; }
    // Reads game files ("models/x.phy"); required for prop geometry.
    void SetReader(GameFileReader reader) { m_reader = std::move(reader); }
    // Entity index of the local player; never an occluder.
    void SetLocalPlayer(int32_t index) { m_localPlayer = index; }

    // Processes one full snapshot. Entities absent for
    // `missedUpdatesBeforeRemove` consecutive updates are removed from the
    // scene. `listenerValid == false` disables the range/inside filters.
    void Update(const std::vector<EntitySnapshot>& entities, const Vec3& listener, bool listenerValid,
                IDynamicGeometrySink& sink);
    void RefreshTransforms(const std::vector<EntitySnapshot>& entities, IDynamicGeometrySink& sink);

    // Removes every tracked mesh (map unload / disable). Keeps the model cache.
    void Clear(IDynamicGeometrySink& sink);
    // Also drops the model cache (map change: model paths may resolve differently).
    void Reset(IDynamicGeometrySink& sink);

    Stats GetStats() const;
    std::vector<TrackedInfo> Tracked() const;
    // Orientation (pitch, yaw, roll) of any entity seen in the last snapshot
    // (range/size filters do not apply). Used for automatic directivity.
    bool EntityAngles(int32_t index, Vec3& angles) const;
    // Moves `position` (world, Source units) out of the collision bounds of
    // tracked entity `index` toward `listener` when it lies inside them
    // (inflated by `margin`). Sounds emitted from inside an occluder's own
    // hull would otherwise be occluded by it. Returns true when moved.
    bool PushEmitterOutside(int32_t index, const Vec3& listener, float margin, Vec3& position) const;

    // Builds the Steam Audio space local mesh for a model-space geometry.
    // Exposed for tests.
    static std::shared_ptr<const MeshData> BuildMesh(const PropModelGeometry& geometry, float unitsPerMeter,
                                                     const MaterialLibrary* materials);
    static std::shared_ptr<const MeshData> BuildBoxMesh(const Vec3& mins, const Vec3& maxs, float unitsPerMeter,
                                                        const IPLMaterial& material);
    // Largest side of the model-space AABB of `geometry` (Source units).
    static float LargestExtent(const PropModelGeometry& geometry);
    // True when `point` (world, Source units) lies inside the OBB `mins..maxs`
    // placed at `t`, inflated by `margin`.
    static bool PointInsideBounds(const Vec3& point, const Transform& t, const Vec3& mins, const Vec3& maxs,
                                  float margin);
    // Same OBB; when `point` is inside, moves it along `toward - point` to just
    // past the box surface. Returns false when outside, or when `toward` is
    // inside the box as well.
    static bool PushOutOfBounds(Vec3& point, const Vec3& toward, const Transform& t, const Vec3& mins,
                                const Vec3& maxs, float margin);

private:
    struct CachedModel {
        std::shared_ptr<const MeshData> mesh;   // null when missing
        Vec3 mins{}, maxs{};                    // model-space bounds (for the inside test)
        float extent = 0.f;
        bool fromCollision = false;
        bool missing = false;
    };

    struct TrackedEntity {
        DynamicGeometryId id = 0;
        uint32_t serial = 0;
        std::string model;
        std::shared_ptr<const MeshData> mesh;
        Vec3 origin{}, angles{};
        Vec3 mins{}, maxs{};
        bool player = false;
        bool fromCollision = false;
        int32_t missed = 0;
    };

    struct BrushTracked {
        Vec3 origin{}, angles{};
        bool initialized = false;
        bool seen = false;
    };

    const CachedModel* LoadModel(const std::string& model, int32_t& budget);
    void TrimModelCache();
    bool Moved(const Vec3& a0, const Vec3& r0, const Vec3& a1, const Vec3& r1) const;
    bool Remove(int32_t index, TrackedEntity& t, IDynamicGeometrySink& sink);
    const IPLMaterial& PlayerMaterial() const;

    DynamicOccluderOptions m_options;
    const MaterialLibrary* m_materials = nullptr;
    GameFileReader m_reader;
    int32_t m_localPlayer = -1;

    std::unordered_map<std::string, CachedModel> m_models;
    std::unordered_map<int32_t, TrackedEntity> m_tracked;
    std::unordered_map<int32_t, BrushTracked> m_brush;
    std::unordered_map<int32_t, Vec3> m_orientation;
    std::shared_ptr<const MeshData> m_playerBox; // cached for the default hull
    Vec3 m_playerBoxMins{}, m_playerBoxMaxs{};
    IPLMaterial m_defaultMaterial{};
    Stats m_stats;
    std::chrono::steady_clock::time_point m_modelDeadline = std::chrono::steady_clock::time_point::max();
};

// Default Source player standing hull.
constexpr Vec3 kPlayerHullMins{-16.f, -16.f, 0.f};
constexpr Vec3 kPlayerHullMaxs{16.f, 16.f, 72.f};

} // namespace sa
