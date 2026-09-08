#pragma once

#include <cstdint>
#include "steamaudio/PathCacheFile.h"

#if defined(_WIN32) && !defined(_WIN64)
#define SA_PATHCALL __stdcall
#else
#define SA_PATHCALL
#endif

namespace sa {

struct PathCacheApiStats {
    uint32_t version = 1;
    uint32_t probes = 0;
    uint32_t completedRows = 0;
    uint32_t complete = 0;
    uint64_t residentBytes = 0;
    uint64_t diskBytes = 0;
    uint64_t pageReads = 0;
};

struct PathCacheProbe { float x, y, z, radius; };

struct PathCacheApi {
    using VersionFn = uint32_t(SA_PATHCALL*)();
    using ConfigureFn = int32_t(SA_PATHCALL*)(const char*, const char*, uint64_t, uint64_t);
    using LookupFn = int32_t(SA_PATHCALL*)(void*, int32_t, int32_t, PathCacheRecord*);
    using StatsFn = int32_t(SA_PATHCALL*)(void*, PathCacheApiStats*);
    using ErrorFn = const char*(SA_PATHCALL*)();
    using ReadProbesFn = int32_t(SA_PATHCALL*)(void*, uint32_t, PathCacheProbe*);
    using AttachFn = int32_t(SA_PATHCALL*)(void*, void*);
    VersionFn version = nullptr;
    ConfigureFn configure = nullptr;
    LookupFn lookup = nullptr;
    StatsFn stats = nullptr;
    ErrorFn error = nullptr;
    ReadProbesFn probes = nullptr;
    AttachFn attach = nullptr;
    bool Available() const { return version && configure && lookup && stats && error && probes && attach && version() == 1; }
};

}
