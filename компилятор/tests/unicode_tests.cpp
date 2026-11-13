#include "support/file_io.hpp"
#include "support/utf8_path.hpp"
#include "tests/test.hpp"
#include "unicode/unicode.hpp"

#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace
{

using namespace splav;

struct NormalizationVector
{
    std::u32string_view source;
    std::u32string_view composed;
    std::u32string_view decomposed;
};

#include "tests/unicode_vectors.inc"

std::string describe(std::u32string_view text)
{
    constexpr std::string_view DIGITS = "0123456789ABCDEF";
    std::string result;
    for (const char32_t codePoint : text)
    {
        result += result.empty() ? "U+" : " U+";
        for (int shift = 20; shift >= 0; shift -= 4)
        {
            const auto digit = static_cast<size_t>((codePoint >> shift) & 0xF);
            if (digit != 0 || shift <= 12)
            {
                result += DIGITS[digit];
            }
        }
    }
    return result;
}

std::u32string codePoints(std::string_view field)
{
    std::u32string result;
    size_t position = 0;
    while (position < field.size())
    {
        while (position < field.size() && field[position] == ' ')
        {
            ++position;
        }
        char32_t codePoint = 0;
        bool any = false;
        while (position < field.size() && field[position] != ' ')
        {
            const char digit = field[position++];
            codePoint = codePoint * 16 + static_cast<char32_t>(digit <= '9' ? digit - '0' : (digit | 0x20) - 'a' + 10);
            any = true;
        }
        if (any)
        {
            result.push_back(codePoint);
        }
    }
    return result;
}

std::vector<std::u32string> conformanceColumns(std::string_view line)
{
    std::vector<std::u32string> columns;
    size_t start = 0;
    while (columns.size() < 5)
    {
        const size_t end = line.find(';', start);
        if (end == std::string_view::npos)
        {
            break;
        }
        columns.push_back(codePoints(line.substr(start, end - start)));
        start = end + 1;
    }
    return columns;
}

}

SPLAV_TEST(decodesUtf8Strictly, "юникод: UTF-8 декодируется строго")
{
    const unicode::Decoded cyrillic = unicode::decodeUtf8("ё", 0);
    context.expect(cyrillic.valid && cyrillic.codePoint == U'ё' && cyrillic.length == 2, "двухбайтовая «ё»");
    const unicode::Decoded astral = unicode::decodeUtf8("\xF0\x9F\x98\x80", 0);
    context.expect(astral.valid && astral.codePoint == 0x1F600 && astral.length == 4, "четырёхбайтовый символ");
    for (const std::string_view broken : {"\xC0\x80", "\xED\xA0\x80", "\xF4\x90\x80\x80", "\xE2\x82", "\x80", "\xFF"})
    {
        const unicode::Decoded decoded = unicode::decodeUtf8(broken, 0);
        context.expect(
            !decoded.valid && decoded.length == 1, "неверная последовательность отвергается по одному байту");
    }
    std::string encoded;
    for (const char32_t codePoint : {U'a', U'ё', char32_t{0x20AC}, char32_t{0x1F600}})
    {
        unicode::appendUtf8(encoded, codePoint);
    }
    context.expectEqual(encoded, std::string("aё€\xF0\x9F\x98\x80"), "кодирование в UTF-8");
}

SPLAV_TEST(classifiesIdentifierCharacters, "юникод: буквы и знаки идентификаторов")
{
    context.expect(
        unicode::isIdentifierStart(U'ж') && unicode::isIdentifierStart(U'Q') && unicode::isIdentifierStart(U'_'),
        "кириллица, латиница и подчёркивание начинают имя");
    context.expect(
        !unicode::isIdentifierStart(U'7') && unicode::isIdentifierContinue(U'7'), "цифра только продолжает имя");
    context.expect(
        !unicode::isIdentifierStart(U'α') && !unicode::isIdentifierContinue(U'α'), "греческая буква не входит в имя");
    context.expect(!unicode::isIdentifierStart(char32_t{0x2160}), "римская цифра — не буква");
    context.expect(unicode::isIdentifierMark(char32_t{0x0301}) && !unicode::isIdentifierStart(char32_t{0x0301}),
        "комбинирующий знак только продолжает имя");
    context.expect(
        !unicode::isIdentifierContinue(char32_t{0x20DD}), "охватывающий знак без XID_Continue не входит в имя");
    context.expect(unicode::identifierScript(U'ж') == unicode::Script::Cyrillic, "письменность «ж»");
    context.expect(unicode::identifierScript(U'o') == unicode::Script::Latin, "письменность латинской «o»");
    context.expect(
        unicode::identifierScript(char32_t{0x0301}) == unicode::Script::Inherited, "ударение наследует письменность");
    context.expect(unicode::identifierScript(char32_t{0x0483}) == unicode::Script::Cyrillic, "титло — кириллица");
    context.expect(
        unicode::identifierScript(char32_t{0x093E}) == unicode::Script::Other, "знак деванагари — другая письменность");
    context.expect(
        unicode::isUppercase(U'Ж') && unicode::isLowercase(U'ж') && !unicode::isLowercase(U'Ж'), "регистр кириллицы");
    context.expect(unicode::isUppercase(U'Q') && unicode::isLowercase(U'q'), "регистр латиницы");
}

SPLAV_TEST(normalizesSampledVectors, "юникод: NFC и NFD на выборке из NormalizationTest.txt")
{
    size_t failures = 0;
    for (const NormalizationVector& vector : NORMALIZATION_VECTORS)
    {
        if (unicode::toNfc(vector.source) != vector.composed || unicode::toNfd(vector.source) != vector.decomposed)
        {
            if (failures++ < 5)
            {
                context.fail("нормализация " + describe(vector.source) + " дала NFC "
                    + describe(unicode::toNfc(vector.source)) + " и NFD " + describe(unicode::toNfd(vector.source)));
            }
        }
    }
    context.expectEqual(failures, size_t{0}, "число расхождений");
    context.expectEqual(
        unicode::encodeUtf8(unicode::toNfc(U"\x438\x306\x435\x308")), std::string("йё"), "и + кратка, е + трема");
}

SPLAV_TEST(normalizesWholeConformanceFile, "юникод: NormalizationTest.txt целиком")
{
    const char* directory = std::getenv("SPLAV_UCD");
    if (directory == nullptr)
    {
        context.skip("переменная SPLAV_UCD не указывает на каталог UCD");
        return;
    }
    const std::optional<std::string> text = readFile(pathFromUtf8(directory) / "NormalizationTest.txt");
    if (!text)
    {
        context.fail("не читается NormalizationTest.txt в каталоге SPLAV_UCD");
        return;
    }
    size_t checked = 0;
    size_t failures = 0;
    size_t lineStart = 0;
    while (lineStart < text->size())
    {
        size_t lineEnd = text->find('\n', lineStart);
        lineEnd = lineEnd == std::string::npos ? text->size() : lineEnd;
        const std::string_view line = std::string_view(*text).substr(lineStart, lineEnd - lineStart);
        lineStart = lineEnd + 1;
        if (line.empty() || line[0] == '#' || line[0] == '@')
        {
            continue;
        }
        const std::vector<std::u32string> columns = conformanceColumns(line);
        if (columns.size() != 5)
        {
            continue;
        }
        ++checked;
        const bool composedHolds = unicode::toNfc(columns[0]) == columns[1] && unicode::toNfc(columns[1]) == columns[1]
            && unicode::toNfc(columns[2]) == columns[1] && unicode::toNfc(columns[3]) == columns[3]
            && unicode::toNfc(columns[4]) == columns[3];
        const bool decomposedHolds = unicode::toNfd(columns[0]) == columns[2]
            && unicode::toNfd(columns[1]) == columns[2] && unicode::toNfd(columns[2]) == columns[2]
            && unicode::toNfd(columns[3]) == columns[4] && unicode::toNfd(columns[4]) == columns[4];
        if (!composedHolds || !decomposedHolds)
        {
            if (failures++ < 5)
            {
                context.fail("строка теста не выполняется для " + describe(columns[0]));
            }
        }
    }
    context.expect(checked > 15000, "прочитано больше 15 000 строк теста");
    context.expectEqual(failures, size_t{0}, "число расхождений");
}

SPLAV_TEST(buildsSkeletons, "юникод: скелеты UTS #39")
{
    context.expect(
        unicode::skeleton(U"ВСЕ") == unicode::skeleton(U"BCE"), "«ВСЕ» кириллицей и «BCE» латиницей совпадают");
    context.expect(
        unicode::skeleton(U"сокет") == unicode::skeleton(U"co\x43A\x435\x442"), "«сокет» и «coкет» совпадают");
    context.expect(unicode::skeleton(U"порт") != unicode::skeleton(U"пост"), "разные имена различаются");
    context.expect(unicode::skeleton(U"а\x34F"
                                     U"б")
            == unicode::skeleton(U"аб"),
        "игнорируемый знак не меняет скелет");
    context.expectEqual(std::string(unicode::version()), std::string("17.0.0"), "версия таблиц");
}
