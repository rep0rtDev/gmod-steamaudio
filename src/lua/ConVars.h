// src/lua/ConVars.h
//
// Console variables. A GMod binary module has no direct ICvar access that
// works identically on x86/x64, so the convars are created through the Lua
// API (CreateClientConVar) and their change callbacks (cvars.AddChangeCallback)
// call back into the module. The mapping from convar name to configuration
// field lives here so both the JSON config and the convars share one schema.
#pragma once

#include <string>
#include <vector>

#include "GarrysMod/Lua/Interface.h"
#include "steamaudio/Config.h"

namespace sa {

class AudioEngine;

namespace lua {

enum class ConVarScope {
    Runtime, // applied immediately
    Static,  // stored, applied on snd_sa_restart
};

struct ConVarDef {
    std::string name;
    std::string defaultValue;
    std::string help;
    ConVarScope scope = ConVarScope::Runtime;
    bool hasRange = false;
    float minValue = 0.f;
    float maxValue = 0.f;
};

// All snd_sa_* definitions (defaults derived from the default configs).
const std::vector<ConVarDef>& ConVarDefinitions();

// Applies one value. Returns false for unknown names. `isStatic` reports
// whether the change requires a restart.
bool ApplyConVar(const std::string& name, const std::string& value, RuntimeConfig& runtime, StaticConfig& fixed,
                 bool& isStatic);

// Creates every convar (if missing) and installs the change callbacks.
void RegisterConVars(GarrysMod::Lua::ILuaBase* LUA, const RuntimeConfig& runtime = RuntimeConfig{},
                     const StaticConfig& fixed = StaticConfig{});

// Layers the current value of every user-modified convar on top of the
// JSON configs (used as AudioEngine's ConfigOverrideFn).
void ApplyConVarOverrides(GarrysMod::Lua::ILuaBase* LUA, RuntimeConfig& runtime, StaticConfig& fixed);

// Change-callback entry point: applies to the engine bound in LuaBindings.
int ConVarChangedCallback(lua_State* L);

} // namespace lua
} // namespace sa
