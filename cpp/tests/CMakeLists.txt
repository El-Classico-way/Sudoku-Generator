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
