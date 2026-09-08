#include "array_math.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    constexpr size_t capacity = 516;
    const ipl::complex_t sentinel{-123.f, 456.f};
    int cases = 0;
    for (int count : {0, 1, 2, 3, 4, 5, 7, 16, 127, 512, 513}) {
        for (int firstOffset : {0, 1}) {
            for (int secondOffset : {0, 1}) {
                for (int outputOffset : {0, 1}) {
                    alignas(16) std::array<ipl::complex_t, capacity> first{}, second{}, output{}, expected{};
                    output.fill(sentinel);
                    auto* a = first.data() + firstOffset;
                    auto* b = second.data() + secondOffset;
                    auto* c = output.data() + outputOffset;
                    for (int i = 0; i < count; ++i) {
                        const float value = static_cast<float>(i % 17) * 0.125f;
                        a[i] = {value + 0.5f, value - 0.75f};
                        b[i] = {value - 0.25f, 0.625f - value};
                        c[i] = {0.25f + value, -0.5f - value};
                        expected[static_cast<size_t>(i)] = c[i] + a[i] * b[i];
                    }
                    std::printf("MAC count=%d offsets=%d/%d/%d\n", count, firstOffset, secondOffset, outputOffset);
                    ipl::ArrayMath::multiplyAccumulate(count, a, b, c);
                    for (int i = 0; i < count; ++i) {
                        const auto want = expected[static_cast<size_t>(i)];
                        const float tolerance = 1e-5f * std::max(1.f, std::abs(want));
                        if (!std::isfinite(c[i].real()) || !std::isfinite(c[i].imag()) ||
                            std::abs(c[i] - want) > tolerance) {
                            std::fprintf(stderr, "Complex MAC mismatch at %d\n", i);
                            return 1;
                        }
                    }
                    for (size_t i = 0; i < capacity; ++i) {
                        if ((i < static_cast<size_t>(outputOffset) || i >= static_cast<size_t>(outputOffset + count)) &&
                            output[i] != sentinel) {
                            std::fprintf(stderr, "Complex MAC wrote outside the output range\n");
                            return 1;
                        }
                    }
                    ++cases;
                }
            }
        }
    }
    std::printf("PASS: %d aligned/unaligned complex MAC cases, including 513-bin FFT rows\n", cases);
    return 0;
}
