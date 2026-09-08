// src/lua/LuaBindings.h
//
// `steamaudio` Lua table exposed to the client realm. All functions are safe
// to call at any time: when the engine is not initialized they return
// false/nil instead of raising.
#pragma once

#include "GarrysMod/Lua/Interface.h"

namespace sa {

class AudioEngine;

namespace lua {

// The engine instance that bindings/convar callbacks operate on (owned by ModuleEntry).
void BindEngine(AudioEngine* engine);
AudioEngine* BoundEngine();

// Pushes the `steamaudio` table onto the stack and registers it as a global.
void RegisterBindings(GarrysMod::Lua::ILuaBase* LUA);

// Runs the Lua-side bootstrap (hooks, concommands, listener/map glue).
// Returns false and logs when RunString is unavailable or errors.
bool RunBootstrap(GarrysMod::Lua::ILuaBase* LUA);

} // namespace lua
} // namespace sa
