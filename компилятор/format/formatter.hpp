#pragma once

#include "syntax/green.hpp"

#include <string>

namespace splav
{

constexpr int FORMAT_INDENT_WIDTH = 4;
constexpr int FORMAT_LINE_WIDTH = 100;

std::string format(const GreenNode& root);

}
