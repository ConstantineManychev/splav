#pragma once

#include "syntax/diagnostic.hpp"
#include "syntax/green.hpp"

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace splav
{

enum class ParseMode : uint8_t
{
    Module,
    Fragment,
    Manifest,
};

struct Parse
{
    std::shared_ptr<const GreenNode> root;
    std::vector<Diagnostic> diagnostics;
};

Parse parse(std::string_view source, ParseMode mode);

}
