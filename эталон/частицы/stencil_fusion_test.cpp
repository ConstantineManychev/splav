#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <immintrin.h>
#include <optional>
#include <vector>

#if defined(__GNUC__) || defined(__clang__)
#define TARGET_AVX2 __attribute__((target("avx2")))
#else
#define TARGET_AVX2
#endif

using Clock = std::chrono::steady_clock;

enum class Boundary
{
    Clamp,
    Wrap,
    Mirror,
    Zero,
};

struct Index2
{
    size_t x;
    size_t y;
};

struct Schedule
{
    size_t cpuChunk;
    size_t cpuStripe;
    size_t gpuGroup;
    size_t unroll;
};

template <class T>
class Field2D
{
public:
    Field2D(size_t w, size_t h) : width_(w), height_(h), data_(w * h, T{}) {}

    size_t width() const { return width_; }
    size_t height() const { return height_; }
    T* raw() { return data_.data(); }
    const T* raw() const { return data_.data(); }
    size_t byteSize() const { return data_.size() * sizeof(T); }

    T& at(size_t x, size_t y) { return data_[y * width_ + x]; }
    const T& at(size_t x, size_t y) const { return data_[y * width_ + x]; }

    bool bitwiseEqual(const Field2D<T>& other) const
    {
        return width_ == other.width_ && height_ == other.height_
            && std::memcmp(data_.data(), other.data_.data(), byteSize()) == 0;
    }

private:
    size_t width_;
    size_t height_;
    std::vector<T> data_;
};

template <class T>
class Window2D
{
public:
    Window2D(const Field2D<T>& field, Boundary boundary)
        : field_(field), boundary_(boundary) {}

    T shift(Index2 pt, ptrdiff_t dx, ptrdiff_t dy) const
    {
        ptrdiff_t w = static_cast<ptrdiff_t>(field_.width());
        ptrdiff_t h = static_cast<ptrdiff_t>(field_.height());
        ptrdiff_t nx = static_cast<ptrdiff_t>(pt.x) + dx;
        ptrdiff_t ny = static_cast<ptrdiff_t>(pt.y) + dy;

        if (nx >= 0 && nx < w && ny >= 0 && ny < h)
        {
            return field_.at(static_cast<size_t>(nx), static_cast<size_t>(ny));
        }

        if (boundary_ == Boundary::Zero)
        {
            return T{};
        }
        if (boundary_ == Boundary::Clamp)
        {
            size_t cx = static_cast<size_t>(std::clamp<ptrdiff_t>(nx, 0, w - 1));
            size_t cy = static_cast<size_t>(std::clamp<ptrdiff_t>(ny, 0, h - 1));
            return field_.at(cx, cy);
        }
        if (boundary_ == Boundary::Wrap)
        {
            size_t wx = static_cast<size_t>((nx % w + w) % w);
            size_t wy = static_cast<size_t>((ny % h + h) % h);
            return field_.at(wx, wy);
        }
        ptrdiff_t mx = nx < 0 ? -nx : (nx >= w ? 2 * w - 2 - nx : nx);
        ptrdiff_t my = ny < 0 ? -ny : (ny >= h ? 2 * h - 2 - ny : ny);
        return field_.at(static_cast<size_t>(std::clamp<ptrdiff_t>(mx, 0, w - 1)),
                         static_cast<size_t>(std::clamp<ptrdiff_t>(my, 0, h - 1)));
    }

    const Field2D<T>& field() const { return field_; }

private:
    const Field2D<T>& field_;
    Boundary boundary_;
};

inline float heatStep(Index2 pt, const Window2D<float>& win, float coeff)
{
    if (pt.x == 0)
    {
        return 100.0f;
    }
    float center = win.shift(pt, 0, 0);
    float left = win.shift(pt, -1, 0);
    float right = win.shift(pt, 1, 0);
    float up = win.shift(pt, 0, -1);
    float down = win.shift(pt, 0, 1);
    float sum = ((left + right) + up) + down;
    return center + coeff * (sum - 4.0f * center);
}

inline uint8_t toBrightness(float temp, float limit)
{
    float norm = std::clamp(temp / limit, 0.0f, 1.0f);
    return static_cast<uint8_t>(norm * 255.0f);
}

static void fillHeatBaseline(Field2D<float>& out, const Window2D<float>& win, float coeff)
{
    for (size_t y = 0; y < out.height(); ++y)
    {
        for (size_t x = 0; x < out.width(); ++x)
        {
            out.at(x, y) = heatStep(Index2{x, y}, win, coeff);
        }
    }
}

TARGET_AVX2 static void fillHeatScheduledAvx2x8(Field2D<float>& out, const Window2D<float>& win, float coeff, Schedule plan)
{
    size_t w = out.width();
    size_t h = out.height();
    const float* __restrict__ srcPtr = win.field().raw();
    float* __restrict__ dstPtr = out.raw();

    for (size_t x = 0; x < w; ++x)
    {
        dstPtr[x] = heatStep(Index2{x, 0}, win, coeff);
        dstPtr[(h - 1) * w + x] = heatStep(Index2{x, h - 1}, win, coeff);
    }
    for (size_t y = 1; y + 1 < h; ++y)
    {
        dstPtr[y * w] = heatStep(Index2{0, y}, win, coeff);
        dstPtr[y * w + (w - 1)] = heatStep(Index2{w - 1, y}, win, coeff);
    }

    size_t tileY = plan.cpuChunk > 0 ? plan.cpuChunk : 32;
    __m256 vCoeff = _mm256_set1_ps(coeff);
    __m256 vFour = _mm256_set1_ps(4.0f);

    for (size_t by = 1; by + 1 < h; by += tileY)
    {
        size_t ey = std::min(h - 1, by + tileY);
        for (size_t y = by; y < ey; ++y)
        {
            const float* __restrict__ rowC = srcPtr + y * w;
            const float* __restrict__ rowU = srcPtr + (y - 1) * w;
            const float* __restrict__ rowD = srcPtr + (y + 1) * w;
            float* __restrict__ outRow = dstPtr + y * w;
            size_t x = 1;
            for (; x + 8 <= w - 1; x += 8)
            {
                __m256 c = _mm256_loadu_ps(rowC + x);
                __m256 l = _mm256_loadu_ps(rowC + x - 1);
                __m256 r = _mm256_loadu_ps(rowC + x + 1);
                __m256 u = _mm256_loadu_ps(rowU + x);
                __m256 d = _mm256_loadu_ps(rowD + x);
                __m256 sum = _mm256_add_ps(_mm256_add_ps(_mm256_add_ps(l, r), u), d);
                __m256 lap = _mm256_sub_ps(sum, _mm256_mul_ps(vFour, c));
                __m256 res = _mm256_add_ps(c, _mm256_mul_ps(vCoeff, lap));
                _mm256_storeu_ps(outRow + x, res);
            }
            for (; x < w - 1; ++x)
            {
                float c = rowC[x];
                float sum = ((rowC[x - 1] + rowC[x + 1]) + rowU[x]) + rowD[x];
                outRow[x] = c + coeff * (sum - 4.0f * c);
            }
        }
    }
}

TARGET_AVX2 static void fillBrightnessPassAvx2x8(Field2D<uint8_t>& out, const Field2D<float>& src, float limit)
{
    size_t total = out.width() * out.height();
    const float* __restrict__ inPtr = src.raw();
    uint8_t* __restrict__ outPtr = out.raw();

    __m256 vLim = _mm256_set1_ps(limit);
    __m256 vZero = _mm256_setzero_ps();
    __m256 vOne = _mm256_set1_ps(1.0f);
    __m256 v255 = _mm256_set1_ps(255.0f);

    size_t i = 0;
    for (; i + 8 <= total; i += 8)
    {
        __m256 t = _mm256_loadu_ps(inPtr + i);
        __m256 norm = _mm256_min_ps(_mm256_max_ps(_mm256_div_ps(t, vLim), vZero), vOne);
        __m256 scaled = _mm256_mul_ps(norm, v255);
        __m256i i32 = _mm256_cvttps_epi32(scaled);
        __m128i lo = _mm256_castsi256_si128(i32);
        __m128i hi = _mm256_extracti128_si256(i32, 1);
        __m128i i16 = _mm_packus_epi32(lo, hi);
        __m128i u8 = _mm_packus_epi16(i16, i16);
        uint64_t packed = static_cast<uint64_t>(_mm_cvtsi128_si64(u8));
        std::memcpy(outPtr + i, &packed, 8);
    }
    for (; i < total; ++i)
    {
        outPtr[i] = toBrightness(inPtr[i], limit);
    }
}

TARGET_AVX2 static void fillFusedHeatAndBrightnessAvx2x8(Field2D<uint8_t>& out, const Window2D<float>& win, float coeff, float limit, Schedule plan)
{
    size_t w = out.width();
    size_t h = out.height();
    const float* __restrict__ srcPtr = win.field().raw();
    uint8_t* __restrict__ dstPtr = out.raw();

    for (size_t x = 0; x < w; ++x)
    {
        dstPtr[x] = toBrightness(heatStep(Index2{x, 0}, win, coeff), limit);
        dstPtr[(h - 1) * w + x] = toBrightness(heatStep(Index2{x, h - 1}, win, coeff), limit);
    }
    for (size_t y = 1; y + 1 < h; ++y)
    {
        dstPtr[y * w] = toBrightness(heatStep(Index2{0, y}, win, coeff), limit);
        dstPtr[y * w + (w - 1)] = toBrightness(heatStep(Index2{w - 1, y}, win, coeff), limit);
    }

    size_t tileY = plan.cpuChunk > 0 ? plan.cpuChunk : 32;
    __m256 vCoeff = _mm256_set1_ps(coeff);
    __m256 vFour = _mm256_set1_ps(4.0f);
    __m256 vLim = _mm256_set1_ps(limit);
    __m256 vZero = _mm256_setzero_ps();
    __m256 vOne = _mm256_set1_ps(1.0f);
    __m256 v255 = _mm256_set1_ps(255.0f);

    for (size_t by = 1; by + 1 < h; by += tileY)
    {
        size_t ey = std::min(h - 1, by + tileY);
        for (size_t y = by; y < ey; ++y)
        {
            const float* __restrict__ rowC = srcPtr + y * w;
            const float* __restrict__ rowU = srcPtr + (y - 1) * w;
            const float* __restrict__ rowD = srcPtr + (y + 1) * w;
            uint8_t* __restrict__ outRow = dstPtr + y * w;
            size_t x = 1;
            for (; x + 8 <= w - 1; x += 8)
            {
                __m256 c = _mm256_loadu_ps(rowC + x);
                __m256 l = _mm256_loadu_ps(rowC + x - 1);
                __m256 r = _mm256_loadu_ps(rowC + x + 1);
                __m256 u = _mm256_loadu_ps(rowU + x);
                __m256 d = _mm256_loadu_ps(rowD + x);
                __m256 sum = _mm256_add_ps(_mm256_add_ps(_mm256_add_ps(l, r), u), d);
                __m256 lap = _mm256_sub_ps(sum, _mm256_mul_ps(vFour, c));
                __m256 t = _mm256_add_ps(c, _mm256_mul_ps(vCoeff, lap));

                __m256 norm = _mm256_min_ps(_mm256_max_ps(_mm256_div_ps(t, vLim), vZero), vOne);
                __m256 scaled = _mm256_mul_ps(norm, v255);
                __m256i i32 = _mm256_cvttps_epi32(scaled);
                __m128i lo = _mm256_castsi256_si128(i32);
                __m128i hi = _mm256_extracti128_si256(i32, 1);
                __m128i i16 = _mm_packus_epi32(lo, hi);
                __m128i u8 = _mm_packus_epi16(i16, i16);
                uint64_t packed = static_cast<uint64_t>(_mm_cvtsi128_si64(u8));
                std::memcpy(outRow + x, &packed, 8);
            }
            for (; x < w - 1; ++x)
            {
                float c = rowC[x];
                float sum = ((rowC[x - 1] + rowC[x + 1]) + rowU[x]) + rowD[x];
                float t = c + coeff * (sum - 4.0f * c);
                outRow[x] = toBrightness(t, limit);
            }
        }
    }
}

constexpr bool validateCompileTimeSchedule(Schedule plan, size_t haloRadius, size_t elemBytes)
{
    bool validStripe = (plan.cpuStripe == 0 || plan.cpuStripe == 1 || plan.cpuStripe == 4
                        || plan.cpuStripe == 8 || plan.cpuStripe == 16);
    if (!validStripe)
    {
        return false;
    }
    if (plan.gpuGroup > 128)
    {
        return false;
    }
    size_t tileSide = 16;
    size_t paddedSide = tileSide + 2 * haloRadius;
    size_t sharedBytes = paddedSide * paddedSide * elemBytes;
    return sharedBytes <= 16384;
}

static std::optional<int64_t> checkedAdd(std::optional<int64_t> a, std::optional<int64_t> b)
{
    if (!a || !b)
    {
        return std::nullopt;
    }
    int64_t res = 0;
    if (__builtin_add_overflow(*a, *b, &res))
    {
        return std::nullopt;
    }
    return res;
}

static std::optional<int64_t> checkedMul(std::optional<int64_t> a, std::optional<int64_t> b)
{
    if (!a || !b)
    {
        return std::nullopt;
    }
    int64_t res = 0;
    if (__builtin_mul_overflow(*a, *b, &res))
    {
        return std::nullopt;
    }
    return res;
}

static std::optional<int64_t> parseI64Checked(const char* str)
{
    std::optional<int64_t> val = 0;
    for (size_t i = 0; str[i] != '\0'; ++i)
    {
        int64_t digit = str[i] - '0';
        val = checkedAdd(checkedMul(val, 10), digit);
    }
    return val;
}

int main()
{
    std::printf("=== Проверка §7.3 (слить, Поле, Окно.сдвиг, настроить) и §3.5 (+?, -?, *?) ===\n");

    size_t w = 2048;
    size_t h = 2048;
    Field2D<float> curA(w, h), nextA(w, h);
    Field2D<float> curB(w, h), nextB(w, h);

    Schedule plan{32, 8, 128, 1};
    for (int step = 0; step < 10; ++step)
    {
        fillHeatBaseline(nextA, Window2D<float>(curA, Boundary::Clamp), 0.1f);
        std::swap(curA, nextA);

        fillHeatScheduledAvx2x8(nextB, Window2D<float>(curB, Boundary::Clamp), 0.1f, plan);
        std::swap(curB, nextB);
    }

    bool scheduleMatch = curA.bitwiseEqual(curB);
    std::printf("  %s Расписание (разделение внутренней области и краёв, AVX2 полоса 8) побитно равно базовому: %s\n",
        scheduleMatch ? "✓" : "✗", scheduleMatch ? "ДА" : "НЕТ");

    Field2D<float> tempIntermediate(w, h);
    Field2D<uint8_t> frameTwoPass(w, h);
    Field2D<uint8_t> frameFused(w, h);

    Window2D<float> win(curA, Boundary::Clamp);
    auto runTwoPass = [&]()
    {
        fillHeatScheduledAvx2x8(tempIntermediate, win, 0.1f, plan);
        fillBrightnessPassAvx2x8(frameTwoPass, tempIntermediate, 100.0f);
    };

    runTwoPass();
    fillFusedHeatAndBrightnessAvx2x8(frameFused, win, 0.1f, 100.0f, plan);

    int iters = 30;
    auto t0 = Clock::now();
    for (int i = 0; i < iters; ++i)
    {
        runTwoPass();
    }
    double twoPassMs = std::chrono::duration<double>(Clock::now() - t0).count() * 1000.0 / iters;

    t0 = Clock::now();
    for (int i = 0; i < iters; ++i)
    {
        fillFusedHeatAndBrightnessAvx2x8(frameFused, win, 0.1f, 100.0f, plan);
    }
    double fusedMs = std::chrono::duration<double>(Clock::now() - t0).count() * 1000.0 / iters;

    bool fusionMatch = frameTwoPass.bitwiseEqual(frameFused);
    std::printf("  %s вычислитель.слить(шагТепла, вЯркость) побитно равно двум вызовам заполнить: %s (2 прохода %.2f мс vs слитое %.2f мс, ускорение %.2fx)\n",
        fusionMatch ? "✓" : "✗", fusionMatch ? "ДА" : "НЕТ", twoPassMs, fusedMs, twoPassMs / fusedMs);

    static_assert(validateCompileTimeSchedule(Schedule{0, 8, 128, 1}, 1, sizeof(float)), "Valid Vulkan schedule");
    static_assert(!validateCompileTimeSchedule(Schedule{0, 8, 256, 1}, 1, sizeof(float)), "Exceeds 128 Vulkan compile-time limit");
    std::printf("  ✓ Проверка плана при сборке (полоса 1/4/8/16, группа <= 128 для гарантий Vulkan): ОК\n");

    auto okNum = parseI64Checked("9223372036854775807");
    auto overflowNum = parseI64Checked("9223372036854775808");
    bool checkedArithOk = okNum.has_value() && (*okNum == INT64_MAX) && !overflowNum.has_value();
    std::printf("  %s Проверяемая арифметика (*? и +? в разобратьЦ64): макс ц64 = %lld, переполнение = %s\n",
        checkedArithOk ? "✓" : "✗",
        static_cast<long long>(okNum.value_or(0)),
        overflowNum.has_value() ? "ошибка" : "Ничего (✓)");

    return (scheduleMatch && fusionMatch && checkedArithOk) ? 0 : 1;
}
