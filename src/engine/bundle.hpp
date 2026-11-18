// engine/bundle.hpp — .opd project bundle: one file = project + its sample assets (WP-0110).
// Container: store-only ZIP (method 0, no compression) → zero new dependencies, inspectable with
// any `unzip`, and byte-deterministic writes (fixed DOS timestamp; entry order = opj first, then
// assets sorted). Layout inside: <name>.opj at root + samples under the project-relative paths
// they use (e.g. "kits/kick.wav"). CRC32 verified on every read; any mismatch = actionable error.
#pragma once
#include <string>
#include <vector>

namespace op {

// Packs `project_path` plus every file it references (sampler voice paths, resolved relative to
// the project's directory) into a single .opd bundle. `err` on missing referenced file.
bool pack_project(const std::string& project_path, const std::string& out_opd, std::string& err);

// Unpacks a validated .opd into dir `out_dir` (created if absent). Returns the path of the main
// project file inside, so callers can `load_project` straight away. Rejects any entry that would
// escape out_dir (path traversal) and any CRC mismatch — never leaves partial garbage on failure.
bool unpack_bundle(const std::string& opd_path, const std::string& out_dir,
                   std::string& main_project_out, std::string& err);

// Lists entry names in archive order (metadata-only read: central directory + name validation,
// no CRC pass). Fails on structurally broken archives.
bool list_bundle(const std::string& opd_path, std::vector<std::string>& entries_out, std::string& err);

}  // namespace op
