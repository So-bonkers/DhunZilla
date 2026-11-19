// src/app/playback_controller.hpp — dz::PlaybackController: the JUCE glue around dz::Player
// (WP-0153). Owns the Player, a juce::AudioDeviceManager and a one-thread juce::ThreadPool that
// runs op::render_project off the message thread; the finished buffer goes to the Player through
// its atomic mailbox. The audio callback is a dumb executor: it only calls player.process().
//
// Threading: requestRender/play/stop/lastError/deviceDescription run on the message thread.
// ~PlaybackController tears everything down in order — stop accepting work, drain the pool,
// remove the audio callback, close the device — so the Player is dead-quiescent when it dies.
// Example (main.cpp window mode):
//   dz::PlaybackController controller;
//   std::string err;
//   if (!controller.openDevice(err)) report(err);  // "no audio device" — app still runs
//   doc.apply(...);                                 // later, from the UI:
//   controller.play(doc);                           // start + kick a background render
#pragma once

#include <atomic>
#include <memory>
#include <string>

#include <juce_audio_devices/juce_audio_devices.h>

#include "doc/document.hpp"
#include "player.hpp"

namespace dz {

class PlaybackController final : private juce::AudioIODeviceCallback {
public:
    PlaybackController();
    ~PlaybackController() override;
    PlaybackController(const PlaybackController&) = delete;
    PlaybackController& operator=(const PlaybackController&) = delete;

    // Open an output device: initialiseWithDefaultDevices(0, 2), then request 48 kHz / 128 frames
    // via setAudioDeviceSetup, preferring an output whose name contains "PipeWire" (ADR-0007).
    // On any error falls back to the default device settings; returns false only if no output
    // device opens at all (`err` set). Registers our callback when it succeeds.
    bool openDevice(std::string& err);

    // "<device name> <rate> Hz, <buffer> frames" with the ACTUAL applied values, or "no audio device".
    std::string deviceDescription() const;

    // Message thread: copy doc.project(), queue a pool render (base_dir = parent dir of
    // doc.path()), and on completion — if still the latest request — submit() the buffer to the
    // Player. Stale results are dropped; a render failure lands in lastError().
    void requestRender(const op::Document& doc);

    // Play = start transport immediately, then (re)render so real audio arrives when ready.
    void play(const op::Document& doc);
    void stop();

    dz::Player& player() noexcept { return player_; }
    const std::string& lastError() const { return last_error_; }

    // Actual applied device values for the --play-seconds report line (valid after openDevice).
    std::string deviceName() const;
    double deviceSampleRate() const;
    int deviceBufferSize() const;

private:
    // ---- juce::AudioIODeviceCallback: dumb executor, no allocation/locks/IO ----
    void audioDeviceIOCallbackWithContext(const float* const* inputChannelData, int numInputChannels,
                                          float* const* outputChannelData, int numOutputChannels,
                                          int numSamples,
                                          const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

    dz::Player player_;  // declared first → destroyed last (see destructor teardown order)

    juce::AudioDeviceManager device_manager_;
    juce::ThreadPool pool_{1};

    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    std::uint64_t request_id_ = 0;      // message thread only: newest render request wins
    bool device_open_ = false;          // message thread: our callback is attached to an open device
    juce::String device_name_;          // cached display name of the output device
    std::string last_error_;            // message thread: last background render failure text
};

}  // namespace dz
