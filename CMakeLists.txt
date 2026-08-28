cmake_minimum_required(VERSION 3.20)

project(
        LLVMInterproceduralGlobalIVOptimization
        LANGUAGES CXX
)

# ============================================================================
# LLVM
# ============================================================================

find_program(
        LLVM_CONFIG
        NAMES llvm-config-22 llvm-config
        REQUIRED
)

execute_process(
        COMMAND "${LLVM_CONFIG}" --cmakedir
        OUTPUT_VARIABLE LLVM_CMAKE_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY
)

find_package(
        LLVM REQUIRED CONFIG
        PATHS "${LLVM_CMAKE_DIR}"
        NO_DEFAULT_PATH
)

list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")

include(AddLLVM)

add_definitions(${LLVM_DEFINITIONS})

message(STATUS "LLVM version: ${LLVM_PACKAGE_VERSION}")
message(STATUS "LLVM_DIR: ${LLVM_DIR}")
message(STATUS "LLVM tools dir: ${LLVM_TOOLS_BINARY_DIR}")

# ============================================================================
# LLVM pass plugin
# ============================================================================

file(
        GLOB_RECURSE
        GLOBAL_IV_SOURCES
        CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

file(
        GLOB_RECURSE
        GLOBAL_IV_HEADERS
        CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
)

add_llvm_pass_plugin(
        GlobalIVPass
        ${GLOBAL_IV_SOURCES}
        ${GLOBAL_IV_HEADERS}
)

target_compile_features(
        GlobalIVPass
        PRIVATE
        cxx_std_23
)

target_include_directories(
        GlobalIVPass
        PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_include_directories(
        GlobalIVPass
        SYSTEM PRIVATE
        ${LLVM_INCLUDE_DIRS}
)

target_compile_options(
        GlobalIVPass
        PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Werror
)

# ============================================================================
# opt
# ============================================================================

find_program(
        OPT_EXECUTABLE
        NAMES opt
        HINTS "${LLVM_TOOLS_BINARY_DIR}"
        NO_DEFAULT_PATH
        REQUIRED
)

message(STATUS "opt: ${OPT_EXECUTABLE}")

# ============================================================================
# Command-line run target
# ============================================================================

add_custom_target(
        run-pass

        COMMAND
        "${OPT_EXECUTABLE}"
        "-load-pass-plugin=$<TARGET_FILE:GlobalIVPass>"
        "-passes=global-iv"
        "-S"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke.ll"

        DEPENDS
        GlobalIVPass

        WORKING_DIRECTORY
        "${CMAKE_CURRENT_SOURCE_DIR}"

        USES_TERMINAL
        VERBATIM
)