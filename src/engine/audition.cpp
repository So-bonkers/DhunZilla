// engine/audition.cpp — WP-0209. Pure model construction only: every output sample is produced by
// the existing sampler voice inside op::render_project (resample -> trim -> repitch), so an
// audition render is bit-exact with the equivalent one-track project render. No new DSP.
#include "engine/audition.hpp"

#include <algorithm>
#include <cmath>

#include "engine/render.hpp"

namespace op::audition {

long long to_frames(double seconds) {
    // Same rule as the renderer's trim lambda (render.cpp, WP-0156): clamp the product before
    // llround so absurd magnitudes stay defined.
    double x = seconds * static_cast<double>(kSampleRate);
    if (x > 4e18) x = 4e18;
    if (x < -4e18) x = -4e18;
    return static_cast<long long>(std::llround(x));
}

Project build_project(const Options& o) {
    const long long n = std::max(o.frames, 1LL);
    // kBpm == 5625/64 at kSampleRate 48000 makes spb exactly 32768.0, so dur_beats = n / 32768.0
    // is exact (power-of-two division) and both total_samples_for and the note length land back on
    // exactly n frames — no off-by-one frame of silence, ever (see WP-0209 task file).
    const double spb = static_cast<double>(kSampleRate) * 60.0 / kBpm;
    const double dur_beats = static_cast<double>(n) / spb;

    Project p;
    p.format_version = 0;
    p.bpm = kBpm;
    p.sample_rate = kSampleRate;
    p.seed = 42;
    p.beats_per_bar = kBeatsPerBar;
    p.length_bars = dur_beats / static_cast<double>(kBeatsPerBar);

    Track t;
    t.name = "audition";
    t.voice.kind = VoiceKind::Sampler;
    t.voice.path = o.path;  // verbatim — the renderer resolves it against base_dir
    t.voice.start_s = o.start_s;
    t.voice.end_s = o.end_s;
    t.voice.has_end_s = o.has_end_s;
    t.voice.root_note = o.root;
    p.tracks.push_back(t);

    Pattern pat;
    pat.track = 0;
    Note note;
    note.t_beats = 0.0;
    note.dur_beats = dur_beats;
    note.midi_note = o.note;
    note.velocity = 1.0;
    pat.notes.push_back(note);
    p.patterns.push_back(pat);

    // p.arrangement and p.tempo_map stay EMPTY (legacy placement of pattern 0 once at bar 0;
    // constant-bpm path). master_gain_db stays 0.0f.
    return p;
}

bool render(const Options& o, const std::string& base_dir, wav::StereoBuffer& out, std::string& err) {
    const Project p = build_project(o);
    return op::render_project(p, base_dir, out, err);
}

}  // namespace op::audition
