// tests/BspFixture.h
//
// Builds small synthetic VBSP files in memory so the parser can be exercised
// without shipping real map data.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "LzmaLiteralEncoder.h"

namespace satest {

struct BspBuilder {
    struct Vec {
        float x, y, z;
    };

    std::vector<uint8_t> lumps[64];
    uint32_t lumpVersions[64] = {};
    bool compressLump[64] = {};      // emit the lump as a Source LZMA blob
    bool compressGameLumpData = false; // compress each game lump payload (GAMELUMPFLAG_COMPRESSED)
    bool l4d2HeaderLayout = false;   // lump_t = {version, fileofs, filelen, fourCC}
    int32_t version = 21;

    static void Put32(std::vector<uint8_t>& v, int32_t x)
    {
        const size_t o = v.size();
        v.resize(o + 4);
        std::memcpy(v.data() + o, &x, 4);
    }
    static void Put16(std::vector<uint8_t>& v, int16_t x)
    {
        const size_t o = v.size();
        v.resize(o + 2);
        std::memcpy(v.data() + o, &x, 2);
    }
    static void PutF(std::vector<uint8_t>& v, float x)
    {
        const size_t o = v.size();
        v.resize(o + 4);
        std::memcpy(v.data() + o, &x, 4);
    }
    static void PutVec(std::vector<uint8_t>& v, Vec p)
    {
        PutF(v, p.x);
        PutF(v, p.y);
        PutF(v, p.z);
    }
    static void PutBytes(std::vector<uint8_t>& v, const void* data, size_t n)
    {
        const auto* b = static_cast<const uint8_t*>(data);
        v.insert(v.end(), b, b + n);
    }
    static void PutZeros(std::vector<uint8_t>& v, size_t n) { v.resize(v.size() + n, 0); }

    int32_t faceCount = 0;
    int32_t vertexCount = 0;
    int32_t edgeCount = 0;
    int32_t surfedgeCount = 0;
    int32_t texinfoCount = 0;

    // Adds a texture (texdata + texinfo) and returns the texinfo index.
    int32_t AddTexture(const std::string& name, int32_t flags = 0)
    {
        std::vector<uint8_t>& strData = lumps[43];
        std::vector<uint8_t>& strTable = lumps[44];
        const int32_t strOffset = static_cast<int32_t>(strData.size());
        PutBytes(strData, name.c_str(), name.size() + 1);
        const int32_t stringId = static_cast<int32_t>(strTable.size() / 4);
        Put32(strTable, strOffset);

        std::vector<uint8_t>& texdata = lumps[2];
        const int32_t texdataIndex = static_cast<int32_t>(texdata.size() / 32);
        PutVec(texdata, {0.5f, 0.5f, 0.5f}); // reflectivity
        Put32(texdata, stringId);
        Put32(texdata, 64); // width
        Put32(texdata, 64); // height
        Put32(texdata, 64);
        Put32(texdata, 64);

        std::vector<uint8_t>& texinfo = lumps[6];
        const int32_t index = texinfoCount++;
        PutZeros(texinfo, 64); // texture/lightmap vecs
        Put32(texinfo, flags);
        Put32(texinfo, texdataIndex);
        return index;
    }

    // Adds a polygon face (vertices in winding order) and returns the face index.
    int32_t AddFace(const std::vector<Vec>& poly, int32_t texinfo, int16_t dispinfo = -1)
    {
        std::vector<uint8_t>& verts = lumps[3];
        std::vector<uint8_t>& edges = lumps[12];
        std::vector<uint8_t>& surfedges = lumps[13];
        const int32_t firstVertex = vertexCount;
        for (const Vec& p : poly) {
            PutVec(verts, p);
            ++vertexCount;
        }
        const int32_t firstEdge = surfedgeCount;
        const int32_t n = static_cast<int32_t>(poly.size());
        for (int32_t i = 0; i < n; ++i) {
            const int32_t a = firstVertex + i;
            const int32_t b = firstVertex + (i + 1) % n;
            if (i % 2 == 0) {
                Put16(edges, static_cast<int16_t>(a));
                Put16(edges, static_cast<int16_t>(b));
                Put32(surfedges, edgeCount);
            } else {
                // Exercise the reversed-edge path.
                Put16(edges, static_cast<int16_t>(b));
                Put16(edges, static_cast<int16_t>(a));
                Put32(surfedges, -edgeCount);
            }
            ++edgeCount;
            ++surfedgeCount;
        }
        std::vector<uint8_t>& faces = lumps[7];
        Put16(faces, 0);                        // planenum
        faces.push_back(0);                     // side
        faces.push_back(1);                     // onNode
        Put32(faces, firstEdge);                // firstedge
        Put16(faces, static_cast<int16_t>(n));  // numedges
        Put16(faces, static_cast<int16_t>(texinfo));
        Put16(faces, dispinfo);
        PutZeros(faces, 56 - 14);
        return faceCount++;
    }

    // Adds an axis-aligned box as 6 faces; returns the first face index.
    int32_t AddBox(Vec mn, Vec mx, int32_t texinfo)
    {
        const Vec v[8] = {{mn.x, mn.y, mn.z}, {mx.x, mn.y, mn.z}, {mx.x, mx.y, mn.z}, {mn.x, mx.y, mn.z},
                          {mn.x, mn.y, mx.z}, {mx.x, mn.y, mx.z}, {mx.x, mx.y, mx.z}, {mn.x, mx.y, mx.z}};
        const int32_t first = faceCount;
        AddFace({v[0], v[1], v[2], v[3]}, texinfo);
        AddFace({v[4], v[7], v[6], v[5]}, texinfo);
        AddFace({v[0], v[4], v[5], v[1]}, texinfo);
        AddFace({v[1], v[5], v[6], v[2]}, texinfo);
        AddFace({v[2], v[6], v[7], v[3]}, texinfo);
        AddFace({v[3], v[7], v[4], v[0]}, texinfo);
        return first;
    }

    // Adds a dmodel_t covering faces [first, first+count).
    int32_t AddModel(int32_t firstFace, int32_t count, Vec origin = {0, 0, 0})
    {
        std::vector<uint8_t>& models = lumps[14];
        const int32_t index = static_cast<int32_t>(models.size() / 48);
        PutVec(models, {-1e4f, -1e4f, -1e4f});
        PutVec(models, {1e4f, 1e4f, 1e4f});
        PutVec(models, origin);
        Put32(models, 0); // headnode
        Put32(models, firstFace);
        Put32(models, count);
        return index;
    }

    void SetEntities(const std::string& text)
    {
        lumps[0].assign(text.begin(), text.end());
        lumps[0].push_back(0);
    }

    // Static prop game lump (version 6 layout, 64-byte entries).
    void AddStaticProps(const std::vector<std::string>& models,
                        const std::vector<std::pair<int, Vec>>& props, uint16_t lumpVersion = 6)
    {
        std::vector<uint8_t> sprp;
        Put32(sprp, static_cast<int32_t>(models.size()));
        for (const std::string& m : models) {
            char name[128] = {};
            std::strncpy(name, m.c_str(), sizeof(name) - 1);
            PutBytes(sprp, name, sizeof(name));
        }
        Put32(sprp, 0); // leaf entries
        Put32(sprp, static_cast<int32_t>(props.size()));
        for (const auto& p : props) {
            PutVec(sprp, p.second);          // origin
            PutVec(sprp, {0.f, 90.f, 0.f});  // angles
            Put16(sprp, static_cast<int16_t>(p.first)); // prop type
            Put16(sprp, 0);                  // first leaf
            Put16(sprp, 0);                  // leaf count
            sprp.push_back(6);               // solid
            sprp.push_back(0);               // flags
            Put32(sprp, 0);                  // skin
            PutF(sprp, 0.f);                 // fademin
            PutF(sprp, 0.f);                 // fademax
            PutVec(sprp, {0, 0, 0});         // lighting origin
            PutF(sprp, 1.f);                 // forced fade scale
            Put16(sprp, 0);                  // min dx
            Put16(sprp, 0);                  // max dx
            // version 6: 64 bytes total
        }
        AddGameLump(('s' << 24) | ('p' << 16) | ('r' << 8) | 'p', lumpVersion, sprp);
    }

    struct GameLump {
        uint32_t id;
        uint16_t version;
        std::vector<uint8_t> data;
    };
    std::vector<GameLump> gameLumps;

    void AddGameLump(uint32_t id, uint16_t lumpVersion, std::vector<uint8_t> data)
    {
        gameLumps.push_back({id, lumpVersion, std::move(data)});
    }

    // Serializes the game lump directory + payloads as they will appear at
    // `baseOffset` in the file (game lump offsets are absolute).
    std::vector<uint8_t> BuildGameLump(size_t baseOffset) const
    {
        std::vector<std::vector<uint8_t>> payloads;
        for (const GameLump& g : gameLumps)
            payloads.push_back(compressGameLumpData ? LzmaLiteralEncoder::SourceBlob(g.data) : g.data);
        std::vector<uint8_t> out;
        Put32(out, static_cast<int32_t>(gameLumps.size()));
        size_t dataCursor = baseOffset + 4 + gameLumps.size() * 16;
        for (size_t i = 0; i < gameLumps.size(); ++i) {
            const GameLump& g = gameLumps[i];
            Put32(out, static_cast<int32_t>(g.id));
            Put16(out, compressGameLumpData ? 1 : 0); // flags
            Put16(out, static_cast<int16_t>(g.version));
            Put32(out, static_cast<int32_t>(dataCursor));
            // Source stores the *uncompressed* size for compressed game lumps;
            // the compressed extent comes from the LZMA header.
            Put32(out, static_cast<int32_t>(g.data.size()));
            dataCursor += payloads[i].size();
        }
        for (const auto& p : payloads)
            PutBytes(out, p.data(), p.size());
        return out;
    }

    std::vector<uint8_t> Build()
    {
        const size_t headerSize = 8 + 64 * 16 + 4;
        std::vector<uint8_t> payload[64];
        uint32_t fourCC[64] = {};
        for (size_t i = 0; i < 64; ++i) {
            if (i == 35 || lumps[i].empty())
                continue;
            if (compressLump[i]) {
                payload[i] = LzmaLiteralEncoder::SourceBlob(lumps[i]);
                fourCC[i] = static_cast<uint32_t>(lumps[i].size());
            } else {
                payload[i] = lumps[i];
            }
        }
        // Game lump goes last so its absolute offsets are known when it is built.
        size_t cursor = headerSize;
        size_t offsets[64] = {};
        for (size_t i = 0; i < 64; ++i) {
            if (i == 35 || payload[i].empty())
                continue;
            offsets[i] = cursor;
            cursor += (payload[i].size() + 3) & ~size_t(3);
        }
        if (!gameLumps.empty()) {
            offsets[35] = cursor;
            std::vector<uint8_t> plain = BuildGameLump(cursor);
            if (compressLump[35]) {
                fourCC[35] = static_cast<uint32_t>(plain.size());
                payload[35] = LzmaLiteralEncoder::SourceBlob(plain);
            } else {
                payload[35] = std::move(plain);
            }
            cursor += (payload[35].size() + 3) & ~size_t(3);
        }

        std::vector<uint8_t> out;
        out.reserve(cursor);
        Put32(out, ('P' << 24) | ('S' << 16) | ('B' << 8) | 'V');
        Put32(out, version);
        for (size_t i = 0; i < 64; ++i) {
            const int32_t len = static_cast<int32_t>(payload[i].size());
            const int32_t ofs = static_cast<int32_t>(len ? offsets[i] : 0);
            const int32_t ver = static_cast<int32_t>(lumpVersions[i]);
            if (l4d2HeaderLayout) {
                Put32(out, ver);
                Put32(out, ofs);
                Put32(out, len);
            } else {
                Put32(out, ofs);
                Put32(out, len);
                Put32(out, ver);
            }
            Put32(out, static_cast<int32_t>(fourCC[i]));
        }
        Put32(out, 0); // map revision
        out.resize(cursor, 0);
        for (size_t i = 0; i < 64; ++i) {
            if (payload[i].empty())
                continue;
            std::memcpy(out.data() + offsets[i], payload[i].data(), payload[i].size());
        }
        return out;
    }
};

// A closed 512-unit room (6 faces) with one func_door brush model and two static props.
template <typename Configure>
inline std::vector<uint8_t> MakeRoomBspWith(Configure configure, BspBuilder* outBuilder = nullptr)
{
    BspBuilder b;
    configure(b);
    const int32_t concrete = b.AddTexture("CONCRETE/CONCRETEFLOOR001A");
    const int32_t sky = b.AddTexture("TOOLS/TOOLSSKYBOX", 0x4);
    const int32_t metal = b.AddTexture("METAL/METALDOOR001");
    const int32_t worldFirst = b.AddBox({-256, -256, 0}, {256, 256, 256}, concrete);
    b.AddFace({{-256, -256, 300}, {256, -256, 300}, {256, 256, 300}, {-256, 256, 300}}, sky);
    const int32_t worldCount = b.faceCount - worldFirst;
    b.AddModel(worldFirst, worldCount);
    const int32_t doorFirst = b.AddBox({-32, -4, 0}, {32, 4, 96}, metal);
    b.AddModel(doorFirst, 6);
    b.SetEntities("{\n\"classname\" \"worldspawn\"\n}\n"
                  "{\n\"classname\" \"func_door\"\n\"model\" \"*1\"\n\"origin\" \"100 0 0\"\n\"targetname\" \"door1\"\n}\n"
                  "{\n\"classname\" \"trigger_multiple\"\n\"model\" \"*1\"\n}\n");
    b.AddStaticProps({"models/props_c17/oildrum001.mdl", "models/props/crate.mdl"},
                     {{0, {10, 20, 0}}, {1, {-50, 0, 0}}});
    if (outBuilder)
        *outBuilder = b;
    return b.Build();
}

inline std::vector<uint8_t> MakeRoomBsp(BspBuilder* outBuilder = nullptr)
{
    return MakeRoomBspWith([](BspBuilder&) {}, outBuilder);
}

} // namespace satest
