// tests/TestStaticProps.cpp
#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "PhyFixture.h"
#include "TestFramework.h"
#include "steamaudio/PhyModel.h"
#include "steamaudio/StaticPropResolver.h"

using namespace sa;
using satest::PhyBuilder;

namespace {

struct Bounds {
    Vec3 mins{1e9f, 1e9f, 1e9f};
    Vec3 maxs{-1e9f, -1e9f, -1e9f};
    void Add(const Vec3& v)
    {
        mins = {std::min(mins.x, v.x), std::min(mins.y, v.y), std::min(mins.z, v.z)};
        maxs = {std::max(maxs.x, v.x), std::max(maxs.y, v.y), std::max(maxs.z, v.z)};
    }
};

Bounds BoundsOf(const std::vector<Vec3>& vertices)
{
    Bounds b;
    for (const Vec3& v : vertices)
        b.Add(v);
    return b;
}

Bounds BoundsOf(const std::vector<IPLVector3>& vertices)
{
    Bounds b;
    for (const IPLVector3& v : vertices)
        b.Add(Vec3::FromIPL(v));
    return b;
}

PhyBuilder OilDrum()
{
    PhyBuilder b;
    PhyBuilder::Solid s;
    s.ledges.push_back(PhyBuilder::Box({-16.f, -16.f, 0.f}, {16.f, 16.f, 48.f}));
    b.solids.push_back(s);
    b.text = "solid {\n\"index\" \"0\"\n\"name\" \"oildrum001\"\n\"mass\" \"40\"\n\"surfaceprop\" \"metal_barrel\"\n}\n"
             "editparams {\n\"rootname\" \"\"\n\"totalmass\" \"40\"\n}\n";
    return b;
}

GameFileReader MapReader(const std::map<std::string, std::vector<uint8_t>>& files)
{
    return [&files](const std::string& path, std::vector<uint8_t>& out, std::string& error) {
        auto it = files.find(path);
        if (it == files.end()) {
            error = "missing";
            return false;
        }
        out = it->second;
        return true;
    };
}

} // namespace

SA_TEST(Phy_OversizedTextIndicesDoNotThrow)
{
    PhyBuilder builder = OilDrum();
    builder.text = "solid { index 99999999999999999999999 surfaceprop metal } "
                   "materialtable { 99999999999999999999999 wood }";
    const auto bytes = builder.Build();
    PhyModel model;
    SA_CHECK(ParsePhy(bytes.data(), bytes.size(), model));
    SA_CHECK(model.TriangleCount() > 0);
    SA_CHECK(model.materialTable.empty());
}

SA_TEST(Phy_ParsesCompactSurfaceThroughLedgeTree)
{
    const std::vector<uint8_t> bytes = OilDrum().Build();
    PhyModel model;
    SA_CHECK(ParsePhy(bytes.data(), bytes.size(), model));
    SA_CHECK_EQ(model.solidCount, 1);
    SA_CHECK_EQ(model.solids.size(), size_t(1));
    SA_CHECK_EQ(model.skippedSolids, size_t(0));
    const PhySolid& s = model.solids[0];
    SA_CHECK_EQ(s.ledges, size_t(1));
    SA_CHECK_EQ(s.triangles.size(), size_t(12));
    SA_CHECK_EQ(s.vertices.size(), size_t(8));
    SA_CHECK(s.surfaceProp == "metal_barrel");
    // IVP meters/Y-up round-trips back into Source units/Z-up.
    const Bounds b = BoundsOf(s.vertices);
    SA_CHECK_NEAR(b.mins.x, -16.f, 0.01f);
    SA_CHECK_NEAR(b.mins.y, -16.f, 0.01f);
    SA_CHECK_NEAR(b.mins.z, 0.f, 0.01f);
    SA_CHECK_NEAR(b.maxs.x, 16.f, 0.01f);
    SA_CHECK_NEAR(b.maxs.y, 16.f, 0.01f);
    SA_CHECK_NEAR(b.maxs.z, 48.f, 0.01f);
    for (const PhyTriangle& t : s.triangles) {
        SA_CHECK(t.indices[0] < 8 && t.indices[1] < 8 && t.indices[2] < 8);
        SA_CHECK_EQ(t.materialIndex, 0);
    }
}

SA_TEST(Phy_MultipleLedgesAndMaterialTable)
{
    PhyBuilder b;
    PhyBuilder::Solid s;
    s.ledges.push_back(PhyBuilder::Box({-8.f, -8.f, 0.f}, {8.f, 8.f, 8.f}, 0));
    s.ledges.push_back(PhyBuilder::Box({-8.f, -8.f, 8.f}, {8.f, 8.f, 40.f}, 1));
    s.ledges.push_back(PhyBuilder::Box({-2.f, -2.f, 40.f}, {2.f, 2.f, 60.f}, 2));
    b.solids.push_back(s);
    b.text = "solid { \"index\" \"0\" \"surfaceprop\" \"wood\" }\n"
             "materialtable { \"1\" \"metal\" \"2\" \"glass\" }\n";
    const std::vector<uint8_t> bytes = b.Build();
    PhyModel model;
    SA_CHECK(ParsePhy(bytes.data(), bytes.size(), model));
    SA_CHECK_EQ(model.solids[0].ledges, size_t(3));
    SA_CHECK_EQ(model.solids[0].triangles.size(), size_t(36));
    SA_CHECK_EQ(model.solids[0].vertices.size(), size_t(24));
    SA_CHECK_EQ(model.materialTable.size(), size_t(2));
    SA_CHECK(model.materialTable[0] == "metal");
    SA_CHECK(model.materialTable[1] == "glass");
    size_t byMaterial[3] = {0, 0, 0};
    for (const PhyTriangle& t : model.solids[0].triangles)
        if (t.materialIndex < 3)
            ++byMaterial[t.materialIndex];
    SA_CHECK_EQ(byMaterial[0], size_t(12));
    SA_CHECK_EQ(byMaterial[1], size_t(12));
    SA_CHECK_EQ(byMaterial[2], size_t(12));

    // Reversed material table orientation is accepted too.
    b.text = "materialtable { \"metal\" \"1\" }\n";
    const std::vector<uint8_t> reversed = b.Build();
    SA_CHECK(ParsePhy(reversed.data(), reversed.size(), model));
    SA_CHECK_EQ(model.materialTable.size(), size_t(1));
    SA_CHECK(model.materialTable[0] == "metal");
}

SA_TEST(Phy_LinearRecoveryWithoutLedgeTree)
{
    PhyBuilder b = OilDrum();
    b.solids[0].writeLedgeTree = false;
    b.solids[0].ledges.push_back(PhyBuilder::Box({0.f, 0.f, 48.f}, {4.f, 4.f, 52.f}));
    const std::vector<uint8_t> bytes = b.Build();
    PhyModel model;
    SA_CHECK(ParsePhy(bytes.data(), bytes.size(), model));
    SA_CHECK_EQ(model.solids[0].ledges, size_t(2));
    SA_CHECK_EQ(model.solids[0].triangles.size(), size_t(24));
    SA_CHECK(model.solids[0].surfaceProp == "metal_barrel");
}

SA_TEST(Phy_SkipsMoppAndForeignSolids)
{
    PhyBuilder b = OilDrum();
    PhyBuilder::Solid mopp = b.solids[0];
    mopp.modelType = 1;
    PhyBuilder::Solid foreign = b.solids[0];
    foreign.bogusVphyId = true;
    PhyBuilder::Solid noIvps = b.solids[0];
    noIvps.writeIvps = false;
    b.solids = {mopp, foreign, noIvps, OilDrum().solids[0]};
    b.text = "solid { \"index\" \"3\" \"surfaceprop\" \"metal\" }";
    const std::vector<uint8_t> bytes = b.Build();
    PhyModel model;
    SA_CHECK(ParsePhy(bytes.data(), bytes.size(), model));
    SA_CHECK_EQ(model.solids.size(), size_t(4));
    SA_CHECK_EQ(model.skippedSolids, size_t(3));
    SA_CHECK(model.solids[0].triangles.empty());
    SA_CHECK(model.solids[1].triangles.empty());
    SA_CHECK(model.solids[2].triangles.empty());
    SA_CHECK_EQ(model.solids[3].triangles.size(), size_t(12));
    SA_CHECK(model.solids[3].surfaceProp == "metal");

    // Only MOPP present: nothing usable.
    b.solids = {mopp};
    const std::vector<uint8_t> moppOnly = b.Build();
    SA_CHECK(!ParsePhy(moppOnly.data(), moppOnly.size(), model));
    SA_CHECK_EQ(model.skippedSolids, size_t(1));
    SA_CHECK(!model.error.empty());
}

SA_TEST(Phy_MalformedInputNeverCrashes)
{
    const std::vector<uint8_t> good = OilDrum().Build();
    PhyModel model;
    // Every truncation length: must not crash; short files fail cleanly.
    for (size_t len = 0; len < good.size(); ++len) {
        const bool ok = ParsePhy(good.data(), len, model);
        if (len < 16 + 32 + 48)
            SA_CHECK(!ok);
        if (ok)
            SA_CHECK(model.TriangleCount() > 0);
    }
    SA_CHECK(!ParsePhy(nullptr, 0, model));

    // Corrupted fields.
    std::vector<uint8_t> bad = good;
    PhyBuilder::Patch32(bad, 8, 100000); // solid count
    SA_CHECK(!ParsePhy(bad.data(), bad.size(), model));
    bad = good;
    PhyBuilder::Patch32(bad, 16, 0x7FFFFFFF); // solid size
    SA_CHECK(!ParsePhy(bad.data(), bad.size(), model));
    bad = good;
    PhyBuilder::Patch32(bad, 16 + 32 + 36, 0x7FFFFFF0); // ledge tree root far out of range => linear recovery
    SA_CHECK(ParsePhy(bad.data(), bad.size(), model));
    SA_CHECK_EQ(model.solids[0].triangles.size(), size_t(12));
    bad = good;
    PhyBuilder::Patch32(bad, 16 + 32 + 48, 0x7FFFFFF0); // c_point_offset out of range
    SA_CHECK(!ParsePhy(bad.data(), bad.size(), model));
    bad = good;
    for (size_t i = 16 + 32 + 48; i < bad.size(); ++i)
        bad[i] = static_cast<uint8_t>(i * 131u + 7u); // garbage surface body
    ParsePhy(bad.data(), bad.size(), model);
    // Deterministic byte-flip mutations of a valid file.
    uint32_t seed = 0x9E3779B9u;
    for (int iteration = 0; iteration < 4000; ++iteration) {
        bad = good;
        const int flips = 1 + static_cast<int>(seed % 6);
        for (int f = 0; f < flips; ++f) {
            seed = seed * 1664525u + 1013904223u;
            bad[seed % bad.size()] ^= static_cast<uint8_t>(1u << ((seed >> 8) % 8));
            seed = seed * 1664525u + 1013904223u;
            bad[seed % bad.size()] = static_cast<uint8_t>(seed >> 16);
        }
        ParsePhy(bad.data(), bad.size(), model);
        for (const PhySolid& s : model.solids)
            for (const PhyTriangle& tri : s.triangles)
                SA_CHECK(tri.indices[0] < s.vertices.size() && tri.indices[1] < s.vertices.size() &&
                         tri.indices[2] < s.vertices.size());
    }
    // Text section alone (no binary solids) is rejected.
    const std::string textOnly = "solid { \"index\" \"0\" }";
    std::vector<uint8_t> t(16, 0);
    PhyBuilder::Patch32(t, 0, 16);
    PhyBuilder::Patch32(t, 8, 1);
    t.insert(t.end(), textOnly.begin(), textOnly.end());
    SA_CHECK(!ParsePhy(t.data(), t.size(), model));
}

SA_TEST(Mdl_HeaderHullAndSurfaceProp)
{
    const std::vector<uint8_t> mdl =
        satest::BuildMdl("props_c17/oildrum001.mdl", {-17.f, -17.f, -1.f}, {17.f, 17.f, 49.f}, "Metal_Barrel");
    MdlInfo info;
    SA_CHECK(ParseMdlInfo(mdl.data(), mdl.size(), info));
    SA_CHECK_EQ(info.version, 48);
    SA_CHECK(info.name == "props_c17/oildrum001.mdl");
    SA_CHECK(info.hasHull);
    SA_CHECK_NEAR(info.hullMin.z, -1.f, 1e-6f);
    SA_CHECK_NEAR(info.hullMax.z, 49.f, 1e-6f);
    SA_CHECK(info.surfaceProp == "metal_barrel");

    std::vector<uint8_t> bad = mdl;
    bad[0] = 'X';
    SA_CHECK(!ParseMdlInfo(bad.data(), bad.size(), info));
    SA_CHECK(!ParseMdlInfo(mdl.data(), 200, info));
    bad = mdl;
    PhyBuilder::Patch32(bad, 308, 100000); // surfaceprop index past the end
    SA_CHECK(ParseMdlInfo(bad.data(), bad.size(), info));
    SA_CHECK(info.surfaceProp.empty());
}

SA_TEST(StaticProps_TransformAppliesAnglesAndScale)
{
    const Transform identity = Transform::FromAngles({100.f, 200.f, 300.f}, {0.f, 0.f, 0.f});
    const Vec3 a = TransformPropVertex({1.f, 2.f, 3.f}, identity, 1.f);
    SA_CHECK_NEAR(a.x, 101.f, 1e-3f);
    SA_CHECK_NEAR(a.y, 202.f, 1e-3f);
    SA_CHECK_NEAR(a.z, 303.f, 1e-3f);

    // Yaw 90: model +X -> world +Y, model +Y -> world -X.
    const Transform yaw = Transform::FromAngles({}, {0.f, 90.f, 0.f});
    const Vec3 fx = TransformPropVertex({1.f, 0.f, 0.f}, yaw, 1.f);
    SA_CHECK_NEAR(fx.x, 0.f, 1e-4f);
    SA_CHECK_NEAR(fx.y, 1.f, 1e-4f);
    const Vec3 fy = TransformPropVertex({0.f, 1.f, 0.f}, yaw, 1.f);
    SA_CHECK_NEAR(fy.x, -1.f, 1e-4f);
    SA_CHECK_NEAR(fy.y, 0.f, 1e-4f);

    // Pitch 90 (nose down): model +X -> world -Z.
    const Transform pitch = Transform::FromAngles({}, {90.f, 0.f, 0.f});
    const Vec3 px = TransformPropVertex({1.f, 0.f, 0.f}, pitch, 1.f);
    SA_CHECK_NEAR(px.z, -1.f, 1e-4f);

    const Vec3 scaled = TransformPropVertex({1.f, 2.f, 3.f}, Transform::FromAngles({}, {}), 2.5f);
    SA_CHECK_NEAR(scaled.x, 2.5f, 1e-4f);
    SA_CHECK_NEAR(scaled.y, 5.f, 1e-4f);
    SA_CHECK_NEAR(scaled.z, 7.5f, 1e-4f);
    const Vec3 zeroScale = TransformPropVertex({1.f, 0.f, 0.f}, Transform::FromAngles({}, {}), 0.f);
    SA_CHECK_NEAR(zeroScale.x, 1.f, 1e-4f); // 0 scale in old BSP versions means "unscaled"
}

SA_TEST(StaticProps_InstancesCollisionModelsIntoScene)
{
    std::map<std::string, std::vector<uint8_t>> files;
    files["models/props_c17/oildrum001.phy"] = OilDrum().Build();
    files["models/props_c17/oildrum001.mdl"] =
        satest::BuildMdl("oildrum", {-17.f, -17.f, -1.f}, {17.f, 17.f, 49.f}, "metal");
    // Box-only model: no .phy, hull from the .mdl.
    files["models/props/crate.mdl"] = satest::BuildMdl("crate", {-20.f, -20.f, 0.f}, {20.f, 20.f, 40.f}, "wood");

    std::vector<BspStaticProp> props;
    BspStaticProp drum;
    drum.model = "models/props_c17/oildrum001.mdl";
    drum.origin = {1000.f, 0.f, 64.f};
    drum.angles = {0.f, 90.f, 0.f};
    drum.solid = 6;
    props.push_back(drum);
    BspStaticProp bigDrum = drum;
    bigDrum.origin = {0.f, 500.f, 0.f};
    bigDrum.angles = {};
    bigDrum.uniformScale = 2.f;
    props.push_back(bigDrum);
    BspStaticProp crate;
    crate.model = "Models\\Props\\Crate.mdl"; // case/slash normalization
    crate.origin = {0.f, 0.f, 0.f};
    crate.solid = 2;
    props.push_back(crate);
    BspStaticProp foliage = drum;
    foliage.model = "models/props_foliage/tree.mdl";
    foliage.solid = 0;
    props.push_back(foliage);
    BspStaticProp missing = drum;
    missing.model = "models/missing/nothing.mdl";
    props.push_back(missing);

    CoordinateConverter converter;
    converter.SetUnitsPerMeter(kDefaultUnitsPerMeter);
    MaterialLibrary materials;
    StaticPropOptions options;
    MeshData mesh;
    StaticPropStats stats;
    ResolveStaticProps(props, MapReader(files), converter, materials, options, mesh, stats);

    SA_CHECK_EQ(stats.placements, size_t(5));
    SA_CHECK_EQ(stats.skippedNonSolid, size_t(1));
    SA_CHECK_EQ(stats.instanced, size_t(3));
    SA_CHECK_EQ(stats.fromPhy, size_t(2));
    SA_CHECK_EQ(stats.fromBox, size_t(1));
    SA_CHECK_EQ(stats.distinctModels, size_t(3));
    SA_CHECK_EQ(stats.missingModels, size_t(1));
    SA_CHECK_EQ(stats.missing.size(), size_t(1));
    SA_CHECK_EQ(mesh.triangles.size(), size_t(36));
    SA_CHECK_EQ(mesh.materialIndices.size(), size_t(36));
    SA_CHECK_EQ(mesh.vertices.size(), size_t(24));
    // Metal (drums) and wood (crate) resolve to two distinct materials.
    SA_CHECK_EQ(mesh.materials.size(), size_t(2));
    for (const IPLTriangle& t : mesh.triangles)
        for (int k = 0; k < 3; ++k)
            SA_CHECK(t.indices[k] >= 0 && static_cast<size_t>(t.indices[k]) < mesh.vertices.size());

    // First drum: yaw 90 around (1000, 0, 64) — box is symmetric in x/y so
    // extents are unchanged; Source (x,y,z) -> SA (x, z, -y) in meters.
    const float mpu = converter.metersPerUnit;
    std::vector<IPLVector3> first(mesh.vertices.begin(), mesh.vertices.begin() + 8);
    Bounds b = BoundsOf(first);
    SA_CHECK_NEAR(b.mins.x, (1000.f - 16.f) * mpu, 1e-3f);
    SA_CHECK_NEAR(b.maxs.x, (1000.f + 16.f) * mpu, 1e-3f);
    SA_CHECK_NEAR(b.mins.y, 64.f * mpu, 1e-3f);
    SA_CHECK_NEAR(b.maxs.y, (64.f + 48.f) * mpu, 1e-3f);
    SA_CHECK_NEAR(b.mins.z, -16.f * mpu, 1e-3f);
    SA_CHECK_NEAR(b.maxs.z, 16.f * mpu, 1e-3f);

    // Second drum: scale 2 at (0, 500, 0): x in [-32, 32], z in [0, 96], y in [468, 532] => SA z in [-532, -468].
    std::vector<IPLVector3> second(mesh.vertices.begin() + 8, mesh.vertices.begin() + 16);
    b = BoundsOf(second);
    SA_CHECK_NEAR(b.mins.x, -32.f * mpu, 1e-3f);
    SA_CHECK_NEAR(b.maxs.x, 32.f * mpu, 1e-3f);
    SA_CHECK_NEAR(b.mins.y, 0.f, 1e-3f);
    SA_CHECK_NEAR(b.maxs.y, 96.f * mpu, 1e-3f);
    SA_CHECK_NEAR(b.mins.z, -532.f * mpu, 1e-3f);
    SA_CHECK_NEAR(b.maxs.z, -468.f * mpu, 1e-3f);

    // Crate: hull box from the .mdl.
    std::vector<IPLVector3> third(mesh.vertices.begin() + 16, mesh.vertices.begin() + 24);
    b = BoundsOf(third);
    SA_CHECK_NEAR(b.mins.x, -20.f * mpu, 1e-3f);
    SA_CHECK_NEAR(b.maxs.y, 40.f * mpu, 1e-3f);

    // Disabled => nothing.
    MeshData none;
    options.enabled = false;
    ResolveStaticProps(props, MapReader(files), converter, materials, options, none, stats);
    SA_CHECK(none.Empty());
    SA_CHECK_EQ(stats.placements, size_t(0));

    // No box fallback => the crate is reported missing.
    options.enabled = true;
    options.boxFallback = false;
    MeshData phyOnly;
    ResolveStaticProps(props, MapReader(files), converter, materials, options, phyOnly, stats);
    SA_CHECK_EQ(stats.instanced, size_t(2));
    SA_CHECK_EQ(stats.missingModels, size_t(2));
}

SA_TEST(StaticProps_LoaderFallsBackAndReportsErrors)
{
    std::map<std::string, std::vector<uint8_t>> files;
    files["models/a.phy"] = std::vector<uint8_t>(40, 0xEE); // corrupt collision model
    files["models/a.mdl"] = satest::BuildMdl("a", {-1.f, -1.f, -1.f}, {1.f, 1.f, 1.f}, "plastic");
    files["models/b.mdl"] = satest::BuildMdl("b", {}, {}, "");
    files["models/c.phy"] = OilDrum().Build();

    StaticPropOptions options;
    PropModelGeometry geometry;
    // Corrupt .phy with a tiny hull: box is below the minimum extent => rejected.
    SA_CHECK(!LoadPropModelGeometry("models/a.mdl", MapReader(files), options, geometry));
    SA_CHECK(!geometry.valid);
    options.minBoxExtentUnits = 0.5f;
    SA_CHECK(LoadPropModelGeometry("models/a.mdl", MapReader(files), options, geometry));
    SA_CHECK(geometry.valid && !geometry.fromCollision);
    SA_CHECK_EQ(geometry.triangles.size(), size_t(12));
    SA_CHECK(geometry.materialNames.size() == 1 && geometry.materialNames[0] == "plastic");

    // Empty hull box.
    SA_CHECK(!LoadPropModelGeometry("models/b.mdl", MapReader(files), options, geometry));
    SA_CHECK(!geometry.error.empty());

    // Collision model present, .phy surfaceprop used, .mdl not required.
    SA_CHECK(LoadPropModelGeometry("models/c.mdl", MapReader(files), options, geometry));
    SA_CHECK(geometry.fromCollision);
    SA_CHECK(geometry.materialNames[0] == "metal_barrel");

    // Per-model triangle cap pushes an oversized collision model to the box path.
    files["models/c.mdl"] = satest::BuildMdl("c", {-16.f, -16.f, 0.f}, {16.f, 16.f, 48.f}, "metal");
    options.maxModelTriangles = 4;
    SA_CHECK(LoadPropModelGeometry("models/c.mdl", MapReader(files), options, geometry));
    SA_CHECK(!geometry.fromCollision);
    SA_CHECK_EQ(geometry.triangles.size(), size_t(12));

    SA_CHECK(!LoadPropModelGeometry("", MapReader(files), options, geometry));
    SA_CHECK(!LoadPropModelGeometry("models/none.mdl", GameFileReader{}, options, geometry));
}
