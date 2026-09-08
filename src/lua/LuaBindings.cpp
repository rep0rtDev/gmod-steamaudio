// src/lua/LuaBindings.cpp
#include "lua/LuaBindings.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/AudioEngine.h"
#include "lua/ConVars.h"
#include "mixing/ProceduralAudioBridge.h"
#include "util/Logging.h"
#include "util/Math.h"

namespace sa {
namespace lua {

namespace {

using namespace GarrysMod::Lua;

AudioEngine* g_engine = nullptr;

const char* StateString(EngineState s)
{
    switch (s) {
    case EngineState::Uninitialized:
        return "uninitialized";
    case EngineState::Inactive:
        return "inactive";
    case EngineState::Fallback:
        return "fallback";
    case EngineState::Active:
        return "active";
    case EngineState::Passthrough:
        return "passthrough";
    }
    return "unknown";
}

Vec3 ToVec3(const Vector& v)
{
    return Vec3{v.x, v.y, v.z};
}

void PushVec3(ILuaBase* LUA, const Vec3& v)
{
    Vector out;
    out.x = v.x;
    out.y = v.y;
    out.z = v.z;
    LUA->PushVector(out);
}

bool OptVector(ILuaBase* LUA, int idx, Vec3& out)
{
    if (LUA->Top() < idx || !LUA->IsType(idx, Type::Vector))
        return false;
    out = ToVec3(LUA->GetVector(idx));
    return true;
}

bool OptAngle(ILuaBase* LUA, int idx, Vec3& out)
{
    if (LUA->Top() < idx || !LUA->IsType(idx, Type::Angle))
        return false;
    out = ToVec3(LUA->GetAngle(idx));
    return true;
}

double OptNumber(ILuaBase* LUA, int idx, double def)
{
    if (LUA->Top() < idx || !LUA->IsType(idx, Type::Number))
        return def;
    return LUA->GetNumber(idx);
}

bool OptBool(ILuaBase* LUA, int idx, bool def)
{
    if (LUA->Top() < idx || LUA->IsType(idx, Type::Nil))
        return def;
    if (LUA->IsType(idx, Type::Number))
        return LUA->GetNumber(idx) != 0.0;
    return LUA->GetBool(idx);
}

std::string OptString(ILuaBase* LUA, int idx, const char* def)
{
    if (LUA->Top() < idx || !LUA->IsType(idx, Type::String))
        return def;
    const char* s = LUA->GetString(idx);
    return s ? s : def;
}

void SetFieldString(ILuaBase* LUA, const char* key, const std::string& value)
{
    LUA->PushString(value.c_str());
    LUA->SetField(-2, key);
}

void SetFieldNumber(ILuaBase* LUA, const char* key, double value)
{
    LUA->PushNumber(value);
    LUA->SetField(-2, key);
}

void SetFieldBool(ILuaBase* LUA, const char* key, bool value)
{
    LUA->PushBool(value);
    LUA->SetField(-2, key);
}

// Reads a source parameter table ({pos=, forward=, gain=, soundlevel=, ...})
// on top of `base`, leaving untouched fields as they are.
void ReadSourceParams(ILuaBase* LUA, int idx, SourceParams& p)
{
    if (!LUA->IsType(idx, Type::Table))
        return;
    LUA->GetField(idx, "pos");
    if (LUA->IsType(-1, Type::Vector)) {
        p.position = ToVec3(LUA->GetVector(-1));
        p.positionValid = 1;
    }
    LUA->Pop();
    LUA->GetField(idx, "forward");
    if (LUA->IsType(-1, Type::Vector))
        p.forward = ToVec3(LUA->GetVector(-1)).Normalized();
    LUA->Pop();
    LUA->GetField(idx, "angles");
    if (LUA->IsType(-1, Type::Angle))
        p.forward = Transform::FromAngles(Vec3{}, ToVec3(LUA->GetAngle(-1))).forward;
    LUA->Pop();
    LUA->GetField(idx, "gain");
    if (LUA->IsType(-1, Type::Number))
        p.gain = Clamp(static_cast<float>(LUA->GetNumber(-1)), 0.f, 4.f);
    LUA->Pop();
    LUA->GetField(idx, "soundlevel");
    if (LUA->IsType(-1, Type::Number))
        p.distMult = SoundLevelToDistMult(static_cast<float>(LUA->GetNumber(-1)));
    LUA->Pop();
    LUA->GetField(idx, "distmult");
    if (LUA->IsType(-1, Type::Number))
        p.distMult = std::max(0.f, static_cast<float>(LUA->GetNumber(-1)));
    LUA->Pop();
    LUA->GetField(idx, "radius");
    if (LUA->IsType(-1, Type::Number))
        p.radiusMeters = Clamp(static_cast<float>(LUA->GetNumber(-1)), 0.f, 10.f);
    LUA->Pop();
    LUA->GetField(idx, "dipole_weight");
    if (LUA->IsType(-1, Type::Number))
        p.dipoleWeight = Clamp(static_cast<float>(LUA->GetNumber(-1)), 0.f, 1.f);
    LUA->Pop();
    LUA->GetField(idx, "dipole_power");
    if (LUA->IsType(-1, Type::Number))
        p.dipolePower = Clamp(static_cast<float>(LUA->GetNumber(-1)), 0.f, 8.f);
    LUA->Pop();
    LUA->GetField(idx, "air_absorption");
    if (LUA->IsType(-1, Type::Number))
        p.airAbsorptionScale = Clamp(static_cast<float>(LUA->GetNumber(-1)), 0.f, 4.f);
    LUA->Pop();
    LUA->GetField(idx, "reverb_gain");
    if (LUA->IsType(-1, Type::Number))
        p.reverbGain = Clamp(static_cast<float>(LUA->GetNumber(-1)), 0.f, 4.f);
    LUA->Pop();
    LUA->GetField(idx, "pitch");
    if (LUA->IsType(-1, Type::Number))
        p.pitch = Clamp(static_cast<float>(LUA->GetNumber(-1)), 0.05f, 20.f);
    LUA->Pop();
    LUA->GetField(idx, "entity");
    if (LUA->IsType(-1, Type::Number))
        p.entityIndex = static_cast<int32_t>(LUA->GetNumber(-1));
    LUA->Pop();
    struct Flag {
        const char* key;
        uint8_t SourceParams::*field;
    };
    const Flag flags[] = {
        {"spatialize", &SourceParams::spatialize},   {"occlusion", &SourceParams::occlusion},
        {"transmission", &SourceParams::transmission}, {"reflections", &SourceParams::reflections},
        {"pathing", &SourceParams::pathing},
    };
    for (const Flag& f : flags) {
        LUA->GetField(idx, f.key);
        if (!LUA->IsType(-1, Type::Nil))
            p.*f.field = OptBool(LUA, LUA->Top(), true) ? 1 : 0;
        LUA->Pop();
    }
}

// ---------------------------------------------------------------------------
// steamaudio.* functions
// ---------------------------------------------------------------------------
LUA_FUNCTION(L_Version)
{
    LUA->PushString(SA_VERSION_STRING);
    LUA->PushNumber(SA_ARCH_BITS);
    return 2;
}

LUA_FUNCTION(L_IsActive)
{
    const bool active = g_engine && (g_engine->State() == EngineState::Active ||
                                     g_engine->State() == EngineState::Fallback);
    LUA->PushBool(active);
    return 1;
}

LUA_FUNCTION(L_GetState)
{
    LUA->PushString(g_engine ? StateString(g_engine->State()) : "uninitialized");
    return 1;
}

LUA_FUNCTION(L_GetStatus)
{
    LUA->CreateTable();
    if (!g_engine) {
        SetFieldString(LUA, "state", "uninitialized");
        return 1;
    }
    const EngineStatus s = g_engine->Status();
    SetFieldString(LUA, "state", StateString(s.state));
    SetFieldString(LUA, "reason", s.stateReason);
    SetFieldString(LUA, "backend", s.backend);
    SetFieldString(LUA, "scene_type", s.sceneType);
    SetFieldString(LUA, "output", s.outputPath);
    SetFieldString(LUA, "output_device", s.outputDevice);
    SetFieldString(LUA, "engine_device", s.audioDeviceClass);
    SetFieldString(LUA, "map", s.mapName);
    SetFieldNumber(LUA, "sample_rate", s.sampleRate);
    SetFieldNumber(LUA, "frame_size", s.frameSize);
    SetFieldBool(LUA, "hrtf", s.hrtf);
    SetFieldBool(LUA, "mixer_hooked", s.mixerHooked);
    SetFieldBool(LUA, "listener_from_engine", s.listenerFromEngine);
    SetFieldBool(LUA, "bass_attached", s.bassAttached);
    SetFieldBool(LUA, "bake_running", s.bakeRunning);
    SetFieldNumber(LUA, "bake_progress", s.bakeProgress);
    SetFieldBool(LUA, "baked_data", s.bakedDataLoaded);
    SetFieldBool(LUA, "geometry_pending", s.geometryPending);
    SetFieldNumber(LUA, "audio_frames", static_cast<double>(s.audioFrames));
    SetFieldNumber(LUA, "underruns", static_cast<double>(s.audioUnderruns));
    SetFieldNumber(LUA, "engine_starves", static_cast<double>(s.engineStarves));
    SetFieldNumber(LUA, "clock_resyncs", static_cast<double>(s.clockResyncs));
    SetFieldNumber(LUA, "clock_rebases", static_cast<double>(s.clockRebases));
    SetFieldNumber(LUA, "non_finite_frames", static_cast<double>(s.nonFiniteFrames));
    SetFieldNumber(LUA, "engine_lead_samples", static_cast<double>(s.engineLeadSamples));
    SetFieldNumber(LUA, "active_sources", s.activeSources);
    SetFieldNumber(LUA, "spatialized_sources", s.spatializedSources);
    SetFieldNumber(LUA, "render_us", s.renderMicros);
    SetFieldNumber(LUA, "render_us_max", s.maxRenderMicros);
    SetFieldNumber(LUA, "peak", s.peak);
    SetFieldNumber(LUA, "room_preset", s.roomPreset);
    SetFieldNumber(LUA, "room_sends", s.roomSends);
    SetFieldNumber(LUA, "player_lowpass_hz", s.playerLowpassHz);
    SetFieldNumber(LUA, "simulation_ticks", static_cast<double>(s.simulationTicks));
    SetFieldNumber(LUA, "simulation_sources", s.simulationSources);
    SetFieldNumber(LUA, "simulation_us", s.simulationMicros);
    SetFieldNumber(LUA, "static_triangles", static_cast<double>(s.staticTriangles));
    SetFieldNumber(LUA, "dynamic_meshes", static_cast<double>(s.dynamicMeshes));
    SetFieldNumber(LUA, "static_props", static_cast<double>(s.staticProps));
    SetFieldNumber(LUA, "static_prop_triangles", static_cast<double>(s.staticPropTriangles));
    SetFieldNumber(LUA, "static_props_missing", static_cast<double>(s.staticPropsMissing));
    SetFieldNumber(LUA, "surfaceprop_entries", static_cast<double>(s.surfacePropEntries));
    SetFieldNumber(LUA, "vmt_lookups", static_cast<double>(s.vmtLookups));
    SetFieldNumber(LUA, "vmt_resolved", static_cast<double>(s.vmtResolved));
    SetFieldNumber(LUA, "bass_channels", s.bassChannels);
    SetFieldNumber(LUA, "procedural_streams", static_cast<double>(s.proceduralStreams));
    SetFieldNumber(LUA, "hook_calls", static_cast<double>(s.hookCalls));
    LUA->CreateTable();
    SetFieldNumber(LUA, "mix_calls", static_cast<double>(s.captureMixCalls));
    SetFieldNumber(LUA, "paint_iterations", static_cast<double>(s.capturePaintIterations));
    SetFieldNumber(LUA, "blocks_44k", static_cast<double>(s.captureBlocks44k));
    SetFieldNumber(LUA, "blocks_22k", static_cast<double>(s.captureBlocks22k));
    SetFieldNumber(LUA, "blocks_11k", static_cast<double>(s.captureBlocks11k));
    SetFieldNumber(LUA, "blocks_partial", static_cast<double>(s.captureBlocksPartial));
    SetFieldNumber(LUA, "duplicate_blocks", static_cast<double>(s.captureDuplicateBlocks));
    SetFieldNumber(LUA, "late_blocks", static_cast<double>(s.captureLateBlocks));
    SetFieldNumber(LUA, "slot_overflow", static_cast<double>(s.captureSlotOverflow));
    LUA->SetField(-2, "capture");
    SetFieldBool(LUA, "audio_priority", s.audioPriorityElevated);
    SetFieldBool(LUA, "restart_pending", g_engine->RestartPending());
    SetFieldBool(LUA, "emitsound_hooked", s.emitSoundHooked);
    SetFieldNumber(LUA, "emitsound_calls", static_cast<double>(s.emitSoundCalls));
    SetFieldNumber(LUA, "names_from_handles", static_cast<double>(s.namesFromHandles));
    SetFieldNumber(LUA, "auto_directivity", static_cast<double>(s.autoDirectivity));
    SetFieldString(LUA, "sound_name_resolver", s.soundNameResolver);
    LUA->CreateTable();
    SetFieldNumber(LUA, "guids", static_cast<double>(s.overrides.guidEntries));
    SetFieldNumber(LUA, "named_guids", static_cast<double>(s.overrides.namedGuids));
    SetFieldNumber(LUA, "entities", static_cast<double>(s.overrides.entityOverrides));
    SetFieldNumber(LUA, "pending", static_cast<double>(s.overrides.pending));
    SetFieldNumber(LUA, "rules", static_cast<double>(s.overrides.rules));
    SetFieldNumber(LUA, "rules_matched", static_cast<double>(s.overrides.rulesMatched));
    SetFieldNumber(LUA, "pending_consumed", static_cast<double>(s.overrides.pendingConsumed));
    SetFieldNumber(LUA, "pending_expired", static_cast<double>(s.overrides.pendingExpired));
    LUA->SetField(-2, "overrides");
    SetFieldString(LUA, "entity_source", s.entitySource);
    SetFieldString(LUA, "entity_list", s.entityListState);
    LUA->CreateTable();
    SetFieldNumber(LUA, "snapshots", static_cast<double>(s.entityList.snapshots));
    SetFieldNumber(LUA, "visited", static_cast<double>(s.entityList.entitiesVisited));
    SetFieldNumber(LUA, "emitted", static_cast<double>(s.entityList.entitiesEmitted));
    SetFieldNumber(LUA, "read_failures", static_cast<double>(s.entityList.readFailures));
    SetFieldNumber(LUA, "highest_index", s.entityList.lastHighestIndex);
    LUA->SetField(-2, "entity_list_stats");
    LUA->CreateTable();
    SetFieldNumber(LUA, "tracked", static_cast<double>(s.occluders.tracked));
    SetFieldNumber(LUA, "props", static_cast<double>(s.occluders.props));
    SetFieldNumber(LUA, "players", static_cast<double>(s.occluders.players));
    SetFieldNumber(LUA, "brush_models", static_cast<double>(s.occluders.brushModels));
    SetFieldNumber(LUA, "model_cache", static_cast<double>(s.occluders.modelCache));
    SetFieldNumber(LUA, "models_missing", static_cast<double>(s.occluders.modelsMissing));
    SetFieldNumber(LUA, "triangles", static_cast<double>(s.occluders.triangles));
    SetFieldNumber(LUA, "skipped_range", static_cast<double>(s.occluders.skippedRange));
    SetFieldNumber(LUA, "skipped_small", static_cast<double>(s.occluders.skippedSmall));
    SetFieldNumber(LUA, "skipped_limit", static_cast<double>(s.occluders.skippedLimit));
    SetFieldNumber(LUA, "skipped_inside", static_cast<double>(s.occluders.skippedInside));
    SetFieldNumber(LUA, "pending_loads", static_cast<double>(s.occluders.pendingLoads));
    SetFieldNumber(LUA, "updates", static_cast<double>(s.occluders.updates));
    SetFieldNumber(LUA, "added", static_cast<double>(s.occluders.added));
    SetFieldNumber(LUA, "removed", static_cast<double>(s.occluders.removed));
    SetFieldNumber(LUA, "transform_updates", static_cast<double>(s.occluders.transformUpdates));
    SetFieldNumber(LUA, "brush_updates", static_cast<double>(s.occluders.brushUpdates));
    LUA->SetField(-2, "occluders");
    return 1;
}

LUA_FUNCTION(L_StatusLines)
{
    LUA->CreateTable();
    if (!g_engine)
        return 1;
    const EngineStatus s = g_engine->Status();
    std::vector<std::string> lines;
    char buf[512];
    std::snprintf(buf, sizeof(buf), "steamaudio %s (%d-bit): %s%s%s", SA_VERSION_STRING, SA_ARCH_BITS,
                  StateString(s.state), s.stateReason.empty() ? "" : " - ", s.stateReason.c_str());
    lines.emplace_back(buf);
    std::snprintf(buf, sizeof(buf), "  backend: %s  scene: %s  hrtf: %s", s.backend.c_str(), s.sceneType.c_str(),
                  s.hrtf ? "on" : "off");
    lines.emplace_back(buf);
    std::snprintf(buf, sizeof(buf), "  output: %s (%s) %d Hz / %d frames  underruns: %llu  engine starves: %llu",
                  s.outputPath.c_str(), s.outputDevice.c_str(), s.sampleRate, s.frameSize,
                  static_cast<unsigned long long>(s.audioUnderruns), static_cast<unsigned long long>(s.engineStarves));
    lines.emplace_back(buf);
    std::snprintf(buf, sizeof(buf), "  clock: lead %.1f ms  resyncs: %llu  rebases: %llu  non-finite frames: %llu",
                  s.sampleRate > 0 ? static_cast<double>(s.engineLeadSamples) * 1000.0 / s.sampleRate : 0.0,
                  static_cast<unsigned long long>(s.clockResyncs), static_cast<unsigned long long>(s.clockRebases),
                  static_cast<unsigned long long>(s.nonFiniteFrames));
    lines.emplace_back(buf);
    std::snprintf(buf, sizeof(buf), "  mixer hooked: %s (%s, %llu calls)  listener: %s  bass: %s (%u channels)",
                  s.mixerHooked ? "yes" : "no", s.audioDeviceClass.c_str(),
                  static_cast<unsigned long long>(s.hookCalls), s.listenerFromEngine ? "engine" : "lua",
                  s.bassAttached ? "attached" : "detached", s.bassChannels);
    lines.emplace_back(buf);
    std::snprintf(buf, sizeof(buf), "  sources: %u active, %u spatialized, %u simulated, %zu procedural",
                  s.activeSources, s.spatializedSources, s.simulationSources, s.proceduralStreams);
    lines.emplace_back(buf);
    std::snprintf(buf, sizeof(buf),
                  "  capture: %llu mix calls / %llu iterations  blocks 44k/22k/11k: %llu/%llu/%llu  partial %llu  "
                  "dup %llu  late %llu  overflow %llu",
                  static_cast<unsigned long long>(s.captureMixCalls),
                  static_cast<unsigned long long>(s.capturePaintIterations),
                  static_cast<unsigned long long>(s.captureBlocks44k), static_cast<unsigned long long>(s.captureBlocks22k),
                  static_cast<unsigned long long>(s.captureBlocks11k),
                  static_cast<unsigned long long>(s.captureBlocksPartial),
                  static_cast<unsigned long long>(s.captureDuplicateBlocks),
                  static_cast<unsigned long long>(s.captureLateBlocks),
                  static_cast<unsigned long long>(s.captureSlotOverflow));
    lines.emplace_back(buf);
    std::snprintf(buf, sizeof(buf),
                  "  audio: %u us (max %u) peak %.2f prio %s  sim: %u us / %llu ticks  runs direct/refl/path: "
                  "%llu/%llu/%llu",
                  s.renderMicros, s.maxRenderMicros, static_cast<double>(s.peak),
                  s.audioPriorityElevated ? "rt" : "normal", s.simulationMicros,
                  static_cast<unsigned long long>(s.simulationTicks),
                  static_cast<unsigned long long>(s.simulationDirectRuns),
                  static_cast<unsigned long long>(s.simulationReflectionRuns),
                  static_cast<unsigned long long>(s.simulationPathingRuns));
    lines.emplace_back(buf);
    std::snprintf(buf, sizeof(buf), "  map: %s  %zu static tris, %zu dynamic meshes  baked: %s%s",
                  s.mapName.empty() ? "(none)" : s.mapName.c_str(), s.staticTriangles, s.dynamicMeshes,
                  s.bakedDataLoaded ? "yes" : "no", s.bakeRunning ? " (baking)" : "");
    lines.emplace_back(buf);
    std::snprintf(buf, sizeof(buf),
                  "  overrides: %zu rules, %zu entities, %zu sounds (%zu named), %zu pending  emitsound hook: %s "
                  "(%llu calls)  names via fs: %s (%llu)",
                  s.overrides.rules, s.overrides.entityOverrides, s.overrides.guidEntries, s.overrides.namedGuids,
                  s.overrides.pending, s.emitSoundHooked ? "yes" : "no",
                  static_cast<unsigned long long>(s.emitSoundCalls), s.soundNameResolver.c_str(),
                  static_cast<unsigned long long>(s.namesFromHandles));
    lines.emplace_back(buf);
    std::snprintf(buf, sizeof(buf),
                  "  occluders: %zu tracked (%zu props, %zu players, %zu tris), %zu brush ents, %zu models cached "
                  "(%zu missing)  skipped: %zu range %zu small %zu limit %zu inside  source: %s",
                  s.occluders.tracked, s.occluders.props, s.occluders.players, s.occluders.triangles,
                  s.occluders.brushModels, s.occluders.modelCache, s.occluders.modelsMissing,
                  s.occluders.skippedRange, s.occluders.skippedSmall, s.occluders.skippedLimit,
                  s.occluders.skippedInside, s.entitySource.c_str());
    lines.emplace_back(buf);
    std::snprintf(buf, sizeof(buf), "  entity list: %s  (%llu snapshots, %llu read failures, highest index %d)",
                  s.entityListState.c_str(), static_cast<unsigned long long>(s.entityList.snapshots),
                  static_cast<unsigned long long>(s.entityList.readFailures), s.entityList.lastHighestIndex);
    lines.emplace_back(buf);
    std::snprintf(buf, sizeof(buf),
                  "  environment: room preset %d (%u sends)  lowpass %.0f Hz  auto directivity: %llu  hull pushes: %llu",
                  s.roomPreset, s.roomSends, static_cast<double>(s.playerLowpassHz),
                  static_cast<unsigned long long>(s.autoDirectivity),
                  static_cast<unsigned long long>(s.emitterHullPushes));
    lines.emplace_back(buf);
    if (g_engine->RestartPending())
        lines.emplace_back("  restart pending: run snd_sa_restart to apply static convars");
    for (size_t i = 0; i < lines.size(); ++i) {
        LUA->PushNumber(static_cast<double>(i + 1));
        LUA->PushString(lines[i].c_str());
        LUA->SetTable(-3);
    }
    return 1;
}

// SoundLines() -> { line... }: listener, active engine sounds and bound
// capture slots with what the renderer did with each (snd_sa_sounds).
LUA_FUNCTION(L_SoundLines)
{
    LUA->CreateTable();
    if (!g_engine)
        return 1;
    const std::vector<std::string> lines = g_engine->DescribeSounds();
    for (size_t i = 0; i < lines.size(); ++i) {
        LUA->PushNumber(static_cast<double>(i + 1));
        LUA->PushString(lines[i].c_str());
        LUA->SetTable(-3);
    }
    return 1;
}

LUA_FUNCTION(L_Tick)
{
    if (g_engine)
        g_engine->Tick(static_cast<float>(OptNumber(LUA, 1, 1.0 / 60.0)));
    return 0;
}

// SetListener(Vector pos, Angle ang) or SetListener(pos, forward, right, up)
LUA_FUNCTION(L_SetListener)
{
    if (!g_engine)
        return 0;
    Vec3 pos;
    if (!OptVector(LUA, 1, pos))
        return 0;
    Vec3 ang;
    if (OptAngle(LUA, 2, ang)) {
        const Transform t = Transform::FromAngles(pos, ang);
        g_engine->SetListener(pos, t.forward, t.right, t.up);
        return 0;
    }
    Vec3 f, r, u;
    if (OptVector(LUA, 2, f) && OptVector(LUA, 3, r) && OptVector(LUA, 4, u))
        g_engine->SetListener(pos, f, r, u);
    return 0;
}

LUA_FUNCTION(L_SetLocalPlayer)
{
    if (g_engine)
        g_engine->SetLocalPlayer(static_cast<int32_t>(OptNumber(LUA, 1, -1)));
    return 0;
}

// SetEnvironment(dsp_room, dsp_player, dsp_water, underwater)
LUA_FUNCTION(L_SetEnvironment)
{
    if (!g_engine)
        return 0;
    EnvironmentState st;
    st.roomPreset = static_cast<int32_t>(OptNumber(LUA, 1, 0.0));
    st.playerPreset = static_cast<int32_t>(OptNumber(LUA, 2, 0.0));
    st.waterPreset = static_cast<int32_t>(OptNumber(LUA, 3, 14.0));
    st.underwater = OptBool(LUA, 4, false) ? 1 : 0;
    st.valid = 1;
    g_engine->SetEnvironment(st);
    return 0;
}

LUA_FUNCTION(L_SetEngineVolume)
{
    if (!g_engine || g_engine->State() == EngineState::Inactive || g_engine->State() == EngineState::Uninitialized)
        return 0;
    RuntimeConfig cfg = g_engine->Runtime();
    cfg.engineVolume = Clamp(static_cast<float>(OptNumber(LUA, 1, 1.0)), 0.f, 1.f);
    cfg.voiceScale = Clamp(static_cast<float>(OptNumber(LUA, 2, 1.0)), 0.f, 2.f);
    g_engine->SetRuntimeConfig(cfg);
    return 0;
}

LUA_FUNCTION(L_LoadMap)
{
    if (!g_engine) {
        LUA->PushBool(false);
        LUA->PushString("engine not initialized");
        return 2;
    }
    std::string error;
    const bool ok = g_engine->LoadMap(LUA->CheckString(1), error);
    LUA->PushBool(ok);
    if (!ok)
        LUA->PushString(error.c_str());
    else
        LUA->PushNil();
    return 2;
}

LUA_FUNCTION(L_LoadMapData)
{
    if (!g_engine) {
        LUA->PushBool(false);
        LUA->PushString("engine not initialized");
        return 2;
    }
    const char* name = LUA->CheckString(1);
    LUA->CheckType(2, Type::String);
    unsigned int len = 0;
    const char* data = LUA->GetString(2, &len);
    std::string error;
    const bool ok = g_engine->LoadMapFromMemory(name, reinterpret_cast<const uint8_t*>(data), len, error);
    LUA->PushBool(ok);
    if (!ok)
        LUA->PushString(error.c_str());
    else
        LUA->PushNil();
    return 2;
}

// steamaudio.ReadGameFile(path) -> data|nil, err|source
// Resolves like the game does (mounted Workshop content included).
LUA_FUNCTION(L_ReadGameFile)
{
    if (!g_engine) {
        LUA->PushNil();
        LUA->PushString("engine not initialized");
        return 2;
    }
    std::vector<uint8_t> bytes;
    std::string error;
    std::string source;
    if (!g_engine->ReadGameFile(LUA->CheckString(1), bytes, error, &source)) {
        LUA->PushNil();
        LUA->PushString(error.c_str());
        return 2;
    }
    if (bytes.empty())
        LUA->PushString("");
    else
        LUA->PushString(reinterpret_cast<const char*>(bytes.data()), static_cast<unsigned int>(bytes.size()));
    LUA->PushString(source.c_str());
    return 2;
}

LUA_FUNCTION(L_GameFileExists)
{
    LUA->PushBool(g_engine && g_engine->GameFileExists(LUA->CheckString(1)));
    return 1;
}

// steamaudio.GetFileSystemInfo() -> { available = bool, status = string }
LUA_FUNCTION(L_GetFileSystemInfo)
{
    LUA->CreateTable();
    SetFieldBool(LUA, "available", g_engine && g_engine->FileSystem().Available());
    SetFieldString(LUA, "status", g_engine ? g_engine->FileSystemStatus() : "engine not initialized");
    return 1;
}

// steamaudio.GetSurfaceProp("concrete/concretefloor001a") ->
//   { texture, vmt, surfaceprop, material, source = "vmt"|"heuristic", base, gamematerial }
LUA_FUNCTION(L_GetSurfaceProp)
{
    const char* texture = LUA->CheckString(1);
    if (!g_engine) {
        LUA->PushNil();
        return 1;
    }
    const AudioEngine::SurfacePropInfo info = g_engine->DescribeTexture(texture);
    LUA->CreateTable();
    SetFieldString(LUA, "texture", info.texture);
    SetFieldString(LUA, "vmt", info.vmtPath);
    SetFieldString(LUA, "surfaceprop", info.surfaceProp);
    SetFieldString(LUA, "material", info.material);
    SetFieldString(LUA, "source", info.source);
    SetFieldString(LUA, "base", info.base);
    SetFieldString(LUA, "gamematerial", info.gameMaterial ? std::string(1, info.gameMaterial) : std::string());
    return 1;
}

LUA_FUNCTION(L_UnloadMap)
{
    if (g_engine)
        g_engine->UnloadMap();
    return 0;
}

LUA_FUNCTION(L_UpdateBrushModel)
{
    bool ok = false;
    Vec3 pos, ang;
    if (g_engine && OptVector(LUA, 2, pos) && OptAngle(LUA, 3, ang))
        ok = g_engine->UpdateBrushModel(static_cast<int32_t>(LUA->CheckNumber(1)), pos, ang);
    LUA->PushBool(ok);
    return 1;
}

// Lua entity walk (fallback while the native IClientEntityList walk is not
// validated): BeginEntities(); PushEntity(...) per entity; EndEntities().
LUA_FUNCTION(L_BeginEntities)
{
    if (g_engine)
        g_engine->BeginLuaEntities();
    return 0;
}

// PushEntity(index, model, Vector pos, Angle ang, Vector mins, Vector maxs, solid, isPlayer, dormant)
LUA_FUNCTION(L_PushEntity)
{
    if (!g_engine)
        return 0;
    EntitySnapshot e;
    e.index = static_cast<int32_t>(LUA->CheckNumber(1));
    e.model = OptString(LUA, 2, "");
    OptVector(LUA, 3, e.origin);
    OptAngle(LUA, 4, e.angles);
    OptVector(LUA, 5, e.mins);
    OptVector(LUA, 6, e.maxs);
    e.solid = static_cast<int32_t>(OptNumber(LUA, 7, kSolidVPhysics));
    e.player = OptBool(LUA, 8, false);
    e.dormant = OptBool(LUA, 9, false);
    g_engine->PushLuaEntity(e);
    return 0;
}

LUA_FUNCTION(L_EndEntities)
{
    if (g_engine)
        g_engine->EndLuaEntities();
    return 0;
}

// EntitySource() -> "native" | "lua" | "off"
LUA_FUNCTION(L_EntitySource)
{
    if (!g_engine) {
        LUA->PushString("off");
        return 1;
    }
    const RuntimeConfig& rt = g_engine->Runtime();
    if (!rt.dynamicGeometry && !rt.dynamicProps && !rt.dynamicPlayers)
        LUA->PushString("off");
    else
        LUA->PushString(g_engine->NativeEntitiesActive() ? "native" : "lua");
    return 1;
}

// GetOccluders() -> { {index=, model=, id=, player=, collision=, triangles=, pos=Vector}, ... }
LUA_FUNCTION(L_GetOccluders)
{
    LUA->CreateTable();
    if (!g_engine)
        return 1;
    const std::vector<DynamicOccluders::TrackedInfo> tracked = g_engine->Occluders().Tracked();
    double n = 0;
    for (const DynamicOccluders::TrackedInfo& t : tracked) {
        LUA->PushNumber(++n);
        LUA->CreateTable();
        SetFieldNumber(LUA, "index", t.index);
        SetFieldString(LUA, "model", t.model);
        SetFieldNumber(LUA, "id", t.id);
        SetFieldBool(LUA, "player", t.player);
        SetFieldBool(LUA, "collision", t.fromCollision);
        SetFieldNumber(LUA, "triangles", static_cast<double>(t.triangles));
        PushVec3(LUA, t.origin);
        LUA->SetField(-2, "pos");
        LUA->SetTable(-3);
    }
    return 1;
}

// AddDynamicMesh({Vector...} triangles, string material, Vector pos, Angle ang, string name) -> id or nil
LUA_FUNCTION(L_AddDynamicMesh)
{
    if (!g_engine) {
        LUA->PushNil();
        return 1;
    }
    LUA->CheckType(1, Type::Table);
    std::vector<Vec3> tris;
    const int count = LUA->ObjLen(1);
    tris.reserve(static_cast<size_t>(std::max(count, 0)));
    for (int i = 1; i <= count; ++i) {
        LUA->PushNumber(i);
        LUA->GetTable(1);
        if (LUA->IsType(-1, Type::Vector))
            tris.push_back(ToVec3(LUA->GetVector(-1)));
        LUA->Pop();
    }
    const std::string material = OptString(LUA, 2, "default");
    Vec3 pos, ang;
    OptVector(LUA, 3, pos);
    OptAngle(LUA, 4, ang);
    const std::string name = OptString(LUA, 5, "lua_mesh");
    const DynamicGeometryId id = g_engine->AddDynamicMesh(tris, material, pos, ang, name);
    if (id == SceneBuilder::kInvalidDynamic)
        LUA->PushNil();
    else
        LUA->PushNumber(id);
    return 1;
}

LUA_FUNCTION(L_UpdateDynamicMesh)
{
    bool ok = false;
    Vec3 pos, ang;
    if (g_engine && OptVector(LUA, 2, pos) && OptAngle(LUA, 3, ang))
        ok = g_engine->UpdateDynamicMesh(static_cast<DynamicGeometryId>(LUA->CheckNumber(1)), pos, ang);
    LUA->PushBool(ok);
    return 1;
}

LUA_FUNCTION(L_RemoveDynamicMesh)
{
    LUA->PushBool(g_engine && g_engine->RemoveDynamicMesh(static_cast<DynamicGeometryId>(LUA->CheckNumber(1))));
    return 1;
}

LUA_FUNCTION(L_Bake)
{
    LUA->PushBool(g_engine && g_engine->RequestBake(OptBool(LUA, 1, false)));
    return 1;
}

LUA_FUNCTION(L_CancelBake)
{
    LUA->PushBool(g_engine && g_engine->CancelBake());
    return 1;
}

LUA_FUNCTION(L_BakeStatus)
{
    const BakeStatus status = g_engine ? g_engine->BakeState() : BakeStatus{};
    LUA->CreateTable();
    SetFieldNumber(LUA, "id", static_cast<double>(status.id));
    SetFieldString(LUA, "phase", BakePhaseName(status.phase));
    SetFieldString(LUA, "detail", status.detail);
    SetFieldBool(LUA, "active", status.active);
    SetFieldBool(LUA, "cache_saved", status.cacheSaved);
    SetFieldBool(LUA, "pathing_allowed", status.pathingAllowed);
    SetFieldBool(LUA, "memory_limited", status.memoryLimited);
    SetFieldNumber(LUA, "pathing_estimated_mb", static_cast<double>(status.pathingEstimatedBytes) / 1048576.0);
    SetFieldNumber(LUA, "pathing_budget_mb", static_cast<double>(status.pathingBudgetBytes) / 1048576.0);
    SetFieldNumber(LUA, "pathing_growth_mb", static_cast<double>(status.pathingGrowthBytes) / 1048576.0);
    SetFieldNumber(LUA, "progress", status.progress);
    SetFieldNumber(LUA, "probes", status.probes);
    SetFieldNumber(LUA, "completed", status.completedProbes);
    SetFieldNumber(LUA, "pass", status.pass);
    SetFieldNumber(LUA, "elapsed", status.elapsedSeconds);
    SetFieldNumber(LUA, "remaining", status.remainingSeconds);
    SetFieldString(LUA, "backend", status.sceneType == IPL_SCENETYPE_EMBREE ? "Embree" :
                                  status.sceneType == IPL_SCENETYPE_RADEONRAYS ? "Radeon Rays" : "Default");
    return 1;
}

LUA_FUNCTION(L_ContinueBakePathing)
{
    LUA->PushBool(g_engine && g_engine->ContinueBakePathing(LUA->GetBool(1)));
    return 1;
}

// CreateStream(sampleRate, channels, bufferSeconds, name, params) -> id or nil
LUA_FUNCTION(L_CreateStream)
{
    if (!g_engine) {
        LUA->PushNil();
        return 1;
    }
    const uint32_t rate = static_cast<uint32_t>(Clamp(OptNumber(LUA, 1, 44100.0), 8000.0, 192000.0));
    const uint32_t channels = static_cast<uint32_t>(Clamp(OptNumber(LUA, 2, 1.0), 1.0, 2.0));
    const float seconds = static_cast<float>(Clamp(OptNumber(LUA, 3, 0.5), 0.1, 10.0));
    const std::string name = OptString(LUA, 4, "lua_stream");
    SourceParams params;
    params.spatialize = 0; // 2D until the caller supplies a position
    params.distMult = SoundLevelToDistMult(75.f);
    if (LUA->Top() >= 5) {
        ReadSourceParams(LUA, 5, params);
        if (params.positionValid)
            params.spatialize = 1;
    }
    const uint32_t id = g_engine->Procedural().CreateStream(rate, channels, seconds, name, params);
    if (id == 0)
        LUA->PushNil();
    else
        LUA->PushNumber(id);
    return 1;
}

// WriteStream(id, string int16le | table floats) -> frames written
LUA_FUNCTION(L_WriteStream)
{
    if (!g_engine) {
        LUA->PushNumber(0);
        return 1;
    }
    const uint32_t id = static_cast<uint32_t>(LUA->CheckNumber(1));
    ProceduralAudioBridge& bridge = g_engine->Procedural();
    const SoundSourcePtr source = bridge.Find(id);
    if (!source) {
        LUA->PushNumber(0);
        return 1;
    }
    const uint32_t channels = std::max<uint32_t>(1, source->InputChannels());
    size_t written = 0;
    if (LUA->IsType(2, Type::String)) {
        unsigned int len = 0;
        const char* data = LUA->GetString(2, &len);
        const size_t samples = len / sizeof(int16_t);
        const size_t frames = samples / channels;
        if (frames > 0) {
            std::vector<int16_t> pcm(frames * channels);
            std::memcpy(pcm.data(), data, frames * channels * sizeof(int16_t));
            written = bridge.WriteInt16(id, pcm.data(), frames);
        }
    } else if (LUA->IsType(2, Type::Table)) {
        const int count = LUA->ObjLen(2);
        const size_t frames = static_cast<size_t>(std::max(count, 0)) / channels;
        if (frames > 0) {
            std::vector<float> pcm(frames * channels);
            for (size_t i = 0; i < pcm.size(); ++i) {
                LUA->PushNumber(static_cast<double>(i + 1));
                LUA->GetTable(2);
                pcm[i] = LUA->IsType(-1, Type::Number) ? static_cast<float>(LUA->GetNumber(-1)) : 0.f;
                LUA->Pop();
            }
            written = bridge.WriteFloat(id, pcm.data(), frames);
        }
    }
    LUA->PushNumber(static_cast<double>(written));
    return 1;
}

LUA_FUNCTION(L_StreamFramesNeeded)
{
    LUA->PushNumber(g_engine ? static_cast<double>(g_engine->Procedural().FramesNeeded(
                                   static_cast<uint32_t>(LUA->CheckNumber(1))))
                             : 0.0);
    return 1;
}

LUA_FUNCTION(L_StreamFramesQueued)
{
    LUA->PushNumber(g_engine ? static_cast<double>(g_engine->Procedural().FramesQueued(
                                   static_cast<uint32_t>(LUA->CheckNumber(1))))
                             : 0.0);
    return 1;
}

LUA_FUNCTION(L_SetStreamParams)
{
    bool ok = false;
    if (g_engine) {
        const uint32_t id = static_cast<uint32_t>(LUA->CheckNumber(1));
        const SoundSourcePtr source = g_engine->Procedural().Find(id);
        if (source) {
            SourceParams params = source->GetParams();
            ReadSourceParams(LUA, 2, params);
            ok = g_engine->Procedural().SetParams(id, params);
        }
    }
    LUA->PushBool(ok);
    return 1;
}

LUA_FUNCTION(L_FinishStream)
{
    LUA->PushBool(g_engine && g_engine->Procedural().Finish(static_cast<uint32_t>(LUA->CheckNumber(1))));
    return 1;
}

LUA_FUNCTION(L_DestroyStream)
{
    LUA->PushBool(g_engine && g_engine->Procedural().Destroy(static_cast<uint32_t>(LUA->CheckNumber(1))));
    return 1;
}

// SetBassChannelParams(bassHandle, params) — overrides for IGModAudioChannel sources.
LUA_FUNCTION(L_SetBassChannelParams)
{
    bool ok = false;
    if (g_engine) {
        const uint32_t handle = static_cast<uint32_t>(LUA->CheckNumber(1));
        const SoundSourcePtr source = g_engine->Bass().FindSource(handle);
        if (source) {
            SourceParams params = source->GetParams();
            ReadSourceParams(LUA, 2, params);
            ok = g_engine->Bass().SetSourceOverrides(handle, params);
        }
    }
    LUA->PushBool(ok);
    return 1;
}

// ---------------------------------------------------------------------------
// Engine sound overrides (C22/E23)
// ---------------------------------------------------------------------------

// Reads an override table: only keys that are present are marked in `fields`.
// Accepts the same keys as ReadSourceParams (minus entity/engine gains).
SoundOverride ReadSoundOverride(ILuaBase* LUA, int idx)
{
    SoundOverride ov;
    if (!LUA->IsType(idx, Type::Table))
        return ov;

    struct NumberKey {
        const char* key;
        SoundOverride::Field field;
        float SoundOverride::*member;
        float lo, hi;
    };
    const NumberKey numbers[] = {
        {"gain", SoundOverride::kGain, &SoundOverride::gain, 0.f, 4.f},
        {"volume", SoundOverride::kGain, &SoundOverride::gain, 0.f, 4.f},
        {"distmult", SoundOverride::kDistMult, &SoundOverride::distMult, 0.f, 1e6f},
        {"radius", SoundOverride::kRadius, &SoundOverride::radiusMeters, 0.f, 10.f},
        {"dipole_weight", SoundOverride::kDipoleWeight, &SoundOverride::dipoleWeight, 0.f, 1.f},
        {"dipole_power", SoundOverride::kDipolePower, &SoundOverride::dipolePower, 0.f, 8.f},
        {"air_absorption", SoundOverride::kAirAbsorption, &SoundOverride::airAbsorptionScale, 0.f, 4.f},
        {"reverb_gain", SoundOverride::kReverbGain, &SoundOverride::reverbGain, 0.f, 4.f},
    };
    for (const NumberKey& k : numbers) {
        LUA->GetField(idx, k.key);
        if (LUA->IsType(-1, Type::Number)) {
            ov.*k.member = Clamp(static_cast<float>(LUA->GetNumber(-1)), k.lo, k.hi);
            ov.fields |= k.field;
        }
        LUA->Pop();
    }

    LUA->GetField(idx, "soundlevel");
    if (LUA->IsType(-1, Type::Number)) {
        ov.distMult = SoundLevelToDistMult(static_cast<float>(LUA->GetNumber(-1)));
        ov.fields |= SoundOverride::kDistMult;
    }
    LUA->Pop();

    LUA->GetField(idx, "pos");
    if (LUA->IsType(-1, Type::Vector)) {
        ov.position = ToVec3(LUA->GetVector(-1));
        ov.fields |= SoundOverride::kPosition;
    }
    LUA->Pop();

    LUA->GetField(idx, "forward");
    if (LUA->IsType(-1, Type::Vector)) {
        const Vec3 f = ToVec3(LUA->GetVector(-1));
        if (f.LengthSq() > 1e-8f) {
            ov.forward = f.Normalized();
            ov.fields |= SoundOverride::kForward;
        }
    }
    LUA->Pop();

    LUA->GetField(idx, "angles");
    if (LUA->IsType(-1, Type::Angle)) {
        ov.forward = Transform::FromAngles(Vec3{}, ToVec3(LUA->GetAngle(-1))).forward;
        ov.fields |= SoundOverride::kForward;
    }
    LUA->Pop();

    struct Flag {
        const char* key;
        SoundOverride::Field field;
        uint8_t SoundOverride::*member;
    };
    const Flag flags[] = {
        {"spatialize", SoundOverride::kSpatialize, &SoundOverride::spatialize},
        {"occlusion", SoundOverride::kOcclusion, &SoundOverride::occlusion},
        {"transmission", SoundOverride::kTransmission, &SoundOverride::transmission},
        {"reflections", SoundOverride::kReflections, &SoundOverride::reflections},
        {"pathing", SoundOverride::kPathing, &SoundOverride::pathing},
    };
    for (const Flag& f : flags) {
        LUA->GetField(idx, f.key);
        if (!LUA->IsType(-1, Type::Nil)) {
            ov.*f.member = OptBool(LUA, LUA->Top(), true) ? 1 : 0;
            ov.fields |= f.field;
        }
        LUA->Pop();
    }
    return ov;
}

void PushSoundOverride(ILuaBase* LUA, const SoundOverride& ov)
{
    LUA->CreateTable();
    if (ov.Has(SoundOverride::kGain))
        SetFieldNumber(LUA, "gain", ov.gain);
    if (ov.Has(SoundOverride::kDistMult))
        SetFieldNumber(LUA, "distmult", ov.distMult);
    if (ov.Has(SoundOverride::kRadius))
        SetFieldNumber(LUA, "radius", ov.radiusMeters);
    if (ov.Has(SoundOverride::kDipoleWeight))
        SetFieldNumber(LUA, "dipole_weight", ov.dipoleWeight);
    if (ov.Has(SoundOverride::kDipolePower))
        SetFieldNumber(LUA, "dipole_power", ov.dipolePower);
    if (ov.Has(SoundOverride::kAirAbsorption))
        SetFieldNumber(LUA, "air_absorption", ov.airAbsorptionScale);
    if (ov.Has(SoundOverride::kReverbGain))
        SetFieldNumber(LUA, "reverb_gain", ov.reverbGain);
    if (ov.Has(SoundOverride::kPosition)) {
        PushVec3(LUA, ov.position);
        LUA->SetField(-2, "pos");
    }
    if (ov.Has(SoundOverride::kForward)) {
        PushVec3(LUA, ov.forward);
        LUA->SetField(-2, "forward");
    }
    if (ov.Has(SoundOverride::kSpatialize))
        SetFieldBool(LUA, "spatialize", ov.spatialize != 0);
    if (ov.Has(SoundOverride::kOcclusion))
        SetFieldBool(LUA, "occlusion", ov.occlusion != 0);
    if (ov.Has(SoundOverride::kTransmission))
        SetFieldBool(LUA, "transmission", ov.transmission != 0);
    if (ov.Has(SoundOverride::kReflections))
        SetFieldBool(LUA, "reflections", ov.reflections != 0);
    if (ov.Has(SoundOverride::kPathing))
        SetFieldBool(LUA, "pathing", ov.pathing != 0);
}

// steamaudio.SetSoundParams(guid, {params}) -> bool
LUA_FUNCTION(L_SetSoundParams)
{
    const int32_t guid = static_cast<int32_t>(LUA->CheckNumber(1));
    LUA->CheckType(2, Type::Table);
    if (!g_engine || guid == 0) {
        LUA->PushBool(false);
        return 1;
    }
    const SoundOverride ov = ReadSoundOverride(LUA, 2);
    if (ov.Empty()) {
        LUA->PushBool(false);
        return 1;
    }
    g_engine->SoundOverrides().SetForGuid(guid, ov);
    LUA->PushBool(true);
    return 1;
}

// steamaudio.ClearSoundParams(guid) -> bool
LUA_FUNCTION(L_ClearSoundParams)
{
    const int32_t guid = static_cast<int32_t>(LUA->CheckNumber(1));
    LUA->PushBool(g_engine && g_engine->SoundOverrides().ClearGuid(guid));
    return 1;
}

// steamaudio.GetSoundParams(guid) -> table|nil
LUA_FUNCTION(L_GetSoundParams)
{
    const int32_t guid = static_cast<int32_t>(LUA->CheckNumber(1));
    SoundOverride ov;
    if (!g_engine || !g_engine->SoundOverrides().OverrideForGuid(guid, ov)) {
        LUA->PushNil();
        return 1;
    }
    PushSoundOverride(LUA, ov);
    return 1;
}

// steamaudio.SetEntitySoundParams(entIndex, {params}|nil) -> bool
LUA_FUNCTION(L_SetEntitySoundParams)
{
    const int32_t entity = static_cast<int32_t>(LUA->CheckNumber(1));
    if (!g_engine) {
        LUA->PushBool(false);
        return 1;
    }
    if (LUA->Top() < 2 || LUA->IsType(2, Type::Nil)) {
        LUA->PushBool(g_engine->SoundOverrides().ClearEntity(entity));
        return 1;
    }
    LUA->CheckType(2, Type::Table);
    const SoundOverride ov = ReadSoundOverride(LUA, 2);
    if (ov.Empty()) {
        LUA->PushBool(false);
        return 1;
    }
    g_engine->SoundOverrides().SetForEntity(entity, ov);
    LUA->PushBool(true);
    return 1;
}

// steamaudio.ClearEntitySoundParams(entIndex) -> bool
LUA_FUNCTION(L_ClearEntitySoundParams)
{
    const int32_t entity = static_cast<int32_t>(LUA->CheckNumber(1));
    LUA->PushBool(g_engine && g_engine->SoundOverrides().ClearEntity(entity));
    return 1;
}

// steamaudio.ExpectSound(entIndex, channel|nil, {params}, ttlSeconds = 1) -> bool
// The next engine sound started by entIndex (on `channel` when given) takes
// the params. Used by the Lua EmitSound wrapper for sounds whose GUID the
// caller cannot observe (server-networked sounds).
LUA_FUNCTION(L_ExpectSound)
{
    const int32_t entity = static_cast<int32_t>(LUA->CheckNumber(1));
    int32_t channel = SoundRule::kAnyChannel;
    if (LUA->Top() >= 2 && LUA->IsType(2, Type::Number))
        channel = static_cast<int32_t>(LUA->GetNumber(2));
    LUA->CheckType(3, Type::Table);
    const float ttl = static_cast<float>(OptNumber(LUA, 4, 1.0));
    if (!g_engine) {
        LUA->PushBool(false);
        return 1;
    }
    const SoundOverride ov = ReadSoundOverride(LUA, 3);
    if (ov.Empty()) {
        LUA->PushBool(false);
        return 1;
    }
    g_engine->SoundOverrides().ExpectSound(entity, channel, ov, ttl);
    LUA->PushBool(true);
    return 1;
}

// steamaudio.AddSoundRule(pattern, {params}, {channel=, priority=}|nil) -> id|nil
LUA_FUNCTION(L_AddSoundRule)
{
    const char* pattern = LUA->CheckString(1);
    LUA->CheckType(2, Type::Table);
    int32_t channel = SoundRule::kAnyChannel;
    int32_t priority = 0;
    if (LUA->Top() >= 3 && LUA->IsType(3, Type::Table)) {
        LUA->GetField(3, "channel");
        if (LUA->IsType(-1, Type::Number))
            channel = static_cast<int32_t>(LUA->GetNumber(-1));
        LUA->Pop();
        LUA->GetField(3, "priority");
        if (LUA->IsType(-1, Type::Number))
            priority = static_cast<int32_t>(LUA->GetNumber(-1));
        LUA->Pop();
    }
    if (!g_engine || !pattern) {
        LUA->PushNil();
        return 1;
    }
    const SoundOverride ov = ReadSoundOverride(LUA, 2);
    const uint32_t id = g_engine->SoundOverrides().AddRule(pattern, channel, priority, ov);
    if (id == 0)
        LUA->PushNil();
    else
        LUA->PushNumber(id);
    return 1;
}

// steamaudio.RemoveSoundRule(id) -> bool
LUA_FUNCTION(L_RemoveSoundRule)
{
    const uint32_t id = static_cast<uint32_t>(LUA->CheckNumber(1));
    LUA->PushBool(g_engine && g_engine->SoundOverrides().RemoveRule(id));
    return 1;
}

// steamaudio.ClearSoundRules()
LUA_FUNCTION(L_ClearSoundRules)
{
    if (g_engine)
        g_engine->SoundOverrides().ClearRules();
    return 0;
}

// steamaudio.GetSoundRules() -> { {id=, pattern=, channel=, priority=, params={}}... }
LUA_FUNCTION(L_GetSoundRules)
{
    LUA->CreateTable();
    if (!g_engine)
        return 1;
    const std::vector<SoundRule> rules = g_engine->SoundOverrides().Rules();
    for (size_t i = 0; i < rules.size(); ++i) {
        const SoundRule& r = rules[i];
        LUA->PushNumber(static_cast<double>(i + 1));
        LUA->CreateTable();
        SetFieldNumber(LUA, "id", r.id);
        SetFieldString(LUA, "pattern", r.pattern);
        if (r.channel != SoundRule::kAnyChannel)
            SetFieldNumber(LUA, "channel", r.channel);
        SetFieldNumber(LUA, "priority", r.priority);
        PushSoundOverride(LUA, r.override);
        LUA->SetField(-2, "params");
        LUA->SetTable(-3);
    }
    return 1;
}

// steamaudio.ClearSoundOverrides()
LUA_FUNCTION(L_ClearSoundOverrides)
{
    if (g_engine)
        g_engine->SoundOverrides().ClearAll();
    return 0;
}

// steamaudio.GetLastSoundGuid() -> guid (0 when unknown)
LUA_FUNCTION(L_GetLastSoundGuid)
{
    LUA->PushNumber(g_engine ? g_engine->LastEmittedSoundGuid() : 0);
    return 1;
}

// steamaudio.IsSoundPlaying(guid) -> bool
LUA_FUNCTION(L_IsSoundPlaying)
{
    const int32_t guid = static_cast<int32_t>(LUA->CheckNumber(1));
    LUA->PushBool(g_engine && g_engine->IsSoundStillPlaying(guid));
    return 1;
}

// steamaudio.GetActiveSounds() -> { {guid=, entity=, channel=, name=, volume=,
//   pitch=, pos=|nil, from_server=, slot=|nil, spatialized=, overridden=}... }
LUA_FUNCTION(L_GetActiveSounds)
{
    LUA->CreateTable();
    if (!g_engine)
        return 1;
    const std::vector<ActiveSoundInfo>& sounds = g_engine->ActiveEngineSounds();
    for (size_t i = 0; i < sounds.size(); ++i) {
        const ActiveSoundInfo& s = sounds[i];
        LUA->PushNumber(static_cast<double>(i + 1));
        LUA->CreateTable();
        SetFieldNumber(LUA, "guid", s.guid);
        SetFieldNumber(LUA, "entity", s.entity);
        SetFieldNumber(LUA, "channel", s.channel);
        SetFieldString(LUA, "name", s.name);
        SetFieldNumber(LUA, "volume", s.volume);
        SetFieldNumber(LUA, "pitch", s.pitch);
        if (s.hasOrigin) {
            PushVec3(LUA, s.origin);
            LUA->SetField(-2, "pos");
        }
        SetFieldBool(LUA, "from_server", s.fromServer);
        SetFieldBool(LUA, "dry_mix", s.dryMix);
        SetFieldBool(LUA, "sentence", s.sentence);
        if (s.slot >= 0) {
            SetFieldNumber(LUA, "slot", s.slot);
            SetFieldBool(LUA, "spatialized", s.spatialized);
            SetFieldBool(LUA, "overridden", s.overridden);
        }
        LUA->SetTable(-3);
    }
    return 1;
}

LUA_FUNCTION(L_Restart)
{
    if (!g_engine) {
        LUA->PushBool(false);
        LUA->PushString("engine not initialized");
        return 2;
    }
    std::string error;
    const bool ok = g_engine->Restart(error);
    LUA->PushBool(ok);
    if (ok)
        LUA->PushNil();
    else
        LUA->PushString(error.c_str());
    return 2;
}

LUA_FUNCTION(L_DrainLog)
{
    const size_t max = static_cast<size_t>(Clamp(OptNumber(LUA, 1, 32.0), 1.0, 1024.0));
    const std::vector<std::string> lines = log::Drain(max);
    LUA->CreateTable();
    for (size_t i = 0; i < lines.size(); ++i) {
        LUA->PushNumber(static_cast<double>(i + 1));
        LUA->PushString(lines[i].c_str());
        LUA->SetTable(-3);
    }
    return 1;
}

LUA_FUNCTION(L_GetConVarList)
{
    LUA->CreateTable();
    const std::vector<ConVarDef>& defs = ConVarDefinitions();
    for (size_t i = 0; i < defs.size(); ++i) {
        LUA->PushNumber(static_cast<double>(i + 1));
        LUA->CreateTable();
        SetFieldString(LUA, "name", defs[i].name);
        SetFieldString(LUA, "default", defs[i].defaultValue);
        SetFieldString(LUA, "help", defs[i].help);
        SetFieldBool(LUA, "static", defs[i].scope == ConVarScope::Static);
        LUA->SetTable(-3);
    }
    return 1;
}

LUA_FUNCTION(L_GetPaths)
{
    LUA->CreateTable();
    if (g_engine) {
        const EnginePaths& p = g_engine->Paths();
        SetFieldString(LUA, "module", p.moduleDirectory);
        SetFieldString(LUA, "game", p.gameDirectory);
        SetFieldString(LUA, "config", p.configDirectory);
        SetFieldString(LUA, "cache", p.cacheDirectory);
        SetFieldString(LUA, "log", p.logFile);
    }
    return 1;
}

struct Export {
    const char* name;
    CFunc fn;
};

const Export kExports[] = {
    {"Version", L_Version},
    {"IsActive", L_IsActive},
    {"GetState", L_GetState},
    {"GetStatus", L_GetStatus},
    {"StatusLines", L_StatusLines},
    {"SoundLines", L_SoundLines},
    {"Tick", L_Tick},
    {"SetListener", L_SetListener},
    {"SetLocalPlayer", L_SetLocalPlayer},
    {"SetEnvironment", L_SetEnvironment},
    {"SetEngineVolume", L_SetEngineVolume},
    {"LoadMap", L_LoadMap},
    {"LoadMapData", L_LoadMapData},
    {"ReadGameFile", L_ReadGameFile},
    {"GameFileExists", L_GameFileExists},
    {"GetFileSystemInfo", L_GetFileSystemInfo},
    {"GetSurfaceProp", L_GetSurfaceProp},
    {"UnloadMap", L_UnloadMap},
    {"UpdateBrushModel", L_UpdateBrushModel},
    {"BeginEntities", L_BeginEntities},
    {"PushEntity", L_PushEntity},
    {"EndEntities", L_EndEntities},
    {"EntitySource", L_EntitySource},
    {"GetOccluders", L_GetOccluders},
    {"AddDynamicMesh", L_AddDynamicMesh},
    {"UpdateDynamicMesh", L_UpdateDynamicMesh},
    {"RemoveDynamicMesh", L_RemoveDynamicMesh},
    {"Bake", L_Bake},
    {"CancelBake", L_CancelBake},
    {"GetBakeStatus", L_BakeStatus},
    {"ContinueBakePathing", L_ContinueBakePathing},
    {"CreateStream", L_CreateStream},
    {"WriteStream", L_WriteStream},
    {"StreamFramesNeeded", L_StreamFramesNeeded},
    {"StreamFramesQueued", L_StreamFramesQueued},
    {"SetStreamParams", L_SetStreamParams},
    {"FinishStream", L_FinishStream},
    {"DestroyStream", L_DestroyStream},
    {"SetBassChannelParams", L_SetBassChannelParams},
    {"SetSoundParams", L_SetSoundParams},
    {"ClearSoundParams", L_ClearSoundParams},
    {"GetSoundParams", L_GetSoundParams},
    {"SetEntitySoundParams", L_SetEntitySoundParams},
    {"ClearEntitySoundParams", L_ClearEntitySoundParams},
    {"ExpectSound", L_ExpectSound},
    {"AddSoundRule", L_AddSoundRule},
    {"RemoveSoundRule", L_RemoveSoundRule},
    {"ClearSoundRules", L_ClearSoundRules},
    {"GetSoundRules", L_GetSoundRules},
    {"ClearSoundOverrides", L_ClearSoundOverrides},
    {"GetLastSoundGuid", L_GetLastSoundGuid},
    {"IsSoundPlaying", L_IsSoundPlaying},
    {"GetActiveSounds", L_GetActiveSounds},
    {"Restart", L_Restart},
    {"DrainLog", L_DrainLog},
    {"GetConVarList", L_GetConVarList},
    {"GetPaths", L_GetPaths},
};

// Lua-side glue: listener/map/geometry hooks, console commands, log pump.
const char kBootstrap[] = R"LUA(
local sa = steamaudio
if not sa then return end
local HOOK = "steamaudio_module"

local function pump()
    local lines = sa.DrainLog(64)
    for i = 1, #lines do MsgN(lines[i]) end
end

hook.Add("RenderScene", HOOK, function(origin, angles)
    sa.SetListener(origin, angles)
end)

-- Source DSP state -> module-side room/underwater replacement. The engine
-- keeps updating dsp_room from env_sound/soundscapes even though its own DSP
-- output is muted; dsp_player is set by game code (flashbang, damage...).
local cvRoom, cvPlayer, cvWater = GetConVar("dsp_room"), GetConVar("dsp_player"), GetConVar("dsp_water")
local lastRoom, lastPlayer, lastWater, lastUnder = nil, nil, nil, nil
local function pushEnvironment(lp)
    local room = cvRoom and cvRoom:GetInt() or 0
    local player = cvPlayer and cvPlayer:GetInt() or 0
    local water = cvWater and cvWater:GetInt() or 14
    local under = IsValid(lp) and lp:WaterLevel() >= 3 or false
    if room ~= lastRoom or player ~= lastPlayer or water ~= lastWater or under ~= lastUnder then
        lastRoom, lastPlayer, lastWater, lastUnder = room, player, water, under
        sa.SetEnvironment(room, player, water, under)
    end
end

hook.Add("Think", HOOK, function()
    sa.Tick(FrameTime())
    local lp = LocalPlayer()
    if IsValid(lp) then sa.SetLocalPlayer(lp:EntIndex()) end
    pushEnvironment(lp)
    pump()
end)

local function pushVolume()
    local v = GetConVar("volume")
    local vs = GetConVar("voice_scale")
    sa.SetEngineVolume(v and v:GetFloat() or 1, vs and vs:GetFloat() or 1)
end
cvars.AddChangeCallback("volume", pushVolume, HOOK)
cvars.AddChangeCallback("voice_scale", pushVolume, HOOK)
pushVolume()

local function loadMap()
    local map = game.GetMap()
    if not map or map == "" then return end
    local ok, err = sa.LoadMap(map)
    if not ok then
        -- Native lookups (engine filesystem, disk, .gma scan) failed; the
        -- Lua filesystem is the last resort (slow: copies the whole BSP twice).
        local data = file.Read("maps/" .. map .. ".bsp", "GAME")
        if data then ok, err = sa.LoadMapData(map, data) end
    end
    if not ok then MsgN("[steamaudio] map geometry unavailable: " .. tostring(err)) end
    pump()
end
hook.Add("InitPostEntity", HOOK, loadMap)
hook.Add("ShutDown", HOOK, function() sa.UnloadMap() end)
if IsValid(LocalPlayer()) then loadMap() end

-- Dynamic occluders: entity snapshot for the native tracker. Only runs while
-- the native IClientEntityList walk is not validated for this map (the
-- module decides per update; sa.EntitySource() reports "native"/"lua"/"off").
local nextScan = 0
local SOLID_NONE_ = SOLID_NONE or 0
hook.Add("Think", HOOK .. "_geometry", function()
    if not sa.IsActive() then return end
    local now = RealTime()
    if now < nextScan then return end
    local cv = GetConVar("snd_sa_dynamic_interval")
    nextScan = now + math.max(cv and cv:GetFloat() or 100, 16) * 0.001
    if sa.EntitySource() ~= "lua" then return end
    local push = sa.PushEntity
    sa.BeginEntities()
    for _, ent in ipairs(ents.GetAll()) do
        if IsValid(ent) then
            local model = ent:GetModel()
            local solid = ent:GetSolid()
            if model and model ~= "" and (solid ~= SOLID_NONE_ or ent:IsPlayer()) then
                push(ent:EntIndex(), model, ent:GetPos(), ent:GetAngles(), ent:OBBMins(), ent:OBBMaxs(), solid,
                     ent:IsPlayer(), ent:IsDormant())
            end
        end
    end
    sa.EndEntities()
end)

concommand.Add("snd_sa_status", function()
    for _, line in ipairs(sa.StatusLines()) do MsgN(line) end
end, nil, "Print Steam Audio module status")
concommand.Add("snd_sa_sounds", function()
    for _, line in ipairs(sa.SoundLines()) do MsgN(line) end
end, nil, "Print the listener, every active engine sound and how each captured channel is rendered")
concommand.Add("snd_sa_restart", function()
    local ok, err = sa.Restart()
    if not ok then MsgN("[steamaudio] restart failed: " .. tostring(err)) end
    if sa.GetStatus().map == "" then loadMap() end
    pump()
end, nil, "Re-initialize Steam Audio (applies static snd_sa_* convars)")
concommand.Add("snd_sa_bake", function() sa.Bake(true) end, nil, "Bake reverb/pathing probes for the current map")
concommand.Add("snd_sa_bake_cancel", function() sa.CancelBake() end, nil, "Cancel a running bake")
concommand.Add("snd_sa_reload_map", loadMap, nil, "Rebuild the acoustic scene from the current map")
concommand.Add("snd_sa_cvars", function()
    for _, d in ipairs(sa.GetConVarList()) do
        MsgN(string.format("%-34s %-10s %s%s", d.name, d.default, d.help, d.static and " [restart]" or ""))
    end
end, nil, "List Steam Audio convars")
)LUA";

bool RunLua(ILuaBase* LUA, const char* code, const char* chunkName)
{
    const int top = LUA->Top();
    LUA->PushSpecial(SPECIAL_GLOB);
    LUA->GetField(-1, "RunString");
    if (!LUA->IsType(-1, Type::Function)) {
        LUA->Pop(LUA->Top() - top);
        SA_LOGE("[lua] RunString unavailable; %s not executed", chunkName);
        return false;
    }
    LUA->PushString(code);
    LUA->PushString(chunkName);
    LUA->PushBool(false); // return the error string instead of printing it
    if (LUA->PCall(3, 1, 0) != 0) {
        SA_LOGE("[lua] %s: %s", chunkName, LUA->GetString(-1));
        LUA->Pop(LUA->Top() - top);
        return false;
    }
    bool ok = true;
    if (LUA->IsType(-1, Type::String)) {
        SA_LOGE("[lua] %s: %s", chunkName, LUA->GetString(-1));
        ok = false;
    }
    LUA->Pop(LUA->Top() - top);
    return ok;
}

} // namespace

void BindEngine(AudioEngine* engine)
{
    g_engine = engine;
}

AudioEngine* BoundEngine()
{
    return g_engine;
}

void RegisterBindings(ILuaBase* LUA)
{
    LUA->PushSpecial(SPECIAL_GLOB);
    LUA->CreateTable();
    for (const Export& e : kExports) {
        LUA->PushCFunction(e.fn);
        LUA->SetField(-2, e.name);
    }
    LUA->SetField(-2, "steamaudio");
    LUA->Pop();
}

bool RunBootstrap(ILuaBase* LUA)
{
    return RunLua(LUA, kBootstrap, "steamaudio_bootstrap");
}

} // namespace lua
} // namespace sa
