// tests/PhyFixture.h
//
// Builds synthetic studiomdl-style .phy (VPHY / IVP compact surface) and .mdl
// header blobs in memory for the PhyModel and StaticPropResolver tests.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace satest {

struct PhyBuilder {
    struct Vec {
        float x, y, z;
    };

    // One convex piece: Source-space (Hammer units, Z-up) points and triangles.
    struct Ledge {
        std::vector<Vec> points;
        std::vector<uint32_t> triangles; // 3 indices per triangle
        std::vector<uint8_t> materials;  // per triangle, may be empty (=> 0)
    };

    struct Solid {
        std::vector<Ledge> ledges;
        int16_t modelType = 0;          // 0 = compact surface, 1 = MOPP
        bool writeIvps = true;
        bool writeLedgeTree = true;     // false => offset_ledgetree_root = 0 (forces linear recovery)
        bool bogusVphyId = false;
    };

    std::vector<Solid> solids;
    std::string text; // key-value tail

    static void Put32(std::vector<uint8_t>& v, int32_t x)
    {
        const size_t o = v.size();
        v.resize(o + 4);
        std::memcpy(v.data() + o, &x, 4);
    }
    static void PutU32(std::vector<uint8_t>& v, uint32_t x) { Put32(v, static_cast<int32_t>(x)); }
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
    static void Patch32(std::vector<uint8_t>& v, size_t offset, int32_t x) { std::memcpy(v.data() + offset, &x, 4); }

    static constexpr float kHlToIvp = 0.0254f;

    // Source (x, y, z) -> IVP (x, -z, y) in meters (inverse of ConvertPositionToHL).
    static void PutIvpPoint(std::vector<uint8_t>& v, Vec hl)
    {
        PutF(v, hl.x * kHlToIvp);
        PutF(v, -hl.z * kHlToIvp);
        PutF(v, hl.y * kHlToIvp);
        PutF(v, 0.f); // hesse value
    }

    static Ledge Box(Vec mins, Vec maxs, uint8_t material = 0)
    {
        Ledge l;
        for (int i = 0; i < 8; ++i)
            l.points.push_back({(i & 1) ? maxs.x : mins.x, (i & 2) ? maxs.y : mins.y, (i & 4) ? maxs.z : mins.z});
        static const uint32_t quads[6][4] = {{0, 4, 6, 2}, {1, 3, 7, 5}, {0, 1, 5, 4},
                                             {2, 6, 7, 3}, {0, 2, 3, 1}, {4, 5, 7, 6}};
        for (const auto& q : quads) {
            l.triangles.insert(l.triangles.end(), {q[0], q[1], q[2], q[0], q[2], q[3]});
            l.materials.push_back(material);
            l.materials.push_back(material);
        }
        return l;
    }

    static std::vector<uint8_t> EncodeLedge(const Ledge& ledge)
    {
        std::vector<uint8_t> v;
        const size_t triangles = ledge.triangles.size() / 3;
        const size_t size = 16 + triangles * 16 + ledge.points.size() * 16;
        Put32(v, static_cast<int32_t>(16 + triangles * 16)); // c_point_offset
        Put32(v, 0);                                          // ledgetree node offset / client data
        PutU32(v, static_cast<uint32_t>((size / 16) << 8) | 0x4u); // size_div_16 | is_compact_flag
        Put16(v, static_cast<int16_t>(triangles));
        Put16(v, 0);
        for (size_t t = 0; t < triangles; ++t) {
            const uint32_t material = t < ledge.materials.size() ? ledge.materials[t] : 0;
            PutU32(v, static_cast<uint32_t>(t & 0xFFF) | ((material & 0x7F) << 24));
            for (int e = 0; e < 3; ++e) {
                const uint32_t start = ledge.triangles[t * 3 + static_cast<size_t>(e)] & 0xFFFF;
                PutU32(v, start);
            }
        }
        for (const Vec& p : ledge.points)
            PutIvpPoint(v, p);
        return v;
    }

    // IVP_Compact_Surface + ledges + ledge tree; returns the surface bytes.
    static std::vector<uint8_t> EncodeSurface(const Solid& solid)
    {
        std::vector<uint8_t> v;
        for (int i = 0; i < 6; ++i)
            PutF(v, 0.f);        // mass center, rotation inertia
        PutF(v, 1.f);            // upper limit radius
        PutU32(v, 0);            // max deviation | byte size (patched)
        Put32(v, 0);             // offset_ledgetree_root (patched)
        Put32(v, 0);             // dummy[0]
        Put32(v, 0);             // dummy[1]
        Put32(v, solid.writeIvps ? (('I' << 0) | ('V' << 8) | ('P' << 16) | ('S' << 24)) : 0);

        std::vector<size_t> ledgeOffsets;
        for (const Ledge& l : solid.ledges) {
            ledgeOffsets.push_back(v.size());
            const std::vector<uint8_t> bytes = EncodeLedge(l);
            v.insert(v.end(), bytes.begin(), bytes.end());
        }

        if (solid.writeLedgeTree && !ledgeOffsets.empty()) {
            // Right-leaning tree: node i has left child = leaf(i) at +32 and
            // right child = node i+1 (or leaf for the last two).
            const size_t root = v.size();
            std::vector<size_t> nodeOffsets;
            auto writeNode = [&](int32_t rightOffset, int32_t ledgeOffset) {
                nodeOffsets.push_back(v.size());
                Put32(v, rightOffset);
                Put32(v, ledgeOffset);
                for (int i = 0; i < 4; ++i)
                    PutF(v, 0.f);
                Put32(v, 0);
            };
            auto writeLeaf = [&](size_t ledge) {
                const size_t node = v.size();
                writeNode(0, static_cast<int32_t>(static_cast<int64_t>(ledge) - static_cast<int64_t>(node)));
            };
            if (ledgeOffsets.size() == 1) {
                writeLeaf(ledgeOffsets[0]);
            } else {
                for (size_t i = 0; i + 1 < ledgeOffsets.size(); ++i) {
                    // right child sits after the left leaf: +32 (this node) +32 (left leaf)
                    writeNode(64, 0);
                    writeLeaf(ledgeOffsets[i]);
                    if (i + 2 == ledgeOffsets.size())
                        writeLeaf(ledgeOffsets[i + 1]);
                }
            }
            Patch32(v, 36, static_cast<int32_t>(root));
        }
        Patch32(v, 28, static_cast<int32_t>((v.size() & 0xFFFFFF) << 8));
        return v;
    }

    std::vector<uint8_t> Build() const
    {
        std::vector<uint8_t> v;
        Put32(v, 16);
        Put32(v, 0);
        Put32(v, static_cast<int32_t>(solids.size()));
        Put32(v, 0x1234);
        for (const Solid& s : solids) {
            const std::vector<uint8_t> surface = EncodeSurface(s);
            const size_t header = v.size();
            Put32(v, 0); // size (patched)
            Put32(v, s.bogusVphyId ? 0x58585858 : (('V' << 0) | ('P' << 8) | ('H' << 16) | ('Y' << 24)));
            Put16(v, 0x100);
            Put16(v, s.modelType);
            Put32(v, static_cast<int32_t>(surface.size()));
            PutF(v, 0.f);
            PutF(v, 0.f);
            PutF(v, 0.f);
            Put32(v, 0);
            v.insert(v.end(), surface.begin(), surface.end());
            Patch32(v, header, static_cast<int32_t>(v.size() - header - 4));
        }
        v.insert(v.end(), text.begin(), text.end());
        return v;
    }
};

// Minimal studiohdr_t: IDST magic, name, hull box and $surfaceprop string.
inline std::vector<uint8_t> BuildMdl(const std::string& name, PhyBuilder::Vec hullMin, PhyBuilder::Vec hullMax,
                                     const std::string& surfaceProp)
{
    std::vector<uint8_t> v(400, 0);
    const int32_t idst = ('I' << 0) | ('D' << 8) | ('S' << 16) | ('T' << 24);
    std::memcpy(v.data(), &idst, 4);
    const int32_t version = 48;
    std::memcpy(v.data() + 4, &version, 4);
    std::memcpy(v.data() + 12, name.c_str(), std::min<size_t>(name.size(), 63));
    const int32_t length = static_cast<int32_t>(v.size());
    std::memcpy(v.data() + 76, &length, 4);
    std::memcpy(v.data() + 104, &hullMin, 12);
    std::memcpy(v.data() + 116, &hullMax, 12);
    const int32_t surfacePropIndex = 340;
    std::memcpy(v.data() + 308, &surfacePropIndex, 4);
    std::memcpy(v.data() + 340, surfaceProp.c_str(), std::min<size_t>(surfaceProp.size(), 59));
    return v;
}

} // namespace satest
