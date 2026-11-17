#pragma once

#include "sema/comptime.hpp"
#include "sema/program.hpp"
#include "sema/report.hpp"

#include <vector>

namespace splav
{

std::vector<Report> checkTypes(
    Program& program, Module& module, Language language, const EvaluatorFactory& evaluators = {});

}
