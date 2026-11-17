#include "driver/driver.hpp"

#include "format/formatter.hpp"
#include "interp/interpreter.hpp"
#include "sema/checker.hpp"
#include "sema/names.hpp"
#include "sema/program.hpp"
#include "support/file_io.hpp"
#include "support/utf8_path.hpp"
#include "syntax/parser.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>

namespace splav
{

namespace
{

using Arguments = std::span<const std::string>;

struct Command
{
    std::string_view name;
    std::string_view latinName;
    std::string_view summary;
    int (*run)(Arguments);
};

struct Options
{
    Language language = Language::Russian;
    std::optional<ParseMode> mode;
    std::vector<std::string> paths;
    std::optional<std::string> library;
    bool check = false;
    bool valid = true;
};

constexpr std::string_view STANDARD_INPUT = "-";
constexpr std::string_view MANIFEST_NAME = "пакет.спл";

Options parseOptions(Arguments arguments)
{
    constexpr std::string_view MODE = "--режим=";
    constexpr std::string_view LANGUAGE = "--язык=";
    constexpr std::string_view LIBRARY = "--библиотека=";
    Options options;
    for (const std::string& argument : arguments)
    {
        const std::string_view view = argument;
        if (view.starts_with(MODE))
        {
            const std::string_view value = view.substr(MODE.size());
            if (value == "модуль")
            {
                options.mode = ParseMode::Module;
            }
            else if (value == "фрагмент")
            {
                options.mode = ParseMode::Fragment;
            }
            else if (value == "манифест")
            {
                options.mode = ParseMode::Manifest;
            }
            else
            {
                options.valid = false;
            }
        }
        else if (view.starts_with(LANGUAGE))
        {
            const std::string_view value = view.substr(LANGUAGE.size());
            if (value == "en")
            {
                options.language = Language::English;
            }
            else if (value != "ru")
            {
                options.valid = false;
            }
        }
        else if (view.starts_with(LIBRARY))
        {
            options.library = std::string(view.substr(LIBRARY.size()));
        }
        else if (view == "--проверить" || view == "--check")
        {
            options.check = true;
        }
        else if (view.starts_with("--"))
        {
            options.valid = false;
        }
        else
        {
            options.paths.push_back(argument);
        }
    }
    return options;
}

std::optional<std::string> readSource(const std::string& path)
{
    if (path == STANDARD_INPUT)
    {
        return readStandardInput();
    }
    return readFile(pathFromUtf8(path));
}

ParseMode modeFor(const Options& options, const std::string& path)
{
    if (options.mode)
    {
        return *options.mode;
    }
    const bool manifest = path != STANDARD_INPUT && utf8FromPath(pathFromUtf8(path).filename()) == MANIFEST_NAME;
    return manifest ? ParseMode::Manifest : ParseMode::Module;
}

Program programFor(const Options& options, const std::filesystem::path& file)
{
    std::optional<std::filesystem::path> library =
        options.library ? std::optional(pathFromUtf8(*options.library)) : findLibraryRoot(file.parent_path());
    if (!library)
    {
        library = findLibraryRoot(std::filesystem::current_path());
    }
    return Program(findPackageRoot(file), library);
}

bool reportNames(const Options& options, const std::string& path)
{
    const std::filesystem::path file = pathFromUtf8(path);
    Program program = programFor(options, file);
    Module* module = program.loadEntry(file);
    if (module == nullptr)
    {
        return true;
    }
    std::vector<Report> reports = resolveNames(program, *module, options.language);
    if (reports.empty())
    {
        reports = checkTypes(program, *module, options.language);
    }
    for (const Report& report : reports)
    {
        writeStandardError(path + ":" + renderReport(report, module->source, options.language) + "\n");
    }
    return reports.empty();
}

bool reportDiagnostics(const Parse& parsed, std::string_view source, const std::string& path, Language language)
{
    for (const Diagnostic& diagnostic : parsed.diagnostics)
    {
        writeStandardError(path + ":" + renderDiagnostic(diagnostic, source, language) + "\n");
    }
    return parsed.diagnostics.empty();
}

int printVersion(Arguments)
{
    writeStandardOutput("сплав " SPLAV_VERSION "\n");
    return 0;
}

int printTree(Arguments arguments)
{
    const Options options = parseOptions(arguments);
    if (!options.valid || options.paths.size() != 1)
    {
        writeStandardError(
            "использование: сплав дерево [--режим=модуль|фрагмент|манифест] [--язык=ru|en] <файл или ->\n");
        return 2;
    }
    const std::string& path = options.paths.front();
    const std::optional<std::string> source = readSource(path);
    if (!source)
    {
        writeStandardError("сплав: не читается файл «" + path + "»\n");
        return 2;
    }
    const Parse parsed = parse(*source, modeFor(options, path));
    writeStandardOutput(dumpTree(*parsed.root));
    return reportDiagnostics(parsed, *source, path, options.language) ? 0 : 1;
}

int checkFiles(Arguments arguments)
{
    const Options options = parseOptions(arguments);
    if (!options.valid || options.paths.empty())
    {
        writeStandardError("использование: сплав проверить [--режим=модуль|фрагмент|манифест] [--язык=ru|en] "
                           "[--библиотека=путь] <файлы…>\n");
        return 2;
    }
    bool clean = true;
    for (const std::string& path : options.paths)
    {
        const std::optional<std::string> source = readSource(path);
        if (!source)
        {
            writeStandardError("сплав: не читается файл «" + path + "»\n");
            return 2;
        }
        const ParseMode mode = modeFor(options, path);
        const Parse parsed = parse(*source, mode);
        const bool syntaxClean = reportDiagnostics(parsed, *source, path, options.language);
        const bool namesClean =
            !syntaxClean || mode != ParseMode::Module || path == STANDARD_INPUT || reportNames(options, path);
        clean = syntaxClean && namesClean && clean;
    }
    return clean ? 0 : 1;
}

int formatFiles(Arguments arguments)
{
    const Options options = parseOptions(arguments);
    if (!options.valid || options.paths.empty())
    {
        writeStandardError(
            "использование: сплав формат [--проверить] [--режим=модуль|фрагмент|манифест] <файлы или ->\n");
        return 2;
    }
    bool clean = true;
    for (const std::string& path : options.paths)
    {
        const std::optional<std::string> source = readSource(path);
        if (!source)
        {
            writeStandardError("сплав: не читается файл «" + path + "»\n");
            return 2;
        }
        const Parse parsed = parse(*source, modeFor(options, path));
        if (!reportDiagnostics(parsed, *source, path, options.language))
        {
            clean = false;
            continue;
        }
        const std::string formatted = format(*parsed.root);
        if (options.check)
        {
            if (formatted != *source)
            {
                writeStandardError(path + ": формат отличается от канонического\n");
                clean = false;
            }
        }
        else if (path == STANDARD_INPUT)
        {
            writeStandardOutput(formatted);
        }
        else if (formatted != *source && !writeFile(pathFromUtf8(path), formatted))
        {
            writeStandardError("сплав: не записывается файл «" + path + "»\n");
            return 2;
        }
    }
    return clean ? 0 : 1;
}

void writeOutput(bool error, std::string_view text)
{
    if (error)
    {
        writeStandardError(text);
    }
    else
    {
        writeStandardOutput(text);
    }
}

bool readyToRun(Program& program, const Options& options, Module& module, const std::string& path)
{
    bool clean = true;
    for (const std::unique_ptr<Module>& loaded : program.modules())
    {
        for (const Diagnostic& diagnostic : loaded->syntax)
        {
            writeStandardError(utf8FromPath(loaded->path) + ":"
                + renderDiagnostic(diagnostic, loaded->source, options.language) + "\n");
            clean = false;
        }
    }
    if (!clean)
    {
        return false;
    }
    std::vector<Report> reports = resolveNames(program, module, options.language);
    if (reports.empty())
    {
        reports = checkTypes(program, module, options.language);
    }
    for (const Report& report : reports)
    {
        writeStandardError(path + ":" + renderReport(report, module.source, options.language) + "\n");
    }
    return reports.empty();
}

std::filesystem::path entryFile(const std::string& path)
{
    const std::filesystem::path target = pathFromUtf8(path);
    std::error_code error;
    if (std::filesystem::is_directory(target, error))
    {
        return target / pathFromUtf8("вход.спл");
    }
    return target;
}

int runFile(Arguments arguments)
{
    size_t split = 0;
    while (split < arguments.size() && std::string_view(arguments[split]).starts_with("--"))
    {
        ++split;
    }
    const Options options = parseOptions(arguments.subspan(0, split));
    if (!options.valid || split >= arguments.size())
    {
        writeStandardError("использование: сплав запустить [--библиотека=путь] [--язык=ru|en] "
                           "<файл или каталог пакета> [аргументы программы…]\n");
        return 2;
    }
    const std::string& path = arguments[split];
    const std::filesystem::path file = entryFile(path);
    Program program = programFor(options, file);
    Module* module = program.loadEntry(file);
    if (module == nullptr)
    {
        writeStandardError("сплав: не читается файл «" + utf8FromPath(file) + "»\n");
        return 2;
    }
    if (!readyToRun(program, options, *module, utf8FromPath(file)))
    {
        return 1;
    }
    RunOptions run;
    run.arguments.push_back(path);
    for (size_t index = split + 1; index < arguments.size(); ++index)
    {
        run.arguments.push_back(arguments[index]);
    }
    run.write = writeOutput;
    return runProgram(program, *module, run);
}

std::vector<std::filesystem::path> testFiles(const std::string& path)
{
    const std::filesystem::path target = pathFromUtf8(path);
    std::error_code error;
    if (!std::filesystem::is_directory(target, error))
    {
        return {target};
    }
    std::vector<std::filesystem::path> files;
    for (const auto& item : std::filesystem::recursive_directory_iterator(target, error))
    {
        const std::string name = utf8FromPath(item.path().filename());
        if (item.is_regular_file() && name.ends_with(".спл") && name != MANIFEST_NAME)
        {
            files.push_back(item.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

int testFilesCommand(Arguments arguments)
{
    const Options options = parseOptions(arguments);
    if (!options.valid || options.paths.empty())
    {
        writeStandardError("использование: сплав тест [--библиотека=путь] [--язык=ru|en] <файлы или каталоги…>\n");
        return 2;
    }
    size_t total = 0;
    size_t passed = 0;
    bool clean = true;
    for (const std::string& path : options.paths)
    {
        for (const std::filesystem::path& file : testFiles(path))
        {
            Program program = programFor(options, file);
            Module* module = program.loadEntry(file);
            if (module == nullptr)
            {
                writeStandardError("сплав: не читается файл «" + utf8FromPath(file) + "»\n");
                return 2;
            }
            if (!readyToRun(program, options, *module, utf8FromPath(file)))
            {
                clean = false;
                continue;
            }
            RunOptions run;
            run.arguments.push_back(utf8FromPath(file));
            run.write = writeOutput;
            const std::vector<TestOutcome> outcomes = runTests(program, *module, run);
            if (outcomes.empty())
            {
                continue;
            }
            std::string report = utf8FromPath(file) + "\n";
            for (const TestOutcome& outcome : outcomes)
            {
                report += (outcome.passed ? "  ✓ " : "  ✗ ") + outcome.name
                    + (outcome.passed ? std::string() : ": " + outcome.message) + "\n";
                passed += outcome.passed ? 1 : 0;
            }
            total += outcomes.size();
            writeStandardOutput(report);
        }
    }
    writeStandardOutput("итого: " + std::to_string(passed) + " из " + std::to_string(total) + "\n");
    return clean && passed == total ? 0 : 1;
}

constexpr std::array COMMANDS = {
    Command{"формат", "fmt", "привести файлы к каноническому виду", formatFiles},
    Command{"проверить", "check", "проверить синтаксис файлов", checkFiles},
    Command{"запустить", "run", "исполнить программу интерпретатором", runFile},
    Command{"тест", "test", "исполнить функции `[Тест]` интерпретатором", testFilesCommand},
    Command{"дерево", "tree", "отладочная печать синтаксического дерева", printTree},
    Command{"версия", "version", "напечатать версию компилятора", printVersion},
};

int printUsage()
{
    std::string text = "использование: сплав <команда> [аргументы]\n\nкоманды:\n";
    for (const Command& command : COMMANDS)
    {
        text += "  ";
        text += command.name;
        text += " (";
        text += command.latinName;
        text += ") — ";
        text += command.summary;
        text += "\n";
    }
    writeStandardError(text);
    return 2;
}

}

int runDriver(const std::vector<std::string>& arguments)
{
    if (arguments.size() < 2)
    {
        return printUsage();
    }
    const std::string_view requested = arguments[1];
    for (const Command& command : COMMANDS)
    {
        if (requested == command.name || requested == command.latinName)
        {
            return command.run(Arguments(arguments).subspan(2));
        }
    }
    writeStandardError("сплав: неизвестная команда «" + arguments[1] + "»\n");
    return printUsage();
}

}
