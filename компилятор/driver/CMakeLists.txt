add_library(splav_driver STATIC
    driver.cpp
)
target_compile_definitions(splav_driver PRIVATE SPLAV_VERSION="${SPLAV_VERSION}")
target_link_libraries(splav_driver PUBLIC splav_support)

splav_add_executable(splav main.cpp)
set_target_properties(splav PROPERTIES OUTPUT_NAME "сплав")
target_link_libraries(splav PRIVATE splav_driver)
