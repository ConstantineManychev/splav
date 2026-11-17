#include "syntax/diagnostic.hpp"

#include <algorithm>

namespace splav
{

namespace
{

std::string codeText(uint16_t code)
{
    std::string digits = std::to_string(code);
    return "С" + std::string(4 - std::min<size_t>(4, digits.size()), '0') + digits;
}

std::string_view scriptName(unicode::Script script, Language language)
{
    const bool russian = language == Language::Russian;
    switch (script)
    {
    case unicode::Script::Latin:
        return russian ? "латиница" : "Latin";
    case unicode::Script::Cyrillic:
        return russian ? "кириллица" : "Cyrillic";
    default:
        return russian ? "другая письменность" : "another script";
    }
}

std::string_view scriptNameAmong(unicode::Script script, Language language)
{
    const bool russian = language == Language::Russian;
    switch (script)
    {
    case unicode::Script::Latin:
        return russian ? "латиницы" : "Latin";
    case unicode::Script::Cyrillic:
        return russian ? "кириллицы" : "Cyrillic";
    default:
        return russian ? "другой письменности" : "another script";
    }
}

std::string_view literalName(SyntaxKind kind, Language language)
{
    const bool russian = language == Language::Russian;
    if (kind == SyntaxKind::Byte)
    {
        return russian ? "байтовый литерал" : "byte literal";
    }
    return russian ? "символьный литерал" : "character literal";
}

std::string tokenName(SyntaxKind kind, Language language)
{
    const bool russian = language == Language::Russian;
    switch (kind)
    {
    case SyntaxKind::Identifier:
        return russian ? "имя" : "a name";
    case SyntaxKind::Integer:
        return russian ? "целое число" : "an integer";
    case SyntaxKind::Float:
        return russian ? "вещественное число" : "a floating-point number";
    case SyntaxKind::Char:
        return russian ? "символьный литерал" : "a character literal";
    case SyntaxKind::Byte:
        return russian ? "байтовый литерал" : "a byte literal";
    case SyntaxKind::String:
        return russian ? "строковый литерал" : "a string literal";
    case SyntaxKind::EndOfFile:
        return russian ? "конец файла" : "end of file";
    default:
        return "«" + std::string(fixedText(kind)) + "»";
    }
}

std::string russianMessage(const Diagnostic& diagnostic)
{
    constexpr Language LANGUAGE = Language::Russian;
    switch (diagnostic.message)
    {
    case Message::InvalidUtf8:
        return "неверная последовательность UTF-8";
    case Message::UnknownCharacter:
        return "неизвестный символ U+" + diagnostic.text;
    case Message::UnknownSemicolon:
        return "неизвестный символ U+003B: точек с запятой в языке нет, инструкцию завершает перевод строки";
    case Message::SemicolonForColon:
        return "неизвестный символ U+003B: вероятно, `:` — в латинской раскладке `;` и `:` на одной клавише";
    case Message::NumeroSign:
        return "неизвестный символ U+2116: это Shift+3 русской раскладки на месте `#`, но и `#` в языке нет — "
               "атрибуты пишут в `[…]`, комментарии — `//`";
    case Message::ForeignLetter:
        return "неизвестный символ U+" + diagnostic.text
            + ": имя с буквами других письменностей заключают в обратные кавычки";
    case Message::MixedScripts:
        return "смешение письменностей в сегменте `" + diagnostic.text
            + "`: " + std::string(scriptName(diagnostic.rare, LANGUAGE)) + " " + diagnostic.list + " среди "
            + std::string(scriptNameAmong(diagnostic.common, LANGUAGE));
    case Message::UnclosedBlockComment:
        return "блочный комментарий не закрыт";
    case Message::NumberMalformed:
        return "неверная запись числа: `_` стоит только между цифрами, а цифры обязательны";
    case Message::NumberLetterAfter:
        return "буква сразу после числа";
    case Message::LiteralEmpty:
        return std::string(literalName(diagnostic.token, LANGUAGE)) + " пуст";
    case Message::LiteralUnclosed:
        return std::string(literalName(diagnostic.token, LANGUAGE)) + " не закрыт";
    case Message::ByteNotAscii:
        return "байтовый литерал вне ASCII";
    case Message::StringUnclosed:
        return "строковый литерал не закрыт до конца строки";
    case Message::EscapeTruncated:
        return "экранирование оборвано";
    case Message::EscapeUnicodeDigits:
        return "в `\\u{…}` нужно от 1 до 6 шестнадцатеричных цифр";
    case Message::EscapeNotScalar:
        return "`\\u{…}` задаёт не скалярное значение Юникода";
    case Message::EscapeUnknown:
        return "неизвестное экранирование";
    case Message::ExpectedToken:
        return "ожидалось " + tokenName(diagnostic.token, LANGUAGE);
    case Message::ExpectedLiteral:
        return "ожидался литерал";
    case Message::ExpectedDeclaration:
        return "ожидалось объявление";
    case Message::ExpectedExpression:
        return "ожидалось выражение";
    case Message::ExpectedRangeBound:
        return "ожидалась верхняя граница диапазона";
    case Message::ExpectedBlock:
        return "ожидалось «{»";
    case Message::ExpectedNewline:
        return "после инструкции ожидался перевод строки";
    case Message::NonAssociativeComparison:
        return "сравнения неассоциативны: расставьте скобки";
    case Message::KeywordAsName:
        return "ожидалось имя, а `" + std::string(fixedText(diagnostic.token)) + "` — ключевое слово";
    case Message::OnlyComptimeParams:
        return "перед `.` в имени метода стоят только `компил`-параметры типа";
    case Message::NestingTooDeep:
        return "слишком глубокая вложенность: больше " + std::to_string(MAX_NESTING) + " уровней";
    case Message::LiteralInHeader:
        return "литерал в заголовке `" + std::string(fixedText(diagnostic.token))
            + "` пишут в скобках, иначе `{` открывает блок";
    case Message::EscapedNameUnclosed:
        return "экранированное имя не закрыто до конца строки";
    case Message::EscapedNameEmpty:
        return "экранированное имя пусто";
    case Message::EscapedNameSpace:
        return "в экранированном имени нет пробелов и управляющих символов";
    }
    return {};
}

std::string englishMessage(const Diagnostic& diagnostic)
{
    constexpr Language LANGUAGE = Language::English;
    switch (diagnostic.message)
    {
    case Message::InvalidUtf8:
        return "invalid UTF-8 sequence";
    case Message::UnknownCharacter:
        return "unknown character U+" + diagnostic.text;
    case Message::UnknownSemicolon:
        return "unknown character U+003B: the language has no semicolons, a line break ends a statement";
    case Message::SemicolonForColon:
        return "unknown character U+003B: probably `:`, which shares the key with `;` in the Latin layout";
    case Message::NumeroSign:
        return "unknown character U+2116: Shift+3 of the Russian layout in place of `#`, but `#` is not used "
               "either: attributes go in `[…]`, comments start with `//`";
    case Message::ForeignLetter:
        return "unknown character U+" + diagnostic.text + ": a name with letters of other scripts goes in backticks";
    case Message::MixedScripts:
        return "mixed scripts in segment `" + diagnostic.text
            + "`: " + std::string(scriptName(diagnostic.rare, LANGUAGE)) + " " + diagnostic.list + " among "
            + std::string(scriptNameAmong(diagnostic.common, LANGUAGE));
    case Message::UnclosedBlockComment:
        return "block comment is not closed";
    case Message::NumberMalformed:
        return "malformed number: `_` goes only between digits, and digits are required";
    case Message::NumberLetterAfter:
        return "letter right after a number";
    case Message::LiteralEmpty:
        return std::string(literalName(diagnostic.token, LANGUAGE)) + " is empty";
    case Message::LiteralUnclosed:
        return std::string(literalName(diagnostic.token, LANGUAGE)) + " is not closed";
    case Message::ByteNotAscii:
        return "byte literal is outside ASCII";
    case Message::StringUnclosed:
        return "string literal is not closed before the end of the line";
    case Message::EscapeTruncated:
        return "escape sequence is cut off";
    case Message::EscapeUnicodeDigits:
        return "`\\u{…}` takes 1 to 6 hexadecimal digits";
    case Message::EscapeNotScalar:
        return "`\\u{…}` is not a Unicode scalar value";
    case Message::EscapeUnknown:
        return "unknown escape sequence";
    case Message::ExpectedToken:
        return "expected " + tokenName(diagnostic.token, LANGUAGE);
    case Message::ExpectedLiteral:
        return "expected a literal";
    case Message::ExpectedDeclaration:
        return "expected a declaration";
    case Message::ExpectedExpression:
        return "expected an expression";
    case Message::ExpectedRangeBound:
        return "expected the upper bound of the range";
    case Message::ExpectedBlock:
        return "expected «{»";
    case Message::ExpectedNewline:
        return "expected a line break after the statement";
    case Message::NonAssociativeComparison:
        return "comparisons are non-associative: add parentheses";
    case Message::KeywordAsName:
        return "expected a name, but `" + std::string(fixedText(diagnostic.token)) + "` is a keyword";
    case Message::OnlyComptimeParams:
        return "only `компил` parameters of the type may precede `.` in a method name";
    case Message::NestingTooDeep:
        return "nesting is too deep: more than " + std::to_string(MAX_NESTING) + " levels";
    case Message::LiteralInHeader:
        return "a literal in the header of `" + std::string(fixedText(diagnostic.token))
            + "` goes in parentheses, otherwise `{` opens the block";
    case Message::EscapedNameUnclosed:
        return "escaped name is not closed before the end of the line";
    case Message::EscapedNameEmpty:
        return "escaped name is empty";
    case Message::EscapedNameSpace:
        return "an escaped name has no spaces or control characters";
    }
    return {};
}

}

uint16_t codeOf(Message message)
{
    switch (message)
    {
    case Message::InvalidUtf8:
        return 101;
    case Message::UnknownCharacter:
    case Message::UnknownSemicolon:
    case Message::SemicolonForColon:
    case Message::NumeroSign:
    case Message::ForeignLetter:
        return 102;
    case Message::MixedScripts:
        return 103;
    case Message::UnclosedBlockComment:
        return 104;
    case Message::NumberMalformed:
    case Message::NumberLetterAfter:
        return 105;
    case Message::LiteralEmpty:
    case Message::LiteralUnclosed:
    case Message::ByteNotAscii:
        return 106;
    case Message::StringUnclosed:
        return 107;
    case Message::EscapeTruncated:
    case Message::EscapeUnicodeDigits:
    case Message::EscapeNotScalar:
    case Message::EscapeUnknown:
        return 108;
    case Message::ExpectedToken:
    case Message::ExpectedLiteral:
        return 109;
    case Message::ExpectedDeclaration:
        return 110;
    case Message::ExpectedExpression:
    case Message::ExpectedRangeBound:
        return 111;
    case Message::ExpectedBlock:
        return 112;
    case Message::ExpectedNewline:
        return 113;
    case Message::NonAssociativeComparison:
        return 114;
    case Message::KeywordAsName:
        return 115;
    case Message::OnlyComptimeParams:
        return 116;
    case Message::NestingTooDeep:
        return 117;
    case Message::LiteralInHeader:
        return 118;
    case Message::EscapedNameUnclosed:
    case Message::EscapedNameEmpty:
    case Message::EscapedNameSpace:
        return 119;
    }
    return 0;
}

Position positionAt(std::string_view source, uint32_t offset)
{
    const size_t limit = std::min<size_t>(offset, source.size());
    uint32_t line = 1;
    size_t lineStart = 0;
    for (size_t index = 0; index < limit; ++index)
    {
        if (source[index] == '\n')
        {
            ++line;
            lineStart = index + 1;
        }
    }
    uint32_t column = 1;
    for (size_t index = lineStart; index < limit; ++index)
    {
        if ((static_cast<unsigned char>(source[index]) & 0xC0) != 0x80)
        {
            ++column;
        }
    }
    return Position{line, column};
}

std::string renderMessage(const Diagnostic& diagnostic, Language language)
{
    const bool russian = language == Language::Russian;
    std::string message = russian ? russianMessage(diagnostic) : englishMessage(diagnostic);
    if (diagnostic.ending == Ending::Line)
    {
        message += russian ? ", а строка закончилась" : ", but the line ended";
    }
    else if (diagnostic.ending == Ending::File)
    {
        message += russian ? ", а файл закончился" : ", but the file ended";
    }
    return message;
}

std::string renderDiagnostic(const Diagnostic& diagnostic, std::string_view source, Language language)
{
    const Position position = positionAt(source, diagnostic.offset);
    std::string_view anchor =
        source.substr(std::min<size_t>(diagnostic.offset, source.size()), diagnostic.anchorLength);
    anchor = anchor.substr(0, anchor.find('\n'));
    std::string line = std::to_string(position.line) + ":" + std::to_string(position.column);
    line += language == Language::Russian ? " ошибка " : " error ";
    line += codeText(diagnostic.code);
    if (!anchor.empty())
    {
        line += " «" + std::string(anchor) + "»";
    }
    return line + ": " + renderMessage(diagnostic, language);
}

}
