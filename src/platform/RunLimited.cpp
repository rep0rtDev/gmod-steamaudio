#include "platform/BoundedProcess.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <cstdio>
#include <cstring>
#include <exception>
#include <thread>
#include <windows.h>

namespace {
std::string Utf8(const wchar_t* text)
{
    const int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string result(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, &result[0], n, nullptr, nullptr);
    result.pop_back();
    return result;
}
}

int wmain(int argc, wchar_t** argv)
try {
    if (argc == 3 && std::wstring(argv[1]) == L"--probe-memory") {
        const uint64_t mib = std::min<uint64_t>(256, std::stoull(Utf8(argv[2])));
        try {
            std::vector<std::unique_ptr<char[]>> blocks;
            for (uint64_t i = 0; i < mib; i += 4) {
                std::unique_ptr<char[]> block(new char[4 * 1024 * 1024]);
                std::memset(block.get(), 1, 4 * 1024 * 1024);
                blocks.push_back(std::move(block));
            }
        } catch (const std::bad_alloc&) {
            std::puts("MEMORY_LIMIT_ENFORCED");
            return 0;
        }
        std::puts("MEMORY_LIMIT_NOT_REACHED");
        return 3;
    }
    sa::ProcessBudget budget;
    budget.memoryBytes = 3072ull * 1024 * 1024;
    budget.cpuPercent = 35;
    std::string directory, executable;
    std::vector<std::string> arguments;
    for (int i = 1; i < argc; ++i) {
        const std::string option = Utf8(argv[i]);
        if (option == "--" && i + 1 < argc) {
            executable = Utf8(argv[++i]);
            while (++i < argc) arguments.push_back(Utf8(argv[i]));
            break;
        }
        if (i + 1 >= argc) break;
        const std::string value = Utf8(argv[++i]);
        if (option == "--memory-mb") {
            const uint64_t mib = std::stoull(value);
            if (mib > std::numeric_limits<uint64_t>::max() / 1048576) return 2;
            budget.memoryBytes = mib * 1048576;
        }
        else if (option == "--cpu-percent") budget.cpuPercent = static_cast<uint32_t>(std::stoul(value));
        else if (option == "--workdir") directory = value;
        else { std::fprintf(stderr, "Unknown option: %s\n", option.c_str()); return 2; }
    }
    if (executable.empty()) {
        std::fprintf(stderr, "Usage: sa_run_limited --memory-mb N --cpu-percent N --workdir DIR -- EXE ARGS...\n");
        return 2;
    }
    sa::BoundedProcess process;
    std::string error;
    if (!process.Start(executable, arguments, directory, budget, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 2;
    }
    std::fprintf(stderr, "Resource-limited job: memory %llu MiB, CPU %u%%, PID %u\n",
                 static_cast<unsigned long long>(budget.memoryBytes / 1048576), budget.cpuPercent, process.ProcessId());
    uint32_t code = 0;
    for (;;) {
        std::string output;
        const bool running = process.Poll(output, code, error);
        if (!output.empty()) { std::fwrite(output.data(), 1, output.size(), stdout); std::fflush(stdout); }
        if (!running) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!error.empty()) std::fprintf(stderr, "%s\n", error.c_str());
    std::fprintf(stderr, "Job exited %u; peak committed memory %.1f MiB\n", code,
                 static_cast<double>(process.PeakMemoryBytes()) / 1048576.0);
    return static_cast<int>(code);
} catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 2;
}
