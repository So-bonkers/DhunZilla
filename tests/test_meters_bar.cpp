// Unit tests for the WP-0243 additions to op::meters: meter_bar() and measure_project_per_bar() —
// per-bar peak/RMS dBFS arrays for every track and the master, with bar windows taken from the
// renderer's own mapping op::sample_at_bar (tempo map + time signature honoured, final partial bar
// included and measured up to the end of the render). Boring assert-based runner — no framework
// deps, own main. Exit code = number of failures.
// Run: ./build/op_meters_bar_tests
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "engine/analyze.hpp"
#include "engine/meters.hpp"
#include "engine/render.hpp"
#include "engine/wav.hpp"

static int g_failures = 0;
#define CHECK(cond, name)                                                              \
    do {                                                                               \
        if (cond) std::printf("PASS %s\n", name);                                      \
        else { std::printf("FAIL %s  (%s:%d)\n", name, __FILE__, __LINE__); ++g_failures; } \
    } while (0)

// 2.5 bars @ 120 bpm 4/4 @ 48 kHz = 240000 frames → bar_count = ceil(2.5) = 3, windows
// [0,96000) [96000,192000) [192000,240000). Tracks: an always-on sine named a"b (name escaping in
// the JSON checks), a muted sine (all-zero stem → all-null bars) and a blip sine whose single note
// (sine voices are exactly note-length) sits inside bar 1 only → bars 0 and 2 are null.
static const char* kProjectJson = R"JSON({
  "format_version": 0,
  "bpm": 120.0,
  "sample_rate": 48000,
  "seed": 91,
  "length_bars": 2.5,
  "beats_per_bar": 4,
  "tracks": [
    {"name": "a\"b", "voice": {"kind": "sine", "freq_hz": 440.0}},
    {"name": "muted bass", "mute": true, "voice": {"kind": "sine", "freq_hz": 55.0}},
    {"name": "blip", "gain_db": -6.0, "voice": {"kind": "sine", "freq_hz": 880.0}}
  ],
  "patterns": [
    {"track": 0, "notes": [{"t": 0.0, "dur": 10.0}]},
    {"track": 1, "notes": [{"t": 0.0, "dur": 10.0}]},
    {"track": 2, "notes": [{"t": 4.25, "dur": 3.5}]}
  ]
}
)JSON";

// Frames [from, to) of `buf` as a standalone interleaved vector (for analyze_samples comparisons).
static std::vector<float> window_of(const op::wav::StereoBuffer& buf, long long from, long long to) {
    return std::vector<float>(buf.interleaved.begin() + 2 * from, buf.interleaved.begin() + 2 * to);
}

static bool same_opt(const std::optional<double>& a, const std::optional<double>& b) {
    return static_cast<bool>(a) == static_cast<bool>(b) && (!a || *a == *b);
}

int main() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() /
        ("op_meters_bar_tests_wp0243_" + std::to_string(static_cast<long>(::getpid())));  // per-process, no collisions
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    if (ec) {
        std::printf("FATAL: cannot create temp dir %s\n", dir.string().c_str());
        return 2;
    }

    const fs::path opj = dir / "meters_bar.opj";
    {
        std::ofstream f(opj, std::ios::binary);
        f << kProjectJson;
    }

    op::Project p;
    std::string err;
    CHECK(op::load_project(opj.string(), p, err), "meters_bar.test_project_json_loads");

    op::meters::Report rep;
    const bool ok = op::meters::measure_project_per_bar(p, dir.string(), rep, err);
    CHECK(ok, "meters_bar.measure_project_per_bar_succeeds");

    // --- bar_count == ceil(length_bars): 2.5 bars → 3 bars (the count opd timeline prints),
    //     per_bar flagged.
    CHECK(ok && rep.per_bar && rep.bar_count == 3, "meters_bar.bar_count_equals_ceil_length_bars");

    // --- header and whole-buffer values filled exactly as measure_project fills them (and the plain
    //     Report stays per-bar-free: empty bars vectors, per_bar false).
    op::meters::Report plain;
    const bool ok_plain = op::meters::measure_project(p, dir.string(), plain, err);
    bool header_ok = ok && ok_plain && !plain.per_bar && plain.bar_count == 0 &&
                     rep.sample_rate == plain.sample_rate && rep.frames == plain.frames &&
                     rep.duration_s == plain.duration_s &&
                     rep.sample_rate == 48000 && rep.frames == 240000;
    for (size_t i = 0; header_ok && i < rep.tracks.size() && i < plain.tracks.size(); ++i) {
        header_ok = header_ok && rep.tracks[i].name == plain.tracks[i].name &&
                    same_opt(rep.tracks[i].meter.peak_dbfs, plain.tracks[i].meter.peak_dbfs) &&
                    same_opt(rep.tracks[i].meter.rms_dbfs, plain.tracks[i].meter.rms_dbfs) &&
                    same_opt(rep.tracks[i].meter.integrated_lufs, plain.tracks[i].meter.integrated_lufs) &&
                    plain.tracks[i].bars.empty();
    }
    header_ok = header_ok && plain.master_bars.empty() &&
                same_opt(rep.master.peak_dbfs, plain.master.peak_dbfs) &&
                same_opt(rep.master.rms_dbfs, plain.master.rms_dbfs) &&
                same_opt(rep.master.integrated_lufs, plain.master.integrated_lufs);
    CHECK(header_ok, "meters_bar.header_and_whole_buffer_values_match_measure_project");

    // --- one BarMeter per bar for every track (project order) and for the master.
    bool sizes_ok = ok && rep.tracks.size() == 3 && rep.tracks[0].name == "a\"b" &&
                    rep.tracks[1].name == "muted bass" && rep.tracks[2].name == "blip";
    for (const auto& t : rep.tracks) sizes_ok = sizes_ok && t.bars.size() == 3;
    sizes_ok = sizes_ok && rep.master_bars.size() == 3;
    CHECK(sizes_ok, "meters_bar.one_bar_per_bar_index_for_tracks_and_master");

    // --- meter_bar field copy: bar / start_sample / end_sample copied verbatim into the result.
    std::vector<op::wav::StereoBuffer> stems;  // reference render for the analyze comparisons below
    op::wav::StereoBuffer mix;
    const bool rok = op::render_stems(p, dir.string(), stems, mix, err);
    const op::meters::BarMeter manual = rok ? op::meters::meter_bar(mix, 7, 1000, 2000)
                                            : op::meters::BarMeter{};
    CHECK(manual.bar == 7 && manual.start_sample == 1000 && manual.end_sample == 2000,
          "meters_bar.meter_bar_copies_bar_and_window_fields");

    // --- meter_bar peak/RMS == analyze_samples over exactly those frames, unchanged.
    const op::analyze::Features mf = op::analyze::analyze_samples(window_of(mix, 1000, 2000), 2, 48000);
    CHECK(same_opt(manual.peak_dbfs, mf.peak_dbfs) && same_opt(manual.rms_dbfs, mf.rms_dbfs),
          "meters_bar.meter_bar_equals_analyze_samples_of_window");

    // --- meter_bar over an all-silent window → two nulls.
    op::wav::StereoBuffer sil;
    sil.sample_rate = 48000;
    sil.interleaved.assign(static_cast<size_t>(48000) * 2u, 0.0f);
    const op::meters::BarMeter sb = op::meters::meter_bar(sil, 0, 0, 48000);
    CHECK(!sb.peak_dbfs && !sb.rms_dbfs, "meters_bar.meter_bar_silent_window_two_nulls");

    // --- meter_bar over an empty range (start == end) → window fields kept, two nulls.
    const op::meters::BarMeter eb = op::meters::meter_bar(sil, 2, 500, 500);
    CHECK(eb.bar == 2 && eb.start_sample == 500 && eb.end_sample == 500 && !eb.peak_dbfs && !eb.rms_dbfs,
          "meters_bar.meter_bar_empty_range_two_nulls");

    // --- windows come from the renderer's mapping: start == op::sample_at_bar(p, bar),
    //     end == op::sample_at_bar(p, bar + 1), for every bar of every track and of the master.
    bool windows_ok = rok && rep.frames == static_cast<long long>(mix.frames());
    for (const auto& t : rep.tracks) {
        for (const auto& b : t.bars) {
            windows_ok = windows_ok && b.bar >= 0 && b.bar < rep.bar_count &&
                         b.start_sample == op::sample_at_bar(p, static_cast<double>(b.bar)) &&
                         b.end_sample == op::sample_at_bar(p, static_cast<double>(b.bar + 1));
        }
    }
    for (const auto& b : rep.master_bars) {
        windows_ok = windows_ok &&
                     b.start_sample == op::sample_at_bar(p, static_cast<double>(b.bar)) &&
                     b.end_sample == op::sample_at_bar(p, static_cast<double>(b.bar + 1));
    }
    CHECK(windows_ok, "meters_bar.windows_equal_op_sample_at_bar_for_every_bar");

    // --- windows tile the render: 0-based bar indices, first window starts at 0, no gaps, and the
    //     last ends exactly at the last rendered frame (partial bar measured to the end).
    bool tile_ok = true;
    for (const auto& t : rep.tracks) {
        for (long long i = 0; tile_ok && i < static_cast<long long>(t.bars.size()); ++i) {
            tile_ok = tile_ok && t.bars[static_cast<size_t>(i)].bar == i &&
                      t.bars[static_cast<size_t>(i)].start_sample ==
                          (i == 0 ? 0 : t.bars[static_cast<size_t>(i) - 1].end_sample) &&
                      t.bars[static_cast<size_t>(i)].end_sample <= rep.frames;
        }
        tile_ok = tile_ok && !t.bars.empty() && t.bars.back().end_sample == rep.frames;
    }
    for (long long i = 0; tile_ok && i < static_cast<long long>(rep.master_bars.size()); ++i) {
        tile_ok = tile_ok && rep.master_bars[static_cast<size_t>(i)].start_sample ==
                                (i == 0 ? 0 : rep.master_bars[static_cast<size_t>(i) - 1].end_sample);
    }
    tile_ok = tile_ok && !rep.master_bars.empty() && rep.master_bars.back().end_sample == rep.frames;
    CHECK(tile_ok, "meters_bar.windows_tile_the_render_with_no_gaps");

    // --- inaudible (muted) track → every bar null/null.
    bool muted_ok = rep.tracks[1].bars.size() == 3;
    for (const auto& b : rep.tracks[1].bars) muted_ok = muted_ok && !b.peak_dbfs && !b.rms_dbfs;
    CHECK(muted_ok, "meters_bar.muted_track_every_bar_two_nulls");

    // --- a track that sounds in one bar only: blip (note t 4.25 dur 3.5 → inside bar 1) is null in
    //     bars 0 and 2 and measured in bar 1.
    const auto& blip = rep.tracks[2].bars;
    CHECK(blip.size() == 3 && !blip[0].peak_dbfs && !blip[0].rms_dbfs && blip[1].peak_dbfs &&
              blip[1].rms_dbfs && !blip[2].peak_dbfs && !blip[2].rms_dbfs,
          "meters_bar.blip_track_audible_in_bar_one_only");

    // --- every track bar equals analyze_samples of that stem's window; the master bars equal the
    //     same measurement on the mix.
    bool bars_eq_analyze = rok && stems.size() == rep.tracks.size();
    for (size_t i = 0; bars_eq_analyze && i < rep.tracks.size(); ++i) {
        for (const auto& b : rep.tracks[i].bars) {
            const op::analyze::Features f =
                op::analyze::analyze_samples(window_of(stems[i], b.start_sample, b.end_sample), 2, 48000);
            bars_eq_analyze = bars_eq_analyze && same_opt(b.peak_dbfs, f.peak_dbfs) &&
                              same_opt(b.rms_dbfs, f.rms_dbfs);
        }
    }
    for (const auto& b : rep.master_bars) {
        const op::analyze::Features f =
            op::analyze::analyze_samples(window_of(mix, b.start_sample, b.end_sample), 2, 48000);
        bars_eq_analyze = bars_eq_analyze && same_opt(b.peak_dbfs, f.peak_dbfs) &&
                          same_opt(b.rms_dbfs, f.rms_dbfs);
    }
    CHECK(bars_eq_analyze, "meters_bar.track_and_master_bars_equal_analyze_of_their_windows");

    // --- to_json byte-exact: the spec's hand-built per-bar example (one track named a"b, bar 1
    //     silent → nulls, rms -0.0 of bar 0 printed as 0.0000).
    op::meters::Report ex;
    ex.sample_rate = 48000; ex.frames = 192000; ex.duration_s = 4.0;
    ex.per_bar = true; ex.bar_count = 2;
    {
        op::meters::TrackMeter t;
        t.name = "a\"b";
        t.meter.peak_dbfs = -6.0206; t.meter.rms_dbfs = -12.0412; t.meter.integrated_lufs = -15.0550;
        op::meters::BarMeter b0;
        b0.bar = 0; b0.start_sample = 0; b0.end_sample = 96000;
        b0.peak_dbfs = -6.0206; b0.rms_dbfs = -0.0;  // exact negative zero → printed "0.0000"
        op::meters::BarMeter b1;
        b1.bar = 1; b1.start_sample = 96000; b1.end_sample = 192000;  // silent bar → two nulls
        t.bars.push_back(b0);
        t.bars.push_back(b1);
        ex.tracks.push_back(t);
    }
    ex.master.peak_dbfs = -6.0206; ex.master.rms_dbfs = -12.0412; ex.master.integrated_lufs = -15.0550;
    ex.master_bars = ex.tracks[0].bars;
    CHECK(op::meters::to_json(ex) == "{\n"
                                     "  \"sample_rate\": 48000,\n"
                                     "  \"frames\": 192000,\n"
                                     "  \"duration_s\": 4.000000,\n"
                                     "  \"bar_count\": 2,\n"
                                     "  \"tracks\": [\n"
                                     "    {\"index\": 0, \"name\": \"a\\\"b\", \"peak_dbfs\": -6.0206, "
                                     "\"rms_dbfs\": -12.0412, \"integrated_lufs\": -15.0550, "
                                     "\"bars\": [{\"bar\": 0, \"start_sample\": 0, \"end_sample\": 96000, "
                                     "\"peak_dbfs\": -6.0206, \"rms_dbfs\": 0.0000}, "
                                     "{\"bar\": 1, \"start_sample\": 96000, \"end_sample\": 192000, "
                                     "\"peak_dbfs\": null, \"rms_dbfs\": null}]}\n"
                                     "  ],\n"
                                     "  \"master\": {\"peak_dbfs\": -6.0206, \"rms_dbfs\": -12.0412, "
                                     "\"integrated_lufs\": -15.0550, "
                                     "\"bars\": [{\"bar\": 0, \"start_sample\": 0, \"end_sample\": 96000, "
                                     "\"peak_dbfs\": -6.0206, \"rms_dbfs\": 0.0000}, "
                                     "{\"bar\": 1, \"start_sample\": 96000, \"end_sample\": 192000, "
                                     "\"peak_dbfs\": null, \"rms_dbfs\": null}]}\n"
                                     "}\n",
          "meters_bar.to_json_per_bar_matches_spec_example_byte_exact");

    // --- the same Report with per_bar false prints no bar keys anywhere ("bars" and bar_count are
    //     absent even though the bar vectors are filled).
    op::meters::Report off = ex;
    off.per_bar = false;
    const std::string off_json = op::meters::to_json(off);
    CHECK(off_json.find("\"bars\"") == std::string::npos && off_json.find("bar_count") == std::string::npos,
          "meters_bar.to_json_has_no_bar_keys_when_per_bar_false");

    // --- an empty bars array prints as "bars": [] (per_bar true, no bars).
    op::meters::Report empty;
    empty.sample_rate = 48000; empty.frames = 0; empty.duration_s = 0.0;
    empty.per_bar = true; empty.bar_count = 0;
    CHECK(op::meters::to_json(empty) == "{\n"
                                        "  \"sample_rate\": 48000,\n"
                                        "  \"frames\": 0,\n"
                                        "  \"duration_s\": 0.000000,\n"
                                        "  \"bar_count\": 0,\n"
                                        "  \"tracks\": [],\n"
                                        "  \"master\": {\"peak_dbfs\": null, \"rms_dbfs\": null, "
                                        "\"integrated_lufs\": null, \"bars\": []}\n"
                                        "}\n",
          "meters_bar.empty_bars_array_prints_as_bars_empty");

    // --- determinism: measuring the same project twice gives byte-identical per-bar JSON.
    op::meters::Report rep2;
    const bool ok2 = op::meters::measure_project_per_bar(p, dir.string(), rep2, err);
    CHECK(ok2 && op::meters::to_json(rep2) == op::meters::to_json(rep),
          "meters_bar.two_measurements_produce_identical_json");

    fs::remove_all(dir, ec);
    std::printf("%s (%d failures)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures);
    return g_failures;
}
