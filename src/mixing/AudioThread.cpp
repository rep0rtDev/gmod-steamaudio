// src/mixing/AudioThread.cpp
#include "mixing/AudioThread.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "mixing/Dsp.h"
#include "util/Logging.h"
#include "util/Math.h"

namespace sa {

namespace {

constexpr float kLimiterThreshold = 0.98f;
constexpr float kLimiterRelease = 0.9995f; // per sample

size_t NextPow2(size_t v)
{
    size_t p = 1;
    while (p < v)
        p <<= 1;
    return p;
}

} // namespace

AudioThread::AudioThread() = default;

AudioThread::~AudioThread()
{
    Stop();
}

bool AudioThread::Start(const AudioThreadSetup& setup)
{
    Stop();
    if (!setup.fallback || setup.frameSize <= 0 || setup.sampleRate <= 0) {
        SA_LOGE("[audio] invalid thread setup");
        return false;
    }
    m_setup = setup;
    const size_t frame = static_cast<size_t>(setup.frameSize);
    m_latencyFrames = static_cast<size_t>(static_cast<double>(setup.sampleRate) * setup.latencyMs / 1000.0);
    m_latencyFrames = std::max(m_latencyFrames, frame * 2);
    m_latencyFrames = (m_latencyFrames + frame - 1) / frame * frame;

    // Native ring: latency + 4 frames of headroom. Engine rings: 1 s.
    m_outRing = std::make_unique<SpscSampleRing>(NextPow2((m_latencyFrames + frame * 4) * 2));
    const size_t engineRing = NextPow2(static_cast<size_t>(setup.sampleRate) + m_latencyFrames);
    m_outLeft = std::make_unique<TimedSampleRing>(engineRing);
    m_outRight = std::make_unique<TimedSampleRing>(engineRing);

    m_inL.assign(frame, 0.f);
    m_inR.assign(frame, 0.f);
    m_interleaved.assign(frame * 2, 0.f);
    m_silence.assign(frame, 0.f);
    m_masterL.assign(frame, 0.f);
    m_masterR.assign(frame, 0.f);
    m_environment.Initialize(setup.sampleRate, setup.frameSize);
    m_streams.clear();
    m_streams.reserve(setup.maxStreamSources);
    for (SlotRender& s : m_slotRender)
        s = SlotRender{};
    m_renderClock = 0;
    m_clockAnchored = false;
    m_heldFrames = 0;
    m_engineLeadFrames = static_cast<size_t>(static_cast<double>(setup.sampleRate) * setup.engineLeadMs / 1000.0);
    if (m_engineLeadFrames > 0)
        m_engineLeadFrames = std::max(m_engineLeadFrames, frame);
    // Slack before we skip ahead: covers soundtime staleness (one game frame)
    // and DirectSound cursor granularity.
    m_maxLagFrames = std::max(frame * 4, static_cast<size_t>(setup.sampleRate) / 20);
    m_renderClockPublished.store(0, std::memory_order_release);
    m_limiterGain = 1.f;
    m_useRenderer = setup.renderer && setup.renderer->IsValid();
    m_stats.usingFallback.store(!m_useRenderer, std::memory_order_relaxed);
    m_cfg = m_runtime.Load();

    // Pre-fill the native ring with `latency` frames of silence so the device
    // has something to pull while the first frames render.
    if (setup.path == AudioOutputPath::Native) {
        std::vector<float> zeros(m_latencyFrames * 2, 0.f);
        m_outRing->Write(zeros.data(), zeros.size());
    }

    m_running.store(true, std::memory_order_release);
    m_thread = std::thread([this] { Run(); });
    SA_LOGI("[audio] thread started: %d Hz, frame %d, latency %zu frames, engine lead %s, path=%s, renderer=%s",
            setup.sampleRate, setup.frameSize, m_latencyFrames,
            m_engineLeadFrames ? "fixed" : "soundtime", setup.path == AudioOutputPath::Native ? "native" : "engine",
            m_useRenderer ? "steamaudio" : "fallback");
    return true;
}

void AudioThread::Stop()
{
    if (!m_running.exchange(false, std::memory_order_acq_rel)) {
        if (m_thread.joinable())
            m_thread.join();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_wakeMutex);
    }
    m_wake.notify_all();
    if (m_thread.joinable())
        m_thread.join();
    m_environment.Shutdown();
}

bool AudioThread::AddStreamSource(const SoundSourcePtr& source)
{
    if (!source)
        return false;
    Command cmd;
    cmd.type = Command::Type::Add;
    cmd.source = source;
    cmd.id = source->Id();
    if (!m_commands.TryPush(std::move(cmd)))
        return false;
    m_wake.notify_one();
    return true;
}

bool AudioThread::RemoveStreamSource(uint32_t id)
{
    Command cmd;
    cmd.type = Command::Type::Remove;
    cmd.id = id;
    if (!m_commands.TryPush(std::move(cmd)))
        return false;
    m_wake.notify_one();
    return true;
}

// ---------------------------------------------------------------------------
// Consumers
// ---------------------------------------------------------------------------
void AudioThread::PullStereo(float* interleaved, size_t frames)
{
    const size_t wanted = frames * 2;
    const size_t got = m_outRing ? m_outRing->Read(interleaved, wanted) : 0;
    if (got < wanted) {
        std::fill(interleaved + got, interleaved + wanted, 0.f);
        if (m_running.load(std::memory_order_relaxed) && m_clockAnchored)
            m_stats.outputUnderruns.fetch_add(1, std::memory_order_relaxed);
    }
    if (!m_runtime.Load().enabled)
        std::fill(interleaved, interleaved + wanted, 0.f);
    m_wake.notify_one();
}

void AudioThread::FillPaintBuffer(uint64_t clock, src::portable_samplepair_t* out, size_t frames)
{
    if (!out || frames == 0)
        return;
    if (!m_outLeft || !m_outRight || clock < m_latencyFrames) {
        for (size_t i = 0; i < frames; ++i)
            out[i] = src::portable_samplepair_t{0, 0};
        return;
    }
    const uint64_t readClock = clock - m_latencyFrames;
    // Stack chunks: this runs on the engine mixer thread, which must not touch
    // the render thread's scratch buffers.
    size_t done = 0;
    while (done < frames) {
        float chunkL[256];
        float chunkR[256];
        const size_t c = std::min<size_t>(frames - done, 256);
        const size_t gotL = m_outLeft->ReadAt(readClock + done, chunkL, c);
        const size_t gotR = m_outRight->ReadAt(readClock + done, chunkR, c);
        if (gotL < c)
            std::fill(chunkL + gotL, chunkL + c, 0.f);
        if (gotR < c)
            std::fill(chunkR + gotR, chunkR + c, 0.f);
        if ((gotL < c || gotR < c) && m_clockAnchored)
            m_stats.outputUnderruns.fetch_add(1, std::memory_order_relaxed);
        for (size_t i = 0; i < c; ++i) {
            out[done + i].left = static_cast<int32_t>(std::lrint(std::max(-1.f, std::min(1.f, chunkL[i])) * 32767.f));
            out[done + i].right = static_cast<int32_t>(std::lrint(std::max(-1.f, std::min(1.f, chunkR[i])) * 32767.f));
        }
        done += c;
    }
    m_wake.notify_one();
}

// ---------------------------------------------------------------------------
// Audio thread
// ---------------------------------------------------------------------------
void AudioThread::Run()
{
    SetCurrentThreadName("sa-audio");
    m_stats.priorityElevated.store(ElevateCurrentThreadToAudioPriority("sa-audio"), std::memory_order_relaxed);

    while (m_running.load(std::memory_order_acquire)) {
        DrainCommands();
        if (!WaitForWork())
            continue;
        RenderFrame();
    }

    // Teardown: release everything the audio thread owns.
    DrainCommands();
    for (SoundSourcePtr& s : m_streams) {
        EndSource(*s);
        s->MarkRenderFinished();
        m_released.TryPush(std::move(s));
    }
    m_streams.clear();
    if (m_setup.capture) {
        for (uint32_t i = 0; i < ChannelCapture::kSlots; ++i) {
            if (m_slotRender[i].active) {
                EndSource(*m_setup.capture->Slot(i).source);
                m_slotRender[i] = SlotRender{};
            }
        }
    }
}

void AudioThread::DrainCommands()
{
    Command cmd;
    while (m_commands.TryPop(cmd)) {
        if (cmd.type == Command::Type::Add) {
            if (m_streams.size() >= m_setup.maxStreamSources) {
                SA_LOG_RT(QueueFull);
                cmd.source->MarkRenderFinished();
                m_released.TryPush(std::move(cmd.source));
                continue;
            }
            BeginSource(*cmd.source);
            m_streams.push_back(std::move(cmd.source));
        } else {
            for (SoundSourcePtr& s : m_streams) {
                if (s->Id() == cmd.id) {
                    s->RequestStop();
                    break;
                }
            }
        }
    }
}

bool AudioThread::WaitForWork()
{
    const size_t frame = static_cast<size_t>(m_setup.frameSize);
    auto ready = [&]() -> bool {
        if (m_setup.path == AudioOutputPath::Native)
            return m_outRing->ReadableCount() + frame * 2 <= m_latencyFrames * 2;
        if (!m_setup.capture)
            return false;
        const uint64_t frontier = m_setup.capture->PaintedFrontier();
        if (!m_clockAnchored)
            return frontier >= frame;
        return frontier >= m_renderClock + frame;
    };
    if (ready())
        return true;
    std::unique_lock<std::mutex> lock(m_wakeMutex);
    m_wake.wait_for(lock, std::chrono::milliseconds(1), [&] { return !m_running.load(std::memory_order_acquire) || ready(); });
    return m_running.load(std::memory_order_acquire) && ready();
}

ListenerState AudioThread::CurrentListener() const
{
    if (m_setup.hooks) {
        const EngineListener l = m_setup.hooks->GetListener();
        if (l.valid) {
            ListenerState state;
            CoordinateConverter converter;
            converter.SetUnitsPerMeter(m_cfg.unitsPerMeter);
            state.frame = converter.FrameToSA(l.position, l.forward, l.up);
            state.valid = true;
            return state;
        }
    }
    return m_externalListener.Load();
}

void AudioThread::RenderFrame()
{
    const auto t0 = std::chrono::steady_clock::now();
    const size_t frame = static_cast<size_t>(m_setup.frameSize);
    m_cfg = m_runtime.Load();

    // ---- Clock -----------------------------------------------------------
    // Native path: the device pulls one frame per call regardless, but the
    // engine rings are only consumed when the engine has painted this frame
    // (`advanceClock`). Holding keeps a hitch a gap instead of turning the
    // unwritten future into silence *and* skipping the audio once it arrives.
    bool haveEngineData = false;
    bool advanceClock = true;
    if (m_setup.capture) {
        const uint64_t frontier = m_setup.capture->PaintedFrontier();
        if (m_setup.path == AudioOutputPath::Engine) {
            if (!m_clockAnchored) {
                m_renderClock = frontier - frame;
                m_outLeft->Reset(m_renderClock);
                m_outRight->Reset(m_renderClock);
                m_clockAnchored = true;
            }
            haveEngineData = true;
        } else if (frontier >= frame) {
            haveEngineData = ScheduleNativeClock(frontier);
            advanceClock = haveEngineData;
        }
    } else if (!m_clockAnchored) {
        m_clockAnchored = true;
    }

    // ---- Environment (dsp_room / dsp_player / underwater replacement) -----
    EnvironmentConfig envCfg;
    envCfg.roomMode = m_cfg.roomDspMode;
    envCfg.roomOverride = m_cfg.roomDspPreset;
    envCfg.roomGain = m_cfg.roomDspGain;
    envCfg.playerDsp = m_cfg.playerDsp;
    envCfg.underwater = m_cfg.underwaterDsp;
    envCfg.underwaterCutoffHz = m_cfg.underwaterCutoffHz;
    envCfg.underwaterGain = m_cfg.underwaterGain;
    m_environment.SetConfig(envCfg);
    m_environment.SetState(m_environmentState.Load());
    const RoomPreset& room = m_environment.BeginFrame();
    RoomSend roomSend;
    roomSend.bus = m_environment.RoomEnabled() ? m_environment.SendBus() : nullptr;
    roomSend.preset = &room;
    roomSend.always = m_environment.SendAlways();

    // ---- Render ----------------------------------------------------------
    const ListenerState listener = CurrentListener();
    const bool useRenderer = m_useRenderer && m_cfg.enabled;
    if (useRenderer)
        m_setup.renderer->BeginFrame(m_cfg, listener, roomSend);
    else
        m_setup.fallback->BeginFrame(m_cfg, listener, roomSend);
    m_useRenderer = useRenderer || (m_setup.renderer && m_setup.renderer->IsValid());
    m_stats.usingFallback.store(!useRenderer, std::memory_order_relaxed);

    uint32_t active = 0;
    uint32_t spatial = 0;
    uint32_t roomSends = 0;
    m_stats.activeSources.store(0, std::memory_order_relaxed);
    if (m_setup.capture)
        RenderEngineSlots(m_renderClock, haveEngineData);
    RenderStreamSources();

    // Count after rendering (cheap; stats only).
    for (const SlotRender& s : m_slotRender)
        active += s.active ? 1 : 0;
    active += static_cast<uint32_t>(m_streams.size());
    if (m_setup.capture) {
        for (uint32_t i = 0; i < ChannelCapture::kSlots; ++i) {
            if (m_slotRender[i].active) {
                SoundSource& s = *m_setup.capture->Slot(i).source;
                const SourceParams p = s.GetParams();
                spatial += (p.spatialize && p.positionValid) ? 1 : 0;
                roomSends += s.Render().roomSent ? 1 : 0;
            }
        }
    }
    for (const SoundSourcePtr& s : m_streams) {
        const SourceParams p = s->GetParams();
        spatial += (p.spatialize && p.positionValid) ? 1 : 0;
        roomSends += s->Render().roomSent ? 1 : 0;
    }
    m_stats.activeSources.store(active, std::memory_order_relaxed);
    m_stats.spatializedSources.store(spatial, std::memory_order_relaxed);
    m_stats.roomSends.store(roomSends, std::memory_order_relaxed);

    const float* left = nullptr;
    const float* right = nullptr;
    if (useRenderer) {
        m_setup.renderer->EndFrame();
        left = m_setup.renderer->MasterLeft();
        right = m_setup.renderer->MasterRight();
        m_stats.peak.store(m_setup.renderer->LastPeak(), std::memory_order_relaxed);
    } else {
        m_setup.fallback->EndFrame();
        left = m_setup.fallback->MasterLeft();
        right = m_setup.fallback->MasterRight();
        m_stats.peak.store(m_setup.fallback->LastPeak(), std::memory_order_relaxed);
    }
    if (!left || !right) {
        left = m_silence.data();
        right = m_silence.data();
    }
    std::copy(left, left + frame, m_masterL.begin());
    std::copy(right, right + frame, m_masterR.begin());
    m_environment.EndFrame(m_masterL.data(), m_masterR.data());
    m_stats.roomPreset.store(m_environment.EffectiveRoomPreset(), std::memory_order_relaxed);
    m_stats.playerLowpassHz.store(m_environment.PlayerLowpassHz(), std::memory_order_relaxed);
    if (!dsp::AllFinite(m_masterL.data(), frame) || !dsp::AllFinite(m_masterR.data(), frame)) {
        // A NaN anywhere would latch into the limiter and every IIR stage
        // downstream; drop this frame instead.
        std::fill(m_masterL.begin(), m_masterL.end(), 0.f);
        std::fill(m_masterR.begin(), m_masterR.end(), 0.f);
        m_limiterGain = 1.f;
        if (m_stats.nonFiniteFrames.fetch_add(1, std::memory_order_relaxed) == 0)
            RecordClockEvent(ClockEvent::Kind::NonFinite, 0, 0);
    }
    PublishOutput(m_masterL.data(), m_masterR.data());

    if (advanceClock)
        m_renderClock += frame;
    m_renderClockPublished.store(m_renderClock, std::memory_order_release);
    m_stats.framesRendered.fetch_add(1, std::memory_order_release);

    const auto micros = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
    m_stats.lastRenderMicros.store(micros, std::memory_order_relaxed);
    uint32_t prevMax = m_stats.maxRenderMicros.load(std::memory_order_relaxed);
    while (micros > prevMax &&
           !m_stats.maxRenderMicros.compare_exchange_weak(prevMax, micros, std::memory_order_relaxed)) {
    }
}

bool AudioThread::ScheduleNativeClock(uint64_t frontier)
{
    const size_t frame = static_cast<size_t>(m_setup.frameSize);
    const uint64_t latest = frontier - frame; // last fully painted frame start

    // Where we want to be: a fixed distance behind the frontier, or (default)
    // right behind the engine's own play cursor, which is what its mixahead
    // is sized for. Fall back to the configured output latency until the
    // capture has seen a soundtime.
    uint64_t target = 0;
    if (m_engineLeadFrames > 0) {
        target = frontier > m_engineLeadFrames ? frontier - m_engineLeadFrames : 0;
    } else if (const uint64_t soundClock = m_setup.capture->SoundClock(); soundClock != 0) {
        target = soundClock + m_outRing->ReadableCount() / 2 +
                 m_deviceLatencyFrames.load(std::memory_order_acquire);
    } else {
        target = frontier > m_latencyFrames ? frontier - m_latencyFrames : 0;
    }
    target = std::min(target, latest);

    if (!m_clockAnchored) {
        m_renderClock = target;
        m_clockAnchored = true;
        RecordClockEvent(ClockEvent::Kind::Anchor, static_cast<int64_t>(frontier - m_renderClock), 0);
    }

    const int64_t backlog = static_cast<int64_t>(frontier) - static_cast<int64_t>(m_renderClock);
    const int64_t lag = static_cast<int64_t>(target) - static_cast<int64_t>(m_renderClock);
    if (backlog < static_cast<int64_t>(frame)) {
        // Not painted yet (game hitch, or the device clock runs ahead of the
        // engine's): hold this frame.
        m_stats.engineStarves.fetch_add(1, std::memory_order_relaxed);
        if (m_heldFrames++ == 0)
            RecordClockEvent(ClockEvent::Kind::HoldBegin, backlog, lag);
        return false;
    }
    if (m_heldFrames) {
        RecordClockEvent(ClockEvent::Kind::HoldEnd, backlog, lag);
        m_heldFrames = 0;
    }
    if (lag > static_cast<int64_t>(m_maxLagFrames)) {
        // We fell behind the engine's play cursor by more than the slack
        // (device stalled, or its clock runs slower): skip ahead in one step
        // rather than staying late forever.
        RecordClockEvent(ClockEvent::Kind::Resync, backlog, lag);
        m_renderClock = target;
        m_stats.clockResyncs.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

void AudioThread::RecordClockEvent(ClockEvent::Kind kind, int64_t backlog, int64_t lag)
{
    ClockEvent ev;
    ev.kind = kind;
    ev.frame = m_stats.framesRendered.load(std::memory_order_relaxed);
    ev.backlog = backlog;
    ev.lag = lag;
    ev.heldFrames = m_heldFrames;
    ev.activeSources = m_stats.activeSources.load(std::memory_order_relaxed);
    m_clockEvents.TryPush(ev);
}

void AudioThread::BeginSource(SoundSource& source)
{
    SoundSource::RenderState& rs = source.Render();
    rs.currentGain = 0.f;
    rs.gainInitialized = false;
    rs.fadeGain = source.Kind() == SourceKind::EngineChannel ? 1.f : 0.f;
    rs.fadeStart = rs.fadeGain;
    rs.fadingOut = false;
    rs.silentFrames = 0;
    rs.everHadData = false;
    rs.framesRendered = 0;
    rs.framesNoData = 0;
    rs.framesReflections = 0;
    rs.framesPathing = 0;
    rs.peakOutputLevel = 0.f;
    rs.minOcclusionRaw = 1.f;
    rs.outputsValid = false;
    rs.fallback = {};
    rs.roomSent = false;
    rs.doppler.Reset();
    if (rs.effects)
        rs.effects->Reset();
}

void AudioThread::EndSource(SoundSource& source)
{
    SoundSource::RenderState& rs = source.Render();
    if (rs.effects && m_setup.renderer) {
        m_setup.renderer->ReleaseEffects(rs.effects);
        rs.effects = nullptr;
    }
    rs.fadeGain = 0.f;
    rs.currentGain = 0.f;
    rs.fadingOut = false;
    rs.outputsValid = false;
    RenderDiag d = source.LoadRenderDiag();
    if (rs.framesRendered > 0) {
        const SourceParams p = source.GetParams();
        SourceEndEvent ev;
        ev.frame = m_stats.framesRendered.load(std::memory_order_relaxed);
        ev.entity = p.entityIndex;
        ev.position = p.position;
        ev.spatialize = p.spatialize;
        ev.diag = d;
        ev.diag.framesRendered = rs.framesRendered;
        ev.diag.framesNoData = rs.framesNoData;
        ev.diag.framesReflections = rs.framesReflections;
        ev.diag.framesPathing = rs.framesPathing;
        ev.diag.peakOutputLevel = rs.peakOutputLevel;
        ev.diag.minOcclusionRaw = rs.minOcclusionRaw;
        m_sourceEnds.TryPush(ev);
    }
    d = RenderDiag{};
    source.PublishRenderDiag(d);
}

float AudioThread::SourceFrameGain(SoundSource& source, bool hasData, bool ending, bool& finished)
{
    SoundSource::RenderState& rs = source.Render();
    finished = false;
    const float step = m_setup.fadeSeconds > 0.f
                           ? static_cast<float>(m_setup.frameSize) /
                                 (m_setup.fadeSeconds * static_cast<float>(m_setup.sampleRate)) : 1.f;
    rs.fadeStart = rs.fadeGain;
    if (ending) {
        rs.fadingOut = true;
        rs.fadeGain = std::max(0.f, rs.fadeGain - step);
        finished = rs.fadeStart <= 0.f;
    } else {
        rs.fadingOut = false;
        rs.fadeGain = std::min(1.f, rs.fadeGain + step);
    }
    if (m_setup.fadeSeconds <= 0.f)
        rs.fadeStart = rs.fadeGain;
    if (hasData) {
        rs.silentFrames = 0;
        rs.everHadData = true;
    } else {
        ++rs.silentFrames;
        ++rs.framesNoData;
    }
    ++rs.framesRendered;
    return rs.fadeGain;
}

void AudioThread::RenderEngineSlots(uint64_t clock, bool haveEngineData)
{
    ChannelCapture& capture = *m_setup.capture;
    const size_t frame = static_cast<size_t>(m_setup.frameSize);
    float* inputs[2] = {m_inL.data(), m_inR.data()};

    for (uint32_t i = 0; i < ChannelCapture::kSlots; ++i) {
        CaptureSlot& slot = capture.Slot(i);
        SlotRender& sr = m_slotRender[i];
        if (!slot.source)
            continue;
        SoundSource& source = *slot.source;
        const uintptr_t bound = slot.enginePtr.load(std::memory_order_acquire);
        const uint32_t generation = slot.generation.load(std::memory_order_acquire);

        if (bound != 0 && slot.everMixed.load(std::memory_order_acquire) &&
            (!sr.active || sr.generation != generation)) {
            if (sr.active)
                EndSource(source);
            BeginSource(source);
            sr.active = true;
            sr.generation = generation;
        }
        if (!sr.active)
            continue;

        // Always advance both ring cursors so the producer window stays sane.
        size_t gotL = 0, gotR = 0;
        if (haveEngineData) {
            if (TimedSampleRing* ring = source.TimedRing(0))
                gotL = ring->ReadAt(clock, m_inL.data(), frame);
            if (TimedSampleRing* ring = source.TimedRing(1))
                gotR = ring->ReadAt(clock, m_inR.data(), frame);
        }
        if (gotL < frame)
            std::fill(m_inL.begin() + static_cast<std::ptrdiff_t>(gotL), m_inL.end(), 0.f);
        if (gotR < frame)
            std::fill(m_inR.begin() + static_cast<std::ptrdiff_t>(gotR), m_inR.end(), 0.f);

        const uint32_t channels = std::min<uint32_t>(slot.inputChannels.load(std::memory_order_relaxed), 2u);
        const uint64_t lastClock = slot.lastClock.load(std::memory_order_relaxed);
        const bool hasData = haveEngineData && slot.everMixed.load(std::memory_order_acquire) &&
                             lastClock + frame * 2 >= clock;
        const bool ending = bound == 0;

        bool finished = false;
        const float fade = SourceFrameGain(source, hasData, ending, finished);
        if (finished) {
            EndSource(source);
            sr = SlotRender{};
            continue;
        }
        auto& render = source.Render();
        const size_t delay = m_cfg.doppler && render.doppler.Primed()
                                 ? static_cast<size_t>(std::ceil(render.doppler.CurrentDelay())) : 0;
        const bool inputEnded = lastClock < capture.PaintedFrontier();
        if (!hasData && inputEnded && render.silentFrames * frame >
                delay + std::max(frame * 2, static_cast<size_t>(m_setup.sampleRate / 20))) {
            if (render.effects && m_setup.renderer) {
                m_setup.renderer->ReleaseEffects(render.effects);
                render.effects = nullptr;
            }
            continue;
        }
        if (m_useRenderer && m_cfg.enabled)
            m_setup.renderer->RenderSource(source, inputs, channels == 0 ? 1 : channels, fade);
        else
            m_setup.fallback->RenderSource(source, inputs, channels == 0 ? 1 : channels, fade);
    }
}

void AudioThread::RenderStreamSources()
{
    const size_t frame = static_cast<size_t>(m_setup.frameSize);
    float* inputs[2] = {m_inL.data(), m_inR.data()};

    for (size_t i = 0; i < m_streams.size();) {
        SoundSource& source = *m_streams[i];
        const uint32_t channels = std::max<uint32_t>(1, std::min<uint32_t>(source.InputChannels(), 2));
        const size_t got = source.ReadStreamFrames(inputs, frame);
        const bool queued = source.QueuedStreamFrames() > 0;
        const bool hasData = got > 0 || queued;
        const auto& render = source.Render();
        const size_t delaySamples = m_cfg.doppler && render.doppler.Primed()
                                        ? static_cast<size_t>(std::ceil(render.doppler.CurrentDelay())) : 0;
        const size_t tailSamples = delaySamples + std::max(frame * 2, static_cast<size_t>(m_setup.sampleRate / 20));
        const bool drained = render.silentFrames * frame >= tailSamples;
        const bool ending = source.StopRequested() || (source.EndOfStream() && !hasData && drained);

        bool finished = false;
        const float fade = SourceFrameGain(source, hasData, ending, finished);
        if (finished) {
            EndSource(source);
            source.MarkRenderFinished();
            m_released.TryPush(std::move(m_streams[i]));
            m_streams[i] = std::move(m_streams.back());
            m_streams.pop_back();
            continue;
        }
        if (!hasData && render.silentFrames * frame > tailSamples) {
            ++i;
            continue;
        }
        if (m_useRenderer && m_cfg.enabled)
            m_setup.renderer->RenderSource(source, inputs, channels, fade);
        else
            m_setup.fallback->RenderSource(source, inputs, channels, fade);
        ++i;
    }
}

void AudioThread::ApplyLimiter(float* interleaved, size_t frames)
{
    // Peak limiter with instantaneous attack and exponential release; keeps
    // the sum of many sources from clipping without a look-ahead buffer.
    float g = m_limiterGain;
    for (size_t i = 0; i < frames; ++i) {
        float& l = interleaved[i * 2];
        float& r = interleaved[i * 2 + 1];
        const float peak = std::max(std::fabs(l), std::fabs(r));
        if (peak * g > kLimiterThreshold)
            g = kLimiterThreshold / peak;
        else
            g = 1.f - (1.f - g) * kLimiterRelease;
        l *= g;
        r *= g;
    }
    m_limiterGain = g;
}

void AudioThread::PublishOutput(const float* left, const float* right)
{
    const size_t frame = static_cast<size_t>(m_setup.frameSize);
    const float master = std::max(0.f, m_cfg.masterVolume) *
                         (m_setup.path == AudioOutputPath::Native ? std::max(0.f, m_cfg.engineVolume) : 1.f);
    for (size_t i = 0; i < frame; ++i) {
        m_interleaved[i * 2] = left[i] * master;
        m_interleaved[i * 2 + 1] = right[i] * master;
    }
    ApplyLimiter(m_interleaved.data(), frame);

    if (m_setup.path == AudioOutputPath::Native) {
        const size_t written = m_outRing->Write(m_interleaved.data(), frame * 2);
        if (written < frame * 2)
            SA_LOG_RT(Overrun);
    } else {
        // De-interleave into the engine timed rings at the render clock.
        for (size_t i = 0; i < frame; ++i) {
            m_inL[i] = m_interleaved[i * 2];
            m_inR[i] = m_interleaved[i * 2 + 1];
        }
        // Sequential, non-overlapping blocks: Accumulate on a cleared region
        // is a plain write.
        if (!m_outLeft->Accumulate(m_renderClock, m_inL.data(), frame) ||
            !m_outRight->Accumulate(m_renderClock, m_inR.data(), frame))
            SA_LOG_RT(Overrun);
    }
}

} // namespace sa
