#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace splav
{

std::optional<std::string> readFile(const std::filesystem::path& path);
bool writeFile(const std::filesystem::path& path, std::string_view contents);
std::string readStandardInput();
void writeStandardOutput(std::string_view text);
void writeStandardError(std::string_view text);

}
