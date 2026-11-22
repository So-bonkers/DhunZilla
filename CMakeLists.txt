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

add_library(opengine STATIC src/engine/model.cpp src/engine/ops.cpp src/engine/render.cpp src/engine/resample.cpp src/engine/wav.cpp src/engine/midi.cpp src/engine/bundle.cpp src/engine/analyze.cpp src/engine/sample_index.cpp)
target_include_directories(opengine PUBLIC src third_party)
target_sources(opengine PRIVATE src/engine/loudness.cpp)  # WP-0168
target_sources(opengine PRIVATE src/engine/lint.cpp)  # WP-0170
target_sources(opengine PRIVATE src/engine/peaks.cpp)  # WP-0171
target_sources(opengine PRIVATE src/engine/limiter.cpp)  # WP-0176
target_sources(opengine PRIVATE src/engine/meters.cpp)  # WP-0194

# WP-0141: JUCE-free document layer (open/save/dirty, undo/redo, gestures) for the app + CLI.
add_library(dzdoc STATIC src/doc/document.cpp)
target_link_libraries(dzdoc PUBLIC opengine)
target_sources(opengine PRIVATE src/engine/normalize.cpp)  # WP-0169

add_executable(opd src/cli/main.cpp)
target_link_libraries(opd PRIVATE opengine)

# WP-0153: JUCE-free playback core (Player + bar.beat formatting + --render-playback driver).
# Lives in src/app/ but includes no JUCE, so the default build and op_tests exercise it headless.
add_library(dzplayer STATIC src/app/player.cpp src/app/player_rt.cpp src/app/render_playback.cpp)
target_include_directories(dzplayer PUBLIC src/app)
target_link_libraries(dzplayer PUBLIC opengine)

enable_testing()
add_executable(op_tests tests/test_main.cpp)
target_link_libraries(op_tests PRIVATE opengine dzdoc dzplayer)
target_compile_definitions(op_tests PRIVATE OP_TEST_DATA="${CMAKE_SOURCE_DIR}/tests"
    OPD_BIN="$<TARGET_FILE:opd>")  # WP-0162: unit tests exercise `opd steps` default width end-to-end
# WP-0162 fix: op_tests runs the opd binary (OPD_BIN), so building only `--target op_tests` must build opd too.
add_dependencies(op_tests opd)
add_test(NAME unit COMMAND op_tests)

add_executable(op_loudness_tests tests/test_loudness.cpp)
target_link_libraries(op_loudness_tests PRIVATE opengine)
add_test(NAME loudness COMMAND op_loudness_tests)

add_executable(op_normalize_tests tests/test_normalize.cpp)
target_link_libraries(op_normalize_tests PRIVATE opengine)
add_test(NAME normalize COMMAND op_normalize_tests)

add_executable(op_lint_tests tests/test_lint.cpp)
target_link_libraries(op_lint_tests PRIVATE opengine)
add_test(NAME lint COMMAND op_lint_tests)
add_executable(op_lint_rules_tests tests/test_lint_rules.cpp)
target_link_libraries(op_lint_rules_tests PRIVATE opengine)
add_test(NAME lint_rules COMMAND op_lint_rules_tests)

add_executable(op_peaks_tests tests/test_peaks.cpp)
target_link_libraries(op_peaks_tests PRIVATE opengine)
add_test(NAME peaks COMMAND op_peaks_tests)

add_executable(op_limiter_tests tests/test_limiter.cpp)
target_link_libraries(op_limiter_tests PRIVATE opengine)
add_test(NAME limiter COMMAND op_limiter_tests)

add_executable(op_choke_tests tests/test_choke.cpp)
target_link_libraries(op_choke_tests PRIVATE opengine)
add_test(NAME choke COMMAND op_choke_tests)

# WP-0190: dz_latency — Phase 0 callback-budget report for the JUCE-free dzplayer core (report, not a gate).
add_executable(dz_latency tools/dz_latency.cpp)
target_link_libraries(dz_latency PRIVATE opengine dzplayer)

add_executable(op_meters_tests tests/test_meters.cpp)
target_link_libraries(op_meters_tests PRIVATE opengine)
add_test(NAME meters COMMAND op_meters_tests)
add_executable(op_clip_trim_tests tests/test_clip_trim.cpp)
target_link_libraries(op_clip_trim_tests PRIVATE opengine)
add_test(NAME clip_trim COMMAND op_clip_trim_tests)
add_executable(op_clip_trim_render_tests tests/test_clip_trim_render.cpp)
target_link_libraries(op_clip_trim_render_tests PRIVATE opengine)
add_test(NAME clip_trim_render COMMAND op_clip_trim_render_tests)

add_executable(op_clips_tests tests/test_clips.cpp)
target_link_libraries(op_clips_tests PRIVATE opengine)
add_test(NAME clips COMMAND op_clips_tests)
# WP-0210: opd timeline (bar ruler backend). Runs the opd binary, so it needs OPD_BIN and depends on it.
add_executable(op_timeline_tests tests/test_timeline.cpp)
target_link_libraries(op_timeline_tests PRIVATE opengine)
target_compile_definitions(op_timeline_tests PRIVATE OPD_BIN="$<TARGET_FILE:opd>")
add_dependencies(op_timeline_tests opd)
add_test(NAME timeline COMMAND op_timeline_tests)

# WP-0200: dz::Player transport loop region (needs the JUCE-free dzplayer core).
add_executable(op_player_loop_tests tests/test_player_loop.cpp)
target_link_libraries(op_player_loop_tests PRIVATE opengine dzplayer)
add_test(NAME player_loop COMMAND op_player_loop_tests)

# WP-0201: dz::Player metronome click mixed in while playing (dzplayer core + engine click track).
add_executable(op_player_click_tests tests/test_player_click.cpp)
target_link_libraries(op_player_click_tests PRIVATE opengine dzplayer)
add_test(NAME player_click COMMAND op_player_click_tests)

# Our targets only — list explicitly so third_party/ can never trip these gates.
set(DZ_OUR_TARGETS opengine dzdoc dzplayer opd op_tests)
list(APPEND DZ_OUR_TARGETS op_loudness_tests)  # WP-0168
list(APPEND DZ_OUR_TARGETS op_normalize_tests)  # WP-0169
list(APPEND DZ_OUR_TARGETS op_lint_tests)  # WP-0170
list(APPEND DZ_OUR_TARGETS op_peaks_tests)  # WP-0171
list(APPEND DZ_OUR_TARGETS op_choke_tests)  # WP-0181
list(APPEND DZ_OUR_TARGETS op_limiter_tests)  # WP-0176
list(APPEND DZ_OUR_TARGETS dz_latency)  # WP-0190

list(APPEND DZ_OUR_TARGETS op_meters_tests)  # WP-0194
list(APPEND DZ_OUR_TARGETS op_clip_trim_tests)  # WP-0193a
list(APPEND DZ_OUR_TARGETS op_clip_trim_render_tests)  # WP-0193b
list(APPEND DZ_OUR_TARGETS op_clips_tests)  # WP-0199
list(APPEND DZ_OUR_TARGETS op_timeline_tests)  # WP-0210
list(APPEND DZ_OUR_TARGETS op_lint_rules_tests)  # WP-0203
list(APPEND DZ_OUR_TARGETS op_player_loop_tests)  # WP-0200
list(APPEND DZ_OUR_TARGETS op_player_click_tests)  # WP-0201

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

# ---------------------------------------------------------------------------
# WP-0143: JUCE app window + headless snapshot (dhunzilla). OFF by default, so a default configure
# never mentions JUCE and builds exactly opengine dzdoc dzplayer opd op_tests as before. dhunzilla is
# NOT in DZ_OUR_TARGETS: the -Werror/-fsanitize loops above must not reach it or generated JUCE targets.
#
# Local build recipe (point FETCHCONTENT_SOURCE_DIR_JUCE at an unpacked JUCE 9.0.5 tree to build offline):
#   export DISPLAY=${DISPLAY:-:0}   # JUCE needs an X display even for --snapshot (xvfb-run -a in CI)
#   cmake -S . -B build-app -DCMAKE_BUILD_TYPE=Release -DDZ_BUILD_APP=ON \
#         [-DFETCHCONTENT_SOURCE_DIR_JUCE=<path-to-JUCE-9.0.5-source>]
#   cmake --build build-app -j4 --target dhunzilla    # cold JUCE build ~3-5 min, several GB RAM: -j4 max
# Without FETCHCONTENT_SOURCE_DIR_JUCE, FetchContent downloads the pinned tarball.
# Binary lands at build-app/dhunzilla_artefacts/Release/dhunzilla.
option(DZ_BUILD_APP "Build the JUCE app (dhunzilla)" OFF)

if(DZ_BUILD_APP)
  enable_language(C)  # top-level project() is CXX-only; JUCE's CMake aborts without a C compiler
  include(FetchContent)
  FetchContent_Declare(JUCE
      URL https://github.com/juce-framework/JUCE/archive/refs/tags/9.0.5.tar.gz
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(JUCE)

  juce_add_gui_app(dhunzilla
      VERSION 0.1.0
      PRODUCT_NAME "dhunzilla"
      COMPANY_NAME "DhunZilla")
  target_sources(dhunzilla PRIVATE src/app/main.cpp src/app/main_component.cpp src/app/playback_controller.cpp)
  target_compile_definitions(dhunzilla PRIVATE
      JUCE_WEB_BROWSER=0
      JUCE_USE_CURL=0
      JUCE_APPLICATION_NAME_STRING="dhunzilla"
      JUCE_APPLICATION_VERSION_STRING="0.1.0")
  target_include_directories(dhunzilla PRIVATE src/app)
  target_link_libraries(dhunzilla PRIVATE dzdoc dzplayer juce::juce_gui_basics juce::juce_audio_devices
      PUBLIC juce::juce_recommended_config_flags
             juce::juce_recommended_warning_flags)

  # Upstream JUCE warnings are not ours to fix: -Werror applies to our app sources only, via
  # source-file property — never by adding dhunzilla to DZ_OUR_TARGETS.
  if(DZ_WERROR)
    set_source_files_properties(src/app/main.cpp src/app/main_component.cpp src/app/playback_controller.cpp
        PROPERTIES COMPILE_OPTIONS -Werror)
  endif()
endif()
