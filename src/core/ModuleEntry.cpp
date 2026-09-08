// src/core/ModuleEntry.cpp
//
// GMod binary module entry points (gmod13_open / gmod13_close). Loaded from
// the client realm with `require("steamaudio")`. In any other realm (server
// state of a listen server, menu state, dedicated server) the module only
// registers an inert `steamaudio` table: those processes/realms either have no
// audio device or are not allowed to touch it.
#include <memory>
#include <string>

#include "GarrysMod/Lua/Interface.h"
#include "core/AudioEngine.h"
#include "lua/ConVars.h"
#include "lua/LuaBindings.h"
#include "util/Logging.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace {

std::unique_ptr<sa::AudioEngine> g_engine;

std::string DirectoryOf(const std::string& path)
{
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

std::string ParentOf(const std::string& dir)
{
    std::string d = dir;
    while (!d.empty() && (d.back() == '/' || d.back() == '\\'))
        d.pop_back();
    return DirectoryOf(d);
}

std::string Join(const std::string& a, const std::string& b)
{
#if defined(_WIN32)
    const char sep = '\\';
#else
    const char sep = '/';
#endif
    if (a.empty())
        return b;
    if (a.back() == '/' || a.back() == '\\')
        return a + b;
    return a + sep + b;
}

bool Exists(const std::string& path)
{
#if defined(_WIN32)
    return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
#else
    return access(path.c_str(), F_OK) == 0;
#endif
}

std::string ModuleDirectory()
{
#if defined(_WIN32)
    HMODULE self = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&ModuleDirectory), &self)) {
        char buf[MAX_PATH * 2];
        const DWORD n = GetModuleFileNameA(self, buf, sizeof(buf));
        if (n > 0 && n < sizeof(buf))
            return DirectoryOf(std::string(buf, n));
    }
    return ".";
#else
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(&ModuleDirectory), &info) && info.dli_fname)
        return DirectoryOf(info.dli_fname);
    return ".";
#endif
}

std::string WorkingDirectory()
{
#if defined(_WIN32)
    char buf[MAX_PATH * 2];
    const DWORD n = GetCurrentDirectoryA(sizeof(buf), buf);
    return (n > 0 && n < sizeof(buf)) ? std::string(buf, n) : ".";
#else
    char buf[4096];
    return getcwd(buf, sizeof(buf)) ? std::string(buf) : ".";
#endif
}

// garrysmod/ is two levels above lua/bin/<module>.dll; fall back to the
// process working directory (hl2.exe runs from the game root).
sa::EnginePaths ResolvePaths()
{
    sa::EnginePaths paths;
    paths.moduleDirectory = ModuleDirectory();
    const std::string fromModule = ParentOf(ParentOf(paths.moduleDirectory));
    const std::string cwd = WorkingDirectory();
    if (Exists(Join(fromModule, "gameinfo.txt")))
        paths.gameDirectory = fromModule;
    else if (Exists(Join(Join(cwd, "garrysmod"), "gameinfo.txt")))
        paths.gameDirectory = Join(cwd, "garrysmod");
    else if (Exists(Join(cwd, "gameinfo.txt")))
        paths.gameDirectory = cwd;
    else
        paths.gameDirectory = fromModule;
    const std::string data = Join(Join(paths.gameDirectory, "data"), "steamaudio");
    paths.configDirectory = data;
    paths.cacheDirectory = Join(data, "cache");
    paths.logFile = Join(data, "steamaudio.log");
    return paths;
}

bool GlobalBool(GarrysMod::Lua::ILuaBase* LUA, const char* name)
{
    LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
    LUA->GetField(-1, name);
    const bool value = LUA->IsType(-1, GarrysMod::Lua::Type::Bool) && LUA->GetBool(-1);
    LUA->Pop(2);
    return value;
}

} // namespace

GMOD_MODULE_OPEN()
{
    const bool client = GlobalBool(LUA, "CLIENT") && !GlobalBool(LUA, "SERVER") && !GlobalBool(LUA, "MENU_DLL");

    g_engine = std::make_unique<sa::AudioEngine>();
    sa::lua::BindEngine(g_engine.get());
    sa::lua::RegisterBindings(LUA);

    if (!client) {
        // Server/menu realm: no audio work at all; the table stays inert.
        SA_LOGI("[module] loaded outside the client realm; audio engine stays inactive");
        return 0;
    }

    const sa::EnginePaths paths = ResolvePaths();
    std::string error;
    GarrysMod::Lua::ILuaBase* lua = LUA;
    const bool ok = g_engine->Initialize(
        paths, error, [lua](sa::RuntimeConfig& rt, sa::StaticConfig& st) {
            sa::lua::RegisterConVars(lua, rt, st);
            sa::lua::ApplyConVarOverrides(lua, rt, st);
        });
    if (!ok)
        SA_LOGE("[module] initialization failed: %s", error.c_str());
    sa::lua::RunBootstrap(LUA);

    // Surface startup log lines immediately.
    LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
    LUA->GetField(-1, "MsgN");
    const bool haveMsgN = LUA->IsType(-1, GarrysMod::Lua::Type::Function);
    LUA->Pop(2);
    if (haveMsgN) {
        for (const std::string& line : sa::log::Drain(256)) {
            LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
            LUA->GetField(-1, "MsgN");
            LUA->PushString(line.c_str());
            LUA->PCall(1, 0, 0);
            LUA->Pop();
        }
    }
    return 0;
}

// Runs while the client Lua state is being destroyed: hooks, concommands and
// the global table die with the state, and calling back into Lua from here
// (RunString, hook.Remove, ...) faults inside lua_shared. Native-only teardown.
GMOD_MODULE_CLOSE()
{
    (void)LUA;
    if (g_engine) {
        g_engine->Shutdown();
        sa::lua::BindEngine(nullptr);
        g_engine.reset();
    }
    return 0;
}
