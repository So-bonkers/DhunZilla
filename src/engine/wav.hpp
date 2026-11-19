// engine/wav.hpp — canonical stereo WAV writer (pcm16/pcm24/f32) + minimal reader.
// Writer output is bit-exact by construction: explicit little-endian byte assembly, no struct packing.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace op::wav {

struct StereoBuffer {
    int sample_rate = 48000;
    std::vector<float> interleaved;  // L,R,L,R...
    size_t frames() const { return interleaved.size() / 2; }
};

// Saturating round-half-to-even float -> int16 (spec project-model.md §4).
int16_t float_to_pcm16(float x);

// WP-0149: output sample format for write_wav_stereo / `opd render --format`.
enum class SampleFormat { Pcm16, Pcm24, F32 };

// Saturating round-half-to-even float -> signed 24-bit value in [-8388608, 8388607].
// Mirrors float_to_pcm16 exactly with scale 8388608: NaN -> 0; x <= -1 -> -8388608; x >= 1 -> 8388607;
// else round_half_even(x * 8388608.0f) clamped (spec render-harness.md, WP-0149).
int32_t float_to_pcm24(float x);

// Writes `buf` as stereo WAV in the given format (header layouts in docs/spec/render-harness.md).
bool write_wav_stereo(const std::string& path, StereoBuffer& buf, SampleFormat fmt, std::string& err);

// Legacy entry point: one-line Pcm16 wrapper around write_wav_stereo; output stays byte-identical.
bool write_pcm16_stereo(const std::string& path, StereoBuffer& buf, std::string& err);

struct ReadResult {
    bool ok = false;
    int sample_rate = 0;
    int channels = 0;
    std::vector<float> samples;  // interleaved float [-1,1]
    std::string error;
};

// Reads canonical PCM16 or float32 WAV (fmt chunk 1 or 3), mono or stereo. Used by sampler voice.
ReadResult read_wav(const std::string& path);

}  // namespace op::wav
