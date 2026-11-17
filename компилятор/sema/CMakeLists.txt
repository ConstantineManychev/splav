add_library(splav_sema STATIC
    checker.cpp
    coverage.cpp
    declarations.cpp
    memory/ast.cpp
    memory/dump.cpp
    names.cpp
    naming.cpp
    program.cpp
    report.cpp
    syntax_view.cpp
    types.cpp
)
target_link_libraries(splav_sema PUBLIC splav_support splav_syntax splav_unicode)
