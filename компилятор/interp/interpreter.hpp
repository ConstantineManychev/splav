#pragma once

#include "sema/program.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace splav
{

constexpr int PANIC_EXIT_CODE = 101;
constexpr int FAILURE_EXIT_CODE = 1;

struct RunOptions
{
    std::vector<std::string> arguments;
    std::function<void(bool error, std::string_view text)> write;
};

struct TestOutcome
{
    std::string name;
    bool passed = false;
    bool skipped = false;
    std::string message;
};

int runProgram(Program& program, Module& entry, const RunOptions& options);
std::vector<TestOutcome> runTests(Program& program, Module& module, const RunOptions& options);

}
