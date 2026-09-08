#include "platform/BoundedProcess.h"

#include <algorithm>
#include <array>
#include <limits>
#include <windows.h>

namespace sa {
namespace {

struct Handle {
    HANDLE value = nullptr;
    ~Handle() { Close(); }
    void Close() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); value = nullptr; }
};

std::wstring Wide(const std::string& text)
{
    if (text.find('\0') != std::string::npos) return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring result(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), -1, &result[0], n);
    result.pop_back();
    return result;
}

std::wstring Quote(const std::wstring& text)
{
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t c : text) {
        if (c == L'\\') { ++slashes; continue; }
        result.append(c == L'\"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        result += c;
    }
    result.append(slashes * 2, L'\\');
    return result + L'\"';
}

std::string Error(const char* operation) { return std::string(operation) + " (Windows error " + std::to_string(GetLastError()) + ")"; }

}

struct BoundedProcess::Impl {
    Handle job, process, pipe;
    uint64_t peak = 0;
    uint32_t pid = 0;
};

BoundedProcess::BoundedProcess() : m_impl(new Impl) {}
BoundedProcess::~BoundedProcess() { Stop(); }

bool BoundedProcess::Start(const std::string& executable, const std::vector<std::string>& arguments,
                           const std::string& directory, const ProcessBudget& budget, std::string& error)
{
    Stop();
    m_impl.reset(new Impl);
    auto& p = *m_impl;
    error.clear();
    const auto exe = Wide(executable);
    const auto cwd = Wide(directory);
    if (exe.empty() || (!directory.empty() && cwd.empty()) || budget.memoryBytes == 0 ||
        budget.memoryBytes > std::numeric_limits<SIZE_T>::max() || budget.cpuPercent == 0 || budget.cpuPercent > 100 ||
        budget.maximumProcesses == 0) {
        error = "Invalid bounded-process settings";
        return false;
    }
    std::wstring command = Quote(exe);
    for (const auto& argument : arguments) {
        const auto wide = Wide(argument);
        if (!argument.empty() && wide.empty()) { error = "Invalid UTF-8 process argument"; return false; }
        command += L' ';
        command += Quote(wide);
    }
    p.job.value = CreateJobObjectW(nullptr, nullptr);
    if (!p.job.value) { error = Error("CreateJobObject"); return false; }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION memory{};
    memory.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION |
                                             JOB_OBJECT_LIMIT_JOB_MEMORY | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    memory.BasicLimitInformation.ActiveProcessLimit = budget.maximumProcesses;
    memory.JobMemoryLimit = static_cast<SIZE_T>(budget.memoryBytes);
    if (!SetInformationJobObject(p.job.value, JobObjectExtendedLimitInformation, &memory, sizeof(memory))) {
        error = Error("Set job memory/process limit"); return false;
    }
    JOBOBJECT_CPU_RATE_CONTROL_INFORMATION cpu{};
    cpu.ControlFlags = JOB_OBJECT_CPU_RATE_CONTROL_ENABLE | JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;
    cpu.CpuRate = budget.cpuPercent * 100;
    if (!SetInformationJobObject(p.job.value, JobObjectCpuRateControlInformation, &cpu, sizeof(cpu))) {
        error = Error("Set job CPU limit"); return false;
    }
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    Handle write;
    if (!CreatePipe(&p.pipe.value, &write.value, &security, 0) || !SetHandleInformation(p.pipe.value, HANDLE_FLAG_INHERIT, 0)) {
        error = Error("Create child output pipe"); return false;
    }
    Handle input;
    input.value = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr);
    if (input.value == INVALID_HANDLE_VALUE) { error = Error("Open child input"); return false; }
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<uint8_t> attributes(bytes);
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!InitializeProcThreadAttributeList(list, 1, 0, &bytes)) { error = Error("Initialize child attributes"); return false; }
    struct AttributeCleanup {
        LPPROC_THREAD_ATTRIBUTE_LIST value;
        ~AttributeCleanup() { DeleteProcThreadAttributeList(value); }
    } cleanup{list};
    HANDLE handles[] = {write.value, input.value};
    if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof(handles), nullptr, nullptr)) {
        error = Error("Set child handle allowlist"); return false;
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input.value;
    startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = write.value;
    startup.lpAttributeList = list;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), &command[0], nullptr, nullptr, TRUE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS | EXTENDED_STARTUPINFO_PRESENT,
                        nullptr, cwd.empty() ? nullptr : cwd.c_str(), &startup.StartupInfo, &process)) {
        error = Error("Create bounded child"); return false;
    }
    Handle thread;
    thread.value = process.hThread;
    p.process.value = process.hProcess;
    p.pid = process.dwProcessId;
    if (!AssignProcessToJobObject(p.job.value, p.process.value)) {
        error = Error("Assign child to resource-limited job");
        TerminateProcess(p.process.value, ERROR_CANCELLED);
        WaitForSingleObject(p.process.value, 5000);
        p.process.Close();
        return false;
    }
    if (ResumeThread(thread.value) == static_cast<DWORD>(-1)) { error = Error("Resume bounded child"); Stop(); return false; }
    return true;
}

bool BoundedProcess::Poll(std::string& output, uint32_t& exitCode, std::string& error)
{
    auto& p = *m_impl;
    output.clear();
    error.clear();
    exitCode = STILL_ACTIVE;
    if (!p.process.value) { exitCode = ERROR_INVALID_HANDLE; error = "No child process"; return false; }
    DWORD available = 0;
    if (p.pipe.value && PeekNamedPipe(p.pipe.value, nullptr, 0, nullptr, &available, nullptr) && available) {
        std::array<char, 65536> buffer{};
        DWORD read = 0;
        if (ReadFile(p.pipe.value, buffer.data(), std::min<DWORD>(available, static_cast<DWORD>(buffer.size())), &read, nullptr))
            output.assign(buffer.data(), read);
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION memory{};
    if (p.job.value && QueryInformationJobObject(p.job.value, JobObjectExtendedLimitInformation, &memory, sizeof(memory), nullptr))
        p.peak = std::max<uint64_t>(p.peak, memory.PeakJobMemoryUsed);
    DWORD code = STILL_ACTIVE;
    if (!GetExitCodeProcess(p.process.value, &code)) { error = Error("Query child status"); exitCode = ERROR_INVALID_HANDLE; return false; }
    if (WaitForSingleObject(p.process.value, 0) == WAIT_TIMEOUT) return true;
    p.job.Close();
    if (available > output.size()) return true;
    exitCode = code;
    return false;
}

void BoundedProcess::Stop()
{
    if (!m_impl) return;
    auto& p = *m_impl;
    if (p.job.value) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION memory{};
        if (QueryInformationJobObject(p.job.value, JobObjectExtendedLimitInformation, &memory, sizeof(memory), nullptr))
            p.peak = std::max<uint64_t>(p.peak, memory.PeakJobMemoryUsed);
        p.job.Close();
    }
    if (p.process.value) WaitForSingleObject(p.process.value, 5000);
    p.process.Close();
    p.pipe.Close();
}

uint64_t BoundedProcess::PeakMemoryBytes() const { return m_impl->peak; }
uint32_t BoundedProcess::ProcessId() const { return m_impl->pid; }

}
