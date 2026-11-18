// DhunZilla Phase-0 GUI/audio spike (WP-0132). Throwaway probe, not product code; lives in spikes/,
// never linked into src/. Env: DZ_DEVICE, DZ_SR, DZ_BUFFER, DZ_NO_WINDOW=1, DZ_SPIKE_EXIT=1, DZ_SNAPSHOT.
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>

namespace
{
struct ProbeResult
{
    juce::String summary;
    juce::String details;
};


// Counts callbacks and records inter-callback intervals into a preallocated buffer.
// RT-safe: no allocation, no locks, no I/O in audioDeviceIOCallbackWithContext.
struct CallbackTimer final : juce::AudioIODeviceCallback
{
    static constexpr int kMax = 4096;
    double stamps[kMax] {};
    std::atomic<int> n { 0 };
    std::atomic<int> lastBlock { 0 };

    void audioDeviceIOCallbackWithContext (const float* const*, int, float* const* out, int numOut,
                                           int numSamples, const juce::AudioIODeviceCallbackContext&) override
    {
        for (int ch = 0; ch < numOut; ++ch)
            if (out[ch] != nullptr)
                juce::FloatVectorOperations::clear (out[ch], numSamples);
        const int i = n.load (std::memory_order_relaxed);
        if (i < kMax)
        {
            stamps[i] = juce::Time::getMillisecondCounterHiRes();
            n.store (i + 1, std::memory_order_release);
        }
        lastBlock.store (numSamples, std::memory_order_relaxed);
    }
    void audioDeviceAboutToStart (juce::AudioIODevice*) override {}
    void audioDeviceStopped() override {}
};

juce::String envOr (const char* k, const char* d) { return juce::SystemStats::getEnvironmentVariable (k, d); }

ProbeResult probeAudio()
{
    ProbeResult r;
    juce::AudioDeviceManager dm;
    // DZ_DEVICE (output name), DZ_SR (Hz), DZ_BUFFER (samples) select a configuration.
    juce::AudioDeviceManager::AudioDeviceSetup want;
    want.outputDeviceName = envOr ("DZ_DEVICE", "");
    want.sampleRate = envOr ("DZ_SR", "0").getDoubleValue();
    want.bufferSize = envOr ("DZ_BUFFER", "0").getIntValue();
    const juce::String err = dm.initialise (0, 2, nullptr, true, {}, want.outputDeviceName.isNotEmpty()
                                                                    || want.sampleRate > 0 || want.bufferSize > 0
                                                                        ? &want : nullptr);

    juce::StringArray lines;
    lines.add ("JUCE " + juce::String (JUCE_MAJOR_VERSION) + "."
               + juce::String (JUCE_MINOR_VERSION) + "."
               + juce::String (JUCE_BUILDNUMBER));
    lines.add ("device types: " + juce::String (dm.getAvailableDeviceTypes().size()));

    int totalDevices = 0;
    for (auto* type : dm.getAvailableDeviceTypes())
    {
        type->scanForDevices();
        const auto outs = type->getDeviceNames (false);
        totalDevices += outs.size();
        lines.add ("  " + type->getTypeName() + ": " + juce::String (outs.size()) + " output(s)");
        for (const auto& n : outs)
            lines.add ("    - " + n);
    }

    if (auto* dev = dm.getCurrentAudioDevice())
    {
        const double sr = dev->getCurrentSampleRate();
        const int bs = dev->getCurrentBufferSizeSamples();
        const double outMs = sr > 0.0 ? 1000.0 * dev->getOutputLatencyInSamples() / sr : 0.0;
        const double inMs  = sr > 0.0 ? 1000.0 * dev->getInputLatencyInSamples()  / sr : 0.0;
        const double bufMs = sr > 0.0 ? 1000.0 * bs / sr : 0.0;
        lines.add ("current: " + dev->getName() + " @" + juce::String (sr, 0) + " Hz, "
                   + juce::String (bs) + " samples (" + juce::String (bufMs, 2) + " ms)");
        lines.add ("reported latency: out " + juce::String (outMs, 2) + " ms, in "
                   + juce::String (inMs, 2) + " ms");
        // Measure: run silent callbacks for 3 s and look at the interval distribution.
        CallbackTimer timer;
        dm.addAudioCallback (&timer);
        juce::Thread::sleep (3000);
        dm.removeAudioCallback (&timer);
        const int cnt = timer.n.load (std::memory_order_acquire);
        double mean = 0.0, worst = 0.0;
        for (int i = 1; i < cnt; ++i)
        {
            const double d = timer.stamps[i] - timer.stamps[i - 1];
            mean += d;
            worst = juce::jmax (worst, d);
        }
        if (cnt > 1)
            mean /= (cnt - 1);
        lines.add ("measured: " + juce::String (cnt) + " callbacks in 3 s, block "
                   + juce::String (timer.lastBlock.load()) + " samples, interval mean "
                   + juce::String (mean, 2) + " ms, worst " + juce::String (worst, 2) + " ms");
        r.summary = "requested sr=" + juce::String (want.sampleRate, 0) + " buf=" + juce::String (want.bufferSize)
                    + " -> got " + juce::String (sr, 0) + " Hz / " + juce::String (bs) + " samples; reported out "
                    + juce::String (outMs, 2) + " ms; measured block " + juce::String (timer.lastBlock.load())
                    + ", mean " + juce::String (mean, 2) + " ms, worst " + juce::String (worst, 2) + " ms";
    }
    else
    {
        lines.add ("current: NONE");
        r.summary = "no audio device (" + juce::String (totalDevices) + " enumerated)";
    }
    if (err.isNotEmpty())
        lines.add ("init note: " + err);

    r.details = lines.joinIntoString ("\n");
    return r;
}

class ProbeComponent final : public juce::Component
{
public:
    explicit ProbeComponent (juce::String info) : details (std::move (info))
    {
        setSize (760, 440);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff14151a));

        auto area = getLocalBounds().reduced (28);
        g.setColour (juce::Colour (0xffff6b35));
        g.setFont (juce::FontOptions (34.0f, juce::Font::bold));
        g.drawText ("DhunZilla", area.removeFromTop (46), juce::Justification::centredLeft);

        g.setColour (juce::Colour (0xffd8d9e0));
        g.setFont (juce::FontOptions (15.0f));
        g.drawText (juce::String (juce::CharPointer_UTF8 ("Phase-0 spike \xe2\x80\x94 JUCE GUI + audio device probe")),
                    area.removeFromTop (24), juce::Justification::centredLeft);

        area.removeFromTop (18);
        g.setColour (juce::Colour (0xff2a2c36));
        g.fillRoundedRectangle (area.toFloat(), 8.0f);

        auto text = area.reduced (16);
        g.setColour (juce::Colour (0xff9fe8b0));
        g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));
        g.drawMultiLineText (details, text.getX(), text.getY() + 4, text.getWidth());
    }

    juce::String details;
};

class MainWindow final : public juce::DocumentWindow
{
public:
    explicit MainWindow (juce::String info)
        : juce::DocumentWindow ("DhunZilla Spike", juce::Colour (0xff14151a),
                                juce::DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new ProbeComponent (std::move (info)), true);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
};

class SpikeApplication final : public juce::JUCEApplication, private juce::Timer
{
public:
    const juce::String getApplicationName() override    { return "DhunZilla Spike"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }

    void initialise (const juce::String&) override
    {
        const auto probe = probeAudio();
        std::cout << "[probe]\n" << probe.details << "\n[summary] " << probe.summary << std::endl;

        if (envOr ("DZ_NO_WINDOW", "0") == "1")
        {
            juce::MessageManager::callAsync ([this] { quit(); });
            return;
        }
        window = std::make_unique<MainWindow> (probe.details);
        startTimer (1200);
    }

    void shutdown() override { window.reset(); }

    void systemRequestedQuit() override { quit(); }

private:
    void timerCallback() override
    {
        stopTimer();
        if (auto* c = window != nullptr ? window->getContentComponent() : nullptr)
        {
            const auto img = c->createComponentSnapshot (c->getLocalBounds());
            const auto out = juce::File (envOr ("DZ_SNAPSHOT", "/tmp/dz_spike.png"));
            out.deleteFile();
            if (auto stream = out.createOutputStream())
            {
                juce::PNGImageFormat png;
                png.writeImageToStream (img, *stream);
                std::cout << "[snapshot] " << out.getFullPathName() << " "
                          << out.getSize() << " bytes" << std::endl;
            }
        }
        if (juce::SystemStats::getEnvironmentVariable ("DZ_SPIKE_EXIT", "0") == "1")
            juce::Timer::callAfterDelay (400, [this] { quit(); });
    }

    std::unique_ptr<MainWindow> window;
};
} // namespace

START_JUCE_APPLICATION (SpikeApplication)
