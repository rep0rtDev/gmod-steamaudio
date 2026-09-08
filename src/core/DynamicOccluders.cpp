// src/core/DynamicOccluders.cpp
#include "core/DynamicOccluders.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>

#include "util/Logging.h"

namespace sa {

namespace {

constexpr size_t kMaxSnapshotEntities = 16384;

bool IsBrushModel(const std::string& model, int32_t& index)
{
    if (model.size() < 2 || model[0] != '*')
        return false;
    int32_t v = 0;
    for (size_t i = 1; i < model.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(model[i])))
            return false;
        v = v * 10 + (model[i] - '0');
        if (v > 65535)
            return false;
    }
    index = v;
    return v > 0;
}

std::string NormalizeModel(const std::string& model)
{
    std::string s;
    s.reserve(model.size());
    for (char c : model)
        s.push_back(c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    while (!s.empty() && s.front() == '/')
        s.erase(s.begin());
    return s;
}

bool Finite(const Vec3& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool HasVolume(const Vec3& mins, const Vec3& maxs)
{
    return maxs.x > mins.x && maxs.y > mins.y && maxs.z > mins.z;
}

float LargestSide(const Vec3& mins, const Vec3& maxs)
{
    return std::max({maxs.x - mins.x, maxs.y - mins.y, maxs.z - mins.z});
}

bool SameBounds(const Vec3& mins, const Vec3& maxs, const Vec3& refMins, const Vec3& refMaxs)
{
    return (mins - refMins).LengthSq() < 1e-4f && (maxs - refMaxs).LengthSq() < 1e-4f;
}

// Smallest absolute angular difference in degrees.
float AngleDelta(float a, float b)
{
    float d = std::fmod(a - b, 360.f);
    if (d > 180.f)
        d -= 360.f;
    if (d < -180.f)
        d += 360.f;
    return std::fabs(d);
}

} // namespace

// ---------------------------------------------------------------------------
// Mesh helpers
// ---------------------------------------------------------------------------
std::shared_ptr<const MeshData> DynamicOccluders::BuildMesh(const PropModelGeometry& geometry, float unitsPerMeter,
                                                            const MaterialLibrary* materials)
{
    if (!geometry.valid || geometry.triangles.empty())
        return nullptr;
    CoordinateConverter converter;
    converter.SetUnitsPerMeter(unitsPerMeter);
    auto mesh = std::make_shared<MeshData>();
    std::vector<IPLint32> materialIndices;
    materialIndices.reserve(geometry.materialNames.size());
    IPLMaterial generic{};
    if (materials)
        generic = materials->Generic();
    else
        generic = IPLMaterial{{0.10f, 0.20f, 0.30f}, 0.05f, {0.100f, 0.050f, 0.030f}};
    for (const std::string& name : geometry.materialNames) {
        const IPLMaterial& m = (materials && !name.empty()) ? materials->FromSurfaceProp(name) : generic;
        materialIndices.push_back(mesh->AddMaterial(m));
    }
    if (materialIndices.empty())
        materialIndices.push_back(mesh->AddMaterial(generic));

    mesh->vertices.reserve(geometry.vertices.size());
    for (const Vec3& v : geometry.vertices)
        mesh->vertices.push_back(converter.PositionToSA(v).ToIPL());
    mesh->triangles.reserve(geometry.triangles.size());
    mesh->materialIndices.reserve(geometry.triangles.size());
    for (const PhyTriangle& tri : geometry.triangles) {
        IPLTriangle ipl{};
        bool ok = true;
        for (int k = 0; k < 3; ++k) {
            if (tri.indices[k] >= geometry.vertices.size()) {
                ok = false;
                break;
            }
            ipl.indices[k] = static_cast<IPLint32>(tri.indices[k]);
        }
        if (!ok)
            continue;
        mesh->triangles.push_back(ipl);
        const size_t mat = tri.materialIndex < materialIndices.size() ? tri.materialIndex : 0;
        mesh->materialIndices.push_back(materialIndices[mat]);
    }
    mesh->Compact();
    if (mesh->Empty())
        return nullptr;
    return mesh;
}

std::shared_ptr<const MeshData> DynamicOccluders::BuildBoxMesh(const Vec3& mins, const Vec3& maxs,
                                                               float unitsPerMeter, const IPLMaterial& material)
{
    if (!Finite(mins) || !Finite(maxs) || !HasVolume(mins, maxs))
        return nullptr;
    CoordinateConverter converter;
    converter.SetUnitsPerMeter(unitsPerMeter);
    auto mesh = std::make_shared<MeshData>();
    const IPLint32 mat = mesh->AddMaterial(material);
    const Vec3 corners[8] = {
        {mins.x, mins.y, mins.z}, {maxs.x, mins.y, mins.z}, {maxs.x, maxs.y, mins.z}, {mins.x, maxs.y, mins.z},
        {mins.x, mins.y, maxs.z}, {maxs.x, mins.y, maxs.z}, {maxs.x, maxs.y, maxs.z}, {mins.x, maxs.y, maxs.z},
    };
    IPLVector3 v[8];
    for (int i = 0; i < 8; ++i)
        v[i] = converter.PositionToSA(corners[i]).ToIPL();
    // Outward-facing quads (Steam Audio treats triangles as double sided, the
    // order only matters for consistency).
    const int quads[6][4] = {
        {0, 3, 2, 1}, // bottom (-z)
        {4, 5, 6, 7}, // top (+z)
        {0, 1, 5, 4}, // -y
        {2, 3, 7, 6}, // +y
        {1, 2, 6, 5}, // +x
        {3, 0, 4, 7}, // -x
    };
    for (const auto& q : quads) {
        mesh->AddTriangle(v[q[0]], v[q[1]], v[q[2]], mat);
        mesh->AddTriangle(v[q[0]], v[q[2]], v[q[3]], mat);
    }
    mesh->Compact();
    return mesh;
}

float DynamicOccluders::LargestExtent(const PropModelGeometry& geometry)
{
    if (geometry.vertices.empty())
        return 0.f;
    Vec3 mins{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    Vec3 maxs{-mins.x, -mins.y, -mins.z};
    for (const Vec3& v : geometry.vertices) {
        mins.x = std::min(mins.x, v.x);
        mins.y = std::min(mins.y, v.y);
        mins.z = std::min(mins.z, v.z);
        maxs.x = std::max(maxs.x, v.x);
        maxs.y = std::max(maxs.y, v.y);
        maxs.z = std::max(maxs.z, v.z);
    }
    return LargestSide(mins, maxs);
}

bool DynamicOccluders::PointInsideBounds(const Vec3& point, const Transform& t, const Vec3& mins, const Vec3& maxs,
                                         float margin)
{
    if (!HasVolume(mins, maxs))
        return false;
    const Vec3 d = point - t.origin;
    // Model axes: +X forward, +Y left (= -right), +Z up.
    const float x = d.Dot(t.forward);
    const float y = -d.Dot(t.right);
    const float z = d.Dot(t.up);
    return x >= mins.x - margin && x <= maxs.x + margin && y >= mins.y - margin && y <= maxs.y + margin &&
           z >= mins.z - margin && z <= maxs.z + margin;
}

bool DynamicOccluders::PushOutOfBounds(Vec3& point, const Vec3& toward, const Transform& t, const Vec3& mins,
                                       const Vec3& maxs, float margin)
{
    if (!PointInsideBounds(point, t, mins, maxs, margin))
        return false;
    const Vec3 delta = toward - point;
    const float length = delta.Length();
    if (length < 1e-3f)
        return false;
    const Vec3 dir = delta / length;
    // Local (model) axes: +X forward, +Y left (= -right), +Z up.
    const Vec3 d = point - t.origin;
    const float p[3] = {d.Dot(t.forward), -d.Dot(t.right), d.Dot(t.up)};
    const float v[3] = {dir.Dot(t.forward), -dir.Dot(t.right), dir.Dot(t.up)};
    const float lo[3] = {mins.x - margin, mins.y - margin, mins.z - margin};
    const float hi[3] = {maxs.x + margin, maxs.y + margin, maxs.z + margin};
    // Distance along `dir` at which the ray leaves the inflated box: the
    // nearest of the per-axis slab exits.
    float exit = std::numeric_limits<float>::infinity();
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(v[i]) < 1e-6f)
            continue;
        const float bound = v[i] > 0.f ? hi[i] : lo[i];
        exit = std::min(exit, (bound - p[i]) / v[i]);
    }
    if (!std::isfinite(exit) || exit < 0.f || exit >= length)
        return false;
    point += dir * (exit + 0.5f);
    return true;
}

// ---------------------------------------------------------------------------
// Tracker
// ---------------------------------------------------------------------------
DynamicOccluders::DynamicOccluders()
{
    m_defaultMaterial = IPLMaterial{{0.10f, 0.20f, 0.30f}, 0.05f, {0.100f, 0.050f, 0.030f}};
}

void DynamicOccluders::Configure(const DynamicOccluderOptions& options)
{
    const bool unitsChanged = std::fabs(options.unitsPerMeter - m_options.unitsPerMeter) > 1e-4f;
    m_options = options;
    m_options.maxOccluders = std::max(0, m_options.maxOccluders);
    m_options.modelLoadsPerUpdate = std::max(1, m_options.modelLoadsPerUpdate);
    m_options.missedUpdatesBeforeRemove = std::max(1, m_options.missedUpdatesBeforeRemove);
    if (unitsChanged) {
        // Cached meshes are in meters; they must be rebuilt with the new scale.
        m_models.clear();
        m_playerBox.reset();
    }
}

const IPLMaterial& DynamicOccluders::PlayerMaterial() const
{
    return m_materials ? m_materials->FromSurfaceProp("flesh") : m_defaultMaterial;
}

const DynamicOccluders::CachedModel* DynamicOccluders::LoadModel(const std::string& model, int32_t& budget)
{
    auto it = m_models.find(model);
    if (it != m_models.end())
        return &it->second;
    if (budget <= 0)
        return nullptr;
    --budget;

    CachedModel cached;
    if (!m_reader) {
        cached.missing = true;
    } else {
        StaticPropOptions options;
        options.boxFallback = m_options.boxFallback;
        options.maxModelTriangles = m_options.maxModelTriangles;
        PropModelGeometry geometry;
        if (LoadPropModelGeometry(model, m_reader, options, geometry) && geometry.valid) {
            cached.mesh = BuildMesh(geometry, m_options.unitsPerMeter, m_materials);
            cached.fromCollision = geometry.fromCollision;
            cached.extent = LargestExtent(geometry);
            Vec3 mins{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                      std::numeric_limits<float>::max()};
            Vec3 maxs{-mins.x, -mins.y, -mins.z};
            for (const Vec3& v : geometry.vertices) {
                mins.x = std::min(mins.x, v.x);
                mins.y = std::min(mins.y, v.y);
                mins.z = std::min(mins.z, v.z);
                maxs.x = std::max(maxs.x, v.x);
                maxs.y = std::max(maxs.y, v.y);
                maxs.z = std::max(maxs.z, v.z);
            }
            cached.mins = mins;
            cached.maxs = maxs;
            cached.missing = cached.mesh == nullptr;
        } else {
            cached.missing = true;
            SA_LOGD("[dyn] %s: %s", model.c_str(), geometry.error.c_str());
        }
    }
    if (cached.missing)
        ++m_stats.modelsMissing;
    TrimModelCache();
    return &m_models.emplace(model, std::move(cached)).first->second;
}

void DynamicOccluders::TrimModelCache()
{
    if (m_models.size() < m_options.maxModelCache)
        return;
    // Drop models no tracked entity references (their meshes are only held
    // by the cache).
    for (auto it = m_models.begin(); it != m_models.end() && m_models.size() >= m_options.maxModelCache;) {
        if (!it->second.mesh || it->second.mesh.use_count() == 1)
            it = m_models.erase(it);
        else
            ++it;
    }
}

bool DynamicOccluders::Moved(const Vec3& o0, const Vec3& a0, const Vec3& o1, const Vec3& a1) const
{
    if ((o1 - o0).LengthSq() > m_options.positionEpsilonUnits * m_options.positionEpsilonUnits)
        return true;
    const float e = m_options.angleEpsilonDegrees;
    return AngleDelta(a0.x, a1.x) > e || AngleDelta(a0.y, a1.y) > e || AngleDelta(a0.z, a1.z) > e;
}

bool DynamicOccluders::Remove(int32_t index, TrackedEntity& t, IDynamicGeometrySink& sink)
{
    (void)index;
    if (t.id == 0)
        return true;
    if (!sink.RemoveMesh(t.id))
        return false;
    t.id = 0;
    ++m_stats.removed;
    return true;
}

void DynamicOccluders::Update(const std::vector<EntitySnapshot>& entities, const Vec3& listener, bool listenerValid,
                              IDynamicGeometrySink& sink)
{
    ++m_stats.updates;
    m_stats.skippedRange = m_stats.skippedSmall = m_stats.skippedLimit = m_stats.skippedInside = 0;
    m_stats.pendingLoads = 0;
    m_stats.brushModels = 0;

    if (!m_options.enabled)
        Clear(sink);

    // Facing cache for automatic directivity; kept even when geometry is off.
    m_orientation.clear();
    {
        size_t seen = 0;
        for (const EntitySnapshot& e : entities) {
            if (++seen > kMaxSnapshotEntities)
                break;
            if (e.index > 0 && !e.dormant && Finite(e.angles))
                m_orientation[e.index] = e.angles;
        }
    }

    if (!m_options.enabled)
        return;

    for (auto& kv : m_tracked)
        ++kv.second.missed;
    for (auto& kv : m_brush)
        kv.second.seen = false;

    struct Candidate {
        const EntitySnapshot* snap;
        std::string model;
        float distSq;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(std::min(entities.size(), kMaxSnapshotEntities));

    const bool useRange = listenerValid && m_options.rangeUnits > 0.f;
    const float rangeSq = m_options.rangeUnits * m_options.rangeUnits;

    size_t considered = 0;
    for (const EntitySnapshot& e : entities) {
        if (++considered > kMaxSnapshotEntities)
            break;
        if (e.index <= 0 || e.dormant || !Finite(e.origin) || !Finite(e.angles))
            continue;
        if (e.index == m_localPlayer)
            continue;
        const std::string model = NormalizeModel(e.model);
        int32_t brushIndex = 0;
        if (IsBrushModel(model, brushIndex)) {
            ++m_stats.brushModels;
            if (!m_options.brushModels)
                continue;
            BrushTracked& b = m_brush[brushIndex];
            b.seen = true;
            // First sighting always pushes (the scene registered the brush at
            // its BSP spawn origin, which may differ from the live one).
            if (!b.initialized || Moved(b.origin, b.angles, e.origin, e.angles)) {
                if (sink.UpdateBrushModel(brushIndex, e.origin, e.angles)) {
                    ++m_stats.brushUpdates;
                    b.initialized = true;
                    b.origin = e.origin;
                    b.angles = e.angles;
                }
            }
            continue;
        }
        if (e.solid == kSolidNone)
            continue;
        if (e.player ? !m_options.players : !m_options.props)
            continue;
        if (!e.player && model.empty())
            continue;
        float distSq = 0.f;
        if (listenerValid)
            distSq = (e.origin - listener).LengthSq();
        if (useRange && distSq > rangeSq) {
            ++m_stats.skippedRange;
            continue;
        }
        candidates.push_back({&e, model, distSq});
    }

    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate& a, const Candidate& b) { return a.distSq < b.distSq; });

    int32_t loadBudget = m_options.modelLoadsPerUpdate;
    size_t accepted = 0;
    for (const Candidate& c : candidates) {
        const EntitySnapshot& e = *c.snap;
        auto tracked = m_tracked.find(e.index);
        const bool known = tracked != m_tracked.end() && tracked->second.id != 0;
        // Identity change (index reused by another entity / model swap).
        if (known && (tracked->second.serial != e.serial || tracked->second.model != c.model ||
                      tracked->second.player != e.player)) {
            if (!Remove(e.index, tracked->second, sink))
                continue;
            m_tracked.erase(tracked);
            tracked = m_tracked.end();
        }
        if (tracked != m_tracked.end() && tracked->second.id != 0) {
            TrackedEntity& t = tracked->second;
            if (accepted >= static_cast<size_t>(m_options.maxOccluders)) {
                ++m_stats.skippedLimit;
                continue; // stays tracked but counts as missed => removed if this persists
            }
            const Transform tf = Transform::FromAngles(e.origin, e.angles);
            if (listenerValid &&
                PointInsideBounds(listener, tf, t.mins, t.maxs, m_options.listenerMarginUnits)) {
                ++m_stats.skippedInside;
                continue;
            }
            t.missed = 0;
            ++accepted;
            if (Moved(t.origin, t.angles, e.origin, e.angles)) {
                if (sink.UpdateMesh(t.id, tf)) {
                    ++m_stats.transformUpdates;
                    t.origin = e.origin;
                    t.angles = e.angles;
                }
            }
            continue;
        }

        if (accepted >= static_cast<size_t>(m_options.maxOccluders)) {
            ++m_stats.skippedLimit;
            continue;
        }

        // New entity: pick its geometry.
        std::shared_ptr<const MeshData> mesh;
        Vec3 mins = e.mins, maxs = e.maxs;
        bool fromCollision = false;
        if (e.player) {
            if (!HasVolume(mins, maxs) || !Finite(mins) || !Finite(maxs)) {
                mins = kPlayerHullMins;
                maxs = kPlayerHullMaxs;
            }
            if (SameBounds(mins, maxs, kPlayerHullMins, kPlayerHullMaxs)) {
                if (!m_playerBox)
                    m_playerBox = BuildBoxMesh(kPlayerHullMins, kPlayerHullMaxs, m_options.unitsPerMeter,
                                               PlayerMaterial());
                mesh = m_playerBox;
            } else {
                mesh = BuildBoxMesh(mins, maxs, m_options.unitsPerMeter, PlayerMaterial());
            }
        } else {
            const CachedModel* cached = LoadModel(c.model, loadBudget);
            if (!cached) {
                ++m_stats.pendingLoads;
                continue; // budget exhausted; retried next update
            }
            if (cached->missing || !cached->mesh) {
                // No collision data at all: fall back to the entity's OBB when
                // the snapshot carries one.
                if (m_options.boxFallback && HasVolume(mins, maxs) && Finite(mins) && Finite(maxs)) {
                    if (LargestSide(mins, maxs) < m_options.minExtentUnits) {
                        ++m_stats.skippedSmall;
                        continue;
                    }
                    mesh = BuildBoxMesh(mins, maxs, m_options.unitsPerMeter,
                                        m_materials ? m_materials->Generic() : m_defaultMaterial);
                } else {
                    continue;
                }
            } else {
                if (cached->extent < m_options.minExtentUnits) {
                    ++m_stats.skippedSmall;
                    continue;
                }
                mesh = cached->mesh;
                mins = cached->mins;
                maxs = cached->maxs;
                fromCollision = cached->fromCollision;
            }
        }
        if (!mesh)
            continue;

        const Transform tf = Transform::FromAngles(e.origin, e.angles);
        if (listenerValid && PointInsideBounds(listener, tf, mins, maxs, m_options.listenerMarginUnits)) {
            ++m_stats.skippedInside;
            continue;
        }
        const std::string name = (e.player ? "player#" : "ent#") + std::to_string(e.index) + " " + c.model;
        const DynamicGeometryId id = sink.AddMesh(mesh, tf, name);
        if (id == 0)
            continue;
        TrackedEntity t;
        t.id = id;
        t.serial = e.serial;
        t.model = c.model;
        t.mesh = mesh;
        t.origin = e.origin;
        t.angles = e.angles;
        t.mins = mins;
        t.maxs = maxs;
        t.player = e.player;
        t.fromCollision = fromCollision;
        t.missed = 0;
        m_tracked[e.index] = std::move(t);
        ++accepted;
        ++m_stats.added;
    }

    // Entities gone from the snapshot (or filtered out) for long enough.
    for (auto it = m_tracked.begin(); it != m_tracked.end();) {
        if ((it->second.id == 0 || it->second.missed >= m_options.missedUpdatesBeforeRemove) &&
            Remove(it->first, it->second, sink)) {
            it = m_tracked.erase(it);
        } else {
            ++it;
        }
    }
    // Brush entities that vanished (removed brush ent): forget the pose so a
    // reappearance re-pushes it.
    for (auto it = m_brush.begin(); it != m_brush.end();) {
        if (!it->second.seen)
            it = m_brush.erase(it);
        else
            ++it;
    }
}

void DynamicOccluders::Clear(IDynamicGeometrySink& sink)
{
    for (auto it = m_tracked.begin(); it != m_tracked.end();) {
        if (Remove(it->first, it->second, sink))
            it = m_tracked.erase(it);
        else
            ++it;
    }
    m_brush.clear();
    m_orientation.clear();
}

bool DynamicOccluders::PushEmitterOutside(int32_t index, const Vec3& listener, float margin, Vec3& position) const
{
    const auto it = m_tracked.find(index);
    if (it == m_tracked.end() || it->second.id == 0)
        return false;
    const TrackedEntity& t = it->second;
    return PushOutOfBounds(position, listener, Transform::FromAngles(t.origin, t.angles), t.mins, t.maxs, margin);
}

bool DynamicOccluders::EntityAngles(int32_t index, Vec3& angles) const
{
    const auto it = m_orientation.find(index);
    if (it == m_orientation.end())
        return false;
    angles = it->second;
    return true;
}

void DynamicOccluders::Reset(IDynamicGeometrySink& sink)
{
    Clear(sink);
    m_models.clear();
    m_playerBox.reset();
    m_stats.modelsMissing = 0;
}

DynamicOccluders::Stats DynamicOccluders::GetStats() const
{
    Stats s = m_stats;
    s.tracked = 0;
    s.props = 0;
    s.players = 0;
    s.triangles = 0;
    for (const auto& kv : m_tracked) {
        if (kv.second.id == 0)
            continue;
        ++s.tracked;
        if (kv.second.player)
            ++s.players;
        else
            ++s.props;
        if (kv.second.mesh)
            s.triangles += kv.second.mesh->triangles.size();
    }
    s.modelCache = m_models.size();
    return s;
}

std::vector<DynamicOccluders::TrackedInfo> DynamicOccluders::Tracked() const
{
    std::vector<TrackedInfo> out;
    out.reserve(m_tracked.size());
    for (const auto& kv : m_tracked) {
        if (kv.second.id == 0)
            continue;
        TrackedInfo info;
        info.index = kv.first;
        info.model = kv.second.model;
        info.id = kv.second.id;
        info.player = kv.second.player;
        info.fromCollision = kv.second.fromCollision;
        info.triangles = kv.second.mesh ? kv.second.mesh->triangles.size() : 0;
        info.origin = kv.second.origin;
        out.push_back(std::move(info));
    }
    std::sort(out.begin(), out.end(), [](const TrackedInfo& a, const TrackedInfo& b) { return a.index < b.index; });
    return out;
}

} // namespace sa
