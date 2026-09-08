// tests/TestChannelCapture.cpp
//
// Drives ChannelCapture the way the engine mixer drives IAudioDevice: one
// paint iteration = MixBegin, Mix* calls per channel per rate group,
// TransferSamples. Checks that low-rate groups (11025 / 22050 Hz paint
// buffers) end up at the DMA rate in the source ring and that the same
// channel mixed into two paint buffers is not summed twice.
#include "TestFramework.h"
#include "core/ChannelCapture.h"
#include "core/SourceInterfaces.h"
#include "mixing/SoundSource.h"

#include <cmath>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

using namespace sa;

namespace {

constexpr uint32_t kDma = 44100;
constexpr int32_t kIter = 512; // samples per iteration at the DMA rate

struct Harness {
    ChannelCapture capture;
    std::vector<SoundSourcePtr> sources;
    int32_t painted = 0;
    uint64_t clock = 0; // ring clock of the iteration currently being mixed

    Harness()
    {
        for (uint32_t i = 0; i < 4; ++i)
            sources.push_back(std::make_shared<SoundSource>(i, SourceKind::EngineChannel, 2, kDma, kDma, 1 << 16));
        capture.Initialize(sources, kDma);
        capture.OnPaintBegin(0, 0, kIter);
        clock = uint64_t(1) << 32;
    }

    void Begin() { capture.OnMixBegin(kIter); }

    void End()
    {
        painted += kIter;
        capture.OnTransferSamples(painted);
        clock += kIter;
    }

    // Mixes `count` 16-bit mono samples of `data` at 1:1 rate into channel `ch`.
    void Mix16(void* ch, const std::vector<int16_t>& data, int32_t count, int32_t outputOffset = 0)
    {
        capture.CaptureMix(static_cast<src::channel_t*>(ch), data.data(), true, false, outputOffset, 0,
                           static_cast<int32_t>(src::kFixScale), count);
    }

    std::vector<float> Read(uint32_t slot, uint64_t at, size_t count)
    {
        std::vector<float> out(count, 0.f);
        TimedSampleRing* ring = sources[slot]->TimedRing(0);
        SA_CHECK(ring != nullptr);
        ring->ReadAt(at, out.data(), count);
        return out;
    }
};

std::vector<int16_t> Ramp(int32_t count, int16_t step)
{
    std::vector<int16_t> v(static_cast<size_t>(count));
    for (int32_t i = 0; i < count; ++i)
        v[static_cast<size_t>(i)] = static_cast<int16_t>(i * step);
    return v;
}

} // namespace

SA_TEST(ChannelCapture_StockImpactsAndExplosionsKeepEverySample)
{
    const char* directory = std::getenv("SA_TEST_SOUND_DIR");
    if (!directory || !*directory)
        throw satest::Skipped{"SA_TEST_SOUND_DIR not set"};
    for (const char* name : {"physics/concrete/concrete_impact_hard1.wav", "weapons/crowbar/crowbar_impact1.wav",
                              "weapons/explode3.wav", "ambient/explosions/explode_4.wav"}) {
        std::ifstream file(std::filesystem::u8path(directory) / name, std::ios::binary | std::ios::ate);
        SA_CHECK(file.good());
        const auto size = file.tellg();
        SA_CHECK(size >= 12 && size < (16 << 20));
        std::vector<uint8_t> bytes(static_cast<size_t>(size));
        file.seekg(0);
        SA_CHECK(static_cast<bool>(file.read(reinterpret_cast<char*>(bytes.data()), size)));
        SA_CHECK(std::memcmp(bytes.data(), "RIFF", 4) == 0);
        uint16_t channels = 0, bits = 0, format = 0;
        uint32_t rate = 0;
        std::vector<int16_t> pcm;
        for (size_t offset = 12; offset + 8 <= bytes.size();) {
            uint32_t length = 0;
            std::memcpy(&length, bytes.data() + offset + 4, 4);
            SA_CHECK(length <= bytes.size() - offset - 8);
            if (std::memcmp(bytes.data() + offset, "fmt ", 4) == 0) {
                SA_CHECK(length >= 16);
                std::memcpy(&format, bytes.data() + offset + 8, 2);
                std::memcpy(&channels, bytes.data() + offset + 10, 2);
                std::memcpy(&rate, bytes.data() + offset + 12, 4);
                std::memcpy(&bits, bytes.data() + offset + 22, 2);
            } else if (std::memcmp(bytes.data() + offset, "data", 4) == 0) {
                SA_CHECK(length % 2 == 0);
                pcm.resize(length / 2);
                std::memcpy(pcm.data(), bytes.data() + offset + 8, length);
            }
            offset += 8 + length + (length & 1u);
        }
        SA_CHECK_EQ(format, uint16_t(1));
        SA_CHECK_EQ(bits, uint16_t(16));
        SA_CHECK_EQ(rate, kDma);
        SA_CHECK(channels == 1 || channels == 2);
        SA_CHECK(!pcm.empty());
        Harness h;
        int channel = 0;
        const size_t frames = pcm.size() / channels;
        for (size_t begin = 0; begin < frames; begin += kIter) {
            const size_t count = std::min<size_t>(kIter, frames - begin);
            h.Begin();
            h.capture.CaptureMix(reinterpret_cast<src::channel_t*>(&channel), pcm.data() + begin * channels,
                                 true, channels == 2, 0, 0, static_cast<int32_t>(src::kFixScale), static_cast<int32_t>(count));
            h.End();
            for (uint32_t c = 0; c < channels; ++c) {
                std::vector<float> out(kIter);
                h.sources[0]->TimedRing(c)->ReadAt(h.clock - kIter, out.data(), out.size());
                for (size_t i = 0; i < out.size(); ++i) {
                    const float expected = i < count ? pcm[(begin + i) * channels + c] / 32768.f : 0.f;
                    SA_CHECK_NEAR(out[i], expected, 1e-6);
                }
            }
        }
        SA_CHECK_EQ(h.capture.Stats().samplesCaptured.load(), frames);
        std::printf("    %s: %zu frames including tail preserved\n", name, frames);
    }
}

SA_TEST(ChannelCapture_ConfiguredLayoutIsImmediatelyVisible)
{
    ChannelCapture capture;
    capture.Initialize({}, kDma);
    ChannelLayout pinned;
    pinned.origin = 96;
    pinned.guid = 32;
    capture.SetLayoutOverrides(pinned);
    SA_CHECK_EQ(capture.Layout().origin, 96);
    SA_CHECK_EQ(capture.Layout().guid, 32);
}

SA_TEST(ChannelCapture_PublishesPoseBeforeMetadataPoll)
{
    Harness h;
    ChannelLayout layout;
    layout.origin = 96;
    layout.guid = 32;
    h.capture.SetLayoutOverrides(layout);
    std::vector<uint8_t> channel(256, 0);
    Vec3 origin{100.f, 20.f, 30.f};
    int32_t guid = 1;
    std::memcpy(channel.data() + 96, &origin, sizeof(origin));
    std::memcpy(channel.data() + 32, &guid, sizeof(guid));
    const std::vector<int16_t> pcm(kIter, 4096);
    h.Begin();
    h.Mix16(channel.data(), pcm, kIter);
    h.End();
    SourceParams p = h.sources[0]->GetParams();
    SA_CHECK(p.positionValid && p.spatialize);
    SA_CHECK_NEAR(p.position.x, 100.f, 1e-6);
    p.gain = 0.25f;
    h.sources[0]->SetParams(p);
    origin.x = 200.f;
    guid = 2;
    std::memcpy(channel.data() + 96, &origin, sizeof(origin));
    std::memcpy(channel.data() + 32, &guid, sizeof(guid));
    h.Begin();
    h.Mix16(channel.data(), pcm, kIter);
    h.End();
    p = h.sources[0]->GetParams();
    SA_CHECK_NEAR(p.position.x, 200.f, 1e-6);
    SA_CHECK_NEAR(p.gain, 1.f, 1e-6);
}

SA_TEST(ChannelCapture_MetadataFollowsGuidAcrossGenerations)
{
    SoundSource source(1, SourceKind::EngineChannel, 1, kDma, kDma, 8192);
    SourceParams captured;
    captured.positionValid = 1;
    captured.position = {100.f, 0.f, 0.f};
    source.SetCapturedParams(captured, 1, 10);
    SourceParams game = captured;
    game.gain = 0.25f;
    game.entityIndex = 42;
    game.position.x = 200.f;
    source.SetEngineParams(game, 11);
    SA_CHECK_NEAR(source.GetParams().gain, 1.f, 1e-6);
    captured.position.x = 200.f;
    source.SetCapturedParams(captured, 2, 11);
    SA_CHECK_NEAR(source.GetParams().gain, 0.25f, 1e-6);
    SA_CHECK_EQ(source.GetParams().entityIndex, 42);
}

SA_TEST(ChannelCapture_FullRateTailDoesNotChangeRateGroup)
{
    Harness h;
    int channel = 0;
    const std::vector<int16_t> pcm(kIter, 8192);
    h.Begin();
    h.Mix16(&channel, pcm, kIter);
    h.End();
    h.Begin();
    h.Mix16(&channel, pcm, kIter / 2);
    h.End();
    const auto out = h.Read(0, h.clock - kIter, kIter);
    SA_CHECK_NEAR(out[kIter / 2 - 1], 0.25f, 1e-6);
    SA_CHECK_NEAR(out[kIter / 2], 0.f, 1e-6);
    SA_CHECK_EQ(h.capture.Slot(0).inputRate.load(), kDma);
}

SA_TEST(ChannelCapture_FullRateBlockIsStoredAsIs)
{
    Harness h;
    int ch = 0;
    const std::vector<int16_t> pcm = Ramp(kIter, 16);
    h.Begin();
    h.Mix16(&ch, pcm, kIter);
    h.End();

    const std::vector<float> out = h.Read(0, h.clock - kIter, kIter);
    for (int32_t i = 0; i < kIter; i += 37)
        SA_CHECK_NEAR(out[static_cast<size_t>(i)], pcm[static_cast<size_t>(i)] / 32768.f, 1e-6f);
    SA_CHECK_EQ(h.capture.Stats().blocks44k.load(), 1u);
    SA_CHECK_EQ(h.capture.Stats().blocks22k.load(), 0u);
    SA_CHECK_EQ(h.capture.Stats().blocks11k.load(), 0u);
}

SA_TEST(ChannelCapture_HalfRateBlockIsUpsampledToDma)
{
    Harness h;
    int ch = 0;
    // The 22 kHz group delivers kIter / 2 samples per iteration.
    const std::vector<int16_t> pcm = Ramp(kIter / 2, 64);
    h.Begin();
    h.Mix16(&ch, pcm, kIter / 2);
    h.End();

    const std::vector<float> out = h.Read(0, h.clock - kIter, kIter);
    // The block must span the whole iteration (no silent second half) and
    // rise monotonically like the ramp.
    SA_CHECK(out[kIter - 1] > 0.9f * (pcm.back() / 32768.f));
    for (int32_t i = 1; i < kIter; ++i)
        SA_CHECK(out[static_cast<size_t>(i)] >= out[static_cast<size_t>(i - 1)] - 1e-6f);
    // Linear interpolation lands between neighbouring input samples.
    for (int32_t i = 8; i < kIter - 8; i += 51) {
        const float lo = pcm[static_cast<size_t>(i / 2 - 1)] / 32768.f;
        const float hi = pcm[static_cast<size_t>(i / 2 + 1)] / 32768.f;
        SA_CHECK(out[static_cast<size_t>(i)] >= lo - 1e-6f && out[static_cast<size_t>(i)] <= hi + 1e-6f);
    }
    SA_CHECK_EQ(h.capture.Stats().blocks22k.load(), 1u);
}

SA_TEST(ChannelCapture_QuarterRateBlockIsUpsampledToDma)
{
    Harness h;
    int ch = 0;
    const std::vector<int16_t> pcm(static_cast<size_t>(kIter / 4), 8192);
    h.Begin();
    h.Mix16(&ch, pcm, kIter / 4);
    h.End();

    const std::vector<float> out = h.Read(0, h.clock - kIter, kIter);
    // A DC block stays DC over the whole iteration once the interpolation
    // history (silence before the first block) has settled.
    for (int32_t i = 8; i < kIter; ++i)
        SA_CHECK_NEAR(out[static_cast<size_t>(i)], 0.25f, 1e-5f);
    SA_CHECK_EQ(h.capture.Stats().blocks11k.load(), 1u);
}

SA_TEST(ChannelCapture_LowRateGroupIsContinuousAcrossIterations)
{
    Harness h;
    int ch = 0;
    const std::vector<int16_t> a = Ramp(kIter / 2, 32);
    std::vector<int16_t> b(static_cast<size_t>(kIter / 2));
    for (int32_t i = 0; i < kIter / 2; ++i)
        b[static_cast<size_t>(i)] = static_cast<int16_t>((kIter / 2 + i) * 32);

    h.Begin();
    h.Mix16(&ch, a, kIter / 2);
    h.End();
    h.Begin();
    h.Mix16(&ch, b, kIter / 2);
    h.End();

    const std::vector<float> out = h.Read(0, h.clock - 2 * kIter, 2 * kIter);
    // The ramp continues across the iteration boundary without a step: every
    // increment is about half an input step (32 / 32768 / 2).
    const float expectedStep = 32.f / 32768.f / 2.f;
    for (int32_t i = 4; i < 2 * kIter; ++i) {
        const float d = out[static_cast<size_t>(i)] - out[static_cast<size_t>(i - 1)];
        SA_CHECK_NEAR(d, expectedStep, expectedStep * 0.5f);
    }
}

SA_TEST(ChannelCapture_PartialBlockKeepsChannelGroup)
{
    Harness h;
    int ch = 0;
    const std::vector<int16_t> full(static_cast<size_t>(kIter / 2), 4096);
    const std::vector<int16_t> tail(static_cast<size_t>(kIter / 8), 4096);

    h.Begin();
    h.Mix16(&ch, full, kIter / 2);
    h.End();
    // Sound ends mid-iteration: kIter/8 samples at the 22 kHz group rate,
    // which is kIter/4 DMA samples of audio and then silence.
    h.Begin();
    h.Mix16(&ch, tail, kIter / 8);
    h.End();

    const std::vector<float> out = h.Read(0, h.clock - kIter, kIter);
    for (int32_t i = 0; i < kIter / 4 - 2; ++i)
        SA_CHECK_NEAR(out[static_cast<size_t>(i)], 0.125f, 1e-5f);
    for (int32_t i = kIter / 4 + 1; i < kIter; ++i)
        SA_CHECK_NEAR(out[static_cast<size_t>(i)], 0.f, 1e-6f);
    SA_CHECK_EQ(h.capture.Stats().blocksPartial.load(), 1u);
}

SA_TEST(ChannelCapture_SameChannelTwicePerIterationIsNotSummed)
{
    Harness h;
    int ch = 0;
    const std::vector<int16_t> pcm(static_cast<size_t>(kIter), 8192);
    h.Begin();
    h.Mix16(&ch, pcm, kIter); // facing buffer
    h.Mix16(&ch, pcm, kIter); // facing-away buffer, same PCM
    h.End();

    const std::vector<float> out = h.Read(0, h.clock - kIter, kIter);
    for (int32_t i = 0; i < kIter; i += 29)
        SA_CHECK_NEAR(out[static_cast<size_t>(i)], 0.25f, 1e-6f);
    SA_CHECK_EQ(h.capture.Stats().duplicateBlocks.load(), 1u);
}

SA_TEST(ChannelCapture_TwoChannelsStayOnSeparateSlots)
{
    Harness h;
    int chA = 0, chB = 0;
    const std::vector<int16_t> a(static_cast<size_t>(kIter), 8192);
    const std::vector<int16_t> b(static_cast<size_t>(kIter / 2), -8192);
    h.Begin();
    h.Mix16(&chB, b, kIter / 2);
    h.Mix16(&chA, a, kIter);
    h.End();

    const int32_t slotA = h.capture.FindSlotByPointer(reinterpret_cast<uintptr_t>(&chA));
    const int32_t slotB = h.capture.FindSlotByPointer(reinterpret_cast<uintptr_t>(&chB));
    SA_CHECK(slotA >= 0 && slotB >= 0 && slotA != slotB);
    const std::vector<float> outA = h.Read(static_cast<uint32_t>(slotA), h.clock - kIter, kIter);
    const std::vector<float> outB = h.Read(static_cast<uint32_t>(slotB), h.clock - kIter, kIter);
    SA_CHECK_NEAR(outA[kIter / 2], 0.25f, 1e-6f);
    SA_CHECK_NEAR(outB[kIter / 2], -0.25f, 1e-5f);
    SA_CHECK_EQ(h.capture.Slot(static_cast<uint32_t>(slotA)).inputRate.load(), kDma);
    SA_CHECK_EQ(h.capture.Slot(static_cast<uint32_t>(slotB)).inputRate.load(), kDma / 2);
}
