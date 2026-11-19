// engine/sample_index.cpp — see docs/spec/sample-index.md (WP-0163). Boring explicit code: recursive
// scan on non-followed statuses, one std::sort, byte-exact JSON assembled with snprintf ("C" locale).
#include "engine/sample_index.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "engine/dsp.hpp"
#include "engine/wav.hpp"

namespace fs = std::filesystem;

namespace op::sample_index {

std::string fnv1a64_hex(const unsigned char* data, std::size_t n) {
    uint64_t h = 14695981039346656037ULL;  // FNV-1a 64-bit offset basis
    for (std::size_t i = 0; i < n; ++i) {
        h ^= static_cast<uint64_t>(data[i]);
        h *= 1099511628211ULL;  // FNV prime
    }
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
    return std::string(buf, 16);
}

namespace {

// JSON string escaping per spec: " and \ backslash-escaped; bytes < 0x20 as \u00XX (lowercase hex);
// every other byte (including UTF-8) passes through raw.
void escape_into(std::string& out, const std::string& s) {
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c == '"') {
            out += "\\\"";
        } else if (c == '\\') {
            out += "\\\\";
        } else if (c < 0x20u) {
            char u[8];
            std::snprintf(u, sizeof u, "\\u%04x", static_cast<unsigned>(c));
            out += u;
        } else {
            out += static_cast<char>(c);
        }
    }
}

// ASCII-lower-cased extension match against the candidate set (spec "Which files are scanned").
bool is_candidate_ext(const std::string& name) {
    const fs::path p(name);
    const std::string ext = p.extension().string();  // ".wav", ".WAV", "" for no dot — empty never matches
    if (ext.size() < 2 || ext.size() > 5) return false;
    std::string lower;
    lower.reserve(ext.size());
    for (const char ch : ext) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return lower == ".wav" || lower == ".aif" || lower == ".aiff" || lower == ".aifc";
}

// Whole-file binary read. Returns false on open failure or an I/O error mid-read (EOF is success).
bool read_file_bytes(const std::string& path, std::vector<unsigned char>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.clear();
    char buf[65536];
    while (f.read(buf, static_cast<std::streamsize>(sizeof buf)) || f.gcount() > 0)
        out.insert(out.end(), buf, buf + f.gcount());
    return !f.bad();
}

struct Found {
    std::string rel;   // path relative to the scanned dir, '/' separators, no leading "./"
    std::string abs;   // full filesystem path
};

// Recurse `abs` (reported under `rel_prefix`). Hidden entries (name starts with '.') are skipped
// entirely; symlinks are skipped on the non-followed status; only regular files with a candidate
// extension are collected. Unreadable sub-directories are skipped silently (never fail the call).
void scan_dir(const fs::path& abs, const std::string& rel_prefix, std::vector<Found>& found) {
    std::error_code ec;
    fs::directory_iterator it(abs, fs::directory_options::skip_permission_denied, ec);
    if (ec) return;  // cannot even open the directory: nothing to list inside it
    for (; it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) continue;  // unreadable entry: skip silently
        const std::string name = it->path().filename().string();
        if (!name.empty() && name[0] == '.') continue;  // hidden file or directory: not descended
        std::error_code sec;
        const fs::file_status st = it->symlink_status(sec);  // non-followed status (spec)
        if (sec || fs::is_symlink(st)) continue;             // symlinks are never followed nor listed
        const std::string rel = rel_prefix.empty() ? name : rel_prefix + "/" + name;
        if (fs::is_directory(st)) {
            scan_dir(it->path(), rel, found);
        } else if (fs::is_regular_file(st) && is_candidate_ext(name)) {
            found.push_back({rel, it->path().string()});
        }
    }
}

// Renders one candidate entry into `out` (one JSON object, no separator). Returns true when the
// entry belongs in "files", false when it belongs in "errors" — per-file failures never propagate.
bool render_entry(const Found& f, std::string& out) {
    op::wav::ReadResult rr = op::wav::read_audio(f.abs);
    if (!rr.ok) {
        out += "{\"path\":\"";
        escape_into(out, f.rel);
        out += "\",\"error\":\"";
        escape_into(out, rr.error);
        out += "\"}";
        return false;
    }
    // Hash over the raw bytes of the whole file; unreadable-for-hashing is an error entry (spec).
    std::vector<unsigned char> bytes;
    if (!read_file_bytes(f.abs, bytes)) {
        out += "{\"path\":\"";
        escape_into(out, f.rel);
        out += "\",\"error\":\"cannot read file\"}";
        return false;
    }
    // peak = max |sample| over all samples, as double. No libm: explicit sign flip (AGENTS.md).
    double peak = 0.0;
    for (const float s : rr.samples) {
        const double v = static_cast<double>(s);
        const double a = v < 0.0 ? -v : v;
        if (a > peak) peak = a;
    }
    const size_t channels = static_cast<size_t>(rr.channels);
    const size_t frames = rr.samples.size() / channels;
    char num[64];
    out += "{\"path\":\"";
    escape_into(out, f.rel);
    out += "\",\"format\":\"";
    out += rr.format;  // fixed tokens "wav"/"aiff" from the reader — no escaping needed
    out += "\",\"sample_rate\":";
    out += std::to_string(rr.sample_rate);
    out += ",\"channels\":";
    out += std::to_string(rr.channels);
    out += ",\"frames\":";
    out += std::to_string(frames);
    out += ",\"duration_s\":";
    std::snprintf(num, sizeof num, "%.6f", static_cast<double>(frames) / static_cast<double>(rr.sample_rate));
    out += num;
    out += ",\"peak_dbfs\":";
    if (peak == 0.0) {
        out += "null";  // all-silent or zero frames
    } else {
        std::snprintf(num, sizeof num, "%.3f", 20.0 * op::dsp::log10d_(peak));
        out += num;
    }
    out += ",\"hash\":\"";
    out += fnv1a64_hex(bytes.data(), bytes.size());
    out += "\"}";
    return true;
}

}  // namespace

bool build_sample_index(const std::string& dir, std::string& json_out, std::string& err) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec) || ec) {
        err = dir;
        return false;
    }
    std::vector<Found> found;
    scan_dir(fs::path(dir), "", found);
    // Explicit sort so the output never depends on directory iteration order (spec "Determinism").
    // `found` is sorted by rel once; splitting into files/errors preserves that order, so both
    // lists are sorted by path as the spec requires.
    std::sort(found.begin(), found.end(),
              [](const Found& a, const Found& b) { return a.rel < b.rel; });

    std::vector<std::string> file_frags, error_frags;
    file_frags.reserve(found.size());
    error_frags.reserve(found.size());
    for (const Found& f : found) {
        std::string frag;
        if (render_entry(f, frag)) file_frags.push_back(std::move(frag));
        else error_frags.push_back(std::move(frag));
    }

    json_out = "{\"version\":1,\"files\":[";
    for (size_t i = 0; i < file_frags.size(); ++i) {
        if (i) json_out += ",";
        json_out += file_frags[i];
    }
    json_out += "],\"errors\":[";
    for (size_t i = 0; i < error_frags.size(); ++i) {
        if (i) json_out += ",";
        json_out += error_frags[i];
    }
    json_out += "]}\n";
    return true;
}

}  // namespace op::sample_index
