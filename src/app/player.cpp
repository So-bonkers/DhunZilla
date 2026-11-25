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

// ---- pattern mode (WP-0229) ------------------------------------------------------------------------
// prepare_pattern runs entirely on the calling (control) thread, in the order of §3 rule 3: id lookup,
// one pattern-mode statement (op::pattern_mode_project with loops == 1 — the repetition is the
// transport wrap's job, not the render's), the wrap length from the engine's own beat→sample map, then
// the off-thread render. Until the render succeeds nothing in the player changes, so every failure
// leaves the stored pattern, the current buffer and the flag exactly as they were.

bool Player::prepare_pattern(const op::Project& p, const std::string& pattern_id, std::string& err) {
    // §3 rule 3.1: ids are matched byte-for-byte (spec §Pattern ids); an EMPTY id selects nothing,
    // so unnamed patterns cannot be auditioned. Unknown or empty id: err non-empty and containing
    // pattern_id, nothing else touched.
    std::size_t index = p.patterns.size();
    if (!pattern_id.empty()) {
        for (std::size_t i = 0; i < p.patterns.size(); ++i) {
            if (p.patterns[i].id == pattern_id) {
                index = i;
                break;
            }
        }
    }
    if (index == p.patterns.size()) {
        err = "unknown pattern '" + pattern_id + "'";
        return false;
    }

    // §3 rule 3.2: exactly one statement — loops == 1. On failure err stays exactly as the engine
    // set it (e.g. the spec's "has no length" message); nothing in the player changes.
    op::Project pat_proj;
    if (!op::pattern_mode_project(p, index, 1, pat_proj, err)) return false;

    // §3 rule 3.3: the wrap length P is the pattern's own statement length, taken from the engine's
    // beat→sample map — never re-derived. With a tempo map this is the frame count of the FIRST
    // statement (later statements are not re-mapped; documented limitation, §3 rule 12).
    const long long len = op::sample_at_beat(p, op::pattern_extent(p.patterns[index]));
    if (!(len > 0)) {
        err = "pattern '" + pattern_id + "' has no length in samples";
        return false;
    }

    // §3 rule 3.4: render the statement OFF-THREAD. Empty base dir: sampler-asset resolution is out
    // of scope here. On failure the buffer is destroyed on this thread and err stays exactly as the
    // engine set it.
    op::wav::StereoBuffer* buf = new op::wav::StereoBuffer();
    if (!op::render_project(pat_proj, "", *buf, err)) {
        delete buf;
        return false;
    }

    // §3 rule 3.5: hand the buffer over the way submit() does — the pattern render BECOMES the
    // buffer being played. A never-adopted previous hand-off and the most recent retiree are
    // destroyed here, on the control thread (§3 rule 11); process() never destroys anything. Then
    // store the window and the state: last successful prepare wins (it replaces buffer and window);
    // a later submit() keeps both. The flag is NOT touched — no implicit enable (§3 rule 2).
    delete pending_.exchange(buf);
    reclaim();
    pattern_len_.store(len);
    pattern_stored_ = true;
    err.clear();  // §3 rule 3.5: success reports an empty err
    return true;
}

bool Player::has_pattern() const { return pattern_stored_; }

void Player::set_pattern_mode(bool enabled) { pattern_mode_.store(enabled); }

bool Player::pattern_mode() const { return pattern_mode_.load(); }

}  // namespace dz
