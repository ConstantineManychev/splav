find_program(SPLAV_FILECHECK FileCheck HINTS "${LLVM_TOOLS_BINARY_DIR}" NO_DEFAULT_PATH)
if(NOT SPLAV_FILECHECK)
    set(SPLAV_FILECHECK_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/FileCheck.cpp")
    file(DOWNLOAD "${SPLAV_FILECHECK_SOURCE_URL}" "${SPLAV_FILECHECK_SOURCE}"
        EXPECTED_HASH SHA256=${SPLAV_FILECHECK_SOURCE_SHA256}
        TLS_VERIFY ON
    )
    add_llvm_executable(FileCheck "${SPLAV_FILECHECK_SOURCE}")
    llvm_update_compile_flags(FileCheck)
    target_link_libraries(FileCheck PRIVATE LLVMFileCheck LLVMSupport)
    set(SPLAV_FILECHECK "$<TARGET_FILE:FileCheck>")
endif()

add_test(NAME диалект-разбор-и-печать
    COMMAND "${CMAKE_COMMAND}"
        "-DSPLAV_OPT=$<TARGET_FILE:splav-opt>"
        "-DFILECHECK=${SPLAV_FILECHECK}"
        "-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/roundtrip.mlir"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_filecheck.cmake"
)
