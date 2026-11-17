#pragma once

#include "sema/syntax_view.hpp"
#include "sema/types.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace splav
{

struct PatternConstructor
{
    std::string key;
    std::string display;
    std::vector<std::string> labels;
    std::vector<TypePtr> types;
};

using ConstructorsOf = std::function<std::optional<std::vector<PatternConstructor>>(const TypePtr&)>;

struct ArmPattern
{
    SyntaxNodeView pattern;
    bool guarded = false;
};

struct Coverage
{
    std::optional<std::string> missing;
    std::vector<SyntaxNodeView> unreachable;
};

Coverage checkCoverage(
    const std::vector<ArmPattern>& arms, const TypePtr& scrutinee, const ConstructorsOf& constructorsOf);

}
