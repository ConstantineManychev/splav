#include "unicode/unicode.hpp"

#include <algorithm>
#include <iterator>

namespace splav::unicode
{

namespace
{

enum Flag : uint8_t
{
    LETTER = 1,
    MARK = 2,
    LATIN = 4,
    CYRILLIC = 8,
    INHERITED = 16,
    OTHER = 32,
    LOWER = 64,
    UPPER = 128,
};

struct IdentifierRange
{
    char32_t first;
    char32_t last;
    uint8_t flags;
};

struct CombiningClassRange
{
    char32_t first;
    char32_t last;
    uint8_t combiningClass;
};

struct Decomposition
{
    char32_t codePoint;
    char32_t first;
    char32_t second;
};

struct Composition
{
    char32_t first;
    char32_t second;
    char32_t composite;
};

struct CodePointRange
{
    char32_t first;
    char32_t last;
};

struct Confusable
{
    char32_t source;
    uint32_t offset;
    uint32_t length;
};

#include "unicode/tables.inc"

constexpr char32_t HANGUL_SYLLABLE_BASE = 0xAC00;
constexpr char32_t HANGUL_LEADING_BASE = 0x1100;
constexpr char32_t HANGUL_VOWEL_BASE = 0x1161;
constexpr char32_t HANGUL_TRAILING_BASE = 0x11A7;
constexpr char32_t HANGUL_LEADING_COUNT = 19;
constexpr char32_t HANGUL_VOWEL_COUNT = 21;
constexpr char32_t HANGUL_TRAILING_COUNT = 28;
constexpr char32_t HANGUL_SYLLABLE_COUNT = HANGUL_LEADING_COUNT * HANGUL_VOWEL_COUNT * HANGUL_TRAILING_COUNT;
constexpr char32_t LAST_CODE_POINT = 0x10FFFF;
constexpr char32_t FIRST_SURROGATE = 0xD800;
constexpr char32_t LAST_SURROGATE = 0xDFFF;

template <typename Range, size_t Count>
const Range* findRange(const Range (&ranges)[Count], char32_t codePoint)
{
    const Range* found = std::upper_bound(std::begin(ranges), std::end(ranges), codePoint,
        [](char32_t value, const Range& range) { return value < range.first; });
    if (found == std::begin(ranges))
    {
        return nullptr;
    }
    --found;
    return codePoint <= found->last ? found : nullptr;
}

uint8_t identifierFlags(char32_t codePoint)
{
    const IdentifierRange* range = findRange(IDENTIFIER_RANGES, codePoint);
    return range == nullptr ? uint8_t{0} : range->flags;
}

uint8_t combiningClass(char32_t codePoint)
{
    const CombiningClassRange* range = findRange(COMBINING_CLASS_RANGES, codePoint);
    return range == nullptr ? uint8_t{0} : range->combiningClass;
}

bool isHangulSyllable(char32_t codePoint)
{
    return codePoint >= HANGUL_SYLLABLE_BASE && codePoint < HANGUL_SYLLABLE_BASE + HANGUL_SYLLABLE_COUNT;
}

void appendDecomposed(std::u32string& output, char32_t codePoint)
{
    if (isHangulSyllable(codePoint))
    {
        const char32_t index = codePoint - HANGUL_SYLLABLE_BASE;
        output.push_back(HANGUL_LEADING_BASE + index / (HANGUL_VOWEL_COUNT * HANGUL_TRAILING_COUNT));
        output.push_back(
            HANGUL_VOWEL_BASE + index % (HANGUL_VOWEL_COUNT * HANGUL_TRAILING_COUNT) / HANGUL_TRAILING_COUNT);
        if (index % HANGUL_TRAILING_COUNT != 0)
        {
            output.push_back(HANGUL_TRAILING_BASE + index % HANGUL_TRAILING_COUNT);
        }
        return;
    }
    const Decomposition* found = std::lower_bound(std::begin(DECOMPOSITIONS), std::end(DECOMPOSITIONS), codePoint,
        [](const Decomposition& entry, char32_t value) { return entry.codePoint < value; });
    if (found == std::end(DECOMPOSITIONS) || found->codePoint != codePoint)
    {
        output.push_back(codePoint);
        return;
    }
    appendDecomposed(output, found->first);
    if (found->second != 0)
    {
        appendDecomposed(output, found->second);
    }
}

void sortCombiningMarks(std::u32string& text)
{
    size_t index = 0;
    while (index < text.size())
    {
        if (combiningClass(text[index]) == 0)
        {
            ++index;
            continue;
        }
        size_t end = index;
        while (end < text.size() && combiningClass(text[end]) != 0)
        {
            ++end;
        }
        std::stable_sort(text.begin() + static_cast<std::ptrdiff_t>(index),
            text.begin() + static_cast<std::ptrdiff_t>(end),
            [](char32_t left, char32_t right) { return combiningClass(left) < combiningClass(right); });
        index = end;
    }
}

char32_t composePair(char32_t first, char32_t second)
{
    if (first >= HANGUL_LEADING_BASE && first < HANGUL_LEADING_BASE + HANGUL_LEADING_COUNT
        && second >= HANGUL_VOWEL_BASE && second < HANGUL_VOWEL_BASE + HANGUL_VOWEL_COUNT)
    {
        return HANGUL_SYLLABLE_BASE
            + ((first - HANGUL_LEADING_BASE) * HANGUL_VOWEL_COUNT + (second - HANGUL_VOWEL_BASE))
            * HANGUL_TRAILING_COUNT;
    }
    if (isHangulSyllable(first) && (first - HANGUL_SYLLABLE_BASE) % HANGUL_TRAILING_COUNT == 0
        && second > HANGUL_TRAILING_BASE && second < HANGUL_TRAILING_BASE + HANGUL_TRAILING_COUNT)
    {
        return first + (second - HANGUL_TRAILING_BASE);
    }
    const Composition* found = std::lower_bound(std::begin(COMPOSITIONS), std::end(COMPOSITIONS),
        Composition{first, second, 0}, [](const Composition& left, const Composition& right)
    { return left.first != right.first ? left.first < right.first : left.second < right.second; });
    if (found == std::end(COMPOSITIONS) || found->first != first || found->second != second)
    {
        return 0;
    }
    return found->composite;
}

bool isDefaultIgnorable(char32_t codePoint)
{
    return findRange(DEFAULT_IGNORABLE_RANGES, codePoint) != nullptr;
}

void appendPrototype(std::u32string& output, char32_t codePoint)
{
    const Confusable* found = std::lower_bound(std::begin(CONFUSABLES), std::end(CONFUSABLES), codePoint,
        [](const Confusable& entry, char32_t value) { return entry.source < value; });
    if (found == std::end(CONFUSABLES) || found->source != codePoint)
    {
        output.push_back(codePoint);
        return;
    }
    output.append(CONFUSABLE_TARGETS + found->offset, found->length);
}

bool isContinuationByte(std::string_view text, size_t offset)
{
    return offset < text.size() && (static_cast<unsigned char>(text[offset]) & 0xC0) == 0x80;
}

char32_t continuationBits(std::string_view text, size_t offset)
{
    return static_cast<char32_t>(static_cast<unsigned char>(text[offset]) & 0x3F);
}

}

std::string_view version()
{
    return UNICODE_VERSION;
}

Decoded decodeUtf8(std::string_view text, size_t offset)
{
    constexpr Decoded INVALID{0xFFFD, 1, false};
    if (offset >= text.size())
    {
        return Decoded{0, 0, false};
    }
    const auto lead = static_cast<unsigned char>(text[offset]);
    if (lead < 0x80)
    {
        return Decoded{lead, 1, true};
    }
    if (lead >= 0xC2 && lead <= 0xDF)
    {
        if (!isContinuationByte(text, offset + 1))
        {
            return INVALID;
        }
        return Decoded{(static_cast<char32_t>(lead & 0x1F) << 6) | continuationBits(text, offset + 1), 2, true};
    }
    if (lead >= 0xE0 && lead <= 0xEF)
    {
        if (!isContinuationByte(text, offset + 1) || !isContinuationByte(text, offset + 2))
        {
            return INVALID;
        }
        const char32_t codePoint = (static_cast<char32_t>(lead & 0x0F) << 12)
            | (continuationBits(text, offset + 1) << 6) | continuationBits(text, offset + 2);
        if (codePoint < 0x800 || (codePoint >= FIRST_SURROGATE && codePoint <= LAST_SURROGATE))
        {
            return INVALID;
        }
        return Decoded{codePoint, 3, true};
    }
    if (lead >= 0xF0 && lead <= 0xF4)
    {
        if (!isContinuationByte(text, offset + 1) || !isContinuationByte(text, offset + 2)
            || !isContinuationByte(text, offset + 3))
        {
            return INVALID;
        }
        const char32_t codePoint = (static_cast<char32_t>(lead & 0x07) << 18)
            | (continuationBits(text, offset + 1) << 12) | (continuationBits(text, offset + 2) << 6)
            | continuationBits(text, offset + 3);
        if (codePoint < 0x10000 || codePoint > LAST_CODE_POINT)
        {
            return INVALID;
        }
        return Decoded{codePoint, 4, true};
    }
    return INVALID;
}

void appendUtf8(std::string& output, char32_t codePoint)
{
    if (codePoint < 0x80)
    {
        output.push_back(static_cast<char>(codePoint));
    }
    else if (codePoint < 0x800)
    {
        output.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
    else if (codePoint < 0x10000)
    {
        output.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
    else
    {
        output.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
}

std::string encodeUtf8(std::u32string_view text)
{
    std::string output;
    output.reserve(text.size() * 2);
    for (const char32_t codePoint : text)
    {
        appendUtf8(output, codePoint);
    }
    return output;
}

bool isIdentifierLetter(char32_t codePoint)
{
    return (identifierFlags(codePoint) & LETTER) != 0;
}

bool isIdentifierMark(char32_t codePoint)
{
    return (identifierFlags(codePoint) & MARK) != 0;
}

bool isIdentifierStart(char32_t codePoint)
{
    return codePoint == U'_' || isIdentifierLetter(codePoint);
}

bool isIdentifierContinue(char32_t codePoint)
{
    return codePoint == U'_' || (codePoint >= U'0' && codePoint <= U'9') || identifierFlags(codePoint) != 0;
}

Script identifierScript(char32_t codePoint)
{
    const uint8_t flags = identifierFlags(codePoint);
    if ((flags & LATIN) != 0)
    {
        return Script::Latin;
    }
    if ((flags & CYRILLIC) != 0)
    {
        return Script::Cyrillic;
    }
    if ((flags & INHERITED) != 0)
    {
        return Script::Inherited;
    }
    if ((flags & OTHER) != 0)
    {
        return Script::Other;
    }
    return Script::None;
}

bool isLowercase(char32_t codePoint)
{
    return (identifierFlags(codePoint) & LOWER) != 0;
}

bool isUppercase(char32_t codePoint)
{
    return (identifierFlags(codePoint) & UPPER) != 0;
}

std::u32string toNfd(std::u32string_view text)
{
    std::u32string output;
    output.reserve(text.size());
    for (const char32_t codePoint : text)
    {
        appendDecomposed(output, codePoint);
    }
    sortCombiningMarks(output);
    return output;
}

std::u32string toNfc(std::u32string_view text)
{
    constexpr int BLOCKED_FROM_START = 256;
    const std::u32string decomposed = toNfd(text);
    if (decomposed.empty())
    {
        return decomposed;
    }
    std::u32string output;
    output.reserve(decomposed.size());
    output.push_back(decomposed[0]);
    size_t starterPosition = 0;
    int lastClass = combiningClass(decomposed[0]) == 0 ? 0 : BLOCKED_FROM_START;
    for (size_t index = 1; index < decomposed.size(); ++index)
    {
        const char32_t codePoint = decomposed[index];
        const int currentClass = combiningClass(codePoint);
        const char32_t composite = composePair(output[starterPosition], codePoint);
        if (composite != 0 && (lastClass < currentClass || lastClass == 0))
        {
            output[starterPosition] = composite;
            continue;
        }
        if (currentClass == 0)
        {
            starterPosition = output.size();
        }
        lastClass = currentClass;
        output.push_back(codePoint);
    }
    return output;
}

std::u32string skeleton(std::u32string_view text)
{
    std::u32string mapped;
    for (const char32_t codePoint : toNfd(text))
    {
        if (!isDefaultIgnorable(codePoint))
        {
            appendPrototype(mapped, codePoint);
        }
    }
    return toNfd(mapped);
}

}
