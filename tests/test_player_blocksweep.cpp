// tests/test_player_blocksweep.cpp — WP-0241: the live transport path is byte-identical to `opd render`.
//
// Phase 1 playback renders a project offline and hands the buffer to dz::Player, whose callback copies
// frames out of it (src/app/player.hpp, src/app/player_rt.cpp). Loop region (WP-0200), metronome (WP-0201)
// and any later per-block transport work can change what that callback emits while `opd render` stays
// untouched. This test pins the invariant over the whole golden corpus: every tests/fixtures/goldens/*.opj
// is rendered in-process, submitted to a fresh dz::Player, and the callback is driven by ONE fixed-seed
// sequence of callback sizes (32..512 samples, both inclusive). Exactly the project's frames are kept —
// the tail of the stopping block is dropped, the way dz::run_render_playback does it — written as canonical
// PCM16 stereo WAV, and the file bytes must equal those of the real `opd render` of the same project.
//
// Test-only: no engine, CLI, app or spec code is touched, and no behaviour changes. A mismatch is reported
// honestly (mismatch + the first differing byte on stderr), never patched around.
//
// Determinism (ADR-0004): the only randomness source is std::mt19937 seeded with the literal 20240241u,
// constructed fresh per project. No wall clock, no std::random_device, so two runs print identical SWEEP
// lines. Every byte comparison in this file goes through bytes_are_identical(), and the two negative
// controls exercise exactly that helper, so a comparison that cannot fail cannot be hidden.
//
// Output contract (parsed by the harness): one line per corpus project, in corpus order (sorted by full
// path), nothing before SWEEP and nothing after the match= value:
//   SWEEP <stem> frames=<F> blocks=<B> min_block=<M> max_block=<X> bytes=<N> match=<byte-identical|mismatch>
// then the PASS/FAIL lines, then "ALL PASS (0 failures)" or "<k> FAILURE(S)". Exit code = number of failures.
// Run: ./build/op_player_blocksweep_tests
#include <unistd.h>  // getpid

#include <sys/wait.h>  // WIFEXITED / WEXITSTATUS

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <sstream>
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

namespace fs = std::filesystem;

// The callback sizes the fake device draws, inclusive at both ends. At the 48 kHz golden rate the largest
// callback is ~10.7 ms; the smallest is the tightest plausible device block.
static constexpr int kMinBlock = 32;
static constexpr int kMaxBlock = 512;
static constexpr unsigned kRngSeed = 20240241u;  // fixed seed, fresh mt19937 per project (ADR-0004)

// Canonical stereo PCM16 layout: 44-byte header, then 4 bytes per frame.
static constexpr std::size_t kWavHeaderBytes = 44;
static constexpr std::size_t kBytesPerFrame = 4;

static const fs::path& scratch_dir() {
    static const fs::path dir = fs::temp_directory_path() /
        ("op_player_blocksweep_wp0241_" + std::to_string(static_cast<long>(::getpid())));  // per-process
    return dir;
}

static std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// THE comparison helper: whole file, same size and memcmp == 0. No tolerance, no epsilon, no ignored tail,
// no per-channel or per-frame comparison. Every byte comparison in this file, including the two negative
// controls, goes through here.
static bool bytes_are_identical(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0;
}

// The corpus: every regular file directly in tests/fixtures/goldens with extension .opj, sorted by full
// path, not recursive — the same set harness/run_goldens.sh iterates. OP_TEST_DATA is "<repo>/tests".
static std::vector<fs::path> golden_corpus() {
    const fs::path dir = fs::path(OP_TEST_DATA) / "fixtures" / "goldens";
    std::vector<fs::path> files;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; it != end; ++it) {
        const fs::path p = it->path();
        if (p.extension() != ".opj") continue;
        std::error_code rec;
        if (!fs::is_regular_file(p, rec)) continue;
        files.push_back(p);
    }
    std::sort(files.begin(), files.end());
    return files;
}

// One project's sweep: reference bytes from the real CLI, live bytes from the transport, and the counters.
struct Sweep {
    std::string stem;
    bool cli_ok = false;      // `opd render` exited 0
    bool rendered = false;    // load_project + render_project both succeeded
    bool written = false;     // the live WAV was written
    long long frames = 0;     // F — frames of the render
    long long blocks = 0;     // B — process() calls it took
    int min_block = 0;        // smallest block drawn for this project
    int max_block = 0;        // largest block drawn for this project
    bool counters_ok = false;  // !playing() && frames_played() == F after the run
    std::vector<int> drawn;   // the block sizes, in call order (for mismatch diagnostics)
    std::string ref;          // bytes of <stem>.ref.wav
    std::string live;         // bytes of <stem>.live.wav
    bool identical = false;   // cli_ok && rendered && written && ref bytes == live bytes
};

// One project, exactly the procedure in the work package: CLI reference → in-process render → submit to a
// fresh Player → fixed-seed 32..512-sample callbacks → keep F frames → write → compare.
static Sweep sweep_project(const fs::path& opj, const std::string& tag) {
    Sweep s;
    s.stem = opj.stem().string();
    std::string err;

    const fs::path ref_path = scratch_dir() / (s.stem + tag + ".ref.wav");
    const fs::path live_path = scratch_dir() / (s.stem + tag + ".live.wav");

    // (1) Reference — the real CLI binary, the same invocation harness/run_goldens.sh makes.
    const std::string cmd = std::string("\"") + OPD_BIN + "\" render \"" + opj.string() +
                            "\" -o \"" + ref_path.string() + "\" >/dev/null 2>&1";
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (pipe != nullptr) {
        char sink[256];
        while (std::fgets(sink, sizeof sink, pipe)) { /* output already redirected: nothing to keep */ }
        const int status = ::pclose(pipe);
        s.cli_ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }
    s.ref = read_file(ref_path);

    // (2) Live path — render in-process with base_dir = the project's directory (sampler path resolution,
    // exactly as opd render does it) and submit a copy to a fresh player. Plain playback: no loop region,
    // no prepare_click, no stop().
    op::Project p;
    op::wav::StereoBuffer buf;
    s.rendered = op::load_project(opj.string(), p, err) &&
                 op::render_project(p, opj.parent_path().string(), buf, err);
    if (s.rendered) s.frames = static_cast<long long>(buf.frames());

    dz::Player player;
    if (s.rendered) {
        player.submit(std::make_unique<op::wav::StereoBuffer>(buf));
        player.play();

        // (3) Fake device — block sizes from ONE fixed-seed sequence, kept calling until every frame of
        // the project has been produced.
        std::mt19937 rng(kRngSeed);
        std::vector<float> left(static_cast<size_t>(kMaxBlock)), right(static_cast<size_t>(kMaxBlock));
        op::wav::StereoBuffer out;
        out.sample_rate = buf.sample_rate;
        out.interleaved.reserve(static_cast<size_t>(s.frames > 0 ? s.frames : 0) * 2);

        int lo = kMaxBlock + 1, hi = kMinBlock - 1;
        long long produced = 0;
        while (produced < s.frames) {
            const int block =
                kMinBlock + static_cast<int>(rng() % static_cast<unsigned>(kMaxBlock - kMinBlock + 1));
            player.process(left.data(), right.data(), block);
            ++s.blocks;
            s.drawn.push_back(block);
            lo = std::min(lo, block);
            hi = std::max(hi, block);

            // (4) Concatenation — exactly F frames: take the leading frames that still fit and drop the
            // rest of the block (the player zero-fills that tail and parks at frame 0; it is not output).
            const long long take = std::min(static_cast<long long>(block), s.frames - produced);
            for (long long i = 0; i < take; ++i) {
                out.interleaved.push_back(left[static_cast<size_t>(i)]);
                out.interleaved.push_back(right[static_cast<size_t>(i)]);
            }
            produced += block;
        }
        s.min_block = s.blocks > 0 ? lo : 0;
        s.max_block = s.blocks > 0 ? hi : 0;
        s.counters_ok = !player.playing() && player.frames_played() == s.frames;

        // (5) Compare — write the concatenated buffer as canonical PCM16 stereo and compare whole files.
        s.written = op::wav::write_pcm16_stereo(live_path.string(), out, err);
    }
    s.live = read_file(live_path);
    s.identical = s.cli_ok && s.rendered && s.written && bytes_are_identical(s.ref, s.live);
    return s;
}

static void print_sweep_line(const Sweep& s) {
    std::printf("SWEEP %s frames=%lld blocks=%lld min_block=%d max_block=%d bytes=%llu match=%s\n",
                s.stem.c_str(), s.frames, s.blocks, s.min_block, s.max_block,
                static_cast<unsigned long long>(s.live.size()),
                s.identical ? "byte-identical" : "mismatch");
}

// Mismatch diagnostics go to STDERR only — stdout carries exactly the SWEEP lines, the PASS/FAIL lines and
// the summary. The first differing byte and the callback number that produced it are what a PROPOSE: note
// for this package needs.
static void report_mismatch(const Sweep& s) {
    if (s.identical) return;
    const std::size_t n = std::min(s.ref.size(), s.live.size());
    std::size_t off = 0;
    while (off < n && s.ref[off] == s.live[off]) ++off;
    long long frame = -1, callback = -1;
    if (off >= kWavHeaderBytes && off < s.live.size()) {
        frame = static_cast<long long>((off - kWavHeaderBytes) / kBytesPerFrame);
        long long acc = 0;
        for (std::size_t i = 0; i < s.drawn.size(); ++i) {
            acc += s.drawn[i];
            if (frame < acc) { callback = static_cast<long long>(i) + 1; break; }
        }
    }
    std::fprintf(stderr,
                 "MISMATCH %s: cli_ok=%d rendered=%d written=%d ref_bytes=%llu live_bytes=%llu "
                 "first_diff_byte=%llu first_diff_frame=%lld callback=%lld\n",
                 s.stem.c_str(), s.cli_ok ? 1 : 0, s.rendered ? 1 : 0, s.written ? 1 : 0,
                 static_cast<unsigned long long>(s.ref.size()),
                 static_cast<unsigned long long>(s.live.size()),
                 static_cast<unsigned long long>(off), frame, callback);
}

int main() {
    std::error_code ec;
    fs::remove_all(scratch_dir(), ec);
    fs::create_directories(scratch_dir(), ec);
    if (ec) {
        std::printf("FATAL: cannot create temp dir %s\n", scratch_dir().string().c_str());
        return 2;
    }

    const std::vector<fs::path> corpus = golden_corpus();

    std::vector<Sweep> sweeps;
    sweeps.reserve(corpus.size());
    for (const fs::path& opj : corpus) {
        sweeps.push_back(sweep_project(opj, ""));
        print_sweep_line(sweeps.back());
        report_mismatch(sweeps.back());
    }

    // --- one CHECK per corpus project: its live WAV bytes equal the opd render bytes, and its render,
    // submit and write all succeeded.
    for (const Sweep& s : sweeps) {
        CHECK(s.identical, ("blocksweep." + s.stem + ".matches_opd_render").c_str());
    }

    // --- every golden project of the corpus was swept, and the corpus is the corpus.
    std::size_t swept = 0;
    for (const Sweep& s : sweeps)
        if (s.cli_ok && s.rendered && s.written) ++swept;
    CHECK(swept == corpus.size() && corpus.size() >= 10, "blocksweep.every_golden_project_swept");

    // --- whole-corpus byte identity.
    bool all_identical = !sweeps.empty() && sweeps.size() == corpus.size();
    for (const Sweep& s : sweeps) all_identical = all_identical && s.identical;
    CHECK(all_identical, "blocksweep.every_project_is_byte_identical");

    // --- the callback count is consistent with 32..512-sample callbacks producing exactly F frames:
    // (F + 511) / 512 <= B <= (F + 31) / 32, integer arithmetic.
    bool counts_ok = !sweeps.empty();
    for (const Sweep& s : sweeps) {
        const long long fewest = (s.frames + kMaxBlock - 1) / kMaxBlock;
        const long long most = (s.frames + kMinBlock - 1) / kMinBlock;
        counts_ok = counts_ok && s.blocks >= fewest && s.blocks <= most;
    }
    CHECK(counts_ok, "blocksweep.callback_counts_match_the_frame_totals");

    // --- references are canonical stereo PCM16: 44 + 4 * F bytes, and the live file has the same size.
    bool sizes_ok = !sweeps.empty();
    for (const Sweep& s : sweeps) {
        const std::size_t want = kWavHeaderBytes + kBytesPerFrame * static_cast<std::size_t>(s.frames);
        sizes_ok = sizes_ok && s.ref.size() == want && s.live.size() == want;
    }
    CHECK(sizes_ok, "blocksweep.references_are_canonical_pcm16_stereo_wavs");

    // --- the transport ended where a one-shot playthrough must end: not playing, parked at home, with
    // exactly F frames copied.
    bool counters_ok = !sweeps.empty();
    for (const Sweep& s : sweeps) counters_ok = counters_ok && s.counters_ok;
    CHECK(counters_ok, "blocksweep.transport_counters_match_the_played_frames");

    // --- the sweep really sweeps block sizes: every draw inside [32, 512], and across the corpus the
    // smallest drawn block is <= 64 while the largest is >= 448 (a single-size sweep fails this).
    bool in_range = true;
    int global_lo = kMaxBlock + 1, global_hi = kMinBlock - 1;
    std::size_t draws = 0;
    for (const Sweep& s : sweeps) {
        for (int b : s.drawn) {
            ++draws;
            if (b < kMinBlock || b > kMaxBlock) in_range = false;
            global_lo = std::min(global_lo, b);
            global_hi = std::max(global_hi, b);
        }
    }
    CHECK(in_range && draws > 0 && global_lo <= 64 && global_hi >= 448,
          "blocksweep.block_sizes_stay_in_range_and_actually_vary");

    // --- the sequence is fixed-seed, so the same project swept twice gives bit-identical live bytes. No
    // extra SWEEP line: stdout carries one per corpus project, in corpus order.
    bool repeat_identical = false;
    if (!corpus.empty() && !sweeps.empty()) {
        const Sweep again = sweep_project(corpus[0], ".repeat");
        repeat_identical = again.cli_ok && again.rendered && again.written &&
                           bytes_are_identical(sweeps[0].live, again.live);
    }
    CHECK(repeat_identical, "blocksweep.two_fixed_seed_sweeps_are_bit_identical");

    // --- negative controls, both through bytes_are_identical() on a real swept project: the probe is the
    // first identical sweep whose payload reaches frame 5000 + 128 (every golden in the corpus does).
    const Sweep* probe = nullptr;
    const std::size_t need = kWavHeaderBytes + kBytesPerFrame * (5000 + 128);
    for (const Sweep& s : sweeps)
        if (s.identical && s.live.size() >= need) { probe = &s; break; }

    bool flip_detected = false, swap_detected = false;
    if (probe != nullptr) {
        std::string flipped = probe->live;
        const std::size_t flip_at = kWavHeaderBytes + kBytesPerFrame * 100;
        flipped[flip_at] = static_cast<char>(flipped[flip_at] ^ 0xFF);
        flip_detected = !bytes_are_identical(flipped, probe->ref);

        std::string swapped = probe->live;
        const std::size_t at_a = kWavHeaderBytes + kBytesPerFrame * 1000;
        const std::size_t at_b = kWavHeaderBytes + kBytesPerFrame * 5000;
        const std::size_t chunk = kBytesPerFrame * 128;  // two 128-frame chunks
        for (std::size_t i = 0; i < chunk; ++i) std::swap(swapped[at_a + i], swapped[at_b + i]);
        swap_detected = !bytes_are_identical(swapped, probe->ref);
    }
    CHECK(flip_detected, "blocksweep.negative_control_bit_flip_detected");
    CHECK(swap_detected, "blocksweep.negative_control_block_swap_detected");

    fs::remove_all(scratch_dir(), ec);
    if (g_failures == 0) std::printf("ALL PASS (0 failures)\n");
    else std::printf("%d FAILURE(S)\n", g_failures);
    return g_failures;
}
