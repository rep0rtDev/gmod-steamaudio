// tests/TestDsp.cpp
#include "TestFramework.h"
#include "mixing/Dsp.h"
#include "mixing/SoundSource.h"
#include "mixing/BassBridge.h"

#include <cmath>
#include <limits>
#include <vector>

using namespace sa;

namespace sa {
struct BassBridgeTestAccess {
    static bass::BOOL SA_BASSCALL Info(bass::DWORD, bass::ChannelInfo* info)
    {
        *info = {};
        info->freq = 44100;
        info->chans = 1;
        info->flags = bass::kSample3D;
        return 1;
    }
    static bass::HDSP SA_BASSCALL Dsp(bass::DWORD, bass::DspProc, void*, int32_t) { return 1; }
    static bass::BOOL SA_BASSCALL RemoveDsp(bass::DWORD, bass::HDSP) { return 1; }
    inline static bass::DWORD activeState = bass::kActivePlaying;
    static bass::DWORD SA_BASSCALL Active(bass::DWORD) { return activeState; }
    static bass::BOOL SA_BASSCALL Attribute(bass::DWORD, bass::DWORD, float* value) { *value = 44100.f; return 1; }
    static int32_t SA_BASSCALL Error() { return 0; }

    static void CheckFirstBlock(bool endBeforeTick = false)
    {
        activeState = bass::kActivePlaying;
        BassBridge bridge;
        bridge.m_initialized = true;
        bridge.m_attached.store(true);
        bridge.m_api.channelGetInfo = Info;
        bridge.m_api.channelSetDSP = Dsp;
        bridge.m_api.channelRemoveDSP = RemoveDsp;
        bridge.m_api.channelIsActive = Active;
        bridge.m_api.channelGetAttribute = Attribute;
        bridge.m_api.errorGetCode = Error;
        bool registered = false;
        bridge.m_hooks.allocateId = [] { return 1000u; };
        bridge.m_hooks.registerSource = [&](const SoundSourcePtr& source) {
            SA_CHECK_NEAR(source->GetParams().position.y, -1706.f, 1e-6);
            SA_CHECK_NEAR(source->GetParams().gain, 0.4f, 1e-6);
            registered = true;
            return true;
        };
        bridge.m_hooks.unregisterSource = [](const SoundSourcePtr&) {};
        bridge.OnStreamCreated(123, bass::kSample3D, "test");
        const auto channel = bridge.FindChannel(123);
        SA_CHECK(channel != nullptr);
        const bass::Vector3D position{-85.f, 1706.f, -144.f};
        bridge.OnPosition(123, &position, nullptr);
        bridge.OnVolume(123, 0.4f);
        std::vector<int16_t> pcm(1024, 16384);
        bridge.ProcessBlock(*channel, pcm.data(), static_cast<bass::DWORD>(pcm.size() * sizeof(int16_t)));
        SA_CHECK(!registered);
        SA_CHECK_EQ(channel->source->QueuedStreamFrames(), pcm.size());
        for (int16_t sample : pcm) SA_CHECK_EQ(sample, int16_t(0));
        const auto source = channel->source;
        if (endBeforeTick) {
            activeState = bass::kActiveStopped;
            bridge.OnStreamFreed(123);
        }
        bridge.Tick(RuntimeConfig{});
        SA_CHECK(registered);
        SA_CHECK_NEAR(source->GetParams().position.y, -1706.f, 1e-6);
        SA_CHECK_EQ(source->QueuedStreamFrames(), pcm.size());
    }
};
}

SA_TEST(BassBridge_CapturesFirstBlockBeforeRegistration)
{
    BassBridgeTestAccess::CheckFirstBlock();
}

SA_TEST(BassBridge_ShortStreamFinishesBeforeFirstTickWithoutLosingPcm)
{
    BassBridgeTestAccess::CheckFirstBlock(true);
}

SA_TEST(BassCoordinates_GmodRoundTripKeepsSourceNearListener)
{
    BassBridgeOptions options;
    const Vec3 actualSource{-85.f, -1706.f, -144.f};
    const Vec3 listener{44.f, -1725.f, -80.f};
    const bass::Vector3D bassPosition{actualSource.x, -actualSource.y, actualSource.z};
    const Vec3 restored = options.ToSource(bassPosition);
    SA_CHECK_NEAR(restored.x, actualSource.x, 1e-6);
    SA_CHECK_NEAR(restored.y, actualSource.y, 1e-6);
    SA_CHECK_NEAR(restored.z, actualSource.z, 1e-6);
    SA_CHECK(Vec3::Distance(restored, listener) < 150.f);
    SA_CHECK_NEAR(options.ToSource({0.f, 1.f, 0.f}).y, -1.f, 1e-6);
}

SA_TEST(Resampler_UnityRatioIsTransparent)
{
    dsp::Resampler rs;
    rs.Prepare(1024);
    std::vector<float> in(100);
    for (size_t i = 0; i < in.size(); ++i)
        in[i] = static_cast<float>(i);
    rs.Push(in.data(), in.size());
    std::vector<float> out(100);
    const size_t n = rs.Pull(out.data(), out.size(), 1.0);
    SA_CHECK(n >= 96); // 3 history + 1 lookahead consumed by the window
    // History primes with 3 zeros so output k corresponds to input k-2.
    for (size_t k = 4; k < n; ++k)
        SA_CHECK_NEAR(out[k], in[k - 2], 1e-3);
}

SA_TEST(Resampler_NeverDropsSamplesAcrossBlocks)
{
    dsp::Resampler rs;
    rs.Prepare(4096);
    const double ratio = 44100.0 / 48000.0;
    // Feed a ramp in irregular block sizes and pull in irregular sizes; the
    // output must remain a monotonically increasing ramp (no discontinuity).
    float value = 0.f;
    std::vector<float> out;
    size_t blockSizes[] = {37, 128, 5, 511, 64, 300};
    for (int round = 0; round < 40; ++round) {
        const size_t bs = blockSizes[round % 6];
        std::vector<float> block(bs);
        for (float& s : block)
            s = value++;
        SA_CHECK_EQ(rs.Push(block.data(), bs), bs);
        std::vector<float> tmp(bs * 2);
        const size_t n = rs.Pull(tmp.data(), tmp.size(), ratio);
        out.insert(out.end(), tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(n));
    }
    SA_CHECK(out.size() > 800);
    // Skip the first outputs, which interpolate across the zero history.
    for (size_t i = 8; i < out.size(); ++i) {
        const float step = out[i] - out[i - 1];
        SA_CHECK_NEAR(step, ratio, 0.02);
    }
}

SA_TEST(Resampler_InputNeededMatchesPull)
{
    dsp::Resampler rs;
    rs.Prepare(4096);
    const double ratio = 1.5;
    const size_t need = rs.InputNeeded(100, ratio);
    std::vector<float> in(need, 0.5f);
    rs.Push(in.data(), in.size());
    std::vector<float> out(100);
    SA_CHECK_EQ(rs.Pull(out.data(), 100, ratio), size_t(100));
}

SA_TEST(SoundSource_HighRateFillsLargeOutputFrame)
{
    SoundSource source(1, SourceKind::Procedural, 1, 44100, 192000, 32768);
    std::vector<float> input(20000, 0.2f), output(4096);
    SA_CHECK_EQ(source.WriteInterleaved(input.data(), input.size()), input.size());
    source.SetEndOfStream();
    float* channels[] = {output.data()};
    SA_CHECK_EQ(source.ReadStreamFrames(channels, output.size()), output.size());
    SA_CHECK(dsp::AllFinite(output.data(), output.size()));
    SA_CHECK_NEAR(output.back(), 0.2f, 1e-5);
}

SA_TEST(SoundSource_SanitizesPcmAndAppliesProceduralPitch)
{
    SoundSource source(2, SourceKind::Procedural, 1, 44100, 44100, 8192);
    SourceParams params;
    params.pitch = 2.f;
    source.SetParams(params);
    std::vector<float> input(4096, 0.25f), output(512);
    input[10] = std::nanf("");
    input[11] = std::numeric_limits<float>::infinity();
    source.WriteInterleaved(input.data(), input.size());
    source.SetEndOfStream();
    float* channels[] = {output.data()};
    SA_CHECK_EQ(source.ReadStreamFrames(channels, output.size()), output.size());
    SA_CHECK(dsp::AllFinite(output.data(), output.size()));
    SA_CHECK(source.QueuedStreamFrames() < 3200);
}

SA_TEST(Smoother_ConvergesToTarget)
{
    dsp::Smoother s;
    s.SetTimeConstant(0.01f, 48000.f);
    s.Snap(0.f);
    float v = 0.f;
    for (int i = 0; i < 48000; ++i)
        v = s.Next(1.f);
    SA_CHECK_NEAR(v, 1.f, 1e-3);
}
