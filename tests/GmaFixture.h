// tests/GmaFixture.h
//
// Builds GMAD archives in memory (the format gmad.exe writes) so the reader
// can be exercised without shipping binary fixtures.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "LzmaLiteralEncoder.h"

namespace satest {

struct GmaFixtureFile {
    std::string name;
    std::vector<uint8_t> data;
};

class GmaFixture {
public:
    uint8_t version = 3;
    uint64_t steamId = 76561198000000000ull;
    uint64_t timestamp = 1700000000ull;
    std::vector<std::string> requiredContent;
    std::string name = "Test Addon";
    std::string description = "{\"description\":\"fixture\",\"type\":\"map\",\"tags\":[\"fun\"]}";
    std::string author = "Author";
    int32_t addonVersion = 1;
    std::vector<GmaFixtureFile> files;

    static void Put32(std::vector<uint8_t>& out, uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            out.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
    static void Put64(std::vector<uint8_t>& out, uint64_t v)
    {
        for (int i = 0; i < 8; ++i)
            out.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
    static void PutString(std::vector<uint8_t>& out, const std::string& s)
    {
        out.insert(out.end(), s.begin(), s.end());
        out.push_back(0);
    }

    // Offset of the first file payload (for offset assertions).
    size_t DataStart() const { return BuildIndex().size(); }

    std::vector<uint8_t> BuildIndex() const
    {
        std::vector<uint8_t> out;
        out.push_back('G');
        out.push_back('M');
        out.push_back('A');
        out.push_back('D');
        out.push_back(version);
        Put64(out, steamId);
        Put64(out, timestamp);
        if (version > 1) {
            for (const std::string& c : requiredContent)
                PutString(out, c);
            PutString(out, "");
        }
        PutString(out, name);
        PutString(out, description);
        PutString(out, author);
        Put32(out, static_cast<uint32_t>(addonVersion));
        uint32_t number = 1;
        for (const GmaFixtureFile& f : files) {
            Put32(out, number++);
            PutString(out, f.name);
            Put64(out, f.data.size());
            Put32(out, 0); // CRC (unused by the reader)
        }
        Put32(out, 0);
        return out;
    }

    std::vector<uint8_t> Build() const
    {
        std::vector<uint8_t> out = BuildIndex();
        for (const GmaFixtureFile& f : files)
            out.insert(out.end(), f.data.begin(), f.data.end());
        Put32(out, 0xDEADBEEFu); // trailing whole-file CRC written by gmad
        return out;
    }

    // Wraps `plain` in an LZMA-alone container (13-byte header + raw stream),
    // the way Workshop downloads are stored. `declareSize == false` writes the
    // "unknown size" marker so the streaming decoder path is exercised.
    static std::vector<uint8_t> LzmaAlone(const std::vector<uint8_t>& plain, bool declareSize = true)
    {
        std::vector<uint8_t> out(5);
        LzmaLiteralEncoder::Properties(out.data());
        Put64(out, declareSize ? static_cast<uint64_t>(plain.size()) : ~uint64_t(0));
        const std::vector<uint8_t> raw = LzmaLiteralEncoder::Encode(plain.data(), plain.size());
        out.insert(out.end(), raw.begin(), raw.end());
        return out;
    }
};

inline std::vector<uint8_t> Bytes(const std::string& s)
{
    return std::vector<uint8_t>(s.begin(), s.end());
}

} // namespace satest
