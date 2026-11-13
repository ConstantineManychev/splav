layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;

layout(constant_id = 0) const bool TRACE = false;

layout(std430, binding = 0) readonly buffer InPx { float px[]; };
layout(std430, binding = 1) readonly buffer InPy { float py[]; };
layout(std430, binding = 2) readonly buffer InPz { float pz[]; };
layout(std430, binding = 3) readonly buffer InVx { float vx[]; };
layout(std430, binding = 4) readonly buffer InVy { float vy[]; };
layout(std430, binding = 5) readonly buffer InVz { float vz[]; };
layout(std430, binding = 6) readonly buffer InId { uint id[]; };
layout(std430, binding = 7) readonly buffer InStarts { uint starts[]; };

layout(std430, binding = 8) writeonly buffer OutPx { float outPx[]; };
layout(std430, binding = 9) writeonly buffer OutPy { float outPy[]; };
layout(std430, binding = 10) writeonly buffer OutPz { float outPz[]; };
layout(std430, binding = 11) writeonly buffer OutVx { float outVx[]; };
layout(std430, binding = 12) writeonly buffer OutVy { float outVy[]; };
layout(std430, binding = 13) writeonly buffer OutVz { float outVz[]; };
layout(std430, binding = 14) writeonly buffer OutId { uint outId[]; };
layout(std430, binding = 15) writeonly buffer Trace { uint trace[]; };

#include "world.glsl"

const uint TRACE_CANDIDATES = 0u;
const uint TRACE_RATIO = 1u;
const uint TRACE_CELL = 4u;
const uint TRACE_WALL = 7u;
const uint TRACE_DV = 10u;
const uint TRACE_NEW_V = 13u;
const uint TRACE_DP = 16u;
const uint TRACE_OUT = 19u;
const uint TRACE_INDEX = 22u;
const uint TRACE_ID = 23u;
const uint TRACE_ACCELERATION = 24u;
const uint TRACE_HEADER = 28u;

const uint PAIR_OTHER = 0u;
const uint PAIR_D = 1u;
const uint PAIR_D2 = 4u;
const uint PAIR_SUM_XY = 7u;
const uint PAIR_SQ = 8u;
const uint PAIR_IN_RANGE = 9u;
const uint PAIR_DIST = 10u;
const uint PAIR_OVERLAP = 11u;
const uint PAIR_NUM = 12u;
const uint PAIR_FACTOR = 13u;
const uint PAIR_F = 14u;
const uint PAIR_A = 17u;
const uint PAIR_SLOTS = 20u;
const uint PAIR_LIMIT = 96u;

void traceVector(uint slot, float first, float second, float third)
{
    trace[slot] = floatBitsToUint(first);
    trace[slot + 1u] = floatBitsToUint(second);
    trace[slot + 2u] = floatBitsToUint(third);
}

#include "cell.glsl"

precise float wallForce(float coord)
{
    precise float radius = world.diameter * 0.5f;
    if (coord < radius)
    {
        precise float diff = radius - coord;
        precise float res = world.stiffness * diff;
        return res;
    }
    precise float top = world.size - radius;
    if (coord > top)
    {
        precise float diff = top - coord;
        precise float res = world.stiffness * diff;
        return res;
    }
    return 0.0f;
}

void main()
{
    uint idx = gl_GlobalInvocationID.x;
    if (idx >= world.particleCount)
    {
        return;
    }
    bool traced = TRACE && idx == world.traceIndex;

    precise float x = px[idx];
    precise float y = py[idx];
    precise float z = pz[idx];

    precise float ratioX = cellRatio(x);
    precise float ratioY = cellRatio(y);
    precise float ratioZ = cellRatio(z);
    int cellX = cellCoord(ratioX);
    int cellY = cellCoord(ratioY);
    int cellZ = cellCoord(ratioZ);
    int side = int(world.cellsPerSide);

    precise float ax = wallForce(x) - 0.0f;
    precise float ay = wallForce(y) - 0.0f;
    precise float az = wallForce(z) - world.gravity;

    if (traced)
    {
        traceVector(TRACE_RATIO, ratioX, ratioY, ratioZ);
        trace[TRACE_CELL] = uint(cellX);
        trace[TRACE_CELL + 1u] = uint(cellY);
        trace[TRACE_CELL + 2u] = uint(cellZ);
        traceVector(TRACE_WALL, ax, ay, az);
    }

    precise float diamSq = world.diameter * world.diameter;
    uint candidates = 0u;

    for (int nbr = 0; nbr < 27; ++nbr)
    {
        int nx = cellX + (nbr % 3) - 1;
        int ny = cellY + ((nbr / 3) % 3) - 1;
        int nz = cellZ + (nbr / 9) - 1;

        if (nx >= 0 && nx < side && ny >= 0 && ny < side && nz >= 0 && nz < side)
        {
            int flatIdx = (nz * side + ny) * side + nx;
            uint first = starts[flatIdx];
            uint last = starts[flatIdx + 1];

            for (uint other = first; other < last; ++other)
            {
                if (other != idx)
                {
                    precise float dx = x - px[other];
                    precise float dy = y - py[other];
                    precise float dz = z - pz[other];

                    precise float dx2 = dx * dx;
                    precise float dy2 = dy * dy;
                    precise float dz2 = dz * dz;
                    precise float sumXY = dx2 + dy2;
                    precise float sq = sumXY + dz2;
                    bool inRange = !(sq >= diamSq || sq == 0.0f);

                    uint slot = TRACE_HEADER + PAIR_SLOTS * candidates;
                    bool tracePair = traced && candidates < PAIR_LIMIT;
                    if (tracePair)
                    {
                        trace[slot + PAIR_OTHER] = other;
                        traceVector(slot + PAIR_D, dx, dy, dz);
                        traceVector(slot + PAIR_D2, dx2, dy2, dz2);
                        trace[slot + PAIR_SUM_XY] = floatBitsToUint(sumXY);
                        trace[slot + PAIR_SQ] = floatBitsToUint(sq);
                        trace[slot + PAIR_IN_RANGE] = inRange ? 1u : 0u;
                    }

                    if (inRange)
                    {
                        precise float dist = ROOT(sq);
                        precise float overlap = world.diameter - dist;
                        precise float num = world.stiffness * overlap;
                        precise float factor = DIVIDE(num, dist);

                        precise float fx = dx * factor;
                        precise float fy = dy * factor;
                        precise float fz = dz * factor;

                        ax = ax + fx;
                        ay = ay + fy;
                        az = az + fz;

                        if (tracePair)
                        {
                            trace[slot + PAIR_DIST] = floatBitsToUint(dist);
                            trace[slot + PAIR_OVERLAP] = floatBitsToUint(overlap);
                            trace[slot + PAIR_NUM] = floatBitsToUint(num);
                            trace[slot + PAIR_FACTOR] = floatBitsToUint(factor);
                            traceVector(slot + PAIR_F, fx, fy, fz);
                            traceVector(slot + PAIR_A, ax, ay, az);
                        }
                    }
                    ++candidates;
                }
            }
        }
    }

    precise float dvx = ax * world.timeStep;
    precise float dvy = ay * world.timeStep;
    precise float dvz = az * world.timeStep;

    precise float newVx = vx[idx] + dvx;
    precise float newVy = vy[idx] + dvy;
    precise float newVz = vz[idx] + dvz;

    precise float dpx = newVx * world.timeStep;
    precise float dpy = newVy * world.timeStep;
    precise float dpz = newVz * world.timeStep;

    precise float nextX = x + dpx;
    precise float nextY = y + dpy;
    precise float nextZ = z + dpz;

    outPx[idx] = nextX;
    outPy[idx] = nextY;
    outPz[idx] = nextZ;

    outVx[idx] = newVx;
    outVy[idx] = newVy;
    outVz[idx] = newVz;
    outId[idx] = id[idx];

    if (traced)
    {
        trace[TRACE_CANDIDATES] = candidates;
        traceVector(TRACE_ACCELERATION, ax, ay, az);
        traceVector(TRACE_DV, dvx, dvy, dvz);
        traceVector(TRACE_NEW_V, newVx, newVy, newVz);
        traceVector(TRACE_DP, dpx, dpy, dpz);
        traceVector(TRACE_OUT, nextX, nextY, nextZ);
        trace[TRACE_INDEX] = idx;
        trace[TRACE_ID] = id[idx];
    }
}
