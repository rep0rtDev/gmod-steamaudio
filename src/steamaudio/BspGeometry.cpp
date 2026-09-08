// src/steamaudio/BspGeometry.cpp
#include "BspGeometry.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <unordered_map>

#include "util/Logging.h"
#include "util/Lzma.h"

namespace sa {

namespace {

constexpr int32_t kBspIdent = ('P' << 24) | ('S' << 16) | ('B' << 8) | 'V'; // "VBSP"
constexpr size_t kHeaderLumps = 64;

enum LumpId : size_t {
    LUMP_ENTITIES = 0,
    LUMP_PLANES = 1,
    LUMP_TEXDATA = 2,
    LUMP_VERTEXES = 3,
    LUMP_TEXINFO = 6,
    LUMP_FACES = 7,
    LUMP_EDGES = 12,
    LUMP_SURFEDGES = 13,
    LUMP_MODELS = 14,
    LUMP_DISPINFO = 26,
    LUMP_DISP_VERTS = 33,
    LUMP_GAME_LUMP = 35,
    LUMP_TEXDATA_STRING_DATA = 43,
    LUMP_TEXDATA_STRING_TABLE = 44,
};

enum SurfFlags : int32_t {
    SURF_LIGHT = 0x1,
    SURF_SKY2D = 0x2,
    SURF_SKY = 0x4,
    SURF_WARP = 0x8,
    SURF_TRANS = 0x10,
    SURF_NOPORTAL = 0x20,
    SURF_TRIGGER = 0x40,
    SURF_NODRAW = 0x80,
    SURF_HINT = 0x100,
    SURF_SKIP = 0x200,
    SURF_NOLIGHT = 0x400,
    SURF_BUMPLIGHT = 0x800,
    SURF_NOSHADOWS = 0x1000,
    SURF_NODECALS = 0x2000,
    SURF_NOCHOP = 0x4000,
    SURF_HITBOX = 0x8000,
};

struct LumpHeader {
    uint32_t offset = 0;
    uint32_t length = 0;
    uint32_t version = 0;
    uint32_t uncompressedSize = 0; // `fourCC` field; nonzero when the lump is LZMA-compressed
};

// Bounds-checked little-endian reader.
class Reader {
public:
    Reader(const uint8_t* data, size_t size) : m_data(data), m_size(size) {}

    bool InRange(size_t offset, size_t length) const
    {
        return offset <= m_size && length <= m_size - offset;
    }
    int32_t I32(size_t offset) const
    {
        int32_t v = 0;
        if (InRange(offset, 4))
            std::memcpy(&v, m_data + offset, 4);
        return v;
    }
    uint32_t U32(size_t offset) const { return static_cast<uint32_t>(I32(offset)); }
    int16_t I16(size_t offset) const
    {
        int16_t v = 0;
        if (InRange(offset, 2))
            std::memcpy(&v, m_data + offset, 2);
        return v;
    }
    uint16_t U16(size_t offset) const { return static_cast<uint16_t>(I16(offset)); }
    uint8_t U8(size_t offset) const { return InRange(offset, 1) ? m_data[offset] : 0; }
    float F32(size_t offset) const
    {
        float v = 0.f;
        if (InRange(offset, 4))
            std::memcpy(&v, m_data + offset, 4);
        return v;
    }
    Vec3 Vector(size_t offset) const { return {F32(offset), F32(offset + 4), F32(offset + 8)}; }
    const uint8_t* Ptr(size_t offset) const { return m_data + offset; }
    size_t Size() const { return m_size; }

private:
    const uint8_t* m_data;
    size_t m_size;
};

// A lump's payload: either a view into the file or a decompressed copy.
struct LumpData {
    const uint8_t* data = nullptr;
    size_t size = 0;
    uint32_t version = 0;
    size_t fileOffset = 0; // where the (possibly compressed) lump lives in the file
    bool compressed = false;
    std::vector<uint8_t> owned;

    Reader R() const { return Reader(data, size); }

    void View(const uint8_t* p, size_t n)
    {
        data = p;
        size = n;
        owned.clear();
    }
    void Own(std::vector<uint8_t>&& bytes)
    {
        owned = std::move(bytes);
        data = owned.data();
        size = owned.size();
    }
};

// Materializes a lump from the file, transparently decompressing Source LZMA
// blobs. Returns false (and leaves the lump empty) when the data is corrupt.
bool LoadLumpData(const Reader& file, size_t offset, size_t length, LumpData& out, std::string& error)
{
    out = LumpData{};
    out.fileOffset = offset;
    if (length == 0)
        return true;
    if (!file.InRange(offset, length)) {
        error = "lump extends past end of file";
        return false;
    }
    const uint8_t* p = file.Ptr(offset);
    if (lzma::IsSourceCompressed(p, length)) {
        std::vector<uint8_t> bytes;
        std::string lzErr;
        if (!lzma::DecompressSource(p, length, bytes, &lzErr, size_t(256) << 20)) {
            error = lzErr;
            return false;
        }
        out.Own(std::move(bytes));
        out.compressed = true;
        return true;
    }
    out.View(p, length);
    return true;
}

struct Face {
    uint16_t planenum;
    int32_t firstEdge;
    int16_t numEdges;
    int16_t texinfo;
    int16_t dispinfo;
};

constexpr size_t kFaceSize = 56;
constexpr size_t kTexinfoSize = 72;
constexpr size_t kTexdataSize = 32;
constexpr size_t kModelSize = 48;
constexpr size_t kDispInfoSize = 176;
constexpr size_t kDispVertSize = 20;
constexpr size_t kEdgeSize = 4;

std::string ToLower(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Minimal parser for the entity lump: a sequence of { "key" "value" ... } blocks.
std::vector<std::map<std::string, std::string>> ParseEntities(const char* text, size_t length)
{
    std::vector<std::map<std::string, std::string>> result;
    size_t i = 0;
    auto skipWs = [&]() {
        while (i < length && std::isspace(static_cast<unsigned char>(text[i])))
            ++i;
    };
    auto readQuoted = [&](std::string& out) -> bool {
        skipWs();
        if (i >= length || text[i] != '"')
            return false;
        ++i;
        const size_t start = i;
        while (i < length && text[i] != '"')
            ++i;
        if (i >= length)
            return false;
        out.assign(text + start, i - start);
        ++i;
        return true;
    };
    while (true) {
        skipWs();
        if (i >= length || text[i] == '\0')
            break;
        if (text[i] != '{') {
            ++i;
            continue;
        }
        ++i;
        std::map<std::string, std::string> entity;
        while (true) {
            skipWs();
            if (i >= length)
                break;
            if (text[i] == '}') {
                ++i;
                break;
            }
            std::string key, value;
            if (!readQuoted(key) || !readQuoted(value)) {
                // Malformed; skip to next brace.
                while (i < length && text[i] != '}')
                    ++i;
                if (i < length)
                    ++i;
                break;
            }
            entity[ToLower(key)] = value;
        }
        result.push_back(std::move(entity));
    }
    return result;
}

bool ParseVector(const std::string& s, Vec3& out)
{
    float x = 0.f, y = 0.f, z = 0.f;
    if (std::sscanf(s.c_str(), "%f %f %f", &x, &y, &z) != 3)
        return false;
    out = {x, y, z};
    return true;
}

struct ParseContext {
    const Reader& file;
    LumpData lumps[kHeaderLumps];
    const CoordinateConverter& converter;
    const MaterialLibrary& materials;
    const SurfacePropResolver& resolver;
    std::unordered_map<int32_t, IPLMaterial> texinfoMaterialCache;
    std::unordered_map<int32_t, int32_t> texinfoFlagsCache;

    ParseContext(const Reader& f, const CoordinateConverter& c, const MaterialLibrary& m,
                 const SurfacePropResolver& res)
        : file(f), converter(c), materials(m), resolver(res)
    {
    }

    Reader R(LumpId id) const { return lumps[id].R(); }
    size_t Count(LumpId id, size_t stride) const { return lumps[id].size / stride; }

    Face ReadFace(size_t index) const
    {
        const Reader r = R(LUMP_FACES);
        const size_t o = index * kFaceSize;
        Face f{};
        f.planenum = r.U16(o);
        f.firstEdge = r.I32(o + 4);
        f.numEdges = r.I16(o + 8);
        f.texinfo = r.I16(o + 10);
        f.dispinfo = r.I16(o + 12);
        return f;
    }

    int32_t TexinfoFlags(int32_t texinfo)
    {
        auto it = texinfoFlagsCache.find(texinfo);
        if (it != texinfoFlagsCache.end())
            return it->second;
        int32_t flags = 0;
        if (texinfo >= 0 && static_cast<size_t>(texinfo) < Count(LUMP_TEXINFO, kTexinfoSize))
            flags = R(LUMP_TEXINFO).I32(static_cast<size_t>(texinfo) * kTexinfoSize + 64);
        texinfoFlagsCache[texinfo] = flags;
        return flags;
    }

    std::string TextureName(int32_t texinfo) const
    {
        if (texinfo < 0 || static_cast<size_t>(texinfo) >= Count(LUMP_TEXINFO, kTexinfoSize))
            return {};
        const int32_t texdata = R(LUMP_TEXINFO).I32(static_cast<size_t>(texinfo) * kTexinfoSize + 68);
        if (texdata < 0 || static_cast<size_t>(texdata) >= Count(LUMP_TEXDATA, kTexdataSize))
            return {};
        const int32_t stringId = R(LUMP_TEXDATA).I32(static_cast<size_t>(texdata) * kTexdataSize + 12);
        if (stringId < 0 || static_cast<size_t>(stringId) >= Count(LUMP_TEXDATA_STRING_TABLE, 4))
            return {};
        const int32_t stringOffset = R(LUMP_TEXDATA_STRING_TABLE).I32(static_cast<size_t>(stringId) * 4);
        const LumpData& strings = lumps[LUMP_TEXDATA_STRING_DATA];
        if (stringOffset < 0 || static_cast<size_t>(stringOffset) >= strings.size)
            return {};
        const char* start = reinterpret_cast<const char*>(strings.data + static_cast<size_t>(stringOffset));
        const size_t maxLen = strings.size - static_cast<size_t>(stringOffset);
        size_t len = 0;
        while (len < maxLen && start[len] != '\0')
            ++len;
        return std::string(start, len);
    }

    const IPLMaterial& MaterialFor(int32_t texinfo)
    {
        auto it = texinfoMaterialCache.find(texinfo);
        if (it != texinfoMaterialCache.end())
            return it->second;
        const std::string texture = TextureName(texinfo);
        std::string surfaceProp;
        if (resolver && !texture.empty())
            surfaceProp = resolver(texture);
        const IPLMaterial& mat = !surfaceProp.empty() ? materials.FromSurfaceProp(surfaceProp)
                                                      : materials.FromTextureName(texture);
        return texinfoMaterialCache.emplace(texinfo, mat).first->second;
    }

    // Skips faces that do not represent solid acoustic surfaces.
    bool ShouldSkipFace(const Face& f)
    {
        const int32_t flags = TexinfoFlags(f.texinfo);
        if (flags & (SURF_SKY | SURF_SKY2D | SURF_TRIGGER | SURF_HINT | SURF_SKIP))
            return true;
        const std::string tex = ToLower(TextureName(f.texinfo));
        if (tex.rfind("tools/", 0) == 0) {
            // Solid tool textures that still block sound.
            return !(tex == "tools/toolsnodraw" || tex == "tools/toolsblack" || tex == "tools/toolsblocklight" ||
                     tex == "tools/toolsinvisible" || tex == "tools/toolsclip" || tex == "tools/toolsplayerclip" ||
                     tex == "tools/toolsnpcclip");
        }
        return false;
    }

    // Reads the polygon vertices of a face (Source space).
    bool FaceVertices(const Face& f, std::vector<Vec3>& out) const
    {
        out.clear();
        if (f.numEdges < 3 || f.firstEdge < 0)
            return false;
        const Reader surfedges = R(LUMP_SURFEDGES);
        const Reader edges = R(LUMP_EDGES);
        const Reader vertexes = R(LUMP_VERTEXES);
        const size_t surfedgeCount = Count(LUMP_SURFEDGES, 4);
        const size_t edgeCount = Count(LUMP_EDGES, kEdgeSize);
        const size_t vertexCount = Count(LUMP_VERTEXES, 12);
        for (int32_t e = 0; e < f.numEdges; ++e) {
            const size_t surfedgeIndex = static_cast<size_t>(f.firstEdge) + static_cast<size_t>(e);
            if (surfedgeIndex >= surfedgeCount)
                return false;
            const int32_t surfedge = surfedges.I32(surfedgeIndex * 4);
            const size_t edgeIndex = static_cast<size_t>(surfedge < 0 ? -static_cast<int64_t>(surfedge) : surfedge);
            if (edgeIndex >= edgeCount)
                return false;
            const size_t eo = edgeIndex * kEdgeSize;
            const uint16_t v = surfedge >= 0 ? edges.U16(eo) : edges.U16(eo + 2);
            if (v >= vertexCount)
                return false;
            out.push_back(vertexes.Vector(static_cast<size_t>(v) * 12));
        }
        return true;
    }

    void AppendPolygon(MeshData& mesh, const std::vector<Vec3>& poly, IPLint32 material, const Vec3& pivot)
    {
        if (poly.size() < 3)
            return;
        const IPLint32 base = static_cast<IPLint32>(mesh.vertices.size());
        for (const Vec3& v : poly)
            mesh.vertices.push_back(converter.PositionToSA(v - pivot).ToIPL());
        for (size_t i = 1; i + 1 < poly.size(); ++i) {
            IPLTriangle tri{};
            tri.indices[0] = base;
            tri.indices[1] = base + static_cast<IPLint32>(i);
            tri.indices[2] = base + static_cast<IPLint32>(i + 1);
            mesh.triangles.push_back(tri);
            mesh.materialIndices.push_back(material);
        }
    }

    void AppendDisplacement(MeshData& mesh, const Face& f, const std::vector<Vec3>& corners, IPLint32 material,
                            const Vec3& pivot)
    {
        if (corners.size() != 4)
            return;
        const size_t dispCount = Count(LUMP_DISPINFO, kDispInfoSize);
        if (f.dispinfo < 0 || static_cast<size_t>(f.dispinfo) >= dispCount)
            return;
        const Reader dispInfo = R(LUMP_DISPINFO);
        const Reader dispVerts = R(LUMP_DISP_VERTS);
        const size_t o = static_cast<size_t>(f.dispinfo) * kDispInfoSize;
        const Vec3 startPosition = dispInfo.Vector(o);
        const int32_t dispVertStart = dispInfo.I32(o + 12);
        const int32_t power = dispInfo.I32(o + 20);
        if (power < 2 || power > 4)
            return;
        const int32_t size = (1 << power) + 1;
        const size_t needed = static_cast<size_t>(size) * static_cast<size_t>(size);
        const size_t dispVertCount = Count(LUMP_DISP_VERTS, kDispVertSize);
        if (dispVertStart < 0 || static_cast<size_t>(dispVertStart) + needed > dispVertCount)
            return;

        // Rotate corners so that corner 0 is the displacement start position.
        size_t startIndex = 0;
        float bestDist = Vec3::Distance(corners[0], startPosition);
        for (size_t i = 1; i < 4; ++i) {
            const float d = Vec3::Distance(corners[i], startPosition);
            if (d < bestDist) {
                bestDist = d;
                startIndex = i;
            }
        }
        Vec3 c[4];
        for (size_t i = 0; i < 4; ++i)
            c[i] = corners[(startIndex + i) % 4];

        const IPLint32 base = static_cast<IPLint32>(mesh.vertices.size());
        const float inv = 1.f / static_cast<float>(size - 1);
        const Vec3 edge0 = (c[1] - c[0]) * inv;
        const Vec3 edge1 = (c[2] - c[3]) * inv;
        for (int32_t row = 0; row < size; ++row) {
            const Vec3 endA = c[0] + edge0 * static_cast<float>(row);
            const Vec3 endB = c[3] + edge1 * static_cast<float>(row);
            const Vec3 seg = (endB - endA) * inv;
            for (int32_t col = 0; col < size; ++col) {
                const size_t vi = static_cast<size_t>(dispVertStart) + static_cast<size_t>(row * size + col);
                const size_t vo = vi * kDispVertSize;
                const Vec3 dir = dispVerts.Vector(vo);
                const float dist = dispVerts.F32(vo + 12);
                const Vec3 pos = endA + seg * static_cast<float>(col) + dir * dist;
                mesh.vertices.push_back(converter.PositionToSA(pos - pivot).ToIPL());
            }
        }
        for (int32_t row = 0; row + 1 < size; ++row) {
            for (int32_t col = 0; col + 1 < size; ++col) {
                const IPLint32 i0 = base + row * size + col;
                const IPLint32 i1 = i0 + 1;
                const IPLint32 i2 = i0 + size;
                const IPLint32 i3 = i2 + 1;
                IPLTriangle a{}, b{};
                // Alternate the diagonal like the engine does for a smoother surface.
                if (((row + col) & 1) == 0) {
                    a.indices[0] = i0; a.indices[1] = i2; a.indices[2] = i3;
                    b.indices[0] = i0; b.indices[1] = i3; b.indices[2] = i1;
                } else {
                    a.indices[0] = i0; a.indices[1] = i2; a.indices[2] = i1;
                    b.indices[0] = i1; b.indices[1] = i2; b.indices[2] = i3;
                }
                mesh.triangles.push_back(a);
                mesh.triangles.push_back(b);
                mesh.materialIndices.push_back(material);
                mesh.materialIndices.push_back(material);
            }
        }
    }

    // Builds the mesh for dmodel_t `modelIndex` with vertices relative to `pivot`.
    size_t BuildModel(int32_t modelIndex, const Vec3& pivot, MeshData& mesh)
    {
        const size_t modelCount = Count(LUMP_MODELS, kModelSize);
        if (modelIndex < 0 || static_cast<size_t>(modelIndex) >= modelCount)
            return 0;
        const Reader models = R(LUMP_MODELS);
        const size_t mo = static_cast<size_t>(modelIndex) * kModelSize;
        const int32_t firstFace = models.I32(mo + 40);
        const int32_t numFaces = models.I32(mo + 44);
        const size_t faceCount = Count(LUMP_FACES, kFaceSize);
        if (firstFace < 0 || numFaces < 0 || static_cast<size_t>(firstFace) + static_cast<size_t>(numFaces) > faceCount)
            return 0;

        size_t skipped = 0;
        std::vector<Vec3> poly;
        for (int32_t i = 0; i < numFaces; ++i) {
            const Face f = ReadFace(static_cast<size_t>(firstFace + i));
            if (ShouldSkipFace(f)) {
                ++skipped;
                continue;
            }
            if (!FaceVertices(f, poly)) {
                ++skipped;
                continue;
            }
            const IPLint32 material = mesh.AddMaterial(MaterialFor(f.texinfo));
            if (f.dispinfo >= 0)
                AppendDisplacement(mesh, f, poly, material, pivot);
            else
                AppendPolygon(mesh, poly, material, pivot);
        }
        return skipped;
    }

    // Locates a game lump by id and materializes its payload. Game lump
    // offsets are absolute file offsets; when the whole game lump was
    // decompressed they are rebased onto the decompressed buffer. Individually
    // compressed game lumps (GAMELUMPFLAG_COMPRESSED) carry their own LZMA
    // header, whose `lzmaSize` gives the compressed extent.
    bool LoadGameLump(uint32_t wantedId, LumpData& out, uint16_t& versionOut, std::string& error) const
    {
        const LumpData& game = lumps[LUMP_GAME_LUMP];
        const Reader g = game.R();
        if (game.size < 4)
            return false;
        const int32_t lumpCount = g.I32(0);
        if (lumpCount <= 0 || lumpCount > 64)
            return false;
        constexpr uint16_t kCompressedFlag = 0x0001;
        for (int32_t i = 0; i < lumpCount; ++i) {
            const size_t go = 4 + static_cast<size_t>(i) * 16;
            if (!g.InRange(go, 16))
                return false;
            if (g.U32(go) != wantedId)
                continue;
            const uint16_t flags = g.U16(go + 4);
            versionOut = g.U16(go + 6);
            const int32_t fileofs = g.I32(go + 8);
            const int32_t filelen = g.I32(go + 12);
            if (fileofs < 0 || filelen < 0)
                return false;

            Reader src = file;
            size_t offset = static_cast<size_t>(fileofs);
            if (game.compressed) {
                src = g;
                if (offset < game.fileOffset)
                    return false;
                offset -= game.fileOffset;
            }
            if (!src.InRange(offset, 0) ||
                ((flags & kCompressedFlag) && !src.InRange(offset, lzma::kSourceHeaderSize))) {
                error = "game lump offset is outside the file";
                return false;
            }
            size_t length = static_cast<size_t>(filelen);
            if ((flags & kCompressedFlag) || (src.InRange(offset, lzma::kSourceHeaderSize) &&
                                              lzma::IsSourceCompressed(src.Ptr(offset), src.Size() - offset))) {
                lzma::SourceHeader header;
                if (!lzma::ReadSourceHeader(src.Ptr(offset), src.Size() - offset, header)) {
                    error = "game lump LZMA header is corrupt";
                    return false;
                }
                length = lzma::kSourceHeaderSize + header.lzmaSize;
            }
            return LoadLumpData(src, offset, length, out, error);
        }
        return false;
    }

    void ParseStaticProps(std::vector<BspStaticProp>& props, std::string& error) const
    {
        constexpr uint32_t kStaticPropId = ('s' << 24) | ('p' << 16) | ('r' << 8) | 'p';
        LumpData sprp;
        uint16_t version = 0;
        if (!LoadGameLump(kStaticPropId, sprp, version, error))
            return;
        const Reader r = sprp.R();
        {
            size_t p = 0;
            const size_t end = sprp.size;
            if (end < 4)
                return;
            const int32_t dictEntries = r.I32(p);
            p += 4;
            if (dictEntries < 0 || static_cast<size_t>(dictEntries) > (end - p) / 128)
                return;
            std::vector<std::string> names;
            names.reserve(static_cast<size_t>(dictEntries));
            for (int32_t d = 0; d < dictEntries; ++d) {
                const char* s = reinterpret_cast<const char*>(r.Ptr(p));
                size_t len = 0;
                while (len < 128 && s[len] != '\0')
                    ++len;
                names.emplace_back(s, len);
                p += 128;
            }
            if (p + 4 > end)
                return;
            const int32_t leafEntries = r.I32(p);
            p += 4;
            if (leafEntries < 0 || static_cast<size_t>(leafEntries) > (end - p) / 2)
                return;
            p += static_cast<size_t>(leafEntries) * 2;
            if (p + 4 > end)
                return;
            const int32_t propEntries = r.I32(p);
            p += 4;
            if (propEntries <= 0)
                return;
            const size_t remaining = end - p;
            const size_t stride = remaining / static_cast<size_t>(propEntries);
            if (stride < 32)
                return;
            for (int32_t e = 0; e < propEntries; ++e) {
                const size_t o = p + static_cast<size_t>(e) * stride;
                BspStaticProp prop;
                prop.origin = r.Vector(o);
                prop.angles = r.Vector(o + 12);
                const uint16_t type = r.U16(o + 24);
                prop.solid = r.U8(o + 30);
                if (version >= 11 && stride >= 80)
                    prop.uniformScale = r.F32(o + stride - 4);
                if (type < names.size())
                    prop.model = names[type];
                else
                    continue;
                props.push_back(std::move(prop));
            }
        }
    }
};

// Decodes the 64 lump_t headers. Source 2013 stores {fileofs, filelen,
// version, fourCC}; the Left 4 Dead 2 / CS:GO branch (also BSP v21) stores
// {version, fileofs, filelen, fourCC}. Both are seen in Garry's Mod (CS:GO
// maps are commonly ported), so the layout is chosen by plausibility.
void ReadLumpHeaders(const Reader& r, LumpHeader out[kHeaderLumps])
{
    LumpHeader standard[kHeaderLumps];
    LumpHeader rotated[kHeaderLumps];
    int standardScore = 0, rotatedScore = 0;
    for (size_t i = 0; i < kHeaderLumps; ++i) {
        const size_t lo = 8 + i * 16;
        const uint32_t f0 = r.U32(lo), f1 = r.U32(lo + 4), f2 = r.U32(lo + 8), f3 = r.U32(lo + 12);
        standard[i] = LumpHeader{f0, f1, f2, f3};
        rotated[i] = LumpHeader{f1, f2, f0, f3};
        // Empty lumps carry no evidence; a nonempty lump votes for or against.
        auto score = [&](const LumpHeader& l) {
            if (l.length == 0)
                return 0;
            return (r.InRange(l.offset, l.length) && l.version < 64) ? 1 : -1;
        };
        standardScore += score(standard[i]);
        rotatedScore += score(rotated[i]);
    }
    const LumpHeader* chosen = rotatedScore > standardScore ? rotated : standard;
    for (size_t i = 0; i < kHeaderLumps; ++i)
        out[i] = chosen[i];
}

} // namespace

bool IsAcousticBrushClass(const std::string& classname)
{
    static const char* const kSkipPrefixes[] = {"trigger_", "func_areaportal", "func_occluder", "func_clip",
                                                 "func_ladder", "func_illusionary", "func_buyzone", "func_bomb",
                                                 "func_hostage", "func_precipitation", "func_smokevolume",
                                                 "func_dustmotes", "func_dustcloud", "func_viscluster",
                                                 "func_nobuild", "func_nogrenades", "func_respawnroom",
                                                 "func_regenerate", "func_capturezone", "func_flagdetection",
                                                 "func_monitor", "func_reflective_glass", "func_useableladder",
                                                 "func_extinguisher", "func_instance", "info_", "env_",
                                                 "func_lod", "func_fish_pool", "func_vehicleclip",
                                                 "func_conveyor", "point_", "logic_", "ai_", "game_"};
    for (const char* prefix : kSkipPrefixes) {
        if (classname.rfind(prefix, 0) == 0)
            return false;
    }
    return true;
}

bool ParseBsp(const uint8_t* data, size_t size, const CoordinateConverter& converter,
              const MaterialLibrary& materials, const SurfacePropResolver& resolver, BspGeometry& out)
{
    out = BspGeometry{};
    Reader r(data, size);
    const size_t headerSize = 8 + kHeaderLumps * 16 + 4;
    if (!data || size < headerSize) {
        out.error = "file too small for a BSP header";
        return false;
    }
    if (r.I32(0) != kBspIdent) {
        out.error = "not a VBSP file";
        return false;
    }
    out.version = r.I32(4);
    if (out.version < 17 || out.version > 27) {
        out.error = "unsupported BSP version " + std::to_string(out.version);
        return false;
    }

    ParseContext ctx(r, converter, materials, resolver);
    LumpHeader headers[kHeaderLumps];
    ReadLumpHeaders(r, headers);
    static const LumpId kUsedLumps[] = {LUMP_ENTITIES, LUMP_TEXDATA, LUMP_VERTEXES, LUMP_TEXINFO,
                                        LUMP_FACES, LUMP_EDGES, LUMP_SURFEDGES, LUMP_MODELS,
                                        LUMP_DISPINFO, LUMP_DISP_VERTS, LUMP_GAME_LUMP,
                                        LUMP_TEXDATA_STRING_DATA, LUMP_TEXDATA_STRING_TABLE};
    for (LumpId id : kUsedLumps) {
        const LumpHeader& h = headers[id];
        LumpData& l = ctx.lumps[id];
        std::string err;
        // Corrupt lumps are ignored rather than aborting the whole map.
        if (!LoadLumpData(r, h.offset, h.length, l, err)) {
            SA_LOGW("BSP lump %zu unusable: %s", static_cast<size_t>(id), err.c_str());
            l = LumpData{};
        }
        l.version = h.version;
        if (l.compressed) {
            ++out.compressedLumps;
            if (h.uncompressedSize != 0 && h.uncompressedSize != l.size)
                SA_LOGW("BSP lump %zu: header says %u bytes, LZMA produced %zu", static_cast<size_t>(id),
                        h.uncompressedSize, l.size);
        }
    }
    if (ctx.lumps[LUMP_FACES].size == 0 || ctx.lumps[LUMP_VERTEXES].size == 0 ||
        ctx.lumps[LUMP_MODELS].size == 0) {
        out.error = "BSP has no face/vertex/model lumps";
        return false;
    }

    // World geometry (model 0, pivot at map origin).
    out.skippedFaces += ctx.BuildModel(0, Vec3{}, out.world);
    out.world.Compact();

    // Entities: brush models and metadata.
    const LumpData& entLump = ctx.lumps[LUMP_ENTITIES];
    if (entLump.size > 0) {
        const auto entities = ParseEntities(reinterpret_cast<const char*>(entLump.data), entLump.size);
        for (const auto& ent : entities) {
            auto model = ent.find("model");
            auto classname = ent.find("classname");
            if (model == ent.end() || classname == ent.end())
                continue;
            if (model->second.empty() || model->second[0] != '*')
                continue;
            const int32_t modelIndex = std::atoi(model->second.c_str() + 1);
            if (modelIndex <= 0)
                continue;
            if (!IsAcousticBrushClass(ToLower(classname->second)))
                continue;

            BspBrushModel brush;
            brush.modelIndex = modelIndex;
            brush.classname = classname->second;
            auto targetname = ent.find("targetname");
            if (targetname != ent.end())
                brush.targetname = targetname->second;
            auto origin = ent.find("origin");
            if (origin != ent.end())
                ParseVector(origin->second, brush.spawnOrigin);
            auto solidity = ent.find("solidity");
            // func_brush: 0 toggles, 1 never solid, 2 always solid.
            if (solidity != ent.end() && solidity->second == "1")
                brush.solid = false;
            auto solid = ent.find("solid");
            if (solid != ent.end() && solid->second == "0")
                brush.solid = false;

            out.skippedFaces += ctx.BuildModel(modelIndex, brush.spawnOrigin, brush.mesh);
            brush.mesh.Compact();
            if (!brush.mesh.Empty())
                out.brushModels.push_back(std::move(brush));
        }
    }

    {
        std::string err;
        ctx.ParseStaticProps(out.staticProps, err);
        if (!err.empty())
            SA_LOGW("BSP static prop lump unusable: %s", err.c_str());
    }

    SA_LOGI("BSP v%d parsed: world %zu tris, %zu brush models, %zu static props, %zu faces skipped, %zu lumps "
            "LZMA-compressed",
            out.version, out.world.triangles.size(), out.brushModels.size(), out.staticProps.size(),
            out.skippedFaces, out.compressedLumps);
    return !out.world.Empty();
}

} // namespace sa
