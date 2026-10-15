cmake_minimum_required(VERSION 3.20)

project(SudokuStudio VERSION 0.1.0 LANGUAGES CXX)

option(SUDOKU_BUILD_TESTS "Build C++ tests and register them with CTest" ON)

# Applied per target (not globally). Extensions are off so code stays
# standard C++20 on every compiler.
function(sudoku_configure_target target)
  target_compile_features(${target} PRIVATE cxx_std_20)
  set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF
                                             CXX_STANDARD_REQUIRED ON)
  target_compile_options(${target} PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>)
endfunction()

# Engine core. Public headers live under cpp/include/sudoku/.
add_library(sudoku_core STATIC cpp/src/board.cpp cpp/src/solver.cpp
  cpp/src/generator.cpp cpp/src/puzzle.cpp cpp/src/candidates.cpp
  cpp/src/deduction.cpp cpp/src/singles.cpp cpp/src/locked.cpp cpp/src/pairs.cpp cpp/src/triples.cpp cpp/src/xwing.cpp
  cpp/src/verify.cpp cpp/src/logical.cpp cpp/src/difficulty.cpp cpp/src/targeted.cpp cpp/src/diagnostics.cpp
  cpp/src/json_reader.cpp cpp/src/protocol.cpp)
target_include_directories(sudoku_core PUBLIC cpp/include)
target_compile_definitions(sudoku_core PRIVATE SUDOKU_ENGINE_VERSION="${PROJECT_VERSION}")
sudoku_configure_target(sudoku_core)

add_executable(sudoku_cli cpp/src/main.cpp)
target_compile_definitions(sudoku_cli PRIVATE
  SUDOKU_VERSION="${PROJECT_VERSION}")
target_link_libraries(sudoku_cli PRIVATE sudoku_core)
sudoku_configure_target(sudoku_cli)

option(SUDOKU_BUILD_BENCHMARKS "Build the benchmark runner (sudoku_bench)" ON)
if(SUDOKU_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

if(SUDOKU_BUILD_TESTS)
  enable_testing()
  add_subdirectory(cpp/tests)
endif()
