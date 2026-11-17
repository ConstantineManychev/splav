add_library(splav_sema STATIC
    names.cpp
    program.cpp
    report.cpp
    syntax_view.cpp
)
target_link_libraries(splav_sema PUBLIC splav_support splav_syntax splav_unicode)
