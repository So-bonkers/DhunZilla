// tests/test_player_click.cpp — WP-0201: dz::Player mixes the project's own metronome click while
// playing. Boring assert-based runner — no framework deps, own main, no file I/O and no wall clock;
// the player is driven by simulated device callbacks of 128 frames on this thread, exactly like
// tests/test_player_loop.cpp does. The reference for every mixed sample is the engine's own
// op::render_click buffer — the same code `opd click` runs (WP-0148) — read at the wrapped source
// frame and compared bit-exactly with memcmp/==, never a tolerance. Projects are built in code with
// timing fields only: render_click ignores tracks, patterns, arrangement and gains.
// Normative rules quoted below are WP-0201 "Spec — what correct means" §3.
// Exit code = number of failures. Run: ./build/op_player_click_tests

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
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

static constexpr int kBlock = 128;                // simulated device callback size
static constexpr float kSentinel = -987654.321f;  // pre-fill: a slot process() forgot to write shows up

// A timing-only project: render_click/beat_onsets read bpm, sample_rate, beats_per_bar and
// length_bars — tracks, patterns, arrangement and gains are ignored by the click path (WP-0148).
static op::Project timing_project(double bpm, int beats_per_bar, double length_bars) {
    op::Project p;
    p.format_version = 0;
    p.bpm = bpm;
    p.sample_rate = 48000;
    p.seed = 42;
    p.length_bars = length_bars;
    p.beats_per_bar = beats_per_bar;
    return p;
}

// A ramp buffer like the loop tests': frame f holds left = base + f, right = -base - f — every
// sample names its own source frame, so a wrong index or a channel swap cannot hide.
static std::unique_ptr<op::wav::StereoBuffer> make_buffer(long long frames, float base) {
    auto buf = std::make_unique<op::wav::StereoBuffer>();
    buf->sample_rate = 48000;
    buf->interleaved.resize(static_cast<size_t>(frames) * 2);
    for (long long i = 0; i < frames; ++i) {
        buf->interleaved[static_cast<size_t>(2 * i)] = base + static_cast<float>(i);
        buf->interleaved[static_cast<size_t>(2 * i + 1)] = -base - static_cast<float>(i);
    }
    return buf;
}

// A truly silent buffer: every frame zero, so whatever comes out of process() with the click armed
// is exactly what was mixed.
static std::unique_ptr<op::wav::StereoBuffer> make_silent_buffer(long long frames) {
    auto buf = std::make_unique<op::wav::StereoBuffer>();
    buf->sample_rate = 48000;
    buf->interleaved.assign(static_cast<size_t>(frames) * 2, 0.0f);
    return buf;
}

// The closed form §3 rule 8 quotes from WP-0200: with entry position 0 and window [start, end),
// output slot g carries source frame f(g). Written straight from the task text as an independent
// cross-check of the callback's incremental (per-slot) form.
static long long wrapped_frame(long long start, long long end, long long g) {
    if (g < end) return g;
    const long long len = end - start;
    return start + (g - end) % len;
}

// ---------------------------------------------------------------------------
// §3 rules 1/2: defaults; the flag round-trips independently of any track; prepare_click keeps it off.

static void test_defaults_and_flag() {
    dz::Player p;
    // Rule 1: a fresh Player has no track and the flag off.
    const bool fresh = !p.has_click() && !p.click_enabled();

    // Rule 2: the flag is pure state — it round-trips with no track stored, and never renders.
    p.set_click_enabled(true);
    const bool on_no_track = p.click_enabled() && !p.has_click();
    p.set_click_enabled(false);
    const bool off_again = !p.click_enabled();

    // Rules 2/3: a successful prepare stores the track (has_click true, err empty) but keeps the
    // flag exactly as it was — there is no implicit enable in prepare_click.
    std::string err;
    const bool ok = p.prepare_click(timing_project(120.0, 3, 1.0), err);
    CHECK(fresh && on_no_track && off_again && ok && err.empty() && p.has_click() && !p.click_enabled(),
          "click.defaults_and_flag_round_trip");

    // Rule 9 (restated): flag off with a stored track — the callback output is bit-identical to the
    // raw buffer, and the zero-filled tail past the end stays zero (rule 7).
    const op::Project proj = timing_project(120.0, 3, 1.0);
    const long long frames = op::total_samples_for(proj);  // 72000 at 120 bpm / 3 beats
    dz::Player q;
    std::string err2;
    if (!q.prepare_click(proj, err2) || !err2.empty()) {
        CHECK(false, "click.defaults_prepare_failed");
        return;
    }
    auto audio = make_buffer(frames, 200.0f);
    const op::wav::StereoBuffer raw = *audio;  // the untouched copy to compare against
    q.submit(std::move(audio));
    q.play();
    float l[kBlock];
    float r[kBlock];
    bool plain_ok = true;
    for (long long g = 0; g < frames && plain_ok; g += kBlock) {
        std::fill(l, l + kBlock, kSentinel);
        std::fill(r, r + kBlock, kSentinel);
        q.process(l, r, kBlock);
        for (int i = 0; i < kBlock && plain_ok; ++i) {
            const long long f = g + i;
            if (f >= frames) {
                if (l[i] != 0.0f || r[i] != 0.0f) plain_ok = false;  // rule 7: zero-filled tail
            } else if (std::memcmp(&l[i], &raw.interleaved[static_cast<size_t>(2 * f)], sizeof(float)) != 0 ||
                       std::memcmp(&r[i], &raw.interleaved[static_cast<size_t>(2 * f + 1)], sizeof(float)) != 0) {
                plain_ok = false;  // no tolerance: bit-identical or it fails
            }
        }
    }
    CHECK(plain_ok && !q.playing() && q.position_frames() == 0 && q.frames_played() == frames,
          "click.flag_off_output_is_plain_audio_bit_identical");
}

// ---------------------------------------------------------------------------
// §3 rule 3: a failing prepare_click changes nothing — stored track and flag stay exactly as they were.

static void test_failed_prepare_keeps_state() {
    std::string err;
    const op::Project good = timing_project(120.0, 3, 1.0);
    const long long frames = op::total_samples_for(good);
    op::wav::StereoBuffer ref_click;
    if (!op::render_click(good, ref_click, err) || !err.empty()) {
        CHECK(false, "click.failed_prepare_setup_render_failed");
        return;
    }

    dz::Player p;
    if (!p.prepare_click(good, err) || !err.empty()) {
        CHECK(false, "click.failed_prepare_setup_prepare_failed");
        return;
    }
    p.set_click_enabled(true);

    // Rule 3: length_bars = 0 makes the render fail — false + non-empty err (exactly what
    // render_click set; we do not assert its text), and has_click()/click_enabled() unchanged.
    op::Project bad = good;
    bad.length_bars = 0.0;
    err.clear();
    const bool failed = p.prepare_click(bad, err);
    const bool state_kept = !failed && !err.empty() && p.has_click() && p.click_enabled();

    // The previously stored track still mixes: with silent audio the first block must equal the
    // reference click bit-for-bit — proof the failed call replaced nothing.
    p.submit(make_silent_buffer(frames));
    p.play();
    float l[kBlock];
    float r[kBlock];
    std::fill(l, l + kBlock, kSentinel);
    std::fill(r, r + kBlock, kSentinel);
    p.process(l, r, kBlock);
    bool old_track_mixes = true;
    for (int i = 0; i < kBlock && old_track_mixes; ++i) {
        if (std::memcmp(&l[i], &ref_click.interleaved[static_cast<size_t>(2 * i)], sizeof(float)) != 0 ||
            std::memcmp(&r[i], &ref_click.interleaved[static_cast<size_t>(2 * i + 1)], sizeof(float)) != 0)
            old_track_mixes = false;
    }
    CHECK(state_kept && old_track_mixes, "click.failed_prepare_leaves_track_and_flag_untouched");
}

// ---------------------------------------------------------------------------
// §3 rules 6/7/12: silent audio + click enabled == the op::render_click buffer bit-for-bit, bursts
// exactly at op::beat_onsets(p), onset samples 0.5f, silence between bursts and past the end.

static void test_silent_audio_equals_render_click() {
    std::string err;
    // 120 bpm, 3/4, one bar → 72000 frames: NOT a multiple of kBlock, so the zero-filled tail of
    // the stopping block is exercised too. Everything before that must equal the click reference.
    const op::Project p = timing_project(120.0, 3, 1.0);
    op::wav::StereoBuffer ref_click;
    if (!op::render_click(p, ref_click, err) || !err.empty()) {
        CHECK(false, "click.silent_reference_render_failed");
        return;
    }
    const long long frames = static_cast<long long>(ref_click.frames());
    CHECK(frames % kBlock != 0 && op::total_samples_for(p) == frames,
          "click.reference_length_not_block_aligned");

    dz::Player pl;
    if (!pl.prepare_click(p, err) || !err.empty()) {
        CHECK(false, "click.silent_prepare_failed");
        return;
    }
    // Rule 12: submit + prepare + enable all happen before the very first process() call → mixing
    // is exact from frame 0 of that first block.
    pl.set_click_enabled(true);
    pl.submit(make_silent_buffer(frames));
    pl.play();

    float l[kBlock];
    float r[kBlock];
    bool bit_exact = true;
    long long g = 0;
    const int blocks = static_cast<int>(frames / kBlock) + 2;  // past the end: the stop fires inside
    for (int b = 0; b < blocks && bit_exact; ++b) {
        std::fill(l, l + kBlock, kSentinel);
        std::fill(r, r + kBlock, kSentinel);
        pl.process(l, r, kBlock);
        for (int i = 0; i < kBlock && bit_exact; ++i, ++g) {
            // Rule 7: slots past the end of the buffer are zero — and so is everything there in a
            // click-armed block.
            const float want_l = g < frames ? ref_click.interleaved[static_cast<size_t>(2 * g)] : 0.0f;
            const float want_r = g < frames ? ref_click.interleaved[static_cast<size_t>(2 * g + 1)] : 0.0f;
            if (std::memcmp(&l[i], &want_l, sizeof(float)) != 0 ||
                std::memcmp(&r[i], &want_r, sizeof(float)) != 0)
                bit_exact = false;  // no tolerance: bit-identical or it fails
        }
    }
    CHECK(bit_exact && !pl.playing() && pl.position_frames() == 0 && pl.frames_played() == frames,
          "click.silent_audio_with_click_is_render_click_bit_for_bit");

    // The burst starts are exactly op::beat_onsets(p): every onset frame carries the peak sample
    // 0.5f on both channels (cos(0)·exp(0) at half scale), and every frame in a gap between bursts
    // is zero — so nothing was mixed off the project's own beat→sample mapping.
    const std::vector<long long> onsets = op::beat_onsets(p);
    const long long burst = 48000LL * 30 / 1000;  // render.cpp: sr * 30 / 1000 frames per burst
    bool onsets_ok = !onsets.empty() && onsets[0] == 0;
    for (size_t i = 0; i < onsets.size() && onsets_ok; ++i) {
        const long long o = onsets[i];
        if (o >= frames || ref_click.interleaved[static_cast<size_t>(2 * o)] != 0.5f ||
            ref_click.interleaved[static_cast<size_t>(2 * o + 1)] != 0.5f)
            onsets_ok = false;  // every onset sample is exactly 0.5f, both channels
        const long long gap_end = (i + 1 < onsets.size()) ? onsets[i + 1] : frames;
        for (long long f = o + burst; f < gap_end && onsets_ok; ++f)
            if (ref_click.interleaved[static_cast<size_t>(2 * f)] != 0.0f) onsets_ok = false;  // gaps silent
    }
    CHECK(onsets_ok, "click.burst_starts_are_exactly_beat_onsets_with_0p5_onset_samples");
}

// ---------------------------------------------------------------------------
// §3 rules 6/9: mixing over a NON-silent buffer is exactly audio + click per channel (one float
// addition each; left never sees the right click channel), and flag off == plain audio.

static void test_mix_is_audio_plus_click() {
    std::string err;
    const op::Project p = timing_project(120.0, 3, 1.0);
    op::wav::StereoBuffer ref_click;
    if (!op::render_click(p, ref_click, err) || !err.empty()) {
        CHECK(false, "click.mix_reference_render_failed");
        return;
    }
    const long long frames = static_cast<long long>(ref_click.frames());

    // A ramp whose frame index is visible in every sample (base + f), so the expected value of each
    // slot is computable per channel: left wants base+f+clickL, right wants -base-f+clickR.
    const float base = 300.0f;

    dz::Player on;   // track stored AND flag on → mixed
    if (!on.prepare_click(p, err) || !err.empty()) {
        CHECK(false, "click.mix_prepare_on_failed");
        return;
    }
    on.set_click_enabled(true);
    on.submit(make_buffer(frames, base));

    dz::Player off;  // the same track stored, flag never enabled → rule 9: plain audio bit-identical
    if (!off.prepare_click(p, err) || !err.empty()) {
        CHECK(false, "click.mix_prepare_off_failed");
        return;
    }
    off.submit(make_buffer(frames, base));

    on.play();
    off.play();
    float l_on[kBlock];
    float r_on[kBlock];
    float l_off[kBlock];
    float r_off[kBlock];
    bool mix_ok = true;
    bool plain_ok = true;
    long long g = 0;
    const int blocks = static_cast<int>(frames / kBlock) + 1;
    for (int b = 0; b < blocks && (mix_ok || plain_ok); ++b) {
        std::fill(l_on, l_on + kBlock, kSentinel);
        std::fill(r_on, r_on + kBlock, kSentinel);
        std::fill(l_off, l_off + kBlock, kSentinel);
        std::fill(r_off, r_off + kBlock, kSentinel);
        on.process(l_on, r_on, kBlock);
        off.process(l_off, r_off, kBlock);
        for (int i = 0; i < kBlock && (mix_ok || plain_ok); ++i, ++g) {
            if (g >= frames) break;  // the zero tail after the stop is covered by the test above
            const float a_l = base + static_cast<float>(g);
            const float a_r = -base - static_cast<float>(g);
            // Rule 6: one float addition per channel, in this order — no gain, clamp or cross-talk.
            const float want_l = a_l + ref_click.interleaved[static_cast<size_t>(2 * g)];
            const float want_r = a_r + ref_click.interleaved[static_cast<size_t>(2 * g + 1)];
            if (std::memcmp(&l_on[i], &want_l, sizeof(float)) != 0 ||
                std::memcmp(&r_on[i], &want_r, sizeof(float)) != 0)
                mix_ok = false;
            if (std::memcmp(&l_off[i], &a_l, sizeof(float)) != 0 ||
                std::memcmp(&r_off[i], &a_r, sizeof(float)) != 0)
                plain_ok = false;
        }
    }
    CHECK(mix_ok, "click.mix_is_exactly_audio_plus_click_per_channel");
    CHECK(plain_ok && on.frames_played() == off.frames_played(),
          "click.flag_off_with_stored_track_is_plain_audio_and_counters_match");
}

// ---------------------------------------------------------------------------
// §3 rule 8: under a loop the click follows the WRAPPED frame — window [S, E) with neither bound a
// multiple of the block size, several rounds; bursts re-fire at the wrapped beat positions.

static void test_loop_wrap_click_follows_wrapped_frame() {
    std::string err;
    // 240 bpm → one beat is exactly 12000 frames. Window [1000, 13000): S % kBlock = 104 and
    // E % kBlock = 72 — neither lands on a block edge — and the window holds the onset at frame
    // 12000, so every round re-fires that burst. Click track: 8 beats → 96000 frames.
    const op::Project p = timing_project(240.0, 4, 2.0);
    op::wav::StereoBuffer ref_click;
    if (!op::render_click(p, ref_click, err) || !err.empty()) {
        CHECK(false, "click.loop_reference_render_failed");
        return;
    }
    const long long s = 1000;
    const long long e = 13000;
    const long long len = e - s;
    const bool bounds_mid_block = s % kBlock != 0 && e % kBlock != 0 && len % kBlock != 0;
    CHECK(bounds_mid_block && static_cast<long long>(ref_click.frames()) >= e,
          "click.loop_window_bounds_land_mid_block");

    dz::Player pl;
    if (!pl.prepare_click(p, err) || !err.empty()) {
        CHECK(false, "click.loop_prepare_failed");
        return;
    }
    pl.set_click_enabled(true);
    const float base = 50.0f;
    pl.submit(make_buffer(20000, base));  // longer than the window end, not block-aligned itself
    pl.set_loop(s, e);
    pl.play();

    // Drive entry pass + three full rounds: every slot of every block comes from the buffer under a
    // loop, so every slot must equal audio[f(g)] + click[f(g)] at the wrapped frame f(g).
    const long long want_frames = e + 3 * len;
    const int blocks = static_cast<int>((want_frames + kBlock - 1) / kBlock);
    float l[kBlock];
    float r[kBlock];
    bool bit_exact = true;
    long long refires = 0;  // output slots carrying the window's onset frame 12000
    long long g = 0;
    for (int b = 0; b < blocks && bit_exact; ++b) {
        std::fill(l, l + kBlock, kSentinel);
        std::fill(r, r + kBlock, kSentinel);
        pl.process(l, r, kBlock);
        for (int i = 0; i < kBlock && bit_exact; ++i, ++g) {
            const long long f = wrapped_frame(s, e, g);  // §3 rule 8's closed form, entry position 0
            const float want_l = (base + static_cast<float>(f)) + ref_click.interleaved[static_cast<size_t>(2 * f)];
            const float want_r = (-base - static_cast<float>(f)) + ref_click.interleaved[static_cast<size_t>(2 * f + 1)];
            if (std::memcmp(&l[i], &want_l, sizeof(float)) != 0 ||
                std::memcmp(&r[i], &want_r, sizeof(float)) != 0)
                bit_exact = false;  // no tolerance: the click must ride the wrapped frame exactly
            if (f == 12000) ++refires;
        }
    }
    CHECK(bit_exact && pl.playing() && pl.frames_played() == static_cast<long long>(blocks) * kBlock,
          "click.loop_every_slot_equals_reference_at_wrapped_frame");

    // Bursts re-fire at the wrapped beat positions: entry pass (g = 12000) plus one hit per round
    // (g = 24000, 36000, 48000) — and each such slot carries exactly audio + 0.5f.
    bool refire_ok = refires >= 4;
    for (long long h = 12000; h < want_frames && refire_ok; h += len) {
        if (wrapped_frame(s, e, h) != 12000) refire_ok = false;  // the onset lands here every round
    }
    CHECK(refire_ok && refires >= 4, "click.loop_bursts_refire_at_wrapped_beat_positions");
}

// ---------------------------------------------------------------------------
// §6: a loop window SHORTER than one callback block — [0, 97) with 128-frame callbacks wraps inside
// EVERY block, so almost every slot exercises the wrap. This is the case that catches a click
// indexed by pos + i (or any running output counter) instead of by the wrapped source frame: the
// beat-0 burst spans frames [0, 1440), i.e. the whole window, so EVERY slot must carry a nonzero
// click value — an implementation indexing by the output slot would add silence past frame 1439.

static void test_window_shorter_than_block_wraps_every_block() {
    std::string err;
    const op::Project p = timing_project(120.0, 4, 1.0);  // beat 0 at frame 0; burst covers [0, 1440)
    op::wav::StereoBuffer ref_click;
    if (!op::render_click(p, ref_click, err) || !err.empty()) {
        CHECK(false, "click.short_reference_render_failed");
        return;
    }
    const long long s = 0;
    const long long e = 97;  // < kBlock: the wrap happens inside every single block

    dz::Player pl;
    if (!pl.prepare_click(p, err) || !err.empty()) {
        CHECK(false, "click.short_prepare_failed");
        return;
    }
    pl.set_click_enabled(true);
    const float base = 700.0f;
    pl.submit(make_buffer(500, base));
    pl.set_loop(s, e);
    pl.play();

    constexpr int kBlocks = 20;  // 2560 slots over a 97-frame window: ~26 rounds, wrap in every block
    float l[kBlock];
    float r[kBlock];
    bool bit_exact = true;
    long long onset_hits = 0;  // slots whose wrapped frame is the beat-0 onset (f == 0 → +0.5f)
    long long g = 0;
    for (int b = 0; b < kBlocks && bit_exact; ++b) {
        std::fill(l, l + kBlock, kSentinel);
        std::fill(r, r + kBlock, kSentinel);
        pl.process(l, r, kBlock);
        for (int i = 0; i < kBlock && bit_exact; ++i, ++g) {
            const long long f = wrapped_frame(s, e, g);  // = g % 97 from slot 97 on
            const float want_l = (base + static_cast<float>(f)) + ref_click.interleaved[static_cast<size_t>(2 * f)];
            const float want_r = (-base - static_cast<float>(f)) + ref_click.interleaved[static_cast<size_t>(2 * f + 1)];
            if (std::memcmp(&l[i], &want_l, sizeof(float)) != 0 ||
                std::memcmp(&r[i], &want_r, sizeof(float)) != 0)
                bit_exact = false;
            if (f == 0) ++onset_hits;
        }
    }
    // g ≡ 0 (mod 97) wraps back to frame 0: slots 0, 97, 194, … — 27 hits in 2560 slots.
    CHECK(bit_exact && onset_hits == 27 && pl.playing() && pl.frames_played() == kBlocks * static_cast<long long>(kBlock),
          "click.window_shorter_than_block_wraps_click_in_every_slot");

    // §3 rule 6, mono: right == nullptr reads/writes only the left channel — no crash (ASAN builds
    // catch any read of click[2f+1] here) and the left values stay exact.
    dz::Player m;
    if (!m.prepare_click(p, err) || !err.empty()) {
        CHECK(false, "click.short_mono_prepare_failed");
        return;
    }
    m.set_click_enabled(true);
    m.submit(make_buffer(500, base));
    m.set_loop(s, e);
    m.play();
    bool mono_ok = true;
    long long gm = 0;
    for (int b = 0; b < 3 && mono_ok; ++b) {
        std::fill(l, l + kBlock, kSentinel);
        m.process(l, nullptr, kBlock);
        for (int i = 0; i < kBlock && mono_ok; ++i, ++gm) {
            const long long f = wrapped_frame(s, e, gm);
            const float want_l = (base + static_cast<float>(f)) + ref_click.interleaved[static_cast<size_t>(2 * f)];
            if (std::memcmp(&l[i], &want_l, sizeof(float)) != 0) mono_ok = false;
        }
    }
    CHECK(mono_ok && m.playing(), "click.short_window_mono_reads_only_left_click_channel");
}

// ---------------------------------------------------------------------------
// §3 rule 7: with a track armed and the flag on, a block before play() and the block after stop()
// are exactly zero — the transport stopped ⇒ silence; only copied slots ever carry the click.

static void test_zero_where_nothing_was_copied() {
    std::string err;
    const op::Project p = timing_project(120.0, 3, 1.0);
    dz::Player pl;
    if (!pl.prepare_click(p, err) || !err.empty()) {
        CHECK(false, "click.zero_prepare_failed");
        return;
    }
    pl.set_click_enabled(true);
    pl.submit(make_buffer(1000, 400.0f));

    float l[kBlock];
    float r[kBlock];

    // The first block adopts the pending buffer but playing() is false → pure silence, click or no
    // click (rule 7).
    std::fill(l, l + kBlock, kSentinel);
    std::fill(r, r + kBlock, kSentinel);
    pl.process(l, r, kBlock);
    const bool before_play_zero =
        std::all_of(l, l + kBlock, [](float v) { return v == 0.0f; }) &&
        std::all_of(r, r + kBlock, [](float v) { return v == 0.0f; });

    // One real block to prove the click really is armed and mixing: frame 0 = ramp 400 + onset 0.5.
    pl.play();
    std::fill(l, l + kBlock, kSentinel);
    std::fill(r, r + kBlock, kSentinel);
    pl.process(l, r, kBlock);
    const float want_0 = 400.0f + 0.5f;  // exact in binary: both operands and the sum are representable
    const bool armed_and_mixing =
        pl.playing() && std::memcmp(&l[0], &want_0, sizeof(float)) == 0 && l[1] != 0.0f;

    // stop() → the next block is exactly zero again even though the track stays armed and adopted.
    pl.stop();
    std::fill(l, l + kBlock, kSentinel);
    std::fill(r, r + kBlock, kSentinel);
    pl.process(l, r, kBlock);
    const bool after_stop_zero =
        std::all_of(l, l + kBlock, [](float v) { return v == 0.0f; }) &&
        std::all_of(r, r + kBlock, [](float v) { return v == 0.0f; });

    CHECK(before_play_zero && armed_and_mixing && after_stop_zero,
          "click.no_click_where_nothing_was_copied_before_play_or_after_stop");
}

int main() {
    test_defaults_and_flag();
    test_failed_prepare_keeps_state();
    test_silent_audio_equals_render_click();
    test_mix_is_audio_plus_click();
    test_loop_wrap_click_follows_wrapped_frame();
    test_window_shorter_than_block_wraps_every_block();
    test_zero_where_nothing_was_copied();

    if (g_failures == 0) std::printf("ALL PASS (0 failures)\n");
    else std::printf("%d FAILURE(S)\n", g_failures);
    return g_failures;
}
