// src/steamaudio/PhyModel.h
//
// Readers for the model-side files a static prop needs to become acoustic
// geometry:
//
//   * models/x.phy  — VPHY collision model written by studiomdl: one or more
//     Ipion (IVP) compact surfaces (convex "ledges" of triangles) followed by a
//     key-value text section with the per-solid $surfaceprop and an optional
//     material table;
//   * models/x.mdl  — only the studiohdr_t hull bounding box and $surfaceprop
//     are read, used when a prop has no collision model (SOLID_BBOX props or a
//     missing .phy).
//
// All output is in Source model space (Hammer units, Z-up); the caller places
// the model in the world and converts to Steam Audio space. Every read is
// bounds-checked against the buffer so hostile/corrupt Workshop content cannot
// crash the client.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "util/Math.h"

namespace sa {

struct PhyTriangle {
    uint32_t indices[3];    // into PhySolid::vertices
    uint8_t materialIndex;  // 0 => solid surfaceprop, otherwise PhyModel::materialTable[index - 1]
};

struct PhySolid {
    std::vector<Vec3> vertices;      // Source model space, units
    std::vector<PhyTriangle> triangles;
    std::string surfaceProp;         // from the text section; may be empty
    size_t ledges = 0;               // convex pieces
};

struct PhyModel {
    int32_t solidCount = 0;
    int32_t checksum = 0;
    std::vector<PhySolid> solids;
    std::vector<std::string> materialTable; // 1-based in PhyTriangle::materialIndex
    size_t skippedSolids = 0;               // MOPP / unknown model types
    std::string error;

    size_t TriangleCount() const;
};

// Parses a complete .phy file. Returns false only when nothing usable was
// found; partial results (some solids skipped) return true with `skippedSolids`
// set and `error` describing the last problem.
bool ParsePhy(const uint8_t* data, size_t size, PhyModel& out);

struct MdlInfo {
    int32_t version = 0;
    int32_t checksum = 0;
    std::string name;
    Vec3 hullMin;
    Vec3 hullMax;
    std::string surfaceProp;
    bool hasHull = false;   // false when hull_min == hull_max
};

// Reads the studiohdr_t fields listed above from a .mdl file.
bool ParseMdlInfo(const uint8_t* data, size_t size, MdlInfo& out);

// Appends the 12 triangles of an axis-aligned box to `vertices`/`triangles`.
void AppendBox(const Vec3& mins, const Vec3& maxs, std::vector<Vec3>& vertices, std::vector<PhyTriangle>& triangles,
               uint8_t materialIndex = 0);

} // namespace sa
