// tests/TestHrtfRenderer.cpp
//
// End-to-end check of the Steam Audio rendering chain (coordinate conversion,
// listener frame, binaural effect, master mix) against the real runtime.
// Runs only when SA_PHONON_DIR points at a directory containing the Steam
// Audio library for the host platform; skipped otherwise so the default test
// run has no external dependency.
#include "TestFramework.h"
#include "BspFixture.h"
#include "steamaudio/BspGeometry.h"
#include "steamaudio/ReflectionSimulator.h"
#include "steamaudio/CPUFallbackBackend.h"
#include "steamaudio/PathingSimulator.h"
#include "steamaudio/PhyModel.h"
#include "steamaudio/GPUBackend.h"
#include "platform/PlatformAudioOutput.h"
#include "util/Logging.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <type_traits>
#include "mixing/SoundSource.h"
#include "mixing/SimulationThread.h"
#include "steamaudio/Config.h"
#include "steamaudio/HRTFRenderer.h"
#include "steamaudio/PhononContext.h"
#include "steamaudio/Simulator.h"
#include "util/Math.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace sa;

namespace {

struct Channels {
    double left = 0.0;
    double right = 0.0;
};

const char* PhononDir()
{
    const char* dir = std::getenv("SA_PHONON_DIR");
    return (dir && *dir) ? dir : nullptr;
}

// Renders a mono noise burst placed at `sourcePos` (Source units) for a
// listener at the origin looking down +X and returns per-ear RMS.
Channels RenderAt(PhononContext& ctx, HRTFRenderer& renderer, const Vec3& sourcePos, bool hrtf)
{
    const int32_t frame = ctx.FrameSize();
    SoundSource source(1, SourceKind::EngineChannel, 1, static_cast<uint32_t>(ctx.SampleRate()),
                       static_cast<uint32_t>(ctx.SampleRate()), 8192);
    SourceParams params;
    params.position = sourcePos;
    params.positionValid = 1;
    params.spatialize = 1;
    params.distMult = 0.f; // no distance attenuation: compare ears only
    params.reflections = 0;
    params.pathing = 0;
    source.SetParams(params);
    source.Render().effects = renderer.AcquireEffects();
    SA_CHECK(source.Render().effects != nullptr);

    RuntimeConfig cfg;
    cfg.hrtf = hrtf;
    cfg.reflections = false;
    cfg.pathing = false;
    cfg.doppler = false;

    CoordinateConverter converter;
    converter.SetUnitsPerMeter(cfg.unitsPerMeter);
    ListenerState listener;
    listener.frame = converter.FrameToSA(Vec3{0.f, 0.f, 0.f}, Vec3{1.f, 0.f, 0.f}, Vec3{0.f, 0.f, 1.f});
    listener.valid = true;

    std::vector<float> mono(static_cast<size_t>(frame));
    const float* inputs[1] = {mono.data()};
    uint32_t seed = 12345u;
    Channels rms;
    size_t counted = 0;
    // Several frames so the HRTF filter state settles before measuring.
    for (int f = 0; f < 8; ++f) {
        for (float& s : mono) {
            seed = seed * 1664525u + 1013904223u;
            s = (static_cast<float>(seed >> 8) / 8388608.f) - 1.f;
        }
        renderer.BeginFrame(cfg, listener);
        renderer.RenderSource(source, inputs, 1, 1.f);
        renderer.EndFrame();
        if (f < 2)
            continue;
        const float* l = renderer.MasterLeft();
        const float* r = renderer.MasterRight();
        for (int32_t i = 0; i < frame; ++i) {
            rms.left += static_cast<double>(l[i]) * l[i];
            rms.right += static_cast<double>(r[i]) * r[i];
        }
        counted += static_cast<size_t>(frame);
    }
    renderer.ReleaseEffects(source.Render().effects);
    source.Render().effects = nullptr;
    rms.left = std::sqrt(rms.left / static_cast<double>(counted));
    rms.right = std::sqrt(rms.right / static_cast<double>(counted));
    return rms;
}

double Db(double a, double b) { return 20.0 * std::log10((a + 1e-12) / (b + 1e-12)); }

float IPLCALL TestDistanceCallback(IPLfloat32 distanceMeters, void* userData)
{
    const float* distMult = static_cast<const float*>(userData);
    return SourceDistanceGain(*distMult, distanceMeters * 52.49f);
}

void AddAcousticBox(MeshData& mesh, const Vec3& lo, const Vec3& hi, const IPLMaterial& material)
{
    std::vector<Vec3> vertices;
    std::vector<PhyTriangle> triangles;
    AppendBox(lo, hi, vertices, triangles);
    const int32_t index = mesh.AddMaterial(material);
    for (const auto& triangle : triangles)
        mesh.AddTriangle(vertices[triangle.indices[0]].ToIPL(), vertices[triangle.indices[1]].ToIPL(),
                         vertices[triangle.indices[2]].ToIPL(), index);
}

struct TwoRoomScene {
    MeshData walls, door;
    TwoRoomScene()
    {
        IPLMaterial material{};
        for (float& band : material.absorption) band = 0.2f;
        material.transmission[0] = 0.05f;
        material.transmission[1] = 0.02f;
        material.transmission[2] = 0.01f;
        material.scattering = 0.5f;
        AddAcousticBox(walls, {-6.f, -0.25f, -4.f}, {6.f, 0.f, 4.f}, material);
        AddAcousticBox(walls, {-6.f, 3.f, -4.f}, {6.f, 3.25f, 4.f}, material);
        AddAcousticBox(walls, {-6.25f, 0.f, -4.f}, {-6.f, 3.f, 4.f}, material);
        AddAcousticBox(walls, {6.f, 0.f, -4.f}, {6.25f, 3.f, 4.f}, material);
        AddAcousticBox(walls, {-6.f, 0.f, -4.25f}, {6.f, 3.f, -4.f}, material);
        AddAcousticBox(walls, {-6.f, 0.f, 4.f}, {6.f, 3.f, 4.25f}, material);
        AddAcousticBox(walls, {-0.15f, 0.f, -4.f}, {0.15f, 3.f, -0.75f}, material);
        AddAcousticBox(walls, {-0.15f, 0.f, 0.75f}, {0.15f, 3.f, 4.f}, material);
        AddAcousticBox(walls, {-0.15f, 2.4f, -0.75f}, {0.15f, 3.f, 0.75f}, material);
        AddAcousticBox(door, {-0.15f, 0.f, -0.75f}, {0.15f, 2.4f, 0.75f}, material);
    }
};

struct SimulationOutputOverride {
    using Function = std::decay_t<decltype(iplSourceGetOutputs)>;
    Function original = iplSourceGetOutputs;
    IPLSource target;
    IPLSimulationOutputs outputs{};
    inline static SimulationOutputOverride* active = nullptr;
    explicit SimulationOutputOverride(IPLSource source) : target(source)
    {
        active = this;
        const_cast<Function&>(iplSourceGetOutputs) = &Dispatch;
    }
    ~SimulationOutputOverride()
    {
        const_cast<Function&>(iplSourceGetOutputs) = original;
        active = nullptr;
    }
    static void IPLCALL Dispatch(IPLSource source, IPLSimulationFlags flags, IPLSimulationOutputs* output)
    {
        if (source == active->target) *output = active->outputs;
        else active->original(source, flags, output);
    }
};

struct BlockedReflectionRun {
    using Function = std::decay_t<decltype(iplSimulatorRunReflections)>;
    Function original = iplSimulatorRunReflections;
    SimulationThread& worker;
    SoundSource& source;
    std::atomic<bool> entered{false}, released{false};
    inline static BlockedReflectionRun* active = nullptr;

    BlockedReflectionRun(SimulationThread& thread, SoundSource& sound) : worker(thread), source(sound)
    {
        active = this;
        const_cast<Function&>(iplSimulatorRunReflections) = &Dispatch;
    }
    ~BlockedReflectionRun()
    {
        released.store(true, std::memory_order_release);
        worker.Stop();
        const_cast<Function&>(iplSimulatorRunReflections) = original;
        active = nullptr;
    }
    static void IPLCALL Dispatch(IPLSimulator simulator)
    {
        auto& self = *active;
        if (self.source.AudioSimulationSource()) {
            self.entered.store(true, std::memory_order_release);
            while (!self.released.load(std::memory_order_acquire))
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        self.original(simulator);
    }
};

struct BakeDeviceObserver {
    using Function = std::decay_t<decltype(iplReflectionsBakerBake)>;
    Function original = iplReflectionsBakerBake;
    BackendDevices live;
    std::atomic<bool> observed{false}, shared{false}, release{false};
    inline static BakeDeviceObserver* active = nullptr;

    explicit BakeDeviceObserver(const BackendDevices& devices) : live(devices)
    {
        active = this;
        const_cast<Function&>(iplReflectionsBakerBake) = &Dispatch;
    }
    ~BakeDeviceObserver()
    {
        const_cast<Function&>(iplReflectionsBakerBake) = original;
        active = nullptr;
    }
    static void IPLCALL Dispatch(IPLContext context, IPLReflectionsBakeParams* params,
                                 IPLProgressCallback progress, void* userData)
    {
        auto& self = *active;
        const bool unsafe = !params || params->sceneType != IPL_SCENETYPE_RADEONRAYS ||
                            !params->openCLDevice || !params->radeonRaysDevice ||
                            params->openCLDevice == self.live.openCL || params->radeonRaysDevice == self.live.radeonRays;
        self.shared.store(unsafe, std::memory_order_relaxed);
        self.observed.store(true, std::memory_order_release);
        if (unsafe) return;
        while (!self.release.load(std::memory_order_acquire))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        self.original(context, params, progress, userData);
    }
};

} // namespace

// In-game path: the direct-path parameters come from a Steam Audio simulator
// (iplSourceGetOutputs) rather than the renderer's own estimate. The rendered
// level must match the simulated distance attenuation; a dropped field
// (air absorption / directivity / transmission left at zero) would silence
// every spatialized source while 2D sounds still play.
SA_TEST(GpuBackend_DefaultEnumerationKeepsPlainOpenCLDevices)
{
    const char* dir = PhononDir();
    if (!dir || !std::getenv("SA_TEST_GPU"))
        throw satest::Skipped{"SA_PHONON_DIR and SA_TEST_GPU are required for the hardware GPU test"};
    PhononContext context;
    StaticConfig config;
    std::string error;
    SA_CHECK(context.Initialize(config, {dir}, error));
    StaticConfig plain = config;
    plain.enableTan = false;
    plain.gpuComputeUnits = 0;
    const auto plainDevices = GPUBackend::Enumerate(context, plain);
    std::printf("    plain OpenCL devices: %zu\n", plainDevices.size());
    SA_CHECK(!plainDevices.empty());
    const auto defaultDevices = GPUBackend::Enumerate(context, config);
    std::printf("    default OpenCL devices: %zu\n", defaultDevices.size());
    SA_CHECK(!defaultDevices.empty());
    GPUBackend backend;
    const bool initialized = backend.Initialize(context, config, error);
    if (!initialized) std::printf("    GPU initialization: %s\n", error.c_str());
    SA_CHECK(initialized);
    SA_CHECK(backend.Devices().radeonRays != nullptr || backend.Devices().tan != nullptr);
}

SA_TEST(ReflectionBake_GpuDeviceIsolatedFromLiveSceneUpdates)
{
    const char* dir = PhononDir();
    if (!dir || !std::getenv("SA_TEST_GPU"))
        throw satest::Skipped{"SA_PHONON_DIR and SA_TEST_GPU are required for GPU bake isolation"};
    StaticConfig fixed;
    fixed.maxSources = 2;
    fixed.maxRays = 512;
    fixed.maxIrDuration = 0.25f;
    fixed.simulationThreads = 1;
    fixed.enableTan = false;
    fixed.gpuComputeUnits = 0;
    fixed.sceneType = SceneTypePreference::RadeonRays;
    PhononContext context;
    std::string error;
    SA_CHECK(context.Initialize(fixed, {dir}, error));
    GPUBackend backend;
    SA_CHECK(backend.Initialize(context, fixed, error));
    const BackendDevices devices = backend.Devices();
    SA_CHECK(devices.sceneType == IPL_SCENETYPE_RADEONRAYS && devices.radeonRays && devices.openCL);
    const char* map = std::getenv("SA_TEST_BSP");
    const bool realMap = map && *map;
    std::vector<uint8_t> bytes;
    if (realMap) {
        std::ifstream file(map, std::ios::binary | std::ios::ate);
        SA_CHECK(file.good());
        const auto size = file.tellg();
        SA_CHECK(size > 0 && size < (1 << 29));
        bytes.resize(static_cast<size_t>(size));
        file.seekg(0);
        SA_CHECK(static_cast<bool>(file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))));
    } else {
        bytes = satest::MakeRoomBsp();
    }
    auto geometry = std::make_shared<BspGeometry>();
    SA_CHECK(ParseBsp(bytes.data(), bytes.size(), CoordinateConverter{}, MaterialLibrary{}, {}, *geometry));
    SceneBuilder live;
    SA_CHECK(live.Initialize(context, devices));
    SA_CHECK(live.AddStaticMesh(geometry->world, "gpu_live"));
    live.Commit();
    Simulator simulator;
    SA_CHECK(simulator.Initialize(context, devices, fixed, error));
    simulator.SetScene(live.Scene());
    BakeDeviceObserver observer(devices);
    ReflectionSimulator reflections;
    SA_CHECK(reflections.Initialize(context, simulator, fixed, devices));
    RuntimeConfig runtime;
    runtime.probeSpacing = realMap ? 4.f : 2.f;
    runtime.bakeRays = 256;
    runtime.bakeBounces = 4;
    runtime.bakeDuration = 0.2f;
    runtime.ambisonicOrder = 1;
    SA_CHECK(reflections.StartBake(geometry, runtime, "", {}));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!observer.observed.load(std::memory_order_acquire) && reflections.BakeInProgress() &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    observer.release.store(true, std::memory_order_release);
    SA_CHECK(observer.observed.load(std::memory_order_acquire));
    SA_CHECK(!observer.shared.load(std::memory_order_relaxed));
    int updates = 0;
    bool cancelRequested = false;
    while (reflections.BakeInProgress() && std::chrono::steady_clock::now() < deadline) {
        if (updates < 24) {
            MeshData extra;
            IPLMaterial material{};
            for (float& band : material.absorption) band = 0.5f;
            const int index = extra.AddMaterial(material);
            const float x = 20.f + static_cast<float>(updates);
            extra.AddTriangle({x, 0.f, 0.f}, {x, 2.f, 0.f}, {x, 0.f, 2.f}, index);
            SA_CHECK(live.AddStaticMesh(extra, "gpu_live_update"));
            live.Commit();
            ++updates;
        }
        const BakeStatus status = reflections.Status();
        if (realMap && !cancelRequested && status.completedProbes >= 32 && status.completedProbes < status.probes) {
            reflections.CancelBake();
            cancelRequested = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    SA_CHECK(!reflections.BakeInProgress());
    SA_CHECK(updates > 0);
    const BakeStatus status = reflections.Status();
    if (cancelRequested) {
        SA_CHECK(status.phase == BakePhase::Cancelled);
        SA_CHECK(!reflections.PollBake());
    } else {
        SA_CHECK(reflections.PollBake());
        SA_CHECK(reflections.HasBakedData());
    }
    SA_CHECK(status.sceneType == IPL_SCENETYPE_RADEONRAYS);
    reflections.Shutdown();
    SA_CHECK(live.AddStaticMesh(geometry->world, "gpu_live_after_bake"));
    live.Commit();
    std::printf("    isolated GPU bake: %d live BVH updates, %d/%d probes, cancellation=%d\n",
                updates, status.completedProbes, status.probes, cancelRequested ? 1 : 0);
}

SA_TEST(Pathing_PhysicalModeValidatesRoutes)
{
    PathingSimulator pathing;
    RuntimeConfig runtime;
    SourceParams source;
    IPLSimulationInputs inputs{};
    pathing.FillSourceInputs(runtime, source, nullptr, inputs);
    SA_CHECK(inputs.enableValidation == IPL_TRUE);
    runtime.physicalAcoustics = false;
    pathing.FillSourceInputs(runtime, source, nullptr, inputs);
    SA_CHECK(inputs.enableValidation == IPL_FALSE);
    runtime.pathingValidation = true;
    pathing.FillSourceInputs(runtime, source, nullptr, inputs);
    SA_CHECK(inputs.enableValidation == IPL_TRUE);
}

SA_TEST(SimulationThread_PrimesNewCaptureWithoutWaitingForPeriodicTick)
{
    const char* dir = PhononDir();
    if (!dir) throw satest::Skipped{"SA_PHONON_DIR not set"};
    StaticConfig fixed;
    fixed.maxSources = 2;
    fixed.maxIrDuration = 0.25f;
    fixed.simulationThreads = 1;
    PhononContext context;
    std::string error;
    SA_CHECK(context.Initialize(fixed, {dir}, error));
    Simulator simulator;
    SA_CHECK(simulator.Initialize(context, {}, fixed, error));
    auto source = std::make_shared<SoundSource>(47, SourceKind::EngineChannel, 1, 44100, 44100, 8192);
    ChannelCapture capture;
    capture.Initialize({source}, 44100);
    ChannelLayout layout;
    layout.origin = 96;
    capture.SetLayoutOverrides(layout);
    std::vector<uint8_t> channel(512, 0);
    const Vec3 position{200.f, 0.f, 32.f};
    std::memcpy(channel.data() + 96, &position, sizeof(position));
    SimulationThread worker;
    RuntimeConfig runtime;
    runtime.simulationIntervalMs = 5000.f;
    runtime.reflections = runtime.pathing = false;
    worker.SetRuntimeConfig(runtime);
    ListenerState listener;
    listener.valid = true;
    listener.frame = CoordinateConverter{}.FrameToSA({}, {1.f, 0.f, 0.f}, {0.f, 0.f, 1.f});
    worker.SetExternalListener(listener);
    SimulationThreadSetup setup;
    setup.context = &context;
    setup.simulator = &simulator;
    setup.capture = &capture;
    SA_CHECK(worker.Start(setup));
    const auto initialDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (worker.Stats().directRuns.load() == 0 && std::chrono::steady_clock::now() < initialDeadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const uint64_t before = worker.Stats().directRuns.load();
    SA_CHECK(before > 0);
    std::vector<int16_t> pcm(512, 4096);
    capture.OnPaintBegin(0, 0, 512);
    capture.OnMixBegin(512);
    capture.CaptureMix(reinterpret_cast<src::channel_t*>(channel.data()), pcm.data(), true, false, 0, 0,
                       static_cast<int32_t>(src::kFixScale), 512);
    capture.OnTransferSamples(512);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!(source->SimulationReadyFlags() & IPL_SIMULATIONFLAGS_DIRECT) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const bool ready = (source->SimulationReadyFlags() & IPL_SIMULATIONFLAGS_DIRECT) != 0;
    const uint64_t runs = worker.Stats().directRuns.load();
    worker.Stop();
    SA_CHECK(ready);
    SA_CHECK_EQ(runs, before + 1);
}

namespace {

void CheckDeferredGeometryCommit(bool brushModel)
{
    const char* dir = PhononDir();
    if (!dir) throw satest::Skipped{"SA_PHONON_DIR not set"};
    StaticConfig fixed;
    fixed.maxSources = 2;
    fixed.maxIrDuration = 0.25f;
    fixed.simulationThreads = 1;
    PhononContext context;
    std::string error;
    SA_CHECK(context.Initialize(fixed, {dir}, error));
    SceneBuilder scene;
    SA_CHECK(scene.Initialize(context, {}));
    TwoRoomScene fixture;
    SA_CHECK(scene.AddStaticMesh(fixture.walls, "commit_cadence"));
    scene.Commit();
    Simulator simulator;
    SA_CHECK(simulator.Initialize(context, {}, fixed, error));
    SimulationThread worker;
    RuntimeConfig runtime;
    runtime.simulationIntervalMs = 5000.f;
    runtime.dynamicUpdateIntervalMs = 100.f;
    runtime.reflections = runtime.pathing = false;
    worker.SetRuntimeConfig(runtime);
    SimulationThreadSetup setup;
    setup.context = &context;
    setup.simulator = &simulator;
    setup.scene = &scene;
    SA_CHECK(worker.Start(setup));
    const DynamicGeometryId id = brushModel ? 1u : worker.AllocDynamicId();
    if (brushModel) {
        auto geometry = std::make_shared<BspGeometry>();
        geometry->world = fixture.walls;
        geometry->mapName = "commit_cadence";
        BspBrushModel brush;
        brush.modelIndex = 1;
        brush.classname = "func_door";
        brush.mesh = fixture.door;
        geometry->brushModels.push_back(std::move(brush));
        SA_CHECK(worker.SetMapGeometry(geometry, geometry->mapName));
    } else {
        SA_CHECK(worker.AddDynamicGeometry(id, std::make_shared<MeshData>(fixture.door), Transform{}, "door"));
    }
    const auto firstDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (worker.Stats().sceneCommits.load() == 0 && std::chrono::steady_clock::now() < firstDeadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    SA_CHECK(worker.Stats().sceneCommits.load() > 0);
    for (int update = 1; update <= 4; ++update) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const uint64_t before = worker.Stats().sceneCommits.load();
        const Vec3 origin{128.f * static_cast<float>(update), 0.f, 0.f};
        const bool queued = brushModel ? worker.UpdateBrushModel(1, origin, {})
                                       : worker.UpdateDynamicGeometry(id, Transform::FromAngles(origin, {}));
        SA_CHECK(queued);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (worker.Stats().sceneCommits.load() == before && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const uint64_t after = worker.Stats().sceneCommits.load();
        if (after == before)
            std::printf("    %s update %d was queued but no geometry commit occurred\n",
                        brushModel ? "brush" : "dynamic", update);
        SA_CHECK_EQ(after, before + 1);
    }
    worker.Stop();
}

}

SA_TEST(SimulationThread_DeferredGeometryCommitWakesBeforePeriodicTick)
{
    CheckDeferredGeometryCommit(false);
}

SA_TEST(SimulationThread_DeferredBrushCommitWakesBeforePeriodicTick)
{
    CheckDeferredGeometryCommit(true);
}

SA_TEST(SimulationThread_PublishesDirectBeforeSlowReflections)
{
    const char* dir = PhononDir();
    if (!dir) throw satest::Skipped{"SA_PHONON_DIR not set"};
    StaticConfig fixed;
    fixed.maxSources = 2;
    fixed.maxIrDuration = 0.25f;
    fixed.simulationThreads = 1;
    PhononContext context;
    std::string error;
    SA_CHECK(context.Initialize(fixed, {dir}, error));
    SceneBuilder scene;
    SA_CHECK(scene.Initialize(context, {}));
    scene.Commit();
    Simulator simulator;
    SA_CHECK(simulator.Initialize(context, {}, fixed, error));
    ReflectionSimulator reflections;
    SA_CHECK(reflections.Initialize(context, simulator, fixed));
    auto source = std::make_shared<SoundSource>(42, SourceKind::Procedural, 1, 44100, 44100, 8192);
    SourceParams params;
    params.position = {200.f, 0.f, 32.f};
    params.positionValid = 1;
    source->SetParams(params);
    SimulationThread worker;
    BlockedReflectionRun block(worker, *source);
    RuntimeConfig runtime;
    runtime.numRays = 64;
    runtime.numBounces = 1;
    runtime.irDuration = 0.2f;
    runtime.hybridTransitionTime = 0.05f;
    worker.SetRuntimeConfig(runtime);
    ListenerState listener;
    listener.valid = true;
    listener.frame = CoordinateConverter{}.FrameToSA({}, {1.f, 0.f, 0.f}, {0.f, 0.f, 1.f});
    worker.SetExternalListener(listener);
    SimulationThreadSetup setup;
    setup.context = &context;
    setup.simulator = &simulator;
    setup.reflections = &reflections;
    setup.scene = &scene;
    SA_CHECK(worker.Start(setup));
    SA_CHECK(worker.AddStreamSource(source));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!block.entered.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const bool entered = block.entered.load(std::memory_order_acquire);
    const IPLSimulationFlags ready = source->SimulationReadyFlags();
    block.released.store(true, std::memory_order_release);
    worker.Stop();
    SA_CHECK(entered);
    SA_CHECK((ready & IPL_SIMULATIONFLAGS_DIRECT) != 0);
    SA_CHECK((ready & IPL_SIMULATIONFLAGS_REFLECTIONS) == 0);
}

SA_TEST(HrtfRenderer_DoesNotInventRoomReverbBeforeSimulation)
{
    const char* dir = PhononDir();
    if (!dir) throw satest::Skipped{"SA_PHONON_DIR not set"};
    StaticConfig fixed;
    fixed.maxSources = 2;
    PhononContext context;
    std::string error;
    SA_CHECK(context.Initialize(fixed, {dir}, error));
    HRTFRenderer renderer;
    SA_CHECK(renderer.Initialize(context, {}, fixed, error));
    SoundSource source(43, SourceKind::EngineChannel, 1, 44100, 44100, 8192);
    SourceParams params;
    params.position = {200.f, 0.f, 32.f};
    params.positionValid = 1;
    source.SetParams(params);
    const auto& preset = FindRoomPreset(7);
    SA_CHECK(preset.Active());
    SA_CHECK(RoomMixForSource(preset, 200.f, 0.f) > 0.f);
    std::vector<float> input(fixed.frameSize, 0.25f), room(fixed.frameSize, 0.f);
    const float* channels[] = {input.data()};
    ListenerState listener;
    listener.valid = true;
    listener.frame = CoordinateConverter{}.FrameToSA({0.f, 0.f, 32.f}, {1.f, 0.f, 0.f}, {0.f, 0.f, 1.f});
    renderer.BeginFrame({}, listener, {room.data(), &preset, false});
    renderer.RenderSource(source, channels, 1, 1.f);
    renderer.EndFrame();
    const bool sent = source.Render().roomSent;
    renderer.ReleaseEffects(source.Render().effects);
    source.Render().effects = nullptr;
    SA_CHECK(!sent);
    for (float sample : room) SA_CHECK_NEAR(sample, 0.f, 1e-7);
}

SA_TEST(HrtfRenderer_FirstImpulseUsesCapturedEngineAttenuation)
{
    const char* dir = PhononDir();
    if (!dir) throw satest::Skipped{"SA_PHONON_DIR not set"};
    StaticConfig fixed;
    fixed.maxSources = 2;
    fixed.maxIrDuration = 0.25f;
    PhononContext context;
    std::string error;
    SA_CHECK(context.Initialize(fixed, {dir}, error));
    HRTFRenderer renderer;
    SA_CHECK(renderer.Initialize(context, {}, fixed, error));
    SoundSource source(45, SourceKind::EngineChannel, 1, 44100, 44100, 8192);
    SourceParams params;
    params.position = {200.f, 0.f, 32.f};
    params.positionValid = 1;
    source.SetParams(params);
    ListenerState listener;
    listener.valid = true;
    listener.frame = CoordinateConverter{}.FrameToSA({0.f, 0.f, 32.f}, {1.f, 0.f, 0.f}, {0.f, 0.f, 1.f});
    RuntimeConfig runtime;
    runtime.reflections = runtime.pathing = false;
    std::vector<float> pcm(fixed.frameSize);
    const float* input[] = {pcm.data()};
    const auto render = [&](bool captured) {
        params.engineGainValid = captured ? 1 : 0;
        params.engineDirectGain = 0.025f;
        source.SetParams(params);
        source.Render().gainInitialized = false;
        if (source.Render().effects) source.Render().effects->Reset();
        double energy = 0.0;
        for (int frame = 0; frame < 8; ++frame) {
            std::fill(pcm.begin(), pcm.end(), 0.f);
            if (frame == 0) pcm[0] = 0.5f;
            renderer.BeginFrame(runtime, listener);
            renderer.RenderSource(source, input, 1, 1.f);
            renderer.EndFrame();
            for (int i = 0; i < fixed.frameSize; ++i)
                energy += renderer.MasterLeft()[i] * renderer.MasterLeft()[i] + renderer.MasterRight()[i] * renderer.MasterRight()[i];
        }
        return energy;
    };
    for (bool hrtf : {false, true}) {
        runtime.hrtf = hrtf;
        const double open = render(false), captured = render(true);
        SA_CHECK(open > 0.0 && captured > 0.0);
        SA_CHECK_NEAR(std::sqrt(captured / open), 0.025, 0.0005);
    }
    renderer.ReleaseEffects(source.Render().effects);
    source.Render().effects = nullptr;
}

SA_TEST(HrtfRenderer_PathingDoesNotDuplicateVisibleDirectSound)
{
    const char* dir = PhononDir();
    if (!dir) throw satest::Skipped{"SA_PHONON_DIR not set"};
    StaticConfig fixed;
    fixed.maxSources = 2;
    fixed.maxIrDuration = 0.25f;
    PhononContext context;
    std::string error;
    SA_CHECK(context.Initialize(fixed, {dir}, error));
    Simulator simulator;
    SA_CHECK(simulator.Initialize(context, {}, fixed, error));
    SoundSource source(46, SourceKind::Procedural, 1, 44100, 44100, 8192);
    SourceParams params;
    params.position = {200.f, 0.f, 32.f};
    params.positionValid = 1;
    source.SetParams(params);
    SA_CHECK(simulator.CreateSource(source));
    source.MarkSimulationReady(static_cast<IPLSimulationFlags>(IPL_SIMULATIONFLAGS_DIRECT | IPL_SIMULATIONFLAGS_PATHING));
    SimulationOutputOverride injected(source.Sim().source);
    auto& outputs = injected.outputs;
    outputs.direct.distanceAttenuation = outputs.direct.directivity = outputs.direct.occlusion = 1.f;
    for (float& band : outputs.direct.airAbsorption) band = 1.f;
    for (float& band : outputs.pathing.eqCoeffs) band = 1.f;
    float sh[4] = {1.f, 0.f, 0.f, 0.f};
    outputs.pathing.shCoeffs = sh;
    outputs.pathing.order = 0;
    outputs.pathing.normalizeEQ = IPL_TRUE;
    HRTFRenderer renderer;
    SA_CHECK(renderer.Initialize(context, {}, fixed, error));
    ListenerState listener;
    listener.valid = true;
    listener.frame = CoordinateConverter{}.FrameToSA({0.f, 0.f, 32.f}, {1.f, 0.f, 0.f}, {0.f, 0.f, 1.f});
    RuntimeConfig runtime;
    runtime.hrtf = runtime.reflections = false;
    std::vector<float> pcm(fixed.frameSize);
    const float* input[] = {pcm.data()};
    const auto render = [&] {
        if (source.Render().effects) source.Render().effects->Reset();
        uint32_t seed = 513;
        double energy = 0.0;
        for (int frame = 0; frame < 24; ++frame) {
            for (float& sample : pcm) {
                seed = seed * 1664525u + 1013904223u;
                sample = (static_cast<float>(seed >> 8) / 8388608.f - 1.f) * 0.25f;
            }
            renderer.BeginFrame(runtime, listener);
            renderer.RenderSource(source, input, 1, 1.f);
            renderer.EndFrame();
            if (frame >= 8)
                for (int i = 0; i < fixed.frameSize; ++i)
                    energy += renderer.MasterLeft()[i] * renderer.MasterLeft()[i] + renderer.MasterRight()[i] * renderer.MasterRight()[i];
        }
        return energy;
    };
    runtime.pathing = false;
    const double direct = render();
    runtime.pathing = true;
    const double combined = render();
    SA_CHECK(direct > 0.0);
    SA_CHECK_NEAR(combined / direct, 1.0, 0.005);
    outputs.direct.occlusion = 0.f;
    runtime.directGain = 0.f;
    const double path = render();
    for (float& band : outputs.pathing.eqCoeffs) band = 0.1f;
    const double attenuatedPath = render();
    SA_CHECK(path > 0.0 && attenuatedPath > 0.0);
    SA_CHECK_NEAR(std::sqrt(attenuatedPath / path), 0.1, 0.005);
    renderer.ReleaseEffects(source.Render().effects);
    source.Render().effects = nullptr;
    simulator.DestroySource(source);
    simulator.ProcessDeferredReleases(true);
}

SA_TEST(HrtfRenderer_TwoRoomsRespectDoorAndMaterial)
{
    const char* dir = PhononDir();
    if (!dir) throw satest::Skipped{"SA_PHONON_DIR not set"};
    StaticConfig fixed;
    fixed.maxSources = 2;
    fixed.maxRays = 2048;
    fixed.maxIrDuration = 0.25f;
    fixed.simulationThreads = 1;
    PhononContext context;
    std::string error;
    SA_CHECK(context.Initialize(fixed, {dir}, error));
    BackendDevices devices;
    GPUBackend gpu;
    if (std::getenv("SA_TEST_GPU")) {
        fixed.sceneType = SceneTypePreference::RadeonRays;
        fixed.enableTan = false;
        fixed.gpuComputeUnits = 0;
        SA_CHECK(gpu.Initialize(context, fixed, error));
        devices = gpu.Devices();
    }
    TwoRoomScene fixture;
    CoordinateConverter converter;
    SceneBuilder scene;
    SA_CHECK(scene.Initialize(context, devices));
    SA_CHECK(scene.AddStaticMesh(fixture.walls, "two_rooms"));
    const auto door = scene.AddDynamic(fixture.door, converter.TransformToSA(Transform{}), "door");
    SA_CHECK(door != SceneBuilder::kInvalidDynamic);
    scene.Commit();
    scene.UpdateDynamic(door, converter.TransformToSA(Transform{}));
    SA_CHECK(!scene.HasPendingChanges());
    const auto movedDoor = converter.TransformToSA(Transform::FromAngles({128.f, 0.f, 0.f}, {}));
    scene.UpdateDynamic(door, movedDoor);
    SA_CHECK(scene.HasPendingChanges());
    scene.Commit();
    scene.UpdateDynamic(door, movedDoor);
    SA_CHECK(!scene.HasPendingChanges());
    scene.UpdateDynamic(door, converter.TransformToSA(Transform{}));
    scene.Commit();
    Simulator simulator;
    SA_CHECK(simulator.Initialize(context, devices, fixed, error));
    simulator.SetScene(scene.Scene());
    ReflectionSimulator reflections;
    SA_CHECK(reflections.Initialize(context, simulator, fixed, devices));
    auto geometry = std::make_shared<BspGeometry>();
    geometry->world = fixture.walls;
    geometry->world.Append(fixture.door);
    RuntimeConfig runtime;
    runtime.hrtf = false;
    runtime.pathing = false;
    runtime.irDuration = 0.2f;
    runtime.numRays = 2048;
    runtime.numBounces = 8;
    runtime.bakeRays = 512;
    runtime.bakeBounces = 8;
    runtime.bakeDuration = 0.2f;
    runtime.probeSpacing = 2.f;
    SA_CHECK(reflections.StartBake(geometry, runtime, "", {}));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (reflections.BakeInProgress() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    SA_CHECK(!reflections.BakeInProgress());
    SA_CHECK(reflections.PollBake());
    SoundSource source(44, SourceKind::Procedural, 1, 44100, 44100, 8192);
    SourceParams params;
    params.positionValid = 1;
    params.position = converter.PositionToSource({-2.f, 1.5f, 0.f});
    source.SetParams(params);
    SA_CHECK(simulator.CreateSource(source));
    HRTFRenderer renderer;
    SA_CHECK(renderer.Initialize(context, devices, fixed, error));
    ListenerState listener;
    listener.valid = true;
    listener.frame = converter.FrameToSA(converter.PositionToSource({2.f, 1.5f, 0.f}), {-1.f, 0.f, 0.f}, {0.f, 0.f, 1.f});
    const auto flags = static_cast<IPLSimulationFlags>(IPL_SIMULATIONFLAGS_DIRECT | IPL_SIMULATIONFLAGS_REFLECTIONS);
    IPLSimulationSharedInputs shared{};
    shared.listener = listener.frame;
    reflections.FillSharedInputs(runtime, shared);
    simulator.SetSharedInputs(flags, shared);
    IPLDirectEffect reference = nullptr;
    IPLDirectEffectSettings referenceSettings{1};
    SA_CHECK(iplDirectEffectCreate(context.Handle(), context.MutableAudioSettings(), &referenceSettings, &reference) == IPL_STATUS_SUCCESS);
    struct Result { double direct = 0.0, reference = 0.0, reflections = 0.0; float visibility = 0.f; };
    std::vector<float> pcm(fixed.frameSize), referencePcm(fixed.frameSize);
    const float* input[] = {pcm.data()};
    float* refInput[] = {pcm.data()};
    float* refOutput[] = {referencePcm.data()};
    IPLAudioBuffer inBuffer{1, fixed.frameSize, refInput}, outBuffer{1, fixed.frameSize, refOutput};
    float distanceMultiplier = 0.f;
    const auto measure = [&](bool closed) {
        Transform transform;
        transform.origin = converter.PositionToSource({0.f, closed ? 0.f : 5.f, 0.f});
        scene.UpdateDynamic(door, converter.TransformToSA(transform));
        scene.Commit();
        IPLSimulationInputs in{};
        in.flags = flags;
        in.directFlags = static_cast<IPLDirectSimulationFlags>(IPL_DIRECTSIMULATIONFLAGS_DISTANCEATTENUATION |
                            IPL_DIRECTSIMULATIONFLAGS_AIRABSORPTION | IPL_DIRECTSIMULATIONFLAGS_OCCLUSION |
                            IPL_DIRECTSIMULATIONFLAGS_TRANSMISSION);
        in.source = converter.FrameToSA(params.position, {1.f, 0.f, 0.f}, {0.f, 0.f, 1.f});
        in.distanceAttenuationModel.type = IPL_DISTANCEATTENUATIONTYPE_CALLBACK;
        in.distanceAttenuationModel.callback = &TestDistanceCallback;
        in.distanceAttenuationModel.userData = &distanceMultiplier;
        in.distanceAttenuationModel.dirty = IPL_TRUE;
        in.airAbsorptionModel.type = IPL_AIRABSORPTIONTYPE_DEFAULT;
        in.occlusionType = IPL_OCCLUSIONTYPE_RAYCAST;
        in.numOcclusionSamples = in.numTransmissionRays = 1;
        reflections.FillSourceInputs(runtime, params, in);
        iplSourceSetInputs(source.Sim().source, flags, &in);
        simulator.Commit();
        simulator.RunDirect();
        simulator.RunReflections();
        source.MarkSimulationReady(flags);
        IPLSimulationOutputs outputs{};
        iplSourceGetOutputs(source.Sim().source, flags, &outputs);
        Result result;
        result.visibility = outputs.direct.occlusion;
        IPLDirectEffectParams direct = outputs.direct;
        direct.flags = static_cast<IPLDirectEffectFlags>(IPL_DIRECTEFFECTFLAGS_APPLYAIRABSORPTION |
                           IPL_DIRECTEFFECTFLAGS_APPLYDIRECTIVITY | IPL_DIRECTEFFECTFLAGS_APPLYOCCLUSION |
                           IPL_DIRECTEFFECTFLAGS_APPLYTRANSMISSION);
        direct.transmissionType = IPL_TRANSMISSIONTYPE_FREQDEPENDENT;
        iplDirectEffectReset(reference);
        if (source.Render().effects) source.Render().effects->Reset();
        RuntimeConfig dry = runtime;
        dry.reflections = false;
        uint32_t seed = 931;
        for (int frame = 0; frame < 24; ++frame) {
            for (float& sample : pcm) {
                seed = seed * 1664525u + 1013904223u;
                sample = (static_cast<float>(seed >> 8) / 8388608.f - 1.f) * 0.25f;
            }
            iplDirectEffectApply(reference, &direct, &inBuffer, &outBuffer);
            renderer.BeginFrame(dry, listener);
            renderer.RenderSource(source, input, 1, 1.f);
            renderer.EndFrame();
            if (frame >= 8)
                for (int i = 0; i < fixed.frameSize; ++i) {
                    result.direct += renderer.MasterLeft()[i] * renderer.MasterLeft()[i] + renderer.MasterRight()[i] * renderer.MasterRight()[i];
                    result.reference += referencePcm[i] * referencePcm[i];
                }
        }
        source.Render().effects->Reset();
        RuntimeConfig wet = runtime;
        wet.directGain = 0.f;
        for (int frame = 0; frame < 64; ++frame) {
            std::fill(pcm.begin(), pcm.end(), 0.f);
            if (frame == 0) pcm[0] = 0.5f;
            renderer.BeginFrame(wet, listener);
            renderer.RenderSource(source, input, 1, 1.f);
            renderer.EndFrame();
            for (int i = 0; i < fixed.frameSize; ++i)
                result.reflections += renderer.MasterLeft()[i] * renderer.MasterLeft()[i] + renderer.MasterRight()[i] * renderer.MasterRight()[i];
        }
        return result;
    };
    const Result closed = measure(true), open = measure(false), closedAgain = measure(true);
    scene.ClearDynamic();
    scene.ClearStatic();
    scene.Commit();
    const Result empty = measure(false);
    iplDirectEffectRelease(&reference);
    renderer.ReleaseEffects(source.Render().effects);
    source.Render().effects = nullptr;
    simulator.DestroySource(source);
    simulator.ProcessDeferredReleases(true);
    const double measuredDb = 10.0 * std::log10(closed.direct / open.direct);
    const double materialDb = 10.0 * std::log10(closed.reference / open.reference);
    std::printf("    two rooms: direct closed/open %.2f dB, material reference %.2f dB; reflections closed/open %g/%g\n",
                measuredDb, materialDb, closed.reflections, open.reflections);
    SA_CHECK_NEAR(closed.visibility, 0.f, 1e-6);
    SA_CHECK_NEAR(open.visibility, 1.f, 1e-6);
    SA_CHECK(closed.direct > 0.0 && open.direct > closed.direct);
    SA_CHECK_NEAR(measuredDb, materialDb, 0.2);
    SA_CHECK(open.reflections > 1e-8);
    SA_CHECK(closed.reflections < open.reflections * 0.01 + 1e-9);
    SA_CHECK(closedAgain.reflections < open.reflections * 0.01 + 1e-9);
    SA_CHECK_NEAR(closedAgain.direct / closed.direct, 1.0, 0.02);
    SA_CHECK_NEAR(empty.visibility, 1.f, 1e-6);
    SA_CHECK(empty.reflections < open.reflections * 0.01 + 1e-9);
}

SA_TEST(HrtfRenderer_SimulatedDirectPathKeepsLevel)
{
    const char* dir = PhononDir();
    if (!dir) {
        throw satest::Skipped{"SA_PHONON_DIR not set"};
    }
    StaticConfig cfg;
    cfg.maxSources = 4;
    PhononContext ctx;
    std::string err;
    SA_CHECK(ctx.Initialize(cfg, {dir}, err));
    BackendDevices devices;
    HRTFRenderer renderer;
    SA_CHECK(renderer.Initialize(ctx, devices, cfg, err));
    Simulator simulator;
    SA_CHECK(simulator.Initialize(ctx, devices, cfg, err));
    if (!renderer.IsValid() || !simulator.IsValid()) {
        std::printf("    %s\n", err.c_str());
        return;
    }

    const int32_t frame = ctx.FrameSize();
    SoundSource source(1, SourceKind::EngineChannel, 1, static_cast<uint32_t>(ctx.SampleRate()),
                       static_cast<uint32_t>(ctx.SampleRate()), 8192);
    SA_CHECK(simulator.CreateSource(source));

    RuntimeConfig rt;
    rt.reflections = false;
    rt.pathing = false;
    rt.doppler = true;
    CoordinateConverter converter;
    converter.SetUnitsPerMeter(rt.unitsPerMeter);

    // Source 7 m ahead, SNDLVL_NORM rolloff (dist_mult as the engine stores it).
    const Vec3 sourcePos{7.f * rt.unitsPerMeter, 0.f, 0.f};
    SourceParams params;
    params.position = sourcePos;
    params.positionValid = 1;
    params.spatialize = 1;
    params.gain = 1.f;
    params.distMult = SoundLevelToDistMult(75.f);
    params.reflections = 0;
    params.pathing = 0;
    source.SetParams(params);
    source.Render().effects = renderer.AcquireEffects();
    SA_CHECK(source.Render().effects != nullptr);

    ListenerState listener;
    listener.frame = converter.FrameToSA(Vec3{0.f, 0.f, 0.f}, Vec3{1.f, 0.f, 0.f}, Vec3{0.f, 0.f, 1.f});
    listener.valid = true;

    float distMult = params.distMult;
    IPLSimulationInputs in{};
    in.flags = IPL_SIMULATIONFLAGS_DIRECT;
    in.directFlags = static_cast<IPLDirectSimulationFlags>(IPL_DIRECTSIMULATIONFLAGS_DISTANCEATTENUATION |
                                                           IPL_DIRECTSIMULATIONFLAGS_AIRABSORPTION);
    in.source = converter.FrameToSA(sourcePos, Vec3{1.f, 0.f, 0.f}, Vec3{0.f, 0.f, 1.f});
    in.distanceAttenuationModel.type = IPL_DISTANCEATTENUATIONTYPE_CALLBACK;
    in.distanceAttenuationModel.minDistance = 1.f;
    in.distanceAttenuationModel.callback = &TestDistanceCallback;
    in.distanceAttenuationModel.userData = &distMult;
    in.distanceAttenuationModel.dirty = IPL_TRUE;
    in.airAbsorptionModel.type = IPL_AIRABSORPTIONTYPE_DEFAULT;
    in.occlusionType = IPL_OCCLUSIONTYPE_RAYCAST;
    in.occlusionRadius = 1.f;
    in.numOcclusionSamples = 1;
    in.numTransmissionRays = 1;
    iplSourceSetInputs(source.Sim().source, IPL_SIMULATIONFLAGS_DIRECT, &in);

    IPLSimulationSharedInputs shared{};
    shared.listener = listener.frame;
    simulator.SetSharedInputs(IPL_SIMULATIONFLAGS_DIRECT, shared);
    simulator.Commit();
    simulator.RunDirect();
    source.PublishSimulationSource(source.Sim().source);
    source.MarkSimulationReady(IPL_SIMULATIONFLAGS_DIRECT);

    IPLSimulationOutputs outputs{};
    iplSourceGetOutputs(source.Sim().source, IPL_SIMULATIONFLAGS_DIRECT, &outputs);
    std::printf("    sim direct: flags 0x%x  att %.3f  air %.3f/%.3f/%.3f  directivity %.3f  occl %.3f  "
                "trans %.3f/%.3f/%.3f\n",
                static_cast<unsigned>(outputs.direct.flags), static_cast<double>(outputs.direct.distanceAttenuation),
                static_cast<double>(outputs.direct.airAbsorption[0]), static_cast<double>(outputs.direct.airAbsorption[1]),
                static_cast<double>(outputs.direct.airAbsorption[2]), static_cast<double>(outputs.direct.directivity),
                static_cast<double>(outputs.direct.occlusion), static_cast<double>(outputs.direct.transmission[0]),
                static_cast<double>(outputs.direct.transmission[1]), static_cast<double>(outputs.direct.transmission[2]));
    const float expectedAtt = SourceDistanceGain(params.distMult, 7.f * rt.unitsPerMeter);
    SA_CHECK(std::fabs(outputs.direct.distanceAttenuation - expectedAtt) < 0.02f);

    std::vector<float> mono(static_cast<size_t>(frame));
    const float* inputs[1] = {mono.data()};
    uint32_t seed = 777u;
    const auto renderDb = [&]() {
        double inEnergy = 0.0;
        double outEnergy = 0.0;
        // Let the propagation delay line and the effect's parameter ramps settle.
        for (int f = 0; f < 40; ++f) {
            for (float& s : mono) {
                seed = seed * 1664525u + 1013904223u;
                s = (static_cast<float>(seed >> 8) / 8388608.f) - 1.f;
            }
            renderer.BeginFrame(rt, listener);
            renderer.RenderSource(source, inputs, 1, 1.f);
            renderer.EndFrame();
            if (f < 32)
                continue;
            const float* l = renderer.MasterLeft();
            const float* r = renderer.MasterRight();
            for (int32_t i = 0; i < frame; ++i) {
                inEnergy += static_cast<double>(mono[static_cast<size_t>(i)]) * mono[static_cast<size_t>(i)];
                outEnergy += 0.5 * (static_cast<double>(l[i]) * l[i] + static_cast<double>(r[i]) * r[i]);
            }
        }
        return 10.0 * std::log10((outEnergy + 1e-12) / (inEnergy + 1e-12));
    };
    const double outDb = renderDb();
    const RenderDiag diag = source.LoadRenderDiag();
    std::printf("    render mode %d  att %.3f  in %.1f dB  out %.1f dB  output %.1f dB re input (expected about %.1f dB)\n",
                static_cast<int>(diag.mode), static_cast<double>(diag.distanceAttenuation),
                10.0 * std::log10(static_cast<double>(diag.inputLevel) + 1e-12),
                10.0 * std::log10(static_cast<double>(diag.outputLevel) + 1e-12), outDb,
                20.0 * std::log10(static_cast<double>(expectedAtt)));
    SA_CHECK(diag.mode == RenderDiag::kHrtf);
    SA_CHECK(diag.simOutputs == 1);
    SA_CHECK(diag.inputLevel > 0.1f);
    SA_CHECK(diag.outputLevel > 0.f);
    // HRTF adds a few dB of head-related gain/loss; a dropped parameter would
    // be 40 dB or more below.
    SA_CHECK(outDb > 20.0 * std::log10(static_cast<double>(expectedAtt)) - 12.0);

    // Same source at 40 m: the simulated attenuation must reach the output.
    const Vec3 farPos{40.f * rt.unitsPerMeter, 0.f, 0.f};
    params.position = farPos;
    source.SetParams(params);
    in.source = converter.FrameToSA(farPos, Vec3{1.f, 0.f, 0.f}, Vec3{0.f, 0.f, 1.f});
    iplSourceSetInputs(source.Sim().source, IPL_SIMULATIONFLAGS_DIRECT, &in);
    simulator.Commit();
    simulator.RunDirect();
    IPLSimulationOutputs farOutputs{};
    iplSourceGetOutputs(source.Sim().source, IPL_SIMULATIONFLAGS_DIRECT, &farOutputs);
    const float farAtt = SourceDistanceGain(params.distMult, 40.f * rt.unitsPerMeter);
    SA_CHECK(std::fabs(farOutputs.direct.distanceAttenuation - farAtt) < 0.02f);
    const double farDb = renderDb();
    const double expectedDrop = 20.0 * std::log10(static_cast<double>(farAtt) / static_cast<double>(expectedAtt));
    std::printf("    40 m: att %.3f  output %.1f dB re input  drop %.1f dB (expected %.1f dB)\n",
                static_cast<double>(farAtt), farDb, farDb - outDb, expectedDrop);
    SA_CHECK(std::fabs((farDb - outDb) - expectedDrop) < 4.0);

    renderer.ReleaseEffects(source.Render().effects);
    source.Render().effects = nullptr;
    source.PublishSimulationSource(nullptr);
    simulator.DestroySource(source);
    simulator.ProcessDeferredReleases(true);
}

SA_TEST(ReflectionBake_UsesAnIndependentScene)
{
    const char* dir = PhononDir();
    if (!dir)
        throw satest::Skipped{"SA_PHONON_DIR not set"};
    PhononContext ctx;
    StaticConfig fixed;
    fixed.maxSources = 4;
    fixed.simulationThreads = 1;
    fixed.maxIrDuration = 0.25f;
    fixed.validationLayer = true;
    std::string error;
    SA_CHECK(ctx.Initialize(fixed, {dir}, error));
    CPUFallbackBackend backend;
    SA_CHECK(backend.Initialize(ctx, fixed, error));
    BackendDevices devices = backend.Devices();
    auto geometry = std::make_shared<BspGeometry>();
    const auto bytes = satest::MakeRoomBsp();
    SA_CHECK(ParseBsp(bytes.data(), bytes.size(), CoordinateConverter{}, MaterialLibrary{}, {}, *geometry));
    SceneBuilder live;
    SA_CHECK(live.Initialize(ctx, devices));
    devices.sceneType = live.SceneType();
    if (devices.sceneType == IPL_SCENETYPE_DEFAULT)
        devices.embree = nullptr;
    Simulator simulator;
    SA_CHECK(simulator.Initialize(ctx, devices, fixed, error));
    SA_CHECK(simulator.SupportsReflections());
    SA_CHECK(simulator.SupportsPathing());
    SA_CHECK(live.AddStaticMesh(geometry->world, "live"));
    live.Commit();
    simulator.SetScene(live.Scene());
    ReflectionSimulator reflections([] {
        ProcessMemoryInfo memory;
        memory.valid = true;
        memory.physicalTotal = 16ull << 30;
        memory.physicalAvailable = 8ull << 30;
        memory.commitAvailable = 12ull << 30;
        return memory;
    });
    SA_CHECK(reflections.Initialize(ctx, simulator, fixed, devices));
    RuntimeConfig runtime;
    runtime.ambisonicOrder = 0;
    runtime.bakeRays = 256;
    runtime.bakeBounces = 2;
    runtime.bakeDuration = 0.2f;
    SA_CHECK(reflections.StartBake(geometry, runtime, "", {}));
    live.ClearStatic();
    live.Commit();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (reflections.BakeInProgress() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    SA_CHECK(!reflections.BakeInProgress());
    const bool attached = reflections.PollBake();
    if (!attached)
        for (const std::string& line : log::Drain(256))
            std::printf("    %s\n", line.c_str());
    SA_CHECK(attached);
    SA_CHECK(reflections.HasBakedData());
    SA_CHECK(!reflections.PathingBaked());
    SA_CHECK(reflections.ProbeCount() > 0);
    SourceParams emitter;
    IPLSimulationInputs reflectionInputs{};
    reflections.FillSourceInputs(runtime, emitter, reflectionInputs);
    SA_CHECK(reflectionInputs.baked == IPL_FALSE);
    emitter.listenerRelative = 1;
    reflections.FillSourceInputs(runtime, emitter, reflectionInputs);
    SA_CHECK(reflectionInputs.baked == IPL_TRUE);
    SA_CHECK_EQ(live.StaticTriangleCount(), size_t(0));
    BakeStatus status = reflections.Status();
    SA_CHECK(status.phase == BakePhase::Ready);
    SA_CHECK_EQ(status.completedProbes, reflections.ProbeCount());
    SA_CHECK_EQ(status.sceneType, live.SceneType());
    SA_CHECK(!status.active);
    struct CacheFile {
        std::string path;
        ~CacheFile() { std::remove(path.c_str()); std::remove((path + ".tmp").c_str()); }
    } cache{(std::filesystem::temp_directory_path() /
              ("sa_bake_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
               ".probes")).u8string()};
    SA_CHECK(!std::filesystem::exists(std::filesystem::u8path(cache.path)));
    std::atomic<bool> pathingRan{false};
    SA_CHECK(reflections.StartBake(geometry, runtime, cache.path, [&](IPLScene, IPLProbeBatch, std::atomic<bool>&,
                                                                    IPLProgressCallback, void*) { pathingRan = true; }));
    const auto choiceDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (reflections.Status().phase != BakePhase::AwaitingPathing && reflections.BakeInProgress() &&
           std::chrono::steady_clock::now() < choiceDeadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    SA_CHECK(reflections.Status().phase == BakePhase::AwaitingPathing);
    SA_CHECK(!pathingRan);
    reflections.SetPathingMemoryLimit(0);
    SA_CHECK(!reflections.Status().pathingAllowed);
    SA_CHECK(!reflections.ContinuePathing(true));
    SA_CHECK(!pathingRan);
    SA_CHECK(reflections.ContinuePathing(false));
    while (reflections.BakeInProgress() && std::chrono::steady_clock::now() < choiceDeadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    SA_CHECK(!reflections.BakeInProgress());
    SA_CHECK(reflections.PollBake());
    SA_CHECK(!pathingRan);
    SA_CHECK(reflections.HasBakedData());
    SA_CHECK(reflections.Status().cacheSaved);
    SA_CHECK(reflections.LoadCachedBake(cache.path));
    SA_CHECK(reflections.HasBakedData());
    SA_CHECK(!reflections.PathingBaked());
    SA_CHECK(reflections.Status().phase == BakePhase::Ready);
    SA_CHECK(!reflections.Status().active);
    PathingSimulator pathing;
    SA_CHECK(pathing.Initialize(ctx, simulator, fixed));
    SA_CHECK(reflections.StartBake(geometry, runtime, cache.path, pathing.MakeBakeStep(runtime)));
    const auto pathDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (reflections.Status().phase != BakePhase::AwaitingPathing && reflections.BakeInProgress() &&
           std::chrono::steady_clock::now() < pathDeadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    SA_CHECK(reflections.ContinuePathing(true));
    while (reflections.BakeInProgress() && std::chrono::steady_clock::now() < pathDeadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    SA_CHECK(!reflections.BakeInProgress());
    SA_CHECK(reflections.PollBake());
    SA_CHECK(reflections.PathingBaked());
    SA_CHECK(reflections.LoadCachedBake(cache.path));
    SA_CHECK(reflections.PathingBaked());
    SA_CHECK(reflections.StartBake(geometry, runtime, "", pathing.MakeBakeStep(runtime)));
    const auto cancelDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (reflections.Status().phase != BakePhase::AwaitingPathing && reflections.BakeInProgress() &&
           std::chrono::steady_clock::now() < cancelDeadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    reflections.CancelBake();
    while (reflections.BakeInProgress() && std::chrono::steady_clock::now() < cancelDeadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    SA_CHECK(!reflections.BakeInProgress());
    SA_CHECK(reflections.PollBake());
    SA_CHECK(reflections.HasBakedData());
    SA_CHECK(!reflections.PathingBaked());
    std::atomic<bool> memoryStepStarted{false};
    SA_CHECK(reflections.StartBake(geometry, runtime, cache.path,
        [&](IPLScene, IPLProbeBatch, std::atomic<bool>& cancel, IPLProgressCallback, void*) {
            memoryStepStarted.store(true);
            const auto timeout = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!cancel.load() && std::chrono::steady_clock::now() < timeout)
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }));
    const auto memoryDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (reflections.Status().phase != BakePhase::AwaitingPathing && reflections.BakeInProgress() &&
           std::chrono::steady_clock::now() < memoryDeadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    SA_CHECK(reflections.ContinuePathing(true));
    while (!memoryStepStarted.load() && reflections.BakeInProgress() && std::chrono::steady_clock::now() < memoryDeadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    SA_CHECK(memoryStepStarted.load());
    reflections.SetPathingMemoryLimit(0);
    while (reflections.BakeInProgress() && std::chrono::steady_clock::now() < memoryDeadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    SA_CHECK(!reflections.BakeInProgress());
    SA_CHECK(reflections.PollBake());
    SA_CHECK(reflections.Status().memoryLimited);
    SA_CHECK(reflections.Status().cacheSaved);
    SA_CHECK(reflections.HasBakedData());
    SA_CHECK(!reflections.PathingBaked());
    SA_CHECK(reflections.LoadCachedBake(cache.path));
}

SA_TEST(ReflectionBake_MemoryBudgetRejectsLargeMapsAndKeepsSystemReserve)
{
    constexpr uint64_t mib = 1024 * 1024;
    ProcessMemoryInfo memory;
    memory.valid = true;
    memory.physicalTotal = 16384 * mib;
    memory.physicalAvailable = 8192 * mib;
    memory.commitAvailable = 12288 * mib;
    memory.privateBytes = 4096 * mib;
    SA_CHECK_EQ(ReflectionSimulator::PathingMemoryBudget(1024, memory), 1024 * mib);
    SA_CHECK_EQ(ReflectionSimulator::PathingMemoryBudget(16384, memory), 4096 * mib);
    SA_CHECK(ReflectionSimulator::EstimatePathingMemory(23473) > 32768 * mib);
    SA_CHECK(ReflectionSimulator::EstimatePathingMemory(32) < 1024 * mib);
    SA_CHECK_EQ(ReflectionSimulator::EstimatePathingMemory(UINT32_MAX), UINT64_MAX);
    SA_CHECK_EQ(ReflectionSimulator::EstimatePathingMemory(0), uint64_t(0));
    SA_CHECK_EQ(ReflectionSimulator::PathingMemoryBudget(0, memory), uint64_t(0));
    memory.virtualAvailable = 256 * mib;
    SA_CHECK_EQ(ReflectionSimulator::PathingMemoryBudget(1024, memory), 128 * mib);
    memory.physicalAvailable = 4096 * mib;
    SA_CHECK_EQ(ReflectionSimulator::PathingMemoryBudget(1024, memory), uint64_t(0));
    memory.valid = false;
    SA_CHECK_EQ(ReflectionSimulator::PathingMemoryBudget(1024, memory), uint64_t(0));
}

SA_TEST(ReflectionBake_MemoryWatchdogDetectsGrowthAndExternalPressure)
{
    constexpr uint64_t mib = 1024 * 1024;
    ProcessMemoryInfo memory;
    memory.valid = true;
    memory.physicalTotal = 16384 * mib;
    memory.physicalAvailable = 8192 * mib;
    memory.commitAvailable = 12288 * mib;
    memory.privateBytes = 5000 * mib;
    SA_CHECK(!ReflectionSimulator::PathingMemoryExceeded(4000 * mib, 1024 * mib, memory));
    memory.privateBytes = 5025 * mib;
    SA_CHECK(ReflectionSimulator::PathingMemoryExceeded(4000 * mib, 1024 * mib, memory));
    memory.privateBytes = 4000 * mib;
    memory.physicalAvailable = 4000 * mib;
    SA_CHECK(ReflectionSimulator::PathingMemoryExceeded(4000 * mib, 1024 * mib, memory));
    memory.physicalAvailable = 8192 * mib;
    SA_CHECK(ReflectionSimulator::PathingMemoryExceeded(4000 * mib, 0, memory));
}

SA_TEST(ReflectionBake_EstimateIsPerStageAndHandlesUnknownProgress)
{
    SA_CHECK_NEAR(ReflectionSimulator::EstimateRemaining(10.0, 0.25f), 30.0, 1e-6);
    SA_CHECK_NEAR(ReflectionSimulator::EstimateRemaining(20.0, 1.f), 0.0, 1e-6);
    SA_CHECK(ReflectionSimulator::EstimateRemaining(10.0, 0.f) < 0.0);
    SA_CHECK(ReflectionSimulator::EstimateRemaining(0.01, 0.5f) < 0.0);
    SA_CHECK(ReflectionSimulator::EstimateRemaining(1.0, std::nanf("")) < 0.0);
}

SA_TEST(ReflectionBake_RealMapBackendBenchmark)
{
    const char* dir = PhononDir();
    const char* map = std::getenv("SA_TEST_BSP");
    if (!dir || !map || !*map)
        throw satest::Skipped{"SA_PHONON_DIR and SA_TEST_BSP are required"};
    std::ifstream file(map, std::ios::binary | std::ios::ate);
    SA_CHECK(file.good());
    const auto size = file.tellg();
    SA_CHECK(size > 0 && size < (1 << 29));
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    file.seekg(0);
    SA_CHECK(static_cast<bool>(file.read(reinterpret_cast<char*>(bytes.data()), size)));
    auto geometry = std::make_shared<BspGeometry>();
    SA_CHECK(ParseBsp(bytes.data(), bytes.size(), CoordinateConverter{}, MaterialLibrary{}, {}, *geometry));
    StaticConfig fixed;
    fixed.simulationThreads = 1;
    fixed.maxSources = 1;
    PhononContext ctx;
    std::string error;
    SA_CHECK(ctx.Initialize(fixed, {dir}, error));
    RuntimeConfig runtime;
    int32_t probes = -1;
    for (bool accelerated : {false, true}) {
        CPUFallbackBackend backend;
        fixed.enableEmbree = accelerated;
        SA_CHECK(backend.Initialize(ctx, fixed, error));
        const BackendDevices devices = backend.Devices();
        Simulator simulator;
        SA_CHECK(simulator.Initialize(ctx, devices, fixed, error));
        ReflectionSimulator reflections;
        SA_CHECK(reflections.Initialize(ctx, simulator, fixed, devices));
        SA_CHECK(reflections.StartBake(geometry, runtime, "", {}));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (reflections.BakeInProgress() && reflections.Status().completedProbes < 32 &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        const BakeStatus status = reflections.Status();
        reflections.CancelBake();
        while (reflections.BakeInProgress() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        SA_CHECK(!reflections.BakeInProgress());
        SA_CHECK(status.completedProbes > 0);
        SA_CHECK_EQ(status.sceneType, devices.sceneType);
        if (probes >= 0)
            SA_CHECK_EQ(status.probes, probes);
        probes = status.probes;
        std::printf("    backend=%s triangles=%zu probes=%d completed=%d stage=%.3fs rate=%.1f probes/s\n",
                    accelerated ? "Embree" : "Default", geometry->world.triangles.size(), status.probes,
                    status.completedProbes, status.updatedAt - status.phaseStartedAt,
                    status.completedProbes / std::max(0.001, status.updatedAt - status.phaseStartedAt));
    }
}

SA_TEST(HrtfRenderer_ReflectionTailSurvivesSourceRelease)
{
    const char* dir = PhononDir();
    if (!dir)
        throw satest::Skipped{"SA_PHONON_DIR not set"};
    for (IPLReflectionEffectType type : {IPL_REFLECTIONEFFECTTYPE_CONVOLUTION, IPL_REFLECTIONEFFECTTYPE_HYBRID}) {
        StaticConfig fixed;
        fixed.maxSources = 2;
        fixed.simulationThreads = 1;
        PhononContext ctx;
        std::string error;
        SA_CHECK(ctx.Initialize(fixed, {dir}, error));
        BackendDevices devices;
        devices.reflectionType = type;
        Simulator simulator;
        SA_CHECK(simulator.Initialize(ctx, devices, fixed, error));
        BspGeometry geometry;
        const auto bytes = satest::MakeRoomBsp();
        CoordinateConverter converter;
        SA_CHECK(ParseBsp(bytes.data(), bytes.size(), converter, MaterialLibrary{}, {}, geometry));
        SceneBuilder scene;
        SA_CHECK(scene.Initialize(ctx, devices));
        SA_CHECK(scene.AddStaticMesh(geometry.world, "tail_room"));
        scene.Commit();
        simulator.SetScene(scene.Scene());
        SoundSource source(1, SourceKind::Procedural, 1, 44100, 44100, 8192);
        SourceParams params;
        params.position = {0, 0, 60};
        params.positionValid = 1;
        source.SetParams(params);
        SA_CHECK(simulator.CreateSource(source));
        const IPLSimulationFlags flags = static_cast<IPLSimulationFlags>(IPL_SIMULATIONFLAGS_DIRECT |
                                                                         IPL_SIMULATIONFLAGS_REFLECTIONS);
        IPLSimulationInputs in{};
        in.flags = flags;
        in.directFlags = IPL_DIRECTSIMULATIONFLAGS_DISTANCEATTENUATION;
        in.source = converter.FrameToSA(params.position, {1, 0, 0}, {0, 0, 1});
        in.distanceAttenuationModel.type = IPL_DISTANCEATTENUATIONTYPE_DEFAULT;
        in.distanceAttenuationModel.minDistance = 1.f;
        for (float& scale : in.reverbScale) scale = 1.f;
        in.hybridReverbTransitionTime = 0.1f;
        in.hybridReverbOverlapPercent = 0.25f;
        iplSourceSetInputs(source.Sim().source, flags, &in);
        ListenerState listener;
        listener.valid = true;
        listener.frame = converter.FrameToSA({0, 100, 60}, {1, 0, 0}, {0, 0, 1});
        IPLSimulationSharedInputs shared{};
        shared.listener = listener.frame;
        shared.numRays = 512;
        shared.numBounces = 64;
        shared.duration = 1.5f;
        shared.order = 1;
        shared.irradianceMinDistance = 1.f;
        simulator.SetSharedInputs(flags, shared);
        simulator.Commit();
        simulator.RunDirect();
        simulator.RunReflections();
        source.PublishSimulationSource(source.Sim().source);
        source.MarkSimulationReady(flags);
        HRTFRenderer renderer;
        SA_CHECK(renderer.Initialize(ctx, devices, fixed, error));
        RuntimeConfig cfg;
        cfg.directGain = 0.f;
        cfg.irDuration = 1.5f;
        cfg.pathing = cfg.doppler = false;
        std::vector<float> pcm(static_cast<size_t>(ctx.FrameSize()), 0.f);
        const float* input[] = {pcm.data()};
        uint32_t random = 42;
        double lateEnergy = 0.0;
        for (int frame = 0; frame < 200; ++frame) {
            for (float& sample : pcm) {
                random = random * 1664525u + 1013904223u;
                sample = frame < 20 ? (static_cast<float>(random >> 8) / 8388608.f - 1.f) * 0.5f : 0.f;
            }
            renderer.BeginFrame(cfg, listener);
            if (frame < 25)
                renderer.RenderSource(source, input, 1, 1.f);
            else if (frame == 25) {
                renderer.ReleaseEffects(source.Render().effects);
                source.Render().effects = nullptr;
            }
            renderer.EndFrame();
            if (frame >= 60)
                for (int i = 0; i < ctx.FrameSize(); ++i)
                    lateEnergy += renderer.MasterLeft()[i] * renderer.MasterLeft()[i] +
                                  renderer.MasterRight()[i] * renderer.MasterRight()[i];
        }
        std::printf("    reflection type=%d late energy=%g tails cut=%llu\n", static_cast<int>(type), lateEnergy,
                    static_cast<unsigned long long>(renderer.TailsCut()));
        SA_CHECK(lateEnergy > 1e-6);
        source.PublishSimulationSource(nullptr);
        simulator.DestroySource(source);
        simulator.ProcessDeferredReleases(true);
    }
}

SA_TEST(HrtfRenderer_LocalWeaponKeepsDirectStereoLevel)
{
    const char* dir = PhononDir();
    if (!dir)
        throw satest::Skipped{"SA_PHONON_DIR not set"};
    StaticConfig fixed;
    fixed.maxSources = 2;
    PhononContext ctx;
    std::string error;
    SA_CHECK(ctx.Initialize(fixed, {dir}, error));
    HRTFRenderer renderer;
    SA_CHECK(renderer.Initialize(ctx, BackendDevices{}, fixed, error));
    SoundSource source(1, SourceKind::EngineChannel, 2, 44100, 44100, 8192);
    SourceParams params;
    params.listenerRelative = 1;
    params.positionValid = 1;
    params.position = {10000.f, 0, -5000.f};
    params.distMult = 1.f;
    params.dipoleWeight = 1.f;
    source.SetParams(params);
    ListenerState listener;
    listener.valid = true;
    listener.frame = CoordinateConverter{}.FrameToSA({}, {1, 0, 0}, {0, 0, 1});
    RuntimeConfig cfg;
    cfg.spatializeStereo = false;
    std::vector<float> left(static_cast<size_t>(ctx.FrameSize()), 0.4f), right(left.size(), 0.2f);
    const float* input[] = {left.data(), right.data()};
    renderer.BeginFrame(cfg, listener);
    renderer.RenderSource(source, input, 2, 1.f);
    renderer.EndFrame();
    SA_CHECK_EQ(source.LoadRenderDiag().mode, RenderDiag::kListenerRelative);
    SA_CHECK_NEAR(renderer.MasterLeft()[0], 0.4f, 1e-6);
    SA_CHECK_NEAR(renderer.MasterRight()[0], 0.2f, 1e-6);
    SA_CHECK_NEAR(source.LoadRenderDiag().distanceAttenuation, 1.f, 1e-6);
    renderer.ReleaseEffects(source.Render().effects);
    source.Render().effects = nullptr;
}

SA_TEST(HrtfRenderer_FirstSampleTransientIsPreserved)
{
    const char* dir = PhononDir();
    if (!dir)
        throw satest::Skipped{"SA_PHONON_DIR not set"};
    PhononContext ctx;
    StaticConfig fixed;
    fixed.maxSources = 2;
    std::string error;
    SA_CHECK(ctx.Initialize(fixed, {dir}, error));
    HRTFRenderer renderer;
    SA_CHECK(renderer.Initialize(ctx, BackendDevices{}, fixed, error));
    RuntimeConfig cfg;
    cfg.reflections = cfg.pathing = cfg.doppler = false;
    ListenerState listener;
    listener.valid = true;
    listener.frame = CoordinateConverter{}.FrameToSA({}, {1, 0, 0}, {0, 0, 1});
    for (bool hrtf : {false, true}) {
        cfg.hrtf = hrtf;
        SoundSource source(1, SourceKind::EngineChannel, 1, 44100, 44100, 8192);
        SourceParams params;
        params.positionValid = 1;
        params.position = {100.f, 0, 0};
        source.SetParams(params);
        std::vector<float> input(static_cast<size_t>(ctx.FrameSize()), 0.f);
        input[0] = 0.5f;
        const float* channels[] = {input.data()};
        double energy = 0.0;
        for (int frame = 0; frame < 4; ++frame) {
            renderer.BeginFrame(cfg, listener);
            renderer.RenderSource(source, channels, 1, 1.f);
            renderer.EndFrame();
            for (int i = 0; i < ctx.FrameSize(); ++i)
                energy += renderer.MasterLeft()[i] * renderer.MasterLeft()[i] +
                          renderer.MasterRight()[i] * renderer.MasterRight()[i];
            input[0] = 0.f;
        }
        renderer.ReleaseEffects(source.Render().effects);
        source.Render().effects = nullptr;
        SA_CHECK(energy > 0.01);
    }
}

SA_TEST(HrtfRenderer_PoolRecoveryAndVolumeRamps)
{
    const char* dir = PhononDir();
    if (!dir)
        throw satest::Skipped{"SA_PHONON_DIR not set"};
    PhononContext ctx;
    StaticConfig cfg;
    cfg.maxSources = 1;
    std::string error;
    SA_CHECK(ctx.Initialize(cfg, {dir}, error));
    HRTFRenderer renderer;
    SA_CHECK(renderer.Initialize(ctx, BackendDevices{}, cfg, error));
    RuntimeConfig rt;
    rt.doppler = false;
    rt.reflections = false;
    rt.pathing = false;
    ListenerState listener;
    listener.valid = true;
    listener.frame = CoordinateConverter{}.FrameToSA({}, {1, 0, 0}, {0, 0, 1});
    SoundSource a(1, SourceKind::Procedural, 1, 44100, 44100, 8192);
    SoundSource b(2, SourceKind::Procedural, 1, 44100, 44100, 8192);
    SourceParams p;
    p.position = {0, 100, 0};
    p.positionValid = 1;
    a.SetParams(p);
    b.SetParams(p);
    std::vector<float> pcm(static_cast<size_t>(ctx.FrameSize()), 0.2f);
    const float* input[] = {pcm.data()};
    renderer.BeginFrame(rt, listener);
    renderer.RenderSource(a, input, 1, 1.f);
    renderer.RenderSource(b, input, 1, 1.f);
    renderer.EndFrame();
    SA_CHECK(a.Render().effects != nullptr);
    SA_CHECK_EQ(b.LoadRenderDiag().mode, RenderDiag::kNoEffects);
    renderer.ReleaseEffects(a.Render().effects);
    a.Render().effects = nullptr;
    renderer.BeginFrame(rt, listener);
    renderer.RenderSource(b, input, 1, 1.f);
    renderer.EndFrame();
    SA_CHECK_EQ(b.LoadRenderDiag().mode, RenderDiag::kHrtf);
    p.spatialize = 0;
    b.SetParams(p);
    renderer.BeginFrame(rt, listener);
    renderer.RenderSource(b, input, 1, 1.f);
    renderer.EndFrame();
    SA_CHECK_EQ(renderer.FreeEffectSets(), size_t(1));
    const float previous = renderer.MasterLeft()[ctx.FrameSize() - 1];
    p.gain = 0.f;
    b.SetParams(p);
    renderer.BeginFrame(rt, listener);
    renderer.RenderSource(b, input, 1, 1.f);
    renderer.EndFrame();
    SA_CHECK(std::fabs(renderer.MasterLeft()[0] - previous) < 0.01f);
    SA_CHECK_NEAR(renderer.MasterLeft()[ctx.FrameSize() - 1], 0.f, 1e-6);
}

SA_TEST(HrtfRenderer_LateralSourcesReachTheNearEar)
{
    const char* dir = PhononDir();
    if (!dir) {
        throw satest::Skipped{"SA_PHONON_DIR not set"};
    }
    StaticConfig cfg;
    cfg.maxSources = 4;
    PhononContext ctx;
    std::string err;
    SA_CHECK(ctx.Initialize(cfg, {dir}, err));
    HRTFRenderer renderer;
    BackendDevices devices;
    SA_CHECK(renderer.Initialize(ctx, devices, cfg, err));
    if (!renderer.IsValid()) {
        std::printf("    %s\n", err.c_str());
        return;
    }

    // Source space: +X forward, +Y left, +Z up.
    const Channels left = RenderAt(ctx, renderer, Vec3{0.f, 200.f, 0.f}, true);
    const MixBalance afterLeft = renderer.Balance();
    SA_CHECK(afterLeft.masterL > 4.f * afterLeft.masterR);
    SA_CHECK(afterLeft.direct > 0.f);
    SA_CHECK(afterLeft.reflections == 0.f);
    const Channels right = RenderAt(ctx, renderer, Vec3{0.f, -200.f, 0.f}, true);
    const Channels front = RenderAt(ctx, renderer, Vec3{200.f, 0.f, 0.f}, true);
    std::printf("    hrtf  left  L/R %+.1f dB   right L/R %+.1f dB   front L/R %+.1f dB\n", Db(left.left, left.right),
                Db(right.left, right.right), Db(front.left, front.right));
    SA_CHECK(Db(left.left, left.right) > 6.0);
    SA_CHECK(Db(right.right, right.left) > 6.0);
    SA_CHECK(std::fabs(Db(front.left, front.right)) < 3.0);

    const Channels panLeft = RenderAt(ctx, renderer, Vec3{0.f, 200.f, 0.f}, false);
    const Channels panRight = RenderAt(ctx, renderer, Vec3{0.f, -200.f, 0.f}, false);
    std::printf("    pan   left  L/R %+.1f dB   right L/R %+.1f dB\n", Db(panLeft.left, panLeft.right),
                Db(panRight.left, panRight.right));
    SA_CHECK(Db(panLeft.left, panLeft.right) > 6.0);
    SA_CHECK(Db(panRight.right, panRight.left) > 6.0);

    renderer.Shutdown();
    ctx.Shutdown();
}
