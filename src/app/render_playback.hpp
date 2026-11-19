// src/app/render_playback.hpp — JUCE-free playback helpers (WP-0153), library target `dzplayer`.
//
// format_position turns a frame index into the transport's "bar.beat" text using ONLY the engine's
// own tempo map (op::sample_at_bar) — never re-derived here.
// Example: at 120 bpm, 4/4, 48 kHz (96000 frames/bar): format_position(p, 96000) == "2.1".
//
// run_render_playback is the deterministic headless path that drives dz::Player with a fake device:
// render the project offline, play it back block by block through Player::process, and write the
// produced frames as PCM16 WAV. For any block size B >= 1 the output is byte-identical to
// `opd render <project> -o ref.wav`. With stop_after >= 1 the WAV is additionally preceded by the
// first ceil(stop_after/B)*B frames (a stop + silent block + restart exercise in the middle).
// Example:
//   std::string err;
//   int code = dz::run_render_playback("song.opj", "/tmp/p.wav", 128, -1, err);  // 0 ok, 3 project, 5 render/write
#pragma once

#include <string>

#include "engine/model.hpp"

namespace dz {

// "<bar>.<beat>", both 1-based integers ("1.1", "2.3"). `frame` is first clamped to
// [0, op::total_samples_for(p) - 1]; a frame past the end of a 4-bar project reads "4.4".
std::string format_position(const op::Project& p, long long frame);

// See file header for the full contract. `block` must be >= 1 (the CLI validates the range).
// Returns 0 on success; 3 project missing/invalid (`err` set); 5 render or WAV write failure.
// Prints the summary line(s) to stdout only on success.
int run_render_playback(const std::string& project_path, const std::string& out_path, int block,
                        long long stop_after, std::string& err);

}  // namespace dz
