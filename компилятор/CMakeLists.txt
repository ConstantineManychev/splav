add_subdirectory(support)
add_subdirectory(unicode)
add_subdirectory(syntax)
add_subdirectory(sema)
add_subdirectory(format)
add_subdirectory(driver)

if(SPLAV_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(SPLAV_FUZZ)
    add_subdirectory(fuzz)
endif()

if(SPLAV_ENABLE_MLIR)
    add_subdirectory(dialect)
endif()
