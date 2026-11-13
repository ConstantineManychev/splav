#include "cpu_avx2.hpp"
#include "fma_cases.hpp"
#include "trace.hpp"

#include <dlfcn.h>

#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <random>
#include <string>
#include <vector>

using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLbitfield = unsigned int;
using GLchar = char;
using GLsizeiptr = intptr_t;
using GLintptr = intptr_t;
using GLubyte = unsigned char;
using EGLDisplay = void*;
using EGLConfig = void*;
using EGLContext = void*;
using EGLDeviceEXT = void*;
using EGLint = int32_t;
using EGLBoolean = unsigned int;
using EGLenum = unsigned int;

constexpr GLenum GL_COMPUTE_SHADER = 0x91B9;
constexpr GLenum GL_SHADER_STORAGE_BUFFER = 0x90D2;
constexpr GLenum GL_DYNAMIC_COPY = 0x88EA;
constexpr GLenum GL_COMPILE_STATUS = 0x8B81;
constexpr GLenum GL_LINK_STATUS = 0x8B82;
constexpr GLbitfield GL_SHADER_STORAGE_BARRIER_BIT = 0x00002000;
constexpr GLbitfield GL_ALL_BARRIER_BITS = 0xFFFFFFFF;
constexpr GLbitfield GL_MAP_READ_BIT = 0x0001;
constexpr GLenum GL_RENDERER = 0x1F01;
constexpr GLenum GL_VERSION = 0x1F02;
constexpr EGLint EGL_NONE = 0x3038;
constexpr EGLenum EGL_OPENGL_ES_API = 0x30A0;
constexpr EGLenum EGL_OPENGL_API = 0x30A2;
constexpr EGLint EGL_CONTEXT_OPENGL_PROFILE_MASK = 0x30FD;
constexpr EGLint EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT = 0x00000001;
constexpr EGLint EGL_CONTEXT_MAJOR_VERSION = 0x3098;
constexpr EGLint EGL_CONTEXT_MINOR_VERSION = 0x30FB;
constexpr EGLenum EGL_PLATFORM_DEVICE_EXT = 0x313F;
constexpr EGLenum EGL_PLATFORM_SURFACELESS_MESA = 0x31DD;

struct Gl
{
    GLuint (*createShader)(GLenum);
    void (*shaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*);
    void (*compileShader)(GLuint);
    void (*getShaderiv)(GLuint, GLenum, GLint*);
    void (*getShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
    GLuint (*createProgram)();
    void (*attachShader)(GLuint, GLuint);
    void (*linkProgram)(GLuint);
    void (*getProgramiv)(GLuint, GLenum, GLint*);
    void (*getProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
    void (*useProgram)(GLuint);
    void (*genBuffers)(GLsizei, GLuint*);
    void (*bindBuffer)(GLenum, GLuint);
    void (*bufferData)(GLenum, GLsizeiptr, const void*, GLenum);
    void (*bufferSubData)(GLenum, GLintptr, GLsizeiptr, const void*);
    void (*bindBufferBase)(GLenum, GLuint, GLuint);
    void* (*mapBufferRange)(GLenum, GLintptr, GLsizeiptr, GLbitfield);
    GLubyte (*unmapBuffer)(GLenum);
    void (*dispatchCompute)(GLuint, GLuint, GLuint);
    void (*memoryBarrier)(GLbitfield);
    void (*finish)();
    GLint (*getUniformLocation)(GLuint, const GLchar*);
    void (*uniform1ui)(GLint, GLuint);
    void (*uniform1i)(GLint, GLint);
    void (*uniform1f)(GLint, float);
    const GLubyte* (*getString)(GLenum);
};

static Gl gl;
static std::string shaderVersion = "#version 320 es\n";

template <class Function>
void load(void* library, void* (*getProc)(const char*), Function& target, const char* name)
{
    void* address = library ? dlsym(library, name) : nullptr;
    if (!address && getProc)
    {
        address = getProc(name);
    }
    if (!address)
    {
        std::fprintf(stderr, "нет функции %s\n", name);
        std::exit(2);
    }
    target = reinterpret_cast<Function>(address);
}

bool createContext(int requestedDevice, bool desktop, std::string& description)
{
    void* egl = dlopen("libEGL.so.1", RTLD_NOW);
    void* gles = dlopen("libGLESv2.so.2", RTLD_NOW);
    if (!egl || !gles)
    {
        description = "нет libEGL.so.1 или libGLESv2.so.2";
        return false;
    }
    auto getProc = reinterpret_cast<void* (*)(const char*)>(dlsym(egl, "eglGetProcAddress"));
    auto initialize = reinterpret_cast<EGLBoolean (*)(EGLDisplay, EGLint*, EGLint*)>(dlsym(egl, "eglInitialize"));
    auto bindApi = reinterpret_cast<EGLBoolean (*)(EGLenum)>(dlsym(egl, "eglBindAPI"));
    auto createContextFunction = reinterpret_cast<EGLContext (*)(EGLDisplay, EGLConfig, EGLContext, const EGLint*)>(
        dlsym(egl, "eglCreateContext"));
    auto makeCurrent = reinterpret_cast<EGLBoolean (*)(EGLDisplay, void*, void*, EGLContext)>(dlsym(egl, "eglMakeCurrent"));
    auto getPlatformDisplay = reinterpret_cast<EGLDisplay (*)(EGLenum, void*, const EGLint*)>(getProc("eglGetPlatformDisplayEXT"));
    auto queryDevices = reinterpret_cast<EGLBoolean (*)(EGLint, EGLDeviceEXT*, EGLint*)>(getProc("eglQueryDevicesEXT"));
    std::vector<EGLDisplay> displays;
    if (queryDevices && getPlatformDisplay)
    {
        EGLDeviceEXT devices[16];
        EGLint count = 0;
        if (queryDevices(16, devices, &count))
        {
            for (EGLint index = 0; index < count; ++index)
            {
                displays.push_back(getPlatformDisplay(EGL_PLATFORM_DEVICE_EXT, devices[index], nullptr));
            }
        }
    }
    if (displays.empty() && getPlatformDisplay)
    {
        displays.push_back(getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, nullptr, nullptr));
    }
    EGLint esAttributes[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2, EGL_NONE};
    EGLint desktopAttributes[] = {EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
    const EGLint* contextAttributes = desktop ? desktopAttributes : esAttributes;
    if (desktop)
    {
        shaderVersion = "#version 430\n";
    }
    int seen = 0;
    for (size_t index = 0; index < displays.size(); ++index)
    {
        EGLDisplay display = displays[index];
        EGLint major = 0;
        EGLint minor = 0;
        if (!display || !initialize(display, &major, &minor))
        {
            continue;
        }
        bindApi(desktop ? EGL_OPENGL_API : EGL_OPENGL_ES_API);
        EGLContext context = createContextFunction(display, nullptr, nullptr, contextAttributes);
        if (!context || !makeCurrent(display, nullptr, nullptr, context))
        {
            continue;
        }
        if (requestedDevice >= 0 && seen++ != requestedDevice)
        {
            continue;
        }
        if (desktop)
        {
            gles = nullptr;
        }
        load(gles, getProc, gl.createShader, "glCreateShader");
        load(gles, getProc, gl.shaderSource, "glShaderSource");
        load(gles, getProc, gl.compileShader, "glCompileShader");
        load(gles, getProc, gl.getShaderiv, "glGetShaderiv");
        load(gles, getProc, gl.getShaderInfoLog, "glGetShaderInfoLog");
        load(gles, getProc, gl.createProgram, "glCreateProgram");
        load(gles, getProc, gl.attachShader, "glAttachShader");
        load(gles, getProc, gl.linkProgram, "glLinkProgram");
        load(gles, getProc, gl.getProgramiv, "glGetProgramiv");
        load(gles, getProc, gl.getProgramInfoLog, "glGetProgramInfoLog");
        load(gles, getProc, gl.useProgram, "glUseProgram");
        load(gles, getProc, gl.genBuffers, "glGenBuffers");
        load(gles, getProc, gl.bindBuffer, "glBindBuffer");
        load(gles, getProc, gl.bufferData, "glBufferData");
        load(gles, getProc, gl.bufferSubData, "glBufferSubData");
        load(gles, getProc, gl.bindBufferBase, "glBindBufferBase");
        load(gles, getProc, gl.mapBufferRange, "glMapBufferRange");
        load(gles, getProc, gl.unmapBuffer, "glUnmapBuffer");
        load(gles, getProc, gl.dispatchCompute, "glDispatchCompute");
        load(gles, getProc, gl.memoryBarrier, "glMemoryBarrier");
        load(gles, getProc, gl.finish, "glFinish");
        load(gles, getProc, gl.getUniformLocation, "glGetUniformLocation");
        load(gles, getProc, gl.uniform1ui, "glUniform1ui");
        load(gles, getProc, gl.uniform1i, "glUniform1i");
        load(gles, getProc, gl.uniform1f, "glUniform1f");
        load(gles, getProc, gl.getString, "glGetString");
        description = std::string(reinterpret_cast<const char*>(gl.getString(GL_RENDERER))) + " — "
            + reinterpret_cast<const char*>(gl.getString(GL_VERSION));
        return true;
    }
    description = "не удалось создать контекст OpenGL ES 3.2";
    return false;
}

const char* PRECISION = R"(precision highp float;
precision highp int;
)";

const char* HEADER = R"(layout(local_size_x = 64) in;
layout(std430, binding = 0) buffer Particles { uint particles[]; };
layout(std430, binding = 1) buffer Sorted { uint sorted[]; };
layout(std430, binding = 2) buffer Index { uint indexData[]; };
uniform uint count;
uniform uint cells;
uniform float worldSize;
uniform int cellsPerSide;
uniform float cellSize;
uniform float diameter;
uniform float stiffness;
uniform float gravity;
uniform float timeStep;
uniform uint particlesPerSide;
uniform float latticeStep;
uniform float initialSpeed;
uniform uint stepJ;
uniform uint stepK;

uint keysAt(uint i) { return i; }
uint orderAt(uint i) { return count + i; }
uint startsAt(uint c) { return 2u * count + c; }
uint pairKeyAt(uint i) { return 2u * count + cells + 1u + i; }
uint pairIndexAt(uint i) { return 3u * count + cells + 1u + i; }

float column(uint which, uint i) { return uintBitsToFloat(particles[which * count + i]); }
float sortedColumn(uint which, uint i) { return uintBitsToFloat(sorted[which * count + i]); }

void storeParticle(uint i, vec3 position, vec3 velocity, uint id)
{
    particles[0u * count + i] = floatBitsToUint(position.x);
    particles[1u * count + i] = floatBitsToUint(position.y);
    particles[2u * count + i] = floatBitsToUint(position.z);
    particles[3u * count + i] = floatBitsToUint(velocity.x);
    particles[4u * count + i] = floatBitsToUint(velocity.y);
    particles[5u * count + i] = floatBitsToUint(velocity.z);
    particles[6u * count + i] = id;
}

int cellFromRatio(float ratio)
{
    int raw = int(floor(ratio));
    return clamp(raw, 0, cellsPerSide - 1);
}

int cellCoordinate(float coordinate)
{
    precise float scaled = DIVIDE(coordinate, cellSize);
    return cellFromRatio(scaled);
}

ivec3 cellOf(vec3 position)
{
    return ivec3(cellCoordinate(position.x), cellCoordinate(position.y), cellCoordinate(position.z));
}

uint cellIndex(ivec3 cell)
{
    return uint((cell.z * cellsPerSide + cell.y) * cellsPerSide + cell.x);
}
)";

const char* INIT = R"(
float noise(uint index, uint axis)
{
    uint x = index * 3u + axis;
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    precise float scaled = float(x >> 8) * (2.0 / 16777216.0);
    precise float result = scaled - 1.0;
    return result;
}

void main()
{
    uint i = gl_GlobalInvocationID.x;
    if (i >= count)
    {
        return;
    }
    uint side = particlesPerSide;
    precise vec3 node = vec3(float(i % side), float(i / side % side), float(i / (side * side)));
    precise vec3 shifted = vec3(node.x + 0.5, node.y + 0.5, node.z + 0.5);
    precise vec3 position = vec3(shifted.x * latticeStep, shifted.y * latticeStep, shifted.z * latticeStep);
    precise vec3 raw = vec3(noise(i, 0u), noise(i, 1u), noise(i, 2u));
    precise vec3 velocity = vec3(raw.x * initialSpeed, raw.y * initialSpeed, raw.z * initialSpeed);
    storeParticle(i, position, velocity, i);
}
)";

const char* KEYS = R"(
void main()
{
    uint i = gl_GlobalInvocationID.x;
    if (i >= count)
    {
        return;
    }
    vec3 position = vec3(column(0u, i), column(1u, i), column(2u, i));
    uint key = cellIndex(cellOf(position));
    indexData[keysAt(i)] = key;
    indexData[pairKeyAt(i)] = key;
    indexData[pairIndexAt(i)] = i;
}
)";

const char* BITONIC = R"(
void main()
{
    uint i = gl_GlobalInvocationID.x;
    if (i >= count)
    {
        return;
    }
    uint partner = i ^ stepJ;
    if (partner <= i)
    {
        return;
    }
    uint keyA = indexData[pairKeyAt(i)];
    uint keyB = indexData[pairKeyAt(partner)];
    uint indexA = indexData[pairIndexAt(i)];
    uint indexB = indexData[pairIndexAt(partner)];
    bool greater = keyA > keyB || (keyA == keyB && indexA > indexB);
    bool ascending = (i & stepK) == 0u;
    if (greater == ascending)
    {
        indexData[pairKeyAt(i)] = keyB;
        indexData[pairKeyAt(partner)] = keyA;
        indexData[pairIndexAt(i)] = indexB;
        indexData[pairIndexAt(partner)] = indexA;
    }
}
)";

const char* STARTS = R"(
void main()
{
    uint i = gl_GlobalInvocationID.x;
    if (i >= count)
    {
        return;
    }
    uint key = indexData[pairKeyAt(i)];
    indexData[orderAt(i)] = indexData[pairIndexAt(i)];
    uint first = i == 0u ? 0u : indexData[pairKeyAt(i - 1u)] + 1u;
    for (uint cell = first; cell <= key; ++cell)
    {
        indexData[startsAt(cell)] = i;
    }
    if (i == count - 1u)
    {
        for (uint cell = key + 1u; cell <= cells; ++cell)
        {
            indexData[startsAt(cell)] = count;
        }
    }
}
)";

const char* GATHER = R"(
void main()
{
    uint i = gl_GlobalInvocationID.x;
    if (i >= count)
    {
        return;
    }
    uint source = indexData[orderAt(i)];
    for (uint which = 0u; which < 7u; ++which)
    {
        sorted[which * count + i] = particles[which * count + source];
    }
}
)";

const char* TRACE_BUFFER = R"(
layout(std430, binding = 3) buffer Trace { uint trace[]; };
uniform uint traceIndex;

void traceVector(uint slot, float first, float second, float third)
{
    trace[slot] = floatBitsToUint(first);
    trace[slot + 1u] = floatBitsToUint(second);
    trace[slot + 2u] = floatBitsToUint(third);
}
)";

const char* UPDATE = R"(
float wall(float coordinate)
{
    precise float radius = diameter * 0.5;
    if (coordinate < radius)
    {
        precise float inside = radius - coordinate;
        precise float force = stiffness * inside;
        return force;
    }
    precise float top = worldSize - radius;
    if (coordinate > top)
    {
        precise float outside = top - coordinate;
        precise float force = stiffness * outside;
        return force;
    }
    return 0.0;
}

bool insideWorld(ivec3 cell)
{
    return cell.x >= 0 && cell.x < cellsPerSide && cell.y >= 0 && cell.y < cellsPerSide && cell.z >= 0 && cell.z < cellsPerSide;
}

void main()
{
    uint i = gl_GlobalInvocationID.x;
    if (i >= count)
    {
        return;
    }
    vec3 position = vec3(sortedColumn(0u, i), sortedColumn(1u, i), sortedColumn(2u, i));
    vec3 velocity = vec3(sortedColumn(3u, i), sortedColumn(4u, i), sortedColumn(5u, i));
    uint id = sorted[6u * count + i];
    precise float ratioX = DIVIDE(position.x, cellSize);
    precise float ratioY = DIVIDE(position.y, cellSize);
    precise float ratioZ = DIVIDE(position.z, cellSize);
    ivec3 own = ivec3(cellFromRatio(ratioX), cellFromRatio(ratioY), cellFromRatio(ratioZ));
    precise float ax = wall(position.x) - 0.0;
    precise float ay = wall(position.y) - 0.0;
    precise float az = wall(position.z) - gravity;
#ifdef TRACE
    bool traced = i == traceIndex;
    if (traced)
    {
        traceVector(TRACE_RATIO, ratioX, ratioY, ratioZ);
        trace[TRACE_CELL] = uint(own.x);
        trace[TRACE_CELL + 1u] = uint(own.y);
        trace[TRACE_CELL + 2u] = uint(own.z);
        traceVector(TRACE_WALL, ax, ay, az);
    }
    uint candidates = 0u;
#endif
    precise float limit = diameter * diameter;
    for (int neighbour = 0; neighbour < 27; ++neighbour)
    {
        ivec3 cell = ivec3(own.x + neighbour % 3 - 1, own.y + neighbour / 3 % 3 - 1, own.z + neighbour / 9 - 1);
        if (!insideWorld(cell))
        {
            continue;
        }
        uint cellNumber = cellIndex(cell);
        uint begin = indexData[startsAt(cellNumber)];
        uint end = indexData[startsAt(cellNumber + 1u)];
        for (uint other = begin; other < end; ++other)
        {
            if (other == i)
            {
                continue;
            }
            precise float dx = position.x - sortedColumn(0u, other);
            precise float dy = position.y - sortedColumn(1u, other);
            precise float dz = position.z - sortedColumn(2u, other);
            precise float xx = dx * dx;
            precise float yy = dy * dy;
            precise float zz = dz * dz;
            precise float partial = xx + yy;
            precise float square = partial + zz;
            bool inRange = !(square >= limit || square == 0.0);
#ifdef TRACE
            uint slot = TRACE_HEADER + PAIR_SLOTS * candidates;
            bool tracePair = traced && candidates < PAIR_LIMIT;
            if (tracePair)
            {
                trace[slot + PAIR_OTHER] = other;
                traceVector(slot + PAIR_D, dx, dy, dz);
                traceVector(slot + PAIR_D2, xx, yy, zz);
                trace[slot + PAIR_SUM_XY] = floatBitsToUint(partial);
                trace[slot + PAIR_SQ] = floatBitsToUint(square);
                trace[slot + PAIR_IN_RANGE] = inRange ? 1u : 0u;
            }
            ++candidates;
#endif
            if (!inRange)
            {
                continue;
            }
            precise float distance = ROOT(square);
            precise float overlap = diameter - distance;
            precise float pressure = stiffness * overlap;
            precise float factor = DIVIDE(pressure, distance);
            precise float ex = dx * factor;
            precise float ey = dy * factor;
            precise float ez = dz * factor;
            ax = ax + ex;
            ay = ay + ey;
            az = az + ez;
#ifdef TRACE
            if (tracePair)
            {
                trace[slot + PAIR_DIST] = floatBitsToUint(distance);
                trace[slot + PAIR_OVERLAP] = floatBitsToUint(overlap);
                trace[slot + PAIR_NUM] = floatBitsToUint(pressure);
                trace[slot + PAIR_FACTOR] = floatBitsToUint(factor);
                traceVector(slot + PAIR_F, ex, ey, ez);
                traceVector(slot + PAIR_A, ax, ay, az);
            }
#endif
        }
    }
    precise float dvx = ax * timeStep;
    precise float dvy = ay * timeStep;
    precise float dvz = az * timeStep;
    precise float vx = velocity.x + dvx;
    precise float vy = velocity.y + dvy;
    precise float vz = velocity.z + dvz;
    precise float dpx = vx * timeStep;
    precise float dpy = vy * timeStep;
    precise float dpz = vz * timeStep;
    precise float px = position.x + dpx;
    precise float py = position.y + dpy;
    precise float pz = position.z + dpz;
#ifdef TRACE
    if (traced)
    {
        trace[TRACE_CANDIDATES] = candidates;
        traceVector(TRACE_ACCELERATION, ax, ay, az);
        traceVector(TRACE_DV, dvx, dvy, dvz);
        traceVector(TRACE_NEW_V, vx, vy, vz);
        traceVector(TRACE_DP, dpx, dpy, dpz);
        traceVector(TRACE_OUT, px, py, pz);
        trace[TRACE_INDEX] = i;
        trace[TRACE_ID] = id;
    }
#endif
    storeParticle(i, vec3(px, py, pz), vec3(vx, vy, vz), id);
}
)";

const char* PROBE = R"(layout(local_size_x = 64) in;
layout(std430, binding = 0) buffer Input { float inputs[]; };
layout(std430, binding = 1) buffer Output { float outputs[]; };
uniform uint count;
uniform uint mode;

void main()
{
    uint i = gl_GlobalInvocationID.x;
    if (i >= count)
    {
        return;
    }
    float a = inputs[3u * i];
    float b = inputs[3u * i + 1u];
    float c = inputs[3u * i + 2u];
    if (mode == 0u)
    {
        outputs[i] = a * b + c;
    }
    else if (mode == 1u)
    {
        precise float product = a * b;
        precise float sum = product + c;
        outputs[i] = sum;
    }
    else if (mode == 2u)
    {
        precise float quotient = a / b;
        outputs[i] = quotient;
    }
    else if (mode == 3u)
    {
        precise float root = sqrt(a);
        outputs[i] = root;
    }
    else if (mode == 4u)
    {
        precise float tiny = a * b;
        outputs[i] = tiny;
    }
    else if (mode == 5u)
    {
        outputs[i] = fma(a, b, c);
    }
    else if (mode == 6u)
    {
        outputs[i] = softDivExact(a, b);
    }
    else if (mode == 7u)
    {
        outputs[i] = softSqrtExact(a);
    }
    else
    {
        outputs[i] = softFmaExact(a, b, c);
    }
}
)";

enum class Arithmetic
{
    Hardware,
    Soft,
};

enum class Control
{
    None,
    Fused,
    Reordered,
};

static const char* arithmeticName(Arithmetic arithmetic)
{
    return arithmetic == Arithmetic::Soft ? "доводка" : "аппаратные деление и корень";
}

static std::string strictSource;

static std::string readText(const char* path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        std::fprintf(stderr, "нет файла %s рядом с программой\n", path);
        std::exit(2);
    }
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

static std::string traceConstants()
{
    std::string text;
    const std::pair<const char*, uint32_t> constants[] = {
        {"TRACE_CANDIDATES", TRACE_CANDIDATES}, {"TRACE_RATIO", TRACE_RATIO}, {"TRACE_CELL", TRACE_CELL},
        {"TRACE_WALL", TRACE_WALL}, {"TRACE_DV", TRACE_DV}, {"TRACE_NEW_V", TRACE_NEW_V}, {"TRACE_DP", TRACE_DP},
        {"TRACE_OUT", TRACE_OUT}, {"TRACE_INDEX", TRACE_INDEX}, {"TRACE_ID", TRACE_ID}, {"TRACE_ACCELERATION", TRACE_ACCELERATION},
        {"TRACE_HEADER", TRACE_HEADER}, {"PAIR_OTHER", PAIR_OTHER}, {"PAIR_D", PAIR_D}, {"PAIR_D2", PAIR_D2},
        {"PAIR_SUM_XY", PAIR_SUM_XY}, {"PAIR_SQ", PAIR_SQ}, {"PAIR_IN_RANGE", PAIR_IN_RANGE}, {"PAIR_DIST", PAIR_DIST},
        {"PAIR_OVERLAP", PAIR_OVERLAP}, {"PAIR_NUM", PAIR_NUM}, {"PAIR_FACTOR", PAIR_FACTOR}, {"PAIR_F", PAIR_F},
        {"PAIR_A", PAIR_A}, {"PAIR_SLOTS", PAIR_SLOTS}, {"PAIR_LIMIT", PAIR_LIMIT},
    };
    for (const auto& [name, value] : constants)
    {
        text += formatted("const uint %s = %uu;\n", name, value);
    }
    return text;
}

static std::string programSource(const std::string& body, Arithmetic arithmetic, bool traced, bool simulation = true)
{
    std::string text = shaderVersion;
    if (arithmetic == Arithmetic::Soft)
    {
        text += "#define DIVIDE(numerator, denominator) softDivExact(numerator, denominator)\n#define ROOT(value) softSqrtExact(value)\n";
    }
    else
    {
        text += "#define DIVIDE(numerator, denominator) ((numerator) / (denominator))\n#define ROOT(value) sqrt(value)\n";
    }
    if (traced)
    {
        text += "#define TRACE\n";
    }
    text += PRECISION + strictSource + (simulation ? HEADER : "");
    if (traced)
    {
        text += traceConstants() + TRACE_BUFFER;
    }
    return text + body;
}

static std::string replaceExactly(std::string text, const std::string& from, const std::string& to)
{
    size_t position = text.find(from);
    if (position == std::string::npos)
    {
        std::fprintf(stderr, "в ядре нет строки для контроля: %s\n", from.c_str());
        std::exit(3);
    }
    for (; position != std::string::npos; position = text.find(from, position + to.size()))
    {
        text.replace(position, from.size(), to);
    }
    return text;
}

static std::string updateBody(Control control)
{
    std::string text = UPDATE;
    if (control == Control::Reordered)
    {
        return replaceExactly(text, "for (int neighbour = 0; neighbour < 27; ++neighbour)", "for (int neighbour = 26; neighbour >= 0; --neighbour)");
    }
    if (control == Control::Fused)
    {
        text = replaceExactly(text, "precise float partial = xx + yy;", "float partial = fma(dy, dy, xx);");
        text = replaceExactly(text, "precise float square = partial + zz;", "float square = fma(dz, dz, partial);");
        text = replaceExactly(text, "ax = ax + ex;", "ax = fma(dx, factor, ax);");
        text = replaceExactly(text, "ay = ay + ey;", "ay = fma(dy, factor, ay);");
        text = replaceExactly(text, "az = az + ez;", "az = fma(dz, factor, az);");
        text = replaceExactly(text, "precise float vx = velocity.x + dvx;", "float vx = fma(ax, timeStep, velocity.x);");
        text = replaceExactly(text, "precise float vy = velocity.y + dvy;", "float vy = fma(ay, timeStep, velocity.y);");
        text = replaceExactly(text, "precise float vz = velocity.z + dvz;", "float vz = fma(az, timeStep, velocity.z);");
        text = replaceExactly(text, "precise float px = position.x + dpx;", "float px = fma(vx, timeStep, position.x);");
        text = replaceExactly(text, "precise float py = position.y + dpy;", "float py = fma(vy, timeStep, position.y);");
        text = replaceExactly(text, "precise float pz = position.z + dpz;", "float pz = fma(vz, timeStep, position.z);");
    }
    return text;
}

GLuint compile(const std::string& source)
{
    GLuint shader = gl.createShader(GL_COMPUTE_SHADER);
    const char* text = source.c_str();
    gl.shaderSource(shader, 1, &text, nullptr);
    gl.compileShader(shader);
    GLint status = 0;
    gl.getShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status)
    {
        char log[4096];
        gl.getShaderInfoLog(shader, sizeof(log), nullptr, log);
        std::fprintf(stderr, "шейдер не собран:\n%s\n", log);
        std::exit(3);
    }
    GLuint program = gl.createProgram();
    gl.attachShader(program, shader);
    gl.linkProgram(program);
    gl.getProgramiv(program, GL_LINK_STATUS, &status);
    if (!status)
    {
        char log[4096];
        gl.getProgramInfoLog(program, sizeof(log), nullptr, log);
        std::fprintf(stderr, "программа не связана:\n%s\n", log);
        std::exit(3);
    }
    return program;
}

GLuint buffer(size_t bytes, const void* data)
{
    GLuint name = 0;
    gl.genBuffers(1, &name);
    gl.bindBuffer(GL_SHADER_STORAGE_BUFFER, name);
    gl.bufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(bytes), data, GL_DYNAMIC_COPY);
    return name;
}

void readBack(GLuint name, size_t bytes, void* target)
{
    gl.bindBuffer(GL_SHADER_STORAGE_BUFFER, name);
    void* mapped = gl.mapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(bytes), GL_MAP_READ_BIT);
    std::memcpy(target, mapped, bytes);
    gl.unmapBuffer(GL_SHADER_STORAGE_BUFFER);
}

using Clock = std::chrono::steady_clock;

double since(Clock::time_point start)
{
    return std::chrono::duration<double>(Clock::now() - start).count();
}

static unsigned cpuThreads()
{
    return std::max(2u, std::thread::hardware_concurrency());
}

struct CpuSimulation
{
    World world;
    unsigned threads;
    bool wide;
    Columns particles;
    Columns sorted;
    std::vector<uint32_t> keys;
    std::vector<uint32_t> order;
    std::vector<uint32_t> starts;
    RadixDistributor distributor;

    CpuSimulation(uint32_t side, unsigned threadCount = 1, bool useAvx2 = false)
        : world(worldFor(side)), threads(threadCount), wide(useAvx2)
    {
        size_t count = particleCount(world);
        particles.resize(count);
        sorted.resize(count);
        keys.assign(count, 0);
        order.assign(count, 0);
        starts.assign(cellCount(world) + 1, 0);
        for (size_t index = 0; index < count; ++index)
        {
            particles.store(index, initialParticle(index, world));
        }
    }

    void step()
    {
        size_t count = particles.size();
        parallelFor(count, threads, [&](size_t begin, size_t end)
        {
            for (size_t index = begin; index < end; ++index)
            {
                keys[index] = cellKey(index, particles, world);
            }
        });
        distributor.distribute(keys, cellCount(world), threads, order, starts);
        parallelFor(count, threads, [&](size_t begin, size_t end)
        {
            for (size_t index = begin; index < end; ++index)
            {
                sorted.store(index, particles.load(order[index]));
            }
        });
        if (wide)
        {
            stepAvx2(world, sorted, starts, particles, threads);
            return;
        }
        parallelFor(count, threads, [&](size_t begin, size_t end)
        {
            for (size_t index = begin; index < end; ++index)
            {
                particles.store(index, updateParticle(index, sorted, starts.data(), world));
            }
        });
    }
};

struct GpuSimulation
{
    World world;
    Arithmetic arithmetic;
    Control control;
    uint32_t count;
    uint32_t cells;
    GLuint particles;
    GLuint sorted;
    GLuint index;
    GLuint traceBuffer = 0;
    GLuint init;
    GLuint keys;
    GLuint bitonic;
    GLuint starts;
    GLuint gather;
    GLuint update;
    GLuint tracedUpdate = 0;
    std::vector<uint32_t> raw;

    GpuSimulation(uint32_t side, Arithmetic arithmeticKind, Control controlKind = Control::None)
        : world(worldFor(side)), arithmetic(arithmeticKind), control(controlKind)
    {
        count = static_cast<uint32_t>(particleCount(world));
        cells = static_cast<uint32_t>(cellCount(world));
        if ((count & (count - 1)) != 0)
        {
            std::fprintf(stderr, "битонная сортировка ждёт степень двойки, а частиц %u\n", count);
            std::exit(4);
        }
        init = compile(programSource(INIT, arithmetic, false));
        keys = compile(programSource(KEYS, arithmetic, false));
        bitonic = compile(programSource(BITONIC, arithmetic, false));
        starts = compile(programSource(STARTS, arithmetic, false));
        gather = compile(programSource(GATHER, arithmetic, false));
        update = compile(programSource(updateBody(control), arithmetic, false));
        particles = buffer(size_t{7} * count * 4, nullptr);
        sorted = buffer(size_t{7} * count * 4, nullptr);
        index = buffer((size_t{4} * count + cells + 1) * 4, nullptr);
        raw.assign(size_t{7} * count, 0);
        bind();
        for (GLuint program : {init, keys, bitonic, starts, gather, update})
        {
            setUniforms(program);
        }
        run(init);
        gl.finish();
    }

    void bind()
    {
        gl.bindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, particles);
        gl.bindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, sorted);
        gl.bindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, index);
        if (traceBuffer != 0)
        {
            gl.bindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, traceBuffer);
        }
    }

    void setUniforms(GLuint program)
    {
        gl.useProgram(program);
        gl.uniform1ui(gl.getUniformLocation(program, "count"), count);
        gl.uniform1ui(gl.getUniformLocation(program, "cells"), cells);
        gl.uniform1f(gl.getUniformLocation(program, "worldSize"), world.size);
        gl.uniform1i(gl.getUniformLocation(program, "cellsPerSide"), static_cast<GLint>(world.cellsPerSide));
        gl.uniform1f(gl.getUniformLocation(program, "cellSize"), world.cellSize);
        gl.uniform1f(gl.getUniformLocation(program, "diameter"), world.diameter);
        gl.uniform1f(gl.getUniformLocation(program, "stiffness"), world.stiffness);
        gl.uniform1f(gl.getUniformLocation(program, "gravity"), world.gravity);
        gl.uniform1f(gl.getUniformLocation(program, "timeStep"), world.timeStep);
        gl.uniform1ui(gl.getUniformLocation(program, "particlesPerSide"), world.particlesPerSide);
        gl.uniform1f(gl.getUniformLocation(program, "latticeStep"), world.latticeStep);
        gl.uniform1f(gl.getUniformLocation(program, "initialSpeed"), world.initialSpeed);
    }

    void run(GLuint program)
    {
        gl.useProgram(program);
        gl.dispatchCompute((count + 63) / 64, 1, 1);
        gl.memoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    }

    size_t dispatches() const
    {
        size_t total = 4;
        for (uint32_t k = 2; k <= count; k <<= 1)
        {
            for (uint32_t j = k >> 1; j > 0; j >>= 1)
            {
                ++total;
            }
        }
        return total;
    }

    void sortOnGpu()
    {
        run(keys);
        GLint locationJ = gl.getUniformLocation(bitonic, "stepJ");
        GLint locationK = gl.getUniformLocation(bitonic, "stepK");
        gl.useProgram(bitonic);
        for (uint32_t k = 2; k <= count; k <<= 1)
        {
            for (uint32_t j = k >> 1; j > 0; j >>= 1)
            {
                gl.uniform1ui(locationJ, j);
                gl.uniform1ui(locationK, k);
                gl.dispatchCompute((count + 63) / 64, 1, 1);
                gl.memoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
            }
        }
        run(starts);
        run(gather);
    }

    void step()
    {
        sortOnGpu();
        run(update);
    }

    void unpack(const std::vector<uint32_t>& words, Columns& columns) const
    {
        if (columns.size() != count)
        {
            columns.resize(count);
        }
        std::vector<float>* targets[] = {&columns.px, &columns.py, &columns.pz, &columns.vx, &columns.vy, &columns.vz};
        for (size_t which = 0; which < 6; ++which)
        {
            std::memcpy(targets[which]->data(), words.data() + which * count, size_t{4} * count);
        }
        std::memcpy(columns.id.data(), words.data() + 6 * size_t{count}, size_t{4} * count);
    }

    void pack(const Columns& columns)
    {
        const std::vector<float>* sources[] = {&columns.px, &columns.py, &columns.pz, &columns.vx, &columns.vy, &columns.vz};
        for (size_t which = 0; which < 6; ++which)
        {
            std::memcpy(raw.data() + which * count, sources[which]->data(), size_t{4} * count);
        }
        std::memcpy(raw.data() + 6 * size_t{count}, columns.id.data(), size_t{4} * count);
    }

    void send()
    {
        gl.bindBuffer(GL_SHADER_STORAGE_BUFFER, particles);
        gl.bufferSubData(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(raw.size() * 4), raw.data());
        gl.memoryBarrier(GL_ALL_BARRIER_BITS);
    }

    void receive()
    {
        readBack(particles, raw.size() * 4, raw.data());
    }

    Columns download()
    {
        gl.finish();
        receive();
        Columns columns;
        unpack(raw, columns);
        return columns;
    }

    Columns downloadSorted()
    {
        std::vector<uint32_t> words(size_t{7} * count);
        readBack(sorted, words.size() * 4, words.data());
        Columns columns;
        unpack(words, columns);
        return columns;
    }

    std::vector<uint32_t> downloadIndex()
    {
        std::vector<uint32_t> words(size_t{4} * count + cells + 1);
        readBack(index, words.size() * 4, words.data());
        return words;
    }

    std::vector<uint32_t> traceUpdate(uint32_t particle)
    {
        if (tracedUpdate == 0)
        {
            tracedUpdate = compile(programSource(updateBody(control), arithmetic, true));
            setUniforms(tracedUpdate);
            std::vector<uint32_t> blank(TRACE_WORDS, UINT32_MAX);
            traceBuffer = buffer(blank.size() * 4, blank.data());
        }
        std::vector<uint32_t> blank(TRACE_WORDS, UINT32_MAX);
        gl.bindBuffer(GL_SHADER_STORAGE_BUFFER, traceBuffer);
        gl.bufferSubData(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(blank.size() * 4), blank.data());
        bind();
        gl.useProgram(tracedUpdate);
        gl.uniform1ui(gl.getUniformLocation(tracedUpdate, "traceIndex"), particle);
        gl.dispatchCompute((count + 63) / 64, 1, 1);
        gl.memoryBarrier(GL_ALL_BARRIER_BITS);
        gl.finish();
        std::vector<uint32_t> words(TRACE_WORDS);
        readBack(traceBuffer, words.size() * 4, words.data());
        return words;
    }
};

struct ProbeResult
{
    size_t total = 0;
    size_t mismatches = 0;
    size_t fusedMatches = 0;
    uint32_t maxUlp = 0;
    size_t zeroed = 0;
    size_t nonFinite = 0;
};

static GLuint probeProgram()
{
    static GLuint program = compile(programSource(PROBE, Arithmetic::Soft, false, false));
    return program;
}

static std::vector<float> probeOutputs(uint32_t mode, const std::vector<float>& inputs)
{
    uint32_t count = static_cast<uint32_t>(inputs.size() / 3);
    GLuint program = probeProgram();
    GLuint inputBuffer = buffer(inputs.size() * 4, inputs.data());
    GLuint outputBuffer = buffer(size_t{count} * 4, nullptr);
    gl.bindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, inputBuffer);
    gl.bindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, outputBuffer);
    gl.useProgram(program);
    gl.uniform1ui(gl.getUniformLocation(program, "count"), count);
    gl.uniform1ui(gl.getUniformLocation(program, "mode"), mode);
    gl.dispatchCompute((count + 63) / 64, 1, 1);
    gl.memoryBarrier(GL_ALL_BARRIER_BITS);
    gl.finish();
    std::vector<float> outputs(count);
    readBack(outputBuffer, size_t{count} * 4, outputs.data());
    return outputs;
}

static ProbeResult probe(uint32_t mode, const std::vector<float>& inputs, const std::vector<float>& expected, const std::vector<float>* fused)
{
    std::vector<float> outputs = probeOutputs(mode, inputs);
    ProbeResult result;
    result.total = expected.size();
    for (size_t index = 0; index < expected.size(); ++index)
    {
        uint32_t got = toBits(outputs[index]);
        uint32_t want = toBits(expected[index]);
        if (fmaSameResult(got, want))
        {
            continue;
        }
        ++result.mismatches;
        if ((got & 0x7FFFFFFFu) == 0u && (want & 0x7FFFFFFFu) != 0u)
        {
            ++result.zeroed;
        }
        else if ((got & 0x7F800000u) == 0x7F800000u || (want & 0x7F800000u) == 0x7F800000u)
        {
            ++result.nonFinite;
        }
        else
        {
            result.maxUlp = std::max(result.maxUlp, ulpDistance(got, want));
        }
        if (fused && toBits((*fused)[index]) == got)
        {
            ++result.fusedMatches;
        }
    }
    return result;
}

static std::string describe(const ProbeResult& result)
{
    if (result.mismatches == 0)
    {
        return "все верны";
    }
    std::string text = formatted("неверно %zu", result.mismatches);
    if (result.maxUlp > 0)
    {
        text += formatted(", до %u ulp", result.maxUlp);
    }
    if (result.zeroed > 0)
    {
        text += formatted(", ноль вместо числа %zu", result.zeroed);
    }
    if (result.nonFinite > 0)
    {
        text += formatted(", бесконечность или NaN %zu", result.nonFinite);
    }
    return text;
}

struct ProbeSummary
{
    bool contraction = false;
    bool softExact = false;
    bool denormals = false;
    bool softFma = false;
};

static ProbeSummary runProbes()
{
    std::mt19937 random(20261103);
    std::uniform_real_distribution<float> unit(1.0f, 2.0f);
    std::uniform_int_distribution<int> exponent(-60, 60);
    const uint32_t count = 1u << 20;
    std::vector<float> inputs(size_t{3} * count);
    for (uint32_t index = 0; index < count; ++index)
    {
        inputs[3 * index] = std::ldexp(unit(random), exponent(random) / 4);
        inputs[3 * index + 1] = std::ldexp(unit(random), exponent(random) / 4);
        inputs[3 * index + 2] = -inputs[3 * index] * inputs[3 * index + 1] * (1.0f + std::ldexp(unit(random), -22));
    }
    std::vector<float> unfused(count), fused(count), quotient(count), root(count);
    for (uint32_t index = 0; index < count; ++index)
    {
        volatile float product = inputs[3 * index] * inputs[3 * index + 1];
        unfused[index] = product + inputs[3 * index + 2];
        fused[index] = fmaFloatOf(referenceFmaBits(toBits(inputs[3 * index]), toBits(inputs[3 * index + 1]), toBits(inputs[3 * index + 2])));
        quotient[index] = inputs[3 * index] / inputs[3 * index + 1];
        root[index] = std::sqrt(inputs[3 * index]);
    }
    const uint32_t tinyCount = 1u << 16;
    std::vector<float> tinyInputs(size_t{3} * tinyCount);
    std::vector<float> tinyProduct(tinyCount), tinyQuotient(tinyCount), tinyRoot(tinyCount);
    std::uniform_int_distribution<int> tinyExponent(-149, -127);
    for (uint32_t index = 0; index < tinyCount; ++index)
    {
        tinyInputs[3 * index] = std::ldexp(unit(random), -100);
        tinyInputs[3 * index + 1] = std::ldexp(unit(random), -40);
        tinyProduct[index] = tinyInputs[3 * index] * tinyInputs[3 * index + 1];
        tinyQuotient[index] = tinyInputs[3 * index] / (1.0f / tinyInputs[3 * index + 1]);
    }
    std::vector<float> tinyDivision(size_t{3} * tinyCount);
    std::vector<float> tinyRadicands(size_t{3} * tinyCount);
    for (uint32_t index = 0; index < tinyCount; ++index)
    {
        tinyDivision[3 * index] = std::ldexp(unit(random), -100);
        tinyDivision[3 * index + 1] = std::ldexp(unit(random), 30);
        tinyQuotient[index] = tinyDivision[3 * index] / tinyDivision[3 * index + 1];
        tinyRadicands[3 * index] = std::ldexp(unit(random), tinyExponent(random));
        tinyRoot[index] = std::sqrt(tinyRadicands[3 * index]);
    }
    ProbeResult loose = probe(0, inputs, unfused, &fused);
    ProbeResult strict = probe(1, inputs, unfused, &fused);
    ProbeResult division = probe(2, inputs, quotient, nullptr);
    ProbeResult squareRoot = probe(3, inputs, root, nullptr);
    ProbeResult denormals = probe(4, tinyInputs, tinyProduct, nullptr);
    ProbeResult explicitFma = probe(5, inputs, fused, nullptr);
    ProbeResult softFma = probe(8, inputs, fused, nullptr);
    ProbeResult softDivision = probe(6, inputs, quotient, nullptr);
    ProbeResult softRoot = probe(7, inputs, root, nullptr);
    ProbeResult tinyHardDivision = probe(2, tinyDivision, tinyQuotient, nullptr);
    ProbeResult tinySoftDivision = probe(6, tinyDivision, tinyQuotient, nullptr);
    ProbeResult tinyHardRoot = probe(3, tinyRadicands, tinyRoot, nullptr);
    ProbeResult tinySoftRoot = probe(7, tinyRadicands, tinyRoot, nullptr);
    size_t distinguishing = 0;
    for (uint32_t index = 0; index < count; ++index)
    {
        distinguishing += toBits(unfused[index]) != toBits(fused[index]);
    }
    std::printf("пробы строгой арифметики на %u значениях, FMA и раздельное округление различают %zu:\n", count, distinguishing);
    std::printf("  a*b+c без precise: %zu расхождений с раздельным округлением, из них %zu совпали с FMA\n", loose.mismatches,
        loose.fusedMatches);
    std::printf("  a*b+c с precise:   %s\n", describe(strict).c_str());
    std::printf("  деление:           аппаратно — %s; доводка — %s\n", describe(division).c_str(), describe(softDivision).c_str());
    std::printf("  корень:            аппаратно — %s; доводка — %s\n", describe(squareRoot).c_str(), describe(softRoot).c_str());
    std::printf("  денормализованный результат умножения (%u): %s\n", tinyCount, describe(denormals).c_str());
    std::printf("  денормализованное частное (%u): аппаратно — %s; доводка — %s\n", tinyCount, describe(tinyHardDivision).c_str(),
        describe(tinySoftDivision).c_str());
    std::printf("  корень из денормализованного (%u): аппаратно — %s; доводка — %s\n", tinyCount, describe(tinyHardRoot).c_str(),
        describe(tinySoftRoot).c_str());
    std::printf("  явный fma():       аппаратно — %s; программно — %s\n", describe(explicitFma).c_str(), describe(softFma).c_str());
    bool softFmaExact = softFma.mismatches == 0;
    std::printf("  fma по группам (soft_fma.glsl против эталона на ЦПУ):\n");
    for (uint32_t group = 0; group < FMA_GROUPS; ++group)
    {
        std::vector<FmaTriple> triples = fmaGroupTriples(static_cast<FmaGroup>(group), 1u << 16, 0x5EED0050A0000010ull);
        std::vector<float> groupInputs;
        std::vector<float> groupExpected;
        for (const FmaTriple& triple : triples)
        {
            groupInputs.insert(groupInputs.end(), {fmaFloatOf(triple.multiplicand), fmaFloatOf(triple.multiplier), fmaFloatOf(triple.addend)});
            groupExpected.push_back(fmaFloatOf(referenceFmaBits(triple.multiplicand, triple.multiplier, triple.addend)));
        }
        ProbeResult hardwareGroup = probe(5, groupInputs, groupExpected, nullptr);
        ProbeResult softGroup = probe(8, groupInputs, groupExpected, nullptr);
        softFmaExact = softFmaExact && softGroup.mismatches == 0;
        std::printf("    %s (%zu): аппаратно — %s; программно — %s\n", FMA_GROUP_NAMES[group], triples.size(),
            describe(hardwareGroup).c_str(), describe(softGroup).c_str());
    }
    ProbeSummary summary;
    summary.contraction = strict.mismatches == 0;
    summary.softExact = softDivision.mismatches == 0 && softRoot.mismatches == 0 && tinySoftDivision.mismatches == 0
        && tinySoftRoot.mismatches == 0;
    summary.denormals = denormals.mismatches == 0;
    summary.softFma = softFmaExact;
    return summary;
}

static std::string glDivisionCause(const World& world, const Columns& previous, uint32_t particle, Arithmetic arithmetic)
{
    std::vector<float> inputs;
    for (int axis = 0; axis < 3; ++axis)
    {
        inputs.push_back(fieldOf(previous, axis)[particle]);
        inputs.push_back(world.cellSize);
        inputs.push_back(0.0f);
    }
    std::vector<float> outputs = probeOutputs(arithmetic == Arithmetic::Soft ? 6u : 2u, inputs);
    const char axes[] = "xyz";
    std::string text;
    for (int axis = 0; axis < 3; ++axis)
    {
        float coordinate = inputs[3 * axis];
        uint32_t want = toBits(coordinate / world.cellSize);
        uint32_t got = toBits(outputs[axis]);
        if (want != got)
        {
            text += (text.empty() ? "" : "; ") + divisionVerdict(formatted("частное %c / размер ячейки", axes[axis]), coordinate,
                world.cellSize, want, got) + formatted(", ячейка ЦПУ %d, ГПУ %d", static_cast<int>(std::floor(fromBits(want))),
                static_cast<int>(std::floor(fromBits(got))));
        }
    }
    return text.empty() ? "частные совпали — ключ отличается по другой причине" : text;
}

static void explainGlDivergence(CpuSimulation& cpu, GpuSimulation& gpu, const Columns& previous, const Columns& gpuState, uint32_t step)
{
    std::vector<uint32_t> index = gpu.downloadIndex();
    for (uint32_t particle = 0; particle < gpu.count; ++particle)
    {
        uint32_t want = cellKey(particle, previous, cpu.world);
        if (index[particle] != want)
        {
            std::printf("      первое расхождение — шаг %u, в сортировке: ключ ячейки частицы № %u на ЦПУ %u, на ГПУ %u\n", step,
                previous.id[particle], want, index[particle]);
            std::printf("        причина: %s\n", glDivisionCause(cpu.world, previous, particle, gpu.arithmetic).c_str());
            gpu.bind();
            return;
        }
    }
    Columns gpuSorted = gpu.downloadSorted();
    if (!gpuSorted.bitwiseEqual(cpu.sorted))
    {
        std::printf("      первое расхождение — шаг %u, в сортировке: ключи равны, но порядок частиц другой\n", step);
        return;
    }
    size_t perField[6] = {};
    std::vector<uint32_t> differing = differingPlaces(cpu.particles, gpuState, perField);
    std::printf("      первое расхождение — шаг %u, сортировка совпала, вход шага одинаков: различаются %zu из %u частиц (%s)\n", step,
        differing.size(), gpu.count, fieldCounts(perField).c_str());
    for (size_t shown = 0; shown < std::min<size_t>(differing.size(), 3); ++shown)
    {
        uint32_t place = differing[shown];
        std::vector<uint32_t> gpuTrace = gpu.traceUpdate(place);
        std::vector<uint32_t> cpuTrace = traceOnCpu(cpu.world, cpu.sorted, cpu.starts, place);
        std::printf("        частица № %u (место %u): %s\n", cpu.sorted.id[place], place,
            particleValues(cpu.particles, place, gpuState, place).c_str());
        if (!traceMatchesModel(cpuTrace, cpu.particles, place))
        {
            std::printf("        ВНУТРЕННЯЯ ОШИБКА: трасса ЦПУ не воспроизводит модель\n");
        }
        if (gpuTrace[TRACE_INDEX] != place)
        {
            std::printf("        трасса ГПУ не записана\n");
            continue;
        }
        std::printf("        причина: %s\n", findCause(cpu.world, cpu.sorted, place, cpuTrace, gpuTrace).c_str());
    }
    Columns traced = gpu.download();
    if (!traced.bitwiseEqual(gpuState))
    {
        std::printf("        ядро с трассой дало другой шаг, чем без неё: трасса может не отражать причину\n");
    }
}

struct ComparisonOutcome
{
    bool startEqual = false;
    uint32_t firstDivergence = 0;
    size_t finalDiffering = 0;
};

static const uint32_t GL_CHECKPOINTS[] = {1, 10, 50, 100, 200, 300, 400};

static ComparisonOutcome runComparison(uint32_t side, uint32_t steps, Arithmetic arithmetic, Control control)
{
    CpuSimulation cpu(side);
    GpuSimulation gpu(side, arithmetic, control);
    ComparisonOutcome outcome;
    Columns state = gpu.download();
    outcome.startEqual = state.bitwiseEqual(cpu.particles);
    std::printf("    начальные частицы (%u), посчитанные на ГПУ: %s\n", gpu.count, outcome.startEqual ? "побитно равны ЦПУ" : "РАЗЛИЧАЮТСЯ");
    CheckpointLog log{cpu.particles.size(), {}};
    uint32_t next = 0;
    uint32_t firstContact = 0;
    for (uint32_t step = 1; step <= steps; ++step)
    {
        Columns previous = cpu.particles;
        cpu.step();
        if (firstContact == 0 && countContacts(cpu.world, cpu.sorted, cpu.starts).pairs > 0)
        {
            firstContact = step;
        }
        gpu.step();
        state = gpu.download();
        if (outcome.firstDivergence == 0 && !state.bitwiseEqual(cpu.particles))
        {
            outcome.firstDivergence = step;
            log.flush();
            explainGlDivergence(cpu, gpu, previous, state, step);
            gpu.bind();
            state = gpu.download();
        }
        if (next < std::size(GL_CHECKPOINTS) && GL_CHECKPOINTS[next] == step)
        {
            outcome.finalDiffering = log.check(step, cpu.particles, state, hashById(cpu.particles));
            ++next;
        }
    }
    log.flush();
    std::printf("    первые касания частиц на шаге %u\n", firstContact);
    return outcome;
}

struct Measurement
{
    uint32_t count = 0;
    size_t dispatches = 0;
    double cpuScalar = 0.0;
    double cpuBest = 0.0;
    double gpuStep = 0.0;
    double pack = 0.0;
    double send = 0.0;
    double launch = 0.0;
    double sync = 0.0;
    double receive = 0.0;
    double unpack = 0.0;
    double run = 0.0;
};

template <class Body>
static double medianSeconds(int runs, Body body)
{
    std::vector<double> times;
    for (int repeat = 0; repeat < runs; ++repeat)
    {
        auto start = Clock::now();
        body();
        times.push_back(since(start));
    }
    std::nth_element(times.begin(), times.begin() + runs / 2, times.end());
    return times[runs / 2];
}

static Measurement measure(uint32_t side, size_t steps, size_t runSteps, unsigned threads)
{
    CpuSimulation scalar(side);
    CpuSimulation best(side, threads, cpuHasAvx2());
    GpuSimulation gpu(side, Arithmetic::Soft);
    for (int warm = 0; warm < 2; ++warm)
    {
        scalar.step();
        best.step();
        gpu.step();
    }
    gl.finish();
    Measurement result;
    result.count = gpu.count;
    result.dispatches = gpu.dispatches();
    result.cpuScalar = medianSeconds(3, [&] { scalar.step(); });
    result.cpuBest = std::min(medianSeconds(5, [&] { best.step(); }), result.cpuScalar);
    auto start = Clock::now();
    for (size_t step = 0; step < steps; ++step)
    {
        gpu.step();
    }
    gl.finish();
    result.gpuStep = since(start) / steps;
    Columns host = gpu.download();
    result.pack = medianSeconds(5, [&] { gpu.pack(host); });
    result.send = medianSeconds(5, [&]
    {
        gpu.send();
        gl.finish();
    });
    result.sync = medianSeconds(5, [&] { gl.finish(); });
    result.receive = medianSeconds(5, [&] { gpu.receive(); });
    Columns columns;
    result.unpack = medianSeconds(5, [&] { gpu.unpack(gpu.raw, columns); });
    start = Clock::now();
    for (int repeat = 0; repeat < 200; ++repeat)
    {
        gl.useProgram(gpu.gather);
        gl.dispatchCompute(1, 1, 1);
        gl.memoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    }
    gl.finish();
    result.launch = since(start) / 200;
    start = Clock::now();
    gpu.pack(host);
    gpu.send();
    for (size_t step = 0; step < runSteps; ++step)
    {
        gpu.step();
    }
    gl.finish();
    gpu.receive();
    gpu.unpack(gpu.raw, columns);
    result.run = since(start);
    return result;
}

static void runTimings(const std::vector<uint32_t>& sides, size_t steps, size_t runSteps)
{
    unsigned threads = cpuThreads();
    bool wide = cpuHasAvx2();
    std::vector<Measurement> measurements;
    std::printf("замеры, ГПУ со строгой арифметикой (доводка), мс; ЦПУ: скаляр 1 поток и %s %u потоков, сортировка включена\n",
        wide ? "AVX2×8" : "скаляр", threads);
    std::printf("%9s %10s %10s %10s %9s %9s %9s %9s %9s %9s %10s\n", "частиц", "ЦПУ 1 пот", "ЦПУ лучш", "ГПУ шаг", "упаковка", "туда",
        "ожидание", "обратно", "распаковка", "запусков", "запуск мкс");
    for (uint32_t side : sides)
    {
        Measurement m = measure(side, steps, runSteps, threads);
        measurements.push_back(m);
        std::printf("%9u %10.3f %10.3f %10.3f %9.3f %9.3f %9.3f %9.3f %9.3f %9zu %10.1f\n", m.count, m.cpuScalar * 1e3, m.cpuBest * 1e3,
            m.gpuStep * 1e3, m.pack * 1e3, m.send * 1e3, m.sync * 1e3, m.receive * 1e3, m.unpack * 1e3, m.dispatches, m.launch * 1e6);
    }
    const Measurement& big = measurements.back();
    double latency = 0.0;
    for (const Measurement& m : measurements)
    {
        latency += m.launch / measurements.size();
    }
    double perElement = (big.gpuStep - big.dispatches * latency) / big.count;
    double packPerElement = big.pack / big.count;
    double sendPerElement = big.send / big.count;
    double receivePerElement = big.receive / big.count;
    double unpackPerElement = big.unpack / big.count;
    double sync = big.sync;
    std::printf("модель: время ГПУ = упаковка + туда + шаги × (запуски × задержка + частицы × время на частицу) + ожидание + обратно "
        "+ распаковка; задержка %.1f мкс, на частицу %.4f мкс, порог выигрыша 20%%\n", latency * 1e6, perElement * 1e6);
    std::printf("%9s %13s %13s %13s %13s %10s %10s\n", "частиц", "ЦПУ всего", "ГПУ измерено", "ГПУ модель", "без задержки", "выбор",
        "лучше");
    for (const Measurement& m : measurements)
    {
        double cpuTotal = runSteps * m.cpuBest;
        double fixed = (packPerElement + sendPerElement + receivePerElement + unpackPerElement) * m.count + sync;
        double modelled = fixed + runSteps * (m.dispatches * latency + perElement * m.count);
        double linear = fixed + runSteps * (big.gpuStep / big.count) * m.count;
        bool chooseGpu = modelled < 0.8 * cpuTotal;
        std::printf("%9u %10.1f мс %10.1f мс %10.1f мс %10.1f мс %10s %10s   модель ×%.2f, без задержки ×%.2f\n", m.count,
            cpuTotal * 1e3, m.run * 1e3, modelled * 1e3, linear * 1e3, chooseGpu ? "ГПУ" : "ЦПУ", m.run < cpuTotal ? "ГПУ" : "ЦПУ",
            modelled / m.run, linear / m.run);
    }
    std::printf("  «×k» — предсказанное время прогона ГПУ из %zu шагов к измеренному\n", runSteps);
}

int main(int argc, char** argv)
{
    std::string mode = argc > 1 ? argv[1] : "сравнить";
    int device = -1;
    bool desktop = false;
    for (int index = 2; index < argc; ++index)
    {
        if (std::string(argv[index]) == "--устройство" && index + 1 < argc)
        {
            device = std::atoi(argv[index + 1]);
        }
        if (std::string(argv[index]) == "--gl")
        {
            desktop = true;
        }
    }
    strictSource = readText("strict.glsl") + readText("soft_fma.glsl");
    std::string description;
    if (!createContext(device, desktop, description) && !(desktop = true, createContext(device, true, description)))
    {
        std::printf("пропуск: %s\n", description.c_str());
        return 77;
    }
    std::printf("устройство: %s\n", description.c_str());
    if (mode == "пробы")
    {
        ProbeSummary summary = runProbes();
        return summary.contraction && summary.softExact && summary.softFma ? 0 : 1;
    }
    if (mode == "сжатие" || mode == "перестановка")
    {
        Control control = mode == "сжатие" ? Control::Fused : Control::Reordered;
        std::printf("контроль с доводкой: %s\n", control == Control::Fused ? "явный fma() вместо умножения и сложения"
            : "те же слагаемые в обратном порядке соседних ячеек");
        ComparisonOutcome outcome = runComparison(32, 400, Arithmetic::Soft, control);
        return outcome.firstDivergence != 0 ? 0 : 1;
    }
    if (mode == "замеры")
    {
        runTimings({8, 16, 32, 64}, 10, 200);
        return 0;
    }
    ProbeSummary summary = runProbes();
    std::printf("precise соблюдается: %s; доводка точна: %s; программный fma точен: %s; денормализованные числа: %s\n",
        summary.contraction ? "да" : "НЕТ", summary.softExact ? "да" : "НЕТ", summary.softFma ? "да" : "НЕТ",
        summary.denormals ? "сохраняются" : "теряются");
    std::string summaryLine;
    bool strictMatched = false;
    for (Arithmetic arithmetic : {Arithmetic::Hardware, Arithmetic::Soft})
    {
        std::printf("400 шагов, ЦПУ против GL ES, %s:\n", arithmeticName(arithmetic));
        ComparisonOutcome outcome = runComparison(32, 400, arithmetic, Control::None);
        summaryLine += formatted("%s%s — %s", summaryLine.empty() ? "" : "; ", arithmeticName(arithmetic), outcome.firstDivergence == 0
            ? "побитно все 400 шагов" : formatted("расходится с шага %u, к шагу 400 различаются %zu частиц", outcome.firstDivergence,
                outcome.finalDiffering).c_str());
        if (arithmetic == Arithmetic::Soft)
        {
            strictMatched = outcome.startEqual && outcome.firstDivergence == 0;
        }
    }
    std::printf("итог: %s\n", summaryLine.c_str());
    return strictMatched ? 0 : 1;
}
