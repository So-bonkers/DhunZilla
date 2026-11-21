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
struct TrackMeter {
    std::string name;   // Track::name as stored (may be empty)
    Meter meter;
};
struct Report {
    int sample_rate = 0;
    long long frames = 0;          // frames of the full render (identical for every stem and the master)
    double duration_s = 0.0;       // frames / sample_rate (0.0 when sample_rate <= 0)
    std::vector<TrackMeter> tracks;  // project track order, one entry per track (index = position)
    Meter master;                    // the full mix (includes master_gain_db)
};
Meter meter_buffer(const wav::StereoBuffer& buf);
bool measure_project(const Project& p, const std::string& base_dir, Report& out, std::string& err);
std::string to_json(const Report& r);
}  // namespace op::meters
