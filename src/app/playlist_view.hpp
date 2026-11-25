// src/app/playlist_view.hpp — read-only playlist geometry (WP-0196). Pure functions that turn an
// op::Project into the exact pixel rectangles the dhunzilla playlist panel paints: bar position ->
// x, transport sample-counter position -> bars (through the renderer's own tempo map), and one
// PlaylistBlock per arrangement clip (lane, x range, repeat dividers, fade wedges, trim, label).
// This module is GUI-free by design: it includes no GUI toolkit, does no I/O, holds no state, reads
// no clock and never mutates the project, so the widget's numbers are unit-testable headless
// (tests/test_playlist_view.cpp). The panel is READ-ONLY: it applies no operation; editing comes
// in a later work package. Example:
//   dz::PlaylistBlock b0 = dz::playlist_blocks(p)[0];
//   g.fillRect(b0.x0, top, b0.x1 - b0.x0, dz::kPlaylistBlockHeight);
//   const double bar = dz::bars_at_frame(p, player.position_frames());  // playhead
#pragma once

#include <string>
#include <vector>

#include "engine/model.hpp"

namespace dz {

// Fixed panel geometry (no scrolling, no zoom): x of bar 0, bars 0..15 drawn, px per bar, lane
// pitch, lane background height, block inset/height/narrowest drawn width, playhead line width,
// and the exclusive right edge of the drawn area.
inline constexpr int kPlaylistLeft = 272;             // x of bar 0
inline constexpr int kPlaylistBars = 16;              // bars 0..15 are drawn, no scrolling
inline constexpr int kPlaylistBarWidth = 48;          // px per bar
inline constexpr int kPlaylistLaneHeight = 32;        // lane pitch
inline constexpr int kPlaylistLaneDrawnHeight = 31;   // lane background / bar-line height
inline constexpr int kPlaylistBlockTop = 3;           // block top inset inside its lane
inline constexpr int kPlaylistBlockHeight = 26;       // block height
inline constexpr int kPlaylistBlockMinWidth = 2;      // narrowest drawn block
inline constexpr int kPlaylistPlayheadWidth = 2;      // playhead line width
inline constexpr int kPlaylistRight = 272 + 16 * 48;  // 1040: first x NOT drawn (exclusive edge)

// Bar position -> absolute x. NaN or bar <= 0.0 -> kPlaylistLeft; a bar whose scaled offset
// bar * kPlaylistBarWidth is >= kPlaylistBars * kPlaylistBarWidth -> kPlaylistRight; otherwise
// kPlaylistLeft + llround(bar * kPlaylistBarWidth) (std::llround: half away from zero).
// Example: dz::playlist_x(0.0) == 272, dz::playlist_x(0.5) == 296, dz::playlist_x(16.0) == 1040.
int playlist_x(double bar);

// Transport position in BARS for a dz::Player sample-counter value, through the renderer's own
// tempo map op::sample_at_bar (never a constant-bpm computation). frame <= 0 or length_bars <= 0
// -> 0.0; frame at or past the end of the project parks at length_bars; otherwise a 60-step
// bisection of sample_at_bar. Non-decreasing in `frame` and, for 0 < frame < sample_at_bar(p,
// length_bars), |sample_at_bar(p, bars_at_frame(p, frame)) - frame| <= 1. Compare results with
// tolerance 1e-4 bars (bisection and the engine's rounding never land exactly).
// Example (tempo.opj: 120 bpm, then 72 bpm from bar 4): bars_at_frame(p, 544000) ~= 5.0.
double bars_at_frame(const op::Project& p, long long frame);

// One entry per arrangement clip: everything the widget paints for it. `drawn == false` (invalid
// pattern index, invalid lane, or x0 >= kPlaylistRight) means skip the block; label/label_x/
// label_w are still reported. Geometry converts bars<->beats with the plain product/quotient by
// beats_per_bar (exactly op::beats_at_bar), never the tempo map; clip.offset_beats does not
// affect it. Example:
//   for (const auto& b : dz::playlist_blocks(p))
//       if (b.drawn) draw_block(b);
struct PlaylistBlock {
    int clip = 0;                 // index into p.arrangement
    int lane = 0;                 // patterns[clip.pattern].track (may be out of range)
    bool drawn = false;           // false: invalid pattern index, invalid lane, or x0 >= kPlaylistRight
    int x0 = 0, x1 = 0;           // absolute px
    int repeats = 1;              // clip.repeats, but 1 when the pattern extent is 0.0
    double repeat_beats = 0.0;    // op::pattern_extent(pattern); 0.0 for an invalid pattern index
    double start_bar = 0.0;       // clip.start_bar
    double length_beats = 0.0;    // *clip.length_beats when trimmed, else repeats * repeat_beats;
                                  // 0.0 for an invalid pattern index
    bool trimmed = false;         // clip.length_beats.has_value()
    double fade_in_beats = 0.0;   // clip.fade_in_beats
    double fade_out_beats = 0.0;  // clip.fade_out_beats
    std::vector<int> dividers;    // absolute x of every repeat divider strictly inside (x0, x1)
    int fade_in_px = 0;           // wedge width; 0 = none, else 0 < fade_in_px <= x1 - x0
    int fade_out_px = 0;          // wedge width; 0 = none, else 0 < fade_out_px <= x1 - x0
    std::string label;            // pattern id, or "Pattern <clip.pattern>" when the id is empty
    int label_x = 0;              // x0 + 4
    int label_w = 0;              // max(x1 - x0 - 8, 1)
};

// One entry per p.arrangement clip, in model order: never sorted, never merged; the legacy empty
// arrangement yields an empty vector (no synthesized implicit placements). `p` is never modified.
// Example: const auto blocks = dz::playlist_blocks(doc.project());
std::vector<PlaylistBlock> playlist_blocks(const op::Project& p);

}  // namespace dz
