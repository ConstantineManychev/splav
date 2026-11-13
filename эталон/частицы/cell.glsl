precise float cellRatio(float coord)
{
    precise float ratio = DIVIDE(coord, world.cellSize);
    return ratio;
}

int cellCoord(float ratio)
{
    int raw = int(floor(ratio));
    int limit = int(world.cellsPerSide) - 1;
    return clamp(raw, 0, limit);
}
