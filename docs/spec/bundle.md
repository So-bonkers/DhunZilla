# `.opd` project bundle — spec v0 (WP-0110, 2026-11-18)

One file containing a project + every sample it references. Container: **store-only ZIP**
(method 0), hand-written, zero new dependencies; validated against Info-ZIP `unzip -t`.

- **Layout:** main `.opj` at archive root (its own filename), then sampler assets under their
  project-relative paths (`kits/kick.wav`). Entries ordered: opj first, assets sorted — fixed
  DOS timestamp 1980-01-01 → **byte-deterministic packs**.
- **Pack:** `opd pack <in.opj> -o <song.opd>`; fails actionable on missing referenced sample or
  absolute/escaping sampler path (must stay inside the project dir to be bundleable).
- **Unpack:** `opd unpack <song.opd> -d <dir>` → prints main project path. Two-phase: *all*
  entries validated (names, method==0, local-header agreement, CRC32) before any byte is written;
  entry names containing `..`, leading `/`, or `\` are rejected outright. Failure leaves no
  partially-restored project.
- Compression may be added later via an ADR-gated vendored lib (zlib/zstd); v0 stays dependency-free.
