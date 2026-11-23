// opd — DhunZilla headless CLI. Subcommands: render | gen-testproject | info.
// Exit codes (spec/project-model.md): 0 ok, 2 usage, 3 not-found, 4 parse, 5 render/write error.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#include "engine/analyze.hpp"
#include "engine/audition.hpp"
#include "engine/bundle.hpp"
#include "engine/dsp.hpp"
#include "engine/lint.hpp"
#include "engine/limiter.hpp"
#include "engine/loudness.hpp"
#include "engine/meters.hpp"
#include "engine/midi.hpp"
#include "engine/model.hpp"
#include "engine/normalize.hpp"
#include "engine/ops.hpp"
#include "engine/peaks.hpp"
#include "engine/render.hpp"
#include "engine/sample_index.hpp"
#include "engine/wav.hpp"

namespace fs = std::filesystem;

static void usage() {
    std::fprintf(stderr,
        "usage:\n"
        "  opd render <project.opj> -o <out.wav> [--seed N] [--stems-dir DIR] [--from-bar A] [--to-bar B] [--loop]\n"
        "             [--pattern NAME [--loops N]]\n"
        "             [--normalize-lufs L [--ceiling-dbfs C]]\n"
        "             [--format pcm16|pcm24|f32]\n"
        "             [--limit-dbfs X]   (master brickwall limiter on the final mix, docs/spec/limiter.md)\n"
        "  opd click <project.opj> -o <out.wav>\n"
        "  opd new <out.opj> [--bpm X] [--bars N] [--beats-per-bar N] [--sample-rate SR]\n"
        "          [--track NAME:KIND[:PATH]]... [--force]   (KIND = sine|synth|sampler|metronome)\n"
        "  opd gen-testproject <out.opj>     (also writes kits/kick.wav next to it)\n"
        "  opd midi2opj <in.mid> -o <out.opj>\n"
        "  opd import-midi <proj.opj> <in.mid> [-o out.opj]   (merge as new tracks)\n"
        "  opd attach <proj.opj> --path P --kind stem|transcription|other [--producer S] [-o out]\n"
        "  opd apply <proj.opj> <ops.opjl> [-o out.opj]   (apply a JSON-Lines ops journal, all-or-nothing)\n"
        "  opd diff <a.opj> <b.opj> -o <edits.opjl>   (write the journal that transforms a into b)\n"
        "  opd opj2mid <in.opj> -o <out.mid>\n"
        "  opd pack <in.opj> -o <song.opd>    (project + samples + attachments in one file)\n"
        "  opd unpack <song.opd> -d <out-dir>\n"
        "  opd info <file>   (.wav | .opj | .opd | .mid)\n"
        "  opd analyze <in.wav> [--json]   (objective audio features as JSON)\n"
        "  opd lint <proj.opj> [--json]   (read-only project sanity checks as JSON)\n"
        "  opd peaks <in.wav> [--bins N]   (min/max waveform peaks per bin as JSON)\n"
        "  opd meters <project.opj>   (per-track and master peak/RMS dBFS + integrated LUFS as JSON)\n"
        "  opd loudness <in.wav> [--json]   (true peak dBTP, momentary/short-term max LUFS, loudness range as JSON)\n"
        "  opd normalize <in.wav> -o <out.wav> --lufs <T> [--ceiling-db <C>] [--format pcm16|pcm24|f32]\n"
        "  opd clips <proj.opj>   (arrangement clips as canonical JSON — read-only, model order)\n"
        "  opd steps <proj.opj> <pattern-index> [--steps N]   (print the 16th-step grid, x = on)\n"
        "  opd timeline <project.opj> [--bars N]   (per-bar ruler: start beat, sample, seconds + tempo as JSON)\n"
        "  opd audition <in.wav> -o <out.wav> [--note N --root R] [--start-s A --end-s B] [--seconds S]\n"
        "             (one note through the sampler voice - resample/trim/repitch - no project needed)\n"
        "  opd samples <dir> [-o <index.json>]   (JSON index of the audio files under a directory)\n");
}

static int fail(const char* code, const std::string& detail) {
    std::fprintf(stderr, "error: %s: %s\n", code, detail.c_str());
    return code[0] == 'u' ? 2 : (std::strstr(code, "not_found") ? 3 : (std::strstr(code, "parse") ? 4 : 5));
}

static std::string sanitize_track_name(const std::string& in) {
    std::string out;
    for (char c : in) {
        if (std::isalnum(static_cast<unsigned char>(c))) out += static_cast<char>(std::tolower(c));
        else if ((c == ' ' || c == '-' || c == '_') && !(out.empty() || out.back() == '_')) out += '_';
    }
    return out;
}

// WP-0145 (spec "Render a bar range"): a bar value is a finite number and the WHOLE argument must parse.
// Rejects "", "abc", "1x", "nan", "inf". Example: parse_bar_value("2.5", v) → true, v == 2.5.
static bool parse_bar_value(const std::string& arg, double& out) {
    char* end = nullptr;
    const double v = std::strtod(arg.c_str(), &end);
    if (end != arg.c_str() + arg.size()) return false;  // nothing consumed or trailing junk
    if (!std::isfinite(v)) return false;                // nan / inf are not bars
    out = v;
    return true;
}

// Defined below (near opd steps); used by cmd_new too.
static bool parse_decimal(const std::string& s, long long max, long long& out);

// WP-0176 (docs/spec/limiter.md): master brickwall peak limiter on the final mix, run immediately before the
// WAV is written. Stereo-linked, look-ahead state starts fresh at frame 0 of what is written. Usage:
//   if (has_limit && !run_master_limiter(buf, limit_db, err)) return fail("render", err);
static bool run_master_limiter(op::wav::StereoBuffer& mix, double ceiling_dbfs, std::string& err) {
    op::limiter::Stats stats;  // the limiter prints nothing of its own (spec §CLI)
    return op::limiter::apply(mix.interleaved, 2, mix.sample_rate, ceiling_dbfs, stats, err);
}

static int cmd_render(int argc, char** argv) {
    if (argc < 4) { usage(); return fail("usage", "render needs project + -o out"); }
    const std::string proj = argv[1];
    std::string out_path, stems_dir;
    long long seed_override = -1;
    bool has_from = false, has_to = false;
    bool want_loop = false;  // WP-0155: --loop renders the project's loop region (= --from-bar/--to-bar)
    std::string from_arg, to_arg;  // raw strings for error messages ("<B as given>")
    double bar_a = 0.0, bar_b = 0.0;
    bool has_limit = false;   // WP-0176: --limit-dbfs present on the final mix (docs/spec/limiter.md)
    std::string limit_arg;    // raw string for error messages ("<X as given>")
    double limit_db = 0.0;    // parsed value; validated with the other option-syntax checks below
    bool has_norm = false, has_ceiling = false;  // WP-0189: normalize while rendering (docs/spec/normalize.md)
    std::string norm_arg, ceiling_arg;           // raw strings for error messages ("<L as given>", "<C as given>")
    double norm_lufs = 0.0, norm_ceiling = -1.0; // parsed values; ceiling default per spec
    bool has_pattern = false, has_loops = false;  // WP-0192: pattern mode (spec §Pattern mode)
    std::string pattern_arg, loops_arg;           // raw strings for error messages ("<NAME as given>", ...)
    long long loops = 1;                          // parsed --loops value; default 1
    op::wav::SampleFormat fmt = op::wav::SampleFormat::Pcm16;  // default: existing behavior (WP-0149)
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "-o") && i + 1 < argc) out_path = argv[++i];
        else if (!std::strcmp(argv[i], "--stems-dir") && i + 1 < argc) stems_dir = argv[++i];
        else if (!std::strcmp(argv[i], "--seed") && i + 1 < argc) seed_override = std::atoll(argv[++i]);
        else if (!std::strcmp(argv[i], "--from-bar") && i + 1 < argc) { from_arg = argv[++i]; has_from = true; }
        else if (!std::strcmp(argv[i], "--to-bar") && i + 1 < argc) { to_arg = argv[++i]; has_to = true; }
        else if (!std::strcmp(argv[i], "--loop")) want_loop = true;  // takes no value
        else if (!std::strcmp(argv[i], "--format") && i + 1 < argc) {
            const std::string f = argv[++i];  // validated here, before the project loads: bad format → nothing written
            if (f == "pcm16") fmt = op::wav::SampleFormat::Pcm16;
            else if (f == "pcm24") fmt = op::wav::SampleFormat::Pcm24;
            else if (f == "f32") fmt = op::wav::SampleFormat::F32;
            else { usage(); return fail("usage", "unknown format '" + f + "' (expected pcm16, pcm24 or f32)"); }
        }
        else if (!std::strcmp(argv[i], "--limit-dbfs") && i + 1 < argc) { limit_arg = argv[++i]; has_limit = true; }
        else if (!std::strcmp(argv[i], "--normalize-lufs") && i + 1 < argc) { norm_arg = argv[++i]; has_norm = true; }
        else if (!std::strcmp(argv[i], "--ceiling-dbfs") && i + 1 < argc) { ceiling_arg = argv[++i]; has_ceiling = true; }
        else if (!std::strcmp(argv[i], "--pattern") && i + 1 < argc) { pattern_arg = argv[++i]; has_pattern = true; }
        else if (!std::strcmp(argv[i], "--loops") && i + 1 < argc) { loops_arg = argv[++i]; has_loops = true; }
        else { usage(); return fail("usage", std::string("unknown arg '") + argv[i] + "'"); }
    }
    if (out_path.empty()) { usage(); return fail("usage", "missing -o <out.wav>"); }
    if (want_loop && (has_from || has_to))
        return fail("usage", "--loop cannot be combined with --from-bar/--to-bar");

    // WP-0192 (spec §Pattern mode): pattern-mode option syntax, checked here — before the project file is
    // opened, so nothing is written on error — right after the existing --loop combination check above.
    if (has_pattern && want_loop) return fail("usage", "--pattern cannot be combined with --loop");
    if (has_pattern && (has_from || has_to))
        return fail("usage", "--pattern cannot be combined with --from-bar/--to-bar");
    if (has_loops && !has_pattern) return fail("usage", "--loops requires --pattern");
    if (has_loops && (!parse_decimal(loops_arg, 1000, loops) || loops < 1))
        { usage(); return fail("usage", "bad --loops value '" + loops_arg + "'"); }

    // WP-0189 (docs/spec/normalize.md §opd render --normalize-lufs): value checks with the other option-syntax
    // checks, i.e. before the project file is opened — nothing is written on any error. Repeating a flag: last wins.
    if (has_norm && !parse_bar_value(norm_arg, norm_lufs))
        { usage(); return fail("usage", "bad --normalize-lufs value '" + norm_arg + "'"); }
    if (has_norm && (norm_lufs < -70.0 || norm_lufs > 0.0))
        { usage(); return fail("usage", "--normalize-lufs must be in [-70, 0]"); }
    if (has_ceiling && !parse_bar_value(ceiling_arg, norm_ceiling))
        { usage(); return fail("usage", "bad --ceiling-dbfs value '" + ceiling_arg + "'"); }
    if (has_ceiling && (norm_ceiling < -30.0 || norm_ceiling > 0.0))
        { usage(); return fail("usage", "--ceiling-dbfs must be in [-30, 0]"); }
    if (has_ceiling && !has_norm)
        { usage(); return fail("usage", "--ceiling-dbfs requires --normalize-lufs"); }

    // WP-0145 validation order (spec): option syntax → project load → bounds. Nothing is written on error.
    if (has_from && !parse_bar_value(from_arg, bar_a)) return fail("usage", "bad --from-bar value '" + from_arg + "'");
    if (has_to && !parse_bar_value(to_arg, bar_b)) return fail("usage", "bad --to-bar value '" + to_arg + "'");
    if (bar_a < 0.0) return fail("usage", "--from-bar must be >= 0");
    if (has_from && has_to && bar_a >= bar_b) return fail("usage", "--from-bar must be less than --to-bar");
    // WP-0176 (docs/spec/limiter.md): --limit-dbfs value checks with the other option-syntax checks, i.e. before
    // the project file is opened — nothing is written on any error. Repeating the flag: the last value wins.
    if (has_limit && !parse_bar_value(limit_arg, limit_db))
        return fail("usage", "bad --limit-dbfs value '" + limit_arg + "'");
    if (has_limit && (limit_db < -30.0 || limit_db > 0.0))
        return fail("usage", "--limit-dbfs must be in [-30, 0]");

    op::Project p;
    std::string err;
    if (!fs::exists(proj)) return fail("not_found", proj);
    if (!op::load_project(proj, p, err)) return fail("parse", err);
    if (seed_override >= 0) p.seed = static_cast<uint64_t>(seed_override);
    if (has_pattern) {  // WP-0192 (spec §Pattern mode): render one pattern on its own, ignoring the arrangement.
        size_t pat_index = 0;
        bool found = false;
        for (size_t i = 0; i < p.patterns.size(); ++i) {  // unnamed patterns cannot be selected
            if (!p.patterns[i].id.empty() && p.patterns[i].id == pattern_arg) { pat_index = i; found = true; break; }
        }
        if (!found) return fail("usage", "unknown pattern '" + pattern_arg + "'");
        op::Project pm;
        if (!op::pattern_mode_project(p, pat_index, loops, pm, err)) return fail("render", err);
        p = std::move(pm);
    }
    if (want_loop) {  // WP-0155: identical to --from-bar <start> --to-bar <end> (same path below)
        if (!p.loop) { usage(); return fail("usage", "project has no loop region"); }
        has_from = has_to = true;
        bar_a = p.loop->start_bar;
        bar_b = p.loop->end_bar;
        char buf_a[64], buf_b[64];
        std::snprintf(buf_a, sizeof buf_a, "%g", bar_a);
        std::snprintf(buf_b, sizeof buf_b, "%g", bar_b);
        from_arg = buf_a;
        to_arg = buf_b;
    }

    // Bar range [S(A), S(B)); defaults A=0, B=length_bars. Slice after the full render — same frames for
    // mix and every stem; no partial-render optimization (WP-0145).
    long long from_frame = 0, to_frame = -1;  // to_frame < 0 → no range requested: write the full render
    if (has_from || has_to) {
        const double a = has_from ? bar_a : 0.0;
        const double b = has_to ? bar_b : p.length_bars;
        if (b > p.length_bars) return fail("usage", "--to-bar " + to_arg + " exceeds length_bars");
        if (a >= p.length_bars) return fail("usage", "--from-bar " + from_arg + " is not before the end of the song");
        from_frame = op::sample_at_bar(p, a);
        const long long s_b = op::sample_at_bar(p, b);
        if (s_b <= from_frame) return fail("usage", "bar range is empty");
        to_frame = s_b;
    }

    const std::string base_dir = fs::path(proj).parent_path().string();
    if (stems_dir.empty()) {
        op::wav::StereoBuffer buf;
        if (!op::render_project(p, base_dir, buf, err)) return fail("render", err);
        if (to_frame >= 0) {
            op::wav::StereoBuffer sliced;
            op::slice_frames(buf, from_frame, to_frame, sliced);
            buf = std::move(sliced);
        }
        // WP-0189 (docs/spec/normalize.md): loudness normalisation on the sliced mix, planned BEFORE any
        // write — a plan failure (silent/too short) exits 5 with no output file at all.
        op::normalize::GainPlan plan;
        if (has_norm && !op::normalize::plan_gain(buf.interleaved, 2, buf.sample_rate, norm_lufs, norm_ceiling, plan, err))
            return fail("normalize", err);
        if (has_norm) op::normalize::apply_gain_db(buf.interleaved, plan.gain_db);
        // WP-0176: master limiter on the final mix (after any bar-range slicing), immediately before writing.
        if (has_limit && !run_master_limiter(buf, limit_db, err)) return fail("render", err);
        if (!op::wav::write_wav_stereo(out_path, buf, fmt, err)) return fail("write", err);
        std::printf("rendered %lld frames @%d Hz -> %s (seed=%llu)\n",
                    static_cast<long long>(buf.frames()), buf.sample_rate, out_path.c_str(),
                    static_cast<unsigned long long>(p.seed));
        if (has_norm)  // WP-0189: the same JSON line as opd normalize, after the rendered line
            std::printf("{\"input_lufs\":%.4f,\"input_peak_dbfs\":%.4f,\"gain_db\":%.4f,\"limited\":%s}\n",
                        plan.input_lufs, plan.input_peak_dbfs, plan.gain_db, plan.limited ? "true" : "false");
    } else {
        std::vector<op::wav::StereoBuffer> stems;
        op::wav::StereoBuffer mix;
        if (!op::render_stems(p, base_dir, stems, mix, err)) return fail("render", err);
        if (to_frame >= 0) {  // same slice for the mix and every stem (spec "Render a bar range")
            op::wav::StereoBuffer sliced_mix;
            op::slice_frames(mix, from_frame, to_frame, sliced_mix);
            mix = std::move(sliced_mix);
            for (auto& s : stems) {
                op::wav::StereoBuffer sliced_stem;
                op::slice_frames(s, from_frame, to_frame, sliced_stem);
                s = std::move(sliced_stem);
            }
        }
        // WP-0189 (docs/spec/normalize.md): plan on the sliced mix BEFORE any write (a plan failure exits 5
        // with no output file, no stems dir, no stem files); the SAME gain goes on every sliced stem, so the
        // stems keep summing to the mix.
        op::normalize::GainPlan plan;
        if (has_norm && !op::normalize::plan_gain(mix.interleaved, 2, mix.sample_rate, norm_lufs, norm_ceiling, plan, err))
            return fail("normalize", err);
        if (has_norm) {
            op::normalize::apply_gain_db(mix.interleaved, plan.gain_db);
            for (auto& s : stems) op::normalize::apply_gain_db(s.interleaved, plan.gain_db);
        }
        // WP-0176: the mix only — stems are never limited (docs/spec/limiter.md §CLI).
        if (has_limit && !run_master_limiter(mix, limit_db, err)) return fail("render", err);
        if (!op::wav::write_wav_stereo(out_path, mix, fmt, err)) return fail("write", err);
        std::error_code ec;
        fs::create_directories(stems_dir, ec);
        if (ec) return fail("write", "cannot create stems dir " + stems_dir);
        for (size_t i = 0; i < stems.size(); ++i) {
            const std::string name = sanitize_track_name(p.tracks[i].name);
            const std::string path = stems_dir + "/" + std::to_string(i) + "_" + (name.empty() ? "track" : name) + ".wav";
            if (!op::wav::write_wav_stereo(path, stems[i], fmt, err)) return fail("write", err);
            std::printf("stem %zu -> %s\n", i, path.c_str());
        }
        std::printf("rendered mix %lld frames @%d Hz -> %s (+%zu stems, seed=%llu)\n",
                    static_cast<long long>(mix.frames()), mix.sample_rate, out_path.c_str(), stems.size(),
                    static_cast<unsigned long long>(p.seed));
        if (has_norm)  // WP-0189: the same JSON line as opd normalize, after the rendered line
            std::printf("{\"input_lufs\":%.4f,\"input_peak_dbfs\":%.4f,\"gain_db\":%.4f,\"limited\":%s}\n",
                        plan.input_lufs, plan.input_peak_dbfs, plan.gain_db, plan.limited ? "true" : "false");
    }
    return 0;
}

// opd click (WP-0148): deterministic metronome track for the project's full length (spec §Click
// track render). Same exit-code contract as render; prints nothing on stdout on success.
static int cmd_click(int argc, char** argv) {
    if (argc < 4) { usage(); return fail("usage", "click needs project + -o out"); }
    const std::string proj = argv[1];
    std::string out_path;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "-o") && i + 1 < argc) out_path = argv[++i];
        else { usage(); return fail("usage", std::string("unknown arg '") + argv[i] + "'"); }
    }
    if (out_path.empty()) { usage(); return fail("usage", "missing -o <out.wav>"); }

    op::Project p;
    std::string err;
    if (!fs::exists(proj)) return fail("not_found", proj);
    if (!op::load_project(proj, p, err)) return fail("parse", err);

    op::wav::StereoBuffer buf;
    if (!op::render_click(p, buf, err)) return fail("render", err);
    if (!op::wav::write_pcm16_stereo(out_path, buf, err)) return fail("write", err);
    return 0;
}

// opd new (WP-0165, spec §New project): canonical new project via op::new_project. EVERY option is
// validated left to right before the output path is examined; nothing is written on any error.
// Exit 2 usage, 5 write (existing file without --force, or unwritable path). Example:
//   opd new song.opj --bpm 140 --bars 8 --track Kick:sampler:kits/kick.wav --force
static bool parse_track_arg(const std::string& v, op::NewTrackSpec& t, std::string& msg) {
    const size_t c1 = v.find(':');
    if (c1 == std::string::npos || c1 == 0) { msg = "expected NAME:KIND[:PATH]"; return false; }
    t.name = v.substr(0, c1);
    const size_t c2 = v.find(':', c1 + 1);
    const std::string kind = c2 == std::string::npos ? v.substr(c1 + 1) : v.substr(c1 + 1, c2 - c1 - 1);
    const bool has_path = c2 != std::string::npos;
    if (kind == "sine")           t.kind = op::VoiceKind::Sine;
    else if (kind == "synth")     t.kind = op::VoiceKind::Synth;
    else if (kind == "sampler")   t.kind = op::VoiceKind::Sampler;
    else if (kind == "metronome") t.kind = op::VoiceKind::Metronome;
    else { msg = "unknown kind '" + kind + "' (expected sine, synth, sampler or metronome)"; return false; }
    if (t.kind == op::VoiceKind::Sampler) {
        const std::string path = has_path ? v.substr(c2 + 1) : "";  // may itself contain ':'
        if (path.empty()) { msg = "sampler needs a PATH"; return false; }
        t.path = path;
    } else if (has_path) {
        msg = "PATH is only valid for sampler"; return false;
    }
    return true;
}

static int cmd_new(int argc, char** argv) {
    op::NewProjectSpec spec;
    std::string out_path;
    bool force = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--force") { force = true; continue; }
        if (a.rfind("--", 0) != 0) {  // positional: the output path, once only
            if (!out_path.empty()) { usage(); return fail("usage", "unknown arg '" + a + "'"); }
            out_path = a;
            continue;
        }
        const std::string opt = a;  // for "missing value for <option>" messages
        auto need_value = [&](std::string& v) -> bool {
            if (i + 1 >= argc) { usage(); fail("usage", "missing value for " + opt); return false; }
            v = argv[++i];
            return true;
        };
        std::string v;
        if (opt == "--bpm") {
            if (!need_value(v)) return 2;
            double bpm = 0.0;
            if (!parse_bar_value(v, bpm) || !(bpm > 0.0) || bpm > 1000.0)
                { usage(); return fail("usage", "bad --bpm value '" + v + "' (expected a number in (0, 1000])"); }
            spec.bpm = bpm;
        } else if (opt == "--bars") {
            if (!need_value(v)) return 2;
            long long n = 0;
            if (!parse_decimal(v, 4096, n) || n < 1)
                { usage(); return fail("usage", "bad --bars value '" + v + "' (expected an integer 1..4096)"); }
            spec.bars = static_cast<int>(n);
        } else if (opt == "--beats-per-bar") {
            if (!need_value(v)) return 2;
            long long n = 0;
            if (!parse_decimal(v, 32, n) || n < 1)
                { usage(); return fail("usage", "bad --beats-per-bar value '" + v + "' (expected an integer 1..32)"); }
            spec.beats_per_bar = static_cast<int>(n);
        } else if (opt == "--sample-rate") {
            if (!need_value(v)) return 2;
            long long n = 0;
            if (!parse_decimal(v, 192000, n) || n < 8000)
                { usage(); return fail("usage", "bad --sample-rate value '" + v + "' (expected an integer 8000..192000)"); }
            spec.sample_rate = static_cast<int>(n);
        } else if (opt == "--track") {
            if (!need_value(v)) return 2;
            op::NewTrackSpec t;
            std::string msg;
            if (!parse_track_arg(v, t, msg))
                { usage(); return fail("usage", "bad --track '" + v + "': " + msg); }
            if (spec.tracks.size() >= 64)
                { usage(); return fail("usage", "too many tracks (max 64)"); }
            spec.tracks.push_back(std::move(t));
        } else {
            usage(); return fail("usage", "unknown arg '" + a + "'");
        }
    }
    if (out_path.empty()) { usage(); return fail("usage", "new needs <out.opj>"); }

    // Output path examined only after every option validated (spec §New project): overwrite is exit 5.
    if (!force && fs::exists(out_path))
        return fail("write", out_path + " already exists (use --force)");
    const op::Project p = op::new_project(spec);
    std::string err;
    if (!op::save_project(out_path, p, err)) return fail("write", err);
    std::printf("new: %s tracks=%zu bars=%d bpm=%g\n", out_path.c_str(), p.tracks.size(), spec.bars, spec.bpm);
    return 0;
}

// Canonical fixture: metronome + sine bassline + sampler kick, 2 bars @ 120 BPM. All constants fixed.
static void synth_kick(op::wav::StereoBuffer& kick) {
    const int sr = 48000;
    const long long len = static_cast<long long>(0.30 * sr);
    kick.sample_rate = sr;
    kick.interleaved.assign(static_cast<size_t>(len) * 2u, 0.0f);
    float phase = 0.0f;
    for (long long n = 0; n < len; ++n) {
        const float t = static_cast<float>(n) / static_cast<float>(sr);
        const float freq = 55.0f + 30.0f * op::dsp::expf_(-t * 40.0f);  // pitch drop, deterministic closed form
        phase += 2.0f * 3.14159265358979323846f * freq / static_cast<float>(sr);
        if (phase > 2.0f * 3.14159265358979323846f) phase -= 2.0f * 3.14159265358979323846f;
        const float env = op::dsp::expf_(-t * 8.0f);
        const float s = 0.8f * op::dsp::sinf_(phase) * env;
        kick.interleaved[n * 2 + 0] = s;
        kick.interleaved[n * 2 + 1] = s;
    }
}

static int cmd_gen_testproject(int argc, char** argv) {
    if (argc < 2) { usage(); return fail("usage", "gen-testproject needs <out.opj>"); }
    const std::string out_opj = argv[1];
    op::Project p;
    p.bpm = 120.0; p.length_bars = 2.0; p.seed = 42;

    op::Track metro; metro.name = "Metronome"; metro.gain_db = -12.0f; metro.voice.kind = op::VoiceKind::Metronome;
    op::Track bass;  bass.name = "Bassline";   bass.gain_db = -6.0f;  bass.pan = -0.3f;
    bass.voice.kind = op::VoiceKind::Sine; bass.voice.freq_hz = 55.0f;
    op::Track kick;  kick.name = "Kick";       kick.gain_db = -2.0f;
    kick.voice.kind = op::VoiceKind::Sampler; kick.voice.path = "kits/kick.wav";
    p.tracks = {metro, bass, kick};

    op::Pattern pm; pm.track = 0;
    for (int b = 0; b < 8; ++b) pm.notes.push_back({static_cast<double>(b), 0.25});
    op::Pattern pb; pb.track = 1;
    const double bass_pat[8] = {0, 0, 1.5, 2, 3, 3.5, 4, 6};
    for (int b = 0; b < 8; ++b) if (!(b % 2) || b == 3 || b == 5) pb.notes.push_back({bass_pat[b], 0.9});
    op::Pattern pk; pk.track = 2;
    for (int b = 0; b < 8; ++b) pk.notes.push_back({static_cast<double>(b), 4.0});
    p.patterns = {pm, pb, pk};

    std::string err;
    const fs::path dir = fs::path(out_opj).parent_path();
    if (!dir.empty()) fs::create_directories(dir / "kits");
    op::wav::StereoBuffer kick_buf;
    synth_kick(kick_buf);
    const std::string kick_path = (dir / "kits" / "kick.wav").string();
    if (!op::wav::write_pcm16_stereo(kick_path, kick_buf, err)) return fail("write", err);
    if (!op::save_project(out_opj, p, err)) return fail("write", err);
    std::printf("wrote %s and %s\n", out_opj.c_str(), kick_path.c_str());
    return 0;
}

static int cmd_midi2opj(int argc, char** argv) {
    if (argc < 2) { usage(); return fail("usage", "midi2opj needs <in.mid>"); }
    const std::string in_mid = argv[1];
    std::string out_opj;
    for (int i = 2; i < argc; ++i)
        if (!std::strcmp(argv[i], "-o") && i + 1 < argc) out_opj = argv[++i];
    if (out_opj.empty()) { usage(); return fail("usage", "midi2opj needs -o <out.opj>"); }
    op::Project p;
    std::string err;
    if (!op::load_midi(in_mid, p, err)) return fail("parse", err);
    if (!op::save_project(out_opj, p, err)) return fail("write", err);
    size_t total_notes = 0;
    for (const auto& pat : p.patterns) total_notes += pat.notes.size();
    std::printf("midi->opj: %zu tracks, %zu notes, bpm=%.2f, length_bars=%.0f -> %s\n",
                p.tracks.size(), total_notes, p.bpm, p.length_bars, out_opj.c_str());
    return 0;
}

static int cmd_opj2mid(int argc, char** argv) {
    if (argc < 2) { usage(); return fail("usage", "opj2mid needs <in.opj>"); }
    const std::string in_opj = argv[1];
    std::string out_mid;
    for (int i = 2; i < argc; ++i)
        if (!std::strcmp(argv[i], "-o") && i + 1 < argc) out_mid = argv[++i];
    if (out_mid.empty()) { usage(); return fail("usage", "opj2mid needs -o <out.mid>"); }
    op::Project p;
    std::string err;
    if (!op::load_project(in_opj, p, err)) return fail("parse", err);
    if (!op::save_midi(out_mid, p, err)) return fail("write", err);
    std::printf("opj->midi: %zu tracks -> %s\n", p.tracks.size(), out_mid.c_str());
    return 0;
}

static int cmd_pack(int argc, char** argv) {
    if (argc < 2) { usage(); return fail("usage", "pack needs <in.opj>"); }
    const std::string in_opj = argv[1];
    std::string out;
    for (int i = 2; i < argc; ++i)
        if (!std::strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
    if (out.empty()) { usage(); return fail("usage", "pack needs -o <song.opd>"); }
    std::string err;
    if (!op::pack_project(in_opj, out, err)) return fail("bundle", err);
    std::printf("packed %s -> %s\n", in_opj.c_str(), out.c_str());
    return 0;
}

static int cmd_unpack(int argc, char** argv) {
    if (argc < 2) { usage(); return fail("usage", "unpack needs <song.opd>"); }
    const std::string in = argv[1];
    std::string dir;
    for (int i = 2; i < argc; ++i)
        if (!std::strcmp(argv[i], "-d") && i + 1 < argc) dir = argv[++i];
    if (dir.empty()) { usage(); return fail("usage", "unpack needs -d <out-dir>"); }
    std::string main_opj, err;
    if (!op::unpack_bundle(in, dir, main_opj, err)) return fail("bundle", err);
    std::printf("unpacked %s -> %s (main: %s)\n", in.c_str(), dir.c_str(), main_opj.c_str());
    return 0;
}

static int cmd_import_midi(int argc, char** argv) {
    if (argc < 3) { usage(); return fail("usage", "import-midi needs <proj.opj> <in.mid>"); }
    const std::string proj = argv[1], mid = argv[2];
    std::string out = proj;  // in-place by default (model-driven: load → engine merge op → save)
    for (int i = 3; i < argc; ++i)
        if (!std::strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
    op::Project p;
    std::string err;
    if (!op::load_project(proj, p, err)) return fail("parse", err);
    const bool has_arr = !p.arrangement.empty();
    if (!op::import_midi_into(p, mid, err)) return fail("parse", err);
    if (!op::save_project(out, p, err)) return fail("write", err);
    std::printf("import-midi: %s -> %s (tracks now %zu)\n", mid.c_str(), out.c_str(), p.tracks.size());
    if (has_arr)
        std::fprintf(stderr, "note: project has an arrangement — imported tracks stay silent until clips reference them\n");
    return 0;
}

static int cmd_attach(int argc, char** argv) {
    if (argc < 2) { usage(); return fail("usage", "attach needs <proj.opj>"); }
    const std::string proj = argv[1];
    op::Attachment a;
    std::string out = proj;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--path") && i + 1 < argc) a.path = argv[++i];
        else if (!std::strcmp(argv[i], "--kind") && i + 1 < argc) a.kind = argv[++i];
        else if (!std::strcmp(argv[i], "--producer") && i + 1 < argc) a.producer = argv[++i];
        else if (!std::strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
    }
    op::Project p;
    std::string err;
    if (!op::load_project(proj, p, err)) return fail("parse", err);
    if (!op::add_attachment(p, std::move(a), err)) return fail("attach", err);
    if (!op::save_project(out, p, err)) return fail("write", err);
    std::printf("attach: %zu attachment(s) in %s\n", p.attachments.size(), out.c_str());
    return 0;
}

static int cmd_apply(int argc, char** argv) {
    if (argc < 3) { usage(); return fail("usage", "apply needs <proj.opj> <ops.opjl>"); }
    const std::string proj = argv[1], journal = argv[2];
    std::string out = proj;  // in-place by default, same convention as import-midi / attach
    for (int i = 3; i < argc; ++i)
        if (!std::strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else { usage(); return fail("usage", std::string("unknown arg '") + argv[i] + "'"); }

    std::string err;
    if (!fs::exists(proj)) return fail("not_found", proj);
    if (!fs::exists(journal)) return fail("not_found", journal);
    op::Project p;
    if (!op::load_project(proj, p, err)) return fail("parse", err);
    std::ifstream jf(journal, std::ios::binary);
    if (!jf) return fail("not_found", journal);
    const std::string text((std::istreambuf_iterator<char>(jf)), std::istreambuf_iterator<char>());

    // All-or-nothing (docs/spec/operations.md §CLI): nothing is written unless every op applied.
    op::JournalOutcome oc;
    if (!op::apply_journal(p, text, err, &oc))
        return fail(oc.failure == op::JournalFailure::kParse ? "parse" : "apply", err);
    if (!op::save_project(out, p, err)) return fail("write", err);
    std::printf("apply: %zu op(s) -> %s\n", oc.applied, out.c_str());
    return 0;
}

// opd diff (docs/spec/operations.md §opd diff): the output file is opened only after the diff
// succeeded, so a usage/not-found/parse/diff failure never creates or modifies it.
static int cmd_diff(int argc, char** argv) {
    if (argc < 3) { usage(); return fail("usage", "diff needs <a.opj> <b.opj> -o <edits.opjl>"); }
    const std::string a_path = argv[1], b_path = argv[2];
    std::string out;
    for (int i = 3; i < argc; ++i)
        if (!std::strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else { usage(); return fail("usage", std::string("unknown arg '") + argv[i] + "'"); }
    if (out.empty()) { usage(); return fail("usage", "diff needs -o <edits.opjl>"); }

    std::string err;
    if (!fs::exists(a_path)) return fail("not_found", a_path);
    if (!fs::exists(b_path)) return fail("not_found", b_path);
    op::Project a, b;
    if (!op::load_project(a_path, a, err)) return fail("parse", err);
    if (!op::load_project(b_path, b, err)) return fail("parse", err);

    std::vector<nlohmann::json> ops;
    if (!op::diff_projects(a, b, ops, err)) return fail("diff", err);

    std::ofstream of(out, std::ios::binary | std::ios::trunc);
    if (!of) return fail("write", "cannot open '" + out + "' for writing");
    for (const auto& o : ops) of << o.dump() << "\n";  // zero ops → zero-byte file (spec)
    of.close();
    if (!of) return fail("write", "cannot write '" + out + "'");
    std::printf("diff: %zu op(s) -> %s\n", ops.size(), out.c_str());
    return 0;
}

// WP-0158: extensions routed to the audio reader in `opd info` (lowercase, as for .wav before).
static bool has_audio_ext(const std::string& path) {
    for (const char* ext : {".wav", ".aif", ".aiff", ".aifc"}) {
        const std::string e(ext);
        if (path.size() > e.size() && path.compare(path.size() - e.size(), e.size(), e) == 0) return true;
    }
    return false;
}

static int cmd_info(int argc, char** argv) {
    if (argc < 2) { usage(); return fail("usage", "info needs a file"); }
    const std::string path = argv[1];
    if (!fs::exists(path)) return fail("not_found", path);
    // WP-0158: WAV and AIFF/AIFF-C audio. read_audio picks the reader by file signature; the label
    // follows the content (rr.format), so an AIFF saved as x.wav prints "aiff:".
    if (has_audio_ext(path)) {
        auto rr = op::wav::read_audio(path);
        if (!rr.ok) return fail("parse", rr.error);
        std::printf("%s: %s rate=%d ch=%d frames=%zu duration=%.3fs\n", rr.format.c_str(), path.c_str(),
                    rr.sample_rate, rr.channels, rr.samples.size() / static_cast<size_t>(rr.channels),
                    static_cast<double>(rr.samples.size() / rr.channels) / rr.sample_rate);
        return 0;
    }
    if ((path.size() > 4 && path.substr(path.size() - 4) == ".mid") ||
        (path.size() > 4 && path.substr(path.size() - 4) == ".MID")) {
        op::Project mp;
        std::string err;
        if (!op::load_midi(path, mp, err)) return fail("parse", err);
        size_t notes = 0;
        for (const auto& pt : mp.patterns) notes += pt.notes.size();
        std::printf("midi: %s bpm=%.2f%s tracks=%zu notes=%zu length≈%.2f bars\n", path.c_str(), mp.bpm,
                    mp.tempo_map.empty() ? "" : " (tempo map)", mp.tracks.size(), notes, mp.length_bars);
        for (size_t i = 0; i < mp.tracks.size(); ++i) {
            size_t n = i < mp.patterns.size() ? mp.patterns[i].notes.size() : 0;
            std::printf("  [%zu] %s: %zu notes\n", i, mp.tracks[i].name.c_str(), n);
        }
        return 0;
    }
    if (path.size() > 4 && path.substr(path.size() - 4) == ".opd") {
        std::vector<std::string> entries;
        std::string err;
        if (!op::list_bundle(path, entries, err)) return fail("parse", err);
        std::printf("bundle: %s entries=%zu\n", path.c_str(), entries.size());
        for (const auto& e : entries) std::printf("  %s\n", e.c_str());
        return 0;
    }
    op::Project p;
    std::string err;
    if (!op::load_project(path, p, err)) return fail("parse", err);
    const long long total = op::total_samples_for(p);
    std::printf("project: %s bpm=%.2f bars=%.2f tracks=%zu patterns=%zu render=%lld frames (%.3fs) seed=%llu\n",
                path.c_str(), p.bpm, p.length_bars, p.tracks.size(), p.patterns.size(), total,
                static_cast<double>(total) / p.sample_rate,
                static_cast<unsigned long long>(p.seed));
    const op::TimeSignature ts = op::effective_time_signature(p);  // WP-0191: silent for the 4/4 default
    if (ts.num != 4 || ts.den != 4) std::printf("time_signature: %d/%d\n", ts.num, ts.den);
    if (p.master_gain_db != 0.0f)  // mixer state (WP-0136): markers per track, master when non-default
        std::printf("master_gain_db=%.2f\n", static_cast<double>(p.master_gain_db));
    if (p.loop)  // WP-0155: printed only when a loop region is set
        std::printf("loop: %g-%g\n", p.loop->start_bar, p.loop->end_bar);
    for (size_t i = 0; i < p.tracks.size(); ++i) {
        const op::Track& t = p.tracks[i];
        std::printf("  [%zu] %s%s%s\n", i, t.name.c_str(), t.mute ? " [mute]" : "",
                    t.solo ? " [solo]" : "");
    }
    if (!p.attachments.empty()) {
        std::printf("attachments=%zu\n", p.attachments.size());
        for (const auto& a : p.attachments)
            std::printf("  [%s] %s%s%s\n", a.kind.c_str(), a.path.c_str(),
                        a.producer.empty() ? "" : " <- ", a.producer.c_str());
    }
    return 0;
}

// opd analyze <in.wav> [--json] — objective audio features as canonical JSON (docs/spec/analyze.md).
// --json is accepted for forward-compatibility (JSON is the only output today). Errors: nothing on
// stdout; exit 2 usage, 3 not-found, 4 parse, 5 analyze error (unsupported sample rate).
static int cmd_analyze(int argc, char** argv) {
    if (argc < 2) { usage(); return fail("usage", "analyze needs <in.wav>"); }
    std::string path;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--json")) continue;
        if (!path.empty() || argv[i][0] == '-') { usage(); return fail("usage", std::string("unknown arg '") + argv[i] + "'"); }
        path = argv[i];
    }
    if (path.empty()) { usage(); return fail("usage", "analyze needs <in.wav>"); }
    if (!fs::exists(path)) return fail("not_found", path);

    op::wav::ReadResult rr = op::wav::read_wav(path);
    if (!rr.ok) return fail("parse", rr.error);
    if (rr.sample_rate < 8000 || rr.sample_rate > 192000)
        return fail("analyze", "unsupported sample rate " + std::to_string(rr.sample_rate));

    const op::analyze::Features f = op::analyze::analyze_samples(rr.samples, rr.channels, rr.sample_rate);
    const std::string json = op::analyze::to_json(f);
    std::fwrite(json.data(), 1, json.size(), stdout);
    return 0;
}

// opd meters <project.opj> (WP-0194) — per-track and master peak/RMS dBFS + integrated LUFS as canonical
// JSON. Renders the stems and the mix IN MEMORY (op::meters over op::render_stems); writes nothing and
// never modifies the project. Exactly one argument, no options. Errors print nothing on stdout: exit 2
// usage, 3 not-found, 4 parse, 5 meters (sample rate outside 8000..192000) or render.
static int cmd_meters(int argc, char** argv) {
    if (argc != 2 || argv[1][0] == '-') { usage(); return fail("usage", "meters needs exactly <project.opj>"); }
    const std::string path = argv[1];
    if (!fs::exists(path)) return fail("not_found", path);

    op::Project p;
    std::string err;
    if (!op::load_project(path, p, err)) return fail("parse", err);
    if (p.sample_rate < 8000 || p.sample_rate > 192000)
        return fail("meters", "unsupported sample rate " + std::to_string(p.sample_rate));

    op::meters::Report r;
    const std::string base_dir = fs::path(path).parent_path().string();
    if (!op::meters::measure_project(p, base_dir, r, err)) return fail("render", err);
    const std::string json = op::meters::to_json(r);
    std::fwrite(json.data(), 1, json.size(), stdout);
    return 0;
}

// opd loudness <in.wav> [--json] — true peak (dBTP), momentary / short-term max LUFS and loudness range
// as canonical JSON (docs/spec/loudness.md). --json is accepted and changes nothing (JSON is the only
// format). Errors: nothing on stdout; exit 2 usage, 3 not-found, 4 parse (reader error), 5 loudness
// error (sample rate outside 8000..192000). WAV and AIFF/AIFF-C via read_audio.
static int cmd_loudness(int argc, char** argv) {
    if (argc < 2) { usage(); return fail("usage", "loudness needs <in.wav>"); }
    std::string path;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--json")) continue;
        if (!path.empty() || argv[i][0] == '-') { usage(); return fail("usage", std::string("unknown arg '") + argv[i] + "'"); }
        path = argv[i];
    }
    if (path.empty()) { usage(); return fail("usage", "loudness needs <in.wav>"); }
    if (!fs::exists(path)) return fail("not_found", path);

    op::wav::ReadResult rr = op::wav::read_audio(path);
    if (!rr.ok) return fail("parse", rr.error);
    if (rr.sample_rate < 8000 || rr.sample_rate > 192000)
        return fail("loudness", "unsupported sample rate " + std::to_string(rr.sample_rate));

    const op::loudness::Result r = op::loudness::measure(rr.samples, rr.channels, rr.sample_rate);
    const std::string json = op::loudness::to_json(r);
    std::fwrite(json.data(), 1, json.size(), stdout);
    return 0;
}

// opd normalize (WP-0169, docs/spec/normalize.md): gain a WAV to a target integrated LUFS without the
// sample peak exceeding a ceiling; mono input is duplicated to stereo. All usage checks happen before
// the input file is touched and nothing is written on error. Exit 2 usage, 3 not-found, 4 parse,
// 5 normalize (sample rate / plan_gain) or write.
static int cmd_normalize(int argc, char** argv) {
    std::string in_path, out_path, lufs_arg;
    bool has_lufs = false;
    std::string ceiling_arg;  // raw string for error messages ("<C as given>")
    op::wav::SampleFormat fmt = op::wav::SampleFormat::Pcm16;  // default: pcm16 (same as render)
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "-o") && i + 1 < argc) out_path = argv[++i];
        else if (!std::strcmp(argv[i], "--lufs") && i + 1 < argc) { lufs_arg = argv[++i]; has_lufs = true; }
        else if (!std::strcmp(argv[i], "--ceiling-db") && i + 1 < argc) ceiling_arg = argv[++i];
        else if (!std::strcmp(argv[i], "--format") && i + 1 < argc) {
            const std::string f = argv[++i];  // validated here, before the input is touched (as in render)
            if (f == "pcm16") fmt = op::wav::SampleFormat::Pcm16;
            else if (f == "pcm24") fmt = op::wav::SampleFormat::Pcm24;
            else if (f == "f32") fmt = op::wav::SampleFormat::F32;
            else { usage(); return fail("usage", "unknown format '" + f + "' (expected pcm16, pcm24 or f32)"); }
        }
        else if (!in_path.empty() || argv[i][0] == '-') {  // extra positional or option without value
            usage(); return fail("usage", std::string("unknown arg '") + argv[i] + "'");
        }
        else in_path = argv[i];
    }
    if (in_path.empty()) { usage(); return fail("usage", "normalize needs <in.wav>"); }
    if (out_path.empty()) { usage(); return fail("usage", "missing -o <out.wav>"); }
    if (!has_lufs) { usage(); return fail("usage", "normalize needs --lufs <T>"); }
    double target_lufs = 0.0, ceiling_db = -1.0;  // ceiling default per spec
    if (!parse_bar_value(lufs_arg, target_lufs))
        { usage(); return fail("usage", "bad --lufs value '" + lufs_arg + "'"); }
    if (!ceiling_arg.empty() && !parse_bar_value(ceiling_arg, ceiling_db))
        { usage(); return fail("usage", "bad --ceiling-db value '" + ceiling_arg + "'"); }
    if (target_lufs < -70.0 || target_lufs > 0.0) { usage(); return fail("usage", "--lufs must be in [-70, 0]"); }
    if (ceiling_db < -30.0 || ceiling_db > 0.0)
        { usage(); return fail("usage", "--ceiling-db must be in [-30, 0]"); }

    if (!fs::exists(in_path)) return fail("not_found", in_path);
    auto rr = op::wav::read_audio(in_path);  // WAV or AIFF, dispatched by file signature (WP-0158)
    if (!rr.ok) return fail("parse", rr.error);
    if (rr.sample_rate < 8000 || rr.sample_rate > 192000)
        return fail("normalize", "unsupported sample rate " + std::to_string(rr.sample_rate));

    // Mono input is duplicated into stereo (L = R): loudness is measured on that stereo buffer, so the
    // written file measures at the target, and the output is always stereo (docs/spec/normalize.md).
    std::vector<float> samples;
    if (rr.channels == 1) {
        samples.reserve(rr.samples.size() * 2u);
        for (const float s : rr.samples) { samples.push_back(s); samples.push_back(s); }
    } else {
        samples = rr.samples;
    }

    op::normalize::GainPlan plan;
    std::string err;
    if (!op::normalize::plan_gain(samples, 2, rr.sample_rate, target_lufs, ceiling_db, plan, err))
        return fail("normalize", err);
    op::normalize::apply_gain_db(samples, plan.gain_db);

    op::wav::StereoBuffer buf;
    buf.sample_rate = rr.sample_rate;  // output stays at the input sample rate
    buf.interleaved = std::move(samples);
    if (!op::wav::write_wav_stereo(out_path, buf, fmt, err)) return fail("write", err);
    std::printf("{\"input_lufs\":%.4f,\"input_peak_dbfs\":%.4f,\"gain_db\":%.4f,\"limited\":%s}\n",
                plan.input_lufs, plan.input_peak_dbfs, plan.gain_db, plan.limited ? "true" : "false");
    return 0;
}

// opd lint <proj.opj> [--json] — read-only project sanity checks as canonical JSON (docs/spec/lint.md).
// --json is accepted for forward-compatibility (JSON is the only output today). base_dir is the project
// file's parent directory, same as opd render. Exit codes: 0 = no error-severity issue (warnings/infos
// alone -> 0); 1 = at least one error-severity issue (JSON is still printed); 2 usage (no file, extra or
// unknown argument); 3 project file not found; 4 project does not parse. Cases 2-4 write nothing to stdout.
static int cmd_lint(int argc, char** argv) {
    std::string path;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--json")) continue;
        if (!path.empty() || argv[i][0] == '-') { usage(); return fail("usage", std::string("unknown arg '") + argv[i] + "'"); }
        path = argv[i];
    }
    if (path.empty()) { usage(); return fail("usage", "lint needs <proj.opj>"); }
    if (!fs::exists(path)) return fail("not_found", path);

    op::Project p;
    std::string err;
    if (!op::load_project(path, p, err)) return fail("parse", err);

    const std::string base_dir = fs::path(path).parent_path().string();
    const std::vector<op::lint::Issue> issues = op::lint::lint_project(p, base_dir);
    size_t errors = 0;
    for (const auto& is : issues)
        if (is.severity == "error") ++errors;
    const std::string json = op::lint::to_json(issues);
    std::fwrite(json.data(), 1, json.size(), stdout);
    return errors > 0 ? 1 : 0;
}

// opd clips <proj.opj> (WP-0199, docs/spec/operations.md §Arrangement clip view) — the arrangement as
// canonical JSON: one entry per clip in model order, with start/end in beats and samples from the
// renderer's own tempo map. Read-only: writes nothing, modifies no project, creates no files; two runs
// on the same file are byte-identical. Exactly one positional argument, no options. Errors print
// nothing on stdout: exit 2 usage, 3 not-found (detail = the path), 4 parse (detail = loader message).
static int cmd_clips(int argc, char** argv) {
    if (argc != 2 || argv[1][0] == '-') { usage(); return fail("usage", "clips needs exactly <project.opj>"); }
    const std::string path = argv[1];
    if (!fs::exists(path)) return fail("not_found", path);

    op::Project p;
    std::string err;
    if (!op::load_project(path, p, err)) return fail("parse", err);

    const std::string json = op::clips_json(p);
    std::fwrite(json.data(), 1, json.size(), stdout);
    return 0;
}

// Parse a decimal argument that must be all digits and fit in [0, max]. Rejects "", "-1", "1.0",
// "x" and anything that would overflow. Example: parse_decimal("16", 256, v) → true, v == 16.
static bool parse_decimal(const std::string& s, long long max, long long& out) {
    if (s.empty() || s.size() > 18) return false;
    for (char c : s)
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    out = std::stoll(s);
    return out <= max;
}

// opd steps (WP-0154, docs/spec/operations.md §Engine view and CLI): print a pattern's step grid.
// Option syntax is checked before the project is loaded. Exit 2 usage, 3 not-found, 4 parse.
static int cmd_steps(int argc, char** argv) {
    std::vector<std::string> pos;
    long long steps = 16;
    bool steps_given = false;  // WP-0162: an explicit --steps beats the length_beats-derived default
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--steps") && i + 1 < argc) {
            long long v = 0;
            if (!parse_decimal(argv[i + 1], 256, v) || v < 1)
                { usage(); return fail("usage", std::string("bad --steps value '") + argv[i + 1] + "'"); }
            steps = v;
            steps_given = true;
            ++i;
        } else if (!std::strncmp(argv[i], "--", 2)) {  // includes a valueless `--steps`
            usage(); return fail("usage", std::string("unknown arg '") + argv[i] + "'");
        } else if (pos.size() < 2) {
            pos.push_back(argv[i]);
        } else {
            usage(); return fail("usage", std::string("unknown arg '") + argv[i] + "'");
        }
    }
    if (pos.size() < 2) { usage(); return fail("usage", "steps needs <proj.opj> <pattern-index>"); }
    long long idx = 0;
    if (!parse_decimal(pos[1], std::numeric_limits<long long>::max(), idx))
        { usage(); return fail("usage", "bad pattern index '" + pos[1] + "'"); }

    const std::string& proj = pos[0];
    if (!fs::exists(proj)) return fail("not_found", proj);
    op::Project p;
    std::string err;
    if (!op::load_project(proj, p, err)) return fail("parse", err);
    if (idx >= static_cast<long long>(p.patterns.size())) {
        usage();
        return fail("usage", "pattern index " + std::to_string(idx) + " out of range (project has " +
                                 std::to_string(p.patterns.size()) + " patterns)");
    }
    // Default width (WP-0162, spec §opd steps default width): an explicit --steps always wins;
    // otherwise a pattern with length_beats uses ceil(length_beats * 4) clamped to [1, 256].
    if (!steps_given) {
        const auto& lb = p.patterns[static_cast<std::size_t>(idx)].length_beats;
        if (lb) {
            double w = std::ceil(*lb * 4.0);
            if (!(w > 1.0)) w = 1.0;    // defensive; the model parse guarantees lb > 0 → ceil ≥ 1
            if (w > 256.0) w = 256.0;   // clamp before the int cast: length_beats has no upper bound
            steps = static_cast<long long>(w);
        }
    }
    std::printf("%s\n", op::step_grid(p.patterns[static_cast<std::size_t>(idx)], static_cast<int>(steps)).c_str());
    return 0;
}

// opd samples <dir> [-o <index.json>] (WP-0163, docs/spec/sample-index.md): deterministic JSON index of
// the audio files under a directory. The index is built BEFORE any output is opened, so exit 2/3 never
// creates the -o file. Exit codes: 0 ok (even with "errors"), 2 usage, 3 dir missing/not a directory,
// 5 cannot write the -o file.
static int cmd_samples(int argc, char** argv) {
    std::string dir, out_path;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "-o") && i + 1 < argc) {
            out_path = argv[++i];
        } else if (!dir.empty() || argv[i][0] == '-') {  // extra positional, unknown option, or bare "-o"
            usage();
            return fail("usage", std::string("unknown arg '") + argv[i] + "'");
        } else {
            dir = argv[i];
        }
    }
    if (dir.empty()) { usage(); return fail("usage", "samples needs <dir>"); }

    std::string json, err;
    if (!op::sample_index::build_sample_index(dir, json, err)) return fail("not_found", err);  // err = dir
    if (out_path.empty()) {
        std::fwrite(json.data(), 1, json.size(), stdout);
        return 0;
    }
    std::ofstream of(out_path, std::ios::binary | std::ios::trunc);
    if (!of) return fail("write", "cannot open '" + out_path + "' for writing");
    of.write(json.data(), static_cast<std::streamsize>(json.size()));
    of.close();
    if (!of) return fail("write", "cannot write '" + out_path + "'");
    return 0;
}

// opd peaks <in.wav> [--bins N] — min/max waveform peaks per bin as JSON (docs/spec/peaks.md, WP-0171).
// Reads WAV or AIFF via op::wav::read_audio; prints to_json(compute_peaks(...)) to stdout and nothing else.
// Exit codes: 2 usage (no file, second positional, unknown option, valueless --bins, bad N), 3 not-found,
// 4 unreadable audio (message = the reader's error). Errors print nothing on stdout.
static int cmd_peaks(int argc, char** argv) {
    std::vector<std::string> pos;
    long long bins = 1024;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--bins") && i + 1 < argc) {
            long long v = 0;
            if (!parse_decimal(argv[i + 1], op::peaks::kMaxBins, v) || v < 1)
                { usage(); return fail("usage", std::string("bad --bins value '") + argv[i + 1] + "'"); }
            bins = v;
            ++i;
        } else if (!std::strncmp(argv[i], "--", 2)) {  // includes a valueless `--bins` and unknown options
            usage(); return fail("usage", std::string("unknown arg '") + argv[i] + "'");
        } else if (pos.size() < 1) {
            pos.push_back(argv[i]);
        } else {  // second positional
            usage(); return fail("usage", std::string("unknown arg '") + argv[i] + "'");
        }
    }
    if (pos.empty()) { usage(); return fail("usage", "peaks needs <in.wav>"); }
    const std::string& path = pos[0];
    if (!fs::exists(path)) return fail("not_found", path);
    op::wav::ReadResult rr = op::wav::read_audio(path);
    if (!rr.ok) return fail("parse", rr.error);
    const op::peaks::Peaks p =
        op::peaks::compute_peaks(rr.samples, rr.channels, rr.sample_rate, static_cast<int>(bins));
    const std::string json = op::peaks::to_json(p);
    std::fwrite(json.data(), 1, json.size(), stdout);
    return 0;
}

// Canonical per-bar ruler JSON (WP-0210, task WP-0210 §JSON): one row per bar i = 0 .. n_bars-1 with its
// start beat, start sample (project sample rate), start seconds and the tempo in force there. Every value
// comes from the renderer's own mapping — op::beats_at_bar / op::sample_at_beat / op::bpm_at_beat — so this
// helper contains no bar/beat/tempo math of its own (the seconds division below is the only arithmetic).
// Byte-exact output: '\n' line endings, two-space header indent, four-space rows, no comma after the last
// row. Example: timeline_json(p, 1) on a 4/4 120 bpm project starts "{\n  \"sample_rate\": 48000,\n".
static std::string timeline_json(const op::Project& p, long long n_bars) {
    const op::TimeSignature ts = op::effective_time_signature(p);
    char line[256];
    std::string out;
    std::snprintf(line, sizeof line, "{\n  \"sample_rate\": %d,\n", p.sample_rate);
    out += line;
    std::snprintf(line, sizeof line, "  \"beats_per_bar\": %d,\n", p.beats_per_bar);
    out += line;
    std::snprintf(line, sizeof line, "  \"time_signature\": \"%d/%d\",\n", ts.num, ts.den);
    out += line;
    std::snprintf(line, sizeof line, "  \"length_bars\": %.6f,\n", p.length_bars);
    out += line;
    out += "  \"bars\": [\n";
    for (long long i = 0; i < n_bars; ++i) {
        const double start_beat = op::beats_at_bar(p, static_cast<double>(i));
        const long long start_sample = op::sample_at_beat(p, start_beat);
        const double start_seconds =
            static_cast<double>(start_sample) / static_cast<double>(p.sample_rate);
        const double bpm = op::bpm_at_beat(p, start_beat);
        std::snprintf(line, sizeof line,
                      "    {\"bar\": %lld, \"start_beat\": %.6f, \"start_sample\": %lld,"
                      " \"start_seconds\": %.6f, \"bpm\": %.2f}%s\n",
                      i, start_beat, start_sample, start_seconds, bpm, (i + 1 < n_bars) ? "," : "");
        out += line;
    }
    out += "  ]\n";
    out += "}\n";
    return out;
}

// opd timeline <project.opj> [--bars N] (WP-0210) — the bar ruler as canonical JSON: per bar its start
// beat, start sample and start seconds plus the tempo in force there, all taken from the renderer's own
// tempo map. Read-only: nothing is written anywhere and the project is never modified; output is
// deterministic. Options are checked before the file is touched. Exit codes: 2 usage (unknown arg or a
// second positional, no project argument, a bad --bars value, or --bars above the project's bar count),
// 3 not-found, 4 parse (message = the loader's error). Errors print nothing on stdout.
static int cmd_timeline(int argc, char** argv) {
    std::vector<std::string> pos;
    bool has_bars = false;
    long long bars_arg = 0;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--bars") && i + 1 < argc) {
            long long v = 0;
            if (!parse_decimal(argv[i + 1], 4096, v) || v < 1)
                { usage(); return fail("usage", std::string("bad --bars value '") + argv[i + 1] + "'"); }
            bars_arg = v;
            has_bars = true;
            ++i;
        } else if (argv[i][0] == '-' || !pos.empty()) {  // unknown option (incl. valueless --bars) or extra positional
            usage(); return fail("usage", std::string("unknown arg '") + argv[i] + "'");
        } else {
            pos.push_back(argv[i]);
        }
    }
    if (pos.empty()) { usage(); return fail("usage", "timeline needs <project.opj>"); }
    const std::string& proj = pos[0];
    if (!fs::exists(proj)) return fail("not_found", proj);

    op::Project p;
    std::string err;
    if (!op::load_project(proj, p, err)) return fail("parse", err);

    // One row per bar by default: a final partial bar counts (length_bars > 0 is enforced by the model).
    const long long n_proj = static_cast<long long>(std::ceil(p.length_bars));
    long long n_bars = n_proj;
    if (has_bars) {
        if (bars_arg > n_proj) {
            usage();
            return fail("usage", "--bars " + std::to_string(bars_arg) + " is larger than the project's " +
                                      std::to_string(n_proj) + " bars");
        }
        n_bars = bars_arg;
    }
    const std::string json = timeline_json(p, n_bars);
    std::fwrite(json.data(), 1, json.size(), stdout);
    return 0;
}

// opd audition <in.wav> -o <out.wav> [--note N --root R] [--start-s A --end-s B] [--seconds S] (WP-0209) —
// render ONE note of a sample through the existing sampler voice (resample/trim/repitch), no project needed.
// The output is bit-exact with `opd render` of the equivalent one-track project (op::audition::build_project).
// Read-only: the input asset is never modified and nothing but the -o file is ever written; deterministic.
// Checks run in this exact order, all before the input file is looked at. Exit codes: 2 usage (each message
// below), 3 not-found, 5 render/write error (the sampler's own message, verbatim). Success prints exactly
// one line to stdout: "auditioned <frames> frames @<rate> Hz -> <out_path>".
static int cmd_audition(int argc, char** argv) {
    std::string in_path, out_path;
    bool has_in = false, has_out = false;
    std::string note_arg, root_arg, start_arg, end_arg, sec_arg;  // raw strings for exact error messages
    bool has_note = false, has_root = false, has_start = false, has_end = false, has_sec = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "-o") && i + 1 < argc) {
            out_path = argv[++i];
            has_out = true;
        } else if (!std::strcmp(argv[i], "--note") && i + 1 < argc) {
            note_arg = argv[++i];
            has_note = true;
        } else if (!std::strcmp(argv[i], "--root") && i + 1 < argc) {
            root_arg = argv[++i];
            has_root = true;
        } else if (!std::strcmp(argv[i], "--start-s") && i + 1 < argc) {
            start_arg = argv[++i];
            has_start = true;
        } else if (!std::strcmp(argv[i], "--end-s") && i + 1 < argc) {
            end_arg = argv[++i];
            has_end = true;
        } else if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) {
            sec_arg = argv[++i];
            has_sec = true;
        } else if (argv[i][0] == '-' || has_in) {  // unknown option (incl. valueless flags) or second positional
            usage(); return fail("usage", std::string("unknown arg '") + argv[i] + "'");
        } else {
            in_path = argv[i];
            has_in = true;
        }
    }

    // 1-2: the two mandatory arguments.
    if (!has_in) { usage(); return fail("usage", "audition needs <in.wav>"); }
    if (!has_out || out_path.empty()) { usage(); return fail("usage", "missing -o <out.wav>"); }

    // 3-4: --seconds syntax then range (0, kMaxSeconds].
    double sec = 0.0;
    if (has_sec) {
        if (!parse_bar_value(sec_arg, sec))
            { usage(); return fail("usage", "bad --seconds value '" + sec_arg + "'"); }
        if (!(sec > 0.0) || !(sec <= op::audition::kMaxSeconds))
            { usage(); return fail("usage", "--seconds must be in (0, 300]"); }
    }

    // 5-6: --start-s syntax then range [0, +inf).
    double start_s = 0.0;
    if (has_start) {
        if (!parse_bar_value(start_arg, start_s))
            { usage(); return fail("usage", "bad --start-s value '" + start_arg + "'"); }
        if (!(start_s >= 0.0)) { usage(); return fail("usage", "--start-s must be >= 0"); }
    }

    // 7-8: --end-s syntax then end > start (start defaults to 0.0 when the flag is absent).
    double end_s = 0.0;
    if (has_end) {
        if (!parse_bar_value(end_arg, end_s))
            { usage(); return fail("usage", "bad --end-s value '" + end_arg + "'"); }
        if (!(end_s > start_s))
            { usage(); return fail("usage", "--end-s must be greater than --start-s"); }
    }

    // 9-10: --note / --root are decimal MIDI pitches in [0, 127].
    long long note = -1, root = -1;
    if (has_note && !parse_decimal(note_arg, 127, note))
        { usage(); return fail("usage", "bad --note value '" + note_arg + "'"); }
    if (has_root && !parse_decimal(root_arg, 127, root))
        { usage(); return fail("usage", "bad --root value '" + root_arg + "'"); }

    // 11: pitch only makes sense as a pair (no root -> the engine never repitches).
    if (has_note != has_root) {
        usage();
        return fail("usage", "--note and --root must be given together");
    }

    // 12: the input file must exist; everything else about it is the sampler's verdict at render time.
    if (!fs::exists(in_path)) return fail("not_found", in_path);

    op::audition::Options o;
    o.path = in_path;
    o.note = has_note ? static_cast<int>(note) : -1;
    o.root = has_root ? static_cast<int>(root) : -1;
    o.start_s = start_s;
    o.end_s = end_s;
    o.has_end_s = has_end;
    if (has_sec) {
        o.frames = op::audition::to_frames(sec);  // explicit length wins over everything
    } else if (has_end) {
        o.frames = op::audition::to_frames(end_s) - op::audition::to_frames(start_s);  // the trimmed span
    } else {
        o.frames = op::audition::kDefaultFrames;  // 2 s
    }

    // Empty base_dir: a relative in_path resolves against the caller's working directory, an absolute
    // one passes through. Errors are the sampler's own — never reworded here.
    op::wav::StereoBuffer buf;
    std::string err;
    if (!op::audition::render(o, "", buf, err)) return fail("render", err);
    if (!op::wav::write_wav_stereo(out_path, buf, op::wav::SampleFormat::Pcm16, err)) return fail("write", err);

    const std::string line = "auditioned " + std::to_string(buf.frames()) + " frames @" +
                             std::to_string(buf.sample_rate) + " Hz -> " + out_path + "\n";
    std::fwrite(line.data(), 1, line.size(), stdout);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 2; }
    const std::string cmd = argv[1];
    if (cmd == "render") return cmd_render(argc - 1, argv + 1);
    if (cmd == "click") return cmd_click(argc - 1, argv + 1);
    if (cmd == "new") return cmd_new(argc - 1, argv + 1);
    if (cmd == "gen-testproject") return cmd_gen_testproject(argc - 1, argv + 1);
    if (cmd == "midi2opj") return cmd_midi2opj(argc - 1, argv + 1);
    if (cmd == "import-midi") return cmd_import_midi(argc - 1, argv + 1);
    if (cmd == "attach") return cmd_attach(argc - 1, argv + 1);
    if (cmd == "apply") return cmd_apply(argc - 1, argv + 1);
    if (cmd == "diff") return cmd_diff(argc - 1, argv + 1);
    if (cmd == "opj2mid") return cmd_opj2mid(argc - 1, argv + 1);
    if (cmd == "pack") return cmd_pack(argc - 1, argv + 1);
    if (cmd == "unpack") return cmd_unpack(argc - 1, argv + 1);
    if (cmd == "info") return cmd_info(argc - 1, argv + 1);
    if (cmd == "analyze") return cmd_analyze(argc - 1, argv + 1);
    if (cmd == "lint") return cmd_lint(argc - 1, argv + 1);
    if (cmd == "peaks") return cmd_peaks(argc - 1, argv + 1);
    if (cmd == "meters") return cmd_meters(argc - 1, argv + 1);
    if (cmd == "loudness") return cmd_loudness(argc - 1, argv + 1);
    if (cmd == "normalize") return cmd_normalize(argc - 1, argv + 1);
    if (cmd == "clips") return cmd_clips(argc - 1, argv + 1);
    if (cmd == "steps") return cmd_steps(argc - 1, argv + 1);
    if (cmd == "samples") return cmd_samples(argc - 1, argv + 1);
    if (cmd == "timeline") return cmd_timeline(argc - 1, argv + 1);
    if (cmd == "audition") return cmd_audition(argc - 1, argv + 1);
    usage();
    return 2;
}
