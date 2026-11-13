#include "support/file_io.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>

namespace splav
{

std::optional<std::string> readFile(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        return std::nullopt;
    }
    std::string contents((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    if (stream.bad())
    {
        return std::nullopt;
    }
    return contents;
}

bool writeFile(const std::filesystem::path& path, std::string_view contents)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream)
    {
        return false;
    }
    stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    stream.flush();
    return stream.good();
}

std::string readStandardInput()
{
    std::string contents;
    char chunk[65536];
    for (;;)
    {
        const size_t count = std::fread(chunk, 1, sizeof(chunk), stdin);
        contents.append(chunk, count);
        if (count < sizeof(chunk))
        {
            return contents;
        }
    }
}

void writeStandardOutput(std::string_view text)
{
    std::fwrite(text.data(), 1, text.size(), stdout);
}

void writeStandardError(std::string_view text)
{
    std::fwrite(text.data(), 1, text.size(), stderr);
}

}
