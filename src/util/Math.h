// src/util/Math.h
//
// Small vector helpers and Source <-> Steam Audio coordinate conversion.
//
// Source uses a right-handed Z-up system measured in inches-ish "units"
// (Hammer units, 1 m ~= 52.49 units at the default player scale).
// Steam Audio uses a right-handed Y-up system with -Z forward, in meters.
//
//   Source  (x, y, z)  ->  Steam Audio  (x, z, -y) * metersPerUnit
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "steamaudio/PhononApi.h"

namespace sa {

struct Vec3 {
    float x = 0.f, y = 0.f, z = 0.f;

    constexpr Vec3() = default;
    constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3 operator/(float s) const { return {x / s, y / s, z / s}; }
    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    Vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
    Vec3 operator-() const { return {-x, -y, -z}; }

    float Dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    Vec3 Cross(const Vec3& o) const
    {
        return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
    }
    float LengthSq() const { return Dot(*this); }
    float Length() const { return std::sqrt(LengthSq()); }
    Vec3 Normalized() const
    {
        const float len = Length();
        return len > 1e-6f ? (*this) / len : Vec3{0.f, 0.f, 1.f};
    }
    bool IsFinite() const { return std::isfinite(x) && std::isfinite(y) && std::isfinite(z); }
    static float Distance(const Vec3& a, const Vec3& b) { return (a - b).Length(); }

    IPLVector3 ToIPL() const { return IPLVector3{x, y, z}; }
    static Vec3 FromIPL(const IPLVector3& v) { return {v.x, v.y, v.z}; }
};

// Row-major 3x3 rotation + translation, used for dynamic geometry.
struct Transform {
    // Columns are the Source-space basis vectors (forward, right, up) of the entity.
    Vec3 forward{1.f, 0.f, 0.f};
    Vec3 right{0.f, -1.f, 0.f};
    Vec3 up{0.f, 0.f, 1.f};
    Vec3 origin{};

    // Source AngleVectors(): angles are (pitch, yaw, roll) in degrees.
    static Transform FromAngles(const Vec3& origin, const Vec3& angles)
    {
        constexpr float kDeg = 3.14159265358979323846f / 180.f;
        const float sp = std::sin(angles.x * kDeg), cp = std::cos(angles.x * kDeg);
        const float sy = std::sin(angles.y * kDeg), cy = std::cos(angles.y * kDeg);
        const float sr = std::sin(angles.z * kDeg), cr = std::cos(angles.z * kDeg);
        Transform t;
        t.origin = origin;
        t.forward = Vec3{cp * cy, cp * sy, -sp};
        t.right = Vec3{-1.f * sr * sp * cy + -1.f * cr * -sy, -1.f * sr * sp * sy + -1.f * cr * cy, -1.f * sr * cp};
        t.up = Vec3{cr * sp * cy + -sr * -sy, cr * sp * sy + -sr * cy, cr * cp};
        return t;
    }
};

// Default Source unit scale: 1 unit = 0.01905 m (1 foot = 16 units).
constexpr float kDefaultUnitsPerMeter = 52.4934f;

struct CoordinateConverter {
    float metersPerUnit = 1.f / kDefaultUnitsPerMeter;

    void SetUnitsPerMeter(float unitsPerMeter)
    {
        metersPerUnit = unitsPerMeter > 1e-3f ? 1.f / unitsPerMeter : 1.f / kDefaultUnitsPerMeter;
    }

    // Position: scale + axis swap.
    Vec3 PositionToSA(const Vec3& src) const
    {
        return {src.x * metersPerUnit, src.z * metersPerUnit, -src.y * metersPerUnit};
    }
    Vec3 PositionToSource(const Vec3& sa) const
    {
        const float unitsPerMeter = 1.f / metersPerUnit;
        return {sa.x * unitsPerMeter, -sa.z * unitsPerMeter, sa.y * unitsPerMeter};
    }
    // Direction: axis swap only.
    static Vec3 DirectionToSA(const Vec3& src) { return {src.x, src.z, -src.y}; }
    static Vec3 DirectionToSource(const Vec3& sa) { return {sa.x, -sa.z, sa.y}; }

    float LengthToSA(float units) const { return units * metersPerUnit; }
    float LengthToSource(float meters) const { return meters / metersPerUnit; }

    // Builds a Steam Audio coordinate frame from Source-space listener vectors.
    IPLCoordinateSpace3 FrameToSA(const Vec3& origin, const Vec3& forward, const Vec3& up) const
    {
        IPLCoordinateSpace3 frame{};
        const Vec3 f = DirectionToSA(forward).Normalized();
        Vec3 u = DirectionToSA(up).Normalized();
        // Re-orthogonalize.
        Vec3 r = f.Cross(u);
        if (r.LengthSq() < 1e-6f) {
            r = f.Cross(Vec3{0.f, 1.f, 0.f});
            if (r.LengthSq() < 1e-6f)
                r = Vec3{1.f, 0.f, 0.f};
        }
        r = r.Normalized();
        u = r.Cross(f).Normalized();
        frame.ahead = f.ToIPL();
        frame.up = u.ToIPL();
        frame.right = r.ToIPL();
        frame.origin = PositionToSA(origin).ToIPL();
        return frame;
    }

    // 4x4 column-major-in-memory as Steam Audio expects (IPLMatrix4x4 is elements[row][col]).
    IPLMatrix4x4 TransformToSA(const Transform& t) const
    {
        // Source basis columns, converted into SA space.
        const Vec3 fx = DirectionToSA(t.forward);   // Source +X maps here
        const Vec3 fy = DirectionToSA(-t.right);    // Source +Y (left) maps here
        const Vec3 fz = DirectionToSA(t.up);        // Source +Z maps here
        const Vec3 o = PositionToSA(t.origin);

        // Input vertex (in SA space, built from Source model space via PositionToSA):
        //   vSA = (mx, mz, -my)  -> model x = vSA.x, y = -vSA.z, z = vSA.y
        // world = o + x*fx + y*fy + z*fz  (fx/fy/fz already in SA space)
        // => world = o + vSA.x*fx + (-vSA.z)*fy + vSA.y*fz
        IPLMatrix4x4 m{};
        m.elements[0][0] = fx.x; m.elements[0][1] = fz.x; m.elements[0][2] = -fy.x; m.elements[0][3] = o.x;
        m.elements[1][0] = fx.y; m.elements[1][1] = fz.y; m.elements[1][2] = -fy.y; m.elements[1][3] = o.y;
        m.elements[2][0] = fx.z; m.elements[2][1] = fz.z; m.elements[2][2] = -fy.z; m.elements[2][3] = o.z;
        m.elements[3][0] = 0.f;  m.elements[3][1] = 0.f;  m.elements[3][2] = 0.f;   m.elements[3][3] = 1.f;
        return m;
    }
};

inline float Clamp01(float v) { return std::min(1.f, std::max(0.f, v)); }

template <typename T>
inline T Clamp(T v, T lo, T hi) { return std::min(hi, std::max(lo, v)); }

inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }

inline float DbToLinear(float db) { return std::pow(10.f, db / 20.f); }
inline float LinearToDb(float lin) { return lin > 1e-9f ? 20.f * std::log10(lin) : -180.f; }

inline bool IsPowerOfTwo(size_t v) { return v != 0 && (v & (v - 1)) == 0; }

inline size_t NextPowerOfTwo(size_t v)
{
    if (v == 0)
        return 1;
    --v;
    for (size_t shift = 1; shift < sizeof(size_t) * 8; shift <<= 1)
        v |= v >> shift;
    return v + 1;
}

// Source engine "soundlevel" (dB SPL at 1 m-ish) to distance multiplier, as in
// SNDLVL_TO_DIST_MULT: dist_mult = 10^(60/20) / 10^(sndlvl/20) / 36.
inline float SoundLevelToDistMult(float sndlvl)
{
    if (sndlvl <= 0.f)
        return 0.f;
    return (std::pow(10.f, 60.f / 20.f) / std::pow(10.f, sndlvl / 20.f)) / 36.f;
}

inline float DistMultToSoundLevel(float distMult)
{
    if (distMult <= 0.f)
        return 0.f;
    // distMult = 10^(3 - sndlvl/20) / 36  =>  sndlvl = 20*(3 - log10(36*distMult))
    return 20.f * (3.f - std::log10(36.f * distMult));
}

// Reproduction of Source's SND_GetGainFromMult (snd_dma.cpp): gain falls off
// as 1/relative_distance once the source is farther than the reference
// distance, clamped by snd_gain_min/snd_gain_max.
inline float SourceDistanceGain(float distMult, float distUnits, float gainMin = 0.01f, float gainMax = 1.f)
{
    if (distMult <= 0.f)
        return 1.f;
    const float relative = distUnits * distMult;
    float gain = 1.f;
    if (relative > 0.1f)
        gain = 1.f / relative;
    else
        gain = 10.f;
    return Clamp(gain, gainMin, gainMax);
}

} // namespace sa
