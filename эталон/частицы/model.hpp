#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

struct World
{
    float size;
    uint32_t cellsPerSide;
    float cellSize;
    float diameter;
    float stiffness;
    float gravity;
    float timeStep;
    uint32_t particlesPerSide;
    float latticeStep;
    float initialSpeed;
};

struct Vec3
{
    float x;
    float y;
    float z;
};

struct Particle
{
    Vec3 position;
    Vec3 velocity;
    uint32_t id;
};

struct Cell
{
    int32_t x;
    int32_t y;
    int32_t z;
};

struct Columns
{
    std::vector<float> px, py, pz, vx, vy, vz;
    std::vector<uint32_t> id;

    void resize(size_t count)
    {
        for (auto* column : {&px, &py, &pz, &vx, &vy, &vz})
        {
            column->assign(count, 0.0f);
        }
        id.assign(count, 0);
    }

    size_t size() const
    {
        return id.size();
    }

    Particle load(size_t index) const
    {
        return Particle{{px[index], py[index], pz[index]}, {vx[index], vy[index], vz[index]}, id[index]};
    }

    void store(size_t index, const Particle& particle)
    {
        px[index] = particle.position.x;
        py[index] = particle.position.y;
        pz[index] = particle.position.z;
        vx[index] = particle.velocity.x;
        vy[index] = particle.velocity.y;
        vz[index] = particle.velocity.z;
        id[index] = particle.id;
    }

    bool bitwiseEqual(const Columns& other) const
    {
        auto same = [](const auto& a, const auto& b)
        {
            return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(a[0])) == 0;
        };
        return same(px, other.px) && same(py, other.py) && same(pz, other.pz) && same(vx, other.vx)
            && same(vy, other.vy) && same(vz, other.vz) && same(id, other.id);
    }
};

constexpr float DIAMETER = 1.0f;
constexpr float LATTICE_GAP = 1.05f;
constexpr float STIFFNESS = 1000.0f;
constexpr float GRAVITY = 1.0f;
constexpr float TIME_STEP = 0.002f;
constexpr float INITIAL_SPEED = 0.1f;
constexpr size_t CHUNK = 4096;

inline unsigned workersFor(size_t count, unsigned threads)
{
    size_t chunks = (count + CHUNK - 1) / CHUNK;
    return static_cast<unsigned>(std::clamp<size_t>(chunks, 1, std::max(threads, 1u)));
}

inline World worldFor(uint32_t particlesPerSide)
{
    float latticeStep = DIAMETER * LATTICE_GAP;
    float size = 2.0f * static_cast<float>(particlesPerSide) * latticeStep;
    uint32_t cellsPerSide = static_cast<uint32_t>(size / DIAMETER);
    return World{
        size,
        cellsPerSide,
        size / static_cast<float>(cellsPerSide),
        DIAMETER,
        STIFFNESS,
        GRAVITY,
        TIME_STEP,
        particlesPerSide,
        latticeStep,
        INITIAL_SPEED,
    };
}

inline size_t particleCount(const World& world)
{
    size_t side = world.particlesPerSide;
    return side * side * side;
}

inline size_t cellCount(const World& world)
{
    size_t side = world.cellsPerSide;
    return side * side * side;
}

inline Vec3 add(Vec3 a, Vec3 b)
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

inline Vec3 subtract(Vec3 a, Vec3 b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

inline Vec3 scale(Vec3 a, float factor)
{
    return {a.x * factor, a.y * factor, a.z * factor};
}

inline float dot(Vec3 a, Vec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline float noise(size_t index, uint32_t axis)
{
    uint32_t x = static_cast<uint32_t>(index) * 3u + axis;
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return static_cast<float>(x >> 8) * (2.0f / 16777216.0f) - 1.0f;
}

inline Particle initialParticle(size_t index, const World& world)
{
    size_t side = world.particlesPerSide;
    Vec3 node{
        static_cast<float>(index % side),
        static_cast<float>(index / side % side),
        static_cast<float>(index / (side * side)),
    };
    Vec3 position = scale(add(node, Vec3{0.5f, 0.5f, 0.5f}), world.latticeStep);
    Vec3 velocity = scale(Vec3{noise(index, 0), noise(index, 1), noise(index, 2)}, world.initialSpeed);
    return Particle{position, velocity, static_cast<uint32_t>(index)};
}

inline int32_t cellCoordinate(float coordinate, const World& world)
{
    int32_t raw = static_cast<int32_t>(std::floor(coordinate / world.cellSize));
    return std::clamp(raw, 0, static_cast<int32_t>(world.cellsPerSide) - 1);
}

inline Cell cellOf(Vec3 position, const World& world)
{
    return Cell{
        cellCoordinate(position.x, world),
        cellCoordinate(position.y, world),
        cellCoordinate(position.z, world),
    };
}

inline uint32_t cellIndex(Cell cell, const World& world)
{
    int32_t side = static_cast<int32_t>(world.cellsPerSide);
    return static_cast<uint32_t>((cell.z * side + cell.y) * side + cell.x);
}

inline bool inside(Cell cell, const World& world)
{
    int32_t side = static_cast<int32_t>(world.cellsPerSide);
    return cell.x >= 0 && cell.x < side && cell.y >= 0 && cell.y < side && cell.z >= 0 && cell.z < side;
}

inline Vec3 elastic(Vec3 own, Vec3 other, const World& world)
{
    Vec3 difference = subtract(own, other);
    float square = dot(difference, difference);
    if (square >= world.diameter * world.diameter || square == 0.0f)
    {
        return Vec3{0.0f, 0.0f, 0.0f};
    }
    float distance = std::sqrt(square);
    return scale(difference, world.stiffness * (world.diameter - distance) / distance);
}

inline float wall(float coordinate, const World& world)
{
    float radius = world.diameter * 0.5f;
    if (coordinate < radius)
    {
        return world.stiffness * (radius - coordinate);
    }
    float top = world.size - radius;
    if (coordinate > top)
    {
        return world.stiffness * (top - coordinate);
    }
    return 0.0f;
}

inline Vec3 externalAcceleration(Vec3 position, const World& world)
{
    Vec3 walls{wall(position.x, world), wall(position.y, world), wall(position.z, world)};
    return subtract(walls, Vec3{0.0f, 0.0f, world.gravity});
}

inline Particle shift(const Particle& particle, Vec3 acceleration, const World& world)
{
    Vec3 velocity = add(particle.velocity, scale(acceleration, world.timeStep));
    Vec3 position = add(particle.position, scale(velocity, world.timeStep));
    return Particle{position, velocity, particle.id};
}

inline uint32_t cellKey(size_t index, const Columns& particles, const World& world)
{
    Vec3 position{particles.px[index], particles.py[index], particles.pz[index]};
    return cellIndex(cellOf(position, world), world);
}

inline Particle updateParticle(size_t index, const Columns& particles, const uint32_t* starts, const World& world)
{
    Particle particle = particles.load(index);
    Cell own = cellOf(particle.position, world);
    Vec3 acceleration = externalAcceleration(particle.position, world);
    for (size_t neighbour = 0; neighbour < 27; ++neighbour)
    {
        Cell cell{
            own.x + static_cast<int32_t>(neighbour % 3) - 1,
            own.y + static_cast<int32_t>(neighbour / 3 % 3) - 1,
            own.z + static_cast<int32_t>(neighbour / 9) - 1,
        };
        if (inside(cell, world))
        {
            size_t flat = cellIndex(cell, world);
            for (size_t other = starts[flat]; other < starts[flat + 1]; ++other)
            {
                if (other != index)
                {
                    Vec3 position{particles.px[other], particles.py[other], particles.pz[other]};
                    acceleration = add(acceleration, elastic(particle.position, position, world));
                }
            }
        }
    }
    return shift(particle, acceleration, world);
}

inline Particle updateByAllPairs(size_t index, const Columns& particles, const World& world)
{
    Particle particle = particles.load(index);
    Vec3 acceleration = externalAcceleration(particle.position, world);
    for (size_t other = 0; other < particles.size(); ++other)
    {
        if (other != index)
        {
            Vec3 position{particles.px[other], particles.py[other], particles.pz[other]};
            acceleration = add(acceleration, elastic(particle.position, position, world));
        }
    }
    return shift(particle, acceleration, world);
}

template <class Body>
void parallelFor(size_t count, unsigned threads, Body body)
{
    unsigned workers = workersFor(count, threads);
    std::atomic<size_t> next{0};
    auto worker = [&]
    {
        for (;;)
        {
            size_t begin = next.fetch_add(CHUNK);
            if (begin >= count)
            {
                return;
            }
            body(begin, std::min(count, begin + CHUNK));
        }
    };
    std::vector<std::thread> pool;
    for (unsigned thread = 1; thread < workers; ++thread)
    {
        pool.emplace_back(worker);
    }
    worker();
    for (auto& thread : pool)
    {
        thread.join();
    }
}

class RadixDistributor
{
public:
    void distribute(
        const std::vector<uint32_t>& keys,
        size_t cells,
        unsigned threads,
        std::vector<uint32_t>& order,
        std::vector<uint32_t>& starts)
    {
        size_t count = keys.size();
        threads = workersFor(count, threads);
        keysFront_.resize(count);
        keysBack_.resize(count);
        orderBack_.resize(count);
        counts_.assign(threads, std::vector<uint32_t>(RADIX, 0));
        size_t block = (count + threads - 1) / threads;
        auto range = [&](unsigned thread)
        {
            size_t begin = std::min(count, thread * block);
            return std::pair{begin, std::min(count, begin + block)};
        };
        forEachThread(threads, [&](unsigned thread)
        {
            auto [begin, end] = range(thread);
            for (size_t index = begin; index < end; ++index)
            {
                keysFront_[index] = keys[index];
                order[index] = static_cast<uint32_t>(index);
            }
        });
        unsigned bits = 1;
        while ((size_t{1} << bits) < cells)
        {
            ++bits;
        }
        std::vector<uint32_t>* sourceKeys = &keysFront_;
        std::vector<uint32_t>* targetKeys = &keysBack_;
        std::vector<uint32_t>* sourceOrder = &order;
        std::vector<uint32_t>* targetOrder = &orderBack_;
        for (unsigned shift = 0; shift < bits; shift += DIGIT_BITS)
        {
            forEachThread(threads, [&](unsigned thread)
            {
                auto& histogram = counts_[thread];
                std::fill(histogram.begin(), histogram.end(), 0u);
                auto [begin, end] = range(thread);
                for (size_t index = begin; index < end; ++index)
                {
                    ++histogram[((*sourceKeys)[index] >> shift) & (RADIX - 1)];
                }
            });
            uint32_t running = 0;
            for (size_t digit = 0; digit < RADIX; ++digit)
            {
                for (unsigned thread = 0; thread < threads; ++thread)
                {
                    uint32_t digitCount = counts_[thread][digit];
                    counts_[thread][digit] = running;
                    running += digitCount;
                }
            }
            forEachThread(threads, [&](unsigned thread)
            {
                auto& offsets = counts_[thread];
                auto [begin, end] = range(thread);
                for (size_t index = begin; index < end; ++index)
                {
                    uint32_t key = (*sourceKeys)[index];
                    uint32_t position = offsets[(key >> shift) & (RADIX - 1)]++;
                    (*targetKeys)[position] = key;
                    (*targetOrder)[position] = (*sourceOrder)[index];
                }
            });
            std::swap(sourceKeys, targetKeys);
            std::swap(sourceOrder, targetOrder);
        }
        if (sourceOrder != &order)
        {
            order.swap(orderBack_);
        }
        const std::vector<uint32_t>& sortedKeys = *sourceKeys;
        forEachThread(threads, [&](unsigned thread)
        {
            auto [begin, end] = range(thread);
            for (size_t index = begin; index < end; ++index)
            {
                size_t firstCell = index == 0 ? 0 : sortedKeys[index - 1] + size_t{1};
                for (size_t cell = firstCell; cell <= sortedKeys[index]; ++cell)
                {
                    starts[cell] = static_cast<uint32_t>(index);
                }
            }
            if (end == count && begin < end)
            {
                for (size_t cell = sortedKeys[count - 1] + size_t{1}; cell <= cells; ++cell)
                {
                    starts[cell] = static_cast<uint32_t>(count);
                }
            }
        });
    }

private:
    static constexpr unsigned DIGIT_BITS = 8;
    static constexpr size_t RADIX = size_t{1} << DIGIT_BITS;

    template <class Body>
    static void forEachThread(unsigned threads, Body body)
    {
        std::vector<std::thread> pool;
        for (unsigned thread = 1; thread < threads; ++thread)
        {
            pool.emplace_back(body, thread);
        }
        body(0u);
        for (auto& worker : pool)
        {
            worker.join();
        }
    }

    std::vector<uint32_t> keysFront_;
    std::vector<uint32_t> keysBack_;
    std::vector<uint32_t> orderBack_;
    std::vector<std::vector<uint32_t>> counts_;
};
