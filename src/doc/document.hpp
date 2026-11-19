// src/doc/document.hpp — GUI-free document object for the app (WP-0141, WP-0137a). Owns a
// Project + op::History + file path and gives a UI everything it needs from a project file:
// open/save with a dirty flag computed from op::project_bytes, undo/redo over the
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
// Autosave + crash recovery (WP-0157). The doc layer has no timers: the app calls autosave()
// periodically. Autosave writes <path>.autosave (never the main file); on the next launch:
//   if (op::Document::recovery_state(path) == op::RecoveryState::AutosaveNewer)
//       op::Document::open_recovered(path, doc, err);  // dirty; the user decides what to keep
//   else
//       op::Document::open(path, doc, err);
// save()/save_as() remove the autosave file of the saved path.
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

enum class RecoveryState { None, AutosaveNewer };

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

    // dirty() == the op::project_bytes of project() differ from the bytes at the last open/save.
    // (Undoing back to the saved state makes it clean again.)
    bool dirty() const;
    bool save(std::string& err);                            // writes path() via op::save_project
    bool save_as(const std::string& path, std::string& err);  // on success path() becomes `path`

    // Autosave (WP-0157). The autosave file of `path` is path + ".autosave". Writes
    // op::project_bytes(project()) atomically (temp file + rename) only when the document is dirty
    // and its revision() differs from the last successful autosave. Never touches path(), the
    // revision, the dirty state or the history. Untitled document: false, err "cannot autosave an
    // untitled document". Write failure: false, err "cannot write autosave '<file>'".
    bool autosave(std::string& err);
    static std::string autosave_path(const std::string& path);  // returns path + ".autosave"
    // AutosaveNewer iff `path` and its autosave both load and their project_bytes differ.
    // Never modifies files, never throws.
    static RecoveryState recovery_state(const std::string& path);
    // Only if recovery_state(path) == AutosaveNewer: `out` holds the autosave content at `path`,
    // dirty() is true against the on-disk main file, no history, revision 0. Otherwise false,
    // `out` unchanged, err "no recoverable autosave for '<path>'".
    static bool open_recovered(const std::string& path, Document& out, std::string& err);

    std::vector<TrackRow> track_rows() const;

private:
    Project project_;
    std::string path_;
    std::uint64_t revision_ = 0;
    History history_;
    std::string saved_bytes_;      // op::project_bytes at the last open/save (dirty baseline)
    bool gesture_active_ = false;
    Project gesture_base_;         // project() as of begin_gesture()
    nlohmann::json last_op_;       // last successful update_gesture operation
    bool have_last_op_ = false;
    std::uint64_t autosaved_revision_ = 0;  // revision at the last successful autosave
    bool have_autosaved_ = false;           // false: never autosaved since open/save
};

}  // namespace op
