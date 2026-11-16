add_library(splav_format STATIC
    formatter.cpp
)
target_link_libraries(splav_format PUBLIC splav_syntax)
