// src/steamaudio/StaticPropResolver.h
//
// Turns the static prop placements found in a BSP into acoustic geometry.
// For every distinct model the collision mesh (.phy) or, failing that, the
// studio hull box (.mdl) is loaded once through the supplied game-file reader
// (engine filesystem / loose files / .gma), then instanced at each placement
// with the prop's origin, angles and uniform scale, and converted to Steam
// Audio space. Materials come from the .phy/.mdl $surfaceprop through the
// MaterialLibrary.
//
// Thread-safety: pure function of its inputs; the reader callback decides
// which thread it may run on.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "steamaudio/BspGeometry.h"
#include "steamaudio/PhyModel.h"
#include "steamaudio/SceneBuilder.h"
#include "util/Math.h"

namespace sa {

// Reads a game-relative file ("models/props/x.phy"). Returns false when missing.
using GameFileReader = std::function<bool(const std::string& path, std::vector<uint8_t>& out, std::string& error)>;

struct StaticPropOptions {
    bool enabled = true;
    bool includeNonSolid = false;        // props flagged SOLID_NONE (foliage, decals) are skipped by default
    bool boxFallback = true;             // use the .mdl hull box when no usable .phy exists
    size_t maxTriangles = 2u << 20;      // hard cap on emitted triangles (whole map)
    size_t maxModelTriangles = 65536;    // per-model cap; larger collision models are box-approximated
    float minBoxExtentUnits = 4.f;       // skip boxes thinner than this in every axis
};

struct StaticPropStats {
    size_t placements = 0;        // props considered
    size_t instanced = 0;         // props that produced geometry
    size_t skippedNonSolid = 0;
    size_t fromPhy = 0;           // placements using a collision mesh
    size_t fromBox = 0;           // placements using the hull box
    size_t missingModels = 0;     // distinct models with neither .phy nor .mdl
    size_t distinctModels = 0;
    size_t triangles = 0;
    std::vector<std::string> missing; // first few missing model paths (diagnostics)
};

// Appends all static props to `out` (Steam Audio space). Never fails: props
// whose models cannot be read are counted in `stats` and skipped.
void ResolveStaticProps(const std::vector<BspStaticProp>& props, const GameFileReader& reader,
                        const CoordinateConverter& converter, const MaterialLibrary& materials,
                        const StaticPropOptions& options, MeshData& out, StaticPropStats& stats);

// Model-space geometry for one model, cached by ResolveStaticProps. Exposed
// for tests and for dynamic props (B9) which reuse the same loader.
struct PropModelGeometry {
    std::vector<Vec3> vertices;          // Source model space, units
    std::vector<PhyTriangle> triangles;
    std::vector<std::string> materialNames; // index 0 = model surfaceprop, then material table
    bool fromCollision = false;
    bool valid = false;
    std::string error;
};

// Loads models/<name>.phy (falling back to the .mdl hull) into model space.
bool LoadPropModelGeometry(const std::string& modelPath, const GameFileReader& reader,
                           const StaticPropOptions& options, PropModelGeometry& out);

// Source static prop transform: model space -> world space (Source units).
Vec3 TransformPropVertex(const Vec3& modelVertex, const Transform& t, float uniformScale);

} // namespace sa
