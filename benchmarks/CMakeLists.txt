# Benchmark corpus and runner. Timing is done entirely outside the engine.
add_library(sudoku_bench_core STATIC bench.cpp)
target_include_directories(sudoku_bench_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sudoku_bench_core PUBLIC sudoku_core)
target_compile_definitions(sudoku_bench_core PRIVATE SUDOKU_BUILD_TYPE="${CMAKE_BUILD_TYPE}")
sudoku_configure_target(sudoku_bench_core)

add_executable(sudoku_bench main.cpp)
target_link_libraries(sudoku_bench PRIVATE sudoku_bench_core)
sudoku_configure_target(sudoku_bench)
