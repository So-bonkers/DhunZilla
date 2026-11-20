// engine/ops.cpp — see engine/ops.hpp / docs/spec/operations.md.
// Strategy (spec §Apply semantics): serialize the current project with the exact save_project
// path, apply the operation as a JSON edit on that copy, then parse the result back with the
// exact load_project path and re-validate it via one more save→parse round-trip. There is no
// second set of validation rules; the original Project is only replaced at the very end.
#include "engine/ops.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
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

bool has_bool(const json& o, const char* key, std::string& err) {
    if (!o.contains(key)) { err = std::string("missing argument '") + key + "'"; return false; }
    if (!o.at(key).is_boolean()) { err = std::string("'") + key + "' must be a boolean"; return false; }
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

// Integer-typed value (WP-0147). The range is left to the model parse, not duplicated here.
bool has_integer(const json& o, const char* key, std::string& err) {
    if (!o.contains(key)) { err = std::string("missing argument '") + key + "'"; return false; }
    if (!o.at(key).is_number_integer()) { err = std::string("'") + key + "' must be an integer"; return false; }
    return true;
}

// True when an integer JSON value fits in a 32-bit `int` (the model's field type). Values that do
// not fit are refused by the caller so they can never wrap silently.
bool fits_int(const json& v) {
    constexpr long long kMin = std::numeric_limits<int>::min();
    constexpr long long kMax = std::numeric_limits<int>::max();
    if (v.is_number_unsigned()) return v.get<uint64_t>() <= static_cast<uint64_t>(kMax);
    const long long iv = v.get<long long>();
    return iv >= kMin && iv <= kMax;
}

// Step-sequencer limits (WP-0154): step index range [0, 4096), MIDI pitch range [0, 127].
constexpr std::size_t kStepLimit = 4096;

// True when the integer JSON value v lies in [lo, hi]. Caller has checked is_number_integer().
// Unsigned values above the signed range are out of range for any hi < 2^63.
bool int_in_range(const json& v, long long lo, long long hi) {
    if (v.is_number_unsigned()) return v.get<uint64_t>() <= static_cast<uint64_t>(hi);
    const long long iv = v.get<long long>();
    return iv >= lo && iv <= hi;
}

// A note time is on the step grid iff it is a non-negative multiple of 0.25. Multiplying by 4 is
// exact (power-of-two scale), so `t * 4` integral <=> `t == s * 0.25` for some integer s >= 0.
bool on_grid(double t) {
    const double q = t * 4.0;
    return t >= 0.0 && std::isfinite(q) && std::floor(q) == q;
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
        } else if (name == "set_sampler_trim") {  // WP-0156; range checks come from the model parse below
            if (!check_keys(op_json, {"index", "start_s", "end_s"}, err)) return false;
            if (!arg_index(op_json, "index", j["tracks"].size(), idx, err) ||
                !has_number(op_json, "start_s", err)) return false;
            if (!op_json.contains("end_s")) { err = "missing argument 'end_s'"; return false; }
            const json& end_v = op_json.at("end_s");
            if (!end_v.is_number() && !end_v.is_null()) { err = "'end_s' must be a number or null"; return false; }
            json& voice = j["tracks"][idx]["voice"];
            if (voice.value("kind", std::string{}) != "sampler") { err = "track is not a sampler voice"; return false; }
            voice["start_s"] = op_json.at("start_s");
            if (end_v.is_null()) voice.erase("end_s");  // null = no end: canonical save omits the key
            else voice["end_s"] = end_v;
        } else if (name == "set_synth_params") {  // WP-0161; types/ranges come from the model parse below (no second ruleset)
            if (!check_keys(op_json, {"index", "waveform", "adsr", "cutoff_hz", "resonance"}, err)) return false;
            if (!arg_index(op_json, "index", j["tracks"].size(), idx, err)) return false;
            const bool has_w = op_json.contains("waveform");
            const bool has_a = op_json.contains("adsr");
            const bool has_c = op_json.contains("cutoff_hz");
            const bool has_r = op_json.contains("resonance");
            if (!has_w && !has_a && !has_c && !has_r) { err = "no synth parameters given"; return false; }
            json& voice = j["tracks"][idx]["voice"];
            if (voice.value("kind", std::string{}) != "synth") { err = "track is not a synth voice"; return false; }
            // Each given key is written as-is; null removes it (= reset to default). adsr replaces
            // the whole object — inner keys not given take their defaults via the model parse.
            if (has_w) {
                const json& w = op_json.at("waveform");
                if (w.is_null()) voice.erase("waveform"); else voice["waveform"] = w;
            }
            if (has_a) {
                const json& a = op_json.at("adsr");
                if (a.is_null()) voice.erase("adsr"); else voice["adsr"] = a;
            }
            if (has_c) {
                const json& c = op_json.at("cutoff_hz");
                if (c.is_null()) voice.erase("cutoff_hz"); else voice["cutoff_hz"] = c;
            }
            if (has_r) {
                const json& r = op_json.at("resonance");
                if (r.is_null()) voice.erase("resonance"); else voice["resonance"] = r;
            }
        } else if (name == "set_sampler_root") {  // WP-0167; range checks come from the model parse below
            if (!check_keys(op_json, {"index", "root_note"}, err)) return false;
            if (!arg_index(op_json, "index", j["tracks"].size(), idx, err)) return false;
            if (!op_json.contains("root_note")) { err = "missing argument 'root_note'"; return false; }
            const json& root_v = op_json.at("root_note");
            if (!root_v.is_number_integer() && !root_v.is_null()) {
                err = "'root_note' must be an integer or null"; return false;
            }
            json& voice = j["tracks"][idx]["voice"];
            if (voice.value("kind", std::string{}) != "sampler") { err = "track is not a sampler voice"; return false; }
            if (root_v.is_null()) voice.erase("root_note");  // null = no root: canonical save omits the key
            else voice["root_note"] = root_v;
        } else if (name == "set_track_mute") {  // mixer (spec §mixer); audibility rule lives in the model
            if (!check_keys(op_json, {"index", "mute"}, err)) return false;
            if (!arg_index(op_json, "index", j["tracks"].size(), idx, err) ||
                !has_bool(op_json, "mute", err)) return false;
            j["tracks"][idx]["mute"] = op_json.at("mute");
        } else if (name == "set_track_solo") {
            if (!check_keys(op_json, {"index", "solo"}, err)) return false;
            if (!arg_index(op_json, "index", j["tracks"].size(), idx, err) ||
                !has_bool(op_json, "solo", err)) return false;
            j["tracks"][idx]["solo"] = op_json.at("solo");
        } else if (name == "set_master_gain") {  // range checked by the parse below (no second ruleset)
            if (!check_keys(op_json, {"gain_db"}, err) || !has_number(op_json, "gain_db", err)) return false;
            j["master_gain_db"] = op_json.at("gain_db");
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
        } else if (name == "set_pattern_swing") {  // WP-0166; range checks come from the model parse below
            if (!check_keys(op_json, {"pattern", "swing"}, err)) return false;
            if (!arg_index(op_json, "pattern", j["patterns"].size(), idx, err) ||
                !has_number(op_json, "swing", err)) return false;
            j["patterns"][idx]["swing"] = op_json.at("swing");
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
        } else if (name == "set_clip_fades") {  // WP-0178; range checks come from the model parse below
            if (!check_keys(op_json, {"index", "fade_in_beats", "fade_out_beats"}, err)) return false;
            const std::size_t n_clips = j.contains("arrangement") ? j["arrangement"].size() : 0;
            if (!arg_index(op_json, "index", n_clips, idx, err)) return false;
            json& clip = j["arrangement"][idx];
            for (const char* key : {"fade_in_beats", "fade_out_beats"}) {
                if (!op_json.contains(key)) { err = std::string("missing argument '") + key + "'"; return false; }
                const json& v = op_json.at(key);
                if (!v.is_number() && !v.is_null()) {
                    err = std::string("'") + key + "' must be a number or null"; return false;
                }
                if (v.is_null()) clip.erase(key);  // null clears the side to its default 0
                else clip[key] = v;
            }
        } else if (name == "remove_clip") {
            if (!check_keys(op_json, {"index"}, err)) return false;
            const std::size_t n_clips = j.contains("arrangement") ? j["arrangement"].size() : 0;
            if (!arg_index(op_json, "index", n_clips, idx, err)) return false;
            j["arrangement"].erase(idx);
        } else if (name == "set_clip_repeats") {
            if (!check_keys(op_json, {"index", "repeats"}, err)) return false;
            const std::size_t n_clips = j.contains("arrangement") ? j["arrangement"].size() : 0;
            if (!arg_index(op_json, "index", n_clips, idx, err) ||
                !has_integer(op_json, "repeats", err)) return false;
            if (!fits_int(op_json.at("repeats"))) { err = "clip.repeats out of range [1..4096]"; return false; }
            j["arrangement"][idx]["repeats"] = op_json.at("repeats");
        } else if (name == "set_clip_pattern") {
            if (!check_keys(op_json, {"index", "pattern"}, err)) return false;
            const std::size_t n_clips = j.contains("arrangement") ? j["arrangement"].size() : 0;
            if (!arg_index(op_json, "index", n_clips, idx, err) ||
                !has_integer(op_json, "pattern", err)) return false;
            if (!fits_int(op_json.at("pattern"))) {
                err = "clip.pattern out of range: " + op_json.at("pattern").dump(); return false;
            }
            j["arrangement"][idx]["pattern"] = op_json.at("pattern");
        } else if (name == "duplicate_clip") {
            if (!check_keys(op_json, {"index", "start_bar"}, err)) return false;
            const std::size_t n_clips = j.contains("arrangement") ? j["arrangement"].size() : 0;
            if (!arg_index(op_json, "index", n_clips, idx, err) ||
                !has_number(op_json, "start_bar", err)) return false;
            json copy = j["arrangement"][idx];
            copy["start_bar"] = op_json.at("start_bar");
            j["arrangement"].insert(j["arrangement"].begin() + static_cast<std::ptrdiff_t>(idx + 1),
                                    std::move(copy));
        } else if (name == "reorder_clip") {
            if (!check_keys(op_json, {"index", "to"}, err)) return false;
            const std::size_t n_clips = j.contains("arrangement") ? j["arrangement"].size() : 0;
            if (!arg_index(op_json, "index", n_clips, idx, err) ||
                !arg_index(op_json, "to", n_clips, sub, err)) return false;
            json& arr = j["arrangement"];
            json moved = arr[idx];
            arr.erase(idx);  // n_clips - 1 remain; `sub` <= n_clips - 1 is a valid final index
            arr.insert(arr.begin() + static_cast<std::ptrdiff_t>(sub), std::move(moved));
        } else if (name == "set_loop") {  // WP-0155: range rules come from the model parse below
            if (!check_keys(op_json, {"start_bar", "end_bar"}, err) ||
                !has_number(op_json, "start_bar", err) || !has_number(op_json, "end_bar", err)) return false;
            json loop = json::object();
            loop["start_bar"] = op_json.at("start_bar");
            loop["end_bar"] = op_json.at("end_bar");
            j["loop"] = std::move(loop);
        } else if (name == "clear_loop") {
            if (!check_keys(op_json, {}, err)) return false;
            j.erase("loop");  // no-op when absent; still a successful (undoable) apply
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
        } else if (name == "set_step") {  // WP-0154, spec §Step sequencer: validation order is normative
            if (!check_keys(op_json, {"pattern", "step", "on", "velocity", "pitch"}, err)) return false;
            if (!arg_index(op_json, "pattern", j["patterns"].size(), idx, err) ||
                !arg_index(op_json, "step", kStepLimit, sub, err) ||
                !has_bool(op_json, "on", err)) return false;
            const bool has_vel = op_json.contains("velocity");
            const bool has_pitch = op_json.contains("pitch");
            if (has_vel && !op_json.at("velocity").is_number()) { err = "'velocity' must be a number"; return false; }
            if (has_pitch) {
                if (!op_json.at("pitch").is_number_integer()) { err = "'pitch' must be an integer"; return false; }
                if (!int_in_range(op_json.at("pitch"), 0, 127)) { err = "'pitch' out of range"; return false; }
            }
            const bool on = op_json.at("on").get<bool>();
            if (!on && has_vel) { err = "'velocity' not allowed when 'on' is false"; return false; }
            if (!on && has_pitch) { err = "'pitch' not allowed when 'on' is false"; return false; }

            // Step s is at beat s*0.25; "on" means t == s*0.25 exactly. The pattern is read through
            // a const reference so a no-op call never creates a `notes` key.
            const double t = static_cast<double>(sub) * 0.25;
            json& pj = j["patterns"][idx];
            const json empty_notes = json::array();
            const json& cur = pj.contains("notes") ? pj.at("notes") : empty_notes;
            std::size_t first_on = cur.size();  // index of the lowest-index note on this step
            for (std::size_t i = 0; i < cur.size(); ++i)
                if (cur[i].value("t", 0.0) == t) { first_on = i; break; }

            if (on && first_on == cur.size()) {  // append the default step note (spec §Default step note)
                if (!pj.contains("notes")) pj["notes"] = json::array();
                json nn = json::object();
                nn["t"] = t;
                nn["dur"] = 0.25;
                if (has_pitch) nn["note"] = op_json.at("pitch");
                if (has_vel) nn["vel"] = op_json.at("velocity");
                pj["notes"].push_back(std::move(nn));
            } else if (on) {  // edit only the lowest-index note; t and dur are never changed
                if (has_vel) pj["notes"][first_on]["vel"] = op_json.at("velocity");
                if (has_pitch) pj["notes"][first_on]["note"] = op_json.at("pitch");
            } else {  // on:false — erase every note on the step, keep the rest in order
                json kept = json::array();
                bool any = false;
                for (const auto& n : cur) {
                    if (n.value("t", 0.0) == t) any = true;
                    else kept.push_back(n);
                }
                if (any) pj["notes"] = std::move(kept);
            }
        } else if (name == "clear_steps") {
            if (!check_keys(op_json, {"pattern"}, err)) return false;
            if (!arg_index(op_json, "pattern", j["patterns"].size(), idx, err)) return false;
            json& pj = j["patterns"][idx];
            const json empty_notes = json::array();
            const json& cur = pj.contains("notes") ? pj.at("notes") : empty_notes;
            json kept = json::array();
            bool any = false;
            for (const auto& n : cur) {
                if (on_grid(n.value("t", 0.0))) any = true;  // off-grid notes are never touched
                else kept.push_back(n);
            }
            if (any) pj["notes"] = std::move(kept);
        } else if (name == "set_pattern_length") {  // WP-0162: positivity/finiteness come from the model parse
            if (!check_keys(op_json, {"pattern", "length_beats"}, err)) return false;
            if (!arg_index(op_json, "pattern", j["patterns"].size(), idx, err) ||
                !has_number(op_json, "length_beats", err)) return false;
            j["patterns"][idx]["length_beats"] = op_json.at("length_beats");
        } else if (name == "clear_pattern_length") {
            if (!check_keys(op_json, {"pattern"}, err)) return false;
            if (!arg_index(op_json, "pattern", j["patterns"].size(), idx, err)) return false;
            j["patterns"][idx].erase("length_beats");  // no-op when absent; still a successful (undoable) apply
        } else if (name == "duplicate_pattern") {  // WP-0179: id rules come from the model parse below
            if (!check_keys(op_json, {"src", "new_id"}, err)) return false;
            std::size_t src_idx = 0;
            if (!arg_index(op_json, "src", j["patterns"].size(), src_idx, err) ||
                !has_string(op_json, "new_id", err)) return false;
            json dup = j["patterns"][src_idx];    // deep copy of the whole pattern object (notes included)
            dup["id"] = op_json.at("new_id");     // empty / taken ids are refused by the parse below
            j["patterns"].push_back(std::move(dup));  // appended: existing indices never shift
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

// ---- diff_projects (WP-0146): see engine/ops.hpp / docs/spec/operations.md §opd diff. ----
namespace {

// Canonical JSON omits default fields, so "absent" means the documented default: read a field
// with that fallback. Pure; never throws (caller checked nothing; `o` is our own canonical dump).
const json& field_or(const json& o, const char* key, const json& fallback) {
    const auto it = o.find(key);
    return it != o.end() ? it.value() : fallback;
}

}  // namespace

bool diff_projects(const Project& a, const Project& b, std::vector<json>& ops, std::string& err) {
    ops.clear();
    err.clear();
    const json ja = project_to_json(a);  // compare and emit from the canonical JSON (spec):
    const json jb = project_to_json(b);  // "equal" here is exactly "saved bytes equal".

    auto unsupported = [&](const std::string& field) {
        ops.clear();
        err = "unsupported difference: " + field;
        return false;
    };

    // Fields no operation can change, checked in exactly the spec's order (first hit reports).
    if (ja.at("format_version") != jb.at("format_version")) return unsupported("format_version");
    if (ja.at("sample_rate") != jb.at("sample_rate")) return unsupported("sample_rate");
    if (ja.at("seed") != jb.at("seed")) return unsupported("seed");
    if (ja.at("beats_per_bar") != jb.at("beats_per_bar")) return unsupported("beats_per_bar");
    static const json kEmptyArray = json::array();
    const json& tempo_a = field_or(ja, "tempo_map", kEmptyArray);
    const json& tempo_b = field_or(jb, "tempo_map", kEmptyArray);
    if (tempo_a != tempo_b) return unsupported("tempo_map");

    const json& tracks_a = ja.at("tracks");
    const json& tracks_b = jb.at("tracks");
    for (std::size_t i = 0; i < std::min(tracks_a.size(), tracks_b.size()); ++i)
        if (tracks_a[i].at("voice") != tracks_b[i].at("voice"))
            return unsupported("tracks[" + std::to_string(i) + "].voice");

    const json& patterns_a = ja.at("patterns");
    const json& patterns_b = jb.at("patterns");
    static const json kNoId = std::string();  // canonical default of pattern id (WP-0179): key omitted
    for (std::size_t i = 0; i < std::min(patterns_a.size(), patterns_b.size()); ++i) {
        if (patterns_a[i].at("track") != patterns_b[i].at("track"))
            return unsupported("patterns[" + std::to_string(i) + "].track");
        if (field_or(patterns_a[i], "id", kNoId) != field_or(patterns_b[i], "id", kNoId))
            return unsupported("patterns[" + std::to_string(i) + "].id");
    }

    const json& clips_a = field_or(ja, "arrangement", kEmptyArray);
    const json& clips_b = field_or(jb, "arrangement", kEmptyArray);
    // Operations can only append attachments: a's list must be a prefix of b's.
    const json& atts_a = field_or(ja, "attachments", kEmptyArray);
    const json& atts_b = field_or(jb, "attachments", kEmptyArray);
    if (atts_a.size() > atts_b.size()) return unsupported("attachments");
    for (std::size_t i = 0; i < atts_a.size(); ++i)
        if (atts_a[i] != atts_b[i]) return unsupported("attachments");

    // Everything that differs is expressible. Emit in the spec's fixed order — byte-stable.
    // Removals first, descending: erasing a tail never shifts any index we touch later, and
    // kept clips/patterns reference only b-valid indices (checked above), so no "in use" hits.
    for (std::size_t i = clips_a.size(); i-- > clips_b.size();)
        ops.push_back({{"op", "remove_clip"}, {"index", i}});
    // Step 1b (WP-0159): only when a has more patterns than b. Re-point shared clips at b's pattern
    // indices first, so that the tail patterns removed next are no longer "in use".
    const std::size_t min_clips = std::min(clips_a.size(), clips_b.size());
    const bool repoint_first = patterns_a.size() > patterns_b.size();
    if (repoint_first) {
        for (std::size_t i = 0; i < min_clips; ++i)
            if (clips_a[i].at("pattern") != clips_b[i].at("pattern"))
                ops.push_back({{"op", "set_clip_pattern"}, {"index", i}, {"pattern", clips_b[i].at("pattern")}});
    }
    for (std::size_t i = patterns_a.size(); i-- > patterns_b.size();)
        ops.push_back({{"op", "remove_pattern"}, {"index", i}});
    for (std::size_t i = tracks_a.size(); i-- > tracks_b.size();)
        ops.push_back({{"op", "remove_track"}, {"index", i}});
    // Loop region (WP-0155): absent counts as a distinct value, so any difference is emitted.
    const bool loop_differs = ja.contains("loop") != jb.contains("loop") ||
                              (ja.contains("loop") && ja.at("loop") != jb.at("loop"));
    // Step 3b: a loop is cleared before the length can change, then the target loop is set in 4b.
    if (loop_differs && ja.contains("loop")) ops.push_back({{"op", "clear_loop"}});

    static const json kZeroDb = 0.0;   // canonical default of master/clip gain_db
    static const json kFalse = false;  // canonical default of mute/solo (omitted at false)
    if (ja.at("bpm") != jb.at("bpm")) ops.push_back({{"op", "set_bpm"}, {"bpm", jb.at("bpm")}});
    if (ja.at("length_bars") != jb.at("length_bars"))
        ops.push_back({{"op", "set_length_bars"}, {"length_bars", jb.at("length_bars")}});
    const json& mg_a = field_or(ja, "master_gain_db", kZeroDb);
    const json& mg_b = field_or(jb, "master_gain_db", kZeroDb);
    if (mg_a != mg_b) ops.push_back({{"op", "set_master_gain"}, {"gain_db", mg_b}});
    // Step 4b: set the target loop after length changes (only when the loops differ).
    if (loop_differs && jb.contains("loop"))
        ops.push_back({{"op", "set_loop"},
                       {"start_bar", jb.at("loop").at("start_bar")},
                       {"end_bar", jb.at("loop").at("end_bar")}});

    for (std::size_t i = 0; i < std::min(tracks_a.size(), tracks_b.size()); ++i) {
        const json& ta = tracks_a[i];
        const json& tb = tracks_b[i];
        if (ta.at("name") != tb.at("name"))
            ops.push_back({{"op", "rename_track"}, {"index", i}, {"name", tb.at("name")}});
        if (ta.at("gain_db") != tb.at("gain_db"))
            ops.push_back({{"op", "set_track_gain"}, {"index", i}, {"gain_db", tb.at("gain_db")}});
        if (ta.at("pan") != tb.at("pan"))
            ops.push_back({{"op", "set_track_pan"}, {"index", i}, {"pan", tb.at("pan")}});
        const json& mute_b = field_or(tb, "mute", kFalse);
        if (field_or(ta, "mute", kFalse) != mute_b)
            ops.push_back({{"op", "set_track_mute"}, {"index", i}, {"mute", mute_b}});
        const json& solo_b = field_or(tb, "solo", kFalse);
        if (field_or(ta, "solo", kFalse) != solo_b)
            ops.push_back({{"op", "set_track_solo"}, {"index", i}, {"solo", solo_b}});
    }
    for (std::size_t i = tracks_a.size(); i < tracks_b.size(); ++i)
        ops.push_back({{"op", "add_track"}, {"track", tracks_b[i]}});

    // Per shared pattern: keep the longest common note prefix, replace everything after it —
    // removals descending (indices of earlier removals stay valid), additions ascending.
    for (std::size_t i = 0; i < std::min(patterns_a.size(), patterns_b.size()); ++i) {
        const json& notes_a = patterns_a[i].at("notes");
        const json& notes_b = patterns_b[i].at("notes");
        std::size_t k = 0;
        while (k < notes_a.size() && k < notes_b.size() && notes_a[k] == notes_b[k]) ++k;
        for (std::size_t j = notes_a.size(); j-- > k;)
            ops.push_back({{"op", "remove_note"}, {"pattern", i}, {"index", j}});
        for (std::size_t j = k; j < notes_b.size(); ++j)
            ops.push_back({{"op", "add_note"}, {"pattern", i}, {"note", notes_b[j]}});
        // Pattern swing (WP-0166, spec §opd diff step 7): after this pattern's note ops; absent = 0.
        const json& swing_b = field_or(patterns_b[i], "swing", kZeroDb);
        if (field_or(patterns_a[i], "swing", kZeroDb) != swing_b)
            ops.push_back({{"op", "set_pattern_swing"}, {"pattern", i}, {"swing", swing_b}});
    }
    // Step 7b (WP-0162): pattern length on shared patterns, after the note edits and before
    // add_pattern — patterns added by step 8 carry their own length_beats in the pattern JSON.
    for (std::size_t i = 0; i < std::min(patterns_a.size(), patterns_b.size()); ++i) {
        const bool has_a = patterns_a[i].contains("length_beats");
        const bool has_b = patterns_b[i].contains("length_beats");
        if (has_b && (!has_a || patterns_a[i].at("length_beats") != patterns_b[i].at("length_beats")))
            ops.push_back({{"op", "set_pattern_length"}, {"pattern", i},
                           {"length_beats", patterns_b[i].at("length_beats")}});
        else if (has_a && !has_b)
            ops.push_back({{"op", "clear_pattern_length"}, {"pattern", i}});
    }
    for (std::size_t i = patterns_a.size(); i < patterns_b.size(); ++i)
        ops.push_back({{"op", "add_pattern"}, {"pattern", patterns_b[i]}});

    // Shared clips, compared position by position (WP-0159): no duplicate_clip/reorder_clip is emitted.
    // Pattern is set here unless step 1b already did it; b's patterns all exist by now (step 8).
    for (std::size_t i = 0; i < min_clips; ++i) {
        if (!repoint_first && clips_a[i].at("pattern") != clips_b[i].at("pattern"))
            ops.push_back({{"op", "set_clip_pattern"}, {"index", i}, {"pattern", clips_b[i].at("pattern")}});
        if (clips_a[i].at("repeats") != clips_b[i].at("repeats"))
            ops.push_back({{"op", "set_clip_repeats"}, {"index", i}, {"repeats", clips_b[i].at("repeats")}});
        if (clips_a[i].at("start_bar") != clips_b[i].at("start_bar"))
            ops.push_back({{"op", "move_clip"}, {"index", i}, {"start_bar", clips_b[i].at("start_bar")}});
        const json& gain_b = field_or(clips_b[i], "gain_db", kZeroDb);
        if (field_or(clips_a[i], "gain_db", kZeroDb) != gain_b)
            ops.push_back({{"op", "set_clip_gain"}, {"index", i}, {"gain_db", gain_b}});
        // Clip fades (WP-0178): ONE op carrying both b values when either side differs (absent = 0.0).
        const json& fi_b = field_or(clips_b[i], "fade_in_beats", kZeroDb);
        const json& fo_b = field_or(clips_b[i], "fade_out_beats", kZeroDb);
        if (field_or(clips_a[i], "fade_in_beats", kZeroDb) != fi_b ||
            field_or(clips_a[i], "fade_out_beats", kZeroDb) != fo_b)
            ops.push_back({{"op", "set_clip_fades"}, {"index", i},
                           {"fade_in_beats", fi_b}, {"fade_out_beats", fo_b}});
    }
    for (std::size_t i = clips_a.size(); i < clips_b.size(); ++i)
        ops.push_back({{"op", "add_clip"}, {"clip", clips_b[i]}});

    static const json kEmptyString = std::string();
    for (std::size_t i = atts_a.size(); i < atts_b.size(); ++i) {
        const json& ab = atts_b[i];
        ops.push_back({{"op", "add_attachment"},
                       {"path", ab.at("path")},
                       {"kind", ab.at("kind")},
                       {"producer", field_or(ab, "producer", kEmptyString)}});
    }

    // Safety net (spec): apply our own journal to a copy of a; it must reproduce b exactly.
    Project work = a;
    for (const json& o : ops) {
        std::string apply_err;
        if (!apply_op(work, o, apply_err)) return unsupported("(journal does not reproduce target)");
    }
    if (project_to_json(work) != jb) return unsupported("(journal does not reproduce target)");
    return true;
}

// ---- step_grid (WP-0154): see engine/ops.hpp / docs/spec/operations.md §Engine view and CLI. ----
std::string step_grid(const Pattern& pattern, int steps) {
    if (steps <= 0) return std::string();
    std::string grid(static_cast<std::size_t>(steps), '.');
    for (const Note& n : pattern.notes) {
        if (!on_grid(n.t_beats)) continue;  // off-grid: never shown
        const double s = n.t_beats * 4.0;   // exact integral step index for on-grid notes
        if (s < static_cast<double>(steps)) grid[static_cast<std::size_t>(s)] = 'x';
    }
    return grid;
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
