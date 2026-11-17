#pragma once

#include "syntax/syntax_kind.hpp"
#include "unicode/unicode.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace splav
{

enum class Language : uint8_t
{
    Russian,
    English,
};

enum class Message : uint8_t
{
    InvalidUtf8,
    UnknownCharacter,
    UnknownSemicolon,
    SemicolonForColon,
    NumeroSign,
    ForeignLetter,
    MixedScripts,
    UnclosedBlockComment,
    NumberMalformed,
    NumberLetterAfter,
    LiteralEmpty,
    LiteralUnclosed,
    ByteNotAscii,
    StringUnclosed,
    EscapeTruncated,
    EscapeUnicodeDigits,
    EscapeNotScalar,
    EscapeUnknown,
    ExpectedToken,
    ExpectedLiteral,
    ExpectedDeclaration,
    ExpectedExpression,
    ExpectedRangeBound,
    ExpectedBlock,
    ExpectedNewline,
    NonAssociativeComparison,
    KeywordAsName,
    OnlyComptimeParams,
    NestingTooDeep,
    LiteralInHeader,
    EscapedNameUnclosed,
    EscapedNameEmpty,
    EscapedNameSpace,
};

enum class Ending : uint8_t
{
    None,
    Line,
    File,
};

struct Diagnostic
{
    uint16_t code = 0;
    uint32_t offset = 0;
    uint32_t anchorLength = 0;
    Message message = Message::InvalidUtf8;
    Ending ending = Ending::None;
    SyntaxKind token = SyntaxKind::Error;
    std::string text;
    std::string list;
    unicode::Script rare = unicode::Script::None;
    unicode::Script common = unicode::Script::None;
};

constexpr int MAX_NESTING = 256;

struct Position
{
    uint32_t line;
    uint32_t column;
};

uint16_t codeOf(Message message);
Position positionAt(std::string_view source, uint32_t offset);
std::string renderMessage(const Diagnostic& diagnostic, Language language);
std::string renderDiagnostic(const Diagnostic& diagnostic, std::string_view source, Language language);

}
