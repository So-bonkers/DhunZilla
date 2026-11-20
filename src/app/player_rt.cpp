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
    long long copied = std::min(static_cast<long long>(num_frames), frames - pos);
    if (copied < 0) copied = 0;  // a shorter swapped-in buffer: copy nothing, end below

    const float* data = current_->interleaved.data();
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
