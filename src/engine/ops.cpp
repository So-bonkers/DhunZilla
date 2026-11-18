// engine/ops.cpp — see engine/ops.hpp / docs/spec/operations.md.
// Strategy (spec §Apply semantics): serialize the current project with the exact save_project
// path, apply the operation as a JSON edit on that copy, then parse the result back with the
// exact load_project path and re-validate it via one more save→parse round-trip. There is no
// second set of validation rules; the original Project is only replaced at the very end.
#include "engine/ops.hpp"

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "engine/model_json.hpp"

namespace op {

namespace {

using nlohmann::json;

// ---- strict argument accessors: validate, never mutate, one-line reasons ----

// Reject anything but an in-range non-negative integer index. Per spec landmine, floats are
// rejected even when integral (`2.0` is NOT ok): is_number_integer() only.
bool arg_index(const json& o, const char* key, std::size_t limit, std::size_t& out,
               std::string& err) {
    if (!o.contains(key)) { err = std::string("missing argument '") + key + "'"; return false; }
    const json& v = o.at(key);
    if (!v.is_number_integer()) { err = std::string("'") + key + "' must be an integer"; return false; }
    long long iv;
    if (v.is_number_unsigned()) {
        const uint64_t uv = v.get<uint64_t>();
        if (uv > static_cast<uint64_t>(std::numeric_limits<long long>::max())) {
            err = std::string("'") + key + "' out of range"; return false;
        }
        iv = static_cast<long long>(uv);
    } else {
        iv = v.get<long long>();
    }
    if (iv < 0 || static_cast<std::size_t>(iv) >= limit) {
        err = std::string("'") + key + "' out of range"; return false;
    }
    out = static_cast<std::size_t>(iv);
    return true;
}

bool has_number(const json& o, const char* key, std::string& err) {
    if (!o.contains(key)) { err = std::string("missing argument '") + key + "'"; return false; }
    if (!o.at(key).is_number()) { err = std::string("'") + key + "' must be a number"; return false; }
    return true;
}

bool has_string(const json& o, const char* key, std::string& err) {
    if (!o.contains(key)) { err = std::string("missing argument '") + key + "'"; return false; }
    if (!o.at(key).is_string()) { err = std::string("'") + key + "' must be a string"; return false; }
    return true;
}

bool has_object(const json& o, const char* key, std::string& err) {
    if (!o.contains(key)) { err = std::string("missing argument '") + key + "'"; return false; }
    if (!o.at(key).is_object()) { err = std::string("'") + key + "' must be an object"; return false; }
    return true;
}

// Keys other than "op" and those listed → reject (spec §Operation: keys not listed → reject).
bool check_keys(const json& o, std::initializer_list<const char*> allowed, std::string& err) {
    for (auto it = o.begin(); it != o.end(); ++it) {
        if (it.key() == "op") continue;
        bool known = false;
        for (const char* a : allowed)
            if (it.key() == a) { known = true; break; }
        if (!known) { err = "unknown argument '" + it.key() + "'"; return false; }
    }
    return true;
}

}  // namespace

// Applies one named op; on failure sets `reason` (the bare "<reason>" half of "<op>: <reason>").
// May throw json::exception from the parse step — apply_op catches.
static bool apply_named(Project& p, const nlohmann::json& op_json, const std::string& name,
                        std::string& reason) {
    json j = project_to_json(p);  // work on a copy of the canonical serialization (spec step 1)
    std::size_t idx = 0, sub = 0;
    std::string& err = reason;  // accessor helpers below report bare reasons
    try {
        if (name == "set_bpm") {
            if (!check_keys(op_json, {"bpm"}, err) || !has_number(op_json, "bpm", err)) return false;
            j["bpm"] = op_json.at("bpm");
        } else if (name == "set_length_bars") {
            if (!check_keys(op_json, {"length_bars"}, err) ||
                !has_number(op_json, "length_bars", err)) return false;
            j["length_bars"] = op_json.at("length_bars");
        } else if (name == "add_track") {
            if (!check_keys(op_json, {"track"}, err) || !has_object(op_json, "track", err)) return false;
            j["tracks"].push_back(op_json.at("track"));
        } else if (name == "remove_track") {
            if (!check_keys(op_json, {"index"}, err)) return false;
            if (!arg_index(op_json, "index", j["tracks"].size(), idx, err)) return false;
            // Indices held by other elements are NOT rewritten → refuse cascades (spec).
            for (const auto& pj : j["patterns"])
                if (pj.value("track", -1) == static_cast<long long>(idx)) { err = "track in use"; return false; }
            j["tracks"].erase(idx);
        } else if (name == "set_track_gain") {
            if (!check_keys(op_json, {"index", "gain_db"}, err)) return false;
            if (!arg_index(op_json, "index", j["tracks"].size(), idx, err) ||
                !has_number(op_json, "gain_db", err)) return false;
            j["tracks"][idx]["gain_db"] = op_json.at("gain_db");
        } else if (name == "set_track_pan") {
            if (!check_keys(op_json, {"index", "pan"}, err)) return false;
            if (!arg_index(op_json, "index", j["tracks"].size(), idx, err) ||
                !has_number(op_json, "pan", err)) return false;
            j["tracks"][idx]["pan"] = op_json.at("pan");
        } else if (name == "rename_track") {
            if (!check_keys(op_json, {"index", "name"}, err)) return false;
            if (!arg_index(op_json, "index", j["tracks"].size(), idx, err) ||
                !has_string(op_json, "name", err)) return false;
            j["tracks"][idx]["name"] = op_json.at("name");
        } else if (name == "add_pattern") {
            if (!check_keys(op_json, {"pattern"}, err) || !has_object(op_json, "pattern", err)) return false;
            j["patterns"].push_back(op_json.at("pattern"));
        } else if (name == "remove_pattern") {
            if (!check_keys(op_json, {"index"}, err)) return false;
            if (!arg_index(op_json, "index", j["patterns"].size(), idx, err)) return false;
            if (j.contains("arrangement"))
                for (const auto& cj : j["arrangement"])
                    if (cj.value("pattern", -1) == static_cast<long long>(idx)) { err = "pattern in use"; return false; }
            j["patterns"].erase(idx);
        } else if (name == "add_note") {
            if (!check_keys(op_json, {"pattern", "note"}, err) || !has_object(op_json, "note", err)) return false;
            if (!arg_index(op_json, "pattern", j["patterns"].size(), idx, err)) return false;
            j["patterns"][idx]["notes"].push_back(op_json.at("note"));
        } else if (name == "remove_note") {
            if (!check_keys(op_json, {"pattern", "index"}, err)) return false;
            if (!arg_index(op_json, "pattern", j["patterns"].size(), idx, err)) return false;
            const json& notes = j["patterns"][idx]["notes"];
            if (!arg_index(op_json, "index", notes.size(), sub, err)) return false;
            j["patterns"][idx]["notes"].erase(sub);
        } else if (name == "add_clip") {
            if (!check_keys(op_json, {"clip"}, err) || !has_object(op_json, "clip", err)) return false;
            if (!j.contains("arrangement")) j["arrangement"] = json::array();
            j["arrangement"].push_back(op_json.at("clip"));
        } else if (name == "move_clip") {
            if (!check_keys(op_json, {"index", "start_bar"}, err)) return false;
            const std::size_t n_clips = j.contains("arrangement") ? j["arrangement"].size() : 0;
            if (!arg_index(op_json, "index", n_clips, idx, err) ||
                !has_number(op_json, "start_bar", err)) return false;
            j["arrangement"][idx]["start_bar"] = op_json.at("start_bar");
        } else if (name == "set_clip_gain") {
            if (!check_keys(op_json, {"index", "gain_db"}, err)) return false;
            const std::size_t n_clips = j.contains("arrangement") ? j["arrangement"].size() : 0;
            if (!arg_index(op_json, "index", n_clips, idx, err) ||
                !has_number(op_json, "gain_db", err)) return false;
            j["arrangement"][idx]["gain_db"] = op_json.at("gain_db");
        } else if (name == "remove_clip") {
            if (!check_keys(op_json, {"index"}, err)) return false;
            const std::size_t n_clips = j.contains("arrangement") ? j["arrangement"].size() : 0;
            if (!arg_index(op_json, "index", n_clips, idx, err)) return false;
            j["arrangement"].erase(idx);
        } else if (name == "add_attachment") {
            // Same rules as op::add_attachment: valid_attachment (enforced by the parse below)
            // plus duplicate-path rejection (the only rule load_project does not enforce).
            if (!check_keys(op_json, {"path", "kind", "producer"}, err)) return false;
            if (!has_string(op_json, "path", err) || !has_string(op_json, "kind", err)) return false;
            if (op_json.contains("producer") && !op_json.at("producer").is_string()) {
                err = "'producer' must be a string"; return false;
            }
            const std::string path = op_json.at("path").get<std::string>();
            if (!j.contains("attachments")) j["attachments"] = json::array();
            for (const auto& aj : j["attachments"])
                if (aj.value("path", "") == path) { err = "attachment path already present: '" + path + "'"; return false; }
            json aj;
            aj["path"] = path;
            aj["kind"] = op_json.at("kind");
            if (op_json.contains("producer")) aj["producer"] = op_json.at("producer");
            j["attachments"].push_back(std::move(aj));
        } else {
            err = "unknown op"; return false;
        }

        // Spec steps 2–4: validate the mutated copy through the untouched save→parse path, then
        // commit. Mistyped nested fields may throw json::exception (WP-0122 contract) → caught below.
        Project parsed;
        if (!project_from_json(j, parsed, err)) return false;
        const json canon = project_to_json(parsed);
        Project done;
        if (!project_from_json(canon, done, err)) return false;
        p = std::move(done);
        return true;
    } catch (const json::exception& e) {
        err = std::string("json: ") + e.what();  // p untouched on every path (spec step 3)
        return false;
    }
}

bool apply_op(Project& p, const json& op_json, std::string& err) {
    if (!op_json.is_object()) { err = "op: operation must be a JSON object"; return false; }
    if (!op_json.contains("op") || !op_json.at("op").is_string()) {
        err = "op: missing 'op' key"; return false;
    }
    const std::string name = op_json.at("op").get<std::string>();
    std::string reason;
    if (!apply_named(p, op_json, name, reason)) { err = name + ": " + reason; return false; }
    return true;
}

bool apply_journal(Project& p, const std::string& jsonl_text, std::string& err,
                   JournalOutcome* outcome) {
    if (outcome) *outcome = JournalOutcome{};

    // Phase 1: parse every line (blank and '#' lines ignored). Line numbers are 1-based over
    // physical lines so errors point into the file. All-or-nothing → nothing applied yet.
    std::vector<std::pair<std::size_t, json>> ops;
    std::istringstream in(jsonl_text);
    std::string line;
    for (std::size_t line_no = 0; std::getline(in, line);) {
        ++line_no;
        if (!line.empty() && line.back() == '\r') line.pop_back();  // tolerate CRLF journals
        const std::size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos) continue;  // blank
        if (line[first] == '#') continue;          // comment
        json o;
        try {
            o = json::parse(line);
        } catch (const json::exception& e) {
            err = "line " + std::to_string(line_no) + ": json: " + e.what();
            if (outcome) outcome->failure = JournalFailure::kParse;
            return false;
        }
        if (!o.is_object()) {
            err = "line " + std::to_string(line_no) + ": op: journal line must be a JSON object";
            if (outcome) outcome->failure = JournalFailure::kParse;
            return false;
        }
        ops.emplace_back(line_no, std::move(o));
    }

    // Phase 2: apply to a working copy; commit only when every op succeeded.
    Project work = p;
    for (const auto& [line_no, o] : ops) {
        if (!apply_op(work, o, err)) {
            err = "line " + std::to_string(line_no) + ": " + err;
            if (outcome) outcome->failure = JournalFailure::kRejected;
            return false;
        }
    }
    p = std::move(work);
    if (outcome) outcome->applied = ops.size();
    return true;
}

bool History::apply(Project& p, const json& op_json, std::string& err) {
    Project before = p;  // snapshot first: failed applies must not touch either stack (spec)
    if (!apply_op(p, op_json, err)) return false;
    undo_.push_back(std::move(before));
    if (undo_.size() > kCap) undo_.erase(undo_.begin());  // cap 256: drop the oldest
    redo_.clear();
    return true;
}

bool History::undo(Project& p) {
    if (undo_.empty()) return false;
    redo_.push_back(p);
    p = std::move(undo_.back());
    undo_.pop_back();
    return true;
}

bool History::redo(Project& p) {
    if (redo_.empty()) return false;
    undo_.push_back(p);
    p = std::move(redo_.back());
    redo_.pop_back();
    return true;
}

}  // namespace op
