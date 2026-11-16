#include "syntax/lexer.hpp"

#include "unicode/unicode.hpp"

#include <algorithm>
#include <array>

namespace splav
{

namespace
{

constexpr std::array PUNCTUATION_BY_LENGTH = {
    SyntaxKind::PlusPercentEq,
    SyntaxKind::MinusPercentEq,
    SyntaxKind::StarPercentEq,
    SyntaxKind::ShlEq,
    SyntaxKind::ShrEq,
    SyntaxKind::DotDotEq,
    SyntaxKind::Ellipsis,
    SyntaxKind::PlusPercent,
    SyntaxKind::MinusPercent,
    SyntaxKind::StarPercent,
    SyntaxKind::PlusQuestion,
    SyntaxKind::MinusQuestion,
    SyntaxKind::StarQuestion,
    SyntaxKind::Shl,
    SyntaxKind::Shr,
    SyntaxKind::EqEq,
    SyntaxKind::NotEq,
    SyntaxKind::LessEq,
    SyntaxKind::GreaterEq,
    SyntaxKind::DotDot,
    SyntaxKind::PlusEq,
    SyntaxKind::MinusEq,
    SyntaxKind::StarEq,
    SyntaxKind::SlashEq,
    SyntaxKind::PercentEq,
    SyntaxKind::AmpEq,
    SyntaxKind::Arrow,
    SyntaxKind::FatArrow,
    SyntaxKind::Plus,
    SyntaxKind::Minus,
    SyntaxKind::Star,
    SyntaxKind::Slash,
    SyntaxKind::Percent,
    SyntaxKind::Amp,
    SyntaxKind::Less,
    SyntaxKind::Greater,
    SyntaxKind::Eq,
    SyntaxKind::Question,
    SyntaxKind::Dot,
    SyntaxKind::Comma,
    SyntaxKind::Colon,
    SyntaxKind::LParen,
    SyntaxKind::RParen,
    SyntaxKind::LBracket,
    SyntaxKind::RBracket,
    SyntaxKind::LBrace,
    SyntaxKind::RBrace,
};

constexpr std::string_view BYTE_PREFIX = "б'";
constexpr std::string_view RAW_STRING_PREFIX = "\\\\";
constexpr char32_t LAST_ASCII = 0x7F;
constexpr char32_t LAST_SCALAR = 0x10FFFF;
constexpr char32_t FIRST_SURROGATE = 0xD800;
constexpr char32_t LAST_SURROGATE = 0xDFFF;
constexpr size_t MAX_UNICODE_ESCAPE_DIGITS = 6;

bool isDecimalDigit(char symbol)
{
    return symbol >= '0' && symbol <= '9';
}

bool isHexDigit(char symbol)
{
    return isDecimalDigit(symbol) || (symbol >= 'a' && symbol <= 'f') || (symbol >= 'A' && symbol <= 'F');
}

bool isBinaryDigit(char symbol)
{
    return symbol == '0' || symbol == '1';
}

std::string hexCode(char32_t codePoint)
{
    constexpr std::string_view DIGITS = "0123456789ABCDEF";
    std::string text;
    for (int shift = 20; shift >= 0; shift -= 4)
    {
        const auto digit = static_cast<size_t>((codePoint >> shift) & 0xF);
        if (digit != 0 || !text.empty() || shift <= 12)
        {
            text += DIGITS[digit];
        }
    }
    return text;
}

std::u32string decodeValid(std::string_view text)
{
    std::u32string result;
    size_t position = 0;
    while (position < text.size())
    {
        const unicode::Decoded decoded = unicode::decodeUtf8(text, position);
        result.push_back(decoded.codePoint);
        position += decoded.length;
    }
    return result;
}

bool isScriptNeutral(unicode::Script script)
{
    return script == unicode::Script::Inherited || script == unicode::Script::None;
}

int scriptOrder(unicode::Script script)
{
    switch (script)
    {
    case unicode::Script::Latin:
        return 0;
    case unicode::Script::Cyrillic:
        return 1;
    default:
        return 2;
    }
}

class Lexer
{
public:
    explicit Lexer(std::string_view text) : source(text)
    {
    }

    Lexed run()
    {
        reportInvalidUtf8();
        while (position < source.size())
        {
            start = position;
            scanToken();
        }
        start = position;
        add(SyntaxKind::EndOfFile);
        return Lexed{std::move(tokens), std::move(error)};
    }

private:
    std::string_view source;
    size_t position = 0;
    size_t start = 0;
    std::vector<Token> tokens;
    std::optional<Diagnostic> error;
    Diagnostic repeated;

    bool startsWith(std::string_view prefix, size_t offset) const
    {
        return source.substr(std::min(offset, source.size())).starts_with(prefix);
    }

    char at(size_t offset) const
    {
        return offset < source.size() ? source[offset] : '\0';
    }

    void add(SyntaxKind kind)
    {
        tokens.push_back(Token{kind, static_cast<uint32_t>(start), static_cast<uint32_t>(position - start)});
    }

    Diagnostic& report(Message message, size_t anchorStart, size_t anchorEnd)
    {
        if (error)
        {
            repeated = Diagnostic{};
            return repeated;
        }
        Diagnostic diagnostic;
        diagnostic.code = codeOf(message);
        diagnostic.message = message;
        diagnostic.offset = static_cast<uint32_t>(anchorStart);
        diagnostic.anchorLength = static_cast<uint32_t>(std::min(anchorEnd, source.size()) - anchorStart);
        error = std::move(diagnostic);
        return *error;
    }

    size_t characterEnd(size_t offset) const
    {
        if (offset >= source.size())
        {
            return source.size();
        }
        return offset + unicode::decodeUtf8(source, offset).length;
    }

    void reportInvalidUtf8()
    {
        size_t offset = 0;
        while (offset < source.size())
        {
            const unicode::Decoded decoded = unicode::decodeUtf8(source, offset);
            if (!decoded.valid)
            {
                report(Message::InvalidUtf8, offset, offset);
                return;
            }
            offset += decoded.length;
        }
    }

    bool continuesName(size_t offset) const
    {
        if (offset >= source.size())
        {
            return false;
        }
        const unicode::Decoded decoded = unicode::decodeUtf8(source, offset);
        return decoded.valid && unicode::isIdentifierContinue(decoded.codePoint);
    }

    void scanToken()
    {
        const char symbol = source[position];
        if (symbol == ' ' || symbol == '\t' || symbol == '\r')
        {
            while (position < source.size()
                && (source[position] == ' ' || source[position] == '\t' || source[position] == '\r'))
            {
                ++position;
            }
            add(SyntaxKind::Whitespace);
        }
        else if (symbol == '\n')
        {
            ++position;
            add(SyntaxKind::Newline);
        }
        else if (startsWith("//", position))
        {
            scanLineComment();
        }
        else if (startsWith("/*", position))
        {
            scanBlockComment();
        }
        else if (startsWith(RAW_STRING_PREFIX, position))
        {
            scanRawString();
        }
        else if (startsWith(BYTE_PREFIX, position))
        {
            position += BYTE_PREFIX.size() - 1;
            scanCharacter(SyntaxKind::Byte);
        }
        else if (symbol == '\'')
        {
            scanCharacter(SyntaxKind::Char);
        }
        else if (symbol == '"')
        {
            scanString();
        }
        else if (isDecimalDigit(symbol))
        {
            scanNumber();
        }
        else
        {
            const unicode::Decoded decoded = unicode::decodeUtf8(source, position);
            if (decoded.valid && unicode::isIdentifierStart(decoded.codePoint))
            {
                scanName();
            }
            else
            {
                scanPunctuation(decoded);
            }
        }
    }

    void scanLineComment()
    {
        const SyntaxKind kind = startsWith("///", position) ? SyntaxKind::DocComment
            : startsWith("//!", position)                   ? SyntaxKind::ModuleDocComment
                                                            : SyntaxKind::LineComment;
        while (position < source.size() && source[position] != '\n')
        {
            ++position;
        }
        add(kind);
    }

    void scanBlockComment()
    {
        size_t depth = 0;
        while (position < source.size())
        {
            if (startsWith("/*", position))
            {
                ++depth;
                position += 2;
            }
            else if (startsWith("*/", position))
            {
                --depth;
                position += 2;
                if (depth == 0)
                {
                    add(SyntaxKind::BlockComment);
                    return;
                }
            }
            else
            {
                ++position;
            }
        }
        report(Message::UnclosedBlockComment, start, start + 2);
        add(SyntaxKind::BlockComment);
    }

    void scanRawString()
    {
        for (;;)
        {
            size_t lineEnd = source.find('\n', position);
            lineEnd = lineEnd == std::string_view::npos ? source.size() : lineEnd;
            size_t next = lineEnd + 1;
            while (next < source.size() && (source[next] == ' ' || source[next] == '\t'))
            {
                ++next;
            }
            if (lineEnd < source.size() && startsWith(RAW_STRING_PREFIX, next))
            {
                position = next;
            }
            else
            {
                position = lineEnd;
                break;
            }
        }
        add(SyntaxKind::RawString);
    }

    std::optional<char32_t> scanEscape()
    {
        if (position + 1 >= source.size() || source[position + 1] == '\n')
        {
            report(Message::EscapeTruncated, position, position + 1);
            ++position;
            return std::nullopt;
        }
        const char kind = source[position + 1];
        const size_t escapeEnd = characterEnd(position + 1);
        switch (kind)
        {
        case 'n':
            position += 2;
            return U'\n';
        case 't':
            position += 2;
            return U'\t';
        case 'r':
            position += 2;
            return U'\r';
        case '0':
            position += 2;
            return U'\0';
        case '\\':
        case '"':
        case '\'':
            position += 2;
            return static_cast<char32_t>(kind);
        default:
            break;
        }
        if (kind == 'u' && at(position + 2) == '{')
        {
            size_t digitsEnd = position + 3;
            char32_t value = 0;
            while (digitsEnd < source.size() && isHexDigit(source[digitsEnd])
                && digitsEnd - (position + 3) <= MAX_UNICODE_ESCAPE_DIGITS)
            {
                const char digit = source[digitsEnd];
                value =
                    value * 16 + static_cast<char32_t>(isDecimalDigit(digit) ? digit - '0' : (digit | 0x20) - 'a' + 10);
                ++digitsEnd;
            }
            const size_t digitCount = digitsEnd - (position + 3);
            if (digitCount < 1 || digitCount > MAX_UNICODE_ESCAPE_DIGITS || at(digitsEnd) != '}')
            {
                report(Message::EscapeUnicodeDigits, position, position + 2);
                position += 2;
                return std::nullopt;
            }
            if (value > LAST_SCALAR || (value >= FIRST_SURROGATE && value <= LAST_SURROGATE))
            {
                report(Message::EscapeNotScalar, position, position + 2);
                position = digitsEnd + 1;
                return std::nullopt;
            }
            position = digitsEnd + 1;
            return value;
        }
        report(Message::EscapeUnknown, position, escapeEnd);
        position = escapeEnd;
        return std::nullopt;
    }

    void scanCharacter(SyntaxKind kind)
    {
        const size_t quote = position;
        ++position;
        std::optional<char32_t> value;
        bool malformed = false;
        if (position < source.size() && source[position] == '\\')
        {
            value = scanEscape();
            malformed = !value.has_value();
        }
        else if (position < source.size() && source[position] != '\'' && source[position] != '\n')
        {
            const unicode::Decoded decoded = unicode::decodeUtf8(source, position);
            value = decoded.codePoint;
            position += decoded.length;
        }
        else
        {
            report(Message::LiteralEmpty, start, position).token = kind;
            malformed = true;
        }
        if (!malformed && (position >= source.size() || source[position] != '\''))
        {
            report(Message::LiteralUnclosed, start, quote + 1).token = kind;
            malformed = true;
        }
        if (malformed)
        {
            while (position < source.size() && source[position] != '\'' && source[position] != '\n')
            {
                ++position;
            }
            position += position < source.size() && source[position] == '\'' ? 1 : 0;
            add(SyntaxKind::Error);
            return;
        }
        ++position;
        if (kind == SyntaxKind::Byte && *value > LAST_ASCII)
        {
            report(Message::ByteNotAscii, start, position);
        }
        add(kind);
    }

    void scanString()
    {
        ++position;
        while (position < source.size() && source[position] != '"' && source[position] != '\n')
        {
            if (source[position] == '\\')
            {
                scanEscape();
            }
            else
            {
                ++position;
            }
        }
        if (position >= source.size() || source[position] == '\n')
        {
            report(Message::StringUnclosed, start, start + 1);
            add(SyntaxKind::Error);
            return;
        }
        ++position;
        add(SyntaxKind::String);
    }

    bool scanDigits(bool (*allowed)(char))
    {
        const size_t begin = position;
        bool valid = true;
        while (position < source.size() && (allowed(source[position]) || source[position] == '_'))
        {
            const bool underscore = source[position] == '_';
            if (underscore && (position == begin || at(position + 1) == '_'))
            {
                valid = false;
            }
            ++position;
        }
        return valid && position > begin && source[position - 1] != '_';
    }

    void failNumber(Message message)
    {
        size_t end = position;
        while (continuesName(end))
        {
            end = characterEnd(end);
        }
        report(message, start, end);
        position = end;
        add(SyntaxKind::Error);
    }

    void scanNumber()
    {
        SyntaxKind kind = SyntaxKind::Integer;
        if (startsWith("0x", position))
        {
            position += 2;
            if (!scanDigits(isHexDigit))
            {
                failNumber(Message::NumberMalformed);
                return;
            }
        }
        else if (startsWith("0b", position))
        {
            position += 2;
            if (!scanDigits(isBinaryDigit))
            {
                failNumber(Message::NumberMalformed);
                return;
            }
        }
        else
        {
            if (!scanDigits(isDecimalDigit))
            {
                failNumber(Message::NumberMalformed);
                return;
            }
            if (at(position) == '.' && isDecimalDigit(at(position + 1)))
            {
                ++position;
                kind = SyntaxKind::Float;
                if (!scanDigits(isDecimalDigit))
                {
                    failNumber(Message::NumberMalformed);
                    return;
                }
            }
            if (at(position) == 'e' || at(position) == 'E')
            {
                const size_t shift = at(position + 1) == '+' || at(position + 1) == '-' ? 2 : 1;
                if (isDecimalDigit(at(position + shift)))
                {
                    position += shift;
                    kind = SyntaxKind::Float;
                    if (!scanDigits(isDecimalDigit))
                    {
                        failNumber(Message::NumberMalformed);
                        return;
                    }
                }
            }
        }
        if (continuesName(position))
        {
            failNumber(Message::NumberLetterAfter);
            return;
        }
        add(kind);
    }

    void reportMixedScripts(const std::u32string& segment, size_t nameStart, size_t nameEnd)
    {
        std::array<size_t, 3> counts = {0, 0, 0};
        for (const char32_t codePoint : segment)
        {
            const unicode::Script script = unicode::identifierScript(codePoint);
            if (!isScriptNeutral(script))
            {
                ++counts[static_cast<size_t>(scriptOrder(script))];
            }
        }
        constexpr std::array ORDERED = {unicode::Script::Latin, unicode::Script::Cyrillic, unicode::Script::Other};
        size_t rare = ORDERED.size();
        for (size_t index = 0; index < ORDERED.size(); ++index)
        {
            if (counts[index] != 0 && (rare == ORDERED.size() || counts[index] < counts[rare]))
            {
                rare = index;
            }
        }
        size_t common = ORDERED.size();
        for (size_t index = 0; index < ORDERED.size(); ++index)
        {
            if (index != rare && counts[index] != 0 && (common == ORDERED.size() || counts[index] > counts[common]))
            {
                common = index;
            }
        }
        std::u32string foreign;
        std::string list;
        for (const char32_t codePoint : segment)
        {
            const unicode::Script script = unicode::identifierScript(codePoint);
            if (!isScriptNeutral(script) && static_cast<size_t>(scriptOrder(script)) == rare
                && foreign.find(codePoint) == std::u32string::npos)
            {
                foreign.push_back(codePoint);
                list += list.empty() ? "`" : ", `";
                unicode::appendUtf8(list, codePoint);
                list += "`";
            }
        }
        Diagnostic& diagnostic = report(Message::MixedScripts, nameStart, nameEnd);
        diagnostic.text = unicode::encodeUtf8(segment);
        diagnostic.list = std::move(list);
        diagnostic.rare = ORDERED[rare];
        diagnostic.common = ORDERED[common];
    }

    void scanName()
    {
        while (continuesName(position))
        {
            position = characterEnd(position);
        }
        const std::u32string name = unicode::toNfc(decodeValid(source.substr(start, position - start)));
        if (name == U"_")
        {
            add(SyntaxKind::Underscore);
            return;
        }
        if (const std::optional<SyntaxKind> keyword = keywordKind(unicode::encodeUtf8(name)))
        {
            SyntaxKind kind = *keyword;
            if ((kind == SyntaxKind::KwBitOr || kind == SyntaxKind::KwBitXor) && at(position) == '='
                && at(position + 1) != '=')
            {
                kind = kind == SyntaxKind::KwBitOr ? SyntaxKind::BitOrEq : SyntaxKind::BitXorEq;
                ++position;
            }
            add(kind);
            return;
        }
        for (const std::u32string& segment : nameSegments(name))
        {
            std::array<bool, 3> seen = {false, false, false};
            for (const char32_t codePoint : segment)
            {
                const unicode::Script script = unicode::identifierScript(codePoint);
                if (!isScriptNeutral(script))
                {
                    seen[static_cast<size_t>(scriptOrder(script))] = true;
                }
            }
            if (std::count(seen.begin(), seen.end(), true) > 1)
            {
                reportMixedScripts(segment, start, position);
                add(SyntaxKind::Error);
                return;
            }
        }
        add(SyntaxKind::Identifier);
    }

    void scanPunctuation(const unicode::Decoded& decoded)
    {
        for (const SyntaxKind kind : PUNCTUATION_BY_LENGTH)
        {
            const std::string_view text = fixedText(kind);
            if (startsWith(text, position))
            {
                position += text.size();
                add(kind);
                return;
            }
        }
        if (decoded.valid)
        {
            const Message message = decoded.codePoint == U';' ? Message::UnknownSemicolon : Message::UnknownCharacter;
            report(message, position, position + decoded.length).text = hexCode(decoded.codePoint);
        }
        position += decoded.length;
        add(SyntaxKind::Error);
    }
};

}

Lexed lex(std::string_view source)
{
    return Lexer(source).run();
}

std::string normalizedName(std::string_view lexeme)
{
    return unicode::encodeUtf8(unicode::toNfc(decodeValid(lexeme)));
}

std::vector<std::u32string> nameSegments(std::u32string_view name)
{
    std::vector<std::u32string> segments;
    size_t pieceStart = 0;
    const auto splitPiece = [&](size_t begin, size_t end)
    {
        if (begin == end)
        {
            return;
        }
        size_t segmentStart = begin;
        for (size_t index = begin + 1; index < end; ++index)
        {
            const char32_t previous = name[index - 1];
            const char32_t current = name[index];
            const bool nextIsLower = index + 1 < end && unicode::isLowercase(name[index + 1]);
            const bool lowerToUpper = unicode::isLowercase(previous) && unicode::isUppercase(current);
            const bool acronymEnd = unicode::isUppercase(previous) && unicode::isUppercase(current) && nextIsLower;
            if (lowerToUpper || acronymEnd)
            {
                segments.emplace_back(name.substr(segmentStart, index - segmentStart));
                segmentStart = index;
            }
        }
        segments.emplace_back(name.substr(segmentStart, end - segmentStart));
    };
    for (size_t index = 0; index <= name.size(); ++index)
    {
        const bool separator =
            index == name.size() || name[index] == U'_' || (name[index] >= U'0' && name[index] <= U'9');
        if (separator)
        {
            splitPiece(pieceStart, index);
            pieceStart = index + 1;
        }
    }
    return segments;
}

}
