// src/util/Logging.h
//
// Thread-safe logging facility. Messages are written to a log file, to the
// debugger (OutputDebugStringA) and buffered in a bounded queue so the game
// thread can drain them into the GMod console via Lua (steamaudio.PollLog).
//
// Thread-safety: every public function may be called from any thread. The
// audio thread should prefer SA_LOG_RT which only records a rate-limited
// counter instead of formatting a string (no heap allocation on the RT path).
#pragma once

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <string>
#include <vector>

namespace sa {
namespace log {

enum class Level : int {
    Debug = 0,
    Info = 1,
    Warn = 2,
    Error = 3,
};

// Opens the log file (truncating) and enables logging. Safe to call twice.
void Init(const std::string& filePath);

// Flushes and closes the log file.
void Shutdown();

void SetLevel(Level level);
Level GetLevel();

// printf-style logging.
void Write(Level level, const char* fmt, ...);
void WriteV(Level level, const char* fmt, va_list args);

// Drains up to `maxCount` pending console messages (oldest first).
std::vector<std::string> Drain(size_t maxCount);

// Real-time safe counters. `Increment` is wait-free; the counters are
// reported by `FlushRTCounters`, which is called from the game thread.
enum class RTCounter : int {
    Underrun = 0,
    Overrun,
    QueueFull,
    EffectFailure,
    DeviceError,
    Count,
};
void IncrementRT(RTCounter counter);
uint64_t GetRT(RTCounter counter);
void FlushRTCounters();

} // namespace log
} // namespace sa

#define SA_LOGD(...) ::sa::log::Write(::sa::log::Level::Debug, __VA_ARGS__)
#define SA_LOGI(...) ::sa::log::Write(::sa::log::Level::Info, __VA_ARGS__)
#define SA_LOGW(...) ::sa::log::Write(::sa::log::Level::Warn, __VA_ARGS__)
#define SA_LOGE(...) ::sa::log::Write(::sa::log::Level::Error, __VA_ARGS__)
#define SA_LOG_RT(counter) ::sa::log::IncrementRT(::sa::log::RTCounter::counter)
