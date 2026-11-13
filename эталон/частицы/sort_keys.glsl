layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;

layout(std430, binding = 0) readonly buffer StatePx { float px[]; };
layout(std430, binding = 1) readonly buffer StatePy { float py[]; };
layout(std430, binding = 2) readonly buffer StatePz { float pz[]; };
layout(std430, binding = 3) writeonly buffer Keys { uint keys[]; };
layout(std430, binding = 4) buffer Counts { uint counts[]; };

#include "world.glsl"
#include "cell.glsl"

void main()
{
    uint idx = gl_GlobalInvocationID.x;
    if (idx >= world.particleCount)
    {
        return;
    }
    precise float ratioX = cellRatio(px[idx]);
    precise float ratioY = cellRatio(py[idx]);
    precise float ratioZ = cellRatio(pz[idx]);
    int side = int(world.cellsPerSide);
    uint key = uint((cellCoord(ratioZ) * side + cellCoord(ratioY)) * side + cellCoord(ratioX));
    keys[idx] = key;
    atomicAdd(counts[key], 1u);
}
