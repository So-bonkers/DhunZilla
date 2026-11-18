// src/doc/document.hpp — GUI-free document object for the app (WP-0141, WP-0137a). Owns a
// Project + op::History + file path and gives a UI everything it needs from a project file:
// open/save with a dirty flag computed from the canonical save bytes, undo/redo over the
// WP-0135 operations, continuous slider gestures that produce exactly ONE undo entry, a revision
// counter for refresh scheduling, and derived track-list rows. No GUI, no audio, no UI toolkit —
// unit-testable in op_tests, reusable by the CLI later. The app (WP-0137b+) calls only this class.
// Example:
//   op::Document doc; std::string err;
//   if (!op::Document::open("song.opj", doc, err)) { /* report err */ }
//   doc.apply(nlohmann::json{{"op", "set_bpm"}, {"bpm", 90.0}}, err);  // dirty, can_undo
//   doc.undo();                                                        // clean again (same bytes)
//   if (doc.begin_gesture()) {                                         // slider press
//       for (float g = -1.f; g >= -10.f; g -= 1.f)                     // drags: previews only
//           doc.update_gesture(nlohmann::json{{"op", "set_track_gain"},
//                                             {"index", 0}, {"gain_db", g}}, err);
//       doc.end_gesture(err);                                          // release: one undo entry
//   }
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/model.hpp"
#include "engine/ops.hpp"

namespace op {

struct TrackRow {  // what a track-list row displays; derived from the model, never stored
    std::string name;
    float gain_db = 0.0f;
    float pan = 0.0f;
    bool mute = false;
    bool solo = false;
    bool audible = true;  // op::track_audible(project, i) — the WP-0136 rule
};

class Document {
public:
    // A fresh document holds a default Project at an empty path, clean, with no history.
    // The real entry point is open().
    Document();

    // Load `path` with op::load_project. On failure: returns false, `out` unchanged, err set.
    // On success: out holds the project, path() == path, dirty() == false, no undo/redo history.
    static bool open(const std::string& path, Document& out, std::string& err);

    const Project& project() const;  // current state (includes an in-progress gesture preview)
    const std::string& path() const;
    std::uint64_t revision() const;  // +1 on EVERY change to project(); UI refreshes when it moves

    // Apply one WP-0135 operation (JSON object) through the owned op::History.
    // Refused (false, err "gesture active") while a gesture is active. Rejected op: false, project,
    // history and revision unchanged.
    bool apply(const nlohmann::json& op, std::string& err);
    bool undo();  // false if nothing to undo or a gesture is active
    bool redo();  // false if nothing to redo or a gesture is active
    bool can_undo() const;
    bool can_redo() const;

    // Continuous edits (slider drag). Exactly ONE undo entry per gesture, however many updates.
    bool begin_gesture();  // false if a gesture is already active
    // Preview: applies `op` to the state captured by begin_gesture() (NOT cumulatively on top of the
    // previous preview) — project() shows the result; history untouched. Invalid op: false, preview
    // unchanged. Calling it with no active gesture: false, err "no gesture".
    bool update_gesture(const nlohmann::json& op, std::string& err);
    // Commit: restore the begin state, then apply the LAST successful update op through History
    // (one undo entry). No successful update → no history entry, returns true. Ends the gesture.
    bool end_gesture(std::string& err);
    void cancel_gesture();  // restore the begin state, no history entry, end the gesture
    bool gesture_active() const;

    // dirty() == the canonical save bytes of project() differ from the bytes at the last open/save.
    // (Undoing back to the saved state makes it clean again.)
    bool dirty() const;
    bool save(std::string& err);                            // writes path() via op::save_project
    bool save_as(const std::string& path, std::string& err);  // on success path() becomes `path`

    std::vector<TrackRow> track_rows() const;

private:
    Project project_;
    std::string path_;
    std::uint64_t revision_ = 0;
    History history_;
    std::string saved_bytes_;      // canonical save bytes at the last open/save (dirty baseline)
    bool gesture_active_ = false;
    Project gesture_base_;         // project() as of begin_gesture()
    nlohmann::json last_op_;       // last successful update_gesture operation
    bool have_last_op_ = false;
};

}  // namespace op
