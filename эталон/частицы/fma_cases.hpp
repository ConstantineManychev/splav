#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

struct FmaTriple
{
    uint32_t multiplicand;
    uint32_t multiplier;
    uint32_t addend;
};

enum FmaGroup : uint32_t
{
    FMA_ORDINARY,
    FMA_CANCELLATION,
    FMA_HALFWAY,
    FMA_SUBNORMAL,
    FMA_OVERFLOW,
    FMA_SPECIAL,
    FMA_GROUPS,
};

inline const char* const FMA_GROUP_NAMES[FMA_GROUPS] = {
    "обычные",
    "сокращение",
    "ровно посередине",
    "денормализованные",
    "у переполнения",
    "особые значения",
};

inline float fmaFloatOf(uint32_t bits)
{
    float value;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

inline uint32_t fmaBitsOf(float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof bits);
    return bits;
}

inline uint64_t fmaBitsOf(double value)
{
    uint64_t bits;
    std::memcpy(&bits, &value, sizeof bits);
    return bits;
}

inline bool fmaIsNan(uint32_t bits)
{
    return (bits & 0x7FFFFFFFu) > 0x7F800000u;
}

inline bool fmaSameResult(uint32_t got, uint32_t want)
{
    return got == want || (fmaIsNan(got) && fmaIsNan(want));
}

struct FmaExactSum
{
    double rounded;
    double error;
    bool finite;
};

inline FmaExactSum fmaExactSum(uint32_t multiplicandBits, uint32_t multiplierBits, uint32_t addendBits)
{
    double multiplicand = fmaFloatOf(multiplicandBits);
    double multiplier = fmaFloatOf(multiplierBits);
    double addend = fmaFloatOf(addendBits);
    double product = multiplicand * multiplier;
    double sum = product + addend;
    if (!std::isfinite(multiplicand) || !std::isfinite(multiplier) || !std::isfinite(addend))
    {
        return FmaExactSum{sum, 0.0, false};
    }
    double virtualAddend = sum - product;
    double virtualProduct = sum - virtualAddend;
    double error = (product - virtualProduct) + (addend - virtualAddend);
    return FmaExactSum{sum, error, true};
}

inline uint32_t referenceFmaBits(uint32_t multiplicandBits, uint32_t multiplierBits, uint32_t addendBits)
{
    FmaExactSum exact = fmaExactSum(multiplicandBits, multiplierBits, addendBits);
    double roundedToOdd = exact.rounded;
    if (exact.error != 0.0 && (fmaBitsOf(roundedToOdd) & 1u) == 0u)
    {
        roundedToOdd = std::nextafter(roundedToOdd, exact.error > 0.0 ? HUGE_VAL : -HUGE_VAL);
    }
    return fmaBitsOf(static_cast<float>(roundedToOdd));
}

inline bool referenceFmaIsHalfway(uint32_t multiplicandBits, uint32_t multiplierBits, uint32_t addendBits)
{
    FmaExactSum exact = fmaExactSum(multiplicandBits, multiplierBits, addendBits);
    if (!exact.finite || exact.error != 0.0)
    {
        return false;
    }
    float rounded = static_cast<float>(exact.rounded);
    if (!std::isfinite(rounded) || static_cast<double>(rounded) == exact.rounded)
    {
        return false;
    }
    float neighbor = std::nextafter(rounded, exact.rounded > rounded ? HUGE_VALF : -HUGE_VALF);
    return (static_cast<double>(rounded) + static_cast<double>(neighbor)) * 0.5 == exact.rounded;
}

class FmaRandom
{
public:
    explicit FmaRandom(uint64_t seed)
        : state(seed)
    {
    }

    uint64_t next()
    {
        uint64_t mixed = (state += 0x9E3779B97F4A7C15ull);
        mixed = (mixed ^ (mixed >> 30)) * 0xBF58476D1CE4E5B9ull;
        mixed = (mixed ^ (mixed >> 27)) * 0x94D049BB133111EBull;
        return mixed ^ (mixed >> 31);
    }

    uint32_t below(uint32_t bound)
    {
        return static_cast<uint32_t>(next() % bound);
    }

    int32_t between(int32_t low, int32_t high)
    {
        return low + static_cast<int32_t>(below(static_cast<uint32_t>(high - low + 1)));
    }

    uint32_t sign()
    {
        return static_cast<uint32_t>(next() & 1u) << 31;
    }

    uint32_t mantissa()
    {
        return static_cast<uint32_t>(next()) & 0x7FFFFFu;
    }

    uint32_t oddBelowPowerOfTwo(uint32_t bits)
    {
        return (static_cast<uint32_t>(next()) & ((1u << bits) - 1u)) | 1u;
    }

private:
    uint64_t state;
};

inline uint32_t fmaNormalBits(uint32_t sign, int32_t exponent, uint32_t mantissa)
{
    int32_t biased = std::clamp(exponent, -126, 127) + 127;
    return sign | (static_cast<uint32_t>(biased) << 23) | (mantissa & 0x7FFFFFu);
}

inline uint32_t fmaScaledBits(uint32_t sign, uint64_t integer, int32_t exponent)
{
    return sign | fmaBitsOf(std::ldexp(static_cast<float>(integer), exponent));
}

inline uint32_t fmaSubnormalBits(FmaRandom& random)
{
    return random.sign() | std::max(random.mantissa(), 1u);
}

inline int32_t fmaUnbiasedExponent(uint32_t bits)
{
    return static_cast<int32_t>((bits >> 23) & 0xFFu) - 127;
}

inline uint64_t fmaSignificand(uint32_t bits)
{
    return (bits & 0x7FFFFFu) | 0x800000u;
}

inline FmaTriple fmaOrdinary(FmaRandom& random)
{
    int32_t exponentA = random.between(-30, 30);
    int32_t exponentB = random.between(-30, 30);
    int32_t exponentC = exponentA + exponentB + random.between(-40, 40);
    return FmaTriple{fmaNormalBits(random.sign(), exponentA, random.mantissa()),
        fmaNormalBits(random.sign(), exponentB, random.mantissa()), fmaNormalBits(random.sign(), exponentC, random.mantissa())};
}

inline FmaTriple fmaCancellation(FmaRandom& random)
{
    uint32_t multiplicand = fmaNormalBits(random.sign(), random.between(-30, 30), random.mantissa());
    uint32_t multiplier = fmaNormalBits(random.sign(), random.between(-30, 30), random.mantissa());
    double product = static_cast<double>(fmaFloatOf(multiplicand)) * static_cast<double>(fmaFloatOf(multiplier));
    uint32_t addend = 0;
    if (random.below(2) == 0)
    {
        uint32_t nearest = fmaBitsOf(static_cast<float>(-product));
        addend = (nearest & 0x80000000u) | ((nearest & 0x7FFFFFFFu) + static_cast<uint32_t>(random.between(-4, 4)));
    }
    else
    {
        uint64_t keptBits = static_cast<uint64_t>(random.between(1, 24));
        uint64_t truncated = fmaBitsOf(-product) & ~((uint64_t{1} << (53 - keptBits)) - 1u);
        double head;
        std::memcpy(&head, &truncated, sizeof head);
        addend = fmaBitsOf(static_cast<float>(head));
    }
    return FmaTriple{multiplicand, multiplier, addend};
}

inline FmaTriple fmaHalfway(FmaRandom& random)
{
    uint32_t kind = random.below(3);
    if (kind == 0)
    {
        int32_t exponentC = random.between(-30, 30);
        uint32_t addend = fmaNormalBits(random.sign(), exponentC, random.mantissa());
        uint64_t left = random.oddBelowPowerOfTwo(static_cast<uint32_t>(random.between(1, 12)));
        uint64_t right = random.oddBelowPowerOfTwo(static_cast<uint32_t>(random.between(1, 12)));
        int32_t total = exponentC - 24;
        int32_t shiftLeft = total / 2 + random.between(-8, 8);
        return FmaTriple{fmaScaledBits(random.sign(), left, shiftLeft), fmaScaledBits(random.sign(), right, total - shiftLeft), addend};
    }
    if (kind == 1)
    {
        uint64_t left = random.oddBelowPowerOfTwo(13) | 0x1000u;
        uint64_t lowest = ((uint64_t{1} << 24) + left - 1) / left;
        uint64_t highest = ((uint64_t{1} << 25) - 1) / left;
        uint64_t right = (lowest + random.below(static_cast<uint32_t>(highest - lowest + 1))) | 1u;
        right = right > highest ? right - 2 : right;
        int32_t shiftLeft = random.between(-20, 0);
        int32_t shiftRight = random.between(-20, 0);
        uint32_t tiny = fmaNormalBits(random.sign(), shiftLeft + shiftRight - random.between(1, 80), random.mantissa());
        return FmaTriple{fmaScaledBits(random.sign(), left, shiftLeft), fmaScaledBits(random.sign(), right, shiftRight), tiny};
    }
    uint32_t multiplicand = fmaNormalBits(random.sign(), random.between(-20, 20), random.mantissa() | 1u);
    uint32_t multiplier = fmaNormalBits(random.sign(), random.between(-20, 20), random.mantissa() | 1u);
    uint64_t exact = fmaSignificand(multiplicand) * fmaSignificand(multiplier);
    int32_t scale = fmaUnbiasedExponent(multiplicand) + fmaUnbiasedExponent(multiplier) - 46;
    uint32_t oppositeSign = ((multiplicand ^ multiplier) & 0x80000000u) ^ 0x80000000u;
    return FmaTriple{multiplicand, multiplier, fmaScaledBits(oppositeSign, exact >> 25, scale + 25)};
}

inline FmaTriple fmaSubnormal(FmaRandom& random)
{
    switch (random.below(4))
    {
    case 0:
    {
        int32_t target = random.between(-155, -120);
        int32_t exponentA = random.between(-100, -20);
        uint32_t multiplicand = fmaNormalBits(random.sign(), exponentA, random.mantissa());
        uint32_t multiplier = fmaNormalBits(random.sign(), target - exponentA, random.mantissa());
        uint32_t choice = random.below(3);
        uint32_t addend = choice == 0 ? random.sign()
            : choice == 1           ? fmaSubnormalBits(random)
                                    : fmaNormalBits(random.sign(), random.between(-126, -110), random.mantissa());
        return FmaTriple{multiplicand, multiplier, addend};
    }
    case 1:
    {
        uint32_t multiplicand = fmaSubnormalBits(random);
        uint32_t multiplier = fmaNormalBits(random.sign(), random.between(0, 60), random.mantissa());
        uint32_t addend = random.below(2) == 0 ? fmaSubnormalBits(random)
                                               : fmaNormalBits(random.sign(), random.between(-126, -60), random.mantissa());
        return FmaTriple{multiplicand, multiplier, addend};
    }
    case 2:
    {
        uint64_t left = random.oddBelowPowerOfTwo(static_cast<uint32_t>(random.between(1, 12)));
        uint64_t right = random.oddBelowPowerOfTwo(static_cast<uint32_t>(random.between(1, 12)));
        int32_t shiftLeft = -75 + random.between(-20, 20);
        return FmaTriple{fmaScaledBits(random.sign(), left, shiftLeft), fmaScaledBits(random.sign(), right, -150 - shiftLeft),
            fmaSubnormalBits(random)};
    }
    default:
    {
        uint32_t choice = random.below(3);
        uint32_t addend = choice == 0 ? random.sign()
            : choice == 1           ? fmaSubnormalBits(random)
                                    : fmaNormalBits(random.sign(), random.between(-126, -120), random.mantissa());
        return FmaTriple{fmaSubnormalBits(random), fmaSubnormalBits(random), addend};
    }
    }
}

inline FmaTriple fmaNearOverflow(FmaRandom& random)
{
    int32_t target = random.between(125, 129);
    int32_t exponentA = random.between(60, 70);
    uint32_t multiplicand = fmaNormalBits(random.sign(), exponentA, random.mantissa());
    uint32_t multiplier = fmaNormalBits(random.sign(), target - exponentA, random.mantissa());
    uint32_t choice = random.below(3);
    uint32_t addend = choice == 0 ? fmaNormalBits(random.sign(), random.between(120, 127), random.mantissa())
        : choice == 1           ? (random.sign() | 0x7F7FFFFFu)
                                : fmaNormalBits(random.sign(), random.between(-10, 10), random.mantissa());
    return FmaTriple{multiplicand, multiplier, addend};
}

inline FmaTriple fmaRandomTriple(FmaGroup group, uint64_t index, uint64_t seed)
{
    FmaRandom random{seed ^ (static_cast<uint64_t>(group) << 56) ^ (index * 0xD1B54A32D192ED03ull)};
    switch (group)
    {
    case FMA_ORDINARY:
        return fmaOrdinary(random);
    case FMA_CANCELLATION:
        return fmaCancellation(random);
    case FMA_HALFWAY:
        return fmaHalfway(random);
    case FMA_SUBNORMAL:
        return fmaSubnormal(random);
    default:
        return fmaNearOverflow(random);
    }
}

inline std::vector<FmaTriple> fmaSpecialTriples()
{
    const uint32_t values[] = {
        0x00000000u, 0x80000000u, 0x00000001u, 0x80000001u, 0x007FFFFFu, 0x807FFFFFu, 0x00800000u, 0x80800000u, 0x00800001u,
        0x3F800000u, 0xBF800000u, 0x3F800001u, 0x3F7FFFFFu, 0x40000000u, 0x34000000u, 0x33800000u, 0x1A000000u, 0x5F800000u,
        0x7F000000u, 0x7F7FFFFFu, 0xFF7FFFFFu, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0x7F800001u, 0xFFC00001u, 0x3F800800u,
    };
    std::vector<FmaTriple> triples;
    for (uint32_t multiplicand : values)
    {
        for (uint32_t multiplier : values)
        {
            for (uint32_t addend : values)
            {
                triples.push_back(FmaTriple{multiplicand, multiplier, addend});
            }
        }
    }
    return triples;
}

inline std::vector<FmaTriple> fmaGroupTriples(FmaGroup group, uint32_t count, uint64_t seed)
{
    if (group == FMA_SPECIAL)
    {
        return fmaSpecialTriples();
    }
    std::vector<FmaTriple> triples(count);
    for (uint32_t index = 0; index < count; ++index)
    {
        triples[index] = fmaRandomTriple(group, index, seed);
    }
    return triples;
}

struct FmaKnownAnswer
{
    FmaTriple triple;
    uint32_t want;
    const char* name;
};

inline const FmaKnownAnswer FMA_KNOWN_ANSWERS[] = {
    {{0x3F800800u, 0x3F800800u, 0xBF800000u}, 0x3A000400u, "(1+2^-12)^2 - 1 = 2^-11 + 2^-24"},
    {{0x3F800800u, 0x3F800800u, 0x17800000u}, 0x3F801001u, "(1+2^-12)^2 + 2^-80: двойное округление дало бы 1 + 2^-11"},
    {{0x00800000u, 0x3F000000u, 0x00000000u}, 0x00400000u, "2^-126 · 0.5 + 0 = 2^-127, денормализованное"},
    {{0x7F7FFFFFu, 0x3F800001u, 0xF3800000u}, 0x7F800000u, "max · (1+2^-23) - 2^104 переполняется"},
    {{0x3F800000u, 0x3F800000u, 0xBF800000u}, 0x00000000u, "1·1 - 1 = +0"},
    {{0x80000000u, 0x3F800000u, 0x80000000u}, 0x80000000u, "-0·1 + -0 = -0"},
};
