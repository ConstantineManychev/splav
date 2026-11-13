#include "model.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>

using UpdateFunction = void (*)(
    int, int,
    const float*, const float*, const float*, const float*, const float*, const float*,
    const uint32_t*, const uint32_t*, const World*,
    float*, float*, float*, float*, float*, float*, uint32_t*);

extern "C"
{
    void update_avx512(int, int, const float*, const float*, const float*, const float*, const float*,
        const float*, const uint32_t*, const uint32_t*, const World*, float*, float*, float*, float*, float*,
        float*, uint32_t*);
    void update_avx2(int, int, const float*, const float*, const float*, const float*, const float*,
        const float*, const uint32_t*, const uint32_t*, const World*, float*, float*, float*, float*, float*,
        float*, uint32_t*);
    void update_avx512_fma(int, int, const float*, const float*, const float*, const float*, const float*,
        const float*, const uint32_t*, const uint32_t*, const World*, float*, float*, float*, float*, float*,
        float*, uint32_t*);
}

enum class Kernel
{
    Scalar,
    ScalarReversed,
    Avx2,
    Avx512,
    Avx512Fma,
};

using Clock = std::chrono::steady_clock;

double secondsSince(Clock::time_point start)
{
    return std::chrono::duration<double>(Clock::now() - start).count();
}

struct PhaseTimes
{
    double keys = 0;
    double sort = 0;
    double gather = 0;
    double update = 0;

    double total() const
    {
        return keys + sort + gather + update;
    }
};

struct Simulation
{
    World world;
    unsigned threads;
    Kernel kernel;
    Columns particles;
    Columns sorted;
    std::vector<uint32_t> keys;
    std::vector<uint32_t> order;
    std::vector<uint32_t> starts;
    RadixDistributor distributor;
    PhaseTimes times;

    Simulation(uint32_t side, unsigned threadCount, Kernel kernelKind)
        : world(worldFor(side)), threads(threadCount), kernel(kernelKind)
    {
        size_t count = particleCount(world);
        particles.resize(count);
        sorted.resize(count);
        keys.assign(count, 0);
        order.assign(count, 0);
        starts.assign(cellCount(world) + 1, 0);
        parallelFor(count, threads, [&](size_t begin, size_t end)
        {
            for (size_t index = begin; index < end; ++index)
            {
                particles.store(index, initialParticle(index, world));
            }
        });
    }

    size_t count() const
    {
        return particles.size();
    }

    void step()
    {
        auto start = Clock::now();
        parallelFor(count(), threads, [&](size_t begin, size_t end)
        {
            for (size_t index = begin; index < end; ++index)
            {
                keys[index] = cellKey(index, particles, world);
            }
        });
        times.keys += secondsSince(start);
        start = Clock::now();
        distributor.distribute(keys, cellCount(world), threads, order, starts);
        times.sort += secondsSince(start);
        start = Clock::now();
        parallelFor(count(), threads, [&](size_t begin, size_t end)
        {
            for (size_t index = begin; index < end; ++index)
            {
                sorted.store(index, particles.load(order[index]));
            }
        });
        times.gather += secondsSince(start);
        start = Clock::now();
        parallelFor(count(), threads, [&](size_t begin, size_t end) { update(begin, end); });
        times.update += secondsSince(start);
    }

    void update(size_t begin, size_t end)
    {
        if (kernel == Kernel::Scalar)
        {
            for (size_t index = begin; index < end; ++index)
            {
                particles.store(index, updateParticle(index, sorted, starts.data(), world));
            }
            return;
        }
        if (kernel == Kernel::ScalarReversed)
        {
            for (size_t index = end; index-- > begin;)
            {
                particles.store(index, updateParticle(index, sorted, starts.data(), world));
            }
            return;
        }
        UpdateFunction function = kernel == Kernel::Avx2 ? update_avx2
            : kernel == Kernel::Avx512 ? update_avx512 : update_avx512_fma;
        function(static_cast<int>(begin), static_cast<int>(end),
            sorted.px.data(), sorted.py.data(), sorted.pz.data(),
            sorted.vx.data(), sorted.vy.data(), sorted.vz.data(),
            sorted.id.data(), starts.data(), &world,
            particles.px.data(), particles.py.data(), particles.pz.data(),
            particles.vx.data(), particles.vy.data(), particles.vz.data(), particles.id.data());
    }

    void run(size_t steps)
    {
        for (size_t step = 0; step < steps; ++step)
        {
            this->step();
        }
    }

    size_t contactCount() const
    {
        size_t contacts = 0;
        for (size_t index = 0; index < count(); ++index)
        {
            Particle particle = sorted.load(index);
            Cell own = cellOf(particle.position, world);
            for (size_t neighbour = 0; neighbour < 27; ++neighbour)
            {
                Cell cell{own.x + static_cast<int32_t>(neighbour % 3) - 1,
                    own.y + static_cast<int32_t>(neighbour / 3 % 3) - 1,
                    own.z + static_cast<int32_t>(neighbour / 9) - 1};
                if (inside(cell, world))
                {
                    size_t flat = cellIndex(cell, world);
                    for (size_t other = starts[flat]; other < starts[flat + 1]; ++other)
                    {
                        Vec3 position{sorted.px[other], sorted.py[other], sorted.pz[other]};
                        Vec3 difference = subtract(particle.position, position);
                        float square = dot(difference, difference);
                        contacts += other != index && square < world.diameter * world.diameter;
                    }
                }
            }
        }
        return contacts / 2;
    }
};

double pairEnergy(Vec3 own, Vec3 other, const World& world)
{
    Vec3 difference = subtract(own, other);
    double distance = std::sqrt(static_cast<double>(dot(difference, difference)));
    double overlap = static_cast<double>(world.diameter) - distance;
    if (overlap <= 0.0)
    {
        return 0.0;
    }
    return 0.5 * static_cast<double>(world.stiffness) * overlap * overlap;
}

double wallEnergy(float coordinate, const World& world)
{
    double force = static_cast<double>(wall(coordinate, world));
    return 0.5 * force * force / static_cast<double>(world.stiffness);
}

double energy(const Columns& particles, const World& world)
{
    double sum = 0.0;
    for (size_t index = 0; index < particles.size(); ++index)
    {
        Particle particle = particles.load(index);
        sum += 0.5 * static_cast<double>(dot(particle.velocity, particle.velocity));
        sum += static_cast<double>(world.gravity) * static_cast<double>(particle.position.z);
        double byX = wallEnergy(particle.position.x, world);
        double byY = wallEnergy(particle.position.y, world);
        double byZ = wallEnergy(particle.position.z, world);
        sum += byX + byY + byZ;
        for (size_t other = index + 1; other < particles.size(); ++other)
        {
            Vec3 position{particles.px[other], particles.py[other], particles.pz[other]};
            sum += pairEnergy(particle.position, position, world);
        }
    }
    return sum;
}

bool report(const char* name, bool passed)
{
    std::printf("%s %s\n", passed ? "✓" : "✗", name);
    return passed;
}

int runTests()
{
    bool all = true;
    {
        Simulation one(32, 1, Kernel::Scalar);
        Simulation two(32, 2, Kernel::Scalar);
        one.run(200);
        two.run(200);
        all &= report("потоки не меняют результат: 1 и 2 потока, 32768 частиц, 200 шагов, побитно",
            one.particles.bitwiseEqual(two.particles));
    }
    {
        Simulation scalar(32, 2, Kernel::Scalar);
        Simulation avx2(32, 2, Kernel::Avx2);
        Simulation avx512(32, 2, Kernel::Avx512);
        Simulation reversed(32, 2, Kernel::ScalarReversed);
        scalar.run(200);
        avx2.run(200);
        avx512.run(200);
        reversed.run(200);
        all &= report("ширина SIMD не меняет результат: скаляр = AVX2×8 = AVX-512×16, побитно",
            scalar.particles.bitwiseEqual(avx2.particles) && scalar.particles.bitwiseEqual(avx512.particles));
        all &= report("порядок исполнения номеров не меняет результат, побитно",
            scalar.particles.bitwiseEqual(reversed.particles));
        std::printf("  контактов после 200 шагов: %zu\n", scalar.contactCount());
    }
    {
        Simulation cells(8, 2, Kernel::Scalar);
        cells.run(200);
        Columns before = cells.particles;
        cells.step();
        Columns allPairs;
        allPairs.resize(cells.count());
        for (size_t index = 0; index < cells.count(); ++index)
        {
            allPairs.store(index, updateByAllPairs(index, cells.sorted, cells.world));
        }
        all &= report("перебор всех пар совпадает с клеточным списком, побитно",
            cells.particles.bitwiseEqual(allPairs));
        std::printf("  контактов: %zu\n", cells.contactCount());
    }
    {
        Simulation small(8, 2, Kernel::Avx512);
        double initial = energy(small.particles, small.world);
        double worst = 0.0;
        for (size_t step = 1; step <= 2000; ++step)
        {
            small.step();
            if (step % 100 == 0)
            {
                double drift = std::fabs(energy(small.particles, small.world) - initial) / std::fabs(initial);
                worst = std::max(worst, drift);
            }
        }
        double final = energy(small.particles, small.world);
        double drift = std::fabs(final - initial) / std::fabs(initial);
        std::printf("  энергия: начальная %.6f, через 2000 шагов %.6f, дрейф %.2e, худший %.2e\n",
            initial, final, drift, worst);
        all &= report("энергия сохраняется с точностью 1e-3", worst < 1e-3);
    }
    return all ? 0 : 1;
}

uint32_t floatBits(float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

int64_t ulpDistance(float a, float b)
{
    auto ordered = [](float value)
    {
        int64_t bits = static_cast<int32_t>(floatBits(value));
        return bits < 0 ? -(bits & 0x7FFFFFFF) : bits;
    };
    return std::llabs(ordered(a) - ordered(b));
}

int runChaos()
{
    Simulation strict(32, 2, Kernel::Avx512);
    Simulation fused(32, 2, Kernel::Avx512Fma);
    Simulation smallStrict(8, 2, Kernel::Avx512);
    Simulation smallFused(8, 2, Kernel::Avx512Fma);
    double strictInitial = energy(smallStrict.particles, smallStrict.world);
    double fusedInitial = energy(smallFused.particles, smallFused.world);
    std::printf("строгий режим против FMA-сжатия (как CUDA по умолчанию), 32768 частиц\n");
    std::printf("%6s %12s %12s %12s %20s\n", "шаг", "ulp скор.", "макс |Δx|", "средн |Δx|", "дрейф энергии 512 ч.");
    size_t checkpoints[] = {1, 10, 100, 300, 1000, 2000, 4000};
    size_t done = 0;
    for (size_t checkpoint : checkpoints)
    {
        strict.run(checkpoint - done);
        fused.run(checkpoint - done);
        smallStrict.run(checkpoint - done);
        smallFused.run(checkpoint - done);
        done = checkpoint;
        std::vector<size_t> byId(strict.count());
        for (size_t index = 0; index < fused.count(); ++index)
        {
            byId[fused.particles.id[index]] = index;
        }
        int64_t worstUlp = 0;
        double worstShift = 0.0;
        double meanShift = 0.0;
        for (size_t index = 0; index < strict.count(); ++index)
        {
            Particle a = strict.particles.load(index);
            Particle b = fused.particles.load(byId[a.id]);
            worstUlp = std::max({worstUlp, ulpDistance(a.velocity.x, b.velocity.x),
                ulpDistance(a.velocity.y, b.velocity.y), ulpDistance(a.velocity.z, b.velocity.z)});
            Vec3 shift = subtract(a.position, b.position);
            double length = std::sqrt(static_cast<double>(dot(shift, shift)));
            worstShift = std::max(worstShift, length);
            meanShift += length / static_cast<double>(strict.count());
        }
        double strictDrift = (energy(smallStrict.particles, smallStrict.world) - strictInitial) / strictInitial;
        double fusedDrift = (energy(smallFused.particles, smallFused.world) - fusedInitial) / fusedInitial;
        std::printf("%6zu %12lld %12.3e %12.3e   %+.2e / %+.2e\n", checkpoint, static_cast<long long>(worstUlp),
            worstShift, meanShift, strictDrift, fusedDrift);
    }
    std::printf("  размер ящика %.1f, диаметр частицы %.1f\n", strict.world.size, strict.world.diameter);
    return 0;
}

double streamTriad(unsigned threads)
{
    size_t count = size_t{1} << 25;
    std::vector<float> a(count, 0.0f), b(count, 1.0f), c(count, 2.0f);
    double best = 1e9;
    for (int repeat = 0; repeat < 5; ++repeat)
    {
        auto start = Clock::now();
        parallelFor(count, threads, [&](size_t begin, size_t end)
        {
            for (size_t index = begin; index < end; ++index)
            {
                a[index] = b[index] + 3.0f * c[index];
            }
        });
        best = std::min(best, secondsSince(start));
    }
    return 3.0 * sizeof(float) * count / best / 1e9;
}

void benchmark(const char* name, uint32_t side, unsigned threads, Kernel kernel, size_t steps)
{
    Simulation simulation(side, threads, kernel);
    simulation.run(20);
    simulation.times = PhaseTimes{};
    simulation.run(steps);
    double perStep = simulation.times.total() / steps;
    double count = static_cast<double>(simulation.count());
    auto ms = [&](double seconds) { return seconds / steps * 1e3; };
    std::printf("%-26s %2u пот. %7.2f мс/шаг: ключи %6.2f, сортировка %6.2f, сбор %6.2f, обновление %7.2f"
        " | %6.1f млн частиц-шагов/с\n",
        name, threads, perStep * 1e3, ms(simulation.times.keys), ms(simulation.times.sort),
        ms(simulation.times.gather), ms(simulation.times.update), count / perStep / 1e6);
    double keyBytes = count * (3 * 4 + 4);
    double gatherBytes = count * (4 + 2 * 7 * 4);
    std::printf("%-26s          ключи %5.1f ГБ/с, сбор %5.1f ГБ/с\n", "", keyBytes / (simulation.times.keys / steps) / 1e9,
        gatherBytes / (simulation.times.gather / steps) / 1e9);
}

struct ParticleRecord
{
    float px, py, pz, vx, vy, vz;
    uint32_t id;
};

struct AosSimulation
{
    World world;
    unsigned threads;
    std::vector<ParticleRecord> particles;
    std::vector<ParticleRecord> sorted;
    std::vector<uint32_t> keys;
    std::vector<uint32_t> order;
    std::vector<uint32_t> starts;
    RadixDistributor distributor;
    PhaseTimes times;

    AosSimulation(uint32_t side, unsigned threadCount) : world(worldFor(side)), threads(threadCount)
    {
        size_t count = particleCount(world);
        particles.resize(count);
        sorted.resize(count);
        keys.assign(count, 0);
        order.assign(count, 0);
        starts.assign(cellCount(world) + 1, 0);
        for (size_t index = 0; index < count; ++index)
        {
            particles[index] = record(initialParticle(index, world));
        }
    }

    static ParticleRecord record(const Particle& particle)
    {
        return ParticleRecord{particle.position.x, particle.position.y, particle.position.z,
            particle.velocity.x, particle.velocity.y, particle.velocity.z, particle.id};
    }

    static Particle particle(const ParticleRecord& record)
    {
        return Particle{{record.px, record.py, record.pz}, {record.vx, record.vy, record.vz}, record.id};
    }

    Particle update(size_t index) const
    {
        Particle own = particle(sorted[index]);
        Cell ownCell = cellOf(own.position, world);
        Vec3 acceleration = externalAcceleration(own.position, world);
        for (size_t neighbour = 0; neighbour < 27; ++neighbour)
        {
            Cell cell{ownCell.x + static_cast<int32_t>(neighbour % 3) - 1,
                ownCell.y + static_cast<int32_t>(neighbour / 3 % 3) - 1,
                ownCell.z + static_cast<int32_t>(neighbour / 9) - 1};
            if (inside(cell, world))
            {
                size_t flat = cellIndex(cell, world);
                for (size_t other = starts[flat]; other < starts[flat + 1]; ++other)
                {
                    if (other != index)
                    {
                        Vec3 position{sorted[other].px, sorted[other].py, sorted[other].pz};
                        acceleration = add(acceleration, elastic(own.position, position, world));
                    }
                }
            }
        }
        return shift(own, acceleration, world);
    }

    void step()
    {
        size_t count = particles.size();
        auto start = Clock::now();
        parallelFor(count, threads, [&](size_t begin, size_t end)
        {
            for (size_t index = begin; index < end; ++index)
            {
                Vec3 position{particles[index].px, particles[index].py, particles[index].pz};
                keys[index] = cellIndex(cellOf(position, world), world);
            }
        });
        times.keys += secondsSince(start);
        start = Clock::now();
        distributor.distribute(keys, cellCount(world), threads, order, starts);
        times.sort += secondsSince(start);
        start = Clock::now();
        parallelFor(count, threads, [&](size_t begin, size_t end)
        {
            for (size_t index = begin; index < end; ++index)
            {
                sorted[index] = particles[order[index]];
            }
        });
        times.gather += secondsSince(start);
        start = Clock::now();
        parallelFor(count, threads, [&](size_t begin, size_t end)
        {
            for (size_t index = begin; index < end; ++index)
            {
                particles[index] = record(update(index));
            }
        });
        times.update += secondsSince(start);
    }
};

void benchmarkAos(uint32_t side, unsigned threads, size_t steps)
{
    AosSimulation simulation(side, threads);
    for (int warmup = 0; warmup < 20; ++warmup)
    {
        simulation.step();
    }
    simulation.times = PhaseTimes{};
    for (size_t step = 0; step < steps; ++step)
    {
        simulation.step();
    }
    double count = static_cast<double>(simulation.particles.size());
    auto ms = [&](double seconds) { return seconds / steps * 1e3; };
    std::printf("%-26s %2u пот. %7.2f мс/шаг: ключи %6.2f, сортировка %6.2f, сбор %6.2f, обновление %7.2f\n",
        "скаляр AoS", threads, ms(simulation.times.total()), ms(simulation.times.keys), ms(simulation.times.sort),
        ms(simulation.times.gather), ms(simulation.times.update));
    double keyBytes = count * (sizeof(ParticleRecord) + 4);
    double gatherBytes = count * (4 + 2 * sizeof(ParticleRecord));
    std::printf("%-26s          ключи %5.1f ГБ/с, сбор %5.1f ГБ/с (с учётом целых записей)\n", "",
        keyBytes / (simulation.times.keys / steps) / 1e9, gatherBytes / (simulation.times.gather / steps) / 1e9);
}

int runBenchmarks()
{
    std::printf("STREAM triad: 1 поток %.1f ГБ/с, 2 потока %.1f ГБ/с\n", streamTriad(1), streamTriad(2));
    uint32_t side = 100;
    size_t steps = 50;
    benchmark("скаляр", side, 1, Kernel::Scalar, steps);
    benchmark("скаляр", side, 2, Kernel::Scalar, steps);
    benchmark("SPMD AVX2 ×8", side, 1, Kernel::Avx2, steps);
    benchmark("SPMD AVX-512 ×16", side, 1, Kernel::Avx512, steps);
    benchmark("SPMD AVX-512 ×16", side, 2, Kernel::Avx512, steps);
    benchmarkAos(side, 1, steps);
    benchmarkAos(side, 2, steps);
    return 0;
}

int main(int argc, char** argv)
{
    std::string mode = argc > 1 ? argv[1] : "tests";
    if (mode == "tests")
    {
        return runTests();
    }
    if (mode == "chaos")
    {
        return runChaos();
    }
    if (mode == "bench")
    {
        return runBenchmarks();
    }
    if (mode == "races")
    {
        uint32_t side = 32;
        size_t steps = 20;
        unsigned threads = 2;
        Simulation simulation(side, threads, Kernel::Scalar);
        simulation.run(steps);
        size_t count = size_t(side) * side * side;
        std::printf("%zu шагов, %zu частиц (%zu кусков по %zu), %u потока: завершено\n",
            steps, count, (count + CHUNK - 1) / CHUNK, CHUNK, threads);
        return 0;
    }
    return 2;
}
