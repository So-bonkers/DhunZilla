// engine/audition.hpp — project-free one-note sample preview (WP-0209). Builds the canonical
// one-track sampler *project* for an audition request (pure model construction, no audio code);
// rendering goes through op::render_project so output is bit-exact with that project's render.
#pragma once
#include <string>

#include "engine/model.hpp"
#include "engine/wav.hpp"

namespace op::audition {

constexpr int kSampleRate = 48000;
constexpr double kBpm = 87.890625;                 // == 5625/64, exactly representable
constexpr int kBeatsPerBar = 4;
constexpr double kMaxSeconds = 300.0;               // largest accepted --seconds value
constexpr long long kDefaultFrames = 96000;         // length when neither --seconds nor --end-s is given

struct Options {
    std::string path;                   // sampler asset path, stored in the voice verbatim
    int note = -1;                      // MIDI pitch of the single note; -1 = no pitch -> shift 0
    int root = -1;                      // asset's root MIDI pitch; -1 = absent -> no repitch
    double start_s = 0.0;               // trim slice start, seconds on the asset timeline
    double end_s = 0.0;                 // trim slice end; meaningful only when has_end_s
    bool has_end_s = false;             // false = play to the end of the asset
    long long frames = kDefaultFrames;  // output length in frames; build_project uses max(frames, 1)
};

// Seconds -> project-rate frames: llround(seconds * 48000), product clamped to [-4e18, 4e18] so
// absurd magnitudes stay defined. Same rounding rule as the renderer's trim (spec project-model.md
// §Sampler trim). Example: to_frames(2.0) == 96000; to_frames(0.0001) == 5; to_frames(-0.5) == -24000.
long long to_frames(double seconds);

// The canonical one-track sampler project for `o` (pure: no I/O, no clock, no randomness). One
// sampler track at defaults + one pattern holding exactly one note {t 0, dur frames/spb}. Empty
// arrangement and tempo map; bpm/sample_rate/seed fixed so total_samples_for(p) == max(frames, 1).
// Example: op::audition::Options o; o.path = "kick.wav"; o.note = 72; o.root = 60;
//          op::Project p = op::audition::build_project(o);  // renders one repitched hit
Project build_project(const Options& o);

// Renders build_project(o) via op::render_project(p, base_dir, out, err) — nothing else. Returns
// whatever render_project returns (unreadable asset, unsupported rate or "sampler: trim slice is
// empty" propagate unchanged). Example:
//   op::wav::StereoBuffer buf; std::string err;
//   if (!op::audition::render(o, dir, buf, err)) { /* sampler's own message in err */ }
bool render(const Options& o, const std::string& base_dir, wav::StereoBuffer& out, std::string& err);

}  // namespace op::audition
