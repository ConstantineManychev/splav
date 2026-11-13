layout(push_constant) uniform PushWorld
{
    float size;
    uint cellsPerSide;
    float cellSize;
    float diameter;
    float stiffness;
    float gravity;
    float timeStep;
    uint particleCount;
    uint traceIndex;
} world;
