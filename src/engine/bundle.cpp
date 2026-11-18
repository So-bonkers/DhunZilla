// engine/bundle.cpp — store-only ZIP for .opd bundles; see bundle.hpp.
// Format references are the PKZIP appnote; we deliberately use the smallest legal subset:
// method 0 (stored), no data descriptors, fixed timestamps → byte-deterministic output.
#include "engine/bundle.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "engine/model.hpp"

namespace fs = std::filesystem;

namespace op {
namespace {

// ---------------------------------------------------------------- CRC32 (IEEE 802.3, reflected)

uint32_t crc32_of(const std::vector<uint8_t>& data) {
    static uint32_t table[256];
    static bool built = false;
    if (!built) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        built = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (uint8_t b : data) c = table[(c ^ b) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// ---------------------------------------------------------------- little-endian writers

void put16(std::vector<uint8_t>& o, uint16_t v) { o.push_back(v & 0xFF); o.push_back(v >> 8); }
void put32(std::vector<uint8_t>& o, uint32_t v) {
    for (int i = 0; i < 4; ++i) o.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}
uint16_t get16(const std::vector<uint8_t>& b, size_t p) {
    return static_cast<uint16_t>(b[p] | (b[p + 1] << 8));
}
uint32_t get32(const std::vector<uint8_t>& b, size_t p) {
    return static_cast<uint32_t>(b[p]) | (static_cast<uint32_t>(b[p + 1]) << 8) |
           (static_cast<uint32_t>(b[p + 2]) << 16) | (static_cast<uint32_t>(b[p + 3]) << 24);
}

constexpr uint16_t kFixedDosDate = (0u << 9) | (1u << 5) | 1u;  // 1980-01-01 — determinism
constexpr uint16_t kFixedDosTime = 0;

bool safe_entry_name(const std::string& name) {
    if (name.empty() || name[0] == '/' || name.find('\\') != std::string::npos) return false;
    size_t pos = 0;
    while (pos < name.size()) {
        const size_t slash = name.find('/', pos);
        const std::string seg = name.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
        if (seg == "..") return false;
        if (slash == std::string::npos) break;
        pos = slash + 1;
    }
    return true;
}

std::vector<uint8_t> read_file_bytes(const fs::path& p, bool& ok) {
    std::ifstream f(p, std::ios::binary);
    if (!f) { ok = false; return {}; }
    ok = true;  // (was missing: success path left the caller's flag at its initial false)
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

struct ZipEntry {
    std::string name;
    std::vector<uint8_t> data;
};

void append_local(std::vector<uint8_t>& o, const ZipEntry& e, uint32_t& offset_out) {
    offset_out = static_cast<uint32_t>(o.size());
    o.insert(o.end(), {'P', 'K', 0x03, 0x04});
    put16(o, 20); put16(o, 0); put16(o, 0);  // version needed, flags, method=store
    put16(o, kFixedDosTime); put16(o, kFixedDosDate);
    put32(o, crc32_of(e.data));
    put32(o, static_cast<uint32_t>(e.data.size()));  // compressed == uncompressed (stored)
    put32(o, static_cast<uint32_t>(e.data.size()));
    put16(o, static_cast<uint16_t>(e.name.size())); put16(o, 0);
    o.insert(o.end(), e.name.begin(), e.name.end());
    o.insert(o.end(), e.data.begin(), e.data.end());
}

}  // namespace

bool pack_project(const std::string& project_path, const std::string& out_opd, std::string& err) {
    Project p;
    if (!load_project(project_path, p, err)) return false;
    const fs::path proj_dir = fs::path(project_path).parent_path();

    // Collect referenced sample paths (deduped, sorted → deterministic entry order).
    std::set<std::string> rels;
    for (const Track& t : p.tracks) {
        if (t.voice.kind != VoiceKind::Sampler) continue;
        const fs::path vp(t.voice.path);
        if (vp.is_absolute()) { err = "bundle: absolute sampler path not bundleable: " + t.voice.path; return false; }
        std::string rel = vp.generic_string();
        if (!safe_entry_name(rel)) { err = "bundle: sampler path escapes project dir: " + rel; return false; }
        rels.insert(rel);
    }
    // Attachments (WP-0116): bundle those present on disk; informational refs may legitimately be
    // absent — skip silently (unlike samplers, whose absence fails the pack).
    for (const Attachment& a : p.attachments) {
        if (!safe_entry_name(a.path)) continue;  // parser validated already; belt-and-braces
        std::error_code ec;
        if (fs::exists(proj_dir / a.path, ec)) rels.insert(a.path);
    }

    std::vector<ZipEntry> entries;
    {  // main project file first, named after its own filename
        bool ok = false;
        auto bytes = read_file_bytes(project_path, ok);
        if (!ok) { err = "bundle: cannot read project '" + project_path + "'"; return false; }
        entries.push_back({fs::path(project_path).filename().generic_string(), std::move(bytes)});
    }
    for (const std::string& rel : rels) {
        bool ok = false;
        auto bytes = read_file_bytes(proj_dir / rel, ok);
        if (!ok) { err = "bundle: referenced sample missing: " + (proj_dir / rel).string(); return false; }
        entries.push_back({rel, std::move(bytes)});
    }

    std::vector<uint8_t> o;
    std::vector<std::pair<const ZipEntry*, uint32_t>> offsets;
    for (const auto& e : entries) {
        uint32_t off = 0;
        append_local(o, e, off);
        offsets.push_back({&e, off});
    }
    const uint32_t cd_off = static_cast<uint32_t>(o.size());
    for (const auto& [e, off] : offsets) {
        o.insert(o.end(), {'P', 'K', 0x01, 0x02});
        put16(o, 20); put16(o, 20); put16(o, 0); put16(o, 0);
        put16(o, kFixedDosTime); put16(o, kFixedDosDate);
        put32(o, crc32_of(e->data));
        put32(o, static_cast<uint32_t>(e->data.size()));
        put32(o, static_cast<uint32_t>(e->data.size()));
        put16(o, static_cast<uint16_t>(e->name.size()));
        put16(o, 0); put16(o, 0); put16(o, 0); put16(o, 0); put32(o, 0);
        put32(o, off);
        o.insert(o.end(), e->name.begin(), e->name.end());
    }
    const uint32_t cd_size = static_cast<uint32_t>(o.size() - cd_off);  // before EOCD bytes are appended!
    o.insert(o.end(), {'P', 'K', 0x05, 0x06});
    put16(o, 0); put16(o, 0);
    put16(o, static_cast<uint16_t>(entries.size()));
    put16(o, static_cast<uint16_t>(entries.size()));
    put32(o, cd_size);
    put32(o, cd_off);
    put16(o, 0);

    std::ofstream f(out_opd, std::ios::binary);
    if (!f) { err = "bundle: cannot write '" + out_opd + "'"; return false; }
    f.write(reinterpret_cast<const char*>(o.data()), static_cast<std::streamsize>(o.size()));
    return f.good();
}

bool unpack_bundle(const std::string& opd_path, const std::string& out_dir,
                   std::string& main_project_out, std::string& err) {
    main_project_out.clear();
    bool ok = false;
    const auto buf = read_file_bytes(opd_path, ok);
    if (!ok || buf.size() < 22) { err = "bundle: cannot read '" + opd_path + "'"; return false; }

    // Find EOCD (fixed layout, comment length 0 by our writer — but scan to be tolerant).
    long eocd = -1;
    const long lo = static_cast<long>(buf.size()) - 22;
    for (long i = lo; i >= 0 && i > lo - 65536; --i) {
        if (buf[i] == 'P' && buf[i + 1] == 'K' && buf[i + 2] == 0x05 && buf[i + 3] == 0x06) { eocd = i; break; }
    }
    if (eocd < 0) { err = "bundle: no end-of-central-directory — not an .opd"; return false; }
    const uint16_t n_entries = get16(buf, static_cast<size_t>(eocd) + 10);
    const uint32_t cd_off = get32(buf, static_cast<size_t>(eocd) + 16);

    // Pass 1: parse + validate everything (names, methods, local headers, CRCs) before writing.
    struct Validated { std::string name; uint32_t data_off; uint32_t size; };
    std::vector<Validated> files;
    size_t p = cd_off;
    for (uint16_t i = 0; i < n_entries; ++i) {
        if (p + 46 > buf.size() || !(buf[p] == 'P' && buf[p + 1] == 'K' && buf[p + 2] == 0x01 && buf[p + 3] == 0x02)) {
            err = "bundle: corrupt central directory entry " + std::to_string(i); return false;
        }
        const uint16_t method = get16(buf, p + 10);
        if (method != 0) { err = "bundle: compressed entry (method " + std::to_string(method) + ") — only store supported"; return false; }
        const uint32_t crc = get32(buf, p + 16);
        const uint32_t usize = get32(buf, p + 24);
        const uint16_t nlen = get16(buf, p + 28), elen = get16(buf, p + 30), clen = get16(buf, p + 32);
        const uint32_t local_off = get32(buf, p + 42);
        // WP-0128 fuzz: name bytes must exist in-file before we construct from them (OOB read fix).
        if (p + 46u + nlen > buf.size()) { err = "bundle: corrupt central directory entry " + std::to_string(i); return false; }
        const std::string name(reinterpret_cast<const char*>(&buf[p + 46]), nlen);
        p += 46 + nlen + elen + clen;
        if (!safe_entry_name(name)) { err = "bundle: unsafe entry name rejected: '" + name + "'"; return false; }
        // Duplicate names would mean silent overwrite in pass 2 — reject explicitly (WP-0128).
        for (const auto& prev : files) {
            if (prev.name == name) { err = "bundle: duplicate entry name '" + name + "'"; return false; }
        }

        // Local header must agree.
        if (local_off + 30 > buf.size() ||
            !(buf[local_off] == 'P' && buf[local_off + 1] == 'K' && buf[local_off + 2] == 0x03 && buf[local_off + 3] == 0x04)) {
            err = "bundle: bad local header for '" + name + "'"; return false;
        }
        const uint16_t l_nlen = get16(buf, local_off + 26), l_elen = get16(buf, local_off + 28);
        const uint32_t data_off = local_off + 30u + l_nlen + l_elen;
        if (static_cast<size_t>(data_off) + usize > buf.size()) { err = "bundle: truncated data for '" + name + "'"; return false; }
        const uint32_t actual = crc32_of(std::vector<uint8_t>(buf.begin() + data_off, buf.begin() + data_off + usize));
        if (actual != crc) { err = "bundle: CRC mismatch for '" + name + "'"; return false; }
        files.push_back({name, data_off, usize});
    }

    // Pass 2: write. Entry names validated above (no '..', no absolute) → cannot escape out_dir.
    std::error_code fec;
    fs::create_directories(out_dir, fec);
    if (fec) { err = "bundle: cannot create '" + out_dir + "': " + fec.message(); return false; }
    std::string main_opj;
    for (const auto& f : files) {
        const fs::path target = fs::path(out_dir) / f.name;
        std::error_code mec;
        if (!target.parent_path().empty()) fs::create_directories(target.parent_path(), mec);
        std::ofstream w(target, std::ios::binary);
        if (!w) { err = "bundle: cannot write '" + target.string() + "'"; return false; }
        w.write(reinterpret_cast<const char*>(&buf[f.data_off]), f.size);
        if (!w.good()) { err = "bundle: short write for '" + f.name + "'"; return false; }
        const std::string lower = f.name.size() >= 4 ? [&]{ std::string s = f.name;
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); }); return s; }() : "";
        if (lower.size() >= 4 && lower.substr(lower.size() - 4) == ".opj" && main_opj.empty())
            main_opj = target.string();
    }
    if (main_opj.empty()) { err = "bundle: no .opj project file inside"; return false; }
    main_project_out = main_opj;
    return true;
}

bool list_bundle(const std::string& opd_path, std::vector<std::string>& entries_out, std::string& err) {
    entries_out.clear();
    bool ok = false;
    const auto buf = read_file_bytes(opd_path, ok);
    if (!ok || buf.size() < 22) { err = "bundle: cannot read '" + opd_path + "'"; return false; }
    long eocd = -1;
    const long lo = static_cast<long>(buf.size()) - 22;
    for (long i = lo; i >= 0 && i > lo - 65536; --i) {
        if (buf[i] == 'P' && buf[i + 1] == 'K' && buf[i + 2] == 0x05 && buf[i + 3] == 0x06) { eocd = i; break; }
    }
    if (eocd < 0) { err = "bundle: no end-of-central-directory — not an .opd"; return false; }
    const uint16_t n_entries = get16(buf, static_cast<size_t>(eocd) + 10);
    size_t p = get32(buf, static_cast<size_t>(eocd) + 16);
    for (uint16_t i = 0; i < n_entries; ++i) {
        if (p + 46 > buf.size() ||
            !(buf[p] == 'P' && buf[p + 1] == 'K' && buf[p + 2] == 0x01 && buf[p + 3] == 0x02)) {
            err = "bundle: corrupt central directory entry " + std::to_string(i); return false;
        }
        const uint16_t nlen = get16(buf, p + 28), elen = get16(buf, p + 30), clen = get16(buf, p + 32);
        // WP-0128 fuzz: name bytes must exist in-file before we construct from them (OOB read fix).
        if (p + 46u + nlen > buf.size()) { err = "bundle: corrupt central directory entry " + std::to_string(i); return false; }
        const std::string name(reinterpret_cast<const char*>(&buf[p + 46]), nlen);
        if (!safe_entry_name(name)) { err = "bundle: unsafe entry name rejected: '" + name + "'"; return false; }
        entries_out.push_back(name);
        p += 46 + nlen + elen + clen;
    }
    return true;
}

}  // namespace op
