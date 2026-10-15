# No C++ test framework was available when this milestone was written
# (see README "Known limitations"). Tests here are plain executables whose
# exit status CTest evaluates; this is NOT a unit-test framework.

add_executable(build_smoke_test build_smoke_test.cpp)
sudoku_configure_target(build_smoke_test)
add_test(NAME build_smoke COMMAND build_smoke_test)

add_test(NAME cli_version COMMAND sudoku_cli --version)
set_tests_properties(cli_version PROPERTIES
  PASS_REGULAR_EXPRESSION "^sudoku_cli ${PROJECT_VERSION}")

add_test(NAME cli_rejects_unknown_args COMMAND sudoku_cli --bogus)
set_tests_properties(cli_rejects_unknown_args PROPERTIES WILL_FAIL TRUE)

add_executable(board_test board_test.cpp)
target_link_libraries(board_test PRIVATE sudoku_core)
sudoku_configure_target(board_test)
add_test(NAME board COMMAND board_test)

add_executable(solver_test solver_test.cpp)
target_link_libraries(solver_test PRIVATE sudoku_core)
sudoku_configure_target(solver_test)
add_test(NAME solver COMMAND solver_test)

add_executable(generator_test generator_test.cpp)
target_link_libraries(generator_test PRIVATE sudoku_core)
sudoku_configure_target(generator_test)
add_test(NAME generator COMMAND generator_test)

add_executable(puzzle_test puzzle_test.cpp)
target_link_libraries(puzzle_test PRIVATE sudoku_core)
sudoku_configure_target(puzzle_test)
add_test(NAME puzzle COMMAND puzzle_test)

add_executable(hardening_test hardening_test.cpp)
target_link_libraries(hardening_test PRIVATE sudoku_core)
sudoku_configure_target(hardening_test)
add_test(NAME hardening COMMAND hardening_test)

add_executable(candidates_test candidates_test.cpp)
target_link_libraries(candidates_test PRIVATE sudoku_core)
sudoku_configure_target(candidates_test)
add_test(NAME candidates COMMAND candidates_test)

add_executable(deduction_test deduction_test.cpp)
target_link_libraries(deduction_test PRIVATE sudoku_core)
sudoku_configure_target(deduction_test)
add_test(NAME deduction COMMAND deduction_test)

add_executable(singles_test singles_test.cpp)
target_link_libraries(singles_test PRIVATE sudoku_core)
sudoku_configure_target(singles_test)
add_test(NAME singles COMMAND singles_test)

add_executable(locked_test locked_test.cpp)
target_link_libraries(locked_test PRIVATE sudoku_core)
sudoku_configure_target(locked_test)
add_test(NAME locked COMMAND locked_test)

add_executable(pairs_test pairs_test.cpp)
target_link_libraries(pairs_test PRIVATE sudoku_core)
sudoku_configure_target(pairs_test)
add_test(NAME pairs COMMAND pairs_test)

add_executable(triples_test triples_test.cpp)
target_link_libraries(triples_test PRIVATE sudoku_core)
sudoku_configure_target(triples_test)
add_test(NAME triples COMMAND triples_test)

add_executable(xwing_test xwing_test.cpp)
target_link_libraries(xwing_test PRIVATE sudoku_core)
sudoku_configure_target(xwing_test)
add_test(NAME xwing COMMAND xwing_test)

add_executable(logical_test logical_test.cpp)
target_link_libraries(logical_test PRIVATE sudoku_core)
sudoku_configure_target(logical_test)
add_test(NAME logical COMMAND logical_test)

add_executable(difficulty_test difficulty_test.cpp)
target_link_libraries(difficulty_test PRIVATE sudoku_core)
sudoku_configure_target(difficulty_test)
add_test(NAME difficulty COMMAND difficulty_test)

add_executable(targeted_test targeted_test.cpp)
target_link_libraries(targeted_test PRIVATE sudoku_core)
sudoku_configure_target(targeted_test)
add_test(NAME targeted COMMAND targeted_test)

if(TARGET sudoku_bench_core)
  add_executable(bench_test bench_test.cpp)
  target_link_libraries(bench_test PRIVATE sudoku_bench_core)
  sudoku_configure_target(bench_test)
  add_test(NAME bench COMMAND bench_test)
  add_test(NAME bench_smoke COMMAND sudoku_bench --tier smoke --repetitions 1)
endif()

add_executable(protocol_test protocol_test.cpp)
target_link_libraries(protocol_test PRIVATE sudoku_core)
sudoku_configure_target(protocol_test)
add_test(NAME protocol COMMAND protocol_test)
