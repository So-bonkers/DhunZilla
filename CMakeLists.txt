cmake_minimum_required(VERSION 3.22)
project(openpattern CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Determinism policy (ADR-0004): no FMA contraction, ever. No fast-math in any build type.
add_compile_options(-Wall -Wextra -ffp-contract=off)

add_library(opengine STATIC src/engine/model.cpp src/engine/render.cpp src/engine/wav.cpp src/engine/midi.cpp)
target_include_directories(opengine PUBLIC src third_party)

add_executable(opd src/cli/main.cpp)
target_link_libraries(opd PRIVATE opengine)

enable_testing()
add_executable(op_tests tests/test_main.cpp)
target_link_libraries(op_tests PRIVATE opengine)
target_compile_definitions(op_tests PRIVATE OP_TEST_DATA="${CMAKE_SOURCE_DIR}/tests")
add_test(NAME unit COMMAND op_tests)
