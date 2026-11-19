// src/app/player.hpp — dz::Player: the JUCE-free playback core (WP-0153).
//
// One rendered stereo buffer is handed to the audio callback through a two-slot atomic pointer
// mailbox — `pending_` (control → audio) and `retired_` (audio → control) — plus a plain
// `current_` pointer owned by the audio thread. No invented lock-free structures: each mailbox is
// a single std::atomic pointer exchanged, never raced. The old buffer is always destroyed on the
// control thread (submit / reclaim / ~Player), never inside process().
//
// Threading contract:
//   - submit/reclaim/play/stop/playing/position_frames/frames_played run on the control thread
//     (the app message thread, or the headless driver in --render-playback) — NEVER the audio thread.
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
};

}  // namespace dz
