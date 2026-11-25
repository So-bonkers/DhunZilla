// tests/test_player_pattern.cpp — WP-0229: dz::Player pattern mode — one named pattern looped
// instead of the arrangement. Boring assert-based runner — no framework deps, own main, no file I/O
// and no wall clock. The player is driven by simulated device callbacks of 128 frames on this
// thread, exactly like tests/test_player_loop.cpp. The reference is the ENGINE's own pattern-mode
// render — what `opd render <proj> -o out.wav --pattern NAME --loops N` writes (spec
// project-model.md §Pattern mode, WP-0192) — built in memory with op::pattern_mode_project +
// op::render_project. Every comparison is bit-exact (memcmp / == on the floats), never a tolerance.
// Normative rules quoted below are WP-0229 "Spec — normative" §3.
// Exit code = number of failures. Run: ./build/op_player_pattern_tests

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

// A ramp buffer: frame f holds left = base + f, right = -base - f. Every sample names its own source
// frame, so a wrong copy shows up as a wrong frame index (same helper style as test_player_loop.cpp).
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

// The wrap's closed form (WP-0200 rule, reused by pattern mode): with entry position `pos` and
// window [start, end), output slot i carries buffer frame f(i).
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
    bool channels_agree = true;      // right == -left on every written slot (stereo drives only)
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

// One-shot pass over a buffer of `frames` frames: every slot up to the end carries its own frame,
// and each later slot is silence (-1) — the WP-0153 path when nothing wraps.
static bool matches_one_pass(const Drive& d, long long frames) {
    for (size_t i = 0; i < d.src.size(); ++i) {
        const long long want = static_cast<long long>(i) < frames ? static_cast<long long>(i) : -1;
        if (d.src[i] != want) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// The project, built in code (no fixtures, no opd): 120 bpm, 48 kHz, 4/4, 4 bars, seed 42 →
// 24000 samples per beat, so every 16th-step position and every swing offset (swing * 0.125 beats)
// is an exact integer sample and the loops render is exactly periodic with the statement length.
//   A (track 0, metronome): length_beats 3.0, swing 0.25, 12 notes on a 16th grid → P = 72000.
//   B (track 0, metronome): no length_beats — extent derived from its notes, 1.75 beats → P = 42000;
//                           swing 0.5.
//   unnamed (id "", track 1, sine): never selectable; contributes only to the song-mode arrangement.
// No note's AUDIO reaches its statement boundary: the metronome tone is a hard 30 ms stop
// (render.cpp), sine stops at t + dur; every sine t + dur <= length with >= 0.1 beat of slack.

static op::Project make_project() {
    op::Project p;
    p.format_version = 0;
    p.bpm = 120.0;
    p.sample_rate = 48000;
    p.seed = 42;
    p.length_bars = 4.0;
    p.beats_per_bar = 4;

    op::Track metro;
    metro.name = "metronome";
    metro.voice.kind = op::VoiceKind::Metronome;
    metro.voice.accent_every = 4;
    p.tracks.push_back(metro);
    op::Track sine;
    sine.name = "sine";
    sine.voice.kind = op::VoiceKind::Sine;
    sine.voice.freq_hz = 440.0f;
    p.tracks.push_back(sine);

    op::Pattern a;
    a.track = 0;
    a.id = "A";
    a.swing = 0.25;
    a.length_beats = 3.0;
    for (int i = 0; i < 12; ++i) {  // 16th grid: t = 0.25*i; last onset 2.75, swung +0.03125,
        op::Note n;                 // tone ends 30 ms later — well inside the 3.0-beat statement
        n.t_beats = 0.25 * i;
        n.dur_beats = 0.25;
        a.notes.push_back(n);
    }
    p.patterns.push_back(a);

    op::Pattern b;
    b.track = 0;
    b.id = "B";
    b.swing = 0.5;  // no length_beats: extent = max(t + dur) = 1.5 + 0.25 = 1.75 beats
    for (int i = 0; i < 7; ++i) {
        op::Note n;
        n.t_beats = 0.25 * i;
        n.dur_beats = 0.25;
        b.notes.push_back(n);
    }
    p.patterns.push_back(b);

    op::Pattern u;
    u.track = 1;
    u.id = "";  // unnamed: never selectable (WP-0179 / §Pattern ids)
    u.length_beats = 3.0;
    {
        op::Note n1;  // sine stops at t + dur: 1.0 and 2.25, both >= 0.75 beats inside 3.0
        n1.t_beats = 0.0;
        n1.dur_beats = 1.0;
        u.notes.push_back(n1);
        op::Note n2;
        n2.t_beats = 1.25;
        n2.dur_beats = 1.0;
        u.notes.push_back(n2);
    }
    p.patterns.push_back(u);

    // Non-empty arrangement so song mode and pattern mode differ audibly.
    op::Clip ca;
    ca.pattern = 0;  // A, four statements
    ca.start_bar = 0.0;
    ca.repeats = 4;
    p.arrangement.push_back(ca);
    op::Clip cu;
    cu.pattern = 2;  // the unnamed pattern, once
    cu.start_bar = 0.0;
    cu.repeats = 1;
    p.arrangement.push_back(cu);
    return p;
}

// The reference: the engine's own pattern-mode render, the exact code path
// `opd render --pattern NAME --loops N` runs (WP-0192). Returns false + err on failure.
static bool render_pattern_ref(const op::Project& p, std::size_t index, long long loops,
                               op::wav::StereoBuffer& ref, std::string& err) {
    op::Project pm;
    if (!op::pattern_mode_project(p, index, loops, pm, err)) return false;
    return op::render_project(pm, "", ref, err);
}

static bool sample_equal(const op::wav::StereoBuffer& ref, long long frame, float l, float r) {
    const float* want = ref.interleaved.data() + 2 * static_cast<size_t>(frame);
    return std::memcmp(&l, want, sizeof(float)) == 0 && std::memcmp(&r, want + 1, sizeof(float)) == 0;
}

// ---------------------------------------------------------------------------
// §3 rule 1/2: defaults — no pattern, flag off; the flag round-trips independently of
// has_pattern(); and the flag alone cannot enable a wrap: flag on with no pattern ever prepared is
// still the plain WP-0153 one-shot pass, zero-fill past the end, then stop and park at frame 0.

static void test_defaults() {
    dz::Player p;
    CHECK(!p.has_pattern() && !p.pattern_mode(), "pat.defaults_no_pattern_and_flag_off");

    p.set_pattern_mode(true);
    const bool on = p.pattern_mode() && !p.has_pattern();  // the flag is independent of the window
    p.set_pattern_mode(false);
    const bool off = !p.pattern_mode();
    p.set_pattern_mode(true);
    CHECK(on && off && p.pattern_mode() && !p.has_pattern(), "pat.flag_round_trips_independent_of_window");

    // Rule 1: flag on, pattern_len == 0 (nothing prepared) → bit-for-bit today's one-shot.
    dz::Player q;
    q.set_pattern_mode(true);
    q.submit(make_ramp_buffer(300, 6000.0f));
    q.play();
    const Drive plain = drive(q, 4, 6000.0f, true);
    CHECK(plain.every_slot_written && plain.channels_agree && matches_one_pass(plain, 300) &&
              !q.playing() && q.position_frames() == 0 && plain.frames_played == 300,
          "pat.flag_on_without_pattern_is_plain_one_shot");
}

// ---------------------------------------------------------------------------
// §3 rule 3.1: id lookup. Unknown id and empty id both fail with err non-empty and containing the
// id; nothing in the player changes. A failed prepare_pattern() AFTER a successful one leaves the
// stored pattern, the buffer and the flag untouched — proven by playing on and checking the wrap.

static void test_prepare_failures() {
    std::string err;
    const op::Project p = make_project();

    dz::Player u;
    err = "pre-set";
    const bool unknown = u.prepare_pattern(p, "nope", err);
    CHECK(!unknown && !err.empty() && err.find("nope") != std::string::npos && !u.has_pattern() &&
              !u.pattern_mode(),
          "pat.unknown_id_fails_with_err_naming_the_id");

    err = "pre-set";
    const bool empty = u.prepare_pattern(p, "", err);
    CHECK(!empty && !err.empty() && !u.has_pattern(), "pat.empty_id_selects_nothing");

    // A failed prepare after a successful one: stored pattern, buffer and flag stay exactly as they
    // were. Prepare A, arm the flag, break things twice, then play: the wrap is still A's own P.
    dz::Player w;
    err.clear();
    const bool prepared = w.prepare_pattern(p, "A", err);
    w.set_pattern_mode(true);
    std::string err1;
    std::string err2;
    const bool bad1 = w.prepare_pattern(p, "nope", err1);
    const bool bad2 = w.prepare_pattern(p, "", err2);
    const bool intact_state = prepared && !bad1 && !bad2 && !err1.empty() &&
                              err1.find("nope") != std::string::npos && !err2.empty() &&
                              w.has_pattern() && w.pattern_mode();
    w.play();
    const long long pa = static_cast<long long>(op::sample_at_beat(p, op::pattern_extent(p.patterns[0])));
    bool wraps_still = intact_state && w.playing();
    long long g = 0;
    const int blocks = static_cast<int>((2 * pa) / kBlock) + 1;  // past the SECOND wrap
    op::wav::StereoBuffer ref4;
    if (!render_pattern_ref(p, 0, 4, ref4, err)) wraps_still = false;
    float l[kBlock];
    float r[kBlock];
    for (int b = 0; b < blocks && wraps_still; ++b) {
        std::fill(l, l + kBlock, kSentinel);
        std::fill(r, r + kBlock, kSentinel);
        w.process(l, r, kBlock);
        for (int i = 0; i < kBlock && wraps_still; ++i, ++g)
            if (!sample_equal(ref4, g % pa, l[i], r[i])) wraps_still = false;
    }
    CHECK(wraps_still && w.playing() && w.position_frames() == g % pa,
          "pat.failed_prepare_after_success_changes_nothing");
}

// ---------------------------------------------------------------------------
// Preconditions of the byte-compare itself (§6): A's statement P = 72000 < the padded loops=1
// buffer's 96000 frames, P % 128 = 64 (wraps never land on a block edge), and the multi-loop
// reference really is periodic — ref[f] == ref[f - P] for every frame f in [P, (N-1)*P) — because
// that is what every pattern-mode byte-compare below rests on.

static void test_preconditions() {
    std::string err;
    const op::Project p = make_project();
    const long long pa = static_cast<long long>(op::sample_at_beat(p, op::pattern_extent(p.patterns[0])));
    op::wav::StereoBuffer one;
    const bool r1 = render_pattern_ref(p, 0, 1, one, err);
    const long long frames1 = static_cast<long long>(one.frames());
    CHECK(r1 && pa == 72000 && frames1 == 96000 && pa < frames1 && pa % kBlock == 64,
          "pat.precondition_statement_shorter_than_padded_buffer_and_unaligned");

    const long long pb = static_cast<long long>(op::sample_at_beat(p, op::pattern_extent(p.patterns[1])));
    CHECK(pb == 42000 && pb % kBlock != 0, "pat.precondition_B_derived_extent_P_42000");

    constexpr long long kN = 4;
    op::wav::StereoBuffer ref4;
    bool periodic = render_pattern_ref(p, 0, kN, ref4, err);
    for (long long f = pa; periodic && f < (kN - 1) * pa; ++f) {
        const float* a = ref4.interleaved.data() + 2 * static_cast<size_t>(f);
        const float* b = ref4.interleaved.data() + 2 * static_cast<size_t>(f - pa);
        if (a[0] != b[0] || a[1] != b[1]) periodic = false;
    }
    CHECK(periodic && static_cast<long long>(ref4.frames()) >= (kN - 1) * pa,
          "pat.precondition_multi_loop_render_is_periodic_with_P");
}

// ---------------------------------------------------------------------------
// §3 rules 2/3.5: a successful prepare clears err, stores the window (has_pattern() is and stays
// true) and hands the loops=1 render over as the buffer being played — but never enables the flag:
// with the flag off that buffer plays one-shot, byte-identical, then zeros, then parks at 0.

static void test_prepare_success_one_shot() {
    std::string err;
    const op::Project p = make_project();
    dz::Player q;
    err = "pre-set non-empty";
    const bool ok = q.prepare_pattern(p, "A", err);
    CHECK(ok && err.empty() && q.has_pattern() && !q.pattern_mode(),
          "pat.success_clears_err_stores_window_flag_untouched");

    op::wav::StereoBuffer one;
    if (!render_pattern_ref(p, 0, 1, one, err)) {
        CHECK(false, "pat.reference_loops1_render_failed");
        return;
    }
    const long long total = static_cast<long long>(one.frames());
    q.play();
    float l[kBlock];
    float r[kBlock];
    bool bit_exact = true;
    long long g = 0;
    const int blocks = static_cast<int>(total / kBlock) + 2;  // past the end: the stop must fire inside
    for (int b = 0; b < blocks && bit_exact; ++b) {
        std::fill(l, l + kBlock, kSentinel);
        std::fill(r, r + kBlock, kSentinel);
        q.process(l, r, kBlock);
        for (int i = 0; i < kBlock && bit_exact; ++i, ++g) {
            const float* want = g < total ? one.interleaved.data() + 2 * static_cast<size_t>(g) : nullptr;
            const float want_l = want != nullptr ? want[0] : 0.0f;
            const float want_r = want != nullptr ? want[1] : 0.0f;
            if (std::memcmp(&l[i], &want_l, sizeof(float)) != 0 ||
                std::memcmp(&r[i], &want_r, sizeof(float)) != 0)
                bit_exact = false;  // no tolerance
        }
    }
    CHECK(bit_exact && !q.playing() && q.position_frames() == 0 && q.frames_played() == total &&
              q.has_pattern(),
          "pat.with_flag_off_the_pattern_buffer_is_one_shot_byte_identical");
}

// ---------------------------------------------------------------------------
// The headline (§3 rule 4): pattern mode on, 2255 callbacks of 128 frames spanning MORE than four
// wraps of A (288640 frames > 4 * 72000): every slot bit-identical to the loops >= 4 pattern-mode
// render read at frame g mod P; playing() stays true; position_frames() == frames driven mod P once
// past the first wrap (and a block ending exactly on the window end leaves it at 0); frames_played()
// equals the frames driven and is never reset by a wrap.

static void test_pattern_mode_wraps_byte_exact() {
    std::string err;
    const op::Project p = make_project();
    const long long pa = static_cast<long long>(op::sample_at_beat(p, op::pattern_extent(p.patterns[0])));
    op::wav::StereoBuffer ref4;
    if (!render_pattern_ref(p, 0, 4, ref4, err)) {
        CHECK(false, "pat.headline_reference_render_failed");
        return;
    }

    dz::Player q;
    if (!q.prepare_pattern(p, "A", err)) {
        CHECK(false, "pat.headline_prepare_failed");
        return;
    }
    q.set_pattern_mode(true);
    q.play();

    const int blocks = (static_cast<int>(4 * pa) / kBlock) + 5;  // 2250 complete wraps + 5
    bool bit_exact = true;
    bool counters_ok = true;
    long long g = 0;
    float l[kBlock];
    float r[kBlock];
    for (int b = 0; b < blocks && bit_exact; ++b) {
        std::fill(l, l + kBlock, kSentinel);
        std::fill(r, r + kBlock, kSentinel);
        q.process(l, r, kBlock);
        for (int i = 0; i < kBlock && bit_exact; ++i, ++g)
            if (!sample_equal(ref4, g % pa, l[i], r[i])) bit_exact = false;  // memcmp-equal, no tolerance
        if (!q.playing()) counters_ok = false;
        if (g > pa && q.position_frames() != g % pa) counters_ok = false;  // rule 4: position == driven mod P
        if (q.frames_played() != g) counters_ok = false;                   // never reset by a wrap
        // 4 * P = 288000 is a whole number of blocks (2250): that block ends exactly on the window
        // end, and the position parks at 0 — not at 288000 % P taken from the un-wrapped counter.
        if (b == static_cast<int>(4 * pa) / kBlock - 1 && q.position_frames() != 0) counters_ok = false;
    }
    CHECK(bit_exact && g > 4 * pa, "pat.headline_pattern_output_is_byte_exact_over_four_wraps");
    CHECK(counters_ok && q.playing() && q.frames_played() == g && q.position_frames() == g % pa &&
              g % pa < pa,
          "pat.headline_counters_hold_through_every_wrap");
}

// ---------------------------------------------------------------------------
// §3 rules 6/7: pattern mode wins over the loop region. A window set with set_loop() is ignored
// while the flag is on and P > 0 — but keeps its state and its WP-0200 meaning with the flag off,
// even over a pattern buffer.

static void test_pattern_wins_over_loop_region() {
    std::string err;
    const op::Project p = make_project();
    const long long pa = static_cast<long long>(op::sample_at_beat(p, op::pattern_extent(p.patterns[0])));
    op::wav::StereoBuffer ref4;
    op::wav::StereoBuffer one;
    if (!render_pattern_ref(p, 0, 4, ref4, err) || !render_pattern_ref(p, 0, 1, one, err)) {
        CHECK(false, "pat.loop_interaction_reference_failed");
        return;
    }

    // Flag on + set_loop(48000, 90000), a window strictly inside the 96000-frame pattern buffer:
    // the output follows the pattern's own P = 72000, not the loop window.
    dz::Player q;
    if (!q.prepare_pattern(p, "A", err)) {
        CHECK(false, "pat.loop_interaction_prepare_failed");
        return;
    }
    q.set_loop(48000, 90000);
    q.set_pattern_mode(true);
    q.play();
    bool follows_pattern = true;
    long long g = 0;
    const int blocks = static_cast<int>((pa + pa) / kBlock) + 1;  // well past one wrap of P
    float l[kBlock];
    float r[kBlock];
    for (int b = 0; b < blocks && follows_pattern; ++b) {
        std::fill(l, l + kBlock, kSentinel);
        std::fill(r, r + kBlock, kSentinel);
        q.process(l, r, kBlock);
        for (int i = 0; i < kBlock && follows_pattern; ++i, ++g)
            if (!sample_equal(ref4, g % pa, l[i], r[i])) follows_pattern = false;
    }
    CHECK(follows_pattern && q.playing() && q.has_loop() && q.pattern_mode(),
          "pat.pattern_mode_ignores_the_loop_region_while_the_flag_is_on");

    // Same loop, flag off: the loop region wraps exactly as in WP-0200 — pre-loop pass to frame
    // 90000, then [48000, 90000) forever — over the pattern buffer.
    dz::Player s;
    if (!s.prepare_pattern(p, "A", err)) {
        CHECK(false, "pat.loop_interaction_prepare_2_failed");
        return;
    }
    s.set_loop(48000, 90000);
    s.play();  // the flag was never set
    bool follows_loop = true;
    g = 0;
    const int blocks2 = static_cast<int>((90000 + pa) / kBlock) + 1;
    for (int b = 0; b < blocks2 && follows_loop; ++b) {
        std::fill(l, l + kBlock, kSentinel);
        std::fill(r, r + kBlock, kSentinel);
        s.process(l, r, kBlock);
        for (int i = 0; i < kBlock && follows_loop; ++i, ++g) {
            const long long src = g < 90000 ? g : expected_source(g, 48000, 90000, 0);
            if (!sample_equal(one, src, l[i], r[i])) follows_loop = false;
        }
    }
    CHECK(follows_loop && s.playing() && s.has_loop() && !s.pattern_mode(),
          "pat.with_flag_off_the_loop_region_wraps_as_before");
}

// ---------------------------------------------------------------------------
// §3 rule 7 (song mode stays byte-identical) + §3 rule 3.5 (a later submit() replaces the buffer
// but keeps the window and has_pattern() true): prepare A, then submit the full song render — with
// the flag off the output is one pass of the song render, zeros past it, parked at 0.

static void test_song_mode_byte_identical() {
    std::string err;
    const op::Project p = make_project();
    op::wav::StereoBuffer song;
    if (!op::render_project(p, "", song, err)) {
        CHECK(false, "pat.song_render_failed");
        return;
    }
    op::wav::StereoBuffer one;
    if (!render_pattern_ref(p, 0, 1, one, err)) {
        CHECK(false, "pat.song_reference_render_failed");
        return;
    }
    // The arrangement really differs from one statement of A (the sine track, and more metronome):
    // song mode and pattern mode are audibly different projects, so a mix-up would be visible.
    bool differs = false;
    const long long cmp = std::min(static_cast<long long>(song.frames()), static_cast<long long>(one.frames()));
    for (long long f = 0; f < cmp && !differs; ++f) {
        if (song.interleaved[static_cast<size_t>(2 * f)] != one.interleaved[static_cast<size_t>(2 * f)])
            differs = true;
    }
    CHECK(differs, "pat.song_and_pattern_audio_differ_in_the_project");

    dz::Player q;
    if (!q.prepare_pattern(p, "A", err)) {
        CHECK(false, "pat.song_prepare_failed");
        return;
    }
    q.submit(std::make_unique<op::wav::StereoBuffer>(song));  // submit replaces the buffer, keeps the window
    q.play();  // flag off
    bool bit_exact = true;
    long long g = 0;
    const long long total = static_cast<long long>(song.frames());
    const int blocks = static_cast<int>(total / kBlock) + 2;
    float l[kBlock];
    float r[kBlock];
    for (int b = 0; b < blocks && bit_exact; ++b) {
        std::fill(l, l + kBlock, kSentinel);
        std::fill(r, r + kBlock, kSentinel);
        q.process(l, r, kBlock);
        for (int i = 0; i < kBlock && bit_exact; ++i, ++g) {
            const float* want = g < total ? song.interleaved.data() + 2 * static_cast<size_t>(g) : nullptr;
            const float want_l = want != nullptr ? want[0] : 0.0f;
            const float want_r = want != nullptr ? want[1] : 0.0f;
            if (std::memcmp(&l[i], &want_l, sizeof(float)) != 0 ||
                std::memcmp(&r[i], &want_r, sizeof(float)) != 0)
                bit_exact = false;
        }
    }
    CHECK(bit_exact && !q.playing() && q.position_frames() == 0 && q.frames_played() == total &&
              q.has_pattern() && !q.pattern_mode(),
          "pat.song_mode_after_prepare_is_byte_identical_one_pass");
}

// ---------------------------------------------------------------------------
// §3 rule 3.5: last prepare wins — A then B → the wrap is B's derived 42000, matching B's loops
// render (and err cleared by the second success).

static void test_last_prepare_wins() {
    std::string err;
    const op::Project p = make_project();
    dz::Player q;
    if (!q.prepare_pattern(p, "A", err) || !q.prepare_pattern(p, "B", err) || !err.empty()) {
        CHECK(false, "pat.last_prepare_wins_prepare_failed");
        return;
    }
    op::wav::StereoBuffer bref4;
    if (!render_pattern_ref(p, 1, 4, bref4, err)) {
        CHECK(false, "pat.last_prepare_wins_reference_failed");
        return;
    }
    const long long pb = 42000;
    q.set_pattern_mode(true);
    q.play();
    bool bit_exact = true;
    long long g = 0;
    const int blocks = static_cast<int>(3 * pb) / kBlock + 2;  // more than two wraps of B
    float l[kBlock];
    float r[kBlock];
    for (int b = 0; b < blocks && bit_exact; ++b) {
        std::fill(l, l + kBlock, kSentinel);
        std::fill(r, r + kBlock, kSentinel);
        q.process(l, r, kBlock);
        for (int i = 0; i < kBlock && bit_exact; ++i, ++g)
            if (!sample_equal(bref4, g % pb, l[i], r[i])) bit_exact = false;
    }
    CHECK(bit_exact && q.playing() && q.has_pattern() && q.position_frames() == g % pb &&
              q.frames_played() == g,
          "pat.last_prepare_wins_wraps_at_Bs_extent");
}

// ---------------------------------------------------------------------------
// §3 rule 6: the flag OFF mid-playback neither seeks nor silences — playback runs on one-shot to
// the end of the buffer (byte-identical), then stops and parks at frame 0. frames_played() is not
// reset by the switch (nor by any earlier wrap).

static void test_flag_off_mid_playback() {
    std::string err;
    const op::Project p = make_project();
    dz::Player q;
    if (!q.prepare_pattern(p, "A", err)) {
        CHECK(false, "pat.mid_playback_prepare_failed");
        return;
    }
    op::wav::StereoBuffer one;
    if (!render_pattern_ref(p, 0, 1, one, err)) {
        CHECK(false, "pat.mid_playback_reference_failed");
        return;
    }
    q.set_pattern_mode(true);
    q.play();
    // Drive 700 blocks = 89600 frames: one wrap of 72000 happened, entry position is 17600.
    const int pre = 700;
    bool wrapped = true;
    long long g = 0;
    float l[kBlock];
    float r[kBlock];
    for (int b = 0; b < pre && wrapped; ++b) {
        std::fill(l, l + kBlock, kSentinel);
        std::fill(r, r + kBlock, kSentinel);
        q.process(l, r, kBlock);
        for (int i = 0; i < kBlock && wrapped; ++i, ++g)
            if (!sample_equal(one, g % 72000, l[i], r[i])) wrapped = false;
    }
    const bool wrapped_once = wrapped && q.playing() && q.position_frames() == 17600;
    q.set_pattern_mode(false);
    const bool no_seek = q.position_frames() == 17600 && q.playing() && !q.pattern_mode() &&
                         q.frames_played() == g;  // no seek, no silence, no counter change
    // From here the buffer runs out one-shot: frames 17600..95999 byte-identical (slot g carries
    // buffer frame g - 72000, the no-seek continuation of the wrapped counter), then zeros, and
    // only then does playback stop and park at 0.
    const long long runout = 96000 - 17600;  // 78400 frames
    const long long end_global = g + runout;  // the global slot at which the buffer is exhausted
    const int blocks = static_cast<int>(runout / kBlock) + 3;
    bool runout_ok = no_seek;
    for (int b = 0; b < blocks && runout_ok; ++b) {
        std::fill(l, l + kBlock, kSentinel);
        std::fill(r, r + kBlock, kSentinel);
        q.process(l, r, kBlock);
        for (int i = 0; i < kBlock && runout_ok; ++i, ++g) {
            const long long src = g < end_global ? g - 72000 : -1;  // 17600 + (g - 89600)
            if (src < 0) {
                if (l[i] != 0.0f || r[i] != 0.0f) runout_ok = false;
            } else if (!sample_equal(one, src, l[i], r[i])) {
                runout_ok = false;
            }
        }
    }
    CHECK(wrapped_once && no_seek && runout_ok && !q.playing() && q.position_frames() == 0 &&
              q.frames_played() == 89600 + runout,
          "pat.flag_off_mid_playback_runs_out_one_shot_then_parks");
}

// ---------------------------------------------------------------------------
// §3 rule 5 (clamping: a submitted buffer shorter than P wraps at its own frame count; the window
// survives a submit) + rule 8 (stop()/play() restart at frame 0) + mono (right == nullptr).

static void test_clamp_short_buffer_mono_restart() {
    std::string err;
    const op::Project p = make_project();
    dz::Player q;
    if (!q.prepare_pattern(p, "A", err)) {
        CHECK(false, "pat.clamp_prepare_failed");
        return;
    }
    q.submit(make_ramp_buffer(500, 9000.0f));  // shorter than P = 72000 → clamp to [0, 500)
    q.set_pattern_mode(true);
    q.play();
    const bool window_kept = q.has_pattern() && q.pattern_mode();
    bool wrap_ok = window_kept;
    long long pos = 0;
    constexpr int kBlocks = 10;  // 1280 frames over a 500-frame window: two wraps
    for (int b = 0; b < kBlocks && wrap_ok; ++b) {
        const Drive blk = drive(q, 1, 9000.0f, false);  // mono: right == nullptr
        for (int i = 0; i < kBlock && wrap_ok; ++i)
            if (blk.src[static_cast<size_t>(i)] != expected_source(pos, 0, 500, i)) wrap_ok = false;
        pos = expected_source(pos, 0, 500, kBlock);
        if (!blk.playing || !q.playing()) wrap_ok = false;
        if (b >= 1 && !(blk.position >= 0 && blk.position < 500)) wrap_ok = false;
    }
    CHECK(wrap_ok && window_kept && q.playing() && q.position_frames() == pos && pos == 1280 % 500,
          "pat.short_submitted_buffer_clamps_the_pattern_window_and_mono_wraps");

    // Rule 8: stop() silences and parks at 0; play() restarts at frame 0 of the buffer, wrapped.
    q.stop();
    const bool stopped_home = !q.playing() && q.position_frames() == 0;
    float l[kBlock];
    std::fill(l, l + kBlock, kSentinel);
    q.process(l, nullptr, kBlock);
    const bool silent = std::all_of(l, l + kBlock, [](float v) { return v == 0.0f; });
    q.play();
    const Drive restart = drive(q, 1, 9000.0f, false);
    bool from_zero = true;
    for (int i = 0; i < kBlock && from_zero; ++i)
        if (restart.src[static_cast<size_t>(i)] != expected_source(0, 0, 500, i)) from_zero = false;
    CHECK(stopped_home && silent && from_zero && q.position_frames() == kBlock % 500 && q.playing(),
          "pat.stop_then_play_restarts_at_frame_zero");
}

int main() {
    test_defaults();
    test_prepare_failures();
    test_preconditions();
    test_prepare_success_one_shot();
    test_pattern_mode_wraps_byte_exact();
    test_pattern_wins_over_loop_region();
    test_song_mode_byte_identical();
    test_last_prepare_wins();
    test_flag_off_mid_playback();
    test_clamp_short_buffer_mono_restart();

    if (g_failures == 0) std::printf("ALL PASS (0 failures)\n");
    else std::printf("%d FAILURE(S)\n", g_failures);
    return g_failures;
}
