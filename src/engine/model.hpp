// engine/model.hpp — project data model (spec/project-model.md v0). Parse/validate only; rendering lives in render.hpp.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace op {

enum class VoiceKind { Metronome, Sine, Sampler };

struct Voice {
    VoiceKind kind = VoiceKind::Metronome;
    float freq_hz = 440.0f;      // sine voice
    int accent_every = 4;        // metronome: downbeat every N beats
    std::string path;            // sampler: wav path (relative to project file dir)
};

struct Note {
    double t_beats = 0.0;
    double dur_beats = 0.25;
};

struct Pattern {
    int track = 0;               // index into Project::tracks
    std::vector<Note> notes;
};

struct Track {
    std::string name;
    float gain_db = 0.0f;
    float pan = 0.0f;            // -1..1 equal-power
    Voice voice;
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
};

// Load/validate per spec. Returns false + machine-parsable err string on any violation.
bool load_project(const std::string& path, Project& out, std::string& err);
bool save_project(const std::string& path, const Project& in, std::string& err);  // canonical (stable key order)

}  // namespace op
