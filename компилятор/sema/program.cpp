#include "sema/program.hpp"

#include "support/file_io.hpp"
#include "support/utf8_path.hpp"

#include <system_error>

namespace splav
{

namespace
{

constexpr std::string_view SOURCE_EXTENSION = ".спл";
constexpr std::string_view MANIFEST_NAME = "пакет.спл";
constexpr std::string_view LIBRARY_DIRECTORY = "библиотека";
constexpr std::string_view PRELUDE_MODULE = "ядро.прелюдия";

bool isFile(const std::filesystem::path& path)
{
    std::error_code failure;
    return std::filesystem::is_regular_file(path, failure);
}

std::filesystem::path pathOfModule(const std::filesystem::path& root, const std::string& name)
{
    std::filesystem::path result = root;
    size_t start = 0;
    while (true)
    {
        const size_t dot = name.find('.', start);
        const std::string part = name.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (dot == std::string::npos)
        {
            result /= pathFromUtf8(part + std::string(SOURCE_EXTENSION));
            return result;
        }
        result /= pathFromUtf8(part);
        start = dot + 1;
    }
}

}

std::string modulePathText(const std::string& name)
{
    std::string text = name;
    for (char& symbol : text)
    {
        symbol = symbol == '.' ? '/' : symbol;
    }
    return text + std::string(SOURCE_EXTENSION);
}

std::filesystem::path findPackageRoot(const std::filesystem::path& file)
{
    const std::filesystem::path directory = std::filesystem::absolute(file).parent_path();
    for (std::filesystem::path probe = directory; !probe.empty(); probe = probe.parent_path())
    {
        if (isFile(probe / pathFromUtf8(MANIFEST_NAME)))
        {
            return probe;
        }
        if (probe == probe.parent_path())
        {
            break;
        }
    }
    return directory;
}

std::optional<std::filesystem::path> findLibraryRoot(const std::filesystem::path& start)
{
    for (std::filesystem::path probe = std::filesystem::absolute(start); !probe.empty(); probe = probe.parent_path())
    {
        const std::filesystem::path candidate = probe / pathFromUtf8(LIBRARY_DIRECTORY);
        if (isFile(pathOfModule(candidate, std::string(PRELUDE_MODULE))))
        {
            return candidate;
        }
        if (probe == probe.parent_path())
        {
            break;
        }
    }
    return std::nullopt;
}

std::string moduleNameFor(const std::filesystem::path& packageRoot, const std::filesystem::path& file)
{
    std::filesystem::path relative = std::filesystem::absolute(file).lexically_relative(packageRoot);
    relative.replace_extension();
    std::string name;
    for (const std::filesystem::path& part : relative)
    {
        name += (name.empty() ? "" : ".") + utf8FromPath(part);
    }
    return name;
}

Program::Program(std::filesystem::path packageRootPath, std::optional<std::filesystem::path> libraryRootPath)
    : packageRoot(std::filesystem::absolute(packageRootPath).lexically_normal())
{
    if (libraryRootPath)
    {
        libraryRoot = std::filesystem::absolute(*libraryRootPath).lexically_normal();
    }
}

Module* Program::store(std::unique_ptr<Module> module)
{
    Module* stored = module.get();
    loaded.push_back(std::move(module));
    if (stored->kind == ModuleKind::Source)
    {
        byName[stored->name] = stored;
    }
    return stored;
}

Module* Program::read(const std::string& name, const std::filesystem::path& path, bool library)
{
    std::optional<std::string> text = readFile(path);
    if (!text)
    {
        return nullptr;
    }
    auto module = std::make_unique<Module>();
    module->name = name;
    module->path = path;
    module->source = std::move(*text);
    module->library = library;
    const Parse parsed = parse(module->source, ParseMode::Module);
    module->root = parsed.root;
    module->syntax = parsed.diagnostics;
    return store(std::move(module));
}

Module* Program::loadEntry(const std::filesystem::path& file)
{
    const std::filesystem::path absolute = std::filesystem::absolute(file).lexically_normal();
    const bool library = libraryRoot && !absolute.lexically_relative(*libraryRoot).empty()
        && *absolute.lexically_relative(*libraryRoot).begin() != "..";
    const std::string name = moduleNameFor(library ? *libraryRoot : packageRoot, absolute);
    if (const auto existing = byName.find(name); existing != byName.end())
    {
        return existing->second;
    }
    return read(name, file, library);
}

Module* Program::loadSource(const std::string& source, const std::string& name)
{
    auto module = std::make_unique<Module>();
    module->name = name;
    module->source = source;
    const Parse parsed = parse(module->source, ParseMode::Module);
    module->root = parsed.root;
    module->syntax = parsed.diagnostics;
    return store(std::move(module));
}

std::optional<std::filesystem::path> Program::locate(const std::string& name) const
{
    const std::filesystem::path local = pathOfModule(packageRoot, name);
    if (isFile(local))
    {
        return local;
    }
    if (libraryRoot)
    {
        const std::filesystem::path library = pathOfModule(*libraryRoot, name);
        if (isFile(library))
        {
            return library;
        }
    }
    return std::nullopt;
}

Module* Program::find(const std::string& name)
{
    if (const auto found = byName.find(name); found != byName.end())
    {
        return found->second;
    }
    const std::optional<std::filesystem::path> path = locate(name);
    if (!path)
    {
        return nullptr;
    }
    const bool library = !isFile(pathOfModule(packageRoot, name));
    return read(name, *path, library);
}

Module* Program::header(const std::string& file)
{
    if (const auto found = headers.find(file); found != headers.end())
    {
        return found->second;
    }
    auto module = std::make_unique<Module>();
    module->name = file;
    module->kind = ModuleKind::CHeader;
    Module* stored = store(std::move(module));
    headers[file] = stored;
    return stored;
}

Module* Program::prelude()
{
    return find(std::string(PRELUDE_MODULE));
}

const std::vector<std::unique_ptr<Module>>& Program::modules() const
{
    return loaded;
}

}
