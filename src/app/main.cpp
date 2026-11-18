// src/app/main.cpp — dhunzilla entry point (WP-0143).
//
// Two modes, parsed from getCommandLineParameterArray() inside initialise(); options may appear
// in any order relative to the project path:
//   dhunzilla [project.opj]                                  → window (native title bar)
//   dhunzilla --snapshot <out.png> [--apply <journal>] <project.opj>
//        headless: build the MainComponent off-screen and write a createComponentSnapshot() PNG.
//        No top-level window is created in this mode; no audio device is ever opened (we link
//        juce_gui_basics only — there is no audio module to open one with).
//
// Exit codes — on failure exactly one line "dhunzilla: <reason>" goes to stderr, stdout stays
// empty and no PNG is written:
//   0  success (snapshot mode prints two lines; see below)
//   2  usage error (grammar checked before any filesystem access)
//   3  project file missing/unreadable/invalid — or --apply journal file unreadable
//   4  --apply journal content rejected (bad JSON line, non-object line, Document::apply refused)
//   5  snapshot PNG cannot be written (e.g. output directory does not exist)
// Snapshot success prints exactly:
//   title: DhunZilla — <file name>
//   snapshot: <out path as given> 1100x700

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>
#include <nlohmann/json.hpp>

#include "doc/document.hpp"
#include "main_component.hpp"

namespace {

void printError(const juce::String& reason) {
    std::fprintf(stderr, "dhunzilla: %s\n", reason.toRawUTF8());
}

// Read `path` as a JSON-lines operations journal (blank lines and '#' comments skipped), parse
// EVERY line first, then apply the ops to `doc` in order through Document::apply — so a rejected
// line never leaves earlier lines applied. Returns 0, or the exit code (3 file unreadable,
// 4 content rejected) with `reason` set for printError().
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
    MainWindow(const juce::String& title, const op::Document& doc)
        : DocumentWindow(title, juce::Colour(0xff14151a), juce::DocumentWindow::closeButton) {
        setContentOwned(new dz::MainComponent(doc), true);  // `doc` outlives us (App member order)
        centreWithSize(getWidth(), getHeight());
    }

    void closeButtonPressed() override {
        if (auto* app = juce::JUCEApplicationBase::getInstance()) app->systemRequestedQuit();
    }
};

class App final : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return juce::String::fromUTF8("dhunzilla"); }
    const juce::String getApplicationVersion() override { return juce::String::fromUTF8("0.1.0"); }

    void initialise(const juce::String&) override;
    void shutdown() override {
        window_.reset();  // before doc_: MainComponent holds a reference to it
        doc_.reset();
    }
    void systemRequestedQuit() override { quit(); }

private:
    std::unique_ptr<op::Document> doc_;      // declared first → destroyed last
    std::unique_ptr<MainWindow> window_;
};

void App::initialise(const juce::String&) {
    // ---- parse: options and the (single) positional project path, any order ----
    const auto args = getCommandLineParameterArray();
    juce::String snapshot_path, apply_path, project_path;
    bool have_snapshot = false, have_apply = false;

    for (int i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg == "--snapshot" || arg == "--apply") {
            const bool is_snapshot = arg == "--snapshot";
            if (i + 1 >= args.size()) {
                printError(juce::String::fromUTF8("usage: ") + arg + juce::String::fromUTF8(" requires a path"));
                setApplicationReturnValue(2); quit(); return;
            }
            if (is_snapshot) { snapshot_path = args[++i]; have_snapshot = true; }
            else             { apply_path    = args[++i]; have_apply    = true; }
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
    if (have_snapshot && project_path.isEmpty()) {
        printError(juce::String::fromUTF8("usage: --snapshot requires a project file"));
        setApplicationReturnValue(2); quit(); return;
    }
    if (have_apply && !have_snapshot) {  // journal edits only make sense with a snapshot this WP
        printError(juce::String::fromUTF8("usage: --apply requires --snapshot"));
        setApplicationReturnValue(2); quit(); return;
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

    // ---- headless snapshot mode ----
    if (have_snapshot) {
        if (have_apply) {
            juce::String reason;
            const int code = applyJournal(apply_path, *doc_, reason);
            if (code != 0) {
                printError(reason);
                setApplicationReturnValue(code); quit(); return;
            }
        }

        dz::MainComponent component(*doc_);  // off-screen: never added to the desktop
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

    // ---- window mode ----
    const juce::String title = project_path.isEmpty()
        ? juce::String::fromUTF8("DhunZilla")
        : dz::windowTitleFor(project_path.toRawUTF8());
    window_ = std::make_unique<MainWindow>(title, *doc_);
    window_->setVisible(true);
}

}  // namespace

START_JUCE_APPLICATION(App)
