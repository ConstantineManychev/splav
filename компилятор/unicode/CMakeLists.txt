add_library(splav_unicode STATIC
    unicode.cpp
)
target_include_directories(splav_unicode PUBLIC "${PROJECT_SOURCE_DIR}/компилятор")
target_link_libraries(splav_unicode PUBLIC splav_flags)
