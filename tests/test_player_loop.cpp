// tests/test_player_loop.cpp — WP-0200: dz::Player honours a transport loop region (sample-exact wrap
// of the transport counter inside the audio callback). Boring assert-based runner — no framework deps,
// own main, no file I/O and no wall clock. The player is driven by simulated device callbacks of 128
// frames on this thread; every ramp-buffer sample states its source frame (frame f holds
// left = base + f, right = -base - f), so a wrong copy shows up as a wrong frame index. The last two
// tests drive real render_project() audio and compare bit-for-bit against the frame stream the spec
// promises: docs/spec/project-model.md §Loop region consumed through dz::Player (src/app/player.hpp).
// Normative rules quoted below are WP-0200 "Spec — what correct means" §2.
// Exit code = number of failures. Run: ./build/op_player_loop_tests

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "engine/model.hpp"
#include "engine/render.hpp"
#include "engine/wav.hpp"
#include "player.hpp"

static int g_failures = 0;
#define CHECK(cond, name)                                                                     \
    do {                                                                                      \
        if (cond) std::printf("PASS %s\n", name);                                             \
        else { std::printf("FAIL %s  (%s:%d)\n", name, __FILE__, __LINE__); ++g_failures; }   \
    } while (0)

static constexpr int kBlock = 128;          // simulated device callback size
static constexpr float kSentinel = -987654.321f;  // pre-fill: a slot process() forgot to write shows up

// A ramp buffer: frame f holds left = base + f, right = -base - f. Every sample names its own source
// frame, and with base != 0 a zero sample can only mean silence.
static std::unique_ptr<op::wav::StereoBuffer> make_ramp_buffer(long long frames, float base) {
    auto buf = std::make_unique<op::wav::StereoBuffer>();
    buf->sample_rate = 48000;
    buf->interleaved.resize(static_cast<size_t>(frames) * 2);
    for (long long i = 0; i < frames; ++i) {
        buf->interleaved[static_cast<size_t>(2 * i)] = base + static_cast<float>(i);
        buf->interleaved[static_cast<size_t>(2 * i + 1)] = -base - static_cast<float>(i);
    }
    return buf;
}

// The spec's own closed form (§2 rule 3): with entry position `pos` and window [start, end),
// output slot i carries buffer frame f(i). Written straight from the task text so it is an
// independent cross-check of the callback's incremental (per-slot) form.
static long long expected_source(long long pos, long long start, long long end, long long i) {
    const long long x = pos + i;
    if (x < end) return x;
    const long long len = end - start;
    return start + (x - start) % len;
}

// One run of simulated device callbacks. `src` records, per output slot, the buffer frame it came
// from (-1 = zero-filled silence), decoded from the ramp values themselves.
struct Drive {
    std::vector<long long> src;
    bool channels_agree = true;     // right == -left on every written slot (stereo drives only)
    bool every_slot_written = true;  // no pre-filled sentinel survived
    bool playing = false;
    long long position = 0;
    long long frames_played = 0;
};

static Drive drive(dz::Player& p, int blocks, float base, bool stereo) {
    Drive d;
    float l[kBlock];
    float r[kBlock];
    for (int b = 0; b < blocks; ++b) {
        std::fill(l, l + kBlock, kSentinel);
        if (stereo) std::fill(r, r + kBlock, kSentinel);
        p.process(l, stereo ? r : nullptr, kBlock);
        for (int i = 0; i < kBlock; ++i) {
            if (l[i] == kSentinel) d.every_slot_written = false;
            // base + f decodes exactly: every value is an integer well inside float32's exact range.
            d.src.push_back(l[i] == 0.0f ? -1 : static_cast<long long>(l[i] - base));
            if (stereo && r[i] != -l[i]) d.channels_agree = false;
        }
    }
    d.playing = p.playing();
    d.position = p.position_frames();
    d.frames_played = p.frames_played();
    return d;
}

// One-shot pass over a buffer of `frames` frames: every slot up to the end carries its own frame, and
// each later slot is silence (-1) — what §2 rule 9 promises when there is no usable window.
static bool matches_one_pass(const Drive& d, long long frames) {
    for (size_t i = 0; i < d.src.size(); ++i) {
        const long long want = static_cast<long long>(i) < frames ? static_cast<long long>(i) : -1;
        if (d.src[i] != want) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// §2 rule 1/2: enabling, disabling, and the predicate's state at every moment.

static void test_enable_disable() {
    // Rule 2: off by default, and clear_loop() on a fresh player keeps it off.
    dz::Player p;
    const bool default_off = !p.has_loop();
    p.clear_loop();
    CHECK(default_off && !p.has_loop(), "loop.off_by_default");

    // Rule 2: reports the stored state immediately — before any buffer is submitted, while stopped,
    // and while playing.
    dz::Player q;
    q.set_loop(64, 300);
    const bool on_before_submit = q.has_loop();
    q.clear_loop();
    const bool off_after_clear = !q.has_loop();
    q.submit(make_ramp_buffer(512, 1000.0f));
    q.play();
    q.set_loop(0, 256);
    const bool on_while_playing = q.has_loop();
    float l[kBlock];
    q.process(l, nullptr, kBlock);
    const bool on_after_a_block = q.has_loop();
    q.clear_loop();
    CHECK(on_before_submit && off_after_clear && on_while_playing && on_after_a_block && !q.has_loop(),
          "loop.set_enables_and_clear_disables");

    // Rule 1: equal, reversed and negative-start pairs all leave the disabled state; a valid pair
    // straight after them enables again.
    dz::Player r;
    r.set_loop(128, 128);
    const bool equal_off = !r.has_loop();
    r.set_loop(400, 96);
    const bool reversed_off = !r.has_loop();
    r.set_loop(-1, 512);
    const bool negative_start_off = !r.has_loop();
    r.set_loop(0, 1);
    CHECK(equal_off && reversed_off && negative_start_off && r.has_loop(), "loop.invalid_pairs_disable");
}

// ---------------------------------------------------------------------------
// §2 rules 3/4/5: the wrap itself — first pass, mid-block switch, counters.

static void test_first_pass_and_wrap() {
    // Window [300, 500) on a 1000-frame buffer, blocks of 128 (rule 8: no seek, so the pre-loop
    // material plays first). Positions: 0 → 128 → 256 → 384, all before the window end.
    dz::Player p;
    p.submit(make_ramp_buffer(1000, 1000.0f));
    p.set_loop(300, 500);
    p.play();
    const Drive first = drive(p, 3, 1000.0f, true);
    bool preloop_ok = first.every_slot_written && first.channels_agree && p.has_loop();
    for (long long i = 0; preloop_ok && i < 3 * kBlock; ++i)
        if (first.src[static_cast<size_t>(i)] != i) preloop_ok = false;
    CHECK(preloop_ok && first.position == 384 && first.playing && first.frames_played == 3 * kBlock,
          "loop.first_pass_plays_up_to_window_end");

    // Rule 3: the wrap lands mid-block. Entry pos 384: slots 0..115 are frames 384..499 (the window
    // end is exclusive, so frame 499 is the last one played), then slot 116 is frame 300 and slot 127
    // is frame 311 — no gap, no overlap, no silence. Rule 4: position reads 312, still playing.
    const Drive wrapped = drive(p, 1, 1000.0f, true);
    bool wrap_ok = wrapped.every_slot_written && wrapped.channels_agree;
    for (int i = 0; i < 116 && wrap_ok; ++i)
        if (wrapped.src[static_cast<size_t>(i)] != 384 + i) wrap_ok = false;
    for (int i = 116; i < kBlock && wrap_ok; ++i)
        if (wrapped.src[static_cast<size_t>(i)] != 300 + (i - 116)) wrap_ok = false;
    CHECK(wrap_ok && wrapped.position == 312 && wrapped.playing && wrapped.frames_played == 4 * kBlock,
          "loop.wrap_mid_block_is_sample_exact");
}

static void test_short_window_and_mono() {
    // Rule 3: a window shorter than the block wraps several times inside one callback. Window [0, 5):
    // slot i of the first block is frame i % 5; the block ends mid-window, so position reads 3.
    dz::Player p;
    p.submit(make_ramp_buffer(256, 2000.0f));
    p.set_loop(0, 5);
    p.play();
    const Drive one = drive(p, 1, 2000.0f, true);
    bool short_ok = one.every_slot_written && one.channels_agree;
    for (int i = 0; short_ok && i < kBlock; ++i)
        if (one.src[static_cast<size_t>(i)] != static_cast<long long>(i % 5)) short_ok = false;
    // A second block continues from 3 and keeps wrapping: same rule, entry position 3.
    const Drive two = drive(p, 1, 2000.0f, true);
    bool cont_ok = two.every_slot_written && two.channels_agree;
    for (int i = 0; cont_ok && i < kBlock; ++i)
        if (two.src[static_cast<size_t>(i)] != expected_source(3, 0, 5, i)) cont_ok = false;
    CHECK(short_ok && one.position == 3 && cont_ok && two.position == ((3 + kBlock) % 5) && two.playing,
          "loop.short_window_wraps_several_times_in_one_block");

    // Rule 3 with right == nullptr (mono): the left channel wraps too. Window [10, 16) on 256 frames;
    // the first block plays 0..15 then repeats the window: last slot is frame 13, position 14.
    dz::Player m;
    m.submit(make_ramp_buffer(256, 3000.0f));
    m.set_loop(10, 16);
    m.play();
    const Drive mono = drive(m, 3, 3000.0f, false);
    bool mono_ok = mono.every_slot_written;
    long long pos = 0;
    for (int b = 0; b < 3 && mono_ok; ++b) {
        for (int i = 0; i < kBlock && mono_ok; ++i)
            if (mono.src[static_cast<size_t>(b * kBlock + i)] != expected_source(pos, 10, 16, i)) mono_ok = false;
        pos = expected_source(pos, 10, 16, kBlock);
    }
    CHECK(mono_ok && mono.position == pos && pos >= 10 && pos < 16 && mono.playing &&
              mono.frames_played == 3 * kBlock,
          "loop.mono_right_null_wraps");
}

static void test_block_edges_and_forever() {
    // Rule 4: a block that ends exactly on the loop end leaves the position at the window start.
    // Window [100, 384) with 384 = 3 × 128: blocks 1..3 run 0..383 with no wrap, and after block 3
    // the next frame to be output is 100 — not 384. Block 4 therefore starts on frame 100.
    dz::Player p;
    p.submit(make_ramp_buffer(512, 4000.0f));
    p.set_loop(100, 384);
    p.play();
    const Drive three = drive(p, 3, 4000.0f, true);
    bool edge_ok = three.every_slot_written && three.channels_agree;
    for (long long i = 0; edge_ok && i < 3 * kBlock; ++i)
        if (three.src[static_cast<size_t>(i)] != i) edge_ok = false;
    const bool park_at_start = p.position_frames() == 100;
    const Drive after = drive(p, 1, 4000.0f, true);
    CHECK(edge_ok && park_at_start && after.src[0] == 100 && after.src[kBlock - 1] == 227 && p.playing(),
          "loop.block_ending_on_window_end_parks_at_start");

    // Rule 4: a window whose end is the end of the buffer repeats forever without stopping; rule 5:
    // frames_played grows by exactly the block size on every call and never resets on a wrap.
    dz::Player q;
    q.submit(make_ramp_buffer(500, 4500.0f));
    q.set_loop(200, 500);
    q.play();
    bool forever_ok = true;
    bool counter_ok = true;
    long long pos = 0;
    constexpr int kBlocks = 20;  // 2560 frames over a 300-frame window: eight wraps, no end of song
    for (int b = 0; b < kBlocks && forever_ok; ++b) {
        const long long before = q.frames_played();
        const Drive blk = drive(q, 1, 4500.0f, true);
        for (int i = 0; i < kBlock && forever_ok; ++i)
            if (blk.src[static_cast<size_t>(i)] != expected_source(pos, 200, 500, i)) forever_ok = false;
        pos = expected_source(pos, 200, 500, kBlock);
        if (!blk.playing || !q.playing()) forever_ok = false;
        // Past the first wrap the position always lies inside the window (§2 rule 4).
        if (b >= 3 && !(blk.position >= 200 && blk.position < 500)) counter_ok = false;
        if (before + kBlock != q.frames_played() || blk.frames_played != before + kBlock) counter_ok = false;
    }
    CHECK(forever_ok && counter_ok && q.playing() && q.frames_played() == kBlocks * kBlock,
          "loop.window_to_buffer_end_repeats_forever_and_counters_hold");
}

// ---------------------------------------------------------------------------
// §2 rules 6/7/9: clamping, unusable windows, clear_loop mid-playback, no-loop behaviour.

static void test_clamp_and_unusable_window() {
    // Rule 7: end past the buffer behaves exactly like end == frames (clamped, never read out of it).
    dz::Player p;
    p.submit(make_ramp_buffer(500, 5500.0f));
    p.set_loop(100, 4000);
    p.play();
    bool clamp_ok = p.has_loop();  // bounds are stored as given (§2 rule 1) even though end > frames
    long long pos = 0;
    for (int b = 0; b < 8 && clamp_ok; ++b) {
        const Drive blk = drive(p, 1, 5500.0f, true);
        for (int i = 0; i < kBlock && clamp_ok; ++i)
            if (blk.src[static_cast<size_t>(i)] != expected_source(pos, 100, 500, i)) clamp_ok = false;
        pos = expected_source(pos, 100, 500, kBlock);
    }
    CHECK(clamp_ok && p.playing() && p.position_frames() < 500, "loop.bounds_past_buffer_are_clamped");

    // Rule 7: start >= frames is unusable for this buffer → one-shot (rule 9) even though has_loop()
    // is true, because nothing is validated against a buffer at call time.
    dz::Player u;
    u.submit(make_ramp_buffer(300, 6000.0f));
    u.set_loop(350, 900);
    u.play();
    const Drive unusable = drive(u, 4, 6000.0f, true);
    bool one_shot_ok = unusable.every_slot_written && matches_one_pass(unusable, 300);
    CHECK(one_shot_ok && !u.playing() && u.position_frames() == 0 && unusable.frames_played == 300,
          "loop.unusable_window_is_one_shot");

    // Rule 7 (start < 0) / rule 1 (negative start disables): the same one-shot result.
    dz::Player n;
    n.submit(make_ramp_buffer(300, 6000.0f));
    n.set_loop(-8, 200);
    n.play();
    const Drive negative = drive(n, 4, 6000.0f, true);
    CHECK(!n.has_loop() && negative.every_slot_written && matches_one_pass(negative, 300) && !n.playing() &&
              n.position_frames() == 0 && negative.frames_played == 300,
          "loop.negative_start_window_is_one_shot");

    // Rule 9: with no loop at all the callback is bit-identical to WP-0153 — one pass, zero-fill of
    // the rest of the block where playback stopped, then playing() false and position parked at 0.
    dz::Player q;
    q.submit(make_ramp_buffer(300, 6000.0f));
    q.play();
    const Drive plain = drive(q, 4, 6000.0f, true);
    CHECK(plain.every_slot_written && plain.channels_agree && matches_one_pass(plain, 300) && !q.playing() &&
              q.position_frames() == 0 && plain.frames_played == 300 && unusable.src == plain.src,
          "loop.no_loop_behaviour_unchanged");
}

// ---------------------------------------------------------------------------
// §2 rules 5/6/8: clear_loop mid-playback, stop()/play().

static void test_clear_loop_and_restart() {
    // Rule 6: clear_loop() mid-playback neither seeks nor silences. Window [100, 300) on 500 frames;
    // after three blocks the position is 184 (block 3 wrapped at frame 299 → 100 … 183).
    dz::Player p;
    p.submit(make_ramp_buffer(500, 7000.0f));
    p.set_loop(100, 300);
    p.play();
    const Drive looping = drive(p, 3, 7000.0f, true);
    bool wrapped_ok = looping.every_slot_written && looping.channels_agree && looping.position == 184;
    const long long played_before_clear = p.frames_played();
    p.clear_loop();
    const bool off_now = !p.has_loop();
    const bool no_seek = p.position_frames() == 184 && p.playing();
    // From here it is one-shot: frames 184..499 (bit-identical to the rest of the buffer), then zeros,
    // and only then does playback stop and park at frame 0. Rule 5: frames_played is cumulative — it
    // was not reset by the wrap or by clear_loop().
    const Drive runout = drive(p, 4, 7000.0f, true);
    bool runout_ok = runout.every_slot_written && runout.channels_agree;
    for (long long j = 0; runout_ok && j < kBlock * 4; ++j) {
        const long long want = 184 + j < 500 ? 184 + j : -1;
        if (runout.src[static_cast<size_t>(j)] != want) runout_ok = false;
    }
    CHECK(wrapped_ok && off_now && no_seek && runout_ok && !p.playing() && p.position_frames() == 0 &&
              played_before_clear == 3 * kBlock &&
              p.frames_played() == played_before_clear + (500 - 184),
          "loop.clear_loop_mid_playback_runs_to_end_then_stops");

    // Rule 8: play() still starts at frame 0, so after stop()+play() the pre-loop material plays
    // again — no seek to the window start.
    dz::Player r;
    r.submit(make_ramp_buffer(1024, 5000.0f));
    r.set_loop(300, 600);
    r.play();
    const Drive looped = drive(r, 8, 5000.0f, true);  // well past the first wrap
    const bool was_looping = looped.every_slot_written && r.playing() && r.has_loop();
    r.stop();
    const bool stopped_home = !r.playing() && r.position_frames() == 0;
    float l[kBlock];
    std::fill(l, l + kBlock, kSentinel);
    r.process(l, nullptr, kBlock);
    const bool silent_while_stopped = std::all_of(l, l + kBlock, [](float v) { return v == 0.0f; });
    r.play();
    const Drive restart = drive(r, 1, 5000.0f, true);
    bool from_zero = true;
    for (int i = 0; i < kBlock && from_zero; ++i)
        if (restart.src[static_cast<size_t>(i)] != i) from_zero = false;
    CHECK(was_looping && stopped_home && silent_while_stopped && from_zero && r.position_frames() == kBlock,
          "loop.stop_then_play_restarts_at_frame_zero");
}

static void test_set_loop_during_playback() {
    // Rule 10: a control-thread change takes effect no later than the next process() call. Two plain
    // blocks first (no loop), then set_loop(300, 320) while playing.
    dz::Player p;
    p.submit(make_ramp_buffer(1000, 8000.0f));
    p.play();
    const Drive plain = drive(p, 2, 8000.0f, true);
    bool plain_ok = plain.every_slot_written && plain.channels_agree;
    for (long long i = 0; plain_ok && i < 2 * kBlock; ++i)
        if (plain.src[static_cast<size_t>(i)] != i) plain_ok = false;

    p.set_loop(300, 320);
    const bool on_now = p.has_loop();
    // Block 3 is the first one that sees the window: entry pos 256, so frames 256..319 (pre-loop pass
    // to the exclusive end) and then a 20-frame window repeating for the rest of the block.
    const Drive blk3 = drive(p, 1, 8000.0f, true);
    bool wrap_now = blk3.every_slot_written && blk3.channels_agree;
    for (int i = 0; wrap_now && i < kBlock; ++i)
        if (blk3.src[static_cast<size_t>(i)] != expected_source(256, 300, 320, i)) wrap_now = false;
    const long long pos_after_3 = p.position_frames();
    // Block 4 keeps wrapping the same window from wherever block 3 left off.
    const Drive blk4 = drive(p, 1, 8000.0f, true);
    bool keeps_wrapping = blk4.every_slot_written;
    for (int i = 0; keeps_wrapping && i < kBlock; ++i)
        if (blk4.src[static_cast<size_t>(i)] != expected_source(pos_after_3, 300, 320, i)) keeps_wrapping = false;
    CHECK(plain_ok && on_now && wrap_now && keeps_wrapping && p.playing() && pos_after_3 >= 300 &&
              pos_after_3 < 320,
          "loop.set_loop_during_playback_takes_effect_next_block");
}

// ---------------------------------------------------------------------------
// Acceptance: byte-exact against real render_project() audio. A project carrying a loop region at
// bars 0.5 → 1.25; the window is S_start = op::sample_at_bar(p, loop->start_bar) to
// S_end = op::sample_at_bar(p, loop->end_bar) — exactly the frames `opd render --loop` slices out of
// the full render (src/cli/main.cpp: --loop == --from-bar start --to-bar end, sliced after rendering).

static long long count_nonzero(const op::wav::StereoBuffer& b, long long from, long long to) {
    long long n = 0;
    for (long long f = from; f < to; ++f) {
        if (b.interleaved[static_cast<size_t>(2 * f)] != 0.0f ||
            b.interleaved[static_cast<size_t>(2 * f + 1)] != 0.0f)
            ++n;
    }
    return n;
}

// Audio across all four bars: a held sine plus a metronome click on every beat, so the loop window is
// never silent and a wrong frame index cannot hide behind zeros.
static op::Project audio_project(double bpm, bool with_loop) {
    op::Project p;
    p.format_version = 0;
    p.bpm = bpm;
    p.sample_rate = 48000;
    p.seed = 42;
    p.length_bars = 4.0;
    p.beats_per_bar = 4;

    op::Track sine;
    sine.name = "sine";
    sine.voice.kind = op::VoiceKind::Sine;
    sine.voice.freq_hz = 440.0f;
    p.tracks.push_back(sine);
    op::Pattern held;
    held.track = 0;
    held.id = "held";
    op::Note n;
    n.t_beats = 0.0;
    n.dur_beats = static_cast<double>(p.beats_per_bar) * p.length_bars;
    held.notes.push_back(n);
    p.patterns.push_back(held);

    op::Track metro;
    metro.name = "M";
    metro.voice.kind = op::VoiceKind::Metronome;
    metro.voice.accent_every = 4;
    p.tracks.push_back(metro);
    op::Pattern clicks;
    clicks.track = 1;
    clicks.id = "clicks";
    const int beats = p.beats_per_bar * static_cast<int>(p.length_bars);
    for (int b = 0; b < beats; ++b) {
        op::Note c;
        c.t_beats = static_cast<double>(b);
        c.dur_beats = 0.25;
        clicks.notes.push_back(c);
    }
    p.patterns.push_back(clicks);

    if (with_loop) p.loop = op::LoopRegion{0.5, 1.25};
    return p;
}

static void test_render_loop_byte_exact() {
    std::string err;
    const op::Project p = audio_project(120.0, true);
    op::wav::StereoBuffer render_buf;
    if (!op::render_project(p, "", render_buf, err)) {
        CHECK(false, "loop.render_project_failed");
        return;
    }
    const long long s_start = op::sample_at_bar(p, p.loop->start_bar);  // 48000 at 120 bpm / 4/4
    const long long s_end = op::sample_at_bar(p, p.loop->end_bar);      // 120000
    const long long len = s_end - s_start;                              // 72000

    // The region's bounds must not be multiples of the block size, or wraps could hide on an edge.
    CHECK(s_start < s_end && s_end % kBlock != 0 && len % kBlock != 0 &&
              count_nonzero(render_buf, s_start, s_end) > (s_end - s_start) / 2,
          "loop.render_window_lands_mid_block_and_has_audio");

    dz::Player player;
    player.submit(std::make_unique<op::wav::StereoBuffer>(render_buf));  // the player owns a copy
    player.set_loop(s_start, s_end);
    player.play();

    // Drive enough blocks for several wraps: one pass to S_end plus four window lengths.
    const long long want_frames = s_end + 4 * len;
    const int blocks = static_cast<int>((want_frames + kBlock - 1) / kBlock);
    float l[kBlock];
    float r[kBlock];
    bool bit_exact = true;
    bool counters_ok = true;
    long long g = 0;
    for (int b = 0; b < blocks && bit_exact; ++b) {
        std::fill(l, l + kBlock, kSentinel);
        std::fill(r, r + kBlock, kSentinel);
        player.process(l, r, kBlock);
        for (int i = 0; i < kBlock && bit_exact; ++i, ++g) {
            const long long want = g < s_end ? g : s_start + (g - s_end) % len;
            const float* ref = render_buf.interleaved.data() + 2 * static_cast<size_t>(want);
            if (std::memcmp(&l[i], ref, sizeof(float)) != 0 ||
                std::memcmp(&r[i], ref + 1, sizeof(float)) != 0)
                bit_exact = false;  // no tolerance: bit-identical or it fails
        }
        if (!player.playing()) counters_ok = false;
        const long long pos = player.position_frames();
        if (g > s_end && !(pos >= s_start && pos < s_end)) counters_ok = false;
        if (player.frames_played() != g) counters_ok = false;
    }
    CHECK(bit_exact, "loop.render_loop_is_byte_exact_over_several_wraps");
    CHECK(counters_ok && player.playing() && player.frames_played() == static_cast<long long>(blocks) * kBlock,
          "loop.render_counter_contract_holds_through_wraps");
}

static void test_render_no_loop_regression() {
    std::string err;
    // Same audio, no loop region, and a bpm whose total length is not a multiple of the block size so
    // the zero-fill tail of the stopping block is exercised.
    const op::Project p = audio_project(119.0, false);
    op::wav::StereoBuffer render_buf;
    if (!op::render_project(p, "", render_buf, err)) {
        CHECK(false, "loop.render_project_no_loop_failed");
        return;
    }
    const long long total = static_cast<long long>(render_buf.frames());
    CHECK(total % kBlock != 0, "loop.no_loop_render_length_not_block_aligned");

    dz::Player player;
    player.submit(std::make_unique<op::wav::StereoBuffer>(render_buf));
    player.play();
    float l[kBlock];
    float r[kBlock];
    bool bit_exact = true;
    long long g = 0;
    const int blocks = static_cast<int>(total / kBlock) + 2;  // past the end: the stop must fire inside
    for (int b = 0; b < blocks && bit_exact; ++b) {
        std::fill(l, l + kBlock, kSentinel);
        std::fill(r, r + kBlock, kSentinel);
        player.process(l, r, kBlock);
        for (int i = 0; i < kBlock && bit_exact; ++i, ++g) {
            const float* ref = g < total ? render_buf.interleaved.data() + 2 * static_cast<size_t>(g) : nullptr;
            const float want_l = ref != nullptr ? ref[0] : 0.0f;
            const float want_r = ref != nullptr ? ref[1] : 0.0f;
            if (std::memcmp(&l[i], &want_l, sizeof(float)) != 0 ||
                std::memcmp(&r[i], &want_r, sizeof(float)) != 0)
                bit_exact = false;
        }
    }
    CHECK(bit_exact && !player.playing() && player.position_frames() == 0 && player.frames_played() == total,
          "loop.no_loop_render_is_one_pass_then_zeros_and_parks");
}

int main() {
    test_enable_disable();
    test_first_pass_and_wrap();
    test_short_window_and_mono();
    test_block_edges_and_forever();
    test_clamp_and_unusable_window();
    test_clear_loop_and_restart();
    test_set_loop_during_playback();
    test_render_loop_byte_exact();
    test_render_no_loop_regression();

    if (g_failures == 0) std::printf("ALL PASS (0 failures)\n");
    else std::printf("%d FAILURE(S)\n", g_failures);
    return g_failures;
}
