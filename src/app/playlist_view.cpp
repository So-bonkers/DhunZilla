// src/app/playlist_view.cpp — see playlist_view.hpp for the contract (WP-0196). GUI-free by
// design: this file includes no GUI toolkit; pure computation, no I/O, no clock, no state, and it
// never mutates the op::Project it reads.

#include "playlist_view.hpp"

#include <algorithm>
#include <cmath>

#include "engine/render.hpp"  // op::sample_at_bar — the renderer's own tempo map (bars_at_frame)

int dz::playlist_x(double bar) {
    // NaN fails `bar > 0.0` and lands on the left edge, exactly like a negative bar does.
    if (!(bar > 0.0)) return kPlaylistLeft;
    if (bar * kPlaylistBarWidth >= static_cast<double>(kPlaylistBars * kPlaylistBarWidth))
        return kPlaylistRight;
    return kPlaylistLeft + static_cast<int>(std::llround(bar * kPlaylistBarWidth));
}

double dz::bars_at_frame(const op::Project& p, long long frame) {
    if (frame <= 0) return 0.0;
    if (p.length_bars <= 0.0) return 0.0;
    // Park at the end once the counter reaches the project's last sample.
    if (op::sample_at_bar(p, p.length_bars) <= frame) return p.length_bars;
    // Fixed 60-step bisection on the renderer's own bar -> sample map (tempo map included):
    // `hi` stays the smallest representable bar whose sample is >= frame.
    double lo = 0.0;
    double hi = p.length_bars;
    for (int i = 0; i < 60; ++i) {
        const double mid = 0.5 * (lo + hi);
        if (op::sample_at_bar(p, mid) >= frame) hi = mid;
        else                                    lo = mid;
    }
    return hi;
}

std::vector<dz::PlaylistBlock> dz::playlist_blocks(const op::Project& p) {
    std::vector<PlaylistBlock> blocks;
    blocks.reserve(p.arrangement.size());
    // Every bar<->beat conversion here is the plain product/quotient with bb (exactly
    // op::beats_at_bar), NEVER the tempo map; bb mirrors the engine's own floor of 1.
    const double bb = p.beats_per_bar >= 1 ? static_cast<double>(p.beats_per_bar) : 1.0;

    for (std::size_t i = 0; i < p.arrangement.size(); ++i) {
        const op::Clip& clip = p.arrangement[i];
        PlaylistBlock b;
        b.clip = static_cast<int>(i);
        b.repeats = clip.repeats;  // stays clip.repeats for an invalid pattern index
        b.start_bar = clip.start_bar;
        b.trimmed = clip.length_beats.has_value();
        b.fade_in_beats = clip.fade_in_beats;
        b.fade_out_beats = clip.fade_out_beats;

        const bool pattern_ok = clip.pattern >= 0 &&
                                static_cast<std::size_t>(clip.pattern) < p.patterns.size();
        const op::Pattern* pattern =
            pattern_ok ? &p.patterns[static_cast<std::size_t>(clip.pattern)] : nullptr;
        b.label = (pattern != nullptr && !pattern->id.empty())
                      ? pattern->id
                      : "Pattern " + std::to_string(clip.pattern);

        if (pattern != nullptr) {
            b.lane = pattern->track;  // may be out of range; checked for `drawn` below
            b.repeat_beats = op::pattern_extent(*pattern);
            if (b.repeat_beats <= 0.0) b.repeats = 1;
            b.length_beats = b.trimmed ? *clip.length_beats
                                       : static_cast<double>(b.repeats) * b.repeat_beats;
        }

        b.x0 = playlist_x(b.start_bar);
        const double end_bar = b.start_bar + b.length_beats / bb;
        b.x1 = std::min(std::max(playlist_x(end_bar), b.x0 + kPlaylistBlockMinWidth), kPlaylistRight);

        const bool lane_ok = b.lane >= 0 && static_cast<std::size_t>(b.lane) < p.tracks.size();
        b.drawn = pattern_ok && lane_ok && b.x0 < kPlaylistRight;

        b.label_x = b.x0 + 4;
        b.label_w = std::max(b.x1 - b.x0 - 8, 1);  // computed for skipped blocks too

        // Repeat dividers, in r order, strictly inside (x0, x1): one exactly at x0 or x1 is not
        // drawn, and a trim that cuts the block short simply drops the later dividers.
        if (b.drawn && b.repeat_beats > 0.0) {
            for (int r = 1; r < b.repeats; ++r) {
                const double divider_bar = b.start_bar + static_cast<double>(r) * b.repeat_beats / bb;
                const int xr = playlist_x(divider_bar);
                if (b.x0 < xr && xr < b.x1) b.dividers.push_back(xr);
            }
        }
        // Fade wedges; a fade longer than the block clamps to the block width.
        if (b.drawn && b.fade_in_beats > 0.0) {
            const int raw = playlist_x(b.start_bar + b.fade_in_beats / bb) - b.x0;
            b.fade_in_px = std::max(0, std::min(raw, b.x1 - b.x0));
        }
        if (b.drawn && b.fade_out_beats > 0.0) {
            const int raw = b.x1 - playlist_x(end_bar - b.fade_out_beats / bb);
            b.fade_out_px = std::max(0, std::min(raw, b.x1 - b.x0));
        }

        blocks.push_back(std::move(b));
    }
    return blocks;
}
