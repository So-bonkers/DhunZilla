// Unit tests for WP-0209 (`opd audition`): the op::audition module (constants, to_frames,
// build_project, render) and the CLI command — including the core contract that every accepted
// audition is bit-identical to `opd render` of the equivalent one-track project. Boring
// assert-based runner — no framework deps, own main. Exit code = number of failures. All files are
// written into one per-process scratch directory under temp_directory_path() (pid in the name) and
// removed at the end; nothing outside it is created. Runs the opd binary via OPD_BIN.
// Run: ./build/op_audition_tests
#include <sys/wait.h>
#include <unistd.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "engine/audition.hpp"
#include "engine/dsp.hpp"
#include "engine/model.hpp"
#include "engine/render.hpp"
#include "engine/wav.hpp"

static int g_failures = 0;
#define CHECK(cond, name)                                                              \
    do {                                                                               \
        if (cond) std::printf("PASS %s\n", name);                                      \
        else { std::printf("FAIL %s  (%s:%d)\n", name, __FILE__, __LINE__); ++g_failures; } \
    } while (0)

namespace fs = std::filesystem;
using op::audition::Options;

// ---------------------------------------------------------------- scratch directory + file helpers

static const fs::path& scratch_dir() {
    static const fs::path dir = [] {
        const fs::path p = fs::temp_directory_path() /
                           ("op_audition_tests_" + std::to_string(static_cast<long>(::getpid())));
        std::error_code ec;
        fs::remove_all(p, ec);
        fs::create_directories(p, ec);
        return p;
    }();
    return dir;
}

static void fatal(const std::string& m) { std::printf("FATAL: %s\n", m.c_str()); std::exit(2); }

static std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static void write_file(const fs::path& p, const std::string& text) {
    std::ofstream of(p, std::ios::binary | std::ios::trunc);
    of << text;
}

static bool files_equal(const fs::path& a, const fs::path& b) {
    const std::string x = read_file(a), y = read_file(b);
    return !x.empty() && x == y;
}

// ---------------------------------------------------------------- assets (float32 WAVs, exact round-trip)

static std::vector<float> g_a48;   // the 48 kHz stereo asset samples (interleaved L,R)
static std::vector<float> g_a441;  // the 44.1 kHz stereo asset samples

// Writes an F32 stereo WAV with deterministic content; returns the interleaved samples written.
static std::vector<float> write_asset(const fs::path& p, int rate, long long frames) {
    op::wav::StereoBuffer b;
    b.sample_rate = rate;
    b.interleaved.resize(static_cast<size_t>(frames) * 2u);
    for (long long n = 0; n < frames; ++n) {
        const double t = static_cast<double>(n) / static_cast<double>(rate);
        b.interleaved[static_cast<size_t>(n) * 2 + 0] =
            static_cast<float>(0.50 * std::sin(6.283185307179586 * 440.0 * t));
        b.interleaved[static_cast<size_t>(n) * 2 + 1] =
            static_cast<float>(-0.25 * std::cos(6.283185307179586 * 330.0 * t));
    }
    std::string err;
    if (!op::wav::write_wav_stereo(p.string(), b, op::wav::SampleFormat::F32, err))
        fatal("asset write: " + err);
    return b.interleaved;
}

static bool eq_buf(const op::wav::StereoBuffer& a, const op::wav::StereoBuffer& b) {
    return a.sample_rate == b.sample_rate && a.interleaved.size() == b.interleaved.size() &&
           std::memcmp(a.interleaved.data(), b.interleaved.data(),
                       a.interleaved.size() * sizeof(float)) == 0;
}

static bool all_zero(const op::wav::StereoBuffer& b, size_t from) {
    for (size_t i = from; i < b.interleaved.size(); ++i)
        if (b.interleaved[i] != 0.0f) return false;
    return true;
}

// The default-track channel gains the mixer applies to a centred 0 dB track (equal_power_pan(0)).
static float g_gl = 1.0f, g_gr = 1.0f;

// ---------------------------------------------------------------- opd runner

struct Run {
    int code = -1;
    std::string out;  // stdout
    std::string err;  // stderr (captured to a scratch file, so one run per call)
};

static Run run_opd(const std::string& args) {  // generic: args start with the subcommand ("render ...")
    static int seq = 0;
    const fs::path errf = scratch_dir() / ("stderr_" + std::to_string(seq++) + ".txt");
    Run r;
    const std::string cmd = std::string("\"") + OPD_BIN + "\" " + args + " 2>\"" + errf.string() + "\"";
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) return r;
    char buf[256];
    while (std::fgets(buf, sizeof buf, pipe)) r.out += buf;
    const int status = ::pclose(pipe);
    r.code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    r.err = read_file(errf);
    return r;
}

static Run run_aud(const std::string& args) { return run_opd("audition " + args); }  // audition <args>

static bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// One error contract: exit code, stderr's LAST line (the usage block may precede it), and the fact
// that nothing was written to the intended output path.
static void check_err(const char* name, const std::string& args, int want_code,
                      const std::string& want_last_line, const fs::path& must_not_exist) {
    const Run r = run_aud(args);
    CHECK(r.code == want_code && ends_with(r.err, want_last_line) && !fs::exists(must_not_exist), name);
}

// ---------------------------------------------------------------- WAV header probe (canonical pcm16 stereo)

static uint16_t rd_u16(const std::string& s, size_t o) {
    return static_cast<uint16_t>(static_cast<unsigned char>(s[o]) |
                                 (static_cast<unsigned char>(s[o + 1]) << 8));
}
static uint32_t rd_u32(const std::string& s, size_t o) {
    return static_cast<uint32_t>(static_cast<unsigned char>(s[o])) |
           (static_cast<uint32_t>(static_cast<unsigned char>(s[o + 1])) << 8) |
           (static_cast<uint32_t>(static_cast<unsigned char>(s[o + 2])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(s[o + 3])) << 24);
}

// True iff p is a canonical pcm16 stereo WAV at want_rate Hz with exactly want_frames frames.
static bool is_pcm16_stereo(const fs::path& p, long long want_frames, uint32_t want_rate) {
    const std::string s = read_file(p);
    if (s.size() != 44 + static_cast<size_t>(want_frames) * 4u) return false;
    return s.compare(0, 4, "RIFF") == 0 && s.compare(8, 4, "WAVE") == 0 && s.compare(12, 4, "fmt ") == 0 &&
           rd_u32(s, 16) == 16u && rd_u16(s, 20) == 1u && rd_u16(s, 22) == 2u && rd_u32(s, 24) == want_rate &&
           rd_u32(s, 28) == want_rate * 4u && rd_u16(s, 32) == 4u && rd_u16(s, 34) == 16u &&
           s.compare(36, 4, "data") == 0 && rd_u32(s, 40) == static_cast<uint32_t>(want_frames * 4);
}

// ---------------------------------------------------------------- the equivalent-project .opj text

static std::string d2s(double v) {
    char b[64];
    std::snprintf(b, sizeof b, "%.17g", v);
    return std::string(b);
}

// The one-track sampler project build_project describes, as canonical-ish JSON the loader accepts.
static std::string equiv_opj(const std::string& asset_path, int note, int root, double start_s, bool has_end,
                             double end_s, long long frames) {
    const double dur = static_cast<double>(frames) / 32768.0;  // spb at kBpm is exactly 32768 samples/beat
    std::string v = "{\"kind\": \"sampler\", \"path\": \"" + asset_path + "\"";
    if (start_s != 0.0) v += ", \"start_s\": " + d2s(start_s);
    if (has_end) v += ", \"end_s\": " + d2s(end_s);
    if (root >= 0) v += ", \"root_note\": " + std::to_string(root);
    std::string n = "{\"t\": 0.0, \"dur\": " + d2s(dur);
    if (note >= 0) n += ", \"note\": " + std::to_string(note);
    n += "}";
    return "{\n  \"format_version\": 0,\n  \"bpm\": 87.890625,\n  \"sample_rate\": 48000,\n  \"seed\": 42,\n"
           "  \"length_bars\": " + d2s(dur / 4.0) + ",\n  \"beats_per_bar\": 4,\n"
           "  \"tracks\": [ { \"name\": \"audition\", \"voice\": " + v + " } } ],\n"
           "  \"patterns\": [ { \"track\": 0, \"notes\": [ " + n + " ] } ]\n}\n";
}

// A hand-written equivalent Project (independent of build_project) for engine-level bit-exactness.
static op::Project hand_project(const std::string& path, int note, int root, double start_s, bool has_end,
                                double end_s, long long frames) {
    op::Project p;
    p.format_version = 0;
    p.bpm = 87.890625;  // == 5625/64: spb is exactly 32768 samples per beat at 48 kHz
    p.sample_rate = 48000;
    p.seed = 42;
    p.beats_per_bar = 4;
    const double dur = static_cast<double>(frames) / 32768.0;
    p.length_bars = dur / 4.0;
    op::Track t;
    t.name = "audition";
    t.voice.kind = op::VoiceKind::Sampler;
    t.voice.path = path;
    t.voice.start_s = start_s;
    t.voice.end_s = end_s;
    t.voice.has_end_s = has_end;
    t.voice.root_note = root;
    p.tracks.push_back(t);
    op::Pattern pat;
    pat.track = 0;
    op::Note note_row;
    note_row.t_beats = 0.0;
    note_row.dur_beats = dur;
    note_row.midi_note = note;
    note_row.velocity = 1.0;
    pat.notes.push_back(note_row);
    p.patterns.push_back(pat);
    return p;
}

// ------------------------------------------------------------------- the tests

int main() {
    const fs::path dir = scratch_dir();
    const std::string d = dir.string();
    float l, r;
    op::dsp::equal_power_pan(0.0f, l, r);  // == the renderer's default-track gains (db_to_lin(0) == 1.0f)
    g_gl = l;
    g_gr = r;

    const fs::path a48 = dir / "asset48.wav";
    const fs::path a441 = dir / "asset441.wav";
    const fs::path tiny = dir / "tiny48.wav";
    const fs::path bad = dir / "bad.bin";
    g_a48 = write_asset(a48, 48000, 48000);   // 1 s of deterministic stereo content
    g_a441 = write_asset(a441, 44100, 4410);  // 0.1 s at 44.1 kHz (engine resamples on load)
    write_asset(tiny, 48000, 100);             // exists but shorter than any trim slice used below
    write_file(bad, "this is not audio, just text bytes");

    // ---- constants ----
    CHECK(op::audition::kBpm == 5625.0 / 64.0, "const: kBpm == 5625/64 exactly");
    CHECK(op::audition::kSampleRate == 48000 && op::audition::kBeatsPerBar == 4, "const: rate 48000, 4 beats per bar");
    CHECK(op::audition::kMaxSeconds == 300.0 && op::audition::kDefaultFrames == 96000LL,
          "const: kMaxSeconds 300, kDefaultFrames 96000");

    // ---- to_frames (llround(seconds * 48000), product clamped to [-4e18, 4e18]) ----
    CHECK(op::audition::to_frames(2.0) == 96000LL, "to_frames: 2.0 -> 96000");
    CHECK(op::audition::to_frames(0.0125) == 600LL, "to_frames: 0.0125 -> 600");
    CHECK(op::audition::to_frames(0.0375) == 1800LL, "to_frames: 0.0375 -> 1800");
    CHECK(op::audition::to_frames(0.0001) == 5LL, "to_frames: 0.0001 -> 5 (4.8 rounds up)");
    CHECK(op::audition::to_frames(0.0) == 0LL, "to_frames: 0.0 -> 0");
    CHECK(op::audition::to_frames(-0.5) == -24000LL, "to_frames: -0.5 -> -24000");
    CHECK(op::audition::to_frames(1e30) == static_cast<long long>(std::llround(4e18)) &&
              op::audition::to_frames(-1e30) == -static_cast<long long>(std::llround(4e18)),
          "to_frames: absurd magnitudes clamp to +/-4e18 before llround");

    // ---- build_project shape (the worked example: 23520 frames, note 72 root 60, slice [0.01, 0.5)) ----
    Options o;
    o.path = "kick.wav";
    o.note = 72;
    o.root = 60;
    o.start_s = 0.01;
    o.end_s = 0.5;
    o.has_end_s = true;
    o.frames = 23520;
    const op::Project p = op::audition::build_project(o);
    CHECK(p.format_version == 0 && p.bpm == op::audition::kBpm && p.sample_rate == 48000 && p.seed == 42 &&
              p.beats_per_bar == 4,
          "shape: header fields (bpm kBpm, 48000, seed 42, 4/4)");
    CHECK(p.length_bars == 0.179443359375, "shape: length_bars == 0.179443359375 (worked example)");
    CHECK(p.tracks.size() == 1 && p.patterns.size() == 1, "shape: exactly one track and one pattern");
    const op::Track& t = p.tracks[0];
    CHECK(t.name == "audition" && t.voice.kind == op::VoiceKind::Sampler && t.voice.path == "kick.wav",
          "shape: track 'audition', sampler voice, path verbatim");
    CHECK(t.voice.start_s == 0.01 && t.voice.end_s == 0.5 && t.voice.has_end_s && t.voice.root_note == 60,
          "shape: voice trim + root fields passed through");
    CHECK(t.gain_db == 0.0f && t.pan == 0.0f && !t.mute && !t.solo && t.choke_group == 0,
          "shape: mixer defaults on the track");
    const op::Pattern& pat = p.patterns[0];
    CHECK(pat.track == 0 && pat.notes.size() == 1 && pat.swing == 0.0 && !pat.length_beats.has_value(),
          "shape: one pattern at track 0, no length_beats, no swing");
    const op::Note& nr = pat.notes[0];
    CHECK(nr.t_beats == 0.0 && nr.dur_beats == 0.7177734375 && nr.midi_note == 72 && nr.velocity == 1.0,
          "shape: note {t 0, dur 0.7177734375, note 72, vel 1}");
    CHECK(p.arrangement.empty() && p.tempo_map.empty() && p.master_gain_db == 0.0f,
          "shape: empty arrangement + tempo map, unity master");

    // ---- total_samples_for(build_project(o)) == max(frames, 1), incl. 1 and a 300 s request ----
    long long counts[] = {1, 2, 600, 48000, 96000, 14400000, 23520};
    bool ts_ok = true;
    for (long long f : counts) {
        Options c = o;
        c.frames = f;
        if (op::total_samples_for(op::audition::build_project(c)) != f) ts_ok = false;
    }
    CHECK(ts_ok, "total: exact frame count for 7 sizes incl. 1 and 300 s");
    Options zero = o;
    zero.frames = 0;
    Options neg = o;
    neg.frames = -7;
    CHECK(op::total_samples_for(op::audition::build_project(zero)) == 1 &&
              op::total_samples_for(op::audition::build_project(neg)) == 1,
          "total: frames 0 and negative clamp to 1");

    // ---- engine bit-exactness vs a hand-written equivalent project (plain / repitch up / down / resampled) ----
    auto render_both_equal = [&](const char* name, Options eo, const std::string& hp, int hn, int hroot,
                                 double hs, bool he, double he_end, long long hf) {
        op::wav::StereoBuffer a, b;
        std::string e1, e2;
        const bool ok_a = op::audition::render(eo, d, a, e1);
        op::Project q = hand_project(hp, hn, hroot, hs, he, he_end, hf);
        const bool ok_b = op::render_project(q, d, b, e2);
        CHECK(ok_a && ok_b && eq_buf(a, b), name);
    };
    {
        Options eo;
        eo.path = "asset48.wav";  // plain: no pitch, no trim, default length
        render_both_equal("exact.plain: audition == hand project render", eo, "asset48.wav", -1, -1, 0.0, false,
                          0.0, op::audition::kDefaultFrames);
    }
    {
        Options eo;
        eo.path = "asset48.wav";
        eo.note = 72;
        eo.root = 60;
        eo.frames = 4800;  // repitch up +12 (ratio exactly 2.0)
        render_both_equal("exact.repitch_up: audition == hand project render", eo, "asset48.wav", 72, 60, 0.0,
                          false, 0.0, 4800);
    }
    {
        Options eo;
        eo.path = "asset48.wav";
        eo.note = 48;
        eo.root = 60;
        eo.start_s = 0.01;
        eo.end_s = 0.05;
        eo.has_end_s = true;
        eo.frames = 1920;  // repitch down -12 (ratio exactly 0.5) with a trim slice
        render_both_equal("exact.repitch_down: audition == hand project render", eo, "asset48.wav", 48, 60,
                          0.01, true, 0.05, 1920);
    }
    {
        Options eo;
        eo.path = "asset441.wav";
        eo.note = 60;
        eo.root = 57;  // odd shift: ratio 2^(3/12), exercises the resampler + interpolation together
        eo.frames = 24000;
        render_both_equal("exact.resampled: audition == hand project render (44.1 kHz asset)", eo,
                          "asset441.wav", 60, 57, 0.0, false, 0.0, 24000);
    }

    // ---- the sampler voice really does the work: +12 -> source frame 2n at output frame n ----
    {
        Options eo;
        eo.path = "asset48.wav";
        eo.note = 72;
        eo.root = 60;  // ratio exactly 2.0, frac always 0 -> exact scaled copies, no interpolation
        op::wav::StereoBuffer buf;
        std::string err;
        const bool ok = op::audition::render(eo, d, buf, err);
        bool samples_ok = ok && buf.frames() == static_cast<size_t>(op::audition::kDefaultFrames);
        for (long long n = 0; samples_ok && n < 2000; ++n) {
            const float el = g_a48[static_cast<size_t>(2 * n) * 2 + 0] * g_gl;
            const float er = g_a48[static_cast<size_t>(2 * n) * 2 + 1] * g_gr;
            if (buf.interleaved[static_cast<size_t>(n) * 2 + 0] != el ||
                buf.interleaved[static_cast<size_t>(n) * 2 + 1] != er)
                samples_ok = false;
        }
        // The source (48000 frames) runs out at output frame 24000 -> silence from there on.
        CHECK(samples_ok, "voice.+12: output frame n == asset frame 2n * channel gain");
        CHECK(ok && buf.interleaved[static_cast<size_t>(23999) * 2] != 0.0f &&
                  all_zero(buf, static_cast<size_t>(24000) * 2),
              "voice.+12: audio through frame 23999, silence after the source runs out");
    }

    // ---- -12 -> interpolated at half positions (ratio exactly 0.5) ----
    {
        Options eo;
        eo.path = "asset48.wav";
        eo.note = 48;
        eo.root = 60;
        eo.frames = 9600;
        op::wav::StereoBuffer buf;
        std::string err;
        const bool ok = op::audition::render(eo, d, buf, err);
        bool samples_ok = ok && buf.frames() == 9600;
        for (long long n = 1; samples_ok && n < 100; n += 2) {  // odd n -> frac 0.5 between asset frames
            const size_t k = static_cast<size_t>(n / 2);
            const double al = g_a48[k * 2 + 0], bl = g_a48[(k + 1) * 2 + 0];
            const double ar = g_a48[k * 2 + 1], br = g_a48[(k + 1) * 2 + 1];
            const float el = static_cast<float>(al + 0.5 * (bl - al)) * g_gl;
            const float er = static_cast<float>(ar + 0.5 * (br - ar)) * g_gr;
            if (buf.interleaved[static_cast<size_t>(n) * 2 + 0] != el ||
                buf.interleaved[static_cast<size_t>(n) * 2 + 1] != er)
                samples_ok = false;
        }
        CHECK(samples_ok, "voice.-12: odd output frames are the exact midpoint interpolations");
    }

    // ---- trim slice [600, 1800) plays, then silence (same frame indices the renderer slices on) ----
    {
        Options eo;
        eo.path = "asset48.wav";
        eo.start_s = 0.0125;
        eo.end_s = 0.0375;
        eo.has_end_s = true;
        eo.frames = 96000;  // --seconds 2: the slice is 1200 frames, everything after it is silence
        op::wav::StereoBuffer buf;
        std::string err;
        const bool ok = op::audition::render(eo, d, buf, err);
        bool slice_ok = ok && buf.frames() == 96000;
        for (long long n = 0; slice_ok && n < 1200; ++n) {
            const float el = g_a48[static_cast<size_t>(600 + n) * 2 + 0] * g_gl;
            const float er = g_a48[static_cast<size_t>(600 + n) * 2 + 1] * g_gr;
            if (buf.interleaved[static_cast<size_t>(n) * 2 + 0] != el ||
                buf.interleaved[static_cast<size_t>(n) * 2 + 1] != er)
                slice_ok = false;
        }
        CHECK(slice_ok, "trim: frames [600,1800) of the asset play first, scaled only by channel gain");
        CHECK(ok && all_zero(buf, static_cast<size_t>(1200) * 2), "trim: silence after the slice ends");
    }

    // ---- two engine renders are identical (determinism) ----
    {
        Options eo;
        eo.path = "asset48.wav";
        eo.note = 63;
        eo.root = 60;
        eo.start_s = 0.02;
        op::wav::StereoBuffer a, b;
        std::string e1, e2;
        CHECK(op::audition::render(eo, d, a, e1) && op::audition::render(eo, d, b, e2) && eq_buf(a, b),
              "determinism: two engine renders are bit-identical");
    }

    // ---- CLI: default run — stdout line byte-exact, pcm16/stereo/48000 header, 96000 frames ----
    const std::string a48s = a48.string();
    const fs::path out_def = dir / "cli_default.wav";
    {
        const Run run = run_aud("\"" + a48s + "\" -o \"" + out_def.string() + "\"");
        CHECK(run.code == 0 && run.out == "auditioned 96000 frames @48000 Hz -> " + out_def.string() + "\n" &&
                  run.err.empty(),
              "cli.default: exit 0, exact stdout line, nothing on stderr");
        CHECK(is_pcm16_stereo(out_def, 96000, 48000u), "cli.default: canonical pcm16 stereo 48000 Hz, 96000 frames");
    }

    // ---- CLI length rules (six): default / --seconds / --end-s span / start+end span / --seconds wins /
    //      --start-s alone keeps the default ----
    {
        const fs::path o1 = dir / "cli_sec.wav";
        const Run s = run_aud("\"" + a48s + "\" -o \"" + o1.string() + "\" --seconds 1.5");
        CHECK(s.code == 0 && s.out == "auditioned 72000 frames @48000 Hz -> " + o1.string() + "\n" &&
                  is_pcm16_stereo(o1, 72000, 48000u),
              "cli.len: --seconds 1.5 -> exactly llround(1.5*48000) = 72000 frames");
        const fs::path o2 = dir / "cli_span.wav";
        const Run sp = run_aud("\"" + a48s + "\" -o \"" + o2.string() + "\" --start-s 0.0125 --end-s 0.0375");
        CHECK(sp.code == 0 && sp.out == "auditioned 1200 frames @48000 Hz -> " + o2.string() + "\n" &&
                  is_pcm16_stereo(o2, 1200, 48000u),
              "cli.len: --start-s/--end-s -> llround(B*48k) - llround(A*48k) = 1200 frames");
        const fs::path o3 = dir / "cli_win.wav";
        const Run w = run_aud("\"" + a48s + "\" -o \"" + o3.string() +
                              "\" --start-s 0.0125 --end-s 0.0375 --seconds 1.0");
        CHECK(w.code == 0 && is_pcm16_stereo(o3, 48000, 48000u), "cli.len: --seconds wins over the slice span");
        const fs::path o4 = dir / "cli_endonly.wav";
        const Run e2 = run_aud("\"" + a48s + "\" -o \"" + o4.string() + "\" --end-s 0.5");
        CHECK(e2.code == 0 && is_pcm16_stereo(o4, 24000, 48000u), "cli.len: --end-s alone -> span from 0 = 24000");
        const fs::path o5 = dir / "cli_startonly.wav";
        const Run st = run_aud("\"" + a48s + "\" -o \"" + o5.string() + "\" --start-s 0.01");
        CHECK(st.code == 0 && is_pcm16_stereo(o5, 96000, 48000u), "cli.len: --start-s alone keeps 96000 default");
        const fs::path o6 = dir / "cli_rep.wav";  // repeated flag: last value wins
        const Run rv = run_aud("\"" + a48s + "\" -o \"" + o6.string() + "\" --seconds 1.0 --seconds 0.5");
        CHECK(rv.code == 0 && is_pcm16_stereo(o6, 24000, 48000u), "cli.len: repeated --seconds, last value wins");
    }

    // ---- CLI determinism (byte-identical files) + read-only asset + only the output file appears ----
    {
        const fs::path dir_ro = dir / "ro";
        fs::create_directories(dir_ro);
        const std::string ro_asset = (dir_ro / "a.wav").string();
        write_asset(dir_ro / "a.wav", 48000, 24000);
        const std::string asset_before = read_file(dir_ro / "a.wav");
        std::vector<std::string> names_before;
        for (const auto& e : fs::directory_iterator(dir_ro)) names_before.push_back(e.path().filename().string());
        const fs::path r1 = dir / "det_1.wav", r2 = dir / "det_2.wav";
        const Run first = run_aud("\"" + ro_asset + "\" -o \"" + r1.string() + "\" --seconds 0.5");
        const Run second = run_aud("\"" + ro_asset + "\" -o \"" + r2.string() + "\" --seconds 0.5");
        CHECK(first.code == 0 && second.code == 0 && files_equal(r1, r2), "cli.determinism: byte-identical outputs");
        std::vector<std::string> names_after;
        for (const auto& e : fs::directory_iterator(dir_ro)) names_after.push_back(e.path().filename().string());
        CHECK(read_file(dir_ro / "a.wav") == asset_before && names_before == names_after,
              "cli.readonly: asset unchanged and nothing written next to it");
    }

    // ---- CLI relative path resolves against the caller's working directory (empty base_dir) ----
    {
        const fs::path rel_out = dir / "ro" / "rel_out.wav";
        std::error_code ec;
        const fs::path saved_cwd = fs::current_path(ec);
        fs::current_path(dir / "ro", ec);
        const Run rel = run_aud("a.wav -o rel_out.wav --seconds 0.25");
        fs::current_path(saved_cwd, ec);
        CHECK(rel.code == 0 && is_pcm16_stereo(rel_out, 12000, 48000u),
              "cli.relative: relative in/out paths resolve against the cwd");
    }

    // ---- bit-exact equivalence end-to-end vs `opd render` of the equivalent .opj ----
    {
        // (a) the worked example from the task file.
        const fs::path wa_aud = dir / "worked_audition.wav", wa_opj = dir / "worked.opj", wa_ren = dir / "worked_render.wav";
        const Run a = run_aud("\"" + a48s + "\" -o \"" + wa_aud.string() +
                              "\" --note 72 --root 60 --start-s 0.01 --end-s 0.5");
        write_file(wa_opj, equiv_opj(a48s, 72, 60, 0.01, true, 0.5, 23520));
        const Run b = run_opd("render \"" + wa_opj.string() + "\" -o \"" + wa_ren.string() + "\"");
        CHECK(a.code == 0 && b.code == 0 && is_pcm16_stereo(wa_aud, 23520, 48000u) && files_equal(wa_aud, wa_ren),
              "equiv.cli: worked example audition == opd render of the equivalent .opj, byte-for-byte");

        // (b) an unpitched, untrimmed case.
        const fs::path up_aud = dir / "plain_audition.wav", up_opj = dir / "plain.opj", up_ren = dir / "plain_render.wav";
        const Run c = run_aud("\"" + a48s + "\" -o \"" + up_aud.string() + "\" --seconds 1.0");
        write_file(up_opj, equiv_opj(a48s, -1, -1, 0.0, false, 0.0, 48000));
        const Run e = run_opd("render \"" + up_opj.string() + "\" -o \"" + up_ren.string() + "\"");
        CHECK(c.code == 0 && e.code == 0 && files_equal(up_aud, up_ren),
              "equiv.cli: unpitched untrimmed audition == equivalent .opj render");

        // (c) a 44.1 kHz asset with an odd repitch.
        const fs::path r44_aud = dir / "r44_audition.wav", r44_opj = dir / "r44.opj", r44_ren = dir / "r44_render.wav";
        const Run f = run_aud("\"" + a441.string() + "\" -o \"" + r44_aud.string() +
                              "\" --note 60 --root 57 --seconds 0.5");
        write_file(r44_opj, equiv_opj(a441.string(), 60, 57, 0.0, false, 0.0, 24000));
        const Run g = run_opd("render \"" + r44_opj.string() + "\" -o \"" + r44_ren.string() + "\"");
        CHECK(f.code == 0 && g.code == 0 && files_equal(r44_aud, r44_ren),
              "equiv.cli: 44.1 kHz repitched audition == equivalent .opj render");
    }

    // ---- every usage error: exit 2, exact message (last stderr line), no output file written ----
    const std::string eout = ("\"" + (dir / "never_written.wav").string() + "\"");
    check_err("err.no_input", "-o " + eout, 2, "error: usage: audition needs <in.wav>\n", dir / "never_written.wav");
    check_err("err.missing_o", "\"" + a48s + "\"", 2, "error: usage: missing -o <out.wav>\n",
              dir / "never_written.wav");
    check_err("err.o_empty", "\"" + a48s + "\" -o \"\"", 2, "error: usage: missing -o <out.wav>\n",
              dir / "never_written.wav");
    check_err("err.sec_bad", "\"" + a48s + "\" -o " + eout + " --seconds abc", 2,
              "error: usage: bad --seconds value 'abc'\n", dir / "never_written.wav");
    check_err("err.sec_nan", "\"" + a48s + "\" -o " + eout + " --seconds inf", 2,
              "error: usage: bad --seconds value 'inf'\n", dir / "never_written.wav");
    check_err("err.sec_zero", "\"" + a48s + "\" -o " + eout + " --seconds 0", 2,
              "error: usage: --seconds must be in (0, 300]\n", dir / "never_written.wav");
    check_err("err.sec_big", "\"" + a48s + "\" -o " + eout + " --seconds 301", 2,
              "error: usage: --seconds must be in (0, 300]\n", dir / "never_written.wav");
    check_err("err.start_bad", "\"" + a48s + "\" -o " + eout + " --start-s x", 2,
              "error: usage: bad --start-s value 'x'\n", dir / "never_written.wav");
    check_err("err.start_neg", "\"" + a48s + "\" -o " + eout + " --start-s -0.5", 2,
              "error: usage: --start-s must be >= 0\n", dir / "never_written.wav");
    check_err("err.end_bad", "\"" + a48s + "\" -o " + eout + " --end-s 1x", 2,
              "error: usage: bad --end-s value '1x'\n", dir / "never_written.wav");
    check_err("err.end_le_start", "\"" + a48s + "\" -o " + eout + " --start-s 0.5 --end-s 0.5", 2,
              "error: usage: --end-s must be greater than --start-s\n", dir / "never_written.wav");
    check_err("err.end_neg_start0", "\"" + a48s + "\" -o " + eout + " --end-s 0", 2,
              "error: usage: --end-s must be greater than --start-s\n", dir / "never_written.wav");
    for (const char* badv : {"128", "-1", "60.5", "C3", ""}) {
        const std::string name = std::string("err.note_bad_") + (badv[0] ? badv : "empty");
        check_err(name.c_str(), "\"" + a48s + "\" -o " + eout + " --note '" + badv + "' --root 60", 2,
                  "error: usage: bad --note value '" + std::string(badv) + "'\n", dir / "never_written.wav");
    }
    check_err("err.root_bad", "\"" + a48s + "\" -o " + eout + " --note 60 --root 128", 2,
              "error: usage: bad --root value '128'\n", dir / "never_written.wav");
    check_err("err.note_only", "\"" + a48s + "\" -o " + eout + " --note 60", 2,
              "error: usage: --note and --root must be given together\n", dir / "never_written.wav");
    check_err("err.root_only", "\"" + a48s + "\" -o " + eout + " --root 60", 2,
              "error: usage: --note and --root must be given together\n", dir / "never_written.wav");
    check_err("err.unknown_opt", "--bogus \"" + a48s + "\" -o " + eout, 2,
              "error: usage: unknown arg '--bogus'\n", dir / "never_written.wav");
    check_err("err.second_pos", "\"" + a48s + "\" \"" + a48s + "\" -o " + eout, 2,
              "error: usage: unknown arg '" + a48s + "'\n", dir / "never_written.wav");
    check_err("err.order_o_before_sec", "\"" + a48s + "\" --seconds abc", 2,
              "error: usage: missing -o <out.wav>\n", dir / "never_written.wav");
    check_err("err.not_found", "\"" + (dir / "nope.wav").string() + "\" -o " + eout, 3,
              "error: not_found: " + (dir / "nope.wav").string() + "\n", dir / "never_written.wav");

    // ---- render errors exit 5 and write nothing: undecodable file, empty trim slice ----
    check_err("err.undecodable_code", "\"" + bad.string() + "\" -o " + eout, 5, "", dir / "never_written.wav");
    {
        const Run u = run_aud("\"" + bad.string() + "\" -o " + eout);
        CHECK(u.code == 5 && ends_with(u.err, "\n") &&
                  u.err.find("error: render: sampler:") != std::string::npos &&
                  !fs::exists(dir / "never_written.wav"),
              "err.undecodable: exit 5 with the sampler's own message, nothing written");
    }
    check_err("err.empty_slice", "\"" + tiny.string() + "\" -o " + eout + " --start-s 0.0125 --end-s 0.0375", 5,
              "error: render: sampler: trim slice is empty\n", dir / "never_written.wav");

    fs::remove_all(dir);
    if (g_failures == 0) std::printf("ALL PASS\n");
    else std::printf("%d FAILURES\n", g_failures);
    return g_failures;
}
