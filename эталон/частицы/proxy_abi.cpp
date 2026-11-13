using size_t = decltype(sizeof(0));

#if defined(_MSC_VER)
#define NOINLINE __declspec(noinline)
#define RESTRICT __restrict
#else
#define NOINLINE __attribute__((noinline))
#define RESTRICT __restrict__
#endif

constexpr size_t POSITION_X = 0;
constexpr size_t POSITION_Y = 1;
constexpr size_t POSITION_Z = 2;
constexpr size_t VELOCITY_X = 3;
constexpr size_t VELOCITY_Y = 4;
constexpr size_t VELOCITY_Z = 5;
constexpr size_t CLONE_WIDTH = 8;
constexpr size_t RANGE_CHUNK = 256;

struct ParticleProxy
{
    float* base;
    size_t capacity;
    size_t index;
};

struct AosParticle
{
    float position[3];
    float velocity[3];
};

static inline float& column(float* base, size_t capacity, size_t which, size_t index)
{
    return base[which * capacity + index];
}

static inline void stepElement(float* base, size_t capacity, size_t index, float gravity, float dt)
{
    float vx = column(base, capacity, VELOCITY_X, index);
    float vy = column(base, capacity, VELOCITY_Y, index);
    float vz = column(base, capacity, VELOCITY_Z, index) - gravity * dt;
    column(base, capacity, VELOCITY_Z, index) = vz;
    column(base, capacity, POSITION_X, index) += vx * dt;
    column(base, capacity, POSITION_Y, index) += vy * dt;
    column(base, capacity, POSITION_Z, index) += vz * dt;
}

static inline void stepColumns(float* RESTRICT px, float* RESTRICT py, float* RESTRICT pz, const float* RESTRICT vx,
    const float* RESTRICT vy, float* RESTRICT vz, size_t first, size_t last, float gravity, float dt)
{
    for (size_t index = first; index < last; ++index)
    {
        float fall = vz[index] - gravity * dt;
        vz[index] = fall;
        px[index] += vx[index] * dt;
        py[index] += vy[index] * dt;
        pz[index] += fall * dt;
    }
}

NOINLINE void stepThreeWords(float* base, size_t capacity, size_t index, float gravity, float dt)
{
    stepElement(base, capacity, index, gravity, dt);
}

NOINLINE void stepProxyValue(ParticleProxy proxy, float gravity, float dt)
{
    stepElement(proxy.base, proxy.capacity, proxy.index, gravity, dt);
}

NOINLINE void stepProxyReference(const ParticleProxy& proxy, float gravity, float dt)
{
    stepElement(proxy.base, proxy.capacity, proxy.index, gravity, dt);
}

static NOINLINE void stepProxyValueInternal(ParticleProxy proxy, float gravity, float dt)
{
    stepElement(proxy.base, proxy.capacity, proxy.index, gravity, dt);
}

NOINLINE void stepEight(float* base, size_t capacity, size_t index, float gravity, float dt)
{
    stepColumns(base + POSITION_X * capacity, base + POSITION_Y * capacity, base + POSITION_Z * capacity, base + VELOCITY_X * capacity,
        base + VELOCITY_Y * capacity, base + VELOCITY_Z * capacity, index, index + CLONE_WIDTH, gravity, dt);
}

NOINLINE void stepRange(float* base, size_t capacity, size_t first, size_t last, float gravity, float dt)
{
    stepColumns(base + POSITION_X * capacity, base + POSITION_Y * capacity, base + POSITION_Z * capacity, base + VELOCITY_X * capacity,
        base + VELOCITY_Y * capacity, base + VELOCITY_Z * capacity, first, last, gravity, dt);
}

NOINLINE void stepAos(AosParticle& particle, float gravity, float dt)
{
    particle.velocity[2] -= gravity * dt;
    particle.position[0] += particle.velocity[0] * dt;
    particle.position[1] += particle.velocity[1] * dt;
    particle.position[2] += particle.velocity[2] * dt;
}

void runInline(float* base, size_t capacity, size_t count, float gravity, float dt)
{
    for (size_t index = 0; index < count; ++index)
    {
        stepElement(base, capacity, index, gravity, dt);
    }
}

void runThreeWords(float* base, size_t capacity, size_t count, float gravity, float dt)
{
    for (size_t index = 0; index < count; ++index)
    {
        stepThreeWords(base, capacity, index, gravity, dt);
    }
}

void runProxyValue(float* base, size_t capacity, size_t count, float gravity, float dt)
{
    for (size_t index = 0; index < count; ++index)
    {
        stepProxyValue(ParticleProxy{base, capacity, index}, gravity, dt);
    }
}

void runProxyReference(float* base, size_t capacity, size_t count, float gravity, float dt)
{
    for (size_t index = 0; index < count; ++index)
    {
        ParticleProxy proxy{base, capacity, index};
        stepProxyReference(proxy, gravity, dt);
    }
}

void runProxyValueInternal(float* base, size_t capacity, size_t count, float gravity, float dt)
{
    for (size_t index = 0; index < count; ++index)
    {
        stepProxyValueInternal(ParticleProxy{base, capacity, index}, gravity, dt);
    }
}

void runEight(float* base, size_t capacity, size_t count, float gravity, float dt)
{
    size_t index = 0;
    for (; index + CLONE_WIDTH <= count; index += CLONE_WIDTH)
    {
        stepEight(base, capacity, index, gravity, dt);
    }
    for (; index < count; ++index)
    {
        stepThreeWords(base, capacity, index, gravity, dt);
    }
}

void runRange(float* base, size_t capacity, size_t count, float gravity, float dt)
{
    for (size_t first = 0; first < count; first += RANGE_CHUNK)
    {
        stepRange(base, capacity, first, first + RANGE_CHUNK < count ? first + RANGE_CHUNK : count, gravity, dt);
    }
}

void runAos(AosParticle* particles, size_t count, float gravity, float dt)
{
    for (size_t index = 0; index < count; ++index)
    {
        stepAos(particles[index], gravity, dt);
    }
}
