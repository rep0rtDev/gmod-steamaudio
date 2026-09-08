// src/steamaudio/PhononApi.cpp
#define SA_PHONON_NO_MACROS
#include "PhononApi.h"

#include <mutex>

#include "util/Logging.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace sa {
namespace phonon {

namespace {

struct LoaderState {
    std::mutex mutex;
    ApiTable table;
    bool loaded = false;
    std::string path;
#ifdef _WIN32
    HMODULE handle = nullptr;
#else
    void* handle = nullptr;
#endif
};

LoaderState& State()
{
    static LoaderState s;
    return s;
}

const char* LibraryFileName()
{
#ifdef _WIN32
    return "phonon.dll";
#elif defined(__APPLE__)
    return "libphonon.dylib";
#else
    return "libphonon.so";
#endif
}

void* OpenLibrary(const std::string& path)
{
#ifdef _WIN32
    // LOAD_WITH_ALTERED_SEARCH_PATH makes dependent DLLs (TrueAudioNext.dll,
    // GPUUtilities.dll) resolve relative to phonon.dll itself.
    return reinterpret_cast<void*>(LoadLibraryExA(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH));
#else
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void* ResolveSymbol(void* handle, const char* name)
{
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(handle), name));
#else
    return dlsym(handle, name);
#endif
}

void CloseLibrary(void* handle)
{
#ifdef _WIN32
    FreeLibrary(reinterpret_cast<HMODULE>(handle));
#else
    dlclose(handle);
#endif
}

std::string JoinPath(const std::string& dir, const char* file)
{
    if (dir.empty())
        return file;
    const char last = dir.back();
    if (last == '/' || last == '\\')
        return dir + file;
#ifdef _WIN32
    return dir + "\\" + file;
#else
    return dir + "/" + file;
#endif
}

} // namespace

bool Load(const std::vector<std::string>& searchDirectories, std::string& errorOut)
{
    LoaderState& s = State();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.loaded)
        return true;

    std::vector<std::string> candidates;
    for (const std::string& dir : searchDirectories)
        candidates.push_back(JoinPath(dir, LibraryFileName()));
    candidates.push_back(LibraryFileName());

    void* handle = nullptr;
    std::string chosen;
    std::string attempts;
    for (const std::string& candidate : candidates) {
        handle = OpenLibrary(candidate);
        if (handle) {
            chosen = candidate;
            break;
        }
        attempts += candidate;
        attempts += "; ";
    }
    if (!handle) {
        errorOut = "could not load " + std::string(LibraryFileName()) + " (tried: " + attempts + ")";
        return false;
    }

    ApiTable table;
    std::string missing;
#define SA_PHONON_RESOLVE(name)                                                        \
    table.name = reinterpret_cast<decltype(table.name)>(ResolveSymbol(handle, #name)); \
    if (!table.name) {                                                                 \
        missing += #name;                                                              \
        missing += ' ';                                                                \
    }
    SA_PHONON_FUNCTIONS(SA_PHONON_RESOLVE)
#undef SA_PHONON_RESOLVE

    if (!missing.empty()) {
        CloseLibrary(handle);
        errorOut = "phonon library at " + chosen + " is missing symbols: " + missing +
                   "(expected Steam Audio " + std::to_string(STEAMAUDIO_VERSION_MAJOR) + "." +
                   std::to_string(STEAMAUDIO_VERSION_MINOR) + ".x)";
        return false;
    }

    table.pathCache.version = reinterpret_cast<PathCacheApi::VersionFn>(ResolveSymbol(handle, "saPathCacheVersion"));
    table.pathCache.configure = reinterpret_cast<PathCacheApi::ConfigureFn>(ResolveSymbol(handle, "saPathCacheConfigure"));
    table.pathCache.lookup = reinterpret_cast<PathCacheApi::LookupFn>(ResolveSymbol(handle, "saPathCacheLookup"));
    table.pathCache.stats = reinterpret_cast<PathCacheApi::StatsFn>(ResolveSymbol(handle, "saPathCacheStats"));
    table.pathCache.error = reinterpret_cast<PathCacheApi::ErrorFn>(ResolveSymbol(handle, "saPathCacheError"));
    table.pathCache.probes = reinterpret_cast<PathCacheApi::ReadProbesFn>(ResolveSymbol(handle, "saPathCacheReadProbes"));
    table.pathCache.attach = reinterpret_cast<PathCacheApi::AttachFn>(ResolveSymbol(handle, "saPathCacheAttach"));
    s.table = table;
#ifdef _WIN32
    s.handle = reinterpret_cast<HMODULE>(handle);
    char resolved[MAX_PATH] = {};
    if (GetModuleFileNameA(s.handle, resolved, MAX_PATH) > 0)
        chosen = resolved;
#else
    s.handle = handle;
#endif
    s.path = chosen;
    s.loaded = true;
    SA_LOGI("Loaded Steam Audio runtime from %s", s.path.c_str());
    return true;
}

void Unload()
{
    LoaderState& s = State();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.loaded)
        return;
    CloseLibrary(reinterpret_cast<void*>(s.handle));
    s.handle = nullptr;
    s.table = ApiTable{};
    s.loaded = false;
    s.path.clear();
}

bool IsLoaded()
{
    LoaderState& s = State();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.loaded;
}

const std::string& LoadedPath()
{
    return State().path;
}

GpuRuntime PreloadGpuRuntime()
{
    GpuRuntime rt;
    std::string dir;
    {
        LoaderState& s = State();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.loaded)
            return rt;
        const size_t slash = s.path.find_last_of("/\\");
        if (slash != std::string::npos)
            dir = s.path.substr(0, slash);
    }

    // Deliberately never freed: the handles keep the delay-loaded modules
    // resident for the lifetime of phonon.dll.
    auto pin = [&](const char* name) {
#ifdef _WIN32
        if (!dir.empty() && OpenLibrary(JoinPath(dir, name)))
            return true;
        return OpenLibrary(name) != nullptr;
#else
        (void)name;
        return false;
#endif
    };

    rt.openCL = pin("OpenCL.dll");
    rt.gpuUtilities = pin("GPUUtilities.dll");
    rt.trueAudioNext = pin("TrueAudioNext.dll");
    if (!rt.openCL)
        rt.missing += "OpenCL.dll ";
    if (!rt.gpuUtilities)
        rt.missing += "GPUUtilities.dll ";
    if (!rt.trueAudioNext)
        rt.missing += "TrueAudioNext.dll ";
    return rt;
}

const ApiTable& Api()
{
    return State().table;
}

} // namespace phonon
} // namespace sa
