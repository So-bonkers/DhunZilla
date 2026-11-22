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

// ---- transport loop region (WP-0200) -----------------------------------------------------------
// Enabling is decided here, once, from the pair itself: a window needs a non-negative start and an
// end strictly past it. Bounds are stored as given — no buffer exists at this point to validate
// against — and process() clamps them per buffer (player_rt.cpp).

void Player::set_loop(long long start_frame, long long end_frame) {
    loop_start_.store(start_frame);
    loop_end_.store(end_frame);
}

void Player::clear_loop() {
    // The disabled state: 0/0 fails the predicate in both has_loop() and process(). No seek, no
    // silence — a running playback simply stops wrapping from here on.
    loop_start_.store(0);
    loop_end_.store(0);
}

bool Player::has_loop() const {
    const long long start = loop_start_.load();
    const long long end = loop_end_.load();
    return start >= 0 && end > start;
}

}  // namespace dz
