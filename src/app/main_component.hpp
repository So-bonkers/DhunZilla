// src/app/main_component.hpp — dhunzilla's top-level component (WP-0143, WP-0153).
//
// Transport bar (top 56 px): Play/Stop TextButtons, a read-only BPM label ("BPM 120.0") and a
// position label showing "bar.beat" (format_position over the Player's frame counter; "1.1" at
// home). Below it: one row per track from Document::track_rows(), in order — name, gain slider
// (-96..24 dB, text box), pan rotary (-1..1, no text box), M and S buttons whose lit colour
// differs when mute/solo is set. Rows are strictly read-only (no mouse clicks reach them); rows
// whose track is not audible per the WP-0136 rule are drawn dimmed. At the very bottom: a status
// line showing the audio device (or the last error) — empty in snapshot mode.
//
// Two flavours (WP-0153): with `playback == nullptr` (headless --snapshot, exactly WP-0143) the
// buttons stay disabled, no timer runs, the status line stays empty and the position reads "1.1".
// With a controller: Play/Stop are wired, and a 30 Hz timer reclaims retired buffers, re-renders
// when the document changed while playing, and updates the position + status labels.
//
// All UI string literals go through juce::String::fromUTF8 / CharPointer_UTF8: raw const char*
// literals are interpreted as Latin-1 by JUCE and would mojibake the em dash U+2014 (ADR-0007 §4).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "doc/document.hpp"

namespace dz {

class PlaybackController;  // playback_controller.hpp (owned by App; must outlive this component)

// "DhunZilla — demo.opj": app name, em dash (U+2014, UTF-8), then the file NAME of `project_path`
// (its last path component; a path ending in '/' falls back to the whole string).
juce::String windowTitleFor(const std::string& project_path);

class MainComponent final : public juce::Component, private juce::Timer {
public:
    static constexpr int kWidth = 1100;   // fixed size for this package (no resize affordance)
    static constexpr int kHeight = 700;
    static constexpr int kTransportHeight = 56;
    static constexpr int kRowHeight = 48;

    // `doc` must outlive this component. The row widgets are built once here — one per track, in
    // Document::track_rows() order — and refresh() only re-reads values from the same Document.
    // `playback == nullptr` keeps the WP-0143 inert behaviour (snapshot mode); with a controller
    // the transport is live: enabled buttons plus the 30 Hz position/status timer.
    explicit MainComponent(const op::Document& doc, PlaybackController* playback = nullptr);
    ~MainComponent() override;  // out-of-line: RowWidgets (unique_ptr target) is defined in the .cpp

    // Re-reads doc.project()/doc.track_rows() and updates every widget value plus row dimming.
    void refresh();

    void paint(juce::Graphics&) override;
    void paintOverChildren(juce::Graphics&) override;  // dims inaudible rows above their widgets
    void resized() override;

private:
    struct RowWidgets;  // defined in the .cpp: name label, gain/pan sliders, M/S buttons

    // 30 Hz (only started with a controller): reclaim retired buffers, re-render after any model
    // change while playing, refresh the bar.beat position and the device/error status line.
    void timerCallback() override;

    const op::Document& doc_;
    PlaybackController* playback_ = nullptr;  // null in snapshot mode (see header note)

    juce::TextButton play_, stop_;
    juce::Label bpm_, position_, status_;
    std::uint64_t requested_revision_ = 0;  // doc.revision() at the last render request
    std::vector<std::unique_ptr<RowWidgets>> rows_;

    // Full-width row rectangles computed in resized(); paintOverChildren() washes the inaudible
    // ones. Sized to match rows_ (empty until the first resized()).
    std::vector<juce::Rectangle<int>> row_rects_;
};

}  // namespace dz
