#pragma once

#include "model.hpp"

#include <immintrin.h>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#define AVX2_TARGET
#else
#include <cpuid.h>
#define AVX2_TARGET __attribute__((target("avx2")))
#endif

inline bool cpuHasAvx2()
{
#if defined(_MSC_VER) && !defined(__clang__)
    int registers[4] = {};
    __cpuid(registers, 0);
    if (registers[0] < 7)
    {
        return false;
    }
    __cpuid(registers, 1);
    bool osSavesAvx = (registers[2] & (1 << 27)) != 0 && (registers[2] & (1 << 28)) != 0;
    __cpuidex(registers, 7, 0);
    return osSavesAvx && (registers[1] & (1 << 5)) != 0;
#else
    unsigned eax = 0;
    unsigned ebx = 0;
    unsigned ecx = 0;
    unsigned edx = 0;
    if (__get_cpuid_max(0, nullptr) < 7 || !__get_cpuid(1, &eax, &ebx, &ecx, &edx))
    {
        return false;
    }
    bool osSavesAvx = (ecx & (1u << 27)) != 0 && (ecx & (1u << 28)) != 0;
    __cpuid_count(7, 0, eax, ebx, ecx, edx);
    return osSavesAvx && (ebx & (1u << 5)) != 0;
#endif
}

struct WideWorld
{
    __m256 cellSize;
    __m256i lastCell;
    __m256i side;
    __m256 radius;
    __m256 top;
    __m256 stiffness;
    __m256 diameter;
    __m256 limit;
    __m256 gravity;
    __m256 timeStep;
};

AVX2_TARGET inline WideWorld widen(const World& world)
{
    float radius = world.diameter * 0.5f;
    float top = world.size - radius;
    return WideWorld{
        _mm256_set1_ps(world.cellSize),
        _mm256_set1_epi32(static_cast<int32_t>(world.cellsPerSide) - 1),
        _mm256_set1_epi32(static_cast<int32_t>(world.cellsPerSide)),
        _mm256_set1_ps(radius),
        _mm256_set1_ps(top),
        _mm256_set1_ps(world.stiffness),
        _mm256_set1_ps(world.diameter),
        _mm256_set1_ps(world.diameter * world.diameter),
        _mm256_set1_ps(world.gravity),
        _mm256_set1_ps(world.timeStep),
    };
}

AVX2_TARGET inline __m256i wideCellCoordinate(__m256 coordinate, const WideWorld& wide)
{
    __m256 ratio = _mm256_div_ps(coordinate, wide.cellSize);
    __m256i raw = _mm256_cvttps_epi32(_mm256_floor_ps(ratio));
    return _mm256_min_epi32(_mm256_max_epi32(raw, _mm256_setzero_si256()), wide.lastCell);
}

AVX2_TARGET inline __m256 wideWall(__m256 coordinate, const WideWorld& wide)
{
    __m256 below = _mm256_cmp_ps(coordinate, wide.radius, _CMP_LT_OQ);
    __m256 above = _mm256_cmp_ps(coordinate, wide.top, _CMP_GT_OQ);
    __m256 inner = _mm256_mul_ps(wide.stiffness, _mm256_sub_ps(wide.radius, coordinate));
    __m256 outer = _mm256_mul_ps(wide.stiffness, _mm256_sub_ps(wide.top, coordinate));
    return _mm256_blendv_ps(_mm256_blendv_ps(_mm256_setzero_ps(), outer, above), inner, below);
}

AVX2_TARGET inline __m256i insideMask(__m256i coordinate, const WideWorld& wide)
{
    __m256i notNegative = _mm256_cmpgt_epi32(coordinate, _mm256_set1_epi32(-1));
    __m256i belowSide = _mm256_cmpgt_epi32(wide.side, coordinate);
    return _mm256_and_si256(notNegative, belowSide);
}

AVX2_TARGET inline void updateEightAvx2(size_t base, const Columns& sorted, const uint32_t* starts, const WideWorld& wide,
    Columns& out)
{
    __m256 x = _mm256_loadu_ps(&sorted.px[base]);
    __m256 y = _mm256_loadu_ps(&sorted.py[base]);
    __m256 z = _mm256_loadu_ps(&sorted.pz[base]);
    __m256i cellX = wideCellCoordinate(x, wide);
    __m256i cellY = wideCellCoordinate(y, wide);
    __m256i cellZ = wideCellCoordinate(z, wide);
    __m256 zero = _mm256_setzero_ps();
    __m256 ax = _mm256_sub_ps(wideWall(x, wide), zero);
    __m256 ay = _mm256_sub_ps(wideWall(y, wide), zero);
    __m256 az = _mm256_sub_ps(wideWall(z, wide), wide.gravity);
    __m256i own = _mm256_add_epi32(_mm256_set1_epi32(static_cast<int32_t>(base)), _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7));
    const int* startWords = reinterpret_cast<const int*>(starts);

    for (int32_t neighbour = 0; neighbour < 27; ++neighbour)
    {
        __m256i nearX = _mm256_add_epi32(cellX, _mm256_set1_epi32(neighbour % 3 - 1));
        __m256i nearY = _mm256_add_epi32(cellY, _mm256_set1_epi32(neighbour / 3 % 3 - 1));
        __m256i nearZ = _mm256_add_epi32(cellZ, _mm256_set1_epi32(neighbour / 9 - 1));
        __m256i inside = _mm256_and_si256(insideMask(nearX, wide), _mm256_and_si256(insideMask(nearY, wide), insideMask(nearZ, wide)));
        if (_mm256_testz_si256(inside, inside))
        {
            continue;
        }
        __m256i flat = _mm256_add_epi32(_mm256_mullo_epi32(_mm256_add_epi32(_mm256_mullo_epi32(nearZ, wide.side), nearY), wide.side), nearX);
        __m256i other = _mm256_mask_i32gather_epi32(_mm256_setzero_si256(), startWords, flat, inside, 4);
        __m256i last = _mm256_mask_i32gather_epi32(_mm256_setzero_si256(), startWords, _mm256_add_epi32(flat, _mm256_set1_epi32(1)),
            inside, 4);
        for (;;)
        {
            __m256i active = _mm256_cmpgt_epi32(last, other);
            if (_mm256_testz_si256(active, active))
            {
                break;
            }
            __m256 activeMask = _mm256_castsi256_ps(active);
            __m256 otherX = _mm256_mask_i32gather_ps(zero, sorted.px.data(), other, activeMask, 4);
            __m256 otherY = _mm256_mask_i32gather_ps(zero, sorted.py.data(), other, activeMask, 4);
            __m256 otherZ = _mm256_mask_i32gather_ps(zero, sorted.pz.data(), other, activeMask, 4);
            __m256 dx = _mm256_sub_ps(x, otherX);
            __m256 dy = _mm256_sub_ps(y, otherY);
            __m256 dz = _mm256_sub_ps(z, otherZ);
            __m256 square = _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(dx, dx), _mm256_mul_ps(dy, dy)), _mm256_mul_ps(dz, dz));
            __m256 outOfRange = _mm256_or_ps(_mm256_cmp_ps(square, wide.limit, _CMP_GE_OQ), _mm256_cmp_ps(square, zero, _CMP_EQ_OQ));
            __m256 distance = _mm256_sqrt_ps(square);
            __m256 factor = _mm256_div_ps(_mm256_mul_ps(wide.stiffness, _mm256_sub_ps(wide.diameter, distance)), distance);
            __m256 fx = _mm256_blendv_ps(_mm256_mul_ps(dx, factor), zero, outOfRange);
            __m256 fy = _mm256_blendv_ps(_mm256_mul_ps(dy, factor), zero, outOfRange);
            __m256 fz = _mm256_blendv_ps(_mm256_mul_ps(dz, factor), zero, outOfRange);
            __m256 counted = _mm256_andnot_ps(_mm256_castsi256_ps(_mm256_cmpeq_epi32(other, own)), activeMask);
            ax = _mm256_blendv_ps(ax, _mm256_add_ps(ax, fx), counted);
            ay = _mm256_blendv_ps(ay, _mm256_add_ps(ay, fy), counted);
            az = _mm256_blendv_ps(az, _mm256_add_ps(az, fz), counted);
            other = _mm256_sub_epi32(other, active);
        }
    }

    __m256 vx = _mm256_add_ps(_mm256_loadu_ps(&sorted.vx[base]), _mm256_mul_ps(ax, wide.timeStep));
    __m256 vy = _mm256_add_ps(_mm256_loadu_ps(&sorted.vy[base]), _mm256_mul_ps(ay, wide.timeStep));
    __m256 vz = _mm256_add_ps(_mm256_loadu_ps(&sorted.vz[base]), _mm256_mul_ps(az, wide.timeStep));
    _mm256_storeu_ps(&out.px[base], _mm256_add_ps(x, _mm256_mul_ps(vx, wide.timeStep)));
    _mm256_storeu_ps(&out.py[base], _mm256_add_ps(y, _mm256_mul_ps(vy, wide.timeStep)));
    _mm256_storeu_ps(&out.pz[base], _mm256_add_ps(z, _mm256_mul_ps(vz, wide.timeStep)));
    _mm256_storeu_ps(&out.vx[base], vx);
    _mm256_storeu_ps(&out.vy[base], vy);
    _mm256_storeu_ps(&out.vz[base], vz);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(&out.id[base]), _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&sorted.id[base])));
}

AVX2_TARGET inline void updateRangeAvx2(size_t begin, size_t end, const Columns& sorted, const uint32_t* starts, const World& world,
    Columns& out)
{
    WideWorld wide = widen(world);
    size_t index = begin;
    for (; index + 8 <= end; index += 8)
    {
        updateEightAvx2(index, sorted, starts, wide, out);
    }
    for (; index < end; ++index)
    {
        out.store(index, updateParticle(index, sorted, starts, world));
    }
}

inline void stepAvx2(const World& world, const Columns& sorted, const std::vector<uint32_t>& starts, Columns& out, unsigned threads)
{
    if (out.size() != sorted.size())
    {
        out.resize(sorted.size());
    }
    parallelFor(sorted.size(), threads, [&](size_t begin, size_t end)
    {
        updateRangeAvx2(begin, end, sorted, starts.data(), world, out);
    });
}
