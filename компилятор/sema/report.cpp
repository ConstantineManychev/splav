#include "sema/report.hpp"

#include <algorithm>

namespace splav
{

namespace
{

std::string placeWithKind(std::string_view source, uint32_t offset, uint32_t length, std::string_view kind)
{
    const Position position = positionAt(source, offset);
    std::string_view anchor = source.substr(std::min<size_t>(offset, source.size()), length);
    anchor = anchor.substr(0, anchor.find('\n'));
    std::string text = std::to_string(position.line) + ":" + std::to_string(position.column) + " ";
    text += kind;
    return anchor.empty() ? text : text + " «" + std::string(anchor) + "»";
}

}

std::string diagnosticCode(uint16_t code)
{
    const std::string digits = std::to_string(code);
    return "С" + std::string(4 - std::min<size_t>(4, digits.size()), '0') + digits;
}

std::string renderReport(const Report& report, std::string_view source, Language language)
{
    const bool russian = language == Language::Russian;
    std::string kind = (russian ? "ошибка " : "error ") + diagnosticCode(report.code);
    std::string text = placeWithKind(source, report.offset, report.length, kind) + ": " + report.message;
    for (const ReportNote& note : report.notes)
    {
        text += "\n  " + placeWithKind(source, note.offset, note.length, russian ? "примечание" : "note") + ": "
            + note.text;
    }
    return text;
}

}
