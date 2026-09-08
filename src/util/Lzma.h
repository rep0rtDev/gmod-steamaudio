// src/util/Lzma.h
//
// LZMA decompression for Source engine data. Source (bspzip -repack, some
// network/file paths) prefixes raw LZMA1 streams with a 17-byte
// `lzma_header_t`:
//
//   uint32 id;            // "LZMA"
//   uint32 actualSize;    // decompressed size
//   uint32 lzmaSize;      // compressed payload size following this header
//   uint8  properties[5]; // lc/lp/pb byte + little-endian dictionary size
//
// The payload is a raw LZMA1 stream without the .lzma container header and
// usually without an end marker (the decoder relies on `actualSize`).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sa {
namespace lzma {

constexpr uint32_t kSourceHeaderId = ('A' << 24) | ('M' << 16) | ('Z' << 8) | 'L'; // "LZMA" in file order
constexpr size_t kSourceHeaderSize = 17;
constexpr size_t kPropsSize = 5;

struct SourceHeader {
    uint32_t actualSize = 0;
    uint32_t lzmaSize = 0;
    uint8_t properties[kPropsSize] = {};
};

// True if `data` starts with a Source lzma_header_t.
bool IsSourceCompressed(const uint8_t* data, size_t size);

// Reads the header; fails if the payload described by it does not fit in `size`.
bool ReadSourceHeader(const uint8_t* data, size_t size, SourceHeader& out);

// Decompresses a Source header-prefixed blob. `maxOutput` bounds the
// decompressed allocation to protect against corrupt headers.
bool DecompressSource(const uint8_t* data, size_t size, std::vector<uint8_t>& out, std::string* error = nullptr,
                      size_t maxOutput = size_t(1) << 30);

// Decompresses a raw LZMA1 stream given its 5 property bytes and the exact
// decompressed size.
bool DecompressRaw(const uint8_t* properties, const uint8_t* in, size_t inSize, size_t outSize,
                   std::vector<uint8_t>& out, std::string* error = nullptr);

// Decompresses a raw LZMA1 stream of unknown length (grows the output up to
// `maxOutput`; the stream must end with an end marker or exhaust the input).
bool DecompressRawStreaming(const uint8_t* properties, const uint8_t* in, size_t inSize, std::vector<uint8_t>& out,
                            std::string* error = nullptr, size_t maxOutput = size_t(1) << 30);

// ".lzma" (LZMA-alone) container as produced by the LZMA SDK / xz --format=lzma
// and used for Workshop downloads: 5 property bytes, uint64 uncompressed size
// (all ones = unknown), then the raw stream.
constexpr size_t kAloneHeaderSize = kPropsSize + 8;
bool IsLzmaAlone(const uint8_t* data, size_t size);
bool DecompressAlone(const uint8_t* data, size_t size, std::vector<uint8_t>& out, std::string* error = nullptr,
                     size_t maxOutput = size_t(1) << 30);

} // namespace lzma
} // namespace sa
