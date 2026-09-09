// src/core/AudioEngine.cpp
#include "core/AudioEngine.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>

#include "steamaudio/CPUFallbackBackend.h"
#include "steamaudio/GPUBackend.h"
#include "steamaudio/StaticPropResolver.h"
#include "steamaudio/SurfaceProps.h"
#include "util/Json.h"
#include "util/KeyValues.h"
#include "util/Logging.h"

#if defined(_WIN32)
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace sa {

namespace {

constexpr float kFlushIntervalSeconds = 1.0f;
constexpr uint32_t kMaxClockHoldLogs = 40;
constexpr float kDeviceRetrySeconds = 2.0f;
constexpr uint32_t kMaxDeviceRetries = 5;
constexpr size_t kEngineSlotRingSeconds = 2;
constexpr size_t kMaxStreamSources = 256;

void RecordTiming(std::chrono::steady_clock::time_point start, uint32_t& last, uint32_t& peak)
{
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();
    last = static_cast<uint32_t>(std::clamp<int64_t>(elapsed, 0, std::numeric_limits<uint32_t>::max()));
    peak = std::max(peak, last);
}

const char* SceneTypeName(IPLSceneType type)
{
    switch (type) {
    case IPL_SCENETYPE_DEFAULT:
        return "default";
    case IPL_SCENETYPE_EMBREE:
        return "embree";
    case IPL_SCENETYPE_RADEONRAYS:
        return "radeonrays";
    case IPL_SCENETYPE_CUSTOM:
        return "custom";
    default:
        return "?";
    }
}

const char* StateName(EngineState s)
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
    return "?";
}

void EnsureDirectory(const std::string& path)
{
    if (path.empty())
        return;
    std::string cur;
    for (size_t i = 0; i < path.size(); ++i) {
        const char c = path[i];
        cur.push_back(c);
        if ((c == '/' || c == '\\' || i + 1 == path.size()) && cur.size() > 1) {
#if defined(_WIN32)
            _mkdir(cur.c_str());
#else
            mkdir(cur.c_str(), 0755);
#endif
        }
    }
}

std::string JoinPath(const std::string& a, const std::string& b)
{
    if (a.empty())
        return b;
    const char last = a.back();
    if (last == '/' || last == '\\')
        return a + b;
#if defined(_WIN32)
    return a + "\\" + b;
#else
    return a + "/" + b;
#endif
}

bool IsAbsolutePath(const std::string& path)
{
    if (path.empty())
        return false;
    if (path[0] == '/' || path[0] == '\\')
        return true;
    return path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':';
}

bool FileExists(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    return f.good();
}

template <typename T>
void ReadNumber(const JsonValue& obj, const char* key, T& field)
{
    if (!obj.Has(key) || !obj[key].IsNumber())
        return;
    const double value = obj[key].AsNumber(static_cast<double>(field));
    if (std::isfinite(value) && value >= static_cast<double>(std::numeric_limits<T>::lowest()) &&
        value <= static_cast<double>(std::numeric_limits<T>::max()))
        field = static_cast<T>(value);
}

void ReadBool(const JsonValue& obj, const char* key, bool& field)
{
    if (obj.Has(key))
        field = obj[key].AsBool(field);
}

void ReadString(const JsonValue& obj, const char* key, std::string& field)
{
    if (obj.Has(key) && obj[key].IsString())
        field = obj[key].AsString();
}

void ParseStaticConfig(const JsonValue& v, StaticConfig& out)
{
    if (!v.IsObject())
        return;
    ReadNumber(v, "sample_rate", out.sampleRate);
    ReadNumber(v, "frame_size", out.frameSize);
    ReadNumber(v, "max_sources", out.maxSources);
    ReadNumber(v, "simulation_threads", out.simulationThreads);
    ReadNumber(v, "max_rays", out.maxRays);
    ReadNumber(v, "max_occlusion_samples", out.maxOcclusionSamples);
    ReadNumber(v, "num_diffuse_samples", out.numDiffuseSamples);
    ReadNumber(v, "max_ir_duration", out.maxIrDuration);
    ReadNumber(v, "max_ambisonic_order", out.maxAmbisonicOrder);
    int32_t backend = static_cast<int32_t>(out.backend);
    ReadNumber(v, "backend", backend);
    out.backend = static_cast<BackendPreference>(Clamp(backend, 0, 2));
    int32_t sceneType = static_cast<int32_t>(out.sceneType);
    ReadNumber(v, "scene_type", sceneType);
    out.sceneType = static_cast<SceneTypePreference>(Clamp(sceneType, 0, 3));
    int32_t reflectionType = static_cast<int32_t>(out.reflectionType);
    ReadNumber(v, "reflection_type", reflectionType);
    out.reflectionType = static_cast<ReflectionTypePreference>(Clamp(reflectionType, 0, 3));
    ReadNumber(v, "gpu_compute_units", out.gpuComputeUnits);
    ReadNumber(v, "gpu_ir_update_fraction", out.gpuIrUpdateFraction);
    ReadBool(v, "hrtf_enabled", out.hrtfEnabled);
    ReadString(v, "sofa_file", out.sofaFile);
    ReadNumber(v, "hrtf_volume_db", out.hrtfVolumeDb);
    int32_t outputMode = static_cast<int32_t>(out.outputMode);
    ReadNumber(v, "output_mode", outputMode);
    out.outputMode = static_cast<OutputMode>(Clamp(outputMode, 0, 2));
    ReadString(v, "output_device_id", out.outputDeviceId);
    ReadNumber(v, "output_latency_ms", out.outputLatencyMs);
    ReadNumber(v, "engine_lead_ms", out.engineLeadMs);
    ReadBool(v, "validation_layer", out.validationLayer);
    ReadBool(v, "enable_embree", out.enableEmbree);
    ReadBool(v, "enable_tan", out.enableTan);
}

void ParseRuntimeConfig(const JsonValue& v, RuntimeConfig& out)
{
    if (!v.IsObject())
        return;
    ReadBool(v, "enabled", out.enabled);
    ReadBool(v, "hrtf", out.hrtf);
    ReadNumber(v, "hrtf_interpolation", out.hrtfInterpolation);
    ReadBool(v, "reflections", out.reflections);
    ReadBool(v, "pathing", out.pathing);
    ReadBool(v, "occlusion", out.occlusion);
    ReadBool(v, "transmission", out.transmission);
    ReadNumber(v, "occlusion_type", out.occlusionType);
    ReadNumber(v, "occlusion_samples", out.occlusionSamples);
    ReadNumber(v, "transmission_rays", out.transmissionRays);
    ReadNumber(v, "num_rays", out.numRays);
    ReadNumber(v, "num_bounces", out.numBounces);
    ReadNumber(v, "ir_duration", out.irDuration);
    ReadNumber(v, "ambisonic_order", out.ambisonicOrder);
    ReadNumber(v, "irradiance_min_distance", out.irradianceMinDistance);
    ReadNumber(v, "hybrid_transition_time", out.hybridTransitionTime);
    ReadNumber(v, "hybrid_overlap_percent", out.hybridOverlapPercent);
    ReadNumber(v, "reverb_gain", out.reverbGain);
    ReadNumber(v, "direct_gain", out.directGain);
    ReadNumber(v, "pathing_gain", out.pathingGain);
    ReadNumber(v, "master_volume", out.masterVolume);
    ReadBool(v, "voice_spatial", out.voiceSpatial);
    ReadBool(v, "spatialize_stereo", out.spatializeStereo);
    ReadBool(v, "bass_spatial", out.bassSpatial);
    ReadNumber(v, "units_per_meter", out.unitsPerMeter);
    ReadNumber(v, "simulation_interval_ms", out.simulationIntervalMs);
    ReadNumber(v, "reflections_interval_ms", out.reflectionsIntervalMs);
    ReadNumber(v, "pathing_interval_ms", out.pathingIntervalMs);
    ReadNumber(v, "source_radius", out.sourceRadius);
    ReadNumber(v, "occlusion_full_visibility", out.occlusionFullVisibility);
    ReadNumber(v, "occlusion_zero_visibility", out.occlusionZeroVisibility);
    ReadNumber(v, "occlusion_min_gain", out.occlusionMinGain);
    ReadBool(v, "physical_acoustics", out.physicalAcoustics);
    ReadNumber(v, "emitter_hull_margin_units", out.emitterHullMarginUnits);
    ReadNumber(v, "air_absorption_scale", out.airAbsorptionScale);
    ReadNumber(v, "distance_gain_min", out.distanceGainMin);
    ReadNumber(v, "distance_gain_max", out.distanceGainMax);
    ReadBool(v, "use_baked_reverb", out.useBakedReverb);
    ReadBool(v, "bake_on_map_load", out.bakeOnMapLoad);
    ReadNumber(v, "probe_spacing", out.probeSpacing);
    ReadNumber(v, "probe_height", out.probeHeight);
    ReadNumber(v, "bake_rays", out.bakeRays);
    ReadNumber(v, "bake_bounces", out.bakeBounces);
    ReadNumber(v, "bake_duration", out.bakeDuration);
    ReadBool(v, "dynamic_geometry", out.dynamicGeometry);
    ReadBool(v, "dynamic_props", out.dynamicProps);
    ReadBool(v, "dynamic_players", out.dynamicPlayers);
    ReadBool(v, "native_entity_list", out.nativeEntityList);
    ReadNumber(v, "dynamic_max_occluders", out.dynamicMaxOccluders);
    ReadNumber(v, "dynamic_range_units", out.dynamicRangeUnits);
    ReadNumber(v, "dynamic_min_extent_units", out.dynamicMinExtentUnits);
    ReadNumber(v, "dynamic_update_interval_ms", out.dynamicUpdateIntervalMs);
    ReadBool(v, "static_props", out.staticProps);
    ReadBool(v, "static_prop_box_fallback", out.staticPropBoxFallback);
    ReadBool(v, "vmt_surfaceprops", out.vmtSurfaceProps);
    ReadBool(v, "surfaceprop_scripts", out.surfacePropScripts);
    ReadNumber(v, "pathing_memory_limit_mb", out.pathingMemoryLimitMiB);
    ReadNumber(v, "pathing_vis_samples", out.pathingVisSamples);
    ReadNumber(v, "pathing_vis_radius", out.pathingVisRadius);
    ReadNumber(v, "pathing_vis_threshold", out.pathingVisThreshold);
    ReadNumber(v, "pathing_vis_range", out.pathingVisRange);
    ReadNumber(v, "pathing_range", out.pathingRange);
    ReadBool(v, "pathing_validation", out.pathingValidation);
    ReadBool(v, "pathing_alternate_paths", out.pathingAlternatePaths);
    ReadNumber(v, "room_dsp_mode", out.roomDspMode);
    ReadNumber(v, "room_dsp_preset", out.roomDspPreset);
    ReadNumber(v, "room_dsp_gain", out.roomDspGain);
    ReadBool(v, "player_dsp", out.playerDsp);
    ReadBool(v, "underwater_dsp", out.underwaterDsp);
    ReadNumber(v, "underwater_cutoff_hz", out.underwaterCutoffHz);
    ReadNumber(v, "underwater_gain", out.underwaterGain);
    ReadBool(v, "doppler", out.doppler);
    ReadNumber(v, "doppler_scale", out.dopplerScale);
    ReadNumber(v, "speed_of_sound", out.speedOfSound);
    ReadBool(v, "auto_directivity", out.autoDirectivity);
    ReadNumber(v, "weapon_dipole_weight", out.weaponDipoleWeight);
    ReadNumber(v, "weapon_dipole_power", out.weaponDipolePower);
    ReadNumber(v, "voice_dipole_weight", out.voiceDipoleWeight);
    ReadNumber(v, "voice_dipole_power", out.voiceDipolePower);
    ReadBool(v, "debug_draw", out.debugDraw);
    ReadNumber(v, "log_level", out.logLevel);
}

} // namespace

// ---------------------------------------------------------------------------
// RateAdaptedSource
// ---------------------------------------------------------------------------
void AudioEngine::RateAdaptedSource::Configure(IOutputSource& inner, int32_t innerRate, int32_t deviceRate,
                                               size_t maxFrames)
{
    m_inner = &inner;
    m_ratio = deviceRate > 0 ? static_cast<double>(innerRate) / static_cast<double>(deviceRate) : 1.0;
    const size_t maxInner = static_cast<size_t>(std::ceil(static_cast<double>(maxFrames) * m_ratio)) + 8;
    m_scratch.assign(maxInner * 2, 0.f);
    for (int c = 0; c < 2; ++c) {
        m_deinterleaved[c].assign(maxInner, 0.f);
        m_outScratch[c].assign(maxFrames + 8, 0.f);
        m_resamplers[c].Prepare(maxInner * 2 + 16);
    }
}

void AudioEngine::RateAdaptedSource::PullStereo(float* interleaved, size_t frames)
{
    if (!m_inner || frames == 0)
        return;
    if (frames > m_outScratch[0].size()) {
        std::fill(interleaved, interleaved + frames * 2, 0.f);
        return;
    }
    // Pull exactly as much inner audio as the resamplers need for `frames`.
    size_t needed = 0;
    for (int c = 0; c < 2; ++c)
        needed = std::max(needed, m_resamplers[c].InputNeeded(frames, m_ratio));
    needed = std::min(needed, m_deinterleaved[0].size());
    if (needed > 0) {
        m_inner->PullStereo(m_scratch.data(), needed);
        for (size_t i = 0; i < needed; ++i) {
            m_deinterleaved[0][i] = m_scratch[i * 2];
            m_deinterleaved[1][i] = m_scratch[i * 2 + 1];
        }
        for (int c = 0; c < 2; ++c)
            m_resamplers[c].Push(m_deinterleaved[c].data(), needed);
    }
    for (int c = 0; c < 2; ++c) {
        const size_t produced = m_resamplers[c].Pull(m_outScratch[c].data(), frames, m_ratio);
        if (produced < frames)
            std::fill(m_outScratch[c].begin() + static_cast<std::ptrdiff_t>(produced),
                      m_outScratch[c].begin() + static_cast<std::ptrdiff_t>(frames), 0.f);
    }
    for (size_t i = 0; i < frames; ++i) {
        interleaved[i * 2] = m_outScratch[0][i];
        interleaved[i * 2 + 1] = m_outScratch[1][i];
    }
}

// ---------------------------------------------------------------------------
// AudioEngine
// ---------------------------------------------------------------------------
AudioEngine::AudioEngine() = default;

AudioEngine::~AudioEngine()
{
    Shutdown();
}

void AudioEngine::SetState(EngineState state, const std::string& reason)
{
    m_state = state;
    m_stateReason = reason;
    SA_LOGI("[engine] state -> %s%s%s", StateName(state), reason.empty() ? "" : ": ", reason.c_str());
}

bool AudioEngine::ReadFile(const std::string& path, std::vector<uint8_t>& out) const
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.good())
        return false;
    const std::streamsize size = f.tellg();
    if (size <= 0 || static_cast<uint64_t>(size) > (uint64_t(1) << 30))
        return false;
    out.resize(static_cast<size_t>(size));
    f.seekg(0);
    return static_cast<bool>(f.read(reinterpret_cast<char*>(out.data()), size));
}

std::vector<std::string> AudioEngine::PhononSearchPaths() const
{
    std::vector<std::string> dirs;
    dirs.push_back(m_paths.moduleDirectory);
    dirs.push_back(JoinPath(m_paths.gameDirectory, "bin"));
    if (!m_paths.gameDirectory.empty()) {
        // <steam>/GarrysMod/bin and /bin/win64 next to garrysmod/
        std::string parent = m_paths.gameDirectory;
        while (!parent.empty() && (parent.back() == '/' || parent.back() == '\\'))
            parent.pop_back();
        const size_t slash = parent.find_last_of("/\\");
        if (slash != std::string::npos) {
            parent = parent.substr(0, slash);
            dirs.push_back(JoinPath(parent, "bin"));
#if SA_ARCH_BITS == 64
            dirs.push_back(JoinPath(JoinPath(parent, "bin"), "win64"));
#endif
        }
    }
    return dirs;
}

bool AudioEngine::LoadConfigs(std::string& error)
{
    m_config = EngineConfig{};
    const std::string cfgPath = JoinPath(m_paths.configDirectory, "steamaudio.json");
    if (FileExists(cfgPath)) {
        const JsonParseResult parsed = ParseJsonFile(cfgPath);
        if (!parsed.ok) {
            SA_LOGW("[engine] %s: %s (line %zu) - using defaults", cfgPath.c_str(), parsed.error.c_str(),
                    parsed.line);
        } else {
            ParseStaticConfig(parsed.value["static"], m_config.fixed);
            ParseRuntimeConfig(parsed.value["runtime"], m_config.runtime);
            SA_LOGI("[engine] loaded %s", cfgPath.c_str());
        }
    }
    if (m_configOverrides)
        m_configOverrides(m_config.runtime, m_config.fixed);
    if (m_pendingStaticValid)
        m_config.fixed = m_pendingStatic; // programmatic static change (e.g. forced output switch)
    m_pendingStaticValid = false;
    m_restartRequested = false;

    m_config.fixed.sampleRate = Clamp(m_config.fixed.sampleRate, 8000, 192000);
    m_config.fixed.frameSize = static_cast<int32_t>(NextPowerOfTwo(static_cast<size_t>(Clamp(m_config.fixed.frameSize, 64, 4096))));
    m_config.fixed.maxSources = Clamp(m_config.fixed.maxSources, 8, 512);
    m_config.fixed.maxAmbisonicOrder = Clamp(m_config.fixed.maxAmbisonicOrder, 0, 3);
    m_config.fixed.outputLatencyMs = Clamp(m_config.fixed.outputLatencyMs, 10, 500);
    m_config.fixed.engineLeadMs = Clamp(m_config.fixed.engineLeadMs, 0, 1000);
    m_config.fixed.maxRays = Clamp(m_config.fixed.maxRays, 64, 65536);
    m_config.fixed.maxOcclusionSamples = Clamp(m_config.fixed.maxOcclusionSamples, 1, 256);
    m_config.fixed.numDiffuseSamples = Clamp(m_config.fixed.numDiffuseSamples, 1, 1024);
    m_config.fixed.maxIrDuration = Clamp(m_config.fixed.maxIrDuration, 0.1f, 10.f);
    if (!m_config.fixed.sofaFile.empty() && !IsAbsolutePath(m_config.fixed.sofaFile))
        m_config.fixed.sofaFile = JoinPath(m_paths.gameDirectory, m_config.fixed.sofaFile);
    log::SetLevel(static_cast<log::Level>(Clamp(m_config.runtime.logLevel, 0, 3)));

    const std::string sigPath = JoinPath(m_paths.configDirectory, "steamaudio_signatures.json");
    std::string sigError;
    if (!SignatureConfig::Load(sigPath, m_signatures, sigError)) {
        error = "signature config: " + sigError;
        return false;
    }
    const std::string matPath = JoinPath(m_paths.configDirectory, "steamaudio_materials.json");
    if (FileExists(matPath) && !m_materials.LoadOverrides(matPath))
        SA_LOGW("[engine] material overrides in %s could not be loaded", matPath.c_str());
    return true;
}

std::vector<SoundSourcePtr> AudioEngine::CreateEngineSlotSources(int32_t sampleRate) const
{
    std::vector<SoundSourcePtr> sources;
    sources.reserve(ChannelCapture::kSlots);
    const size_t capacity = NextPowerOfTwo(static_cast<size_t>(sampleRate) * kEngineSlotRingSeconds);
    for (uint32_t i = 0; i < ChannelCapture::kSlots; ++i) {
        // Slot ids 1..kSlots; stream/BASS ids start after them.
        sources.push_back(std::make_shared<SoundSource>(i + 1, SourceKind::EngineChannel, 2,
                                                        static_cast<uint32_t>(sampleRate),
                                                        static_cast<uint32_t>(sampleRate), capacity));
    }
    return sources;
}

bool AudioEngine::InitializeEngineSide(std::string& error)
{
    if (!m_interfaces.Initialize(m_signatures, error))
        return false;
    if (m_interfaces.IsDedicatedServer())
        return true; // caller turns this into Inactive

    std::string fsError;
    if (m_fileSystem.Initialize(m_signatures.fileSystem, fsError)) {
        m_fileSystemStatus = m_fileSystem.Description();
    } else {
        m_fileSystemStatus = "unavailable: " + fsError;
        SA_LOGW("[fs] engine file system %s; Workshop maps fall back to .gma scanning / Lua file.Read",
                m_fileSystemStatus.c_str());
    }
    std::vector<std::string> gmaDirs;
    for (const std::string& dir : m_signatures.fileSystem.gmaDirectories)
        gmaDirs.push_back(IsAbsolutePath(dir) ? dir : JoinPath(m_paths.gameDirectory, dir));
    m_gma.SetDirectories(std::move(gmaDirs));

    if (m_config.runtime.nativeEntityList) {
        std::string entError;
        if (!m_entityList.Initialize(m_signatures.entityList, entError))
            SA_LOGW("[ents] native entity list unavailable (%s); dynamic occluders use the Lua entity walk",
                    entError.c_str());
    }
    m_occluders.SetMaterials(&m_materials);
    m_occluders.SetReader([this](const std::string& path, std::vector<uint8_t>& out, std::string& readError) {
        return ReadGameFile(path, out, readError);
    });
    m_occluders.Configure(OccluderOptions());

    const AudioDeviceInfo& dev = m_interfaces.AudioDevice();
    int32_t dmaRate = dev.sampleRate > 0 ? dev.sampleRate : m_config.fixed.sampleRate;
    if (dmaRate != m_config.fixed.sampleRate) {
        SA_LOGI("[engine] engine DMA rate %d Hz overrides configured %d Hz", dmaRate, m_config.fixed.sampleRate);
        m_config.fixed.sampleRate = dmaRate;
    }
    m_slotSources = CreateEngineSlotSources(m_config.fixed.sampleRate);
    m_capture.SetLayoutOverrides(m_signatures.channelLayout);
    m_capture.Initialize(m_slotSources, static_cast<uint32_t>(m_config.fixed.sampleRate));
    m_hooks.SetSoundNameResolver([this](const void* handle, std::string& out) {
        return m_fileSystem.FileNameFromHandle(handle, out);
    });
    m_hooks.SetEntityForwardResolver([this](int32_t entity, Vec3& forward) {
        Vec3 angles;
        if (!m_occluders.EntityAngles(entity, angles))
            return false;
        forward = Transform::FromAngles(Vec3{}, angles).forward;
        return true;
    });
    m_hooks.SetEmitterHullResolver([this](int32_t entity, const Vec3& listener, float margin, Vec3& position) {
        return m_occluders.PushEmitterOutside(entity, listener, margin, position);
    });

    if (m_signatures.disableMixerHooks) {
        SA_LOGW("[engine] mixer hooks disabled by configuration; engine sounds stay on the engine mixer");
        m_hooksInstalled = false;
        return true;
    }
    std::string hookError;
    m_hooksInstalled = m_hooks.Install(m_interfaces, m_capture, hookError);
    if (!m_hooksInstalled)
        SA_LOGW("[engine] mixer hooks not installed: %s", hookError.c_str());
    return true;
}

bool AudioEngine::InitializeSteamAudio(std::string& error)
{
    if (!m_context.Initialize(m_config.fixed, PhononSearchPaths(), error))
        return false;

    // Backend selection: GPU first when allowed, CPU otherwise / on failure.
    std::string backendError;
    if (m_config.fixed.backend != BackendPreference::Cpu && GPUBackend::IsSupportedBuild()) {
        auto gpu = std::make_unique<GPUBackend>();
        if (gpu->Initialize(m_context, m_config.fixed, backendError)) {
            m_backend = std::move(gpu);
        } else {
            SA_LOGW("[engine] GPU backend unavailable: %s", backendError.c_str());
            if (m_config.fixed.backend == BackendPreference::Gpu)
                SA_LOGW("[engine] GPU backend was required; falling back to CPU anyway to keep audio alive");
        }
    }
    if (!m_backend) {
        auto cpu = std::make_unique<CPUFallbackBackend>();
        if (!cpu->Initialize(m_context, m_config.fixed, backendError)) {
            error = "CPU backend: " + backendError;
            m_context.Shutdown();
            return false;
        }
        m_backend = std::move(cpu);
    }
    m_devices = m_backend->Devices();
    m_backendDescription = std::string(m_backend->Name()) + " (" + m_devices.description + ")";
    SA_LOGI("[engine] backend: %s", m_backendDescription.c_str());

    if (!m_renderer.Initialize(m_context, m_devices, m_config.fixed, error)) {
        SA_LOGW("[engine] HRTF renderer failed: %s", error.c_str());
        error.clear();
        m_backend->Shutdown();
        m_backend.reset();
        m_context.Shutdown();
        error = "HRTF renderer initialization failed";
        return false;
    }
    m_devices.reflectionType = m_renderer.ReflectionType();
    if (!m_scene.Initialize(m_context, m_devices))
        SA_LOGW("[engine] scene creation failed; simulating in free field");
    else
        m_devices.sceneType = m_scene.SceneType();
    if (!m_simulator.Initialize(m_context, m_devices, m_config.fixed, error)) {
        SA_LOGW("[engine] simulator failed: %s", error.c_str());
        m_scene.Shutdown();
        m_renderer.Shutdown();
        m_backend->Shutdown();
        m_backend.reset();
        m_context.Shutdown();
        return false;
    }
    if (m_scene.IsValid())
        m_simulator.SetScene(m_scene.Scene());
    if (!m_reflections.Initialize(m_context, m_simulator, m_config.fixed, m_devices))
        SA_LOGW("[engine] reflection bake support unavailable");
    if (!m_pathing.Initialize(m_context, m_simulator, m_config.fixed))
        SA_LOGI("[engine] pathing disabled for this configuration");
    m_steamAudioReady = true;
    return true;
}

bool AudioEngine::OpenNativeOutput(std::string& error)
{
    CloseNativeOutput();
    m_output = CreateNativeAudioOutput();
    if (!m_output) {
        error = "no native audio output backend on this platform";
        return false;
    }
    OutputRequest request;
    request.sampleRate = m_config.fixed.sampleRate;
    request.frameSize = m_config.fixed.frameSize;
    request.deviceId = m_config.fixed.outputDeviceId;
    OutputFormat actual;
    // Try with the audio thread directly; if the device rate differs, insert
    // the resampling adapter and restart so the device pulls through it.
    if (!m_output->Start(request, m_audio, actual, error)) {
        m_output.reset();
        return false;
    }
    if (actual.sampleRate != m_config.fixed.sampleRate) {
        m_output->Stop();
        m_rateAdapter.Configure(m_audio, m_config.fixed.sampleRate, actual.sampleRate,
                                static_cast<size_t>(std::max(actual.periodFrames * 4, m_config.fixed.frameSize * 8)));
        if (!m_output->Start(request, m_rateAdapter, actual, error)) {
            m_output.reset();
            return false;
        }
        SA_LOGI("[engine] native output runs at %d Hz; resampling from %d Hz", actual.sampleRate,
                m_config.fixed.sampleRate);
    }
    m_outputFormat = actual;
    m_audio.SetDeviceLatencyFrames(static_cast<uint32_t>(
        static_cast<uint64_t>(std::max(0, actual.bufferFrames)) * m_config.fixed.sampleRate /
        std::max(1, actual.sampleRate)));
    return true;
}

void AudioEngine::CloseNativeOutput()
{
    if (m_output) {
        m_output->Stop();
        m_output.reset();
    }
    m_outputFormat = OutputFormat{};
    m_audio.SetDeviceLatencyFrames(0);
}

bool AudioEngine::InitializeOutput(std::string& error)
{
    const bool enginePossible = m_hooksInstalled && m_hooks.EngineOutputPossible();
    OutputMode mode = m_config.fixed.outputMode;
    if (mode == OutputMode::Engine && !enginePossible) {
        SA_LOGW("[engine] engine output requested but the paint buffer is unavailable; using native output");
        mode = OutputMode::Native;
    }
    if (mode == OutputMode::Auto)
        mode = enginePossible ? OutputMode::Engine : OutputMode::Native;

    if (mode == OutputMode::Engine) {
        m_outputPath = AudioOutputPath::Engine;
        m_hooks.SetOutputSink(&m_audio);
        m_hooks.SetEngineOutputEnabled(true);
        SA_LOGI("[engine] output: engine paint buffer");
        return true;
    }
    m_outputPath = AudioOutputPath::Native;
    m_hooks.SetEngineOutputEnabled(false);
    m_hooks.SetOutputSink(nullptr);
    // The device is opened after the audio thread exists (it pulls from it);
    // StartThreads() calls OpenNativeOutput().
    (void)error;
    return true;
}

ProceduralSourceHooks AudioEngine::MakeSourceHooks()
{
    ProceduralSourceHooks hooks;
    hooks.allocateId = [this]() -> uint32_t { return m_nextSourceId.fetch_add(1, std::memory_order_relaxed); };
    hooks.registerSource = [this](const SoundSourcePtr& source) -> bool {
        if (!m_audio.Running())
            return false;
        if (!m_audio.AddStreamSource(source))
            return false;
        if (m_simulation.Running() && !m_simulation.AddStreamSource(source))
            SA_LOGW("[engine] simulation queue full; source %u renders without simulation", source->Id());
        return true;
    };
    hooks.unregisterSource = [this](const SoundSourcePtr& source) {
        if (!source)
            return;
        source->RequestStop();
        if (m_audio.Running())
            m_audio.RemoveStreamSource(source->Id());
        if (m_simulation.Running())
            m_simulation.RemoveStreamSource(source->Id());
    };
    return hooks;
}

bool AudioEngine::StartThreads(std::string& error)
{
    m_fallback.Initialize(m_config.fixed.frameSize, m_config.fixed.sampleRate);

    AudioThreadSetup audio;
    audio.sampleRate = m_config.fixed.sampleRate;
    audio.frameSize = m_config.fixed.frameSize;
    audio.latencyMs = m_config.fixed.outputLatencyMs;
    audio.engineLeadMs = m_config.fixed.engineLeadMs;
    audio.path = m_outputPath;
    audio.renderer = m_steamAudioReady ? &m_renderer : nullptr;
    audio.fallback = &m_fallback;
    audio.capture = m_hooksInstalled ? &m_capture : nullptr;
    audio.hooks = m_hooksInstalled ? &m_hooks : nullptr;
    audio.maxStreamSources = kMaxStreamSources;
    m_audio.SetRuntimeConfig(m_config.runtime);
    if (!m_audio.Start(audio)) {
        error = "audio thread failed to start";
        return false;
    }

    if (m_outputPath == AudioOutputPath::Native) {
        std::string outError;
        if (!OpenNativeOutput(outError)) {
            SA_LOGW("[engine] native output failed: %s", outError.c_str());
            if (m_hooksInstalled && m_hooks.EngineOutputPossible()) {
                SA_LOGI("[engine] switching to engine output");
                m_audio.Stop();
                m_outputPath = AudioOutputPath::Engine;
                audio.path = m_outputPath;
                m_hooks.SetOutputSink(&m_audio);
                m_hooks.SetEngineOutputEnabled(true);
                if (!m_audio.Start(audio)) {
                    error = "audio thread failed to restart for engine output";
                    return false;
                }
            } else {
                // Keep the pipeline alive on the null device so sources drain;
                // Tick() retries the real device periodically.
                m_output = CreateNullAudioOutput();
                OutputRequest request;
                request.sampleRate = m_config.fixed.sampleRate;
                request.frameSize = m_config.fixed.frameSize;
                OutputFormat actual;
                std::string nullError;
                if (m_output && !m_output->Start(request, m_audio, actual, nullError))
                    m_output.reset();
                m_deviceRetryTimer = kDeviceRetrySeconds;
                m_deviceRetries = 0;
            }
        }
    }

    if (m_steamAudioReady) {
        SimulationThreadSetup sim;
        sim.context = &m_context;
        sim.simulator = &m_simulator;
        sim.reflections = &m_reflections;
        sim.pathing = &m_pathing;
        sim.scene = m_scene.IsValid() ? &m_scene : nullptr;
        sim.capture = m_hooksInstalled ? &m_capture : nullptr;
        sim.hooks = m_hooksInstalled ? &m_hooks : nullptr;
        sim.audioFrameCounter = &m_audio.FrameCounter();
        sim.cacheDirectory = m_paths.cacheDirectory;
        sim.maxStreamSources = kMaxStreamSources;
        m_simulation.SetRuntimeConfig(m_config.runtime);
        if (!m_simulation.Start(sim))
            SA_LOGW("[engine] simulation thread failed to start; sources render without simulation");
    }
    return true;
}

void AudioEngine::InitializeBridges()
{
    m_procedural.Initialize(static_cast<uint32_t>(m_config.fixed.sampleRate), MakeSourceHooks());
    if (m_signatures.disableBassHooks) {
        SA_LOGI("[engine] BASS hooks disabled by configuration");
        return;
    }
    BassBridgeOptions bassOptions;
    bassOptions.moduleName = m_signatures.bassModule;
    bassOptions.muteBassOutput = true;
    std::string bassError;
    if (!m_bass.Initialize(static_cast<uint32_t>(m_config.fixed.sampleRate), bassOptions, MakeSourceHooks(),
                           bassError))
        SA_LOGI("[engine] BASS bridge not attached yet: %s (will retry)", bassError.c_str());
}

bool AudioEngine::Initialize(const EnginePaths& paths, std::string& error, ConfigOverrideFn overrides)
try {
    Shutdown();
    m_state = EngineState::Inactive;
    m_paths = paths;
    m_configOverrides = std::move(overrides);
    EnsureDirectory(m_paths.configDirectory);
    EnsureDirectory(m_paths.cacheDirectory);
    if (!m_paths.logFile.empty())
        log::Init(m_paths.logFile);
    SA_LOGI("[engine] %s %s (%d-bit) initializing", SA_MODULE_NAME, SA_VERSION_STRING, SA_ARCH_BITS);

    if (!LoadConfigs(error)) {
        SetState(EngineState::Inactive, error);
        return false;
    }

    std::string engineError;
    if (!InitializeEngineSide(engineError)) {
        // No engine module / device: nothing we can hook, but the Steam Audio
        // side can still serve Lua-driven streams and BASS through a native device.
        SA_LOGW("[engine] engine side unavailable: %s", engineError.c_str());
        m_hooksInstalled = false;
    }
    if (m_interfaces.IsDedicatedServer()) {
        ShutdownEngineSide();
        SetState(EngineState::Inactive, "dedicated server process: no local audio");
        return true;
    }

    std::string saError;
    if (!InitializeSteamAudio(saError)) {
        SA_LOGW("[engine] Steam Audio unavailable: %s", saError.c_str());
        m_steamAudioReady = false;
    }

    std::string outError;
    InitializeOutput(outError);

    std::string threadError;
    if (!StartThreads(threadError)) {
        error = threadError;
        Shutdown();
        SetState(EngineState::Inactive, error);
        return false;
    }
    InitializeBridges();

    if (!m_config.runtime.enabled) {
        m_hooks.SetPassthrough(true);
        SetState(EngineState::Passthrough, "snd_sa_enabled 0");
    } else if (m_steamAudioReady) {
        m_hooks.SetPassthrough(false);
        SetState(EngineState::Active, m_backendDescription);
    } else {
        m_hooks.SetPassthrough(false);
        SetState(EngineState::Fallback, saError.empty() ? "Steam Audio unavailable" : saError);
    }
    PropagateRuntimeConfig();
    return true;
} catch (const std::exception& failure) {
    error = std::string("audio initialization failed: ") + failure.what();
    Shutdown();
    SetState(EngineState::Inactive, error);
    return false;
}

void AudioEngine::StopThreads()
{
    // Order: stop the device (it pulls from the audio thread), detach the
    // engine sink, then threads.
    CloseNativeOutput();
    m_hooks.SetEngineOutputEnabled(false);
    m_hooks.SetOutputSink(nullptr);
    m_audio.Stop();
    m_simulation.Stop();
    DrainReleasedSources();
    m_fallback.Shutdown();
}

void AudioEngine::ShutdownSteamAudio()
{
    m_pathing.Shutdown();
    m_reflections.Shutdown();
    m_scene.Shutdown();
    m_simulator.Shutdown();
    m_renderer.Shutdown();
    if (m_backend) {
        m_backend->Shutdown();
        m_backend.reset();
    }
    m_context.Shutdown();
    m_steamAudioReady = false;
    m_devices = BackendDevices{};
}

void AudioEngine::ShutdownEngineSide()
{
    if (m_hooksInstalled) {
        m_hooks.Uninstall();
        m_hooksInstalled = false;
    }
    m_capture.Shutdown();
    m_slotSources.clear();
    m_entityList.Shutdown();
    m_fileSystem.Shutdown();
    m_gma.Invalidate();
    m_interfaces.Shutdown();
}

void AudioEngine::Shutdown()
{
    if (m_state == EngineState::Uninitialized)
        return;
    SA_LOGI("[engine] shutting down");
    m_bass.Shutdown();
    m_procedural.Shutdown();
    ResetDynamicOccluders();
    StopThreads();
    m_geometry.reset();
    m_mapName.clear();
    m_surfacePropsLoaded = false;
    m_geometryPending = false;
    m_vmtSurfaceProps.clear();
    m_materials = MaterialLibrary{};
    ShutdownSteamAudio();
    ShutdownEngineSide();
    m_output.reset();
    m_state = EngineState::Uninitialized;
    m_stateReason.clear();
    m_gameTickMicros = m_maxGameTickMicros = 0;
    m_entitySnapshotMicros = m_maxEntitySnapshotMicros = 0;
    m_occluderUpdateMicros = m_maxOccluderUpdateMicros = 0;
    log::FlushRTCounters();
    log::Shutdown();
}

bool AudioEngine::Restart(std::string& error)
{
    const EnginePaths paths = m_paths;
    const std::string map = m_mapName;
    const ConfigOverrideFn overrides = m_configOverrides;
    const StaticConfig fixed = m_pendingStatic;
    const bool fixedValid = m_pendingStaticValid;
    const float engineVolume = m_config.runtime.engineVolume;
    const float voiceScale = m_config.runtime.voiceScale;
    Shutdown();
    m_pendingStatic = fixed;
    m_pendingStaticValid = fixedValid;
    const bool ok = Initialize(paths, error, overrides);
    if (ok) {
        RuntimeConfig runtime = m_config.runtime;
        runtime.engineVolume = engineVolume;
        runtime.voiceScale = voiceScale;
        SetRuntimeConfig(runtime);
        if (!map.empty()) {
            std::string mapError;
            if (!LoadMap(map, mapError))
                SA_LOGW("[engine] map reload after restart failed: %s", mapError.c_str());
        }
    }
    return ok;
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
void AudioEngine::PropagateRuntimeConfig()
{
    m_bass.SetCaptureEnabled(m_config.runtime.enabled);
    m_audio.SetRuntimeConfig(m_config.runtime);
    m_reflections.SetPathingMemoryLimit(m_config.runtime.pathingMemoryLimitMiB);
    m_simulation.SetRuntimeConfig(m_config.runtime);
    m_occluders.Configure(OccluderOptions());
    log::SetLevel(static_cast<log::Level>(Clamp(m_config.runtime.logLevel, 0, 3)));
}

void AudioEngine::SetRuntimeConfig(const RuntimeConfig& cfg)
{
    const bool wasEnabled = m_config.runtime.enabled;
    m_config.runtime = cfg;
    if (m_state == EngineState::Uninitialized || m_state == EngineState::Inactive)
        return;
    PropagateRuntimeConfig();
    if (cfg.enabled != wasEnabled) {
        m_hooks.SetPassthrough(!cfg.enabled);
        if (!cfg.enabled)
            SetState(EngineState::Passthrough, "snd_sa_enabled 0");
        else
            SetState(m_steamAudioReady ? EngineState::Active : EngineState::Fallback,
                     m_steamAudioReady ? m_backendDescription : "Steam Audio unavailable");
    }
}

bool AudioEngine::SetStaticConfig(const StaticConfig& cfg)
{
    m_pendingStatic = cfg;
    m_pendingStaticValid = true;
    return true;
}

// ---------------------------------------------------------------------------
// Listener
// ---------------------------------------------------------------------------
void AudioEngine::SetListener(const Vec3& position, const Vec3& forward, const Vec3& right, const Vec3& up)
{
    if (!position.IsFinite() || !forward.IsFinite() || !right.IsFinite() || !up.IsFinite())
        return;
    m_externalListener.position = position;
    m_externalListener.forward = forward;
    m_externalListener.right = right;
    m_externalListener.up = up;
    m_externalListener.valid = 1;
    m_externalListenerValid = true;
    UpdateExternalListener();
}

void AudioEngine::UpdateExternalListener()
{
    if (!m_externalListenerValid)
        return;
    if (m_hooksInstalled)
        m_hooks.SetExternalListener(m_externalListener.position, m_externalListener.forward,
                                    m_externalListener.right, m_externalListener.up);
    ListenerState state;
    CoordinateConverter converter;
    converter.SetUnitsPerMeter(m_config.runtime.unitsPerMeter);
    state.frame = converter.FrameToSA(m_externalListener.position, m_externalListener.forward, m_externalListener.up);
    state.valid = true;
    m_audio.SetExternalListener(state);
    m_simulation.SetExternalListener(state);
}

// ---------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------
void AudioEngine::RecoverOutputDevice()
{
    if (m_outputPath != AudioOutputPath::Native)
        return;
    const bool lost = !m_output || m_output->NeedsRestart() ||
                      (m_output->BackendName() && std::string(m_output->BackendName()) == "null");
    if (!lost)
        return;
    if (m_deviceRetryTimer > 0.f)
        return;
    m_deviceRetryTimer = kDeviceRetrySeconds;
    if (m_output && m_output->NeedsRestart()) {
        SA_LOGI("[engine] output device changed; reopening");
        m_deviceRetries = 0;
    }
    std::string error;
    if (OpenNativeOutput(error)) {
        SA_LOGI("[engine] native output (re)opened: %s", m_outputFormat.deviceName.c_str());
        m_deviceRetries = 0;
        return;
    }
    ++m_deviceRetries;
    SA_LOGW("[engine] output reopen failed (%u/%u): %s", m_deviceRetries, kMaxDeviceRetries, error.c_str());
    if (!m_output) {
        m_output = CreateNullAudioOutput();
        OutputRequest request;
        request.sampleRate = m_config.fixed.sampleRate;
        request.frameSize = m_config.fixed.frameSize;
        OutputFormat actual;
        std::string nullError;
        if (m_output && !m_output->Start(request, m_audio, actual, nullError))
            m_output.reset();
    }
    if (m_deviceRetries >= kMaxDeviceRetries && m_hooksInstalled && m_hooks.EngineOutputPossible()) {
        SA_LOGW("[engine] giving up on native output; switching to engine output");
        std::string restartError;
        StaticConfig cfg = m_config.fixed;
        cfg.outputMode = OutputMode::Engine;
        SetStaticConfig(cfg);
        if (!Restart(restartError))
            SA_LOGE("[engine] restart for engine output failed: %s", restartError.c_str());
    }
}

void AudioEngine::DrainReleasedSources()
{
    SoundSourcePtr source;
    while (m_audio.Released().TryPop(source)) {
        if (source && m_simulation.Running())
            m_simulation.RemoveStreamSource(source->Id());
        source.reset();
    }
    while (m_simulation.Released().TryPop(source))
        source.reset();
}

void AudioEngine::Tick(float frameTime)
{
    if (m_state == EngineState::Uninitialized || m_state == EngineState::Inactive)
        return;
    const auto tickStart = std::chrono::steady_clock::now();
    frameTime = Clamp(frameTime, 0.f, 1.f);

    if (m_hooksInstalled) {
        if (m_interfaces.AudioDeviceVTableChanged()) {
            // The engine switched to a different device class: our VMT
            // patches live in the old class's vtable. Detach before
            // re-resolving so the scan sees pristine engine vtables.
            SA_LOGW("[engine] engine audio device class changed; reinstalling hooks");
            std::string error;
            m_hooks.Uninstall();
            m_interfaces.RefreshAudioDevice();
            m_hooksInstalled = m_hooks.Install(m_interfaces, m_capture, error);
            if (!m_hooksInstalled)
                SA_LOGE("[engine] hook reinstall failed: %s", error.c_str());
            else {
                m_hooks.SetPassthrough(!m_config.runtime.enabled);
                if (m_outputPath == AudioOutputPath::Engine) {
                    m_hooks.SetOutputSink(&m_audio);
                    m_hooks.SetEngineOutputEnabled(true);
                }
            }
        }
        if (m_hooksInstalled) {
            m_gameClock += static_cast<double>(frameTime);
            m_hooks.PollActiveSounds(m_config.runtime, m_localPlayer, m_gameClock);
        }
    }

    if (m_geometryPending && m_simulation.Running()) {
        m_geometryPending = !(m_geometry ? m_simulation.SetMapGeometry(m_geometry, m_mapName)
                                        : m_simulation.ClearGeometry());
    }
    m_procedural.Tick();
    m_bass.Tick(m_config.runtime);
    DrainReleasedSources();
    if (!m_geometryPending)
        UpdateDynamicOccluders(frameTime);

    if (m_deviceRetryTimer > 0.f)
        m_deviceRetryTimer -= frameTime;
    RecoverOutputDevice();

    m_flushTimer += frameTime;
    if (m_flushTimer >= kFlushIntervalSeconds) {
        m_flushTimer = 0.f;
        log::FlushRTCounters();
    }
    DrainClockEvents();
    DrainSourceEnds();
    RecordTiming(tickStart, m_gameTickMicros, m_maxGameTickMicros);
}

void AudioEngine::DrainSourceEnds()
{
    SourceEndEvent ev;
    while (m_audio.PopSourceEnd(ev)) {
        m_recentSounds[m_recentSoundsNext % kRecentSounds] = ev;
        ++m_recentSoundsNext;
    }
}

void AudioEngine::DrainClockEvents()
{
    ClockEvent ev;
    const double rate = m_config.fixed.sampleRate > 0 ? m_config.fixed.sampleRate / 1000.0 : 44.1;
    while (m_audio.PopClockEvent(ev)) {
        const double t = static_cast<double>(ev.frame) * m_config.fixed.frameSize / (rate * 1000.0);
        switch (ev.kind) {
        case ClockEvent::Kind::Anchor:
            SA_LOGI("[clock] t=%.2fs anchored %.1f ms behind the paint frontier", t, ev.backlog / rate);
            break;
        case ClockEvent::Kind::HoldBegin:
            if (m_clockHoldLogs < kMaxClockHoldLogs)
                SA_LOGI("[clock] t=%.2fs hold: engine stopped painting (backlog %.1f ms, lag %.1f ms, %u sources)", t,
                        ev.backlog / rate, ev.lag / rate, ev.activeSources);
            break;
        case ClockEvent::Kind::HoldEnd:
            if (m_clockHoldLogs < kMaxClockHoldLogs || ev.heldFrames >= 8) {
                SA_LOGI("[clock] t=%.2fs resumed after %u held frames (%.0f ms); backlog %.1f ms, lag %.1f ms", t,
                        ev.heldFrames, ev.heldFrames * m_config.fixed.frameSize / rate, ev.backlog / rate,
                        ev.lag / rate);
                if (++m_clockHoldLogs == kMaxClockHoldLogs)
                    SA_LOGI("[clock] further short holds are counted in snd_sa_status only");
            }
            break;
        case ClockEvent::Kind::Resync:
            SA_LOGI("[clock] t=%.2fs resync: skipped %.1f ms ahead (backlog %.1f ms)", t, ev.lag / rate,
                    ev.backlog / rate);
            break;
        case ClockEvent::Kind::NonFinite:
            SA_LOGW("[clock] t=%.2fs non-finite master output replaced by silence (%u sources)", t, ev.activeSources);
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Map / geometry
// ---------------------------------------------------------------------------
namespace {

constexpr size_t kMaxVmtCache = 16384;
constexpr int kMaxPatchDepth = 4;
constexpr const char* kSurfacePropManifest = "scripts/surfaceproperties_manifest.txt";
constexpr const char* kSurfacePropDefault = "scripts/surfaceproperties.txt";

} // namespace

void AudioEngine::EnsureSurfacePropDatabase()
{
    if (m_surfacePropsLoaded)
        return;
    m_surfacePropsLoaded = true;
    if (!m_config.runtime.surfacePropScripts)
        return;
    const auto start = std::chrono::steady_clock::now();
    auto database = std::make_shared<SurfacePropDatabase>();
    std::vector<std::string> files;
    std::vector<uint8_t> bytes;
    std::string error;
    if (ReadGameFile(kSurfacePropManifest, bytes, error))
        files = SurfacePropDatabase::ParseManifest(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (files.empty())
        files.push_back(kSurfacePropDefault);
    size_t loaded = 0;
    for (const std::string& file : files) {
        if (!ReadGameFile(file, bytes, error)) {
            SA_LOGD("[materials] %s: %s", file.c_str(), error.c_str());
            continue;
        }
        std::string parseError;
        const size_t added =
            database->AddScript(reinterpret_cast<const char*>(bytes.data()), bytes.size(), &parseError);
        if (added == 0)
            SA_LOGW("[materials] %s: %s", file.c_str(), parseError.c_str());
        else
            ++loaded;
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (database->Empty()) {
        SA_LOGW("[materials] no surfaceproperties scripts readable (%zu listed); using built-in aliases only",
                files.size());
        return;
    }
    SA_LOGI("[materials] %zu surfaceprops from %zu/%zu scripts in %.0f ms", database->Size(), loaded, files.size(),
            ms);
    m_materials.SetSurfaceProps(std::move(database));
}

std::string AudioEngine::ResolveSurfaceProp(const std::string& texture)
{
    if (!m_config.runtime.vmtSurfaceProps)
        return std::string();
    std::string path = TextureToVmtPath(texture);
    if (path.empty())
        return std::string();
    auto cached = m_vmtSurfaceProps.find(path);
    if (cached != m_vmtSurfaceProps.end())
        return cached->second;
    if (m_vmtSurfaceProps.size() >= kMaxVmtCache)
        m_vmtSurfaceProps.clear();
    const std::string key = path;
    ++m_vmtLookups;

    std::string result;
    std::vector<uint8_t> bytes;
    std::string error;
    for (int depth = 0; depth < kMaxPatchDepth && !path.empty(); ++depth) {
        if (!ReadGameFile(path, bytes, error))
            break;
        KvDocument doc;
        if (!ParseKeyValues(reinterpret_cast<const char*>(bytes.data()), bytes.size(), doc))
            break;
        std::string include;
        result = VmtSurfaceProp(doc, &include);
        if (!result.empty() || include.empty())
            break;
        path = TextureToVmtPath(include);
    }
    if (!result.empty())
        ++m_vmtResolved;
    m_vmtSurfaceProps.emplace(key, result);
    return result;
}

AudioEngine::SurfacePropInfo AudioEngine::DescribeTexture(const std::string& texture)
{
    EnsureSurfacePropDatabase();
    SurfacePropInfo info;
    info.texture = KvLower(texture);
    info.vmtPath = TextureToVmtPath(texture);
    info.surfaceProp = ResolveSurfaceProp(texture);
    if (!info.surfaceProp.empty()) {
        info.source = "vmt";
        info.material = m_materials.ResolveName(info.surfaceProp);
        if (const SurfacePropDatabase* db = m_materials.SurfaceProps()) {
            if (const SurfacePropEntry* entry = db->Find(info.surfaceProp)) {
                info.base = entry->base;
                info.gameMaterial = entry->gameMaterial;
            }
        }
    } else {
        info.source = "heuristic";
        info.material = m_materials.NameFromTexture(info.texture);
    }
    return info;
}

bool AudioEngine::ApplyBsp(const std::string& mapName, const std::vector<uint8_t>& bytes, std::string& error)
{
    EnsureSurfacePropDatabase();
    m_vmtSurfaceProps.clear(); // newly mounted addons may add/override materials between maps
    const size_t vmtLookupsBefore = m_vmtLookups;
    const size_t vmtResolvedBefore = m_vmtResolved;
    CoordinateConverter converter;
    converter.SetUnitsPerMeter(m_config.runtime.unitsPerMeter);
    auto geometry = std::make_shared<BspGeometry>();
    const SurfacePropResolver resolver = [this](const std::string& texture) { return ResolveSurfaceProp(texture); };
    if (!ParseBsp(bytes.data(), bytes.size(), converter, m_materials, resolver, *geometry)) {
        error = geometry->error.empty() ? "BSP parse failed" : geometry->error;
        return false;
    }
    geometry->mapName = mapName;
    SA_LOGI("[engine] map %s: %zu world triangles, %zu brush models, %zu static props (%zu faces skipped)",
            mapName.c_str(), geometry->world.triangles.size(), geometry->brushModels.size(),
            geometry->staticProps.size(), geometry->skippedFaces);
    if (m_vmtLookups != vmtLookupsBefore) {
        const size_t looked = m_vmtLookups - vmtLookupsBefore;
        const size_t resolved = m_vmtResolved - vmtResolvedBefore;
        SA_LOGI("[materials] %zu new textures: %zu with $surfaceprop from VMT, %zu by name heuristics", looked,
                resolved, looked - resolved);
    }
    ResolveStaticPropGeometry(*geometry, converter);
    m_geometry = geometry;
    m_mapName = mapName;
    if (m_simulation.Running()) {
        m_geometryPending = !m_simulation.SetMapGeometry(m_geometry, mapName);
        if (!m_geometryPending && m_config.runtime.bakeOnMapLoad && m_config.runtime.useBakedReverb)
            m_simulation.RequestBake(false);
    }
    return true;
}

void AudioEngine::ResolveStaticPropGeometry(BspGeometry& geometry, const CoordinateConverter& converter)
{
    if (!m_config.runtime.staticProps || geometry.staticProps.empty())
        return;
    const auto start = std::chrono::steady_clock::now();
    StaticPropOptions options;
    options.boxFallback = m_config.runtime.staticPropBoxFallback;
    const GameFileReader reader = [this](const std::string& path, std::vector<uint8_t>& out, std::string& error) {
        return ReadGameFile(path, out, error);
    };
    StaticPropStats stats;
    ResolveStaticProps(geometry.staticProps, reader, converter, m_materials, options, geometry.staticPropMesh, stats);
    geometry.staticPropMesh.Compact();
    geometry.staticPropsInstanced = stats.instanced;
    geometry.staticPropsMissing = stats.missingModels;
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    SA_LOGI("[props] %zu/%zu static props instanced (%zu from .phy, %zu hull boxes, %zu non-solid skipped), "
            "%zu distinct models (%zu missing), %zu triangles in %.0f ms",
            stats.instanced, stats.placements, stats.fromPhy, stats.fromBox, stats.skippedNonSolid,
            stats.distinctModels, stats.missingModels, geometry.staticPropMesh.triangles.size(), ms);
    for (const std::string& missing : stats.missing)
        SA_LOGD("[props] %s", missing.c_str());
}

bool AudioEngine::LoadMap(const std::string& mapName, std::string& error)
{
    if (mapName.empty()) {
        error = "empty map name";
        return false;
    }
    if (m_state == EngineState::Uninitialized || m_state == EngineState::Inactive) {
        error = "engine inactive";
        return false;
    }
    UnloadMap();
    std::string resolvedBy;
    std::string readError;
    if (!ReadGameFile("maps/" + mapName + ".bsp", m_bspScratch, readError, &resolvedBy)) {
        error = readError + " (use steamaudio.LoadMapData as a last resort)";
        m_mapName = mapName; // remember it so a later LoadMapData/bake has a name
        return false;
    }
    SA_LOGI("[map] maps/%s.bsp: %zu bytes via %s", mapName.c_str(), m_bspScratch.size(), resolvedBy.c_str());
    const bool ok = ApplyBsp(mapName, m_bspScratch, error);
    m_bspScratch.clear();
    m_bspScratch.shrink_to_fit();
    return ok;
}

bool AudioEngine::ReadGameFile(const std::string& relativePath, std::vector<uint8_t>& out, std::string& error,
                               std::string* resolvedBy)
{
    out.clear();
    const std::string rel = GmaArchive::NormalizePath(relativePath);
    if (rel.empty() || rel.find("..") != std::string::npos) {
        error = "invalid game path '" + relativePath + "'";
        return false;
    }
    std::string detail;
    if (m_fileSystem.Available()) {
        std::string fsError;
        if (m_fileSystem.ReadFile(rel, out, fsError)) {
            if (resolvedBy)
                *resolvedBy = "engine filesystem";
            return true;
        }
        detail = "engine fs: " + fsError;
    }
    const std::string candidates[] = {
        JoinPath(m_paths.gameDirectory, rel),
        JoinPath(JoinPath(m_paths.gameDirectory, "download"), rel),
    };
    for (const std::string& path : candidates) {
        if (ReadFile(path, out)) {
            if (resolvedBy)
                *resolvedBy = path;
            return true;
        }
    }
    std::string gmaError;
    std::string archive;
    if (m_gma.Find(rel, out, gmaError, &archive)) {
        if (resolvedBy)
            *resolvedBy = archive;
        return true;
    }
    error = rel + " not found" + (detail.empty() ? "" : " [" + detail + "]") + " [disk: not in " +
            m_paths.gameDirectory + "] [gma: " + gmaError + "]";
    return false;
}

bool AudioEngine::GameFileExists(const std::string& relativePath)
{
    const std::string rel = GmaArchive::NormalizePath(relativePath);
    if (rel.empty() || rel.find("..") != std::string::npos)
        return false;
    if (m_fileSystem.Available() && m_fileSystem.FileExists(rel))
        return true;
    if (FileExists(JoinPath(m_paths.gameDirectory, rel)) ||
        FileExists(JoinPath(JoinPath(m_paths.gameDirectory, "download"), rel)))
        return true;
    return m_gma.Contains(rel);
}

bool AudioEngine::LoadMapFromMemory(const std::string& mapName, const uint8_t* data, size_t size, std::string& error)
{
    if (!data || size < 1024) {
        error = "BSP data too small";
        return false;
    }
    if (m_state == EngineState::Uninitialized || m_state == EngineState::Inactive) {
        error = "engine inactive";
        return false;
    }
    UnloadMap();
    std::vector<uint8_t> bytes(data, data + size);
    return ApplyBsp(mapName, bytes, error);
}

void AudioEngine::UnloadMap()
{
    ResetDynamicOccluders();
    m_geometryPending = false;
    if (m_simulation.Running()) {
        m_simulation.CancelBake();
        m_geometryPending = !m_simulation.ClearGeometry();
    }
    m_geometry.reset();
    m_mapName.clear();
}

// ---------------------------------------------------------------------------
// Dynamic occluders
// ---------------------------------------------------------------------------
namespace {

// IDynamicGeometrySink over the simulation thread's command queue.
class SimulationGeometrySink final : public IDynamicGeometrySink {
public:
    explicit SimulationGeometrySink(SimulationThread& simulation) : m_simulation(simulation) {}

    DynamicGeometryId AddMesh(std::shared_ptr<const MeshData> localMesh, const Transform& transform,
                              const std::string& debugName) override
    {
        if (!m_simulation.Running())
            return SceneBuilder::kInvalidDynamic;
        const DynamicGeometryId id = m_simulation.AllocDynamicId();
        if (!m_simulation.AddDynamicGeometry(id, std::move(localMesh), transform, debugName))
            return SceneBuilder::kInvalidDynamic;
        return id;
    }
    bool UpdateMesh(DynamicGeometryId id, const Transform& transform) override
    {
        return m_simulation.Running() && m_simulation.UpdateDynamicGeometry(id, transform);
    }
    bool RemoveMesh(DynamicGeometryId id) override
    {
        return m_simulation.Running() && m_simulation.RemoveDynamicGeometry(id);
    }
    bool UpdateBrushModel(int32_t modelIndex, const Vec3& origin, const Vec3& angles) override
    {
        return m_simulation.Running() && m_simulation.UpdateBrushModel(modelIndex, origin, angles);
    }

private:
    SimulationThread& m_simulation;
};

} // namespace

DynamicOccluderOptions AudioEngine::OccluderOptions() const
{
    const RuntimeConfig& rt = m_config.runtime;
    DynamicOccluderOptions o;
    o.enabled = rt.dynamicGeometry || rt.dynamicProps || rt.dynamicPlayers;
    o.brushModels = rt.dynamicGeometry;
    o.props = rt.dynamicProps;
    o.players = rt.dynamicPlayers;
    o.boxFallback = rt.staticPropBoxFallback;
    o.maxOccluders = Clamp(rt.dynamicMaxOccluders, 0, 4096);
    o.rangeUnits = rt.dynamicRangeUnits;
    o.minExtentUnits = std::max(0.f, rt.dynamicMinExtentUnits);
    o.unitsPerMeter = rt.unitsPerMeter;
    return o;
}

bool AudioEngine::ListenerPosition(Vec3& out) const
{
    if (m_hooksInstalled) {
        const EngineListener l = m_hooks.GetListener();
        if (l.valid) {
            out = l.position;
            return true;
        }
    }
    if (m_externalListenerValid) {
        out = m_externalListener.position;
        return true;
    }
    return false;
}

void AudioEngine::BeginLuaEntities()
{
    m_luaEntities.clear();
    m_luaBatchOpen = true;
}

void AudioEngine::PushLuaEntity(const EntitySnapshot& entity)
{
    if (!m_luaBatchOpen || entity.index <= 0)
        return;
    if (m_luaEntities.size() >= static_cast<size_t>(m_signatures.entityList.maxEntities))
        return;
    m_luaEntities.push_back(entity);
}

void AudioEngine::EndLuaEntities()
{
    if (!m_luaBatchOpen)
        return;
    m_luaBatchOpen = false;
    m_luaEntitiesReady.swap(m_luaEntities);
    m_luaEntities.clear();
    m_luaBatchReady = true;
}

void AudioEngine::ResetDynamicOccluders()
{
    SimulationGeometrySink sink(m_simulation);
    m_occluders.Reset(sink);
    m_entityList.Invalidate();
    m_luaEntities.clear();
    m_luaEntitiesReady.clear();
    m_luaBatchOpen = false;
    m_luaBatchReady = false;
    m_nativeEntitiesActive = false;
    m_occluderTimer = 0.f;
}

void AudioEngine::UpdateDynamicOccluders(float frameTime)
{
    if (!m_simulation.Running() || m_mapName.empty())
        return;
    const RuntimeConfig& rt = m_config.runtime;
    if (!rt.dynamicGeometry && !rt.dynamicProps && !rt.dynamicPlayers) {
        if (m_occluders.GetStats().tracked > 0) {
            SimulationGeometrySink sink(m_simulation);
            m_occluders.Clear(sink);
        }
        m_luaBatchReady = false;
        return;
    }

    m_occluderTimer += frameTime;
    const float interval = Clamp(rt.dynamicUpdateIntervalMs, 16.f, 5000.f) * 0.001f;
    if (m_occluderTimer < interval)
        return;
    m_occluderTimer = 0.f;

    Vec3 listener{};
    const bool listenerValid = ListenerPosition(listener);
    m_occluders.SetLocalPlayer(m_localPlayer);
    SimulationGeometrySink sink(m_simulation);

    if (rt.nativeEntityList && m_entityList.GetState() != ClientEntityList::State::Failed &&
        m_entityList.GetState() != ClientEntityList::State::Uninitialized) {
        m_entityScratch.clear();
        const auto snapshotStart = std::chrono::steady_clock::now();
        const bool snapshotReady = m_entityList.Snapshot(m_mapName, m_localPlayer, m_entityScratch);
        RecordTiming(snapshotStart, m_entitySnapshotMicros, m_maxEntitySnapshotMicros);
        if (snapshotReady) {
            if (!m_nativeEntitiesActive) {
                SA_LOGI("[ents] native entity walk validated (%s); Lua entity walk no longer needed",
                        m_entityList.Description().c_str());
                m_nativeEntitiesActive = true;
            }
            m_luaBatchReady = false;
            const auto updateStart = std::chrono::steady_clock::now();
            m_occluders.Update(m_entityScratch, listener, listenerValid, sink);
            RecordTiming(updateStart, m_occluderUpdateMicros, m_maxOccluderUpdateMicros);
            return;
        }
        if (m_nativeEntitiesActive) {
            SA_LOGW("[ents] native entity walk stopped (%s); falling back to the Lua entity walk",
                    m_entityList.LastError().c_str());
            m_nativeEntitiesActive = false;
        }
    } else {
        m_nativeEntitiesActive = false;
    }

    if (m_luaBatchReady) {
        m_luaBatchReady = false;
        const auto updateStart = std::chrono::steady_clock::now();
        m_occluders.Update(m_luaEntitiesReady, listener, listenerValid, sink);
        RecordTiming(updateStart, m_occluderUpdateMicros, m_maxOccluderUpdateMicros);
    }
}

bool AudioEngine::UpdateBrushModel(int32_t modelIndex, const Vec3& origin, const Vec3& angles)
{
    if (!m_simulation.Running() || !m_config.runtime.dynamicGeometry)
        return false;
    return m_simulation.UpdateBrushModel(modelIndex, origin, angles);
}

DynamicGeometryId AudioEngine::AddDynamicMesh(const std::vector<Vec3>& triangles, const std::string& material,
                                              const Vec3& origin, const Vec3& angles, const std::string& name)
{
    if (!m_simulation.Running() || triangles.size() < 3)
        return SceneBuilder::kInvalidDynamic;
    CoordinateConverter converter;
    converter.SetUnitsPerMeter(m_config.runtime.unitsPerMeter);
    auto mesh = std::make_shared<MeshData>();
    const IPLint32 mat = mesh->AddMaterial(m_materials.FromSurfaceProp(material));
    for (size_t i = 0; i + 2 < triangles.size(); i += 3) {
        mesh->AddTriangle(converter.PositionToSA(triangles[i]).ToIPL(), converter.PositionToSA(triangles[i + 1]).ToIPL(),
                          converter.PositionToSA(triangles[i + 2]).ToIPL(), mat);
    }
    mesh->Compact();
    if (mesh->Empty())
        return SceneBuilder::kInvalidDynamic;
    const DynamicGeometryId id = m_simulation.AllocDynamicId();
    if (!m_simulation.AddDynamicGeometry(id, mesh, Transform::FromAngles(origin, angles), name))
        return SceneBuilder::kInvalidDynamic;
    return id;
}

bool AudioEngine::UpdateDynamicMesh(DynamicGeometryId id, const Vec3& origin, const Vec3& angles)
{
    if (!m_simulation.Running())
        return false;
    return m_simulation.UpdateDynamicGeometry(id, Transform::FromAngles(origin, angles));
}

bool AudioEngine::RemoveDynamicMesh(DynamicGeometryId id)
{
    if (!m_simulation.Running())
        return false;
    return m_simulation.RemoveDynamicGeometry(id);
}

bool AudioEngine::RequestBake(bool force)
{
    if (!m_simulation.Running() || !m_geometry)
        return false;
    return m_simulation.RequestBake(force);
}

bool AudioEngine::CancelBake()
{
    if (!m_simulation.Running())
        return false;
    return m_simulation.CancelBake();
}

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------
EngineStatus AudioEngine::Status() const
{
    EngineStatus s;
    s.state = m_state;
    s.stateReason = m_stateReason;
    s.backend = m_backendDescription;
    s.sceneType = m_steamAudioReady ? SceneTypeName(m_scene.SceneType()) : "";
    s.outputPath = m_outputPath == AudioOutputPath::Engine ? "engine" : "native";
    s.outputDevice = m_output ? m_outputFormat.deviceName : (m_outputPath == AudioOutputPath::Engine ? "engine" : "");
    s.audioDeviceClass = m_interfaces.AudioDevice().className;
    s.mapName = m_mapName;
    s.geometryPending = m_geometryPending;
    s.sampleRate = m_config.fixed.sampleRate;
    s.frameSize = m_config.fixed.frameSize;
    s.hrtf = m_steamAudioReady && m_renderer.IsValid() && m_config.fixed.hrtfEnabled && m_config.runtime.hrtf;
    s.mixerHooked = m_hooksInstalled;
    s.listenerFromEngine = m_hooksInstalled && m_hooks.ListenerFromEngine();
    s.bassAttached = m_bass.Attached();
    const SimulationThreadStats& sim = m_simulation.Stats();
    s.bakeRunning = m_reflections.BakeInProgress();
    s.bakeProgress = m_reflections.BakeProgress();
    s.bakedDataLoaded = sim.bakedDataAttached.load(std::memory_order_relaxed);
    const AudioThreadStats& audio = m_audio.Stats();
    s.audioFrames = audio.framesRendered.load(std::memory_order_relaxed);
    s.audioUnderruns = audio.outputUnderruns.load(std::memory_order_relaxed) + (m_output ? m_output->Underruns() : 0);
    s.engineStarves = audio.engineStarves.load(std::memory_order_relaxed);
    s.clockResyncs = audio.clockResyncs.load(std::memory_order_relaxed);
    s.nonFiniteFrames = audio.nonFiniteFrames.load(std::memory_order_relaxed);
    if (m_hooksInstalled) {
        s.clockRebases = m_capture.Stats().clockRebases.load(std::memory_order_relaxed);
        const uint64_t frontier = m_capture.PaintedFrontier();
        const uint64_t render = m_audio.RenderClock();
        s.engineLeadSamples = frontier > render ? frontier - render : 0;
    }
    s.activeSources = audio.activeSources.load(std::memory_order_relaxed);
    s.spatializedSources = audio.spatializedSources.load(std::memory_order_relaxed);
    s.renderMicros = audio.lastRenderMicros.load(std::memory_order_relaxed);
    s.maxRenderMicros = audio.maxRenderMicros.load(std::memory_order_relaxed);
    s.peak = audio.peak.load(std::memory_order_relaxed);
    s.roomPreset = audio.roomPreset.load(std::memory_order_relaxed);
    s.roomSends = audio.roomSends.load(std::memory_order_relaxed);
    s.playerLowpassHz = audio.playerLowpassHz.load(std::memory_order_relaxed);
    s.audioPriorityElevated = audio.priorityElevated.load(std::memory_order_relaxed);
    s.simulationTicks = sim.ticks.load(std::memory_order_relaxed);
    s.simulationDirectRuns = sim.directRuns.load(std::memory_order_relaxed);
    s.simulationReflectionRuns = sim.reflectionRuns.load(std::memory_order_relaxed);
    s.simulationPathingRuns = sim.pathingRuns.load(std::memory_order_relaxed);
    s.simulationSources = sim.simulatedSources.load(std::memory_order_relaxed);
    s.simulationMicros = sim.lastDirectMicros.load(std::memory_order_relaxed) +
                         sim.lastReflectionMicros.load(std::memory_order_relaxed) +
                         sim.lastPathingMicros.load(std::memory_order_relaxed);
    s.simulationTickMicros = sim.lastTickMicros.load(std::memory_order_relaxed);
    s.maxSimulationTickMicros = sim.maxTickMicros.load(std::memory_order_relaxed);
    s.simulationCommandMicros = sim.lastCommandMicros.load(std::memory_order_relaxed);
    s.maxSimulationCommandMicros = sim.maxCommandMicros.load(std::memory_order_relaxed);
    s.sceneCommitMicros = sim.lastSceneCommitMicros.load(std::memory_order_relaxed);
    s.maxSceneCommitMicros = sim.maxSceneCommitMicros.load(std::memory_order_relaxed);
    s.sceneCommits = sim.sceneCommits.load(std::memory_order_relaxed);
    s.gameTickMicros = m_gameTickMicros;
    s.maxGameTickMicros = m_maxGameTickMicros;
    s.entitySnapshotMicros = m_entitySnapshotMicros;
    s.maxEntitySnapshotMicros = m_maxEntitySnapshotMicros;
    s.occluderUpdateMicros = m_occluderUpdateMicros;
    s.maxOccluderUpdateMicros = m_maxOccluderUpdateMicros;
    s.staticTriangles = sim.staticTriangles.load(std::memory_order_relaxed);
    s.dynamicMeshes = sim.dynamicMeshes.load(std::memory_order_relaxed);
    if (m_geometry) {
        s.staticProps = m_geometry->staticPropsInstanced;
        s.staticPropTriangles = m_geometry->staticPropMesh.triangles.size();
        s.staticPropsMissing = m_geometry->staticPropsMissing;
    }
    s.surfacePropEntries = m_materials.SurfaceProps() ? m_materials.SurfaceProps()->Size() : 0;
    s.vmtLookups = m_vmtLookups;
    s.vmtResolved = m_vmtResolved;
    s.bassChannels = m_bass.Stats().channels.load(std::memory_order_relaxed);
    s.proceduralStreams = m_procedural.Count();
    s.hookCalls = m_hooksInstalled ? m_hooks.HookCalls() : 0;
    if (m_hooksInstalled) {
        const ChannelCaptureStats& cap = m_capture.Stats();
        s.captureMixCalls = cap.mixCalls.load(std::memory_order_relaxed);
        s.capturePaintIterations = cap.paintIterations.load(std::memory_order_relaxed);
        s.captureBlocks44k = cap.blocks44k.load(std::memory_order_relaxed);
        s.captureBlocks22k = cap.blocks22k.load(std::memory_order_relaxed);
        s.captureBlocks11k = cap.blocks11k.load(std::memory_order_relaxed);
        s.captureBlocksPartial = cap.blocksPartial.load(std::memory_order_relaxed);
        s.captureDuplicateBlocks = cap.duplicateBlocks.load(std::memory_order_relaxed);
        s.captureLateBlocks = cap.lateBlocks.load(std::memory_order_relaxed);
        s.captureSlotOverflow = cap.slotOverflow.load(std::memory_order_relaxed);
    }
    s.emitSoundHooked = m_hooksInstalled && m_hooks.EmitSoundHooked();
    s.emitSoundCalls = m_hooksInstalled ? m_hooks.EmitSoundCalls() : 0;
    s.namesFromHandles = m_hooks.NamesFromHandles();
    s.autoDirectivity = m_hooks.AutoDirectivityApplied();
    s.emitterHullPushes = m_hooks.EmitterHullPushes();
    s.soundNameResolver = m_fileSystem.FileNameResolverStatus();
    s.overrides = m_hooks.Overrides().GetStats();
    const RuntimeConfig& rt = m_config.runtime;
    s.entitySource = (!rt.dynamicGeometry && !rt.dynamicProps && !rt.dynamicPlayers) ? "off"
                     : m_nativeEntitiesActive                                        ? "native"
                                                                                     : "lua";
    switch (m_entityList.GetState()) {
    case ClientEntityList::State::Uninitialized:
        s.entityListState = rt.nativeEntityList ? "uninitialized" : "disabled (snd_sa_native_entities 0)";
        break;
    case ClientEntityList::State::Unvalidated:
        s.entityListState = "resolved (" + m_entityList.Description() + "), awaiting in-map validation" +
                            (m_entityList.LastError().empty() ? "" : ": " + m_entityList.LastError());
        break;
    case ClientEntityList::State::Validated:
        s.entityListState = "validated (" + m_entityList.Description() + ")";
        break;
    case ClientEntityList::State::Failed:
        s.entityListState = "failed: " + m_entityList.LastError();
        break;
    }
    s.entityList = m_entityList.GetStats();
    s.occluders = m_occluders.GetStats();
    return s;
}

namespace {

const char* RenderModeName(uint8_t mode)
{
    switch (mode) {
    case RenderDiag::kIdle: return "idle";
    case RenderDiag::kPassthrough: return "2d";
    case RenderDiag::kNoEffects: return "fallback(no-effects)";
    case RenderDiag::kListenerRelative: return "head+acoustics";
    case RenderDiag::kHrtf: return "hrtf";
    case RenderDiag::kPanning: return "pan";
    case RenderDiag::kFallbackMixer: return "fallback";
    default: return "?";
    }
}

// Listener-relative direction of `pos` (Source units) as the renderer sees it:
// x right, y up, z ahead, all in [-1, 1].
Vec3 RelativeDirection(const EngineListener& l, const Vec3& pos, float& distUnits)
{
    const Vec3 rel = pos - l.position;
    distUnits = rel.Length();
    if (distUnits < 1e-3f)
        return Vec3{};
    const Vec3 dir = rel * (1.f / distUnits);
    return Vec3{dir.Dot(l.right), dir.Dot(l.up), dir.Dot(l.forward)};
}

} // namespace

std::vector<std::string> AudioEngine::DescribeSounds() const
{
    std::vector<std::string> lines;
    char buf[512];
    EngineListener l;
    const char* listenerSource = "none";
    if (m_hooksInstalled && m_hooks.GetListener().valid) {
        l = m_hooks.GetListener();
        listenerSource = m_hooks.ListenerFromEngine() ? "engine hook" : "lua (via hooks)";
    } else if (m_externalListenerValid) {
        l = m_externalListener;
        listenerSource = "lua";
    }
    std::snprintf(buf, sizeof(buf),
                  "listener [%s]: pos (%.0f %.0f %.0f) fwd (%.2f %.2f %.2f) right (%.2f %.2f %.2f) up (%.2f %.2f %.2f) "
                  "local player %d",
                  listenerSource, static_cast<double>(l.position.x), static_cast<double>(l.position.y),
                  static_cast<double>(l.position.z), static_cast<double>(l.forward.x),
                  static_cast<double>(l.forward.y), static_cast<double>(l.forward.z), static_cast<double>(l.right.x),
                  static_cast<double>(l.right.y), static_cast<double>(l.right.z), static_cast<double>(l.up.x),
                  static_cast<double>(l.up.y), static_cast<double>(l.up.z), m_localPlayer);
    lines.emplace_back(buf);
    if (!m_hooksInstalled) {
        lines.emplace_back("mixer hooks not installed: no engine channels captured");
        return lines;
    }

    const ChannelLayout& layout = m_capture.Layout();
    std::snprintf(buf, sizeof(buf), "channel_t layout: origin %d guid %d dist_mult %d fvolume %d stride %d (%s)",
                  layout.origin, layout.guid, layout.distMult, layout.fvolume, layout.stride,
                  m_capture.LayoutStable() ? "stable" : "inferring");
    lines.emplace_back(buf);
    std::snprintf(buf, sizeof(buf), "effect pool: %zu free, %llu tails cut, %llu exhausted",
                  m_renderer.FreeEffectSets(), static_cast<unsigned long long>(m_renderer.TailsCut()),
                  static_cast<unsigned long long>(m_renderer.PoolExhausted()));
    lines.emplace_back(buf);
    const MixBalance balance = m_renderer.Balance();
    const auto db = [](float meanSquare) {
        return meanSquare > 1e-12f ? 10.0 * std::log10(static_cast<double>(meanSquare)) : -120.0;
    };
    std::snprintf(buf, sizeof(buf),
                  "master (last ~100 ms): L %.1f dB  R %.1f dB  (L-R %+.1f dB)  direct %.1f dB  reflections %.1f dB  "
                  "2d %.1f dB",
                  db(balance.masterL), db(balance.masterR), db(balance.masterL) - db(balance.masterR),
                  db(balance.direct), db(balance.reflections), db(balance.nonSpatial));
    lines.emplace_back(buf);

    const std::vector<ActiveSoundInfo>& sounds = m_hooks.ActiveSounds();
    std::snprintf(buf, sizeof(buf), "engine sounds: %zu", sounds.size());
    lines.emplace_back(buf);
    for (const ActiveSoundInfo& s : sounds) {
        std::string pos = "pos n/a";
        if (s.hasOrigin) {
            float dist = 0.f;
            const Vec3 rel = RelativeDirection(l, s.origin, dist);
            std::snprintf(buf, sizeof(buf), "pos (%.0f %.0f %.0f) dist %.0f rel(r/u/f) %.2f/%.2f/%.2f",
                          static_cast<double>(s.origin.x), static_cast<double>(s.origin.y),
                          static_cast<double>(s.origin.z), static_cast<double>(dist), static_cast<double>(rel.x),
                          static_cast<double>(rel.y), static_cast<double>(rel.z));
            pos = buf;
        } else if (s.originPtr != 0) {
            pos = "pos unreadable";
        }
        std::snprintf(buf, sizeof(buf), "  guid %d ent %d ch %d vol %.2f %s%s%s%s slot %d  %s  %s", s.guid, s.entity,
                      s.channel, static_cast<double>(s.volume), s.fromServer ? "srv " : "", s.dryMix ? "drymix " : "",
                      s.updatePositions ? "upd " : "", s.sentence ? "sentence " : "", s.slot, pos.c_str(),
                      s.name.empty() ? "" : s.name.c_str());
        lines.emplace_back(buf);
    }

    std::vector<std::string> slotLines;
    for (uint32_t i = 0; i < m_capture.SlotCount(); ++i) {
        const CaptureSlot& slot = m_capture.Slot(i);
        const uintptr_t ch = slot.enginePtr.load(std::memory_order_acquire);
        if (ch == 0 || !slot.source)
            continue;
        const SourceParams p = slot.source->GetParams();
        const RenderDiag d = slot.source->LoadRenderDiag();
        int32_t guid = 0;
        m_capture.ReadGuid(ch, guid);
        Vec3 chOrigin;
        const bool haveChOrigin = m_capture.ReadOrigin(ch, chOrigin);
        std::string origin = "ch.origin n/a";
        if (haveChOrigin) {
            std::snprintf(buf, sizeof(buf), "ch.origin (%.0f %.0f %.0f)", static_cast<double>(chOrigin.x),
                          static_cast<double>(chOrigin.y), static_cast<double>(chOrigin.z));
            origin = buf;
        }
        std::snprintf(buf, sizeof(buf),
                      "  slot %u guid %d ent %d %s pos%s (%.0f %.0f %.0f) %s  engine dir (%.2f %.2f %.2f) gain %.2f  "
                      "render %s dir(r/u/-f) %.2f/%.2f/%.2f dist %.1fm att %.2f occl %.2f (raw %.2f, min %.2f)%s  "
                      "in %u ch @%u Hz",
                      i, guid, p.entityIndex, p.spatialize ? "3d" : "2d", p.positionValid ? "" : "(invalid)",
                      static_cast<double>(p.position.x), static_cast<double>(p.position.y),
                      static_cast<double>(p.position.z), origin.c_str(),
                      static_cast<double>(slot.engineDirX.load(std::memory_order_relaxed)),
                      static_cast<double>(slot.engineDirY.load(std::memory_order_relaxed)),
                      static_cast<double>(slot.engineDirZ.load(std::memory_order_relaxed)),
                      static_cast<double>(slot.engineGain.load(std::memory_order_relaxed)), RenderModeName(d.mode),
                      static_cast<double>(d.direction.x), static_cast<double>(d.direction.y),
                      static_cast<double>(d.direction.z), static_cast<double>(d.distanceMeters),
                      static_cast<double>(d.distanceAttenuation), static_cast<double>(d.occlusion),
                      static_cast<double>(d.occlusionRaw), static_cast<double>(d.minOcclusionRaw),
                      d.simOutputs ? "" : " (no sim)", slot.inputChannels.load(std::memory_order_relaxed),
                      slot.inputRate.load(std::memory_order_relaxed));
        std::string line = buf;
        std::snprintf(buf, sizeof(buf),
                      "\n      levels: captured %.1f dB (%u blocks, %u late, %u dup)  render in %.1f dB  out %.1f dB "
                      "(peak %.1f)  gain %.2f  frames %u (%u no data, %u refl, %u path)",
                      db(slot.capturedLevel.load(std::memory_order_relaxed)),
                      slot.blocksWritten.load(std::memory_order_relaxed), slot.blocksLate.load(std::memory_order_relaxed),
                      slot.blocksDuplicate.load(std::memory_order_relaxed), db(d.inputLevel), db(d.outputLevel),
                      db(d.peakOutputLevel), static_cast<double>(d.gain), d.framesRendered, d.framesNoData,
                      d.framesReflections, d.framesPathing);
        line += buf;
        slotLines.emplace_back(std::move(line));
    }
    std::snprintf(buf, sizeof(buf), "capture slots bound: %zu", slotLines.size());
    lines.emplace_back(buf);
    lines.insert(lines.end(), slotLines.begin(), slotLines.end());
    const auto bassLines = m_bass.DescribeChannels();
    lines.insert(lines.end(), bassLines.begin(), bassLines.end());

    // Sources that finished rendering, oldest first: what a short sound
    // (footstep, gunshot) went through before it was gone.
    const uint64_t nowFrame = m_audio.FrameCounter().load(std::memory_order_relaxed);
    const double frameMs = m_config.fixed.sampleRate > 0
                               ? 1000.0 * m_config.fixed.frameSize / m_config.fixed.sampleRate
                               : 0.0;
    const size_t count = static_cast<size_t>(std::min<uint64_t>(m_recentSoundsNext, kRecentSounds));
    std::snprintf(buf, sizeof(buf), "recently ended: %zu", count);
    lines.emplace_back(buf);
    for (size_t k = 0; k < count; ++k) {
        const SourceEndEvent& e = m_recentSounds[(m_recentSoundsNext - count + k) % kRecentSounds];
        float dist = 0.f;
        const Vec3 rel = RelativeDirection(l, e.position, dist);
        std::snprintf(buf, sizeof(buf),
                      "  -%.1fs ent %d %s pos (%.0f %.0f %.0f) rel(r/u/f) %.2f/%.2f/%.2f  %s %u frames (%u no data, "
                      "%u refl, %u path)  peak out %.1f dB  att %.2f  occl %.2f (raw %.2f, min %.2f)%s",
                      static_cast<double>(nowFrame - e.frame) * frameMs / 1000.0, e.entity, e.spatialize ? "3d" : "2d",
                      static_cast<double>(e.position.x), static_cast<double>(e.position.y),
                      static_cast<double>(e.position.z), static_cast<double>(rel.x), static_cast<double>(rel.y),
                      static_cast<double>(rel.z), RenderModeName(e.diag.mode), e.diag.framesRendered,
                      e.diag.framesNoData, e.diag.framesReflections, e.diag.framesPathing, db(e.diag.peakOutputLevel),
                      static_cast<double>(e.diag.distanceAttenuation), static_cast<double>(e.diag.occlusion),
                      static_cast<double>(e.diag.occlusionRaw), static_cast<double>(e.diag.minOcclusionRaw),
                      e.diag.simOutputs ? "" : " (no sim)");
        lines.emplace_back(buf);
    }
    return lines;
}

bool AudioEngine::EngineSoundAvailable() const
{
    return m_interfaces.EngineSound() != nullptr && !m_interfaces.IsDedicatedServer();
}

int32_t AudioEngine::LastEmittedSoundGuid() const
{
    src::IEngineSound* snd = EngineSoundAvailable() ? m_interfaces.EngineSound() : nullptr;
    return snd ? snd->GetGuidForLastSoundEmitted() : 0;
}

bool AudioEngine::IsSoundStillPlaying(int32_t guid) const
{
    src::IEngineSound* snd = EngineSoundAvailable() ? m_interfaces.EngineSound() : nullptr;
    return snd && guid != 0 && snd->IsSoundStillPlaying(guid);
}

} // namespace sa
