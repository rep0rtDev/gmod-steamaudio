// tests/TestEngineFileSystem.cpp
#include <string>
#include <vector>

#include "TestFramework.h"
#include "core/EngineFileSystem.h"
#include "util/Json.h"

using namespace sa;

SA_TEST(FileSystem_Sdk2013SlotLayouts)
{
    // IBaseFileSystem declaration order in the secondary vtable.
    const FileSystemSlots gcc = FileSystemSlots::Sdk2013(1, 0, false);
    SA_CHECK(gcc.Complete());
    SA_CHECK_EQ(gcc.thisOffset, 1);
    SA_CHECK_EQ(gcc.read, 0);
    SA_CHECK_EQ(gcc.open, 2);
    SA_CHECK_EQ(gcc.close, 3);
    SA_CHECK_EQ(gcc.seek, 4);
    SA_CHECK_EQ(gcc.tell, 5);
    SA_CHECK_EQ(gcc.sizeHandle, 6);
    SA_CHECK_EQ(gcc.sizeName, 7);
    SA_CHECK_EQ(gcc.fileExists, 10);

    // MSVC swaps the adjacent Size overloads.
    const FileSystemSlots msvc = FileSystemSlots::Sdk2013(1, 0, true);
    SA_CHECK_EQ(msvc.sizeHandle, 7);
    SA_CHECK_EQ(msvc.sizeName, 6);

    // Flattened layout after a 9-entry IAppSystem prefix.
    const FileSystemSlots flat = FileSystemSlots::Sdk2013(0, 9, false);
    SA_CHECK_EQ(flat.thisOffset, 0);
    SA_CHECK_EQ(flat.read, 9);
    SA_CHECK_EQ(flat.fileExists, 19);

    SA_CHECK(!FileSystemSlots{}.Complete());
}

SA_TEST(FileSystem_ConfigParsing)
{
    FileSystemConfig cfg;
    SA_CHECK(cfg.module == "filesystem_stdio.dll");
    SA_CHECK(cfg.interfaceVersion == "VFileSystem022");
    SA_CHECK(cfg.pathId == "GAME");
    SA_CHECK_EQ(cfg.gmaDirectories.size(), size_t(3));
    SA_CHECK(!cfg.slots.Complete());

    const auto r = ParseJson(R"({
        "module": "filesystem_steam.dll",
        "interface": "VFileSystem021",
        "path_id": "MOD",
        "probe_file": "steam.inf",
        "probe_token": "PatchVersion",
        "disabled": true,
        "gma_directories": ["addons", "D:/gma", 42],
        "slots": {
            "x86": { "this_offset": 0, "read": 9, "open": 11, "close": 12, "seek": 13, "tell": 14,
                     "size_handle": 16, "size_name": 15, "file_exists": 19 },
            "x64": { "this_offset": 1, "read": 0, "open": 2, "close": 3, "seek": 4, "tell": 5,
                     "size_handle": 7, "size_name": 6, "file_exists": 10 }
        }
    })");
    SA_CHECK(r.ok);
    cfg.Parse(r.value);
    SA_CHECK(cfg.module == "filesystem_steam.dll");
    SA_CHECK(cfg.interfaceVersion == "VFileSystem021");
    SA_CHECK(cfg.pathId == "MOD");
    SA_CHECK(cfg.probeFile == "steam.inf");
    SA_CHECK(cfg.probeToken == "PatchVersion");
    SA_CHECK(cfg.disabled);
    SA_CHECK_EQ(cfg.gmaDirectories.size(), size_t(2)); // non-strings dropped
    SA_CHECK(cfg.gmaDirectories[1] == "D:/gma");

    cfg.ParseSlots(r.value["slots"]["x86"]);
    SA_CHECK(cfg.slots.Complete());
    SA_CHECK_EQ(cfg.slots.thisOffset, 0);
    SA_CHECK_EQ(cfg.slots.read, 9);
    SA_CHECK_EQ(cfg.slots.sizeHandle, 16);
    SA_CHECK_EQ(cfg.slots.fileExists, 19);

    cfg.ParseSlots(r.value["slots"]["x64"]);
    SA_CHECK_EQ(cfg.slots.thisOffset, 1);
    SA_CHECK_EQ(cfg.slots.read, 0);
    SA_CHECK_EQ(cfg.slots.sizeHandle, 7);

    // Partial slot tables leave the remaining entries untouched and incomplete.
    FileSystemConfig partial;
    const auto p = ParseJson(R"({ "read": 3 })");
    partial.ParseSlots(p.value);
    SA_CHECK_EQ(partial.slots.read, 3);
    SA_CHECK(!partial.slots.Complete());

    // Non-object input is ignored.
    FileSystemConfig untouched;
    untouched.Parse(ParseJson("[1,2,3]").value);
    untouched.ParseSlots(ParseJson("\"x\"").value);
    SA_CHECK(untouched.module == "filesystem_stdio.dll");
    SA_CHECK(!untouched.slots.Complete());
}

SA_TEST(FileSystem_UnavailableIsSafe)
{
    // Off-game (no filesystem_stdio.dll in the process) every entry point must
    // fail gracefully instead of touching a null interface.
    EngineFileSystem fs;
    SA_CHECK(!fs.Available());

    FileSystemConfig cfg;
    cfg.module = "sa_definitely_not_loaded_module.dll";
    std::string error;
    SA_CHECK(!fs.Initialize(cfg, error));
    SA_CHECK(!error.empty());
    SA_CHECK(!fs.Available());
    SA_CHECK(fs.Description().empty());

    std::vector<uint8_t> out{1, 2, 3};
    SA_CHECK(!fs.FileExists("gameinfo.txt"));
    SA_CHECK_EQ(fs.FileSize("gameinfo.txt"), 0u);
    SA_CHECK(!fs.ReadFile("gameinfo.txt", out, error));
    SA_CHECK(out.empty());
    out = {1};
    SA_CHECK(!fs.ReadRange("gameinfo.txt", 0, 16, out, error));
    SA_CHECK(out.empty());

    cfg.disabled = true;
    SA_CHECK(!fs.Initialize(cfg, error));
    SA_CHECK(error.find("disabled") != std::string::npos);
    fs.Shutdown();
    SA_CHECK(!fs.Available());
}
