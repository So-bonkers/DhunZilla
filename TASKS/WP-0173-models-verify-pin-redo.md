---
id: WP-0173
title: Model manager - offline `verify` and `pin` (turn placeholder hashes into real pins)
phase: 3
status: ready
depends_on: [WP-0005]
estimate_agent_hours: 1.0
human_review: none
---

## Context - why this exists
`ai-sidecar/models/manifest.json` carries placeholder digests (`hash_verified: false`) and its notes tell the
operator to "run scripts/pin_hashes" - which does not exist. Plan section 5b/6 requires hash-pinned weights.
This package adds the missing offline tooling: `verify` (are the cached files what the manifest pins?) and
`pin` (measure local files and write the real digest + size into the manifest, setting `hash_verified`).
Both work on local files only. The normative text is the new section "Offline verify and pin (WP-0173)" at
the end of `docs/spec/model-manifest.md` (already updated in the repo; read it, do not edit it).

## Note — second attempt
A previous implementation of this package (branch `wp-0173-models-verify-pin`, do NOT reuse it) failed two
acceptance probes: (1) `verify` when EVERY file of the model is missing must still print the full JSON report
(`ok: false`, one entry per manifest file in manifest order, each `status: "missing"`, `actual_sha256: null`)
and exit 1; (2) `pin` must print exactly the JSON object specified below (`id` + `pinned` list in manifest
order, each `{file, sha256, size_bytes}`) on success. Re-read those parts of the spec carefully.

## Spec - exact interface
All paths below are under `ai-sidecar/`.

`src/ai_sidecar/model_manager.py` (additive; do not change existing behaviour, exceptions' codes, or `download`)
- `import json` at the top.
- `class WeightsMissing(ModelManagerError)` with `error_code = "weights_missing"` (place it with the other errors).
- `def sha256_file(path: Path) -> str` (module level): streaming sha256 hex digest, `CHUNK_SIZE` chunks.
- `ModelManager.verify(self, model_id: str) -> dict[str, Any]` and
  `ModelManager.pin(self, model_id: str, *, from_dir: Path | None = None) -> dict[str, Any]` exactly as the
  spec section says (keys, statuses, ordering, exceptions). Reuse `get_entry`, `cache_root`, `filename_for`.
  `pin` rewrites the manifest with `json.dumps(raw, indent=2) + "\n"` into `<manifest>.tmp` then
  `os.replace`; it edits only `sha256`, `size_bytes`, `hash_verified` of that entry's sources, and it
  checks that ALL files exist BEFORE writing anything.

`src/ai_sidecar/cli.py`
- New subcommand tree `opsidecar models verify <model_id> [--manifest PATH]` and
  `opsidecar models pin <model_id> [--manifest PATH] [--from-dir DIR]` (`models_command` required, so bare
  `opsidecar models` exits 2 via argparse). Handler `_models(args)`: build `ModelManager(Path(args.manifest)
  if args.manifest else None)`; on any `ModelManagerError` print `error: <exc.error_code>: <message>` to
  stderr, nothing to stdout, return 1; otherwise print `json.dumps(outcome, indent=2, sort_keys=True)` to
  stdout and return 0 (for `verify`, return 1 when `outcome["ok"]` is false).

`tests/test_model_verify_pin.py` (new): at least 6 tests, tmp manifests only (see `tests/conftest.py`
`write_manifest` / `tests/test_model_manager.py` for fixtures), no network: verify each of
`missing / ok / hash_mismatch / size_mismatch`; pin writes digest+size+`hash_verified` and leaves every other
byte-field of the manifest equal; pin with a missing file leaves the manifest byte-identical;
`--from-dir`; awaiting-weights refusal; CLI exit codes and the `error: <code>:` stderr line.

## Acceptance criteria
- [ ] Everything above implemented; existing sidecar tests unchanged and green
      (`cd ai-sidecar && UV_CACHE_DIR=<workspace>/.cache/uv uv run pytest -q`), `uv run ruff check .` clean.
- [ ] The bundled `ai-sidecar/models/manifest.json` is NOT modified (pinning real digests is a human step).
- [ ] New tests as above.

## Out of scope - do NOT touch
- `ROADMAP.md`, `src/` (C++), `tests/` (repo root), `harness/`, `.github/`, `docs/` (spec already updated),
  `ai-sidecar/models/manifest.json`, `ai-sidecar/backends/`, `jobs.py`, `app.py`, `base.py`.
- No network access, no new HTTP endpoint, no change to the download gate rules.

## Git
- Branch `wp-0173b-models-verify-pin` from `main`. Commit: `WP-0173: model manager offline verify + pin`.
  Stage explicit paths only (never `git add -A` / `git add .`). Include this task file with the handoff filled.
- **Do not push or merge.**

---
## Handoff report - completed by the agent
- **What changed:** (branch `wp-0173b-models-verify-pin`, additive-only — `git diff --stat main` reports
  213 insertions, 0 deletions)
  - `ai-sidecar/src/ai_sidecar/model_manager.py`: `import json`; new error `WeightsMissing`
    (`error_code = "weights_missing"`) beside the other errors; module-level streaming
    `sha256_file(path)` (CHUNK_SIZE blocks); `ModelManager.verify(model_id)` and
    `ModelManager.pin(model_id, *, from_dir=None)` per the spec section — verify never raises for
    absent files (full report, one entry per source in manifest order), pin measures all files
    first, raises `WeightsMissing` before any write, otherwise rewrites the raw JSON via
    `<manifest>.tmp` + `os.replace` with `json.dumps(raw, indent=2) + "\n"`, editing only
    `sha256`/`size_bytes`/`hash_verified` of that entry's sources. Both refuse `awaiting-weights`
    (`ModelAwaitingWeights`, "nothing to verify"/"nothing to pin"); no license gate, no network.
  - `ai-sidecar/src/ai_sidecar/cli.py`: `models verify|pin` subcommand tree (`models_command`
    required → bare `opsidecar models` exits 2) + handler `_models` (JSON on stdout indent 2 sorted
    keys; verify exit 1 when `ok` false; `ModelManagerError` → `error: <code>: <msg>` on stderr,
    nothing on stdout, exit 1). Docstring usage examples added.
  - `ai-sidecar/tests/test_model_verify_pin.py`: new, 17 tests, tmp manifests only, URLs on the
    RFC-2606 `.invalid` host so accidental network use fails loudly.
  - `docs/spec/model-manifest.md`: runner-installed spec section included unchanged (not reworded).
- **How verified:** (all commands run in `ai-sidecar/` with
  `export UV_CACHE_DIR=/home/shubhankar_dgx/qwen-scratch/.cache/uv`)
  - Full suite green, existing sidecar tests unchanged:
    ```
    $ uv run pytest -q
    136 passed, 2 skipped, 91 warnings in 15.30s
    SKIPPED [1] tests/test_basicpitch_adapter.py:100: OP_TRANSCRIBE_PYTHON unset or basic_pitch/onnxruntime not importable there
    SKIPPED [1] tests/test_demucs_adapter.py:103: OP_DEMUCS_PYTHON unset or demucs not importable there   # both pre-existing
    $ uv run pytest -q tests/test_model_verify_pin.py
    17 passed in 0.05s
    ```
  - Lint/type gates clean:
    ```
    $ uv run ruff check .
    All checks passed!
    $ uv run mypy src/ai_sidecar/model_manager.py src/ai_sidecar/cli.py   # mypy --strict config
    Success: no issues found in 2 source files
    ```
  - Probe (1) — real CLI, cache dir empty, two sources declared → full report + exit 1:
    ```
    $ uv run opsidecar models verify demo --manifest .../manifest.json ; echo $?
    { "files": [ {"actual_sha256": null, "expected_sha256": "0000…0", "file": "alpha.bin", "status": "missing"},
                 {"actual_sha256": null, "expected_sha256": "0000…0", "file": "beta.bin",  "status": "missing"} ],
      "id": "demo", "ok": false }
    exit code: 1
    ```
  - Probe (2) — real CLI `pin` prints exactly `{id, pinned:[{file, sha256, size_bytes}…]}`, exit 0;
    the rewritten manifest keeps every other field and ends with a newline (`trailing newline: True`);
    a follow-up `verify` on the freshly pinned manifest reports `ok: True statuses: ['ok', 'ok']`, exit 0.
  - CLI contracts: bare `uv run opsidecar models` → `error: the following arguments are required:
    models_command`, exit 2; unknown id → stderr `error: unknown_model: no model 'nope' in manifest;
    known ids: ['demo']`, stdout empty, exit 1 (same shape asserted for `weights_missing`).
  - Bundled manifest untouched, and pin-with-missing-file leaves the manifest byte-identical
    (`git diff --exit-code main -- ai-sidecar/models/manifest.json` → no output; test asserts
    `read_bytes()` equality and that no `manifest.json.tmp` is left behind).
- **Metrics:** +137 lines model_manager.py, +49 cli.py, +330-line new test file (17 tests);
  new-file runtime 0.05 s; no existing test touched.
- **PROPOSE / BLOCKED:** nothing blocked. PROPOSE (out of scope, not done): `pin` writes the measured
  size verbatim, so a 0-byte file would produce `size_bytes: 0`, which the `Source` schema (`gt=0`)
  rejects on next load — a "refuse empty files" guard belongs in a future package if the PO wants one.
