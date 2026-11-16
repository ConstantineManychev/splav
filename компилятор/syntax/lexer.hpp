#pragma once

#include "syntax/diagnostic.hpp"
#include "syntax/syntax_kind.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace splav
{

struct Token
{
    SyntaxKind kind;
    uint32_t offset;
    uint32_t length;
};

struct Lexed
{
    std::vector<Token> tokens;
    std::optional<Diagnostic> error;
};

Lexed lex(std::string_view source);
std::string normalizedName(std::string_view lexeme);
std::vector<std::u32string> nameSegments(std::u32string_view name);

}
