cmake_minimum_required(VERSION 3.20)

project(gmod_steamaudio
    VERSION 0.1.4
    DESCRIPTION "Steam Audio replacement for the Garry's Mod (Source) client audio pipeline"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(SA_BUILD_TESTS "Build host-side unit tests for the platform-independent parts" ON)
option(SA_ENABLE_LTO "Enable link-time optimization for release builds" OFF)
option(SA_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

# Static CRT so the module has no VC redistributable dependency inside the
# game process (the engine ships its own, possibly older, runtime).
if(MSVC AND NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
set(SA_STEAMAUDIO_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/steamaudio/include"
    CACHE PATH "Directory containing phonon.h (Steam Audio SDK 4.x)")
set(SA_GMOD_MODULE_BASE_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/gmod-module-base/include"
    CACHE PATH "Directory containing GarrysMod/Lua/Interface.h")
set(SA_MINHOOK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook"
    CACHE PATH "MinHook source tree (include/ and src/)")

# ---------------------------------------------------------------------------
# Platform / architecture detection -> GMod module file name
# ---------------------------------------------------------------------------
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(SA_ARCH_BITS 64)
else()
    set(SA_ARCH_BITS 32)
endif()

if(WIN32)
    if(SA_ARCH_BITS EQUAL 64)
        set(SA_MODULE_SUFFIX "win64")
    else()
        set(SA_MODULE_SUFFIX "win32")
    endif()
elseif(APPLE)
    set(SA_MODULE_SUFFIX "osx")
else()
    if(SA_ARCH_BITS EQUAL 64)
        set(SA_MODULE_SUFFIX "linux64")
    else()
        set(SA_MODULE_SUFFIX "linux")
    endif()
endif()

set(SA_MODULE_NAME "gmcl_steamaudio_${SA_MODULE_SUFFIX}")
message(STATUS "gmod-steamaudio: building ${SA_MODULE_NAME}.dll (${SA_ARCH_BITS}-bit)")

# ---------------------------------------------------------------------------
# Compiler flags
# ---------------------------------------------------------------------------
add_library(sa_compile_options INTERFACE)
if(MSVC)
    target_compile_options(sa_compile_options INTERFACE
        /W4 /permissive- /Zc:__cplusplus /EHsc /utf-8
        /wd4200 # zero-sized array in struct (Source SDK headers)
        /wd4324 # structure padded due to alignas
    )
    target_compile_definitions(sa_compile_options INTERFACE
        _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
    if(SA_WARNINGS_AS_ERRORS)
        target_compile_options(sa_compile_options INTERFACE /WX)
    endif()
else()
    if(SA_WARNINGS_AS_ERRORS)
        target_compile_options(sa_compile_options INTERFACE -Werror)
    endif()
    target_compile_options(sa_compile_options INTERFACE
        -Wall -Wextra -Wshadow -Wno-unused-parameter -Wno-missing-field-initializers
        -fno-strict-aliasing)
    if(WIN32)
        # MinGW: static runtime so the module has no libgcc/libstdc++ DLL dependencies.
        target_link_options(sa_compile_options INTERFACE -static -static-libgcc -static-libstdc++)
        target_compile_definitions(sa_compile_options INTERFACE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
endif()
if(SA_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT sa_ipo_ok OUTPUT sa_ipo_msg)
    if(sa_ipo_ok)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
    endif()
endif()

# ---------------------------------------------------------------------------
# Third-party: MinHook (Windows only)
# ---------------------------------------------------------------------------
if(WIN32)
    add_library(minhook STATIC
        ${SA_MINHOOK_DIR}/src/buffer.c
        ${SA_MINHOOK_DIR}/src/hook.c
        ${SA_MINHOOK_DIR}/src/trampoline.c
        ${SA_MINHOOK_DIR}/src/hde/hde32.c
        ${SA_MINHOOK_DIR}/src/hde/hde64.c)
    target_include_directories(minhook PUBLIC ${SA_MINHOOK_DIR}/include)
    target_compile_definitions(minhook PRIVATE WIN32_LEAN_AND_MEAN)
    set_target_properties(minhook PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# ---------------------------------------------------------------------------
# Third-party: LZMA SDK decoder (public domain) for compressed BSP lumps
# ---------------------------------------------------------------------------
add_library(lzma_dec STATIC third_party/lzma/LzmaDec.c)
target_include_directories(lzma_dec PUBLIC third_party/lzma)
target_compile_definitions(lzma_dec PRIVATE _7ZIP_ST)
set_target_properties(lzma_dec PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# Core library (platform-independent parts + Steam Audio layer)
# ---------------------------------------------------------------------------
set(SA_CORE_SOURCES
    src/util/Logging.cpp
    src/util/Json.cpp
    src/util/SignatureScanner.cpp
    src/util/Lzma.cpp
    src/util/GmaArchive.cpp
    src/util/KeyValues.cpp
    src/mixing/SoundSource.cpp
    src/mixing/FallbackMixer.cpp
    src/mixing/RoomDsp.cpp
    src/mixing/ProceduralAudioBridge.cpp
    src/steamaudio/PhononApi.cpp
    src/steamaudio/PhononContext.cpp
    src/steamaudio/GPUBackend.cpp
    src/steamaudio/CPUFallbackBackend.cpp
    src/steamaudio/SceneBuilder.cpp
    src/steamaudio/BspGeometry.cpp
    src/steamaudio/PhyModel.cpp
    src/steamaudio/StaticPropResolver.cpp
    src/steamaudio/SurfaceProps.cpp
    src/steamaudio/Simulator.cpp
    src/steamaudio/ReflectionSimulator.cpp
    src/steamaudio/PathingSimulator.cpp
    src/steamaudio/PathCacheFile.cpp
    src/steamaudio/HRTFRenderer.cpp
    src/core/ChannelCapture.cpp
    src/core/RttiLocator.cpp
    src/core/VmtHook.cpp
    src/core/EngineInterfaces.cpp
    src/core/EngineFileSystem.cpp
    src/core/SoundOverrides.cpp
    src/core/DynamicOccluders.cpp
    src/core/ClientEntityList.cpp
    src/core/EngineHooks.cpp
    src/core/Detour.cpp
    src/core/AudioEngine.cpp
    src/mixing/AudioThread.cpp
    src/mixing/SimulationThread.cpp
    src/mixing/BassBridge.cpp
    src/platform/PlatformAudioOutput_Null.cpp
)
if(WIN32)
    list(APPEND SA_CORE_SOURCES src/platform/PlatformAudioOutput_Windows.cpp)
endif()

add_library(sa_core STATIC ${SA_CORE_SOURCES})
target_include_directories(sa_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${SA_STEAMAUDIO_INCLUDE_DIR})
target_link_libraries(sa_core PUBLIC sa_compile_options PRIVATE lzma_dec)
target_compile_definitions(sa_core PUBLIC
    SA_MODULE_NAME="${SA_MODULE_NAME}"
    SA_VERSION_STRING="${PROJECT_VERSION}"
    SA_ARCH_BITS=${SA_ARCH_BITS})
if(WIN32)
    target_link_libraries(sa_core PUBLIC minhook ole32 avrt winmm uuid)
else()
    target_link_libraries(sa_core PUBLIC ${CMAKE_DL_LIBS} pthread)
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0)
    target_link_libraries(sa_core PUBLIC stdc++fs) # std::filesystem lives in a separate archive before GCC 9
endif()

# ---------------------------------------------------------------------------
# GMod client module
# ---------------------------------------------------------------------------
set(SA_MODULE_SOURCES
    src/core/ModuleEntry.cpp
    src/lua/LuaBindings.cpp
    src/lua/ConVars.cpp
)

add_library(${SA_MODULE_NAME} SHARED ${SA_MODULE_SOURCES})
target_include_directories(${SA_MODULE_NAME} PRIVATE ${SA_GMOD_MODULE_BASE_INCLUDE_DIR})
target_link_libraries(${SA_MODULE_NAME} PRIVATE sa_core)
set_target_properties(${SA_MODULE_NAME} PROPERTIES
    PREFIX ""
    OUTPUT_NAME "${SA_MODULE_NAME}"
    SUFFIX ".dll")
target_compile_definitions(${SA_MODULE_NAME} PRIVATE GMMODULE)
if(WIN32)
    target_link_libraries(${SA_MODULE_NAME} PRIVATE ole32 avrt winmm uuid)
endif()
if(MSVC AND SA_ARCH_BITS EQUAL 32)
    # GMod's 32-bit client exports; keep SSE2 baseline.
    target_compile_options(${SA_MODULE_NAME} PRIVATE /arch:SSE2)
    target_compile_options(sa_core PRIVATE /arch:SSE2)
endif()

if(WIN32)
    add_executable(sa_run_limited src/platform/RunLimited.cpp src/platform/BoundedProcess_Windows.cpp)
    target_include_directories(sa_run_limited PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(sa_run_limited PRIVATE WINVER=0x0602 _WIN32_WINNT=0x0602)
    target_link_libraries(sa_run_limited PRIVATE sa_compile_options)
    if(MINGW)
        target_link_options(sa_run_limited PRIVATE -municode)
    endif()
endif()

install(TARGETS ${SA_MODULE_NAME} RUNTIME DESTINATION lua/bin LIBRARY DESTINATION lua/bin)
install(DIRECTORY lua/ DESTINATION lua)
install(DIRECTORY config/ DESTINATION data/steamaudio)
install(FILES third_party/steamaudio/LICENSE.md DESTINATION data/steamaudio/licenses RENAME SteamAudio-APACHE-2.0.txt)
set(SA_BUNDLE_RUNTIME_DIR "" CACHE PATH "Steam Audio runtime directory to include in the installation")
if(SA_BUNDLE_RUNTIME_DIR)
    install(FILES "${SA_BUNDLE_RUNTIME_DIR}/phonon.dll" DESTINATION lua/bin)
    foreach(SA_OPTIONAL_RUNTIME TrueAudioNext.dll GPUUtilities.dll)
        if(EXISTS "${SA_BUNDLE_RUNTIME_DIR}/${SA_OPTIONAL_RUNTIME}")
            install(FILES "${SA_BUNDLE_RUNTIME_DIR}/${SA_OPTIONAL_RUNTIME}" DESTINATION lua/bin)
        endif()
    endforeach()
    install(FILES third_party/steamaudio/THIRDPARTY.md DESTINATION data/steamaudio/licenses
        RENAME SteamAudio-THIRDPARTY.md)
endif()
install(FILES third_party/minhook/LICENSE.txt DESTINATION data/steamaudio/licenses RENAME MinHook-LICENSE.txt)
install(FILES README.md DESTINATION data/steamaudio)

# ---------------------------------------------------------------------------
# Host-side tests (no Steam Audio runtime required)
# ---------------------------------------------------------------------------
if(SA_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()
