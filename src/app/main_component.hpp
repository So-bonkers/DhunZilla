// src/app/main_component.hpp — dhunzilla's top-level component (WP-0143, editable since WP-0152).
//
// Transport bar (top 56 px): Play/Stop TextButtons that are inert for now (no playback yet —
// later package), a read-only BPM label ("BPM 120.0") and a position label showing exactly
// "1.1" (bar.beat; the transport stays at home). Below it: one row per track from
// Document::track_rows(), in order — name, gain slider (-96..24 dB, text box), pan rotary
// (-1..1, no text box), M and S buttons whose lit colour differs when mute/solo is set. Rows are
// EDITABLE through op::Document (model-driven: every edit is an operation applied via
// begin_gesture/update_gesture/end_gesture or apply(); the model is the only state — refresh()
// re-reads doc.track_rows() and pushes values into the widgets with dontSendNotification). Rows
// whose track is not audible per the WP-0136 rule are drawn dimmed.
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

    // `doc` must outlive this component and is mutated by every edit. The row widgets are built
    // once here — one per track, in Document::track_rows() order — and refresh() only re-reads
    // values from the same Document.
    explicit MainComponent(op::Document& doc);
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

    // Fired at the end of every refresh(); the window uses it to re-check menu enablement.
    std::function<void()> onDocumentChanged;

    void paint(juce::Graphics&) override;
    void paintOverChildren(juce::Graphics&) override;  // dims inaudible rows above their widgets
    void resized() override;

private:
    struct RowWidgets;  // defined in the .cpp: name label, gain/pan sliders, M/S buttons

    op::Document& doc_;

    juce::TextButton play_, stop_;
    juce::Label bpm_, position_;
    std::vector<std::unique_ptr<RowWidgets>> rows_;

    // Full-width row rectangles computed in resized(); paintOverChildren() washes the inaudible
    // ones. Sized to match rows_ (empty until the first resized()).
    std::vector<juce::Rectangle<int>> row_rects_;

    // Result of the most recent slider onDragEnd (doc_.end_gesture), so scriptDrag can report a
    // refused gesture through its own err parameter.
    bool gesture_ok_ = true;
    std::string gesture_err_;
};

}  // namespace dz
