// src/app/render_playback.cpp — see render_playback.hpp for the contract (WP-0153).

#include "render_playback.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <vector>

#include "engine/render.hpp"
#include "player.hpp"

namespace dz {

std::string format_position(const op::Project& p, long long frame) {
    const long long total = op::total_samples_for(p);
    if (total <= 0) return "1.1";  // empty project: only the home position exists

    long long f = frame;
    if (f < 0) f = 0;
    if (f > total - 1) f = total - 1;

    // b = largest integer in [0, ceil(length_bars) - 1] with sample_at_bar(p, b) <= f.
    // sample_at_bar is non-decreasing in bar, so a plain binary search finds it (inclusive bounds).
    long long high = static_cast<long long>(std::ceil(p.length_bars)) - 1;
    if (high < 0) high = 0;
    long long lo = 0, best = 0;
    while (lo <= high) {
        const long long mid = lo + (high - lo) / 2;
        if (op::sample_at_bar(p, static_cast<double>(mid)) <= f) {
            best = mid;
            lo = mid + 1;
        } else {
            high = mid - 1;
        }
    }

    // k = largest integer in [0, beats_per_bar - 1] with sample_at_bar(p, b + k/beats) <= f.
    const int beats = p.beats_per_bar >= 1 ? p.beats_per_bar : 1;
    int k = 0;
    for (int cand = beats - 1; cand >= 0; --cand) {
        const double bar = static_cast<double>(best) + static_cast<double>(cand) / static_cast<double>(beats);
        if (op::sample_at_bar(p, bar) <= f) {
            k = cand;
            break;
        }
    }

    return std::to_string(best + 1) + "." + std::to_string(k + 1);
}

int run_render_playback(const std::string& project_path, const std::string& out_path, int block,
                        long long stop_after, std::string& err) {
    namespace fs = std::filesystem;
    if (!fs::exists(project_path)) {
        err = "project not found: " + project_path;
        return 3;
    }

    op::Project p;
    if (!op::load_project(project_path, p, err)) return 3;  // err comes from the loader

    // base_dir resolves sampler paths — exactly like `opd render` (src/cli/main.cpp).
    const std::string base_dir = fs::path(project_path).parent_path().string();
    auto rendered = std::make_unique<op::wav::StereoBuffer>();
    if (!op::render_project(p, base_dir, *rendered, err)) return 5;

    const long long total = static_cast<long long>(rendered->frames());
    const int rate = rendered->sample_rate;

    Player player;
    player.submit(std::move(rendered));
    player.play();

    // Fake device: fixed-size block arrays fed through the same Player::process the real audio
    // callback uses. Exactly `want` frames are kept — any tail of the last block is dropped.
    op::wav::StereoBuffer out;
    out.sample_rate = rate;
    std::vector<float> left(static_cast<size_t>(block)), right(static_cast<size_t>(block));
    long long blocks = 0;

    auto run_blocks = [&](long long want) {
        long long produced = 0;
        while (produced < want) {
            player.process(left.data(), right.data(), block);
            ++blocks;
            const long long take = std::min(static_cast<long long>(block), want - produced);
            for (long long i = 0; i < take; ++i) {
                out.interleaved.push_back(left[static_cast<size_t>(i)]);
                out.interleaved.push_back(right[static_cast<size_t>(i)]);
            }
            produced += block;
        }
    };

    long long seg = 0, before = 0, after_stop = 0;
    bool silent_after_stop = false;
    if (stop_after >= 1) {
        seg = std::min(total, ((stop_after + block - 1) / block) * static_cast<long long>(block));
        run_blocks(seg);

        before = player.position_frames();
        player.stop();

        // One more block, not written: must come out silent with the transport parked at home.
        std::fill(left.begin(), left.end(), 1.0f);
        std::fill(right.begin(), right.end(), 1.0f);
        player.process(left.data(), right.data(), block);
        ++blocks;
        bool all_zero = true;
        for (int i = 0; i < block; ++i) {
            if (left[static_cast<size_t>(i)] != 0.0f || right[static_cast<size_t>(i)] != 0.0f) {
                all_zero = false;
                break;
            }
        }
        after_stop = player.position_frames();
        silent_after_stop = all_zero && after_stop == 0;

        // Restart: the whole render again from frame 0, appended after the segment.
        player.play();
        run_blocks(total);
    } else {
        run_blocks(total);
    }

    if (!op::wav::write_pcm16_stereo(out_path, out, err)) return 5;

    std::printf("render-playback: %lld frames @%d Hz block=%d blocks=%lld -> %s\n",
                static_cast<long long>(out.frames()), rate, block, blocks, out_path.c_str());
    if (stop_after >= 1) {
        std::printf("stop-after: segment=%lld frames position_before_stop=%lld position_after_stop=%lld "
                    "silent_after_stop=%d\n",
                    seg, before, after_stop, silent_after_stop ? 1 : 0);
    }
    return 0;
}

}  // namespace dz
