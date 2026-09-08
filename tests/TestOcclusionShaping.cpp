// tests/TestOcclusionShaping.cpp
//
// Occlusion knee / leak floor, and (with SA_PHONON_DIR set) the effect they
// have on a real Steam Audio direct effect: a fully occluded source must stay
// audible at the configured floor instead of vanishing behind the material's
// transmission coefficients.
#include "TestFramework.h"
#include "steamaudio/Config.h"
#include "steamaudio/OcclusionShaping.h"
#include "steamaudio/PhononContext.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace sa;

namespace {

IPLDirectEffectFlags OcclTrans()
{
    return static_cast<IPLDirectEffectFlags>(IPL_DIRECTEFFECTFLAGS_APPLYOCCLUSION |
                                             IPL_DIRECTEFFECTFLAGS_APPLYTRANSMISSION);
}

} // namespace

SA_TEST(OcclusionShaping_KneeMapsVisibility)
{
    RuntimeConfig cfg;
    cfg.occlusionFullVisibility = 0.6f;
    cfg.occlusionZeroVisibility = 0.1f;
    SA_CHECK_NEAR(ShapeOcclusionVisibility(1.0f, cfg), 1.f, 1e-6);
    SA_CHECK_NEAR(ShapeOcclusionVisibility(0.7f, cfg), 1.f, 1e-6);
    SA_CHECK_NEAR(ShapeOcclusionVisibility(0.6f, cfg), 1.f, 1e-6);
    SA_CHECK_NEAR(ShapeOcclusionVisibility(0.35f, cfg), 0.5f, 1e-5);
    SA_CHECK_NEAR(ShapeOcclusionVisibility(0.1f, cfg), 0.f, 1e-6);
    SA_CHECK_NEAR(ShapeOcclusionVisibility(0.0f, cfg), 0.f, 1e-6);
    // Degenerate configuration (full <= zero) still yields a monotonic ramp.
    cfg.occlusionFullVisibility = 0.05f;
    cfg.occlusionZeroVisibility = 0.5f;
    SA_CHECK(ShapeOcclusionVisibility(0.6f, cfg) >= ShapeOcclusionVisibility(0.5f, cfg));
    SA_CHECK_NEAR(ShapeOcclusionVisibility(0.9f, cfg), 1.f, 1e-6);
}

SA_TEST(OcclusionShaping_FloorRaisesTransmissionOrOcclusion)
{
    RuntimeConfig cfg;
    cfg.occlusionFullVisibility = 0.6f;
    cfg.occlusionZeroVisibility = 0.1f;
    cfg.occlusionMinGain = 0.3f;

    // Concrete-like transmission from the simulator, source fully blocked.
    IPLDirectEffectParams p{};
    p.occlusion = 0.05f;
    p.transmission[0] = 0.01f;
    p.transmission[1] = 0.005f;
    p.transmission[2] = 0.001f;
    ShapeOcclusion(p, OcclTrans(), cfg);
    SA_CHECK_NEAR(p.occlusion, 0.f, 1e-6);
    SA_CHECK_NEAR(p.transmission[0], 0.3f, 1e-6);
    SA_CHECK_NEAR(p.transmission[1], 0.18f, 1e-6);
    SA_CHECK_NEAR(p.transmission[2], 0.105f, 1e-6);

    // A material that already leaks more than the floor is left alone.
    IPLDirectEffectParams leaky{};
    leaky.occlusion = 0.05f;
    leaky.transmission[0] = 0.8f;
    leaky.transmission[1] = 0.7f;
    leaky.transmission[2] = 0.6f;
    ShapeOcclusion(leaky, OcclTrans(), cfg);
    SA_CHECK_NEAR(leaky.transmission[0], 0.8f, 1e-6);
    SA_CHECK_NEAR(leaky.transmission[2], 0.6f, 1e-6);

    // Without transmission the floor goes into the occlusion gain itself.
    IPLDirectEffectParams occlOnly{};
    occlOnly.occlusion = 0.0f;
    ShapeOcclusion(occlOnly, IPL_DIRECTEFFECTFLAGS_APPLYOCCLUSION, cfg);
    SA_CHECK_NEAR(occlOnly.occlusion, 0.3f, 1e-6);
    occlOnly.occlusion = 0.35f;
    ShapeOcclusion(occlOnly, IPL_DIRECTEFFECTFLAGS_APPLYOCCLUSION, cfg);
    SA_CHECK_NEAR(occlOnly.occlusion, 0.3f + 0.7f * 0.5f, 1e-5);

    // Occlusion not applied: untouched.
    IPLDirectEffectParams none{};
    none.occlusion = 0.2f;
    none.transmission[0] = 0.01f;
    ShapeOcclusion(none, IPL_DIRECTEFFECTFLAGS_APPLYDISTANCEATTENUATION, cfg);
    SA_CHECK_NEAR(none.occlusion, 0.2f, 1e-6);
    SA_CHECK_NEAR(none.transmission[0], 0.01f, 1e-6);

    // Floor 0 = purely physical transmission.
    cfg.occlusionMinGain = 0.f;
    IPLDirectEffectParams physical{};
    physical.occlusion = 0.0f;
    physical.transmission[0] = 0.01f;
    ShapeOcclusion(physical, OcclTrans(), cfg);
    SA_CHECK_NEAR(physical.occlusion, 0.f, 1e-6);
    SA_CHECK_NEAR(physical.transmission[0], 0.01f, 1e-6);
}

// Real runtime: a direct effect fed the shaped parameters for a source fully
// behind concrete must still output the leak floor (about -10 dB for 0.3),
// while the unshaped parameters take it to -40 dB and below.
SA_TEST(OcclusionShaping_DirectEffectKeepsLeakFloor)
{
    const char* dir = std::getenv("SA_PHONON_DIR");
    if (!dir || !*dir) {
        throw satest::Skipped{"SA_PHONON_DIR not set"};
    }
    StaticConfig cfg;
    PhononContext ctx;
    std::string err;
    SA_CHECK(ctx.Initialize(cfg, {dir}, err));
    IPLDirectEffectSettings settings{};
    settings.numChannels = 1;
    IPLDirectEffect effect = nullptr;
    SA_CHECK(iplDirectEffectCreate(ctx.Handle(), ctx.MutableAudioSettings(), &settings, &effect) ==
             IPL_STATUS_SUCCESS);
    if (!effect)
        return;

    const int32_t frame = ctx.FrameSize();
    std::vector<float> in(static_cast<size_t>(frame));
    std::vector<float> out(static_cast<size_t>(frame));
    float* inData[1] = {in.data()};
    float* outData[1] = {out.data()};
    IPLAudioBuffer inBuf{1, frame, inData};
    IPLAudioBuffer outBuf{1, frame, outData};

    const auto render = [&](const IPLDirectEffectParams& params) {
        uint32_t seed = 4242u;
        double inE = 0.0;
        double outE = 0.0;
        iplDirectEffectReset(effect);
        for (int f = 0; f < 24; ++f) {
            for (float& s : in) {
                seed = seed * 1664525u + 1013904223u;
                s = (static_cast<float>(seed >> 8) / 8388608.f) - 1.f;
            }
            IPLDirectEffectParams p = params;
            iplDirectEffectApply(effect, &p, &inBuf, &outBuf);
            if (f < 16)
                continue;
            for (int32_t i = 0; i < frame; ++i) {
                inE += static_cast<double>(in[static_cast<size_t>(i)]) * in[static_cast<size_t>(i)];
                outE += static_cast<double>(out[static_cast<size_t>(i)]) * out[static_cast<size_t>(i)];
            }
        }
        return 10.0 * std::log10((outE + 1e-12) / (inE + 1e-12));
    };

    IPLDirectEffectParams raw{};
    raw.flags = OcclTrans();
    raw.transmissionType = IPL_TRANSMISSIONTYPE_FREQDEPENDENT;
    raw.distanceAttenuation = 1.f;
    raw.airAbsorption[0] = raw.airAbsorption[1] = raw.airAbsorption[2] = 1.f;
    raw.directivity = 1.f;
    raw.occlusion = 0.02f;
    raw.transmission[0] = 0.01f;
    raw.transmission[1] = 0.005f;
    raw.transmission[2] = 0.001f;

    RuntimeConfig rt;
    rt.occlusionMinGain = 0.3f;
    IPLDirectEffectParams shaped = raw;
    ShapeOcclusion(shaped, raw.flags, rt);

    IPLDirectEffectParams open = raw;
    open.occlusion = 1.f;

    const double openDb = render(open);
    const double rawDb = render(raw);
    const double shapedDb = render(shaped);
    std::printf("    direct effect: open %.1f dB  occluded raw %.1f dB  shaped (floor 0.3) %.1f dB\n", openDb, rawDb,
                shapedDb);
    SA_CHECK(openDb > -3.0);
    SA_CHECK(rawDb < -30.0);
    // Low band at 0.3 (-10.5 dB), mid/high lower: broadband noise lands
    // between the low-band level and the high-band level.
    SA_CHECK(shapedDb > -22.0);
    SA_CHECK(shapedDb < -6.0);
    SA_CHECK(shapedDb > rawDb + 10.0);

    iplDirectEffectRelease(&effect);
}
