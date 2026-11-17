#include "ui/devices/HardwareInsertEditor.h"

#include "devices/HardwareInsert.h"
#include "ui/devices/ParameterControl.h"
#include "ui/theme/DojoTheme.h"

#include <cmath>

namespace dojo::ui
{
namespace
{
using devices::HardwareInsert;

/** The last measurement's correlation over the round trips it searched, its peak marked. */
class CorrelationScope final : public juce::Component
{
public:
    explicit CorrelationScope(HardwareInsert& insert)
        : insert_(insert)
    {
        setInterceptsMouseClicks(false, false);
    }

    void paint(juce::Graphics& g) override
    {
        using namespace theme;
        auto area = getLocalBounds().toFloat();
        g.setColour(colour::meterBackground);
        g.fillRoundedRectangle(area, 3.0f);
        area = area.reduced(5.0f, 4.0f);

        // The time axis (ms of round trip), with a line every 1, 2, 5, 10 ... ms: at most six.
        const double rate = insert_.getSampleRate() > 0.0 ? insert_.getSampleRate() : 48000.0;
        const int lags = std::max(1, insert_.getCorrelationLags() > 0 ? insert_.getCorrelationLags() : static_cast<int>(0.02 * rate));
        const double spanMs = 1000.0 * lags / rate;
        double step = 1.0;
        for (const double candidate : {1.0, 2.0, 5.0, 10.0, 20.0, 50.0, 100.0, 200.0})
            if (spanMs / candidate <= 6.0)
            {
                step = candidate;
                break;
            }
        g.setFont(mono(8.5f));
        for (int line = 0; line * step <= spanMs; ++line)
        {
            const double ms = line * step;
            const float x = area.getX() + area.getWidth() * static_cast<float>(ms / spanMs);
            g.setColour(colour::gridBeat);
            g.drawVerticalLine(static_cast<int>(x), area.getY(), area.getBottom());
            g.setColour(colour::textMuted);
            g.drawText(juce::String(static_cast<int>(ms)) + (line == 0 ? " ms" : ""),
                       juce::Rectangle<float>(x + 2.0f, area.getBottom() - 10.0f, 40.0f, 10.0f), juce::Justification::centredLeft, false);
        }

        const auto& correlation = insert_.getCorrelation();
        if (correlation.size() < 2)
        {
            g.setColour(colour::textMuted);
            g.setFont(label(9.5f));
            g.drawText("PING TO MEASURE THE ROUND TRIP", area, juce::Justification::centred, false);
            return;
        }

        juce::Path curve;
        const float bottom = area.getBottom() - 10.0f;
        const float height = bottom - area.getY();
        for (std::size_t i = 0; i < correlation.size(); ++i)
        {
            const float x = area.getX() + area.getWidth() * static_cast<float>(i) / static_cast<float>(correlation.size() - 1);
            const float y = bottom - height * juce::jlimit(0.0f, 1.0f, correlation[i]);
            if (i == 0)
                curve.startNewSubPath(x, y);
            else
                curve.lineTo(x, y);
        }
        g.setColour(colour::gold.withAlpha(0.85f));
        g.strokePath(curve, juce::PathStrokeType(1.2f));

        if (const auto& calibration = insert_.getCalibration(); calibration.valid)
        {
            const float x = area.getX() + area.getWidth() * std::min(1.0f, static_cast<float>(calibration.latency) / static_cast<float>(lags));
            g.setColour(colour::accentBright);
            g.drawVerticalLine(static_cast<int>(x), area.getY(), bottom);
            g.setFont(mono(9.0f));
            const auto text = juce::String(calibration.latency) + " smp";
            const bool right = x + 70.0f < area.getRight();
            g.drawText(text, juce::Rectangle<float>(right ? x + 4.0f : x - 70.0f, area.getY(), 66.0f, 11.0f),
                       right ? juce::Justification::centredLeft : juce::Justification::centredRight, false);
        }
    }

private:
    HardwareInsert& insert_;
};

//==================================================================================================
class HardwareInsertEditor final : public juce::Component, private juce::Timer
{
public:
    static constexpr int kPreferredWidth = 740;

    HardwareInsertEditor(HardwareInsert& insert, const DeviceEditorContext& context)
        : insert_(insert)
        , channelNames_(context.channelNames)
        , sendGain_(*insert.sendGain_, "Send")
        , returnGain_(*insert.returnGain_, "Return")
        , mix_(*insert.mix_, "Dry/Wet")
        , phase_(*insert.phase_, "Phase")
        , offset_(*insert.offset_, "Offset")
        , autoPing_(*insert.autoPing_, "Auto Ping")
        , pingLevel_(*insert.pingLevel_, "Ping Level")
        , scope_(insert)
    {
        for (auto* caption : {&sendCaption_, &returnCaption_, &latencyCaption_})
        {
            caption->setFont(theme::label(9.5f));
            caption->setColour(juce::Label::textColourId, theme::colour::textMuted);
            addAndMakeVisible(caption);
        }
        sendCaption_.setText("SEND TO OUTPUT", juce::dontSendNotification);
        returnCaption_.setText("RETURN FROM INPUT", juce::dontSendNotification);
        latencyCaption_.setText("ROUND TRIP", juce::dontSendNotification);

        sendTo_.setComponentID("send");
        returnFrom_.setComponentID("return");
        sendTo_.setTooltip("The sound card output(s) the track leaves through, to the hardware's input");
        returnFrom_.setTooltip("The sound card input(s) the hardware's output comes back on");
        sendTo_.onChange = [this] { choose(*insert_.send_, sendTo_); };
        returnFrom_.onChange = [this] { choose(*insert_.return_, returnFrom_); };
        for (auto* combo : {&sendTo_, &returnFrom_})
        {
            combo->setWantsKeyboardFocus(false);
            addAndMakeVisible(combo);
        }

        for (auto* control : {&sendGain_, &returnGain_, &mix_, &phase_, &offset_, &autoPing_, &pingLevel_})
            addAndMakeVisible(control);

        for (auto* warning : {&sendWarning_, &returnWarning_})
        {
            warning->setFont(theme::sans(10.5f));
            warning->setColour(juce::Label::textColourId, theme::colour::meterHigh);
            warning->setJustificationType(juce::Justification::topLeft);
            warning->setMinimumHorizontalScale(1.0f);
            addAndMakeVisible(warning);
        }
        sendWarning_.setComponentID("sendWarning");
        returnWarning_.setComponentID("returnWarning");

        readout_.setComponentID("readout");
        readout_.setFont(theme::mono(17.0f));
        readout_.setColour(juce::Label::textColourId, theme::colour::gold);
        source_.setComponentID("source");
        source_.setFont(theme::sans(10.5f));
        source_.setColour(juce::Label::textColourId, theme::colour::textSecondary);
        status_.setComponentID("status");
        status_.setFont(theme::sans(10.5f));
        status_.setJustificationType(juce::Justification::topLeft);
        status_.setMinimumHorizontalScale(1.0f);
        for (auto* text : {&readout_, &source_, &status_})
            addAndMakeVisible(text);
        addAndMakeVisible(scope_);

        ping_.setComponentID("ping");
        match_.setComponentID("match");
        ping_.setTooltip("Measures the round trip: a short sweep goes out on the send and is found again in the return "
                         "(the device is silent for half a second meanwhile)");
        match_.setTooltip("Sets Return so the loop comes back at the level it was sent (from the last measurement)");
        ping_.onClick = [this]
        {
            insert_.ping();
            refresh();
        };
        match_.onClick = [this] { insert_.matchLevels(); };
        for (auto* button : {&ping_, &match_})
        {
            button->setWantsKeyboardFocus(false);
            addAndMakeVisible(button);
        }
        ping_.setColour(juce::TextButton::buttonColourId, theme::colour::accent.withAlpha(0.8f));

        refresh();
        startTimerHz(15);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(6, 4);
        auto latency = area.removeFromRight(280);
        area.removeFromRight(10);

        const int column = area.getWidth() / 3;
        auto send = area.removeFromLeft(column).withTrimmedRight(8);
        auto ret = area.removeFromLeft(column).withTrimmedRight(8);
        auto mix = area;

        const auto top = [](juce::Rectangle<int>& r, juce::Label& caption, juce::ComboBox& combo)
        {
            caption.setBounds(r.removeFromTop(14));
            combo.setBounds(r.removeFromTop(24));
            r.removeFromTop(6);
        };
        top(send, sendCaption_, sendTo_);
        top(ret, returnCaption_, returnFrom_);
        mix.removeFromTop(44);

        auto sendRow = send.removeFromTop(ParameterControl::kHeight);
        sendGain_.setBounds(sendRow.removeFromLeft(ParameterControl::kWidth));
        sendMeter_ = sendRow.removeFromLeft(10).reduced(0, 6);
        sendWarning_.setBounds(send.removeFromTop(48));

        auto returnRow = ret.removeFromTop(ParameterControl::kHeight);
        returnGain_.setBounds(returnRow.removeFromLeft(ParameterControl::kWidth));
        returnMeter_ = returnRow.removeFromLeft(10).reduced(0, 6);
        returnRow.removeFromLeft(6);
        phase_.setBounds(returnRow.removeFromLeft(ParameterControl::kWidth));
        returnWarning_.setBounds(ret.removeFromTop(48));

        mix.setY(sendGain_.getY());
        auto mixRow = mix.removeFromTop(ParameterControl::kHeight);
        mix_.setBounds(mixRow.removeFromLeft(ParameterControl::kWidth));
        offset_.setBounds(mixRow.removeFromLeft(ParameterControl::kWidth));
        auto pingRow = mix.removeFromTop(ParameterControl::kHeight);
        autoPing_.setBounds(pingRow.removeFromLeft(ParameterControl::kWidth));
        pingLevel_.setBounds(pingRow.removeFromLeft(ParameterControl::kWidth));

        latencyPanel_ = latency;
        latency.reduce(8, 4);
        latencyCaption_.setBounds(latency.removeFromTop(14));
        readout_.setBounds(latency.removeFromTop(22));
        source_.setBounds(latency.removeFromTop(16));
        auto buttons = latency.removeFromBottom(24);
        ping_.setBounds(buttons.removeFromLeft(72));
        buttons.removeFromLeft(6);
        match_.setBounds(buttons.removeFromLeft(72));
        status_.setBounds(latency.removeFromBottom(30));
        latency.removeFromBottom(2);
        scope_.setBounds(latency.reduced(0, 2));
    }

    void paint(juce::Graphics& g) override
    {
        using namespace theme;
        g.setColour(colour::panelRaised);
        g.fillRoundedRectangle(latencyPanel_.toFloat(), 4.0f);
        paintMeter(g, sendMeter_, sendLevel_);
        paintMeter(g, returnMeter_, returnLevel_);
    }

private:
    static void paintMeter(juce::Graphics& g, juce::Rectangle<int> area, float level)
    {
        if (area.isEmpty())
            return;
        g.setColour(theme::colour::meterBackground);
        g.fillRect(area);
        const float db = juce::Decibels::gainToDecibels(level, -60.0f);
        const float proportion = juce::jlimit(0.0f, 1.0f, (db + 60.0f) / 60.0f);
        const auto bar = area.toFloat();
        const auto lit = bar.withTrimmedTop(bar.getHeight() * (1.0f - proportion));
        g.setColour(db > -1.0f ? theme::colour::meterClip : (db > -12.0f ? theme::colour::meterMid : theme::colour::meterLow));
        g.fillRect(lit);
    }

    void choose(juce::AudioParameterChoice& parameter, const juce::ComboBox& combo)
    {
        const int index = combo.getSelectedId() - 1;
        if (index < 0 || index == parameter.getIndex())
            return;
        parameter.beginChangeGesture();
        parameter.setValueNotifyingHost(parameter.convertTo0to1(static_cast<float>(index)));
        parameter.endChangeGesture();
        refresh();
    }

    /** The sound card's channel names (outputs or inputs), or none when unknown. */
    [[nodiscard]] juce::StringArray namesOf(bool outputs) const { return channelNames_ ? channelNames_(outputs) : juce::StringArray{}; }

    /** How many channels the sound card has (-1: unknown). */
    [[nodiscard]] int countOf(bool outputs, const juce::StringArray& names) const
    {
        const auto hardware = insert_.getHardware();
        if (hardware.live)
            return outputs ? hardware.numOutputs : hardware.numInputs;
        return names.isEmpty() ? -1 : names.size();
    }

    /** "3/4  Out 3 + Out 4" for a choice. */
    static juce::String describe(int choice, const juce::StringArray& names)
    {
        const auto channels = HardwareInsert::channelsOf(choice);
        juce::String text = HardwareInsert::channelChoices()[choice];
        if (channels.count == 0 || names.isEmpty())
            return text;
        juce::StringArray parts;
        for (int c = channels.first; c < channels.first + channels.count; ++c)
            if (c < names.size())
                parts.add(names[c]);
        return parts.isEmpty() ? text : text + "   " + parts.joinIntoString(" + ");
    }

    void fill(juce::ComboBox& combo, int selected, bool outputs)
    {
        const auto names = namesOf(outputs);
        const int count = countOf(outputs, names);
        const auto signature = names.joinIntoString("|") + "#" + juce::String(count) + "#" + juce::String(selected);
        auto& last = outputs ? sendSignature_ : returnSignature_;
        if (signature == last)
            return;
        last = signature;

        combo.clear(juce::dontSendNotification);
        const int shown = count >= 0 ? count : 16; // (unknown: the first sixteen)
        combo.addItem("None", 1);
        if (shown >= 2)
            combo.addSectionHeading("Stereo pairs");
        for (int pair = 0; 2 * pair + 1 < shown; ++pair)
            combo.addItem(describe(1 + pair, names), 2 + pair);
        if (shown >= 1)
            combo.addSectionHeading("Single channels");
        for (int channel = 0; channel < shown; ++channel)
            combo.addItem(describe(33 + channel, names), 34 + channel);
        const auto channels = HardwareInsert::channelsOf(selected);
        if (channels.count > 0 && channels.first + channels.count > shown)
        {
            combo.addSeparator();
            combo.addItem(describe(selected, names) + "  (not on this sound card)", selected + 1);
        }
        combo.setSelectedId(selected + 1, juce::dontSendNotification);
    }

    /** Why a choice can't be used, or nothing. */
    [[nodiscard]] juce::String warningFor(bool outputs) const
    {
        const auto channels = outputs ? insert_.getSendChannels() : insert_.getReturnChannels();
        const auto names = namesOf(outputs);
        const int count = countOf(outputs, names);
        const juce::String kind = outputs ? "output" : "input";
        if (channels.count == 0)
            return outputs ? "Nothing is sent." : "Nothing returns: the signal passes through.";
        if (count >= 0 && channels.first + channels.count > count)
            return HardwareInsert::channelChoices()[outputs ? insert_.send_->getIndex() : insert_.return_->getIndex()] + " isn't on this sound card ("
                   + juce::String(count) + " " + kind + (count == 1 ? "" : "s") + "): Preferences > Audio switches more on."
                   + (outputs ? "" : " The signal passes through.");
        return {};
    }

    void refresh()
    {
        fill(sendTo_, insert_.send_->getIndex(), true);
        fill(returnFrom_, insert_.return_->getIndex(), false);
        sendWarning_.setText(warningFor(true), juce::dontSendNotification);
        returnWarning_.setText(warningFor(false), juce::dontSendNotification);

        const int latency = insert_.getLatencySamples();
        const double rate = insert_.getSampleRate() > 0.0 ? insert_.getSampleRate() : 48000.0;
        readout_.setText(juce::String(latency) + " smp   " + juce::String(1000.0 * latency / rate, 2) + " ms", juce::dontSendNotification);

        const auto hardware = insert_.getHardware();
        const auto& calibration = insert_.getCalibration();
        const auto state = insert_.getPingState();
        juce::String source;
        if (state == HardwareInsert::PingState::waiting)
            source = "Waiting for the sound card...";
        else if (state == HardwareInsert::PingState::running || state == HardwareInsert::PingState::analysing)
            source = "Pinging...";
        else if (insert_.isCalibrationCurrent() || (calibration.valid && !hardware.live))
            source = "Measured at " + juce::String(calibration.sampleRate / 1000.0, 1) + " kHz, " + juce::String(calibration.bufferSize)
                     + "-sample buffer";
        else if (calibration.valid)
            source = "Measured with another setup: the driver's figure stands in";
        else if (hardware.reportedLatency > 0)
            source = "The driver's figure (not measured yet)";
        else
            source = "Two buffers, a guess (not measured yet)";
        if (const auto offset = static_cast<int>(std::lround(insert_.offset_->get())); offset != 0)
            source << "  (offset " << (offset > 0 ? "+" : "") << offset << ")";
        source_.setText(source, juce::dontSendNotification);

        const auto& message = insert_.getMessage();
        const bool failed = message.startsWith("Nothing") || message.startsWith("The return") || message.startsWith("Can't");
        status_.setColour(juce::Label::textColourId, failed ? theme::colour::record : theme::colour::textSecondary);
        status_.setText(message, juce::dontSendNotification);

        ping_.setButtonText(state == HardwareInsert::PingState::waiting ? "CANCEL" : "PING");
        ping_.setEnabled(state == HardwareInsert::PingState::idle || state == HardwareInsert::PingState::waiting);
        match_.setEnabled(calibration.valid);
        scope_.repaint();
    }

    void timerCallback() override
    {
        const auto [send, ret] = insert_.takePeaks();
        sendLevel_ = std::max(send, sendLevel_ * 0.8f);
        returnLevel_ = std::max(ret, returnLevel_ * 0.8f);
        repaint(sendMeter_);
        repaint(returnMeter_);
        refresh();
    }

    HardwareInsert& insert_;
    std::function<juce::StringArray(bool)> channelNames_;
    juce::Label sendCaption_, returnCaption_, latencyCaption_;
    juce::ComboBox sendTo_, returnFrom_;
    ParameterControl sendGain_, returnGain_, mix_, phase_, offset_, autoPing_, pingLevel_;
    juce::Label sendWarning_, returnWarning_;
    juce::Label readout_, source_, status_;
    CorrelationScope scope_;
    juce::TextButton ping_{"PING"}, match_{"MATCH"};
    juce::Rectangle<int> sendMeter_, returnMeter_, latencyPanel_;
    float sendLevel_ = 0.0f, returnLevel_ = 0.0f;
    juce::String sendSignature_, returnSignature_;
};

} // namespace

std::unique_ptr<juce::Component> createHardwareInsertEditor(devices::HardwareInsert& insert, const DeviceEditorContext& context,
                                                            int& preferredWidth)
{
    preferredWidth = HardwareInsertEditor::kPreferredWidth;
    return std::make_unique<HardwareInsertEditor>(insert, context);
}

} // namespace dojo::ui
