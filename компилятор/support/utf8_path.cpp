#include "support/utf8_path.hpp"

namespace splav
{

std::filesystem::path pathFromUtf8(std::string_view text)
{
    const auto* begin = reinterpret_cast<const char8_t*>(text.data());
    return std::filesystem::path(std::u8string_view(begin, text.size()));
}

std::string utf8FromPath(const std::filesystem::path& path)
{
    const std::u8string text = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

}
