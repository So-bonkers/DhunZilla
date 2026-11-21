// src/app/main_component.hpp — dhunzilla's top-level component (WP-0143, editable since WP-0152,
// live transport since WP-0153).
//
// Transport bar (top 56 px): Play/Stop TextButtons, a read-only BPM label ("BPM 120.0") and a
// position label showing "bar.beat" (format_position over the Player's frame counter; "1.1" at
// home). Below it: one row per track from Document::track_rows(), in order — name, gain slider
// (-96..24 dB, text box), pan rotary (-1..1, no text box), M and S buttons whose lit colour
// differs when mute/solo is set. Rows are EDITABLE through op::Document (model-driven: every edit
// is an operation applied via begin_gesture/update_gesture/end_gesture or apply(); the model is the
// only state — refresh() re-reads doc.track_rows() and pushes values into the widgets with
// dontSendNotification). Rows whose track is not audible per the WP-0136 rule are drawn dimmed. At
// the very bottom: a status line showing the audio device (or the last error) — empty in snapshot
// mode.
//
// Channel rack (WP-0195): below the track rows, one 16-step row per project pattern (built once
// from doc.project().patterns). A cell click reads its step from the model via op::step_grid and
// applies the set_step operation through Document::apply — one click = one undo entry; refresh()
// re-reads every cell's lit state from the model, so the widgets hold no step state of their own.
//
// Two flavours (WP-0153): with `playback == nullptr` (headless --snapshot) the transport buttons
// stay disabled, no timer runs, the status line stays empty and the position reads "1.1". With a
// controller: Play/Stop are wired, and a 30 Hz timer reclaims retired buffers, re-renders when the
// document changed while playing (an edit during playback is heard), and updates the position +
// status labels.
//
// Scripted driving (scriptDrag/scriptClick/widgetValue/trackCount) runs the very same handlers a
// user's mouse would, so headless verification exercises the real interaction paths; see main.cpp
// for the script grammar. Example:
//   dz::MainComponent comp(doc);
//   std::string err;
//   comp.scriptDrag("gain", 0, -20.0, err);   // one undo entry, like a real drag
//   comp.scriptClick("mute", 1, err);         // toggles via the button's onClick handler
//   (void)comp.handleKey(juce::KeyPress('z', juce::ModifierKeys::commandModifier));  // undo
//
// All UI string literals go through juce::String::fromUTF8 / CharPointer_UTF8: raw const char*
// literals are interpreted as Latin-1 by JUCE and would mojibake the em dash U+2014 (ADR-0007 §4).
#pragma once

#include <functional>
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
    static constexpr int kRackSteps = 16;      // step cells per rack row
    static constexpr int kRackRowHeight = 32;  // rack row pitch
    static constexpr int kRackCellPitch = 36;  // cell-to-cell distance; each cell is 32x24

    // `doc` must outlive this component and is mutated by every edit. The row widgets are built
    // once here — one per track, in Document::track_rows() order — and refresh() only re-reads
    // values from the same Document. `playback == nullptr` keeps the inert snapshot behaviour; with
    // a controller the transport is live: enabled buttons plus the 30 Hz position/status timer.
    explicit MainComponent(op::Document& doc, PlaybackController* playback = nullptr);
    ~MainComponent() override;  // out-of-line: RowWidgets (unique_ptr target) is defined in the .cpp

    // Re-reads doc.project()/doc.track_rows() and updates every widget value plus row dimming, all
    // with dontSendNotification (never triggers a callback), then fires onDocumentChanged.
    void refresh();

    // --- document commands (menus, shortcuts, scripts) ---
    bool doUndo();                  // doc_.undo(); refresh(); returns the Document result
    bool doRedo();                  // doc_.redo(); refresh()
    bool doSave(std::string& err);  // doc_.save(err); refresh()
    // Ctrl+Z → undo, Ctrl+Shift+Z → redo, Ctrl+S → save. True if consumed — a failed command
    // (nothing to undo, save error) is still consumed.
    bool handleKey(const juce::KeyPress& key);
    bool keyPressed(const juce::KeyPress& key) override;  // = handleKey(key)

    // --- scripted driving: run the SAME handlers a user's mouse would ---
    // what: "gain" | "pan". Clamps to the slider range, drives onDragStart → four setValue steps
    // (sendNotificationSync, last one exactly `value`) → onDragEnd: exactly one undo entry.
    // err set (and false returned) on a bad index/control or if Document::end_gesture failed.
    bool scriptDrag(const std::string& what, int track, double value, std::string& err);
    // what: "mute" | "solo". Invokes the button's onClick() std::function directly (NOT
    // triggerClick(), whose async message never runs in a headless script).
    bool scriptClick(const std::string& what, int track, std::string& err);
    // gain|pan → slider value; mute|solo → toggle state as 1.0/0.0. 0.0 for bad what/index.
    double widgetValue(const std::string& what, int track) const;
    int trackCount() const;

    // --- channel rack (WP-0195): scripted driving and verification of the step cells ---
    // Invokes the cell's onClick() std::function directly (never triggerClick()): exactly one
    // `set_step` through Document::apply. On a bad index returns false and sets err to EXACTLY:
    //   "pattern index <p> out of range (0..<n-1>)"   (n = patternCount(); checked first)
    //   "step index <s> out of range (0..15)"
    bool scriptStep(int pattern, int step, std::string& err);
    // The 16 cells' ACTUAL toggle states as a 16-char string of 'x' (lit) and '.' (unlit);
    // "" for a bad index.
    std::string gridText(int pattern) const;
    int patternCount() const;   // number of rack rows = project().patterns.size() when the component was built

    // Fired at the end of every refresh(); the window uses it to re-check menu enablement.
    std::function<void()> onDocumentChanged;

    void paint(juce::Graphics&) override;
    void paintOverChildren(juce::Graphics&) override;  // dims inaudible rows above their widgets
    void resized() override;

private:
    struct RowWidgets;  // defined in the .cpp: name label, gain/pan sliders, M/S buttons
    struct RackRow;     // defined in the .cpp (WP-0195): a juce::Label name + kRackSteps cells

    // 30 Hz (only started with a controller): reclaim retired buffers, re-render after any model
    // change while playing, refresh the bar.beat position and the device/error status line.
    void timerCallback() override;

    op::Document& doc_;
    PlaybackController* playback_ = nullptr;  // null in snapshot mode (see header note)

    juce::TextButton play_, stop_;
    juce::Label bpm_, position_, status_;
    std::uint64_t requested_revision_ = 0;  // doc.revision() at the last render request
    std::vector<std::unique_ptr<RowWidgets>> rows_;

    // Channel rack (WP-0195): one row per pattern, built once in the constructor; lit states live
    // only in the model (refresh() pushes them into the cells). rack_top_ is the y of the first
    // rack row, computed in resized().
    std::vector<std::unique_ptr<RackRow>> rack_;
    int rack_top_ = 0;

    // Full-width row rectangles computed in resized(); paintOverChildren() washes the inaudible
    // ones. Sized to match rows_ (empty until the first resized()).
    std::vector<juce::Rectangle<int>> row_rects_;

    // Result of the most recent slider onDragEnd (doc_.end_gesture), so scriptDrag can report a
    // refused gesture through its own err parameter.
    bool gesture_ok_ = true;
    std::string gesture_err_;
};

}  // namespace dz
