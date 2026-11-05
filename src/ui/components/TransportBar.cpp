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
/** Tempo readout that can be dragged or typed into. Edits are undoable. */
class TransportBar::TempoField final : public juce::Component,
                                       public juce::SettableTooltipClient,
                                       private juce::ValueTree::Listener
{
public:
    explicit TempoField(session::Session& session)
        : session_(session)
    {
        setTooltip("Tempo. Drag up/down to change (Shift: fine), double-click to type.");
        setMouseCursor(juce::MouseCursor::UpDownResizeCursor);

        editor_.setJustification(juce::Justification::centred);
        editor_.setFont(theme::mono(16.0f));
        editor_.setInputRestrictions(7, "0123456789.");
        editor_.onReturnKey = [this]
        {
            commitText();
        };
        editor_.onEscapeKey = [this]
        {
            editor_.setVisible(false);
        };
        editor_.onFocusLost = [this]
        {
            commitText();
        };
        addChildComponent(editor_);

        session_.getState().addListener(this);
    }

    ~TempoField() override { session_.getState().removeListener(this); }

    void paint(juce::Graphics& g) override
    {
        paintReadoutBox(g, getLocalBounds());
        auto inner = getLocalBounds().reduced(6, 4);

        g.setColour(theme::colour::gold);
        g.setFont(theme::mono(17.0f));
        g.drawText(juce::String(session_.getTempo(), 2), inner.withTrimmedBottom(12),
                   juce::Justification::centred, false);

        g.setColour(theme::colour::textMuted);
        g.setFont(theme::label(8.5f));
        g.drawText("BPM", inner.removeFromBottom(12), juce::Justification::centred, false);
    }

    void resized() override { editor_.setBounds(getLocalBounds().reduced(4, 6).withTrimmedBottom(8)); }

    void mouseDown(const juce::MouseEvent&) override
    {
        dragStartTempo_ = session_.getTempo();
        session_.beginTransaction("Change Tempo");
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        const double perPixel = e.mods.isShiftDown() ? 0.01 : 0.25;
        const double tempo =
            std::round((dragStartTempo_ - e.getDistanceFromDragStartY() * perPixel) * 100.0) / 100.0;
        session_.setTempo(tempo);
    }

    void mouseDoubleClick(const juce::MouseEvent&) override
    {
        editor_.setText(juce::String(session_.getTempo(), 2), false);
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
        if (value >= session::kMinTempo && value <= session::kMaxTempo)
        {
            session_.beginTransaction("Change Tempo");
            session_.setTempo(value);
        }
    }

    void valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& property) override
    {
        if (tree == session_.getState() && property == session::ids::tempo)
            repaint();
    }

    void valueTreeRedirected(juce::ValueTree&) override { repaint(); }

    session::Session& session_;
    juce::TextEditor editor_;
    double dragStartTempo_ = 120.0;
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
    , tempoField_(std::make_unique<TempoField>(session))
{
    returnButton_.setTooltip("Return to start (Home)");
    playButton_.setTooltip("Play / pause (Space)");
    stopButton_.setTooltip("Stop. Press again to return to start.");
    recordButton_.setTooltip("Recording arrives with audio/MIDI input (next phase)");
    loopButton_.setTooltip("Loop playback arrives in a later phase");

    returnButton_.onClick = [this]
    {
        controller_.returnToStart();
    };
    playButton_.onClick = [this]
    {
        controller_.togglePlayback();
    };
    stopButton_.onClick = [this]
    {
        if (playing_)
            controller_.stop();
        else
            controller_.returnToStart();
    };

    recordButton_.setEnabled(false);
    loopButton_.setEnabled(false);

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

    testToneButton_.setButtonText("TEST TONE");
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
             &returnButton_, &playButton_, &stopButton_, &recordButton_, &loopButton_, tempoField_.get(),
             &snapButton_, &gridBox_, &testToneButton_, &audioSettingsButton_})
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
}

//==================================================================================================
// Layout

void TransportBar::resized()
{
    auto area = getLocalBounds().reduced(12, 0);

    compactBrand_ = getWidth() < 1500;
    brandArea_ = area.removeFromLeft(compactBrand_ ? 40 : 206);
    area.removeFromLeft(8);

    auto buttons = area.removeFromLeft(5 * kButtonSize + 4 * kButtonGap)
                       .withSizeKeepingCentre(5 * kButtonSize + 4 * kButtonGap, kButtonSize);
    for (auto* button : {&returnButton_, &playButton_, &stopButton_, &recordButton_, &loopButton_})
    {
        button->setBounds(buttons.removeFromLeft(kButtonSize));
        buttons.removeFromLeft(kButtonGap);
    }

    area.removeFromLeft(12);
    positionArea_ = area.removeFromLeft(232).withSizeKeepingCentre(232, 44);
    area.removeFromLeft(8);
    tempoField_->setBounds(area.removeFromLeft(92).withSizeKeepingCentre(92, 44));
    area.removeFromLeft(8);
    signatureArea_ = area.removeFromLeft(60).withSizeKeepingCentre(60, 44);
    area.removeFromLeft(12);
    snapButton_.setBounds(area.removeFromLeft(62).withSizeKeepingCentre(62, 30));
    area.removeFromLeft(4);
    gridBox_.setBounds(area.removeFromLeft(78).withSizeKeepingCentre(78, 30));

    loadArea_ = area.removeFromRight(130).withSizeKeepingCentre(130, 44);
    area.removeFromRight(12);
    audioSettingsButton_.setBounds(area.removeFromRight(72).withSizeKeepingCentre(72, 30));
    area.removeFromRight(8);
    testToneButton_.setBounds(area.removeFromRight(100).withSizeKeepingCentre(100, 30));
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
