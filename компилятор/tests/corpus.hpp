#pragma once

#include "syntax/parser.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace splav::test
{

struct CorpusFile
{
    std::string name;
    std::string source;
    ParseMode mode;
    std::optional<std::string> expectedDiagnostic;
};

std::vector<CorpusFile> corpusFiles(const std::filesystem::path& repositoryRoot);
std::vector<CorpusFile> wellFormedCorpusFiles(const std::filesystem::path& repositoryRoot);

}
