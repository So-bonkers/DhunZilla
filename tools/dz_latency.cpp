// tools/dz_latency.cpp — WP-0190: dz_latency, the Phase 0 latency / xrun REPORT for the JUCE-free
// dz::Player core. It renders a project offline exactly like `opd render` without options, hands the
// result to dz::Player, drives process() from simulated device callbacks of --block frames, times every
// callback against the block budget (default block * 1000 / sample_rate ms), and prints one JSON report.
// It is a REPORT, not a CI gate: overruns never change the exit code. Wall-clock timing lives ONLY here,
// never in engine/model/player code (ADR-0004). See tools/dz_latency.md for the full contract.
//
// Usage:
//   build/opd render tests/fixtures/goldens/arranged.opj -o /tmp/ref.wav
//   build/dz_latency tests/fixtures/goldens/arranged.opj --reference /tmp/ref.wav
// Exit codes: 0 ok, 1 mismatch, 2 usage, 3 input, 5 render.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>  // getpid()

#include <nlohmann/json.hpp>

#include "engine/model.hpp"
#include "engine/render.hpp"
#include "engine/wav.hpp"
#include "player.hpp"

namespace {

const char* kUsage = "usage: dz_latency PROJECT.opj [--block N] [--budget-ms F] [--reference REF.wav]";

// Usage problem: stderr "error: <reason>" then the usage line, exit 2 (nothing ever goes to stdout here).
int usage_error(const std::string& reason) {
    std::fprintf(stderr, "error: %s\n%s\n", reason.c_str(), kUsage);
    return 2;
}

// --block N: decimal digits only ("+8", "1e3", "8.0", "-1", "abc", "128abc", "" are all errors),
// value in [1, 16384]. Leading zeros are fine ("0128" is 128).
bool parse_block(const std::string& s, int& out) {
    if (s.empty()) return false;
    for (const char c : s) {
        if (c < '0' || c > '9') return false;
    }
    const unsigned long long v = std::strtoull(s.c_str(), nullptr, 10);  // overflow -> ULLONG_MAX, rejected below
    if (v < 1 || v > 16384) return false;
    out = static_cast<int>(v);
    return true;
}

// --budget-ms F: fully consumed by std::strtod ("128abc" is an error, "1e3" is fine), finite
// ("nan"/"inf" are errors) and > 0.
bool parse_budget(const std::string& s, double& out) {
    if (s.empty()) return false;
    const char* begin = s.c_str();
    char* end = nullptr;
    const double v = std::strtod(begin, &end);
    if (end != begin + s.size()) return false;  // trailing junk
    if (!std::isfinite(v)) return false;
    if (!(v > 0.0)) return false;
    out = v;
    return true;
}

// Slurp the whole stream into bytes.
std::vector<char> read_all(std::istream& in) {
    return std::vector<char>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

int main(int argc, char** argv) {
    // ---- 1. arguments: exactly one positional PROJECT; options in any order; last occurrence wins ----
    std::string project;
    bool have_project = false;
    std::string block_arg, budget_arg, reference;
    bool has_block = false, has_budget = false, has_reference = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--block" || a == "--budget-ms" || a == "--reference") {
            if (i + 1 >= argc) return usage_error("missing value for " + a);
            const std::string v = argv[++i];
            if (a == "--block") { block_arg = v; has_block = true; }
            else if (a == "--budget-ms") { budget_arg = v; has_budget = true; }
            else { reference = v; has_reference = true; }
        } else if (a.rfind("--", 0) == 0) {
            return usage_error("unknown option '" + a + "'");
        } else if (have_project) {
            return usage_error("unexpected extra argument '" + a + "'");
        } else {
            project = a;
            have_project = true;
        }
    }
    if (!have_project) return usage_error("missing PROJECT argument");

    int block = 128;
    if (has_block && !parse_block(block_arg, block)) return usage_error("bad --block value '" + block_arg + "'");
    double budget_override = 0.0;
    if (has_budget && !parse_budget(budget_arg, budget_override))
        return usage_error("bad --budget-ms value '" + budget_arg + "'");

    // ---- 2. input: project must exist and load ----
    std::string err;
    if (!std::filesystem::exists(project)) {
        std::fprintf(stderr, "error: project not found: %s\n", project.c_str());
        return 3;
    }
    op::Project p;
    if (!op::load_project(project, p, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 3;
    }

    // ---- 3. offline render, exactly like `opd render` without options ----
    auto rendered = std::make_unique<op::wav::StereoBuffer>();
    const std::string dir = std::filesystem::path(project).parent_path().string();
    if (!op::render_project(p, dir, *rendered, err)) {
        std::fprintf(stderr, "error: render failed: %s\n", err.c_str());
        return 5;
    }
    if (rendered->frames() == 0) {
        std::fprintf(stderr, "error: render failed: project rendered zero frames\n");
        return 5;
    }

    // ---- 4. simulated device callbacks through dz::Player::process, timed per block ----
    const std::vector<float> expected = rendered->interleaved;
    const int sample_rate = rendered->sample_rate;
    const long long total_frames = static_cast<long long>(rendered->frames());

    dz::Player player;
    player.submit(std::move(rendered));  // submit takes ownership of the unique_ptr
    player.play();

    std::vector<float> left(static_cast<size_t>(block), 0.0f), right(static_cast<size_t>(block), 0.0f);
    std::vector<float> produced;
    produced.reserve(expected.size());
    const long long blocks = (total_frames + block - 1) / block;  // ceil(total_frames / block)
    std::vector<double> times;
    times.reserve(static_cast<size_t>(blocks));
    for (long long b = 0; b < blocks; ++b) {
        const auto t0 = std::chrono::steady_clock::now();
        player.process(left.data(), right.data(), block);  // the only timed work
        const auto t1 = std::chrono::steady_clock::now();
        times.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        // Copying out happens outside the timed region; the tail of the last block is dropped.
        const long long kept = total_frames - static_cast<long long>(produced.size() / 2);
        const int take = static_cast<int>(std::min(static_cast<long long>(block), kept));
        for (int i = 0; i < take; ++i) {
            produced.push_back(left[i]);
            produced.push_back(right[i]);
        }
    }

    // ---- 5. determinism: callback audio must equal the offline render, bit for bit ----
    const bool output_identical = (produced == expected);

    // ---- 6. optional byte comparison of the callback audio against an `opd render` WAV ----
    bool reference_match = false;
    if (has_reference) {
        std::ifstream ref_in(reference, std::ios::binary);
        if (!ref_in.is_open()) {
            std::fprintf(stderr, "error: cannot read reference: %s\n", reference.c_str());
            return 3;
        }
        const auto tmp = std::filesystem::temp_directory_path() /
                         ("dz_latency_" + std::to_string(static_cast<long long>(getpid())) + ".wav");
        op::wav::StereoBuffer out_buf;
        out_buf.sample_rate = sample_rate;
        out_buf.interleaved = produced;
        if (!op::wav::write_pcm16_stereo(tmp.string(), out_buf, err)) {
            std::error_code ec;
            std::filesystem::remove(tmp, ec);  // never leave a half-written temp behind
            std::fprintf(stderr, "error: render failed: %s\n", err.c_str());
            return 5;
        }
        std::ifstream tmp_in(tmp, std::ios::binary);
        const std::vector<char> produced_bytes = read_all(tmp_in);
        const std::vector<char> reference_bytes = read_all(ref_in);
        reference_match = (produced_bytes == reference_bytes);
        tmp_in.close();
        ref_in.close();
        std::error_code ec;
        std::filesystem::remove(tmp, ec);  // removed on every path
    }

    // ---- 7. statistics over the per-block times ----
    const double budget_ms = has_budget ? budget_override : block * 1000.0 / sample_rate;
    double max_ms = 0.0;
    long long overruns = 0;
    for (const double t : times) {
        if (t > max_ms) max_ms = t;
        if (t > budget_ms) ++overruns;  // strictly greater
    }
    std::vector<double> sorted = times;
    std::sort(sorted.begin(), sorted.end());
    const auto nearest_rank = [&sorted](double q) {
        long long idx = static_cast<long long>(std::ceil(q * static_cast<double>(sorted.size()))) - 1;
        if (idx < 0) idx = 0;
        if (idx >= static_cast<long long>(sorted.size())) idx = static_cast<long long>(sorted.size()) - 1;
        return sorted[static_cast<size_t>(idx)];
    };
    const double p99_ms = nearest_rank(0.99);
    const double p50_ms = nearest_rank(0.50);

    // ---- 8. the report (byte-exact layout: 2-space indent, fixed key order, ends with "}\n") ----
    std::printf("{\n");
    std::printf("  \"project\": %s,\n", nlohmann::json(project).dump().c_str());
    std::printf("  \"sample_rate\": %d,\n", sample_rate);
    std::printf("  \"block_frames\": %d,\n", block);
    std::printf("  \"blocks\": %lld,\n", blocks);
    std::printf("  \"total_frames\": %lld,\n", total_frames);
    std::printf("  \"budget_ms\": %.6f,\n", budget_ms);
    std::printf("  \"max_ms\": %.6f,\n", max_ms);
    std::printf("  \"p99_ms\": %.6f,\n", p99_ms);
    std::printf("  \"p50_ms\": %.6f,\n", p50_ms);
    std::printf("  \"overruns\": %lld,\n", overruns);
    std::printf("  \"output_identical\": %s,\n", output_identical ? "true" : "false");
    if (has_reference) std::printf("  \"reference_match\": %s\n", reference_match ? "true" : "false");
    else std::printf("  \"reference_match\": null\n");
    std::printf("}\n");

    // Overruns NEVER affect the exit code.
    return (output_identical && (!has_reference || reference_match)) ? 0 : 1;
}
