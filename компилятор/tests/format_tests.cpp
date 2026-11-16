#include "format/formatter.hpp"
#include "syntax/lexer.hpp"
#include "syntax/parser.hpp"
#include "tests/corpus.hpp"
#include "tests/test.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace
{

using namespace splav;

bool isGenerated(std::string_view name)
{
    return name.starts_with("модель/заголовки/") || name.starts_with("корпус/злые-сочетания/");
}

std::string formatted(std::string_view source, ParseMode mode = ParseMode::Module)
{
    const Parse parsed = parse(source, mode);
    return parsed.diagnostics.empty() ? format(*parsed.root) : std::string("<не разбирается>");
}

uint32_t nextRandom(uint32_t& state)
{
    state = state * 1664525u + 1013904223u;
    return state >> 8;
}

std::string withInsertedTrivia(std::string_view source, uint32_t seed)
{
    constexpr uint32_t ONE_IN = 9;
    std::string result;
    std::vector<SyntaxKind> brackets;
    uint32_t state = seed;
    for (const Token& token : lex(source).tokens)
    {
        result += source.substr(token.offset, token.length);
        if (isTrivia(token.kind) || token.kind == SyntaxKind::EndOfFile || token.kind == SyntaxKind::RawString)
        {
            continue;
        }
        if (token.kind == SyntaxKind::LParen || token.kind == SyntaxKind::LBracket || token.kind == SyntaxKind::LBrace)
        {
            brackets.push_back(token.kind);
        }
        else if ((token.kind == SyntaxKind::RParen || token.kind == SyntaxKind::RBracket
                     || token.kind == SyntaxKind::RBrace)
            && !brackets.empty())
        {
            brackets.pop_back();
        }
        const bool insideParens = !brackets.empty() && brackets.back() == SyntaxKind::LParen;
        const bool lineBreakIsSafe =
            insideParens && (token.kind == SyntaxKind::Comma || token.kind == SyntaxKind::LParen);
        switch (nextRandom(state) % (ONE_IN * 3))
        {
        case 0:
            result += " /* к */ ";
            break;
        case 1:
            result += lineBreakIsSafe ? " // к\n" : "";
            break;
        case 2:
            result += lineBreakIsSafe ? "\n" : "";
            break;
        default:
            break;
        }
    }
    return result;
}

size_t countOccurrences(std::string_view text, std::string_view needle)
{
    size_t count = 0;
    for (size_t found = text.find(needle); found != std::string_view::npos;
        found = text.find(needle, found + needle.size()))
    {
        ++count;
    }
    return count;
}

std::string firstDifference(std::string_view left, std::string_view right)
{
    size_t lineStart = 0;
    size_t index = 0;
    while (index < left.size() && index < right.size() && left[index] == right[index])
    {
        lineStart = left[index] == '\n' ? index + 1 : lineStart;
        ++index;
    }
    const auto lineFrom = [lineStart](std::string_view text)
    {
        const std::string_view tail = text.substr(std::min(lineStart, text.size()));
        return std::string(tail.substr(0, tail.find('\n')));
    };
    return "    было:  " + lineFrom(left) + "\n    стало: " + lineFrom(right);
}

std::string withCollapsedWhitespace(std::string_view source)
{
    std::string result;
    bool lineStart = true;
    for (const Token& token : lex(source).tokens)
    {
        if (token.kind == SyntaxKind::Whitespace)
        {
            result += lineStart ? "" : " ";
        }
        else
        {
            result += source.substr(token.offset, token.length);
        }
        lineStart = token.kind == SyntaxKind::Newline;
    }
    return result;
}

}

SPLAV_TEST(keepsCorpusCanonical, "формат: рукописный корпус канонический, повторный формат ничего не меняет")
{
    size_t generatedChanged = 0;
    for (const test::CorpusFile& file : test::corpusFiles(context.root()))
    {
        const std::string once = formatted(file.source, file.mode);
        const std::string twice = formatted(once, file.mode);
        if (once != twice)
        {
            context.fail(file.name + ": повторное форматирование меняет текст");
        }
        if (once == file.source)
        {
            continue;
        }
        if (isGenerated(file.name))
        {
            ++generatedChanged;
        }
        else
        {
            context.fail(file.name + ": файл не в каноническом виде, нужен «сплав формат»");
        }
    }
    context.expectEqual(generatedChanged, size_t{3}, "число порождённых файлов со строками длиннее 100 символов");
}

SPLAV_TEST(ignoresSourceSpacing, "формат: результат не зависит от пробелов и отступов исходника")
{
    for (const test::CorpusFile& file : test::corpusFiles(context.root()))
    {
        const std::string squeezed = withCollapsedWhitespace(file.source);
        if (formatted(squeezed, file.mode) != formatted(file.source, file.mode))
        {
            context.fail(file.name + ": формат зависит от пробелов исходника");
        }
    }
}

SPLAV_TEST(survivesInsertedTrivia, "формат: устойчив к комментариям и переносам в случайных местах")
{
    constexpr uint32_t VARIANTS_PER_FILE = 8;
    constexpr size_t MAX_REPORTED = 5;
    size_t failures = 0;
    size_t variants = 0;
    for (const test::CorpusFile& file : test::corpusFiles(context.root()))
    {
        if (isGenerated(file.name))
        {
            continue;
        }
        for (uint32_t seed = 1; seed <= VARIANTS_PER_FILE; ++seed)
        {
            const std::string perturbed =
                withInsertedTrivia(file.source, seed * 2654435761u + static_cast<uint32_t>(file.source.size()));
            const Parse parsed = parse(perturbed, file.mode);
            if (!parsed.diagnostics.empty())
            {
                context.fail(file.name + ": вставка комментариев сломала разбор: "
                    + renderDiagnostic(parsed.diagnostics.front(), perturbed, Language::Russian));
                continue;
            }
            ++variants;
            const std::string once = format(*parsed.root);
            if (countOccurrences(once, "/* к */") != countOccurrences(perturbed, "/* к */")
                || countOccurrences(once, "// к") != countOccurrences(perturbed, "// к"))
            {
                context.fail(file.name + ", вариант " + std::to_string(seed) + ": форматтер потерял комментарий");
            }
            const std::string twice = formatted(once, file.mode);
            if (once != twice && failures++ < MAX_REPORTED)
            {
                context.fail(file.name + ", вариант " + std::to_string(seed) + ": повторный формат меняет текст\n"
                    + firstDifference(once, twice));
            }
        }
    }
    context.expect(variants > 300, "проверено больше 300 вариантов");
    context.expectEqual(failures, size_t{0}, "число неустойчивых вариантов");
}

SPLAV_TEST(normalizesLayout, "формат: Allman, пробелы, запятые и стрелки")
{
    context.expectEqual(formatted("функ ф(а:ц32,б : ц32)->ц32 {\nвозврат а+б*2\n}\n"),
        std::string("функ ф(а: ц32, б: ц32) -> ц32\n{\n    возврат а + б * 2\n}\n"), "скобки по Allman и пробелы");
    context.expectEqual(
        formatted("функ ф()\n{\n\n\n    а = г(1,2,)\n\n\n\n    б = [_]н8{ 1,2 }\n    точка = Точка{х:1,у:2}\n\n}\n"),
        std::string("функ ф()\n{\n    а = г(1, 2)\n\n    б = [_]н8{1, 2}\n    точка = Точка{ х: 1, у: 2 }\n}\n"),
        "пустые строки, висячая запятая и литералы");
    context.expectEqual(
        formatted(
            "функ ф()\n{\n    выбор а\n    {\n        Да => 1\n        Нет(причина) если причина.важна() => {\n}\n"
            "        _ => {\n            б()\n        }\n    }\n}\n"),
        std::string("функ ф()\n{\n    выбор а\n    {\n        Да                                => 1\n"
                    "        Нет(причина) если причина.важна() => {}\n        _                                 =>\n"
                    "        {\n            б()\n        }\n    }\n}\n"),
        "стрелки в столбец, пустой блок в строку");
    context.expectEqual(formatted("[Тест] функ ф()\n{\n    если а {\n    } иначе если б {\n    } иначе {\n    }\n}\n"),
        std::string("[Тест]\nфунк ф()\n{\n    если а\n    {\n    }\n    иначе если б\n    {\n    }\n    иначе\n    {\n "
                    "   }\n}\n"),
        "атрибут на своей строке, иначе на новой строке");
    context.expectEqual(formatted("конст А = 1\nфунк ф()\n{\n}\nконст Б = 2\nконст В = 3\n"),
        std::string("конст А = 1\n\nфунк ф()\n{\n}\n\nконст Б = 2\nконст В = 3\n"),
        "пустая строка вокруг объявлений с телом");
    context.expectEqual(formatted("Пакет{имя:\"а\",\n версия: \"1\"}\n", ParseMode::Manifest),
        std::string("Пакет{\n    имя: \"а\",\n    версия: \"1\",\n}\n"),
        "многострочный литерал: скобки на своих строках");
}

SPLAV_TEST(breaksLongLists, "формат: не поместившиеся аргументы — по одному на строку с висячей запятой")
{
    const std::string call =
        "функ ф()\n{\n    результат = оченьДлинноеИмяФункции(первыйАргумент, второйАргумент, третийАргумент, "
        "четвёртыйАргумент, пятый)\n}\n";
    const std::string expected = "функ ф()\n{\n    результат = оченьДлинноеИмяФункции(\n        первыйАргумент,\n"
                                 "        второйАргумент,\n        третийАргумент,\n        четвёртыйАргумент,\n"
                                 "        пятый,\n    )\n}\n";
    context.expectEqual(formatted(call), expected, "длинный вызов");
    context.expectEqual(formatted(expected), expected, "повторный формат");
    const std::string hugged = "функ ф()\n{\n    совпало = попытка ожидать(вместе.первый(\n        функ() => а(),\n"
                               "        функ() => б(),\n    ))\n}\n";
    context.expectEqual(formatted(hugged), hugged, "вложенный вызов без лишнего отступа");
    const std::string signature = "функ оченьДлинноеИмяФункции(первыйПараметр: ц32, второйПараметр: ц32, "
                                  "третийПараметр: ц32) -> Итог(ц32, Ошибка)\n";
    context.expectEqual(formatted(signature),
        std::string("функ оченьДлинноеИмяФункции(\n    первыйПараметр: ц32,\n    второйПараметр: ц32,\n"
                    "    третийПараметр: ц32,\n) -> Итог(ц32, Ошибка)\n"),
        "сигнатура переносится внутри скобок параметров");
}

SPLAV_TEST(keepsCommentsAndRawStrings, "формат: комментарии и сырые строки сохраняются")
{
    const std::string source =
        "//! модуль\n\n/// документация\nфунк ф()   // хвост\n{\n        // своя строка\n    а = 1 /* внутри */ + 2\n"
        "    запрос =\n  \\\\SELECT 1\n          \\\\FROM т\n}\n";
    const std::string expected =
        "//! модуль\n\n/// документация\nфунк ф() // хвост\n{\n    // своя строка\n"
        "    а = 1 /* внутри */ + 2\n    запрос =\n        \\\\SELECT 1\n        \\\\FROM т\n}\n";
    context.expectEqual(formatted(source), expected, "комментарии и сырая строка");
    context.expectEqual(formatted(expected), expected, "повторный формат");
}
