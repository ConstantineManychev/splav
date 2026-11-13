#include "core_kind.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <vector>

struct AosParticle
{
    float position[3];
    float velocity[3];
};

void runInline(float* base, size_t capacity, size_t count, float gravity, float dt);
void runThreeWords(float* base, size_t capacity, size_t count, float gravity, float dt);
void runProxyValue(float* base, size_t capacity, size_t count, float gravity, float dt);
void runProxyReference(float* base, size_t capacity, size_t count, float gravity, float dt);
void runProxyValueInternal(float* base, size_t capacity, size_t count, float gravity, float dt);
void runEight(float* base, size_t capacity, size_t count, float gravity, float dt);
void runRange(float* base, size_t capacity, size_t count, float gravity, float dt);
void runAos(AosParticle* particles, size_t count, float gravity, float dt);

using Clock = std::chrono::steady_clock;
using SoaRunner = void (*)(float*, size_t, size_t, float, float);

constexpr int RUNS = 7;
constexpr size_t COLUMN_COUNT = 6;
constexpr size_t COLUMN_SKEW = 16;

struct Variant
{
    const char* name;
    SoaRunner soa;
};

struct Measurement
{
    double nsPerElement;
    double checksum;
};

static size_t paddedCapacity(size_t count)
{
    return (count + COLUMN_SKEW - 1) / COLUMN_SKEW * COLUMN_SKEW + COLUMN_SKEW;
}

static void fillSoa(std::vector<float>& block, size_t capacity, size_t count)
{
    for (size_t idx = 0; idx < count; ++idx)
    {
        float shift = static_cast<float>(idx % 97) * 0.01f;
        block[0 * capacity + idx] = 1.0f + shift;
        block[1 * capacity + idx] = 2.0f + shift;
        block[2 * capacity + idx] = 3.0f + shift;
        block[3 * capacity + idx] = 0.1f;
        block[4 * capacity + idx] = 0.2f;
        block[5 * capacity + idx] = 0.3f;
    }
}

static void fillAos(std::vector<AosParticle>& particles)
{
    for (size_t idx = 0; idx < particles.size(); ++idx)
    {
        float shift = static_cast<float>(idx % 97) * 0.01f;
        particles[idx] = AosParticle{{1.0f + shift, 2.0f + shift, 3.0f + shift}, {0.1f, 0.2f, 0.3f}};
    }
}

static void printPadded(const char* text, size_t width)
{
    size_t letters = 0;
    for (const char* cursor = text; *cursor != '\0'; ++cursor)
    {
        if ((static_cast<unsigned char>(*cursor) & 0xC0u) != 0x80u)
        {
            ++letters;
        }
    }
    std::printf("    %s%*s", text, static_cast<int>(width > letters ? width - letters : 0), "");
}

static double median(std::vector<double> values)
{
    std::nth_element(values.begin(), values.begin() + values.size() / 2, values.end());
    return values[values.size() / 2];
}

static Measurement measureSoa(SoaRunner runner, size_t capacity, size_t count, size_t passes, float gravity, float dt, CoreCensus& census)
{
    std::vector<float> block(COLUMN_COUNT * capacity, 0.0f);
    fillSoa(block, capacity, count);
    std::vector<double> times;
    for (int run = 0; run < RUNS; ++run)
    {
        census.sample();
        Clock::time_point start = Clock::now();
        for (size_t pass = 0; pass < passes; ++pass)
        {
            runner(block.data(), capacity, count, gravity, dt);
        }
        double ns = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
        census.sample();
        times.push_back(ns / static_cast<double>(count * passes));
    }
    double checksum = 0.0;
    for (float value : block)
    {
        checksum += value;
    }
    return Measurement{median(times), checksum};
}

static Measurement measureAos(size_t count, size_t passes, float gravity, float dt, CoreCensus& census)
{
    std::vector<AosParticle> particles(count);
    fillAos(particles);
    std::vector<double> times;
    for (int run = 0; run < RUNS; ++run)
    {
        census.sample();
        Clock::time_point start = Clock::now();
        for (size_t pass = 0; pass < passes; ++pass)
        {
            runAos(particles.data(), count, gravity, dt);
        }
        double ns = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
        census.sample();
        times.push_back(ns / static_cast<double>(count * passes));
    }
    double checksum = 0.0;
    for (const AosParticle& particle : particles)
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            checksum += particle.position[axis] + particle.velocity[axis];
        }
    }
    return Measurement{median(times), checksum};
}

static void runSize(const char* label, size_t count, size_t passes, float gravity, float dt)
{
    const Variant variants[] = {
        {"встроено", runInline},
        {"три слова в регистрах", runThreeWords},
        {"структура по значению", runProxyValue},
        {"структура по ссылке", runProxyReference},
        {"структура, внутренняя функция", runProxyValueInternal},
        {"клон на 8 элементов", runEight},
        {"клон на диапазон, по 256", runRange},
    };
    size_t capacities[2] = {count, paddedCapacity(count)};
    CoreCensus census;
    std::printf("  %s: %zu элементов × %zu проходов, медиана из %d, нс на элемент (× к встроенному): шаг столбцов %zu байт | со сдвигом %zu байт\n",
        label, count, passes, RUNS, count * sizeof(float), capacities[1] * sizeof(float));
    Measurement baselines[2] = {measureSoa(runInline, capacities[0], count, passes, gravity, dt, census),
        measureSoa(runInline, capacities[1], count, passes, gravity, dt, census)};
    for (const Variant& variant : variants)
    {
        printPadded(variant.name, 32);
        for (int layout = 0; layout < 2; ++layout)
        {
            Measurement result = variant.soa == runInline ? baselines[layout]
                : measureSoa(variant.soa, capacities[layout], count, passes, gravity, dt, census);
            bool same = result.checksum == baselines[layout].checksum;
            std::printf("%s%7.3f (%5.2f×)%s", layout == 0 ? "" : " | ", result.nsPerElement, result.nsPerElement / baselines[layout].nsPerElement,
                same ? "" : " КОНТРОЛЬНАЯ СУММА РАЗОШЛАСЬ");
        }
        std::printf("\n");
    }
    Measurement aos = measureAos(count, passes, gravity, dt, census);
    printPadded("AoS по ссылке", 32);
    std::printf("%7.3f (%5.2f× к встроенному SoA со сдвигом)\n", aos.nsPerElement, aos.nsPerElement / baselines[1].nsPerElement);
    if (census.hybrid)
    {
        std::printf("    ЦПУ гибридный, ядро в начале и конце каждого прогона (CPUID 0x1A): %s\n", census.text().c_str());
    }
}

int main()
{
    volatile float gravity = 9.81f;
    volatile float dt = 0.002f;
    std::printf("=== Прокси набор: передача между невстроенными функциями (§5 п. 4) ===\n");
    runSize("в кэше", 16384, 1024, gravity, dt);
    runSize("в памяти", 8u << 20, 2, gravity, dt);
    return 0;
}
