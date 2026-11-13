set(SPLAV_LLVM_VERSION "23.1.5")
set(SPLAV_LLVM_TAG "llvmorg-${SPLAV_LLVM_VERSION}")

set(SPLAV_LLVM_ARCHIVE_LINUX_X64 "LLVM-${SPLAV_LLVM_VERSION}-Linux-X64.tar.xz")
set(SPLAV_LLVM_ARCHIVE_LINUX_X64_SHA256 "f72edb0b36580ece7f52e23858f00c18fad4e87395ea7ad05d7e55c39f8fc78a")
set(SPLAV_LLVM_ARCHIVE_MACOS_ARM64 "LLVM-${SPLAV_LLVM_VERSION}-macOS-ARM64.tar.xz")
set(SPLAV_LLVM_ARCHIVE_MACOS_ARM64_SHA256 "aba592b4a15e8f90c634c5367b51ec8418b8bcd2829865db331a17dc9418dcff")
set(SPLAV_LLVM_ARCHIVE_WINDOWS_X64 "clang+llvm-${SPLAV_LLVM_VERSION}-x86_64-pc-windows-msvc.tar.xz")
set(SPLAV_LLVM_ARCHIVE_WINDOWS_X64_SHA256 "62fdada7f6fa2b73856486dfbbdba3d2c1882991621615bcb55c60207611c6d4")

set(SPLAV_FILECHECK_SOURCE_URL
    "https://raw.githubusercontent.com/llvm/llvm-project/${SPLAV_LLVM_TAG}/llvm/utils/FileCheck/FileCheck.cpp")
set(SPLAV_FILECHECK_SOURCE_SHA256 "3c6568b68c5c6d6ec8b9a2acb378a6949c9f5004ae009d2597b7f760f944f320")

macro(splav_require_pinned_mlir)
    find_package(MLIR REQUIRED CONFIG)
    if(NOT LLVM_PACKAGE_VERSION VERSION_EQUAL SPLAV_LLVM_VERSION)
        message(FATAL_ERROR
            "Нужен LLVM/MLIR ${SPLAV_LLVM_VERSION}, а в ${LLVM_DIR} найден ${LLVM_PACKAGE_VERSION}. "
            "Версия закреплена в cmake/llvm.cmake и меняется только отдельным решением (решения/0005).")
    endif()
    message(STATUS "LLVM/MLIR ${LLVM_PACKAGE_VERSION}: ${MLIR_DIR}")
    list(APPEND CMAKE_MODULE_PATH "${MLIR_CMAKE_DIR}" "${LLVM_CMAKE_DIR}")
    include(TableGen)
    include(AddLLVM)
    include(AddMLIR)
    include(HandleLLVMOptions)
endmacro()
