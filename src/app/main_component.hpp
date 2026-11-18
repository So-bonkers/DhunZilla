// src/app/main_component.hpp — dhunzilla's top-level component (WP-0143).
//
// Transport bar (top 56 px): Play/Stop TextButtons that are inert for now (no playback yet —
// later package), a read-only BPM label ("BPM 120.0") and a position label showing exactly
// "1.1" (bar.beat; the transport stays at home). Below it: one row per track from
// Document::track_rows(), in order — name, gain slider (-96..24 dB, text box), pan rotary
// (-1..1, no text box), M and S buttons whose lit colour differs when mute/solo is set. Rows are
// strictly read-only (no mouse clicks reach them); rows whose track is not audible per the
// WP-0136 rule are drawn dimmed.
//
// All UI string literals go through juce::String::fromUTF8 / CharPointer_UTF8: raw const char*
// literals are interpreted as Latin-1 by JUCE and would mojibake the em dash U+2014 (ADR-0007 §4).
#pragma once

#include <memory>
#include <string>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "doc/document.hpp"

namespace dz {

// "DhunZilla — demo.opj": app name, em dash (U+2014, UTF-8), then the file NAME of `project_path`
// (its last path component; a path ending in '/' falls back to the whole string).
juce::String windowTitleFor(const std::string& project_path);

class MainComponent final : public juce::Component {
public:
    static constexpr int kWidth = 1100;   // fixed size for this package (no resize affordance)
    static constexpr int kHeight = 700;
    static constexpr int kTransportHeight = 56;
    static constexpr int kRowHeight = 48;

    // `doc` must outlive this component. The row widgets are built once here — one per track, in
    // Document::track_rows() order — and refresh() only re-reads values from the same Document.
    explicit MainComponent(const op::Document& doc);
    ~MainComponent() override;  // out-of-line: RowWidgets (unique_ptr target) is defined in the .cpp

    // Re-reads doc.project()/doc.track_rows() and updates every widget value plus row dimming.
    void refresh();

    void paint(juce::Graphics&) override;
    void paintOverChildren(juce::Graphics&) override;  // dims inaudible rows above their widgets
    void resized() override;

private:
    struct RowWidgets;  // defined in the .cpp: name label, gain/pan sliders, M/S buttons

    const op::Document& doc_;

    juce::TextButton play_, stop_;
    juce::Label bpm_, position_;
    std::vector<std::unique_ptr<RowWidgets>> rows_;

    // Full-width row rectangles computed in resized(); paintOverChildren() washes the inaudible
    // ones. Sized to match rows_ (empty until the first resized()).
    std::vector<juce::Rectangle<int>> row_rects_;
};

}  // namespace dz
