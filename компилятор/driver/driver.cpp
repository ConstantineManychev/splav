#include "driver/driver.hpp"

#include "format/formatter.hpp"
#include "support/file_io.hpp"
#include "support/utf8_path.hpp"
#include "syntax/parser.hpp"

#include <array>
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
    bool check = false;
    bool valid = true;
};

constexpr std::string_view STANDARD_INPUT = "-";
constexpr std::string_view MANIFEST_NAME = "пакет.спл";

Options parseOptions(Arguments arguments)
{
    constexpr std::string_view MODE = "--режим=";
    constexpr std::string_view LANGUAGE = "--язык=";
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
        writeStandardError(
            "использование: сплав проверить [--режим=модуль|фрагмент|манифест] [--язык=ru|en] <файлы…>\n");
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
        clean = reportDiagnostics(parsed, *source, path, options.language) && clean;
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

constexpr std::array COMMANDS = {
    Command{"формат", "fmt", "привести файлы к каноническому виду", formatFiles},
    Command{"проверить", "check", "проверить синтаксис файлов", checkFiles},
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
