// src/app/player.cpp — dz::Player control-thread side (WP-0153). See player.hpp for the contract.
// The audio-callback half lives in player_rt.cpp.

#include "player.hpp"

#include "engine/render.hpp"

namespace dz {

Player::Player() = default;

Player::~Player() {
    // Safe here only because ~Player runs after the audio callback is gone (see header contract):
    // all three mailbox slots are quiescent, so every owned buffer dies on this thread. Same for the
    // WP-0201 click mailboxes: both click slots and the audio-thread's current track are destroyed
    // here, on the control thread — process() never destroys anything (§3 rule 11).
    delete pending_.exchange(nullptr);
    delete retired_.exchange(nullptr);
    delete current_;
    delete click_pending_.exchange(nullptr);
    delete click_retired_.exchange(nullptr);
    delete click_current_;
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

// ---- metronome click during playback (WP-0201) ------------------------------------------------------
// prepare_click runs entirely on the calling (control) thread and touches exactly one engine entry:
// op::render_click — the same code `opd click` runs (spec §Click track render, WP-0148) — so the
// onsets come from the project's own beat→sample mapping and the accent rule is identical by
// construction. The Project is not kept; later changes are tracked only by calling again.

bool Player::prepare_click(const op::Project& p, std::string& err) {
    // Render first: until render_click succeeds nothing in the player changes, so a failure leaves
    // both the stored track and the flag exactly as they were, with err left exactly as the engine
    // set it — no invented or rewritten messages (§3 rule 3).
    auto* track = new op::wav::StereoBuffer();
    if (!op::render_click(p, *track, err)) {
        delete track;
        return false;
    }

    // Success: hand the rendered track to the callback through the click mailbox — exactly the
    // single-slot exchange submit() uses for audio. A previous pending-but-never-adopted track and
    // the most recent retiree are destroyed here, on the control thread (§3 rule 11). The flag is
    // not touched: there is no implicit enable in prepare_click (§3 rule 2).
    delete click_pending_.exchange(track);
    delete click_retired_.exchange(nullptr);
    click_stored_ = true;
    err.clear();  // §3 rule 3: success reports an empty err
    return true;
}

bool Player::has_click() const { return click_stored_; }

void Player::set_click_enabled(bool enabled) { click_enabled_.store(enabled); }

bool Player::click_enabled() const { return click_enabled_.load(); }

}  // namespace dz
