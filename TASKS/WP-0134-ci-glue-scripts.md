---
id: WP-0134
title: Run the three harness glue scripts in CI (mock backends)
phase: 0
status: done
depends_on: [WP-0133]
estimate_agent_hours: 2
human_review: none
---

## Context — why this exists
`harness/stem_into_project.sh`, `harness/midi_into_project.sh` and `harness/prompt_to_song.sh` are the
end-to-end "AI result lands in an editable project" paths. They share `harness/job_client.py`
(WP-0133). **CI never runs them**, so a bug that broke all three (a wrong `urllib.request.Request`
call in job_client.py) could only be caught by hand. This package makes CI run them on every push,
using the CI-safe mock backends (no ML models, no GPU, no network).

## Spec — what "correct" means
Add one new CI step (or job) to `.github/workflows/ci.yml` that, on a fresh checkout:
1. Builds `build/opd` (reuse the existing `core` job's build — add the step to the `core` job,
   after "Golden renders", so `build/opd` already exists; it then runs on both x64 and arm64).
2. Installs uv (`astral-sh/setup-uv@v5`, `python-version: "3.12"`) so `uv run opsidecar` works, and
   runs `cd ai-sidecar && uv sync` before the glue scripts.
3. Runs a new script `harness/test_glue_scripts.sh` (you write it) that, in a `mktemp -d` dir:
   - copies `tests/fixtures/goldens/demo.opj` **and** `tests/fixtures/goldens/kits/` into fresh
     subdirs (the project references `kits/*.wav` relative to itself — without kits, render fails);
   - runs `stem_into_project.sh <proj>` → must exit 0, and `<projdir>/stems/` must contain
     exactly `bass.wav drums.wav other.wav vocals.wav`;
   - runs `midi_into_project.sh <proj> tests/fixtures/goldens/pitched.wav` → exit 0, and
     `<projdir>/ai/transcribed.mid` exists;
   - runs `prompt_to_song.sh "ci smoke" <outdir>` → exit 0, and `<outdir>/final_render.wav` and
     `<outdir>/song.opj` exist;
   - runs each script with `-h` → exit 0;
   - runs `OP_STEM_BACKEND=demucs` with `OP_DEMUCS_PYTHON` unset → must exit **3** (gate convention);
   - runs `midi_into_project.sh` with no args → must exit **2**;
   - **determinism**: runs the midi case twice from fresh copies and `cmp`s the two resulting
     `.opj` files → must be identical;
   - prints `PASS <case>` / `FAIL <case>` per case and `test_glue_scripts: N failures`, exits
     nonzero if any case failed (same output style as `harness/test_cli_errors.sh` — read it).
   - Unset `OP_TRANSCRIBE_PYTHON OP_DEMUCS_PYTHON OP_GEN_BACKEND OP_STEM_BACKEND OP_TRANSCRIBE_BACKEND`
     at the top so a dev machine with real backends configured still tests the mock path.

## Acceptance criteria
- [x] `bash harness/test_glue_scripts.sh` exits 0 locally and prints `test_glue_scripts: 0 failures`. (full output in handoff)
- [x] **Negative control:** temporarily break job_client.py (e.g. change `"/jobs"` to `"/jobz"`
      in `submit`), confirm the script reports FAIL and exits nonzero, then revert. Paste both outputs
      in the handoff report. Do NOT commit the broken version. (done + reverted; outputs in handoff)
- [x] After the run, no `opsidecar` process is left running (`ps -eo comm | grep -cx opsidecar` → 0). (verified in the same PID namespace as the suite run — CI sandboxes may isolate per-step otherwise)
- [x] `.github/workflows/ci.yml` runs the new script in the `core` job on both matrix entries. (step order + matrix verified with a YAML parse, see handoff)
- [x] Existing checks still pass: `./build/op_tests`, `bash harness/run_goldens.sh`,
      `bash harness/test_cli_errors.sh`, `cd ai-sidecar && uv run pytest -q && uv run ruff check .` (outputs in handoff)
- [x] ROADMAP.md: add a row for WP-0134 directly above the WP-0133 row.

## Out of scope — do NOT touch
- `harness/job_client.py`, the three glue scripts, `ai-sidecar/`, `src/` — except the temporary,
  reverted negative-control edit above. If one of them needs a fix to pass, STOP and write it as
  `PROPOSE:` in the handoff report instead.
- Golden/fixture files under `tests/fixtures/` (read-only).
- `TASKS/WP-0132-gui-spike.md` (untracked, someone else's).
- `ai-sidecar/ai/transcribed.mid` (untracked stray; leave it).

## Git
- Work on a new branch `wp-0134-ci-glue` created from `main`. Commit message:
  `WP-0134: run glue scripts in CI (mock backends) + test_glue_scripts.sh`.
  Include this task file (`TASKS/WP-0134-ci-glue-scripts.md`, with your handoff report filled in)
  in the commit.
- **Do not push and do not merge.** The reviewer will tell you when to push.

## Notes / landmines
- Jobs run in the sidecar launched by `job_client.start_sidecar`; a cold `uv` env can take ~30–60 s
  on first start — don't add shorter timeouts.
- Use `set -uo pipefail` (not `-e`) and capture `$?` per case, like `test_cli_errors.sh`.
- Use `"${VAR:?}"` in any `rm -rf` path, or just rely on `trap 'rm -rf "$TMP"' EXIT` with a literal
  `mktemp -d` result.

---
## Handoff report — completed by the agent
- **What changed:**
  - **New `harness/test_glue_scripts.sh`** (mode 755): CI smoke for the three glue scripts. Unsets
    `OP_TRANSCRIBE_PYTHON OP_DEMUCS_PYTHON OP_GEN_BACKEND OP_STEM_BACKEND OP_TRANSCRIBE_BACKEND` at the top,
    works inside one `mktemp -d` (`trap 'rm -rf "$TMP"' EXIT`), copies `demo.opj` + `kits/` into fresh
    subdirs per case. 14 assertions: stem/midi/prompt happy paths (exit 0 + exact artifact sets:
    `stems/{bass,drums,other,vocals}.wav`, `ai/transcribed.mid`, `final_render.wav`+`song.opj`),
    `-h` on all three → 0, `OP_STEM_BACKEND=demucs` without `OP_DEMUCS_PYTHON` → 3,
    `midi_into_project.sh` no-args → 2, and determinism (two fresh-copy midi runs, `cmp -s` the `.opj`s).
    Style follows `test_cli_errors.sh`: `set -uo pipefail`, per-case `$?`, `PASS/FAIL <case>`,
    `test_glue_scripts: N failures`, nonzero exit on any failure. On FAIL the captured stdout+stderr of
    the case is echoed indented (CI-log friendliness).
  - **`.github/workflows/ci.yml`**: in the `core` job, immediately after "Golden renders" (so `build/opd`
    exists): `- uses: astral-sh/setup-uv@v5` with `{ python-version: "3.12" }`, then step
    **"Glue scripts e2e (mock backends)"** = `cd ai-sidecar && uv sync` + `cd .. && bash harness/test_glue_scripts.sh`.
    Inside the matrix job ⇒ runs on both linux-x64 and linux-arm64.
  - **`ROADMAP.md`**: WP-0134 row added directly above the WP-0133 row.
  - This task file: acceptance boxes ticked, `status: ready → done`, handoff filled.
  - No product behavior changed ⇒ no `docs/spec/` update, no ADR, no demo artifact (no audio-output change).
    Out-of-scope files untouched; the negative-control edit to `harness/job_client.py` was reverted with
    `git restore` and verified (`"/jobs"` back on line 70); broken version NOT committed.
- **How verified:** (commands + actual output)

  Rig: DGX Spark aarch64, g++ 13, Python 3.12.3, uv-managed sidecar, warm `ai-sidecar/.venv`.
  ⚠️ Env note: this agent container mounts `~/.cache` read-only, so every local run below exports
  `UV_CACHE_DIR=<workspace>/.uv-cache` (a sandbox artifact — first attempt without
  it failed with uv `Read-only file system (os error 30) at ~/.cache/uv/...`, which is NOT a product bug;
  CI runners write to the default cache, and neither the script nor the workflow sets this variable).

  `$ bash harness/test_glue_scripts.sh` (warm run, wall time 12 s):
  ```
  PASS stem_exit0 (exit 0)
  PASS stem_files (exactly: bass.wav drums.wav other.wav vocals.wav )
  PASS midi_exit0 (exit 0)
  PASS midi_artifact
  PASS prompt_exit0 (exit 0)
  PASS prompt_artifacts
  PASS help_stem (exit 0)
  PASS help_midi (exit 0)
  PASS help_prompt (exit 0)
  PASS demucs_gate (exit 3)
  PASS midi_usage (exit 2)
  PASS det_run1 (exit 0)
  PASS det_run2 (exit 0)
  PASS determinism (opj byte-identical)
  test_glue_scripts: 0 failures
  SCRIPT_EXIT=0
  leftover opsidecar count: 0     # `ps -eo comm | grep -cx opsidecar`, run in the SAME shell/PID namespace as the suite
  ```

  Existing checks, same session:
  ```
  $ ./build/op_tests                          → ALL PASS (0 failures)        exit=0   (207 PASS lines, incl. fuzz)
  $ bash harness/run_goldens.sh               → run_goldens: 6 fixtures, 0 failures  exit=0  (incl. negative-control)
  $ bash harness/test_cli_errors.sh           → test_cli_errors: 0 failures          exit=0
  $ cd ai-sidecar && uv run pytest -q         → 102 passed, 2 skipped, 57 warnings in 11.29s   exit=0
  $ cd ai-sidecar && uv run ruff check .      → All checks passed!                   exit=0
  ```

  CI workflow validated by parsing (GitHub runners can't be exercised pre-push):
  ```
  core job steps:
    - actions/checkout@v4
    - lukka/get-cmake@latest
    - Build (determinism flags enforced by CMakeLists; ADR-0004)
    - Unit tests
    - Golden renders (bit-exact vs committed fixtures)
    - astral-sh/setup-uv@v5
    - Glue scripts e2e (mock backends)
    - Negative control — corrupt fixture must FAIL the harness
  golden idx=4, glue idx=6 -> glue after goldens: True
  core matrix entries: ['linux-x64', 'linux-arm64']
  YAML OK
  ```

- **Negative control output:** `submit` in `harness/job_client.py` edited `"/jobs"` → `"/jobz"`, suite re-run:
  ```
  FAIL stem_exit0: want 0 got 1
      Traceback (most recent call last):
        File "<stdin>", line 8, in <module>
        File ".../harness/job_client.py", line 70, in submit
          resp = req("POST", base + "/jobz", {"backend": backend, "task": task, "params": params})
                 ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
        File ".../harness/job_client.py", line 65, in req
          with urllib.request.urlopen(rq) as r:
               ^^^^^^^^^^^^^^^^^^^^^^^^^^
        ...
      urllib.error.HTTPError: HTTP Error 404: Not Found
  FAIL stem_files: want [bass.wav drums.wav other.wav vocals.wav ] got []
  FAIL midi_exit0: want 0 got 1        # identical HTTPError 404 traceback at job_client.py line 70 (submit)
  FAIL midi_artifact: missing /tmp/tmp.TmRmk62Z8Y/midi/ai/transcribed.mid
  FAIL prompt_exit0: want 0 got 1      # identical traceback, same root cause
  FAIL prompt_artifacts: missing /tmp/tmp.TmRmk62Z8Y/song/final_render.wav /tmp/tmp.TmRmk62Z8Y/song/song.opj
  PASS help_stem (exit 0)
  PASS help_midi (exit 0)
  PASS help_prompt (exit 0)
  PASS demucs_gate (exit 3)
  PASS midi_usage (exit 2)
  FAIL det_run1: want 0 got 1          # identical traceback
  FAIL det_run2: want 0 got 1          # identical traceback
  PASS determinism (opj byte-identical)   # honest caveat: vacuous here — both .opj were untouched copies, so cmp passed; the failing det_run1/2 carry the signal
  test_glue_scripts: 8 failures
  SCRIPT_EXIT=1
  leftover opsidecar count: 0
  ```
  Reverted with `git restore harness/job_client.py` (grep confirms line 70 back to `"/jobs"`; `git status` shows
  the file unmodified), then re-ran clean — output identical to the green run above (`test_glue_scripts: 0 failures`,
  exit 0, leftover count 0). The broken version is not in the commit.

- **Metrics:** 14 assertions/case-set; ~12 s warm (5 sidecar launches), cold uv env covered by job_client's 60 s port wait;
  negative control detected all 5 e2e breakages (8 failed assertions) and exited nonzero.

- **Follow-ups noticed (not done):**
  - The CI step could also assert `ps -eo comm | grep -cx opsidecar` after the script (the leak check I ran
    locally); left out to keep the step exactly as specified in the WP.
  - `harness/roundtrip_eval.sh` and `harness/sidecar_smoke.sh` still carry their own lifecycle copies
    (WP-0133 deliberately kept smoke tests unconverted) — untouched, consistent with that decision.

- **PROPOSE / BLOCKED:** None. Nothing outside scope needed a change; all three glue scripts passed as-is on mock backends.
