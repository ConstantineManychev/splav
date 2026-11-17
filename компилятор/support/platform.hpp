#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace splav
{

std::vector<std::string> utf8Arguments(int argc, char** argv);
void prepareStandardStreams();
void runWithStack(size_t bytes, const std::function<void()>& work);

}
