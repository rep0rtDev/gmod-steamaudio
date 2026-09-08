// src/steamaudio/PhyModel.cpp
#include "steamaudio/PhyModel.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace sa {
namespace {

// Ipion works in meters, Y-up; Source's vphysics converts with
// ConvertPositionToHL: hl = (ivp.x, ivp.z, -ivp.y) / METERS_PER_INCH.
constexpr float kIvpToHl = 1.f / 0.0254f;

constexpr int32_t kVphyId = ('V' << 0) | ('P' << 8) | ('H' << 16) | ('Y' << 24);
constexpr int32_t kIvpsId = ('I' << 0) | ('V' << 8) | ('P' << 16) | ('S' << 24);
constexpr int32_t kIdstId = ('I' << 0) | ('D' << 8) | ('S' << 16) | ('T' << 24);

constexpr size_t kPhyHeaderSize = 16;          // phyheader_t
constexpr size_t kSolidHeaderSize = 32;        // compactsurfaceheader_t
constexpr size_t kCompactSurfaceSize = 48;     // IVP_Compact_Surface
constexpr size_t kLedgeSize = 16;              // IVP_Compact_Ledge
constexpr size_t kTriangleSize = 16;           // IVP_Compact_Triangle
constexpr size_t kPointSize = 16;              // IVP_Compact_Poly_Point (x, y, z, hesse)
constexpr size_t kLedgeNodeSize = 32;          // IVP_Compact_Ledgetree_Node
constexpr size_t kMaxLedgesPerSolid = 65536;
constexpr size_t kMaxTrianglesPerSolid = 4u << 20;
constexpr int kMaxTreeDepth = 64;

struct Reader {
    const uint8_t* data;
    size_t size;

    bool Has(size_t offset, size_t length) const { return offset <= size && length <= size - offset; }
    int32_t I32(size_t offset) const
    {
        int32_t v;
        std::memcpy(&v, data + offset, 4);
        return v;
    }
    uint32_t U32(size_t offset) const { return static_cast<uint32_t>(I32(offset)); }
    int16_t I16(size_t offset) const
    {
        int16_t v;
        std::memcpy(&v, data + offset, 2);
        return v;
    }
    float F32(size_t offset) const
    {
        float v;
        std::memcpy(&v, data + offset, 4);
        return v;
    }
    Vec3 Vector(size_t offset) const { return {F32(offset), F32(offset + 4), F32(offset + 8)}; }
};

// One IVP compact surface = one solid. All offsets below are relative to
// `base` (the start of the IVP_Compact_Surface), bounds are `end`.
class SurfaceParser {
public:
    SurfaceParser(const Reader& r, size_t base, size_t end, PhySolid& out) : m_r(r), m_base(base), m_end(end), m_out(out) {}

    bool Parse(std::string& error)
    {
        if (!m_r.Has(m_base, kCompactSurfaceSize)) {
            error = "compact surface header truncated";
            return false;
        }
        const int32_t rootOffset = m_r.I32(m_base + 36);
        bool any = false;
        if (rootOffset > 0 && static_cast<size_t>(rootOffset) + kLedgeNodeSize <= m_end - m_base) {
            std::unordered_set<size_t> seenLedges;
            any = WalkTree(static_cast<size_t>(rootOffset), 0, seenLedges);
        }
        if (!any) {
            // Ledges are laid out back to back after the surface header, ahead
            // of the ledge tree; recover them linearly when the tree is unusable.
            m_out.vertices.clear();
            m_out.triangles.clear();
            m_out.ledges = 0;
            m_vertexRemap.clear();
            size_t offset = kCompactSurfaceSize;
            const size_t stop = rootOffset > 0 ? std::min(static_cast<size_t>(rootOffset), m_end - m_base) : m_end - m_base;
            while (offset + kLedgeSize <= stop && m_out.ledges < kMaxLedgesPerSolid) {
                const size_t ledgeSize = LedgeSize(offset);
                if (ledgeSize < kLedgeSize || offset + ledgeSize > stop)
                    break;
                if (!ReadLedge(offset))
                    break;
                offset += ledgeSize;
                any = true;
            }
        }
        if (!any)
            error = "no collision ledges found";
        return any;
    }

private:
    size_t LedgeSize(size_t ledge) const
    {
        if (!m_r.Has(m_base + ledge, kLedgeSize))
            return 0;
        const uint32_t packed = m_r.U32(m_base + ledge + 8);
        return static_cast<size_t>(packed >> 8) * 16; // size_div_16 occupies the top 24 bits
    }

    bool WalkTree(size_t node, int depth, std::unordered_set<size_t>& seenLedges)
    {
        if (depth > kMaxTreeDepth || !m_r.Has(m_base + node, kLedgeNodeSize) || node + kLedgeNodeSize > m_end - m_base ||
            m_seenNodes.size() >= kMaxLedgesPerSolid * 2 || !m_seenNodes.insert(node).second)
            return false;
        const int32_t rightOffset = m_r.I32(m_base + node);
        const int32_t ledgeOffset = m_r.I32(m_base + node + 4);
        if (rightOffset == 0) {
            if (ledgeOffset == 0)
                return false;
            const int64_t ledge = static_cast<int64_t>(node) + ledgeOffset;
            if (ledge < static_cast<int64_t>(kCompactSurfaceSize) || ledge >= static_cast<int64_t>(m_end - m_base))
                return false;
            if (!seenLedges.insert(static_cast<size_t>(ledge)).second)
                return true; // shared leaf, already emitted
            return ReadLedge(static_cast<size_t>(ledge));
        }
        if (rightOffset < 0 || m_out.ledges >= kMaxLedgesPerSolid)
            return false;
        const bool left = WalkTree(node + kLedgeNodeSize, depth + 1, seenLedges);
        const bool right = WalkTree(node + static_cast<size_t>(rightOffset), depth + 1, seenLedges);
        return left || right;
    }

    bool ReadLedge(size_t ledge)
    {
        if (!m_r.Has(m_base + ledge, kLedgeSize) || ledge + kLedgeSize > m_end - m_base)
            return false;
        const size_t abs = m_base + ledge;
        const int32_t pointOffset = m_r.I32(abs);
        const int16_t triangleCount = m_r.I16(abs + 12);
        if (triangleCount <= 0 || pointOffset <= 0)
            return false;
        const size_t triangles = static_cast<size_t>(triangleCount);
        if (triangles > kMaxTrianglesPerSolid || m_out.triangles.size() + triangles > kMaxTrianglesPerSolid)
            return false;
        const size_t triBase = ledge + kLedgeSize;
        if (triBase + triangles * kTriangleSize > m_end - m_base)
            return false;
        const size_t pointBase = ledge + static_cast<size_t>(pointOffset);
        if (pointBase >= m_end - m_base)
            return false;
        const size_t pointCapacity = (m_end - m_base - pointBase) / kPointSize;

        m_vertexRemap.clear();
        size_t emitted = 0;
        for (size_t t = 0; t < triangles; ++t) {
            const size_t tri = m_base + triBase + t * kTriangleSize;
            const uint32_t head = m_r.U32(tri);
            const uint8_t material = static_cast<uint8_t>((head >> 24) & 0x7F);
            uint32_t idx[3];
            bool ok = true;
            for (int e = 0; e < 3 && ok; ++e) {
                const uint32_t edge = m_r.U32(tri + 4 + static_cast<size_t>(e) * 4);
                const uint32_t point = edge & 0xFFFF;
                if (point >= pointCapacity) {
                    ok = false;
                    break;
                }
                idx[e] = Vertex(pointBase, point);
            }
            if (!ok)
                continue;
            if (idx[0] == idx[1] || idx[1] == idx[2] || idx[0] == idx[2])
                continue;
            PhyTriangle out{};
            out.indices[0] = idx[0];
            out.indices[1] = idx[1];
            out.indices[2] = idx[2];
            out.materialIndex = material;
            m_out.triangles.push_back(out);
            ++emitted;
        }
        if (emitted == 0)
            return false;
        ++m_out.ledges;
        return true;
    }

    // Points are shared inside a ledge; dedupe per ledge by index.
    uint32_t Vertex(size_t pointBase, uint32_t point)
    {
        auto it = m_vertexRemap.find(point);
        if (it != m_vertexRemap.end())
            return it->second;
        const size_t abs = m_base + pointBase + static_cast<size_t>(point) * kPointSize;
        const Vec3 ivp = m_r.Vector(abs);
        Vec3 hl{ivp.x * kIvpToHl, ivp.z * kIvpToHl, -ivp.y * kIvpToHl};
        if (!hl.IsFinite())
            hl = Vec3{};
        const uint32_t index = static_cast<uint32_t>(m_out.vertices.size());
        m_out.vertices.push_back(hl);
        m_vertexRemap.emplace(point, index);
        return index;
    }

    const Reader& m_r;
    size_t m_base;
    size_t m_end;
    PhySolid& m_out;
    std::unordered_map<uint32_t, uint32_t> m_vertexRemap;
    std::unordered_set<size_t> m_seenNodes;
};

// --- key-value text section -------------------------------------------------

struct Token {
    enum Kind { End, Open, Close, String } kind = End;
    std::string text;
};

class KeyValueLexer {
public:
    explicit KeyValueLexer(const char* begin, const char* end) : m_p(begin), m_end(end) {}

    Token Next()
    {
        SkipSpaceAndComments();
        Token t;
        if (m_p >= m_end)
            return t;
        const char c = *m_p;
        if (c == '{') {
            ++m_p;
            t.kind = Token::Open;
            return t;
        }
        if (c == '}') {
            ++m_p;
            t.kind = Token::Close;
            return t;
        }
        t.kind = Token::String;
        if (c == '"') {
            ++m_p;
            while (m_p < m_end && *m_p != '"')
                t.text.push_back(*m_p++);
            if (m_p < m_end)
                ++m_p;
            return t;
        }
        while (m_p < m_end && !std::isspace(static_cast<unsigned char>(*m_p)) && *m_p != '{' && *m_p != '}' &&
               *m_p != '"')
            t.text.push_back(*m_p++);
        return t;
    }

private:
    void SkipSpaceAndComments()
    {
        for (;;) {
            while (m_p < m_end && (std::isspace(static_cast<unsigned char>(*m_p)) || *m_p == '\0'))
                ++m_p;
            if (m_p + 1 < m_end && m_p[0] == '/' && m_p[1] == '/') {
                while (m_p < m_end && *m_p != '\n')
                    ++m_p;
                continue;
            }
            return;
        }
    }

    const char* m_p;
    const char* m_end;
};

std::string Lower(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

int32_t ParseIndex(const std::string& s)
{
    int32_t value = -1;
    const auto result = std::from_chars(s.data(), s.data() + s.size(), value);
    return result.ec == std::errc{} && result.ptr == s.data() + s.size() && value >= 0 ? value : -1;
}

bool IsInteger(const std::string& s)
{
    return ParseIndex(s) >= 0;
}

// Fills PhySolid::surfaceProp (by "index") and PhyModel::materialTable from the
// text section. Unknown blocks are skipped structurally.
void ParseTextSection(const char* begin, const char* end, PhyModel& model)
{
    KeyValueLexer lex(begin, end);
    for (;;) {
        Token name = lex.Next();
        if (name.kind == Token::End)
            return;
        if (name.kind != Token::String)
            continue;
        Token open = lex.Next();
        if (open.kind != Token::Open)
            continue;
        const std::string block = Lower(name.text);
        std::vector<std::pair<std::string, std::string>> pairs;
        int depth = 1;
        while (depth > 0) {
            Token key = lex.Next();
            if (key.kind == Token::End)
                break;
            if (key.kind == Token::Close) {
                --depth;
                continue;
            }
            if (key.kind == Token::Open) {
                ++depth;
                continue;
            }
            Token value = lex.Next();
            if (value.kind == Token::End)
                break;
            if (value.kind == Token::Open) {
                ++depth;
                continue;
            }
            if (value.kind == Token::Close) {
                --depth;
                continue;
            }
            if (depth == 1)
                pairs.emplace_back(Lower(key.text), value.text);
        }
        if (block == "solid") {
            int32_t index = -1;
            std::string surfaceProp;
            for (const auto& kv : pairs) {
                if (kv.first == "index" && IsInteger(kv.second))
                    index = ParseIndex(kv.second);
                else if (kv.first == "surfaceprop")
                    surfaceProp = Lower(kv.second);
            }
            if (index >= 0 && static_cast<size_t>(index) < model.solids.size())
                model.solids[static_cast<size_t>(index)].surfaceProp = surfaceProp;
            else if (index < 0 && model.solids.size() == 1 && model.solids[0].surfaceProp.empty())
                model.solids[0].surfaceProp = surfaceProp;
        } else if (block == "materialtable") {
            // Written either as "index" "name" or "name" "index".
            for (const auto& kv : pairs) {
                std::string indexText;
                std::string materialName;
                if (IsInteger(kv.first)) {
                    indexText = kv.first;
                    materialName = kv.second;
                } else if (IsInteger(kv.second)) {
                    indexText = kv.second;
                    materialName = kv.first;
                } else {
                    continue;
                }
                const int32_t index = ParseIndex(indexText);
                if (index <= 0 || index > 127)
                    continue;
                if (model.materialTable.size() < static_cast<size_t>(index))
                    model.materialTable.resize(static_cast<size_t>(index));
                model.materialTable[static_cast<size_t>(index) - 1] = Lower(materialName);
            }
        }
    }
}

std::string ReadCString(const uint8_t* data, size_t size, size_t offset, size_t maxLength)
{
    if (offset >= size)
        return {};
    size_t len = 0;
    while (offset + len < size && len < maxLength && data[offset + len] != '\0')
        ++len;
    return std::string(reinterpret_cast<const char*>(data + offset), len);
}

} // namespace

size_t PhyModel::TriangleCount() const
{
    size_t n = 0;
    for (const PhySolid& s : solids)
        n += s.triangles.size();
    return n;
}

bool ParsePhy(const uint8_t* data, size_t size, PhyModel& out)
{
    out = PhyModel{};
    if (!data || size < kPhyHeaderSize) {
        out.error = "file too small";
        return false;
    }
    const Reader r{data, size};
    const int32_t headerSize = r.I32(0);
    out.checksum = r.I32(12);
    out.solidCount = r.I32(8);
    if (headerSize < static_cast<int32_t>(kPhyHeaderSize) || static_cast<size_t>(headerSize) > size) {
        out.error = "bad phy header size";
        return false;
    }
    if (out.solidCount <= 0 || out.solidCount > 1024) {
        out.error = "implausible solid count " + std::to_string(out.solidCount);
        return false;
    }

    size_t offset = static_cast<size_t>(headerSize);
    for (int32_t s = 0; s < out.solidCount; ++s) {
        if (!r.Has(offset, kSolidHeaderSize)) {
            out.error = "solid " + std::to_string(s) + " header truncated";
            break;
        }
        const int32_t solidSize = r.I32(offset);
        const int32_t id = r.I32(offset + 4);
        const int16_t modelType = r.I16(offset + 10);
        if (solidSize < static_cast<int32_t>(kSolidHeaderSize - 4) || !r.Has(offset + 4, static_cast<size_t>(solidSize))) {
            out.error = "solid " + std::to_string(s) + " size out of range";
            break;
        }
        const size_t solidEnd = offset + 4 + static_cast<size_t>(solidSize);
        if (id != kVphyId) {
            out.error = "solid " + std::to_string(s) + " is not VPHY";
            ++out.skippedSolids;
            out.solids.emplace_back();
            offset = solidEnd;
            continue;
        }
        const size_t surface = offset + kSolidHeaderSize;
        if (modelType != 0 || !r.Has(surface, kCompactSurfaceSize) || r.I32(surface + 44) != kIvpsId) {
            out.error = modelType != 0 ? "solid " + std::to_string(s) + " uses MOPP collision (unsupported)"
                                       : "solid " + std::to_string(s) + " missing IVPS surface";
            ++out.skippedSolids;
            out.solids.emplace_back();
            offset = solidEnd;
            continue;
        }
        PhySolid solid;
        SurfaceParser parser(r, surface, solidEnd, solid);
        std::string err;
        if (!parser.Parse(err)) {
            out.error = "solid " + std::to_string(s) + ": " + err;
            ++out.skippedSolids;
            out.solids.emplace_back();
        } else {
            out.solids.push_back(std::move(solid));
        }
        offset = solidEnd;
    }

    if (offset < size)
        ParseTextSection(reinterpret_cast<const char*>(data + offset), reinterpret_cast<const char*>(data + size), out);

    return out.TriangleCount() > 0;
}

bool ParseMdlInfo(const uint8_t* data, size_t size, MdlInfo& out)
{
    out = MdlInfo{};
    constexpr size_t kMinHeader = 312; // through surfacepropindex
    if (!data || size < kMinHeader)
        return false;
    const Reader r{data, size};
    if (r.I32(0) != kIdstId)
        return false;
    out.version = r.I32(4);
    out.checksum = r.I32(8);
    out.name = ReadCString(data, size, 12, 64);
    out.hullMin = r.Vector(104);
    out.hullMax = r.Vector(116);
    if (!out.hullMin.IsFinite() || !out.hullMax.IsFinite()) {
        out.hullMin = out.hullMax = Vec3{};
    }
    out.hasHull = (out.hullMax - out.hullMin).LengthSq() > 1e-6f;
    const int32_t surfacePropIndex = r.I32(308);
    if (surfacePropIndex > 0 && static_cast<size_t>(surfacePropIndex) < size)
        out.surfaceProp = Lower(ReadCString(data, size, static_cast<size_t>(surfacePropIndex), 128));
    return true;
}

void AppendBox(const Vec3& mins, const Vec3& maxs, std::vector<Vec3>& vertices, std::vector<PhyTriangle>& triangles,
               uint8_t materialIndex)
{
    const uint32_t base = static_cast<uint32_t>(vertices.size());
    for (int i = 0; i < 8; ++i) {
        vertices.push_back({(i & 1) ? maxs.x : mins.x, (i & 2) ? maxs.y : mins.y, (i & 4) ? maxs.z : mins.z});
    }
    // Faces as quads (counter-clockwise seen from outside).
    static const uint32_t kQuads[6][4] = {
        {0, 4, 6, 2}, // -x
        {1, 3, 7, 5}, // +x
        {0, 1, 5, 4}, // -y
        {2, 6, 7, 3}, // +y
        {0, 2, 3, 1}, // -z
        {4, 5, 7, 6}, // +z
    };
    for (const auto& q : kQuads) {
        PhyTriangle a{};
        a.indices[0] = base + q[0];
        a.indices[1] = base + q[1];
        a.indices[2] = base + q[2];
        a.materialIndex = materialIndex;
        PhyTriangle b{};
        b.indices[0] = base + q[0];
        b.indices[1] = base + q[2];
        b.indices[2] = base + q[3];
        b.materialIndex = materialIndex;
        triangles.push_back(a);
        triangles.push_back(b);
    }
}

} // namespace sa
