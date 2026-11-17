#pragma once

#include "syntax/diagnostic.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace splav
{

struct ReportNote
{
    uint32_t offset;
    uint32_t length;
    std::string text;
};

struct Report
{
    uint16_t code;
    uint32_t offset;
    uint32_t length;
    std::string message;
    std::vector<ReportNote> notes;
};

std::string renderReport(const Report& report, std::string_view source, Language language);
std::string diagnosticCode(uint16_t code);

}
