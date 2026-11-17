#pragma once

#include "sema/declarations.hpp"
#include "sema/types.hpp"

#include <functional>
#include <memory>
#include <vector>

namespace splav
{

class ComptimeEvaluator
{
public:
    virtual ~ComptimeEvaluator() = default;
    virtual TypePtr evaluateType(const FunctionInfo& function, const std::vector<TypePtr>& arguments) = 0;
};

using EvaluatorFactory = std::function<std::unique_ptr<ComptimeEvaluator>(Program&, Module&, Declarations&)>;

}
