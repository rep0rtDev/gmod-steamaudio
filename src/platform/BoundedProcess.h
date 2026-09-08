#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sa {

struct ProcessBudget {
    uint64_t memoryBytes = 1024ull * 1024 * 1024;
    uint32_t cpuPercent = 35;
    uint32_t maximumProcesses = 32;
};

class BoundedProcess {
public:
    BoundedProcess();
    ~BoundedProcess();
    BoundedProcess(const BoundedProcess&) = delete;
    BoundedProcess& operator=(const BoundedProcess&) = delete;
    bool Start(const std::string& executable, const std::vector<std::string>& arguments,
               const std::string& directory, const ProcessBudget& budget, std::string& error);
    bool Poll(std::string& output, uint32_t& exitCode, std::string& error);
    void Stop();
    uint64_t PeakMemoryBytes() const;
    uint32_t ProcessId() const;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}
