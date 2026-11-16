add_library(splav_syntax STATIC
    diagnostic.cpp
    green.cpp
    lexer.cpp
    parser.cpp
    syntax_kind.cpp
)
target_link_libraries(splav_syntax PUBLIC splav_unicode)
