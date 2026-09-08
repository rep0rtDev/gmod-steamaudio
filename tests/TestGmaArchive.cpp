// tests/TestGmaArchive.cpp
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "GmaFixture.h"
#include "TestFramework.h"
#include "util/GmaArchive.h"
#include "util/Lzma.h"

using namespace sa;

namespace {

namespace fs = std::filesystem;

struct TempDir {
    fs::path path;

    TempDir()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("sa_gma_test_" + std::to_string(stamp));
        fs::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    std::string Write(const std::string& name, const std::vector<uint8_t>& bytes) const
    {
        const fs::path file = path / name;
        fs::create_directories(file.parent_path());
        std::ofstream f(file, std::ios::binary);
        f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return file.u8string();
    }
};

satest::GmaFixture MapAddon()
{
    satest::GmaFixture fx;
    fx.requiredContent = {"hl2"};
    fx.files.push_back({"maps/gm_fixture.bsp", satest::Bytes("VBSP-fixture-bytes-0123456789")});
    fx.files.push_back({"materials/Maps/gm_fixture/wall.vmt", satest::Bytes("\"LightmappedGeneric\"{}")});
    fx.files.push_back({"models/props/crate.mdl", std::vector<uint8_t>(5000, 0x5A)});
    fx.files.push_back({"maps/gm_fixture.nav", {}});
    return fx;
}

} // namespace

SA_TEST(Gma_ParsesHeaderAndIndex)
{
    const satest::GmaFixture fx = MapAddon();
    std::vector<uint8_t> bytes = fx.Build();

    GmaArchive archive;
    std::string error;
    SA_CHECK(archive.OpenMemory(std::move(bytes), error));
    SA_CHECK(archive.IsOpen());
    SA_CHECK(!archive.WasCompressed());

    const GmaHeader& h = archive.Header();
    SA_CHECK_EQ(h.version, 3);
    SA_CHECK_EQ(h.steamId, fx.steamId);
    SA_CHECK_EQ(h.timestamp, fx.timestamp);
    SA_CHECK_EQ(h.requiredContent.size(), size_t(1));
    SA_CHECK(h.requiredContent[0] == "hl2");
    SA_CHECK(h.name == fx.name);
    SA_CHECK(h.description == fx.description);
    SA_CHECK(h.author == fx.author);
    SA_CHECK_EQ(h.addonVersion, 1);

    SA_CHECK_EQ(archive.Entries().size(), size_t(4));
    const size_t dataStart = fx.DataStart();
    SA_CHECK_EQ(archive.Entries()[0].offset, dataStart);
    SA_CHECK_EQ(archive.Entries()[0].size, fx.files[0].data.size());
    SA_CHECK_EQ(archive.Entries()[1].offset, dataStart + fx.files[0].data.size());
    SA_CHECK_EQ(archive.Entries()[2].size, uint64_t(5000));
    // Names are canonicalised on load.
    SA_CHECK(archive.Entries()[1].name == "materials/maps/gm_fixture/wall.vmt");
}

SA_TEST(Gma_FindIsCaseAndSeparatorInsensitive)
{
    GmaArchive archive;
    std::string error;
    SA_CHECK(archive.OpenMemory(MapAddon().Build(), error));

    SA_CHECK(archive.Find("maps/gm_fixture.bsp") != nullptr);
    SA_CHECK(archive.Find("MAPS\\GM_Fixture.BSP") != nullptr);
    SA_CHECK(archive.Find("/maps/gm_fixture.bsp") != nullptr);
    SA_CHECK(archive.Find("materials/maps/gm_fixture/WALL.vmt") != nullptr);
    SA_CHECK(archive.Find("maps/other.bsp") == nullptr);
    SA_CHECK(archive.Find("gm_fixture.bsp") == nullptr);

    SA_CHECK(GmaArchive::NormalizePath("\\\\Maps\\A\\B.bsp") == "maps/a/b.bsp");
}

SA_TEST(Gma_ExtractsFromMemoryAndDisk)
{
    const satest::GmaFixture fx = MapAddon();
    const std::vector<uint8_t> bytes = fx.Build();

    GmaArchive mem;
    std::string error;
    SA_CHECK(mem.OpenMemory(std::vector<uint8_t>(bytes), error));
    std::vector<uint8_t> out;
    SA_CHECK(mem.Extract(*mem.Find("maps/gm_fixture.bsp"), out, error));
    SA_CHECK(out == fx.files[0].data);
    SA_CHECK(mem.Extract(*mem.Find("models/props/crate.mdl"), out, error));
    SA_CHECK(out == fx.files[2].data);
    SA_CHECK(mem.Extract(*mem.Find("maps/gm_fixture.nav"), out, error));
    SA_CHECK(out.empty());

    TempDir tmp;
    const std::string path = tmp.Write("addon.gma", bytes);
    GmaArchive disk;
    SA_CHECK(disk.Open(path, error));
    SA_CHECK(disk.Path() == path);
    SA_CHECK_EQ(disk.Entries().size(), size_t(4));
    SA_CHECK(disk.Extract(*disk.Find("materials/maps/gm_fixture/wall.vmt"), out, error));
    SA_CHECK(out == fx.files[1].data);
    SA_CHECK(disk.Extract(*disk.Find("models/props/crate.mdl"), out, error));
    SA_CHECK(out == fx.files[2].data);

    // Entries that lie about their extent are refused rather than read past EOF.
    GmaEntry bogus = *disk.Find("maps/gm_fixture.bsp");
    bogus.size = 1 << 20;
    SA_CHECK(!disk.Extract(bogus, out, error));
    SA_CHECK(out.empty());
}

SA_TEST(Gma_IndexLargerThanInitialWindowIsReadIncrementally)
{
    // An index that does not fit in the first 256 KiB read forces Open() to
    // widen its window instead of giving up.
    satest::GmaFixture fx;
    for (int i = 0; i < 6000; ++i)
        fx.files.push_back({"materials/models/very/long/directory/name/to/inflate/the/index/file_" +
                                std::to_string(i) + ".vtf",
                            satest::Bytes("x")});
    fx.files.push_back({"maps/last.bsp", satest::Bytes("last")});
    const std::vector<uint8_t> bytes = fx.Build();
    SA_CHECK(fx.DataStart() > 256 * 1024);

    TempDir tmp;
    GmaArchive disk;
    std::string error;
    SA_CHECK(disk.Open(tmp.Write("big.gma", bytes), error));
    SA_CHECK_EQ(disk.Entries().size(), size_t(6001));
    std::vector<uint8_t> out;
    SA_CHECK(disk.Extract(*disk.Find("maps/last.bsp"), out, error));
    SA_CHECK(out == satest::Bytes("last"));
}

SA_TEST(Gma_OpensLzmaAloneCompressedArchives)
{
    const satest::GmaFixture fx = MapAddon();
    const std::vector<uint8_t> plain = fx.Build();

    for (bool declared : {true, false}) {
        std::vector<uint8_t> packed = satest::GmaFixture::LzmaAlone(plain, declared);
        SA_CHECK(lzma::IsLzmaAlone(packed.data(), packed.size()));

        GmaArchive mem;
        std::string error;
        const bool ok = mem.OpenMemory(std::vector<uint8_t>(packed), error);
        if (!declared) {
            // A literal-only stream without end marker and without a declared
            // size is ambiguous; the decoder must refuse rather than guess.
            SA_CHECK(!ok);
            continue;
        }
        SA_CHECK(ok);
        SA_CHECK(mem.WasCompressed());
        std::vector<uint8_t> out;
        SA_CHECK(mem.Extract(*mem.Find("maps/gm_fixture.bsp"), out, error));
        SA_CHECK(out == fx.files[0].data);

        TempDir tmp;
        GmaArchive disk;
        SA_CHECK(disk.Open(tmp.Write("123456.cache", packed), error));
        SA_CHECK(disk.WasCompressed());
        SA_CHECK(disk.Extract(*disk.Find("models/props/crate.mdl"), out, error));
        SA_CHECK(out == fx.files[2].data);
    }
}

SA_TEST(Gma_RejectsMalformedArchives)
{
    std::string error;
    std::vector<uint8_t> out;

    GmaArchive bad;
    SA_CHECK(!bad.OpenMemory(satest::Bytes("not an archive at all, just text"), error));
    SA_CHECK(!bad.IsOpen());

    std::vector<uint8_t> wrongVersion = MapAddon().Build();
    wrongVersion[4] = 9;
    SA_CHECK(!bad.OpenMemory(std::move(wrongVersion), error));

    std::vector<uint8_t> truncated = MapAddon().Build();
    truncated.resize(MapAddon().DataStart() - 6); // cut inside the index
    SA_CHECK(!bad.OpenMemory(std::move(truncated), error));
    SA_CHECK(error.find("truncated") != std::string::npos);

    // Index claims more payload than the archive holds.
    satest::GmaFixture liar = MapAddon();
    std::vector<uint8_t> bytes = liar.Build();
    bytes.resize(liar.DataStart() + 10);
    SA_CHECK(!bad.OpenMemory(std::move(bytes), error));

    // Entry sizes that overflow are rejected before any allocation.
    satest::GmaFixture huge;
    huge.files.push_back({"a", {}});
    std::vector<uint8_t> hugeBytes = huge.BuildIndex();
    // Patch the size field of entry 1 (after number + name + NUL).
    const size_t sizeField = huge.DataStart() - 4 /*sentinel*/ - 4 /*crc*/ - 8;
    for (int i = 0; i < 8; ++i)
        hugeBytes[sizeField + i] = 0xFF;
    SA_CHECK(!bad.OpenMemory(std::move(hugeBytes), error));

    // Disk: missing file and non-archive file.
    TempDir tmp;
    SA_CHECK(!bad.Open((tmp.path / "missing.gma").u8string(), error));
    SA_CHECK(!bad.Open(tmp.Write("text.gma", satest::Bytes("hello world, definitely not GMAD")), error));
    SA_CHECK(!bad.Extract(GmaEntry{}, out, error));
}

SA_TEST(Gma_LocatorSearchesDirectories)
{
    TempDir tmp;

    satest::GmaFixture a;
    a.files.push_back({"maps/gm_alpha.bsp", satest::Bytes("alpha-bsp")});
    a.files.push_back({"shared/common.txt", satest::Bytes("from-a")});
    const std::string pathA = tmp.Write("addons/alpha.gma", a.Build());

    satest::GmaFixture b;
    b.files.push_back({"maps/gm_beta.bsp", satest::Bytes("beta-bsp")});
    b.files.push_back({"shared/common.txt", satest::Bytes("from-b")});
    tmp.Write("cache/workshop/2000.gma", b.Build());

    satest::GmaFixture c;
    c.files.push_back({"maps/gm_gamma.bsp", satest::Bytes("gamma-bsp")});
    tmp.Write("cache/workshop/3000.cache", satest::GmaFixture::LzmaAlone(c.Build()));

    tmp.Write("addons/readme.txt", satest::Bytes("ignored"));
    tmp.Write("addons/broken.gma", satest::Bytes("GMADzzz")); // unparsable, must be skipped

    GmaLocator locator;
    locator.SetDirectories({(tmp.path / "cache" / "workshop").u8string(), (tmp.path / "addons").u8string(),
                            (tmp.path / "does-not-exist").u8string()});
    SA_CHECK_EQ(locator.ScanArchives().size(), size_t(4));

    std::vector<uint8_t> out;
    std::string error, archive;
    SA_CHECK(locator.Find("maps/gm_alpha.bsp", out, error, &archive));
    SA_CHECK(out == satest::Bytes("alpha-bsp"));
    SA_CHECK(fs::u8path(archive).lexically_normal() == fs::u8path(pathA).lexically_normal());
    SA_CHECK(locator.Find("MAPS/GM_BETA.bsp", out, error, &archive));
    SA_CHECK(out == satest::Bytes("beta-bsp"));
    SA_CHECK(locator.Find("maps/gm_gamma.bsp", out, error, &archive));
    SA_CHECK(out == satest::Bytes("gamma-bsp"));
    SA_CHECK(archive.find("3000.cache") != std::string::npos);

    // Directory order is the priority order.
    SA_CHECK(locator.Find("shared/common.txt", out, error));
    SA_CHECK(out == satest::Bytes("from-b"));

    SA_CHECK(!locator.Find("maps/nope.bsp", out, error));
    SA_CHECK(out.empty());
    SA_CHECK(error.find("not found") != std::string::npos);

    archive.clear();
    SA_CHECK(locator.Contains("Maps\\gm_gamma.bsp", &archive));
    SA_CHECK(archive.find("3000.cache") != std::string::npos);
    SA_CHECK(!locator.Contains("maps/nope.bsp"));

    // New archives appear after Invalidate().
    satest::GmaFixture d;
    d.files.push_back({"maps/gm_delta.bsp", satest::Bytes("delta-bsp")});
    tmp.Write("addons/delta.gma", d.Build());
    SA_CHECK(!locator.Find("maps/gm_delta.bsp", out, error));
    locator.Invalidate();
    SA_CHECK(locator.Find("maps/gm_delta.bsp", out, error));
    SA_CHECK(out == satest::Bytes("delta-bsp"));

    GmaLocator empty;
    empty.SetDirectories({(tmp.path / "nothing").u8string()});
    SA_CHECK(!empty.Find("maps/gm_alpha.bsp", out, error));
}

SA_TEST(LzmaAlone_HeaderDetectionAndDecoding)
{
    const std::vector<uint8_t> plain = satest::Bytes("The quick brown fox jumps over the lazy dog 0123456789");
    std::vector<uint8_t> packed = satest::GmaFixture::LzmaAlone(plain);
    SA_CHECK(lzma::IsLzmaAlone(packed.data(), packed.size()));

    std::vector<uint8_t> out;
    std::string error;
    SA_CHECK(lzma::DecompressAlone(packed.data(), packed.size(), out, &error));
    SA_CHECK(out == plain);

    // Output limit is enforced against the declared size.
    SA_CHECK(!lzma::DecompressAlone(packed.data(), packed.size(), out, &error, plain.size() - 1));

    // Corrupt property byte / zero dictionary / absurd declared size are not
    // mistaken for LZMA-alone streams.
    std::vector<uint8_t> badProps = packed;
    badProps[0] = 0xFF;
    SA_CHECK(!lzma::IsLzmaAlone(badProps.data(), badProps.size()));
    std::vector<uint8_t> zeroDict = packed;
    for (int i = 1; i <= 4; ++i)
        zeroDict[i] = 0;
    SA_CHECK(!lzma::IsLzmaAlone(zeroDict.data(), zeroDict.size()));
    std::vector<uint8_t> hugeSize = packed;
    for (int i = 5; i < 12; ++i)
        hugeSize[i] = 0xFF;
    hugeSize[12] = 0x7F;
    SA_CHECK(!lzma::IsLzmaAlone(hugeSize.data(), hugeSize.size()));
    SA_CHECK(!lzma::IsLzmaAlone(packed.data(), 12));
    SA_CHECK(!lzma::IsLzmaAlone(satest::Bytes("GMAD").data(), 4));

    // Truncated payload fails cleanly.
    std::vector<uint8_t> cut(packed.begin(), packed.begin() + static_cast<std::ptrdiff_t>(packed.size() / 2));
    SA_CHECK(!lzma::DecompressAlone(cut.data(), cut.size(), out, &error));
    SA_CHECK(out.empty());

    // Streaming decoder: unknown size + explicit end marker is not something the
    // literal encoder produces, but the unknown-size path must at least not
    // return garbage for an empty payload.
    std::vector<uint8_t> emptyUnknown = satest::GmaFixture::LzmaAlone({}, false);
    const bool ok = lzma::DecompressAlone(emptyUnknown.data(), emptyUnknown.size(), out, &error);
    SA_CHECK(!ok || out.empty());
}
