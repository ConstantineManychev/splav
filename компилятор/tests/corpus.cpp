#include "tests/corpus.hpp"

#include "support/file_io.hpp"
#include "support/utf8_path.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <string_view>

namespace splav::test
{

namespace
{

constexpr std::array<std::string_view, 10> CORPUS_DIRECTORIES = {
    "корпус/json",
    "корпус/эхо",
    "корпус/частицы",
    "корпус/ecs",
    "корпус/сжать",
    "корпус/злые",
    "корпус/злые-сочетания",
    "библиотека/ядро",
    "библиотека/сеть",
    "модель/заголовки",
};
constexpr std::string_view SOURCE_EXTENSION = ".спл";
constexpr std::string_view MANIFEST_NAME = "пакет.спл";

}

std::vector<CorpusFile> corpusFiles(const std::filesystem::path& repositoryRoot)
{
    std::vector<CorpusFile> files;
    for (const std::string_view directory : CORPUS_DIRECTORIES)
    {
        std::vector<std::filesystem::path> paths;
        std::error_code failure;
        for (const auto& entry : std::filesystem::directory_iterator(repositoryRoot / pathFromUtf8(directory), failure))
        {
            if (utf8FromPath(entry.path().extension()) == SOURCE_EXTENSION)
            {
                paths.push_back(entry.path());
            }
        }
        std::sort(paths.begin(), paths.end());
        for (const std::filesystem::path& path : paths)
        {
            const std::string fileName = utf8FromPath(path.filename());
            std::optional<std::string> source = readFile(path);
            if (source)
            {
                const ParseMode mode = fileName == MANIFEST_NAME ? ParseMode::Manifest : ParseMode::Module;
                files.push_back(CorpusFile{std::string(directory) + "/" + fileName, std::move(*source), mode});
            }
        }
    }
    return files;
}

}
