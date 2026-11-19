// engine/sample_index.hpp — deterministic JSON index of a sample directory (WP-0163, docs/spec/sample-index.md).
// Backend for the sample browser: per-file format/rate/channels/length/peak + stable content hash.
// Deterministic by construction: explicit sort, no clock, no randomness, "C"-locale number formatting.
#pragma once
#include <cstddef>
#include <string>

namespace op::sample_index {

// 64-bit FNV-1a over `n` bytes (offset basis 14695981039346656037, prime 1099511628211), as exactly
// 16 lowercase hex digits. Example: fnv1a64_hex("", 0) == "cbf29ce484222325".
std::string fnv1a64_hex(const unsigned char* data, std::size_t n);

// Scans `dir` and returns the index JSON text (format in docs/spec/sample-index.md, ending in "\n").
// Returns false and sets `err` only if `dir` does not exist or is not a directory (err = the dir path).
// Unreadable/invalid audio files never fail the call; they go under "errors".
// Usage:
//   std::string json, err;
//   if (!op::sample_index::build_sample_index("kits", json, err)) handle(err);  // err == "kits"
bool build_sample_index(const std::string& dir, std::string& json_out, std::string& err);

}  // namespace op::sample_index
