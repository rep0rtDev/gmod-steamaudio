// src/mixing/BassAbi.h
//
// Minimal ABI declarations for the subset of un4seen BASS 2.4 used by the
// BASS bridge. Only the exported C functions and plain structs are declared
// here (function names, calling convention, argument layout and constant
// values are part of the public BASS 2.4 API). bass.h itself is not vendored;
// the functions are resolved at runtime from the bass.dll already loaded by
// the Garry's Mod client.
#pragma once

#include <cstdint>

namespace sa {
namespace bass {

#if defined(_WIN32) && !defined(_WIN64)
#define SA_BASSCALL __stdcall
#else
#define SA_BASSCALL
#endif

using DWORD = uint32_t;
using QWORD = uint64_t;
using BOOL = int32_t;
using HSTREAM = DWORD;
using HDSP = DWORD;
using HPLUGIN = DWORD;
using HSAMPLE = DWORD;

constexpr DWORD kVersion24 = 0x02040000u;

// BASS_SAMPLE_* / BASS_STREAM_* flags
constexpr DWORD kSample8Bits = 1;
constexpr DWORD kSampleMono = 2;
constexpr DWORD kSample3D = 8;
constexpr DWORD kSampleFloat = 256;
constexpr DWORD kStreamDecode = 0x200000;

// BASS_ATTRIB_*
constexpr DWORD kAttribFreq = 1;
constexpr DWORD kAttribVol = 2;
constexpr DWORD kAttribPan = 3;

// BASS_ACTIVE_*
constexpr DWORD kActiveStopped = 0;
constexpr DWORD kActivePlaying = 1;
constexpr DWORD kActiveStalled = 2;
constexpr DWORD kActivePaused = 3;

// BASS_ERROR_*
constexpr int32_t kErrorHandle = 5;

// BASS_3DMODE_*
constexpr int32_t k3DModeNormal = 0;
constexpr int32_t k3DModeRelative = 1;
constexpr int32_t k3DModeOff = 2;

struct Vector3D {
    float x, y, z;
};

struct ChannelInfo {
    DWORD freq;
    DWORD chans;
    DWORD flags;
    DWORD ctype;
    DWORD origres;
    HPLUGIN plugin;
    HSAMPLE sample;
    const char* filename;
};

struct FileProcs {
    void(SA_BASSCALL* close)(void* user);
    QWORD(SA_BASSCALL* length)(void* user);
    DWORD(SA_BASSCALL* read)(void* buffer, DWORD length, void* user);
    BOOL(SA_BASSCALL* seek)(QWORD offset, void* user);
};

using DspProc = void(SA_BASSCALL*)(HDSP handle, DWORD channel, void* buffer, DWORD length, void* user);
using DownloadProc = void(SA_BASSCALL*)(const void* buffer, DWORD length, void* user);
using StreamProc = DWORD(SA_BASSCALL*)(HSTREAM handle, void* buffer, DWORD length, void* user);

// Exported function signatures.
using StreamCreateFileFn = HSTREAM(SA_BASSCALL*)(BOOL mem, const void* file, QWORD offset, QWORD length, DWORD flags);
using StreamCreateURLFn = HSTREAM(SA_BASSCALL*)(const char* url, DWORD offset, DWORD flags, DownloadProc proc,
                                                void* user);
using StreamCreateFileUserFn = HSTREAM(SA_BASSCALL*)(DWORD system, DWORD flags, const FileProcs* procs, void* user);
using StreamCreateFn = HSTREAM(SA_BASSCALL*)(DWORD freq, DWORD chans, DWORD flags, StreamProc proc, void* user);
using StreamFreeFn = BOOL(SA_BASSCALL*)(HSTREAM handle);
using ChannelSetDSPFn = HDSP(SA_BASSCALL*)(DWORD handle, DspProc proc, void* user, int32_t priority);
using ChannelRemoveDSPFn = BOOL(SA_BASSCALL*)(DWORD handle, HDSP dsp);
using ChannelGetInfoFn = BOOL(SA_BASSCALL*)(DWORD handle, ChannelInfo* info);
using ChannelIsActiveFn = DWORD(SA_BASSCALL*)(DWORD handle);
using ChannelPlayFn = BOOL(SA_BASSCALL*)(DWORD handle, BOOL restart);
using ChannelStopFn = BOOL(SA_BASSCALL*)(DWORD handle);
using ChannelSet3DPositionFn = BOOL(SA_BASSCALL*)(DWORD handle, const Vector3D* pos, const Vector3D* orient,
                                                   const Vector3D* vel);
using ChannelSet3DAttributesFn = BOOL(SA_BASSCALL*)(DWORD handle, int32_t mode, float min, float max,
                                                     int32_t iangle, int32_t oangle, float outvol);
using ChannelSetAttributeFn = BOOL(SA_BASSCALL*)(DWORD handle, DWORD attrib, float value);
using ChannelGetAttributeFn = BOOL(SA_BASSCALL*)(DWORD handle, DWORD attrib, float* value);
using ErrorGetCodeFn = int32_t(SA_BASSCALL*)();
using GetVersionFn = DWORD(SA_BASSCALL*)();

} // namespace bass
} // namespace sa
