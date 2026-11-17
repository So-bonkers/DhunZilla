// engine/wav.hpp — canonical PCM16 stereo writer (44-byte header, no chunks) + minimal reader.
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
