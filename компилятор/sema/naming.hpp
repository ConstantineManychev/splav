#pragma once

#include <string>
#include <string_view>

namespace splav
{

std::string nameKey(std::string_view lexeme);
std::u32string decodedName(std::string_view text);
bool startsUppercase(std::string_view name);

}
