---
id: WP-0140
title: Glue scripts resolve relative project/audio paths against the caller's cwd (+ test)
phase: 1
status: ready
depends_on: []
estimate_agent_hours: 1
human_review: none
---

## Context — why this exists
An untracked stray file `ai-sidecar/ai/transcribed.mid` appeared in the main checkout on 2026-11-18
16:11 and nobody can explain it. Hypothesis (unverified): the pre-WP-0133 glue scripts did
`cd "$ROOT/ai-sidecar"` before their Python phase, so a **relative** project path such as `demo.opj`
resolved under `ai-sidecar/`, and `midi_into_project.sh` wrote `<project dir>/ai/transcribed.mid`
there. The current scripts (via `harness/job_client.py`) no longer `cd`, but relative paths are not
tested anywhere. This package confirms or refutes the hypothesis and pins correct behavior with tests.

## Spec — correct behavior
For `harness/stem_into_project.sh`, `harness/midi_into_project.sh`, `harness/prompt_to_song.sh`:
any relative path argument (project, audio, outdir) is resolved against **the caller's current working
directory**, and every artifact lands next to the project (or in the given outdir) — never under
`ai-sidecar/` or the repo root.

## Acceptance criteria
- [ ] Investigation written in the handoff: check out the pre-WP-0133 script (`git show 384d3fe:harness/midi_into_project.sh`)
      into a temp dir **outside the repo** and show (with commands + output) whether a relative project
      path reproduces a stray `ai-sidecar/ai/transcribed.mid` — then delete anything the reproduction
      created. State the conclusion: hypothesis confirmed / refuted / other cause.
- [ ] If the current scripts still mis-resolve any relative path, fix them (convert args to absolute at the
      top of each script, e.g. `PROJ="$(cd "$(dirname "$PROJ")" && pwd)/$(basename "$PROJ")"` after the
      existence check). If they already behave correctly, change no script.
- [ ] Extend `harness/test_glue_scripts.sh` with a case per script that `cd`s into a temp dir and passes
      **relative** paths (e.g. `cd "$TMP/rel" && bash "$GL/midi_into_project.sh" demo.opj pitched.wav`),
      asserting artifacts land next to the project and that `git -C "$ROOT" status --porcelain` shows
      **no new untracked files** in the repo afterwards.
- [ ] `bash harness/test_glue_scripts.sh` → 0 failures; existing checks green (`op_tests`, goldens, cli_errors).
- [ ] Do **not** delete the existing stray `ai-sidecar/ai/` directory in the main checkout — just report on it.
- [ ] ROADMAP.md: WP-0140 row at the top of the table.

## Out of scope — do NOT touch
- `harness/job_client.py` behavior other than path handling (if you believe it needs changes, PROPOSE).
- `src/`, `tests/fixtures/`, `ai-sidecar/` source, `docs/adr/`.

## Git
- Branch `wp-0140-glue-relative-paths` from `main`. Commit: `WP-0140: glue scripts resolve relative paths against caller cwd (+ tests)`.
  Stage explicit paths only (never `git add -A` / `git add .`). Include this task file with the handoff filled.
- **Do not push or merge.**

---
## Handoff report — completed by the agent
- **Investigation & conclusion:** hypothesis **confirmed**. Old script `git show 384d3fe:harness/midi_into_project.sh`
  placed in a temp root outside the repo (`<T>/harness/`, a copy of `ai-sidecar/` as `<T>/ai-sidecar/`, `<T>/build`
  → real build), run from `<T>/p` (demo.opj + kits + pitched.wav):
  - `midi_into_project.sh demo.opj pitched.wav` → job fails `mocktrans: input file not found: pitched.wav`, no stray
    (the script `cd`s into ai-sidecar/ before the job, so the relative audio is looked up there).
  - `midi_into_project.sh demo.opj <abs>/pitched.wav` → transcription succeeds, MIDI copied to
    `Path("demo.opj").parent / "ai/transcribed.mid"` = **`<T>/ai-sidecar/ai/transcribed.mid`** (105 bytes), then
    `import-midi failed: error: parse: cannot open 'demo.opj'`, exit 1. `cmp` with the stray in the main checkout:
    **identical**. So the stray came from an earlier relative-project/absolute-audio run of the pre-WP-0133 script.
    The reproduction's temp root was deleted; the main checkout's stray `ai-sidecar/ai/` was left untouched as required.
- **What changed:** current scripts no longer `cd`, but `harness/job_client.py` starts the sidecar with
  `cwd=ai-sidecar/` and passes audio paths through, so relative audio still failed:
  `midi_into_project.sh demo.opj pitched.wav` and `stem_into_project.sh demo.opj pitched.wav` (from a project dir)
  → `input file not found: pitched.wav — pass a path the sidecar process can read`. Fixed by converting the
  project and audio arguments to absolute paths after the existence checks in both scripts (`abspath()` helper).
  `prompt_to_song.sh` already handled a relative outdir correctly → unchanged. `harness/test_glue_scripts.sh`: one
  relative-path case per script (`env -C <projdir> … demo.opj pitched.wav` / `songout`), artifact checks, and
  `git -C "$ROOT" status --porcelain` before == after. ROADMAP row.
- **How verified:** `bash harness/test_glue_scripts.sh` → `rel_stem_exit0`, `rel_stem_artifacts`, `rel_midi_exit0`,
  `rel_midi_artifact`, `rel_prompt_exit0`, `rel_prompt_artifacts`, `rel_no_repo_strays` PASS, `0 failures`.
  Negative control: with the pre-fix `midi_into_project.sh` restored, `rel_midi_exit0` and `rel_midi_artifact` FAIL
  (2 failures) — the tests catch the bug. `op_tests` ALL PASS · `run_goldens.sh` 0 failures · `test_cli_errors.sh` 0 failures.
- **PROPOSE / BLOCKED:** PROPOSE: `job_client.submit()` could absolutize `*_path` params itself so future callers
  can't hit this; out of scope here. Completed by the orchestrator (goldfish run paused by the owner).
