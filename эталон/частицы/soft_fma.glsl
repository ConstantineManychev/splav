struct Unpacked
{
    int exponent;
    uint mantissa;
};

Unpacked unpackNormalized(uint magnitude)
{
    Unpacked result;
    result.exponent = int(magnitude >> 23u);
    result.mantissa = magnitude & MANTISSA_MASK;
    if (result.exponent == 0)
    {
        int shift = 23 - findMSB(result.mantissa);
        result.mantissa <<= uint(shift);
        result.exponent = 1 - shift;
    }
    else
    {
        result.mantissa |= HIDDEN_BIT;
    }
    return result;
}

uvec2 wideShiftLeftOne(uvec2 value)
{
    return uvec2((value.x << 1u) | (value.y >> 31u), value.y << 1u);
}

uvec2 wideShiftRightJam(uvec2 value, uint amount)
{
    if (amount == 0u)
    {
        return value;
    }
    if (amount < 32u)
    {
        uint lost = value.y << (32u - amount);
        uint low = (value.y >> amount) | (value.x << (32u - amount));
        return uvec2(value.x >> amount, low | (lost != 0u ? 1u : 0u));
    }
    if (amount == 32u)
    {
        return uvec2(0u, value.x | (value.y != 0u ? 1u : 0u));
    }
    if (amount < 64u)
    {
        uint inner = amount - 32u;
        uint lost = value.y | (value.x << (32u - inner));
        return uvec2(0u, (value.x >> inner) | (lost != 0u ? 1u : 0u));
    }
    return uvec2(0u, (value.x | value.y) != 0u ? 1u : 0u);
}

uvec2 wideAdd(uvec2 left, uvec2 right)
{
    uint low = left.y + right.y;
    uint carry = low < left.y ? 1u : 0u;
    return uvec2(left.x + right.x + carry, low);
}

uvec2 wideSubtract(uvec2 left, uvec2 right)
{
    uint borrow = left.y < right.y ? 1u : 0u;
    return uvec2(left.x - right.x - borrow, left.y - right.y);
}

int wideLeadingZeros(uvec2 value)
{
    if (value.x != 0u)
    {
        return 31 - findMSB(value.x);
    }
    return value.y != 0u ? 63 - findMSB(value.y) : 64;
}

uint narrowShiftRightJam(uint value, uint amount)
{
    if (amount == 0u)
    {
        return value;
    }
    if (amount < 31u)
    {
        return (value >> amount) | ((value << (32u - amount)) != 0u ? 1u : 0u);
    }
    return value != 0u ? 1u : 0u;
}

uint roundPackBits(uint signBit, int exponent, uint mantissa)
{
    uint roundBits = mantissa & 0x7Fu;
    if (exponent < 0)
    {
        mantissa = narrowShiftRightJam(mantissa, uint(-exponent));
        exponent = 0;
        roundBits = mantissa & 0x7Fu;
    }
    else if (exponent > 0xFD || (exponent == 0xFD && mantissa + 0x40u >= 0x80000000u))
    {
        return signBit | INFINITY_BITS;
    }
    mantissa = (mantissa + 0x40u) >> 7u;
    if (roundBits == 0x40u)
    {
        mantissa &= ~1u;
    }
    if (mantissa == 0u)
    {
        exponent = 0;
    }
    return signBit + (uint(exponent) << 23u) + mantissa;
}

uint softFmaBits(uint bitsA, uint bitsB, uint bitsC)
{
    uint signProduct = (bitsA ^ bitsB) & SIGN_MASK;
    uint signAddend = bitsC & SIGN_MASK;
    uint magnitudeA = bitsA & MAGNITUDE_MASK;
    uint magnitudeB = bitsB & MAGNITUDE_MASK;
    uint magnitudeC = bitsC & MAGNITUDE_MASK;
    if (magnitudeA > INFINITY_BITS || magnitudeB > INFINITY_BITS || magnitudeC > INFINITY_BITS)
    {
        return QUIET_NAN;
    }
    if (magnitudeA == INFINITY_BITS || magnitudeB == INFINITY_BITS)
    {
        bool invalid = magnitudeA == 0u || magnitudeB == 0u || (magnitudeC == INFINITY_BITS && signAddend != signProduct);
        return invalid ? QUIET_NAN : (signProduct | INFINITY_BITS);
    }
    if (magnitudeC == INFINITY_BITS)
    {
        return bitsC;
    }
    if (magnitudeA == 0u || magnitudeB == 0u)
    {
        return magnitudeC == 0u ? (signProduct & signAddend) : bitsC;
    }

    Unpacked a = unpackNormalized(magnitudeA);
    Unpacked b = unpackNormalized(magnitudeB);
    int exponentProduct = a.exponent + b.exponent - 126;
    uvec2 product = multiplyWide(a.mantissa << 7u, b.mantissa << 7u);
    if (product.x < 0x20000000u)
    {
        exponentProduct -= 1;
        product = wideShiftLeftOne(product);
    }
    if (magnitudeC == 0u)
    {
        return roundPackBits(signProduct, exponentProduct - 1, wideShiftRightJam(product, 31u).y);
    }

    Unpacked c = unpackNormalized(magnitudeC);
    uint mantissaC = c.mantissa << 6u;
    int exponentGap = exponentProduct - c.exponent;
    if (signProduct == signAddend)
    {
        int exponentSum = exponentProduct;
        uint mantissaSum;
        if (exponentGap <= 0)
        {
            exponentSum = c.exponent;
            mantissaSum = mantissaC + wideShiftRightJam(product, uint(32 - exponentGap)).y;
        }
        else
        {
            uvec2 sum = wideAdd(product, wideShiftRightJam(uvec2(mantissaC, 0u), uint(exponentGap)));
            mantissaSum = wideShiftRightJam(sum, 32u).y;
        }
        if (mantissaSum < 0x40000000u)
        {
            exponentSum -= 1;
            mantissaSum <<= 1u;
        }
        return roundPackBits(signProduct, exponentSum, mantissaSum);
    }

    uint signDifference = signProduct;
    int exponentDifference = exponentProduct;
    uvec2 wideC = uvec2(mantissaC, 0u);
    uvec2 difference;
    if (exponentGap < 0)
    {
        signDifference = signAddend;
        exponentDifference = c.exponent;
        difference = wideSubtract(wideC, wideShiftRightJam(product, uint(-exponentGap)));
    }
    else if (exponentGap == 0)
    {
        difference = wideSubtract(product, wideC);
        if (difference.x == 0u && difference.y == 0u)
        {
            return 0u;
        }
        if ((difference.x & SIGN_MASK) != 0u)
        {
            signDifference ^= SIGN_MASK;
            difference = wideSubtract(uvec2(0u, 0u), difference);
        }
    }
    else
    {
        difference = wideSubtract(product, wideShiftRightJam(wideC, uint(exponentGap)));
    }
    int shift = wideLeadingZeros(difference) - 1;
    exponentDifference -= shift;
    shift -= 32;
    uint mantissaDifference = shift < 0 ? wideShiftRightJam(difference, uint(-shift)).y : difference.y << uint(shift);
    return roundPackBits(signDifference, exponentDifference, mantissaDifference);
}

float softFmaExact(float multiplicand, float multiplier, float addend)
{
    return uintBitsToFloat(softFmaBits(floatBitsToUint(multiplicand), floatBitsToUint(multiplier), floatBitsToUint(addend)));
}
