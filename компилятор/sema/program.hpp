#pragma once

#include "syntax/green.hpp"
#include "syntax/parser.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace splav
{

enum class ModuleKind
{
    Source,
    CHeader,
};

struct Module
{
    std::string name;
    ModuleKind kind = ModuleKind::Source;
    std::filesystem::path path;
    std::string source;
    std::shared_ptr<const GreenNode> root;
    std::vector<Diagnostic> syntax;
    bool library = false;
};

class Program
{
public:
    Program(std::filesystem::path packageRoot, std::optional<std::filesystem::path> libraryRoot);

    Module* loadEntry(const std::filesystem::path& file);
    Module* loadSource(const std::string& source, const std::string& name);
    Module* find(const std::string& name);
    Module* header(const std::string& file);
    std::optional<std::filesystem::path> locate(const std::string& name) const;
    Module* prelude();

    const std::vector<std::unique_ptr<Module>>& modules() const;

private:
    Module* store(std::unique_ptr<Module> module);
    Module* read(const std::string& name, const std::filesystem::path& path, bool library);

    std::filesystem::path packageRoot;
    std::optional<std::filesystem::path> libraryRoot;
    std::vector<std::unique_ptr<Module>> loaded;
    std::map<std::string, Module*> byName;
    std::map<std::string, Module*> headers;
};

std::filesystem::path findPackageRoot(const std::filesystem::path& file);
std::optional<std::filesystem::path> findLibraryRoot(const std::filesystem::path& start);
std::string moduleNameFor(const std::filesystem::path& packageRoot, const std::filesystem::path& file);
std::string modulePathText(const std::string& name);

}
