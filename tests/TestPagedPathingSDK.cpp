#include "TestFramework.h"
#include "steamaudio/PhononContext.h"
#include "steamaudio/SceneBuilder.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

using namespace sa;

namespace {
void IPLCALL PathProgress(float, void*) {}
struct CacheFiles {
    std::string path;
    ~CacheFiles()
    {
        std::error_code error;
        std::filesystem::remove(std::filesystem::u8path(path), error);
        std::filesystem::remove(std::filesystem::u8path(path + ".partial"), error);
    }
};
struct Batches {
    std::vector<IPLProbeBatch> values;
    ~Batches() { for (auto& batch : values) if (batch) iplProbeBatchRelease(&batch); }
    IPLProbeBatch Add(PhononContext& context, const std::vector<IPLSphere>& probes)
    {
        IPLProbeBatch batch = nullptr;
        SA_CHECK(iplProbeBatchCreate(context.Handle(), &batch) == IPL_STATUS_SUCCESS);
        values.push_back(batch);
        for (const auto& probe : probes) iplProbeBatchAddProbe(batch, probe);
        iplProbeBatchCommit(batch);
        return batch;
    }
};
}

SA_TEST(PagedPathingSDK_MatchesLegacyRoutesAndRoundTrips)
{
    const char* directory = std::getenv("SA_PHONON_DIR");
    if (!directory) throw satest::Skipped{"SA_PHONON_DIR is required"};
    PhononContext context;
    StaticConfig fixed;
    fixed.maxSources = 2;
    fixed.maxIrDuration = 0.1f;
    std::string error;
    SA_CHECK(context.Initialize(fixed, {directory}, error));
    const auto& api = phonon::Api().pathCache;
    if (!api.Available()) {
        SA_CHECK(std::getenv("SA_REQUIRE_PAGED_PATHING") == nullptr);
        throw satest::Skipped{"Paged pathing extension not available in this reference SDK"};
    }
    const auto temp = std::filesystem::temp_directory_path();
    CacheFiles files{(temp / ("sa_sdk_paths_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".paths")).u8string()};
    constexpr uint64_t key = 567891;
    constexpr uint64_t budget = 8ull * 1024 * 1024;
    SA_CHECK_EQ(api.configure(temp.u8string().c_str(), "", key, budget), 0);
    SceneBuilder scene;
    SA_CHECK(scene.Initialize(context, BackendDevices{}));
    MeshData wall;
    IPLMaterial material{};
    material.absorption[0] = material.absorption[1] = material.absorption[2] = 0.05f;
    const auto index = wall.AddMaterial(material);
    wall.AddTriangle({0, 0, -8}, {0, 4, -8}, {0, 4, 4}, index);
    wall.AddTriangle({0, 0, -8}, {0, 4, 4}, {0, 0, 4}, index);
    SA_CHECK(scene.AddStaticMesh(wall, "path_test_wall"));
    scene.Commit();
    std::vector<IPLSphere> probes;
    for (float x : {-4.5f, -1.5f, 1.5f, 4.5f})
        for (float z : {-6.f, -3.f, 0.f, 3.f, 6.f})
            probes.push_back(IPLSphere{{x, 1.5f, z}, 3.f});
    Batches batches;
    const auto legacy = batches.Add(context, probes);
    const auto paged = batches.Add(context, probes);
    IPLPathBakeParams params{};
    params.scene = scene.Scene();
    params.identifier.type = IPL_BAKEDDATATYPE_PATHING;
    params.identifier.variation = IPL_BAKEDDATAVARIATION_DYNAMIC;
    params.numSamples = 1;
    params.radius = 0.1f;
    params.threshold = 0.1f;
    params.visRange = 4.f;
    params.pathRange = 100.f;
    params.numThreads = 2;
    params.probeBatch = legacy;
    iplPathBakerBake(context.Handle(), &params, PathProgress, nullptr);
    SA_CHECK_EQ(api.configure(temp.u8string().c_str(), files.path.c_str(), key, budget), 0);
    params.probeBatch = paged;
    iplPathBakerBake(context.Handle(), &params, PathProgress, nullptr);
    PathCacheApiStats stats;
    SA_CHECK(api.stats(paged, &stats));
    SA_CHECK_EQ(stats.complete, 1u);
    SA_CHECK_EQ(stats.completedRows, probes.size());
    SA_CHECK(stats.diskBytes > 0 && stats.residentBytes < budget);
    size_t indirect = 0;
    for (int start = 0; start < static_cast<int>(probes.size()); ++start) {
        for (int end = 0; end < static_cast<int>(probes.size()); ++end) {
            PathCacheRecord expected, actual;
            const int a = api.lookup(legacy, start, end, &expected);
            const int b = api.lookup(paged, start, end, &actual);
            SA_CHECK_EQ(a, b);
            if (a) {
                SA_CHECK(std::memcmp(&expected, &actual, sizeof(actual)) == 0);
                indirect += actual.flags == 0 ? 1 : 0;
            }
        }
    }
    SA_CHECK(indirect > 0);
    IPLSerializedObject serialized = nullptr;
    IPLSerializedObjectSettings settings{};
    SA_CHECK(iplSerializedObjectCreate(context.Handle(), &settings, &serialized) == IPL_STATUS_SUCCESS);
    iplProbeBatchSave(paged, serialized);
    SA_CHECK(iplSerializedObjectGetSize(serialized) > 0);
    SA_CHECK_EQ(api.configure(temp.u8string().c_str(), "", key, budget), 0);
    IPLProbeBatch loaded = nullptr;
    const auto loadedStatus = iplProbeBatchLoad(context.Handle(), serialized, &loaded);
    iplSerializedObjectRelease(&serialized);
    SA_CHECK(loadedStatus == IPL_STATUS_SUCCESS && loaded);
    batches.values.push_back(loaded);
    SA_CHECK(api.stats(loaded, &stats));
    SA_CHECK_EQ(stats.complete, 1u);
    std::vector<PathCacheProbe> extracted(probes.size());
    SA_CHECK_EQ(api.probes(loaded, static_cast<uint32_t>(extracted.size()), extracted.data()), static_cast<int32_t>(probes.size()));
    for (size_t i = 0; i < probes.size(); ++i) {
        SA_CHECK_EQ(extracted[i].x, probes[i].center.x);
        SA_CHECK_EQ(extracted[i].y, probes[i].center.y);
        SA_CHECK_EQ(extracted[i].z, probes[i].center.z);
        SA_CHECK_EQ(extracted[i].radius, probes[i].radius);
    }
    for (int start = 0; start < static_cast<int>(probes.size()); ++start) {
        for (int end = 0; end < static_cast<int>(probes.size()); ++end) {
            PathCacheRecord expected, actual;
            const int a = api.lookup(legacy, start, end, &expected);
            const int b = api.lookup(loaded, start, end, &actual);
            SA_CHECK_EQ(a, b);
            if (a) SA_CHECK(std::memcmp(&expected, &actual, sizeof(actual)) == 0);
        }
    }
    SA_CHECK(api.attach(legacy, loaded));
    SA_CHECK(!api.stats(loaded, &stats));
    SA_CHECK(api.stats(legacy, &stats));
    SA_CHECK_EQ(stats.complete, 1u);
    SA_CHECK(stats.residentBytes < budget);
    std::printf("    %zu probe pairs matched; %zu indirect routes; paged resident %llu bytes, disk %llu bytes\n",
                probes.size() * probes.size(), indirect, static_cast<unsigned long long>(stats.residentBytes),
                static_cast<unsigned long long>(stats.diskBytes));
}
