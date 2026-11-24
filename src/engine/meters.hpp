// engine/meters.hpp — per-track and master peak/RMS dBFS + integrated LUFS as JSON (WP-0194).
// Renders the project's stems and full mix IN MEMORY with op::render_stems and measures every buffer
// with the existing op::analyze::analyze_samples. Read-only: nothing is written to disk, the project
// is never modified, and no DSP is added here (all math lives in analyze.cpp). Deterministic.
//
// Example:
//   op::Project p; std::string err;
//   if (!op::load_project("song.opj", p, err)) { /* handle */ }
//   op::meters::Report r;
//   if (op::meters::measure_project(p, "dir/of/song", r, err))
//       std::fputs(op::meters::to_json(r).c_str(), stdout);  // byte-exact deterministic text
#pragma once
#include <optional>
#include <string>
#include <vector>
#include "engine/model.hpp"
#include "engine/wav.hpp"
namespace op::meters {
struct Meter {
    std::optional<double> peak_dbfs;
    std::optional<double> rms_dbfs;
    std::optional<double> integrated_lufs;
};
struct BarMeter {                     // one bar of level history (WP-0243)
    long long bar = 0;                // 0-based bar index
    long long start_sample = 0;       // op::sample_at_bar(p, bar)
    long long end_sample = 0;         // op::sample_at_bar(p, bar + 1.0)
    std::optional<double> peak_dbfs;  // empty = the bar is silent (or empty)
    std::optional<double> rms_dbfs;   // empty = the bar is silent (or empty)
};
struct TrackMeter {
    std::string name;   // Track::name as stored (may be empty)
    Meter meter;
    std::vector<BarMeter> bars;  // WP-0243: per-bar history, filled only by measure_project_per_bar
};
struct Report {
    int sample_rate = 0;
    long long frames = 0;          // frames of the full render (identical for every stem and the master)
    double duration_s = 0.0;       // frames / sample_rate (0.0 when sample_rate <= 0)
    bool per_bar = false;          // WP-0243: true only when measure_project_per_bar filled the bar arrays
    long long bar_count = 0;       // WP-0243: std::ceil(p.length_bars) — the bar count opd timeline prints
    std::vector<TrackMeter> tracks;  // project track order, one entry per track (index = position)
    Meter master;                    // the full mix (includes master_gain_db)
    std::vector<BarMeter> master_bars;  // WP-0243: per-bar history of the mix
};
Meter meter_buffer(const wav::StereoBuffer& buf);
bool measure_project(const Project& p, const std::string& base_dir, Report& out, std::string& err);
// WP-0243: measure frames [start_sample, end_sample) of `buf` with op::analyze::analyze_samples over
// exactly those frames; bar/start_sample/end_sample are copied into the result. Precondition (callers
// pass renderer-mapped values): 0 <= start_sample <= end_sample <= buf.frames().
BarMeter meter_bar(const wav::StereoBuffer& buf, long long bar, long long start_sample, long long end_sample);
// WP-0243: like measure_project (same renders, same errors) plus per-bar peak/RMS for every track and
// the master, using the renderer's own bar→sample mapping (op::sample_at_bar) so the tempo map and
// time signature are honoured; a final partial bar is included and measured up to the end of the render.
bool measure_project_per_bar(const Project& p, const std::string& base_dir, Report& out, std::string& err);
std::string to_json(const Report& r);
}  // namespace op::meters
