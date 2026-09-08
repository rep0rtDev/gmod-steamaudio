// tests/TestBspGeometry.cpp
#include "BspFixture.h"
#include "TestFramework.h"
#include "steamaudio/BspGeometry.h"
#include "mixing/SimulationThread.h"

using namespace sa;

namespace {

bool Parse(const std::vector<uint8_t>& file, BspGeometry& out, SurfacePropResolver resolver = {})
{
    CoordinateConverter converter;
    MaterialLibrary materials;
    return ParseBsp(file.data(), file.size(), converter, materials, resolver, out);
}

} // namespace

SA_TEST(Bsp_BakeFingerprintTracksGeometryAndSettings)
{
    BspGeometry geometry;
    SA_CHECK(Parse(satest::MakeRoomBsp(), geometry));
    RuntimeConfig cfg;
    IPLSimulationSettings settings{};
    const uint64_t first = SimulationThread::BakeFingerprint(geometry, cfg, settings);
    cfg.masterVolume = 0.5f;
    SA_CHECK_EQ(SimulationThread::BakeFingerprint(geometry, cfg, settings), first);
    cfg.probeSpacing += 1.f;
    SA_CHECK(SimulationThread::BakeFingerprint(geometry, cfg, settings) != first);
    cfg.probeSpacing -= 1.f;
    geometry.world.vertices[0].x += 1.f;
    SA_CHECK(SimulationThread::BakeFingerprint(geometry, cfg, settings) != first);
    geometry.world.vertices[0].x -= 1.f;
    geometry.world.materials[0].scattering += 0.1f;
    SA_CHECK(SimulationThread::BakeFingerprint(geometry, cfg, settings) != first);
}

SA_TEST(Bsp_CompressedGameLumpRejectsInvalidOffset)
{
    auto bytes = satest::MakeRoomBsp();
    uint32_t gameOffset = 0;
    std::memcpy(&gameOffset, bytes.data() + 8 + 35 * 16, sizeof(gameOffset));
    const uint16_t compressed = 1;
    const uint32_t badOffset = static_cast<uint32_t>(bytes.size() + 4096);
    std::memcpy(bytes.data() + gameOffset + 8, &compressed, sizeof(compressed));
    std::memcpy(bytes.data() + gameOffset + 12, &badOffset, sizeof(badOffset));
    BspGeometry geometry;
    SA_CHECK(Parse(bytes, geometry));
    SA_CHECK(geometry.staticProps.empty());
}

SA_TEST(Bsp_RejectsGarbage)
{
    BspGeometry g;
    std::vector<uint8_t> junk(2048, 0xAB);
    SA_CHECK(!Parse(junk, g));
    SA_CHECK(!g.error.empty());
    std::vector<uint8_t> tiny(16, 0);
    SA_CHECK(!Parse(tiny, g));
}

SA_TEST(Bsp_ParsesWorldBrushModelsAndStaticProps)
{
    BspGeometry g;
    SA_CHECK(Parse(satest::MakeRoomBsp(), g));
    SA_CHECK_EQ(g.version, 21);
    // 6 quads -> 12 triangles; the sky face is skipped.
    SA_CHECK_EQ(g.world.triangles.size(), size_t(12));
    SA_CHECK_EQ(g.skippedFaces, size_t(1));
    SA_CHECK_EQ(g.world.materials.size(), size_t(1));
    SA_CHECK_EQ(g.world.materialIndices.size(), size_t(12));

    // Coordinates are converted to meters, Y-up: the 512x512x256 unit room
    // becomes ~9.75 x 4.88 x 9.75 m.
    float minY = 1e9f, maxY = -1e9f, maxX = -1e9f;
    for (const IPLVector3& v : g.world.vertices) {
        minY = std::min(minY, v.y);
        maxY = std::max(maxY, v.y);
        maxX = std::max(maxX, v.x);
    }
    SA_CHECK_NEAR(minY, 0.f, 1e-3);
    SA_CHECK_NEAR(maxY, 256.f / 52.4934f, 1e-3);
    SA_CHECK_NEAR(maxX, 256.f / 52.4934f, 1e-3);

    // Only the func_door references *1 as an acoustic class (trigger_ is skipped).
    SA_CHECK_EQ(g.brushModels.size(), size_t(1));
    SA_CHECK(g.brushModels[0].classname == "func_door");
    SA_CHECK(g.brushModels[0].targetname == "door1");
    SA_CHECK_EQ(g.brushModels[0].modelIndex, 1);
    SA_CHECK_NEAR(g.brushModels[0].spawnOrigin.x, 100.f, 1e-4);
    SA_CHECK_EQ(g.brushModels[0].mesh.triangles.size(), size_t(12));

    SA_CHECK_EQ(g.staticProps.size(), size_t(2));
    SA_CHECK(g.staticProps[0].model == "models/props_c17/oildrum001.mdl");
    SA_CHECK(g.staticProps[1].model == "models/props/crate.mdl");
    SA_CHECK_NEAR(g.staticProps[1].origin.x, -50.f, 1e-4);
    SA_CHECK_NEAR(g.staticProps[0].angles.y, 90.f, 1e-4);
    SA_CHECK_EQ(int(g.staticProps[0].solid), 6);
}

SA_TEST(Bsp_UsesSurfacePropResolver)
{
    BspGeometry g;
    int calls = 0;
    SA_CHECK(Parse(satest::MakeRoomBsp(), g, [&](const std::string& tex) {
        ++calls;
        return tex.find("METAL") != std::string::npos ? "metal" : "concrete";
    }));
    SA_CHECK(calls >= 2);
    MaterialLibrary lib;
    const IPLMaterial& metal = lib.FromSurfaceProp("metal");
    const IPLMaterial& doorMat = g.brushModels[0].mesh.materials[0];
    SA_CHECK_NEAR(doorMat.absorption[0], metal.absorption[0], 1e-6);
}

SA_TEST(Bsp_ToleratesCorruptLumpTable)
{
    std::vector<uint8_t> file = satest::MakeRoomBsp();
    // Point the texinfo lump past the end of the file: parser must ignore it
    // (all faces then have no texture but still produce geometry).
    const size_t texinfoHeader = 8 + 6 * 16;
    const int32_t bogus = static_cast<int32_t>(file.size() * 4);
    std::memcpy(file.data() + texinfoHeader, &bogus, 4);
    BspGeometry g;
    SA_CHECK(Parse(file, g));
    SA_CHECK(g.world.triangles.size() >= 12);
}

namespace {

// Everything the uncompressed room fixture is expected to yield.
void CheckRoom(const BspGeometry& g)
{
    SA_CHECK_EQ(g.world.triangles.size(), size_t(12));
    SA_CHECK_EQ(g.skippedFaces, size_t(1));
    SA_CHECK_EQ(g.brushModels.size(), size_t(1));
    SA_CHECK(g.brushModels[0].classname == "func_door");
    SA_CHECK_EQ(g.staticProps.size(), size_t(2));
    SA_CHECK(g.staticProps[1].model == "models/props/crate.mdl");
    SA_CHECK_NEAR(g.staticProps[1].origin.x, -50.f, 1e-4);
}

} // namespace

SA_TEST(Bsp_ParsesLzmaCompressedLumps)
{
    // bspzip -repack style: every data lump is an LZMA blob, game lump
    // payloads are compressed individually with GAMELUMPFLAG_COMPRESSED.
    BspGeometry g;
    SA_CHECK(Parse(satest::MakeRoomBspWith([](satest::BspBuilder& b) {
                       for (bool& c : b.compressLump)
                           c = true;
                       b.compressLump[35] = false;
                       b.compressGameLumpData = true;
                   }),
                   g));
    CheckRoom(g);
    SA_CHECK(g.compressedLumps >= 10);

    // Mixed: only entities + faces compressed, static props plain.
    BspGeometry g2;
    SA_CHECK(Parse(satest::MakeRoomBspWith([](satest::BspBuilder& b) {
                       b.compressLump[0] = true;
                       b.compressLump[7] = true;
                   }),
                   g2));
    CheckRoom(g2);
    SA_CHECK_EQ(g2.compressedLumps, size_t(2));
}

SA_TEST(Bsp_ParsesCompressedGameLumpAsWhole)
{
    // Whole lump 35 compressed: game lump offsets are still absolute file
    // offsets and must be rebased onto the decompressed buffer.
    BspGeometry g;
    SA_CHECK(Parse(satest::MakeRoomBspWith([](satest::BspBuilder& b) { b.compressLump[35] = true; }), g));
    CheckRoom(g);
    SA_CHECK_EQ(g.compressedLumps, size_t(1));
}

SA_TEST(Bsp_ParsesL4D2LumpHeaderLayout)
{
    BspGeometry g;
    SA_CHECK(Parse(satest::MakeRoomBspWith([](satest::BspBuilder& b) {
                       b.l4d2HeaderLayout = true;
                       b.lumpVersions[7] = 1;
                       b.compressLump[3] = true;
                   }),
                   g));
    CheckRoom(g);
    SA_CHECK_EQ(g.compressedLumps, size_t(1));
}

SA_TEST(Bsp_ToleratesCorruptCompressedLump)
{
    std::vector<uint8_t> file = satest::MakeRoomBspWith([](satest::BspBuilder& b) {
        b.compressLump[6] = true; // texinfo
        b.compressGameLumpData = true;
    });
    // Corrupt the texinfo LZMA header's compressed-size field so it claims
    // more data than the lump holds; the lump is dropped, geometry survives.
    uint32_t texinfoOfs = 0;
    std::memcpy(&texinfoOfs, file.data() + 8 + 6 * 16, 4);
    const uint32_t huge = 0x7FFFFFF0u;
    std::memcpy(file.data() + texinfoOfs + 8, &huge, 4);
    BspGeometry g;
    SA_CHECK(Parse(file, g));
    SA_CHECK(g.world.triangles.size() >= 12);
    SA_CHECK_EQ(g.staticProps.size(), size_t(2));

    // Corrupt the sprp game lump payload header: props are dropped, the rest parses.
    std::vector<uint8_t> file2 = satest::MakeRoomBspWith([](satest::BspBuilder& b) {
        b.compressGameLumpData = true;
    });
    uint32_t gameOfs = 0;
    std::memcpy(&gameOfs, file2.data() + 8 + 35 * 16, 4);
    uint32_t sprpOfs = 0;
    std::memcpy(&sprpOfs, file2.data() + gameOfs + 4 + 8, 4);
    file2[sprpOfs] = 'X'; // break the "LZMA" id
    BspGeometry g2;
    SA_CHECK(Parse(file2, g2));
    SA_CHECK_EQ(g2.world.triangles.size(), size_t(12));
    SA_CHECK_EQ(g2.staticProps.size(), size_t(0));
}
