#include "support/platform.hpp"

#ifdef _WIN32
#include <windows.h>

#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <shellapi.h>
#endif

namespace splav
{

#ifdef _WIN32

namespace
{

std::string utf8FromWide(const wchar_t* text)
{
    const int sizeWithTerminator = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (sizeWithTerminator <= 1)
    {
        return {};
    }
    std::string result(static_cast<size_t>(sizeWithTerminator - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), sizeWithTerminator - 1, nullptr, nullptr);
    return result;
}

}

std::vector<std::string> utf8Arguments([[maybe_unused]] int argc, [[maybe_unused]] char** argv)
{
    int count = 0;
    wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count);
    std::vector<std::string> arguments;
    if (wide == nullptr)
    {
        return arguments;
    }
    arguments.reserve(static_cast<size_t>(count));
    for (int index = 0; index < count; ++index)
    {
        arguments.push_back(utf8FromWide(wide[index]));
    }
    LocalFree(wide);
    return arguments;
}

void prepareStandardStreams()
{
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
}

#else

std::vector<std::string> utf8Arguments(int argc, char** argv)
{
    return std::vector<std::string>(argv, argv + argc);
}

void prepareStandardStreams()
{
}

#endif

}
