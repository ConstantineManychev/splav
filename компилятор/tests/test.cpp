#include "tests/test.hpp"

#include "support/file_io.hpp"

#include <algorithm>
#include <utility>

namespace splav::test
{

namespace
{

struct Case
{
    std::string_view name;
    Body body;
};

std::vector<Case>& registry()
{
    static std::vector<Case> cases;
    return cases;
}

bool selected(std::string_view name, const std::vector<std::string>& filters)
{
    if (filters.empty())
    {
        return true;
    }
    return std::any_of(filters.begin(), filters.end(),
        [name](const std::string& filter) { return name.find(filter) != std::string_view::npos; });
}

}

Context::Context(std::filesystem::path root) : repositoryRoot(std::move(root))
{
}

const std::filesystem::path& Context::root() const
{
    return repositoryRoot;
}

const std::vector<Failure>& Context::failures() const
{
    return recorded;
}

void Context::fail(std::string message, std::source_location location)
{
    recorded.push_back(Failure{std::move(message), location.file_name(), location.line()});
}

void Context::expect(bool condition, std::string_view what, std::source_location location)
{
    if (!condition)
    {
        fail(std::string(what), location);
    }
}

int registerCase(std::string_view name, Body body)
{
    registry().push_back(Case{name, body});
    return static_cast<int>(registry().size());
}

int runAll(const std::filesystem::path& repositoryRoot, const std::vector<std::string>& filters)
{
    size_t total = 0;
    size_t passed = 0;
    for (const Case& testCase : registry())
    {
        if (!selected(testCase.name, filters))
        {
            continue;
        }
        ++total;
        Context context(repositoryRoot);
        testCase.body(context);
        std::string report = context.failures().empty() ? "✓ " : "✗ ";
        report += testCase.name;
        report += "\n";
        for (const Failure& failure : context.failures())
        {
            report += "    " + failure.file + ":" + std::to_string(failure.line) + ": " + failure.message + "\n";
        }
        writeStandardOutput(report);
        passed += context.failures().empty() ? 1 : 0;
    }
    writeStandardOutput("итого: " + std::to_string(passed) + " из " + std::to_string(total) + "\n");
    return passed == total && total > 0 ? 0 : 1;
}

}
