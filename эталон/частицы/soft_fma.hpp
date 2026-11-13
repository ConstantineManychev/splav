#pragma once

#include <bit>
#include <cstdint>
#include <cstring>

namespace soft_glsl
{
using uint = std::uint32_t;

struct uvec2
{
    uint x;
    uint y;

    uvec2() = default;

    constexpr uvec2(uint high, uint low)
        : x(high)
        , y(low)
    {
    }
};

constexpr uint SIGN_MASK = 0x80000000u;
constexpr uint MAGNITUDE_MASK = 0x7FFFFFFFu;
constexpr uint MANTISSA_MASK = 0x007FFFFFu;
constexpr uint HIDDEN_BIT = 0x00800000u;
constexpr uint QUIET_NAN = 0x7FC00000u;
constexpr uint INFINITY_BITS = 0x7F800000u;

inline uint floatBitsToUint(float value)
{
    uint bits;
    std::memcpy(&bits, &value, sizeof bits);
    return bits;
}

inline float uintBitsToFloat(uint bits)
{
    float value;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

inline int findMSB(uint value)
{
    return 31 - std::countl_zero(value);
}

inline uvec2 multiplyWide(uint left, uint right)
{
    std::uint64_t full = static_cast<std::uint64_t>(left) * right;
    return uvec2(static_cast<uint>(full >> 32), static_cast<uint>(full));
}

#include "soft_fma.glsl"
}
