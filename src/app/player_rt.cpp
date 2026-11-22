// src/app/player_rt.cpp — dz::Player::process: THE AUDIO CALLBACK (WP-0153).
// Dumb executor per AGENTS.md real-time rules: atomics, plain loops and <algorithm> min/max only.

#include "player.hpp"

#include <algorithm>

namespace dz {

void Player::process(float* left, float* right, int num_frames) noexcept {
    // Buffer hand-off at the start of a block: adopt a fresh buffer only when the retire slot is
    // empty; otherwise try again on the next block. Nothing is ever destroyed in here.
    if (retired_.load() == nullptr) {
        const op::wav::StereoBuffer* fresh = pending_.exchange(nullptr);
        if (fresh != nullptr) {
            if (current_ != nullptr) retired_.store(current_);
            current_ = fresh;
        }
    }

    // A stop zeroes the position at the start of a block.
    if (reset_requested_.exchange(false)) position_.store(0);

    if (!playing_.load() || current_ == nullptr) {
        for (int i = 0; i < num_frames; ++i) left[i] = 0.0f;
        if (right != nullptr) {
            for (int i = 0; i < num_frames; ++i) right[i] = 0.0f;
        }
        return;
    }

    const long long frames = static_cast<long long>(current_->frames());
    const long long pos = position_.load();
    const float* data = current_->interleaved.data();

    // WP-0200: snapshot the loop bounds once per block (a control-thread change lands no later than
    // the next call). end_eff keeps every read inside this buffer, so a window reaching past its end
    // behaves exactly like one ending on it; a start outside [0, end_eff) is unusable here.
    const long long loop_start = loop_start_.load();
    const long long end_eff = std::min(loop_end_.load(), frames);

    if (loop_start >= 0 && loop_start < end_eff) {
        // Looping: every slot of the block comes from the buffer, switching mid-block. After frame
        // end_eff - 1 the next read is back at loop_start — no gap, no overlap, no silence — so a
        // block longer than the window wraps more than once. Playback never ends in here; only
        // stop() ends it. Integer modulo only: no helper that could allocate.
        const long long len = end_eff - loop_start;
        long long p = pos;
        for (int i = 0; i < num_frames; ++i) {
            if (p >= end_eff) p = loop_start + (p - loop_start) % len;
            const long long src = 2 * p;
            left[i] = data[src];
            if (right != nullptr) right[i] = data[src + 1];
            ++p;
        }
        // A block ending exactly on end_eff reports its next frame as loop_start, not end_eff.
        if (p >= end_eff) p = loop_start + (p - loop_start) % len;
        position_.store(p);
        frames_played_.store(frames_played_.load() + num_frames);  // every slot was copied
        return;
    }

    // No usable window (no loop, or a start this buffer cannot serve): WP-0153 one-shot path —
    // copy up to the end of the buffer, zero-fill the rest of the block, then park at home.
    long long copied = std::min(static_cast<long long>(num_frames), frames - pos);
    if (copied < 0) copied = 0;  // a shorter swapped-in buffer: copy nothing, end below

    for (long long i = 0; i < copied; ++i) {
        const long long src = 2 * (pos + i);
        left[i] = data[src];
        if (right != nullptr) right[i] = data[src + 1];
    }
    for (long long i = copied; i < num_frames; ++i) {
        left[i] = 0.0f;
        if (right != nullptr) right[i] = 0.0f;
    }

    position_.store(pos + copied);
    frames_played_.store(frames_played_.load() + copied);

    // End of song: stop and park at home.
    if (pos + copied >= frames) {
        playing_.store(false);
        position_.store(0);
    }
}

}  // namespace dz
