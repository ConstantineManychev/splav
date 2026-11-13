#pragma once

#include <string>
#include <vector>

namespace splav
{

std::vector<std::string> utf8Arguments(int argc, char** argv);
void prepareStandardStreams();

}
