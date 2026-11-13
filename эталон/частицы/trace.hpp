#pragma once

#include "model.hpp"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

constexpr uint32_t NO_TRACE = UINT32_MAX;

enum TraceSlot : uint32_t
{
    TRACE_CANDIDATES = 0,
    TRACE_RATIO = 1,
    TRACE_CELL = 4,
    TRACE_WALL = 7,
    TRACE_DV = 10,
    TRACE_NEW_V = 13,
    TRACE_DP = 16,
    TRACE_OUT = 19,
    TRACE_INDEX = 22,
    TRACE_ID = 23,
    TRACE_ACCELERATION = 24,
    TRACE_HEADER = 28,
};

enum PairSlot : uint32_t
{
    PAIR_OTHER = 0,
    PAIR_D = 1,
    PAIR_D2 = 4,
    PAIR_SUM_XY = 7,
    PAIR_SQ = 8,
    PAIR_IN_RANGE = 9,
    PAIR_DIST = 10,
    PAIR_OVERLAP = 11,
    PAIR_NUM = 12,
    PAIR_FACTOR = 13,
    PAIR_F = 14,
    PAIR_A = 17,
    PAIR_SLOTS = 20,
    PAIR_LIMIT = 96,
};

constexpr uint32_t TRACE_WORDS = TRACE_HEADER + PAIR_SLOTS * PAIR_LIMIT;

inline std::string formatted(const char* pattern, ...)
{
    va_list args;
    va_start(args, pattern);
    va_list measured;
    va_copy(measured, args);
    int length = std::vsnprintf(nullptr, 0, pattern, measured);
    va_end(measured);
    std::string text(length > 0 ? static_cast<size_t>(length) : 0, '\0');
    std::vsnprintf(text.data(), text.size() + 1, pattern, args);
    va_end(args);
    return text;
}

inline uint32_t toBits(float value)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, 4);
    return bits;
}

inline float fromBits(uint32_t bits)
{
    float value = 0.0f;
    std::memcpy(&value, &bits, 4);
    return value;
}

inline uint32_t ulpDistance(uint32_t got, uint32_t want)
{
    auto ordered = [](uint32_t bits)
    {
        return (bits & 0x80000000u) ? -static_cast<int64_t>(bits & 0x7FFFFFFFu) : static_cast<int64_t>(bits);
    };
    int64_t delta = ordered(got) - ordered(want);
    return static_cast<uint32_t>(std::min<int64_t>(delta < 0 ? -delta : delta, UINT32_MAX));
}

static const char* const FIELD_NAMES[6] = {"px", "py", "pz", "vx", "vy", "vz"};

inline const std::vector<float>& fieldOf(const Columns& state, int which)
{
    const std::vector<float>* fields[6] = {&state.px, &state.py, &state.pz, &state.vx, &state.vy, &state.vz};
    return *fields[which];
}

struct StateHash
{
    uint64_t value = 0xCBF29CE484222325ull;

    void add(uint32_t word)
    {
        value = (value ^ word) * 0x100000001B3ull;
    }
};

inline uint64_t hashInOrder(const Columns& state)
{
    StateHash hash;
    for (size_t idx = 0; idx < state.size(); ++idx)
    {
        for (int which = 0; which < 6; ++which)
        {
            hash.add(toBits(fieldOf(state, which)[idx]));
        }
        hash.add(state.id[idx]);
    }
    return hash.value;
}

inline std::vector<uint32_t> placeById(const Columns& state)
{
    std::vector<uint32_t> place(state.size(), UINT32_MAX);
    for (size_t idx = 0; idx < state.size(); ++idx)
    {
        if (state.id[idx] < place.size())
        {
            place[state.id[idx]] = static_cast<uint32_t>(idx);
        }
    }
    return place;
}

inline uint64_t hashById(const Columns& state)
{
    std::vector<uint32_t> place = placeById(state);
    StateHash hash;
    for (uint32_t where : place)
    {
        for (int which = 0; which < 6; ++which)
        {
            hash.add(where == UINT32_MAX ? UINT32_MAX : toBits(fieldOf(state, which)[where]));
        }
    }
    return hash.value;
}

inline std::string showBits(uint32_t bits)
{
    return formatted("0x%08X (%.9g)", bits, static_cast<double>(fromBits(bits)));
}

inline std::string particleValues(const Columns& want, uint32_t wantIndex, const Columns& got, uint32_t gotIndex)
{
    std::string text;
    for (int which = 0; which < 6; ++which)
    {
        uint32_t wantBits = toBits(fieldOf(want, which)[wantIndex]);
        uint32_t gotBits = toBits(fieldOf(got, which)[gotIndex]);
        if (wantBits != gotBits)
        {
            text += formatted("%s%s ЦПУ %s, ГПУ %s, %u ulp", text.empty() ? "" : "; ", FIELD_NAMES[which], showBits(wantBits).c_str(),
                showBits(gotBits).c_str(), ulpDistance(gotBits, wantBits));
        }
    }
    return text.empty() ? "значения совпадают" : text;
}

struct StateDifference
{
    size_t differing = 0;
    size_t missing = 0;
    size_t perField[6] = {};
    uint32_t maxUlp[6] = {};
    double maxDelta[6] = {};
    uint32_t firstId = UINT32_MAX;
};

inline StateDifference compareById(const Columns& want, const Columns& got)
{
    StateDifference diff;
    std::vector<uint32_t> wantPlace = placeById(want);
    std::vector<uint32_t> gotPlace = placeById(got);
    for (uint32_t ident = 0; ident < wantPlace.size(); ++ident)
    {
        if (wantPlace[ident] == UINT32_MAX || ident >= gotPlace.size() || gotPlace[ident] == UINT32_MAX)
        {
            ++diff.missing;
            continue;
        }
        bool differs = false;
        for (int which = 0; which < 6; ++which)
        {
            float wantValue = fieldOf(want, which)[wantPlace[ident]];
            float gotValue = fieldOf(got, which)[gotPlace[ident]];
            if (toBits(wantValue) != toBits(gotValue))
            {
                differs = true;
                ++diff.perField[which];
                diff.maxUlp[which] = std::max(diff.maxUlp[which], ulpDistance(toBits(gotValue), toBits(wantValue)));
                diff.maxDelta[which] = std::max(diff.maxDelta[which],
                    std::fabs(static_cast<double>(gotValue) - static_cast<double>(wantValue)));
            }
        }
        if (differs)
        {
            ++diff.differing;
            diff.firstId = std::min(diff.firstId, ident);
        }
    }
    return diff;
}

inline bool sameState(const StateDifference& diff)
{
    return diff.differing == 0 && diff.missing == 0;
}

inline void printMismatch(uint32_t step, const StateDifference& diff, const Columns& want, const Columns& got, uint64_t wantHash)
{
    std::string fields;
    for (int which = 0; which < 6; ++which)
    {
        if (diff.perField[which] > 0)
        {
            fields += formatted("%s%s %zu (до %u ulp, |Δ| до %.3g)", fields.empty() ? "" : ", ", FIELD_NAMES[which],
                diff.perField[which], diff.maxUlp[which], diff.maxDelta[which]);
        }
    }
    std::printf("      шаг %3u: различаются %zu из %zu частиц — %s; сумма эталона 0x%016llX, ГПУ 0x%016llX\n", step, diff.differing,
        want.size(), fields.c_str(), static_cast<unsigned long long>(wantHash), static_cast<unsigned long long>(hashById(got)));
    if (diff.missing > 0)
    {
        std::printf("        у ГПУ потеряно %zu номеров частиц\n", diff.missing);
    }
    if (diff.firstId != UINT32_MAX)
    {
        std::printf("        первая — № %u: %s\n", diff.firstId,
            particleValues(want, placeById(want)[diff.firstId], got, placeById(got)[diff.firstId]).c_str());
    }
}

struct CheckpointLog
{
    size_t particles = 0;
    std::vector<uint32_t> matched;

    void flush()
    {
        if (matched.empty())
        {
            return;
        }
        std::string steps;
        for (uint32_t step : matched)
        {
            steps += formatted("%s%u", steps.empty() ? "" : ", ", step);
        }
        std::printf("      %s %s: все %zu частиц побитно равны эталону во всех шести полях\n", matched.size() == 1 ? "шаг" : "шаги",
            steps.c_str(), particles);
        matched.clear();
    }

    size_t check(uint32_t step, const Columns& want, const Columns& got, uint64_t wantHash)
    {
        StateDifference diff = compareById(want, got);
        if (sameState(diff))
        {
            matched.push_back(step);
            return 0;
        }
        flush();
        printMismatch(step, diff, want, got, wantHash);
        return diff.differing + diff.missing;
    }
};

struct Contacts
{
    size_t pairs = 0;
    size_t walls = 0;
};

inline Contacts countContacts(const World& world, const Columns& sorted, const std::vector<uint32_t>& starts)
{
    Contacts contacts;
    for (uint32_t index = 0; index < sorted.size(); ++index)
    {
        Vec3 own{sorted.px[index], sorted.py[index], sorted.pz[index]};
        Vec3 outside = externalAcceleration(own, world);
        contacts.walls += outside.x != 0.0f || outside.y != 0.0f || outside.z != -world.gravity ? 1 : 0;
        Cell cell = cellOf(own, world);
        for (int32_t neighbour = 0; neighbour < 27; ++neighbour)
        {
            Cell near{cell.x + neighbour % 3 - 1, cell.y + neighbour / 3 % 3 - 1, cell.z + neighbour / 9 - 1};
            if (!inside(near, world))
            {
                continue;
            }
            uint32_t flat = cellIndex(near, world);
            for (uint32_t other = starts[flat]; other < starts[flat + 1]; ++other)
            {
                Vec3 difference = subtract(own, Vec3{sorted.px[other], sorted.py[other], sorted.pz[other]});
                float square = dot(difference, difference);
                contacts.pairs += other != index && !(square >= world.diameter * world.diameter || square == 0.0f) ? 1 : 0;
            }
        }
    }
    return contacts;
}

inline std::vector<uint32_t> differingPlaces(const Columns& want, const Columns& got, size_t perField[6])
{
    std::vector<uint32_t> places;
    for (int which = 0; which < 6; ++which)
    {
        perField[which] = 0;
    }
    for (uint32_t idx = 0; idx < want.size(); ++idx)
    {
        bool differs = got.id[idx] != want.id[idx];
        for (int which = 0; which < 6; ++which)
        {
            if (toBits(fieldOf(got, which)[idx]) != toBits(fieldOf(want, which)[idx]))
            {
                ++perField[which];
                differs = true;
            }
        }
        if (differs)
        {
            places.push_back(idx);
        }
    }
    return places;
}

inline std::string fieldCounts(const size_t perField[6])
{
    std::string fields;
    for (int which = 0; which < 6; ++which)
    {
        if (perField[which] > 0)
        {
            fields += formatted("%s%s %zu", fields.empty() ? "" : ", ", FIELD_NAMES[which], perField[which]);
        }
    }
    return fields.empty() ? "только номера" : fields;
}

inline std::vector<uint32_t> traceOnCpu(const World& world, const Columns& sorted, const std::vector<uint32_t>& starts,
    uint32_t index)
{
    std::vector<uint32_t> trace(TRACE_WORDS, UINT32_MAX);
    const std::vector<float>* coordinates[3] = {&sorted.px, &sorted.py, &sorted.pz};
    float position[3] = {sorted.px[index], sorted.py[index], sorted.pz[index]};
    float velocity[3] = {sorted.vx[index], sorted.vy[index], sorted.vz[index]};
    int32_t cell[3] = {};
    float acceleration[3] = {};
    for (int axis = 0; axis < 3; ++axis)
    {
        float ratio = position[axis] / world.cellSize;
        cell[axis] = cellCoordinate(position[axis], world);
        acceleration[axis] = wall(position[axis], world) - (axis == 2 ? world.gravity : 0.0f);
        trace[TRACE_RATIO + axis] = toBits(ratio);
        trace[TRACE_CELL + axis] = static_cast<uint32_t>(cell[axis]);
        trace[TRACE_WALL + axis] = toBits(acceleration[axis]);
    }
    uint32_t candidates = 0;
    for (int32_t neighbour = 0; neighbour < 27; ++neighbour)
    {
        Cell near{cell[0] + neighbour % 3 - 1, cell[1] + neighbour / 3 % 3 - 1, cell[2] + neighbour / 9 - 1};
        if (!inside(near, world))
        {
            continue;
        }
        uint32_t flat = cellIndex(near, world);
        for (uint32_t other = starts[flat]; other < starts[flat + 1]; ++other)
        {
            if (other == index)
            {
                continue;
            }
            float difference[3] = {};
            float squares[3] = {};
            for (int axis = 0; axis < 3; ++axis)
            {
                difference[axis] = position[axis] - (*coordinates[axis])[other];
                squares[axis] = difference[axis] * difference[axis];
            }
            float sumXY = squares[0] + squares[1];
            float square = sumXY + squares[2];
            bool inRange = !(square >= world.diameter * world.diameter || square == 0.0f);
            uint32_t* slot = candidates < PAIR_LIMIT ? &trace[TRACE_HEADER + PAIR_SLOTS * candidates] : nullptr;
            if (slot != nullptr)
            {
                slot[PAIR_OTHER] = other;
                for (int axis = 0; axis < 3; ++axis)
                {
                    slot[PAIR_D + axis] = toBits(difference[axis]);
                    slot[PAIR_D2 + axis] = toBits(squares[axis]);
                }
                slot[PAIR_SUM_XY] = toBits(sumXY);
                slot[PAIR_SQ] = toBits(square);
                slot[PAIR_IN_RANGE] = inRange ? 1u : 0u;
            }
            if (inRange)
            {
                float distance = std::sqrt(square);
                float overlap = world.diameter - distance;
                float numerator = world.stiffness * overlap;
                float factor = numerator / distance;
                float force[3] = {};
                for (int axis = 0; axis < 3; ++axis)
                {
                    force[axis] = difference[axis] * factor;
                    acceleration[axis] = acceleration[axis] + force[axis];
                }
                if (slot != nullptr)
                {
                    slot[PAIR_DIST] = toBits(distance);
                    slot[PAIR_OVERLAP] = toBits(overlap);
                    slot[PAIR_NUM] = toBits(numerator);
                    slot[PAIR_FACTOR] = toBits(factor);
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        slot[PAIR_F + axis] = toBits(force[axis]);
                        slot[PAIR_A + axis] = toBits(acceleration[axis]);
                    }
                }
            }
            ++candidates;
        }
    }
    trace[TRACE_CANDIDATES] = candidates;
    for (int axis = 0; axis < 3; ++axis)
    {
        float change = acceleration[axis] * world.timeStep;
        float speed = velocity[axis] + change;
        float shiftBy = speed * world.timeStep;
        trace[TRACE_ACCELERATION + axis] = toBits(acceleration[axis]);
        trace[TRACE_DV + axis] = toBits(change);
        trace[TRACE_NEW_V + axis] = toBits(speed);
        trace[TRACE_DP + axis] = toBits(shiftBy);
        trace[TRACE_OUT + axis] = toBits(position[axis] + shiftBy);
    }
    trace[TRACE_INDEX] = index;
    trace[TRACE_ID] = sorted.id[index];
    return trace;
}

inline bool traceMatchesModel(const std::vector<uint32_t>& trace, const Columns& out, uint32_t index)
{
    for (int axis = 0; axis < 3; ++axis)
    {
        if (trace[TRACE_OUT + axis] != toBits(fieldOf(out, axis)[index]) || trace[TRACE_NEW_V + axis] != toBits(fieldOf(out, 3 + axis)[index]))
        {
            return false;
        }
    }
    return true;
}

inline bool subnormalBits(uint32_t bits)
{
    uint32_t magnitude = bits & 0x7FFFFFFFu;
    return magnitude != 0u && magnitude < 0x00800000u;
}

inline bool zeroBits(uint32_t bits)
{
    return (bits & 0x7FFFFFFFu) == 0u;
}

struct Fusion
{
    float left;
    float right;
    float addend;
};

inline std::string comparison(const std::string& what, uint32_t want, uint32_t got)
{
    return formatted("%s: ЦПУ %s, ГПУ %s, %u ulp", what.c_str(), showBits(want).c_str(), showBits(got).c_str(), ulpDistance(got, want));
}

inline std::string flushVerdict(uint32_t want, uint32_t got, std::initializer_list<float> inputs)
{
    if (subnormalBits(want) && zeroBits(got))
    {
        return " — ноль вместо денормализованного результата (FTZ)";
    }
    for (float input : inputs)
    {
        if (subnormalBits(toBits(input)))
        {
            return " — на входе денормализованное число (DAZ)";
        }
    }
    return "";
}

inline std::string divisionVerdict(const std::string& what, float numerator, float denominator, uint32_t want, uint32_t got)
{
    std::string text = comparison(formatted("%s = %s / %s", what.c_str(), showBits(toBits(numerator)).c_str(),
        showBits(toBits(denominator)).c_str()), want, got);
    std::string flush = flushVerdict(want, got, {numerator, denominator});
    if (!flush.empty())
    {
        return text + flush;
    }
    volatile float inverse = 1.0f / denominator;
    if (got == toBits(numerator * inverse))
    {
        return text + " — ГПУ делит умножением на обратную величину";
    }
    return text + " — ГПУ округлил частное иначе";
}

inline std::string rootVerdict(const std::string& what, float radicand, uint32_t want, uint32_t got)
{
    std::string text = comparison(formatted("%s = корень %s", what.c_str(), showBits(toBits(radicand)).c_str()), want, got);
    std::string flush = flushVerdict(want, got, {radicand});
    return text + (flush.empty() ? " — ГПУ округлил корень иначе" : flush);
}

inline std::string productVerdict(const std::string& what, float left, float right, uint32_t want, uint32_t got)
{
    std::string text = comparison(formatted("%s = %s × %s", what.c_str(), showBits(toBits(left)).c_str(),
        showBits(toBits(right)).c_str()), want, got);
    std::string flush = flushVerdict(want, got, {left, right});
    return text + (flush.empty() ? " — умножение дало другой результат" : flush);
}

inline std::string differenceVerdict(const std::string& what, float left, float right, uint32_t want, uint32_t got)
{
    std::string text = comparison(formatted("%s = %s − %s", what.c_str(), showBits(toBits(left)).c_str(),
        showBits(toBits(right)).c_str()), want, got);
    std::string flush = flushVerdict(want, got, {left, right});
    return text + (flush.empty() ? " — вычитание дало другой результат" : flush);
}

inline std::string sumVerdict(const std::string& what, float left, float right, uint32_t want, uint32_t got,
    std::initializer_list<Fusion> fusions)
{
    std::string text = comparison(formatted("%s = %s + %s", what.c_str(), showBits(toBits(left)).c_str(),
        showBits(toBits(right)).c_str()), want, got);
    for (const Fusion& fusion : fusions)
    {
        if (got == toBits(std::fma(fusion.left, fusion.right, fusion.addend)))
        {
            return text + formatted(" — совпадает с fma(%.9g, %.9g, %.9g): ГПУ слил умножение со сложением (сжатие)",
                static_cast<double>(fusion.left), static_cast<double>(fusion.right), static_cast<double>(fusion.addend));
        }
    }
    std::string flush = flushVerdict(want, got, {left, right});
    return text + (flush.empty() ? " — сложение дало другой результат" : flush);
}

inline std::string findCause(const World& world, const Columns& sorted, uint32_t index, const std::vector<uint32_t>& cpu,
    const std::vector<uint32_t>& gpu)
{
    const char axes[] = "xyz";
    const std::vector<float>* coordinates[3] = {&sorted.px, &sorted.py, &sorted.pz};
    const std::vector<float>* velocities[3] = {&sorted.vx, &sorted.vy, &sorted.vz};
    std::string notes;
    auto withNotes = [&notes](const std::string& cause)
    {
        return notes.empty() ? cause : cause + "; раньше без последствий: " + notes;
    };

    for (int axis = 0; axis < 3; ++axis)
    {
        uint32_t want = cpu[TRACE_RATIO + axis];
        uint32_t got = gpu[TRACE_RATIO + axis];
        bool cellDiffers = cpu[TRACE_CELL + axis] != gpu[TRACE_CELL + axis];
        if (want == got && !cellDiffers)
        {
            continue;
        }
        std::string what = divisionVerdict(formatted("ячейка по %c: частное", axes[axis]), (*coordinates[axis])[index],
            world.cellSize, want, got);
        if (cellDiffers)
        {
            return withNotes(what + formatted("; ячейка на ЦПУ %d, на ГПУ %d — ГПУ обходит других соседей",
                static_cast<int32_t>(cpu[TRACE_CELL + axis]), static_cast<int32_t>(gpu[TRACE_CELL + axis])));
        }
        notes += (notes.empty() ? "" : "; ") + what + " (ячейка та же)";
    }

    float radius = world.diameter * 0.5f;
    float top = world.size - radius;
    for (int axis = 0; axis < 3; ++axis)
    {
        uint32_t want = cpu[TRACE_WALL + axis];
        uint32_t got = gpu[TRACE_WALL + axis];
        if (want != got)
        {
            float coordinate = (*coordinates[axis])[index];
            float gap = coordinate < radius ? radius - coordinate : top - coordinate;
            float gravity = axis == 2 ? world.gravity : 0.0f;
            return withNotes(sumVerdict(formatted("стена и тяжесть по %c", axes[axis]), wall(coordinate, world), -gravity, want, got,
                {Fusion{world.stiffness, gap, -gravity}}));
        }
    }

    uint32_t cpuCount = cpu[TRACE_CANDIDATES];
    uint32_t gpuCount = gpu[TRACE_CANDIDATES];
    uint32_t traced = std::min({cpuCount, gpuCount, static_cast<uint32_t>(PAIR_LIMIT)});
    float previous[3] = {fromBits(cpu[TRACE_WALL]), fromBits(cpu[TRACE_WALL + 1]), fromBits(cpu[TRACE_WALL + 2])};
    for (uint32_t pair = 0; pair < traced; ++pair)
    {
        const uint32_t* want = &cpu[TRACE_HEADER + PAIR_SLOTS * pair];
        const uint32_t* got = &gpu[TRACE_HEADER + PAIR_SLOTS * pair];
        uint32_t other = want[PAIR_OTHER];
        std::string prefix = formatted("сосед %u (№ %u): ", pair + 1, sorted.id[other]);
        if (got[PAIR_OTHER] != other)
        {
            return withNotes(prefix + formatted("ГПУ взял частицу на месте %u вместо %u — обход соседей разный", got[PAIR_OTHER], other));
        }
        float d[3] = {};
        float d2[3] = {};
        for (int axis = 0; axis < 3; ++axis)
        {
            d[axis] = fromBits(want[PAIR_D + axis]);
            d2[axis] = fromBits(want[PAIR_D2 + axis]);
            if (want[PAIR_D + axis] != got[PAIR_D + axis])
            {
                return withNotes(prefix + differenceVerdict(formatted("d%c", axes[axis]), (*coordinates[axis])[index],
                    (*coordinates[axis])[other], want[PAIR_D + axis], got[PAIR_D + axis]));
            }
        }
        for (int axis = 0; axis < 3; ++axis)
        {
            if (want[PAIR_D2 + axis] != got[PAIR_D2 + axis])
            {
                return withNotes(prefix + productVerdict(formatted("d%c²", axes[axis]), d[axis], d[axis], want[PAIR_D2 + axis],
                    got[PAIR_D2 + axis]));
            }
        }
        float sumXY = fromBits(want[PAIR_SUM_XY]);
        if (want[PAIR_SUM_XY] != got[PAIR_SUM_XY])
        {
            return withNotes(prefix + sumVerdict("dx² + dy²", d2[0], d2[1], want[PAIR_SUM_XY], got[PAIR_SUM_XY],
                {Fusion{d[0], d[0], d2[1]}, Fusion{d[1], d[1], d2[0]}}));
        }
        float square = fromBits(want[PAIR_SQ]);
        if (want[PAIR_SQ] != got[PAIR_SQ])
        {
            return withNotes(prefix + sumVerdict("|d|²", sumXY, d2[2], want[PAIR_SQ], got[PAIR_SQ], {Fusion{d[2], d[2], sumXY}}));
        }
        if (want[PAIR_IN_RANGE] != got[PAIR_IN_RANGE])
        {
            return withNotes(prefix + formatted("сравнение |d|² = %s с диаметром²: ЦПУ %s, ГПУ %s%s", showBits(want[PAIR_SQ]).c_str(),
                want[PAIR_IN_RANGE] ? "касание" : "нет касания", got[PAIR_IN_RANGE] ? "касание" : "нет касания",
                subnormalBits(want[PAIR_SQ]) ? " — денормализованный |d|² сравнён как ноль (FTZ)" : ""));
        }
        if (want[PAIR_IN_RANGE] == 0u)
        {
            continue;
        }
        float distance = fromBits(want[PAIR_DIST]);
        if (want[PAIR_DIST] != got[PAIR_DIST])
        {
            return withNotes(prefix + rootVerdict("расстояние", square, want[PAIR_DIST], got[PAIR_DIST]));
        }
        float overlap = fromBits(want[PAIR_OVERLAP]);
        if (want[PAIR_OVERLAP] != got[PAIR_OVERLAP])
        {
            return withNotes(prefix + differenceVerdict("перекрытие", world.diameter, distance, want[PAIR_OVERLAP], got[PAIR_OVERLAP]));
        }
        float numerator = fromBits(want[PAIR_NUM]);
        if (want[PAIR_NUM] != got[PAIR_NUM])
        {
            return withNotes(prefix + productVerdict("жёсткость × перекрытие", world.stiffness, overlap, want[PAIR_NUM], got[PAIR_NUM]));
        }
        float factor = fromBits(want[PAIR_FACTOR]);
        if (want[PAIR_FACTOR] != got[PAIR_FACTOR])
        {
            return withNotes(prefix + divisionVerdict("множитель силы", numerator, distance, want[PAIR_FACTOR], got[PAIR_FACTOR]));
        }
        for (int axis = 0; axis < 3; ++axis)
        {
            if (want[PAIR_F + axis] != got[PAIR_F + axis])
            {
                return withNotes(prefix + productVerdict(formatted("f%c", axes[axis]), d[axis], factor, want[PAIR_F + axis],
                    got[PAIR_F + axis]));
            }
        }
        for (int axis = 0; axis < 3; ++axis)
        {
            if (want[PAIR_A + axis] != got[PAIR_A + axis])
            {
                return withNotes(prefix + sumVerdict(formatted("a%c", axes[axis]), previous[axis], fromBits(want[PAIR_F + axis]),
                    want[PAIR_A + axis], got[PAIR_A + axis], {Fusion{d[axis], factor, previous[axis]}}));
            }
            previous[axis] = fromBits(want[PAIR_A + axis]);
        }
    }
    if (cpuCount != gpuCount)
    {
        return withNotes(formatted("число соседей: ЦПУ %u, ГПУ %u", cpuCount, gpuCount));
    }

    for (int axis = 0; axis < 3; ++axis)
    {
        if (cpu[TRACE_ACCELERATION + axis] != gpu[TRACE_ACCELERATION + axis])
        {
            return withNotes(comparison(formatted("ускорение по %c после %u соседей, за пределами трассы из %u", axes[axis], cpuCount,
                static_cast<uint32_t>(PAIR_LIMIT)), cpu[TRACE_ACCELERATION + axis], gpu[TRACE_ACCELERATION + axis]));
        }
    }
    for (int axis = 0; axis < 3; ++axis)
    {
        float acceleration = fromBits(cpu[TRACE_ACCELERATION + axis]);
        float change = fromBits(cpu[TRACE_DV + axis]);
        float speed = fromBits(cpu[TRACE_NEW_V + axis]);
        float shiftBy = fromBits(cpu[TRACE_DP + axis]);
        float velocity = (*velocities[axis])[index];
        float coordinate = (*coordinates[axis])[index];
        if (cpu[TRACE_DV + axis] != gpu[TRACE_DV + axis])
        {
            return withNotes(productVerdict(formatted("dv%c", axes[axis]), acceleration, world.timeStep, cpu[TRACE_DV + axis],
                gpu[TRACE_DV + axis]));
        }
        if (cpu[TRACE_NEW_V + axis] != gpu[TRACE_NEW_V + axis])
        {
            return withNotes(sumVerdict(formatted("v%c", axes[axis]), velocity, change, cpu[TRACE_NEW_V + axis], gpu[TRACE_NEW_V + axis],
                {Fusion{acceleration, world.timeStep, velocity}}));
        }
        if (cpu[TRACE_DP + axis] != gpu[TRACE_DP + axis])
        {
            return withNotes(productVerdict(formatted("dp%c", axes[axis]), speed, world.timeStep, cpu[TRACE_DP + axis],
                gpu[TRACE_DP + axis]));
        }
        if (cpu[TRACE_OUT + axis] != gpu[TRACE_OUT + axis])
        {
            return withNotes(sumVerdict(formatted("p%c", axes[axis]), coordinate, shiftBy, cpu[TRACE_OUT + axis], gpu[TRACE_OUT + axis],
                {Fusion{speed, world.timeStep, coordinate}}));
        }
    }
    return withNotes("все промежуточные значения трассы совпали");
}

