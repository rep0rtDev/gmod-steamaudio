// src/util/Logging.cpp
#include "Logging.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <deque>
#include <mutex>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace sa {
namespace log {

namespace {

struct State {
    std::mutex mutex;                 // guards everything below
    FILE* file = nullptr;
    std::deque<std::string> pending;  // messages waiting for the game thread
    std::atomic<int> level{static_cast<int>(Level::Info)};
    std::array<std::atomic<uint64_t>, static_cast<size_t>(RTCounter::Count)> rt{};
    std::array<uint64_t, static_cast<size_t>(RTCounter::Count)> rtReported{};
};

State& GetState()
{
    static State state;
    return state;
}

constexpr size_t kMaxPending = 512;

const char* LevelName(Level level)
{
    switch (level) {
    case Level::Debug: return "DEBUG";
    case Level::Info: return "INFO ";
    case Level::Warn: return "WARN ";
    case Level::Error: return "ERROR";
    }
    return "?????";
}

const char* CounterName(RTCounter counter)
{
    switch (counter) {
    case RTCounter::Underrun: return "stream input underruns";
    case RTCounter::Overrun: return "capture ring overruns";
    case RTCounter::QueueFull: return "dropped queue messages";
    case RTCounter::EffectFailure: return "effect apply failures";
    case RTCounter::DeviceError: return "output device errors";
    case RTCounter::Count: break;
    }
    return "unknown";
}

std::string Timestamp()
{
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    const std::time_t t = system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d", tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<int>(ms.count()));
    return buf;
}

} // namespace

void Init(const std::string& filePath)
{
    State& s = GetState();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.file) {
        std::fclose(s.file);
        s.file = nullptr;
    }
    s.file = std::fopen(filePath.c_str(), "w");
    if (s.file) {
        std::fprintf(s.file, "[%s] INFO  Log opened\n", Timestamp().c_str());
        std::fflush(s.file);
    }
}

void Shutdown()
{
    State& s = GetState();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.file) {
        std::fprintf(s.file, "[%s] INFO  Log closed\n", Timestamp().c_str());
        std::fclose(s.file);
        s.file = nullptr;
    }
    s.pending.clear();
}

void SetLevel(Level level)
{
    GetState().level.store(static_cast<int>(level), std::memory_order_relaxed);
}

Level GetLevel()
{
    return static_cast<Level>(GetState().level.load(std::memory_order_relaxed));
}

void Write(Level level, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    WriteV(level, fmt, args);
    va_end(args);
}

void WriteV(Level level, const char* fmt, va_list args)
{
    State& s = GetState();
    if (static_cast<int>(level) < s.level.load(std::memory_order_relaxed))
        return;

    char body[2048];
    va_list copy;
    va_copy(copy, args);
    std::vsnprintf(body, sizeof(body), fmt, copy);
    va_end(copy);

    std::string line = "[steamaudio] ";
    line += LevelName(level);
    line += ' ';
    line += body;

    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.file) {
        std::fprintf(s.file, "[%s] %s\n", Timestamp().c_str(), line.c_str());
        std::fflush(s.file);
    }
#ifdef _WIN32
    OutputDebugStringA((line + "\n").c_str());
#else
    std::fprintf(stderr, "%s\n", line.c_str());
#endif
    if (s.pending.size() >= kMaxPending)
        s.pending.pop_front();
    s.pending.push_back(std::move(line));
}

std::vector<std::string> Drain(size_t maxCount)
{
    State& s = GetState();
    std::vector<std::string> out;
    std::lock_guard<std::mutex> lock(s.mutex);
    while (!s.pending.empty() && out.size() < maxCount) {
        out.push_back(std::move(s.pending.front()));
        s.pending.pop_front();
    }
    return out;
}

void IncrementRT(RTCounter counter)
{
    GetState().rt[static_cast<size_t>(counter)].fetch_add(1, std::memory_order_relaxed);
}

uint64_t GetRT(RTCounter counter)
{
    return GetState().rt[static_cast<size_t>(counter)].load(std::memory_order_relaxed);
}

void FlushRTCounters()
{
    State& s = GetState();
    for (size_t i = 0; i < static_cast<size_t>(RTCounter::Count); ++i) {
        const uint64_t now = s.rt[i].load(std::memory_order_relaxed);
        uint64_t& reported = s.rtReported[i];
        if (now != reported) {
            Write(Level::Warn, "%s: +%llu (total %llu)", CounterName(static_cast<RTCounter>(i)),
                  static_cast<unsigned long long>(now - reported), static_cast<unsigned long long>(now));
            reported = now;
        }
    }
}

} // namespace log
} // namespace sa
