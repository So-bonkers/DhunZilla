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
    // Synth voice v1 (WP-0161, docs/spec/project-model.md §Synth voice v1). Checked in the spec's
    // order, first failure wins, each rule only when its key is present. A JSON null fails the
    // type checks = type error (spec: "A null value in a project file is a type error").
    const bool has_waveform = j.contains("waveform");
    const bool has_adsr_key = j.contains("adsr");
    const bool has_cutoff = j.contains("cutoff_hz");
    const bool has_resonance = j.contains("resonance");
    if (has_waveform || has_adsr_key || has_cutoff || has_resonance) {
        // 1. Key present on a non-synth voice: report the first present key in spec order.
        if (v.kind != VoiceKind::Synth) {
            const char* first = has_waveform ? "waveform"
                              : has_adsr_key ? "adsr"
                              : has_cutoff   ? "cutoff_hz"
                                             : "resonance";
            err = std::string("voice.") + first + " is only valid for synth voices"; return false;
        }
        // 2. waveform: one of the three strings (any non-string is the same message).
        if (has_waveform) {
            const json& w = j.at("waveform");
            if (!w.is_string()) { err = "voice.waveform must be sine|saw|square"; return false; }
            const std::string ws = w.get<std::string>();
            if (ws == "sine") v.waveform = Waveform::Sine;
            else if (ws == "saw") v.waveform = Waveform::Saw;
            else if (ws == "square") v.waveform = Waveform::Square;
            else { err = "voice.waveform must be sine|saw|square"; return false; }
        }
        // 3. adsr: object; unknown inner keys first; then the four in spec order, each a number
        //    in range (the negated double comparison rejects NaN too).
        if (has_adsr_key) {
            const json& a = j.at("adsr");
            if (!a.is_object()) { err = "voice.adsr must be an object"; return false; }
            for (auto it = a.begin(); it != a.end(); ++it) {
                const std::string& k = it.key();
                if (k != "attack_s" && k != "decay_s" && k != "sustain" && k != "release_s") {
                    err = "voice.adsr has unknown key '" + k + "'"; return false;
                }
            }
            v.has_adsr = true;  // present-and-object → ADSR envelope, inner defaults for absent keys
            if (a.contains("attack_s")) {
                const json& x = a.at("attack_s");
                if (!x.is_number() || !(x.get<double>() >= 0.0 && x.get<double>() <= 10.0)) {
                    err = "voice.adsr.attack_s must be a number in [0, 10]"; return false;
                }
                v.attack_s = x.get<double>();
            }
            if (a.contains("decay_s")) {
                const json& x = a.at("decay_s");
                if (!x.is_number() || !(x.get<double>() >= 0.0 && x.get<double>() <= 10.0)) {
                    err = "voice.adsr.decay_s must be a number in [0, 10]"; return false;
                }
                v.decay_s = x.get<double>();
            }
            if (a.contains("sustain")) {
                const json& x = a.at("sustain");
                if (!x.is_number() || !(x.get<double>() >= 0.0 && x.get<double>() <= 1.0)) {
                    err = "voice.adsr.sustain must be a number in [0, 1]"; return false;
                }
                v.sustain = x.get<double>();
            }
            if (a.contains("release_s")) {
                const json& x = a.at("release_s");
                if (!x.is_number() || !(x.get<double>() >= 0.0 && x.get<double>() <= 10.0)) {
                    err = "voice.adsr.release_s must be a number in [0, 10]"; return false;
                }
                v.release_s = x.get<double>();
            }
        }
        // 4. cutoff_hz: a number in [20, 20000].
        if (has_cutoff) {
            const json& x = j.at("cutoff_hz");
            if (!x.is_number() || !(x.get<double>() >= 20.0 && x.get<double>() <= 20000.0)) {
                err = "voice.cutoff_hz must be a number in [20, 20000]"; return false;
            }
            v.cutoff_hz = x.get<double>();
        }
        // 5. resonance: a number in [0.5, 20].
        if (has_resonance) {
            const json& x = j.at("resonance");
            if (!x.is_number() || !(x.get<double>() >= 0.5 && x.get<double>() <= 20.0)) {
                err = "voice.resonance must be a number in [0.5, 20]"; return false;
            }
            v.resonance = x.get<double>();
        }
    }
    // Sampler repitch root (WP-0167, docs/spec/project-model.md §Sampler repitch): checked only when present.
    if (j.contains("root_note")) {
        if (v.kind != VoiceKind::Sampler) { err = "voice.root_note is only valid for sampler voices"; return false; }
        const json& rn = j.at("root_note");
        // nlohmann stores any integer literal that fits int64/uint64 as an integer (larger ones as float),
        // so a value passing is_number_integer() can be read below without overflow.
        bool in_range = false;
        if (rn.is_number_integer()) {
            if (rn.is_number_unsigned()) in_range = rn.get<uint64_t>() <= 127u;
            else in_range = rn.get<long long>() >= 0 && rn.get<long long>() <= 127;
        }
        // A null, a string, a float (60.0, 60.5) or an integer outside 0..127 all give the same message.
        if (!in_range) { err = "voice.root_note must be an integer 0..127"; return false; }
        v.root_note = static_cast<int>(rn.get<long long>());
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
        // Sampler choke group (WP-0181, spec project-model.md §Sampler choke groups): checked right
        // after solo, before the voice. Only a JSON integer in 1..16 passes — same unsigned/signed
        // read pattern as voice.root_note above; null/string/float/bool/out-of-range share one message.
        if (tj.contains("choke_group")) {
            const json& cg = tj.at("choke_group");
            bool ok = false;
            if (cg.is_number_integer()) {
                if (cg.is_number_unsigned()) ok = cg.get<uint64_t>() >= 1 && cg.get<uint64_t>() <= 16;
                else {
                    const long long iv = cg.get<long long>();
                    ok = iv >= 1 && iv <= 16;
                }
            }
            if (!ok) { err = "track.choke_group must be an integer in [1, 16]"; return false; }
            t.choke_group = static_cast<int>(cg.get<long long>());
        }
        if (!parse_voice(tj.value("voice", json::object()), t.voice, err)) return false;
        // Checked after the voice parsed (spec §Sampler choke groups rule 2).
        if (t.choke_group != 0 && t.voice.kind != VoiceKind::Sampler) {
            err = "track.choke_group is only valid for sampler tracks"; return false;
        }
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
            // Pattern id (WP-0179, spec project-model.md §Pattern ids): checked right after
            // pattern.track, before swing. First failure wins; uniqueness scans earlier patterns.
            if (pj.contains("id")) {
                const json& iv = pj.at("id");
                if (!iv.is_string()) { err = "pattern.id must be a string"; return false; }
                pat.id = iv.get<std::string>();
                if (pat.id.empty()) { err = "pattern.id must not be empty"; return false; }
                if (pat.id.size() > 64) { err = "pattern.id must be at most 64 bytes"; return false; }
                for (const Pattern& prev : p.patterns)
                    if (!prev.id.empty() && prev.id == pat.id) {
                        err = "pattern.id '" + pat.id + "' is already in use"; return false;
                    }
            }
            // Pattern swing (WP-0166, spec project-model.md §Pattern swing): checked right after
            // pattern.track, before the notes. -0.0 passes (< 0.0 is false for -0.0) and equals 0.
            if (pj.contains("swing")) {
                const json& sw = pj.at("swing");
                if (!sw.is_number() || !std::isfinite(sw.get<double>()) ||
                    sw.get<double>() < 0.0 || sw.get<double>() > 0.75) {
                    err = "pattern.swing must be a number in [0, 0.75]"; return false;
                }
                pat.swing = sw.get<double>();
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
            // Explicit pattern length (WP-0162, spec §Explicit pattern length): checked after the
            // notes; any JSON number read as double, finite and > 0 required, no upper bound.
            if (pj.contains("length_beats")) {
                const json& lv = pj.at("length_beats");
                if (!lv.is_number() || !std::isfinite(lv.get<double>()) || lv.get<double>() <= 0.0) {
                    err = "pattern.length_beats must be a finite number > 0"; return false;
                }
                pat.length_beats = lv.get<double>();
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
            // Clip fades (WP-0178, spec project-model.md §Clip fades): checked right after clip.gain_db,
            // fade-in before fade-out. -0.0 passes (< 0.0 is false for -0.0) and equals 0.
            if (cj.contains("fade_in_beats")) {
                const json& fv = cj.at("fade_in_beats");
                if (!fv.is_number() || !std::isfinite(fv.get<double>()) ||
                    fv.get<double>() < 0.0 || fv.get<double>() > 4096.0) {
                    err = "clip.fade_in_beats must be a number in [0, 4096]"; return false;
                }
                c.fade_in_beats = fv.get<double>();
            }
            if (cj.contains("fade_out_beats")) {
                const json& fv = cj.at("fade_out_beats");
                if (!fv.is_number() || !std::isfinite(fv.get<double>()) ||
                    fv.get<double>() < 0.0 || fv.get<double>() > 4096.0) {
                    err = "clip.fade_out_beats must be a number in [0, 4096]"; return false;
                }
                c.fade_out_beats = fv.get<double>();
            }
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
            if (v.root_note >= 0) j["root_note"] = v.root_note;  // WP-0167: absent root omitted
            break;
        case VoiceKind::Synth: {
            j["kind"] = "synth"; j["attack_ms"] = v.attack_ms; j["release_ms"] = v.release_ms;
            // WP-0161 canonical save (spec §Canonical save): each v1 key written only when set —
            // a legacy voice emits exactly the same bytes as before. adsr, when present, writes
            // all four inner keys even at their defaults; resonance only when != 0.7071.
            if (v.waveform != Waveform::Legacy) {
                j["waveform"] = v.waveform == Waveform::Sine ? "sine"
                              : v.waveform == Waveform::Saw  ? "saw"
                                                             : "square";
            }
            if (v.has_adsr) {
                json a;
                a["attack_s"] = v.attack_s;
                a["decay_s"] = v.decay_s;
                a["sustain"] = v.sustain;
                a["release_s"] = v.release_s;
                j["adsr"] = std::move(a);
            }
            if (v.cutoff_hz != 0.0) j["cutoff_hz"] = v.cutoff_hz;
            if (v.resonance != 0.7071) j["resonance"] = v.resonance;
            break;
        }
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

double pattern_extent(const Pattern& pat) {
    if (pat.length_beats) return *pat.length_beats;  // explicit loop period (WP-0162)
    double extent = 0.0;                             // derived: max(t + dur), unchanged legacy rule
    for (const Note& n : pat.notes) {
        const double e = n.t_beats + n.dur_beats;
        if (e > extent) extent = e;
    }
    return extent;
}

// WP-0165 (spec project-model.md §New project — additive): canonical new-project factory. Pure;
// builds the Project directly (Document-free), no validation (callers validate). Voice defaults in
// the struct already match the spec per kind (sine 440 Hz, synth 5/30 ms, metronome every 4); only
// the sampler path is copied through, verbatim, unchecked.
Project new_project(const NewProjectSpec& spec) {
    Project p;
    p.format_version = 0;
    p.bpm = spec.bpm;
    p.sample_rate = spec.sample_rate;
    p.seed = 42;
    p.length_bars = static_cast<double>(spec.bars);
    p.beats_per_bar = spec.beats_per_bar;
    const std::vector<NewTrackSpec> tracks = spec.tracks.empty()
        ? std::vector<NewTrackSpec>{{"Synth", VoiceKind::Synth, ""}}  // default track (spec)
        : spec.tracks;
    for (size_t i = 0; i < tracks.size(); ++i) {
        Track t;
        t.name = tracks[i].name;
        t.voice.kind = tracks[i].kind;
        if (tracks[i].kind == VoiceKind::Sampler) t.voice.path = tracks[i].path;
        p.tracks.push_back(std::move(t));
        Pattern pat;
        pat.track = static_cast<int>(i);
        p.patterns.push_back(std::move(pat));
        Clip c;
        c.pattern = static_cast<int>(i);
        c.start_bar = 0.0;
        c.repeats = 1;
        p.arrangement.push_back(c);
    }
    return p;
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
        if (t.choke_group != 0) tj["choke_group"] = t.choke_group;  // WP-0181: omitted when none → legacy bytes
        tj["voice"] = voice_to_json(t.voice);
        tracks.push_back(std::move(tj));
    }
    j["tracks"] = std::move(tracks);
    json patterns = json::array();
    for (const auto& pat : p.patterns) {
        json pj;
        pj["track"] = pat.track;
        if (!pat.id.empty()) pj["id"] = pat.id;  // WP-0179: omitted when unnamed → legacy bytes
        if (pat.length_beats) pj["length_beats"] = *pat.length_beats;  // WP-0162: omitted when absent → legacy bytes
        if (pat.swing != 0.0) pj["swing"] = pat.swing;  // WP-0166: omitted at default → legacy canonical bytes
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
            if (c.fade_in_beats != 0.0) cj["fade_in_beats"] = c.fade_in_beats;   // WP-0178: omitted at default
            if (c.fade_out_beats != 0.0) cj["fade_out_beats"] = c.fade_out_beats;  // WP-0178: omitted at default
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
