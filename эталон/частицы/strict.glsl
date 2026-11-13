#ifdef DENORM_PRESERVE
spirv_execution_mode(extensions = ["SPV_KHR_float_controls"], capabilities = [4464], 4459, 32);
#endif

const uint SIGN_MASK = 0x80000000u;
const uint MAGNITUDE_MASK = 0x7FFFFFFFu;
const uint MANTISSA_MASK = 0x007FFFFFu;
const uint HIDDEN_BIT = 0x00800000u;
const uint LARGEST_MANTISSA = 0x00FFFFFFu;
const uint QUIET_NAN = 0x7FC00000u;
const uint INFINITY_BITS = 0x7F800000u;

uvec2 multiplyWide(uint left, uint right)
{
    uint high;
    uint low;
    umulExtended(left, right, high, low);
    return uvec2(high, low);
}

void unpackFinite(uint bits, out int exponent, out uint mantissa)
{
    exponent = int((bits >> 23u) & 0xFFu);
    mantissa = bits & MANTISSA_MASK;
    if (exponent == 0)
    {
        int shift = 23 - findMSB(mantissa);
        mantissa <<= uint(shift);
        exponent = 1 - shift;
    }
    else
    {
        mantissa |= HIDDEN_BIT;
    }
}

bool quotientRoundsUp(uint candidate, uint divisor, uint targetHi, uint targetLo)
{
    uint productHi;
    uint productLo;
    umulExtended(2u * candidate + 1u, divisor, productHi, productLo);
    if (productHi != targetHi)
    {
        return productHi < targetHi;
    }
    if (productLo != targetLo)
    {
        return productLo < targetLo;
    }
    return (candidate & 1u) != 0u;
}

uint roundQuotient(uint candidate, uint lowest, uint divisor, uint targetHi, uint targetLo)
{
    while (quotientRoundsUp(candidate, divisor, targetHi, targetLo))
    {
        ++candidate;
    }
    while (candidate > lowest && !quotientRoundsUp(candidate - 1u, divisor, targetHi, targetLo))
    {
        --candidate;
    }
    return candidate;
}

float softDivExact(float numerator, float denominator)
{
    uint bitsA = floatBitsToUint(numerator);
    uint bitsB = floatBitsToUint(denominator);
    uint sign = (bitsA ^ bitsB) & SIGN_MASK;
    uint magnitudeA = bitsA & MAGNITUDE_MASK;
    uint magnitudeB = bitsB & MAGNITUDE_MASK;
    if (magnitudeA > INFINITY_BITS || magnitudeB > INFINITY_BITS)
    {
        return uintBitsToFloat(QUIET_NAN);
    }
    if (magnitudeA == INFINITY_BITS)
    {
        return uintBitsToFloat(magnitudeB == INFINITY_BITS ? QUIET_NAN : (sign | INFINITY_BITS));
    }
    if (magnitudeB == INFINITY_BITS)
    {
        return uintBitsToFloat(sign);
    }
    if (magnitudeB == 0u)
    {
        return uintBitsToFloat(magnitudeA == 0u ? QUIET_NAN : (sign | INFINITY_BITS));
    }
    if (magnitudeA == 0u)
    {
        return uintBitsToFloat(sign);
    }

    int exponentA;
    uint mantissaA;
    unpackFinite(magnitudeA, exponentA, mantissaA);
    int exponentB;
    uint mantissaB;
    unpackFinite(magnitudeB, exponentB, mantissaB);

    int exponent = exponentA - exponentB + 127;
    if (mantissaA < mantissaB)
    {
        mantissaA <<= 1u;
        exponent -= 1;
    }
    float ratio = float(mantissaA) / float(mantissaB);

    if (exponent >= 1)
    {
        uint seed = clamp(uint(ratio * 8388608.0), HIDDEN_BIT, LARGEST_MANTISSA);
        uint mantissa = roundQuotient(seed, HIDDEN_BIT, mantissaB, mantissaA >> 8u, mantissaA << 24u);
        if (mantissa > LARGEST_MANTISSA)
        {
            mantissa >>= 1u;
            exponent += 1;
        }
        if (exponent >= 255)
        {
            return uintBitsToFloat(sign | INFINITY_BITS);
        }
        return uintBitsToFloat(sign | (uint(exponent) << 23u) | (mantissa & MANTISSA_MASK));
    }

    int shift = exponent + 22;
    if (shift < -1)
    {
        return uintBitsToFloat(sign);
    }
    uint scale = uint(shift + 1);
    uint targetHi = scale == 0u ? 0u : (mantissaA >> (32u - scale));
    uint seed = min(uint(ldexp(ratio, shift)), HIDDEN_BIT);
    uint subnormal = roundQuotient(seed, 0u, mantissaB, targetHi, mantissaA << scale);
    return uintBitsToFloat(sign | subnormal);
}

bool rootRoundsUp(uint candidate, uint targetHi, uint targetLo)
{
    uint odd = 2u * candidate + 1u;
    uint productHi;
    uint productLo;
    umulExtended(odd, odd, productHi, productLo);
    return productHi < targetHi || (productHi == targetHi && productLo < targetLo);
}

float softSqrtExact(float value)
{
    uint bits = floatBitsToUint(value);
    if ((bits & MAGNITUDE_MASK) == 0u || bits == INFINITY_BITS)
    {
        return uintBitsToFloat(bits);
    }
    if (bits > INFINITY_BITS)
    {
        return uintBitsToFloat(QUIET_NAN);
    }

    int exponent;
    uint mantissa;
    unpackFinite(bits, exponent, mantissa);
    exponent -= 127;
    if ((exponent & 1) != 0)
    {
        mantissa <<= 1u;
        exponent -= 1;
    }

    uint candidate = clamp(uint(sqrt(float(mantissa) * 8388608.0)), HIDDEN_BIT, LARGEST_MANTISSA);
    uint targetHi = mantissa >> 7u;
    uint targetLo = mantissa << 25u;
    while (rootRoundsUp(candidate, targetHi, targetLo))
    {
        ++candidate;
    }
    while (candidate > HIDDEN_BIT && !rootRoundsUp(candidate - 1u, targetHi, targetLo))
    {
        --candidate;
    }

    int resultExponent = (exponent >> 1) + 127;
    if (candidate > LARGEST_MANTISSA)
    {
        candidate >>= 1u;
        resultExponent += 1;
    }
    return uintBitsToFloat((uint(resultExponent) << 23u) | (candidate & MANTISSA_MASK));
}
