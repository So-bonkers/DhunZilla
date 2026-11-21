// engine/model.hpp — project data model (spec/project-model.md v0). Parse/validate only; rendering lives in render.hpp.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace op {

enum class VoiceKind { Metronome, Sine, Sampler, Synth };

// Synth voice v1 oscillator (WP-0161, spec §Synth voice v1). Legacy = the pre-WP-0161 timbre
// (fundamental + 0.35 octave partial) — it is NOT the same as Sine.
enum class Waveform { Legacy, Sine, Saw, Square };

struct Voice {
    VoiceKind kind = VoiceKind::Metronome;
    float freq_hz = 440.0f;      // sine voice (synth: base A4 reference when note absent)
    int accent_every = 4;        // metronome: downbeat every N beats
    std::string path;            // sampler: wav path (relative to project file dir)
    double start_s = 0.0;        // sampler: slice start, seconds on the asset timeline (WP-0156)
    double end_s = 0.0;          // sampler: slice end, seconds; valid only when has_end_s (WP-0156)
    bool has_end_s = false;      // sampler: false = play to end of asset (WP-0156)
    int root_note = -1;          // sampler: MIDI pitch the asset sounds at; -1 = absent (no repitch, WP-0167)
    float attack_ms = 5.0f;      // synth: linear attack ramp (legacy envelope)
    float release_ms = 30.0f;    // synth: linear release ramp at note end (legacy envelope)
    // Synth voice v1 (WP-0161, spec §Synth voice v1). All defaults = "key absent" → legacy path.
    Waveform waveform = Waveform::Legacy;  // oscillator shape; Legacy = pre-WP-0161 timbre
    bool has_adsr = false;       // adsr object present → ADSR envelope with release tail
    double attack_s = 0.005;     // adsr: attack seconds [0, 10]
    double decay_s = 0.0;        // adsr: decay seconds [0, 10]
    double sustain = 1.0;        // adsr: sustain level [0, 1]
    double release_s = 0.030;    // adsr: release seconds [0, 10] (tail after note-off)
    double cutoff_hz = 0.0;      // biquad low-pass cutoff [20, 20000]; 0 = no filter (never from a file)
    double resonance = 0.7071;   // filter Q [0.5, 20]; omitted at exactly 0.7071 on save
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
    std::string id;              // optional name (WP-0179): "" = unnamed; non-empty ids unique
    double swing = 0.0;          // render-time groove, [0, 0.75] (WP-0166)
    std::vector<Note> notes;
    std::optional<double> length_beats;  // explicit loop period (WP-0162, spec §Explicit pattern
                                         // length): finite > 0; absent = derived from the notes
};

// Pattern extent (loop period) in beats: `*length_beats` when present, else
// max(note.t + note.dur) over its notes (0.0 for a pattern without notes). The single source of
// truth for repetition spacing — renderer and MIDI export both consult it. Example:
//   op::Pattern p; p.length_beats = 4.0;  op::pattern_extent(p);  // → 4.0
double pattern_extent(const Pattern& pat);

// Playlist clip: one placement of a pattern on the timeline (spec §arrangement).
// Pattern extent (loop period) = pattern_extent(): explicit length_beats, else max(note.t + note.dur).
struct Clip {
    int pattern = 0;             // index into Project::patterns
    double start_bar = 0.0;      // placement, bars from project start
    int repeats = 1;             // statements of the pattern, spaced by extent
    double gain_db = 0.0;        // per-clip level (WP-0120), [-96, 24]; 0 omitted on save → legacy bytes
    double fade_in_beats = 0.0;   // linear fade-in over this many beats from the clip start (WP-0178)
    double fade_out_beats = 0.0;  // linear fade-out over this many beats ending at the clip end (WP-0178)
};

// Bar-anchored piecewise-constant tempo change (WP-0105): from bar `bar` onward the project
// runs at `bpm`. Bars keep beats_per_bar beats across changes.
struct TempoPoint {
    double bar = 0.0;            // effective from this bar (>= 0)
    double bpm = 120.0;
};

struct Track {
    std::string name;
    float gain_db = 0.0f;        // [-96, 24], validated at parse (WP-0136 closed a pre-existing gap)
    float pan = 0.0f;            // -1..1 equal-power, validated at parse (WP-0136)
    bool mute = false;           // mixer (spec §mixer): silent regardless of solo; omitted at default
    bool solo = false;           // mixer: when any track solos, only non-muted solo tracks are audible
    int choke_group = 0;         // sampler choke group 1..16 (WP-0181); 0 = none
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

// Transport loop region (WP-0155, spec project-model.md §Loop region): bars [start_bar, end_bar),
// 0-based, fractional allowed. Live playback only; offline render ignores it.
struct LoopRegion {
    double start_bar = 0.0;
    double end_bar = 0.0;
};

// Time signature (WP-0191, task WP-0191 §Project JSON key): num quarter-note-beat multiples of a
// `den`-note. Only whole-quarter bar lengths load: (num * 4) % den == 0 and num * 4 / den <= 32.
// The engine maps bars through Project::beats_per_bar; this struct only records /8-and-/16
// signatures that beats_per_bar alone cannot carry (den != 4).
struct TimeSignature {
    int num = 4;
    int den = 4;
};

struct Project {
    int format_version = 0;
    double bpm = 120.0;
    int sample_rate = 48000;
    uint64_t seed = 42;
    double length_bars = 4.0;
    int beats_per_bar = 4;
    float master_gain_db = 0.0f;  // mixer master fader (spec §mixer): [-96, 24]; multiplies every
                                  // output sample after sum; exactly 0.0 skips → bit-identical legacy
    std::vector<Track> tracks;
    std::vector<Pattern> patterns;
    std::vector<Clip> arrangement;  // empty = legacy semantics: every pattern once, at bar 0
    std::vector<TempoPoint> tempo_map;  // empty = constant bpm (legacy bit-exact); else first bar must be 0
    std::vector<Attachment> attachments;  // empty when absent (legacy byte-stable); informational only
    std::optional<LoopRegion> loop;  // absent = no loop region (WP-0155); saved only when set
    std::optional<TimeSignature> time_signature;  // WP-0191: present only for den != 4 (3/4 normalizes to beats_per_bar)
};

// The signature a project plays with (WP-0191): its stored time_signature if present, else
// {beats_per_bar, 4}. Example: op::Project p; p.beats_per_bar = 6;
//   op::effective_time_signature(p);  // → {6, 4}
inline TimeSignature effective_time_signature(const Project& p) {
    return p.time_signature.value_or(TimeSignature{p.beats_per_bar, 4});
}

// Model-driven mutation op: validate and append an attachment. Rejects blank/absolute/escaping
// paths, kinds outside {stem, transcription, other}, and duplicate paths. Returns false + err on
// any violation with *p unchanged. Example:
//   op::Attachment a{"stems/drums.wav", "stem", "demucs"};  op::add_attachment(proj, a, err);
bool add_attachment(Project& p, Attachment a, std::string& err);

// Mixer audibility rule (spec §mixer): with any_solo = some track has solo == true, track i is
// audible iff !mute && (!any_solo || solo) — mute wins over solo. The single source of truth:
// mix and stems both consult it, so an inaudible stem stays all-zero by construction. Example:
//   for (size_t i = 0; i < p.tracks.size(); ++i) if (track_audible(p, i)) render_track(i);
bool track_audible(const Project& p, std::size_t i);

// New-project factory (WP-0165, spec project-model.md §New project — additive). Pure: no I/O, no
// clock, no randomness; builds the Project struct directly (never through Document) and does NOT
// validate ranges — callers do (e.g. `opd new`). Content is fixed by the spec: format_version 0,
// seed 42, one Pattern{track:i, notes:[]} + Clip{pattern:i, start_bar:0, repeats:1} per track.
struct NewTrackSpec {
    std::string name;                     // non-empty (validated by the caller, e.g. opd new)
    VoiceKind kind = VoiceKind::Synth;
    std::string path;                     // sampler only: stored verbatim as voice.path
};
struct NewProjectSpec {
    double bpm = 120.0;                   // (0, 1000]
    int bars = 4;                         // 1..4096  -> Project::length_bars
    int beats_per_bar = 4;                // 1..32
    int sample_rate = 48000;              // 8000..192000
    std::vector<NewTrackSpec> tracks;     // empty = the single default track {"Synth", Synth, ""}
};
Project new_project(const NewProjectSpec& spec);
// Example: op::NewProjectSpec s; s.bars = 8;
//          op::Project p = op::new_project(s);   // renders silence until notes are added

// Load/validate per spec. Returns false + machine-parsable err string on any violation.
bool load_project(const std::string& path, Project& out, std::string& err);

// The exact bytes save_project writes for `p`: canonical JSON (stable key order) dumped with
// indent 2 plus one trailing '\n'. Pure function of `p`; the single definition of the file format.
std::string project_bytes(const Project& p);

bool save_project(const std::string& path, const Project& in, std::string& err);  // canonical (stable key order)

}  // namespace op
