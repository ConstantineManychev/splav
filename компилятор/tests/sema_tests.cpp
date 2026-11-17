#include "sema/checker.hpp"
#include "sema/names.hpp"
#include "sema/program.hpp"
#include "support/file_io.hpp"
#include "support/utf8_path.hpp"
#include "tests/corpus.hpp"
#include "tests/test.hpp"

#include <algorithm>
#include <set>
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
constexpr size_t EXPECTED_TYPE_EXAMPLES = 16;
const std::set<std::string> CHECKED_BY_MODEL = {"01-неполныйВыбор.спл", "02-недостижимаяВетвь.спл"};

std::string resolved(const std::filesystem::path& root, const std::filesystem::path& file)
{
    Program program(findPackageRoot(file), root / pathFromUtf8("библиотека"));
    Module* module = program.loadEntry(file);
    if (module == nullptr)
    {
        return "не читается";
    }
    std::vector<Report> reports = resolveNames(program, *module, Language::Russian);
    if (reports.empty())
    {
        reports = checkTypes(program, *module, Language::Russian);
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

size_t compareExamples(test::Context& context, std::string_view directoryName, const std::set<std::string>& silent)
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
        if (silent.contains(name))
        {
            context.expectEqual(actual, std::string(), name + ": проверяет модель, у компилятора ошибок нет");
            continue;
        }
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
    context.expectEqual(
        compareExamples(context, NAME_EXAMPLES, {}), EXPECTED_NAME_EXAMPLES, "число злых примеров имён");
}

SPLAV_TEST(checksTypeExamples, "типы: злые примеры С0303–С0316 дают ровно свой .диаг, двойники чисты")
{
    context.expectEqual(
        compareExamples(context, TYPE_EXAMPLES, CHECKED_BY_MODEL), EXPECTED_TYPE_EXAMPLES, "число злых примеров типов");
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
