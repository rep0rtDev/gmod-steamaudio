// src/steamaudio/BspGeometry.h
//
// Parser for Source engine BSP files (versions 19-21, as shipped with Garry's
// Mod maps) that extracts the acoustic geometry:
//
//   * worldspawn brush faces (model 0) and displacement surfaces,
//   * brush entity models ("*N") as separate local-space meshes so they can be
//     tracked as dynamic geometry at runtime,
//   * static prop placements (model name, origin, angles, solidity); their
//     collision meshes are resolved afterwards by StaticPropResolver from the
//     models' .phy/.mdl files and stored in `staticPropMesh`.
//
// The parser works on an in-memory copy of the file so it can run on the
// simulation thread without touching the engine's filesystem.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "steamaudio/SceneBuilder.h"
#include "util/Math.h"

namespace sa {

struct BspStaticProp {
    std::string model;   // "models/props_c17/oildrum001.mdl"
    Vec3 origin;         // Source units
    Vec3 angles;         // pitch, yaw, roll (degrees)
    uint8_t solid = 0;   // SOLID_* (0 none, 2 bbox, 6 vphysics)
    float uniformScale = 1.f;
};

struct BspBrushModel {
    int32_t modelIndex = 0; // the N in "*N"
    std::string classname;
    std::string targetname;
    Vec3 spawnOrigin;       // Source units, from the entity "origin" key
    MeshData mesh;          // local space (vertices relative to spawnOrigin), SA units
    bool solid = true;
};

struct BspGeometry {
    std::string mapName;
    int32_t version = 0;
    MeshData world;                       // worldspawn + displacements, SA units
    std::vector<BspBrushModel> brushModels;
    std::vector<BspStaticProp> staticProps;
    MeshData staticPropMesh;              // instanced prop collision geometry, SA units (filled after parsing)
    size_t staticPropsInstanced = 0;
    size_t staticPropsMissing = 0;        // distinct models without .phy/.mdl
    size_t skippedFaces = 0;
    size_t compressedLumps = 0; // LZMA lumps transparently decompressed (bspzip -repack)
    std::string error;
};

// Returns the Source $surfaceprop for a texture path ("concrete/concretefloor001a"),
// or an empty string when unknown. Engine layer implements it through
// IMaterialSystem; the parser falls back to name heuristics.
using SurfacePropResolver = std::function<std::string(const std::string& texture)>;

bool ParseBsp(const uint8_t* data, size_t size, const CoordinateConverter& converter,
              const MaterialLibrary& materials, const SurfacePropResolver& resolver, BspGeometry& out);

// Whether a brush entity class should be treated as acoustically solid geometry.
bool IsAcousticBrushClass(const std::string& classname);

} // namespace sa
