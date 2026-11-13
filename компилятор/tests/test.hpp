#pragma once

#include <filesystem>
#include <source_location>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace splav::test
{

struct Failure
{
    std::string message;
    std::string file;
    unsigned line;
};

class Context
{
public:
    explicit Context(std::filesystem::path repositoryRoot);

    const std::filesystem::path& root() const;
    const std::vector<Failure>& failures() const;

    void fail(std::string message, std::source_location location = std::source_location::current());
    void skip(std::string reason);
    const std::string& skipReason() const;

    void expect(bool condition, std::string_view what, std::source_location location = std::source_location::current());

    template <typename Actual, typename Expected>
    void expectEqual(const Actual& actual, const Expected& expected, std::string_view what,
        std::source_location location = std::source_location::current())
    {
        if (actual == expected)
        {
            return;
        }
        std::ostringstream message;
        message << what << ": получено «" << actual << "», ожидалось «" << expected << "»";
        fail(message.str(), location);
    }

private:
    std::filesystem::path repositoryRoot;
    std::vector<Failure> recorded;
    std::string skipped;
};

using Body = void (*)(Context&);

int registerCase(std::string_view name, Body body);
int runAll(const std::filesystem::path& repositoryRoot, const std::vector<std::string>& filters);

}

#define SPLAV_TEST(identifier, name)                                                                                   \
    static void identifier(splav::test::Context& context);                                                             \
    [[maybe_unused]] static const int identifier##Registered = splav::test::registerCase(name, identifier);            \
    static void identifier([[maybe_unused]] splav::test::Context& context)
