#include "interp/comptime.hpp"
#include "sema/checker.hpp"
#include "sema/names.hpp"
#include "sema/program.hpp"
#include "support/file_io.hpp"
#include "support/utf8_path.hpp"
#include "tests/corpus.hpp"
#include "tests/test.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace
{

using namespace splav;

constexpr std::string_view NAME_EXAMPLES = "корпус/злые-имена";
constexpr std::string_view TYPE_EXAMPLES = "корпус/злые-типы";
constexpr std::string_view C_HEADER_IMAGES = "модель/заголовки";
constexpr std::string_view FIXED_TWIN_SUFFIX = ".верно.спл";
constexpr std::string_view DIAGNOSTICS_EXTENSION = ".диаг";
constexpr size_t EXPECTED_NAME_EXAMPLES = 11;
constexpr size_t EXPECTED_TYPE_EXAMPLES = 17;

std::string resolved(const std::filesystem::path& root, const std::filesystem::path& file)
{
    Program program(findPackageRoot(file), root / pathFromUtf8("библиотека"));
    Module* module = program.loadEntry(file);
    if (module == nullptr)
    {
        return "не читается";
    }
    std::vector<Report> reports = resolveNames(program, *module, Language::Russian, makeComptimeEvaluator);
    if (reports.empty())
    {
        reports = checkTypes(program, *module, Language::Russian, makeComptimeEvaluator);
    }
    std::string text;
    for (const Report& report : reports)
    {
        text += (text.empty() ? "" : "\n") + renderReport(report, module->source, Language::Russian);
    }
    return text;
}

std::string trimmed(std::string text)
{
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
    {
        text.pop_back();
    }
    return text;
}

size_t compareExamples(test::Context& context, std::string_view directoryName)
{
    const std::filesystem::path directory = context.root() / pathFromUtf8(directoryName);
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(directory))
    {
        if (utf8FromPath(entry.path().extension()) == ".спл")
        {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    size_t examples = 0;
    for (const std::filesystem::path& file : files)
    {
        const std::string name = utf8FromPath(file.filename());
        const std::string actual = resolved(context.root(), file);
        if (name.ends_with(FIXED_TWIN_SUFFIX))
        {
            context.expectEqual(actual, std::string(), name + ": двойник без ошибок");
            continue;
        }
        ++examples;
        std::filesystem::path diagnostics = file;
        diagnostics.replace_extension(pathFromUtf8(DIAGNOSTICS_EXTENSION));
        const std::optional<std::string> expected = readFile(diagnostics);
        context.expect(expected.has_value(), name + ": есть файл .диаг");
        context.expectEqual(actual, trimmed(expected.value_or("")), name);
    }
    return examples;
}

}

SPLAV_TEST(resolvesNameExamples, "имена: злые примеры С02xx дают ровно свой .диаг, двойники чисты")
{
    context.expectEqual(compareExamples(context, NAME_EXAMPLES), EXPECTED_NAME_EXAMPLES, "число злых примеров имён");
}

SPLAV_TEST(checksTypeExamples, "типы: злые примеры С0301–С0316 дают ровно свой .диаг, двойники чисты")
{
    context.expectEqual(compareExamples(context, TYPE_EXAMPLES), EXPECTED_TYPE_EXAMPLES, "число злых примеров типов");
}

SPLAV_TEST(checksCorpus, "имена и типы: корпус, библиотека, злые примеры памяти и сочетания без ошибок")
{
    size_t checked = 0;
    for (const test::CorpusFile& file : test::wellFormedCorpusFiles(context.root()))
    {
        const bool nameExample = (file.name.starts_with(NAME_EXAMPLES) || file.name.starts_with(TYPE_EXAMPLES))
            && !file.name.ends_with(FIXED_TWIN_SUFFIX);
        if (file.mode != ParseMode::Module || nameExample || file.name.starts_with(C_HEADER_IMAGES))
        {
            continue;
        }
        ++checked;
        const std::string actual = resolved(context.root(), context.root() / pathFromUtf8(file.name));
        if (!actual.empty())
        {
            context.fail(file.name + ":\n" + actual);
        }
    }
    context.expect(checked > 600, "проверено больше 600 файлов");
}

SPLAV_TEST(respellsYoToDeclaration, "имена: е и ё приводятся к написанию объявления")
{
    Program program(context.root(), context.root() / pathFromUtf8("библиотека"));
    const std::string source = "подключить ядро.ос\n"
                               "\n"
                               "конст ВСЁ: ц32 = 3\n"
                               "\n"
                               "функ ещё(счёт: ц32) -> ц32\n"
                               "{\n"
                               "    возврат счет + ВСЕ\n"
                               "}\n"
                               "\n"
                               "функ вход() -> Итог(пусто, ос.ОшибкаВвода)\n"
                               "{\n"
                               "    изм счетчик: ц32 = 0\n"
                               "    счётчик += еще(1)\n"
                               "    `счетчик` += 1\n"
                               "    возврат Успех()\n"
                               "}\n";
    Module* module = program.loadSource(source, "проба");
    std::string respelled = source;
    const std::vector<Respelling> respellings = canonicalSpellings(program, *module);
    for (auto respelling = respellings.rbegin(); respelling != respellings.rend(); ++respelling)
    {
        respelled.replace(respelling->offset, respelling->length, respelling->text);
    }
    context.expectEqual(respellings.size(), size_t{4}, "число замен");
    context.expect(respelled.find("возврат счёт + ВСЁ") != std::string::npos, "параметр и константа");
    context.expect(respelled.find("    счетчик += ещё(1)") != std::string::npos, "привязка и функция");
    context.expect(respelled.find("`счетчик` += 1") != std::string::npos, "экранированное имя остаётся как написано");
}

SPLAV_TEST(skipsDeadComptimeBranch, "типы: неактивная ветвь `компил если` не проверяется")
{
    Program program(context.root(), context.root() / pathFromUtf8("библиотека"));
    const std::string source = R"(подключить ядро.ос

функ выбрать() -> ц32
{
    компил если 1 > 2
    {
        число: н8 = "не число"
        _ = ос.несуществующая()
        возврат число как ц32
    }
    иначе
    {
        возврат 1
    }
}

функ всегда() -> ц32
{
    компил если 2 > 1
    {
        строка: стр = 5
        _ = строка
    }
    возврат 0
}

функ обобщённая(компил Т: тип, значение: Т) -> ц32
{
    компил если Т == ц32
    {
        возврат значение
    }
    возврат 0
}
)";
    Module* module = program.loadSource(source, "проба");
    std::vector<Report> reports = resolveNames(program, *module, Language::Russian, makeComptimeEvaluator);
    context.expectEqual(reports.size(), size_t{0}, "ошибки имён");
    reports = checkTypes(program, *module, Language::Russian, makeComptimeEvaluator);
    std::string text;
    for (const Report& report : reports)
    {
        text += renderReport(report, module->source, Language::Russian) + "\n";
    }
    context.expectEqual(text, std::string("21:23 ошибка С0303 «5»: ожидался тип `стр`, а получен `компил_цел`\n"),
        "только активная ветвь");
}

SPLAV_TEST(checksMatchCoverage, "типы: полнота `выбор` (С0301) и недостижимые ветви (С0302) как в модели")
{
    Program program(context.root(), context.root() / pathFromUtf8("библиотека"));
    const std::string source = R"(переч Фигура
{
    Круг(радиус: в64)
    Отрезок(ц32, ц32)
    Ничто
}

структ Точка
{
    х: ц32
    у: бул
}

функ пары(п: (бул, бул)) -> ц32
{
    возврат выбор п
    {
        (да, _) => 1
        (_, нет) => 2
    }
}

функ вложенная(о: ?Фигура) -> ц32
{
    возврат выбор о
    {
        Есть(Круг(_)) => 1
        Есть(Ничто) => 2
        Ничего => 3
    }
}

функ поля(т: Точка) -> ц32
{
    возврат выбор т
    {
        Точка { у: да, х: _ } => 1
        Точка { у: нет } => 2
        Точка { х: 3 } => 3
    }
}

функ охрана(ф: Фигура) -> ц32
{
    возврат выбор ф
    {
        Круг(радиус: _) => 1
        Отрезок(первый, _) если первый > 0 => первый
        Ничто => 3
    }
}

функ полная(ф: Фигура, б: бул) -> ц32
{
    возврат выбор (ф, б)
    {
        (Круг(_), _) или (Ничто, _) => 1
        (Отрезок(_, второй), да) => второй
        (Отрезок(первый, _), нет) => первый
    }
}
)";
    Module* module = program.loadSource(source, "проба");
    std::vector<Report> reports = resolveNames(program, *module, Language::Russian, makeComptimeEvaluator);
    context.expectEqual(reports.size(), size_t{0}, "ошибки имён");
    reports = checkTypes(program, *module, Language::Russian, makeComptimeEvaluator);
    std::string text;
    for (const Report& report : reports)
    {
        text += renderReport(report, module->source, Language::Russian) + "\n";
    }
    context.expectEqual(text,
        std::string("16:13 ошибка С0301 «выбор»: `выбор` перебирает не все значения: нет ветви для `(нет, да)`\n"
                    "25:13 ошибка С0301 «выбор»: `выбор` перебирает не все значения: нет ветви для "
                    "`Есть(Отрезок(_, _))`\n"
                    "39:9 ошибка С0302 «Точка { х: 3 }»: ветвь недостижима: все её значения разбирают ветви выше\n"
                    "45:13 ошибка С0301 «выбор»: `выбор` перебирает не все значения: нет ветви для `Отрезок(_, _)`\n"),
        "отчёты о полноте");
}
