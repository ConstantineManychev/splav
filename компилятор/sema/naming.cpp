#include "sema/naming.hpp"

#include "syntax/lexer.hpp"
#include "unicode/unicode.hpp"

#include <algorithm>

namespace splav
{

std::string nameKey(std::string_view lexeme)
{
    const std::string name = normalizedName(lexeme);
    std::string key;
    key.reserve(name.size());
    for (size_t index = 0; index < name.size(); ++index)
    {
        const unsigned char lead = static_cast<unsigned char>(name[index]);
        const unsigned char next = index + 1 < name.size() ? static_cast<unsigned char>(name[index + 1]) : 0;
        if (lead == 0xD1 && next == 0x91)
        {
            key += "\xD0\xB5";
            ++index;
        }
        else if (lead == 0xD0 && next == 0x81)
        {
            key += "\xD0\x95";
            ++index;
        }
        else
        {
            key += name[index];
        }
    }
    return key;
}

std::u32string decodedName(std::string_view text)
{
    std::u32string result;
    size_t offset = 0;
    while (offset < text.size())
    {
        const unicode::Decoded symbol = unicode::decodeUtf8(text, offset);
        result += symbol.codePoint;
        offset += std::max<size_t>(1, symbol.length);
    }
    return result;
}

bool startsUppercase(std::string_view name)
{
    const std::u32string symbols = decodedName(name);
    return !symbols.empty() && unicode::isUppercase(symbols.front());
}

}
