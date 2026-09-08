// tests/LzmaLiteralEncoder.h
//
// Minimal LZMA1 encoder that emits every byte as a literal (no matches). The
// output is a valid raw LZMA stream for any decoder, which makes it a
// convenient way to produce Source-style compressed lumps from arbitrary
// fixture data without depending on an external compressor.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace satest {

class LzmaLiteralEncoder {
public:
    static constexpr int kLc = 3, kLp = 0, kPb = 2;
    static constexpr uint32_t kDictSize = 1u << 16;

    static void Properties(uint8_t out[5])
    {
        out[0] = static_cast<uint8_t>((kPb * 5 + kLp) * 9 + kLc);
        std::memcpy(out + 1, &kDictSize, 4);
    }

    static std::vector<uint8_t> Encode(const uint8_t* data, size_t size)
    {
        LzmaLiteralEncoder e;
        for (size_t i = 0; i < size; ++i)
            e.EncodeLiteral(i, i > 0 ? data[i - 1] : 0, data[i]);
        e.Flush();
        return e.m_out;
    }

    // Wraps a raw stream in Source's lzma_header_t.
    static std::vector<uint8_t> SourceBlob(const std::vector<uint8_t>& plain)
    {
        const std::vector<uint8_t> raw = Encode(plain.data(), plain.size());
        std::vector<uint8_t> out;
        const uint32_t id = ('A' << 24) | ('M' << 16) | ('Z' << 8) | 'L';
        const uint32_t actual = static_cast<uint32_t>(plain.size());
        const uint32_t lzmaSize = static_cast<uint32_t>(raw.size());
        auto put32 = [&](uint32_t v) {
            for (int i = 0; i < 4; ++i)
                out.push_back(static_cast<uint8_t>(v >> (8 * i)));
        };
        put32(id);
        put32(actual);
        put32(lzmaSize);
        uint8_t props[5];
        Properties(props);
        out.insert(out.end(), props, props + 5);
        out.insert(out.end(), raw.begin(), raw.end());
        return out;
    }

private:
    static constexpr uint16_t kProbInit = 1024;
    static constexpr int kNumStates = 12;
    static constexpr int kNumPosStates = 1 << kPb;

    uint16_t m_isMatch[kNumStates][kNumPosStates];
    std::vector<uint16_t> m_literal;
    uint64_t m_low = 0;
    uint32_t m_range = 0xFFFFFFFFu;
    uint8_t m_cache = 0;
    uint64_t m_cacheSize = 1;
    std::vector<uint8_t> m_out;

    LzmaLiteralEncoder() : m_literal(size_t(0x300) << (kLc + kLp), kProbInit)
    {
        for (auto& row : m_isMatch)
            for (auto& p : row)
                p = kProbInit;
    }

    void ShiftLow()
    {
        if (static_cast<uint32_t>(m_low) < 0xFF000000u || (m_low >> 32) != 0) {
            const uint8_t carry = static_cast<uint8_t>(m_low >> 32);
            uint8_t temp = m_cache;
            do {
                m_out.push_back(static_cast<uint8_t>(temp + carry));
                temp = 0xFF;
            } while (--m_cacheSize != 0);
            m_cache = static_cast<uint8_t>(m_low >> 24);
        }
        ++m_cacheSize;
        m_low = (m_low & 0x00FFFFFFu) << 8;
    }

    void EncodeBit(uint16_t& prob, uint32_t bit)
    {
        const uint32_t bound = (m_range >> 11) * prob;
        if (bit == 0) {
            m_range = bound;
            prob = static_cast<uint16_t>(prob + ((2048 - prob) >> 5));
        } else {
            m_low += bound;
            m_range -= bound;
            prob = static_cast<uint16_t>(prob - (prob >> 5));
        }
        while (m_range < (1u << 24)) {
            m_range <<= 8;
            ShiftLow();
        }
    }

    void EncodeLiteral(size_t pos, uint8_t prevByte, uint8_t byte)
    {
        // State stays 0 because only literals are emitted.
        EncodeBit(m_isMatch[0][pos & (kNumPosStates - 1)], 0);
        const size_t ctx = ((pos & ((1u << kLp) - 1)) << kLc) + (prevByte >> (8 - kLc));
        uint16_t* probs = &m_literal[ctx * 0x300];
        uint32_t symbol = 1;
        for (int i = 7; i >= 0; --i) {
            const uint32_t bit = (byte >> i) & 1;
            EncodeBit(probs[symbol], bit);
            symbol = (symbol << 1) | bit;
        }
    }

    void Flush()
    {
        for (int i = 0; i < 5; ++i)
            ShiftLow();
    }
};

} // namespace satest
