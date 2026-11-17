#include "syntax/lexer.hpp"
#include "syntax/parser.hpp"
#include "tests/corpus.hpp"
#include "tests/test.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace
{

using namespace splav;

constexpr size_t EXPECTED_CORPUS_FILES = 684;
constexpr size_t EXPECTED_REJECTED_FILES = 24;

std::string firstDiagnostic(
    std::string_view source, ParseMode mode = ParseMode::Module, Language language = Language::Russian)
{
    const Parse parsed = parse(source, mode);
    return parsed.diagnostics.empty() ? std::string() : renderDiagnostic(parsed.diagnostics.front(), source, language);
}

}

SPLAV_TEST(parsesCorpusLosslessly, "синтаксис: корпус разбирается и печатается обратно байт в байт")
{
    const std::vector<test::CorpusFile> files = test::corpusFiles(context.root());
    context.expectEqual(files.size(), EXPECTED_CORPUS_FILES, "число файлов корпуса");
    size_t rejected = 0;
    for (const test::CorpusFile& file : files)
    {
        const Parse parsed = parse(file.source, file.mode);
        const std::string reported = parsed.diagnostics.empty()
            ? std::string()
            : renderDiagnostic(parsed.diagnostics.front(), file.source, Language::Russian);
        if (file.expectedDiagnostic)
        {
            ++rejected;
            context.expectEqual(parsed.diagnostics.size(), size_t{1}, file.name + ": число диагностик");
            context.expectEqual(reported, *file.expectedDiagnostic, file.name + ": диагностика из файла .диаг");
        }
        else if (!parsed.diagnostics.empty())
        {
            context.fail(file.name + ": " + reported);
        }
        if (parsed.root->text() != file.source)
        {
            context.fail(file.name + ": дерево печатается не байт в байт");
        }
    }
    context.expectEqual(rejected, EXPECTED_REJECTED_FILES, "число злых примеров синтаксиса");
}

SPLAV_TEST(keepsEveryByteOfBrokenInput, "синтаксис: дерево хранит каждый байт и при ошибках")
{
    const std::vector<std::string> sources = {
        "",
        "функ",
        "функ ф()\n{\n    х = а == не б\n}\n",
        "конст Х = \"абв\nконст У = 1\n",
        "/* не закрыт\nконст Х = 1\n",
        "конст Х = 4ГБ + 'аб' + \"\\q\"\n",
        std::string("конст Х = \xD0\n\xFF\xFE"),
        "функ ф(а: *?ц32)\n{\n    б = *?",
        "\r\n\tконст  Х   =  1 // хвост\r\n",
        "а = 1 б = 2 ; @ # $\n",
    };
    for (const std::string& source : sources)
    {
        for (const ParseMode mode : {ParseMode::Module, ParseMode::Fragment, ParseMode::Manifest})
        {
            context.expectEqual(parse(source, mode).root->text(), source, "текст дерева");
        }
    }
}

SPLAV_TEST(reportsFirstErrorLikePrototype, "синтаксис: первая ошибка с кодом, позицией и якорем")
{
    context.expectEqual(firstDiagnostic("функ ф()\n{\n    х = а == не б\n}\n"),
        std::string("3:14 ошибка С0111 «не»: ожидалось выражение"), "не после сравнения");
    context.expectEqual(firstDiagnostic("функ ф()\n{\n    с\x6Fкет = 1\n}\n"),
        std::string(
            "3:5 ошибка С0103 «с\x6Fкет»: смешение письменностей в сегменте `с\x6Fкет`: латиница `o` среди кириллицы"),
        "омоглиф");
    context.expectEqual(firstDiagnostic("конст Х\n"),
        std::string("1:8 ошибка С0109: ожидалось «=», а строка закончилась"), "обрыв строки");
    context.expectEqual(firstDiagnostic("функ ф()\n{\n"),
        std::string("3:1 ошибка С0111: ожидалось выражение, а файл закончился"), "обрыв файла");
    context.expectEqual(firstDiagnostic("конст Х = 1 +\nконст У = \"\\q\"\n"),
        std::string("2:12 ошибка С0108 «\\q»: неизвестное экранирование"), "лексическая ошибка раньше синтаксической");
    context.expectEqual(firstDiagnostic("Пакет{ имя: \"а\" } лишнее\n", ParseMode::Manifest),
        std::string("1:19 ошибка С0109 «лишнее»: ожидалось конец файла"), "манифест с хвостом");
    context.expectEqual(firstDiagnostic("конст Х\n", ParseMode::Module, Language::English),
        std::string("1:8 error С0109: expected «=», but the line ended"), "английский вывод");
}

SPLAV_TEST(splitsNamesIntoSegments, "синтаксис: сегменты имени по регистру, цифрам и подчёркиванию")
{
    const auto segments = [](std::u32string_view name)
    {
        std::string joined;
        for (const std::u32string& segment : nameSegments(name))
        {
            joined += (joined.empty() ? "" : "|") + unicode::encodeUtf8(segment);
        }
        return joined;
    };
    context.expectEqual(segments(U"HTTPЗапрос"), std::string("HTTP|Запрос"), "аббревиатура перед словом");
    context.expectEqual(segments(U"http_запрос"), std::string("http|запрос"), "подчёркивание");
    context.expectEqual(segments(U"кодUtf16"), std::string("код|Utf"), "цифры");
    context.expectEqual(segments(U"строкаСЭкранами"), std::string("строка|С|Экранами"), "однобуквенное слово");
    context.expectEqual(normalizedName("и\xCC\x86мя"), std::string("имя").replace(0, 2, "й"), "NFC имени");
}

SPLAV_TEST(buildsExpectedTreeShape, "синтаксис: строение дерева и место пробелов")
{
    const std::string source = "[Тест]\nфунк ф(а: ц32) -> ц32\n{\n    возврат а + 1 * 2\n}\n";
    const Parse parsed = parse(source, ParseMode::Module);
    const std::string expected = "Module@0..75\n"
                                 "  FunctionDecl@0..74\n"
                                 "    Attribute@0..10\n"
                                 "      LBracket@0..1 \"[\"\n"
                                 "      Identifier@1..9 \"Тест\"\n"
                                 "      RBracket@9..10 \"]\"\n"
                                 "    Newline@10..11 \"\\n\"\n"
                                 "    Signature@11..40\n"
                                 "      KwFn@11..19 \"функ\"\n"
                                 "      Whitespace@19..20 \" \"\n"
                                 "      Identifier@20..22 \"ф\"\n"
                                 "      ParamList@22..32\n"
                                 "        LParen@22..23 \"(\"\n"
                                 "        Param@23..31\n"
                                 "          Identifier@23..25 \"а\"\n"
                                 "          Colon@25..26 \":\"\n"
                                 "          Whitespace@26..27 \" \"\n"
                                 "          NameExpr@27..31\n"
                                 "            Identifier@27..31 \"ц32\"\n"
                                 "        RParen@31..32 \")\"\n"
                                 "      Whitespace@32..33 \" \"\n"
                                 "      Arrow@33..35 \"->\"\n"
                                 "      Whitespace@35..36 \" \"\n"
                                 "      NameExpr@36..40\n"
                                 "        Identifier@36..40 \"ц32\"\n"
                                 "    Newline@40..41 \"\\n\"\n"
                                 "    Block@41..74\n"
                                 "      LBrace@41..42 \"{\"\n"
                                 "      Newline@42..43 \"\\n\"\n"
                                 "      Whitespace@43..47 \"    \"\n"
                                 "      ReturnStmt@47..72\n"
                                 "        KwReturn@47..61 \"возврат\"\n"
                                 "        Whitespace@61..62 \" \"\n"
                                 "        BinaryExpr@62..72\n"
                                 "          NameExpr@62..64\n"
                                 "            Identifier@62..64 \"а\"\n"
                                 "          Whitespace@64..65 \" \"\n"
                                 "          Plus@65..66 \"+\"\n"
                                 "          Whitespace@66..67 \" \"\n"
                                 "          BinaryExpr@67..72\n"
                                 "            Literal@67..68\n"
                                 "              Integer@67..68 \"1\"\n"
                                 "            Whitespace@68..69 \" \"\n"
                                 "            Star@69..70 \"*\"\n"
                                 "            Whitespace@70..71 \" \"\n"
                                 "            Literal@71..72\n"
                                 "              Integer@71..72 \"2\"\n"
                                 "      Newline@72..73 \"\\n\"\n"
                                 "      RBrace@73..74 \"}\"\n"
                                 "  Newline@74..75 \"\\n\"\n";
    context.expectEqual(dumpTree(*parsed.root), expected, "дамп дерева");
    context.expect(parsed.diagnostics.empty(), "ошибок нет");
}
