# `opd analyze` — objective audio features as JSON (spec)

Status: normative (WP-0144). The AI copilot uses these objective "feature diffs" (loudness, spectral
centroid, peak) to judge a render before/after a change. Everything is deterministic: same file in →
byte-identical text out.

## Files
- `src/engine/analyze.hpp`, `src/engine/analyze.cpp` (`src/engine/analyze.cpp` is part of the `opengine`
  library sources in `CMakeLists.txt`).
- `src/engine/dsp.hpp`: four wrappers right after the `cosd_` line (ADR-0004: libm only via dsp.hpp):
  `inline double tand_(double x){return std::tan(x);}`, `inline double powd_(double x,double y){return std::pow(x,y);}`,
  `inline double log10d_(double x){return std::log10(x);}`, `inline double sqrtd_(double x){return std::sqrt(x);}`.
  analyze.cpp must not call `std::`/`::` math functions directly (use `op::dsp::` wrappers; `+ - * /` and
  comparisons are fine). Spectral analysis additionally uses the existing `sind_`/`cosd_`.
- `src/cli/main.cpp`: subcommand `analyze` (+ usage line).
- `tests/test_main.cpp`: unit tests.

## API (exactly this, namespace `op::analyze`)
```cpp
#pragma once
#include <optional>
#include <string>
#include <vector>
namespace op::analyze {
struct Features {
    int sample_rate = 0;
    int channels = 0;                                   // 1 or 2
    long long frames = 0;                               // samples per channel
    double duration_s = 0.0;                            // frames / sample_rate
    std::vector<std::optional<double>> peak_dbfs_per_channel;  // size == channels; nullopt = silent channel
    std::optional<double> peak_dbfs;                    // max over channels
    std::optional<double> rms_dbfs;
    std::optional<double> integrated_lufs;
    std::optional<double> spectral_centroid_hz;
};
// interleaved float samples in [-1,1] (non-finite samples are treated as 0.0). Precondition:
// channels in {1,2}, sample_rate in [8000, 192000] (the CLI enforces this; analyze_samples need not).
Features analyze_samples(const std::vector<float>& interleaved, int channels, int sample_rate);
// Canonical JSON text, byte-exact format below, ending in "}\n".
std::string to_json(const Features& f);
}  // namespace op::analyze
```

## Definitions (all arithmetic in `double`; sums accumulate sequentially in ascending index order; read
samples as the `float` values from `op::wav::read_wav`, converted to double)
- **peak_dbfs_per_channel[c]** = 20·log10(max_i |x_c[i]|) (sample peak, not true peak); `nullopt` if that
  max is exactly 0 or there are 0 frames. **peak_dbfs** = 20·log10(max over all channels), same null rule.
- **rms_dbfs** = 20·log10(sqrt(S / (frames·channels))), S = sum of x² over all samples of all channels;
  `nullopt` if S == 0 or frames == 0.
- **integrated_lufs** — ITU-R BS.1770-4 / EBU R128, channel weights 1.0 (L, R; mono = one channel):
  1. K-weighting = two cascaded biquads per channel, Direct Form I, zero initial state, run once over the
     whole file, coefficients derived for the file's sample rate fs by the standard bilinear formulas
     (so every rate in 8000..192000 works; at 48000 they reproduce the BS.1770 table values).
     Stage 1 (high shelf): f0 = 1681.974450955533, G = 3.999843853973347 dB, Q = 0.7071752369554196.
       K = tand_(π·f0/fs); Vh = 10^(G/20); Vb = Vh^0.4996667741545416; a0 = 1 + K/Q + K².
       b0 = (Vh + Vb·K/Q + K²)/a0; b1 = 2(K² − Vh)/a0; b2 = (Vh − Vb·K/Q + K²)/a0;
       a1 = 2(K² − 1)/a0; a2 = (1 − K/Q + K²)/a0.
     Stage 2 (high-pass, RLB): f0 = 38.13547087602444, Q = 0.5003270373238773; K = tand_(π·f0/fs);
       b = (1, −2, 1); a1 = 2(K² − 1)/(1 + K/Q + K²); a2 = (1 − K/Q + K²)/(1 + K/Q + K²).
     Difference equation: y[n] = b0·x[n] + b1·x[n−1] + b2·x[n−2] − a1·y[n−1] − a2·y[n−2].
     Sanity values at fs = 48000 (a unit test must assert them to 1e-9): stage 1 b0 = 1.53512485958697,
     b1 = −2.69169618940638, b2 = 1.19839281085285, a1 = −1.69065929318241, a2 = 0.73248077421585;
     stage 2 a1 = −1.99004745483398, a2 = 0.99007225036621. Use π = `op::dsp::kPi_d()`.
  2. Blocks: length B = (4·fs + 5)/10 samples (integer division), hop H = (fs + 5)/10; block j covers
     [j·H, j·H + B); only complete blocks (j·H + B ≤ frames). If there are no blocks → `nullopt`.
  3. z_j = Σ_c (1/B)·Σ_{i in block} y_c[i]²  (weights 1.0); block loudness l_j = −0.691 + 10·log10(z_j)
     (z_j == 0 → l_j = −infinity, i.e. it fails the gate).
  4. Absolute gate: keep blocks with l_j > −70. If none kept → `nullopt`.
  5. Relative gate: Γ = −0.691 + 10·log10(mean of z_j over the kept blocks) − 10; keep blocks (from the
     absolute-gated set) with l_j > Γ.
  6. integrated_lufs = −0.691 + 10·log10(mean of z_j over the finally kept blocks).
- **spectral_centroid_hz**: mono signal m[i] = x[i] (mono) or (L[i] + R[i])/2. Frames of N = 2048 samples,
  hop 1024, frame k starts at k·1024, only complete frames (k·1024 + 2048 ≤ frames); window w[n] = 0.5 −
  0.5·cosd_(2π·n/N) for n = 0..N−1 (periodic Hann). Magnitude spectrum |X[b]| for b = 0..1024 of the
  windowed frame via an iterative radix-2 FFT with twiddles computed directly as
  cosd_(2π·t/N), −sind_(2π·t/N) (no recurrence). Centroid = (Σ_frames Σ_b b·(fs/N)·|X[b]|) /
  (Σ_frames Σ_b |X[b]|) over ALL frames (one global ratio, not a mean of per-frame centroids).
  `nullopt` if there are no complete frames or the denominator ≤ 1e-9.

## CLI
`opd analyze <in.wav> [--json]` — prints the JSON below to stdout (`--json` is accepted and changes
nothing; JSON is the only format). Exit codes/strings follow `fail()` in main.cpp:
- missing argument or unknown flag → usage + `error: usage: ...`, exit 2
- file does not exist → `error: not_found: <path>`, exit 3
- unreadable/invalid WAV (`read_wav` fails) → `error: parse: <read_wav error>`, exit 4
- sample rate outside 8000..192000 → `error: analyze: unsupported sample rate <n>`, exit 5
Nothing is printed to stdout on error. `usage()` carries the line
`  opd analyze <in.wav> [--json]   (objective audio features as JSON)`.

## Output format (byte-exact; `to_json`)
```
{
  "sample_rate": 48000,
  "channels": 2,
  "frames": 480000,
  "duration_s": 10.000000,
  "peak_dbfs": -6.0206,
  "peak_dbfs_per_channel": [-6.0206, -9.0309],
  "rms_dbfs": -23.0103,
  "integrated_lufs": -23.0021,
  "spectral_centroid_hz": 1000.1234
}
```
Keys in exactly this order, 2-space indent, `\n` line endings, one trailing `\n` after `}`. Integers
(`sample_rate`, `channels`, `frames`) printed plainly; `duration_s` with `%.6f`; every other number with
`%.4f` (use `snprintf` with the C locale-independent `%f`; if the result is `-0.0000` print `0.0000`);
any `nullopt` prints the bare token `null` (silence/too short -> e.g. `"integrated_lufs": null`).
`peak_dbfs_per_channel` is printed inline as `[a, b]` (elements separated by `", "`, nulls allowed). No
file path appears in the output.
