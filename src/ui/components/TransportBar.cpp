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

TransportBar::TransportBar(engine::EngineController& controller)
    : controller_(controller)
    , returnButton_("Return to start", IconButton::Icon::returnToStart, theme::colour::textPrimary)
    , playButton_("Play", IconButton::Icon::play, theme::colour::play)
    , stopButton_("Stop", IconButton::Icon::stop, theme::colour::textPrimary)
    , recordButton_("Record", IconButton::Icon::record, theme::colour::record)
    , loopButton_("Loop", IconButton::Icon::loop, theme::colour::gold)
{
    returnButton_.setTooltip("Return to start (Home)");
    playButton_.setTooltip("Play / pause (Space)");
    stopButton_.setTooltip("Stop. Press again to return to start.");
    recordButton_.setTooltip("Record arm - arrives with audio/MIDI input in Phase 2");
    loopButton_.setTooltip("Loop - arrives with the arranger in Phase 3");

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

    for (auto* child : std::initializer_list<juce::Component*>{
             &returnButton_, &playButton_, &stopButton_, &recordButton_, &loopButton_, &testToneButton_})
        addAndMakeVisible(child);
}

TransportBar::~TransportBar() = default;

void TransportBar::update(const engine::TelemetrySnapshot& telemetry, double sampleRate)
{
    if (telemetry.playing != playing_)
    {
        playing_ = telemetry.playing;
        playButton_.setIcon(playing_ ? IconButton::Icon::pause : IconButton::Icon::play);
        playButton_.setLit(playing_);
    }

    if (telemetry.playheadSamples != playheadSamples_ || !juce::exactlyEqual(sampleRate, sampleRate_))
    {
        playheadSamples_ = telemetry.playheadSamples;
        sampleRate_ = sampleRate;
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

    brandArea_ = area.removeFromLeft(206);
    area.removeFromLeft(8);

    auto buttons = area.removeFromLeft(5 * kButtonSize + 4 * kButtonGap)
                       .withSizeKeepingCentre(5 * kButtonSize + 4 * kButtonGap, kButtonSize);
    for (auto* button : {&returnButton_, &playButton_, &stopButton_, &recordButton_, &loopButton_})
    {
        button->setBounds(buttons.removeFromLeft(kButtonSize));
        buttons.removeFromLeft(kButtonGap);
    }

    area.removeFromLeft(16);
    positionArea_ = area.removeFromLeft(248).withSizeKeepingCentre(248, 44);
    area.removeFromLeft(8);
    tempoArea_ = area.removeFromLeft(96).withSizeKeepingCentre(96, 44);
    area.removeFromLeft(8);
    signatureArea_ = area.removeFromLeft(64).withSizeKeepingCentre(64, 44);

    loadArea_ = area.removeFromRight(150).withSizeKeepingCentre(150, 44);
    area.removeFromRight(12);
    testToneButton_.setBounds(area.removeFromRight(108).withSizeKeepingCentre(108, 30));
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
    paintTempoAndSignature(g);
    paintLoadMeter(g);
}

void TransportBar::paintBrand(juce::Graphics& g) const
{
    auto area = brandArea_;
    DojoLookAndFeel::drawBrandMark(g, area.removeFromLeft(34).withSizeKeepingCentre(30, 30).toFloat(),
                                   theme::colour::accent);
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
        time::secondsToQuarterNotes(seconds, time::kDefaultTempoBpm), kSignature);
    const auto clock = time::secondsToClockTime(seconds);

    auto area = positionArea_.reduced(12, 4);
    auto bbtArea = area.removeFromLeft(128);

    g.setColour(playing_ ? theme::colour::textPrimary : theme::colour::textSecondary);
    g.setFont(theme::mono(21.0f));
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

void TransportBar::paintTempoAndSignature(juce::Graphics& g) const
{
    const auto paintBox = [&g](juce::Rectangle<int> box, const juce::String& value, const char* caption)
    {
        paintReadoutBox(g, box);
        auto inner = box.reduced(6, 4);

        g.setColour(theme::colour::gold);
        g.setFont(theme::mono(17.0f));
        g.drawText(value, inner.withTrimmedBottom(12), juce::Justification::centred, false);

        g.setColour(theme::colour::textMuted);
        g.setFont(theme::label(8.5f));
        g.drawText(caption, inner.removeFromBottom(12), juce::Justification::centred, false);
    };

    paintBox(tempoArea_, juce::String(time::kDefaultTempoBpm, 2), "BPM");
    paintBox(signatureArea_, juce::String(kSignature.numerator) + "/" + juce::String(kSignature.denominator),
             "METER");
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
