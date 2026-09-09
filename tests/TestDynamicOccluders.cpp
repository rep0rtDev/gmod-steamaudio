// tests/TestDynamicOccluders.cpp
#include <map>
#include <string>
#include <vector>

#include "PhyFixture.h"
#include "TestFramework.h"
#include "core/DynamicOccluders.h"

using namespace sa;
using satest::PhyBuilder;

namespace {

struct FakeSink final : public IDynamicGeometrySink {
    struct Instance {
        std::shared_ptr<const MeshData> mesh;
        Transform transform;
        std::string name;
        bool alive = true;
    };
    std::map<DynamicGeometryId, Instance> instances;
    std::map<int32_t, std::pair<Vec3, Vec3>> brushes;
    DynamicGeometryId next = 1;
    int adds = 0, updates = 0, removes = 0, brushUpdates = 0;
    bool failAdds = false;
    bool failUpdates = false;
    bool failRemoves = false;

    DynamicGeometryId AddMesh(std::shared_ptr<const MeshData> localMesh, const Transform& transform,
                              const std::string& debugName) override
    {
        if (failAdds)
            return 0;
        ++adds;
        const DynamicGeometryId id = next++;
        instances[id] = {std::move(localMesh), transform, debugName, true};
        return id;
    }
    bool UpdateMesh(DynamicGeometryId id, const Transform& transform) override
    {
        if (failUpdates)
            return false;
        auto it = instances.find(id);
        if (it == instances.end() || !it->second.alive)
            return false;
        ++updates;
        it->second.transform = transform;
        return true;
    }
    bool RemoveMesh(DynamicGeometryId id) override
    {
        if (failRemoves)
            return false;
        auto it = instances.find(id);
        if (it == instances.end() || !it->second.alive)
            return false;
        ++removes;
        it->second.alive = false;
        return true;
    }
    bool UpdateBrushModel(int32_t modelIndex, const Vec3& origin, const Vec3& angles) override
    {
        ++brushUpdates;
        brushes[modelIndex] = {origin, angles};
        return true;
    }

    size_t Alive() const
    {
        size_t n = 0;
        for (const auto& kv : instances)
            n += kv.second.alive ? 1 : 0;
        return n;
    }
    const Instance* AliveNamed(const std::string& needle) const
    {
        for (const auto& kv : instances)
            if (kv.second.alive && kv.second.name.find(needle) != std::string::npos)
                return &kv.second;
        return nullptr;
    }
};

std::vector<uint8_t> DrumPhy()
{
    PhyBuilder b;
    PhyBuilder::Solid s;
    s.ledges.push_back(PhyBuilder::Box({-16.f, -16.f, 0.f}, {16.f, 16.f, 48.f}));
    b.solids.push_back(s);
    b.text = "solid {\n\"index\" \"0\"\n\"surfaceprop\" \"metal_barrel\"\n}\n";
    return b.Build();
}

struct Files {
    std::map<std::string, std::vector<uint8_t>> files;
    int reads = 0;
    GameFileReader Reader()
    {
        return [this](const std::string& path, std::vector<uint8_t>& out, std::string& error) {
            ++reads;
            auto it = files.find(path);
            if (it == files.end()) {
                error = "missing";
                return false;
            }
            out = it->second;
            return true;
        };
    }
};

EntitySnapshot Prop(int32_t index, const std::string& model, Vec3 origin, Vec3 angles = {})
{
    EntitySnapshot e;
    e.index = index;
    e.serial = 100u + static_cast<uint32_t>(index);
    e.model = model;
    e.origin = origin;
    e.angles = angles;
    e.mins = {-16.f, -16.f, 0.f};
    e.maxs = {16.f, 16.f, 48.f};
    return e;
}

EntitySnapshot Player(int32_t index, Vec3 origin)
{
    EntitySnapshot e;
    e.index = index;
    e.serial = 7u;
    e.model = "models/player/kleiner.mdl";
    e.origin = origin;
    e.mins = {-16.f, -16.f, 0.f};
    e.maxs = {16.f, 16.f, 72.f};
    e.player = true;
    return e;
}

EntitySnapshot Brush(int32_t index, int32_t brush, Vec3 origin, Vec3 angles = {})
{
    EntitySnapshot e;
    e.index = index;
    e.serial = 1u;
    e.model = "*" + std::to_string(brush);
    e.origin = origin;
    e.angles = angles;
    e.solid = kSolidBsp;
    return e;
}

DynamicOccluderOptions Options()
{
    DynamicOccluderOptions o;
    o.modelLoadsPerUpdate = 8;
    return o;
}

} // namespace

SA_TEST(DynOcc_BoxMeshIsClosedAndInSteamAudioSpace)
{
    IPLMaterial m{};
    const std::shared_ptr<const MeshData> mesh =
        DynamicOccluders::BuildBoxMesh({-16.f, -32.f, 0.f}, {16.f, 32.f, 48.f}, 39.37f, m);
    SA_CHECK(mesh != nullptr);
    SA_CHECK_EQ(mesh->triangles.size(), size_t(12));
    SA_CHECK_EQ(mesh->vertices.size(), mesh->triangles.size() * 3);
    SA_CHECK_EQ(mesh->materialIndices.size(), size_t(12));
    // Source (x, y, z) -> Steam Audio (x, z, -y), scaled to meters.
    float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f, minZ = 1e9f, maxZ = -1e9f;
    for (const IPLVector3& v : mesh->vertices) {
        minX = std::min(minX, v.x), maxX = std::max(maxX, v.x);
        minY = std::min(minY, v.y), maxY = std::max(maxY, v.y);
        minZ = std::min(minZ, v.z), maxZ = std::max(maxZ, v.z);
    }
    const float u = 1.f / 39.37f;
    SA_CHECK_NEAR(minX, -16.f * u, 1e-4);
    SA_CHECK_NEAR(maxX, 16.f * u, 1e-4);
    SA_CHECK_NEAR(minY, 0.f, 1e-4);
    SA_CHECK_NEAR(maxY, 48.f * u, 1e-4);
    SA_CHECK_NEAR(minZ, -32.f * u, 1e-4);
    SA_CHECK_NEAR(maxZ, 32.f * u, 1e-4);
    // Degenerate boxes produce nothing.
    SA_CHECK(DynamicOccluders::BuildBoxMesh({0.f, 0.f, 0.f}, {0.f, 0.f, 0.f}, 39.37f, m) == nullptr);
}

SA_TEST(DynOcc_PointInsideBoundsRespectsRotationAndMargin)
{
    const Vec3 mins{-10.f, -50.f, 0.f}, maxs{10.f, 50.f, 20.f};
    const Transform identity = Transform::FromAngles({100.f, 0.f, 0.f}, {});
    SA_CHECK(DynamicOccluders::PointInsideBounds({100.f, 40.f, 10.f}, identity, mins, maxs, 0.f));
    SA_CHECK(!DynamicOccluders::PointInsideBounds({140.f, 0.f, 10.f}, identity, mins, maxs, 0.f));
    SA_CHECK(DynamicOccluders::PointInsideBounds({112.f, 0.f, 10.f}, identity, mins, maxs, 4.f));
    // Yaw 90: the long axis now runs along world X.
    const Transform yawed = Transform::FromAngles({100.f, 0.f, 0.f}, {0.f, 90.f, 0.f});
    SA_CHECK(DynamicOccluders::PointInsideBounds({140.f, 0.f, 10.f}, yawed, mins, maxs, 0.f));
    SA_CHECK(!DynamicOccluders::PointInsideBounds({100.f, 40.f, 10.f}, yawed, mins, maxs, 0.f));
}

SA_TEST(DynOcc_PushOutOfBoundsMovesEmitterTowardListener)
{
    const Vec3 mins{-16.f, -16.f, 0.f}, maxs{16.f, 16.f, 72.f};
    const Transform identity = Transform::FromAngles({100.f, 0.f, 0.f}, {});
    // Emitter at the entity origin (feet, on the bottom face); listener +X.
    Vec3 p{100.f, 0.f, 0.f};
    SA_CHECK(DynamicOccluders::PushOutOfBounds(p, {400.f, 0.f, 0.f}, identity, mins, maxs, 4.f));
    SA_CHECK_NEAR(p.x, 100.f + 16.f + 4.f + 0.5f, 1e-3);
    SA_CHECK_NEAR(p.y, 0.f, 1e-4);
    SA_CHECK_NEAR(p.z, 0.f, 1e-4);
    SA_CHECK(!DynamicOccluders::PointInsideBounds(p, identity, mins, maxs, 4.f));
    // Already outside: untouched.
    Vec3 q{140.f, 0.f, 10.f};
    SA_CHECK(!DynamicOccluders::PushOutOfBounds(q, {400.f, 0.f, 0.f}, identity, mins, maxs, 0.f));
    SA_CHECK_NEAR(q.x, 140.f, 1e-4);
    // Listener inside the box as well: nothing sensible to do.
    Vec3 r{100.f, 0.f, 30.f};
    SA_CHECK(!DynamicOccluders::PushOutOfBounds(r, {105.f, 5.f, 40.f}, identity, mins, maxs, 0.f));
    // Rotated box (yaw 90) with the listener along the diagonal: exits the
    // nearest face, still outside afterwards.
    const Transform yawed = Transform::FromAngles({0.f, 0.f, 0.f}, {0.f, 90.f, 0.f});
    const Vec3 tall{-16.f, -48.f, 0.f}, tallMax{16.f, 48.f, 72.f};
    Vec3 s{0.f, 0.f, 36.f};
    SA_CHECK(DynamicOccluders::PushOutOfBounds(s, {300.f, 300.f, 36.f}, yawed, tall, tallMax, 0.f));
    SA_CHECK(!DynamicOccluders::PointInsideBounds(s, yawed, tall, tallMax, 0.f));
    SA_CHECK(s.x > 0.f && s.y > 0.f);
    SA_CHECK_NEAR(s.z, 36.f, 1e-4);
}

SA_TEST(DynOcc_PropUsesCollisionModelAndTracksLifecycle)
{
    Files files;
    files.files["models/props_c17/oildrum001.phy"] = DrumPhy();
    DynamicOccluders occ;
    occ.Configure(Options());
    occ.SetReader(files.Reader());
    FakeSink sink;

    std::vector<EntitySnapshot> ents = {Prop(5, "models/props_c17/oildrum001.mdl", {200.f, 0.f, 0.f})};
    occ.Update(ents, {}, true, sink);
    SA_CHECK_EQ(sink.adds, 1);
    SA_CHECK_EQ(sink.Alive(), size_t(1));
    const FakeSink::Instance* inst = sink.AliveNamed("ent#5");
    SA_CHECK(inst != nullptr);
    SA_CHECK_EQ(inst->mesh->triangles.size(), size_t(12));
    SA_CHECK_NEAR(inst->transform.origin.x, 200.f, 1e-4);
    DynamicOccluders::Stats st = occ.GetStats();
    SA_CHECK_EQ(st.tracked, size_t(1));
    SA_CHECK_EQ(st.props, size_t(1));
    SA_CHECK_EQ(st.modelCache, size_t(1));
    SA_CHECK_EQ(st.triangles, size_t(12));
    SA_CHECK(occ.Tracked()[0].fromCollision);

    // Small movement below the epsilon: no update pushed.
    ents[0].origin = {200.2f, 0.f, 0.f};
    occ.Update(ents, {}, true, sink);
    SA_CHECK_EQ(sink.updates, 0);
    // Real movement: one transform update, no re-add.
    ents[0].origin = {250.f, 10.f, 0.f};
    ents[0].angles = {0.f, 45.f, 0.f};
    occ.Update(ents, {}, true, sink);
    SA_CHECK_EQ(sink.updates, 1);
    SA_CHECK_EQ(sink.adds, 1);
    SA_CHECK_NEAR(sink.AliveNamed("ent#5")->transform.origin.x, 250.f, 1e-4);

    // Entity vanishes: removed after missedUpdatesBeforeRemove snapshots.
    const std::vector<EntitySnapshot> none;
    occ.Update(none, {}, true, sink);
    SA_CHECK_EQ(sink.removes, 0);
    occ.Update(none, {}, true, sink);
    SA_CHECK_EQ(sink.removes, 1);
    SA_CHECK_EQ(sink.Alive(), size_t(0));
    SA_CHECK_EQ(occ.GetStats().tracked, size_t(0));
    // The model stays cached for the next spawn.
    SA_CHECK_EQ(occ.GetStats().modelCache, size_t(1));
}

SA_TEST(DynOcc_ModelCacheSharedAcrossEntitiesAndSerialChangeReplaces)
{
    Files files;
    files.files["models/props_c17/oildrum001.phy"] = DrumPhy();
    DynamicOccluders occ;
    occ.Configure(Options());
    occ.SetReader(files.Reader());
    FakeSink sink;

    std::vector<EntitySnapshot> ents;
    for (int i = 1; i <= 5; ++i)
        ents.push_back(Prop(i, "models/props_c17/oildrum001.mdl", {static_cast<float>(i) * 100.f, 0.f, 0.f}));
    occ.Update(ents, {}, true, sink);
    SA_CHECK_EQ(sink.adds, 5);
    // One .phy read (plus at most the .mdl probe) for five entities.
    SA_CHECK(files.reads <= 2);
    const std::shared_ptr<const MeshData> shared = sink.AliveNamed("ent#1")->mesh;
    for (int i = 2; i <= 5; ++i)
        SA_CHECK(sink.AliveNamed("ent#" + std::to_string(i))->mesh == shared);

    // Index 3 is reused by a different entity (serial changes): re-added.
    ents[2].serial += 1000;
    occ.Update(ents, {}, true, sink);
    SA_CHECK_EQ(sink.removes, 1);
    SA_CHECK_EQ(sink.adds, 6);
    SA_CHECK_EQ(sink.Alive(), size_t(5));

    // Model swap on the same entity: re-added with the new geometry.
    ents[0].model = "models/props/crate.mdl";
    files.files["models/props/crate.mdl"] = satest::BuildMdl("crate", {-20.f, -20.f, 0.f}, {20.f, 20.f, 40.f}, "wood");
    occ.Update(ents, {}, true, sink);
    SA_CHECK_EQ(sink.removes, 2);
    SA_CHECK_EQ(sink.adds, 7);
    SA_CHECK(sink.AliveNamed("ent#1")->mesh != shared);
    SA_CHECK_EQ(occ.GetStats().modelCache, size_t(2));
}

SA_TEST(DynOcc_MissingModelFallsBackToEntityBoxOrSkips)
{
    Files files;
    DynamicOccluders occ;
    DynamicOccluderOptions o = Options();
    occ.Configure(o);
    occ.SetReader(files.Reader());
    FakeSink sink;

    std::vector<EntitySnapshot> ents = {Prop(1, "models/nothing.mdl", {100.f, 0.f, 0.f})};
    occ.Update(ents, {}, true, sink);
    SA_CHECK_EQ(sink.adds, 1);
    SA_CHECK_EQ(occ.GetStats().modelsMissing, size_t(1));
    SA_CHECK(!occ.Tracked()[0].fromCollision);

    // Too small for the extent threshold.
    ents.push_back(Prop(2, "models/tiny.mdl", {300.f, 0.f, 0.f}));
    ents[1].mins = {-2.f, -2.f, 0.f};
    ents[1].maxs = {2.f, 2.f, 4.f};
    occ.Update(ents, {}, true, sink);
    SA_CHECK_EQ(sink.adds, 1);
    SA_CHECK_EQ(occ.GetStats().skippedSmall, size_t(1));

    // Box fallback disabled: entity with no geometry is simply ignored.
    o.boxFallback = false;
    DynamicOccluders strict;
    strict.Configure(o);
    strict.SetReader(files.Reader());
    FakeSink sink2;
    strict.Update(ents, {}, true, sink2);
    SA_CHECK_EQ(sink2.adds, 0);
    SA_CHECK_EQ(strict.GetStats().tracked, size_t(0));
}

SA_TEST(DynOcc_DormantSnapshotsNeedNoGeometryDetails)
{
    Files files;
    files.files["models/drum.phy"] = DrumPhy();
    DynamicOccluders occ;
    occ.Configure(Options());
    occ.SetReader(files.Reader());
    FakeSink sink;
    const EntitySnapshot active = Prop(12, "models/drum.mdl", {200.f, 0.f, 0.f});
    occ.Update({active}, {}, true, sink);
    SA_CHECK_EQ(sink.Alive(), size_t(1));
    const int reads = files.reads;
    EntitySnapshot dormant;
    dormant.index = active.index;
    dormant.dormant = true;
    occ.Update({dormant}, {}, true, sink);
    SA_CHECK_EQ(sink.Alive(), size_t(1));
    SA_CHECK_EQ(files.reads, reads);
    occ.Update({dormant}, {}, true, sink);
    SA_CHECK_EQ(sink.Alive(), size_t(0));
    occ.Update({active}, {}, true, sink);
    SA_CHECK_EQ(sink.Alive(), size_t(1));
    SA_CHECK_EQ(files.reads, reads);
}

SA_TEST(DynOcc_PlayersUseHullAndLocalPlayerExcluded)
{
    DynamicOccluders occ;
    occ.Configure(Options());
    occ.SetLocalPlayer(1);
    FakeSink sink;

    std::vector<EntitySnapshot> ents = {Player(1, {0.f, 0.f, 0.f}), Player(2, {300.f, 0.f, 0.f}),
                                        Player(3, {-300.f, 0.f, 0.f})};
    ents[2].mins = ents[2].maxs = {}; // no OBB from the snapshot: default hull
    occ.Update(ents, {}, true, sink);
    SA_CHECK_EQ(sink.adds, 2);
    SA_CHECK(sink.AliveNamed("player#1") == nullptr);
    SA_CHECK(sink.AliveNamed("player#2") != nullptr);
    SA_CHECK(sink.AliveNamed("player#3") != nullptr);
    // Both standard hulls share one mesh.
    SA_CHECK(sink.AliveNamed("player#2")->mesh == sink.AliveNamed("player#3")->mesh);
    SA_CHECK_EQ(occ.GetStats().players, size_t(2));

    DynamicOccluderOptions o = Options();
    o.players = false;
    occ.Configure(o);
    occ.Update(ents, {}, true, sink);
    occ.Update(ents, {}, true, sink);
    SA_CHECK_EQ(sink.Alive(), size_t(0));
}

SA_TEST(DynOcc_ListenerInsideEntityIsSkippedUntilOutside)
{
    Files files;
    DynamicOccluders occ;
    occ.Configure(Options());
    occ.SetReader(files.Reader());
    FakeSink sink;

    // Vehicle-sized box the listener sits in.
    std::vector<EntitySnapshot> ents = {Prop(9, "models/vehicle.mdl", {0.f, 0.f, 0.f})};
    ents[0].mins = {-60.f, -40.f, 0.f};
    ents[0].maxs = {60.f, 40.f, 60.f};
    occ.Update(ents, {0.f, 0.f, 30.f}, true, sink);
    SA_CHECK_EQ(sink.adds, 0);
    SA_CHECK_EQ(occ.GetStats().skippedInside, size_t(1));

    occ.Update(ents, {500.f, 0.f, 30.f}, true, sink);
    SA_CHECK_EQ(sink.adds, 1);
    SA_CHECK_EQ(occ.GetStats().skippedInside, size_t(0));

    // Listener climbs in: tracked entity is dropped after the miss window.
    occ.Update(ents, {0.f, 0.f, 30.f}, true, sink);
    occ.Update(ents, {0.f, 0.f, 30.f}, true, sink);
    SA_CHECK_EQ(sink.removes, 1);
    SA_CHECK_EQ(occ.GetStats().tracked, size_t(0));
    // Without a listener the inside test is off.
    occ.Update(ents, {}, false, sink);
    SA_CHECK_EQ(sink.adds, 2);
}

SA_TEST(DynOcc_RangeAndCountBudgetsPreferNearest)
{
    Files files;
    DynamicOccluders occ;
    DynamicOccluderOptions o = Options();
    o.maxOccluders = 3;
    o.rangeUnits = 1000.f;
    occ.Configure(o);
    occ.SetReader(files.Reader());
    FakeSink sink;

    std::vector<EntitySnapshot> ents;
    // Unsorted distances 900, 100, 500, 300, 1500 (out of range).
    const float xs[] = {900.f, 100.f, 500.f, 300.f, 1500.f};
    for (int i = 0; i < 5; ++i)
        ents.push_back(Prop(i + 1, "models/box.mdl", {xs[i], 0.f, 0.f}));
    occ.Update(ents, {}, true, sink);
    SA_CHECK_EQ(sink.adds, 3);
    SA_CHECK(sink.AliveNamed("ent#2") != nullptr); // 100
    SA_CHECK(sink.AliveNamed("ent#4") != nullptr); // 300
    SA_CHECK(sink.AliveNamed("ent#3") != nullptr); // 500
    SA_CHECK(sink.AliveNamed("ent#1") == nullptr); // 900 over the count limit
    SA_CHECK(sink.AliveNamed("ent#5") == nullptr); // out of range
    const DynamicOccluders::Stats st = occ.GetStats();
    SA_CHECK_EQ(st.skippedLimit, size_t(1));
    SA_CHECK_EQ(st.skippedRange, size_t(1));

    // Listener moves (above the boxes, not inside ent#5): the far ones become
    // the nearest set.
    occ.Update(ents, {1500.f, 0.f, 200.f}, true, sink);
    occ.Update(ents, {1500.f, 0.f, 200.f}, true, sink);
    SA_CHECK(sink.AliveNamed("ent#5") != nullptr); // 200
    SA_CHECK(sink.AliveNamed("ent#1") != nullptr); // ~632
    SA_CHECK(sink.AliveNamed("ent#3") == nullptr); // ~1020, out of range
    SA_CHECK(sink.AliveNamed("ent#2") == nullptr);
    SA_CHECK_EQ(sink.Alive(), size_t(2));
}

SA_TEST(DynOcc_ModelLoadBudgetDefersNewEntities)
{
    Files files;
    files.files["models/a.phy"] = DrumPhy();
    files.files["models/b.phy"] = DrumPhy();
    files.files["models/c.phy"] = DrumPhy();
    DynamicOccluders occ;
    DynamicOccluderOptions o = Options();
    o.modelLoadsPerUpdate = 1;
    occ.Configure(o);
    occ.SetReader(files.Reader());
    FakeSink sink;

    std::vector<EntitySnapshot> ents = {Prop(1, "models/a.mdl", {100.f, 0.f, 0.f}),
                                        Prop(2, "models/b.mdl", {200.f, 0.f, 0.f}),
                                        Prop(3, "models/c.mdl", {300.f, 0.f, 0.f}),
                                        Prop(4, "models/a.mdl", {400.f, 0.f, 0.f})};
    occ.Update(ents, {}, true, sink);
    SA_CHECK_EQ(sink.adds, 2); // a (twice: cached) ; b and c deferred
    SA_CHECK_EQ(occ.GetStats().pendingLoads, size_t(2));
    occ.Update(ents, {}, true, sink);
    SA_CHECK_EQ(sink.adds, 3);
    occ.Update(ents, {}, true, sink);
    SA_CHECK_EQ(sink.adds, 4);
    SA_CHECK_EQ(occ.GetStats().pendingLoads, size_t(0));
    SA_CHECK_EQ(sink.removes, 0);
}

SA_TEST(DynOcc_BrushEntitiesPushTransformsOnceAndOnMove)
{
    DynamicOccluders occ;
    occ.Configure(Options());
    FakeSink sink;

    std::vector<EntitySnapshot> ents = {Brush(20, 12, {0.f, 0.f, 0.f}), Brush(21, 13, {100.f, 0.f, 64.f})};
    occ.Update(ents, {}, true, sink);
    // Zero pose still gets pushed once (spawn origin may differ).
    SA_CHECK_EQ(sink.brushUpdates, 2);
    SA_CHECK_EQ(occ.GetStats().brushModels, size_t(2));
    SA_CHECK_EQ(sink.adds, 0);

    occ.Update(ents, {}, true, sink);
    SA_CHECK_EQ(sink.brushUpdates, 2);
    ents[1].origin.z = 128.f; // elevator moved
    occ.Update(ents, {}, true, sink);
    SA_CHECK_EQ(sink.brushUpdates, 3);
    SA_CHECK_NEAR(sink.brushes[13].first.z, 128.f, 1e-4);
    ents[0].angles.y = 90.f; // door swung
    occ.Update(ents, {}, true, sink);
    SA_CHECK_EQ(sink.brushUpdates, 4);
    SA_CHECK_NEAR(sink.brushes[12].second.y, 90.f, 1e-4);

    // Brush ent removed then respawned: pose re-pushed.
    const std::vector<EntitySnapshot> only = {ents[1]};
    occ.Update(only, {}, true, sink);
    occ.Update(ents, {}, true, sink);
    SA_CHECK_EQ(sink.brushUpdates, 5);

    DynamicOccluderOptions o = Options();
    o.brushModels = false;
    occ.Configure(o);
    ents[0].origin.x = 500.f;
    occ.Update(ents, {}, true, sink);
    SA_CHECK_EQ(sink.brushUpdates, 5);
}

SA_TEST(DynOcc_RetriesRejectedUpdatesRemovalsAndClear)
{
    DynamicOccluders tracker;
    tracker.Configure(Options());
    FakeSink sink;
    std::vector<EntitySnapshot> entities = {Player(1, {100.f, 0.f, 0.f})};
    tracker.Update(entities, {}, false, sink);
    sink.failUpdates = true;
    entities[0].origin.x = 200.f;
    tracker.Update(entities, {}, false, sink);
    SA_CHECK_NEAR(tracker.Tracked()[0].origin.x, 100.f, 1e-6);
    sink.failUpdates = false;
    tracker.Update(entities, {}, false, sink);
    SA_CHECK_EQ(sink.updates, 1);
    SA_CHECK_NEAR(tracker.Tracked()[0].origin.x, 200.f, 1e-6);
    sink.failRemoves = true;
    tracker.Update({}, {}, false, sink);
    tracker.Update({}, {}, false, sink);
    SA_CHECK_EQ(tracker.GetStats().tracked, size_t(1));
    SA_CHECK_EQ(sink.Alive(), size_t(1));
    sink.failRemoves = false;
    tracker.Update({}, {}, false, sink);
    SA_CHECK_EQ(tracker.GetStats().tracked, size_t(0));
    SA_CHECK_EQ(sink.Alive(), size_t(0));
    tracker.Update(entities, {}, false, sink);
    sink.failRemoves = true;
    tracker.Clear(sink);
    SA_CHECK_EQ(tracker.GetStats().tracked, size_t(1));
    sink.failRemoves = false;
    tracker.Clear(sink);
    SA_CHECK_EQ(tracker.GetStats().tracked, size_t(0));
}

SA_TEST(DynOcc_InvalidSnapshotsAndDisabledAreIgnored)
{
    Files files;
    DynamicOccluders occ;
    occ.Configure(Options());
    occ.SetReader(files.Reader());
    FakeSink sink;

    std::vector<EntitySnapshot> ents = {Prop(1, "models/a.mdl", {100.f, 0.f, 0.f})};
    ents.push_back(Prop(2, "models/a.mdl", {std::numeric_limits<float>::quiet_NaN(), 0.f, 0.f}));
    ents.push_back(Prop(3, "models/a.mdl", {200.f, 0.f, 0.f}));
    ents[2].dormant = true;
    ents.push_back(Prop(4, "", {200.f, 0.f, 0.f}));
    ents.push_back(Prop(5, "models/a.mdl", {200.f, 0.f, 0.f}));
    ents[4].solid = kSolidNone;
    ents.push_back(Prop(0, "models/a.mdl", {200.f, 0.f, 0.f}));
    occ.Update(ents, {}, true, sink);
    SA_CHECK_EQ(sink.adds, 1);

    // Sink refusing adds (simulation not running) does not leave ghosts.
    sink.failAdds = true;
    ents.push_back(Prop(6, "models/a.mdl", {300.f, 0.f, 0.f}));
    occ.Update(ents, {}, true, sink);
    SA_CHECK_EQ(occ.GetStats().tracked, size_t(1));
    sink.failAdds = false;
    occ.Update(ents, {}, true, sink);
    SA_CHECK_EQ(occ.GetStats().tracked, size_t(2));

    // Disabling clears the scene; Reset drops the cache too.
    DynamicOccluderOptions o = Options();
    o.enabled = false;
    occ.Configure(o);
    occ.Update(ents, {}, true, sink);
    SA_CHECK_EQ(sink.Alive(), size_t(0));
    occ.Reset(sink);
    SA_CHECK_EQ(occ.GetStats().modelCache, size_t(0));
}
