#include "driver/driver.hpp"

#include "support/file_io.hpp"

#include <array>
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

int printVersion(Arguments)
{
    writeStandardOutput("сплав " SPLAV_VERSION "\n");
    return 0;
}

constexpr std::array COMMANDS = {
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
