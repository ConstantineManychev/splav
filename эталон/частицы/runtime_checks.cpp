#include "model.hpp"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <immintrin.h>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

using Clock = std::chrono::steady_clock;

static double msSince(Clock::time_point t0)
{
    return std::chrono::duration<double>(Clock::now() - t0).count() * 1000.0;
}

struct TreeNode
{
    const char* name;
    TreeNode* parent;
    TreeNode* firstChild;
    TreeNode* nextSibling;
    uint64_t payload;
};

class BumpRegion
{
public:
    explicit BumpRegion(size_t cap) : cap_(cap)
    {
#ifdef _WIN32
        base_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, cap, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
#else
        base_ = static_cast<uint8_t*>(mmap(nullptr, cap, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
#endif
        cur_ = base_;
        lim_ = base_ + cap;
    }

    BumpRegion(BumpRegion&& other) noexcept
        : base_(other.base_), cur_(other.cur_), lim_(other.lim_), cap_(other.cap_)
    {
        other.base_ = nullptr;
        other.cur_ = nullptr;
        other.lim_ = nullptr;
        other.cap_ = 0;
    }

    BumpRegion& operator=(BumpRegion&& other) noexcept
    {
        if (this != &other)
        {
            releaseMemory();
            base_ = other.base_;
            cur_ = other.cur_;
            lim_ = other.lim_;
            cap_ = other.cap_;
            other.base_ = nullptr;
            other.cur_ = nullptr;
            other.lim_ = nullptr;
            other.cap_ = 0;
        }
        return *this;
    }

    BumpRegion(const BumpRegion&) = delete;
    BumpRegion& operator=(const BumpRegion&) = delete;

    ~BumpRegion()
    {
        releaseMemory();
    }

    void releaseMemory()
    {
        if (base_)
        {
#ifdef _WIN32
            VirtualFree(base_, 0, MEM_RELEASE);
#else
            munmap(base_, cap_);
#endif
            base_ = nullptr;
        }
    }

    template <class T, class... Args>
    T* alloc(Args&&... args)
    {
        uintptr_t cur = reinterpret_cast<uintptr_t>(cur_);
        uintptr_t aligned = (cur + alignof(T) - 1) & ~(alignof(T) - 1);
        uintptr_t next = aligned + sizeof(T);
        if (next > reinterpret_cast<uintptr_t>(lim_))
        {
            std::fprintf(stderr, "Region exhausted\n");
            std::exit(1);
        }
        cur_ = reinterpret_cast<uint8_t*>(next);
        return new (reinterpret_cast<void*>(aligned)) T{std::forward<Args>(args)...};
    }

    void reset()
    {
        cur_ = base_;
    }

    void protectReadOnly()
    {
#ifdef _WIN32
        DWORD oldProt = 0;
        if (!VirtualProtect(base_, cap_, PAGE_READONLY, &oldProt))
        {
            std::fprintf(stderr, "VirtualProtect failed\n");
            std::exit(1);
        }
#else
        mprotect(base_, cap_, PROT_READ);
#endif
    }

    void protectReadWrite()
    {
#ifdef _WIN32
        DWORD oldProt = 0;
        VirtualProtect(base_, cap_, PAGE_READWRITE, &oldProt);
#else
        mprotect(base_, cap_, PROT_READ | PROT_WRITE);
#endif
    }

private:
    uint8_t* base_ = nullptr;
    uint8_t* cur_ = nullptr;
    uint8_t* lim_ = nullptr;
    size_t cap_ = 0;
};

static void runRegionBenchmark()
{
    std::printf("=== 2. Проверка Bump-аллокатора региона (§11: выделение и сброс O(1)) ===\n");
    size_t nodeCnt = 2'000'000;
    BumpRegion reg(nodeCnt * sizeof(TreeNode) + 4096);

    auto t0 = Clock::now();
    TreeNode* root = reg.alloc<TreeNode>("root", nullptr, nullptr, nullptr, 0ULL);
    for (size_t idx = 1; idx < nodeCnt; ++idx)
    {
        TreeNode* child = reg.alloc<TreeNode>("node", root, nullptr, root->firstChild, idx);
        root->firstChild = child;
    }
    double regAllocMs = msSince(t0);

    t0 = Clock::now();
    reg.reset();
    double regResetMs = msSince(t0);

    std::vector<TreeNode*> ptrs(nodeCnt);
    t0 = Clock::now();
    for (size_t idx = 0; idx < nodeCnt; ++idx)
    {
        ptrs[idx] = new TreeNode{"node", nullptr, nullptr, nullptr, idx};
    }
    double heapAllocMs = msSince(t0);

    t0 = Clock::now();
    for (size_t idx = 0; idx < nodeCnt; ++idx)
    {
        delete ptrs[idx];
    }
    double heapFreeMs = msSince(t0);

    std::printf("  Выделение 2 млн узлов: Регион = %6.2f мс | new/malloc = %6.2f мс (ускорение %.1fx)\n",
        regAllocMs, heapAllocMs, heapAllocMs / regAllocMs);
    std::printf("  Освобождение 2 млн узлов: Регион O(1) = %6.4f мс | delete/free = %6.2f мс\n\n",
        regResetMs, heapFreeMs);
}

struct NodeGraphCapsule
{
    BumpRegion reg;
    TreeNode* firstNode;
    size_t count;
};

template <class CapsuleT>
class SharedCapsule
{
public:
    explicit SharedCapsule(CapsuleT&& cap)
    {
        state_ = new ControlBlock{std::move(cap), std::atomic<size_t>{1}};
        state_->capsule.reg.protectReadOnly();
    }

    SharedCapsule(const SharedCapsule&) = delete;
    SharedCapsule& operator=(const SharedCapsule&) = delete;

    SharedCapsule(SharedCapsule&& other) noexcept : state_(other.state_)
    {
        other.state_ = nullptr;
    }

    SharedCapsule copy() const
    {
        state_->refCnt.fetch_add(1, std::memory_order_seq_cst);
        return SharedCapsule(state_);
    }

    const CapsuleT& data() const
    {
        return state_->capsule;
    }

    size_t refCount() const
    {
        return state_->refCnt.load(std::memory_order_seq_cst);
    }

    void release()
    {
        if (!state_)
        {
            return;
        }
        if (state_->refCnt.fetch_sub(1, std::memory_order_seq_cst) == 1)
        {
            state_->capsule.reg.protectReadWrite();
            delete state_;
        }
        state_ = nullptr;
    }

    bool tryUnfreeze(CapsuleT& outCap)
    {
        if (!state_ || state_->refCnt.load(std::memory_order_seq_cst) != 1)
        {
            return false;
        }
        state_->capsule.reg.protectReadWrite();
        outCap = std::move(state_->capsule);
        delete state_;
        state_ = nullptr;
        return true;
    }

    void unlockHardwareProtectionForFaultRecovery()
    {
        if (state_)
        {
            state_->capsule.reg.protectReadWrite();
        }
    }

    void relockHardwareProtection()
    {
        if (state_)
        {
            state_->capsule.reg.protectReadOnly();
        }
    }

private:
    struct ControlBlock
    {
        CapsuleT capsule;
        std::atomic<size_t> refCnt;
    };

    explicit SharedCapsule(ControlBlock* blk) : state_(blk) {}

    ControlBlock* state_ = nullptr;
};

#ifdef _WIN32
static SharedCapsule<NodeGraphCapsule>* gActiveShared = nullptr;
static volatile bool gCaughtWriteViolation = false;

static LONG WINAPI VectoredWriteFaultHandler(EXCEPTION_POINTERS* info)
{
    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && gActiveShared != nullptr)
    {
        gCaughtWriteViolation = true;
        gActiveShared->unlockHardwareProtectionForFaultRecovery();
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

static void runCapsuleFreezeTest()
{
    std::printf("=== 3. Проверка капсулы и память.заморозить(капсула) -> память.Общий(К) (§6.3) ===\n");
    size_t cnt = 1'000'000;
    BumpRegion reg(cnt * sizeof(TreeNode) + 65536);
    TreeNode* first = nullptr;
    for (size_t idx = 0; idx < cnt; ++idx)
    {
        TreeNode* n = reg.alloc<TreeNode>("frozen", nullptr, nullptr, nullptr, idx + 1);
        if (idx == 0)
        {
            first = n;
        }
    }

    NodeGraphCapsule capsule{std::move(reg), first, cnt};
    SharedCapsule<NodeGraphCapsule> shared(std::move(capsule));

    unsigned threads = 4;
    std::vector<uint64_t> sums(threads, 0);
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads; ++t)
    {
        SharedCapsule<NodeGraphCapsule> taskCopy = shared.copy();
        pool.emplace_back([&, t, taskShared = std::move(taskCopy)]() mutable
        {
            const NodeGraphCapsule& cap = taskShared.data();
            uint64_t localSum = 0;
            const TreeNode* base = cap.firstNode;
            for (size_t idx = 0; idx < cap.count; ++idx)
            {
                localSum += base[idx].payload;
            }
            sums[t] = localSum;
            taskShared.release();
        });
    }
    for (auto& worker : pool)
    {
        worker.join();
    }

    bool sumsMatch = true;
    uint64_t expected = (cnt * (cnt + 1)) / 2;
    for (unsigned t = 0; t < threads; ++t)
    {
        sumsMatch &= (sums[t] == expected);
    }
    std::printf("  %s 4 задачи получили общая.копия(), прочитали 1 млн узлов и вызвали освободить() (сумма = %llu, счётчик = %zu)\n",
        sumsMatch ? "✓" : "✗", static_cast<unsigned long long>(sums[0]), shared.refCount());

#ifdef _WIN32
    gActiveShared = &shared;
    gCaughtWriteViolation = false;
    PVOID handler = AddVectoredExceptionHandler(1, VectoredWriteFaultHandler);
    first->payload = 999999ULL;
    RemoveVectoredExceptionHandler(handler);
    gActiveShared = nullptr;
    first->payload = 1ULL;
    shared.relockHardwareProtection();

    std::printf("  %s Защита страниц арены от записи из опасно/C (EXCEPTION_ACCESS_VIOLATION): %s\n",
        gCaughtWriteViolation ? "✓" : "✗", gCaughtWriteViolation ? "ПОЙМАНА" : "ПРОПУЩЕНА");
#endif

    NodeGraphCapsule unfrozen{BumpRegion(4096), nullptr, 0};
    bool unfrozenOk = shared.tryUnfreeze(unfrozen);
    unfrozen.firstNode->payload = 42ULL;
    std::printf("  %s разморозить() при единственной копии вернул изменяемую капсулу: %s (запись payload = %llu)\n\n",
        unfrozenOk ? "✓" : "✗", unfrozenOk ? "ДА" : "НЕТ",
        static_cast<unsigned long long>(unfrozen.firstNode->payload));
}

static bool floatBitwiseEqualSpec(float a, float b)
{
    if (std::isnan(a) && std::isnan(b))
    {
        return true;
    }
    uint32_t bitsA = 0, bitsB = 0;
    std::memcpy(&bitsA, &a, 4);
    std::memcpy(&bitsB, &b, 4);
    return bitsA == bitsB;
}

static void runIeeeSpecRules()
{
    std::printf("=== 4. Правила ожидатьПобитно (§7.2 п. 6: различение +0.0/-0.0, равенство NaN, денормалы) ===\n");

    float posZero = 0.0f * 1.0f;
    float negZero = 0.0f * -1.0f;
    float invPos = 1.0f / posZero;
    float invNeg = 1.0f / negZero;

    bool zerosDiffer = !floatBitwiseEqualSpec(posZero, negZero);
    std::printf("  %s +0.0f и -0.0f различаются побитно (1/+0.0 = %+f, 1/-0.0 = %+f): %s\n",
        zerosDiffer ? "✓" : "✗", invPos, invNeg, zerosDiffer ? "ДА (канонизации нет)" : "НЕТ");

    uint32_t x86NanBits = 0xFFC00000u;
    uint32_t gpuNanBits = 0x7FC00001u;
    float x86Nan = 0.0f, gpuNan = 0.0f;
    std::memcpy(&x86Nan, &x86NanBits, 4);
    std::memcpy(&gpuNan, &gpuNanBits, 4);

    bool nansEqual = floatBitwiseEqualSpec(x86Nan, gpuNan);
    std::printf("  %s Разные аппаратные NaN (0x%08X и 0x%08X) равны в ожидатьПобитно: %s\n",
        nansEqual ? "✓" : "✗", x86NanBits, gpuNanBits, nansEqual ? "ДА" : "НЕТ");

    volatile float tiny = 1.0e-38f;
    volatile float factor = 1.0e-4f;
    unsigned int defaultCsr = _mm_getcsr();
    _mm_setcsr(defaultCsr & ~(0x8040u));
    float strictSubnormal = tiny * factor;
    _mm_setcsr(defaultCsr);

    uint32_t subBits = 0;
    std::memcpy(&subBits, &strictSubnormal, 4);
    std::printf("  %s Денормализованные числа сохраняются при выключенных FTZ/DAZ: %e (0x%08X)\n\n",
        subBits != 0 ? "✓" : "✗", strictSubnormal, subBits);
}

int main(int argc, char** argv)
{
    std::vector<std::string> modes;
    for (int idx = 1; idx < argc; ++idx)
    {
        modes.push_back(argv[idx]);
    }
    if (modes.empty())
    {
        modes.push_back("all");
    }
    auto wanted = [&modes](const char* name)
    {
        for (const std::string& mode : modes)
        {
            if (mode == name || mode == "all")
            {
                return true;
            }
        }
        return false;
    };
    if (wanted("region"))
    {
        runRegionBenchmark();
    }
    if (wanted("freeze"))
    {
        runCapsuleFreezeTest();
    }
    if (wanted("ieee"))
    {
        runIeeeSpecRules();
    }
    return 0;
}
