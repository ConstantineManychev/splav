include(llvm)
splav_require_pinned_mlir()

include_directories(SYSTEM ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
include_directories("${PROJECT_SOURCE_DIR}/компилятор" "${PROJECT_BINARY_DIR}/компилятор")
separate_arguments(SPLAV_LLVM_DEFINITIONS NATIVE_COMMAND "${LLVM_DEFINITIONS}")
add_definitions(${SPLAV_LLVM_DEFINITIONS})

set(LLVM_TARGET_DEFINITIONS splav_ops.td)
mlir_tablegen(splav_ops.h.inc -gen-op-decls)
mlir_tablegen(splav_ops.cpp.inc -gen-op-defs)
mlir_tablegen(splav_dialect.h.inc -gen-dialect-decls -dialect=splav)
mlir_tablegen(splav_dialect.cpp.inc -gen-dialect-defs -dialect=splav)
add_public_tablegen_target(splav_dialect_generated)

add_mlir_dialect_library(splav_dialect
    splav_dialect.cpp
    splav_ops.cpp
    DEPENDS
    splav_dialect_generated
    LINK_LIBS PUBLIC
    MLIRIR
    MLIRInferTypeOpInterface
)

add_subdirectory(splav-opt)

if(SPLAV_BUILD_TESTS)
    add_subdirectory(tests)
endif()
