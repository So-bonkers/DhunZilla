cmake_minimum_required(VERSION 3.22)
project(dhunzilla CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Determinism policy (ADR-0004): no FMA contraction, ever. No fast-math in any build type.
add_compile_options(-Wall -Wextra -ffp-contract=off)

# WP-0139: AGENTS.md promises "zero new warnings" — make it machine-enforced on OUR targets only
# (target_compile_options, never global flags) so vendored/third-party code can't break the build.
option(DZ_WERROR "Treat warnings as errors on our targets" ON)

# WP-0139: ASAN+UBSAN build for the CI sanitizers job. Off by default; determinism flags above stay.
option(DZ_SANITIZE "Build with ASAN+UBSAN" OFF)

add_library(opengine STATIC src/engine/model.cpp src/engine/ops.cpp src/engine/render.cpp src/engine/wav.cpp src/engine/midi.cpp src/engine/bundle.cpp)
target_include_directories(opengine PUBLIC src third_party)

# WP-0141: JUCE-free document layer (open/save/dirty, undo/redo, gestures) for the app + CLI.
add_library(dzdoc STATIC src/doc/document.cpp)
target_link_libraries(dzdoc PUBLIC opengine)

add_executable(opd src/cli/main.cpp)
target_link_libraries(opd PRIVATE opengine)

enable_testing()
add_executable(op_tests tests/test_main.cpp)
target_link_libraries(op_tests PRIVATE opengine dzdoc)
target_compile_definitions(op_tests PRIVATE OP_TEST_DATA="${CMAKE_SOURCE_DIR}/tests")
add_test(NAME unit COMMAND op_tests)

# Our targets only — list explicitly so third_party/ can never trip these gates.
set(DZ_OUR_TARGETS opengine dzdoc opd op_tests)

if(DZ_WERROR)
  foreach(tgt ${DZ_OUR_TARGETS})
    target_compile_options(${tgt} PRIVATE -Werror)
  endforeach()
endif()

if(DZ_SANITIZE)
  set(DZ_SAN_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
  # No use-after-return "fake stack": with it, the JSON fuzz tests (nlohmann lexer at -O0) go from ~6 s
  # to >10 min, which made the suite look hung. Heap/stack overflow, use-after-free, leaks and UB stay on.
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    list(APPEND DZ_SAN_FLAGS --param=asan-use-after-return=0)
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    list(APPEND DZ_SAN_FLAGS -fsanitize-address-use-after-return=never)
  endif()
  foreach(tgt ${DZ_OUR_TARGETS})
    target_compile_options(${tgt} PRIVATE ${DZ_SAN_FLAGS})
    # For the static lib these propagate to its consumers at link time; executables get them directly.
    target_link_options(${tgt} PRIVATE ${DZ_SAN_FLAGS})
  endforeach()
endif()
