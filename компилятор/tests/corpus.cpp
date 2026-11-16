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

struct CorpusDirectory
{
    std::string_view name;
    bool holdsRejectedSources = false;
};

constexpr std::array CORPUS_DIRECTORIES = {
    CorpusDirectory{"корпус/json"},
    CorpusDirectory{"корпус/эхо"},
    CorpusDirectory{"корпус/частицы"},
    CorpusDirectory{"корпус/ecs"},
    CorpusDirectory{"корпус/сжать"},
    CorpusDirectory{"корпус/злые"},
    CorpusDirectory{"корпус/злые-сочетания"},
    CorpusDirectory{"корпус/злые-синтаксис", true},
    CorpusDirectory{"корпус/злые-имена"},
    CorpusDirectory{"корпус/злые-типы"},
    CorpusDirectory{"библиотека/ядро"},
    CorpusDirectory{"библиотека/сеть"},
    CorpusDirectory{"модель/заголовки"},
};
constexpr std::string_view SOURCE_EXTENSION = ".спл";
constexpr std::string_view FIXED_TWIN_SUFFIX = ".верно.спл";
constexpr std::string_view DIAGNOSTICS_EXTENSION = ".диаг";
constexpr std::string_view MANIFEST_NAME = "пакет.спл";

std::optional<std::string> expectedDiagnosticFor(const std::filesystem::path& source)
{
    std::filesystem::path diagnostics = source;
    diagnostics.replace_extension(pathFromUtf8(DIAGNOSTICS_EXTENSION));
    std::optional<std::string> text = readFile(diagnostics);
    if (!text)
    {
        return std::string();
    }
    while (!text->empty() && (text->back() == '\n' || text->back() == '\r'))
    {
        text->pop_back();
    }
    return text;
}

}

std::vector<CorpusFile> corpusFiles(const std::filesystem::path& repositoryRoot)
{
    std::vector<CorpusFile> files;
    for (const CorpusDirectory& directory : CORPUS_DIRECTORIES)
    {
        std::vector<std::filesystem::path> paths;
        std::error_code failure;
        for (const auto& entry :
            std::filesystem::directory_iterator(repositoryRoot / pathFromUtf8(directory.name), failure))
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
                const bool rejected = directory.holdsRejectedSources && !fileName.ends_with(FIXED_TWIN_SUFFIX);
                files.push_back(CorpusFile{std::string(directory.name) + "/" + fileName, std::move(*source), mode,
                    rejected ? expectedDiagnosticFor(path) : std::nullopt});
            }
        }
    }
    return files;
}

std::vector<CorpusFile> wellFormedCorpusFiles(const std::filesystem::path& repositoryRoot)
{
    std::vector<CorpusFile> files = corpusFiles(repositoryRoot);
    std::erase_if(files, [](const CorpusFile& file) { return file.expectedDiagnostic.has_value(); });
    return files;
}

}
