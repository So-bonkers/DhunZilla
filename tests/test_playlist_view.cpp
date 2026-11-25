// Unit tests for WP-0196 (read-only playlist geometry): dz::playlist_x, dz::bars_at_frame and
// dz::playlist_blocks from src/app/playlist_view.hpp in the JUCE-free dzplayer core. Boring
// assert-based runner — no framework deps, own main. Exit code = number of failures. Every project
// is built in code; this test does no file I/O. Run: ./build/op_playlist_view_tests
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "engine/model.hpp"
#include "engine/render.hpp"
#include "playlist_view.hpp"

static int g_failures = 0;
#define CHECK(cond, name)                                                              \
    do {                                                                               \
        if (cond) std::printf("PASS %s\n", name);                                      \
        else { std::printf("FAIL %s  (%s:%d)\n", name, __FILE__, __LINE__); ++g_failures; } \
    } while (0)

// A minimal 120 bpm, 4/4, 48 kHz, 4-bar project with one track and one pattern (one note).
static op::Project make_project() {
    op::Project p;
    p.tracks = {op::Track{}};
    op::Pattern pat;
    pat.notes = {op::Note{}};  // one note at t 0, dur 0.25 -> extent 0.25
    p.patterns = {pat};
    return p;
}

// tempo.opj-style tempo change: 120 bpm, then 72 bpm from bar 4, 8 bars.
static op::Project make_tempo_project() {
    op::Project p = make_project();
    p.length_bars = 8.0;
    p.tempo_map = {op::TempoPoint{0.0, 120.0}, op::TempoPoint{4.0, 72.0}};
    return p;
}

// The clip_fades.opj geometry, built in code (pattern extent 1.5 beats: notes at t 0 and t 1,
// both 0.5 beats long).
static op::Project make_fades_project() {
    op::Project p = make_project();
    p.patterns[0].notes = {op::Note{0.0, 0.5}, op::Note{1.0, 0.5}};
    op::Clip c0;
    c0.pattern = 0;
    c0.start_bar = 0.0;
    c0.repeats = 2;
    c0.fade_in_beats = 0.5;
    c0.fade_out_beats = 1.0;
    op::Clip c1;
    c1.pattern = 0;
    c1.start_bar = 3.0;
    c1.repeats = 1;
    c1.gain_db = -6.0;
    p.arrangement = {c0, c1};
    return p;
}

int main() {
    using dz::bars_at_frame;
    using dz::kPlaylistLeft;
    using dz::kPlaylistRight;
    using dz::playlist_blocks;
    using dz::playlist_x;

    // ------------------------------------------------------------------ playlist_x
    CHECK(playlist_x(0.0) == kPlaylistLeft, "playlist.x_zero");
    CHECK(playlist_x(0.5) == 296, "playlist.x_half_bar");
    CHECK(playlist_x(1.0) == 320, "playlist.x_one_bar");
    CHECK(playlist_x(-3.0) == kPlaylistLeft, "playlist.x_negative_clamps_left");
    CHECK(playlist_x(std::nan("")) == kPlaylistLeft, "playlist.x_nan_clamps_left");
    CHECK(playlist_x(0.15625) == 280, "playlist.x_rounds_half_away_from_zero");  // 7.5 -> 8
    CHECK(playlist_x(16.0) == kPlaylistRight, "playlist.x_exactly_16_bars");
    CHECK(playlist_x(23.75) == kPlaylistRight, "playlist.x_past_the_view");

    // ------------------------------------------------------------------ bars_at_frame
    const op::Project p120 = make_project();  // 120 bpm: 1 bar == 96000 samples, 4 bars == 384000
    CHECK(bars_at_frame(p120, 0) == 0.0, "playlist.bars_at_frame_zero_frame");
    CHECK(bars_at_frame(p120, -96000) == 0.0, "playlist.bars_at_frame_negative_frame");
    op::Project zero_len = make_project();
    zero_len.length_bars = 0.0;
    CHECK(bars_at_frame(zero_len, 48000) == 0.0, "playlist.bars_at_frame_zero_length_bars");
    CHECK(std::fabs(bars_at_frame(p120, 96000) - 1.0) <= 1e-4, "playlist.bars_at_frame_one_bar_120bpm");
    CHECK(std::fabs(bars_at_frame(p120, 48000) - 0.5) <= 1e-4, "playlist.bars_at_frame_half_bar_120bpm");
    CHECK(bars_at_frame(p120, 384000) == 4.0, "playlist.bars_at_frame_parks_at_end");
    CHECK(bars_at_frame(p120, 384001) == 4.0, "playlist.bars_at_frame_parks_past_end");

    const op::Project ptempo = make_tempo_project();  // sample_at_bar(5.0) == 544000 here
    CHECK(std::fabs(bars_at_frame(ptempo, 544000) - 5.0) <= 1e-4, "playlist.bars_at_frame_tempo_change");
    // A constant-120-bpm computation would answer 544000/96000 = 5.667 — that must NOT happen.
    CHECK(bars_at_frame(ptempo, 544000) < 5.4, "playlist.bars_at_frame_uses_tempo_map_not_constant_bpm");

    bool roundtrip_ok = true;
    for (long long f = 1; f < 384000; f += 4096) {
        const long long back = op::sample_at_bar(p120, bars_at_frame(p120, f));
        if (std::llabs(back - f) > 1) roundtrip_ok = false;
    }
    CHECK(roundtrip_ok, "playlist.bars_at_frame_round_trip_within_1_sample");
    roundtrip_ok = true;
    for (long long f = 1; f < 1024000; f += 1024) {  // over the tempo change too
        const long long back = op::sample_at_bar(ptempo, bars_at_frame(ptempo, f));
        if (std::llabs(back - f) > 1) roundtrip_ok = false;
    }
    CHECK(roundtrip_ok, "playlist.bars_at_frame_round_trip_within_1_sample_tempo");
    bool monotonic = true;
    double prev = 0.0;
    for (long long f = 0; f <= 1024000; f += 9973) {
        const double bars = bars_at_frame(ptempo, f);
        if (bars < prev) monotonic = false;
        prev = bars;
    }
    CHECK(monotonic, "playlist.bars_at_frame_monotonic_in_frame");

    // ------------------------------------------------------------------ playlist_blocks: worked example (clip_fades.opj geometry)
    const op::Project fades = make_fades_project();
    const auto blocks = playlist_blocks(fades);
    CHECK(blocks.size() == 2, "playlist.blocks_one_entry_per_clip");
    const auto& b0 = blocks[0];
    CHECK(b0.clip == 0 && b0.lane == 0 && b0.drawn && b0.x0 == 272 && b0.x1 == 308,
          "playlist.blocks_example_clip0_geometry");
    CHECK(b0.dividers == std::vector<int>{290}, "playlist.blocks_example_repeat_divider");
    CHECK(b0.repeats == 2 && b0.repeat_beats == 1.5 && b0.length_beats == 3.0,
          "playlist.blocks_example_length_is_repeats_times_extent");
    CHECK(b0.fade_in_px == 6 && b0.fade_out_px == 12, "playlist.blocks_example_fade_wedges");
    CHECK(b0.label == "Pattern 0" && b0.label_x == 276 && b0.label_w == 28,
          "playlist.blocks_example_label");
    CHECK(b0.start_bar == 0.0 && !b0.trimmed && b0.fade_in_beats == 0.5 && b0.fade_out_beats == 1.0,
          "playlist.blocks_example_stored_fields_copied");
    const auto& b1 = blocks[1];
    CHECK(b1.clip == 1 && b1.drawn && b1.x0 == 416 && b1.x1 == 434 && b1.dividers.empty() &&
              b1.fade_in_px == 0 && b1.fade_out_px == 0,
          "playlist.blocks_example_clip1");

    // Legacy empty arrangement -> empty vector, no synthesized placements.
    op::Project legacy = make_project();
    CHECK(playlist_blocks(legacy).empty(), "playlist.blocks_empty_arrangement_yields_nothing");

    // Model order: never sorted by position, one entry per clip.
    op::Project order = make_project();
    order.patterns[0].length_beats = 2.0;
    order.arrangement = {op::Clip{}, op::Clip{}};
    order.arrangement[0].start_bar = 4.0;
    order.arrangement[1].start_bar = 1.0;
    const auto ob = playlist_blocks(order);
    CHECK(ob.size() == 2 && ob[0].clip == 0 && ob[0].start_bar == 4.0 && ob[0].x0 == 464 &&
              ob[1].clip == 1 && ob[1].start_bar == 1.0 && ob[1].x0 == 320,
          "playlist.blocks_stay_in_model_order");

    // Lane = the pattern's track; pattern id as the label; dividers at every pattern_extent.
    op::Project lane = make_project();
    lane.tracks = {op::Track{}, op::Track{}};
    lane.patterns[0].track = 1;
    lane.patterns[0].id = "Groove A";
    lane.patterns[0].length_beats = 2.0;
    lane.arrangement = {op::Clip{}};
    lane.arrangement[0].repeats = 3;
    const auto lb = playlist_blocks(lane);
    CHECK(lb[0].lane == 1, "playlist.blocks_lane_is_the_patterns_track");
    CHECK(lb[0].label == "Groove A", "playlist.blocks_label_uses_the_pattern_id");
    CHECK(lb[0].x0 == 272 && lb[0].x1 == 344 && lb[0].length_beats == 6.0 &&
              lb[0].dividers == (std::vector<int>{296, 320}),
          "playlist.blocks_dividers_spaced_by_the_pattern_extent");

    // Trim shortens the drawn block; a divider exactly at x1 is skipped, later ones vanish.
    op::Project trim = make_project();
    trim.patterns[0].length_beats = 2.0;
    trim.arrangement = {op::Clip{}};
    trim.arrangement[0].repeats = 4;
    trim.arrangement[0].length_beats = 4.0;  // trimmed: two repeats drawn out of four
    const auto tb = playlist_blocks(trim);
    CHECK(tb[0].trimmed && tb[0].length_beats == 4.0 && tb[0].x1 == 320,
          "playlist.blocks_trim_sets_the_drawn_length");
    CHECK(tb[0].dividers == std::vector<int>{296}, "playlist.blocks_divider_exactly_at_x1_is_skipped");
    CHECK(tb[0].repeats == 4, "playlist.blocks_repeats_report_as_stored");

    // Minimum drawn width 2 px, and label_w floors at 1 for a narrow block.
    op::Project tiny = make_project();
    tiny.patterns[0].length_beats = 0.05;
    tiny.arrangement = {op::Clip{}};
    const auto nb = playlist_blocks(tiny);
    CHECK(nb[0].drawn && nb[0].x1 - nb[0].x0 == 2, "playlist.blocks_minimum_width_2px");
    CHECK(nb[0].label_x == nb[0].x0 + 4 && nb[0].label_w == 1, "playlist.blocks_label_w_floors_at_1");

    // Clamping to the right edge; a clip starting at bar 16.0 is not drawn at all.
    op::Project wide = make_project();
    wide.patterns[0].length_beats = 2.0;
    wide.arrangement = {op::Clip{}, op::Clip{}};
    wide.arrangement[0].start_bar = 15.5;
    wide.arrangement[0].repeats = 6;
    wide.arrangement[1].start_bar = 16.0;
    const auto wb = playlist_blocks(wide);
    CHECK(wb[0].drawn && wb[0].x0 == 1016 && wb[0].x1 == kPlaylistRight,
          "playlist.blocks_clamped_to_kPlaylistRight");
    CHECK(!wb[1].drawn && wb[1].x0 == kPlaylistRight, "playlist.blocks_start_at_bar_16_not_drawn");

    // Invalid pattern index: not drawn, zeroed extents, repeats as stored, "Pattern <n>" label,
    // and label_x/label_w are still reported.
    op::Project bad = make_project();
    bad.arrangement = {op::Clip{}, op::Clip{}};
    bad.arrangement[0].pattern = 7;
    bad.arrangement[0].repeats = 5;
    bad.arrangement[1].pattern = 0;
    bad.patterns[0].track = 42;  // lane outside tracks
    const auto bb = playlist_blocks(bad);
    CHECK(!bb[0].drawn && bb[0].lane == 0 && bb[0].repeat_beats == 0.0 &&
              bb[0].length_beats == 0.0 && bb[0].repeats == 5,
          "playlist.blocks_invalid_pattern_not_drawn");
    CHECK(bb[0].label == "Pattern 7" && bb[0].label_x == 276 && bb[0].label_w == 1,
          "playlist.blocks_invalid_pattern_label_still_reported");
    CHECK(!bb[1].drawn && bb[1].lane == 42 && bb[1].label == "Pattern 0",
          "playlist.blocks_invalid_lane_not_drawn");

    // Pattern extent 0.0 (a pattern without notes): repeats coerced to 1, zero length, no dividers.
    op::Project empty_pat = make_project();
    empty_pat.patterns[0].notes.clear();
    empty_pat.arrangement = {op::Clip{}};
    empty_pat.arrangement[0].repeats = 4;
    const auto eb = playlist_blocks(empty_pat);
    CHECK(eb[0].repeats == 1 && eb[0].repeat_beats == 0.0 && eb[0].length_beats == 0.0 &&
              eb[0].drawn && eb[0].dividers.empty(),
          "playlist.blocks_zero_extent_coerces_repeats_to_1");

    // Fades longer than the block clamp to the block width.
    op::Project fade_clamp = make_project();
    fade_clamp.patterns[0].length_beats = 2.0;
    fade_clamp.arrangement = {op::Clip{}};
    fade_clamp.arrangement[0].length_beats = 2.0;  // 24 px wide
    fade_clamp.arrangement[0].fade_in_beats = 4.0;
    fade_clamp.arrangement[0].fade_out_beats = 5.0;
    const auto fc = playlist_blocks(fade_clamp);
    CHECK(fc[0].fade_in_px == 24 && fc[0].fade_out_px == 24,
          "playlist.blocks_fades_longer_than_the_block_clamp");

    // clip.offset_beats changes no geometry at all, untrimmed or trimmed.
    op::Project offset = make_project();
    offset.patterns[0].length_beats = 2.0;
    offset.arrangement = {op::Clip{}};
    offset.arrangement[0].start_bar = 1.0;
    offset.arrangement[0].repeats = 2;
    const auto plain = playlist_blocks(offset);  // offset 0, untrimmed: x 320..368, divider 344
    offset.arrangement[0].offset_beats = 3.0;
    const auto shifted = playlist_blocks(offset);
    CHECK(plain[0].x0 == 320 && plain[0].x1 == 368 && plain[0].dividers == std::vector<int>{344} &&
              shifted[0].x0 == plain[0].x0 && shifted[0].x1 == plain[0].x1 &&
              shifted[0].dividers == plain[0].dividers &&
              shifted[0].length_beats == plain[0].length_beats,
          "playlist.blocks_offset_beats_changes_no_untrimmed_geometry");
    op::Project trim_a = offset;                  // offset 3.0, trimmed
    trim_a.arrangement[0].length_beats = 5.0;
    op::Project trim_b = offset;                  // offset 0.0, trimmed, everything else equal
    trim_b.arrangement[0].offset_beats = 0.0;
    trim_b.arrangement[0].length_beats = 5.0;
    const auto ta = playlist_blocks(trim_a);
    const auto tb2 = playlist_blocks(trim_b);
    CHECK(ta[0].x0 == 320 && ta[0].x1 == 380 && ta[0].dividers == std::vector<int>{344} &&
              tb2[0].x0 == ta[0].x0 && tb2[0].x1 == ta[0].x1 &&
              tb2[0].dividers == ta[0].dividers,
          "playlist.blocks_offset_beats_changes_no_trimmed_geometry");

    // beats_per_bar scaling: bars are beats / beats_per_bar (exactly op::beats_at_bar).
    op::Project signature = make_project();
    signature.beats_per_bar = 8;
    signature.patterns[0].length_beats = 4.0;
    signature.arrangement = {op::Clip{}};
    signature.arrangement[0].repeats = 2;
    const auto sb = playlist_blocks(signature);
    CHECK(sb[0].length_beats == 8.0 && sb[0].x0 == 272 && sb[0].x1 == 320 &&
              sb[0].dividers == std::vector<int>{296},
          "playlist.blocks_beats_per_bar_scales_the_bar_math");

    // The input project is never modified.
    const op::Project snapshot = make_fades_project();
    op::Project mut = snapshot;
    (void)playlist_blocks(mut);
    CHECK(op::project_bytes(mut) == op::project_bytes(snapshot),
          "playlist.blocks_input_project_unmodified");

    std::printf("%d failure(s)\n", g_failures);
    return g_failures;
}
