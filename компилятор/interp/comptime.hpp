#pragma once

#include "sema/comptime.hpp"

#include <memory>

namespace splav
{

std::unique_ptr<ComptimeEvaluator> makeComptimeEvaluator(Program& program, Module& module, Declarations& declarations);

}
