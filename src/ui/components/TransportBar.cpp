#include "ui/components/TransportBar.h"

#include "core/MusicalTime.h"
#include "ui/theme/DojoLookAndFeel.h"
#include "ui/theme/DojoTheme.h"

#include <cmath>

namespace dojo::ui
{
namespace
{
constexpr int kButtonSize = 36;
constexpr int kButtonGap = 4;
constexpr time::TimeSignature kSignature{4, 4};

void paintReadoutBox(juce::Graphics& g, juce::Rectangle<int> area)
{
    g.setColour(theme::colour::meterBackground);
    g.fillRoundedRectangle(area.toFloat(), theme::metrics::cornerRadius);
    g.setColour(theme::colour::outline);
    g.drawRoundedRectangle(area.toFloat().reduced(0.5f), theme::metrics::cornerRadius, 1.0f);
}
} // namespace

//==================================================================================================
/** A numeric readout that can be dragged or typed into, bound to a session property. Undoable. */
class TransportBar::ValueField final : public juce::Component,
                                       public juce::SettableTooltipClient,
                                       private juce::ValueTree::Listener
{
public:
    struct Spec
    {
        juce::String caption;
        juce::Identifier property;
        std::function<double()> get;
        std::function<void(double)> set;
        std::function<juce::String(double)> format;
        double perPixel = 0.25;
        double finePerPixel = 0.01;
        double minimum = 0.0;
        double maximum = 1.0;
        int decimals = 2;
        juce::String undoName;
        juce::String allowedCharacters = "0123456789.-";
    };

    ValueField(session::Session& session, Spec spec)
        : session_(session)
        , spec_(std::move(spec))
    {
        setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
        editor_.setJustification(juce::Justification::centred);
        editor_.setFont(theme::mono(16.0f));
        editor_.setInputRestrictions(8, spec_.allowedCharacters);
        editor_.onReturnKey = [this] { commitText(); };
        editor_.onEscapeKey = [this] { editor_.setVisible(false); };
        editor_.onFocusLost = [this] { commitText(); };
        addChildComponent(editor_);
        session_.getState().addListener(this);
    }

    ~ValueField() override { session_.getState().removeListener(this); }

    void paint(juce::Graphics& g) override
    {
        paintReadoutBox(g, getLocalBounds());
        auto inner = getLocalBounds().reduced(6, 4);
        g.setColour(theme::colour::gold);
        g.setFont(theme::mono(16.0f));
        g.drawText(spec_.format(spec_.get()), inner.withTrimmedBottom(12), juce::Justification::centred, false);
        g.setColour(theme::colour::textMuted);
        g.setFont(theme::label(8.5f));
        g.drawText(spec_.caption, inner.removeFromBottom(12), juce::Justification::centred, false);
    }

    void resized() override { editor_.setBounds(getLocalBounds().reduced(4, 6).withTrimmedBottom(8)); }

    void mouseDown(const juce::MouseEvent&) override
    {
        dragStart_ = spec_.get();
        session_.beginTransaction(spec_.undoName);
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        const double perPixel = e.mods.isShiftDown() ? spec_.finePerPixel : spec_.perPixel;
        const double scale = std::pow(10.0, spec_.decimals);
        const double value = std::round((dragStart_ - e.getDistanceFromDragStartY() * perPixel) * scale) / scale;
        spec_.set(juce::jlimit(spec_.minimum, spec_.maximum, value));
    }

    void mouseDoubleClick(const juce::MouseEvent&) override
    {
        editor_.setText(juce::String(spec_.get(), spec_.decimals), false);
        editor_.setVisible(true);
        editor_.selectAll();
        editor_.grabKeyboardFocus();
    }

private:
    void commitText()
    {
        if (!editor_.isVisible())
            return;
        editor_.setVisible(false);
        const double value = editor_.getText().getDoubleValue();
        if (value >= spec_.minimum && value <= spec_.maximum)
        {
            session_.beginTransaction(spec_.undoName);
            spec_.set(value);
        }
    }

    void valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& property) override
    {
        if (tree == session_.getState() && property == spec_.property)
            repaint();
    }

    void valueTreeRedirected(juce::ValueTree&) override { repaint(); }

    session::Session& session_;
    Spec spec_;
    juce::TextEditor editor_;
    double dragStart_ = 0.0;
};

//==================================================================================================
TransportBar::TransportBar(engine::EngineController& controller, session::Session& session,
                           EditorState& editor)
    : controller_(controller)
    , session_(session)
    , editor_(editor)
    , returnButton_("Return to start", IconButton::Icon::returnToStart, theme::colour::textPrimary)
    , playButton_("Play", IconButton::Icon::play, theme::colour::play)
    , stopButton_("Stop", IconButton::Icon::stop, theme::colour::textPrimary)
    , recordButton_("Record", IconButton::Icon::record, theme::colour::record)
    , loopButton_("Loop", IconButton::Icon::loop, theme::colour::gold)
    , punchInButton_("Punch In", IconButton::Icon::punchIn, theme::colour::record)
    , punchOutButton_("Punch Out", IconButton::Icon::punchOut, theme::colour::record)
{
    tempoField_ = std::make_unique<ValueField>(
        session, ValueField::Spec{"BPM", session::ids::tempo, [this] { return session_.getTempo(); },
                                  [this](double v) { session_.setTempo(v); }, [](double v) { return juce::String(v, 2); }, 0.25,
                                  0.01, session::kMinTempo, session::kMaxTempo, 2, "Change Tempo", "0123456789."});
    tempoField_->setTooltip("Tempo. Drag up/down to change (Shift: fine), double-click to type.");

    speedField_ = std::make_unique<ValueField>(
        session, ValueField::Spec{"SPEED", session::ids::speed, [this] { return session_.getSpeed(); },
                                  [this](double v) { session_.setSpeed(v); },
                                  [](double v) { return juce::String(v, 2) + "x"; }, 0.01, 0.001, session::kMinStretch,
                                  session::kMaxStretch, 2, "Change Speed", "0123456789."});
    speedField_->setTooltip("Global playback speed (varispeed). With KEEP PITCH the pitch stays. Double-click to type.");

    transposeField_ = std::make_unique<ValueField>(
        session, ValueField::Spec{"PITCH", session::ids::transpose, [this] { return session_.getTranspose(); },
                                  [this](double v) { session_.setTranspose(v); },
                                  [](double v) { return (v > 0.0 ? "+" : "") + juce::String(v, 1); }, 0.1, 0.01, -24.0, 24.0, 1,
                                  "Change Transpose"});
    transposeField_->setTooltip("Global transposition in semitones (audio clips and MIDI). Double-click to type.");

    keepPitchButton_.setButtonText("KEEP PITCH");
    keepPitchButton_.setClickingTogglesState(true);
    keepPitchButton_.setWantsKeyboardFocus(false);
    keepPitchButton_.setTooltip("Keep the pitch when the speed changes (time-stretch instead of varispeed)");
    keepPitchButton_.setToggleState(session_.getKeepPitch(), juce::dontSendNotification);
    keepPitchButton_.onClick = [this]
    {
        session_.beginTransaction("Keep Pitch");
        session_.setKeepPitch(keepPitchButton_.getToggleState());
    };

    returnButton_.setTooltip("Return to start (Home)");
    playButton_.setTooltip("Play / pause (Space)");
    stopButton_.setTooltip("Stop. Press again to return to start.");
    recordButton_.setTooltip("Record onto the armed tracks (R)");
    loopButton_.setTooltip("Loop playback between the loop markers (L). Drag in the ruler's top strip to set them.");
    punchInButton_.setTooltip("Punch in: recording starts at the loop start (the track plays as it was until then)");
    punchOutButton_.setTooltip("Punch out: recording ends at the loop end (the track plays as it was after it)");

    returnButton_.onClick = [this]
    {
        if (onReturnToStart)
            onReturnToStart();
        else
            controller_.returnToStart();
    };
    playButton_.onClick = [this]
    {
        if (onTogglePlayback)
            onTogglePlayback();
        else
            controller_.togglePlayback();
    };
    stopButton_.onClick = [this]
    {
        if (playing_)
            controller_.stop();
        else if (onReturnToStart)
            onReturnToStart();
        else
            controller_.returnToStart();
    };

    recordButton_.onClick = [this]
    {
        if (onRecordClicked)
            onRecordClicked();
    };
    loopButton_.onClick = [this]
    {
        session_.beginTransaction("Toggle Loop");
        session_.setLoopEnabled(!session_.isLoopEnabled());
    };
    punchInButton_.onClick = [this] { session_.setPunchInEnabled(!session_.isPunchInEnabled()); };
    punchOutButton_.onClick = [this] { session_.setPunchOutEnabled(!session_.isPunchOutEnabled()); };

    snapButton_.setButtonText("SNAP");
    snapButton_.setTooltip("Snap clip edits to the grid (hold Alt or Shift while dragging to bypass)");
    snapButton_.setClickingTogglesState(true);
    snapButton_.setWantsKeyboardFocus(false);
    snapButton_.setToggleState(editor_.isSnapEnabled(), juce::dontSendNotification);
    snapButton_.onClick = [this]
    {
        editor_.setSnapEnabled(snapButton_.getToggleState());
    };

    gridBox_.addItem("Bar", 1);
    gridBox_.addItem("Beat", 2);
    gridBox_.addItem("1/8", 3);
    gridBox_.addItem("1/16", 4);
    gridBox_.setTooltip("Grid size");
    gridBox_.setWantsKeyboardFocus(false);
    gridBox_.setSelectedId(static_cast<int>(editor_.getGrid()) + 1, juce::dontSendNotification);
    gridBox_.onChange = [this]
    {
        editor_.setGrid(static_cast<EditorState::Grid>(juce::jlimit(0, 3, gridBox_.getSelectedId() - 1)));
    };

    testToneButton_.setButtonText("TONE");
    testToneButton_.setTooltip(
        "Play a -18 dBFS 440 Hz sine through the master bus to verify the audio device");
    testToneButton_.setClickingTogglesState(true);
    testToneButton_.setWantsKeyboardFocus(false);
    testToneButton_.setToggleState(controller_.isTestToneEnabled(), juce::dontSendNotification);
    testToneButton_.onClick = [this]
    {
        controller_.setTestToneEnabled(testToneButton_.getToggleState());
    };

    audioSettingsButton_.setButtonText("AUDIO");
    audioSettingsButton_.setTooltip(
        "Audio device settings: driver, device, sample rate, buffer size (Ctrl+,)");
    audioSettingsButton_.setWantsKeyboardFocus(false);
    audioSettingsButton_.onClick = [this]
    {
        if (onAudioSettingsRequested)
            onAudioSettingsRequested();
    };

    for (auto* child : std::initializer_list<juce::Component*>{
             &returnButton_, &playButton_, &stopButton_, &recordButton_, &loopButton_, &punchInButton_, &punchOutButton_, tempoField_.get(),
             speedField_.get(), &keepPitchButton_, transposeField_.get(), &snapButton_, &gridBox_, &testToneButton_,
             &audioSettingsButton_})
        addAndMakeVisible(child);

    editor_.addListener(this);
}

TransportBar::~TransportBar()
{
    editor_.removeListener(this);
}

void TransportBar::editorStateChanged()
{
    snapButton_.setToggleState(editor_.isSnapEnabled(), juce::dontSendNotification);
    gridBox_.setSelectedId(static_cast<int>(editor_.getGrid()) + 1, juce::dontSendNotification);
}

void TransportBar::update(const engine::TelemetrySnapshot& telemetry, double sampleRate)
{
    if (telemetry.playing != playing_)
    {
        playing_ = telemetry.playing;
        playButton_.setIcon(playing_ ? IconButton::Icon::pause : IconButton::Icon::play);
        playButton_.setLit(playing_);
    }

    const double tempo = session_.getTempo();
    if (telemetry.playheadSamples != playheadSamples_ || !juce::exactlyEqual(sampleRate, sampleRate_) ||
        !juce::exactlyEqual(tempo, tempoShown_))
    {
        playheadSamples_ = telemetry.playheadSamples;
        sampleRate_ = sampleRate;
        tempoShown_ = tempo;
        repaint(positionArea_);
    }

    if (std::abs(telemetry.dspLoad - dspLoad_) > 0.002f)
    {
        dspLoad_ = telemetry.dspLoad;
        repaint(loadArea_);
    }

    if (testToneButton_.getToggleState() != controller_.isTestToneEnabled())
        testToneButton_.setToggleState(controller_.isTestToneEnabled(), juce::dontSendNotification);

    if (session_.isLoopEnabled() != loopShown_)
    {
        loopShown_ = session_.isLoopEnabled();
        loopButton_.setLit(loopShown_);
    }
    if (session_.isPunchInEnabled() != punchInShown_)
    {
        punchInShown_ = session_.isPunchInEnabled();
        punchInButton_.setLit(punchInShown_);
    }
    if (session_.isPunchOutEnabled() != punchOutShown_)
    {
        punchOutShown_ = session_.isPunchOutEnabled();
        punchOutButton_.setLit(punchOutShown_);
    }

    if (keepPitchButton_.getToggleState() != session_.getKeepPitch())
        keepPitchButton_.setToggleState(session_.getKeepPitch(), juce::dontSendNotification);
}

void TransportBar::setRecording(bool recording)
{
    if (recording == recording_)
        return;
    recording_ = recording;
    recordButton_.setLit(recording_);
}

//==================================================================================================
// Layout

void TransportBar::resized()
{
    auto area = getLocalBounds().reduced(12, 0);

    compactBrand_ = getWidth() < 1760;
    brandArea_ = area.removeFromLeft(compactBrand_ ? 40 : 206);
    area.removeFromLeft(8);

    auto buttons = area.removeFromLeft(5 * kButtonSize + 4 * kButtonGap)
                       .withSizeKeepingCentre(5 * kButtonSize + 4 * kButtonGap, kButtonSize);
    for (auto* button : {&returnButton_, &playButton_, &stopButton_, &recordButton_, &loopButton_})
    {
        button->setBounds(buttons.removeFromLeft(kButtonSize));
        buttons.removeFromLeft(kButtonGap);
    }
    // Punch in / out, smaller, next to the loop button whose range they use.
    area.removeFromLeft(kButtonGap);
    constexpr int kPunchSize = 28;
    for (auto* button : {&punchInButton_, &punchOutButton_})
    {
        button->setBounds(area.removeFromLeft(kPunchSize).withSizeKeepingCentre(kPunchSize, kPunchSize));
        area.removeFromLeft(2);
    }

    area.removeFromLeft(10);
    positionArea_ = area.removeFromLeft(216).withSizeKeepingCentre(216, 44);
    area.removeFromLeft(8);
    tempoField_->setBounds(area.removeFromLeft(92).withSizeKeepingCentre(92, 44));
    area.removeFromLeft(6);
    signatureArea_ = area.removeFromLeft(50).withSizeKeepingCentre(50, 44);
    area.removeFromLeft(6);
    speedField_->setBounds(area.removeFromLeft(74).withSizeKeepingCentre(74, 44));
    area.removeFromLeft(4);
    keepPitchButton_.setBounds(area.removeFromLeft(82).withSizeKeepingCentre(82, 26));
    area.removeFromLeft(4);
    transposeField_->setBounds(area.removeFromLeft(64).withSizeKeepingCentre(64, 44));
    area.removeFromLeft(10);
    snapButton_.setBounds(area.removeFromLeft(62).withSizeKeepingCentre(62, 30));
    area.removeFromLeft(4);
    gridBox_.setBounds(area.removeFromLeft(78).withSizeKeepingCentre(78, 30));

    loadArea_ = area.removeFromRight(130).withSizeKeepingCentre(130, 44);
    area.removeFromRight(12);
    audioSettingsButton_.setBounds(area.removeFromRight(72).withSizeKeepingCentre(72, 30));
    area.removeFromRight(6);
    testToneButton_.setBounds(area.removeFromRight(56).withSizeKeepingCentre(56, 30));
}

//==================================================================================================
// Painting

void TransportBar::paint(juce::Graphics& g)
{
    g.fillAll(theme::colour::panel);
    g.setColour(theme::colour::outline);
    g.fillRect(getLocalBounds().removeFromBottom(1));

    paintBrand(g);
    paintPositionDisplay(g);
    paintSignature(g);
    paintLoadMeter(g);
}

void TransportBar::paintBrand(juce::Graphics& g) const
{
    auto area = brandArea_;
    DojoLookAndFeel::drawBrandMark(g, area.removeFromLeft(34).withSizeKeepingCentre(30, 30).toFloat(),
                                   theme::colour::accent);
    if (compactBrand_)
        return;

    area.removeFromLeft(10);

    auto title = area.removeFromTop(area.getHeight() / 2 + 4);
    g.setColour(theme::colour::textPrimary);
    g.setFont(theme::sansBold(17.0f).withExtraKerningFactor(0.12f));
    g.drawText("THE DOJO", title, juce::Justification::bottomLeft, false);

    g.setColour(theme::colour::textMuted);
    g.setFont(theme::label(9.0f));
    g.drawText("DIGITAL AUDIO WORKSTATION", area.withTrimmedTop(2), juce::Justification::topLeft, false);
}

void TransportBar::paintPositionDisplay(juce::Graphics& g) const
{
    paintReadoutBox(g, positionArea_);

    const double rate = sampleRate_ > 0.0 ? sampleRate_ : 48000.0;
    const double seconds = time::samplesToSeconds(std::max<SampleCount>(0, playheadSamples_), rate);
    const auto bbt = time::quarterNotesToBarsBeatsTicks(
        time::secondsToQuarterNotes(seconds, session_.getTempo()), kSignature);
    const auto clock = time::secondsToClockTime(seconds);

    auto area = positionArea_.reduced(12, 4);
    auto bbtArea = area.removeFromLeft(118);

    g.setColour(playing_ ? theme::colour::textPrimary : theme::colour::textSecondary);
    g.setFont(theme::mono(20.0f));
    const auto bbtText = juce::String(bbt.bar).paddedLeft('0', 3) + "." + juce::String(bbt.beat) + "." +
                         juce::String(bbt.tick).paddedLeft('0', 3);
    g.drawText(bbtText, bbtArea.withTrimmedBottom(12), juce::Justification::centredLeft, false);

    g.setColour(theme::colour::textMuted);
    g.setFont(theme::label(8.5f));
    g.drawText("BAR  BEAT  TICK", bbtArea.removeFromBottom(12), juce::Justification::centredLeft, false);

    g.setColour(theme::colour::textSecondary);
    g.setFont(theme::mono(13.0f));
    const auto clockText = juce::String(clock.hours).paddedLeft('0', 2) + ":" +
                           juce::String(clock.minutes).paddedLeft('0', 2) + ":" +
                           juce::String(clock.seconds).paddedLeft('0', 2) + "." +
                           juce::String(clock.milliseconds).paddedLeft('0', 3);
    g.drawText(clockText, area.withTrimmedBottom(12), juce::Justification::centredRight, false);

    g.setColour(theme::colour::textMuted);
    g.setFont(theme::label(8.5f));
    g.drawText("TIME", area.removeFromBottom(12), juce::Justification::centredRight, false);
}

void TransportBar::paintSignature(juce::Graphics& g) const
{
    paintReadoutBox(g, signatureArea_);
    auto inner = signatureArea_.reduced(6, 4);

    g.setColour(theme::colour::gold);
    g.setFont(theme::mono(17.0f));
    g.drawText(juce::String(kSignature.numerator) + "/" + juce::String(kSignature.denominator),
               inner.withTrimmedBottom(12), juce::Justification::centred, false);

    g.setColour(theme::colour::textMuted);
    g.setFont(theme::label(8.5f));
    g.drawText("METER", inner.removeFromBottom(12), juce::Justification::centred, false);
}

void TransportBar::paintLoadMeter(juce::Graphics& g) const
{
    auto area = loadArea_.reduced(0, 6);

    g.setColour(theme::colour::textMuted);
    g.setFont(theme::label(9.0f));
    g.drawText("DSP LOAD", area.removeFromTop(12), juce::Justification::centredLeft, false);

    const float load = juce::jlimit(0.0f, 1.0f, dspLoad_);
    auto bar = area.removeFromBottom(8).toFloat();
    auto text = area;

    g.setColour(theme::colour::meterBackground);
    g.fillRoundedRectangle(bar, 2.0f);

    const auto fill = load < 0.6f ? theme::colour::meterLow
                                  : (load < 0.85f ? theme::colour::meterMid : theme::colour::meterClip);
    g.setColour(fill);
    g.fillRoundedRectangle(bar.withWidth(juce::jmax(2.0f, bar.getWidth() * load)), 2.0f);

    g.setColour(theme::colour::textSecondary);
    g.setFont(theme::mono(11.0f));
    g.drawText(juce::String(dspLoad_ * 100.0f, 1) + "%", text, juce::Justification::centredRight, false);
}

} // namespace dojo::ui
