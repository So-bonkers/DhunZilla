// src/app/main.cpp — dhunzilla entry point (WP-0143, WP-0152, WP-0153).
//
// Modes, parsed from getCommandLineParameterArray() inside initialise(); options and the single
// positional project path may appear in any order:
//   dhunzilla [project.opj]                                  → window (native title bar + menus; live
//        transport through a PlaybackController, WP-0153)
//   dhunzilla --snapshot <out.png> [--apply <journal>] <project.opj>
//        headless: build the MainComponent off-screen and write a createComponentSnapshot() PNG.
//   dhunzilla --script <actions.txt> [--snapshot <out.png>] [--save-as <out.opj>]
//             [--apply <journal>] <project.opj>
//        headless scripted-input mode (WP-0152): drive the real MainComponent the way a user
//        would (slider drags, button clicks, Ctrl+Z), then optionally write a snapshot PNG and/or
//        save the edited project. No top-level window is created in either headless mode; no
//        audio device is ever opened and no PlaybackController exists (the component keeps its
//        inert transport).
//   dhunzilla --render-playback <out.wav> [--block B] [--stop-after N] <project.opj>   (WP-0153)
//        headless deterministic playback (dz::run_render_playback): render offline, drive the same
//        Player::process the audio callback uses with a fake device, write PCM16 WAV. JUCE is only
//        needed for application startup — no window, no audio device.
//   dhunzilla --position-at <FRAME> <project.opj>            → prints "position: <bar>.<beat>"
//   dhunzilla --play-seconds <N> <project.opj>               → local smoke ONLY (never CI): opens
//        the real audio device, plays until N s of frames or song end, prints one "played:" line.
//   --script/--snapshot/--save-as are one mode family; --render-playback, --position-at and
//   --play-seconds are each mutually exclusive with it and with each other (usage error, exit 2).
//
// Script grammar (UTF-8, one command per line; blank lines and lines whose first non-blank char
// is '#' are skipped; tokens separated by spaces/tabs):
//   drag gain <track> <db> | drag pan <track> <pan>        — a full gesture: ONE undo entry
//   click mute <track>     | click solo <track>            — one toggle = one undo entry
//   undo | redo | save                                     — doUndo/doRedo/doSave; false → exit 6
//   key ctrl+z | key ctrl+shift+z | key ctrl+s             — handleKey(); never fails at runtime
//   expect undo|redo|dirty <0|1>                           — Document flags must equal the value
//   expect gain <track> <db> | expect pan <track> <v>      — model values within 0.001
//   expect mute|solo <track> <0|1>                         — model flags equal
//   expect widget gain|pan <track> <v>                     — actual widget value within 0.001
//   expect widget mute|solo <track> <0|1>                  — actual widget toggle state
// The WHOLE file is parsed before any command runs (syntax errors → exit 4). On success stdout
// gets one line per executed command: `[<line>] <tokens…> undo=<0|1> redo=<0|1> dirty=<0|1>`
// (flags read AFTER the command), then the two WP-0143 snapshot lines if --snapshot, then
// `saved: <path as given>` if --save-as. On any failure stdout is empty and no PNG / save-as file
// is written; a valid but empty/comment-only script succeeds with no command lines.
//
// Exit codes — on failure exactly one line "dhunzilla: <reason>" goes to stderr, stdout stays
// empty (script failures are prefixed "script line <n>: "):
//   0  success
//   2  usage error (grammar checked before any filesystem access)
//   3  project file missing/unreadable/invalid — or --apply journal / --script file unreadable
//   4  --apply journal content rejected, or script syntax error
//   5  snapshot PNG / --save-as file / --render-playback WAV cannot be written, or render failed
//   6  --script: a command failed at run time (nothing to undo/redo, index out of range, save
//      failed, gesture refused); --play-seconds: audio failure (no device opened, or no frames
//      produced in time) — the two modes never coexist
//   7  an expect did not hold (--script)
// Snapshot success prints exactly:
//   title: DhunZilla — <file name>
//   snapshot: <out path as given> 1100x700

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <climits>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>
#include <nlohmann/json.hpp>

#include "doc/document.hpp"
#include "main_component.hpp"
#include "playback_controller.hpp"
#include "render_playback.hpp"

namespace {

void printError(const juce::String& reason) {
    std::fprintf(stderr, "dhunzilla: %s\n", reason.toRawUTF8());
}

// Strict decimal integer (optional leading '-'): the whole string must be consumed.
bool parse_ll(const juce::String& text, long long& out) {
    const char* raw = text.toRawUTF8();
    if (*raw == '\0') return false;
    errno = 0;
    char* end = nullptr;
    const long long value = std::strtoll(raw, &end, 10);
    if (errno != 0 || end == raw || *end != '\0') return false;
    out = value;
    return true;
}

// Read `path` as a JSON-lines operations journal (blank lines and '#' comments skipped), parse
// EVERY line first (so malformed JSON is reported before anything is applied), then apply the ops
// to `doc` in order through Document::apply. A rejected op stops there with earlier ops already
// applied to `doc` — harmless here because the caller exits 4 without writing any output.
// Returns 0, or the exit code (3 file unreadable, 4 content rejected) with `reason` set.
int applyJournal(const juce::String& path, op::Document& doc, juce::String& reason) {
    // JUCE 9 dropped File::readTextFile(String&) — read through an InputStream instead.
    auto stream = juce::File(path).createInputStream();
    if (!stream) {
        reason = juce::String::fromUTF8("cannot read journal file ") + path;
        return 3;
    }
    const juce::String text = stream->readEntireStreamAsString();

    std::vector<nlohmann::json> ops;
    juce::StringArray lines;
    lines.addLines(text);
    for (const auto& raw : lines) {
        const auto line = raw.trim();
        if (line.isEmpty() || line[0] == '#') continue;  // isEmpty() first: short-circuits [0]
        nlohmann::json op;
        try {
            op = nlohmann::json::parse(line.toRawUTF8());
        } catch (const nlohmann::json::exception&) {
            reason = juce::String::fromUTF8("journal line is not valid JSON: ") + line;
            return 4;
        }
        if (!op.is_object()) {
            reason = juce::String::fromUTF8("journal line is not a JSON object: ") + line;
            return 4;
        }
        ops.push_back(std::move(op));
    }

    for (auto& op_json : ops) {
        std::string err;
        if (!doc.apply(op_json, err)) {
            reason = juce::String::fromUTF8("journal operation rejected: ") + juce::String::fromUTF8(err.c_str());
            return 4;
        }
    }
    return 0;
}

// --------------------------------------------------------------------------- scripted input mode

enum class Cmd {
    dragGain, dragPan, clickMute, clickSolo,
    undo, redo, save,
    keyUndo, keyRedo, keySave,
    expectUndo, expectRedo, expectDirty,
    expectGain, expectPan, expectMute, expectSolo,
    expectWidgetGain, expectWidgetPan, expectWidgetMute, expectWidgetSolo,
};

struct ScriptCommand {
    int line = 0;                    // 1-based source line number
    std::vector<std::string> tokens;  // original tokens, echoed on stdout joined by single spaces
    Cmd cmd{};
    int track = 0;                   // <track> for drag/click/track-expects
    double value = 0.0;              // <db>/<pan>/<v> for numeric commands and expects
    bool flag = false;               // <0|1> for boolean expects
};

// Split on spaces/tabs only (the script grammar's separators); empty tokens never appear.
std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> out;
    std::string tok;
    for (const char c : line) {
        if (c == ' ' || c == '\t') {
            if (!tok.empty()) { out.push_back(tok); tok.clear(); }
        } else {
            tok += c;
        }
    }
    if (!tok.empty()) out.push_back(tok);
    return out;
}

// std::strtod over the WHOLE token: anything not fully numeric is a syntax error.
bool parseNumber(const std::string& tok, double& out) {
    if (tok.empty()) return false;
    const char* begin = tok.c_str();
    char* end = nullptr;
    errno = 0;
    const double v = std::strtod(begin, &end);
    if (errno == ERANGE || end != begin + tok.size() || end == begin) return false;
    out = v;
    return true;
}

// std::strtol over the WHOLE token (negatives allowed; range-checked at run time).
bool parseTrack(const std::string& tok, int& out) {
    if (tok.empty()) return false;
    const char* begin = tok.c_str();
    char* end = nullptr;
    errno = 0;
    const long v = std::strtol(begin, &end, 10);
    if (errno == ERANGE || end != begin + tok.size() || end == begin) return false;
    if (v < INT_MIN || v > INT_MAX) return false;
    out = static_cast<int>(v);
    return true;
}

bool parseBool(const std::string& tok, bool& out) {
    if (tok == "0") { out = false; return true; }
    if (tok == "1") { out = true;  return true; }
    return false;
}

// Parse the WHOLE script before anything runs. Returns 0 on success, or 4 with `reason` set and
// `bad_line` holding the offending 1-based line number.
int parseScript(const std::string& text, std::vector<ScriptCommand>& out, int& bad_line,
                juce::String& reason) {
    // Split on '\n', tolerate CRLF; comments/blank lines skipped (first non-blank char '#').
    std::vector<std::string> lines;
    std::string current;
    for (const char c : text) {
        if (c == '\n') { lines.push_back(current); current.clear(); }
        else if (c != '\r') current += c;
    }
    lines.push_back(current);

    auto fail = [&](int line_no, const std::string& what) {
        bad_line = line_no;
        reason = juce::String::fromUTF8(what.c_str());
        return 4;
    };

    for (std::size_t li = 0; li < lines.size(); ++li) {
        const int line_no = static_cast<int>(li) + 1;
        const auto toks = tokenize(lines[li]);
        if (toks.empty() || toks[0][0] == '#') continue;  // blank or comment line

        ScriptCommand sc;
        sc.line = line_no;
        sc.tokens = toks;
        const auto& c0 = toks[0];

        if (c0 == "drag") {
            if (toks.size() != 4) return fail(line_no, "'drag' expects 'gain|pan <track> <value>'");
            if (toks[1] == "gain")      sc.cmd = Cmd::dragGain;
            else if (toks[1] == "pan")  sc.cmd = Cmd::dragPan;
            else return fail(line_no, "unknown drag control '" + toks[1] + "'");
            if (!parseTrack(toks[2], sc.track))
                return fail(line_no, "expected a track index, got '" + toks[2] + "'");
            if (!parseNumber(toks[3], sc.value))
                return fail(line_no, "expected a number, got '" + toks[3] + "'");
        } else if (c0 == "click") {
            if (toks.size() != 3) return fail(line_no, "'click' expects 'mute|solo <track>'");
            if (toks[1] == "mute")      sc.cmd = Cmd::clickMute;
            else if (toks[1] == "solo") sc.cmd = Cmd::clickSolo;
            else return fail(line_no, "unknown click control '" + toks[1] + "'");
            if (!parseTrack(toks[2], sc.track))
                return fail(line_no, "expected a track index, got '" + toks[2] + "'");
        } else if (c0 == "undo") {
            if (toks.size() != 1) return fail(line_no, "'undo' takes no arguments");
            sc.cmd = Cmd::undo;
        } else if (c0 == "redo") {
            if (toks.size() != 1) return fail(line_no, "'redo' takes no arguments");
            sc.cmd = Cmd::redo;
        } else if (c0 == "save") {
            if (toks.size() != 1) return fail(line_no, "'save' takes no arguments");
            sc.cmd = Cmd::save;
        } else if (c0 == "key") {
            if (toks.size() != 2) return fail(line_no, "'key' expects 'ctrl+z|ctrl+shift+z|ctrl+s'");
            if (toks[1] == "ctrl+z")           sc.cmd = Cmd::keyUndo;
            else if (toks[1] == "ctrl+shift+z") sc.cmd = Cmd::keyRedo;
            else if (toks[1] == "ctrl+s")       sc.cmd = Cmd::keySave;
            else return fail(line_no, "unknown key combo '" + toks[1] + "'");
        } else if (c0 == "expect") {
            if (toks.size() < 3) return fail(line_no, "'expect' expects an object and a value");
            const auto& what = toks[1];
            if (what == "undo" || what == "redo" || what == "dirty") {
                if (toks.size() != 3) return fail(line_no, "'expect " + what + "' expects '<0|1>'");
                if (!parseBool(toks[2], sc.flag))
                    return fail(line_no, "expected 0 or 1, got '" + toks[2] + "'");
                sc.cmd = what == "undo" ? Cmd::expectUndo
                       : what == "redo" ? Cmd::expectRedo : Cmd::expectDirty;
            } else if (what == "gain" || what == "pan") {
                if (toks.size() != 4) return fail(line_no, "'expect " + what + "' expects '<track> <value>'");
                if (!parseTrack(toks[2], sc.track))
                    return fail(line_no, "expected a track index, got '" + toks[2] + "'");
                if (!parseNumber(toks[3], sc.value))
                    return fail(line_no, "expected a number, got '" + toks[3] + "'");
                sc.cmd = what == "gain" ? Cmd::expectGain : Cmd::expectPan;
            } else if (what == "mute" || what == "solo") {
                if (toks.size() != 4) return fail(line_no, "'expect " + what + "' expects '<track> <0|1>'");
                if (!parseTrack(toks[2], sc.track))
                    return fail(line_no, "expected a track index, got '" + toks[2] + "'");
                if (!parseBool(toks[3], sc.flag))
                    return fail(line_no, "expected 0 or 1, got '" + toks[3] + "'");
                sc.cmd = what == "mute" ? Cmd::expectMute : Cmd::expectSolo;
            } else if (what == "widget") {
                if (toks.size() != 5) return fail(line_no, "'expect widget' expects 'gain|pan <track> <value>' or 'mute|solo <track> <0|1>'");
                const auto& sub = toks[2];
                if (!parseTrack(toks[3], sc.track))
                    return fail(line_no, "expected a track index, got '" + toks[3] + "'");
                if (sub == "gain" || sub == "pan") {
                    if (!parseNumber(toks[4], sc.value))
                        return fail(line_no, "expected a number, got '" + toks[4] + "'");
                    sc.cmd = sub == "gain" ? Cmd::expectWidgetGain : Cmd::expectWidgetPan;
                } else if (sub == "mute" || sub == "solo") {
                    if (!parseBool(toks[4], sc.flag))
                        return fail(line_no, "expected 0 or 1, got '" + toks[4] + "'");
                    sc.cmd = sub == "mute" ? Cmd::expectWidgetMute : Cmd::expectWidgetSolo;
                } else {
                    return fail(line_no, "unknown widget control '" + sub + "'");
                }
            } else {
                return fail(line_no, "unknown expect object '" + what + "'");
            }
        } else {
            return fail(line_no, "unknown command '" + c0 + "'");
        }

        out.push_back(std::move(sc));
    }
    return 0;
}

std::string indexRangeError(int track, int n) {
    return "track index " + std::to_string(track) + " out of range (0.." +
           std::to_string(n - 1) + ")";
}

std::string fmt3(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.3f", v);
    return buf;
}

bool nearEq(double a, double b) { return std::fabs(a - b) <= 0.001; }

std::string boolTxt(bool b) { return b ? "1" : "0"; }

juce::String jstr(const std::string& s) { return juce::String::fromUTF8(s.c_str()); }

// Minimal top-level window: native title bar with a close button, File/Edit menu bar, content
// owned here. The menu model is the window itself (it outlives the menu bar component).
class MainWindow final : public juce::DocumentWindow, public juce::MenuBarModel {
public:
    enum MenuIds { saveItem = 1, undoItem, redoItem };

    MainWindow(const juce::String& title, op::Document& doc, dz::PlaybackController* playback)
        : DocumentWindow(title, juce::Colour(0xff14151a), juce::DocumentWindow::closeButton),
          doc_(doc) {
        // `doc` and `playback` outlive us (App member order: window_ destroyed first).
        auto content = std::make_unique<dz::MainComponent>(doc, playback);
        content_ = content.get();
        setContentOwned(content.release(), true);
        content_->onDocumentChanged = [this] { menuItemsChanged(); };  // keep Undo/Redo enablement live
        setMenuBar(this);
        centreWithSize(getWidth(), getHeight());
    }

    ~MainWindow() override {
        if (content_ != nullptr) content_->onDocumentChanged = nullptr;  // `this` goes away first
        setMenuBar(nullptr);
    }

    void closeButtonPressed() override {
        if (auto* app = juce::JUCEApplicationBase::getInstance()) app->systemRequestedQuit();
    }

    // --- juce::MenuBarModel ---
    juce::StringArray getMenuBarNames() override {
        return {juce::String::fromUTF8("File"), juce::String::fromUTF8("Edit")};
    }

    juce::PopupMenu getMenuForIndex(int index, const juce::String&) override {
        juce::PopupMenu menu;
        if (index == 0) {
            juce::PopupMenu::Item save(juce::String::fromUTF8("Save"));
            save.itemID = saveItem;
            save.shortcutKeyDescription = juce::String::fromUTF8("Ctrl+S");
            menu.addItem(save);
        } else if (index == 1) {
            juce::PopupMenu::Item undo(juce::String::fromUTF8("Undo"));
            undo.itemID = undoItem;
            undo.shortcutKeyDescription = juce::String::fromUTF8("Ctrl+Z");
            undo.isEnabled = doc_.can_undo();
            menu.addItem(undo);

            juce::PopupMenu::Item redo(juce::String::fromUTF8("Redo"));
            redo.itemID = redoItem;
            redo.shortcutKeyDescription = juce::String::fromUTF8("Ctrl+Shift+Z");
            redo.isEnabled = doc_.can_redo();
            menu.addItem(redo);
        }
        return menu;
    }

    void menuItemSelected(int menuItemID, int /*topLevelMenuIndex*/) override {
        if (content_ == nullptr) return;
        switch (menuItemID) {
            case saveItem: {
                std::string err;
                if (!content_->doSave(err))  // one warning line to stderr; the window stays open
                    printError(juce::String::fromUTF8(err.c_str()));
                break;
            }
            case undoItem: content_->doUndo(); break;
            case redoItem: content_->doRedo(); break;
            default: break;
        }
    }

private:
    op::Document& doc_;               // for menu enablement (can_undo/can_redo)
    dz::MainComponent* content_ = nullptr;  // owned by the DocumentWindow base
};

class App final : public juce::JUCEApplication, private juce::Timer {
public:
    const juce::String getApplicationName() override { return juce::String::fromUTF8("dhunzilla"); }
    const juce::String getApplicationVersion() override { return juce::String::fromUTF8("0.1.0"); }

    void initialise(const juce::String&) override;
    void shutdown() override {
        // Spec order: window, then controller, then document (MainComponent holds references).
        window_.reset();
        playback_.reset();
        doc_.reset();
    }
    void systemRequestedQuit() override { quit(); }

private:
    // --play-seconds watcher: finish on target frames or song end, bail out after N + 40 s.
    void timerCallback() override;

    std::unique_ptr<op::Document> doc_;             // declared first → destroyed last
    std::unique_ptr<dz::PlaybackController> playback_;
    std::unique_ptr<MainWindow> window_;

    bool smoke_active_ = false;                     // --play-seconds in flight
    long long smoke_target_frames_ = 0;
    juce::uint32 smoke_start_ms_ = 0;               // unsigned math survives counter wraparound
    long long smoke_limit_ms_ = 0;
};

void App::initialise(const juce::String&) {
    // ---- parse: options and the (single) positional project path, any order ----
    const auto args = getCommandLineParameterArray();
    juce::String snapshot_path, apply_path, script_path, save_as_path, render_pb_path, project_path;
    bool have_snapshot = false, have_apply = false, have_script = false, have_save_as = false;
    bool have_render_pb = false, have_pos_at = false, have_play_sec = false;
    bool have_block = false, have_stop_after = false;
    long long position_at = 0, play_seconds = 0, stop_after = -1;
    int block = 128;

    // Every usage error below is a grammar check: none touches the filesystem.
    for (int i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg == "--snapshot" || arg == "--apply" || arg == "--script" || arg == "--save-as" ||
            arg == "--render-playback") {
            if (i + 1 >= args.size()) {
                printError(juce::String::fromUTF8("usage: ") + arg + juce::String::fromUTF8(" requires a path"));
                setApplicationReturnValue(2); quit(); return;
            }
            const auto& value = args[i + 1];
            if (arg == "--snapshot")             { snapshot_path  = value; have_snapshot  = true; }
            else if (arg == "--apply")           { apply_path     = value; have_apply     = true; }
            else if (arg == "--script")          { script_path    = value; have_script    = true; }
            else if (arg == "--save-as")         { save_as_path   = value; have_save_as   = true; }
            else                                 { render_pb_path = value; have_render_pb = true; }
            ++i;
        } else if (arg == "--block" || arg == "--stop-after" || arg == "--position-at" ||
                   arg == "--play-seconds") {
            long long value = 0;
            if (i + 1 >= args.size() || !parse_ll(args[i + 1], value)) {
                printError(juce::String::fromUTF8("usage: ") + arg +
                           juce::String::fromUTF8(" requires an integer value"));
                setApplicationReturnValue(2); quit(); return;
            }
            ++i;
            if (arg == "--block") {
                if (value < 1 || value > 8192) {
                    printError(juce::String::fromUTF8("usage: --block must be in 1..8192"));
                    setApplicationReturnValue(2); quit(); return;
                }
                block = static_cast<int>(value); have_block = true;
            } else if (arg == "--stop-after") {
                if (value < 1) {
                    printError(juce::String::fromUTF8("usage: --stop-after must be >= 1"));
                    setApplicationReturnValue(2); quit(); return;
                }
                stop_after = value; have_stop_after = true;
            } else if (arg == "--position-at") {
                if (value < 0) {
                    printError(juce::String::fromUTF8("usage: --position-at must be >= 0"));
                    setApplicationReturnValue(2); quit(); return;
                }
                position_at = value; have_pos_at = true;
            } else {
                if (value < 1) {
                    printError(juce::String::fromUTF8("usage: --play-seconds must be >= 1"));
                    setApplicationReturnValue(2); quit(); return;
                }
                play_seconds = value; have_play_sec = true;
            }
        } else if (arg.startsWith(juce::String::fromUTF8("--"))) {
            printError(juce::String::fromUTF8("usage: unknown option ") + arg);
            setApplicationReturnValue(2); quit(); return;
        } else {
            if (!project_path.isEmpty()) {
                printError(juce::String::fromUTF8("usage: more than one project file given"));
                setApplicationReturnValue(2); quit(); return;
            }
            project_path = arg;
        }
    }
    // Usage rules — all checked before touching the file system.
    if (project_path.isEmpty() && (have_snapshot || have_script || have_save_as)) {
        const juce::String opt = have_snapshot ? juce::String::fromUTF8("--snapshot")
                               : have_script   ? juce::String::fromUTF8("--script")
                                               : juce::String::fromUTF8("--save-as");
        printError(juce::String::fromUTF8("usage: ") + opt + juce::String::fromUTF8(" requires a project file"));
        setApplicationReturnValue(2); quit(); return;
    }

    // --script/--snapshot/--save-as form ONE mode family (they combine); the others are exclusive.
    const int modes = ((have_snapshot || have_script) ? 1 : 0) + (have_render_pb ? 1 : 0) +
                      (have_pos_at ? 1 : 0) + (have_play_sec ? 1 : 0);
    if (modes > 1) {
        printError(juce::String::fromUTF8(
            "usage: at most one of --snapshot, --render-playback, --position-at, --play-seconds"));
        setApplicationReturnValue(2); quit(); return;
    }
    if ((have_block || have_stop_after) && !have_render_pb) {
        printError(juce::String::fromUTF8("usage: --block and --stop-after require --render-playback"));
        setApplicationReturnValue(2); quit(); return;
    }
    if (have_save_as && !have_script) {  // --save-as only makes sense next to a driving script
        printError(juce::String::fromUTF8("usage: --save-as requires --script"));
        setApplicationReturnValue(2); quit(); return;
    }
    if (have_apply && !have_snapshot && !have_script) {  // journal edits need a headless mode
        printError(juce::String::fromUTF8("usage: --apply requires --snapshot or --script"));
        setApplicationReturnValue(2); quit(); return;
    }
    if (modes == 1 && project_path.isEmpty()) {
        juce::String flag = juce::String::fromUTF8("--snapshot");
        if (have_render_pb) flag = juce::String::fromUTF8("--render-playback");
        else if (have_pos_at) flag = juce::String::fromUTF8("--position-at");
        else if (have_play_sec) flag = juce::String::fromUTF8("--play-seconds");
        printError(juce::String::fromUTF8("usage: ") + flag + juce::String::fromUTF8(" requires a project file"));
        setApplicationReturnValue(2); quit(); return;
    }

    // ---- headless render-playback mode (dzplayer; no window, no audio device) ----
    if (have_render_pb) {
        std::string err;
        const int rc = dz::run_render_playback(project_path.toStdString(), render_pb_path.toStdString(),
                                               block, stop_after, err);
        if (rc != 0) {
            printError(juce::String::fromUTF8(err.c_str()));
            setApplicationReturnValue(rc); quit(); return;
        }
        setApplicationReturnValue(0); quit(); return;
    }

    // ---- headless position-at mode: bar.beat of one frame via the engine's tempo map ----
    if (have_pos_at) {
        op::Project p;
        std::string err;
        if (!op::load_project(project_path.toStdString(), p, err)) {
            printError(juce::String::fromUTF8(err.c_str()));
            setApplicationReturnValue(3); quit(); return;
        }
        std::printf("position: %s\n", dz::format_position(p, position_at).c_str());
        setApplicationReturnValue(0); quit(); return;
    }

    // ---- load the project (a default-constructed Document backs the no-path window) ----
    doc_ = std::make_unique<op::Document>();
    if (!project_path.isEmpty()) {
        std::string err;
        if (!op::Document::open(project_path.toRawUTF8(), *doc_, err)) {
            printError(juce::String::fromUTF8(err.c_str()));
            setApplicationReturnValue(3); quit(); return;
        }
    }

    // ---- headless snapshot mode (no --script): exactly the WP-0143 behaviour (never opens a device
    //      or creates a controller) ----
    if (!have_script && have_snapshot) {
        if (have_apply) {
            juce::String reason;
            const int code = applyJournal(apply_path, *doc_, reason);
            if (code != 0) {
                printError(reason);
                setApplicationReturnValue(code); quit(); return;
            }
        }

        dz::MainComponent component(*doc_);  // off-screen, no controller: WP-0143 inert transport
        const auto image = component.createComponentSnapshot(component.getLocalBounds(), true, 1.0f);

        juce::File out(snapshot_path);
        out.deleteFile();  // a failed write must not leave stale bytes behind
        bool written = false;
        if (image.getWidth() > 0) {
            if (auto stream = out.createOutputStream()) {
                juce::PNGImageFormat png;
                written = png.writeImageToStream(image, *stream);
            }
        }
        if (!written) {
            printError(juce::String::fromUTF8("cannot write PNG to ") + snapshot_path);
            setApplicationReturnValue(5); quit(); return;
        }

        std::printf("title: %s\n", dz::windowTitleFor(project_path.toRawUTF8()).toRawUTF8());
        std::printf("snapshot: %s %dx%d\n", snapshot_path.toRawUTF8(), image.getWidth(), image.getHeight());
        setApplicationReturnValue(0); quit(); return;
    }

    // ---- headless scripted-input mode (WP-0152) ----
    if (have_script) {
        if (have_apply) {  // journal first, exactly as in snapshot mode
            juce::String reason;
            const int code = applyJournal(apply_path, *doc_, reason);
            if (code != 0) {
                printError(reason);
                setApplicationReturnValue(code); quit(); return;
            }
        }

        dz::MainComponent component(*doc_);  // the real component, off-screen

        auto stream = juce::File(script_path).createInputStream();
        if (!stream) {
            printError(juce::String::fromUTF8("cannot read script file ") + script_path);
            setApplicationReturnValue(3); quit(); return;
        }
        const std::string script_text = stream->readEntireStreamAsString().toRawUTF8();

        // Parse the WHOLE script before running anything.
        std::vector<ScriptCommand> commands;
        int bad_line = 0;
        juce::String reason;
        const int parse_code = parseScript(script_text, commands, bad_line, reason);
        if (parse_code != 0) {
            printError(juce::String::fromUTF8(("script line " + std::to_string(bad_line) + ": ").c_str()) +
                       reason);
            setApplicationReturnValue(parse_code); quit(); return;
        }

        // stdout is buffered: any later failure (runtime, PNG, save-as) must print NOTHING.
        std::string out_buf;
        const auto fail = [&](int code, int line_no, const juce::String& why) {
            printError(juce::String::fromUTF8(("script line " + std::to_string(line_no) + ": ").c_str()) +
                       why);
            setApplicationReturnValue(code); quit();
        };

        for (const auto& sc : commands) {
            const int n = component.trackCount();

            // --- run-time index checks for the track-based expects ---
            if ((sc.cmd >= Cmd::expectGain && sc.cmd <= Cmd::expectSolo) ||
                (sc.cmd >= Cmd::expectWidgetGain && sc.cmd <= Cmd::expectWidgetSolo)) {
                if (sc.track < 0 || sc.track >= n) {
                    fail(6, sc.line, juce::String::fromUTF8(indexRangeError(sc.track, n).c_str()));
                    return;
                }
            }

            bool ok = true;
            juce::String why;
            int fcode = 7;  // failure exit code: expect misses are 7, command failures are 6
            switch (sc.cmd) {
                case Cmd::dragGain:
                case Cmd::dragPan: {
                    std::string err;
                    ok = component.scriptDrag(sc.cmd == Cmd::dragGain ? "gain" : "pan",
                                              sc.track, sc.value, err);
                    why = juce::String::fromUTF8(err.c_str());
                    fcode = 6;
                    break;
                }
                case Cmd::clickMute:
                case Cmd::clickSolo: {
                    std::string err;
                    ok = component.scriptClick(sc.cmd == Cmd::clickMute ? "mute" : "solo",
                                               sc.track, err);
                    why = juce::String::fromUTF8(err.c_str());
                    fcode = 6;
                    break;
                }
                case Cmd::undo:
                    if ((ok = component.doUndo()) == false) { why = juce::String::fromUTF8("nothing to undo"); fcode = 6; }
                    break;
                case Cmd::redo:
                    if ((ok = component.doRedo()) == false) { why = juce::String::fromUTF8("nothing to redo"); fcode = 6; }
                    break;
                case Cmd::save: {
                    std::string err;
                    ok = component.doSave(err);
                    if (!ok) {
                        why = err.empty() ? juce::String::fromUTF8("save failed")
                                          : juce::String::fromUTF8("save failed: ") +
                                                juce::String::fromUTF8(err.c_str());
                        fcode = 6;
                    }
                    break;
                }
                case Cmd::keyUndo:
                    component.handleKey(juce::KeyPress('z', juce::ModifierKeys::commandModifier, 0));
                    break;
                case Cmd::keyRedo:
                    component.handleKey(juce::KeyPress('z',
                        juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0));
                    break;
                case Cmd::keySave:
                    component.handleKey(juce::KeyPress('s', juce::ModifierKeys::commandModifier, 0));
                    break;

                case Cmd::expectUndo:
                    if (doc_->can_undo() != sc.flag) {
                        ok = false;
                        why = jstr("expected undo " + boolTxt(sc.flag) + ", got " +
                                   boolTxt(doc_->can_undo()));
                    }
                    break;
                case Cmd::expectRedo:
                    if (doc_->can_redo() != sc.flag) {
                        ok = false;
                        why = jstr("expected redo " + boolTxt(sc.flag) + ", got " +
                                   boolTxt(doc_->can_redo()));
                    }
                    break;
                case Cmd::expectDirty:
                    if (doc_->dirty() != sc.flag) {
                        ok = false;
                        why = jstr("expected dirty " + boolTxt(sc.flag) + ", got " +
                                   boolTxt(doc_->dirty()));
                    }
                    break;

                case Cmd::expectGain:
                case Cmd::expectPan: {
                    const auto rows = doc_->track_rows();
                    const double got = sc.cmd == Cmd::expectGain
                                           ? static_cast<double>(rows[static_cast<std::size_t>(sc.track)].gain_db)
                                           : static_cast<double>(rows[static_cast<std::size_t>(sc.track)].pan);
                    if (!nearEq(got, sc.value)) {
                        ok = false;
                        why = jstr(std::string(sc.cmd == Cmd::expectGain ? "expected gain " : "expected pan ") +
                                   fmt3(sc.value) + ", got " + fmt3(got));
                    }
                    break;
                }
                case Cmd::expectMute:
                case Cmd::expectSolo: {
                    const auto rows = doc_->track_rows();
                    const bool got = sc.cmd == Cmd::expectMute
                                         ? rows[static_cast<std::size_t>(sc.track)].mute
                                         : rows[static_cast<std::size_t>(sc.track)].solo;
                    if (got != sc.flag) {
                        ok = false;
                        why = jstr(std::string(sc.cmd == Cmd::expectMute ? "expected mute " : "expected solo ") +
                                   boolTxt(sc.flag) + ", got " + boolTxt(got));
                    }
                    break;
                }
                case Cmd::expectWidgetGain:
                case Cmd::expectWidgetPan: {
                    const double got = component.widgetValue(sc.cmd == Cmd::expectWidgetGain ? "gain" : "pan", sc.track);
                    if (!nearEq(got, sc.value)) {
                        ok = false;
                        why = jstr(std::string(sc.cmd == Cmd::expectWidgetGain ? "expected widget gain " : "expected widget pan ") +
                                   fmt3(sc.value) + ", got " + fmt3(got));
                    }
                    break;
                }
                case Cmd::expectWidgetMute:
                case Cmd::expectWidgetSolo: {
                    const double got = component.widgetValue(sc.cmd == Cmd::expectWidgetMute ? "mute" : "solo", sc.track);
                    if ((got >= 0.5) != sc.flag) {
                        ok = false;
                        why = jstr(std::string(sc.cmd == Cmd::expectWidgetMute ? "expected widget mute " : "expected widget solo ") +
                                   boolTxt(sc.flag) + ", got " + boolTxt(got >= 0.5));
                    }
                    break;
                }
            }

            if (!ok) { fail(fcode, sc.line, why); return; }  // command failures: 6, expect misses: 7

            // Echo line: flags are read AFTER the command ran.
            out_buf += "[" + std::to_string(sc.line) + "] ";
            for (std::size_t t = 0; t < sc.tokens.size(); ++t) {
                if (t != 0) out_buf += ' ';
                out_buf += sc.tokens[t];
            }
            out_buf += " undo=" + boolTxt(doc_->can_undo()) +
                       " redo=" + boolTxt(doc_->can_redo()) +
                       " dirty=" + boolTxt(doc_->dirty());
            out_buf += '\n';
        }

        // --- snapshot (if asked): the two WP-0143 lines, after all command lines ---
        if (have_snapshot) {
            const auto image = component.createComponentSnapshot(component.getLocalBounds(), true, 1.0f);
            juce::File out(snapshot_path);
            out.deleteFile();  // a failed write must not leave stale bytes behind
            bool written = false;
            if (image.getWidth() > 0) {
                if (auto png_stream = out.createOutputStream()) {
                    juce::PNGImageFormat png;
                    written = png.writeImageToStream(image, *png_stream);
                }
            }
            if (!written) {
                printError(juce::String::fromUTF8("cannot write PNG to ") + snapshot_path);
                setApplicationReturnValue(5); quit(); return;  // stdout buffer stays undelivered
            }
            out_buf += "title: ";
            out_buf += dz::windowTitleFor(project_path.toRawUTF8()).toRawUTF8();
            out_buf += "\nsnapshot: ";
            out_buf += snapshot_path.toRawUTF8();
            out_buf += " " + std::to_string(image.getWidth()) + "x" + std::to_string(image.getHeight());
            out_buf += '\n';
        }

        // --- save-as (if asked): the final model state through Document::save_as ---
        if (have_save_as) {
            std::string err;
            if (!doc_->save_as(save_as_path.toRawUTF8(), err)) {
                if (have_snapshot) juce::File(snapshot_path).deleteFile();  // no output on failure
                printError(juce::String::fromUTF8("cannot write save-as file ") + save_as_path);
                setApplicationReturnValue(5); quit(); return;
            }
            out_buf += "saved: ";
            out_buf += save_as_path.toRawUTF8();
            out_buf += '\n';
        }

        std::fputs(out_buf.c_str(), stdout);  // success: everything at once, in order
        setApplicationReturnValue(0); quit(); return;
    }

    // ---- --play-seconds smoke (local only, never CI): real device, no window ----
    if (have_play_sec) {
        playback_ = std::make_unique<dz::PlaybackController>();
        std::string aerr;
        if (!playback_->openDevice(aerr)) {
            printError(juce::String::fromUTF8("audio: ") + juce::String::fromUTF8(aerr.c_str()));
            setApplicationReturnValue(6); quit(); return;
        }
        playback_->play(*doc_);
        smoke_target_frames_ = play_seconds * 48000;
        smoke_start_ms_ = juce::Time::getMillisecondCounter();
        smoke_limit_ms_ = (play_seconds + 40) * 1000;
        smoke_active_ = true;
        startTimer(50);
        return;  // the timer ends the run (quit() from timerCallback)
    }

    // ---- window mode: live transport through the controller ----
    playback_ = std::make_unique<dz::PlaybackController>();
    std::string aerr;
    if (!playback_->openDevice(aerr)) {
        // One line to stderr, then continue without sound (the UI shows "no audio device").
        printError(juce::String::fromUTF8("audio: ") + juce::String::fromUTF8(aerr.c_str()));
    }

    const juce::String title = project_path.isEmpty()
        ? juce::String::fromUTF8("DhunZilla")
        : dz::windowTitleFor(project_path.toRawUTF8());
    window_ = std::make_unique<MainWindow>(title, *doc_, playback_.get());
    window_->setVisible(true);
}

void App::timerCallback() {
    if (!smoke_active_) return;
    auto& player = playback_->player();
    const long long played = player.frames_played();
    // Song end: playing() only drops once a buffer has actually run to its end.
    if (played >= smoke_target_frames_ || (!player.playing() && played > 0)) {
        std::printf("played: %lld frames, device=%s sr=%d buf=%d\n", played,
                    playback_->deviceName().c_str(), static_cast<int>(playback_->deviceSampleRate()),
                    playback_->deviceBufferSize());
        smoke_active_ = false;
        setApplicationReturnValue(0); quit(); return;
    }
    if (played == 0 &&
        juce::Time::getMillisecondCounter() - smoke_start_ms_ > static_cast<juce::uint32>(smoke_limit_ms_)) {
        printError(juce::String::fromUTF8("audio: callback produced no frames"));
        smoke_active_ = false;
        setApplicationReturnValue(6); quit();
    }
}

}  // namespace

START_JUCE_APPLICATION(App)
