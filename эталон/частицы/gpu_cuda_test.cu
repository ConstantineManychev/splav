#include "model.hpp"
#include <cuda_runtime.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

static void checkCuda(cudaError_t res, const char* opName)
{
    if (res != cudaSuccess)
    {
        std::fprintf(stderr, "CUDA error in %s: %s\n", opName, cudaGetErrorString(res));
        std::exit(1);
    }
}

__device__ __forceinline__ int32_t devCellCoord(float coord, World world)
{
    float ratio = __fdiv_rn(coord, world.cellSize);
    int32_t raw = static_cast<int32_t>(floorf(ratio));
    int32_t limit = static_cast<int32_t>(world.cellsPerSide) - 1;
    return raw < 0 ? 0 : (raw > limit ? limit : raw);
}

__device__ __forceinline__ float devWallStrict(float coord, World world)
{
    float radius = __fmul_rn(world.diameter, 0.5f);
    if (coord < radius)
    {
        return __fmul_rn(world.stiffness, __fsub_rn(radius, coord));
    }
    float top = __fsub_rn(world.size, radius);
    if (coord > top)
    {
        return __fmul_rn(world.stiffness, __fsub_rn(top, coord));
    }
    return 0.0f;
}

__global__ void updateKernelStrict(
    uint32_t cnt,
    const float* __restrict__ px,
    const float* __restrict__ py,
    const float* __restrict__ pz,
    const float* __restrict__ vx,
    const float* __restrict__ vy,
    const float* __restrict__ vz,
    const uint32_t* __restrict__ id,
    const uint32_t* __restrict__ starts,
    World world,
    float* __restrict__ outPx,
    float* __restrict__ outPy,
    float* __restrict__ outPz,
    float* __restrict__ outVx,
    float* __restrict__ outVy,
    float* __restrict__ outVz,
    uint32_t* __restrict__ outId)
{
    uint32_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= cnt)
    {
        return;
    }

    float x = px[idx];
    float y = py[idx];
    float z = pz[idx];

    int32_t cellX = devCellCoord(x, world);
    int32_t cellY = devCellCoord(y, world);
    int32_t cellZ = devCellCoord(z, world);
    int32_t side = static_cast<int32_t>(world.cellsPerSide);

    float ax = __fsub_rn(devWallStrict(x, world), 0.0f);
    float ay = __fsub_rn(devWallStrict(y, world), 0.0f);
    float az = __fsub_rn(devWallStrict(z, world), world.gravity);

    float diamSq = __fmul_rn(world.diameter, world.diameter);

    for (int32_t nbr = 0; nbr < 27; ++nbr)
    {
        int nx = cellX + (nbr % 3) - 1;
        int ny = cellY + ((nbr / 3) % 3) - 1;
        int nz = cellZ + (nbr / 9) - 1;

        if (nx >= 0 && nx < side && ny >= 0 && ny < side && nz >= 0 && nz < side)
        {
            int flatIdx = (nz * side + ny) * side + nx;
            uint32_t first = starts[flatIdx];
            uint32_t last = starts[flatIdx + 1];

            for (uint32_t other = first; other < last; ++other)
            {
                if (other != idx)
                {
                    float dx = __fsub_rn(x, px[other]);
                    float dy = __fsub_rn(y, py[other]);
                    float dz = __fsub_rn(z, pz[other]);

                    float dx2 = __fmul_rn(dx, dx);
                    float dy2 = __fmul_rn(dy, dy);
                    float dz2 = __fmul_rn(dz, dz);
                    float sq = __fadd_rn(__fadd_rn(dx2, dy2), dz2);

                    if (!(sq >= diamSq || sq == 0.0f))
                    {
                        float dist = __fsqrt_rn(sq);
                        float overlap = __fsub_rn(world.diameter, dist);
                        float num = __fmul_rn(world.stiffness, overlap);
                        float factor = __fdiv_rn(num, dist);

                        ax = __fadd_rn(ax, __fmul_rn(dx, factor));
                        ay = __fadd_rn(ay, __fmul_rn(dy, factor));
                        az = __fadd_rn(az, __fmul_rn(dz, factor));
                    }
                }
            }
        }
    }

    float newVx = __fadd_rn(vx[idx], __fmul_rn(ax, world.timeStep));
    float newVy = __fadd_rn(vy[idx], __fmul_rn(ay, world.timeStep));
    float newVz = __fadd_rn(vz[idx], __fmul_rn(az, world.timeStep));

    outPx[idx] = __fadd_rn(x, __fmul_rn(newVx, world.timeStep));
    outPy[idx] = __fadd_rn(y, __fmul_rn(newVy, world.timeStep));
    outPz[idx] = __fadd_rn(z, __fmul_rn(newVz, world.timeStep));

    outVx[idx] = newVx;
    outVy[idx] = newVy;
    outVz[idx] = newVz;
    outId[idx] = id[idx];
}

struct GpuBuffers
{
    float *px, *py, *pz, *vx, *vy, *vz;
    float *outPx, *outPy, *outPz, *outVx, *outVy, *outVz;
    uint32_t *id, *outId, *starts;

    void alloc(size_t pCnt, size_t cCnt)
    {
        size_t fBytes = pCnt * sizeof(float);
        size_t uBytes = pCnt * sizeof(uint32_t);
        size_t sBytes = (cCnt + 1) * sizeof(uint32_t);

        checkCuda(cudaMalloc(&px, fBytes), "alloc px");
        checkCuda(cudaMalloc(&py, fBytes), "alloc py");
        checkCuda(cudaMalloc(&pz, fBytes), "alloc pz");
        checkCuda(cudaMalloc(&vx, fBytes), "alloc vx");
        checkCuda(cudaMalloc(&vy, fBytes), "alloc vy");
        checkCuda(cudaMalloc(&vz, fBytes), "alloc vz");
        checkCuda(cudaMalloc(&id, uBytes), "alloc id");

        checkCuda(cudaMalloc(&outPx, fBytes), "alloc outPx");
        checkCuda(cudaMalloc(&outPy, fBytes), "alloc outPy");
        checkCuda(cudaMalloc(&outPz, fBytes), "alloc outPz");
        checkCuda(cudaMalloc(&outVx, fBytes), "alloc outVx");
        checkCuda(cudaMalloc(&outVy, fBytes), "alloc outVy");
        checkCuda(cudaMalloc(&outVz, fBytes), "alloc outVz");
        checkCuda(cudaMalloc(&outId, uBytes), "alloc outId");
        checkCuda(cudaMalloc(&starts, sBytes), "alloc starts");
    }

    void freeAll()
    {
        for (void* ptr : {
            static_cast<void*>(px), static_cast<void*>(py), static_cast<void*>(pz),
            static_cast<void*>(vx), static_cast<void*>(vy), static_cast<void*>(vz),
            static_cast<void*>(outPx), static_cast<void*>(outPy), static_cast<void*>(outPz),
            static_cast<void*>(outVx), static_cast<void*>(outVy), static_cast<void*>(outVz),
            static_cast<void*>(id), static_cast<void*>(outId), static_cast<void*>(starts)})
        {
            cudaFree(ptr);
        }
    }

    void upload(const Columns& cols, const std::vector<uint32_t>& hostStarts)
    {
        size_t fBytes = cols.size() * sizeof(float);
        size_t uBytes = cols.size() * sizeof(uint32_t);
        size_t sBytes = hostStarts.size() * sizeof(uint32_t);

        checkCuda(cudaMemcpy(px, cols.px.data(), fBytes, cudaMemcpyHostToDevice), "up px");
        checkCuda(cudaMemcpy(py, cols.py.data(), fBytes, cudaMemcpyHostToDevice), "up py");
        checkCuda(cudaMemcpy(pz, cols.pz.data(), fBytes, cudaMemcpyHostToDevice), "up pz");
        checkCuda(cudaMemcpy(vx, cols.vx.data(), fBytes, cudaMemcpyHostToDevice), "up vx");
        checkCuda(cudaMemcpy(vy, cols.vy.data(), fBytes, cudaMemcpyHostToDevice), "up vy");
        checkCuda(cudaMemcpy(vz, cols.vz.data(), fBytes, cudaMemcpyHostToDevice), "up vz");
        checkCuda(cudaMemcpy(id, cols.id.data(), uBytes, cudaMemcpyHostToDevice), "up id");
        checkCuda(cudaMemcpy(starts, hostStarts.data(), sBytes, cudaMemcpyHostToDevice), "up starts");
    }

    void download(Columns& cols)
    {
        size_t fBytes = cols.size() * sizeof(float);
        size_t uBytes = cols.size() * sizeof(uint32_t);

        checkCuda(cudaMemcpy(cols.px.data(), outPx, fBytes, cudaMemcpyDeviceToHost), "down px");
        checkCuda(cudaMemcpy(cols.py.data(), outPy, fBytes, cudaMemcpyDeviceToHost), "down py");
        checkCuda(cudaMemcpy(cols.pz.data(), outPz, fBytes, cudaMemcpyDeviceToHost), "down pz");
        checkCuda(cudaMemcpy(cols.vx.data(), outVx, fBytes, cudaMemcpyDeviceToHost), "down vx");
        checkCuda(cudaMemcpy(cols.vy.data(), outVy, fBytes, cudaMemcpyDeviceToHost), "down vy");
        checkCuda(cudaMemcpy(cols.vz.data(), outVz, fBytes, cudaMemcpyDeviceToHost), "down vz");
        checkCuda(cudaMemcpy(cols.id.data(), outId, uBytes, cudaMemcpyDeviceToHost), "down id");
    }
};

static void runSimulationStep(
    World world,
    Columns& particles,
    Columns& sorted,
    std::vector<uint32_t>& keys,
    std::vector<uint32_t>& order,
    std::vector<uint32_t>& starts,
    RadixDistributor& dist,
    GpuBuffers* gpuBufs)
{
    size_t cnt = particles.size();
    unsigned threads = 2;

    parallelFor(cnt, threads, [&](size_t begin, size_t end)
    {
        for (size_t idx = begin; idx < end; ++idx)
        {
            keys[idx] = cellKey(idx, particles, world);
        }
    });

    dist.distribute(keys, cellCount(world), threads, order, starts);

    parallelFor(cnt, threads, [&](size_t begin, size_t end)
    {
        for (size_t idx = begin; idx < end; ++idx)
        {
            sorted.store(idx, particles.load(order[idx]));
        }
    });

    if (gpuBufs == nullptr)
    {
        parallelFor(cnt, threads, [&](size_t begin, size_t end)
        {
            for (size_t idx = begin; idx < end; ++idx)
            {
                particles.store(idx, updateParticle(idx, sorted, starts.data(), world));
            }
        });
    }
    else
    {
        gpuBufs->upload(sorted, starts);
        uint32_t block = 256;
        uint32_t grid = static_cast<uint32_t>((cnt + block - 1) / block);
        updateKernelStrict<<<grid, block>>>(
            static_cast<uint32_t>(cnt),
            gpuBufs->px, gpuBufs->py, gpuBufs->pz,
            gpuBufs->vx, gpuBufs->vy, gpuBufs->vz,
            gpuBufs->id, gpuBufs->starts, world,
            gpuBufs->outPx, gpuBufs->outPy, gpuBufs->outPz,
            gpuBufs->outVx, gpuBufs->outVy, gpuBufs->outVz,
            gpuBufs->outId);
        checkCuda(cudaGetLastError(), "launch updateKernelStrict");
        checkCuda(cudaDeviceSynchronize(), "sync updateKernelStrict");
        gpuBufs->download(particles);
    }
}

int main()
{
    cudaDeviceProp prop{};
    checkCuda(cudaGetDeviceProperties(&prop, 0), "getDeviceProperties");
    std::printf("CUDA Устройство: %s (SM %d.%d)\n", prop.name, prop.major, prop.minor);

    uint32_t side = 32;
    size_t steps = 200;
    World world = worldFor(side);
    size_t cnt = particleCount(world);

    Columns cpuParticles, cpuSorted, gpuParticles, gpuSorted;
    cpuParticles.resize(cnt);
    cpuSorted.resize(cnt);
    gpuParticles.resize(cnt);
    gpuSorted.resize(cnt);

    for (size_t idx = 0; idx < cnt; ++idx)
    {
        Particle initP = initialParticle(idx, world);
        cpuParticles.store(idx, initP);
        gpuParticles.store(idx, initP);
    }

    std::vector<uint32_t> cpuKeys(cnt), cpuOrder(cnt), cpuStarts(cellCount(world) + 1);
    std::vector<uint32_t> gpuKeys(cnt), gpuOrder(cnt), gpuStarts(cellCount(world) + 1);
    RadixDistributor cpuDist, gpuDist;

    GpuBuffers gpuBufs{};
    gpuBufs.alloc(cnt, cellCount(world));

    for (size_t step = 0; step < steps; ++step)
    {
        runSimulationStep(world, cpuParticles, cpuSorted, cpuKeys, cpuOrder, cpuStarts, cpuDist, nullptr);
        runSimulationStep(world, gpuParticles, gpuSorted, gpuKeys, gpuOrder, gpuStarts, gpuDist, &gpuBufs);
    }

    bool equal = cpuParticles.bitwiseEqual(gpuParticles);
    std::printf("%s ЦПУ и CUDA ГПУ (%s) совпадают побитно: %zu частиц, %zu шагов\n",
        equal ? "✓" : "✗", prop.name, cnt, steps);

    gpuBufs.freeAll();

    uint32_t benchSide = 100;
    World benchWorld = worldFor(benchSide);
    size_t benchCnt = particleCount(benchWorld);
    Columns benchParticles, benchSorted;
    benchParticles.resize(benchCnt);
    benchSorted.resize(benchCnt);
    for (size_t idx = 0; idx < benchCnt; ++idx)
    {
        benchParticles.store(idx, initialParticle(idx, benchWorld));
    }
    std::vector<uint32_t> bKeys(benchCnt), bOrder(benchCnt), bStarts(cellCount(benchWorld) + 1);
    RadixDistributor bDist;
    runSimulationStep(benchWorld, benchParticles, benchSorted, bKeys, bOrder, bStarts, bDist, nullptr);

    GpuBuffers benchGpu{};
    benchGpu.alloc(benchCnt, cellCount(benchWorld));
    benchGpu.upload(benchSorted, bStarts);

    uint32_t block = 256;
    uint32_t grid = static_cast<uint32_t>((benchCnt + block - 1) / block);

    for (int warmup = 0; warmup < 10; ++warmup)
    {
        updateKernelStrict<<<grid, block>>>(
            static_cast<uint32_t>(benchCnt),
            benchGpu.px, benchGpu.py, benchGpu.pz,
            benchGpu.vx, benchGpu.vy, benchGpu.vz,
            benchGpu.id, benchGpu.starts, benchWorld,
            benchGpu.outPx, benchGpu.outPy, benchGpu.outPz,
            benchGpu.outVx, benchGpu.outVy, benchGpu.outVz,
            benchGpu.outId);
    }
    checkCuda(cudaDeviceSynchronize(), "warmup sync");

    size_t benchSteps = 100;
    auto start = std::chrono::steady_clock::now();
    for (size_t s = 0; s < benchSteps; ++s)
    {
        updateKernelStrict<<<grid, block>>>(
            static_cast<uint32_t>(benchCnt),
            benchGpu.px, benchGpu.py, benchGpu.pz,
            benchGpu.vx, benchGpu.vy, benchGpu.vz,
            benchGpu.id, benchGpu.starts, benchWorld,
            benchGpu.outPx, benchGpu.outPy, benchGpu.outPz,
            benchGpu.outVx, benchGpu.outVy, benchGpu.outVz,
            benchGpu.outId);
    }
    checkCuda(cudaDeviceSynchronize(), "bench sync");
    double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    double msPerStep = (elapsed / benchSteps) * 1000.0;
    std::printf("CUDA ядро в GDDR6 (1 млн частиц, строгий IEEE-754): %.3f мс/шаг (%.1f млн частиц-шагов/с)\n",
        msPerStep, (benchCnt / (elapsed / benchSteps)) / 1e6);

    benchGpu.freeAll();
    return equal ? 0 : 1;
}
