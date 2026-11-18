// src/doc/document.cpp — Document layer over op::Project + op::History (see document.hpp).
// No file I/O beyond load_project/save_project, no clock, no GUI. All state changes flow through
// the operation layer; dirty tracking compares canonical save bytes (the same ones save_project
// writes), so undoing back to the saved state makes the document clean again by construction.
#include "doc/document.hpp"

#include <utility>

#include "engine/model_json.hpp"  // project_to_json: canonical serialization, engine-internal bridge

namespace op {
namespace {

// The exact bytes save_project writes for a Project (keep in sync with model.cpp::save_project):
// canonical JSON dump(2) + trailing newline. Pure function of `p`, never throws.
std::string canonical_bytes(const Project& p) { return project_to_json(p).dump(2) + "\n"; }

}  // namespace

Document::Document() : saved_bytes_(canonical_bytes(project_)) {}

bool Document::open(const std::string& path, Document& out, std::string& err) {
    Project loaded;  // parse into a scratch first: on failure `out` must be untouched
    if (!load_project(path, loaded, err)) return false;
    out.project_ = std::move(loaded);
    out.path_ = path;
    out.revision_ = 0;
    out.history_ = History();  // no undo/redo history after open
    out.saved_bytes_ = canonical_bytes(out.project_);
    out.gesture_active_ = false;
    out.gesture_base_ = Project{};
    out.last_op_ = nlohmann::json();
    out.have_last_op_ = false;
    return true;
}

const Project& Document::project() const { return project_; }
const std::string& Document::path() const { return path_; }
std::uint64_t Document::revision() const { return revision_; }

bool Document::apply(const nlohmann::json& op_json, std::string& err) {
    if (gesture_active_) {
        err = "gesture active";
        return false;
    }
    if (!history_.apply(project_, op_json, err)) return false;  // rejected: nothing changed
    ++revision_;
    return true;
}

bool Document::undo() {
    if (gesture_active_ || !history_.undo(project_)) return false;
    ++revision_;
    return true;
}

bool Document::redo() {
    if (gesture_active_ || !history_.redo(project_)) return false;
    ++revision_;
    return true;
}

bool Document::can_undo() const { return history_.can_undo(); }
bool Document::can_redo() const { return history_.can_redo(); }

bool Document::begin_gesture() {
    if (gesture_active_) return false;
    gesture_base_ = project_;
    last_op_ = nlohmann::json();
    have_last_op_ = false;
    gesture_active_ = true;
    return true;
}

bool Document::update_gesture(const nlohmann::json& op_json, std::string& err) {
    if (!gesture_active_) {
        err = "no gesture";
        return false;
    }
    // Preview from the begin state, never on top of the previous preview.
    Project preview = gesture_base_;
    if (!apply_op(preview, op_json, err)) return false;  // invalid op: preview unchanged
    project_ = std::move(preview);
    last_op_ = op_json;
    have_last_op_ = true;
    ++revision_;
    return true;
}

bool Document::end_gesture(std::string& err) {
    if (!gesture_active_) {
        err = "no gesture";
        return false;
    }
    const std::string before = canonical_bytes(project_);  // what the last preview showed
    project_ = gesture_base_;                              // commit from the begin state ...
    if (have_last_op_ && !history_.apply(project_, last_op_, err)) {
        // Cannot happen in practice: the op already succeeded on this same base as a preview.
        // Leave the document at the begin state, gesture over; history untouched.
        gesture_active_ = false;
        if (canonical_bytes(project_) != before) ++revision_;
        return false;
    }
    gesture_active_ = false;
    if (canonical_bytes(project_) != before) ++revision_;  // commit == last preview → no bump
    return true;
}

void Document::cancel_gesture() {
    if (!gesture_active_) return;
    const std::string before = canonical_bytes(project_);
    project_ = gesture_base_;
    gesture_active_ = false;
    if (canonical_bytes(project_) != before) ++revision_;
}

bool Document::gesture_active() const { return gesture_active_; }

bool Document::dirty() const { return canonical_bytes(project_) != saved_bytes_; }

bool Document::save(std::string& err) {
    if (!save_project(path_, project_, err)) return false;
    saved_bytes_ = canonical_bytes(project_);
    return true;
}

bool Document::save_as(const std::string& path, std::string& err) {
    if (!save_project(path, project_, err)) return false;
    path_ = path;
    saved_bytes_ = canonical_bytes(project_);
    return true;
}

std::vector<TrackRow> Document::track_rows() const {
    std::vector<TrackRow> rows;
    rows.reserve(project_.tracks.size());
    for (std::size_t i = 0; i < project_.tracks.size(); ++i) {
        const Track& t = project_.tracks[i];
        rows.push_back(TrackRow{t.name, t.gain_db, t.pan, t.mute, t.solo, track_audible(project_, i)});
    }
    return rows;
}

}  // namespace op
