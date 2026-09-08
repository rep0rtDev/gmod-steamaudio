// src/platform/PlatformAudioOutput_Null.cpp
//
// Null output: a timer-paced consumer thread that pulls frames and discards
// them, so the rendering pipeline advances at real-time rate without a
// device. Also hosts the portable pieces of thread priority handling.
#include "platform/PlatformAudioOutput.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <avrt.h>
#include <psapi.h>
#else
#include <pthread.h>
#include <sched.h>
#ifdef __linux__
#include <sys/sysinfo.h>
#endif
#endif

#include "util/Logging.h"

namespace sa {

namespace {

class NullAudioOutput final : public IPlatformAudioOutput {
public:
    ~NullAudioOutput() override { Stop(); }

    bool Start(const OutputRequest& request, IOutputSource& source, OutputFormat& actual,
               std::string& error) override
    {
        Stop();
        if (request.sampleRate <= 0 || request.frameSize <= 0) {
            error = "invalid null output request";
            return false;
        }
        m_format.sampleRate = request.sampleRate;
        m_format.channels = 2;
        m_format.periodFrames = request.frameSize;
        m_format.deviceName = "null";
        m_source = &source;
        m_running.store(true, std::memory_order_release);
        m_thread = std::thread([this] { Run(); });
        actual = m_format;
        return true;
    }

    void Stop() override
    {
        m_running.store(false, std::memory_order_release);
        if (m_thread.joinable())
            m_thread.join();
        m_source = nullptr;
    }

    bool IsRunning() const override { return m_running.load(std::memory_order_acquire); }
    bool NeedsRestart() const override { return false; }
    const OutputFormat& Format() const override { return m_format; }
    uint64_t FramesDelivered() const override { return m_delivered.load(std::memory_order_relaxed); }
    uint64_t Underruns() const override { return 0; }
    const char* BackendName() const override { return "null"; }

private:
    void Run()
    {
        SetCurrentThreadName("sa-null-output");
        std::vector<float> buffer(static_cast<size_t>(m_format.periodFrames) * 2);
        const auto period = std::chrono::duration<double>(static_cast<double>(m_format.periodFrames) /
                                                          static_cast<double>(m_format.sampleRate));
        auto next = std::chrono::steady_clock::now();
        while (m_running.load(std::memory_order_acquire)) {
            m_source->PullStereo(buffer.data(), static_cast<size_t>(m_format.periodFrames));
            m_delivered.fetch_add(static_cast<uint64_t>(m_format.periodFrames), std::memory_order_relaxed);
            next += std::chrono::duration_cast<std::chrono::steady_clock::duration>(period);
            std::this_thread::sleep_until(next);
            // If we fell far behind (debugger, suspend), resynchronise.
            const auto now = std::chrono::steady_clock::now();
            if (now - next > std::chrono::milliseconds(200))
                next = now;
        }
    }

    OutputFormat m_format;
    IOutputSource* m_source = nullptr;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<uint64_t> m_delivered{0};
};

} // namespace

std::unique_ptr<IPlatformAudioOutput> CreateNullAudioOutput()
{
    return std::make_unique<NullAudioOutput>();
}

#ifndef _WIN32
std::unique_ptr<IPlatformAudioOutput> CreateNativeAudioOutput()
{
    return CreateNullAudioOutput();
}
#endif

// ---------------------------------------------------------------------------
// Thread priority / naming
// ---------------------------------------------------------------------------
bool ElevateCurrentThreadToAudioPriority(const char* threadName)
{
#ifdef _WIN32
    bool ok = false;
    // MMCSS "Pro Audio" gives the thread a scheduling category above normal
    // time-critical threads without starving the system.
    DWORD taskIndex = 0;
    HANDLE task = AvSetMmThreadCharacteristicsA("Pro Audio", &taskIndex);
    if (task) {
        AvSetMmThreadPriority(task, AVRT_PRIORITY_HIGH);
        ok = true;
        // The handle is intentionally kept for the thread's lifetime; MMCSS
        // characteristics are revoked automatically when the thread exits.
    } else {
        SA_LOGW("[thread] AvSetMmThreadCharacteristics failed for %s: %lu", threadName, GetLastError());
    }
    if (SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL))
        ok = true;
    else
        SA_LOGW("[thread] SetThreadPriority failed for %s: %lu", threadName, GetLastError());
    return ok;
#else
    // SCHED_FIFO requires CAP_SYS_NICE / RLIMIT_RTPRIO; fall back to a
    // negative nice value when unavailable.
    sched_param param{};
    param.sched_priority = sched_get_priority_min(SCHED_FIFO) + 10;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) == 0)
        return true;
    SA_LOGW("[thread] SCHED_FIFO unavailable for %s (needs rtprio); staying SCHED_OTHER", threadName);
    return false;
#endif
}

ProcessMemoryInfo QueryProcessMemory()
{
    ProcessMemoryInfo result;
#ifdef _WIN32
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
        result.physicalTotal = memory.ullTotalPhys;
        result.physicalAvailable = memory.ullAvailPhys;
        result.commitAvailable = memory.ullAvailPageFile;
        result.virtualAvailable = memory.ullAvailVirtual;
        result.valid = true;
    }
    using GetMemoryFn = BOOL(WINAPI*)(HANDLE, PPROCESS_MEMORY_COUNTERS, DWORD);
    static const auto getMemory = reinterpret_cast<GetMemoryFn>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32GetProcessMemoryInfo")));
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (getMemory && getMemory(GetCurrentProcess(), reinterpret_cast<PPROCESS_MEMORY_COUNTERS>(&counters), sizeof(counters)))
        result.privateBytes = counters.PrivateUsage;
#elif defined(__linux__)
    struct sysinfo memory{};
    if (sysinfo(&memory) == 0) {
        result.physicalTotal = static_cast<uint64_t>(memory.totalram) * memory.mem_unit;
        result.physicalAvailable = static_cast<uint64_t>(memory.freeram) * memory.mem_unit;
        result.commitAvailable = result.physicalAvailable + static_cast<uint64_t>(memory.freeswap) * memory.mem_unit;
        result.valid = true;
    }
#endif
    return result;
}

void SetCurrentThreadName(const char* name)
{
#ifdef _WIN32
    using SetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    auto fn = kernel ? reinterpret_cast<SetThreadDescriptionFn>(
                           reinterpret_cast<void*>(GetProcAddress(kernel, "SetThreadDescription")))
                     : nullptr;
    if (fn) {
        wchar_t wide[64] = {};
        MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, 63);
        fn(GetCurrentThread(), wide);
    }
#else
    char truncated[16] = {};
    for (size_t i = 0; i < 15 && name[i] != '\0'; ++i)
        truncated[i] = name[i];
    pthread_setname_np(pthread_self(), truncated);
#endif
}

} // namespace sa
