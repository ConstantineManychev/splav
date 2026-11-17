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
constexpr std::string_view C_HEADER_IMAGES = "модель/заголовки";
constexpr std::string_view FIXED_TWIN_SUFFIX = ".верно.спл";
constexpr std::string_view DIAGNOSTICS_EXTENSION = ".диаг";
constexpr size_t EXPECTED_NAME_EXAMPLES = 11;
const std::set<std::string> AWAITING_TYPES = {"05-закрытоеПоле.спл"};

std::string resolved(const std::filesystem::path& root, const std::filesystem::path& file)
{
    Program program(findPackageRoot(file), root / pathFromUtf8("библиотека"));
    Module* module = program.loadEntry(file);
    if (module == nullptr)
    {
        return "не читается";
    }
    std::string text;
    for (const Report& report : resolveNames(program, *module, Language::Russian))
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

}

SPLAV_TEST(resolvesNameExamples, "имена: злые примеры С02xx дают ровно свой .диаг, двойники чисты")
{
    const std::filesystem::path directory = context.root() / pathFromUtf8(NAME_EXAMPLES);
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
            context.expectEqual(actual, std::string(), name + ": двойник без ошибок имён");
            continue;
        }
        ++examples;
        if (AWAITING_TYPES.contains(name))
        {
            context.expectEqual(actual, std::string(), name + ": ждёт проверки типов, ошибок имён нет");
            continue;
        }
        std::filesystem::path diagnostics = file;
        diagnostics.replace_extension(pathFromUtf8(DIAGNOSTICS_EXTENSION));
        const std::optional<std::string> expected = readFile(diagnostics);
        context.expect(expected.has_value(), name + ": есть файл .диаг");
        context.expectEqual(actual, trimmed(expected.value_or("")), name);
    }
    context.expectEqual(examples, EXPECTED_NAME_EXAMPLES, "число злых примеров имён");
}

SPLAV_TEST(resolvesCorpusNames, "имена: корпус, библиотека, злые примеры памяти и типов и сочетания без ошибок имён")
{
    size_t checked = 0;
    for (const test::CorpusFile& file : test::wellFormedCorpusFiles(context.root()))
    {
        const bool nameExample = file.name.starts_with(NAME_EXAMPLES) && !file.name.ends_with(FIXED_TWIN_SUFFIX);
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
