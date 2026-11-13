add_subdirectory(support)
add_subdirectory(driver)

if(SPLAV_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(SPLAV_ENABLE_MLIR)
    add_subdirectory(dialect)
endif()
