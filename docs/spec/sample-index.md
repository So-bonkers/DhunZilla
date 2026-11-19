# `opd samples` — deterministic JSON index of a sample directory (spec)

Status: normative (WP-0163). Backend for the prototype's sample browser: a fast, deterministic listing of
the audio files in a folder with the facts a browser shows (format, rate, channels, length, peak) plus a
stable content hash to key caches. Deterministic: same tree in → byte-identical text out — no clock, no
randomness, no dependence on directory iteration order.

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
