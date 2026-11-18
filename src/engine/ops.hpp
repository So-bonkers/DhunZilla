// engine/ops.hpp — serializable model operations, JSON-Lines journals, undo/redo history.
// Normative spec: docs/spec/operations.md (v1). Every project mutation the CLI/GUI/copilot
// performs is one of these JSON operation objects; apply_op is atomic and validation re-uses
// the save→parse path (no second set of rules). No file I/O, clock or randomness here.
#pragma once
#include <cstddef>
#include <string>

#include <nlohmann/json.hpp>

#include "engine/model.hpp"

namespace op {

// Apply one operation object to *p atomically: on success the mutated project replaces *p; on
// any failure (unknown op, unknown/missing/mistyped argument, index out of range, in-use
// rejection, model-validation failure) *p is byte-for-byte unchanged and err is one line
// "<op>: <reason>". Example:
//   nlohmann::json o = nlohmann::json::parse(R"({"op":"set_bpm","bpm":90.0})");
//   std::string err;
//   if (!op::apply_op(proj, o, err)) { /* proj untouched */ }
bool apply_op(Project& p, const nlohmann::json& op_json, std::string& err);

// Which phase of apply_journal failed — lets callers (e.g. opd) map exit codes without
// string-matching the error message. kParse: a journal line is not valid JSON / not an object;
// kRejected: apply_op rejected a well-formed operation.
enum class JournalFailure { kNone, kParse, kRejected };

// Optional details from apply_journal (kept out of the core bool+err signature).
struct JournalOutcome {
    JournalFailure failure = JournalFailure::kNone;  // set when apply_journal returns false
    std::size_t applied = 0;                         // ops applied on success
};

// Apply a JSON-Lines journal (docs/spec/operations.md §Journal) all-or-nothing: one operation
// per line; blank lines and lines whose first non-blank char is '#' are ignored; if any line
// fails, *p is unchanged and err names the 1-based physical line number ("line N: <op>:
// <reason>"). If `outcome` != nullptr its fields report the failure phase / applied count.
// Example:
//   std::string err; op::JournalOutcome oc{};
//   if (!op::apply_journal(proj, R"({"op":"set_bpm","bpm":90})\n", err, &oc)) { /* unchanged */ }
bool apply_journal(Project& p, const std::string& jsonl_text, std::string& err,
                   JournalOutcome* outcome = nullptr);

// Undo/redo over apply_op using whole-project snapshots (spec §Undo/redo): projects are small,
// snapshots are boring and obviously correct. Cap 256 undo entries — the oldest is dropped.
// Failed applies touch neither stack. Example:
//   op::History h; std::string err;
//   h.apply(proj, nlohmann::json{{"op", "set_bpm"}, {"bpm", 90.0}}, err);  // success
//   h.undo(proj);  // bpm back to before the apply
//   h.redo(proj);  // forward again
class History {
public:
    static constexpr std::size_t kCap = 256;  // max undo snapshots kept

    // On success: push the pre-apply snapshot to the undo stack (dropping the oldest beyond
    // cap) and clear the redo stack. On failure: *p and both stacks unchanged.
    bool apply(Project& p, const nlohmann::json& op_json, std::string& err);

    // Restore the most recent pre-apply snapshot (pushing the current state to redo).
    // Returns false when there is nothing to undo (*p unchanged).
    bool undo(Project& p);

    // Mirror image of undo(). Returns false when there is nothing to redo (*p unchanged).
    bool redo(Project& p);

    bool can_undo() const { return !undo_.empty(); }
    bool can_redo() const { return !redo_.empty(); }
    std::size_t undo_depth() const { return undo_.size(); }

private:
    std::vector<Project> undo_;  // pre-apply snapshots, oldest first
    std::vector<Project> redo_;  // states displaced by undo(), oldest first
};

}  // namespace op
