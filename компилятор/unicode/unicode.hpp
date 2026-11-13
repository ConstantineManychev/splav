#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace splav::unicode
{

enum class Script : uint8_t
{
    None,
    Latin,
    Cyrillic,
    Inherited,
    Other,
};

struct Decoded
{
    char32_t codePoint;
    uint8_t length;
    bool valid;
};

std::string_view version();

Decoded decodeUtf8(std::string_view text, size_t offset);
void appendUtf8(std::string& output, char32_t codePoint);
std::string encodeUtf8(std::u32string_view text);

bool isIdentifierLetter(char32_t codePoint);
bool isIdentifierMark(char32_t codePoint);
bool isIdentifierStart(char32_t codePoint);
bool isIdentifierContinue(char32_t codePoint);
Script identifierScript(char32_t codePoint);
bool isLowercase(char32_t codePoint);
bool isUppercase(char32_t codePoint);

std::u32string toNfd(std::u32string_view text);
std::u32string toNfc(std::u32string_view text);
std::u32string skeleton(std::u32string_view text);

}
