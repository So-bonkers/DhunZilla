// src/app/playback_controller.cpp — see playback_controller.hpp for the contract (WP-0153).

#include "playback_controller.hpp"

#include <filesystem>
#include <utility>

#include "engine/render.hpp"

namespace dz {

PlaybackController::PlaybackController() = default;

PlaybackController::~PlaybackController() {
    // Teardown order matters: async completions check the alive flag, so false first; then drain
    // the pool (waits for a running render); then detach and close audio — JUCE guarantees no
    // callback is still executing once removeAudioCallback returns. Only then may player_ die.
    alive_->store(false);
    pool_.removeAllJobs(true, 10000);
    device_manager_.removeAudioCallback(this);
    device_manager_.closeAudioDevice();
}

bool PlaybackController::openDevice(std::string& err) {
    last_error_.clear();
    device_manager_.initialiseWithDefaultDevices(0, 2);

    // Prefer an output whose name contains "PipeWire" (ADR-0007 default Linux path).
    juce::String target;
    if (auto* type = device_manager_.getCurrentDeviceTypeObject()) {
        for (const auto& name : type->getDeviceNames(false)) {
            if (name.containsIgnoreCase("PipeWire")) {
                target = name;
                break;
            }
        }
    }

    juce::AudioDeviceManager::AudioDeviceSetup setup = device_manager_.getAudioDeviceSetup();
    if (!target.isEmpty()) setup.outputDeviceName = target;
    setup.sampleRate = 48000.0;
    setup.bufferSize = 128;
    if (device_manager_.setAudioDeviceSetup(setup, true).isNotEmpty()) {
        // Preferred settings refused: fall back to whatever the system default gives us.
        device_manager_.initialiseWithDefaultDevices(0, 2);
    }

    juce::AudioIODevice* device = device_manager_.getCurrentAudioDevice();
    if (device == nullptr) {
        err = "no audio device";
        return false;
    }

    device_name_ = device->getName();
    device_manager_.addAudioCallback(this);
    device_open_ = true;
    return true;
}

std::string PlaybackController::deviceDescription() const {
    if (!device_open_) return "no audio device";  // message thread only (UI), same as openDevice
    const auto setup = device_manager_.getAudioDeviceSetup();
    const juce::String rate = juce::String(static_cast<int>(setup.sampleRate));  // rates are integral
    return (device_name_ + " " + rate + " Hz, " + juce::String(setup.bufferSize) + " frames").toStdString();
}

std::string PlaybackController::deviceName() const {
    if (!device_open_) return std::string();
    return device_name_.toStdString();
}

double PlaybackController::deviceSampleRate() const {
    if (!device_open_) return 0.0;
    return device_manager_.getAudioDeviceSetup().sampleRate;
}

int PlaybackController::deviceBufferSize() const {
    if (!device_open_) return 0;
    return device_manager_.getAudioDeviceSetup().bufferSize;
}

void PlaybackController::requestRender(const op::Document& doc) {
    // Copy the model out on this (message) thread: the pool job must never touch Document.
    const op::Project copy = doc.project();
    const std::string base_dir = std::filesystem::path(doc.path()).parent_path().string();
    const auto req = ++request_id_;  // monotonic; only the newest request may submit

    pool_.addJob([this, alive = alive_, copy, base_dir, req] {
        struct Result {
            bool ok = false;
            op::wav::StereoBuffer buf;
            std::string err;
        };
        auto result = std::make_shared<Result>();
        result->ok = op::render_project(copy, base_dir, result->buf, result->err);

        juce::MessageManager::callAsync([this, alive, req, result] {
            if (!alive->load()) return;             // controller destroyed: drop silently
            if (req != request_id_) return;         // a newer render has since been requested
            if (!result->ok) {
                last_error_ = result->err;          // shown by the UI status line until the next try
                return;
            }
            player_.submit(std::make_unique<op::wav::StereoBuffer>(std::move(result->buf)));
        });
    });
}

void PlaybackController::play(const op::Document& doc) {
    player_.play();       // transport starts immediately (silence until the buffer arrives)
    requestRender(doc);
}

void PlaybackController::stop() { player_.stop(); }

// ---- juce::AudioIODeviceCallback: dumb executor (AGENTS.md real-time rules) ----

void PlaybackController::audioDeviceIOCallbackWithContext(const float* const*, int,
                                                          float* const* outputChannelData,
                                                          int numOutputChannels, int numSamples,
                                                          const juce::AudioIODeviceCallbackContext&) {
    if (outputChannelData == nullptr || numOutputChannels < 1) return;
    player_.process(outputChannelData[0], numOutputChannels >= 2 ? outputChannelData[1] : nullptr,
                    numSamples);
    for (int ch = 2; ch < numOutputChannels; ++ch) {
        if (outputChannelData[ch] != nullptr) {
            juce::zeromem(outputChannelData[ch], sizeof(float) * static_cast<size_t>(numSamples));
        }
    }
}

void PlaybackController::audioDeviceAboutToStart(juce::AudioIODevice*) {}
void PlaybackController::audioDeviceStopped() {}

}  // namespace dz
