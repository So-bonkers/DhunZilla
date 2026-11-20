// src/app/player.cpp — dz::Player control-thread side (WP-0153). See player.hpp for the contract.
// The audio-callback half lives in player_rt.cpp.

#include "player.hpp"

namespace dz {

Player::Player() = default;

Player::~Player() {
    // Safe here only because ~Player runs after the audio callback is gone (see header contract):
    // all three mailbox slots are quiescent, so every owned buffer dies on this thread.
    delete pending_.exchange(nullptr);
    delete retired_.exchange(nullptr);
    delete current_;
}

void Player::submit(std::unique_ptr<op::wav::StereoBuffer> buf) {
    // A never-adopted hand-off is destroyed here (control thread), then collect the last retiree.
    delete pending_.exchange(buf.release());
    reclaim();
}

void Player::reclaim() { delete retired_.exchange(nullptr); }

void Player::play() {
    // Already playing: same value, changes nothing. After stop(): position_ is 0 (applied or
    // still requested), so playback restarts from frame 0.
    playing_.store(true);
}

void Player::stop() {
    playing_.store(false);
    reset_requested_.store(true);
}

bool Player::playing() const { return playing_.load(); }

long long Player::position_frames() const {
    if (reset_requested_.load()) return 0;  // a pending stop reads as "at home" immediately
    return position_.load();
}

long long Player::frames_played() const { return frames_played_.load(); }

}  // namespace dz
