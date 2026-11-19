#include "engine/model.hpp"

#include <nlohmann/json.hpp>
#include <cmath>
#include <fstream>

#include "engine/model_json.hpp"

namespace op {

// File-local helper (defined below): shared by the parser and add_attachment.
bool valid_attachment(const Attachment& a, std::string& err);
namespace {

using nlohmann::json;

bool parse_voice(const json& j, Voice& v, std::string& err) {
    if (!j.is_object()) { err = "voice must be object"; return false; }
    const std::string kind = j.value("kind", "");
    if (kind == "metronome")      v.kind = VoiceKind::Metronome;
    else if (kind == "sine")      v.kind = VoiceKind::Sine;
    else if (kind == "sampler")   v.kind = VoiceKind::Sampler;
    else if (kind == "synth")     v.kind = VoiceKind::Synth;
    else { err = "voice.kind unknown: '" + kind + "'"; return false; }
    v.freq_hz = j.value("freq_hz", 440.0f);
    v.accent_every = j.value("accent_every", 4);
    v.path = j.value("path", std::string{});
    v.attack_ms = j.value("attack_ms", 5.0f);
    v.release_ms = j.value("release_ms", 30.0f);
    if (v.kind == VoiceKind::Sampler && v.path.empty()) { err = "sampler voice requires path"; return false; }
    if (v.accent_every <= 0) { err = "accent_every must be > 0"; return false; }
    if (!(v.attack_ms >= 0.0f) || !(v.release_ms >= 0.0f)) { err = "attack_ms/release_ms must be >= 0"; return false; }
    // Sampler trim (WP-0156, docs/spec/project-model.md §Sampler trim). Checked only when a key is present.
    const bool has_start = j.contains("start_s");
    const bool has_end = j.contains("end_s");
    if (has_start || has_end) {
        if (v.kind != VoiceKind::Sampler) { err = "voice.start_s/end_s are only valid for sampler voices"; return false; }
        if (has_start) {
            const json& s = j.at("start_s");
            if (!s.is_number() || !std::isfinite(s.get<double>()) || s.get<double>() < 0.0) {
                err = "voice.start_s must be a finite number >= 0"; return false;
            }
            v.start_s = s.get<double>();
        }
        if (has_end) {
            const json& e = j.at("end_s");
            if (!e.is_number() || !std::isfinite(e.get<double>()) || e.get<double>() <= 0.0) {
                err = "voice.end_s must be a finite number > 0"; return false;
            }
            v.end_s = e.get<double>();
            v.has_end_s = true;
            if (v.end_s <= v.start_s) { err = "voice.end_s must be greater than voice.start_s"; return false; }
        }
    }
    return true;
}

}  // namespace

// Engine-internal (model_json.hpp): the exact parse+validate path load_project uses. May throw
// nlohmann::json::exception on mistyped fields — callers wrap (load_project, ops.cpp).
bool project_from_json(const json& j, Project& p, std::string& err) {
    p.format_version = j.value("format_version", -1);
    if (p.format_version != 0) { err = "unsupported format_version: " + std::to_string(p.format_version); return false; }
    p.bpm = j.value("bpm", 120.0);
    p.sample_rate = j.value("sample_rate", 48000);
    p.seed = j.value("seed", 42ull);
    p.length_bars = j.value("length_bars", 4.0);
    p.beats_per_bar = j.value("beats_per_bar", 4);
    if (p.bpm <= 0.0) { err = "bpm must be > 0"; return false; }
    if (p.sample_rate <= 0) { err = "sample_rate must be > 0"; return false; }
    if (p.beats_per_bar <= 0) { err = "beats_per_bar must be > 0"; return false; }
    if (p.length_bars <= 0.0) { err = "length_bars must be > 0"; return false; }
    p.master_gain_db = j.value("master_gain_db", 0.0f);  // mixer master fader (spec §mixer)
    if (!(p.master_gain_db >= -96.0f) || p.master_gain_db > 24.0f) {  // NaN-safe: rejects NaN too
        err = "master_gain_db out of range [-96, 24]"; return false;
    }

    if (!j.contains("tracks") || !j["tracks"].is_array() || j["tracks"].empty()) {
        err = "tracks must be a non-empty array"; return false;
    }
    for (const auto& tj : j["tracks"]) {
        Track t;
        t.name = tj.value("name", std::string{});
        t.gain_db = tj.value("gain_db", 0.0f);
        t.pan = tj.value("pan", 0.0f);
        // WP-0136: close the pre-existing gap — gain/pan used to load unbounded (+99 dB, pan 5).
        if (!(t.gain_db >= -96.0f) || t.gain_db > 24.0f) { err = "track.gain_db out of range [-96, 24]"; return false; }
        if (!(t.pan >= -1.0f) || t.pan > 1.0f) { err = "track.pan out of range [-1, 1]"; return false; }
        if (tj.contains("mute")) {
            if (!tj.at("mute").is_boolean()) { err = "track.mute must be a boolean"; return false; }
            t.mute = tj.at("mute").get<bool>();
        }
        if (tj.contains("solo")) {
            if (!tj.at("solo").is_boolean()) { err = "track.solo must be a boolean"; return false; }
            t.solo = tj.at("solo").get<bool>();
        }
        if (!parse_voice(tj.value("voice", json::object()), t.voice, err)) return false;
        p.tracks.push_back(std::move(t));
    }

    if (j.contains("patterns")) {
        if (!j["patterns"].is_array()) { err = "patterns must be an array"; return false; }
        for (const auto& pj : j["patterns"]) {
            Pattern pat;
            pat.track = pj.value("track", -1);
            if (pat.track < 0 || static_cast<size_t>(pat.track) >= p.tracks.size()) {
                err = "pattern.track out of range: " + std::to_string(pat.track); return false;
            }
            if (pj.contains("notes") && pj["notes"].is_array()) {
                for (const auto& nj : pj["notes"]) {
                    Note n;
                    n.t_beats = nj.value("t", 0.0);
                    n.dur_beats = nj.value("dur", 0.25);
                    n.midi_note = nj.value("note", -1);
                    n.velocity = nj.value("vel", 1.0);
                    if (n.t_beats < 0.0 || n.dur_beats <= 0.0) { err = "note needs t >= 0 and dur > 0"; return false; }
                    if (n.midi_note < -1 || n.midi_note > 127) { err = "note.note must be a MIDI number 0..127 (or absent)"; return false; }
                    if (!(n.velocity > 0.0) || n.velocity > 1.0) { err = "note.vel must be in (0, 1]"; return false; }
                    pat.notes.push_back(n);
                }
            }
            p.patterns.push_back(std::move(pat));
        }
    }

    if (j.contains("arrangement")) {
        if (!j["arrangement"].is_array()) { err = "arrangement must be an array"; return false; }
        for (const auto& cj : j["arrangement"]) {
            Clip c;
            c.pattern = cj.value("pattern", -1);
            if (c.pattern < 0 || static_cast<size_t>(c.pattern) >= p.patterns.size()) {
                err = "clip.pattern out of range: " + std::to_string(c.pattern); return false;
            }
            c.start_bar = cj.value("start_bar", 0.0);
            if (!(c.start_bar >= 0.0)) { err = "clip.start_bar must be a finite number >= 0"; return false; }
            c.repeats = cj.value("repeats", 1);
            if (c.repeats < 1 || c.repeats > 4096) { err = "clip.repeats out of range [1..4096]"; return false; }
            c.gain_db = cj.value("gain_db", 0.0);
            if (!(c.gain_db >= -96.0) || c.gain_db > 24.0) { err = "clip.gain_db out of range [-96, 24]"; return false; }
            p.arrangement.push_back(c);
        }
    }

    if (j.contains("tempo_map")) {
        if (!j["tempo_map"].is_array()) { err = "tempo_map must be an array"; return false; }
        double prev_bar = -1.0;
        for (const auto& tj : j["tempo_map"]) {
            TempoPoint tp;
            tp.bar = tj.value("bar", 0.0);
            tp.bpm = tj.value("bpm", 0.0);
            if (!(tp.bar >= 0.0)) { err = "tempo_map.bar must be a finite number >= 0"; return false; }
            if (!(tp.bpm > 0.0) || !(tp.bpm <= 1000.0)) { err = "tempo_map.bpm out of range (0, 1000]"; return false; }
            if (tp.bar <= prev_bar) { err = "tempo_map bars must be strictly increasing"; return false; }
            prev_bar = tp.bar;
            p.tempo_map.push_back(tp);
        }
        if (!p.tempo_map.empty() && p.tempo_map[0].bar != 0.0) {
            err = "tempo_map must start at bar 0 — prepend {\"bar\": 0, \"bpm\": <initial>}"; return false;
        }
    }
    if (j.contains("loop")) {  // WP-0155: optional transport loop region (spec project-model.md §Loop region)
        const json& lj = j["loop"];
        if (!lj.is_object()) { err = "loop must be an object"; return false; }
        if (!lj.contains("start_bar") || !lj.contains("end_bar") ||
            !lj["start_bar"].is_number() || !lj["end_bar"].is_number()) {
            err = "loop.start_bar and loop.end_bar must be numbers"; return false;
        }
        LoopRegion lr;
        lr.start_bar = lj["start_bar"].get<double>();
        lr.end_bar = lj["end_bar"].get<double>();
        if (!(lr.start_bar >= 0.0) || !std::isfinite(lr.start_bar)) {  // NaN-safe
            err = "loop.start_bar must be a finite number >= 0"; return false;
        }
        if (!std::isfinite(lr.end_bar)) { err = "loop.end_bar must be a finite number"; return false; }
        if (lr.end_bar <= lr.start_bar) { err = "loop.end_bar must be greater than loop.start_bar"; return false; }
        if (lr.end_bar > p.length_bars) { err = "loop.end_bar must be <= length_bars"; return false; }
        for (auto it = lj.begin(); it != lj.end(); ++it) {
            if (it.key() != "start_bar" && it.key() != "end_bar") {
                err = "loop has unknown key '" + it.key() + "'"; return false;
            }
        }
        p.loop = lr;
    }
    if (j.contains("attachments")) {  // WP-0116: informational AI-artifact references
        if (!j["attachments"].is_array()) { err = "attachments must be an array"; return false; }
        for (const auto& aj : j["attachments"]) {
            Attachment a;
            a.path = aj.value("path", "");
            a.kind = aj.value("kind", "");
            a.producer = aj.value("producer", "");
            if (!valid_attachment(a, err)) return false;
            p.attachments.push_back(std::move(a));
        }
    }
    return true;
}

namespace {

json voice_to_json(const Voice& v) {
    json j;
    // Stable key order (ordered_json would be needed for byte-stable dumps across nlohmann versions;
    // object() is insertion-ordered in nlohmann 3.11 — we insert explicitly.)
    switch (v.kind) {
        case VoiceKind::Metronome: j["kind"] = "metronome"; j["accent_every"] = v.accent_every; break;
        case VoiceKind::Sine:      j["kind"] = "sine"; j["freq_hz"] = v.freq_hz; break;
        case VoiceKind::Sampler:
            j["kind"] = "sampler";
            j["path"] = v.path;
            if (v.start_s != 0.0) j["start_s"] = v.start_s;   // WP-0156: default keys omitted
            if (v.has_end_s) j["end_s"] = v.end_s;
            break;
        case VoiceKind::Synth:     j["kind"] = "synth"; j["attack_ms"] = v.attack_ms; j["release_ms"] = v.release_ms; break;
    }
    return j;
}

}  // namespace

bool valid_attachment(const Attachment& a, std::string& err) {
    if (a.path.empty()) { err = "attachment.path must not be empty"; return false; }
    if (a.path[0] == '/' || a.path.find('\\') != std::string::npos) {
        err = "attachment.path must be project-relative with '/' separators: '" + a.path + "'"; return false;
    }
    size_t pos = 0;
    while (pos < a.path.size()) {
        const size_t slash = a.path.find('/', pos);
        const std::string seg = a.path.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
        if (seg == "..") { err = "attachment.path must not escape the project dir: '" + a.path + "'"; return false; }
        if (slash == std::string::npos) break;
        pos = slash + 1;
    }
    if (a.kind != "stem" && a.kind != "transcription" && a.kind != "other") {
        err = "attachment.kind must be stem|transcription|other, got '" + a.kind + "'"; return false;
    }
    return true;
}

bool add_attachment(Project& p, Attachment a, std::string& err) {
    if (!valid_attachment(a, err)) return false;
    for (const auto& e : p.attachments)
        if (e.path == a.path) { err = "attachment path already present: '" + a.path + "'"; return false; }
    p.attachments.push_back(std::move(a));
    return true;
}

bool track_audible(const Project& p, std::size_t i) {
    bool any_solo = false;
    for (const Track& t : p.tracks) {
        if (t.solo) { any_solo = true; break; }
    }
    const Track& t = p.tracks[i];
    return !t.mute && (!any_solo || t.solo);  // mute wins over solo (spec §mixer)
}

bool load_project(const std::string& path, Project& out, std::string& err) {
    out = Project{};  // full reset: loading into a dirty Project must never accumulate state
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open '" + path + "'"; return false; }
    json j;
    try {
        j = json::parse(f);
        // project_from_json's .value() calls throw type_error on mistyped fields (WP-0122 fuzz: 22 such
        // paths) — same failure contract as a syntax error. Parse into tmp so a throw mid-way can
        // never leave a half-filled project in *out.
        Project tmp;
        if (!project_from_json(j, tmp, err)) return false;
        out = std::move(tmp);
        return true;
    } catch (const json::exception& e) {
        err = std::string("json: ") + e.what();
        return false;
    }
}

// Engine-internal (model_json.hpp): builds the canonical JSON object save_project writes
// (stable insertion order, omit-at-default rules). Pure; never throws.
json project_to_json(const Project& p) {
    json j;
    j["format_version"] = p.format_version;
    j["bpm"] = p.bpm;
    j["sample_rate"] = p.sample_rate;
    j["seed"] = p.seed;
    j["length_bars"] = p.length_bars;
    j["beats_per_bar"] = p.beats_per_bar;
    if (p.master_gain_db != 0.0f) j["master_gain_db"] = p.master_gain_db;  // omit at default (spec §mixer)
    json tracks = json::array();
    for (const auto& t : p.tracks) {
        json tj;
        tj["name"] = t.name;
        tj["gain_db"] = t.gain_db;
        tj["pan"] = t.pan;
        if (t.mute) tj["mute"] = true;  // omitted at default → legacy canonical bytes unchanged
        if (t.solo) tj["solo"] = true;
        tj["voice"] = voice_to_json(t.voice);
        tracks.push_back(std::move(tj));
    }
    j["tracks"] = std::move(tracks);
    json patterns = json::array();
    for (const auto& pat : p.patterns) {
        json pj;
        pj["track"] = pat.track;
        json notes = json::array();
        for (const auto& n : pat.notes) {
            json nj;
            nj["t"] = n.t_beats;
            nj["dur"] = n.dur_beats;
            if (n.midi_note >= 0) nj["note"] = n.midi_note;  // omitted when absent → legacy output byte-identical
            if (n.velocity != 1.0) nj["vel"] = n.velocity;   // same rule for velocity (spec §velocity)
            notes.push_back(std::move(nj));
        }
        pj["notes"] = std::move(notes);
        patterns.push_back(std::move(pj));
    }
    j["patterns"] = std::move(patterns);

    if (!p.arrangement.empty()) {  // omitted when empty → canonical output of legacy projects unchanged
        json arr = json::array();
        for (const auto& c : p.arrangement) {
            json cj;
            cj["pattern"] = c.pattern;
            cj["start_bar"] = c.start_bar;
            cj["repeats"] = c.repeats;
            if (c.gain_db != 0.0) cj["gain_db"] = c.gain_db;  // omitted at unity → legacy canonical bytes
            arr.push_back(std::move(cj));
        }
        j["arrangement"] = std::move(arr);
    }

    if (!p.tempo_map.empty()) {  // omitted when empty → canonical output of legacy projects unchanged
        json tm = json::array();
        for (const auto& tp : p.tempo_map) tm.push_back({{"bar", tp.bar}, {"bpm", tp.bpm}});
        j["tempo_map"] = std::move(tm);
    }
    if (p.loop) {  // WP-0155: omitted when absent → legacy canonical bytes unchanged
        j["loop"] = json{{"start_bar", p.loop->start_bar}, {"end_bar", p.loop->end_bar}};
    }
    if (!p.attachments.empty()) {  // omitted when empty → legacy canonical bytes unchanged
        json arr = json::array();
        for (const auto& a : p.attachments) {
            json aj;
            aj["path"] = a.path;
            aj["kind"] = a.kind;
            if (!a.producer.empty()) aj["producer"] = a.producer;
            arr.push_back(std::move(aj));
        }
        j["attachments"] = std::move(arr);
    }
    return j;
}

std::string project_bytes(const Project& p) { return project_to_json(p).dump(2) + "\n"; }

bool save_project(const std::string& path, const Project& p, std::string& err) {
    std::ofstream f(path, std::ios::binary);
    if (!f) { err = "cannot write '" + path + "'"; return false; }
    f << project_bytes(p);
    return true;
}

}  // namespace op
