// Unit tests for clip trim part A (WP-0193a): the optional clip keys offset_beats / length_beats
// (parse, validate, canonical save), the undoable set_clip_trim op (null clears each side) and
// `opd diff` emission/replay. Boring assert-based runner — no framework deps, own main. Exit code
// = number of failures. Writes no files: projects are built in memory with nlohmann::json +
// op::project_from_json and compared through op::project_bytes (the canonical save bytes).
// Run: ./build/op_clip_trim_tests
#include <cstdio>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/model.hpp"
#include "engine/model_json.hpp"
#include "engine/ops.hpp"

static int g_failures = 0;
#define CHECK(cond, name)                                                              \
    do {                                                                               \
        if (cond) std::printf("PASS %s\n", name);                                      \
        else { std::printf("FAIL %s  (%s:%d)\n", name, __FILE__, __LINE__); ++g_failures; } \
    } while (0)

// One track, one pattern, exactly one clip. A fresh copy on every call.
static nlohmann::json base_json() {
    return nlohmann::json::parse(R"JSON({
      "format_version": 0,
      "bpm": 120.0,
      "sample_rate": 48000,
      "seed": 42,
      "length_bars": 4.0,
      "beats_per_bar": 4,
      "tracks": [{"name": "lead", "voice": {"kind": "sine", "freq_hz": 440.0}}],
      "patterns": [{"track": 0, "notes": [{"t": 0.0, "dur": 1.0}]}],
      "arrangement": [{"pattern": 0, "start_bar": 0.0, "repeats": 2}]
    })JSON");
}

// Parse into a fresh Project (project_from_json appends to out.arrangement — never reuse `out`).
static bool parse(const nlohmann::json& j, op::Project& out, std::string& err) {
    out = op::Project{};
    return op::project_from_json(j, out, err);
}

// Parse error when the clip's `key` is set to `value`; empty string when accepted.
static std::string clip_parse_error(const char* key, const nlohmann::json& value) {
    nlohmann::json j = base_json();
    j["arrangement"][0][key] = value;
    op::Project p;
    std::string err;
    return parse(j, p, err) ? std::string() : err;
}

// Apply one parsed op object given as JSON text.
static bool apply_text(op::Project& p, const char* op_json, std::string& err) {
    return op::apply_op(p, nlohmann::json::parse(op_json), err);
}

// Apply every emitted diff op to a copy of `from`; true iff all succeeded and the canonical bytes
// equal want's.
static bool replay(const std::vector<nlohmann::json>& ops, const op::Project& from,
                   const op::Project& want, std::string& err) {
    op::Project work = from;
    for (const nlohmann::json& o : ops)
        if (!op::apply_op(work, o, err)) return false;
    return op::project_bytes(work) == op::project_bytes(want);
}

int main() {
    // ---- Defaults: keys absent = offset 0, untrimmed (spec §Clip keys). ----
    op::Project a;
    std::string err;
    CHECK(parse(base_json(), a, err), "base project parses");
    CHECK(a.arrangement.size() == 1, "base has one clip");
    CHECK(a.arrangement[0].offset_beats == 0.0, "default offset_beats is 0");
    CHECK(!a.arrangement[0].length_beats.has_value(), "default length_beats absent");

    // ---- Accepted values: extremes, -0.0, integer form (spec §Clip keys). ----
    CHECK(clip_parse_error("offset_beats", 0) == "", "offset 0 accepted");
    CHECK(clip_parse_error("offset_beats", 4096) == "", "offset 4096 accepted");
    CHECK(clip_parse_error("offset_beats", 0.001) == "", "offset 0.001 accepted");
    CHECK(clip_parse_error("offset_beats", -0.0) == "", "offset -0.0 accepted (equals 0)");
    CHECK(clip_parse_error("length_beats", 0.001) == "", "length 0.001 accepted");
    CHECK(clip_parse_error("length_beats", 4096) == "", "length 4096 accepted");
    {
        nlohmann::json j = base_json();
        j["arrangement"][0]["offset_beats"] = 3;  // integer form
        j["arrangement"][0]["length_beats"] = 2;  // integer form
        op::Project p;
        std::string e;
        CHECK(parse(j, p, e), "integer-form trim keys parse");
        CHECK(p.arrangement[0].offset_beats == 3.0, "integer offset stored as double 3.0");
        CHECK(p.arrangement[0].length_beats.has_value() && *p.arrangement[0].length_beats == 2.0,
              "integer length stored as double 2.0");
    }

    // ---- Parse error strings, exact (spec §Clip trim parse errors). ----
    const std::string kOff = "clip.offset_beats must be a number in [0, 4096]";
    const std::string kLen = "clip.length_beats must be a number in (0, 4096]";
    CHECK(clip_parse_error("offset_beats", -0.5) == kOff, "offset negative rejected");
    CHECK(clip_parse_error("offset_beats", 4096.5) == kOff, "offset above max rejected");
    CHECK(clip_parse_error("offset_beats", "1") == kOff, "offset string rejected");
    CHECK(clip_parse_error("offset_beats", nullptr) == kOff, "offset null rejected");
    CHECK(clip_parse_error("offset_beats", nlohmann::json::parse("4294967296")) == kOff,
          "offset 4294967296 out of range, not wrapped");
    CHECK(clip_parse_error("offset_beats", nlohmann::json::parse("18446744073709551615")) == kOff,
          "offset 18446744073709551615 out of range, not wrapped");
    CHECK(clip_parse_error("length_beats", 0) == kLen, "length 0 rejected");
    CHECK(clip_parse_error("length_beats", -1.0) == kLen, "length negative rejected");
    CHECK(clip_parse_error("length_beats", 4096.5) == kLen, "length above max rejected");
    CHECK(clip_parse_error("length_beats", "2") == kLen, "length string rejected");
    CHECK(clip_parse_error("length_beats", nullptr) == kLen, "length null rejected");
    CHECK(clip_parse_error("length_beats", nlohmann::json::parse("4294967296")) == kLen,
          "length 4294967296 out of range, not wrapped");

    // ---- Error order: offset before length; the fades checks run before the trim ones. ----
    {
        nlohmann::json j = base_json();
        j["arrangement"][0]["offset_beats"] = -1.0;
        j["arrangement"][0]["length_beats"] = 0.0;
        op::Project p;
        std::string e;
        CHECK(!parse(j, p, e) && e == kOff, "offset error wins over length error");
    }
    {
        nlohmann::json j = base_json();
        j["arrangement"][0]["fade_out_beats"] = "x";
        j["arrangement"][0]["offset_beats"] = -1.0;
        op::Project p;
        std::string e;
        CHECK(!parse(j, p, e) && e == "clip.fade_out_beats must be a number in [0, 4096]",
              "fade error wins over trim error");
    }

    // ---- Canonical save: keys omitted at default, written when set, byte-identical re-parse. ----
    const std::string base_bytes = op::project_bytes(a);
    CHECK(base_bytes.find("offset_beats") == std::string::npos, "save omits offset_beats at default");
    CHECK(base_bytes.find("length_beats") == std::string::npos, "save omits length_beats when untrimmed");
    {
        nlohmann::json j = base_json();
        j["arrangement"][0]["offset_beats"] = 1.5;
        j["arrangement"][0]["length_beats"] = 2.25;
        op::Project p;
        std::string e;
        CHECK(parse(j, p, e), "trim keys parse into the model");
        const std::string bytes = op::project_bytes(p);
        CHECK(bytes.find("\"offset_beats\": 1.5") != std::string::npos, "save writes offset_beats when set");
        CHECK(bytes.find("\"length_beats\": 2.25") != std::string::npos, "save writes length_beats when present");
        op::Project again;
        CHECK(parse(nlohmann::json::parse(bytes), again, e) && op::project_bytes(again) == bytes,
              "trim keys round-trip byte-exact");
    }

    // ---- set_clip_trim: set both, null clears each side, offset 0 accepted, null-both restores. ----
    op::Project p = a;
    const std::string orig = op::project_bytes(p);
    CHECK(apply_text(p, R"({"op":"set_clip_trim","index":0,"offset_beats":1.0,"length_beats":2.0})", err),
          "set_clip_trim accepts two numbers");
    CHECK(p.arrangement[0].offset_beats == 1.0 && p.arrangement[0].length_beats.has_value() &&
              *p.arrangement[0].length_beats == 2.0,
          "op stores both values in the model");
    {
        const std::string bytes = op::project_bytes(p);
        CHECK(bytes.find("\"offset_beats\": 1.0") != std::string::npos &&
                  bytes.find("\"length_beats\": 2.0") != std::string::npos,
              "op result saves both keys");
    }
    CHECK(apply_text(p, R"({"op":"set_clip_trim","index":0,"offset_beats":null,"length_beats":2.0})", err) &&
              op::project_bytes(p).find("offset_beats") == std::string::npos,
          "null offset clears the offset key");
    CHECK(op::project_bytes(p).find("\"length_beats\": 2.0") != std::string::npos,
          "clearing offset keeps the length side");
    CHECK(apply_text(p, R"({"op":"set_clip_trim","index":0,"offset_beats":1.0,"length_beats":null})", err) &&
              op::project_bytes(p).find("length_beats") == std::string::npos,
          "null length clears the length key");
    CHECK(apply_text(p, R"({"op":"set_clip_trim","index":0,"offset_beats":null,"length_beats":null})", err),
          "null clears both sides");
    CHECK(op::project_bytes(p) == orig, "null-both restores the original bytes exactly");
    CHECK(apply_text(p, R"({"op":"set_clip_trim","index":0,"offset_beats":0,"length_beats":null})", err),
          "explicit offset 0 op accepted");
    CHECK(op::project_bytes(p) == orig, "offset 0 saves without the key");

    // ---- Op errors (exact messages; a rejected op leaves the project unchanged). ----
    const std::string before = op::project_bytes(p);
    CHECK(!apply_text(p, R"({"op":"set_clip_trim","index":0,"offset_beats":1.0,"length_beats":2.0,"bogus":3})", err) &&
              err == "set_clip_trim: unknown argument 'bogus'",
          "unknown key rejected");
    CHECK(!apply_text(p, R"({"op":"set_clip_trim","index":0,"length_beats":2.0})", err) &&
              err == "set_clip_trim: missing argument 'offset_beats'",
          "missing offset_beats rejected");
    CHECK(!apply_text(p, R"({"op":"set_clip_trim","index":0,"offset_beats":1.0})", err) &&
              err == "set_clip_trim: missing argument 'length_beats'",
          "missing length_beats rejected");
    CHECK(!apply_text(p, R"({"op":"set_clip_trim","index":0,"offset_beats":"1","length_beats":2.0})", err) &&
              err == "set_clip_trim: 'offset_beats' must be a number or null",
          "non-number offset rejected");
    CHECK(!apply_text(p, R"({"op":"set_clip_trim","index":0,"offset_beats":1.0,"length_beats":true})", err) &&
              err == "set_clip_trim: 'length_beats' must be a number or null",
          "non-number length rejected");
    CHECK(!apply_text(p, R"({"op":"set_clip_trim","index":0,"offset_beats":5000,"length_beats":2.0})", err) &&
              err == "set_clip_trim: clip.offset_beats must be a number in [0, 4096]",
          "offset out of range rejected via the parse");
    CHECK(!apply_text(p, R"({"op":"set_clip_trim","index":0,"offset_beats":1.0,"length_beats":0})", err) &&
              err == "set_clip_trim: clip.length_beats must be a number in (0, 4096]",
          "length 0 rejected via the parse");
    CHECK(!apply_text(p, R"({"op":"set_clip_trim","index":5,"offset_beats":1.0,"length_beats":2.0})", err) &&
              err == "set_clip_trim: 'index' out of range",
          "index out of range rejected");
    CHECK(op::project_bytes(p) == before, "rejected ops leave the project unchanged");

    // ---- History: undo/redo byte-exact (no new undo code; spec §Clip trim operation). ----
    op::History h;
    const std::string pre = op::project_bytes(p);
    CHECK(h.apply(p, nlohmann::json::parse(R"({"op":"set_clip_trim","index":0,"offset_beats":1.25,"length_beats":3.5})"), err),
          "History apply succeeds");
    const std::string post = op::project_bytes(p);
    CHECK(post != pre, "History apply changes the bytes");
    CHECK(h.undo(p) && op::project_bytes(p) == pre, "History undo byte-exact");
    CHECK(h.redo(p) && op::project_bytes(p) == post, "History redo byte-exact");

    // ---- opd diff: set / clear / partial-clear texts and replay (spec §opd diff amendment). ----
    nlohmann::json jb = base_json();
    jb["arrangement"][0]["offset_beats"] = 1.0;
    jb["arrangement"][0]["length_beats"] = 2.0;
    op::Project b;
    CHECK(parse(jb, b, err), "trimmed project b parses");
    std::vector<nlohmann::json> ops;
    CHECK(op::diff_projects(a, b, ops, err) && ops.size() == 1, "diff emits exactly one trim op (set)");
    CHECK(ops.size() == 1 &&
              ops[0].dump() == R"({"index":0,"length_beats":2.0,"offset_beats":1.0,"op":"set_clip_trim"})",
          "trim op text, set: b values, keys sorted");
    CHECK(replay(ops, a, b, err), "set journal replay reproduces b byte-for-byte");

    CHECK(op::diff_projects(b, a, ops, err) && ops.size() == 1 &&
              ops[0].dump() == R"({"index":0,"length_beats":null,"offset_beats":null,"op":"set_clip_trim"})",
          "trim op text, clear: both null");
    CHECK(replay(ops, b, a, err), "clear journal replay reproduces a byte-for-byte");

    nlohmann::json jc = base_json();
    jc["arrangement"][0]["length_beats"] = 2.0;  // length only: offset cleared
    op::Project c;
    CHECK(parse(jc, c, err), "length-only project c parses");
    CHECK(op::diff_projects(b, c, ops, err) && ops.size() == 1 &&
              ops[0].dump() == R"({"index":0,"length_beats":2.0,"offset_beats":null,"op":"set_clip_trim"})",
          "trim op text, partial clear: offset null, length kept");
    CHECK(replay(ops, b, c, err), "partial-clear journal replay reproduces c byte-for-byte");

    op::Project a2;
    CHECK(parse(base_json(), a2, err) && op::diff_projects(a, a2, ops, err) && ops.empty(),
          "equal projects emit nothing");

    nlohmann::json jd = base_json();
    jd["arrangement"][0]["gain_db"] = 1.5;
    jd["arrangement"][0]["fade_in_beats"] = 0.5;
    jd["arrangement"][0]["offset_beats"] = 1.0;
    op::Project d;
    CHECK(parse(jd, d, err), "gain+fades+trim project d parses");
    CHECK(op::diff_projects(a, d, ops, err) && ops.size() == 3, "gain+fades+trim emit three ops");
    {
        int ig = -1, if_in = -1, it = -1;
        for (int k = 0; k < static_cast<int>(ops.size()); ++k) {
            const std::string n = ops[k].at("op").get<std::string>();
            if (n == "set_clip_gain") ig = k;
            else if (n == "set_clip_fades") if_in = k;
            else if (n == "set_clip_trim") it = k;
        }
        CHECK(ig >= 0 && ig < if_in && if_in < it, "diff order: gain, fades, trim");
    }

    // ---- add_clip carries trim keys; diff expresses an added trimmed clip as add_clip only. ----
    {
        op::Project w = a;
        CHECK(apply_text(w, R"({"op":"add_clip","clip":{"pattern":0,"start_bar":2.0,"repeats":1,"offset_beats":0.5,"length_beats":1.0}})", err),
              "add_clip carries trim keys");
        const std::string wb = op::project_bytes(w);
        CHECK(wb.find("\"offset_beats\": 0.5") != std::string::npos &&
                  wb.find("\"length_beats\": 1.0") != std::string::npos,
              "added clip saves its trim keys");
        CHECK(op::diff_projects(a, w, ops, err) && ops.size() == 1 &&
                  ops[0].at("op") == "add_clip" && ops[0].at("clip").contains("length_beats"),
              "diff adds the trimmed clip via add_clip, no separate trim op");
    }

    if (g_failures == 0) std::printf("ALL PASS\n");
    else std::printf("%d FAILURES\n", g_failures);
    return g_failures;
}
