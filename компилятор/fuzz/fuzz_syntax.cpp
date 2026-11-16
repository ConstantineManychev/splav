#include "format/formatter.hpp"
#include "syntax/parser.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    const std::string_view source(reinterpret_cast<const char*>(data), size);
    for (const splav::ParseMode mode :
        {splav::ParseMode::Module, splav::ParseMode::Fragment, splav::ParseMode::Manifest})
    {
        const splav::Parse parsed = splav::parse(source, mode);
        if (parsed.root->text() != source || parsed.diagnostics.size() > 1)
        {
            std::abort();
        }
        if (!parsed.diagnostics.empty())
        {
            continue;
        }
        const std::string formatted = splav::format(*parsed.root);
        const splav::Parse reparsed = splav::parse(formatted, mode);
        if (!reparsed.diagnostics.empty() || splav::format(*reparsed.root) != formatted)
        {
            std::abort();
        }
    }
    return 0;
}
