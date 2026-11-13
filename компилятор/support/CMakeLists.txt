add_library(splav_support STATIC
    file_io.cpp
    platform.cpp
    utf8_path.cpp
)
target_include_directories(splav_support PUBLIC "${PROJECT_SOURCE_DIR}/компилятор")
target_link_libraries(splav_support PUBLIC splav_flags)
if(WIN32)
    target_link_libraries(splav_support PRIVATE shell32)
endif()
