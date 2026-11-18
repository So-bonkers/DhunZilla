#include "engine/render.hpp"

#include <cmath>
#include <map>
#include <vector>

#include "engine/dsp.hpp"

namespace op {
namespace {

constexpr double kMaxRenderSeconds = 600.0;  // sanity cap for offline v0

// Voice renderers add into interleaved stereo `out` at absolute frame offset, applying track gain+pan.
struct MixTarget {
    std::vector<float>* out;   // interleaved L,R
    long long total_frames;
    float gain_l, gain_r;
};

inline void add_stereo(MixTarget& t, long long frame, float l, float r) {
    if (frame < 0 || frame >= t.total_frames) return;
    (*t.out)[frame * 2 + 0] += l * t.gain_l;
    (*t.out)[frame * 2 + 1] += r * t.gain_r;
}

inline float hann_attack(long long n, long long attack_samples) {
    if (attack_samples <= 0) return 1.0f;
    if (n >= attack_samples) return 1.0f;
    return 0.5f * (1.0f - dsp::cosf_(dsp::kPi() * static_cast<float>(n) / static_cast<float>(attack_samples)));
}

void render_metronome(MixTarget& t, long long start, const Note& note, const Voice& v, int sr) {
    const long long beat_idx = static_cast<long long>(std::llround(note.t_beats));
    const float freq = ((beat_idx % v.accent_every) == 0 ? 1000.0f : 800.0f);
    const long long len = static_cast<long long>(0.030 * sr);          // spec: hard stop at 30 ms
    const long long attack = static_cast<long long>(0.005 * sr);       // Hann ramp 5 ms
    const float inv_tau = 1.0f / static_cast<float>(sr * 0.008);       // tau = 8 ms
    const float dphi = dsp::twoPi() * freq / static_cast<float>(sr);
    for (long long n = 0; n < len; ++n) {
        const float env = hann_attack(n, attack) * dsp::expf_(-static_cast<float>(n) * inv_tau);
        const float s = dsp::sinf_(dphi * static_cast<float>(n)) * env;
        add_stereo(t, start + n, s, s);
    }
}

void render_sine(MixTarget& t, long long start, const Note& note, const Voice& v, int sr, double spb) {
    long long len = static_cast<long long>(std::llround(note.dur_beats * spb));
    if (len <= 0) return;
    const float dphi = dsp::twoPi() * v.freq_hz / static_cast<float>(sr);
    const long long edge = static_cast<long long>((len < 2 * static_cast<long long>(0.005 * sr)) ? len / 2 : 0.005 * sr);
    for (long long n = 0; n < len; ++n) {
        const float env = hann_attack(n, edge) * hann_attack(len - 1 - n, edge);
        const float s = dsp::sinf_(dphi * static_cast<float>(n)) * env;
        add_stereo(t, start + n, s, s);
    }
}

// synth voice (spec §synth): pitch from note.midi_note (12-TET, A4=440), fallback voice.freq_hz.
// Fundamental + phase-locked octave partial (amplitude 0.35, normalized); linear attack, sustain,
// linear release ramp. Pure dsp wrappers → deterministic like every other voice.
void render_synth(MixTarget& t, long long start, const Note& note, const Voice& v, int sr, double spb) {
    long long len = static_cast<long long>(std::llround(note.dur_beats * spb));
    if (len <= 0) return;
    const float f = (note.midi_note >= 0)
        ? 440.0f * dsp::powf_(2.0f, static_cast<float>(note.midi_note - 69) / 12.0f)
        : v.freq_hz;
    const long long atk = static_cast<long long>(v.attack_ms * 0.001f * static_cast<float>(sr));
    const long long rel = static_cast<long long>(v.release_ms * 0.001f * static_cast<float>(sr));
    const float dphi = dsp::twoPi() * f / static_cast<float>(sr);
    const float dphi_oct = 2.0f * dphi;  // integer multiple → phase-locked, no extra state
    constexpr float kNorm = 1.0f / 1.35f;
    for (long long n = 0; n < len; ++n) {
        float env = 1.0f;
        if (atk > 0 && n < atk) env = static_cast<float>(n) / static_cast<float>(atk);
        const long long since_end = len - n;
        if (rel > 0 && since_end <= rel) env *= static_cast<float>(since_end) / static_cast<float>(rel);
        const float s = (dsp::sinf_(dphi * static_cast<float>(n)) +
                         0.35f * dsp::sinf_(dphi_oct * static_cast<float>(n))) * kNorm * env;
        add_stereo(t, start + n, s, s);
    }
}

bool render_sampler(MixTarget& t, long long start, const Note& note, const std::string& abs_path, int sr,
                    double spb, std::map<std::string, wav::ReadResult>& cache, std::string& err) {
    auto it = cache.find(abs_path);
    if (it == cache.end()) {
        wav::ReadResult rr = wav::read_wav(abs_path);
        if (!rr.ok) { err = "sampler: " + rr.error; return false; }
        if (rr.sample_rate != sr) { err = "sampler: sample-rate mismatch (v0 requires project rate)"; return false; }
        it = cache.emplace(abs_path, std::move(rr)).first;
    }
    const wav::ReadResult& rr = it->second;
    long long avail = static_cast<long long>(rr.samples.size()) / rr.channels;
    long long want = static_cast<long long>(std::llround(note.dur_beats * spb));
    long long len = avail < want ? avail : want;
    for (long long n = 0; n < len; ++n) {
        float l, r;
        if (rr.channels == 1) { l = r = rr.samples[n]; }
        else { l = rr.samples[n * 2]; r = rr.samples[n * 2 + 1]; }
        add_stereo(t, start + n, l, r);
    }
    return true;
}

std::string join_path(const std::string& dir, const std::string& rel) {
    if (!rel.empty() && rel[0] == '/') return rel;  // absolute path passthrough
    if (dir.empty()) return rel;
    if (dir.back() == '/') return dir + rel;
    return dir + "/" + rel;
}

// ---------------------------------------------------------------- tempo map (WP-0105)
// Bar-anchored piecewise-constant tempo. Empty segments = constant bpm: at() then computes
// exactly llround(beats * spb_const) — the legacy expression, so legacy renders stay bit-exact.
struct TempoSeg {
    double start_beats = 0.0;      // segment start on the beat timeline
    long long start_sample = 0;    // exact accumulated sample at segment start
    double spb = 0.0;              // samples per beat inside this segment
};

struct TempoMap {
    std::vector<TempoSeg> segs;    // empty → constant-tempo legacy path
    double spb_const = 0.0;

    size_t seg_index(double beats) const {
        size_t i = 0;
        for (size_t j = 0; j < segs.size(); ++j)
            if (beats >= segs[j].start_beats) i = j; else break;
        return i;
    }
    long long at(double beats) const {
        if (segs.empty()) return std::llround(beats * spb_const);
        const TempoSeg& s = segs[seg_index(beats)];
        return s.start_sample + static_cast<long long>(std::llround((beats - s.start_beats) * s.spb));
    }
    double spb_at(double beats) const {  // duration simplification: local tempo at note start (spec §tempo)
        if (segs.empty()) return spb_const;
        return segs[seg_index(beats)].spb;
    }
};

TempoMap build_tempo_map(const Project& p, int sr) {
    TempoMap tm;
    if (p.tempo_map.empty()) {
        tm.spb_const = static_cast<double>(sr) * 60.0 / p.bpm;
        return tm;
    }
    const double bpb = static_cast<double>(p.beats_per_bar);
    double seg_b0 = p.tempo_map[0].bar * bpb;  // model validator guarantees first point at bar 0
    double seg_spb = static_cast<double>(sr) * 60.0 / p.tempo_map[0].bpm;
    long long cum = 0;
    for (size_t i = 1; i < p.tempo_map.size(); ++i) {
        const double next_b = p.tempo_map[i].bar * bpb;
        tm.segs.push_back({seg_b0, cum, seg_spb});
        cum += static_cast<long long>(std::llround((next_b - seg_b0) * seg_spb));
        seg_b0 = next_b;
        seg_spb = static_cast<double>(sr) * 60.0 / p.tempo_map[i].bpm;
    }
    tm.segs.push_back({seg_b0, cum, seg_spb});
    return tm;
}

}  // namespace

long long total_samples_for(const Project& p) {
    const double beats = p.length_bars * static_cast<double>(p.beats_per_bar);
    if (p.tempo_map.empty())
        return static_cast<long long>(std::ceil(beats * static_cast<double>(p.sample_rate) * 60.0 / p.bpm));
    // Piecewise: full segments accumulate llround samples (same rule as placement), final partial ceil.
    const TempoMap tm = build_tempo_map(p, p.sample_rate);
    for (size_t i = 1; i < tm.segs.size(); ++i)
        if (beats < tm.segs[i].start_beats)
            return tm.segs[i - 1].start_sample +
                   static_cast<long long>(std::ceil((beats - tm.segs[i - 1].start_beats) * tm.segs[i - 1].spb));
    const TempoSeg& last = tm.segs.back();
    return last.start_sample + static_cast<long long>(std::ceil((beats - last.start_beats) * last.spb));
}

bool render_subset(const Project& p, const std::string& base_dir, const std::vector<size_t>* subset,
                   wav::StereoBuffer& out, std::string& err) {
    const int sr = p.sample_rate;
    const long long total = total_samples_for(p);
    if (total <= 0) { err = "render_error: zero-length render"; return false; }
    if (static_cast<double>(total) / static_cast<double>(sr) > kMaxRenderSeconds) {
        err = "render_error: project exceeds 600s offline cap"; return false;
    }

    out.sample_rate = sr;
    out.interleaved.assign(static_cast<size_t>(total) * 2u, 0.0f);
    const TempoMap tmap = build_tempo_map(p, sr);  // constant bpm → legacy bit-exact math

    std::map<std::string, wav::ReadResult> sampler_cache;

    const size_t ntracks = p.tracks.size();
    const size_t iters = subset ? subset->size() : ntracks;
    for (size_t k = 0; k < iters; ++k) {
        const size_t ti = subset ? (*subset)[k] : k;
        const Track& tr = p.tracks[ti];
        MixTarget t;
        t.out = &out.interleaved;
        t.total_frames = total;
        const float g = dsp::db_to_lin(tr.gain_db);
        float l, r;
        dsp::equal_power_pan(tr.pan, l, r);
        t.gain_l = g * l;
        t.gain_r = g * r;

        // Placement (spec §arrangement): explicit clips, or legacy default = every pattern once at bar 0.
        // The default path iterates patterns in declaration order with offset exactly 0.0 and passes
        // note.t_beats through unmodified → bit-identical to pre-WP-0101 renders (goldens prove it).
        const double bpb = static_cast<double>(p.beats_per_bar);
        auto render_pattern_at = [&](const Pattern& pat, double offset_beats) {
            for (const Note& note : pat.notes) {
                const double t_abs = (offset_beats == 0.0) ? note.t_beats : offset_beats + note.t_beats;
                const long long start = tmap.at(t_abs);
                const double spb = tmap.spb_at(t_abs);  // local tempo at note start governs duration
                if (start >= total) continue;  // beyond render window
                // Per-note velocity: copy the mix target with pre-multiplied gains so voice code
                // never changes; default velocity skips the copy entirely → bit-identical legacy.
                MixTarget* target = &t;
                MixTarget vel_t;
                if (note.velocity != 1.0) {
                    vel_t = t;
                    const float v = static_cast<float>(note.velocity);
                    vel_t.gain_l *= v;
                    vel_t.gain_r *= v;
                    target = &vel_t;
                }
                switch (tr.voice.kind) {
                    case VoiceKind::Metronome: render_metronome(*target, start, note, tr.voice, sr); break;
                    case VoiceKind::Sine:      render_sine(*target, start, note, tr.voice, sr, spb); break;
                    case VoiceKind::Synth:     render_synth(*target, start, note, tr.voice, sr, spb); break;
                    case VoiceKind::Sampler:
                        if (!render_sampler(*target, start, note, join_path(base_dir, tr.voice.path), sr, spb, sampler_cache, err))
                            return false;
                        break;
                }
            }
            return true;
        };

        if (p.arrangement.empty()) {
            for (const Pattern& pat : p.patterns) {
                if (static_cast<size_t>(pat.track) != ti) continue;
                if (!render_pattern_at(pat, 0.0)) return false;
            }
        } else {
            for (const Clip& clip : p.arrangement) {
                const Pattern& pat = p.patterns[static_cast<size_t>(clip.pattern)];
                if (static_cast<size_t>(pat.track) != ti) continue;
                double extent = 0.0;  // loop period = pattern note extent, beats
                for (const Note& n : pat.notes) {
                    const double e = n.t_beats + n.dur_beats;
                    if (e > extent) extent = e;
                }
                const int reps = (extent > 0.0) ? clip.repeats : 1;
                const double base = clip.start_bar * bpb;
                for (int r = 0; r < reps; ++r) {
                    if (!render_pattern_at(pat, base + static_cast<double>(r) * extent)) return false;
                }
            }
        }
    }
    return true;
}

// Public API wrappers. render_project must remain bit-identical to pre-stems behavior —
// committed goldens are the regression net for this refactor.
bool render_project(const Project& p, const std::string& base_dir, wav::StereoBuffer& out, std::string& err) {
    return render_subset(p, base_dir, nullptr, out, err);
}

bool render_stems(const Project& p, const std::string& base_dir, std::vector<wav::StereoBuffer>& stems,
                  wav::StereoBuffer& mix, std::string& err) {
    if (!render_subset(p, base_dir, nullptr, mix, err)) return false;
    stems.clear();
    stems.resize(p.tracks.size());
    for (size_t i = 0; i < p.tracks.size(); ++i) {
        const std::vector<size_t> one{i};
        if (!render_subset(p, base_dir, &one, stems[i], err)) return false;
    }
    return true;
}

}  // namespace op
