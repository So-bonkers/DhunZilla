#include "engine/render.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include "engine/dsp.hpp"
#include "engine/resample.hpp"

namespace op {
namespace {

constexpr double kMaxRenderSeconds = 600.0;  // sanity cap for offline v0

// Per-clip fade envelope (WP-0178, spec project-model.md §Clip fades). Absolute frame bounds: a
// *_len of 0 means that ramp is off. Fade-in region [in_start, in_start+in_len); fade-out region
// [out_start, out_start+out_len). When they overlap, fade-in wins (checked first). Example:
//   ClipEnv e{100, 240, 0, 0}; clip_env_gain(e, 220) == float(120.0/240.0);
struct ClipEnv {
    long long in_start = 0, in_len = 0, out_start = 0, out_len = 0;
};

// Linear fade gain at absolute `frame`, in spec order: fade-in first, then fade-out, else unity.
// Plain arithmetic only (no libm): the ratio is computed in double and cast to float once.
inline float clip_env_gain(const ClipEnv& e, long long frame) {
    if (e.in_len > 0 && frame >= e.in_start && frame < e.in_start + e.in_len)
        return static_cast<float>(static_cast<double>(frame - e.in_start) / static_cast<double>(e.in_len));
    if (e.out_len > 0 && frame >= e.out_start && frame < e.out_start + e.out_len)
        return static_cast<float>(1.0 - static_cast<double>(frame - e.out_start) / static_cast<double>(e.out_len));
    return 1.0f;
}

// Voice renderers add into interleaved stereo `out` at absolute frame offset, applying track gain+pan.
struct MixTarget {
    std::vector<float>* out;   // interleaved L,R
    long long total_frames;
    float gain_l, gain_r;
    const ClipEnv* env = nullptr;  // WP-0178: per-clip fade envelope; null = none
    long long choke_at = -1;       // WP-0181: cut frame (-1 = no choke group); see spec §Sampler choke groups
    long long choke_fade = 0;      // WP-0181: fade length in frames (>= 1 whenever choke_at >= 0)
};

inline void add_stereo(MixTarget& t, long long frame, float l, float r) {
    if (frame < 0 || frame >= t.total_frames) return;
    // Choke cut (WP-0181): beyond the fade end the voice adds nothing; inside it, the contribution
    // is scaled by the linear gain g = float(F - d)/float(F). With choke_at < 0 the statements
    // below are byte-for-byte the pre-WP-0181 code path (g stays exactly what env gives: 1.0f or
    // the clip fade; and 1.0f * x == x exactly, so a pure choke scales identically too).
    float g = t.env ? clip_env_gain(*t.env, frame) : 1.0f;
    if (t.choke_at >= 0 && frame >= t.choke_at) {
        const long long d = frame - t.choke_at;
        if (d >= t.choke_fade) return;
        g *= static_cast<float>(t.choke_fade - d) / static_cast<float>(t.choke_fade);
    }
    (*t.out)[frame * 2 + 0] += l * t.gain_l * g;
    (*t.out)[frame * 2 + 1] += r * t.gain_r * g;
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

// synth voice v1 (WP-0161, spec §Synth voice v1): subtractive synth taken ONLY when
// waveform != Legacy || has_adsr || cutoff_hz > 0 — the legacy path above stays untouched.
// Everything is IEEE double per spec (float only for the final sample into the mixer); a
// feature left at its default uses the exact legacy float expression widened to double, so
// e.g. waveform+legacy-envelope or legacy-osc+ADSR render as specified. Polyphonic: every
// note is an independent voice, phase 0 and filter state 0 at its start; release tail after
// note-off. Only dsp::sind_/cosd_/powd_ for transcendentals (floor/ceil/min are exact).
void render_synth_v1(MixTarget& t, long long start, const Note& note, const Voice& v, int sr, double spb) {
    const long long len = static_cast<long long>(std::llround(note.dur_beats * spb));
    if (len <= 0) return;
    const double srd = static_cast<double>(sr);
    const bool has_waveform = v.waveform != Waveform::Legacy;
    const bool has_adsr = v.has_adsr;
    const bool has_filter = v.cutoff_hz > 0.0;

    // Pitch (spec §Pitch): double 12-TET for the v1 oscillator. Out-of-range → render nothing.
    double f = 0.0, dt = 0.0;
    if (has_waveform) {
        f = (note.midi_note >= 0)
            ? 440.0 * dsp::powd_(2.0, static_cast<double>(note.midi_note - 69) / 12.0)
            : static_cast<double>(v.freq_hz);
        dt = f / srd;
        if (f <= 0.0 || dt >= 0.5) return;
    }

    // Legacy oscillator (waveform absent): the exact legacy float expression, widened to double.
    float dphi = 0.0f, dphi_oct = 0.0f;
    if (!has_waveform) {
        const float f_legacy = (note.midi_note >= 0)
            ? 440.0f * dsp::powf_(2.0f, static_cast<float>(note.midi_note - 69) / 12.0f)
            : v.freq_hz;
        dphi = dsp::twoPi() * f_legacy / static_cast<float>(sr);
        dphi_oct = 2.0f * dphi;
    }

    // PolyBLEP correction (spec §Oscillator), using this note's dt.
    auto blep = [dt](double tt) -> double {
        if (tt < dt) { const double x = tt / dt; return x + x - x * x - 1.0; }
        if (tt > 1.0 - dt) { const double x = (tt - 1.0) / dt; return x * x + x + x + 1.0; }
        return 0.0;
    };

    // Filter (spec §Filter): RBJ cookbook low-pass, direct form I, per-note zero state.
    double B0 = 0.0, B1 = 0.0, B2 = 0.0, A1 = 0.0, A2 = 0.0;
    if (has_filter) {
        const double fc = std::min(v.cutoff_hz, 0.45 * srd);
        const double w0 = 2.0 * dsp::kPi_d() * fc / srd;
        const double cw = dsp::cosd_(w0), sw = dsp::sind_(w0);
        const double alpha = sw / (2.0 * v.resonance);
        const double a0 = 1.0 + alpha;
        B0 = ((1.0 - cw) / 2.0) / a0;
        B1 = (1.0 - cw) / a0;
        B2 = B0;
        A1 = (-2.0 * cw) / a0;
        A2 = (1.0 - alpha) / a0;
    }

    // Envelope (spec §ADSR). g(t): the divisions are unreachable when A == 0 / D == 0 because
    // of the comparisons, exactly as the spec notes.
    const double A = v.attack_s, D = v.decay_s, S = v.sustain, R = v.release_s;
    auto g = [A, D, S](double tt) -> double {
        if (tt < A) return tt / A;
        if (tt < A + D) return 1.0 - (1.0 - S) * (tt - A) / D;
        return S;
    };
    const double e_off = has_adsr ? g(static_cast<double>(len) / srd) : 1.0;
    const long long tail = has_adsr ? static_cast<long long>(std::ceil(R * srd)) : 0;  // R == 0 → no tail

    // Legacy envelope (adsr absent): the exact legacy float expression, widened to double.
    const long long atk = static_cast<long long>(v.attack_ms * 0.001f * static_cast<float>(sr));
    const long long rel = static_cast<long long>(v.release_ms * 0.001f * static_cast<float>(sr));

    double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;  // filter state, zero at voice start (retrigger rule)
    for (long long n = 0; n < len + tail; ++n) {
        double x;
        if (has_waveform) {
            const double ph = f * static_cast<double>(n) / srd;  // multiply first, then divide (spec)
            const double fr = ph - std::floor(ph);               // ∈ [0,1)
            double w;
            if (v.waveform == Waveform::Sine) {
                w = dsp::sind_(2.0 * dsp::kPi_d() * fr);
            } else if (v.waveform == Waveform::Saw) {
                w = (2.0 * fr - 1.0) - blep(fr);
            } else {  // Square
                double t2 = fr + 0.5;
                if (t2 >= 1.0) t2 -= 1.0;
                w = (fr < 0.5 ? 1.0 : -1.0) + blep(fr) - blep(t2);
            }
            x = 0.5 * w;
        } else {
            constexpr float kNorm = 1.0f / 1.35f;
            const float nn = static_cast<float>(n);
            x = static_cast<double>((dsp::sinf_(dphi * nn) + 0.35f * dsp::sinf_(dphi_oct * nn)) * kNorm);
        }

        double y = x;
        if (has_filter) {
            y = B0 * x + B1 * x1 + B2 * x2 - A1 * y1 - A2 * y2;
            x2 = x1; x1 = x;
            y2 = y1; y1 = y;
        }

        double env;
        if (!has_adsr) {
            float e = 1.0f;
            if (atk > 0 && n < atk) e = static_cast<float>(n) / static_cast<float>(atk);
            const long long since_end = len - n;
            if (rel > 0 && since_end <= rel) e *= static_cast<float>(since_end) / static_cast<float>(rel);
            env = static_cast<double>(e);
        } else if (n < len) {
            env = g(static_cast<double>(n) / srd);
        } else {  // release tail: linear from the note-off level to silence over R seconds
            const long long k = n - len;
            env = e_off * (1.0 - (static_cast<double>(k) / srd) / R);
        }

        add_stereo(t, start + n, static_cast<float>(y * env), static_cast<float>(y * env));
    }
}

// Repitch ratio (WP-0167, docs/spec/project-model.md §Sampler repitch): 2^(shift/12) as a double from the
// correctly-rounded octave-fraction constants T[r] = 2^(r/12), with 2^q applied by exact multiplication or
// division by 2.0 in a loop — no pow(), no libm (ADR-0004). shift is -127..127, so q is small. Example:
//   sampler_ratio(12) == 2.0 exactly; sampler_ratio(-5) == T[7] / 2.0.
static double sampler_ratio(int shift) {
    constexpr double kT[12] = {1.0,               1.0594630943592953, 1.122462048309373,
                               1.189207115002721, 1.2599210498948732, 1.3348398541700344,
                               1.4142135623730951, 1.4983070768766815, 1.5874010519681996,
                               1.681792830507429, 1.7817974362806785, 1.887748625363387};
    int q = shift / 12;             // C++ division truncates toward zero...
    int r = shift - 12 * q;         // ...so correct to floor division for negative shifts
    if (r < 0) { r += 12; --q; }
    double ratio = kT[r];
    for (int i = 0; i < q; ++i) ratio *= 2.0;
    for (int i = 0; i > q; --i) ratio /= 2.0;
    return ratio;
}

bool render_sampler(MixTarget& t, long long start, const Note& note, const Voice& v, const std::string& abs_path,
                    int sr, double spb, std::map<std::string, wav::ReadResult>& cache, std::string& err) {
    auto it = cache.find(abs_path);
    if (it == cache.end()) {
        wav::ReadResult rr = wav::read_audio(abs_path);
        if (!rr.ok) { err = "sampler: " + rr.error; return false; }
        if (rr.sample_rate != sr) {
            // WP-0138: convert once at load into the cache entry; playback then takes the exact
            // native-rate path below. Native-rate assets are untouched (bit-identical to before).
            if (!resample::rate_supported(rr.sample_rate)) {
                err = "sampler: unsupported sample rate " + std::to_string(rr.sample_rate);
                return false;
            }
            std::vector<float> converted;
            if (!resample::windowsinc(rr.samples, rr.channels, rr.sample_rate, sr, converted)) {
                err = "sampler: resample failed";  // defensive: rates/channels validated above
                return false;
            }
            rr.samples.swap(converted);
            rr.sample_rate = sr;
        }
        it = cache.emplace(abs_path, std::move(rr)).first;
    }
    const wav::ReadResult& rr = it->second;
    long long avail = static_cast<long long>(rr.samples.size()) / rr.channels;
    // Sampler trim (WP-0156): slice [i0, i1) of the project-rate buffer, computed after resampling.
    // Default (no start_s, no end_s) skips all arithmetic: i0 = 0, i1 = avail (bit-identical path).
    long long i0 = 0, i1 = avail;
    if (v.start_s != 0.0 || v.has_end_s) {
        // Clamp the double before llround so absurd values stay defined (they are empty slices anyway).
        auto to_frame = [sr](double sec) {
            double x = sec * static_cast<double>(sr);
            if (x > 4e18) x = 4e18;
            return static_cast<long long>(std::llround(x));
        };
        i0 = to_frame(v.start_s);
        if (v.has_end_s) i1 = to_frame(v.end_s);
        if (i1 > avail) i1 = avail;
        if (i1 <= i0) { err = "sampler: trim slice is empty"; return false; }
    }
    long long want = static_cast<long long>(std::llround(note.dur_beats * spb));
    long long slice = i1 - i0;
    // Sampler repitch (WP-0167, docs/spec/project-model.md §Sampler repitch): applied to the sliced
    // project-rate buffer. Only a note whose pitch differs from the voice root takes this path —
    // absent root, absent note pitch or P == root fall through to the untouched loop below (bit-exact).
    if (v.root_note >= 0 && note.midi_note >= 0 && note.midi_note != v.root_note) {
        const double ratio = sampler_ratio(note.midi_note - v.root_note);
        for (long long n = 0; n < want; ++n) {
            const double pos = static_cast<double>(n) * ratio;  // per-frame multiply, never a running sum
            const long long k = static_cast<long long>(pos);    // pos >= 0 → truncation IS the floor
            if (k + 1 >= slice) break;                          // needs x[k+1]: source ran out (L == 1: silence)
            const double frac = pos - static_cast<double>(k);
            float l, r;
            if (rr.channels == 1) {
                const double a = rr.samples[i0 + k], b = rr.samples[i0 + k + 1];
                l = r = static_cast<float>(a + frac * (b - a));
            } else {
                const long long f = (i0 + k) * 2;
                const double al = rr.samples[f], ar = rr.samples[f + 1];
                l = static_cast<float>(al + frac * (rr.samples[f + 2] - al));
                r = static_cast<float>(ar + frac * (rr.samples[f + 3] - ar));
            }
            add_stereo(t, start + n, l, r);
        }
        return true;
    }
    long long len = slice < want ? slice : want;
    for (long long n = 0; n < len; ++n) {
        float l, r;
        const long long f = i0 + n;
        if (rr.channels == 1) { l = r = rr.samples[f]; }
        else { l = rr.samples[f * 2]; r = rr.samples[f * 2 + 1]; }
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

// Pattern swing (WP-0166, spec project-model.md §Pattern swing): a note is swung iff its
// pattern-local t is exactly an odd 16th step. Multiplying by 4 is exact (power-of-two scale).
bool is_odd_sixteenth(double t) {
    const double q = t * 4.0;
    return std::floor(q) == q && std::fmod(q, 2.0) == 1.0;
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

// Absolute beat position of a note within one placement (WP-0181 refactor of the render loop's
// t_abs math — same operations, same order): offset + note t, with pattern swing (WP-0166) added
// last to odd 16ths. Shared by the render loop and the choke trigger pre-pass so both derive
// identical start frames. Example: pat.swing = 0.5, note.t_beats = 0.75, offset = 4.0 → 4.8125.
inline double note_abs_beat(const Pattern& pat, const Note& note, double offset_beats) {
    double t_abs = (offset_beats == 0.0) ? note.t_beats : offset_beats + note.t_beats;
    if (pat.swing != 0.0 && is_odd_sixteenth(note.t_beats)) t_abs += pat.swing * 0.125;
    return t_abs;
}

// Enumerate every pattern placement of track `ti` exactly as the renderer sees it (spec
// §arrangement): legacy default = every pattern of the track once at bar 0; otherwise arrangement
// clips in order, filtered to this track, one call per repeat. Calls f(pattern, offset_beats,
// clip_gain_db, env) with the clip's fade envelope (WP-0178; nullptr when the clip has no fades)
// and stops at the first f that returns false. This is the render loop's pre-WP-0181 placement
// code moved verbatim — the choke trigger pre-pass shares it so triggers can never drift from
// actual placements. Example:
//   for_each_placement(p, 0, tmap, [&](const Pattern& pat, double off, double, const ClipEnv*) {
//       return true; });
template <class F>
bool for_each_placement(const Project& p, std::size_t ti, const TempoMap& tmap, F&& f) {
    const double bpb = static_cast<double>(p.beats_per_bar);
    if (p.arrangement.empty()) {
        for (const Pattern& pat : p.patterns) {
            if (static_cast<size_t>(pat.track) != ti) continue;
            if (!f(pat, 0.0, 0.0, nullptr)) return false;
        }
        return true;
    }
    for (const Clip& clip : p.arrangement) {
        const Pattern& pat = p.patterns[static_cast<size_t>(clip.pattern)];
        if (static_cast<size_t>(pat.track) != ti) continue;
        const double extent = pattern_extent(pat);  // loop period, beats (WP-0162 helper)
        const int reps = (extent > 0.0) ? clip.repeats : 1;
        const double base = clip.start_bar * bpb;
        // Clip fades (WP-0178): one envelope per clip spanning all its repeats, built only when
        // a fade is set. Region bounds via the renderer's own TempoMap::at() (spec §Clip fades).
        const ClipEnv* env = nullptr;
        ClipEnv cenv;
        if (clip.fade_in_beats != 0.0 || clip.fade_out_beats != 0.0) {
            const double clip_end_beats = base + static_cast<double>(reps) * extent;
            const long long A = tmap.at(base);
            cenv.in_start = A;
            cenv.in_len = tmap.at(base + clip.fade_in_beats) - A;
            const long long E = tmap.at(clip_end_beats);
            cenv.out_start = tmap.at(clip_end_beats - clip.fade_out_beats);
            cenv.out_len = E - cenv.out_start;
            env = &cenv;
        }
        for (int r = 0; r < reps; ++r) {
            if (!f(pat, base + static_cast<double>(r) * extent, clip.gain_db, env)) return false;
        }
    }
    return true;
}

// Sampler choke groups (WP-0181, spec project-model.md §Sampler choke groups): the cut frame of a
// voice started at `start` on track `ti` — the smallest trigger start frame strictly greater than
// `start` belonging to a DIFFERENT track of `group`, or -1 when there is none. `triggers` vectors
// are sorted by (frame, track); scanning from the first frame > start and skipping this track's
// own entries gives the earliest cut (a voice already fading is never restarted — earliest wins).
long long choke_cut_frame(const std::map<int, std::vector<std::pair<long long, size_t>>>& triggers,
                          int group, long long start, size_t ti) {
    const auto it = triggers.find(group);
    if (it == triggers.end()) return -1;
    const std::vector<std::pair<long long, size_t>>& vec = it->second;
    const auto p = std::lower_bound(vec.begin(), vec.end(),
                                    std::make_pair(start + 1, static_cast<size_t>(0)));
    for (auto q = p; q != vec.end(); ++q)
        if (q->second != ti) return q->first;
    return -1;
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

long long sample_at_bar(const Project& p, double bar) {
    // Spec "Render a bar range" (WP-0145): reuse the renderer's own tempo map — never re-implement the math.
    const long long total = total_samples_for(p);
    if (bar >= p.length_bars) return total;
    const TempoMap tm = build_tempo_map(p, p.sample_rate);
    const long long s = tm.at(bar * static_cast<double>(p.beats_per_bar));
    return std::min(s, total);
}

void slice_frames(const wav::StereoBuffer& buf, long long from_frame, long long to_frame, wav::StereoBuffer& out) {
    // Contract: 0 <= from < to <= buf.frames(); the CLI validates before calling.
    out.sample_rate = buf.sample_rate;
    const size_t from = static_cast<size_t>(from_frame) * 2u;
    const size_t to = static_cast<size_t>(to_frame) * 2u;
    out.interleaved.assign(buf.interleaved.begin() + static_cast<std::ptrdiff_t>(from),
                           buf.interleaved.begin() + static_cast<std::ptrdiff_t>(to));
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

    // Sampler choke groups (WP-0181, spec project-model.md §Sampler choke groups): every note of an
    // audible track that has a choke_group is a trigger at its placement start frame (< total).
    // Built over ALL tracks — independent of `subset` — so stems see exactly the same cuts as the
    // mix. group -> sorted (start frame, track index); empty when no track has a group.
    std::map<int, std::vector<std::pair<long long, size_t>>> choke_triggers;
    for (size_t ti = 0; ti < ntracks; ++ti) {
        if (p.tracks[ti].choke_group == 0 || !track_audible(p, ti)) continue;
        std::vector<std::pair<long long, size_t>>& vec = choke_triggers[p.tracks[ti].choke_group];
        for_each_placement(p, ti, tmap, [&](const Pattern& pat, double offset_beats, double, const ClipEnv*) {
            for (const Note& note : pat.notes) {
                const long long s = tmap.at(note_abs_beat(pat, note, offset_beats));
                if (s < total) vec.emplace_back(s, ti);
            }
            return true;
        });
    }
    for (auto& kv : choke_triggers) std::sort(kv.second.begin(), kv.second.end());
    // Fade length F = max(1, sample_rate * 5 / 1000) frames: integer division → 240 @48 kHz, 220 @44.1 kHz.
    long long choke_fade_frames = static_cast<long long>(sr) * 5 / 1000;
    if (choke_fade_frames < 1) choke_fade_frames = 1;

    const size_t iters = subset ? subset->size() : ntracks;
    for (size_t k = 0; k < iters; ++k) {
        const size_t ti = subset ? (*subset)[k] : k;
        // Mixer (spec §mixer): inaudible tracks are skipped entirely, not rendered at gain 0.
        // Stems path gets its all-zero full-length stem "for free" from the reset above.
        if (!track_audible(p, ti)) continue;
        const Track& tr = p.tracks[ti];
        MixTarget t;
        t.out = &out.interleaved;
        t.total_frames = total;
        const float g = dsp::db_to_lin(tr.gain_db);
        float l, r;
        dsp::equal_power_pan(tr.pan, l, r);
        t.gain_l = g * l;
        t.gain_r = g * r;

        // Placement (spec §arrangement) runs through for_each_placement below — the same code the
        // choke trigger pre-pass enumerates, so triggers and voices can never disagree.
        auto render_pattern_at = [&](const Pattern& pat, double offset_beats, double clip_gain_db,
                                     const ClipEnv* env) {
            // Clip gain (WP-0120): one mix-target copy per clip; 0.0 skips entirely → bit-identical.
            // WP-0178: a fade envelope also needs the copy (it rides on the same target); when both
            // are absent nothing is copied and the default path stays bit-exact.
            MixTarget* mt = &t;
            MixTarget clip_t;
            if (clip_gain_db != 0.0 || env != nullptr) {
                clip_t = t;
                if (clip_gain_db != 0.0) {
                    const float cg = dsp::db_to_lin(clip_gain_db);
                    clip_t.gain_l *= cg;
                    clip_t.gain_r *= cg;
                }
                clip_t.env = env;
                mt = &clip_t;
            }
            for (const Note& note : pat.notes) {
                // Absolute beat position: offset + t, pattern swing (WP-0166) added last to odd 16ths
                // (helper shared with the choke trigger pre-pass — same math, bit-exact).
                const double t_abs = note_abs_beat(pat, note, offset_beats);
                const long long start = tmap.at(t_abs);
                const double spb = tmap.spb_at(t_abs);  // local tempo at note start governs duration
                if (start >= total) continue;  // beyond render window
                // Per-note velocity: copy the mix target with pre-multiplied gains so voice code
                // never changes; default velocity skips the copy entirely → bit-identical legacy.
                MixTarget* target = mt;
                MixTarget vel_t;
                if (note.velocity != 1.0) {
                    vel_t = *mt;
                    const float v = static_cast<float>(note.velocity);
                    vel_t.gain_l *= v;
                    vel_t.gain_r *= v;
                    target = &vel_t;
                }
                // Sampler choke (WP-0181): a grouped voice is cut at the earliest trigger frame of
                // another track in its group. Tracks without a group take the existing path with no
                // extra work.
                MixTarget choke_t;
                if (tr.choke_group != 0) {
                    const long long cut = choke_cut_frame(choke_triggers, tr.choke_group, start, ti);
                    if (cut >= 0) {
                        choke_t = *target;
                        choke_t.choke_at = cut;
                        choke_t.choke_fade = choke_fade_frames;
                        target = &choke_t;
                    }
                }
                switch (tr.voice.kind) {
                    case VoiceKind::Metronome: render_metronome(*target, start, note, tr.voice, sr); break;
                    case VoiceKind::Sine:      render_sine(*target, start, note, tr.voice, sr, spb); break;
                    case VoiceKind::Synth:
                        // WP-0161: v1 path only when a v1 feature is set; legacy renders untouched.
                        if (tr.voice.waveform != Waveform::Legacy || tr.voice.has_adsr || tr.voice.cutoff_hz > 0.0)
                            render_synth_v1(*target, start, note, tr.voice, sr, spb);
                        else
                            render_synth(*target, start, note, tr.voice, sr, spb);
                        break;
                    case VoiceKind::Sampler:
                        if (!render_sampler(*target, start, note, tr.voice, join_path(base_dir, tr.voice.path), sr, spb,
                                       sampler_cache, err))
                            return false;
                        break;
                }
            }
            return true;
        };

        if (!for_each_placement(p, ti, tmap,
                                [&](const Pattern& pat, double offset_beats, double clip_gain_db,
                                    const ClipEnv* env) {
                                    return render_pattern_at(pat, offset_beats, clip_gain_db, env);
                                }))
            return false;
    }

    // Master gain (spec §mixer): one multiply per sample after all tracks are summed. Exactly 0.0
    // skips it → bit-identical to pre-WP-0136 renders. render_stems calls render_subset per stem,
    // so the same multiply lands on the mix and every stem (sum of stems ≈ mix, float-assoc only).
    if (p.master_gain_db != 0.0f) {
        const float m = dsp::db_to_lin(p.master_gain_db);
        for (float& s : out.interleaved) s *= m;
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

// --- Click track render (WP-0148, spec §Click track render). Timing only: uses the same
// TempoMap/at() mapping as note placement, so clicks land exactly where notes would.

std::vector<long long> beat_onsets(const Project& p) {
    std::vector<long long> onsets;
    const long long total = total_samples_for(p);
    const TempoMap tm = build_tempo_map(p, p.sample_rate);
    for (long long b = 0;; ++b) {
        const long long s = tm.at(static_cast<double>(b));
        if (s >= total) break;
        onsets.push_back(s);
    }
    return onsets;
}

bool render_click(const Project& p, wav::StereoBuffer& out, std::string& err) {
    const int sr = p.sample_rate;
    const long long total = total_samples_for(p);
    if (total <= 0) { err = "render_error: zero-length render"; return false; }
    if (static_cast<double>(total) / static_cast<double>(sr) > kMaxRenderSeconds) {
        err = "render_error: project exceeds 600s offline cap"; return false;
    }

    out.sample_rate = sr;
    out.interleaved.assign(static_cast<size_t>(total) * 2u, 0.0f);

    const long long L = static_cast<long long>(sr) * 30 / 1000;  // burst length (spec: integer division)
    const double tau = static_cast<double>(sr) / 200.0;          // envelope time constant
    const std::vector<long long> onsets = beat_onsets(p);
    for (size_t i = 0; i < onsets.size(); ++i) {
        const bool downbeat = static_cast<long long>(i) % p.beats_per_bar == 0;
        const double f = downbeat ? 1500.0 : 1000.0;
        const long long start = onsets[i];
        for (long long n = 0; n < L && start + n < total; ++n) {  // truncated at end of buffer
            const double ph = 2.0 * dsp::kPi_d() * f * static_cast<double>(n) / static_cast<double>(sr);
            const float env = dsp::expf_(static_cast<float>(-(static_cast<double>(n) / tau)));
            const float x = 0.5f * static_cast<float>(dsp::cosd_(ph)) * env;
            out.interleaved[(start + n) * 2 + 0] = x;  // assigned, not added (spec: bursts never overlap)
            out.interleaved[(start + n) * 2 + 1] = x;
        }
    }
    return true;
}

// --- Pattern mode (WP-0192, spec project-model.md §Pattern mode) -----------------------------------
// Builds the pattern-mode project: a copy of `p` with a one-clip arrangement for one pattern and the
// length_bars that clip needs. Pure struct work — no render, no I/O, no clock; the caller renders the
// result through the normal path, which is what makes the output bit-identical to the equivalent
// hand-written project. Preconditions (caller-checked): pattern_index < p.patterns.size(), loops >= 1.
bool pattern_mode_project(const Project& p, std::size_t pattern_index, long long loops, Project& out,
                          std::string& err) {
    const Pattern& pat = p.patterns[pattern_index];
    const double extent = pattern_extent(pat);  // loop period, beats (same helper as the renderer)
    if (!(extent > 0.0)) {
        err = "pattern '" + pat.id + "' has no length (no notes and no length_beats)";
        return false;
    }
    out = p;
    Clip clip;                        // gain_db 0, no fades: the defaults
    clip.pattern = static_cast<int>(pattern_index);
    clip.start_bar = 0.0;
    clip.repeats = static_cast<int>(loops);
    out.arrangement = {clip};
    out.length_bars = std::ceil(static_cast<double>(loops) * extent / static_cast<double>(p.beats_per_bar));
    return true;
}

}  // namespace op
