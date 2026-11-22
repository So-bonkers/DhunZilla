// Unit tests for clip trim in MIDI export (WP-0193c): `op::save_midi` applies the clip window
// [offset_beats, offset_beats + length_beats) over the unrolled source timeline (spec
// project-model.md §Clip trim) so exported events land at base + (s - offset). Boring assert-based
// runner — no framework deps, own main. Exit code = number of failures. Scratch .mid files go to
// <temp_directory_path()>/op_clip_trim_midi_<pid>; positions are read back with op::load_midi
// (beats = tick / 480), never with a second SMF parser. Every beat used is a multiple of 0.125, so
// every exported tick is exact and beats compare exactly or within 1e-9.
// Run: ./build/op_clip_trim_midi_tests
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

#include "engine/midi.hpp"
#include "engine/model.hpp"

static int g_failures = 0;
#define CHECK(cond, name)                                                              \
    do {                                                                               \
        if (cond) std::printf("PASS %s\n", name);                                      \
        else { std::printf("FAIL %s  (%s:%d)\n", name, __FILE__, __LINE__); ++g_failures; } \
    } while (0)

static std::filesystem::path g_dir;  // <temp>/op_clip_trim_midi_<pid>

static std::string fpath(const char* name) { return (g_dir / name).string(); }

static op::Note mk_note(double t, double dur, int midi) {
    op::Note n;
    n.t_beats = t;
    n.dur_beats = dur;
    n.midi_note = midi;
    return n;
}

// One track ("lead", synth), one pattern on that track, and the given clips.
static op::Project mk_project(std::vector<op::Note> notes, std::vector<op::Clip> clips) {
    op::Project p;
    p.bpm = 120.0;
    p.sample_rate = 48000;
    p.seed = 42;
    p.length_bars = 8.0;
    p.beats_per_bar = 4;
    op::Track tr;
    tr.name = "lead";
    tr.voice.kind = op::VoiceKind::Synth;
    tr.voice.freq_hz = 440.0f;
    p.tracks.push_back(tr);
    op::Pattern pat;
    pat.track = 0;
    pat.notes = std::move(notes);
    p.patterns.push_back(std::move(pat));
    p.arrangement = std::move(clips);
    return p;
}

static op::Clip mk_clip(int pattern, double start_bar, int repeats) {
    op::Clip c;
    c.pattern = pattern;
    c.start_bar = start_bar;
    c.repeats = repeats;
    return c;
}

// The worked-example pattern of §1: notes at t = 0, 1, 2, 3 with dur 0.5 → E = max(t + dur) = 3.5.
static std::vector<op::Note> four_notes_e35() {
    return {mk_note(0.0, 0.5, 60), mk_note(1.0, 0.5, 61), mk_note(2.0, 0.5, 62), mk_note(3.0, 0.5, 63)};
}

// Four notes at t = 0..3 with dur 1 → E = 4 (used for the exclusive-upper-bound case).
static std::vector<op::Note> four_notes_e4() {
    return {mk_note(0.0, 1.0, 60), mk_note(1.0, 1.0, 61), mk_note(2.0, 1.0, 62), mk_note(3.0, 1.0, 63)};
}

// One expected exported event: MIDI pitch, onset in beats, duration in beats.
struct Ev {
    int pitch;
    double t;
    double dur;
};

static bool ev_less(const Ev& a, const Ev& b) {
    return a.t != b.t ? a.t < b.t : a.pitch < b.pitch;
}

// True iff `got` is exactly the multiset `want` (pitches equal, beats within 1e-9).
static bool match(const std::vector<op::Note>& got, const std::vector<Ev>& want) {
    if (got.size() != want.size()) return false;
    std::vector<Ev> g;
    for (const op::Note& n : got) g.push_back(Ev{n.midi_note, n.t_beats, n.dur_beats});
    std::sort(g.begin(), g.end(), ev_less);
    std::vector<Ev> w = want;
    std::sort(w.begin(), w.end(), ev_less);
    for (size_t i = 0; i < w.size(); ++i)
        if (g[i].pitch != w[i].pitch || std::fabs(g[i].t - w[i].t) > 1e-9 ||
            std::fabs(g[i].dur - w[i].dur) > 1e-9)
            return false;
    return true;
}

// All note events in the file, across every pattern load_midi built, sorted by (t_beats, midi_note).
static bool read_notes(const char* name, std::vector<op::Note>& out_notes, std::string& err) {
    op::Project back;
    if (!op::load_midi(fpath(name), back, err)) return false;
    out_notes.clear();
    for (const op::Pattern& pat : back.patterns)
        for (const op::Note& n : pat.notes) out_notes.push_back(n);
    std::sort(out_notes.begin(), out_notes.end(), [](const op::Note& a, const op::Note& b) {
        return a.t_beats != b.t_beats ? a.t_beats < b.t_beats : a.midi_note < b.midi_note;
    });
    return true;
}

static bool read_all_bytes(const char* name, std::vector<uint8_t>& out) {
    std::ifstream f(fpath(name), std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

int main() {
    // Scratch dir: pid-scoped (never a fixed /tmp/op_* path); drop any stale one first.
    g_dir = std::filesystem::temp_directory_path() /
            ("op_clip_trim_midi_" + std::to_string(static_cast<long long>(::getpid())));
    std::error_code sec;
    std::filesystem::remove_all(g_dir, sec);
    std::filesystem::create_directories(g_dir, sec);
    CHECK(std::filesystem::is_directory(g_dir), "scratch dir created");

    std::string err;

    // ---- 1. Untrimmed clip, two repeats: placement base + r*E + n.t (pre-WP-0193c path). ----
    {
        const op::Project p = mk_project(four_notes_e35(), {mk_clip(0, 0.0, 2)});
        CHECK(op::save_midi(fpath("untrimmed.mid"), p, err), "1 save_midi untrimmed returns true");
        std::vector<op::Note> n;
        CHECK(read_notes("untrimmed.mid", n, err), "1 untrimmed file loads");
        // E = 3.5: repeat 0 at 0..3, repeat 1 at 3.5..6.5 — all eight notes present.
        CHECK(match(n, {{60, 0.0, 0.5}, {61, 1.0, 0.5}, {62, 2.0, 0.5}, {63, 3.0, 0.5},
                        {60, 3.5, 0.5}, {61, 4.5, 0.5}, {62, 5.5, 0.5}, {63, 6.5, 0.5}}),
              "1 untrimmed: all notes at base + r*E + n.t");
    }

    // ---- 2 + 3. The §1 worked example: offset_beats only, material slides to the clip start. ----
    {
        op::Clip c = mk_clip(0, 0.0, 2);
        c.offset_beats = 2.0;  // len = R*E - w0 = 7 - 2 = 5 → window [2, 7)
        const op::Project p = mk_project(four_notes_e35(), {c});
        CHECK(op::save_midi(fpath("worked.mid"), p, err), "2 save_midi offset-only returns true");
        std::vector<op::Note> n;
        CHECK(read_notes("worked.mid", n, err), "2 offset-only file loads");
        // s = 2,3,3.5,4.5,5.5,6.5 → beats 0,1,1.5,2.5,3.5,4.5 (ticks 0,480,720,1200,1680,2160).
        CHECK(match(n, {{62, 0.0, 0.5}, {63, 1.0, 0.5}, {60, 1.5, 0.5}, {61, 2.5, 0.5},
                        {62, 3.5, 0.5}, {63, 4.5, 0.5}}),
              "2 worked example: six notes at base + (s - offset)");
        CHECK(!n.empty() && n.front().t_beats == 0.0 && n.front().midi_note == 62,
              "3 first exported note sits on s = offset_beats at tick 0");
    }

    // ---- 4. offset_beats + length_beats: exactly the notes with w0 <= s < w0 + len. ----
    {
        op::Clip c = mk_clip(0, 0.0, 1);
        c.offset_beats = 1.0;
        c.length_beats = 2.0;  // E = 3.5 → window [1, 3): keeps s = 1, 2 (s = 3 sits on the end)
        const op::Project p = mk_project(four_notes_e35(), {c});
        CHECK(op::save_midi(fpath("off_len.mid"), p, err), "4 save_midi offset+length returns true");
        std::vector<op::Note> n;
        CHECK(read_notes("off_len.mid", n, err), "4 offset+length file loads");
        CHECK(match(n, {{61, 0.0, 0.5}, {62, 1.0, 0.5}}), "4 window [1,3): only s = 1 and 2 export");
    }

    // ---- 4b. The window is over the *unrolled* timeline: it can span a repeat boundary. ----
    {
        op::Clip c = mk_clip(0, 0.0, 2);
        c.offset_beats = 2.0;
        c.length_beats = 2.0;  // window [2, 4): s = 2, 3 (repeat 0) and s = 3.5 (note t=0 of repeat 1)
        const op::Project p = mk_project(four_notes_e35(), {c});
        CHECK(op::save_midi(fpath("off_len_span.mid"), p, err), "4b save_midi spanning window returns true");
        std::vector<op::Note> n;
        CHECK(read_notes("off_len_span.mid", n, err), "4b spanning file loads");
        CHECK(match(n, {{62, 0.0, 0.5}, {63, 1.0, 0.5}, {60, 1.5, 0.5}}),
              "4b window counts source positions across repeats (s = 3.5 plays)");
    }

    // ---- 5. Window end is exclusive: a note sitting exactly on w0 + len is dropped. ----
    {
        op::Clip c = mk_clip(0, 0.0, 1);
        c.offset_beats = 1.0;
        c.length_beats = 2.0;  // E = 4 → window [1, 3): keeps s = 1, 2; drops the note on s = 3
        const op::Project p = mk_project(four_notes_e4(), {c});
        CHECK(op::save_midi(fpath("exclusive.mid"), p, err), "5 save_midi exclusive-end returns true");
        std::vector<op::Note> n;
        CHECK(read_notes("exclusive.mid", n, err), "5 exclusive-end file loads");
        CHECK(match(n, {{61, 0.0, 1.0}, {62, 1.0, 1.0}}), "5 note on the window end (s = 3) excluded");
    }

    // ---- 6. Non-integer start_bar: base = 1.5 bars * 4 = 6 beats, measured after the shift. ----
    {
        op::Clip c = mk_clip(0, 1.5, 1);
        c.offset_beats = 1.0;  // E = 4 → len = 3 → window [1, 4) → s = 1,2,3 at beats 6,7,8
        const op::Project p = mk_project(four_notes_e4(), {c});
        CHECK(op::save_midi(fpath("start_bar.mid"), p, err), "6 save_midi start_bar 1.5 returns true");
        std::vector<op::Note> n;
        CHECK(read_notes("start_bar.mid", n, err), "6 start_bar file loads");
        CHECK(match(n, {{61, 6.0, 1.0}, {62, 7.0, 1.0}, {63, 8.0, 1.0}}),
              "6 trimmed notes land at base 6 + (s - offset)");
    }

    // ---- 7. length_beats longer than the material: only the existing repeats are scanned. ----
    {
        op::Clip c = mk_clip(0, 0.0, 2);
        c.offset_beats = 3.0;
        c.length_beats = 10.0;  // window [3, 13) reaches past the material (R*E = 7)
        const op::Project p = mk_project(four_notes_e35(), {c});
        CHECK(op::save_midi(fpath("long_len.mid"), p, err), "7 save_midi long length returns true");
        std::vector<op::Note> n;
        CHECK(read_notes("long_len.mid", n, err), "7 long-length file loads");
        // Only the two existing repeats are scanned: s = 3, 3.5, 4.5, 5.5, 6.5 → beats 0 … 3.5.
        CHECK(match(n, {{63, 0.0, 0.5}, {60, 0.5, 0.5}, {61, 1.5, 0.5}, {62, 2.5, 0.5}, {63, 3.5, 0.5}}),
              "7 length past the material adds no extra repeats");
    }

    // ---- 8. offset_beats past the material (len <= 0): true return, zero note events. ----
    {
        op::Clip c = mk_clip(0, 0.0, 1);
        c.offset_beats = 4.0;  // E = 3.5 → len = -0.5 ≤ 0 → nothing plays
        const op::Project p = mk_project(four_notes_e35(), {c});
        CHECK(op::save_midi(fpath("empty.mid"), p, err), "8 save_midi returns true for an empty window");
        std::vector<op::Note> n;
        std::string load_err;
        CHECK(!read_notes("empty.mid", n, load_err) &&
                  load_err == "midi: file contains no playable note events",
              "8 exported file has no playable note events");
        std::vector<uint8_t> bytes;
        CHECK(read_all_bytes("empty.mid", bytes) && !bytes.empty(), "8 empty-window file still written");
    }

    // ---- 9. Durations are not shortened: a note ringing past the window end keeps its full dur. ----
    {
        const std::vector<op::Note> notes = {mk_note(0.0, 0.5, 60), mk_note(3.0, 2.0, 63)};  // E = 5
        op::Clip c = mk_clip(0, 0.0, 1);
        c.offset_beats = 2.0;
        c.length_beats = 1.5;  // window [2, 3.5): only s = 3 → beat 1.0, dur stays 2.0
        const op::Project p = mk_project(notes, {c});
        CHECK(op::save_midi(fpath("long_note.mid"), p, err), "9 save_midi long note returns true");
        std::vector<op::Note> n;
        CHECK(read_notes("long_note.mid", n, err), "9 long-note file loads");
        CHECK(match(n, {{63, 1.0, 2.0}}), "9 onset gated but dur_beats 2.0 kept past the window end");
    }

    // ---- 10. Legacy project (no arrangement): every pattern once at bar 0. ----
    {
        op::Project p = mk_project({mk_note(0.0, 0.5, 60)}, {});
        op::Pattern second;
        second.track = 0;
        second.notes = {mk_note(1.5, 0.25, 64)};
        p.patterns.push_back(second);
        CHECK(p.arrangement.empty() && op::save_midi(fpath("legacy.mid"), p, err),
              "10 save_midi legacy project returns true");
        std::vector<op::Note> n;
        CHECK(read_notes("legacy.mid", n, err), "10 legacy file loads");
        CHECK(match(n, {{60, 0.0, 0.5}, {64, 1.5, 0.25}}),
              "10 legacy: both patterns exported once at bar 0");
    }

    // ---- 11. Two clips of one pattern with different trims → independent windows. ----
    {
        op::Clip a = mk_clip(0, 0.0, 1);
        a.length_beats = 1.0;  // window [0, 1): s = 0 → beat 0
        op::Clip b = mk_clip(0, 2.0, 1);
        b.offset_beats = 2.0;
        b.length_beats = 1.0;  // window [2, 3): s = 2 → base 8 + 0 = beat 8
        const op::Project p = mk_project(four_notes_e4(), {a, b});
        CHECK(op::save_midi(fpath("two_clips.mid"), p, err), "11 save_midi two trimmed clips returns true");
        std::vector<op::Note> n;
        CHECK(read_notes("two_clips.mid", n, err), "11 two-clip file loads");
        CHECK(match(n, {{60, 0.0, 1.0}, {62, 8.0, 1.0}}),
              "11 per-clip windows: offset_beats is not shared between clips");
    }

    // ---- 12. Byte determinism: the same trimmed project exports byte-identically twice. ----
    {
        op::Clip c = mk_clip(0, 0.5, 3);
        c.offset_beats = 1.25;
        c.length_beats = 4.5;
        const op::Project p = mk_project(four_notes_e35(), {c});
        const bool ok1 = op::save_midi(fpath("det_a.mid"), p, err);
        const bool ok2 = op::save_midi(fpath("det_b.mid"), p, err);
        std::vector<uint8_t> a, b;
        CHECK(ok1 && ok2 && read_all_bytes("det_a.mid", a) && read_all_bytes("det_b.mid", b) && !a.empty() &&
                  a == b,
              "12 same trimmed project exports byte-identically");
    }

    // ---- 13. offset_beats exactly 0 with no length keeps the untrimmed path (same bytes as 1). ----
    {
        op::Clip c = mk_clip(0, 0.0, 2);
        c.offset_beats = 0.0;  // explicit zero = untrimmed
        const op::Project p = mk_project(four_notes_e35(), {c});
        CHECK(op::save_midi(fpath("zero_off.mid"), p, err), "13 save_midi offset 0 returns true");
        std::vector<uint8_t> a, b;
        CHECK(read_all_bytes("untrimmed.mid", a) && read_all_bytes("zero_off.mid", b) && a == b,
              "13 explicit offset 0 exports byte-identically to an absent key");
    }

    // ---- 14. Offset landing inside the second repeat: src_base + n.t is what gets windowed. ----
    {
        op::Clip c = mk_clip(0, 0.0, 2);
        c.offset_beats = 4.0;  // len = 7 - 4 = 3 → window [4, 7): s = 4.5, 5.5, 6.5 only
        const op::Project p = mk_project(four_notes_e35(), {c});
        CHECK(op::save_midi(fpath("mid_repeat.mid"), p, err), "14 save_midi mid-repeat offset returns true");
        std::vector<op::Note> n;
        CHECK(read_notes("mid_repeat.mid", n, err), "14 mid-repeat file loads");
        CHECK(match(n, {{61, 0.5, 0.5}, {62, 1.5, 0.5}, {63, 2.5, 0.5}}),
              "14 window inside repeat 2: nothing from repeat 1 leaks");
    }

    if (g_failures == 0) std::printf("ALL PASS\n");
    else std::printf("%d FAILURES\n", g_failures);
    return g_failures;
}
