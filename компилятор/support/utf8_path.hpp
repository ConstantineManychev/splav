#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace splav
{

std::filesystem::path pathFromUtf8(std::string_view text);
std::string utf8FromPath(const std::filesystem::path& path);

}
