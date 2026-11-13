#include "support/file_io.hpp"
#include "support/platform.hpp"
#include "support/utf8_path.hpp"
#include "tests/test.hpp"

#include <string>
#include <string_view>
#include <vector>

int main(int argc, char** argv)
{
    splav::prepareStandardStreams();
    const std::vector<std::string> arguments = splav::utf8Arguments(argc, argv);
    constexpr std::string_view ROOT_OPTION = "--корень=";
    std::string root;
    std::vector<std::string> filters;
    for (size_t index = 1; index < arguments.size(); ++index)
    {
        const std::string_view argument = arguments[index];
        if (argument.starts_with(ROOT_OPTION))
        {
            root = argument.substr(ROOT_OPTION.size());
        }
        else
        {
            filters.emplace_back(argument);
        }
    }
    if (root.empty())
    {
        splav::writeStandardError("использование: splav_tests --корень=<корень репозитория> [часть имени теста]…\n");
        return 2;
    }
    return splav::test::runAll(splav::pathFromUtf8(root), filters);
}
