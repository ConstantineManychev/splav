#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define CORE_KIND_X86 1
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

enum class CoreKind
{
    Uniform,
    Performance,
    Efficient,
    Other,
};

struct CpuidWords
{
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
};

inline CpuidWords cpuidLeaf(uint32_t leaf, uint32_t subleaf)
{
#if defined(CORE_KIND_X86) && defined(_MSC_VER) && !defined(__clang__)
    int words[4] = {};
    __cpuidex(words, static_cast<int>(leaf), static_cast<int>(subleaf));
    return CpuidWords{static_cast<uint32_t>(words[0]), static_cast<uint32_t>(words[1]), static_cast<uint32_t>(words[2]),
        static_cast<uint32_t>(words[3])};
#elif defined(CORE_KIND_X86)
    unsigned eax = 0;
    unsigned ebx = 0;
    unsigned ecx = 0;
    unsigned edx = 0;
    __cpuid_count(leaf, subleaf, eax, ebx, ecx, edx);
    return CpuidWords{eax, ebx, ecx, edx};
#else
    (void)leaf;
    (void)subleaf;
    return CpuidWords{0, 0, 0, 0};
#endif
}

inline bool cpuIsHybrid()
{
    constexpr uint32_t STRUCTURED_FEATURES = 7;
    constexpr uint32_t HYBRID_FLAG = 1u << 15;
    return cpuidLeaf(0, 0).eax >= STRUCTURED_FEATURES && (cpuidLeaf(STRUCTURED_FEATURES, 0).edx & HYBRID_FLAG) != 0;
}

inline CoreKind currentCoreKind(bool hybrid)
{
    constexpr uint32_t HYBRID_INFORMATION = 0x1A;
    constexpr uint32_t PERFORMANCE_CORE = 0x40;
    constexpr uint32_t EFFICIENT_CORE = 0x20;
    if (!hybrid)
    {
        return CoreKind::Uniform;
    }
    if (cpuidLeaf(0, 0).eax < HYBRID_INFORMATION)
    {
        return CoreKind::Other;
    }
    uint32_t type = cpuidLeaf(HYBRID_INFORMATION, 0).eax >> 24;
    return type == PERFORMANCE_CORE ? CoreKind::Performance : type == EFFICIENT_CORE ? CoreKind::Efficient : CoreKind::Other;
}

struct CoreCensus
{
    bool hybrid = cpuIsHybrid();
    unsigned performance = 0;
    unsigned efficient = 0;
    unsigned other = 0;

    void sample()
    {
        switch (currentCoreKind(hybrid))
        {
        case CoreKind::Performance:
            ++performance;
            break;
        case CoreKind::Efficient:
            ++efficient;
            break;
        case CoreKind::Other:
            ++other;
            break;
        case CoreKind::Uniform:
            break;
        }
    }

    std::string text() const
    {
        char line[160];
        std::snprintf(line, sizeof line, other == 0 ? "P %u, E %u" : "P %u, E %u, иное %u", performance, efficient, other);
        return line;
    }
};
