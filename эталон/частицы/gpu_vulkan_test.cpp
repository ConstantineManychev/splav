#include "core_kind.hpp"
#include "cpu_avx2.hpp"
#include "fma_cases.hpp"
#include "trace.hpp"
#include <vulkan/vulkan.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>
#include <xmmintrin.h>

#if defined(_MSC_VER) && !defined(__clang__)
#define SSE41_TARGET
#else
#define SSE41_TARGET __attribute__((target("sse4.1")))
#endif

using Clock = std::chrono::steady_clock;

constexpr uint32_t PROBE_PAIRS = 1u << 20;
constexpr uint32_t INPUT_COLUMNS = 8;
constexpr uint32_t OUTPUT_COLUMNS = 7;
constexpr uint32_t STARTS_COLUMN = 7;
constexpr uint32_t PARTICLE_COLUMNS = INPUT_COLUMNS + OUTPUT_COLUMNS;
constexpr uint32_t PARTICLE_BINDINGS = PARTICLE_COLUMNS + 1;
constexpr uint32_t BENCH_STEPS = 30;
constexpr uint32_t PATH_WARMUPS = 2;
constexpr uint32_t PATH_RUNS = 9;
constexpr uint32_t PATH_STAMPS = 4;
constexpr uint32_t RESIDENT_STAMPS = 7;
constexpr uint32_t RESIDENT_STAGES = RESIDENT_STAMPS - 1;
constexpr uint32_t RESIDENT_WARMUPS = 3;
constexpr uint32_t RESIDENT_BATCHES = 5;
constexpr uint32_t SCAN_BLOCK = 1024;
constexpr size_t MAPPED_PROBE_BYTES = size_t{16} << 20;
constexpr uint32_t PROBE_LANES = 6;
constexpr uint32_t TRAJECTORY_SIDE = 32;
constexpr uint32_t TRAJECTORY_STEPS = 400;
constexpr uint32_t CHECKPOINTS[] = {1, 10, 50, 100, 200, 300, 400};
constexpr uint32_t CHECKPOINT_COUNT = sizeof(CHECKPOINTS) / sizeof(CHECKPOINTS[0]);
constexpr uint32_t TRACED_PARTICLES = 3;
constexpr VkMemoryPropertyFlags UNUSABLE_MEMORY = VK_MEMORY_PROPERTY_PROTECTED_BIT | VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT
    | VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD;
constexpr VkBufferUsageFlags COLUMN_USAGE = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
    | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

static const char* const RESIDENT_STAGE_NAMES[RESIDENT_STAGES] = {"ключи", "префиксные суммы", "раскладка", "ранги", "сбор", "шаг"};

static unsigned cpuThreads()
{
    return std::max(2u, std::thread::hardware_concurrency());
}

static bool cpuHasSse41()
{
#if defined(_MSC_VER) && !defined(__clang__)
    int registers[4] = {};
    __cpuid(registers, 1);
    return (registers[2] & (1 << 19)) != 0;
#else
    unsigned eax = 0;
    unsigned ebx = 0;
    unsigned ecx = 0;
    unsigned edx = 0;
    return __get_cpuid(1, &eax, &ebx, &ecx, &edx) && (ecx & (1u << 19)) != 0;
#endif
}

SSE41_TARGET static void streamingCopy(void* target, const void* source, size_t bytes)
{
    __m128i* to = static_cast<__m128i*>(target);
    __m128i* from = static_cast<__m128i*>(const_cast<void*>(source));
    size_t lines = bytes / 64;
    for (size_t line = 0; line < lines; ++line)
    {
        __m128i first = _mm_stream_load_si128(from + 4 * line);
        __m128i second = _mm_stream_load_si128(from + 4 * line + 1);
        __m128i third = _mm_stream_load_si128(from + 4 * line + 2);
        __m128i fourth = _mm_stream_load_si128(from + 4 * line + 3);
        _mm_storeu_si128(to + 4 * line, first);
        _mm_storeu_si128(to + 4 * line + 1, second);
        _mm_storeu_si128(to + 4 * line + 2, third);
        _mm_storeu_si128(to + 4 * line + 3, fourth);
    }
}

static void checkVk(VkResult res, const char* op)
{
    if (res != VK_SUCCESS)
    {
        std::fprintf(stderr, "Vulkan error %d at %s\n", static_cast<int>(res), op);
        std::exit(1);
    }
}

static double msBetween(Clock::time_point start, Clock::time_point end)
{
    return std::chrono::duration<double, std::milli>(end - start).count();
}

static double msSince(Clock::time_point start)
{
    return msBetween(start, Clock::now());
}

static double gigabytesPerSecond(double bytes, double ms)
{
    return ms > 0.0 ? bytes / ms / 1.0e6 : 0.0;
}

struct PushWorld
{
    float size;
    uint32_t cellsPerSide;
    float cellSize;
    float diameter;
    float stiffness;
    float gravity;
    float timeStep;
    uint32_t particleCount;
    uint32_t traceIndex;
};

static PushWorld pushFor(const World& world, size_t count)
{
    return PushWorld{world.size, world.cellsPerSide, world.cellSize, world.diameter, world.stiffness, world.gravity,
        world.timeStep, static_cast<uint32_t>(count), NO_TRACE};
}

static uint32_t groupsFor(size_t count)
{
    return static_cast<uint32_t>((count + 255) / 256);
}

static uint32_t blocksFor(size_t count)
{
    return static_cast<uint32_t>((count + SCAN_BLOCK - 1) / SCAN_BLOCK);
}

static std::vector<uint32_t> loadSpv(const std::string& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
    {
        std::fprintf(stderr, "Cannot open %s\n", path.c_str());
        std::exit(1);
    }
    size_t bytes = static_cast<size_t>(file.tellg());
    file.seekg(0);
    std::vector<uint32_t> code(bytes / sizeof(uint32_t));
    file.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(bytes));
    return code;
}

static std::vector<uint32_t> loadOptionalSpv(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    return file ? loadSpv(path) : std::vector<uint32_t>{};
}

struct ShaderSet
{
    std::string modeName;
    std::vector<uint32_t> update;
    std::vector<uint32_t> updateSoft;
    std::vector<uint32_t> updateApprox;
    std::vector<uint32_t> probe;
    std::vector<uint32_t> keys;
    std::vector<uint32_t> keysSoft;
    std::vector<uint32_t> probeFma;
    std::vector<uint32_t> probeFmaKhr;
};

static ShaderSet loadShaders(const std::string& modeName, const std::string& suffix)
{
    return ShaderSet{modeName, loadSpv("update" + suffix + ".spv"), loadSpv("update_soft" + suffix + ".spv"),
        loadSpv("update_approx" + suffix + ".spv"), loadSpv("probe" + suffix + ".spv"), loadSpv("sort_keys" + suffix + ".spv"),
        loadSpv("sort_keys_soft" + suffix + ".spv"), loadSpv("probe_fma" + suffix + ".spv"),
        loadOptionalSpv("probe_fma_khr" + suffix + ".spv")};
}

struct SortShaders
{
    std::vector<uint32_t> reduce;
    std::vector<uint32_t> scan;
    std::vector<uint32_t> scatter;
    std::vector<uint32_t> rank;
    std::vector<uint32_t> gather;
};

static SortShaders loadSortShaders()
{
    return SortShaders{loadSpv("scan_reduce.spv"), loadSpv("scan.spv"), loadSpv("sort_scatter.spv"), loadSpv("sort_rank.spv"),
        loadSpv("sort_gather.spv")};
}

struct MemoryWish
{
    VkMemoryPropertyFlags flag;
    bool wanted;
};

struct MemoryChoice
{
    VkMemoryPropertyFlags required;
    std::vector<MemoryWish> wishes;
};

static const MemoryChoice UPLOAD_MEMORY{VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
    {{VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false}, {VK_MEMORY_PROPERTY_HOST_CACHED_BIT, false}}};
static const MemoryChoice READBACK_MEMORY{VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
    {{VK_MEMORY_PROPERTY_HOST_CACHED_BIT, true}, {VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true},
        {VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false}}};
static const MemoryChoice LEGACY_READBACK_MEMORY{VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, {}};
static const MemoryChoice DEVICE_MEMORY{VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, {{VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, false}}};

static uint32_t chooseMemoryType(VkPhysicalDevice physDev, uint32_t typeBits, const MemoryChoice& choice)
{
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(physDev, &props);
    uint32_t chosen = UINT32_MAX;
    uint32_t chosenScore = 0;
    for (uint32_t idx = 0; idx < props.memoryTypeCount; ++idx)
    {
        VkMemoryPropertyFlags flags = props.memoryTypes[idx].propertyFlags;
        bool usable = (typeBits & (1u << idx)) != 0 && (flags & choice.required) == choice.required && (flags & UNUSABLE_MEMORY) == 0;
        if (!usable)
        {
            continue;
        }
        uint32_t score = 0;
        for (const MemoryWish& wish : choice.wishes)
        {
            score = 2 * score + (((flags & wish.flag) != 0) == wish.wanted ? 1u : 0u);
        }
        if (chosen == UINT32_MAX || score > chosenScore)
        {
            chosen = idx;
            chosenScore = score;
        }
    }
    return chosen;
}

static VkMemoryPropertyFlags memoryFlags(VkPhysicalDevice physDev, uint32_t typeIndex)
{
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(physDev, &props);
    return props.memoryTypes[typeIndex].propertyFlags;
}

static std::string memoryFlagsName(VkMemoryPropertyFlags flags)
{
    static const std::pair<VkMemoryPropertyFlags, const char*> NAMES[] = {
        {VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "DEVICE_LOCAL"},
        {VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, "HOST_VISIBLE"},
        {VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, "HOST_COHERENT"},
        {VK_MEMORY_PROPERTY_HOST_CACHED_BIT, "HOST_CACHED"},
        {VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT, "LAZILY_ALLOCATED"},
        {VK_MEMORY_PROPERTY_PROTECTED_BIT, "PROTECTED"},
        {VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD, "DEVICE_COHERENT_AMD"},
        {VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD, "DEVICE_UNCACHED_AMD"},
    };
    std::string text;
    for (const auto& [flag, name] : NAMES)
    {
        if (flags & flag)
        {
            text += (text.empty() ? "" : " | ") + std::string(name);
        }
    }
    return text.empty() ? "без флагов" : text;
}

struct VkBuf
{
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize size = 0;
    uint32_t memoryType = UINT32_MAX;
    bool coherent = true;

    void create(VkPhysicalDevice physDev, VkDevice dev, VkDeviceSize bytes, VkBufferUsageFlags usage, const MemoryChoice& choice)
    {
        size = bytes;
        VkBufferCreateInfo bInfo{};
        bInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bInfo.size = bytes;
        bInfo.usage = usage;
        bInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        checkVk(vkCreateBuffer(dev, &bInfo, nullptr, &buf), "vkCreateBuffer");

        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(dev, buf, &req);
        memoryType = chooseMemoryType(physDev, req.memoryTypeBits, choice);
        if (memoryType == UINT32_MAX)
        {
            std::fprintf(stderr, "Нет типа памяти с флагами %s\n", memoryFlagsName(choice.required).c_str());
            std::exit(1);
        }
        VkMemoryPropertyFlags flags = memoryFlags(physDev, memoryType);
        coherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;

        VkMemoryAllocateInfo aInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size, memoryType};
        checkVk(vkAllocateMemory(dev, &aInfo, nullptr, &mem), "vkAllocateMemory");
        checkVk(vkBindBufferMemory(dev, buf, mem, 0), "vkBindBufferMemory");
        if (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
        {
            checkVk(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, &mapped), "vkMapMemory");
        }
    }

    void invalidate(VkDevice dev) const
    {
        if (mapped && !coherent)
        {
            VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, mem, 0, VK_WHOLE_SIZE};
            checkVk(vkInvalidateMappedMemoryRanges(dev, 1, &range), "vkInvalidateMappedMemoryRanges");
        }
    }

    void flush(VkDevice dev) const
    {
        if (mapped && !coherent)
        {
            VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, mem, 0, VK_WHOLE_SIZE};
            checkVk(vkFlushMappedMemoryRanges(dev, 1, &range), "vkFlushMappedMemoryRanges");
        }
    }

    void destroy(VkDevice dev)
    {
        if (mapped)
        {
            vkUnmapMemory(dev, mem);
        }
        vkDestroyBuffer(dev, buf, nullptr);
        vkFreeMemory(dev, mem, nullptr);
        *this = VkBuf{};
    }
};

static void memoryBarrier(VkCommandBuffer cmd, VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
    VkPipelineStageFlags dstStage, VkAccessFlags dstAccess)
{
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, srcAccess, dstAccess};
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 1, &barrier, 0, nullptr, 0, nullptr);
}

static void computeToHost(VkCommandBuffer cmd)
{
    memoryBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        VK_ACCESS_HOST_READ_BIT);
}

static void computeToCompute(VkCommandBuffer cmd)
{
    memoryBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
}

static void computeToTransfer(VkCommandBuffer cmd)
{
    memoryBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_TRANSFER_READ_BIT);
}

static void transferToCompute(VkCommandBuffer cmd)
{
    memoryBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
}

static void transferToHost(VkCommandBuffer cmd)
{
    memoryBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        VK_ACCESS_HOST_READ_BIT);
}

struct FloatControls
{
    bool denormPreserve = false;
    bool denormFlush = false;
    bool roundingRte = false;
    bool signedZeroInfNan = false;
    VkShaderFloatControlsIndependence independence = VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE;
};

static FloatControls queryFloatControls(VkPhysicalDevice physDev)
{
    FloatControls controls;
    VkPhysicalDeviceFloatControlsProperties floats{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES};
    VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &floats};
    vkGetPhysicalDeviceProperties2(physDev, &props);
    controls.denormPreserve = floats.shaderDenormPreserveFloat32 == VK_TRUE;
    controls.denormFlush = floats.shaderDenormFlushToZeroFloat32 == VK_TRUE;
    controls.roundingRte = floats.shaderRoundingModeRTEFloat32 == VK_TRUE;
    controls.signedZeroInfNan = floats.shaderSignedZeroInfNanPreserveFloat32 == VK_TRUE;
    controls.independence = floats.denormBehaviorIndependence;
    return controls;
}

static const char* yesNo(bool value)
{
    return value ? "да" : "нет";
}

struct ShaderFmaSupport
{
    bool headers = false;
    bool extension = false;
    bool float32 = false;
};

static ShaderFmaSupport queryShaderFma(VkPhysicalDevice physDev)
{
    ShaderFmaSupport support;
#ifdef VK_KHR_shader_fma
    support.headers = true;
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(physDev, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    vkEnumerateDeviceExtensionProperties(physDev, nullptr, &count, extensions.data());
    for (const VkExtensionProperties& extension : extensions)
    {
        support.extension = support.extension || std::strcmp(extension.extensionName, VK_KHR_SHADER_FMA_EXTENSION_NAME) == 0;
    }
    if (support.extension)
    {
        VkPhysicalDeviceShaderFmaFeaturesKHR fma{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FMA_FEATURES_KHR};
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &fma};
        vkGetPhysicalDeviceFeatures2(physDev, &features);
        support.float32 = fma.shaderFmaFloat32 == VK_TRUE;
    }
#else
    (void)physDev;
#endif
    return support;
}

static std::string shaderFmaText(const ShaderFmaSupport& support)
{
    if (!support.headers)
    {
        return "заголовки Vulkan старше расширения, не проверялось; умножитьСложить в строгом режиме — программно (soft_fma.glsl)";
    }
    if (!support.extension)
    {
        return "нет; умножитьСложить в строгом режиме — программно (soft_fma.glsl)";
    }
    return support.float32 ? "есть, shaderFmaFloat32 — да; умножитьСложить в строгом режиме — OpFmaKHR"
                           : "есть, но shaderFmaFloat32 — нет; умножитьСложить в строгом режиме — программно (soft_fma.glsl)";
}

static const char* independenceName(VkShaderFloatControlsIndependence value)
{
    if (value == VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_ALL)
    {
        return "свой для каждой разрядности";
    }
    if (value == VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_32_BIT_ONLY)
    {
        return "свой только для 32 бит";
    }
    return "общий для всех разрядностей";
}

struct GpuContext
{
    VkPhysicalDevice physDev = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    VkCommandBuffer cmdBuf = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    uint32_t timestampBits = 0;
    double timestampPeriodNs = 0.0;
    uint32_t maxGroups = 0;
    bool fmaKhr = false;

    bool create(VkPhysicalDevice device, bool enableFmaKhr)
    {
        physDev = device;
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(device, &props);
        timestampPeriodNs = props.limits.timestampPeriod;
        maxGroups = props.limits.maxComputeWorkGroupCount[0];

        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, families.data());
        uint32_t computeFamily = UINT32_MAX;
        for (uint32_t idx = 0; idx < familyCount; ++idx)
        {
            if (families[idx].queueFlags & VK_QUEUE_COMPUTE_BIT)
            {
                computeFamily = idx;
                break;
            }
        }
        if (computeFamily == UINT32_MAX)
        {
            return false;
        }
        timestampBits = families[computeFamily].timestampValidBits;

        float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, computeFamily, 1, &priority};
        VkDeviceCreateInfo devInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, nullptr, 0, 1, &queueInfo, 0, nullptr, 0, nullptr, nullptr};
#ifdef VK_KHR_shader_fma
        const char* fmaExtension = VK_KHR_SHADER_FMA_EXTENSION_NAME;
        VkPhysicalDeviceShaderFmaFeaturesKHR fmaFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FMA_FEATURES_KHR};
        fmaFeatures.shaderFmaFloat32 = VK_TRUE;
        if (enableFmaKhr)
        {
            devInfo.pNext = &fmaFeatures;
            devInfo.enabledExtensionCount = 1;
            devInfo.ppEnabledExtensionNames = &fmaExtension;
        }
        fmaKhr = enableFmaKhr;
#else
        (void)enableFmaKhr;
#endif
        checkVk(vkCreateDevice(device, &devInfo, nullptr, &dev), "vkCreateDevice");
        vkGetDeviceQueue(dev, computeFamily, 0, &queue);

        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
            VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, computeFamily};
        checkVk(vkCreateCommandPool(dev, &poolInfo, nullptr, &cmdPool), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, cmdPool,
            VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        checkVk(vkAllocateCommandBuffers(dev, &allocInfo, &cmdBuf), "vkAllocateCommandBuffers");
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        checkVk(vkCreateFence(dev, &fenceInfo, nullptr, &fence), "vkCreateFence");
        return true;
    }

    void begin()
    {
        VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        checkVk(vkBeginCommandBuffer(cmdBuf, &beginInfo), "vkBeginCommandBuffer");
    }

    void end()
    {
        checkVk(vkEndCommandBuffer(cmdBuf), "vkEndCommandBuffer");
    }

    void submitAndWait()
    {
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &cmdBuf, 0, nullptr};
        checkVk(vkResetFences(dev, 1, &fence), "vkResetFences");
        checkVk(vkQueueSubmit(queue, 1, &submit, fence), "vkQueueSubmit");
        checkVk(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    }

    void finish()
    {
        end();
        submitAndWait();
    }

    double ticksToMs(uint64_t from, uint64_t to) const
    {
        uint64_t mask = timestampBits >= 64 ? ~0ull : ((1ull << timestampBits) - 1);
        return static_cast<double>((to - from) & mask) * timestampPeriodNs / 1.0e6;
    }

    void destroy()
    {
        vkDestroyFence(dev, fence, nullptr);
        vkDestroyCommandPool(dev, cmdPool, nullptr);
        vkDestroyDevice(dev, nullptr);
    }
};

struct ComputeLayout
{
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
    uint32_t bindingCount = 0;

    void create(VkDevice dev, uint32_t bindings, uint32_t pushBytes)
    {
        bindingCount = bindings;
        std::vector<VkDescriptorSetLayoutBinding> slots(bindings);
        for (uint32_t idx = 0; idx < bindings; ++idx)
        {
            slots[idx] = {idx, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        }
        VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, bindings, slots.data()};
        checkVk(vkCreateDescriptorSetLayout(dev, &setInfo, nullptr, &setLayout), "vkCreateDescriptorSetLayout");
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, pushBytes};
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1, &setLayout, 1, &range};
        checkVk(vkCreatePipelineLayout(dev, &layoutInfo, nullptr, &pipeLayout), "vkCreatePipelineLayout");
    }

    VkDescriptorSet bind(VkDevice dev, VkDescriptorPool pool, const std::vector<const VkBuf*>& buffers) const
    {
        if (buffers.size() != bindingCount)
        {
            std::fprintf(stderr, "Набор из %zu буферов для раскладки из %u привязок\n", buffers.size(), bindingCount);
            std::exit(1);
        }
        VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, pool, 1, &setLayout};
        VkDescriptorSet set = VK_NULL_HANDLE;
        checkVk(vkAllocateDescriptorSets(dev, &allocInfo, &set), "vkAllocateDescriptorSets");
        std::vector<VkDescriptorBufferInfo> infos(buffers.size());
        std::vector<VkWriteDescriptorSet> writes(buffers.size());
        for (size_t idx = 0; idx < buffers.size(); ++idx)
        {
            infos[idx] = {buffers[idx]->buf, 0, buffers[idx]->size};
            writes[idx] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, static_cast<uint32_t>(idx), 0, 1,
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &infos[idx], nullptr};
        }
        vkUpdateDescriptorSets(dev, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        return set;
    }

    void destroy(VkDevice dev)
    {
        vkDestroyPipelineLayout(dev, pipeLayout, nullptr);
        vkDestroyDescriptorSetLayout(dev, setLayout, nullptr);
    }
};

struct ComputePipeline
{
    VkShaderModule shader = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;

    void create(VkDevice dev, const ComputeLayout& layout, const std::vector<uint32_t>& spv, bool traced = false)
    {
        VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, spv.size() * sizeof(uint32_t),
            spv.data()};
        checkVk(vkCreateShaderModule(dev, &moduleInfo, nullptr, &shader), "vkCreateShaderModule");
        VkBool32 traceValue = VK_TRUE;
        VkSpecializationMapEntry traceEntry{0, 0, sizeof(VkBool32)};
        VkSpecializationInfo traceSpec{1, &traceEntry, sizeof(VkBool32), &traceValue};
        VkComputePipelineCreateInfo pipeInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipeInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, shader, "main",
            traced ? &traceSpec : nullptr};
        pipeInfo.layout = layout.pipeLayout;
        checkVk(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pipeInfo, nullptr, &pipeline), "vkCreateComputePipelines");
    }

    void destroy(VkDevice dev)
    {
        vkDestroyPipeline(dev, pipeline, nullptr);
        vkDestroyShaderModule(dev, shader, nullptr);
    }
};

static VkDescriptorPool makeDescriptorPool(VkDevice dev, uint32_t sets, uint32_t buffers)
{
    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, buffers};
    VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, sets, 1, &poolSize};
    VkDescriptorPool pool = VK_NULL_HANDLE;
    checkVk(vkCreateDescriptorPool(dev, &info, nullptr, &pool), "vkCreateDescriptorPool");
    return pool;
}

static void dispatchKernel(VkCommandBuffer cmd, const ComputeLayout& layout, VkPipeline pipeline, VkDescriptorSet set,
    const void* push, uint32_t pushBytes, uint32_t groups)
{
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout.pipeLayout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, layout.pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, pushBytes, push);
    vkCmdDispatch(cmd, groups, 1, 1);
}

template <class Body>
static double medianMs(int runs, Body body)
{
    std::vector<double> times;
    for (int run = 0; run < runs; ++run)
    {
        Clock::time_point start = Clock::now();
        body();
        times.push_back(msSince(start));
    }
    std::nth_element(times.begin(), times.begin() + runs / 2, times.end());
    return times[runs / 2];
}

struct SplitMix
{
    uint64_t state;

    uint64_t next()
    {
        uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    uint32_t below(uint32_t bound)
    {
        return static_cast<uint32_t>(next() % bound);
    }

    int32_t between(int32_t low, int32_t high)
    {
        return low + static_cast<int32_t>(below(static_cast<uint32_t>(high - low + 1)));
    }

    uint32_t sign()
    {
        return static_cast<uint32_t>(next() & 1u) << 31;
    }
};

static float normalFloat(SplitMix& rng, int32_t lowExponent, int32_t highExponent)
{
    uint32_t exponent = static_cast<uint32_t>(rng.between(lowExponent, highExponent) + 127) << 23;
    uint32_t mantissa = static_cast<uint32_t>(rng.next()) & 0x7FFFFFu;
    return fromBits(rng.sign() | exponent | mantissa);
}

static float subnormalFloat(SplitMix& rng)
{
    uint32_t mantissa = std::max<uint32_t>(static_cast<uint32_t>(rng.next()) & 0x7FFFFFu, 1u);
    return fromBits(rng.sign() | mantissa);
}

static float edgeMantissaFloat(SplitMix& rng)
{
    uint32_t exponent = static_cast<uint32_t>(rng.between(-10, 10) + 127) << 23;
    uint32_t offset = rng.below(64);
    uint32_t mantissa = (rng.next() & 1u) ? offset : (0x7FFFFFu - offset);
    return fromBits(rng.sign() | exponent | mantissa);
}

static float positive(float value)
{
    return fromBits(toBits(value) & 0x7FFFFFFFu);
}

static bool isSubnormal(float value)
{
    uint32_t magnitude = toBits(value) & 0x7FFFFFFFu;
    return magnitude != 0u && magnitude < 0x00800000u;
}

enum DivisionGroup : uint8_t
{
    DIVISION_ORDINARY,
    DIVISION_HUGE_DIVISOR,
    DIVISION_SUBNORMAL,
    DIVISION_GROUPS,
};

enum RootGroup : uint8_t
{
    ROOT_NORMAL,
    ROOT_SUBNORMAL,
    ROOT_GROUPS,
};

static const char* const DIVISION_NAMES[DIVISION_GROUPS] = {
    "деление, обычные",
    "деление, |делитель| ≥ 2^126",
    "деление, денормализованные",
};

static const char* const ROOT_NAMES[ROOT_GROUPS] = {
    "корень, нормальные",
    "корень, денормализованные",
};

static DivisionGroup classifyDivision(float numerator, float denominator, float quotient)
{
    bool underflow = quotient == 0.0f && numerator != 0.0f;
    if (isSubnormal(numerator) || isSubnormal(denominator) || isSubnormal(quotient) || underflow)
    {
        return DIVISION_SUBNORMAL;
    }
    if (std::fabs(denominator) >= 0x1p126f)
    {
        return DIVISION_HUGE_DIVISOR;
    }
    return DIVISION_ORDINARY;
}

constexpr uint32_t FMA_PROBE_TRIPLES = 1u << 16;
constexpr uint64_t FMA_PROBE_SEED = 0x5EED0050A0000010ull;
constexpr uint32_t FMA_LANES = 3;

static_assert(sizeof(FmaTriple) == 3 * sizeof(uint32_t));

struct ProbeInputs
{
    std::vector<float> lanes;
    std::vector<uint32_t> wantQuotient;
    std::vector<uint32_t> wantRoot;
    std::vector<DivisionGroup> divisionGroup;
    std::vector<RootGroup> rootGroup;
    std::vector<FmaTriple> fmaTriples;
    std::vector<uint32_t> wantFma;
    std::vector<FmaGroup> fmaGroup;
};

static ProbeInputs makeProbeInputs()
{
    ProbeInputs inputs;
    inputs.lanes.assign(static_cast<size_t>(PROBE_PAIRS) * 4, 0.0f);
    inputs.wantQuotient.resize(PROBE_PAIRS);
    inputs.wantRoot.resize(PROBE_PAIRS);
    inputs.divisionGroup.resize(PROBE_PAIRS);
    inputs.rootGroup.resize(PROBE_PAIRS);
    SplitMix rng{0x5EED0050ull};
    for (uint32_t idx = 0; idx < PROBE_PAIRS; ++idx)
    {
        uint32_t kind = idx % 16;
        float numerator = 0.0f;
        float denominator = 0.0f;
        if (kind < 12)
        {
            numerator = normalFloat(rng, -40, 40);
            denominator = normalFloat(rng, -40, 40);
        }
        else if (kind == 12)
        {
            numerator = normalFloat(rng, 100, 127);
            denominator = normalFloat(rng, 126, 127);
        }
        else if (kind == 13)
        {
            numerator = normalFloat(rng, -126, -100);
            denominator = normalFloat(rng, 10, 40);
        }
        else if (kind == 14)
        {
            numerator = subnormalFloat(rng);
            denominator = normalFloat(rng, -30, 5);
        }
        else
        {
            numerator = edgeMantissaFloat(rng);
            denominator = edgeMantissaFloat(rng);
        }
        float radicand = kind < 14 ? positive(normalFloat(rng, -126, 127)) : positive(subnormalFloat(rng));
        float quotient = numerator / denominator;

        inputs.lanes[4 * static_cast<size_t>(idx) + 0] = numerator;
        inputs.lanes[4 * static_cast<size_t>(idx) + 1] = denominator;
        inputs.lanes[4 * static_cast<size_t>(idx) + 2] = radicand;
        inputs.wantQuotient[idx] = toBits(quotient);
        inputs.wantRoot[idx] = toBits(std::sqrt(radicand));
        inputs.divisionGroup[idx] = classifyDivision(numerator, denominator, quotient);
        inputs.rootGroup[idx] = isSubnormal(radicand) ? ROOT_SUBNORMAL : ROOT_NORMAL;
    }
    for (uint32_t group = 0; group < FMA_GROUPS; ++group)
    {
        for (const FmaTriple& triple : fmaGroupTriples(static_cast<FmaGroup>(group), FMA_PROBE_TRIPLES, FMA_PROBE_SEED))
        {
            inputs.fmaTriples.push_back(triple);
            inputs.wantFma.push_back(referenceFmaBits(triple.multiplicand, triple.multiplier, triple.addend));
            inputs.fmaGroup.push_back(static_cast<FmaGroup>(group));
        }
    }
    return inputs;
}

struct ScalarProbe
{
    float inputs[11];
    uint32_t expected[8];
};

static ScalarProbe makeScalarProbe()
{
    volatile float a = 1.0f + std::ldexp(1.0f, -12);
    volatile float b = 1.0f + std::ldexp(1.0f, -12);
    volatile float c = -1.0f;
    volatile float divNum = 1.0f;
    volatile float divDen = 7.0f;
    volatile float sqArg = 3.0f;
    volatile float tinyA = 1.0e-38f;
    volatile float tinyB = 1.0e-4f;
    volatile float prod = a * b;

    ScalarProbe probe{{a, b, c, divNum, divDen, sqArg, tinyA, tinyB, a, b, c}, {}};
    probe.expected[0] = toBits(prod + c);
    probe.expected[1] = toBits(divNum / divDen);
    probe.expected[2] = toBits(std::sqrt(static_cast<float>(sqArg)));
    probe.expected[3] = toBits(tinyA * tinyB);
    probe.expected[4] = referenceFmaBits(toBits(a), toBits(b), toBits(c));
    probe.expected[5] = probe.expected[1];
    probe.expected[6] = probe.expected[2];
    probe.expected[7] = probe.expected[4];
    return probe;
}

struct Tally
{
    uint32_t total = 0;
    uint32_t wrong = 0;
    uint32_t zeroed = 0;
    uint32_t nonFinite = 0;
    uint32_t maxUlp = 0;

    void add(uint32_t got, uint32_t want)
    {
        ++total;
        bool gotNan = (got & 0x7FFFFFFFu) > 0x7F800000u;
        bool wantNan = (want & 0x7FFFFFFFu) > 0x7F800000u;
        if (got == want || (gotNan && wantNan))
        {
            return;
        }
        ++wrong;
        if ((got & 0x7FFFFFFFu) == 0u && (want & 0x7FFFFFFFu) != 0u)
        {
            ++zeroed;
            return;
        }
        if ((got & 0x7F800000u) == 0x7F800000u || (want & 0x7F800000u) == 0x7F800000u)
        {
            ++nonFinite;
            return;
        }
        maxUlp = std::max(maxUlp, ulpDistance(got, want));
    }

    std::string describe() const
    {
        if (wrong == 0)
        {
            return "все верны";
        }
        std::string text = "неверно " + std::to_string(wrong);
        std::string details;
        auto append = [&details](const std::string& part)
        {
            details += details.empty() ? part : ", " + part;
        };
        if (maxUlp > 0)
        {
            append("до " + std::to_string(maxUlp) + " ulp");
        }
        if (zeroed > 0)
        {
            append("ноль вместо числа " + std::to_string(zeroed));
        }
        if (nonFinite > 0)
        {
            append("бесконечность или NaN " + std::to_string(nonFinite));
        }
        return details.empty() ? text : text + " (" + details + ")";
    }
};

enum ProbeLane : uint32_t
{
    LANE_HARDWARE = 0,
    LANE_SOFT = 1,
    LANE_APPROX = 2,
};

struct ProbeResult
{
    uint32_t scalars[8] = {};
    std::vector<uint32_t> lanes;

    uint32_t quotient(size_t item, ProbeLane lane) const
    {
        return lanes[PROBE_LANES * item + lane];
    }

    uint32_t root(size_t item, ProbeLane lane) const
    {
        return lanes[PROBE_LANES * item + 3 + lane];
    }
};

static ProbeResult runProbe(GpuContext& gpu, const std::vector<uint32_t>& spv, const ScalarProbe& scalar, const std::vector<float>& items)
{
    uint32_t itemCount = static_cast<uint32_t>(items.size() / 4);
    VkDeviceSize inBytes = static_cast<VkDeviceSize>(items.size()) * sizeof(float);
    VkDeviceSize outBytes = static_cast<VkDeviceSize>(itemCount) * PROBE_LANES * sizeof(uint32_t);
    VkBuf scalarIn;
    VkBuf scalarOut;
    VkBuf pairIn;
    VkBuf pairOut;
    scalarIn.create(gpu.physDev, gpu.dev, sizeof(scalar.inputs), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, UPLOAD_MEMORY);
    scalarOut.create(gpu.physDev, gpu.dev, sizeof(ProbeResult::scalars), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, READBACK_MEMORY);
    pairIn.create(gpu.physDev, gpu.dev, inBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, UPLOAD_MEMORY);
    pairOut.create(gpu.physDev, gpu.dev, outBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, READBACK_MEMORY);
    std::memcpy(scalarIn.mapped, scalar.inputs, sizeof(scalar.inputs));
    std::memcpy(pairIn.mapped, items.data(), static_cast<size_t>(inBytes));

    ComputeLayout layout;
    layout.create(gpu.dev, 4, sizeof(uint32_t));
    ComputePipeline pipeline;
    pipeline.create(gpu.dev, layout, spv);
    VkDescriptorPool pool = makeDescriptorPool(gpu.dev, 1, 4);
    VkDescriptorSet set = layout.bind(gpu.dev, pool, {&scalarIn, &scalarOut, &pairIn, &pairOut});

    gpu.begin();
    dispatchKernel(gpu.cmdBuf, layout, pipeline.pipeline, set, &itemCount, sizeof(itemCount), std::max(groupsFor(itemCount), 1u));
    computeToHost(gpu.cmdBuf);
    gpu.finish();

    ProbeResult result;
    scalarOut.invalidate(gpu.dev);
    pairOut.invalidate(gpu.dev);
    std::memcpy(result.scalars, scalarOut.mapped, sizeof(result.scalars));
    result.lanes.resize(static_cast<size_t>(itemCount) * PROBE_LANES);
    std::memcpy(result.lanes.data(), pairOut.mapped, static_cast<size_t>(outBytes));

    vkDestroyDescriptorPool(gpu.dev, pool, nullptr);
    pipeline.destroy(gpu.dev);
    layout.destroy(gpu.dev);
    for (VkBuf* buffer : {&scalarIn, &scalarOut, &pairIn, &pairOut})
    {
        buffer->destroy(gpu.dev);
    }
    return result;
}

static void reportProbe(const std::string& modeName, const ScalarProbe& scalar, const ProbeInputs& inputs, const ProbeResult& result)
{
    auto mark = [&](int idx, const char* failure)
    {
        return result.scalars[idx] == scalar.expected[idx] ? "ОК" : failure;
    };
    bool subnormalKept = result.scalars[3] == scalar.expected[3] && result.scalars[3] != 0u;
    std::printf("    Пробы (%s): a*b+c=%s | 1/7=%s | корень 3=%s | денормализованные=%s | fma рядом с a*b=%s, отдельно=%s | доводка 1/7=%s, "
        "корень 3=%s\n",
        modeName.c_str(), mark(0, "СЖАТО"), mark(1, "НЕВЕРНО"), mark(2, "НЕВЕРНО"), subnormalKept ? "ОК" : "FTZ",
        mark(4, "НЕ СЛИТ"), mark(7, "НЕ СЛИТ"), mark(5, "НЕВЕРНО"), mark(6, "НЕВЕРНО"));

    Tally division[DIVISION_GROUPS][3];
    Tally root[ROOT_GROUPS][3];
    for (uint32_t idx = 0; idx < PROBE_PAIRS; ++idx)
    {
        for (ProbeLane lane : {LANE_HARDWARE, LANE_SOFT, LANE_APPROX})
        {
            division[inputs.divisionGroup[idx]][lane].add(result.quotient(idx, lane), inputs.wantQuotient[idx]);
            root[inputs.rootGroup[idx]][lane].add(result.root(idx, lane), inputs.wantRoot[idx]);
        }
    }

    std::printf("    Деление и корень на %u входах (%s):\n", PROBE_PAIRS, modeName.c_str());
    for (uint32_t group = 0; group < DIVISION_GROUPS; ++group)
    {
        std::printf("      %s (%u): аппаратно — %s; доводка — %s; приближённо — %s\n", DIVISION_NAMES[group],
            division[group][LANE_HARDWARE].total, division[group][LANE_HARDWARE].describe().c_str(),
            division[group][LANE_SOFT].describe().c_str(), division[group][LANE_APPROX].describe().c_str());
    }
    for (uint32_t group = 0; group < ROOT_GROUPS; ++group)
    {
        std::printf("      %s (%u): аппаратно — %s; доводка — %s; приближённо — %s\n", ROOT_NAMES[group],
            root[group][LANE_HARDWARE].total, root[group][LANE_HARDWARE].describe().c_str(),
            root[group][LANE_SOFT].describe().c_str(), root[group][LANE_APPROX].describe().c_str());
    }
}

static std::vector<uint32_t> runFmaProbe(GpuContext& gpu, const std::vector<uint32_t>& spv, const std::vector<FmaTriple>& triples)
{
    uint32_t tripleCount = static_cast<uint32_t>(triples.size());
    VkDeviceSize bytes = static_cast<VkDeviceSize>(tripleCount) * FMA_LANES * sizeof(uint32_t);
    VkBuf input;
    VkBuf output;
    input.create(gpu.physDev, gpu.dev, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, UPLOAD_MEMORY);
    output.create(gpu.physDev, gpu.dev, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, READBACK_MEMORY);
    std::memcpy(input.mapped, triples.data(), static_cast<size_t>(bytes));

    ComputeLayout layout;
    layout.create(gpu.dev, 2, sizeof(uint32_t));
    ComputePipeline pipeline;
    pipeline.create(gpu.dev, layout, spv);
    VkDescriptorPool pool = makeDescriptorPool(gpu.dev, 1, 2);
    VkDescriptorSet set = layout.bind(gpu.dev, pool, {&input, &output});

    gpu.begin();
    dispatchKernel(gpu.cmdBuf, layout, pipeline.pipeline, set, &tripleCount, sizeof(tripleCount), std::max(groupsFor(tripleCount), 1u));
    computeToHost(gpu.cmdBuf);
    gpu.finish();

    output.invalidate(gpu.dev);
    std::vector<uint32_t> lanes(static_cast<size_t>(tripleCount) * FMA_LANES);
    std::memcpy(lanes.data(), output.mapped, static_cast<size_t>(bytes));

    vkDestroyDescriptorPool(gpu.dev, pool, nullptr);
    pipeline.destroy(gpu.dev);
    layout.destroy(gpu.dev);
    input.destroy(gpu.dev);
    output.destroy(gpu.dev);
    return lanes;
}

static void reportFmaProbe(const std::string& modeName, const ProbeInputs& inputs, const std::vector<uint32_t>& lanes, bool khr)
{
    Tally tally[FMA_GROUPS][FMA_LANES];
    for (size_t idx = 0; idx < inputs.fmaTriples.size(); ++idx)
    {
        for (uint32_t lane = 0; lane < (khr ? FMA_LANES : 2u); ++lane)
        {
            tally[inputs.fmaGroup[idx]][lane].add(lanes[FMA_LANES * idx + lane], inputs.wantFma[idx]);
        }
    }
    std::printf("    fma на %zu тройках (%s), эталон на ЦПУ с округлением к нечётному:\n", inputs.fmaTriples.size(), modeName.c_str());
    for (uint32_t group = 0; group < FMA_GROUPS; ++group)
    {
        std::string khrText = khr ? "; OpFmaKHR — " + tally[group][2].describe() : "";
        std::printf("      %s (%u): fma() — %s; программный — %s%s\n", FMA_GROUP_NAMES[group], tally[group][0].total,
            tally[group][0].describe().c_str(), tally[group][1].describe().c_str(), khrText.c_str());
    }
}

struct BenchCase
{
    std::string name;
    World world;
    Columns particles;
    Columns sorted;
    std::vector<uint32_t> starts;
    size_t contacts = 0;
};

struct ParticleBuffers
{
    VkDevice dev = VK_NULL_HANDLE;
    std::vector<VkBuf> staging;
    std::vector<VkBuf> readback;
    std::vector<VkBuf> legacy;
    std::vector<VkBuf> device;
    VkBuf trace;

    void create(GpuContext& gpu, size_t particles, size_t startCount)
    {
        dev = gpu.dev;
        auto columnBytes = [&](uint32_t column)
        {
            return static_cast<VkDeviceSize>(column == STARTS_COLUMN ? startCount : particles) * sizeof(float);
        };
        staging.resize(INPUT_COLUMNS);
        readback.resize(OUTPUT_COLUMNS);
        legacy.resize(OUTPUT_COLUMNS);
        device.resize(PARTICLE_COLUMNS);
        for (uint32_t column = 0; column < INPUT_COLUMNS; ++column)
        {
            staging[column].create(gpu.physDev, gpu.dev, columnBytes(column), COLUMN_USAGE, UPLOAD_MEMORY);
        }
        for (uint32_t column = 0; column < OUTPUT_COLUMNS; ++column)
        {
            readback[column].create(gpu.physDev, gpu.dev, columnBytes(0), COLUMN_USAGE, READBACK_MEMORY);
            legacy[column].create(gpu.physDev, gpu.dev, columnBytes(0), COLUMN_USAGE, LEGACY_READBACK_MEMORY);
        }
        for (uint32_t column = 0; column < PARTICLE_COLUMNS; ++column)
        {
            device[column].create(gpu.physDev, gpu.dev, columnBytes(column), COLUMN_USAGE, DEVICE_MEMORY);
        }
        trace.create(gpu.physDev, gpu.dev, TRACE_WORDS * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, READBACK_MEMORY);
        if (legacy.front().memoryType == readback.front().memoryType)
        {
            for (VkBuf& buffer : legacy)
            {
                buffer.destroy(dev);
            }
            legacy.clear();
        }
    }

    void pack(const Columns& sorted, const std::vector<uint32_t>& starts)
    {
        size_t bytes = sorted.size() * sizeof(float);
        const void* sources[OUTPUT_COLUMNS] = {sorted.px.data(), sorted.py.data(), sorted.pz.data(), sorted.vx.data(),
            sorted.vy.data(), sorted.vz.data(), sorted.id.data()};
        for (uint32_t column = 0; column < OUTPUT_COLUMNS; ++column)
        {
            std::memcpy(staging[column].mapped, sources[column], bytes);
        }
        std::memcpy(staging[STARTS_COLUMN].mapped, starts.data(), starts.size() * sizeof(uint32_t));
    }

    void clearOutputs(size_t count)
    {
        for (VkBuf& buffer : readback)
        {
            std::memset(buffer.mapped, 0, count * sizeof(float));
            buffer.flush(dev);
        }
    }

    void unpack(Columns& out, const std::vector<VkBuf>& columns) const
    {
        size_t bytes = out.size() * sizeof(float);
        void* targets[OUTPUT_COLUMNS] = {out.px.data(), out.py.data(), out.pz.data(), out.vx.data(), out.vy.data(), out.vz.data(),
            out.id.data()};
        for (uint32_t column = 0; column < OUTPUT_COLUMNS; ++column)
        {
            columns[column].invalidate(dev);
            std::memcpy(targets[column], columns[column].mapped, bytes);
        }
    }

    Columns download(size_t count) const
    {
        Columns out;
        out.resize(count);
        unpack(out, readback);
        return out;
    }

    void recordCopyIn(VkCommandBuffer cmd, size_t count, size_t startCount) const
    {
        for (uint32_t column = 0; column < INPUT_COLUMNS; ++column)
        {
            VkBufferCopy region{0, 0, static_cast<VkDeviceSize>(column == STARTS_COLUMN ? startCount : count) * sizeof(float)};
            vkCmdCopyBuffer(cmd, staging[column].buf, device[column].buf, 1, &region);
        }
    }

    void recordCopyOut(VkCommandBuffer cmd, size_t count, const std::vector<VkBuf>& columns) const
    {
        VkBufferCopy region{0, 0, static_cast<VkDeviceSize>(count) * sizeof(float)};
        for (uint32_t column = 0; column < OUTPUT_COLUMNS; ++column)
        {
            vkCmdCopyBuffer(cmd, device[INPUT_COLUMNS + column].buf, columns[column].buf, 1, &region);
        }
    }

    void copyInputsToDevice(GpuContext& gpu, size_t count, size_t startCount)
    {
        gpu.begin();
        recordCopyIn(gpu.cmdBuf, count, startCount);
        transferToCompute(gpu.cmdBuf);
        gpu.finish();
    }

    std::vector<const VkBuf*> hostBindings() const
    {
        std::vector<const VkBuf*> list;
        for (const VkBuf& buffer : staging)
        {
            list.push_back(&buffer);
        }
        for (const VkBuf& buffer : readback)
        {
            list.push_back(&buffer);
        }
        list.push_back(&trace);
        return list;
    }

    std::vector<const VkBuf*> deviceBindings() const
    {
        std::vector<const VkBuf*> list;
        for (const VkBuf& buffer : device)
        {
            list.push_back(&buffer);
        }
        list.push_back(&trace);
        return list;
    }

    void destroy()
    {
        for (std::vector<VkBuf>* group : {&staging, &readback, &legacy, &device})
        {
            for (VkBuf& buffer : *group)
            {
                buffer.destroy(dev);
            }
        }
        trace.destroy(dev);
    }
};

static void recordStep(VkCommandBuffer cmd, const ComputeLayout& layout, VkPipeline pipeline, VkDescriptorSet set,
    const PushWorld& push, uint32_t groups)
{
    dispatchKernel(cmd, layout, pipeline, set, &push, sizeof(PushWorld), groups);
}

struct StepTiming
{
    double kernelMs = -1.0;
    double batchMs = 0.0;
    double waitedMs = 0.0;
};

static StepTiming measureSteps(GpuContext& gpu, VkQueryPool queries, const ComputeLayout& layout, VkPipeline pipeline,
    VkDescriptorSet set, const PushWorld& push, uint32_t groups)
{
    bool stamped = queries != VK_NULL_HANDLE;
    auto recordBatch = [&](uint32_t steps, bool withStamps)
    {
        gpu.begin();
        if (withStamps)
        {
            vkCmdResetQueryPool(gpu.cmdBuf, queries, 0, 2 * steps);
        }
        for (uint32_t step = 0; step < steps; ++step)
        {
            if (step > 0)
            {
                computeToCompute(gpu.cmdBuf);
            }
            if (withStamps)
            {
                vkCmdWriteTimestamp(gpu.cmdBuf, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, queries, 2 * step);
            }
            recordStep(gpu.cmdBuf, layout, pipeline, set, push, groups);
            if (withStamps)
            {
                vkCmdWriteTimestamp(gpu.cmdBuf, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, queries, 2 * step + 1);
            }
        }
        gpu.end();
    };

    recordBatch(5, false);
    gpu.submitAndWait();

    StepTiming timing;
    Clock::time_point start = Clock::now();
    for (uint32_t step = 0; step < BENCH_STEPS; ++step)
    {
        gpu.begin();
        recordStep(gpu.cmdBuf, layout, pipeline, set, push, groups);
        gpu.finish();
    }
    timing.waitedMs = msSince(start) / BENCH_STEPS;

    recordBatch(BENCH_STEPS, stamped);
    start = Clock::now();
    gpu.submitAndWait();
    timing.batchMs = msSince(start) / BENCH_STEPS;

    if (stamped)
    {
        std::vector<uint64_t> ticks(2 * BENCH_STEPS);
        checkVk(vkGetQueryPoolResults(gpu.dev, queries, 0, 2 * BENCH_STEPS, ticks.size() * sizeof(uint64_t), ticks.data(),
            sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT), "vkGetQueryPoolResults");
        std::vector<double> kernels(BENCH_STEPS);
        for (uint32_t step = 0; step < BENCH_STEPS; ++step)
        {
            kernels[step] = gpu.ticksToMs(ticks[2 * step], ticks[2 * step + 1]);
        }
        std::nth_element(kernels.begin(), kernels.begin() + BENCH_STEPS / 2, kernels.end());
        timing.kernelMs = kernels[BENCH_STEPS / 2];
    }
    return timing;
}

static double bestOf(const StepTiming& timing)
{
    return timing.kernelMs >= 0.0 ? timing.kernelMs : timing.batchMs;
}

static void printTiming(const std::string& label, const StepTiming& timing)
{
    if (timing.kernelMs >= 0.0)
    {
        std::printf("      %s: %.3f | %.3f | %.3f\n", label.c_str(), timing.kernelMs, timing.batchMs, timing.waitedMs);
    }
    else
    {
        std::printf("      %s: — | %.3f | %.3f\n", label.c_str(), timing.batchMs, timing.waitedMs);
    }
}

static void distributeAndSort(const World& world, const Columns& particles, Columns& sorted, std::vector<uint32_t>& starts,
    unsigned threads)
{
    size_t count = particles.size();
    std::vector<uint32_t> keys(count);
    std::vector<uint32_t> order(count);
    starts.assign(cellCount(world) + 1, 0);
    RadixDistributor distributor;
    parallelFor(count, threads, [&](size_t begin, size_t end)
    {
        for (size_t idx = begin; idx < end; ++idx)
        {
            keys[idx] = cellKey(idx, particles, world);
        }
    });
    distributor.distribute(keys, cellCount(world), threads, order, starts);
    sorted.resize(count);
    parallelFor(count, threads, [&](size_t begin, size_t end)
    {
        for (size_t idx = begin; idx < end; ++idx)
        {
            sorted.store(idx, particles.load(order[idx]));
        }
    });
}

static Columns initialColumns(const World& world)
{
    Columns particles;
    size_t count = particleCount(world);
    particles.resize(count);
    for (size_t idx = 0; idx < count; ++idx)
    {
        particles.store(idx, initialParticle(idx, world));
    }
    return particles;
}

static void stepOnCpu(const World& world, const Columns& sorted, const std::vector<uint32_t>& starts, Columns& out,
    unsigned threads)
{
    if (out.size() != sorted.size())
    {
        out.resize(sorted.size());
    }
    parallelFor(sorted.size(), threads, [&](size_t begin, size_t end)
    {
        for (size_t idx = begin; idx < end; ++idx)
        {
            out.store(idx, updateParticle(idx, sorted, starts.data(), world));
        }
    });
}

static unsigned threadsFor(size_t count)
{
    return static_cast<unsigned>(std::clamp<size_t>((count + CHUNK - 1) / CHUNK, 1, cpuThreads()));
}

struct Reference
{
    World world;
    Columns start;
    std::vector<uint64_t> hashes;
    std::vector<Columns> checkpoints;
    std::vector<uint64_t> checkpointHashes;
    std::vector<Contacts> checkpointContacts;
    uint32_t firstContact = 0;
    double cpuMs = 0.0;
};

static Reference makeReference()
{
    Reference reference;
    reference.world = worldFor(TRAJECTORY_SIDE);
    reference.hashes.assign(TRAJECTORY_STEPS + 1, 0);
    reference.start = initialColumns(reference.world);
    Columns state = reference.start;
    unsigned threads = threadsFor(state.size());
    Columns sorted;
    std::vector<uint32_t> starts;
    uint32_t next = 0;
    Clock::time_point began = Clock::now();
    for (uint32_t step = 1; step <= TRAJECTORY_STEPS; ++step)
    {
        distributeAndSort(reference.world, state, sorted, starts, threads);
        bool checkpoint = next < CHECKPOINT_COUNT && CHECKPOINTS[next] == step;
        if (reference.firstContact == 0 || checkpoint)
        {
            Contacts contacts = countContacts(reference.world, sorted, starts);
            if (reference.firstContact == 0 && contacts.pairs > 0)
            {
                reference.firstContact = step;
            }
            if (checkpoint)
            {
                reference.checkpointContacts.push_back(contacts);
            }
        }
        stepOnCpu(reference.world, sorted, starts, state, threads);
        reference.hashes[step] = hashInOrder(state);
        if (checkpoint)
        {
            reference.checkpoints.push_back(state);
            reference.checkpointHashes.push_back(hashById(state));
            ++next;
        }
    }
    reference.cpuMs = msSince(began);
    return reference;
}

static Columns stepOnGpu(GpuContext& gpu, ParticleBuffers& buffers, const ComputeLayout& layout, VkPipeline pipeline,
    VkDescriptorSet set, const World& world, const Columns& sorted, const std::vector<uint32_t>& starts, uint32_t traceIndex)
{
    buffers.pack(sorted, starts);
    buffers.clearOutputs(sorted.size());
    if (traceIndex != NO_TRACE)
    {
        std::memset(buffers.trace.mapped, 0xFF, static_cast<size_t>(buffers.trace.size));
        buffers.trace.flush(gpu.dev);
    }
    PushWorld push = pushFor(world, sorted.size());
    push.traceIndex = traceIndex;
    gpu.begin();
    recordStep(gpu.cmdBuf, layout, pipeline, set, push, groupsFor(sorted.size()));
    computeToHost(gpu.cmdBuf);
    gpu.finish();
    buffers.trace.invalidate(gpu.dev);
    return buffers.download(sorted.size());
}

struct TrajectoryVariant
{
    std::string name;
    const std::vector<uint32_t>* spv;
    VkPipeline pipeline;
};

struct TrajectoryOutcome
{
    uint32_t firstDivergence = 0;
    size_t finalDiffering = 0;
};

static void explainDivergence(GpuContext& gpu, ParticleBuffers& buffers, const ComputeLayout& layout, VkDescriptorSet set,
    const TrajectoryVariant& variant, const World& world, const Columns& sorted, const std::vector<uint32_t>& starts,
    const Columns& gpuOut, uint32_t step)
{
    Columns cpuOut;
    stepOnCpu(world, sorted, starts, cpuOut, threadsFor(sorted.size()));
    size_t perField[6] = {};
    std::vector<uint32_t> differing = differingPlaces(cpuOut, gpuOut, perField);
    std::printf("      первое расхождение — шаг %u, вход одинаков: различаются %zu из %zu частиц (%s)\n", step, differing.size(),
        sorted.size(), fieldCounts(perField).c_str());
    if (differing.empty())
    {
        return;
    }

    ComputePipeline traced;
    traced.create(gpu.dev, layout, *variant.spv, true);
    for (size_t shown = 0; shown < std::min<size_t>(differing.size(), TRACED_PARTICLES); ++shown)
    {
        uint32_t index = differing[shown];
        Columns tracedOut = stepOnGpu(gpu, buffers, layout, traced.pipeline, set, world, sorted, starts, index);
        std::vector<uint32_t> gpuTrace(TRACE_WORDS);
        std::memcpy(gpuTrace.data(), buffers.trace.mapped, TRACE_WORDS * sizeof(uint32_t));
        std::vector<uint32_t> cpuTrace = traceOnCpu(world, sorted, starts, index);
        std::printf("        частица № %u (место %u): %s\n", sorted.id[index], index, particleValues(cpuOut, index, gpuOut, index).c_str());
        if (!traceMatchesModel(cpuTrace, cpuOut, index))
        {
            std::printf("        ВНУТРЕННЯЯ ОШИБКА: трасса ЦПУ не воспроизводит модель\n");
        }
        if (gpuTrace[TRACE_INDEX] != index)
        {
            std::printf("        трасса ГПУ не записана\n");
            continue;
        }
        std::printf("        причина: %s\n", findCause(world, sorted, index, cpuTrace, gpuTrace).c_str());
        if (!tracedOut.bitwiseEqual(gpuOut))
        {
            std::printf("        конвейер с трассой дал другой шаг, чем без неё: трасса может не отражать причину\n");
        }
    }
    traced.destroy(gpu.dev);
}

static TrajectoryOutcome runTrajectory(GpuContext& gpu, ParticleBuffers& buffers, const ComputeLayout& layout, VkDescriptorSet set,
    const TrajectoryVariant& variant, const Reference& reference)
{
    std::printf("    %u шагов, %s, сортировка на ЦПУ:\n", TRAJECTORY_STEPS, variant.name.c_str());
    TrajectoryOutcome outcome;
    CheckpointLog log{reference.start.size(), {}};
    unsigned threads = threadsFor(reference.start.size());
    Columns state = reference.start;
    Columns sorted;
    std::vector<uint32_t> starts;
    uint32_t next = 0;
    for (uint32_t step = 1; step <= TRAJECTORY_STEPS; ++step)
    {
        distributeAndSort(reference.world, state, sorted, starts, threads);
        Columns out = stepOnGpu(gpu, buffers, layout, variant.pipeline, set, reference.world, sorted, starts, NO_TRACE);
        if (outcome.firstDivergence == 0 && hashInOrder(out) != reference.hashes[step])
        {
            outcome.firstDivergence = step;
            log.flush();
            explainDivergence(gpu, buffers, layout, set, variant, reference.world, sorted, starts, out, step);
        }
        state = std::move(out);
        if (next < CHECKPOINT_COUNT && CHECKPOINTS[next] == step)
        {
            outcome.finalDiffering = log.check(step, reference.checkpoints[next], state, reference.checkpointHashes[next]);
            ++next;
        }
    }
    log.flush();
    return outcome;
}

struct ResidentKernels
{
    ComputeLayout keysLayout;
    ComputeLayout reduceLayout;
    ComputeLayout scanLayout;
    ComputeLayout scatterLayout;
    ComputeLayout rankLayout;
    ComputeLayout gatherLayout;
    ComputePipeline reduce;
    ComputePipeline scan;
    ComputePipeline scatter;
    ComputePipeline rank;
    ComputePipeline gather;

    void create(VkDevice dev, const SortShaders& shaders)
    {
        keysLayout.create(dev, 5, sizeof(PushWorld));
        reduceLayout.create(dev, 2, sizeof(uint32_t));
        scanLayout.create(dev, 3, sizeof(uint32_t));
        scatterLayout.create(dev, 4, sizeof(uint32_t));
        rankLayout.create(dev, 3, sizeof(uint32_t));
        gatherLayout.create(dev, 2 * OUTPUT_COLUMNS + 1, sizeof(uint32_t));
        reduce.create(dev, reduceLayout, shaders.reduce);
        scan.create(dev, scanLayout, shaders.scan);
        scatter.create(dev, scatterLayout, shaders.scatter);
        rank.create(dev, rankLayout, shaders.rank);
        gather.create(dev, gatherLayout, shaders.gather);
    }

    void destroy(VkDevice dev)
    {
        for (ComputePipeline* pipeline : {&reduce, &scan, &scatter, &rank, &gather})
        {
            pipeline->destroy(dev);
        }
        for (ComputeLayout* layout : {&keysLayout, &reduceLayout, &scanLayout, &scatterLayout, &rankLayout, &gatherLayout})
        {
            layout->destroy(dev);
        }
    }
};

struct ResidentState
{
    VkDevice dev = VK_NULL_HANDLE;
    uint32_t particles = 0;
    uint32_t startCount = 0;
    std::vector<VkBuf> state;
    std::vector<VkBuf> sorted;
    std::vector<VkBuf> staging;
    std::vector<VkBuf> stateOut;
    std::vector<VkBuf> sortedOut;
    VkBuf starts;
    VkBuf counts;
    VkBuf keys;
    VkBuf unordered;
    VkBuf trace;
    VkBuf zero;
    VkBuf startsOut;
    VkBuf countsOut;
    std::vector<uint32_t> levelCounts;
    std::vector<VkBuf> scanned;
    std::vector<VkBuf> totals;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet keysSet = VK_NULL_HANDLE;
    VkDescriptorSet scatterSet = VK_NULL_HANDLE;
    VkDescriptorSet rankSet = VK_NULL_HANDLE;
    VkDescriptorSet gatherSet = VK_NULL_HANDLE;
    VkDescriptorSet updateSet = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> reduceSets;
    std::vector<VkDescriptorSet> scanSets;

    const VkBuf* scannedOf(size_t level) const
    {
        return level == 0 ? &starts : &scanned[level];
    }

    const VkBuf* sourceOf(size_t level) const
    {
        return level == 0 ? &counts : &totals[level - 1];
    }

    void create(GpuContext& gpu, const ResidentKernels& kernels, const ComputeLayout& updateLayout, size_t particleCount, size_t cells,
        bool readable)
    {
        dev = gpu.dev;
        particles = static_cast<uint32_t>(particleCount);
        startCount = static_cast<uint32_t>(cells + 1);
        VkDeviceSize columnBytes = static_cast<VkDeviceSize>(particles) * sizeof(uint32_t);
        VkDeviceSize startBytes = static_cast<VkDeviceSize>(startCount) * sizeof(uint32_t);
        auto makeColumns = [&](std::vector<VkBuf>& columns, const MemoryChoice& choice)
        {
            columns.resize(OUTPUT_COLUMNS);
            for (VkBuf& column : columns)
            {
                column.create(gpu.physDev, gpu.dev, columnBytes, COLUMN_USAGE, choice);
            }
        };
        makeColumns(state, DEVICE_MEMORY);
        makeColumns(sorted, DEVICE_MEMORY);
        makeColumns(staging, UPLOAD_MEMORY);
        starts.create(gpu.physDev, gpu.dev, startBytes, COLUMN_USAGE, DEVICE_MEMORY);
        counts.create(gpu.physDev, gpu.dev, startBytes, COLUMN_USAGE, DEVICE_MEMORY);
        keys.create(gpu.physDev, gpu.dev, columnBytes, COLUMN_USAGE, DEVICE_MEMORY);
        unordered.create(gpu.physDev, gpu.dev, columnBytes, COLUMN_USAGE, DEVICE_MEMORY);
        trace.create(gpu.physDev, gpu.dev, TRACE_WORDS * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, DEVICE_MEMORY);
        zero.create(gpu.physDev, gpu.dev, sizeof(uint32_t), COLUMN_USAGE, DEVICE_MEMORY);
        if (readable)
        {
            makeColumns(stateOut, READBACK_MEMORY);
            makeColumns(sortedOut, READBACK_MEMORY);
            startsOut.create(gpu.physDev, gpu.dev, startBytes, COLUMN_USAGE, READBACK_MEMORY);
            countsOut.create(gpu.physDev, gpu.dev, startBytes, COLUMN_USAGE, READBACK_MEMORY);
        }

        levelCounts.push_back(startCount);
        while (levelCounts.back() > SCAN_BLOCK)
        {
            levelCounts.push_back(blocksFor(levelCounts.back()));
        }
        size_t levels = levelCounts.size();
        scanned.resize(levels);
        totals.resize(levels - 1);
        for (size_t level = 0; level + 1 < levels; ++level)
        {
            totals[level].create(gpu.physDev, gpu.dev, static_cast<VkDeviceSize>(levelCounts[level + 1]) * sizeof(uint32_t),
                COLUMN_USAGE, DEVICE_MEMORY);
            scanned[level + 1].create(gpu.physDev, gpu.dev, static_cast<VkDeviceSize>(levelCounts[level + 1]) * sizeof(uint32_t),
                COLUMN_USAGE, DEVICE_MEMORY);
        }

        uint32_t sets = 5 + static_cast<uint32_t>(2 * levels - 1);
        uint32_t descriptors = kernels.keysLayout.bindingCount + kernels.scatterLayout.bindingCount + kernels.rankLayout.bindingCount
            + kernels.gatherLayout.bindingCount + updateLayout.bindingCount + static_cast<uint32_t>(levels) * kernels.scanLayout.bindingCount
            + static_cast<uint32_t>(levels - 1) * kernels.reduceLayout.bindingCount;
        pool = makeDescriptorPool(dev, sets, descriptors);
        keysSet = kernels.keysLayout.bind(dev, pool, {&state[0], &state[1], &state[2], &keys, &counts});
        for (size_t level = 0; level + 1 < levels; ++level)
        {
            reduceSets.push_back(kernels.reduceLayout.bind(dev, pool, {sourceOf(level), &totals[level]}));
        }
        for (size_t level = 0; level < levels; ++level)
        {
            const VkBuf* offsets = level + 1 == levels ? &zero : scannedOf(level + 1);
            scanSets.push_back(kernels.scanLayout.bind(dev, pool, {sourceOf(level), scannedOf(level), offsets}));
        }
        scatterSet = kernels.scatterLayout.bind(dev, pool, {&keys, &starts, &counts, &unordered});
        rankSet = kernels.rankLayout.bind(dev, pool, {&keys, &starts, &unordered});
        std::vector<const VkBuf*> gatherBuffers;
        std::vector<const VkBuf*> updateBuffers;
        for (const VkBuf& column : state)
        {
            gatherBuffers.push_back(&column);
        }
        gatherBuffers.push_back(&keys);
        for (const VkBuf& column : sorted)
        {
            gatherBuffers.push_back(&column);
            updateBuffers.push_back(&column);
        }
        updateBuffers.push_back(&starts);
        for (const VkBuf& column : state)
        {
            updateBuffers.push_back(&column);
        }
        updateBuffers.push_back(&trace);
        gatherSet = kernels.gatherLayout.bind(dev, pool, gatherBuffers);
        updateSet = updateLayout.bind(dev, pool, updateBuffers);
    }

    void load(GpuContext& gpu, const Columns& start)
    {
        size_t bytes = start.size() * sizeof(float);
        const void* sources[OUTPUT_COLUMNS] = {start.px.data(), start.py.data(), start.pz.data(), start.vx.data(), start.vy.data(),
            start.vz.data(), start.id.data()};
        for (uint32_t column = 0; column < OUTPUT_COLUMNS; ++column)
        {
            std::memcpy(staging[column].mapped, sources[column], bytes);
        }
        gpu.begin();
        VkBufferCopy region{0, 0, bytes};
        for (uint32_t column = 0; column < OUTPUT_COLUMNS; ++column)
        {
            vkCmdCopyBuffer(gpu.cmdBuf, staging[column].buf, state[column].buf, 1, &region);
        }
        vkCmdFillBuffer(gpu.cmdBuf, counts.buf, 0, VK_WHOLE_SIZE, 0u);
        vkCmdFillBuffer(gpu.cmdBuf, zero.buf, 0, VK_WHOLE_SIZE, 0u);
        transferToCompute(gpu.cmdBuf);
        gpu.finish();
    }

    void recordReadback(VkCommandBuffer cmd) const
    {
        computeToTransfer(cmd);
        VkBufferCopy columnRegion{0, 0, static_cast<VkDeviceSize>(particles) * sizeof(uint32_t)};
        VkBufferCopy startRegion{0, 0, static_cast<VkDeviceSize>(startCount) * sizeof(uint32_t)};
        for (uint32_t column = 0; column < OUTPUT_COLUMNS; ++column)
        {
            vkCmdCopyBuffer(cmd, state[column].buf, stateOut[column].buf, 1, &columnRegion);
            vkCmdCopyBuffer(cmd, sorted[column].buf, sortedOut[column].buf, 1, &columnRegion);
        }
        vkCmdCopyBuffer(cmd, starts.buf, startsOut.buf, 1, &startRegion);
        vkCmdCopyBuffer(cmd, counts.buf, countsOut.buf, 1, &startRegion);
        transferToHost(cmd);
    }

    void download(Columns& stateColumns, Columns& sortedColumns, std::vector<uint32_t>& startValues, std::vector<uint32_t>& countValues) const
    {
        auto unpackColumns = [&](const std::vector<VkBuf>& from, Columns& to)
        {
            to.resize(particles);
            void* targets[OUTPUT_COLUMNS] = {to.px.data(), to.py.data(), to.pz.data(), to.vx.data(), to.vy.data(), to.vz.data(),
                to.id.data()};
            for (uint32_t column = 0; column < OUTPUT_COLUMNS; ++column)
            {
                from[column].invalidate(dev);
                std::memcpy(targets[column], from[column].mapped, static_cast<size_t>(particles) * sizeof(uint32_t));
            }
        };
        auto unpackWords = [&](const VkBuf& from, std::vector<uint32_t>& to)
        {
            to.resize(startCount);
            from.invalidate(dev);
            std::memcpy(to.data(), from.mapped, static_cast<size_t>(startCount) * sizeof(uint32_t));
        };
        unpackColumns(stateOut, stateColumns);
        unpackColumns(sortedOut, sortedColumns);
        unpackWords(startsOut, startValues);
        unpackWords(countsOut, countValues);
    }

    void destroy()
    {
        vkDestroyDescriptorPool(dev, pool, nullptr);
        for (std::vector<VkBuf>* group : {&state, &sorted, &staging, &stateOut, &sortedOut, &scanned, &totals})
        {
            for (VkBuf& buffer : *group)
            {
                buffer.destroy(dev);
            }
        }
        for (VkBuf* buffer : {&starts, &counts, &keys, &unordered, &trace, &zero, &startsOut, &countsOut})
        {
            buffer->destroy(dev);
        }
    }
};

static void recordResidentStep(VkCommandBuffer cmd, const ResidentKernels& kernels, const ComputeLayout& updateLayout, VkPipeline keys,
    VkPipeline update, const ResidentState& resident, const PushWorld& push, VkQueryPool queries, uint32_t firstQuery)
{
    auto stamp = [&](uint32_t stage)
    {
        if (queries != VK_NULL_HANDLE)
        {
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, firstQuery + stage);
        }
    };
    uint32_t particles = resident.particles;
    uint32_t particleGroups = groupsFor(particles);
    stamp(0);
    dispatchKernel(cmd, kernels.keysLayout, keys, resident.keysSet, &push, sizeof(push), particleGroups);
    computeToCompute(cmd);
    stamp(1);
    size_t levels = resident.levelCounts.size();
    for (size_t level = 0; level + 1 < levels; ++level)
    {
        uint32_t count = resident.levelCounts[level];
        dispatchKernel(cmd, kernels.reduceLayout, kernels.reduce.pipeline, resident.reduceSets[level], &count, sizeof(count),
            blocksFor(count));
        computeToCompute(cmd);
    }
    for (size_t level = levels; level-- > 0;)
    {
        uint32_t count = resident.levelCounts[level];
        dispatchKernel(cmd, kernels.scanLayout, kernels.scan.pipeline, resident.scanSets[level], &count, sizeof(count), blocksFor(count));
        computeToCompute(cmd);
    }
    stamp(2);
    dispatchKernel(cmd, kernels.scatterLayout, kernels.scatter.pipeline, resident.scatterSet, &particles, sizeof(particles), particleGroups);
    computeToCompute(cmd);
    stamp(3);
    dispatchKernel(cmd, kernels.rankLayout, kernels.rank.pipeline, resident.rankSet, &particles, sizeof(particles), particleGroups);
    computeToCompute(cmd);
    stamp(4);
    dispatchKernel(cmd, kernels.gatherLayout, kernels.gather.pipeline, resident.gatherSet, &particles, sizeof(particles), particleGroups);
    computeToCompute(cmd);
    stamp(5);
    dispatchKernel(cmd, updateLayout, update, resident.updateSet, &push, sizeof(push), particleGroups);
    computeToCompute(cmd);
    stamp(6);
}

static bool nearCellBoundary(float ratio)
{
    float nearest = std::round(ratio);
    return nearest != 0.0f && ulpDistance(toBits(ratio), toBits(nearest)) <= 2;
}

static std::string describeSortDifference(const World& world, const Columns& want, const std::vector<uint32_t>& wantStarts,
    const Columns& got, const std::vector<uint32_t>& gotStarts)
{
    size_t moved = 0;
    size_t firstPlace = want.size();
    for (size_t place = 0; place < want.size(); ++place)
    {
        if (want.id[place] != got.id[place])
        {
            ++moved;
            firstPlace = std::min(firstPlace, place);
        }
    }
    size_t startsDiffer = 0;
    for (size_t cell = 0; cell < wantStarts.size() && cell < gotStarts.size(); ++cell)
    {
        startsDiffer += wantStarts[cell] != gotStarts[cell] ? 1 : 0;
    }
    std::string text = formatted("не на своих местах %zu частиц, начала различаются у %zu ячеек", moved, startsDiffer);
    if (firstPlace == want.size())
    {
        return text;
    }
    std::vector<uint32_t> gotPlaces = placeById(got);
    auto cellAt = [&](uint32_t place)
    {
        return static_cast<uint32_t>(std::upper_bound(gotStarts.begin(), gotStarts.end(), place) - gotStarts.begin()) - 1;
    };
    size_t shownPlace = firstPlace;
    for (size_t place = firstPlace; place < want.size(); ++place)
    {
        uint32_t placeGot = gotPlaces[want.id[place]];
        if (want.id[place] != got.id[place] && placeGot != UINT32_MAX && cellAt(placeGot) != cellKey(place, want, world))
        {
            shownPlace = place;
            break;
        }
    }
    uint32_t ident = want.id[shownPlace];
    uint32_t gotPlace = gotPlaces[ident];
    uint32_t wantCell = cellKey(shownPlace, want, world);
    if (gotPlace == UINT32_MAX)
    {
        return text + formatted("; частицы № %u в сортировке ГПУ нет", ident);
    }
    uint32_t gotCell = cellAt(gotPlace);
    text += formatted("; %s — № %u: у ЦПУ место %zu в ячейке %u, у ГПУ место %u в ячейке %u",
        shownPlace == firstPlace ? "первая" : "причина", ident, shownPlace, wantCell, gotPlace, gotCell);
    if (wantCell == gotCell)
    {
        return text + " (ячейка та же, порядок внутри ячейки иной)";
    }
    static const char* const AXES[3] = {"x", "y", "z"};
    float coordinates[3] = {want.px[shownPlace], want.py[shownPlace], want.pz[shownPlace]};
    uint32_t side = world.cellsPerSide;
    uint32_t wantAxis[3] = {wantCell % side, wantCell / side % side, wantCell / (side * side)};
    uint32_t gotAxis[3] = {gotCell % side, gotCell / side % side, gotCell / (side * side)};
    for (int axis = 0; axis < 3; ++axis)
    {
        if (wantAxis[axis] != gotAxis[axis])
        {
            float ratio = coordinates[axis] / world.cellSize;
            text += formatted("; ось %s: координата %s, частное на размер ячейки %s точно, ячейка ЦПУ %u, ГПУ %u%s", AXES[axis],
                showBits(toBits(coordinates[axis])).c_str(), showBits(toBits(ratio)).c_str(), wantAxis[axis], gotAxis[axis],
                nearCellBoundary(ratio) ? " — частное у границы ячеек, ГПУ округлил деление иначе" : "");
        }
    }
    return text;
}

struct ResidentVariant
{
    std::string name;
    const std::vector<uint32_t>* updateSpv;
    VkPipeline update;
    VkPipeline keys;
};

static TrajectoryOutcome runResidentTrajectory(GpuContext& gpu, const ResidentKernels& kernels, ResidentState& resident,
    const ComputeLayout& updateLayout, const ResidentVariant& variant, ParticleBuffers& buffers, VkDescriptorSet hostSet,
    const Reference& reference)
{
    std::printf("    %u шагов, %s, сортировка и шаг на ГПУ, между шагами данные не покидают ГПУ (копия выгружается только для сверки):\n",
        TRAJECTORY_STEPS, variant.name.c_str());
    const World& world = reference.world;
    resident.load(gpu, reference.start);
    PushWorld push = pushFor(world, reference.start.size());
    TrajectoryOutcome outcome;
    CheckpointLog log{reference.start.size(), {}};
    unsigned threads = threadsFor(reference.start.size());
    Columns previous = reference.start;
    Columns state;
    Columns sorted;
    Columns cpuSorted;
    std::vector<uint32_t> starts;
    std::vector<uint32_t> counts;
    std::vector<uint32_t> cpuStarts;
    uint32_t sortMatches = 0;
    bool countsCleared = true;
    std::string firstSortDifference;
    uint32_t next = 0;
    for (uint32_t step = 1; step <= TRAJECTORY_STEPS; ++step)
    {
        gpu.begin();
        recordResidentStep(gpu.cmdBuf, kernels, updateLayout, variant.keys, variant.update, resident, push, VK_NULL_HANDLE, 0);
        resident.recordReadback(gpu.cmdBuf);
        gpu.finish();
        resident.download(state, sorted, starts, counts);
        countsCleared = countsCleared && std::all_of(counts.begin(), counts.end(), [](uint32_t value) { return value == 0; });
        distributeAndSort(world, previous, cpuSorted, cpuStarts, threads);
        bool sortSame = sorted.bitwiseEqual(cpuSorted) && starts == cpuStarts;
        if (sortSame)
        {
            ++sortMatches;
        }
        else if (firstSortDifference.empty())
        {
            firstSortDifference = formatted("шаг %u: %s", step, describeSortDifference(world, cpuSorted, cpuStarts, sorted, starts).c_str());
        }
        if (outcome.firstDivergence == 0 && hashInOrder(state) != reference.hashes[step])
        {
            outcome.firstDivergence = step;
            log.flush();
            if (sortSame)
            {
                TrajectoryVariant traced{variant.name, variant.updateSpv, variant.update};
                explainDivergence(gpu, buffers, updateLayout, hostSet, traced, world, sorted, starts, state, step);
            }
            else
            {
                std::printf("      первое расхождение — шаг %u: сортировка ГПУ отличается от устойчивой сортировки ЦПУ того же состояния — %s\n",
                    step, describeSortDifference(world, cpuSorted, cpuStarts, sorted, starts).c_str());
            }
        }
        previous = state;
        if (next < CHECKPOINT_COUNT && CHECKPOINTS[next] == step)
        {
            outcome.finalDiffering = log.check(step, reference.checkpoints[next], state, reference.checkpointHashes[next]);
            ++next;
        }
    }
    log.flush();
    std::printf("      сортировка ГПУ совпала с устойчивой сортировкой ЦПУ того же состояния (порядок частиц и начала ячеек) на %u из %u шагов%s; "
        "счётчики ячеек после каждого шага вернулись к нулю: %s\n", sortMatches, TRAJECTORY_STEPS,
        firstSortDifference.empty() ? "" : ("; первое отличие — " + firstSortDifference).c_str(), countsCleared ? "да" : "НЕТ");
    return outcome;
}

struct ResidentTiming
{
    double stageMs[RESIDENT_STAGES] = {};
    double stampedMs = -1.0;
    double batchMs = 0.0;
};

static ResidentTiming measureResident(GpuContext& gpu, VkQueryPool queries, const ResidentKernels& kernels,
    const ComputeLayout& updateLayout, VkPipeline keys, VkPipeline update, ResidentState& resident, const World& world, const Columns& start)
{
    resident.load(gpu, start);
    PushWorld push = pushFor(world, start.size());
    auto recordBatch = [&](uint32_t steps, VkQueryPool stampPool)
    {
        gpu.begin();
        if (stampPool != VK_NULL_HANDLE)
        {
            vkCmdResetQueryPool(gpu.cmdBuf, stampPool, 0, RESIDENT_STAMPS * steps);
        }
        for (uint32_t step = 0; step < steps; ++step)
        {
            recordResidentStep(gpu.cmdBuf, kernels, updateLayout, keys, update, resident, push, stampPool, RESIDENT_STAMPS * step);
        }
        gpu.end();
    };
    recordBatch(RESIDENT_WARMUPS, VK_NULL_HANDLE);
    gpu.submitAndWait();

    ResidentTiming timing;
    std::vector<double> batches;
    for (uint32_t batch = 0; batch < RESIDENT_BATCHES; ++batch)
    {
        recordBatch(BENCH_STEPS, VK_NULL_HANDLE);
        Clock::time_point began = Clock::now();
        gpu.submitAndWait();
        batches.push_back(msSince(began) / BENCH_STEPS);
    }
    std::sort(batches.begin(), batches.end());
    timing.batchMs = batches[batches.size() / 2];

    if (queries != VK_NULL_HANDLE)
    {
        recordBatch(BENCH_STEPS, queries);
        gpu.submitAndWait();
        std::vector<uint64_t> ticks(RESIDENT_STAMPS * BENCH_STEPS);
        checkVk(vkGetQueryPoolResults(gpu.dev, queries, 0, RESIDENT_STAMPS * BENCH_STEPS, ticks.size() * sizeof(uint64_t), ticks.data(),
            sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT), "vkGetQueryPoolResults");
        std::vector<std::pair<double, uint32_t>> totals;
        for (uint32_t step = 0; step < BENCH_STEPS; ++step)
        {
            uint32_t first = RESIDENT_STAMPS * step;
            totals.push_back({gpu.ticksToMs(ticks[first], ticks[first + RESIDENT_STAMPS - 1]), step});
        }
        std::nth_element(totals.begin(), totals.begin() + BENCH_STEPS / 2, totals.end());
        uint32_t first = RESIDENT_STAMPS * totals[BENCH_STEPS / 2].second;
        for (uint32_t stage = 0; stage < RESIDENT_STAGES; ++stage)
        {
            timing.stageMs[stage] = gpu.ticksToMs(ticks[first + stage], ticks[first + stage + 1]);
        }
        timing.stampedMs = totals[BENCH_STEPS / 2].first;
    }
    return timing;
}

static void printResident(const std::string& label, const ResidentTiming& timing)
{
    std::string text;
    if (timing.stampedMs >= 0.0)
    {
        for (uint32_t stage = 0; stage < RESIDENT_STAGES; ++stage)
        {
            text += formatted("%s%s %.3f", stage == 0 ? "" : " + ", RESIDENT_STAGE_NAMES[stage], timing.stageMs[stage]);
        }
        text += formatted(" = %.3f по меткам времени (шаг с медианной суммой из %u); ", timing.stampedMs, BENCH_STEPS);
    }
    text += formatted("медиана %u партий по %u шагов без меток по часам ЦПУ — %.3f на шаг", RESIDENT_BATCHES, BENCH_STEPS, timing.batchMs);
    std::printf("        %s: %s\n", label.c_str(), text.c_str());
}

struct Variant
{
    const ShaderSet* shaders;
    ComputePipeline hardware;
    ComputePipeline soft;
    ComputePipeline approx;
    ComputePipeline keysHardware;
    ComputePipeline keysSoft;
};

struct Parts
{
    bool probes = true;
    bool trajectories = true;
    bool timing = true;
    uint32_t benchSide = 100;
};

struct CpuTiming
{
    unsigned threads = 1;
    double sortMs = 0.0;
    double scalarOneMs = 0.0;
    double scalarAllMs = 0.0;
    double wideOneMs = 0.0;
    double wideAllMs = 0.0;
    bool wideMatches = false;
    bool wide = false;
    CoreCensus scalarCores;
    CoreCensus wideCores;
};

static double medianOf(std::vector<double> values)
{
    std::nth_element(values.begin(), values.begin() + values.size() / 2, values.end());
    return values[values.size() / 2];
}

template <class First, class Second>
static std::pair<double, double> alternatingMedianMs(int rounds, First first, Second second)
{
    std::vector<double> firstTimes;
    std::vector<double> secondTimes;
    for (int round = 0; round < rounds; ++round)
    {
        for (int turn = 0; turn < 2; ++turn)
        {
            bool firstNow = (turn == 0) == (round % 2 == 0);
            Clock::time_point start = Clock::now();
            if (firstNow)
            {
                first();
            }
            else
            {
                second();
            }
            (firstNow ? firstTimes : secondTimes).push_back(msSince(start));
        }
    }
    return {medianOf(firstTimes), medianOf(secondTimes)};
}

static CpuTiming measureCpu(const BenchCase& bench)
{
    CpuTiming timing;
    timing.threads = cpuThreads();
    timing.wide = cpuHasAvx2();
    Columns sorted;
    std::vector<uint32_t> starts;
    timing.sortMs = medianMs(3, [&] { distributeAndSort(bench.world, bench.particles, sorted, starts, timing.threads); });
    Columns scalar;
    Columns wide;
    auto scalarOne = [&]
    {
        timing.scalarCores.sample();
        stepOnCpu(bench.world, bench.sorted, bench.starts, scalar, 1);
        timing.scalarCores.sample();
    };
    auto scalarAll = [&] { stepOnCpu(bench.world, bench.sorted, bench.starts, scalar, timing.threads); };
    if (!timing.wide)
    {
        timing.scalarOneMs = medianMs(3, scalarOne);
        timing.scalarAllMs = medianMs(5, scalarAll);
        return timing;
    }
    auto wideOne = [&]
    {
        timing.wideCores.sample();
        stepAvx2(bench.world, bench.sorted, bench.starts, wide, 1);
        timing.wideCores.sample();
    };
    auto wideAll = [&] { stepAvx2(bench.world, bench.sorted, bench.starts, wide, timing.threads); };
    std::tie(timing.scalarOneMs, timing.wideOneMs) = alternatingMedianMs(5, scalarOne, wideOne);
    std::tie(timing.scalarAllMs, timing.wideAllMs) = alternatingMedianMs(7, scalarAll, wideAll);
    timing.wideMatches = wide.bitwiseEqual(scalar);
    return timing;
}

static double bestCpuMs(const CpuTiming& timing)
{
    return timing.wide ? std::min(timing.wideAllMs, timing.scalarAllMs) : timing.scalarAllMs;
}

static const char* bestCpuName(const CpuTiming& timing)
{
    return timing.wide && timing.wideAllMs <= timing.scalarAllMs ? "AVX2×8" : "скаляр";
}

static std::string rateText(double gigabytes)
{
    return gigabytes >= 1.0 ? formatted("%.1f", gigabytes) : formatted("%.3f", gigabytes);
}

static std::string ratioText(double gpuMs, double cpuMs)
{
    return gpuMs < cpuMs ? formatted("быстрее в %.2f раза", cpuMs / gpuMs) : formatted("медленнее в %.2f раза", gpuMs / cpuMs);
}

static void printMemoryTypes(VkPhysicalDevice physDev)
{
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(physDev, &props);
    std::printf("    Типы памяти:\n");
    for (uint32_t idx = 0; idx < props.memoryTypeCount; ++idx)
    {
        const VkMemoryType& type = props.memoryTypes[idx];
        const VkMemoryHeap& heap = props.memoryHeaps[type.heapIndex];
        std::printf("      %u: куча %u (%.0f МБ%s) — %s\n", idx, type.heapIndex, static_cast<double>(heap.size) / 1048576.0,
            (heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? ", на устройстве" : "", memoryFlagsName(type.propertyFlags).c_str());
    }
}

struct MappedSpeed
{
    double writeGbps = 0.0;
    double readGbps = 0.0;
    double streamGbps = 0.0;
};

static bool measureMappedType(GpuContext& gpu, uint32_t typeIndex, MappedSpeed& speed)
{
    VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, MAPPED_PROBE_BYTES, typeIndex};
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (vkAllocateMemory(gpu.dev, &info, nullptr, &memory) != VK_SUCCESS)
    {
        return false;
    }
    void* mapped = nullptr;
    if (vkMapMemory(gpu.dev, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
    {
        vkFreeMemory(gpu.dev, memory, nullptr);
        return false;
    }
    bool coherent = (memoryFlags(gpu.physDev, typeIndex) & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, memory, 0, VK_WHOLE_SIZE};
    auto invalidate = [&]
    {
        if (!coherent)
        {
            vkInvalidateMappedMemoryRanges(gpu.dev, 1, &range);
        }
    };
    std::vector<uint8_t> source(MAPPED_PROBE_BYTES, 0x5A);
    std::vector<uint8_t> target(MAPPED_PROBE_BYTES, 0);
    double bytes = static_cast<double>(MAPPED_PROBE_BYTES);
    speed.writeGbps = gigabytesPerSecond(bytes, medianMs(3, [&] { std::memcpy(mapped, source.data(), MAPPED_PROBE_BYTES); }));
    if (!coherent)
    {
        vkFlushMappedMemoryRanges(gpu.dev, 1, &range);
    }
    speed.readGbps = gigabytesPerSecond(bytes, medianMs(3, [&]
    {
        invalidate();
        std::memcpy(target.data(), mapped, MAPPED_PROBE_BYTES);
    }));
    if (cpuHasSse41())
    {
        speed.streamGbps = gigabytesPerSecond(bytes, medianMs(3, [&]
        {
            invalidate();
            streamingCopy(target.data(), mapped, MAPPED_PROBE_BYTES);
        }));
    }
    vkUnmapMemory(gpu.dev, memory);
    vkFreeMemory(gpu.dev, memory, nullptr);
    return true;
}

static void printMappedSpeeds(GpuContext& gpu)
{
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(gpu.physDev, &props);
    std::printf("    ЦПУ через отображённую память, %zu МБ, ГБ/с: запись memcpy | чтение memcpy | чтение потоковыми загрузками MOVNTDQA:\n",
        MAPPED_PROBE_BYTES >> 20);
    for (uint32_t idx = 0; idx < props.memoryTypeCount; ++idx)
    {
        VkMemoryPropertyFlags flags = props.memoryTypes[idx].propertyFlags;
        if (!(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) || (flags & UNUSABLE_MEMORY))
        {
            continue;
        }
        MappedSpeed speed;
        if (!measureMappedType(gpu, idx, speed))
        {
            std::printf("      тип %u (%s): не выделилась\n", idx, memoryFlagsName(flags).c_str());
            continue;
        }
        std::printf("      тип %u (%s): %s | %s | %s\n", idx, memoryFlagsName(flags).c_str(), rateText(speed.writeGbps).c_str(),
            rateText(speed.readGbps).c_str(), speed.streamGbps > 0.0 ? rateText(speed.streamGbps).c_str() : "нет SSE4.1");
    }
}

struct PathPhases
{
    double prepareMs = 0.0;
    double launchMs = 0.0;
    double copyInMs = 0.0;
    double kernelMs = 0.0;
    double copyOutMs = 0.0;
    double receiveMs = 0.0;
    double wholeMs = 0.0;
};

struct PathRun
{
    PathPhases median;
    double fastestMs = 0.0;
    double slowestMs = 0.0;
};

struct PathSetup
{
    std::string name;
    VkDescriptorSet set;
    bool copies;
    const std::vector<VkBuf>* target;
};

static PathPhases runPathOnce(GpuContext& gpu, VkQueryPool queries, ParticleBuffers& buffers, const ComputeLayout& layout,
    VkPipeline pipeline, const PathSetup& setup, const BenchCase& bench, Columns& out)
{
    size_t count = bench.sorted.size();
    PushWorld push = pushFor(bench.world, count);
    auto stamp = [&](uint32_t query)
    {
        if (queries != VK_NULL_HANDLE)
        {
            vkCmdWriteTimestamp(gpu.cmdBuf, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, query);
        }
    };

    Clock::time_point began = Clock::now();
    buffers.pack(bench.sorted, bench.starts);
    Clock::time_point prepared = Clock::now();
    gpu.begin();
    if (queries != VK_NULL_HANDLE)
    {
        vkCmdResetQueryPool(gpu.cmdBuf, queries, 0, PATH_STAMPS);
    }
    stamp(0);
    if (setup.copies)
    {
        buffers.recordCopyIn(gpu.cmdBuf, count, bench.starts.size());
        transferToCompute(gpu.cmdBuf);
    }
    stamp(1);
    recordStep(gpu.cmdBuf, layout, pipeline, setup.set, push, groupsFor(count));
    if (setup.copies)
    {
        computeToTransfer(gpu.cmdBuf);
    }
    else
    {
        computeToHost(gpu.cmdBuf);
    }
    stamp(2);
    if (setup.copies)
    {
        buffers.recordCopyOut(gpu.cmdBuf, count, *setup.target);
        transferToHost(gpu.cmdBuf);
    }
    stamp(3);
    gpu.finish();
    Clock::time_point finished = Clock::now();
    buffers.unpack(out, *setup.target);
    Clock::time_point received = Clock::now();

    PathPhases phases;
    phases.prepareMs = msBetween(began, prepared);
    phases.receiveMs = msBetween(finished, received);
    phases.wholeMs = msBetween(began, received);
    phases.launchMs = msBetween(prepared, finished);
    if (queries != VK_NULL_HANDLE)
    {
        uint64_t ticks[PATH_STAMPS] = {};
        checkVk(vkGetQueryPoolResults(gpu.dev, queries, 0, PATH_STAMPS, sizeof(ticks), ticks, sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT), "vkGetQueryPoolResults");
        phases.copyInMs = gpu.ticksToMs(ticks[0], ticks[1]);
        phases.kernelMs = gpu.ticksToMs(ticks[1], ticks[2]);
        phases.copyOutMs = gpu.ticksToMs(ticks[2], ticks[3]);
        phases.launchMs -= phases.copyInMs + phases.kernelMs + phases.copyOutMs;
    }
    return phases;
}

static PathRun measurePath(GpuContext& gpu, VkQueryPool queries, ParticleBuffers& buffers, const ComputeLayout& layout,
    VkPipeline pipeline, const PathSetup& setup, const BenchCase& bench, const Columns& expected, bool& matches)
{
    Columns out;
    out.resize(bench.sorted.size());
    for (uint32_t run = 0; run < PATH_WARMUPS; ++run)
    {
        runPathOnce(gpu, queries, buffers, layout, pipeline, setup, bench, out);
    }
    matches = out.bitwiseEqual(expected);
    std::vector<PathPhases> runs;
    for (uint32_t run = 0; run < PATH_RUNS; ++run)
    {
        runs.push_back(runPathOnce(gpu, queries, buffers, layout, pipeline, setup, bench, out));
    }
    std::sort(runs.begin(), runs.end(), [](const PathPhases& left, const PathPhases& right) { return left.wholeMs < right.wholeMs; });
    return PathRun{runs[PATH_RUNS / 2], runs.front().wholeMs, runs.back().wholeMs};
}

static void printPath(const PathSetup& setup, const PathRun& run, bool stamped, double inBytes, double outBytes, bool matches)
{
    const PathPhases& phases = run.median;
    std::string text = formatted("подготовка %.3f", phases.prepareMs);
    if (stamped)
    {
        text += formatted(" + запуск %.3f", phases.launchMs);
        if (setup.copies)
        {
            text += formatted(" + туда %.3f (%.1f МБ, %s ГБ/с)", phases.copyInMs, inBytes / 1.0e6,
                rateText(gigabytesPerSecond(inBytes, phases.copyInMs)).c_str());
        }
        text += formatted(" + ядро %.3f", phases.kernelMs);
        if (setup.copies)
        {
            text += formatted(" + обратно %.3f (%.1f МБ, %s ГБ/с)", phases.copyOutMs, outBytes / 1.0e6,
                rateText(gigabytesPerSecond(outBytes, phases.copyOutMs)).c_str());
        }
    }
    else
    {
        text += formatted(" + запуск, работа ГПУ и ожидание %.3f", phases.launchMs);
    }
    text += formatted(" + получение %.3f (%s ГБ/с) = %.3f целиком; разброс целого по прогонам %.3f–%.3f; результат %s", phases.receiveMs,
        rateText(gigabytesPerSecond(outBytes, phases.receiveMs)).c_str(), phases.wholeMs, run.fastestMs, run.slowestMs,
        matches ? "побитно равен ЦПУ" : "НЕ РАВЕН ЦПУ");
    std::printf("        %s: %s\n", setup.name.c_str(), text.c_str());
}

static void testPhysicalDevice(VkPhysicalDevice physDev, const ShaderSet& defaults, const ShaderSet& preserve, const SortShaders& sortShaders,
    const ScalarProbe& scalar, const ProbeInputs& pairs, const Reference& reference, const std::vector<BenchCase>& benches,
    const std::vector<CpuTiming>& cpuTimings, const Parts& parts)
{
    VkPhysicalDeviceProperties devProps{};
    vkGetPhysicalDeviceProperties(physDev, &devProps);
    std::printf("  Устройство: %s, Vulkan %u.%u\n", devProps.deviceName, VK_API_VERSION_MAJOR(devProps.apiVersion),
        VK_API_VERSION_MINOR(devProps.apiVersion));
    if (devProps.apiVersion < VK_API_VERSION_1_2)
    {
        std::printf("    Нужен Vulkan 1.2: устройство пропущено\n\n");
        return;
    }

    FloatControls controls = queryFloatControls(physDev);
    std::printf("    Float controls, 32 бита: DenormPreserve — %s, DenormFlushToZero — %s, RoundingModeRTE — %s, "
        "SignedZeroInfNanPreserve — %s; режим денормализованных чисел %s\n",
        yesNo(controls.denormPreserve), yesNo(controls.denormFlush), yesNo(controls.roundingRte),
        yesNo(controls.signedZeroInfNan), independenceName(controls.independence));

    ShaderFmaSupport fmaSupport = queryShaderFma(physDev);
    std::printf("    VK_KHR_shader_fma: %s\n", shaderFmaText(fmaSupport).c_str());

    GpuContext gpu;
    if (!gpu.create(physDev, fmaSupport.float32))
    {
        std::printf("    Нет очереди вычислений: устройство пропущено\n\n");
        return;
    }
    if (gpu.timestampBits > 0)
    {
        std::printf("    Метки времени: разрядность %u, %.3f нс на такт\n", gpu.timestampBits, gpu.timestampPeriodNs);
    }
    else
    {
        std::printf("    Метки времени: очередь вычислений их не поддерживает\n");
    }
    printMemoryTypes(physDev);

    std::vector<const ShaderSet*> modes{&defaults};
    if (controls.denormPreserve)
    {
        modes.push_back(&preserve);
    }
    if (parts.probes)
    {
        for (const ShaderSet* mode : modes)
        {
            ProbeResult result = runProbe(gpu, mode->probe, scalar, pairs.lanes);
            reportProbe(mode->modeName, scalar, pairs, result);
            bool khr = gpu.fmaKhr && !mode->probeFmaKhr.empty();
            if (gpu.fmaKhr && !khr)
            {
                std::printf("    Шейдер probe_fma_khr для режима «%s» не собран: путь OpFmaKHR не проверен\n", mode->modeName.c_str());
            }
            reportFmaProbe(mode->modeName, pairs, runFmaProbe(gpu, khr ? mode->probeFmaKhr : mode->probeFma, pairs.fmaTriples), khr);
        }
    }
    if (!controls.denormPreserve)
    {
        std::printf("    DenormPreserve не поддерживается: строгий режим на этом устройстве недоступен\n");
    }

    size_t bufferParticles = std::max(benches.front().sorted.size(), reference.start.size());
    size_t bufferStarts = std::max(benches.front().starts.size(), cellCount(reference.world) + 1);
    ParticleBuffers buffers;
    buffers.create(gpu, bufferParticles, bufferStarts);
    uint32_t legacyType = buffers.legacy.empty() ? buffers.readback.front().memoryType : buffers.legacy.front().memoryType;
    std::printf("    Выбор памяти: подготовка — тип %u, получение — тип %u (%s), данные на устройстве — тип %u; в v0.050.7 получение шло %s\n",
        buffers.staging.front().memoryType, buffers.readback.front().memoryType,
        memoryFlagsName(memoryFlags(physDev, buffers.readback.front().memoryType)).c_str(), buffers.device.front().memoryType,
        buffers.legacy.empty() ? "из того же типа" : formatted("из типа %u (%s)", legacyType, memoryFlagsName(memoryFlags(physDev, legacyType)).c_str()).c_str());

    ComputeLayout layout;
    layout.create(gpu.dev, PARTICLE_BINDINGS, sizeof(PushWorld));
    VkDescriptorPool pool = makeDescriptorPool(gpu.dev, 2, 2 * PARTICLE_BINDINGS);
    VkDescriptorSet hostSet = layout.bind(gpu.dev, pool, buffers.hostBindings());
    VkDescriptorSet deviceSet = layout.bind(gpu.dev, pool, buffers.deviceBindings());

    ResidentKernels kernels;
    kernels.create(gpu.dev, sortShaders);
    std::vector<Variant> variants;
    for (const ShaderSet* mode : modes)
    {
        Variant variant{mode, {}, {}, {}, {}, {}};
        variant.hardware.create(gpu.dev, layout, mode->update);
        variant.soft.create(gpu.dev, layout, mode->updateSoft);
        variant.approx.create(gpu.dev, layout, mode->updateApprox);
        variant.keysHardware.create(gpu.dev, kernels.keysLayout, mode->keys);
        variant.keysSoft.create(gpu.dev, kernels.keysLayout, mode->keysSoft);
        variants.push_back(variant);
    }

    if (parts.trajectories)
    {
        std::vector<TrajectoryVariant> trajectories;
        for (const Variant& variant : variants)
        {
            trajectories.push_back({variant.shaders->modeName + ", аппаратно", &variant.shaders->update, variant.hardware.pipeline});
            trajectories.push_back({variant.shaders->modeName + ", доводка", &variant.shaders->updateSoft, variant.soft.pipeline});
        }
        trajectories.push_back({variants[0].shaders->modeName + ", приближённо (контроль)", &variants[0].shaders->updateApprox,
            variants[0].approx.pipeline});
        std::printf("    ЦПУ против Vulkan: %zu частиц, %u шагов, сравнение по номерам частиц каждого поля после шагов 1, 10, 50, 100, 200, 300, 400\n",
            reference.start.size(), TRAJECTORY_STEPS);
        std::string summary;
        auto summarize = [&](const std::string& name, const TrajectoryOutcome& outcome)
        {
            summary += formatted("%s%s — %s", summary.empty() ? "" : "; ", name.c_str(), outcome.firstDivergence == 0
                ? formatted("побитно все %u шагов", TRAJECTORY_STEPS).c_str()
                : formatted("расходится с шага %u, к шагу %u различаются %zu частиц", outcome.firstDivergence, TRAJECTORY_STEPS,
                    outcome.finalDiffering).c_str());
        };
        for (const TrajectoryVariant& trajectory : trajectories)
        {
            summarize(trajectory.name, runTrajectory(gpu, buffers, layout, hostSet, trajectory, reference));
        }
        ResidentState residentCheck;
        residentCheck.create(gpu, kernels, layout, reference.start.size(), cellCount(reference.world), true);
        for (const Variant& variant : variants)
        {
            std::vector<ResidentVariant> residents{
                {variant.shaders->modeName + ", аппаратно", &variant.shaders->update, variant.hardware.pipeline, variant.keysHardware.pipeline},
                {variant.shaders->modeName + ", доводка", &variant.shaders->updateSoft, variant.soft.pipeline, variant.keysSoft.pipeline},
            };
            for (const ResidentVariant& resident : residents)
            {
                TrajectoryOutcome outcome = runResidentTrajectory(gpu, kernels, residentCheck, layout, resident, buffers, hostSet, reference);
                summarize(resident.name + ", всё на ГПУ", outcome);
            }
        }
        residentCheck.destroy();
        std::printf("    Итог: %s\n", summary.c_str());
    }

    if (parts.timing)
    {
        printMappedSpeeds(gpu);
        VkQueryPool queries = VK_NULL_HANDLE;
        if (gpu.timestampBits > 0)
        {
            VkQueryPoolCreateInfo queryInfo{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, nullptr, 0, VK_QUERY_TYPE_TIMESTAMP,
                RESIDENT_STAMPS * BENCH_STEPS, 0};
            checkVk(vkCreateQueryPool(gpu.dev, &queryInfo, nullptr, &queries), "vkCreateQueryPool");
        }
        const Variant& strict = variants.back();
        const BenchCase& largest = benches.front();
        bool residentFits = groupsFor(largest.sorted.size()) <= gpu.maxGroups && blocksFor(largest.starts.size()) <= gpu.maxGroups;
        ResidentState residentBench;
        if (residentFits)
        {
            residentBench.create(gpu, kernels, layout, largest.sorted.size(), cellCount(largest.world), false);
        }
        for (size_t caseIndex = 0; caseIndex < benches.size(); ++caseIndex)
        {
            const BenchCase& bench = benches[caseIndex];
            const CpuTiming& cpu = cpuTimings[caseIndex];
            buffers.pack(bench.sorted, bench.starts);
            buffers.copyInputsToDevice(gpu, bench.sorted.size(), bench.starts.size());
            PushWorld push = pushFor(bench.world, bench.sorted.size());
            uint32_t groups = groupsFor(bench.sorted.size());
            std::printf("    Шаг %zu частиц, %s, мс (ядро по меткам времени, медиана | партия из %u шагов | с ожиданием каждого шага):\n",
                bench.sorted.size(), bench.name.c_str(), BENCH_STEPS);
            StepTiming hostSoft = measureSteps(gpu, queries, layout, strict.soft.pipeline, hostSet, push, groups);
            printTiming("память хоста, доводка, " + strict.shaders->modeName, hostSoft);
            std::vector<StepTiming> deviceHardware;
            std::vector<StepTiming> deviceSoft;
            for (const Variant& variant : variants)
            {
                deviceHardware.push_back(measureSteps(gpu, queries, layout, variant.hardware.pipeline, deviceSet, push, groups));
                printTiming("память устройства, аппаратные, " + variant.shaders->modeName, deviceHardware.back());
                deviceSoft.push_back(measureSteps(gpu, queries, layout, variant.soft.pipeline, deviceSet, push, groups));
                printTiming("память устройства, доводка, " + variant.shaders->modeName, deviceSoft.back());
            }
            double placement = bestOf(hostSoft) / bestOf(deviceSoft.back());
            std::printf("      память устройства %s памяти хоста в %.2f раза; доводка стоит", placement >= 1.0 ? "быстрее" : "медленнее",
                placement >= 1.0 ? placement : 1.0 / placement);
            for (size_t idx = 0; idx < variants.size(); ++idx)
            {
                std::printf("%s %+.1f %% (%s)", idx == 0 ? "" : ",", (bestOf(deviceSoft[idx]) / bestOf(deviceHardware[idx]) - 1.0) * 100.0,
                    variants[idx].shaders->modeName.c_str());
            }
            if (variants.size() > 1)
            {
                std::printf("; DenormPreserve меняет шаг на %+.1f %% аппаратно и %+.1f %% с доводкой",
                    (bestOf(deviceHardware[1]) / bestOf(deviceHardware[0]) - 1.0) * 100.0,
                    (bestOf(deviceSoft[1]) / bestOf(deviceSoft[0]) - 1.0) * 100.0);
            }
            std::printf("\n");

            Columns expected;
            stepOnCpu(bench.world, bench.sorted, bench.starts, expected, cpu.threads);
            double inBytes = static_cast<double>(bench.sorted.size() * OUTPUT_COLUMNS + bench.starts.size()) * sizeof(float);
            double outBytes = static_cast<double>(bench.sorted.size() * OUTPUT_COLUMNS) * sizeof(float);
            std::printf("      полный шаг с данными на ЦПУ, доводка, мс: все фазы замерены внутри одних и тех же прогонов, показан прогон с медианным "
                "временем из %u, слагаемые дают целое точно; подготовка — запись входа в отображённую память, запуск — запись команд, отправка "
                "и ожидание сверх работы ГПУ, получение — чтение результата из отображённой памяти:\n", PATH_RUNS);
            std::vector<PathSetup> setups{
                {"через копии в память устройства", deviceSet, true, &buffers.readback},
                {"без копий, ядро читает и пишет память хоста", hostSet, false, &buffers.readback},
            };
            if (!buffers.legacy.empty())
            {
                setups.push_back({formatted("через копии, получение из типа %u, как в v0.050.7", buffers.legacy.front().memoryType), deviceSet, true,
                    &buffers.legacy});
            }
            double copiesMs = 0.0;
            for (size_t idx = 0; idx < setups.size(); ++idx)
            {
                bool matches = false;
                PathRun run = measurePath(gpu, queries, buffers, layout, strict.soft.pipeline, setups[idx], bench, expected, matches);
                printPath(setups[idx], run, queries != VK_NULL_HANDLE, inBytes, outBytes, matches);
                if (idx == 0)
                {
                    copiesMs = run.median.wholeMs;
                }
            }
            double cpuStepMs = bestCpuMs(cpu);
            double kernelMs = bestOf(deviceSoft.back());
            std::printf("      резидентный конвейер: данные живут на ГПУ, сортировка подсчётом по ячейкам и шаг без пересылок, мс на шаг:\n");
            double residentMs = -1.0;
            if (residentFits)
            {
                for (bool soft : {true, false})
                {
                    ResidentTiming timing = measureResident(gpu, queries, kernels, layout, soft ? strict.keysSoft.pipeline : strict.keysHardware.pipeline,
                        soft ? strict.soft.pipeline : strict.hardware.pipeline, residentBench, bench.world, bench.particles);
                    printResident(formatted("%s, %s", soft ? "доводка" : "аппаратные", strict.shaders->modeName.c_str()), timing);
                    if (soft)
                    {
                        residentMs = timing.stampedMs >= 0.0 ? timing.stampedMs : timing.batchMs;
                    }
                }
            }
            else
            {
                std::printf("        пропущен: нужно больше групп, чем %u\n", gpu.maxGroups);
            }
            std::printf("      против ЦПУ %s, %u потоков: шаг на отсортированных данных %.3f мс — ГПУ через копии %.3f (%s), только ядро %.3f (%s)",
                bestCpuName(cpu), cpu.threads, cpuStepMs, copiesMs, ratioText(copiesMs, cpuStepMs).c_str(), kernelMs,
                ratioText(kernelMs, cpuStepMs).c_str());
            if (residentMs >= 0.0)
            {
                std::printf("; сортировка и шаг %.3f + %.3f = %.3f мс — резидентный ГПУ %.3f (%s)", cpu.sortMs, cpuStepMs, cpu.sortMs + cpuStepMs,
                    residentMs, ratioText(residentMs, cpu.sortMs + cpuStepMs).c_str());
            }
            std::printf("\n");
        }
        if (residentFits)
        {
            residentBench.destroy();
        }
        if (queries != VK_NULL_HANDLE)
        {
            vkDestroyQueryPool(gpu.dev, queries, nullptr);
        }
    }
    std::printf("\n");

    for (Variant& variant : variants)
    {
        for (ComputePipeline* pipeline : {&variant.hardware, &variant.soft, &variant.approx, &variant.keysHardware, &variant.keysSoft})
        {
            pipeline->destroy(gpu.dev);
        }
    }
    kernels.destroy(gpu.dev);
    vkDestroyDescriptorPool(gpu.dev, pool, nullptr);
    layout.destroy(gpu.dev);
    buffers.destroy();
    gpu.destroy();
}

static BenchCase makeBenchCase(const std::string& name, uint32_t side, float latticeGap)
{
    BenchCase bench;
    bench.world = worldFor(side);
    bench.world.latticeStep = bench.world.diameter * latticeGap;
    bench.particles = initialColumns(bench.world);
    distributeAndSort(bench.world, bench.particles, bench.sorted, bench.starts, threadsFor(bench.particles.size()));
    bench.contacts = countContacts(bench.world, bench.sorted, bench.starts).pairs;
    bench.name = formatted("%s, %zu касаний", name.c_str(), bench.contacts);
    return bench;
}

static void printCpuTiming(const BenchCase& bench, const CpuTiming& timing)
{
    std::printf("ЦПУ, шаг %zu частиц, %s, мс: сортировка %u потоков %.3f; скаляр 1 поток %.3f, %u потоков %.3f", bench.sorted.size(),
        bench.name.c_str(), timing.threads, timing.sortMs, timing.scalarOneMs, timing.threads, timing.scalarAllMs);
    if (timing.wide)
    {
        std::printf("; AVX2×8 1 поток %.3f, %u потоков %.3f (медианы, скаляр и AVX2 чередуются); AVX2 побитно равен скаляру — %s",
            timing.wideOneMs, timing.threads, timing.wideAllMs, timing.wideMatches ? "да" : "НЕТ");
    }
    else
    {
        std::printf("; AVX2 на этом ЦПУ нет");
    }
    if (timing.scalarCores.hybrid)
    {
        std::printf("; ЦПУ гибридный, ядро потока в начале и конце каждого прогона в один поток (CPUID 0x1A): скаляр — %s",
            timing.scalarCores.text().c_str());
        if (timing.wide)
        {
            std::printf(", AVX2 — %s", timing.wideCores.text().c_str());
        }
    }
    std::printf("\n");
}

static Parts parseParts(int argc, char** argv)
{
    Parts parts;
    bool named = false;
    Parts chosen{false, false, false, parts.benchSide};
    for (int idx = 1; idx < argc; ++idx)
    {
        std::string part = argv[idx];
        if ((part == "--сторона" || part == "--side") && idx + 1 < argc)
        {
            parts.benchSide = chosen.benchSide = static_cast<uint32_t>(std::atoi(argv[++idx]));
            continue;
        }
        named = true;
        if (part == "пробы" || part == "probes")
        {
            chosen.probes = true;
        }
        else if (part == "траектории" || part == "trajectories")
        {
            chosen.trajectories = true;
        }
        else if (part == "замеры" || part == "timing")
        {
            chosen.timing = true;
        }
        else
        {
            std::fprintf(stderr, "Неизвестная часть %s; есть: пробы (probes), траектории (trajectories), замеры (timing), "
                "--сторона N (--side N)\n", argv[idx]);
            std::exit(2);
        }
    }
    return named ? chosen : parts;
}

int main(int argc, char** argv)
{
    _mm_setcsr(_mm_getcsr() & ~0x8040u);
    Parts parts = parseParts(argc, argv);

    ShaderSet defaults = loadShaders("по умолчанию", "");
    ShaderSet preserve = loadShaders("DenormPreserve", "_preserve");
    SortShaders sortShaders = loadSortShaders();
    ScalarProbe scalar = makeScalarProbe();
    ProbeInputs pairs = makeProbeInputs();
    Reference reference = makeReference();
    std::vector<BenchCase> benches;
    benches.push_back(makeBenchCase("решётка без касаний", parts.benchSide, LATTICE_GAP));
    benches.push_back(makeBenchCase("плотная решётка", parts.benchSide, 0.99f));
    std::vector<CpuTiming> cpuTimings;
    if (parts.timing)
    {
        for (const BenchCase& bench : benches)
        {
            cpuTimings.push_back(measureCpu(bench));
            printCpuTiming(bench, cpuTimings.back());
        }
    }

    std::printf("Эталон ЦПУ: %zu частиц, %u шагов от начальной решётки за %.0f мс, первые касания на шаге %u\n", reference.start.size(),
        TRAJECTORY_STEPS, reference.cpuMs, reference.firstContact);
    for (uint32_t idx = 0; idx < CHECKPOINT_COUNT; ++idx)
    {
        std::printf("  шаг %3u: касаний %5zu пар, у стены %4zu частиц, сумма по номерам 0x%016llX\n", CHECKPOINTS[idx],
            reference.checkpointContacts[idx].pairs, reference.checkpointContacts[idx].walls,
            static_cast<unsigned long long>(reference.checkpointHashes[idx]));
    }

    VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "SplavVulkanVerify", 1, nullptr, 1, VK_API_VERSION_1_2};
    VkInstanceCreateInfo instInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &appInfo, 0, nullptr, 0, nullptr};
    VkInstance instance = VK_NULL_HANDLE;
    checkVk(vkCreateInstance(&instInfo, nullptr, &instance), "vkCreateInstance");

    uint32_t devCount = 0;
    vkEnumeratePhysicalDevices(instance, &devCount, nullptr);
    std::vector<VkPhysicalDevice> devs(devCount);
    vkEnumeratePhysicalDevices(instance, &devCount, devs.data());

    std::printf("Найдено Vulkan-устройств: %u\n\n", devCount);
    for (VkPhysicalDevice physDev : devs)
    {
        testPhysicalDevice(physDev, defaults, preserve, sortShaders, scalar, pairs, reference, benches, cpuTimings, parts);
    }

    vkDestroyInstance(instance, nullptr);
    return 0;
}
