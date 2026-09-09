// src/lua/ConVars.cpp
#include "lua/ConVars.h"

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <unordered_map>

#include "core/AudioEngine.h"
#include "lua/LuaBindings.h"
#include "util/Logging.h"
#include "util/Math.h"

namespace sa {
namespace lua {

namespace {

// Each entry knows how to format its default and how to apply a string.
struct Binding {
    ConVarDef def;
    std::function<void(const std::string&, RuntimeConfig&, StaticConfig&)> apply;
};

std::string FormatFloat(float v)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(v));
    return buf;
}

std::string FormatInt(int32_t v)
{
    return std::to_string(v);
}

std::string FormatBool(bool v)
{
    return v ? "1" : "0";
}

bool ParseBool(const std::string& s)
{
    if (s.empty())
        return false;
    if (s == "true" || s == "yes" || s == "on")
        return true;
    if (s == "false" || s == "no" || s == "off")
        return false;
    return std::strtod(s.c_str(), nullptr) != 0.0;
}

float ParseFloat(const std::string& s, float fallback)
{
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || !std::isfinite(v) || std::fabs(v) > std::numeric_limits<float>::max())
        return fallback;
    return static_cast<float>(v);
}

int32_t ParseInt(const std::string& s, int32_t fallback)
{
    char* end = nullptr;
    const double value = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || !std::isfinite(value) || value < std::numeric_limits<int32_t>::min() ||
        value > std::numeric_limits<int32_t>::max())
        return fallback;
    return static_cast<int32_t>(value);
}

class Schema {
public:
    Schema(const RuntimeConfig& rt = RuntimeConfig{}, const StaticConfig& st = StaticConfig{})
    {

        // --- runtime -----------------------------------------------------
        RtBool("snd_sa_enabled", rt.enabled, &RuntimeConfig::enabled,
               "Route audio through Steam Audio (0 = engine mixer passthrough).");
        RtBool("snd_sa_physical_acoustics", rt.physicalAcoustics, &RuntimeConfig::physicalAcoustics,
               "Preserve simulated wall attenuation and source-dependent reflections (0 = legacy acoustic model).");
        RtBool("snd_sa_hrtf", rt.hrtf, &RuntimeConfig::hrtf, "Binaural HRTF rendering (0 = panning).");
        RtInt("snd_sa_hrtf_interpolation", rt.hrtfInterpolation, &RuntimeConfig::hrtfInterpolation,
              "HRTF interpolation: 0 nearest, 1 bilinear.", 0, 1);
        RtBool("snd_sa_reflections", rt.reflections, &RuntimeConfig::reflections, "Simulated reflections/reverb.");
        RtBool("snd_sa_pathing", rt.pathing, &RuntimeConfig::pathing, "Sound propagation around geometry.");
        RtBool("snd_sa_occlusion", rt.occlusion, &RuntimeConfig::occlusion, "Ray-traced occlusion.");
        RtBool("snd_sa_transmission", rt.transmission, &RuntimeConfig::transmission,
               "Transmission through occluding geometry.");
        RtInt("snd_sa_occlusion_type", rt.occlusionType, &RuntimeConfig::occlusionType,
              "0 raycast, 1 volumetric occlusion.", 0, 1);
        RtInt("snd_sa_occlusion_samples", rt.occlusionSamples, &RuntimeConfig::occlusionSamples,
              "Volumetric occlusion samples per source.", 1, 256);
        RtInt("snd_sa_transmission_rays", rt.transmissionRays, &RuntimeConfig::transmissionRays,
              "Transmission rays per source.", 1, 16);
        RtInt("snd_sa_rays", rt.numRays, &RuntimeConfig::numRays, "Real-time reflection rays.", 64, 65536);
        RtInt("snd_sa_bounces", rt.numBounces, &RuntimeConfig::numBounces, "Real-time reflection bounces.", 1, 64);
        RtFloat("snd_sa_ir_duration", rt.irDuration, &RuntimeConfig::irDuration,
                "Impulse response length in seconds.", 0.1f, 10.f);
        RtInt("snd_sa_ambisonic_order", rt.ambisonicOrder, &RuntimeConfig::ambisonicOrder,
              "Ambisonic order for reflections.", 0, 3);
        RtFloat("snd_sa_irradiance_min_distance", rt.irradianceMinDistance, &RuntimeConfig::irradianceMinDistance,
                "Minimum distance (m) for irradiance.", 0.01f, 10.f);
        RtFloat("snd_sa_hybrid_transition", rt.hybridTransitionTime, &RuntimeConfig::hybridTransitionTime,
                "Hybrid reverb transition time (s).", 0.f, 5.f);
        RtFloat("snd_sa_hybrid_overlap", rt.hybridOverlapPercent, &RuntimeConfig::hybridOverlapPercent,
                "Hybrid reverb overlap fraction.", 0.f, 1.f);
        RtFloat("snd_sa_reverb_gain", rt.reverbGain, &RuntimeConfig::reverbGain, "Reflections mix gain.", 0.f,
                4.f);
        RtFloat("snd_sa_direct_gain", rt.directGain, &RuntimeConfig::directGain, "Direct path mix gain.", 0.f, 4.f);
        RtFloat("snd_sa_pathing_gain", rt.pathingGain, &RuntimeConfig::pathingGain, "Pathing mix gain.", 0.f, 4.f);
        RtFloat("snd_sa_master_volume", rt.masterVolume, &RuntimeConfig::masterVolume,
                "Module master volume (multiplied with 'volume').", 0.f, 2.f);
        RtBool("snd_sa_voice_spatial", rt.voiceSpatial, &RuntimeConfig::voiceSpatial, "Spatialize voice chat.");
        RtBool("snd_sa_spatialize_stereo", rt.spatializeStereo, &RuntimeConfig::spatializeStereo,
               "Downmix and spatialize stereo world sounds.");
        RtBool("snd_sa_bass_spatial", rt.bassSpatial, &RuntimeConfig::bassSpatial,
               "Spatialize 3D sound.PlayFile/PlayURL channels.");
        RtFloat("snd_sa_units_per_meter", rt.unitsPerMeter, &RuntimeConfig::unitsPerMeter,
                "Source units per meter.", 1.f, 1000.f);
        RtFloat("snd_sa_sim_interval_ms", rt.simulationIntervalMs, &RuntimeConfig::simulationIntervalMs,
                "Direct simulation interval (ms).", 1.f, 200.f);
        RtFloat("snd_sa_reflections_interval_ms", rt.reflectionsIntervalMs, &RuntimeConfig::reflectionsIntervalMs,
                "Reflection simulation interval (ms).", 10.f, 2000.f);
        RtFloat("snd_sa_pathing_interval_ms", rt.pathingIntervalMs, &RuntimeConfig::pathingIntervalMs,
                "Pathing simulation interval (ms).", 10.f, 2000.f);
        RtFloat("snd_sa_source_radius", rt.sourceRadius, &RuntimeConfig::sourceRadius,
                "Source radius (m) for volumetric occlusion.", 0.01f, 10.f);
        RtFloat("snd_sa_occlusion_full", rt.occlusionFullVisibility, &RuntimeConfig::occlusionFullVisibility,
                "Legacy acoustic mode: visibility fraction at/above which a source counts as unoccluded.", 0.05f, 1.f);
        RtFloat("snd_sa_occlusion_zero", rt.occlusionZeroVisibility, &RuntimeConfig::occlusionZeroVisibility,
                "Legacy acoustic mode: visibility fraction at/below which a source counts as fully occluded.", 0.f, 0.95f);
        RtFloat("snd_sa_occlusion_min", rt.occlusionMinGain, &RuntimeConfig::occlusionMinGain,
                "Legacy acoustic mode wall-leak floor; ignored when physical acoustics is enabled.",
                0.f, 1.f);
        RtFloat("snd_sa_emitter_hull_margin", rt.emitterHullMarginUnits, &RuntimeConfig::emitterHullMarginUnits,
                "Extra distance (units) an emitter is moved out of its own entity's bounds toward the listener.",
                0.f, 64.f);
        RtFloat("snd_sa_air_absorption", rt.airAbsorptionScale, &RuntimeConfig::airAbsorptionScale,
                "Air absorption scale.", 0.f, 4.f);
        RtFloat("snd_sa_distance_gain_min", rt.distanceGainMin, &RuntimeConfig::distanceGainMin,
                "Distance attenuation floor.", 0.f, 1.f);
        RtFloat("snd_sa_distance_gain_max", rt.distanceGainMax, &RuntimeConfig::distanceGainMax,
                "Distance attenuation ceiling.", 0.f, 4.f);
        RtBool("snd_sa_baked_reverb", rt.useBakedReverb, &RuntimeConfig::useBakedReverb,
               "Use baked reverb probes; physical mode limits listener-local reverb to listener-relative sounds.");
        RtBool("snd_sa_bake_on_map_load", rt.bakeOnMapLoad, &RuntimeConfig::bakeOnMapLoad,
               "Bake reverb probes in the background after map load.");
        RtFloat("snd_sa_probe_spacing", rt.probeSpacing, &RuntimeConfig::probeSpacing, "Probe spacing (m).", 0.5f,
                50.f);
        RtFloat("snd_sa_probe_height", rt.probeHeight, &RuntimeConfig::probeHeight, "Probe height (m).", 0.1f,
                10.f);
        RtInt("snd_sa_bake_rays", rt.bakeRays, &RuntimeConfig::bakeRays, "Bake rays.", 256, 262144);
        RtInt("snd_sa_bake_bounces", rt.bakeBounces, &RuntimeConfig::bakeBounces, "Bake bounces.", 1, 128);
        RtInt("snd_sa_pathing_memory_mb", rt.pathingMemoryLimitMiB, &RuntimeConfig::pathingMemoryLimitMiB,
              "Pathing memory budget (MiB); 0 disables pathing bake. Safety reserve is also enforced.", 0, 16384);
        RtFloat("snd_sa_bake_duration", rt.bakeDuration, &RuntimeConfig::bakeDuration, "Bake IR duration (s).", 0.1f,
                10.f);
        RtBool("snd_sa_dynamic_geometry", rt.dynamicGeometry, &RuntimeConfig::dynamicGeometry,
               "Track moving brush entities (doors, elevators, func_movelinear) as dynamic geometry.");
        RtBool("snd_sa_dynamic_props", rt.dynamicProps, &RuntimeConfig::dynamicProps,
               "Physics/dynamic props, ragdolls and vehicles occlude and reflect sound (collision model or hull box).");
        RtBool("snd_sa_dynamic_players", rt.dynamicPlayers, &RuntimeConfig::dynamicPlayers,
               "Other players occlude sound (hull box).");
        RtBool("snd_sa_native_entities", rt.nativeEntityList, &RuntimeConfig::nativeEntityList,
               "Walk the client entity list natively (falls back to the Lua walk when the layout does not validate; "
               "applies after snd_sa_restart).");
        RtInt("snd_sa_dynamic_max", rt.dynamicMaxOccluders, &RuntimeConfig::dynamicMaxOccluders,
              "Maximum dynamic occluders (props + players) in the scene.", 0, 4096);
        RtFloat("snd_sa_dynamic_range", rt.dynamicRangeUnits, &RuntimeConfig::dynamicRangeUnits,
                "Only entities within this distance of the listener become occluders (units, 0 = unlimited).", 0.f,
                100000.f);
        RtFloat("snd_sa_dynamic_min_size", rt.dynamicMinExtentUnits, &RuntimeConfig::dynamicMinExtentUnits,
                "Skip entities whose largest hull side is below this (units).", 0.f, 1024.f);
        RtFloat("snd_sa_dynamic_interval", rt.dynamicUpdateIntervalMs, &RuntimeConfig::dynamicUpdateIntervalMs,
                "Minimum interval between entity scan starts (ms); pending scans continue in later frames.", 16.f, 5000.f);
        RtFloat("snd_sa_dynamic_scan_budget_ms", rt.dynamicScanBudgetMs, &RuntimeConfig::dynamicScanBudgetMs,
                "Native entity scan budget per game frame (ms); one engine call may overrun it.", 0.25f, 8.f);
        RtFloat("snd_sa_dynamic_model_budget_ms", rt.dynamicModelBudgetMs, &RuntimeConfig::dynamicModelBudgetMs,
                "Soft model preparation budget per occluder update (ms); one model operation may overrun it.", 0.25f, 8.f);
        RtBool("snd_sa_static_props", rt.staticProps, &RuntimeConfig::staticProps,
               "Add static prop collision models (.phy) to the acoustic scene (applies on next map load).");
        RtBool("snd_sa_vmt_surfaceprops", rt.vmtSurfaceProps, &RuntimeConfig::vmtSurfaceProps,
               "Read $surfaceprop from materials/*.vmt for world faces (applies on next map load).");
        RtBool("snd_sa_surfaceprop_scripts", rt.surfacePropScripts, &RuntimeConfig::surfacePropScripts,
               "Load scripts/surfaceproperties*.txt so custom surfaceprops inherit acoustic materials "
               "(applies after snd_sa_restart).");
        RtBool("snd_sa_static_prop_box_fallback", rt.staticPropBoxFallback, &RuntimeConfig::staticPropBoxFallback,
               "Approximate static props without a collision model by their hull box.");
        RtInt("snd_sa_pathing_vis_samples", rt.pathingVisSamples, &RuntimeConfig::pathingVisSamples,
              "Pathing visibility samples.", 1, 64);
        RtFloat("snd_sa_pathing_vis_radius", rt.pathingVisRadius, &RuntimeConfig::pathingVisRadius,
                "Pathing visibility radius (m).", 0.f, 10.f);
        RtFloat("snd_sa_pathing_vis_threshold", rt.pathingVisThreshold, &RuntimeConfig::pathingVisThreshold,
                "Pathing visibility threshold.", 0.f, 1.f);
        RtFloat("snd_sa_pathing_vis_range", rt.pathingVisRange, &RuntimeConfig::pathingVisRange,
                "Pathing visibility range (m).", 1.f, 10000.f);
        RtFloat("snd_sa_pathing_range", rt.pathingRange, &RuntimeConfig::pathingRange, "Pathing range (m).", 1.f,
                10000.f);
        RtBool("snd_sa_pathing_validation", rt.pathingValidation, &RuntimeConfig::pathingValidation,
               "Validate baked paths at runtime.");
        RtBool("snd_sa_pathing_alternate", rt.pathingAlternatePaths, &RuntimeConfig::pathingAlternatePaths,
               "Search alternate paths.");
        RtInt("snd_sa_room_dsp", rt.roomDspMode, &RuntimeConfig::roomDspMode,
              "dsp_room replacement: 0 off, 1 only for sources without Steam Audio reflections, 2 always.", 0, 2);
        RtInt("snd_sa_room_dsp_preset", rt.roomDspPreset, &RuntimeConfig::roomDspPreset,
              "Force a room preset (-1 = follow dsp_room; 1-29 legacy, 100+ automatic templates).", -1, 200);
        RtFloat("snd_sa_room_dsp_gain", rt.roomDspGain, &RuntimeConfig::roomDspGain, "Room reverb wet gain.", 0.f,
                4.f);
        RtBool("snd_sa_player_dsp", rt.playerDsp, &RuntimeConfig::playerDsp,
               "dsp_player replacement (muffle/lowpass presets set by game code).");
        RtBool("snd_sa_underwater", rt.underwaterDsp, &RuntimeConfig::underwaterDsp,
               "Underwater lowpass + dsp_water reverb while submerged.");
        RtFloat("snd_sa_underwater_cutoff", rt.underwaterCutoffHz, &RuntimeConfig::underwaterCutoffHz,
                "Underwater lowpass cutoff (Hz).", 100.f, 20000.f);
        RtFloat("snd_sa_underwater_gain", rt.underwaterGain, &RuntimeConfig::underwaterGain, "Underwater gain.", 0.f,
                1.f);
        RtBool("snd_sa_doppler", rt.doppler, &RuntimeConfig::doppler,
               "Propagation delay / Doppler for moving sources and listener.");
        RtFloat("snd_sa_doppler_scale", rt.dopplerScale, &RuntimeConfig::dopplerScale,
                "Doppler strength (1 = physical, 0 = off).", 0.f, 4.f);
        RtFloat("snd_sa_speed_of_sound", rt.speedOfSound, &RuntimeConfig::speedOfSound, "Speed of sound (m/s).",
                50.f, 5000.f);
        RtBool("snd_sa_auto_directivity", rt.autoDirectivity, &RuntimeConfig::autoDirectivity,
               "Infer dipole directivity for weapon / voice channels without explicit overrides.");
        RtFloat("snd_sa_weapon_dipole_weight", rt.weaponDipoleWeight, &RuntimeConfig::weaponDipoleWeight,
                "Automatic directivity weight for CHAN_WEAPON sounds.", 0.f, 1.f);
        RtFloat("snd_sa_weapon_dipole_power", rt.weaponDipolePower, &RuntimeConfig::weaponDipolePower,
                "Automatic directivity sharpness for CHAN_WEAPON sounds.", 0.f, 8.f);
        RtFloat("snd_sa_voice_dipole_weight", rt.voiceDipoleWeight, &RuntimeConfig::voiceDipoleWeight,
                "Automatic directivity weight for CHAN_VOICE sounds.", 0.f, 1.f);
        RtFloat("snd_sa_voice_dipole_power", rt.voiceDipolePower, &RuntimeConfig::voiceDipolePower,
                "Automatic directivity sharpness for CHAN_VOICE sounds.", 0.f, 8.f);
        RtBool("snd_sa_debug", rt.debugDraw, &RuntimeConfig::debugDraw, "Debug overlay.");
        RtInt("snd_sa_log_level", rt.logLevel, &RuntimeConfig::logLevel, "Log verbosity: 0 debug, 1 info, 2 warn, 3 error.", 0, 3);

        // --- static (restart) ----------------------------------------------
        StInt("snd_sa_backend", static_cast<int32_t>(st.backend),
              [](StaticConfig& c, int32_t v) { c.backend = static_cast<BackendPreference>(Clamp(v, 0, 2)); },
              "0 auto, 1 CPU, 2 GPU (TrueAudio Next / Radeon Rays, x64 only).", 0, 2);
        StInt("snd_sa_scene_type", static_cast<int32_t>(st.sceneType),
              [](StaticConfig& c, int32_t v) { c.sceneType = static_cast<SceneTypePreference>(Clamp(v, 0, 3)); },
              "0 auto, 1 built-in, 2 Embree, 3 Radeon Rays.", 0, 3);
        StInt("snd_sa_reflection_type", static_cast<int32_t>(st.reflectionType),
              [](StaticConfig& c, int32_t v) {
                  c.reflectionType = static_cast<ReflectionTypePreference>(Clamp(v, 0, 3));
              },
              "0 convolution, 1 parametric, 2 hybrid, 3 TrueAudio Next.", 0, 3);
        StInt("snd_sa_output", static_cast<int32_t>(st.outputMode),
              [](StaticConfig& c, int32_t v) { c.outputMode = static_cast<OutputMode>(Clamp(v, 0, 2)); },
              "0 auto, 1 native WASAPI, 2 engine paint buffer.", 0, 2);
        StString("snd_sa_output_device", st.outputDeviceId,
                 [](StaticConfig& c, const std::string& v) { c.outputDeviceId = v; },
                 "WASAPI endpoint id for native output (empty = default).");
        StInt("snd_sa_latency_ms", st.outputLatencyMs,
              [](StaticConfig& c, int32_t v) { c.outputLatencyMs = Clamp(v, 10, 500); },
              "Native output buffer (ms).", 10, 500);
        StInt("snd_sa_frame_size", st.frameSize,
              [](StaticConfig& c, int32_t v) { c.frameSize = Clamp(v, 64, 4096); },
              "Steam Audio frame size (samples).", 64, 4096);
        StInt("snd_sa_max_sources", st.maxSources,
              [](StaticConfig& c, int32_t v) { c.maxSources = Clamp(v, 8, 512); }, "Simulated source budget.", 8,
              512);
        StInt("snd_sa_max_rays", st.maxRays, [](StaticConfig& c, int32_t v) { c.maxRays = Clamp(v, 64, 65536); },
              "Upper bound for snd_sa_rays.", 64, 65536);
        StInt("snd_sa_max_occlusion_samples", st.maxOcclusionSamples,
              [](StaticConfig& c, int32_t v) { c.maxOcclusionSamples = Clamp(v, 1, 256); },
              "Upper bound for snd_sa_occlusion_samples.", 1, 256);
        StFloat("snd_sa_max_ir_duration", st.maxIrDuration,
                [](StaticConfig& c, float v) { c.maxIrDuration = Clamp(v, 0.1f, 10.f); },
                "Upper bound for snd_sa_ir_duration (s).", 0.1f, 10.f);
        StInt("snd_sa_max_ambisonic_order", st.maxAmbisonicOrder,
              [](StaticConfig& c, int32_t v) { c.maxAmbisonicOrder = Clamp(v, 0, 3); },
              "Upper bound for snd_sa_ambisonic_order.", 0, 3);
        StInt("snd_sa_simulation_threads", st.simulationThreads,
              [](StaticConfig& c, int32_t v) { c.simulationThreads = Clamp(v, 0, 64); },
              "Steam Audio worker threads (0 = auto).", 0, 64);
        StString("snd_sa_sofa", st.sofaFile, [](StaticConfig& c, const std::string& v) { c.sofaFile = v; },
                 "Custom HRTF (.sofa) file, relative to garrysmod/.");
        StFloat("snd_sa_hrtf_volume_db", st.hrtfVolumeDb,
                [](StaticConfig& c, float v) { c.hrtfVolumeDb = Clamp(v, -40.f, 20.f); }, "HRTF gain (dB).", -40.f,
                20.f);
        StInt("snd_sa_gpu_compute_units", st.gpuComputeUnits,
              [](StaticConfig& c, int32_t v) { c.gpuComputeUnits = Clamp(v, 0, 256); },
              "OpenCL compute units reserved for convolution (0 = all).", 0, 256);
        StFloat("snd_sa_gpu_ir_update_fraction", st.gpuIrUpdateFraction,
                [](StaticConfig& c, float v) { c.gpuIrUpdateFraction = Clamp(v, 0.f, 1.f); },
                "Fraction of TAN compute for IR updates.", 0.f, 1.f);
        StBool("snd_sa_embree", st.enableEmbree, [](StaticConfig& c, bool v) { c.enableEmbree = v; },
               "Allow the Embree ray tracer (x64).");
        StBool("snd_sa_tan", st.enableTan, [](StaticConfig& c, bool v) { c.enableTan = v; },
               "Allow TrueAudio Next convolution (x64, AMD GPU).");
        StBool("snd_sa_validation", st.validationLayer, [](StaticConfig& c, bool v) { c.validationLayer = v; },
               "Enable Steam Audio's validation layer (slow).");

        for (const Binding& b : m_bindings)
            m_defs.push_back(b.def);
    }

    const std::vector<ConVarDef>& Defs() const { return m_defs; }

    bool Apply(const std::string& name, const std::string& value, RuntimeConfig& rt, StaticConfig& st,
               bool& isStatic) const
    {
        auto it = m_index.find(name);
        if (it == m_index.end())
            return false;
        const Binding& b = m_bindings[it->second];
        isStatic = b.def.scope == ConVarScope::Static;
        b.apply(value, rt, st);
        return true;
    }

private:
    void Add(Binding b)
    {
        m_index[b.def.name] = m_bindings.size();
        m_bindings.push_back(std::move(b));
    }

    static ConVarDef MakeDef(const char* name, std::string def, const char* help, ConVarScope scope, bool ranged,
                             float lo, float hi)
    {
        ConVarDef d;
        d.name = name;
        d.defaultValue = std::move(def);
        d.help = help;
        d.scope = scope;
        d.hasRange = ranged;
        d.minValue = lo;
        d.maxValue = hi;
        return d;
    }

    void RtBool(const char* name, bool def, bool RuntimeConfig::*field, const char* help)
    {
        Binding b;
        b.def = MakeDef(name, FormatBool(def), help, ConVarScope::Runtime, true, 0.f, 1.f);
        b.apply = [field](const std::string& v, RuntimeConfig& rt, StaticConfig&) { rt.*field = ParseBool(v); };
        Add(std::move(b));
    }
    void RtInt(const char* name, int32_t def, int32_t RuntimeConfig::*field, const char* help, int32_t lo,
               int32_t hi)
    {
        Binding b;
        b.def = MakeDef(name, FormatInt(def), help, ConVarScope::Runtime, true, static_cast<float>(lo),
                        static_cast<float>(hi));
        b.apply = [field, def, lo, hi](const std::string& v, RuntimeConfig& rt, StaticConfig&) {
            rt.*field = Clamp(ParseInt(v, def), lo, hi);
        };
        Add(std::move(b));
    }
    void RtFloat(const char* name, float def, float RuntimeConfig::*field, const char* help, float lo, float hi)
    {
        Binding b;
        b.def = MakeDef(name, FormatFloat(def), help, ConVarScope::Runtime, true, lo, hi);
        b.apply = [field, def, lo, hi](const std::string& v, RuntimeConfig& rt, StaticConfig&) {
            rt.*field = Clamp(ParseFloat(v, def), lo, hi);
        };
        Add(std::move(b));
    }
    void StInt(const char* name, int32_t def, std::function<void(StaticConfig&, int32_t)> set, const char* help,
               int32_t lo, int32_t hi)
    {
        Binding b;
        b.def = MakeDef(name, FormatInt(def), help, ConVarScope::Static, true, static_cast<float>(lo),
                        static_cast<float>(hi));
        b.apply = [set, def](const std::string& v, RuntimeConfig&, StaticConfig& st) { set(st, ParseInt(v, def)); };
        Add(std::move(b));
    }
    void StFloat(const char* name, float def, std::function<void(StaticConfig&, float)> set, const char* help,
                 float lo, float hi)
    {
        Binding b;
        b.def = MakeDef(name, FormatFloat(def), help, ConVarScope::Static, true, lo, hi);
        b.apply = [set, def](const std::string& v, RuntimeConfig&, StaticConfig& st) { set(st, ParseFloat(v, def)); };
        Add(std::move(b));
    }
    void StBool(const char* name, bool def, std::function<void(StaticConfig&, bool)> set, const char* help)
    {
        Binding b;
        b.def = MakeDef(name, FormatBool(def), help, ConVarScope::Static, true, 0.f, 1.f);
        b.apply = [set](const std::string& v, RuntimeConfig&, StaticConfig& st) { set(st, ParseBool(v)); };
        Add(std::move(b));
    }
    void StString(const char* name, const std::string& def, std::function<void(StaticConfig&, const std::string&)> set,
                  const char* help)
    {
        Binding b;
        b.def = MakeDef(name, def, help, ConVarScope::Static, false, 0.f, 0.f);
        b.apply = [set](const std::string& v, RuntimeConfig&, StaticConfig& st) { set(st, v); };
        Add(std::move(b));
    }

    std::vector<Binding> m_bindings;
    std::unordered_map<std::string, size_t> m_index;
    std::vector<ConVarDef> m_defs;
};

Schema& GetSchema()
{
    static Schema schema;
    return schema;
}

// Returns the current string value of a convar via GetConVar(name):GetString().
bool ReadConVarValue(GarrysMod::Lua::ILuaBase* LUA, const std::string& name, std::string& out)
{
    using namespace GarrysMod::Lua;
    const int top = LUA->Top();
    LUA->PushSpecial(SPECIAL_GLOB);
    LUA->GetField(-1, "GetConVar");
    if (!LUA->IsType(-1, Type::Function)) {
        LUA->Pop(LUA->Top() - top);
        return false;
    }
    LUA->PushString(name.c_str());
    if (LUA->PCall(1, 1, 0) != 0 || LUA->IsType(-1, Type::Nil)) {
        LUA->Pop(LUA->Top() - top);
        return false;
    }
    // stack: glob, convar
    LUA->GetField(-1, "GetString");
    if (!LUA->IsType(-1, Type::Function)) {
        LUA->Pop(LUA->Top() - top);
        return false;
    }
    LUA->Push(-2);
    if (LUA->PCall(1, 1, 0) != 0) {
        LUA->Pop(LUA->Top() - top);
        return false;
    }
    const char* s = LUA->GetString(-1);
    out = s ? s : "";
    LUA->Pop(LUA->Top() - top);
    return true;
}

} // namespace

const std::vector<ConVarDef>& ConVarDefinitions()
{
    return GetSchema().Defs();
}

bool ApplyConVar(const std::string& name, const std::string& value, RuntimeConfig& runtime, StaticConfig& fixed,
                 bool& isStatic)
{
    return GetSchema().Apply(name, value, runtime, fixed, isStatic);
}

int ConVarChangedCallback(lua_State* L)
{
    GarrysMod::Lua::ILuaBase* LUA = L->luabase;
    LUA->SetState(L);
    AudioEngine* engine = BoundEngine();
    const char* name = LUA->IsType(1, GarrysMod::Lua::Type::String) ? LUA->GetString(1) : nullptr;
    const char* value = LUA->IsType(3, GarrysMod::Lua::Type::String) ? LUA->GetString(3) : nullptr;
    if (!engine || !name || !value)
        return 0;
    RuntimeConfig rt = engine->Runtime();
    StaticConfig st = engine->Static();
    bool isStatic = false;
    if (!ApplyConVar(name, value, rt, st, isStatic))
        return 0;
    if (isStatic) {
        engine->MarkRestartPending();
        SA_LOGI("[cvar] %s = %s (takes effect after snd_sa_restart)", name, value);
    } else {
        engine->SetRuntimeConfig(rt);
    }
    return 0;
}

void RegisterConVars(GarrysMod::Lua::ILuaBase* LUA, const RuntimeConfig& runtime, const StaticConfig& fixed)
{
    GetSchema() = Schema(runtime, fixed);
    using namespace GarrysMod::Lua;
    const int top = LUA->Top();
    LUA->PushSpecial(SPECIAL_GLOB);
    const int glob = LUA->Top();
    for (const ConVarDef& def : ConVarDefinitions()) {
        // CreateClientConVar(name, default, shouldsave, userinfo, helptext, min, max)
        LUA->GetField(glob, "CreateClientConVar");
        if (!LUA->IsType(-1, Type::Function)) {
            LUA->Pop();
            break;
        }
        LUA->PushString(def.name.c_str());
        LUA->PushString(def.defaultValue.c_str());
        LUA->PushBool(true);
        LUA->PushBool(false);
        LUA->PushString(def.help.c_str());
        if (def.hasRange) {
            LUA->PushNumber(def.minValue);
            LUA->PushNumber(def.maxValue);
        } else {
            LUA->PushNil();
            LUA->PushNil();
        }
        if (LUA->PCall(7, 1, 0) != 0) {
            SA_LOGW("[cvar] CreateClientConVar(%s) failed: %s", def.name.c_str(), LUA->GetString(-1));
        }
        LUA->Pop();

        // cvars.AddChangeCallback(name, callback, identifier)
        LUA->GetField(glob, "cvars");
        if (LUA->IsType(-1, Type::Table)) {
            LUA->GetField(-1, "AddChangeCallback");
            if (LUA->IsType(-1, Type::Function)) {
                LUA->PushString(def.name.c_str());
                LUA->PushCFunction(ConVarChangedCallback);
                LUA->PushString("steamaudio");
                if (LUA->PCall(3, 0, 0) != 0) {
                    SA_LOGW("[cvar] AddChangeCallback(%s) failed: %s", def.name.c_str(), LUA->GetString(-1));
                    LUA->Pop();
                }
            } else {
                LUA->Pop();
            }
        }
        LUA->Pop();
    }
    LUA->Pop(LUA->Top() - top);
}

void ApplyConVarOverrides(GarrysMod::Lua::ILuaBase* LUA, RuntimeConfig& runtime, StaticConfig& fixed)
{
    std::string value;
    for (const ConVarDef& def : ConVarDefinitions()) {
        if (!ReadConVarValue(LUA, def.name, value))
            continue;
        if (value == def.defaultValue && !def.hasRange)
            continue; // untouched convar: JSON config stays authoritative
        bool isStatic = false;
        ApplyConVar(def.name, value, runtime, fixed, isStatic);
    }
}

} // namespace lua
} // namespace sa
