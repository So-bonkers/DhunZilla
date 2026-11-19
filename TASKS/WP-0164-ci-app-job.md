---
id: WP-0164
title: CI job that builds the JUCE app and runs the headless snapshot
phase: 1
status: ready
depends_on: [WP-0143]
estimate_agent_hours: 1
human_review: none
---

## Context — why this exists
The JUCE app (`DZ_BUILD_APP`, target `dhunzilla`, `--snapshot`) shipped in WP-0143 but **CI never builds
it**, so a change that breaks the app compiles green. WP-0137's plan asked for an `app` CI job; it was
never added. This package adds exactly that job. **It is CI-only: NO source change of any kind.**

## Spec — what "correct" means
Edit only `.github/workflows/ci.yml`: **append** one new job `app` at the END of the file (after the
`sidecar` job, separated by one blank line). Do not modify, reorder, reformat or re-comment any existing
line (`core`, `sanitizers`, `sidecar` and the header must stay byte-identical). Do not touch `CMakeLists.txt`
or anything under `src/`.

The job must be exactly this (indentation is 2 spaces per level, like the rest of the file):

```yaml
  # WP-0164: the JUCE app was never built in CI. Builds `dhunzilla` and runs the headless --snapshot
  # under Xvfb; asserts exit 0 and a 1100x700 PNG. No audio in CI — never run --play-seconds here.
  app:
    strategy:
      fail-fast: false
      matrix:
        include:
          - os: ubuntu-24.04        # linux-x64
            name: linux-x64
          - os: ubuntu-24.04-arm    # linux-arm64 (native ARM runner)
            name: linux-arm64
    name: app ${{ matrix.name }}
    runs-on: ${{ matrix.os }}
    steps:
      - uses: actions/checkout@v4
      - uses: lukka/get-cmake@latest
      - name: Install JUCE Linux dependencies
        run: |
          sudo apt-get update
          sudo apt-get install -y --no-install-recommends \
            libasound2-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev \
            libxext-dev libxcomposite-dev libxrender-dev libfreetype-dev libfontconfig1-dev xvfb
      # Key is pinned to the JUCE version in CMakeLists.txt (FetchContent tag 9.0.5): bumping JUCE there
      # means bumping the number here, which invalidates the cache on purpose.
      - name: Cache JUCE download and build tree
        uses: actions/cache@v4
        with:
          path: build-app/_deps
          key: juce-9.0.5-${{ matrix.name }}
      - name: Configure (DZ_BUILD_APP=ON)
        run: cmake -S . -B build-app -DCMAKE_BUILD_TYPE=Release -DDZ_BUILD_APP=ON
      - name: Build dhunzilla
        run: cmake --build build-app -j4 --target dhunzilla
      - name: Snapshot under Xvfb
        run: xvfb-run -a ./build-app/dhunzilla_artefacts/Release/dhunzilla --snapshot snap.png tests/fixtures/goldens/demo.opj
      - name: Assert snapshot is a 1100x700 PNG
        run: |
          python3 - <<'PY'
          d = open("snap.png", "rb").read()
          assert d[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
          assert d[12:16] == b"IHDR", "first chunk is not IHDR"
          w = int.from_bytes(d[16:20], "big")
          h = int.from_bytes(d[20:24], "big")
          assert (w, h) == (1100, 700), f"snapshot is {w}x{h}, expected 1100x700"
          print(f"snapshot OK: {w}x{h}")
          PY
      - name: Upload snapshot
        if: always()
        uses: actions/upload-artifact@v4
        with:
          name: snapshot-${{ matrix.name }}
          path: snap.png
          if-no-files-found: ignore
```

Keep the step names, the `build-app` directory, the cache key `juce-9.0.5-${{ matrix.name }}` and the
commands character-for-character. A failing snapshot command already fails its step (exit status ≠ 0),
so do not add `|| true` anywhere.

## Acceptance criteria
- [ ] `.github/workflows/ci.yml` still parses as YAML (`python3 -c "import yaml,sys;yaml.safe_load(open('.github/workflows/ci.yml'))"`
      if PyYAML exists; otherwise eyeball indentation) and `git diff --stat` shows ONLY that file plus
      this task file and ROADMAP.md, with only added lines in `ci.yml`.
- [ ] The commands in the job work locally (you cannot run Actions): configure + build with
      `cmake -S . -B build-app -DCMAKE_BUILD_TYPE=Release -DDZ_BUILD_APP=ON -DFETCHCONTENT_SOURCE_DIR_JUCE=<workspace>/exp-juce/build/_deps/juce-src`
      then `cmake --build build-app -j4 --target dhunzilla`, then (with `DISPLAY=${DISPLAY:-:1}`, and
      `XAUTHORITY=/run/user/1000/gdm/Xauthority` if needed, instead of `xvfb-run -a`) the snapshot
      command and the python assertion. Paste the outputs. (`build-app/` is gitignored.)
- [ ] Default gates still green (nothing in them changed): default `cmake -B build` build, `./build/op_tests`,
      `bash harness/run_goldens.sh`, `bash harness/test_cli_errors.sh`, `bash harness/test_glue_scripts.sh`.
- [ ] ROADMAP.md: WP-0164 row at the top of the work-package table.

## Out of scope — do NOT touch
- **No source changes: `src/` (including `src/app/`), `CMakeLists.txt`, `tests/`, `harness/`, `ai-sidecar/`,
  `docs/`, golden fixtures.** The only repo file you change besides this task file is `.github/workflows/ci.yml`
  (and the ROADMAP.md row). Existing CI jobs. Branch protection / required-checks settings (the owner adds
  the new `app` checks later).

## Git
- Branch `wp-0164-ci-app-job` from `main`. Commit: `WP-0164: CI job building the JUCE app + headless snapshot`.
  Stage explicit paths only (never `git add -A` / `git add .`). Include this task file with the handoff filled.
- **Do not push or merge.** Build with `-j4` at most.

---
## Handoff report — completed by the agent
- **What changed:**
  - `.github/workflows/ci.yml`: appended the `app` job at the END of the file, after `sidecar`, separated by one blank line — byte-for-byte identical to the block in the Spec section (verified with `diff`, see below). Nothing else in the file touched: diff is **54 insertions(+), 0 deletions**.
  - `ROADMAP.md`: WP-0164 row added at the top of the work-package table.
  - No source changes: nothing under `src/`, `CMakeLists.txt`, `tests/`, `harness/`, `ai-sidecar/`, `docs/` modified (`git diff --stat` evidence below).
- **How verified:** (commands + actual output)
  - YAML parse gate:
    ```
    $ python3 -c "import yaml,sys;yaml.safe_load(open('.github/workflows/ci.yml'));print('YAML OK')"
    YAML OK
    exit=0
    ```
  - Diff is additions-only in `ci.yml`, and only the three allowed files change (task file shows here once staged):
    ```
    $ git diff --numstat .github/workflows/ci.yml
    54      0       .github/workflows/ci.yml
    ```
  - Appended block matches the task's Spec code fence character-for-character (spec lines 25–77 vs last 53 lines of `ci.yml`; the 54th added line is the required blank separator):
    ```
    $ sed -n '25,77p' TASKS/WP-0164-ci-app-job.md > spec_block.txt && tail -n 53 .github/workflows/ci.yml > ci_block.txt
    $ diff spec_block.txt ci_block.txt && echo "BLOCK MATCHES SPEC BYTE-FOR-BYTE"
    BLOCK MATCHES SPEC BYTE-FOR-BYTE
    ```
  - Local job commands (Actions can't run here; ran the acceptance variant with
    `-DFETCHCONTENT_SOURCE_DIR_JUCE=/home/shubhankar_dgx/qwen-scratch/exp-juce/build/_deps/juce-src` and a real `DISPLAY=:1` instead of `xvfb-run -a`; no Xvfb/xvfb-run is installed on this rig):
    ```
    $ cmake -S . -B build-app -DCMAKE_BUILD_TYPE=Release -DDZ_BUILD_APP=ON -DFETCHCONTENT_SOURCE_DIR_JUCE=/home/shubhankar_dgx/qwen-scratch/exp-juce/build/_deps/juce-src
    -- Configuring done (0.5s)
    -- Generating done (0.0s)
    -- Build files have been written to: /home/shubhankar_dgx/qwen-scratch/openpattern/build-app

    $ cmake --build build-app -j4 --target dhunzilla
    [ 14%] Building CXX object CMakeFiles/opengine.dir/src/engine/midi.cpp.o
    ...
    [ 28%] Building CXX object CMakeFiles/dhunzilla.dir/src/app/main.cpp.o
    [ 30%] Building CXX object CMakeFiles/dhunzilla.dir/src/app/main_component.cpp.o
    [ 33%] Linking CXX executable dhunzilla_artefacts/Release/dhunzilla
    [100%] Built target dhunzilla

    $ export DISPLAY=${DISPLAY:-:1}
    $ ./build-app/dhunzilla_artefacts/Release/dhunzilla --snapshot snap.png tests/fixtures/goldens/demo.opj
    title: DhunZilla — demo.opj
    snapshot: snap.png 1100x700
    exit=0

    $ python3 - <<'PY'   # verbatim from the job's assert step
    d = open("snap.png", "rb").read()
    ...
    PY
    snapshot OK: 1100x700
    exit=0
    ```
    (`snap.png` was deleted afterwards; `build-app/` is gitignored.)
  - Default gates, all with real (unpiped) exit codes:
    ```
    $ cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
    [100%] Linking CXX executable op_tests
    [100%] Built target op_tests

    $ ./build/op_tests ; echo "op_tests exit=$?"
    ALL PASS (0 failures)
    op_tests exit=0

    $ bash harness/run_goldens.sh ; echo "run_goldens exit=$?"
    PASS negative-control (corrupt golden rejected)
    run_goldens: 9 fixtures, 0 failures
    run_goldens exit=0

    $ bash harness/test_cli_errors.sh ; echo "test_cli_errors exit=$?"
    test_cli_errors: 0 failures
    test_cli_errors exit=0

    $ export UV_CACHE_DIR=/home/shubhankar_dgx/qwen-scratch/.cache/uv
    $ bash harness/test_glue_scripts.sh ; echo "test_glue_scripts exit=$?"
    PASS rel_no_repo_strays
    test_glue_scripts: 0 failures
    test_glue_scripts exit=0
    ```
- **Follow-ups noticed (not done):**
  - Owner-side only, already flagged in the task's Out-of-scope: add the two new required status checks `app linux-x64` and `app linux-arm64` to branch protection when this lands.
  - The `app` job runs without `-Werror` context on JUCE sources by design (CMakeLists applies `-Werror` only to our two app sources via source-file property); nothing to change.
- **PROPOSE / BLOCKED:** none.
