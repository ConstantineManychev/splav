#include "syntax/parser.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
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
    }
    return 0;
}
