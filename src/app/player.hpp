// src/app/player.hpp — dz::Player: the JUCE-free playback core (WP-0153).
//
// One rendered stereo buffer is handed to the audio callback through a two-slot atomic pointer
// mailbox — `pending_` (control → audio) and `retired_` (audio → control) — plus a plain
// `current_` pointer owned by the audio thread. No invented lock-free structures: each mailbox is
// a single std::atomic pointer exchanged, never raced. The old buffer is always destroyed on the
// control thread (submit / reclaim / ~Player), never inside process().
//
// Threading contract:
//   - submit/reclaim/play/stop/playing/position_frames/frames_played/set_loop/clear_loop/has_loop run
//     on the control thread (the app message thread, or the headless driver in --render-playback) —
//     NEVER the audio thread.
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
};

}  // namespace dz
