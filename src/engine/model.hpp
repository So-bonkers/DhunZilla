// engine/model.hpp — project data model (spec/project-model.md v0). Parse/validate only; rendering lives in render.hpp.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace op {

enum class VoiceKind { Metronome, Sine, Sampler, Synth };

struct Voice {
    VoiceKind kind = VoiceKind::Metronome;
    float freq_hz = 440.0f;      // sine voice (synth: base A4 reference when note absent)
    int accent_every = 4;        // metronome: downbeat every N beats
    std::string path;            // sampler: wav path (relative to project file dir)
    float attack_ms = 5.0f;      // synth: linear attack ramp
    float release_ms = 30.0f;    // synth: linear release ramp at note end
};

struct Note {
    double t_beats = 0.0;
    double dur_beats = 0.25;
    int midi_note = -1;          // synth voice pitch (MIDI number, 0..127); -1 = absent → freq_hz
    double velocity = 1.0;       // per-note gain multiplier (0,1]; applied via mix-target scaling —
                                 // exactly 1.0 is bit-identical to pre-WP-0111 renders (spec §velocity)
};

struct Pattern {
    int track = 0;               // index into Project::tracks
    std::vector<Note> notes;
};

// Playlist clip: one placement of a pattern on the timeline (spec §arrangement).
// Pattern extent (loop period) = max(note.t + note.dur) over its notes, in beats.
struct Clip {
    int pattern = 0;             // index into Project::patterns
    double start_bar = 0.0;      // placement, bars from project start
    int repeats = 1;             // statements of the pattern, spaced by extent
    double gain_db = 0.0;        // per-clip level (WP-0120), [-96, 24]; 0 omitted on save → legacy bytes
};

// Bar-anchored piecewise-constant tempo change (WP-0105): from bar `bar` onward the project
// runs at `bpm`. Bars keep beats_per_bar beats across changes.
struct TempoPoint {
    double bar = 0.0;            // effective from this bar (>= 0)
    double bpm = 120.0;
};

struct Track {
    std::string name;
    float gain_db = 0.0f;
    float pan = 0.0f;            // -1..1 equal-power
    Voice voice;
};

// AI-artifact reference (WP-0116): a file that lives beside the project but is NOT part of the
// render graph — separated stems, transcriptions, reference renders. Purely informational: the
// renderer never reads attachments; .opd packing bundles those that exist on disk. The engine
// never invents them (no wall-clock, no side effects) — callers add via add_attachment().
struct Attachment {
    std::string path;      // project-relative, '/'-separated, no '..' segments
    std::string kind;      // "stem" | "transcription" | "other"
    std::string producer;  // free-form provenance: tool name/version/job id, e.g. "demucs ht-demucs"
};

struct Project {
    int format_version = 0;
    double bpm = 120.0;
    int sample_rate = 48000;
    uint64_t seed = 42;
    double length_bars = 4.0;
    int beats_per_bar = 4;
    std::vector<Track> tracks;
    std::vector<Pattern> patterns;
    std::vector<Clip> arrangement;  // empty = legacy semantics: every pattern once, at bar 0
    std::vector<TempoPoint> tempo_map;  // empty = constant bpm (legacy bit-exact); else first bar must be 0
    std::vector<Attachment> attachments;  // empty when absent (legacy byte-stable); informational only
};

// Model-driven mutation op: validate and append an attachment. Rejects blank/absolute/escaping
// paths, kinds outside {stem, transcription, other}, and duplicate paths. Returns false + err on
// any violation with *p unchanged. Example:
//   op::Attachment a{"stems/drums.wav", "stem", "demucs"};  op::add_attachment(proj, a, err);
bool add_attachment(Project& p, Attachment a, std::string& err);

// Load/validate per spec. Returns false + machine-parsable err string on any violation.
bool load_project(const std::string& path, Project& out, std::string& err);
bool save_project(const std::string& path, const Project& in, std::string& err);  // canonical (stable key order)

}  // namespace op
