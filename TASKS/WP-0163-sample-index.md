---
id: WP-0163
title: opd samples — deterministic JSON index of a sample directory (backend for the sample browser)
phase: 1
status: ready
depends_on: []
estimate_agent_hours: 1.0
human_review: none
---

## Context — why this exists
The prototype's sample browser needs a fast, deterministic listing of the audio files in a folder with the
facts a browser shows (format, rate, channels, length, peak) plus a stable content hash to key caches.
`opd samples <dir> [-o index.json]` produces it. Readers already exist: `op::wav::read_audio` (WAV and
AIFF, dispatch by file signature) in `src/engine/wav.hpp`.

## Spec — normative
Create `docs/spec/sample-index.md` containing the following (copy it verbatim, plus a short "Files" list).

### Files
`src/engine/sample_index.hpp`, `src/engine/sample_index.cpp` (add the .cpp to the `opengine` sources in
`CMakeLists.txt`), `src/cli/main.cpp` (subcommand + usage line), tests in `tests/test_main.cpp`.

### API (exactly this, namespace `op::sample_index`, header `engine/sample_index.hpp`)
```cpp
// 64-bit FNV-1a over `n` bytes (offset basis 14695981039346656037, prime 1099511628211), as exactly
// 16 lowercase hex digits. Example: fnv1a64_hex("", 0) == "cbf29ce484222325".
std::string fnv1a64_hex(const unsigned char* data, std::size_t n);

// Scans `dir` and returns the index JSON text (format below, ending in "\n").
// Returns false and sets `err` only if `dir` does not exist or is not a directory (err = the dir path).
// Unreadable/invalid audio files never fail the call; they go under "errors".
bool build_sample_index(const std::string& dir, std::string& json_out, std::string& err);
```

### Which files are scanned
- Recurse into all subdirectories. Any entry (file or directory) whose NAME starts with `.` is skipped
  entirely (a hidden directory is not descended into).
- Symlinks are NOT followed and NOT listed: a symlink to a file or to a directory is skipped. Only regular
  files (`std::filesystem::is_regular_file` on the non-followed status) are candidates.
- A regular file is a candidate iff its extension, ASCII-lower-cased, is one of `.wav`, `.aif`, `.aiff`,
  `.aifc`. Everything else is ignored silently (neither listed nor an error). The format is decided by the
  reader (`read_audio` dispatches by signature), not by the extension.
- Candidate paths are reported relative to `dir`, with `/` separators, no leading `./`.

### Per-file facts
Load with `op::wav::read_audio(full_path)`. On `ok`: `format` = `rr.format` ("wav" or "aiff"),
`sample_rate`, `channels`, `frames` = `samples.size() / channels`, `duration_s` = frames / sample_rate
computed in double and printed `%.6f`, `peak_dbfs` = `20 * log10(peak)` where peak = max |sample| over all
samples (as double) computed with `op::dsp::log10d_` and printed `%.3f`; if peak == 0 (all-silent or zero
frames) `peak_dbfs` is the JSON literal `null`. `hash` = `fnv1a64_hex` over the raw bytes of the file
(whole file, read in binary). If the file cannot be read for hashing, treat it as an error with the text
`cannot read file`. On `!ok`: the entry goes to `errors` with `error` = `rr.error` verbatim.
Number printing uses the "C" locale (`snprintf`), never streams with imbued locales.

### Output (byte-exact, single line + trailing newline, keys in this order, no spaces)
```
{"version":1,"files":[{"path":"a/b.wav","format":"wav","sample_rate":48000,"channels":2,"frames":4800,"duration_s":0.100000,"peak_dbfs":-6.021,"hash":"0123456789abcdef"}],"errors":[{"path":"bad.wav","error":"<reader error>"}]}
```
- `files` sorted by `path` with plain byte-wise `std::string` comparison (ascending); `errors` likewise.
- Empty cases: `"files":[]` / `"errors":[]`.
- String escaping (path, error): `"`→`\"`, `\`→`\\`, bytes < 0x20 → `\u00XX` (lowercase hex, 4 digits);
  all other bytes (including UTF-8) pass through raw.

### CLI
`opd samples <dir> [-o <index.json>]`
- Without `-o`: JSON to stdout. With `-o`: JSON written to the file (binary, same bytes), stdout empty.
- Exit codes: 0 success (even if `errors` is non-empty); 2 usage (missing dir, unknown option, `-o`
  without value, extra positional arg) via the existing `usage()`+`fail("usage", ...)` pattern; 3 `<dir>`
  missing or not a directory (`fail("not_found", dir)`); 5 cannot write the `-o` file (`fail("write", ...)`).
  On exit 2/3, nothing on stdout and no output file is created.
- Add a usage line: `  opd samples <dir> [-o <index.json>]   (JSON index of the audio files under a directory)`.
- Determinism: same tree in → byte-identical output; no clock, no randomness, no dependence on directory
  iteration order (sort explicitly).

## Acceptance criteria
- [ ] Implementation exactly as specified; `docs/spec/sample-index.md` written.
- [ ] Unit tests in `tests/test_main.cpp` (≥ 6 new CHECKs, using a per-test unique temp dir:
      `std::filesystem::temp_directory_path()` + name containing the pid; remove it afterwards):
      FNV known vectors (`""` → `cbf29ce484222325`, `"a"` → `af63dc4c8601ec8c`); a tree with nested
      dirs, a hidden file, a hidden dir, a non-audio file, a bad `.wav`, and an AIFF → exact expected JSON;
      sorted order; peak `null` for silence; symlink skipped; nonexistent dir → false.
- [ ] CLI cases covered in `harness/test_cli_errors.sh` (append: usage_samples_no_dir → 2,
      samples_missing_dir → 3, samples_bad_option → 2).
- [ ] No libm calls outside `dsp.hpp` wrappers; no new dependencies; 0 warnings (`-Werror`).
- [ ] All checks green: build, `op_tests`, `harness/run_goldens.sh`, `harness/test_cli_errors.sh`,
      `harness/test_glue_scripts.sh`, and a sanitizer build (`-DDZ_SANITIZE=ON`) of `op_tests`. Build with `-j4` max.

## Out of scope — do NOT touch
- `src/app/`, existing fixtures/goldens, `ai-sidecar/`, `docs/adr/`, other spec files.
- **Do not edit ROADMAP.md** (the elephant adds rows at ship time).
- Waveform thumbnails, caching/incremental indexing, tags/BPM detection, other audio formats.

## Git
- Branch `wp-0163-sample-index` from `main`. Commit: `WP-0163: opd samples — deterministic JSON sample index`.
  Stage explicit paths only (never `git add -A` / `git add .`). Include this task file with the handoff filled.
- **Do not push or merge.**

---
## Handoff report — completed by the agent
- **What changed:**
  - `docs/spec/sample-index.md` — new spec, copied verbatim from this task file (verified with `diff`, see below), plus a title/status preamble.
  - `src/engine/sample_index.hpp` / `src/engine/sample_index.cpp` — new `op::sample_index`: `fnv1a64_hex` and `build_sample_index`. Recursive scan on non-followed statuses (`symlink_status`), hidden entries skipped by name, candidate extensions `.wav/.aif/.aiff/.aifc` (ASCII lower-cased), one explicit byte-wise sort of relative paths, then files/errors split preserves that order. Per-file facts via `op::wav::read_audio`; peak computed without libm (explicit sign flip); only math call in the TU is `op::dsp::log10d_`. Numbers via `snprintf` ("C" locale). Each candidate file is read once for audio + once for hashing (no double classification pass).
  - `CMakeLists.txt` — `src/engine/sample_index.cpp` added to the `opengine` sources.
  - `src/cli/main.cpp` — `cmd_samples` + dispatch + usage line (exact text from spec). Index is built before any output file is opened, so exit 2/3 never creates the `-o` file; stdout via `fwrite`; `-o` written binary.
  - `tests/test_main.cpp` — `test_sample_index()` with **9 new CHECKs** in a per-test unique temp dir (`temp_directory_path()/op_samples_wp0163_<pid>`, removed afterwards). Fixture bytes are explicit LE/BE assembly; the three file hashes embedded in the expected JSON were computed with an independent Python FNV-1a implementation over those exact byte strings (not by the code under test), so `samples.tree_exact_json` is not self-referential. Added `#include <unistd.h>` for `getpid()` and the `engine/sample_index.hpp` include.
  - `harness/test_cli_errors.sh` — appended WP-0163 section: the three required cases (`usage_samples_no_dir`→2, `samples_missing_dir`→3, `samples_bad_option`→2) plus `-o` without value→2, extra positional→2, exit-3 stdout-empty + no output file, two-run byte-identical determinism, and `-o` file bytes == stdout bytes with empty stdout.
  - No other files touched; nothing outside scope changed.
- **How verified:** (all commands run on this aarch64 rig; full logs kept locally)
  - Build, `-Werror`, zero warnings: `cmake --build build -j4` → `[100%] Built target op_tests` (no warning lines). Sanitizer build likewise clean (see below).
  - Unit tests: `./build/op_tests` →
    ```
    PASS samples.fnv1a_empty
    PASS samples.fnv1a_a
    PASS samples.tree_build_ok
    PASS samples.tree_exact_json
    PASS samples.symlinks_hidden_and_nonaudio_absent
    PASS samples.empty_dir_arrays
    PASS samples.determinism_identical_rerun
    PASS samples.nonexistent_dir_false
    PASS samples.file_as_dir_false
    ALL PASS (0 failures)
    ```
  - Independent oracle for the exact JSON: the same fixture tree was rebuilt byte-for-byte in Python and `./build/opd samples <tree>` output compared against the expected string → `MATCH` (exit 0). FNV known vectors also asserted inside the independent Python implementation (`""`→`cbf29ce484222325`, `"a"`→`af63dc4c8601ec8c`).
  - Goldens: `./harness/run_goldens.sh` → `run_goldens: 9 fixtures, 0 failures`.
  - CLI contracts: `./harness/test_cli_errors.sh` →
    ```
    PASS usage_samples_no_dir (exit 2)
    PASS samples_missing_dir (exit 3)
    PASS samples_missing_dir_no_output
    PASS samples_err_stdout_empty (exit 3)
    PASS samples_bad_option (exit 2)
    PASS samples_o_no_value (exit 2)
    PASS samples_extra_arg (exit 2)
    PASS samples_determinism (byte-identical JSON)
    PASS samples_o_file_matches_stdout (stdout empty)
    test_cli_errors: 0 failures
    ```
  - Glue e2e: `UV_CACHE_DIR=/home/shubhankar_dgx/qwen-scratch/.cache/uv ./harness/test_glue_scripts.sh` → `test_glue_scripts: 0 failures`.
  - Sanitizers (`-DDZ_SANITIZE=ON`, ASAN+UBSAN, `-j4`): `cmake --build build-san -j4 && ./build-san/op_tests` → `ALL PASS (0 failures)` (same 9 sample CHECKs pass under sanitizers).
  - Spec verbatim: `diff <(sed -n '20,74p' TASKS/WP-0163-sample-index.md) <(tail -n +8 docs/spec/sample-index.md)` → empty (`VERBATIM OK`).
  - No libm outside `dsp.hpp`: grep of `src/engine/sample_index.cpp` for math symbols finds only `op::dsp::log10d_`.
- **Metrics:** +2 engine files (143 lines .cpp), CLI +31 lines, tests +139 lines / 9 CHECKs, harness +13 checks; audio output unchanged (no demo artifact needed — index is text).
- **PROPOSE / BLOCKED:** nothing blocked. `PROPOSE:` if the sample browser later needs mtime-based incremental indexing, extend the format with a `"version":2` rather than changing v1 bytes.
