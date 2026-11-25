// src/app/player.hpp — dz::Player: the JUCE-free playback core (WP-0153).
//
// One rendered stereo buffer is handed to the audio callback through a two-slot atomic pointer
// mailbox — `pending_` (control → audio) and `retired_` (audio → control) — plus a plain
// `current_` pointer owned by the audio thread. No invented lock-free structures: each mailbox is
// a single std::atomic pointer exchanged, never raced. The old buffer is always destroyed on the
// control thread (submit / reclaim / ~Player), never inside process().
//
// Threading contract:
//   - submit/reclaim/play/stop/playing/position_frames/frames_played/set_loop/clear_loop/has_loop,
//     prepare_click/has_click/set_click_enabled/click_enabled and
//     prepare_pattern/has_pattern/set_pattern_mode/pattern_mode run on the control thread (the app
//     message thread, or the headless driver in --render-playback) — NEVER the audio thread.
//   - process() runs ONLY on the audio thread (player_rt.cpp: atomics and plain loops, nothing else).
//   - ~Player must run after the audio callback has been removed from the device, so `current_` is
//     quiescent before it is destroyed here.
//
// Usage (headless fake device):
//   dz::Player player;
//   auto buf = std::make_unique<op::wav::StereoBuffer>(rendered);
//   player.submit(std::move(buf));
//   player.play();
//   float left[128], right[128];
//   player.process(left, right, 128);   // would be the device callback
#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "engine/model.hpp"
#include "engine/wav.hpp"

namespace dz {

class Player {
public:
    Player();
    ~Player();
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    // ---- control thread (never the audio thread) ----

    // Hand `buf` to the audio callback. A previously submitted buffer that was never picked up is
    // destroyed here on the control thread. Also collects the buffer process() last retired.
    void submit(std::unique_ptr<op::wav::StereoBuffer> buf);
    // Destroy the buffer process() most recently retired, if any (call from a timer or after stop).
    void reclaim();
    void play();  // already playing: no-op; after stop(): starts from frame 0
    void stop();  // silences immediately and zeroes the position at the start of the next block
    bool playing() const;
    long long position_frames() const;  // 0 while a stop is pending, else the callback's sample counter
    long long frames_played() const;    // cumulative frames copied from a buffer by process()

    // ---- transport loop region (WP-0200) ---------------------------------------------------
    // Absolute frame indices into the buffer being played: the window is [start_frame, end_frame),
    // so frame end_frame itself is NOT part of the loop. The player knows frames only — no bars,
    // no tempo, no project; callers convert bars with op::sample_at_bar (engine/render.hpp).
    //
    // set_loop enables looping iff start_frame >= 0 && end_frame > start_frame; every other pair
    // (equal, reversed, negative start) leaves the player in the disabled state — exactly what
    // clear_loop() produces. Bounds are stored as given: nothing is validated against a buffer at
    // call time, process() clamps them per buffer instead (end_eff = min(end_frame, frames)).
    //
    // While looping, process() fills every slot of every block from the window — switching
    // mid-block, wrapping as many times as a long block needs — and playback never ends: only
    // stop() ends it. clear_loop() neither seeks nor silences: playback runs on from its current
    // position to the end of the buffer, then stops and parks at frame 0. Changing the bounds while
    // playing takes effect no later than the next process() call; the callback snapshots both
    // atomics once per block, so one in-flight block may pair an old start with a new end.
    void set_loop(long long start_frame, long long end_frame);
    void clear_loop();
    bool has_loop() const;  // true iff looping is currently enabled (see the predicate above)

    // ---- metronome click during playback (WP-0201) -------------------------------------------------
    // The project's own click track — the one `opd click` renders (spec §Click track render) — mixed
    // into every copied slot while playing. prepare_click renders OFF-THREAD with op::render_click
    // and stores the result; nothing in dz::Player re-derives a click, so beat→sample mapping and the
    // accent rule are identical to the offline click by construction. Mixing needs BOTH the flag on
    // AND a stored track: prepare_click never enables the click, set_click_enabled never renders or
    // allocates. The Project is NOT kept — later project changes are not tracked; call prepare_click
    // again. A successful prepare replaces any previous track; there is no unload (has_click() has no
    // way back to false). With the flag off the callback output stays bit-identical to pre-WP-0201.
    //
    // Usage:
    //   dz::Player player;
    //   std::string err;
    //   if (!player.prepare_click(project, err)) { /* handle: track and flag unchanged */ }
    //   player.set_click_enabled(true);   // Play now gives a metronome that follows the tempo map
    bool prepare_click(const op::Project& p, std::string& err);  // renders OFF-THREAD and stores the track
    bool has_click() const;                                      // a click track is stored
    void set_click_enabled(bool enabled);                        // the click-enable flag
    bool click_enabled() const;                                  // the flag as last set

    // ---- pattern mode (WP-0229) ------------------------------------------------------------------
    // FL Studio-style "pattern mode": audition ONE named pattern on loop instead of the arrangement.
    // prepare_pattern() runs entirely on the calling (control) thread and renders OFF-THREAD through
    // the engine's own pattern-mode path (op::pattern_mode_project with loops == 1, then
    // op::render_project) — exactly what `opd render --pattern NAME --loops 1` writes — so beat→sample
    // mapping, swing and the explicit pattern length are identical to the offline render by
    // construction. The rendered statement travels through the existing audio mailbox exactly the way
    // submit() hands a buffer over: the pattern render BECOMES the buffer being played, and the buffer
    // previously in flight is destroyed here, on the control thread. Pattern mode needs BOTH the flag
    // on AND a stored window: prepare_pattern never enables the flag, set_pattern_mode never renders,
    // allocates or frees. The Project is NOT kept — later project edits are not tracked; call
    // prepare_pattern again (the last successful call wins: it replaces both buffer and window).
    //
    // The wrap length P is the pattern's own statement length: op::pattern_extent mapped through
    // op::sample_at_beat on the project's tempo map — the frame count of the FIRST statement (later
    // statements are not re-mapped; documented limitation, not a bug). While the flag is on and
    // P > 0, process() wraps [0, min(P, frames)) of the buffer it has adopted via the existing
    // WP-0200 loop path — the loop region is not consulted for those blocks (set_loop/clear_loop/
    // has_loop keep their state and their WP-0200 meaning); P is clamped per buffer inside process(),
    // nothing is validated against a buffer at call time. Turning the flag off mid-playback neither
    // seeks nor silences: playback runs on under the loop region if one is set, otherwise one-shot to
    // the end of the buffer, then stops and parks at frame 0. There is no unload / clear_pattern():
    // has_pattern() has no way back to false; a later submit() replaces the buffer but keeps the
    // window and has_pattern() true. With the flag off (or no pattern ever prepared) song-mode output
    // stays bit-identical to pre-WP-0229.
    //
    // Usage:
    //   dz::Player player;
    //   std::string err;
    //   if (!player.prepare_pattern(project, "A", err)) { /* handle: nothing changed */ }
    //   player.set_pattern_mode(true);   // Play now loops pattern A, not the arrangement
    bool prepare_pattern(const op::Project& p, const std::string& pattern_id, std::string& err);
    bool has_pattern() const;             // a pattern window is stored (no way back to false)
    void set_pattern_mode(bool enabled);  // the pattern-mode flag
    bool pattern_mode() const;            // the flag as last set

    // ---- audio thread (implementation in player_rt.cpp — dumb executor) ----
    void process(float* left, float* right, int num_frames) noexcept;  // right may be nullptr (mono)

private:
    std::atomic<const op::wav::StereoBuffer*> pending_{nullptr};  // control → audio
    std::atomic<const op::wav::StereoBuffer*> retired_{nullptr};  // audio → control
    const op::wav::StereoBuffer* current_ = nullptr;              // owned by the audio thread

    std::atomic<bool> playing_{false};
    std::atomic<bool> reset_requested_{false};
    std::atomic<long long> position_{0};       // written only by process()
    std::atomic<long long> frames_played_{0};  // written only by process()

    // WP-0200 loop window, absolute frames into the buffer being played. Written only by the control
    // thread (set_loop/clear_loop); read once per block by process(). 0/0 = disabled.
    std::atomic<long long> loop_start_{0};
    std::atomic<long long> loop_end_{0};

    // WP-0201 click track. Exactly the single-slot exchange handshake submit()/process() already use
    // for audio: click_pending_ (control → audio), click_retired_ (audio → control), click_current_
    // owned by the audio thread. Every click buffer is destroyed on the control thread only — inside
    // prepare_click or ~Player — never in process(); at most two tracks are alive at a time.
    std::atomic<bool> click_enabled_{false};                      // control write, one load per block
    bool click_stored_ = false;                                   // control thread only (has_click())
    std::atomic<const op::wav::StereoBuffer*> click_pending_{nullptr};   // control → audio
    std::atomic<const op::wav::StereoBuffer*> click_retired_{nullptr};   // audio → control
    const op::wav::StereoBuffer* click_current_ = nullptr;               // owned by the audio thread

    // WP-0229 pattern mode. NO extra buffer mailbox: the rendered pattern travels through the audio
    // mailbox above exactly like submit(), so pattern buffers die on the control thread only. Here
    // live just the flag and the window length: written only by the control thread
    // (prepare_pattern / set_pattern_mode), loaded once per block by process().
    std::atomic<bool> pattern_mode_{false};    // control write, one load per block
    std::atomic<long long> pattern_len_{0};    // frames per pattern statement; 0 = no window
    bool pattern_stored_ = false;              // control thread only (has_pattern())
};

}  // namespace dz
