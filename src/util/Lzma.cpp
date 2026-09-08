// src/util/Lzma.cpp
#include "Lzma.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "LzmaDec.h"

namespace sa {
namespace lzma {

namespace {

void* SzAlloc(ISzAllocPtr, size_t size) { return std::malloc(size); }
void SzFree(ISzAllocPtr, void* address) { std::free(address); }
const ISzAlloc g_alloc = {SzAlloc, SzFree};

uint32_t ReadU32(const uint8_t* p)
{
    uint32_t v = 0;
    std::memcpy(&v, p, 4);
    return v;
}

void SetError(std::string* error, const char* message)
{
    if (error)
        *error = message;
}

const char* SzResultName(SRes res)
{
    switch (res) {
    case SZ_ERROR_DATA: return "LZMA data error";
    case SZ_ERROR_MEM: return "LZMA out of memory";
    case SZ_ERROR_UNSUPPORTED: return "LZMA unsupported properties";
    case SZ_ERROR_INPUT_EOF: return "LZMA input truncated";
    default: return "LZMA decode failed";
    }
}

} // namespace

bool IsSourceCompressed(const uint8_t* data, size_t size)
{
    return data && size >= kSourceHeaderSize && ReadU32(data) == kSourceHeaderId;
}

bool ReadSourceHeader(const uint8_t* data, size_t size, SourceHeader& out)
{
    if (!IsSourceCompressed(data, size))
        return false;
    out.actualSize = ReadU32(data + 4);
    out.lzmaSize = ReadU32(data + 8);
    std::memcpy(out.properties, data + 12, kPropsSize);
    return out.lzmaSize <= size - kSourceHeaderSize;
}

bool DecompressRaw(const uint8_t* properties, const uint8_t* in, size_t inSize, size_t outSize,
                   std::vector<uint8_t>& out, std::string* error)
{
    out.assign(outSize, 0);
    if (outSize == 0)
        return true;
    if (!in || inSize == 0) {
        SetError(error, "LZMA payload is empty");
        out.clear();
        return false;
    }
    SizeT destLen = outSize;
    SizeT srcLen = inSize;
    ELzmaStatus status = LZMA_STATUS_NOT_SPECIFIED;
    const SRes res = LzmaDecode(out.data(), &destLen, in, &srcLen, properties, static_cast<unsigned>(kPropsSize),
                                LZMA_FINISH_ANY, &status, &g_alloc);
    if (res != SZ_OK) {
        SetError(error, SzResultName(res));
        out.clear();
        return false;
    }
    if (destLen != outSize) {
        SetError(error, "LZMA stream ended before the expected size");
        out.clear();
        return false;
    }
    return true;
}

bool DecompressRawStreaming(const uint8_t* properties, const uint8_t* in, size_t inSize, std::vector<uint8_t>& out,
                            std::string* error, size_t maxOutput)
{
    out.clear();
    if (!in || inSize == 0) {
        SetError(error, "LZMA payload is empty");
        return false;
    }
    CLzmaDec dec;
    LzmaDec_Construct(&dec);
    SRes res = LzmaDec_Allocate(&dec, properties, static_cast<unsigned>(kPropsSize), &g_alloc);
    if (res != SZ_OK) {
        SetError(error, SzResultName(res));
        return false;
    }
    LzmaDec_Init(&dec);

    size_t inPos = 0;
    size_t produced = 0;
    out.resize(std::min<size_t>(maxOutput, std::max<size_t>(inSize * 4, 64 * 1024)));
    bool finished = false;
    while (!finished) {
        if (produced == out.size()) {
            if (out.size() >= maxOutput) {
                LzmaDec_Free(&dec, &g_alloc);
                SetError(error, "LZMA decompressed size exceeds limit");
                out.clear();
                return false;
            }
            out.resize(std::min(maxOutput, out.size() * 2));
        }
        SizeT destLen = out.size() - produced;
        SizeT srcLen = inSize - inPos;
        ELzmaStatus status = LZMA_STATUS_NOT_SPECIFIED;
        res = LzmaDec_DecodeToBuf(&dec, out.data() + produced, &destLen, in + inPos, &srcLen, LZMA_FINISH_ANY,
                                  &status);
        produced += destLen;
        inPos += srcLen;
        if (res != SZ_OK) {
            LzmaDec_Free(&dec, &g_alloc);
            SetError(error, SzResultName(res));
            out.clear();
            return false;
        }
        if (status == LZMA_STATUS_FINISHED_WITH_MARK) {
            finished = true;
        } else if (inPos == inSize) {
            // No end marker and input exhausted: the stream is complete only if
            // the decoder is not mid-symbol.
            if (destLen == 0 && srcLen == 0) {
                if (status == LZMA_STATUS_NEEDS_MORE_INPUT) {
                    LzmaDec_Free(&dec, &g_alloc);
                    SetError(error, "LZMA input truncated");
                    out.clear();
                    return false;
                }
                finished = true;
            }
        } else if (destLen == 0 && srcLen == 0) {
            LzmaDec_Free(&dec, &g_alloc);
            SetError(error, "LZMA decoder made no progress");
            out.clear();
            return false;
        }
    }
    LzmaDec_Free(&dec, &g_alloc);
    out.resize(produced);
    return true;
}

bool IsLzmaAlone(const uint8_t* data, size_t size)
{
    if (!data || size < kAloneHeaderSize)
        return false;
    // Properties byte encodes lc/lp/pb and must be < 9*5*5; the dictionary size
    // is stored little-endian and is never zero in practice. The 64-bit
    // uncompressed size is either "unknown" (all ones) or something sane.
    if (data[0] >= 225)
        return false;
    uint32_t dictSize = 0;
    std::memcpy(&dictSize, data + 1, 4);
    if (dictSize == 0 || dictSize > (uint32_t(3) << 29))
        return false;
    uint64_t declared = 0;
    std::memcpy(&declared, data + kPropsSize, 8);
    return declared == ~uint64_t(0) || declared < (uint64_t(1) << 40);
}

bool DecompressAlone(const uint8_t* data, size_t size, std::vector<uint8_t>& out, std::string* error,
                     size_t maxOutput)
{
    if (!IsLzmaAlone(data, size)) {
        SetError(error, "not an LZMA-alone stream");
        return false;
    }
    uint64_t declared = 0;
    std::memcpy(&declared, data + kPropsSize, 8);
    const uint8_t* payload = data + kAloneHeaderSize;
    const size_t payloadSize = size - kAloneHeaderSize;
    if (declared == ~uint64_t(0))
        return DecompressRawStreaming(data, payload, payloadSize, out, error, maxOutput);
    if (declared > maxOutput) {
        SetError(error, "LZMA decompressed size exceeds limit");
        return false;
    }
    return DecompressRaw(data, payload, payloadSize, static_cast<size_t>(declared), out, error);
}

bool DecompressSource(const uint8_t* data, size_t size, std::vector<uint8_t>& out, std::string* error,
                      size_t maxOutput)
{
    SourceHeader header;
    if (!IsSourceCompressed(data, size)) {
        SetError(error, "missing LZMA header");
        return false;
    }
    if (!ReadSourceHeader(data, size, header)) {
        SetError(error, "LZMA header describes more data than available");
        return false;
    }
    if (header.actualSize > maxOutput) {
        SetError(error, "LZMA decompressed size exceeds limit");
        return false;
    }
    return DecompressRaw(header.properties, data + kSourceHeaderSize, header.lzmaSize, header.actualSize, out,
                         error);
}

} // namespace lzma
} // namespace sa
