cmake_minimum_required(VERSION 3.25)

project(splav LANGUAGES CXX)

set(SPLAV_VERSION "0.050.13")

option(SPLAV_WERROR "Считать предупреждения ошибками" OFF)
option(SPLAV_BUILD_TESTS "Собирать тесты" ON)
option(SPLAV_SANITIZE "Собирать с AddressSanitizer и UndefinedBehaviorSanitizer" OFF)
option(SPLAV_FUZZ "Собирать цели libFuzzer" OFF)
option(SPLAV_ENABLE_MLIR "Собирать диалект splav на закреплённом LLVM/MLIR" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(flags)
include(windows_utf8)

if(SPLAV_ENABLE_MLIR)
    enable_language(C)
endif()

if(SPLAV_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(компилятор)
