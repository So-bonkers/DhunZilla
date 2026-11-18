// opd — OpenPattern headless CLI. Subcommands: render | gen-testproject | info.
// Exit codes (spec/project-model.md): 0 ok, 2 usage, 3 not-found, 4 parse, 5 render/write error.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "engine/bundle.hpp"
#include "engine/dsp.hpp"
#include "engine/midi.hpp"
#include "engine/model.hpp"
#include "engine/render.hpp"
#include "engine/wav.hpp"

namespace fs = std::filesystem;

static void usage() {
    std::fprintf(stderr,
        "usage:\n"
        "  opd render <project.opj> -o <out.wav> [--seed N]\n"
        "  opd gen-testproject <out.opj>     (also writes kits/kick.wav next to it)\n"
        "  opd midi2opj <in.mid> -o <out.opj>\n"
        "  opd import-midi <proj.opj> <in.mid> [-o out.opj]   (merge as new tracks)\n"
        "  opd opj2mid <in.opj> -o <out.mid>\n"
        "  opd pack <in.opj> -o <song.opd>    (project + samples in one file)\n"
        "  opd unpack <song.opd> -d <out-dir>\n"
        "  opd info <file.wav|project.opj>\n");
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

static int cmd_render(int argc, char** argv) {
    if (argc < 4) { usage(); return fail("usage", "render needs project + -o out"); }
    const std::string proj = argv[1];
    std::string out_path, stems_dir;
    long long seed_override = -1;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "-o") && i + 1 < argc) out_path = argv[++i];
        else if (!std::strcmp(argv[i], "--stems-dir") && i + 1 < argc) stems_dir = argv[++i];
        else if (!std::strcmp(argv[i], "--seed") && i + 1 < argc) seed_override = std::atoll(argv[++i]);
        else { usage(); return fail("usage", std::string("unknown arg '") + argv[i] + "'"); }
    }
    if (out_path.empty()) { usage(); return fail("usage", "missing -o <out.wav>"); }

    op::Project p;
    std::string err;
    if (!fs::exists(proj)) return fail("not_found", proj);
    if (!op::load_project(proj, p, err)) return fail("parse", err);
    if (seed_override >= 0) p.seed = static_cast<uint64_t>(seed_override);

    const std::string base_dir = fs::path(proj).parent_path().string();
    if (stems_dir.empty()) {
        op::wav::StereoBuffer buf;
        if (!op::render_project(p, base_dir, buf, err)) return fail("render", err);
        if (!op::wav::write_pcm16_stereo(out_path, buf, err)) return fail("write", err);
        std::printf("rendered %lld frames @%d Hz -> %s (seed=%llu)\n",
                    static_cast<long long>(buf.frames()), buf.sample_rate, out_path.c_str(),
                    static_cast<unsigned long long>(p.seed));
    } else {
        std::vector<op::wav::StereoBuffer> stems;
        op::wav::StereoBuffer mix;
        if (!op::render_stems(p, base_dir, stems, mix, err)) return fail("render", err);
        if (!op::wav::write_pcm16_stereo(out_path, mix, err)) return fail("write", err);
        std::error_code ec;
        fs::create_directories(stems_dir, ec);
        if (ec) return fail("write", "cannot create stems dir " + stems_dir);
        for (size_t i = 0; i < stems.size(); ++i) {
            const std::string name = sanitize_track_name(p.tracks[i].name);
            const std::string path = stems_dir + "/" + std::to_string(i) + "_" + (name.empty() ? "track" : name) + ".wav";
            if (!op::wav::write_pcm16_stereo(path, stems[i], err)) return fail("write", err);
            std::printf("stem %zu -> %s\n", i, path.c_str());
        }
        std::printf("rendered mix %lld frames @%d Hz -> %s (+%zu stems, seed=%llu)\n",
                    static_cast<long long>(mix.frames()), mix.sample_rate, out_path.c_str(), stems.size(),
                    static_cast<unsigned long long>(p.seed));
    }
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

static int cmd_info(int argc, char** argv) {
    if (argc < 2) { usage(); return fail("usage", "info needs a file"); }
    const std::string path = argv[1];
    if (!fs::exists(path)) return fail("not_found", path);
    if (path.size() > 4 && path.substr(path.size() - 4) == ".wav") {
        auto rr = op::wav::read_wav(path);
        if (!rr.ok) return fail("parse", rr.error);
        std::printf("wav: %s rate=%d ch=%d frames=%zu duration=%.3fs\n", path.c_str(), rr.sample_rate,
                    rr.channels, rr.samples.size() / static_cast<size_t>(rr.channels),
                    static_cast<double>(rr.samples.size() / rr.channels) / rr.sample_rate);
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
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 2; }
    const std::string cmd = argv[1];
    if (cmd == "render") return cmd_render(argc - 1, argv + 1);
    if (cmd == "gen-testproject") return cmd_gen_testproject(argc - 1, argv + 1);
    if (cmd == "midi2opj") return cmd_midi2opj(argc - 1, argv + 1);
    if (cmd == "import-midi") return cmd_import_midi(argc - 1, argv + 1);
    if (cmd == "opj2mid") return cmd_opj2mid(argc - 1, argv + 1);
    if (cmd == "pack") return cmd_pack(argc - 1, argv + 1);
    if (cmd == "unpack") return cmd_unpack(argc - 1, argv + 1);
    if (cmd == "info") return cmd_info(argc - 1, argv + 1);
    usage();
    return 2;
}
