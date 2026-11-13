struct World
{
    float size;
    uint32 cellsPerSide;
    float cellSize;
    float diameter;
    float stiffness;
    float gravity;
    float timeStep;
    uint32 particlesPerSide;
    float latticeStep;
    float initialSpeed;
};

static inline int cellCoordinate(float coordinate, uniform const World& world)
{
    int raw = (int)floor(coordinate / world.cellSize);
    return clamp(raw, 0, (uniform int)world.cellsPerSide - 1);
}

static inline float wall(float coordinate, uniform const World& world)
{
    uniform float radius = world.diameter * 0.5f;
    float result = 0.0f;
    if (coordinate < radius)
    {
        result = world.stiffness * (radius - coordinate);
    }
    else
    {
        uniform float top = world.size - radius;
        if (coordinate > top)
        {
            result = world.stiffness * (top - coordinate);
        }
    }
    return result;
}

export void UPDATE_NAME(
    uniform int begin,
    uniform int end,
    uniform const float px[],
    uniform const float py[],
    uniform const float pz[],
    uniform const float vx[],
    uniform const float vy[],
    uniform const float vz[],
    uniform const uint32 id[],
    uniform const uint32 starts[],
    uniform const World& world,
    uniform float outPx[],
    uniform float outPy[],
    uniform float outPz[],
    uniform float outVx[],
    uniform float outVy[],
    uniform float outVz[],
    uniform uint32 outId[])
{
    uniform int side = (int)world.cellsPerSide;
    foreach (index = begin ... end)
    {
        float x = px[index];
        float y = py[index];
        float z = pz[index];
        int cellX = cellCoordinate(x, world);
        int cellY = cellCoordinate(y, world);
        int cellZ = cellCoordinate(z, world);
        float ax = wall(x, world) - 0.0f;
        float ay = wall(y, world) - 0.0f;
        float az = wall(z, world) - world.gravity;
        for (uniform int neighbour = 0; neighbour < 27; ++neighbour)
        {
            int nx = cellX + (neighbour % 3) - 1;
            int ny = cellY + (neighbour / 3 % 3) - 1;
            int nz = cellZ + (neighbour / 9) - 1;
            if (nx >= 0 && nx < side && ny >= 0 && ny < side && nz >= 0 && nz < side)
            {
                int flat = (nz * side + ny) * side + nx;
                uint32 first = starts[flat];
                uint32 last = starts[flat + 1];
                for (uint32 other = first; other < last; ++other)
                {
                    if (other != (uint32)index)
                    {
                        float dx = x - px[other];
                        float dy = y - py[other];
                        float dz = z - pz[other];
                        float square = dx * dx + dy * dy + dz * dz;
                        float fx = 0.0f;
                        float fy = 0.0f;
                        float fz = 0.0f;
                        if (!(square >= world.diameter * world.diameter || square == 0.0f))
                        {
                            float distance = sqrt(square);
                            float factor = world.stiffness * (world.diameter - distance) / distance;
                            fx = dx * factor;
                            fy = dy * factor;
                            fz = dz * factor;
                        }
                        ax = ax + fx;
                        ay = ay + fy;
                        az = az + fz;
                    }
                }
            }
        }
        float newVx = vx[index] + ax * world.timeStep;
        float newVy = vy[index] + ay * world.timeStep;
        float newVz = vz[index] + az * world.timeStep;
        outPx[index] = x + newVx * world.timeStep;
        outPy[index] = y + newVy * world.timeStep;
        outPz[index] = z + newVz * world.timeStep;
        outVx[index] = newVx;
        outVy[index] = newVy;
        outVz[index] = newVz;
        outId[index] = id[index];
    }
}
