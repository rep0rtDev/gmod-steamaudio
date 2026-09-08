// tests/TestFramework.h
//
// Tiny self-contained test harness (no external dependencies so the tests
// build with every toolchain the module targets).
#pragma once

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace satest {

struct TestCase {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& Registry()
{
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) { Registry().push_back({name, std::move(fn)}); }
};

struct Failure {
    std::string message;
};

struct Skipped {
    std::string message;
};

inline int& FailureCount()
{
    static int count = 0;
    return count;
}

inline void Fail(const char* file, int line, const std::string& what)
{
    std::fprintf(stderr, "    %s:%d: %s\n", file, line, what.c_str());
    throw Failure{what};
}

inline int RunAll(int argc, char** argv)
{
    const std::string filter = argc > 1 ? argv[1] : "";
    int passed = 0, failed = 0, skipped = 0;
    for (const TestCase& t : Registry()) {
        if (!filter.empty() && std::string(t.name).find(filter) == std::string::npos)
            continue;
        std::printf("[ RUN  ] %s\n", t.name);
        try {
            t.fn();
            std::printf("[  OK  ] %s\n", t.name);
            ++passed;
        } catch (const Skipped& e) {
            std::printf("[ SKIP ] %s (%s)\n", t.name, e.message.c_str());
            ++skipped;
        } catch (const Failure&) {
            std::printf("[ FAIL ] %s\n", t.name);
            ++failed;
        } catch (const std::exception& e) {
            std::printf("[ FAIL ] %s (exception: %s)\n", t.name, e.what());
            ++failed;
        }
    }
    std::printf("%d passed, %d failed, %d skipped\n", passed, failed, skipped);
    return failed == 0 && passed + skipped > 0 ? 0 : 1;
}

} // namespace satest

#define SA_TEST(name)                                                                                          \
    static void satest_##name();                                                                               \
    static ::satest::Registrar satest_reg_##name(#name, &satest_##name);                                       \
    static void satest_##name()

#define SA_CHECK(cond)                                                                                         \
    do {                                                                                                       \
        if (!(cond))                                                                                           \
            ::satest::Fail(__FILE__, __LINE__, "check failed: " #cond);                                        \
    } while (0)

#define SA_CHECK_EQ(a, b)                                                                                      \
    do {                                                                                                       \
        if (!((a) == (b)))                                                                                     \
            ::satest::Fail(__FILE__, __LINE__, std::string("expected ") + #a + " == " + #b + " (" +            \
                                                   std::to_string(a) + " vs " + std::to_string(b) + ")");    \
    } while (0)

#define SA_CHECK_STREQ(a, b)                                                                                   \
    do {                                                                                                       \
        const std::string sa_sa_ = (a), sa_sb_ = (b);                                                          \
        if (sa_sa_ != sa_sb_)                                                                                  \
            ::satest::Fail(__FILE__, __LINE__, std::string("expected ") + #a + " == " + #b + " (\"" + sa_sa_ +  \
                                                   "\" vs \"" + sa_sb_ + "\")");                             \
    } while (0)

#define SA_CHECK_NEAR(a, b, eps)                                                                               \
    do {                                                                                                       \
        const double sa_a_ = static_cast<double>(a), sa_b_ = static_cast<double>(b);                           \
        if (!std::isfinite(sa_a_) || !std::isfinite(sa_b_) || !(std::fabs(sa_a_ - sa_b_) <= (eps)))                                                                  \
            ::satest::Fail(__FILE__, __LINE__, std::string("expected ") + #a + " ~= " + #b + " (" +            \
                                                   std::to_string(sa_a_) + " vs " + std::to_string(sa_b_) +    \
                                                   ")");                                                       \
    } while (0)
