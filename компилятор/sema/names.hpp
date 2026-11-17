#pragma once

#include "sema/comptime.hpp"
#include "sema/program.hpp"
#include "sema/report.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace splav
{

struct Respelling
{
    uint32_t offset;
    uint32_t length;
    std::string text;
};

std::vector<Report> resolveNames(
    Program& program, Module& module, Language language, const EvaluatorFactory& evaluators = {});
std::vector<Respelling> canonicalSpellings(Program& program, Module& module);

}
