// src/app/main_component.cpp — see main_component.hpp for the contract (WP-0143, WP-0152, WP-0153).

#include "main_component.hpp"

#include <algorithm>

#include <nlohmann/json.hpp>
#include "playback_controller.hpp"
#include "render_playback.hpp"

namespace {

using juce::Colour;
using juce::String;

// juce::Colour's uint32 ctor is not constexpr in JUCE 9 — plain const file-scope values.
const Colour kBackground{0xff14151a};   // near-black, slightly blue
const Colour kTransportBg{0xff1c1e26};  // transport bar strip
const Colour kSeparator{0xff2c2f3a};    // hairline under the transport bar
const Colour kText{0xffd8dae2};         // primary text on dark
const Colour kCaption{0xff8b8fa0};      // secondary labels
const Colour kButtonOff{0xff3a3e4a};    // unlit button face
const Colour kMuteOn{0xffb03a2e};       // M lit: reddish
const Colour kSoloOn{0xffd9a13b};       // S lit: amber
const Colour kRowShade{0x590d0e12};     // wash over an inaudible row (~35% near-black — dims the
                                        // row while keeping lit M/S colours recognisable through it)

// ASCII-only lowercase for key-code comparison (getKeyCode() reports letters as uppercase ASCII).
int lowerAscii(int c) { return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c; }

}  // namespace

juce::String dz::windowTitleFor(const std::string& project_path) {
    const auto path = String::fromUTF8(project_path.c_str());
    auto file_name = juce::File(path).getFileName();
    if (file_name.isEmpty()) file_name = path;  // trailing '/' or bare root: show what we were given
    return String(juce::CharPointer_UTF8("DhunZilla \xe2\x80\x94 ")) + file_name;
}

struct dz::MainComponent::RowWidgets {
    juce::Label name;
    juce::Slider gain;
    juce::Slider pan;
    juce::TextButton mute;
    juce::TextButton solo;
};

dz::MainComponent::~MainComponent() = default;  // RowWidgets complete here (see header note)

dz::MainComponent::MainComponent(op::Document& doc, PlaybackController* playback)
    : doc_(doc), playback_(playback) {
    // --- transport bar: inert without a controller (snapshot mode == WP-0143), live with one ---
    play_.setButtonText(String::fromUTF8("Play"));
    stop_.setButtonText(String::fromUTF8("Stop"));
    for (auto* button : {&play_, &stop_}) {
        button->setEnabled(playback_ != nullptr);  // snapshot mode: nothing to act on (spec)
        button->setColour(juce::TextButton::buttonColourId, kButtonOff);
        button->setColour(juce::TextButton::textColourOffId, kText);
    }
    if (playback_ != nullptr) {  // live transport: Play starts + renders, Stop silences + parks
        play_.onClick = [this] {
            playback_->play(doc_);
            requested_revision_ = doc_.revision();  // the click's render covers today's model
        };
        stop_.onClick = [this] { playback_->stop(); };
        requested_revision_ = doc.revision();
        startTimerHz(30);  // reclaim + position/status refresh (spec: 30 Hz)
    }

    bpm_.setColour(juce::Label::textColourId, kText);
    bpm_.setJustificationType(juce::Justification::centredLeft);
    position_.setColour(juce::Label::textColourId, kText);
    position_.setJustificationType(juce::Justification::centredRight);

    status_.setColour(juce::Label::textColourId, kCaption);  // empty in snapshot mode: paints nothing
    status_.setJustificationType(juce::Justification::centredLeft);

    addAndMakeVisible(play_);
    addAndMakeVisible(stop_);
    addAndMakeVisible(bpm_);
    addAndMakeVisible(position_);
    addAndMakeVisible(status_);

    // --- one editable row per track, in Document order (WP-0152) ---
    const auto tracks = doc.track_rows();
    rows_.reserve(tracks.size());
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        const int index = static_cast<int>(i);  // row index captured by value in every callback
        auto row = std::make_unique<RowWidgets>();

        row->name.setColour(juce::Label::textColourId, kText);
        row->name.setJustificationType(juce::Justification::centredLeft);

        row->gain.setSliderStyle(juce::Slider::LinearHorizontal);
        row->gain.setTextBoxStyle(juce::Slider::TextBoxRight, false, 64, 24);
        row->gain.setRange(-96.0, 24.0);
        row->gain.setNumDecimalPlacesToDisplay(1);

        row->pan.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        row->pan.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        row->pan.setRange(-1.0, 1.0);

        // A slider drag is ONE gesture: JUCE fires onDragStart (ScopedDragNotification in
        // Slider::mouseDown) BEFORE the first value change and onDragEnd at mouse-up, so
        // begin_gesture runs first and onValueChange always sees gesture_active() during a real
        // drag. Keyboard/text-box/wheel edits arrive without a gesture → plain apply().
        row->gain.onDragStart = [this] { doc_.begin_gesture(); };
        row->gain.onValueChange = [this, index] {
            const auto value = rows_[static_cast<std::size_t>(index)]->gain.getValue();
            const nlohmann::json op{{"op", "set_track_gain"}, {"index", index}, {"gain_db", value}};
            std::string err;
            if (doc_.gesture_active()) doc_.update_gesture(op, err);
            else                       doc_.apply(op, err);
            refresh();
        };
        row->gain.onDragEnd = [this] {
            std::string err;
            gesture_ok_ = doc_.end_gesture(err);
            gesture_err_ = gesture_ok_ ? std::string() : err;
            refresh();
        };

        row->pan.onDragStart = [this] { doc_.begin_gesture(); };
        row->pan.onValueChange = [this, index] {
            const auto value = rows_[static_cast<std::size_t>(index)]->pan.getValue();
            const nlohmann::json op{{"op", "set_track_pan"}, {"index", index}, {"pan", value}};
            std::string err;
            if (doc_.gesture_active()) doc_.update_gesture(op, err);
            else                       doc_.apply(op, err);
            refresh();
        };
        row->pan.onDragEnd = [this] {
            std::string err;
            gesture_ok_ = doc_.end_gesture(err);
            gesture_err_ = gesture_ok_ ? std::string() : err;
            refresh();
        };

        row->mute.setButtonText(String::fromUTF8("M"));
        row->solo.setButtonText(String::fromUTF8("S"));
        row->mute.setColour(juce::TextButton::buttonOnColourId, kMuteOn);
        row->solo.setColour(juce::TextButton::buttonOnColourId, kSoloOn);
        for (auto* toggle : {&row->mute, &row->solo}) {
            // A real toggled-button look (lit colour when true). The lit state comes ONLY from
            // refresh() reading the model — no setClickingTogglesState: onClick applies the op and
            // refresh() sets the visual state. Do NOT use setEnabled(false) here — a disabled
            // button always paints the OFF colour, which would hide the lit mute/solo state.
            toggle->setColour(juce::TextButton::buttonColourId, kButtonOff);
            toggle->setColour(juce::TextButton::textColourOffId, kText);
            toggle->setColour(juce::TextButton::textColourOnId, kText);
        }

        row->mute.onClick = [this, index] {
            std::string err;
            const auto rows = doc_.track_rows();
            if (index >= 0 && static_cast<std::size_t>(index) < rows.size())
                doc_.apply(nlohmann::json{{"op", "set_track_mute"}, {"index", index},
                                          {"mute", !rows[static_cast<std::size_t>(index)].mute}}, err);
            refresh();  // runs even when apply() failed — the model stays the truth
        };
        row->solo.onClick = [this, index] {
            std::string err;
            const auto rows = doc_.track_rows();
            if (index >= 0 && static_cast<std::size_t>(index) < rows.size())
                doc_.apply(nlohmann::json{{"op", "set_track_solo"}, {"index", index},
                                          {"solo", !rows[static_cast<std::size_t>(index)].solo}}, err);
            refresh();
        };

        addAndMakeVisible(row->name);
        addAndMakeVisible(row->gain);
        addAndMakeVisible(row->pan);
        addAndMakeVisible(row->mute);
        addAndMakeVisible(row->solo);

        rows_.push_back(std::move(row));
    }

    setWantsKeyboardFocus(true);  // key presses reach us; child sliders bubble unhandled keys up

    refresh();

    // LAST statement: setSize() triggers resized(), which lays out every widget added above.
    setSize(kWidth, kHeight);
}

void dz::MainComponent::refresh() {
    bpm_.setText(String::fromUTF8("BPM ") + String(doc_.project().bpm, 1), juce::dontSendNotification);
    // bar.beat from the Player's counter — which reads 0 at home, so stopped (or snapshot mode)
    // formats to exactly "1.1" (spec: refresh() formats the position this way too).
    const long long frame = playback_ != nullptr ? playback_->player().position_frames() : 0;
    position_.setText(String::fromUTF8(format_position(doc_.project(), frame).c_str()),
                      juce::dontSendNotification);

    const auto tracks = doc_.track_rows();
    for (std::size_t i = 0; i < rows_.size() && i < tracks.size(); ++i) {
        const auto& track = tracks[i];
        auto& row = *rows_[i];
        row.name.setText(String::fromUTF8(track.name.c_str()), juce::dontSendNotification);
        row.gain.setValue(track.gain_db, juce::dontSendNotification);
        row.pan.setValue(track.pan, juce::dontSendNotification);
        row.mute.setToggleState(track.mute, juce::dontSendNotification);
        row.solo.setToggleState(track.solo, juce::dontSendNotification);
        // Dim the name itself (spec example: alpha 0.45); the row wash in paintOverChildren covers
        // the rest of the widgets.
        row.name.setColour(juce::Label::textColourId, track.audible ? kText : kText.withAlpha(0.45f));
    }

    repaint();  // row dimming depends on track_rows() too, not just widget values

    if (onDocumentChanged) onDocumentChanged();
}

bool dz::MainComponent::doUndo() {
    const bool ok = doc_.undo();
    refresh();
    return ok;
}

bool dz::MainComponent::doRedo() {
    const bool ok = doc_.redo();
    refresh();
    return ok;
}

bool dz::MainComponent::doSave(std::string& err) {
    const bool ok = doc_.save(err);
    refresh();
    return ok;
}

bool dz::MainComponent::handleKey(const juce::KeyPress& key) {
    // "Ctrl" is commandModifier (== Ctrl on Linux). Compare the key code case-insensitively (a
    // Shift+Z press may report 'Z') and the modifier flags masked to command|shift|alt EXACTLY.
    const int mods = key.getModifiers().getRawFlags() &
                     (juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier |
                      juce::ModifierKeys::altModifier);
    const int code = lowerAscii(key.getKeyCode());

    if (code == 'z' && mods == juce::ModifierKeys::commandModifier) {
        doUndo();  // consumed even when there is nothing to undo
        return true;
    }
    if (code == 'z' &&
        mods == (juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier)) {
        doRedo();
        return true;
    }
    if (code == 's' && mods == juce::ModifierKeys::commandModifier) {
        std::string err;
        doSave(err);  // consumed even when the save failed (the window prints its own warning)
        return true;
    }
    return false;
}

bool dz::MainComponent::keyPressed(const juce::KeyPress& key) { return handleKey(key); }

bool dz::MainComponent::scriptDrag(const std::string& what, int track, double value,
                                   std::string& err) {
    const int n = static_cast<int>(rows_.size());
    if (track < 0 || track >= n) {
        err = "track index " + std::to_string(track) + " out of range (0.." +
              std::to_string(n - 1) + ")";
        return false;
    }
    juce::Slider* slider = nullptr;
    if (what == "gain")      slider = &rows_[static_cast<std::size_t>(track)]->gain;
    else if (what == "pan")  slider = &rows_[static_cast<std::size_t>(track)]->pan;
    else { err = "unknown control " + what; return false; }

    const auto range = slider->getRange();
    value = juce::jlimit(range.getStart(), range.getEnd(), value);
    const double cur = slider->getValue();

    gesture_ok_ = true;
    gesture_err_.clear();
    if (slider->onDragStart) slider->onDragStart();
    for (int k = 1; k <= 4; ++k) {
        // Four steps like a real drag; the last lands exactly on `value`. setValue with an
        // unchanged value fires nothing, as in real use — so a no-move drag yields no undo entry.
        const double v = (k == 4) ? value : cur + (value - cur) * static_cast<double>(k) / 4.0;
        slider->setValue(v, juce::sendNotificationSync);
    }
    if (slider->onDragEnd) slider->onDragEnd();

    err = gesture_err_;
    return gesture_ok_;
}

bool dz::MainComponent::scriptClick(const std::string& what, int track, std::string& err) {
    const int n = static_cast<int>(rows_.size());
    if (track < 0 || track >= n) {
        err = "track index " + std::to_string(track) + " out of range (0.." +
              std::to_string(n - 1) + ")";
        return false;
    }
    juce::TextButton* button = nullptr;
    if (what == "mute")      button = &rows_[static_cast<std::size_t>(track)]->mute;
    else if (what == "solo") button = &rows_[static_cast<std::size_t>(track)]->solo;
    else { err = "unknown control " + what; return false; }

    // Invoke the std::function directly: triggerClick() posts an asynchronous message that never
    // runs in a headless script.
    button->onClick();
    return true;
}

double dz::MainComponent::widgetValue(const std::string& what, int track) const {
    if (track < 0 || track >= static_cast<int>(rows_.size())) return 0.0;
    const auto& row = *rows_[static_cast<std::size_t>(track)];
    if (what == "gain") return row.gain.getValue();
    if (what == "pan")  return row.pan.getValue();
    if (what == "mute") return row.mute.getToggleState() ? 1.0 : 0.0;
    if (what == "solo") return row.solo.getToggleState() ? 1.0 : 0.0;
    return 0.0;
}

int dz::MainComponent::trackCount() const { return static_cast<int>(rows_.size()); }

void dz::MainComponent::timerCallback() {
    if (playback_ == nullptr) return;  // defensive: the timer only starts with a controller
    auto& player = playback_->player();

    // Collect buffers the audio callback retired (control-thread ownership rule, player.hpp).
    player.reclaim();

    // "After any model change while playing": re-render so the audible buffer follows the edit.
    if (player.playing() && doc_.revision() != requested_revision_) {
        requested_revision_ = doc_.revision();
        playback_->requestRender(doc_);
    }

    position_.setText(String::fromUTF8(format_position(doc_.project(), player.position_frames()).c_str()),
                      juce::dontSendNotification);

    const std::string& error = playback_->lastError();  // a failed render outranks the device line
    status_.setText(String::fromUTF8((error.empty() ? playback_->deviceDescription() : error).c_str()),
                    juce::dontSendNotification);
}

void dz::MainComponent::paint(juce::Graphics& g) {
    g.fillAll(kBackground);
    g.setColour(kTransportBg);
    g.fillRect(0, 0, getWidth(), kTransportHeight);
    g.setColour(kSeparator);
    g.fillRect(0, kTransportHeight - 1, getWidth(), 1);

    // Small caption so the position value reads as bar.beat in a bare snapshot.
    const auto pos_bounds = position_.getBounds();
    if (!pos_bounds.isEmpty()) {
        g.setFont(juce::Font(juce::FontOptions(13.0f)));
        g.setColour(kCaption);
        g.drawFittedText(String::fromUTF8("Position"), pos_bounds.getX() - 96, pos_bounds.getY(),
                         88, pos_bounds.getHeight(), juce::Justification::centredRight, 1);
    }
}

void dz::MainComponent::paintOverChildren(juce::Graphics& g) {
    // Children paint before this callback (Component::paintComponentAndChildren), so the wash sits
    // above every widget in the row — a dimmed row is unmistakably dimmed, lit M/S and all.
    const auto tracks = doc_.track_rows();
    const std::size_t count = std::min(rows_.size(), tracks.size());
    g.setColour(kRowShade);
    for (std::size_t i = 0; i < count; ++i) {
        if (!tracks[i].audible && i < row_rects_.size() && !row_rects_[i].isEmpty())
            g.fillRoundedRectangle(row_rects_[i].toFloat(), 6.0f);
    }
}

void dz::MainComponent::resized() {
    const auto transport = getLocalBounds().withHeight(kTransportHeight);
    const int button_y = (kTransportHeight - 32) / 2;

    int x = transport.getX() + 16;
    play_.setBounds(x, transport.getY() + button_y, 84, 32);
    x += 92;
    stop_.setBounds(x, transport.getY() + button_y, 84, 32);
    x += 108;
    bpm_.setBounds(x, transport.getY(), 200, kTransportHeight);

    position_.setBounds(transport.getRight() - 16 - 72, transport.getY(), 72, kTransportHeight);

    status_.setBounds(16, getHeight() - 28, getWidth() - 32, 24);  // bottom status line (spec)

    row_rects_.assign(rows_.size(), {});
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        const int y = kTransportHeight + 12 + static_cast<int>(i) * kRowHeight;
        const juce::Rectangle<int> row(16, y, getWidth() - 32, kRowHeight);
        row_rects_[i] = row;

        auto& widgets = *rows_[i];
        widgets.name.setBounds(row.getX(), row.getY(), 240, kRowHeight);
        widgets.gain.setBounds(row.getX() + 256, row.getY() + 8, 320, 32);
        widgets.pan.setBounds(row.getX() + 600, row.getY() + 4, 40, 40);
        widgets.mute.setBounds(row.getX() + 672, row.getY() + 10, 52, 28);
        widgets.solo.setBounds(row.getX() + 732, row.getY() + 10, 52, 28);
    }
}
