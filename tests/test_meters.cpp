// Unit tests for op::meters (WP-0194): per-track and master peak/RMS dBFS + integrated LUFS measured
// from the in-memory stems/mix of op::render_stems with op::analyze::analyze_samples. Boring
// assert-based runner — no framework deps, own main. Exit code = number of failures.
// Run: ./build/op_meters_tests
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

static bool near_(double got, double want, double tol) { return got >= want - tol && got <= want + tol; }

// Two bars @ 120 bpm 4/4 = 4 s of audio: an audible sine track, a muted sine track (all-zero stem →
// nulls) and a quieter third track, so track order and names are distinguishable. master_gain_db -6.0
// lets the gain check verify the master really carries it.
static const char* kProjectJson = R"JSON({
  "format_version": 0,
  "bpm": 120.0,
  "sample_rate": 48000,
  "seed": 77,
  "length_bars": 2.0,
  "beats_per_bar": 4,
  "master_gain_db": -6.0,
  "tracks": [
    {"name": "lead", "voice": {"kind": "sine", "freq_hz": 440.0}},
    {"name": "muted bass", "mute": true, "voice": {"kind": "sine", "freq_hz": 55.0}},
    {"name": "blip", "gain_db": -12.0, "voice": {"kind": "sine", "freq_hz": 880.0}}
  ],
  "patterns": [
    {"track": 0, "notes": [{"t": 0.0, "dur": 8.0}]},
    {"track": 1, "notes": [{"t": 0.0, "dur": 8.0}]},
    {"track": 2, "notes": [{"t": 0.0, "dur": 8.0}]}
  ]
}
)JSON";

int main() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() /
        ("op_meters_tests_wp0194_" + std::to_string(static_cast<long>(::getpid())));  // per-process, no collisions
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    if (ec) {
        std::printf("FATAL: cannot create temp dir %s\n", dir.string().c_str());
        return 2;
    }

    const fs::path opj = dir / "meters.opj";
    {
        std::ofstream f(opj, std::ios::binary);
        f << kProjectJson;
    }

    op::Project p;
    std::string err;
    CHECK(op::load_project(opj.string(), p, err), "meters.test_project_json_loads");

    op::meters::Report rep;
    const bool ok = op::meters::measure_project(p, dir.string(), rep, err);
    CHECK(ok, "meters.measure_project_succeeds");

    // --- report header: rate / frames (2 bars @ 120 bpm 4/4 @ 48 kHz) / duration.
    CHECK(rep.sample_rate == 48000 && rep.frames == 192000 && near_(rep.duration_s, 4.0, 1e-12),
          "meters.header_rate_frames_duration");

    // --- one entry per track, in project order, names exactly as stored.
    bool names_ok = rep.tracks.size() == p.tracks.size() && p.tracks.size() == 3;
    for (size_t i = 0; i < rep.tracks.size() && i < p.tracks.size(); ++i)
        names_ok = names_ok && rep.tracks[i].name == p.tracks[i].name;
    CHECK(names_ok && rep.tracks.size() == 3 && rep.tracks[0].name == "lead" &&
              rep.tracks[1].name == "muted bass" && rep.tracks[2].name == "blip",
          "meters.track_order_and_names");

    // --- muted track → all-zero stem → three nulls.
    const op::meters::Meter& muted = rep.tracks[1].meter;
    CHECK(!muted.peak_dbfs && !muted.rms_dbfs && !muted.integrated_lufs, "meters.muted_track_three_nulls");

    // --- audible tracks are measured (all three values present).
    CHECK(rep.tracks[0].meter.peak_dbfs && rep.tracks[0].meter.rms_dbfs && rep.tracks[0].meter.integrated_lufs &&
              rep.tracks[2].meter.peak_dbfs && rep.tracks[2].meter.rms_dbfs && rep.tracks[2].meter.integrated_lufs,
          "meters.audible_tracks_fully_measured");

    // --- reference render: every stem has full length and its meter equals analyze_samples of that stem.
    std::vector<op::wav::StereoBuffer> stems;
    op::wav::StereoBuffer mix;
    const bool rok = op::render_stems(p, dir.string(), stems, mix, err);
    CHECK(rok, "meters.reference_render_stems_succeeds");
    bool per_track_ok = rok && stems.size() == rep.tracks.size();
    for (size_t i = 0; per_track_ok && i < rep.tracks.size(); ++i) {
        const op::analyze::Features f = op::analyze::analyze_samples(stems[i].interleaved, 2, mix.sample_rate);
        per_track_ok = stems[i].frames() == static_cast<size_t>(rep.frames) &&
                       rep.tracks[i].meter.peak_dbfs == f.peak_dbfs &&   // same optionals, unchanged (spec)
                       rep.tracks[i].meter.rms_dbfs == f.rms_dbfs &&
                       rep.tracks[i].meter.integrated_lufs == f.integrated_lufs;
    }
    CHECK(per_track_ok, "meters.track_meter_equals_analyze_samples_of_stem");

    // --- master is the meter of the mix exactly as render_stems returns it.
    const op::analyze::Features fm = op::analyze::analyze_samples(mix.interleaved, 2, mix.sample_rate);
    CHECK(rep.master.peak_dbfs && fm.peak_dbfs && rep.master.peak_dbfs == fm.peak_dbfs &&
              rep.master.rms_dbfs == fm.rms_dbfs && rep.master.integrated_lufs == fm.integrated_lufs,
          "meters.master_equals_meter_of_mix");

    // --- master_gain_db is applied to the master: with the fader at 0 dB the peak rises by exactly 6 dB.
    op::Project p0 = p;
    p0.master_gain_db = 0.0f;
    op::meters::Report rep0;
    const bool ok0 = op::meters::measure_project(p0, dir.string(), rep0, err);
    CHECK(ok0 && rep.master.peak_dbfs && rep0.master.peak_dbfs &&
              near_(*rep0.master.peak_dbfs - *rep.master.peak_dbfs, 6.0, 0.001),
          "meters.master_gain_db_applied_to_master");

    // --- to_json byte-exact: the spec's two-track example (second inaudible → nulls).
    op::meters::Report ex;
    ex.sample_rate = 48000; ex.frames = 192000; ex.duration_s = 4.0;
    {
        op::meters::TrackMeter t;
        t.name = "kick";
        t.meter.peak_dbfs = -6.0206; t.meter.rms_dbfs = -12.3456; t.meter.integrated_lufs = -14.0;
        ex.tracks.push_back(t);
    }
    {
        op::meters::TrackMeter t;
        t.name = "pad";  // meter left empty → nulls
        ex.tracks.push_back(t);
    }
    ex.master.peak_dbfs = -6.0206; ex.master.rms_dbfs = -12.3456; ex.master.integrated_lufs = -14.0;
    CHECK(op::meters::to_json(ex) == "{\n"
                                     "  \"sample_rate\": 48000,\n"
                                     "  \"frames\": 192000,\n"
                                     "  \"duration_s\": 4.000000,\n"
                                     "  \"tracks\": [\n"
                                     "    {\"index\": 0, \"name\": \"kick\", \"peak_dbfs\": -6.0206, "
                                     "\"rms_dbfs\": -12.3456, \"integrated_lufs\": -14.0000},\n"
                                     "    {\"index\": 1, \"name\": \"pad\", \"peak_dbfs\": null, "
                                     "\"rms_dbfs\": null, \"integrated_lufs\": null}\n"
                                     "  ],\n"
                                     "  \"master\": {\"peak_dbfs\": -6.0206, \"rms_dbfs\": -12.3456, "
                                     "\"integrated_lufs\": -14.0000}\n"
                                     "}\n",
          "meters.to_json_matches_spec_example_byte_exact");

    // --- to_json: "-0.0000" → "0.0000" (also for the exact negative zero), nulls, and name escaping
    // ('"' \" , '\' \\ , byte 0x01 → \u0001 lowercase hex, UTF-8 é verbatim).
    op::meters::Report nz;
    nz.sample_rate = 8000; nz.frames = 1; nz.duration_s = 0.000125;
    {
        op::meters::TrackMeter t;
        t.name = std::string("a\"b\\c") + '\x01' + std::string("\xc3\xa9");
        t.meter.peak_dbfs = -0.00004;  // %.4f would print "-0.0000" → spec says "0.0000"
        t.meter.rms_dbfs.reset();      // null
        t.meter.integrated_lufs = 3.14159;  // → "3.1416"
        nz.tracks.push_back(t);
    }
    nz.master.peak_dbfs = -0.0;  // exact negative zero → "0.0000" as well
    CHECK(op::meters::to_json(nz) == "{\n"
                                     "  \"sample_rate\": 8000,\n"
                                     "  \"frames\": 1,\n"
                                     "  \"duration_s\": 0.000125,\n"
                                     "  \"tracks\": [\n"
                                     "    {\"index\": 0, \"name\": \"a\\\"b\\\\c\\u0001" "\xc3\xa9" "\", "
                                     "\"peak_dbfs\": 0.0000, \"rms_dbfs\": null, \"integrated_lufs\": 3.1416}\n"
                                     "  ],\n"
                                     "  \"master\": {\"peak_dbfs\": 0.0000, \"rms_dbfs\": null, "
                                     "\"integrated_lufs\": null}\n"
                                     "}\n",
          "meters.to_json_negzero_nulls_and_escaped_name_byte_exact");

    // --- zero tracks: the tracks line is exactly `  "tracks": [],`.
    op::meters::Report et;
    et.sample_rate = 48000; et.frames = 0; et.duration_s = 0.0;
    CHECK(op::meters::to_json(et) == "{\n"
                                     "  \"sample_rate\": 48000,\n"
                                     "  \"frames\": 0,\n"
                                     "  \"duration_s\": 0.000000,\n"
                                     "  \"tracks\": [],\n"
                                     "  \"master\": {\"peak_dbfs\": null, \"rms_dbfs\": null, "
                                     "\"integrated_lufs\": null}\n"
                                     "}\n",
          "meters.empty_tracks_json_is_single_empty_array_line");

    // --- meter_buffer: zero-frame buffer → three nulls; all-zero (silent) buffer → three nulls too.
    op::wav::StereoBuffer zbuf;
    zbuf.sample_rate = 48000;
    const op::meters::Meter zm = op::meters::meter_buffer(zbuf);
    CHECK(!zm.peak_dbfs && !zm.rms_dbfs && !zm.integrated_lufs, "meters.zero_frame_buffer_all_null");
    op::wav::StereoBuffer sil;
    sil.sample_rate = 48000;
    sil.interleaved.assign(static_cast<size_t>(48000) * 2u, 0.0f);
    const op::meters::Meter sm = op::meters::meter_buffer(sil);
    CHECK(!sm.peak_dbfs && !sm.rms_dbfs && !sm.integrated_lufs, "meters.all_zero_buffer_all_null");

    // --- determinism: measuring the same project twice gives byte-identical JSON.
    op::meters::Report rep2;
    const bool ok2 = op::meters::measure_project(p, dir.string(), rep2, err);
    CHECK(ok2 && op::meters::to_json(rep2) == op::meters::to_json(rep),
          "meters.two_measurements_produce_identical_json");

    fs::remove_all(dir, ec);
    std::printf("%s (%d failures)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures);
    return g_failures;
}
