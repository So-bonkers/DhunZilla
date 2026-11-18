#include "engine/model.hpp"

#include <nlohmann/json.hpp>
#include <fstream>

namespace op {
namespace {

using nlohmann::json;

bool parse_voice(const json& j, Voice& v, std::string& err) {
    if (!j.is_object()) { err = "voice must be object"; return false; }
    const std::string kind = j.value("kind", "");
    if (kind == "metronome")      v.kind = VoiceKind::Metronome;
    else if (kind == "sine")      v.kind = VoiceKind::Sine;
    else if (kind == "sampler")   v.kind = VoiceKind::Sampler;
    else { err = "voice.kind unknown: '" + kind + "'"; return false; }
    v.freq_hz = j.value("freq_hz", 440.0f);
    v.accent_every = j.value("accent_every", 4);
    v.path = j.value("path", std::string{});
    if (v.kind == VoiceKind::Sampler && v.path.empty()) { err = "sampler voice requires path"; return false; }
    if (v.accent_every <= 0) { err = "accent_every must be > 0"; return false; }
    return true;
}

bool parse_project(const json& j, Project& p, std::string& err) {
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

    if (!j.contains("tracks") || !j["tracks"].is_array() || j["tracks"].empty()) {
        err = "tracks must be a non-empty array"; return false;
    }
    for (const auto& tj : j["tracks"]) {
        Track t;
        t.name = tj.value("name", std::string{});
        t.gain_db = tj.value("gain_db", 0.0f);
        t.pan = tj.value("pan", 0.0f);
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
                    if (n.t_beats < 0.0 || n.dur_beats <= 0.0) { err = "note needs t >= 0 and dur > 0"; return false; }
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
            p.arrangement.push_back(c);
        }
    }
    return true;
}

json voice_to_json(const Voice& v) {
    json j;
    // Stable key order (ordered_json would be needed for byte-stable dumps across nlohmann versions;
    // object() is insertion-ordered in nlohmann 3.11 — we insert explicitly.)
    j["kind"] = v.kind == VoiceKind::Metronome ? "metronome" : (v.kind == VoiceKind::Sine ? "sine" : "sampler");
    if (v.kind == VoiceKind::Sine) j["freq_hz"] = v.freq_hz;
    if (v.kind == VoiceKind::Metronome) j["accent_every"] = v.accent_every;
    if (v.kind == VoiceKind::Sampler) j["path"] = v.path;
    return j;
}

}  // namespace

bool load_project(const std::string& path, Project& out, std::string& err) {
    out = Project{};  // full reset: loading into a dirty Project must never accumulate state
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open '" + path + "'"; return false; }
    json j;
    try {
        j = json::parse(f);
    } catch (const json::exception& e) {
        err = std::string("json parse: ") + e.what();
        return false;
    }
    return parse_project(j, out, err);
}

bool save_project(const std::string& path, const Project& p, std::string& err) {
    json j;
    j["format_version"] = p.format_version;
    j["bpm"] = p.bpm;
    j["sample_rate"] = p.sample_rate;
    j["seed"] = p.seed;
    j["length_bars"] = p.length_bars;
    j["beats_per_bar"] = p.beats_per_bar;
    json tracks = json::array();
    for (const auto& t : p.tracks) {
        json tj;
        tj["name"] = t.name;
        tj["gain_db"] = t.gain_db;
        tj["pan"] = t.pan;
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
            notes.push_back({{"t", n.t_beats}, {"dur", n.dur_beats}});
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
            arr.push_back(std::move(cj));
        }
        j["arrangement"] = std::move(arr);
    }

    std::ofstream f(path, std::ios::binary);
    if (!f) { err = "cannot write '" + path + "'"; return false; }
    f << j.dump(2) << "\n";
    return true;
}

}  // namespace op
