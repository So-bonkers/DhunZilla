# WP-0132 — Glue script dedup (shared sidecar library)

**Status: done** · Owner: orchestrator · Behavior-preserving refactor inside harness/ only.

## Scope
Extract the copy-pasted serve-start/port-scan/submit/poll lifecycle from stem_into_project.sh,
midi_into_project.sh, prompt_to_song.sh into `harness/job_client.py` (stdlib-only dev tooling);
add `-h/--help` to all three. sidecar_smoke.sh deliberately NOT converted — it tests the raw HTTP
surface explicitly and should keep its own copies (no shared-code blind spots in a smoke test).

## Acceptance — evidence
- **Behavior preservation, byte-proven:** pre-refactor baseline runs captured first; post-refactor
  fresh runs → `cmp` clean on stem result .opj, midi result .opj, prompt-song .opj AND final_render.wav
  ("STEM/MIDI/PROMPT-SONG BYTE-IDENTICAL"). Real-mode smoke: basicpitch@… ONNX through the new path ✓.
- Gate convention intact (demucs-without-runtime → exit 3). All three --help OK.
- ~90 lines of duplicated lifecycle removed; one place to update when the API evolves.

## Handoff report
**What changed:** new harness/job_client.py (~100 lines incl. docstrings); three scripts rewired onto
it (python phases shrank dramatically); help flags; ROADMAP. No engine/sidecar/C++ changes.
**Bug caught by the byte-comparison discipline itself:** my first lib draft called
`urllib.request.Request(method, url, data=…)` — positional arg 1 is *url*, so submit() blew up with
"multiple values for argument 'data'". Ten minutes from writing to caught, because the e2e proof ran
immediately after. Refactors without byte-level before/after captures are just vibes.
**Test-loop gotchas logged:** my verification loop dropped the `.sh` extension (false "HELP BROKEN")
and an earlier baseline diff compared a mutated-after-stem file against a fresh one — both were test
harness errors, not product bugs; noted so future-me diffs FINAL states from FRESH starts.
