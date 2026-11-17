#pragma once

#include "sema/program.hpp"
#include "sema/report.hpp"

#include <vector>

namespace splav
{

std::vector<Report> resolveNames(Program& program, Module& module, Language language);

}
