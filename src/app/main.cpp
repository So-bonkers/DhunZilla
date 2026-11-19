// src/app/main.cpp — dhunzilla entry point (WP-0143, WP-0153).
//
// Modes, parsed from getCommandLineParameterArray() inside initialise(); options may appear in
// any order relative to the project path:
//   dhunzilla [project.opj]                                  → window (native title bar)
//   dhunzilla --snapshot <out.png> [--apply <journal>] <project.opj>
//        headless: build the MainComponent off-screen and write a createComponentSnapshot() PNG.
//        No top-level window is created in this mode; no audio device is ever opened, and no
//        PlaybackController exists (the component keeps its inert WP-0143 transport).
//   dhunzilla --render-playback <out.wav> [--block B] [--stop-after N] <project.opj>
//        headless deterministic playback (dz::run_render_playback): render offline, drive the same
//        Player::process the audio callback uses with a fake device, write PCM16 WAV. JUCE is only
//        needed for application startup — no window, no audio device.
//   dhunzilla --position-at <FRAME> <project.opj>            → prints "position: <bar>.<beat>"
//   dhunzilla --play-seconds <N> <project.opj>               → local smoke ONLY (never CI): opens
//        the real audio device, plays until N s of frames or song end, prints one "played:" line.
//
// Exit codes — on failure exactly one line "dhunzilla: <reason>" goes to stderr, stdout stays
// empty and no output file is written:
//   0  success (snapshot mode prints two lines; render-playback prints its own summary)
//   2  usage error (grammar checked before any filesystem access)
//   3  project file missing/unreadable/invalid — or --apply journal file unreadable
//   4  --apply journal content rejected (bad JSON line, non-object line, Document::apply refused)
//   5  output cannot be written / render failed (PNG for --snapshot, WAV for --render-playback)
//   6  audio failure (--play-seconds only: no device opened, or no frames produced in time)
// Snapshot success prints exactly:
//   title: DhunZilla — <file name>
//   snapshot: <out path as given> 1100x700

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
// applied to `doc` — harmless here because the caller exits 4 without writing a snapshot.
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

// Minimal top-level window: native title bar with a close button, content owned here.
class MainWindow final : public juce::DocumentWindow {
public:
    MainWindow(const juce::String& title, const op::Document& doc, dz::PlaybackController* playback)
        : DocumentWindow(title, juce::Colour(0xff14151a), juce::DocumentWindow::closeButton) {
        // `doc` and `playback` outlive us (App member order: window_ destroyed first).
        setContentOwned(new dz::MainComponent(doc, playback), true);
        centreWithSize(getWidth(), getHeight());
    }

    void closeButtonPressed() override {
        if (auto* app = juce::JUCEApplicationBase::getInstance()) app->systemRequestedQuit();
    }
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
    juce::String snapshot_path, apply_path, render_pb_path, project_path;
    bool have_snapshot = false, have_apply = false;
    bool have_render_pb = false, have_pos_at = false, have_play_sec = false;
    bool have_block = false, have_stop_after = false;
    long long position_at = 0, play_seconds = 0, stop_after = -1;
    int block = 128;

    // Every usage error below is a grammar check: none touches the filesystem.
    for (int i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg == "--snapshot" || arg == "--apply" || arg == "--render-playback") {
            if (i + 1 >= args.size()) {
                printError(juce::String::fromUTF8("usage: ") + arg + juce::String::fromUTF8(" requires a path"));
                setApplicationReturnValue(2); quit(); return;
            }
            if (arg == "--snapshot")        { snapshot_path  = args[++i]; have_snapshot   = true; }
            else if (arg == "--apply")      { apply_path     = args[++i]; have_apply      = true; }
            else                            { render_pb_path = args[++i]; have_render_pb  = true; }
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

    const int modes = (have_snapshot ? 1 : 0) + (have_render_pb ? 1 : 0) +
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
    if (have_apply && !have_snapshot) {  // journal edits only make sense with a snapshot this WP
        printError(juce::String::fromUTF8("usage: --apply requires --snapshot"));
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

    // ---- headless snapshot mode (never opens a device or creates a controller) ----
    if (have_snapshot) {
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
