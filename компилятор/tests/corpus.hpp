#pragma once

#include "syntax/parser.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace splav::test
{

struct CorpusFile
{
    std::string name;
    std::string source;
    ParseMode mode;
};

std::vector<CorpusFile> corpusFiles(const std::filesystem::path& repositoryRoot);

}
