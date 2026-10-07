cmake_minimum_required(VERSION 3.20)
project(backtest_engine VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Option to disable architecture-specific host optimizations for portable packaging
option(PORTABLE_BUILD "Build portable binaries without host-specific vectorization" OFF)

# Treat compiler warnings as errors.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        -Wall -Wextra -Wpedantic -Werror
        -Wconversion -Wshadow -Wno-attributes
    )
    # Optimization flags for release
    if(PORTABLE_BUILD)
        set(CMAKE_CXX_FLAGS_RELEASE "-O3 -msse4.2 -DNDEBUG")
    else()
        set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")
    endif()
elseif(MSVC)
    add_compile_options(/W4 /WX /wd4324)  # 4324: padding warning for packed structs
    set(CMAKE_CXX_FLAGS_RELEASE "/O2 /Ob2 /DNDEBUG")
endif()
# Core static library
add_library(backtest_core STATIC
    src/book/book.cpp
    src/engine/engine.cpp
    src/execution/execution.cpp
    src/feed/feed.cpp
    src/risk/risk.cpp
)

target_include_directories(backtest_core PUBLIC include)

# Disable MSVC warnings for C runtime calls.
if(MSVC)
    target_compile_definitions(backtest_core PUBLIC _CRT_SECURE_NO_WARNINGS)
endif()
# Optional: Pybind11 module
option(BUILD_PYBIND11 "Build the Python bindings" OFF)

if(BUILD_PYBIND11)
    find_package(pybind11 REQUIRED)

    pybind11_add_module(_core src/bindings/module.cpp)
    target_link_libraries(_core PRIVATE backtest_core)
    target_compile_definitions(_core PRIVATE BUILD_PYBIND11)

    # Static linking for MinGW to avoid DLL dependency issues
    if(MINGW)
        target_link_options(_core PRIVATE -static)
    endif()
endif()
# Tests (using a simple main-based test runner)
option(BUILD_TESTS "Build C++ tests" ON)

if(BUILD_TESTS)
    enable_testing()

    # Tests use assert() which is a no-op in Release. Relax -Werror for tests.
    function(add_backtest_test test_name test_src)
        add_executable(${test_name} ${test_src})
        target_link_libraries(${test_name} PRIVATE backtest_core)
        target_compile_options(${test_name} PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang>:-Wno-error=unused-variable -Wno-error=unused-parameter -Wno-error=unused-but-set-variable>
        )
        target_compile_definitions(${test_name} PRIVATE DEBUG)
        add_test(NAME ${test_name} COMMAND ${test_name})
    endfunction()

    add_backtest_test(test_book tests/test_book.cpp)
    add_backtest_test(test_scheduler tests/test_scheduler.cpp)
    add_backtest_test(test_execution tests/test_execution.cpp)
    add_backtest_test(test_engine tests/test_engine.cpp)
endif()
