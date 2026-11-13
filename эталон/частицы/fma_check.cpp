#include "fma_cases.hpp"
#include "soft_fma.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

constexpr uint64_t FMA_SEED = 0x5EED0050A0000010ull;

struct Miss
{
    FmaTriple triple;
    uint32_t got;
    uint32_t want;
};

struct GroupTally
{
    uint64_t total = 0;
    uint64_t halfway = 0;
    uint64_t softWrong = 0;
    uint64_t libraryWrong = 0;
    uint32_t libraryMaxUlp = 0;
    bool hasSoftMiss = false;
    bool hasLibraryMiss = false;
    Miss softMiss{};
    Miss libraryMiss{};

    void merge(const GroupTally& other)
    {
        total += other.total;
        halfway += other.halfway;
        softWrong += other.softWrong;
        libraryWrong += other.libraryWrong;
        libraryMaxUlp = std::max(libraryMaxUlp, other.libraryMaxUlp);
        if (!hasSoftMiss && other.hasSoftMiss)
        {
            hasSoftMiss = true;
            softMiss = other.softMiss;
        }
        if (!hasLibraryMiss && other.hasLibraryMiss)
        {
            hasLibraryMiss = true;
            libraryMiss = other.libraryMiss;
        }
    }
};

static uint32_t ulpDistance(uint32_t left, uint32_t right)
{
    auto ordered = [](uint32_t bits)
    {
        return (bits & 0x80000000u) != 0u ? static_cast<int64_t>(0x80000000u) - static_cast<int64_t>(bits & 0x7FFFFFFFu)
                                          : static_cast<int64_t>(0x80000000u) + static_cast<int64_t>(bits);
    };
    int64_t distance = ordered(left) - ordered(right);
    return static_cast<uint32_t>(std::min<int64_t>(distance < 0 ? -distance : distance, UINT32_MAX));
}

static uint32_t libraryFmaBits(const FmaTriple& triple)
{
    return fmaBitsOf(std::fma(fmaFloatOf(triple.multiplicand), fmaFloatOf(triple.multiplier), fmaFloatOf(triple.addend)));
}

static void checkTriple(const FmaTriple& triple, GroupTally& tally)
{
    uint32_t want = referenceFmaBits(triple.multiplicand, triple.multiplier, triple.addend);
    uint32_t soft = soft_glsl::softFmaBits(triple.multiplicand, triple.multiplier, triple.addend);
    uint32_t library = libraryFmaBits(triple);
    ++tally.total;
    tally.halfway += referenceFmaIsHalfway(triple.multiplicand, triple.multiplier, triple.addend) ? 1u : 0u;
    if (!fmaSameResult(soft, want))
    {
        ++tally.softWrong;
        if (!tally.hasSoftMiss)
        {
            tally.hasSoftMiss = true;
            tally.softMiss = Miss{triple, soft, want};
        }
    }
    if (!fmaSameResult(library, want))
    {
        ++tally.libraryWrong;
        if (!fmaIsNan(library) && !fmaIsNan(want))
        {
            tally.libraryMaxUlp = std::max(tally.libraryMaxUlp, ulpDistance(library, want));
        }
        if (!tally.hasLibraryMiss)
        {
            tally.hasLibraryMiss = true;
            tally.libraryMiss = Miss{triple, library, want};
        }
    }
}

static GroupTally checkGroup(FmaGroup group, uint64_t count, unsigned workers)
{
    if (group == FMA_SPECIAL)
    {
        GroupTally tally;
        for (const FmaTriple& triple : fmaSpecialTriples())
        {
            checkTriple(triple, tally);
        }
        return tally;
    }
    std::vector<GroupTally> partial(workers);
    std::vector<std::thread> threads;
    for (unsigned worker = 0; worker < workers; ++worker)
    {
        threads.emplace_back([&, worker]
        {
            uint64_t first = count * worker / workers;
            uint64_t last = count * (worker + 1) / workers;
            for (uint64_t index = first; index < last; ++index)
            {
                checkTriple(fmaRandomTriple(group, index, FMA_SEED), partial[worker]);
            }
        });
    }
    GroupTally tally;
    for (unsigned worker = 0; worker < workers; ++worker)
    {
        threads[worker].join();
        tally.merge(partial[worker]);
    }
    return tally;
}

static std::string missText(const Miss& miss)
{
    char line[160];
    std::snprintf(line, sizeof line, "fma(%08X, %08X, %08X) = %08X вместо %08X", miss.triple.multiplicand, miss.triple.multiplier,
        miss.triple.addend, miss.got, miss.want);
    return line;
}

static std::string verdict(uint64_t wrong, uint32_t maxUlp, bool hasMiss, const Miss& miss)
{
    if (wrong == 0)
    {
        return "все верны";
    }
    std::string text = "неверно " + std::to_string(wrong);
    if (maxUlp > 0)
    {
        text += ", до " + std::to_string(maxUlp) + " ulp";
    }
    return hasMiss ? text + "; первое: " + missText(miss) : text;
}

static bool checkKnownAnswers()
{
    bool referenceOk = true;
    bool softOk = true;
    bool libraryOk = true;
    for (const FmaKnownAnswer& known : FMA_KNOWN_ANSWERS)
    {
        uint32_t reference = referenceFmaBits(known.triple.multiplicand, known.triple.multiplier, known.triple.addend);
        uint32_t soft = soft_glsl::softFmaBits(known.triple.multiplicand, known.triple.multiplier, known.triple.addend);
        uint32_t library = libraryFmaBits(known.triple);
        referenceOk = referenceOk && reference == known.want;
        softOk = softOk && soft == known.want;
        libraryOk = libraryOk && library == known.want;
        if (reference != known.want || soft != known.want || library != known.want)
        {
            std::printf("    %s: ждём %08X, эталон %08X, программный %08X, std::fma %08X\n", known.name, known.want, reference, soft, library);
        }
    }
    std::printf("  Известные ответы (%zu): эталон — %s, программный — %s, std::fma — %s\n", std::size(FMA_KNOWN_ANSWERS),
        referenceOk ? "верно" : "НЕВЕРНО", softOk ? "верно" : "НЕВЕРНО", libraryOk ? "верно" : "НЕВЕРНО");
    return referenceOk && softOk;
}

int main(int argc, char** argv)
{
    uint64_t perGroup = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : (uint64_t{1} << 24);
    unsigned workers = std::max(1u, std::thread::hardware_concurrency());
    std::printf("=== Программный fma (soft_fma.glsl, тот же текст, что в шейдерах): побитно против независимого эталона на ЦПУ ===\n");
    std::printf("  Эталон: произведение точно в double, сумма с ошибкой по Кнуту, округление к нечётному, затем к float\n");
    bool ok = checkKnownAnswers();
    auto start = std::chrono::steady_clock::now();
    GroupTally overall;
    for (uint32_t group = 0; group < FMA_GROUPS; ++group)
    {
        GroupTally tally = checkGroup(static_cast<FmaGroup>(group), perGroup, workers);
        std::string halfway = tally.halfway > 0 ? ", из них ровно посередине " + std::to_string(tally.halfway) : "";
        std::printf("  %s (%llu%s): программный — %s; std::fma — %s\n", FMA_GROUP_NAMES[group],
            static_cast<unsigned long long>(tally.total), halfway.c_str(),
            verdict(tally.softWrong, 0, tally.hasSoftMiss, tally.softMiss).c_str(),
            verdict(tally.libraryWrong, tally.libraryMaxUlp, tally.hasLibraryMiss, tally.libraryMiss).c_str());
        overall.merge(tally);
    }
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    ok = ok && overall.softWrong == 0;
    std::printf("  Итого %llu троек за %.1f с на %u потоках: программный fma %s; std::fma этой платформы %s\n",
        static_cast<unsigned long long>(overall.total), seconds, workers, overall.softWrong == 0 ? "совпал с эталоном побитно" : "РАСХОДИТСЯ",
        overall.libraryWrong == 0 ? "тоже" : "расходится — не годится в эталон");
    return ok ? 0 : 1;
}
