// src/doc/document.cpp — Document layer over op::Project + op::History (see document.hpp).
// No file I/O beyond load_project/save_project, no clock, no GUI. All state changes flow through
// the operation layer; dirty tracking compares op::project_bytes (the exact bytes save_project
// writes), so undoing back to the saved state makes the document clean again by construction.
#include "doc/document.hpp"

#include <cstdio>
#include <fstream>
#include <utility>

namespace op {

namespace {

// Autosave temp file of an autosave file: <path>.autosave.tmp
std::string autosave_tmp_path(const std::string& path) { return Document::autosave_path(path) + ".tmp"; }

// Best-effort removal of the autosave file and its temp file of `path` (missing is fine).
void remove_autosave_files(const std::string& path) {
    const std::string file = Document::autosave_path(path);
    std::remove(file.c_str());
    std::remove((file + ".tmp").c_str());
}

}  // namespace

Document::Document() : saved_bytes_(project_bytes(project_)) {}

bool Document::open(const std::string& path, Document& out, std::string& err) {
    Project loaded;  // parse into a scratch first: on failure `out` must be untouched
    if (!load_project(path, loaded, err)) return false;
    out.project_ = std::move(loaded);
    out.path_ = path;
    out.revision_ = 0;
    out.history_ = History();  // no undo/redo history after open
    out.saved_bytes_ = project_bytes(out.project_);
    out.gesture_active_ = false;
    out.gesture_base_ = Project{};
    out.last_op_ = nlohmann::json();
    out.have_last_op_ = false;
    out.have_autosaved_ = false;
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
    const std::string before = project_bytes(project_);  // what the last preview showed
    project_ = gesture_base_;                              // commit from the begin state ...
    if (have_last_op_ && !history_.apply(project_, last_op_, err)) {
        // Cannot happen in practice: the op already succeeded on this same base as a preview.
        // Leave the document at the begin state, gesture over; history untouched.
        gesture_active_ = false;
        if (project_bytes(project_) != before) ++revision_;
        return false;
    }
    gesture_active_ = false;
    if (project_bytes(project_) != before) ++revision_;  // commit == last preview → no bump
    return true;
}

void Document::cancel_gesture() {
    if (!gesture_active_) return;
    const std::string before = project_bytes(project_);
    project_ = gesture_base_;
    gesture_active_ = false;
    if (project_bytes(project_) != before) ++revision_;
}

bool Document::gesture_active() const { return gesture_active_; }

bool Document::dirty() const { return project_bytes(project_) != saved_bytes_; }

bool Document::save(std::string& err) {
    if (!save_project(path_, project_, err)) return false;
    saved_bytes_ = project_bytes(project_);
    remove_autosave_files(path_);
    have_autosaved_ = false;
    return true;
}

bool Document::save_as(const std::string& path, std::string& err) {
    if (!save_project(path, project_, err)) return false;
    path_ = path;
    saved_bytes_ = project_bytes(project_);
    remove_autosave_files(path_);
    have_autosaved_ = false;
    return true;
}

std::string Document::autosave_path(const std::string& path) { return path + ".autosave"; }

bool Document::autosave(std::string& err) {
    if (path_.empty()) {
        err = "cannot autosave an untitled document";
        return false;
    }
    if (!dirty()) return true;  // clean: an existing autosave file stays as it is
    if (have_autosaved_ && autosaved_revision_ == revision_) return true;  // already current

    const std::string file = autosave_path(path_);
    const std::string tmp = autosave_tmp_path(path_);
    const std::string bytes = project_bytes(project_);

    bool ok = false;
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (f) {
            f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            f.flush();
            f.close();
            ok = !f.fail();
        }
    }
    if (ok && std::rename(tmp.c_str(), file.c_str()) == 0) {
        autosaved_revision_ = revision_;
        have_autosaved_ = true;
        return true;
    }
    std::remove(tmp.c_str());  // ignore the result: the temp file may simply not exist
    err = "cannot write autosave '" + file + "'";
    return false;
}

RecoveryState Document::recovery_state(const std::string& path) {
    try {
        Project main_project;
        Project autosave_project;
        std::string err;
        if (!load_project(path, main_project, err)) return RecoveryState::None;
        if (!load_project(autosave_path(path), autosave_project, err)) return RecoveryState::None;
        return project_bytes(main_project) != project_bytes(autosave_project) ? RecoveryState::AutosaveNewer
                                                                              : RecoveryState::None;
    } catch (...) {
        return RecoveryState::None;
    }
}

bool Document::open_recovered(const std::string& path, Document& out, std::string& err) {
    if (recovery_state(path) != RecoveryState::AutosaveNewer) {
        err = "no recoverable autosave for '" + path + "'";
        return false;
    }
    Project recovered;  // parse into scratches first: on failure `out` must be untouched
    Project on_disk;
    if (!load_project(autosave_path(path), recovered, err)) return false;
    if (!load_project(path, on_disk, err)) return false;
    out.project_ = std::move(recovered);
    out.path_ = path;
    out.revision_ = 0;
    out.history_ = History();
    out.saved_bytes_ = project_bytes(on_disk);  // baseline = main file, so the recovery is dirty
    out.gesture_active_ = false;
    out.gesture_base_ = Project{};
    out.last_op_ = nlohmann::json();
    out.have_last_op_ = false;
    out.have_autosaved_ = false;
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
