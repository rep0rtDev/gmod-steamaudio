# gmod-steamaudio

**Development branch:** bounded-memory pathing is being integrated. GitHub Actions
artifacts are development snapshots, not a claim that the full worker-based bake
pipeline is ready for normal play. Local tests cover the paged cache primitives;
the custom SDK build, route-equivalence tests, worker integration and full-map
memory measurements still need to be completed. The workflow has not yet been
verified by a successful GitHub Actions run.

Native Garry's Mod **client** binary module (`gmcl_steamaudio_win32.dll` /
`gmcl_steamaudio_win64.dll`) that takes over the Source engine's client audio
pipeline and renders it with [Steam Audio](https://valvesoftware.github.io/steam-audio/) 4.x:

* per-channel PCM is captured from the engine mixer (`IAudioDevice::Mix*` slots)
  after the engine has decoded WAV/MP3/OGG/sentences/voice, and the engine's own
  spatialization, DSP and reverb paths are bypassed;
* `sound.PlayFile` / `sound.PlayURL` (BASS-backed `IGModAudioChannel`s) are
  captured through BASS DSP callbacks and muted at the BASS output;
* Lua can push live PCM (procedural audio) as additional sources;
* every source goes through Steam Audio direct simulation (distance
  attenuation, air absorption, directivity, occlusion, transmission), HRTF
  binaural rendering, ray-traced reflections/reverb (convolution, parametric,
  hybrid or TrueAudio Next) and pathing;
* the acoustic scene is built from the map BSP (world brushes, displacements,
  brush entities as instanced meshes, optional Lua meshes), with probe baking
  and an on-disk bake cache; LZMA-compressed lumps (`bspzip -repack`, common
  on Workshop maps) and the L4D2/CS:GO lump header layout are handled;
* static props occlude and reflect: every placement in the `sprp` game lump is
  instanced from its model's VPHY collision hulls (`models/*.phy`, parsed
  natively, `$surfaceprop` and material table included) or, when a prop has no
  usable collision model, from the `.mdl` hull box; `SOLID_NONE` props
  (foliage, decals) are skipped;
* world faces get their acoustic material from the real Source material chain:
  the face's texture is mapped to `materials/<texture>.vmt`, the VMT's
  `$surfaceprop` is read (Patch materials, `insert`/`replace` and included
  VMTs handled), the name is resolved through
  `scripts/surfaceproperties_manifest.txt` + `surfaceproperties*.txt`
  (`base` inheritance, `gamematerial` letter) and finally mapped to the
  acoustic material table; texture-name heuristics remain the fallback;
* game files (map BSP, model `.phy`/`.mdl`, `.vmt` materials and
  `scripts/surfaceproperties*.txt`) are read the way the engine sees them: through the engine `IFileSystem`
  (`VFileSystem022`, mounted Workshop content included), then loose files
  under `garrysmod/` and `garrysmod/download/`, then a built-in `.gma` reader
  that indexes `addons/`, `cache/workshop/` and `cache/` (plain and LZMA
  compressed archives), and finally Lua `file.Read`;
* output either goes back into the engine paint buffer (default, the engine's
  device does the playback) or to a native WASAPI endpoint;
* a dedicated real-time audio thread and a simulation thread talk through
  SPSC queues / timed sample rings; the game thread never blocks on them;
* GPU backends (Radeon Rays / TrueAudio Next, x64 + OpenCL only) are probed at
  start-up and the CPU path (built-in ray tracer or Embree) is used otherwise;
* if Steam Audio cannot be initialized at all, a built-in stereo fallback mixer
  keeps audio alive.

Everything Steam Audio related is loaded dynamically from `phonon.dll` at
run time; the module has no static dependency on `phonon.lib`, `bass.lib` or
the Source SDK.

## Status / what has and has not been verified

* Compiles cleanly (`-Wall -Wextra -Werror`) with GCC on Linux
  (platform-independent parts), MinGW-w64 GCC (x86_64 and i686, full module,
  exports `gmod13_open` / `gmod13_close`). GitHub Actions
  (`.github/workflows/build.yml`) additionally builds MSVC 2022 x86/x64 and
  runs the host-side unit tests (`tests/`, see below) on every push.
* **Not** yet run inside a live Garry's Mod client. Engine-internal addresses
  (`g_AudioDevice`, paint buffer, `channel_t` layout) are resolved at run time through RTTI first and byte
  patterns second; the patterns in `config/steamaudio_signatures.json` are
  empty and must be filled in for the current GMod build (see below). Without
  them the module still loads: if the audio device object cannot be located
  and validated, mixer hooks are skipped and the module runs BASS/procedural
  sources only (state `passthrough`).
* The engine `IFileSystem` vtable layout is likewise unverified on a live
  client: candidate layouts (secondary `IBaseFileSystem` vtable, flattened
  after an `IAppSystem` prefix, with/without MSVC's swapped `Size` overloads)
  are probed against `gameinfo.txt` and only a layout that reads it back is
  used; otherwise the module falls back to disk/`.gma`/Lua. Fixed slots can
  be pinned under `filesystem.slots` in the signatures file.
  `steamaudio.GetFileSystemInfo()` reports which path is in use.
* The VPHY (`.phy`) reader follows the public `phyheader_t` / IVP compact
  surface layout (Source SDK 2013 `phyfile.h`, `ivp_compact_ledge.hxx`) and is
  covered by tests against synthetic files only; it has not been run over the
  stock `hl2`/`garrysmod` model set. MOPP (`modelType 1`) surfaces are skipped
  and fall back to the hull box. `steamaudio.GetStatus()` reports
  `static_props`, `static_prop_triangles` and `static_props_missing`.
* The KeyValues reader (`src/util/KeyValues.cpp`) used for `.vmt` and
  `surfaceproperties*.txt` is bounded (node/depth/token limits, tolerant of
  truncated input) and covered by tests on synthetic files; it has not been
  run over the stock `hl2`/`garrysmod` material set. `steamaudio.GetStatus()`
  reports `surfaceprop_entries`, `vmt_lookups` and `vmt_resolved`;
  `steamaudio.GetSurfaceProp("concrete/concretefloor001a")` shows the whole
  resolution chain for one texture.
* Per-sound overrides (`steamaudio.SetSoundParams` & co., see below) key on
  the GUIDs/entities the engine reports through the public `IEngineSound`
  interface (`GetActiveSounds`, `GetGuidForLastSoundEmitted`); those calls
  are documented SDK API but have not been exercised on a live client here.
  The optional `IEngineSound::EmitSound` VMT hook that records sample names
  assumes the SDK 2013 slot order (slots 4/5) and is validated at install
  time; when it is unavailable, names come from the engine's file-name
  handles only if `filesystem.slots.filename_string` is configured, otherwise
  name rules simply never match and GUID/entity overrides still work.
* Dynamic occluders (props, ragdolls, vehicles, players, moving brush
  entities) are tracked natively (`src/core/DynamicOccluders.cpp`, covered by
  fake-sink tests) from an entity snapshot. The snapshot preferably comes from
  a native walk of `IClientEntityList` (`VClientEntityList003`) plus
  `IVModelInfoClient::GetModelName`; the vtable slots in
  `config/steamaudio_signatures.json` (`entity_list.slots`) are Source SDK
  2013 declaration order and **not** verified against the GMod client. They
  are validated in-map (entity 0 must be the world with model
  `maps/<map>.bsp` and `SOLID_BSP`, the local player's class must end in
  `Player`, slots must point into `client.dll`/`engine.dll`), reads are
  SEH-guarded on MSVC, and any failure switches the module to the Lua entity
  walk (`ents.GetAll()` every `snd_sa_dynamic_interval` ms) for the rest of
  the map. `steamaudio.EntitySource()` / `GetStatus().entity_source` reports
  `native`, `lua` or `off`.
* Source's `dsp_room` / `dsp_player` / `dsp_water` processing is replaced
  module-side (`src/mixing/RoomDsp.cpp`, see *Environment DSP* below). The
  preset table is derived from the public `dsp_presets.txt` semantics (room
  size / decay / wet level per legacy ID and automatic template); it is not a
  sample-exact copy of the engine's DSP kernels. The engine DSP state is read
  from Lua (`dsp_room`, `dsp_player`, `dsp_water`, `WaterLevel()`); on a live
  client whether soundscapes still drive `dsp_room` while the engine mixer is
  detoured is unverified. Doppler (propagation delay) and automatic directivity
  are covered by host-side tests but have not been listened to in-game.

## Layout

```
CMakeLists.txt
config/
  steamaudio.json              default static/runtime configuration (data/steamaudio/)
  steamaudio_signatures.json   engine module names, RTTI class names, vtable slots, byte patterns
  steamaudio_materials.json    $surfaceprop -> acoustic material overrides/aliases
lua/autorun/client/steamaudio_loader.lua   require("steamaudio") on client start
lua/steamaudio/emitsound.lua               steamaudio.EmitSound / PlayWorldSound / Rule wrappers
src/core/        ModuleEntry, AudioEngine (orchestrator), EngineInterfaces (RTTI/pattern
                 resolution), EngineFileSystem (IFileSystem via CreateInterface),
                 EngineHooks (IAudioDevice VMT hooks + IEngineSound),
                 ChannelCapture (channel_t PCM capture), Detour (MinHook RAII), VmtHook,
                 ClientEntityList (guarded IClientEntityList walk), DynamicOccluders
                 (props/players/brush entities -> dynamic scene geometry)
src/steamaudio/  PhononApi (dynamic phonon.dll binding), PhononContext, HRTFRenderer,
                 Simulator, ReflectionSimulator (+ baking), PathingSimulator, SceneBuilder,
                 BspGeometry, PhyModel (VPHY/.mdl readers), StaticPropResolver,
                 SurfaceProps (surfaceproperties.txt database, VMT $surfaceprop),
                 GPUBackend, CPUFallbackBackend, Config
src/mixing/      AudioThread, SimulationThread, SoundSource, LockFreeQueue, TimedSampleRing,
                 ProceduralAudioBridge, BassBridge (+ BassAbi), FallbackMixer, Dsp
src/platform/    PlatformAudioOutput (WASAPI, null)
src/lua/         LuaBindings (steamaudio.* table + Lua glue), ConVars (snd_sa_*)
src/util/        Logging, SignatureScanner, Json, KeyValues (Valve KV1 reader), Math, Lzma,
                 GmaArchive (.gma reader/locator)
third_party/     phonon.h (Steam Audio 4.8.1), gmod-module-base headers, MinHook, LZMA SDK decoder
```

## Building

Requirements: CMake 3.20+, a C++17 compiler. No SDKs need to be installed: the
Steam Audio headers, the GMod module headers and MinHook are vendored.

### Windows, MSVC (Visual Studio 2022)

```bat
:: 64-bit (GMod x86-64 beta branch)
cmake -S . -B build\win64 -G "Visual Studio 17 2022" -A x64
cmake --build build\win64 --config RelWithDebInfo

:: 32-bit (GMod main branch)
cmake -S . -B build\win32 -G "Visual Studio 17 2022" -A Win32
cmake --build build\win32 --config RelWithDebInfo
```

### Cross-compiling with MinGW-w64 (used for CI-style checks)

```sh
cmake -S . -B build/mingw64 -DCMAKE_SYSTEM_NAME=Windows \
      -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc-posix \
      -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++-posix
cmake --build build/mingw64 -j

cmake -S . -B build/mingw32 -DCMAKE_SYSTEM_NAME=Windows \
      -DCMAKE_C_COMPILER=i686-w64-mingw32-gcc-posix \
      -DCMAKE_CXX_COMPILER=i686-w64-mingw32-g++-posix
cmake --build build/mingw32 -j
```

The `-posix` thread model variants are required for `std::thread`/`std::mutex`.

### Linux (compile check only)

```sh
cmake -S . -B build/linux && cmake --build build/linux -j
```

Produces `libsa_core.a` and a `gmcl_steamaudio_linux64.dll` that is only a
compile artifact (Windows is the only supported runtime).

### Tests

`SA_BUILD_TESTS` (default ON) builds `tests/sa_tests`, a dependency-free
suite covering the platform-independent parts: JSON parser, SPSC queue,
`TimedSampleRing`, resampler/smoother, signature scanner, LZMA decoding and
the BSP parser (against synthetic in-memory VBSPs, plain and LZMA-compressed), the VPHY/`.mdl` readers and the
static prop instancer, the `.gma` reader and the engine filesystem slot layouts. Run with `ctest --test-dir build/linux
--output-on-failure` or execute `sa_tests [name-filter]` directly.
Use `-DSA_WARNINGS_AS_ERRORS=ON` to turn warnings into errors (`/WX`, `-Werror`).

## Installing into Garry's Mod

1. Copy `gmcl_steamaudio_win32.dll` (main branch) and/or
   `gmcl_steamaudio_win64.dll` (x86-64 branch) to `garrysmod/lua/bin/`.
2. Copy `phonon.dll` from the Steam Audio SDK release
   (`steamaudio_4.x.x.zip` → `lib/windows-x86/phonon.dll` or
   `lib/windows-x64/phonon.dll`, matching the game architecture) next to the
   module (`garrysmod/lua/bin/`) or into `GarrysMod/bin/` (`bin/win64/` for x64).
   For TrueAudio Next / Radeon Rays also copy `TrueAudioNext.dll`,
   `GPUUtilities.dll` from the same folder (x64 only).
3. Copy `config/*.json` to `garrysmod/data/steamaudio/` (or run `cmake --install`
   with `--prefix <garrysmod>`).
4. Copy `lua/autorun/client/steamaudio_loader.lua` to
   `garrysmod/lua/autorun/client/` and `lua/steamaudio/emitsound.lua` to
   `garrysmod/lua/steamaudio/` (or `require("steamaudio")` from your own
   client code; the wrappers are optional).
5. Start the game, run `snd_sa_status` in the console. The log is written to
   `garrysmod/data/steamaudio/steamaudio.log` and echoed to the console.

The module is inert when loaded from the server realm or a dedicated server
(`steamaudio.GetState()` returns `"inactive"`); all audio work happens in the
client process only.

## Engine signatures

`EngineInterfaces` finds the engine's `IAudioDevice` implementation
(`CAudioDirectSound`, `CAudioXAudio2`, ...) through MSVC RTTI in `engine.dll`
and validates the vtable (slot count, function pointers inside `.text`,
`DeviceDmaSpeed()`/`DeviceChannels()` return plausible values). When RTTI is
stripped or the class was renamed, fill `audio_device.g_AudioDevice` in
`config/steamaudio_signatures.json` with an IDA-style pattern that references
the `g_AudioDevice` global:

```json
"g_AudioDevice": {
  "x86": { "pattern": "8B 0D ? ? ? ? 8B 01 FF 50 04", "offset": 2, "mode": "absolute", "deref": 1 },
  "x64": { "pattern": "48 8B 0D ? ? ? ? 48 8B 01 FF 50 08", "offset": 3, "length": 7, "mode": "relative", "deref": 1 }
}
```

`mode` is `absolute` (32-bit pointer at `offset`), `relative` (rip-relative
displacement at `offset`, instruction length `length`) or `direct` (the match
address itself). `deref` applies that many pointer dereferences. `paint_buffer`
uses the same rule format and is only needed for `snd_sa_output 2` when the
`PaintBegin/PaintEnd` slots cannot be used to locate the buffer. `vtable_slots`
overrides individual `IAudioDevice` slot indices if the layout differs from the
Source SDK 2013 order; `channel_layout` overrides `channel_t` field offsets
(bytes) when inference fails (`-1` = infer at run time).

`disable_emitsound_hook: true` skips the `IEngineSound::EmitSound` VMT hook
(sample names for `AddSoundRule` then rely on `filesystem.slots.filename_string`,
the `IFileSystem::String(FileNameHandle_t, char*, int)` slot; `-1` = off).

## Console variables

All convars are client-side and archived. *Runtime* convars take effect
immediately; *static* ones are read at start-up and after `snd_sa_restart`.

Runtime:

| convar | default | meaning |
| --- | --- | --- |
| `snd_sa_enabled` | 1 | master switch; 0 = engine mixes as usual (passthrough) |
| `snd_sa_hrtf` | 1 | binaural rendering (0 = panning) |
| `snd_sa_hrtf_interpolation` | 1 | 0 nearest, 1 bilinear |
| `snd_sa_reflections` | 1 | ray-traced reflections / reverb |
| `snd_sa_pathing` | 1 | sound propagation around geometry |
| `snd_sa_occlusion` | 1 | direct-path occlusion |
| `snd_sa_transmission` | 1 | through-wall transmission |
| `snd_sa_occlusion_type` | 1 | 0 raycast, 1 volumetric |
| `snd_sa_occlusion_samples` | 16 | volumetric occlusion samples |
| `snd_sa_transmission_rays` | 1 | |
| `snd_sa_rays` | 2048 | real-time reflection rays |
| `snd_sa_bounces` | 8 | |
| `snd_sa_ir_duration` | 1.0 | impulse response length (s) |
| `snd_sa_ambisonic_order` | 1 | |
| `snd_sa_irradiance_min_distance` | 1.0 | |
| `snd_sa_hybrid_transition` / `snd_sa_hybrid_overlap` | 0.5 / 0.25 | hybrid reverb split |
| `snd_sa_reverb_gain` / `snd_sa_direct_gain` / `snd_sa_pathing_gain` | 1.0 | mix levels |
| `snd_sa_master_volume` | 1.0 | |
| `snd_sa_voice_spatial` | 1 | spatialize voice chat |
| `snd_sa_spatialize_stereo` | 1 | downmix + spatialize positional stereo sounds; UI/music still use their non-spatial policy |
| `snd_sa_bass_spatial` | 1 | spatialize 3D `sound.PlayFile`/`PlayURL` channels |
| `snd_sa_units_per_meter` | 52.4934 | Source units per meter |
| `snd_sa_sim_interval_ms` | 100 | direct simulation period |
| `snd_sa_reflections_interval_ms` / `snd_sa_pathing_interval_ms` | 250 | |
| `snd_sa_source_radius` | 0.5 | volumetric occlusion radius (m) |
| `snd_sa_occlusion_full` | 0.6 | visibility at/above which a source is unoccluded (volumetric partial visibility of emitters on/inside geometry is ignored) |
| `snd_sa_occlusion_zero` | 0.1 | visibility at/below which a source is fully occluded |
| `snd_sa_occlusion_min` | 0.3 | low-band gain a fully occluded source still leaks through walls (mid/high bands leak less); 0 = physical transmission only |
| `snd_sa_emitter_hull_margin` | 4 | extra units an emitter is moved out of its own entity's collision bounds toward the listener |
| `snd_sa_air_absorption` | 1.0 | |
| `snd_sa_distance_gain_min` / `snd_sa_distance_gain_max` | 0.01 / 1.0 | |
| `snd_sa_baked_reverb` | 1 | use baked probe data when available |
| `snd_sa_bake_on_map_load` | 1 | bake probes in the background after map load |
| `snd_sa_probe_spacing` / `snd_sa_probe_height` | 4.0 / 1.5 | probe grid (m) |
| `snd_sa_bake_rays` / `snd_sa_bake_bounces` / `snd_sa_bake_duration` | 8192 / 16 / 1.5 | |
| `snd_sa_dynamic_geometry` | 1 | track moving brush entities (doors, elevators, `func_movelinear`) |
| `snd_sa_dynamic_props` | 1 | physics/dynamic props, ragdolls, vehicles occlude sound (`.phy` collision model, hull box fallback) |
| `snd_sa_dynamic_players` | 1 | other players occlude sound (hull box) |
| `snd_sa_native_entities` | 1 | walk the client entity list natively; falls back to the Lua walk when the layout does not validate (`snd_sa_restart`) |
| `snd_sa_dynamic_max` / `snd_sa_dynamic_range` / `snd_sa_dynamic_min_size` | 256 / 3000 / 8 | occluder count limit, listener range and minimum hull side (units); nearest entities win |
| `snd_sa_dynamic_interval` | 100 | occluder update interval (ms) |
| `snd_sa_static_props` | 1 | instance static prop collision models into the scene (next map load) |
| `snd_sa_static_prop_box_fallback` | 1 | hull-box approximation for props without a usable `.phy` |
| `snd_sa_vmt_surfaceprops` | 1 | read `$surfaceprop` from `materials/*.vmt` for world faces (next map load) |
| `snd_sa_surfaceprop_scripts` | 1 | load `scripts/surfaceproperties*.txt` so custom surfaceprops inherit acoustic materials (after `snd_sa_restart`) |
| `snd_sa_pathing_vis_*`, `snd_sa_pathing_range`, `snd_sa_pathing_validation`, `snd_sa_pathing_alternate` | | pathing tuning |
| `snd_sa_room_dsp` | 1 | `dsp_room` replacement: 0 off, 1 only for sources without Steam Audio reflections, 2 always |
| `snd_sa_room_dsp_preset` | -1 | force a room preset (-1 follow `dsp_room`; 1–29 legacy, 100+ automatic templates) |
| `snd_sa_room_dsp_gain` | 1.0 | room reverb wet gain |
| `snd_sa_player_dsp` | 1 | `dsp_player` replacement (muffle/lowpass presets set by game code) |
| `snd_sa_underwater` | 1 | underwater lowpass + gain while the local player is submerged |
| `snd_sa_underwater_cutoff` / `snd_sa_underwater_gain` | 900 / 0.8 | underwater lowpass cutoff (Hz) and gain |
| `snd_sa_doppler` | 0 | optional physical propagation delay / Doppler; disabled by default to avoid adding travel-time latency |
| `snd_sa_doppler_scale` | 1.0 | Doppler strength (1 physical, 0 off) |
| `snd_sa_speed_of_sound` | 343 | m/s, used for propagation delay |
| `snd_sa_auto_directivity` | 1 | infer dipole directivity for weapon / voice channels without explicit overrides |
| `snd_sa_weapon_dipole_weight` / `snd_sa_weapon_dipole_power` | 0.25 / 1.0 | `CHAN_WEAPON` directivity; default has no silent directions |
| `snd_sa_voice_dipole_weight` / `snd_sa_voice_dipole_power` | 0.15 / 1.0 | `CHAN_VOICE`, voice-range channels and sentences |
| `snd_sa_debug` | 0 | |
| `snd_sa_log_level` | 1 | 0 debug, 1 info, 2 warn, 3 error |

Static (restart required):

| convar | default | meaning |
| --- | --- | --- |
| `snd_sa_backend` | 0 | 0 auto (GPU if available), 1 CPU, 2 GPU |
| `snd_sa_scene_type` | 0 | 0 auto, 1 built-in ray tracer, 2 Embree, 3 Radeon Rays |
| `snd_sa_reflection_type` | 2 | 0 convolution, 1 parametric, 2 hybrid, 3 TrueAudio Next |
| `snd_sa_output` | 0 | 0 auto, 1 native WASAPI, 2 engine paint buffer |
| `snd_sa_output_device` | "" | WASAPI endpoint id (empty = default) |
| `snd_sa_latency_ms` | 40 | native output latency |
| `snd_sa_frame_size` | 512 | |
| `snd_sa_max_sources` | 64 | |
| `snd_sa_max_rays`, `snd_sa_max_occlusion_samples`, `snd_sa_max_ir_duration`, `snd_sa_max_ambisonic_order` | 4096 / 32 / 1.5 / 1 | simulator limits |
| `snd_sa_simulation_threads` | 0 | 0 = cores - 2 |
| `snd_sa_sofa` | "" | custom HRTF (SOFA file) |
| `snd_sa_hrtf_volume_db` | 0 | |
| `snd_sa_gpu_compute_units` / `snd_sa_gpu_ir_update_fraction` | 8 / 0.5 | OpenCL reservation |
| `snd_sa_embree` / `snd_sa_tan` | 1 | allow Embree / TrueAudio Next |
| `snd_sa_validation` | 0 | Steam Audio validation layer |

Console commands: `snd_sa_status`, `snd_sa_sounds`, `snd_sa_restart`,
`snd_sa_bake`, `snd_sa_bake_cancel`, `snd_sa_reload_map`, `snd_sa_cvars`.

`snd_sa_sounds` prints the listener, every sound the engine reports active
(guid, entity, origin, listener-relative direction, matched capture slot) and
every captured channel with the parameters the renderer used for it (3D/2D,
position, the engine's own pan direction, render mode `hrtf`/`pan`/`2d`,
direction, distance, attenuation, occlusion). The `master` line shows the
smoothed level of the last ~100 ms of the stereo master per ear and how much of
it came from direct HRTF/panning, decoded reflections and 2D passthrough — with
a single source off to one side, L and R should differ by several dB. Run it
while a sound plays to see whether a source is spatialized and where the
renderer thinks it is.

GPU mode: `snd_sa_backend 2; snd_sa_scene_type 3; snd_sa_reflection_type 3;
snd_sa_restart` (x64, AMD GPU with OpenCL, `TrueAudioNext.dll` +
`GPUUtilities.dll` present). CPU mode: `snd_sa_backend 1; snd_sa_restart`.

## GitHub builds and GPU selection

GitHub Actions builds the patched Steam Audio 4.8.1 SDK from commit
`0da18255cca520771f363ee01f100572b39a308e`, applying
`third_party/steamaudio/paged-pathing.patch`. Dependency build results are cached.
The Windows x64 job then builds and tests the module with that SDK and publishes
a full development ZIP. Standard hosted CI has no RTX GPU: hardware-specific
OpenCL/Radeon Rays verification remains a separate local test (`SA_TEST_GPU=1`).
Game installations, extracted sounds/maps, bake caches, tokens and local SDK
build directories are not part of the public source upload.

The SDK's older dependencies need CMake 3.x; the Visual Studio bundled CMake
3.31.6 works, while CMake 4 rejects their old policy baselines. Local SDK builds
must use `_CL_=/MP1`, `CMAKE_BUILD_PARALLEL_LEVEL=1`, and the `sa_run_limited`
launcher (3072 MiB committed-memory budget and 35% CPU by default). Limiting
CMake's job count alone does not constrain MSVC's internal `/MP` parallelism.
The launcher places the entire child process tree in a Windows Job Object and
closes remaining children when the parent command exits.

GPU enumeration retries ordinary OpenCL if the TAN/CU-reservation query finds
no devices. NVIDIA GPUs must not be excluded just because they lack AMD's TAN
reservation features. On 0.1.4, the equivalent manual configuration is
`snd_sa_tan 0; snd_sa_gpu_compute_units 0; snd_sa_backend 2; snd_sa_scene_type 3; snd_sa_restart`.
This enables Radeon Rays with CPU convolution, not AMD TrueAudio Next.

## First-map acoustic preparation

Version 0.1.2 keeps the selected ray tracer (including Embree) in the independent
bake scene. Probe density, ray count and bounce count are not reduced. Building
that scene now happens on the bake worker, not on the simulation thread.

The client loader displays the current stage, stage progress, processed probes,
elapsed time and an approximate **remaining time for the current stage**. The
estimate is initially unknown and changes with the geometry being processed.
In single-player a preparation window captures player input while it is open.
Its skip button (or Escape) releases input and requests cancellation; HRTF and
occlusion remain available. Multiplayer uses a non-blocking HUD notification.
`snd_sa_bake_status` opens the window explicitly; `snd_sa_bake_cancel` cancels.

Reflection data is saved before optional pathing starts. Pathing requires an
explicit confirmation in the window because the SDK builds an all-pairs probe
matrix: 23,473 probes mean about 551 million pairs and potentially substantial
memory use. Choosing to play with the completed reflections skips this stage
without discarding the reflection cache. Existing reflection caches without
pathing are reused instead of automatically starting the same bake again.

Cache replacement is atomic: an interrupted write does not overwrite the last
complete cache. `steamaudio.GetBakeStatus()` exposes the preparation state and
`steamaudio.ContinueBakePathing(true|false)` answers the pathing confirmation.

Since 0.1.4, `snd_sa_pathing_memory_mb` sets a pathing memory budget in MiB
(default 1024; 0 prevents new pathing bakes). The same setting is available as
`runtime.pathing_memory_limit_mb` in JSON. The preparation screen shows the
estimated requirement and the effective budget before enabling the pathing
button. The native API repeats the check, so a console/Lua call cannot bypass it.
The budget is also constrained by available physical memory, commit capacity
and address space, reserving at least 512 MiB or a quarter of physical RAM for
the system. Probe counts exceeding the SDK's signed 16-bit path indices are
rejected as well.

A watchdog monitors process-memory growth and available memory during pathing;
it requests SDK cancellation when the budget or safety reserve is breached.
The reflection cache remains usable after this cancellation. This is a
conservative preflight estimate plus cooperative cancellation, **not an OS hard
RSS limit**: an SDK allocation or cancellation already in progress can exceed
the budget. A strict allocation ceiling would require a separate bake process.

## Native sound levels and transients

Version 0.1.3 preserves the attack of a newly captured engine sound instead of
fading its first audio block in from zero. Volume changes and explicit stops
still use smoothing. A missing paint block during an engine hitch is no longer
interpreted as the end of a live source when retiring its effects.

Local-player weapon sounds keep their listener-relative direct stereo signal
and Source's player-weapon level compensation. Steam Audio still processes
their environmental reflections; the direct signal is not filtered as if the
weapon were an external emitter below/behind the listener. `snd_sa_sounds`
labels this route `head+acoustics`.

Native voice PCM has already passed through Source's voice gain processing;
`voice_scale` is not applied to it a second time by the module. Acoustic material
coefficients affect reflection, absorption and transmission, not the WAV chosen
by the game for an impact. These fixes do not replace the material table.

Version 0.1.4 corrects the GMod-to-BASS coordinate conversion: GMod passes
`(x, -y, z)` to BASS, so the bridge reverses Y for both positions and directions.
This also applies to voice systems implemented using BASS streams; they do not
use the Source voice-channel policy controlled by `snd_sa_voice_spatial`.
Capture now buffers the first DSP block before source registration instead of
letting it escape through BASS's original output. Short streams that end before
the first game-thread tick still drain their captured PCM.

`snd_sa_sounds` also lists live BASS streams, raw/converted positions, queued
PCM, levels and attenuation. `stream input underruns` reports missing producer
data, not a WASAPI device underrun; check the separate output counter in
`snd_sa_status` when diagnosing hardware-output starvation.

## Lua API (`steamaudio.*`, client)

```lua
steamaudio.Version()                       -- "0.1.0", 32|64
steamaudio.IsActive() / GetState() / GetStatus() / StatusLines()
steamaudio.Restart()                       -- ok, err
steamaudio.SetListener(pos, ang)           -- done automatically from RenderScene
steamaudio.LoadMap(name) / LoadMapData(name, bspString) / UnloadMap()
steamaudio.UpdateBrushModel(modelIndex, pos, ang)
steamaudio.EntitySource()                  -- "native" | "lua" | "off"
steamaudio.GetOccluders()                  -- { {index, model, id, player, collision, triangles, pos}, ... }
-- Lua entity walk (used automatically while EntitySource() == "lua"):
steamaudio.BeginEntities()
steamaudio.PushEntity(entIndex, model, pos, ang, obbMins, obbMaxs, solid, isPlayer, isDormant)
steamaudio.EndEntities()
steamaudio.AddDynamicMesh({v1,v2,v3, ...}, surfaceprop, pos, ang, name) -- id
steamaudio.UpdateDynamicMesh(id, pos, ang) / RemoveDynamicMesh(id)
steamaudio.Bake(force) / CancelBake()

-- live PCM sources
local id = steamaudio.CreateStream(44100, 1, 0.5, "synth", { pos = Vector(0,0,0), soundlevel = 80 })
steamaudio.WriteStream(id, int16LittleEndianString)   -- or a table of floats
steamaudio.WriteStream(id, {0.1, -0.1, ...})
steamaudio.StreamFramesNeeded(id) / StreamFramesQueued(id)
steamaudio.SetStreamParams(id, { pos = ..., forward = ..., gain = 1, soundlevel = 75,
                                 radius = 0.5, dipole_weight = 0, dipole_power = 1,
                                 occlusion = true, transmission = true, reflections = true,
                                 pathing = true, spatialize = true, entity = entIndex })
steamaudio.FinishStream(id) / DestroyStream(id)

steamaudio.SetBassChannelParams(bassHandle, params)   -- IGModAudioChannel overrides
steamaudio.DrainLog(n) / GetConVarList() / GetPaths()

-- per-sound overrides for ordinary engine sounds (EmitSound, sound.Play,
-- soundscripts, server-networked sounds); see "Per-sound overrides" below
steamaudio.SetSoundParams(guid, params) / ClearSoundParams(guid) / GetSoundParams(guid)
steamaudio.SetEntitySoundParams(entIndex, params) / ClearEntitySoundParams(entIndex)
steamaudio.ExpectSound(entIndex, channel|nil, params, ttlSeconds)
steamaudio.AddSoundRule("weapons/*", params, { priority = 0, channel = CHAN_WEAPON }) -- id
steamaudio.RemoveSoundRule(id) / ClearSoundRules() / GetSoundRules()
steamaudio.ClearSoundOverrides()          -- everything above at once
steamaudio.GetLastSoundGuid() / IsSoundPlaying(guid) / GetActiveSounds()

-- Lua-side wrappers (lua/steamaudio/emitsound.lua), vanilla behaviour + params
steamaudio.EmitSound(ent, name, level, pitch, volume, channel, flags, dsp, params) -- guid, "guid"|"expect"|"none"
steamaudio.PlayWorldSound(name, pos, level, pitch, volume, params)
steamaudio.StopSound(ent, name) / SetEntityParams(ent, params) / ClearEntityParams(ent)
steamaudio.Rule(pattern, params, priority, channel)

-- game files as the engine sees them (Workshop .gma included)
steamaudio.ReadGameFile("maps/gm_construct.bsp")  -- data, source  |  nil, err
steamaudio.GameFileExists("models/props_c17/oildrum001.mdl")
steamaudio.GetFileSystemInfo()                    -- { available = bool, status = "..." }

-- how a world texture ends up as an acoustic material
steamaudio.GetSurfaceProp("concrete/concretefloor001a")
-- { texture, vmt = "materials/concrete/concretefloor001a.vmt", surfaceprop = "concrete",
--   material = "concrete", source = "vmt"|"heuristic", base, gamematerial }
```

## Per-sound overrides

Engine sounds keep playing exactly as before (the engine decodes, pitches and
mixes them); an override only changes how the module spatializes the captured
channel. `params` is a table with any subset of

| key | meaning |
| --- | --- |
| `gain` / `volume` | multiplier on the engine volume (0–4) |
| `soundlevel` or `distmult` | Source sound level (dB) / attenuation multiplier; `soundlevel = 0` = no distance attenuation |
| `radius` | volumetric occlusion radius (m) |
| `dipole_weight`, `dipole_power` | directivity |
| `air_absorption` | air absorption scale |
| `reverb_gain` | reflections/reverb send |
| `pos` | source origin (Source units) |
| `forward` or `angles` | directivity orientation |
| `spatialize`, `occlusion`, `transmission`, `reflections`, `pathing` | booleans; `spatialize = false` also turns the other four off |

Keys that are absent keep the engine-derived value, so `{ reverb_gain = 0 }`
only removes the reverb send. Pitch is not overridable: the engine resamples
before the module sees the PCM — pass it to `EmitSound` as usual.

Matching, most specific wins, and fields not set by a more specific layer fall
through to the broader one:

1. `SetSoundParams(guid, …)` — exact sound. GUIDs come from
   `GetLastSoundGuid()` right after starting a sound (or `GetActiveSounds()`).
   Repeated calls layer onto the existing override (only the given keys
   change); `ClearSoundParams` removes it. An override is kept for ~5 s after
   the sound stops and is dropped afterwards; a new sound on the same engine
   channel gets a new GUID and never inherits it.
2. `ExpectSound(entIndex, channel, …, ttl)` — one-shot: the first *new* sound
   from that entity (on that channel; `nil`/`CHAN_AUTO` = any) within `ttl`
   (0.05–30 s) takes the params and they become its GUID override. Sounds
   already playing never consume it; it expires silently otherwise. This is
   the fallback for sounds whose GUID cannot be observed.
3. `SetEntitySoundParams(entIndex, …)` — every sound from the entity.
   Layers like (1); `ClearEntitySoundParams` or an empty table removes it.
4. `AddSoundRule(pattern, …, { priority, channel })` — glob on the normalized
   sample path (`weapons/ar2/fire1.wav`; case-insensitive, `\` → `/`, sound
   characters `*#@><^)(}$!?` and a leading `sound/` stripped). Higher
   priority wins, then later registration. `channel` restricts to a `CHAN_*`.
   Rules need the sample name, which is known when the `EmitSound` hook is
   installed (`steamaudio.GetStatus().emitsound_hooked`) or the file-name
   handle resolver is configured; a bare `"*"` also matches nameless sounds.

No match → the sound is spatialized purely from engine data. Limits: 2048
GUID entries, 256 pending expectations, 512 rules (oldest dropped).
`GetStatus()` reports `emitsound_hooked`, `emitsound_calls`,
`names_from_handles`, `sound_name_resolver` and an `overrides` table
(`guids`, `named_guids`, `entities`, `pending`, `rules`, `rules_matched`,
`pending_consumed`, `pending_expired`); `GetActiveSounds()` shows `name`,
`spatialized` and `overridden` per sound.

```lua
-- quiet, dry gunshots from this weapon only
steamaudio.EmitSound(wep, "Weapon_AR2.Single", 75, 100, 1, CHAN_WEAPON, 0, 0,
                     { gain = 0.7, reverb_gain = 0, dipole_weight = 0.6, forward = wep:GetForward() })
-- all music 2D, no simulation
steamaudio.Rule("music/*", { spatialize = false })
-- a radio prop: everything it plays comes from its speaker cone
steamaudio.SetEntityParams(radio, { dipole_weight = 0.8, dipole_power = 2, radius = 0.2 })
```

Without the module's engine hooks (passthrough state) all of the above is
accepted and stored but has no audible effect; the wrappers then behave
exactly like the vanilla calls.

## Environment DSP (dsp_room / underwater), Doppler, automatic directivity

The engine's own DSP chain (`dsp_room`, `dsp_player`, `dsp_water`,
`dsp_facingaway`, …) is muted together with its spatializer. The module
replaces the parts that are audible in practice:

* **Room reverb** (`src/mixing/RoomDsp.cpp`). Legacy `dsp_room` IDs 0–29 and
  the automatic templates (100+; `dsp_room` is set to these by soundscapes
  and `env_sound` on most maps) map to a parametric reverb (pre-delay, decay,
  damping, wet level, room size). Unknown IDs resolve to 0 (off), never to a
  guessed preset. Each source has a room send whose level follows the Source
  distance model (`dist_mult` / sound level, closer sources drier). With
  `snd_sa_room_dsp 1` (default) the room reverb is only used for sources that
  do **not** already get Steam Audio reflections (2D / passthrough / disabled
  reflections), so nothing is processed twice; `2` always adds it, `0` turns
  it off. `snd_sa_room_dsp_preset` pins a preset regardless of `dsp_room`.
* **Underwater**. When the local player is submerged (`WaterLevel() >= 3`)
  the master output gets a bounded lowpass (`snd_sa_underwater_cutoff`, 100–20000 Hz)
  and gain (`snd_sa_underwater_gain`, 0–1) and the room reverb switches to the
  `dsp_water` preset. Cutoff and gain are smoothed (no clicks on entering or
  leaving water). Off or missing state = fully transparent.
* **dsp_player**. Presets set by game code (damage, flashbang, …) become a
  master lowpass/gain; timed presets expire on schedule, permanent ones stay
  until game code resets `dsp_player 0`.
* **Doppler / propagation delay** (`dsp::PropagationDelay`). Each engine
  source gets a fractional delay line driven by `distance / speed_of_sound *
  doppler_scale` (capped at 0.35 s). Moving sources / listener change the
  delay smoothly (rate-limited to ±0.5 sample/sample, so pitch shifts stay
  bounded and artefact-free); a jump of 0.25 s or more (teleport, new sound)
  snaps instead of sweeping. Explicit sound pitch is untouched — the delay is
  applied on top of the engine's own pitch handling.
* **Automatic directivity** (`src/core/AutoDirectivity.h`). Sounds on
  `CHAN_WEAPON` get a dipole (`snd_sa_weapon_dipole_*`), sounds on
  `CHAN_VOICE`, the voice channel range and sentences get
  `snd_sa_voice_dipole_*`, facing the emitting entity's forward vector (the
  listener's forward for the local player, the cached entity angles from the
  occluder snapshot for everything else — so it needs the entity walk, i.e.
  at least one of `snd_sa_dynamic_*` enabled). If no valid orientation is
  known the sound stays omnidirectional and fully audible. Explicit per-sound
  overrides (`dipole_weight` / `dipole_power` / `forward`) are applied after
  the heuristic and always win. `GetStatus().auto_directivity` counts sounds
  that received an inferred dipole.

The Lua loader forwards the engine state with
`steamaudio.SetEnvironment(dsp_room, dsp_player, dsp_water, underwater)` only
when one of the values changes; `GetStatus()` reports `room_preset`,
`room_sends` and `player_lowpass_hz` (the effective master lowpass, 0 when
bypassed).

## Third-party

* Steam Audio SDK headers — Apache 2.0 (`third_party/steamaudio/THIRDPARTY.md`).
* MinHook — BSD 2-clause (`third_party/minhook/LICENSE.txt`).
* LZMA SDK decoder (`LzmaDec.c`) — public domain (`third_party/lzma/`).
* garrysmod_common module headers (`GarrysMod/Lua/*.h`).
