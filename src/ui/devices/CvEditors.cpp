#include "ui/devices/CvEditors.h"

#include "ui/devices/MoreDeviceEditors.h"
#include "ui/theme/DojoTheme.h"

#include <cmath>

namespace dojo::ui
{

CvEditor::CvEditor(devices::CvDevice& device, const DeviceEditorContext&)
    : device_(device)
    , instrument_(dynamic_cast<devices::CvInstrument*>(&device))
{
    for (auto* parameter : device_.getParameters())
        grid_.add(*parameter);
    viewport_.setViewedComponent(&grid_, false);
    viewport_.setScrollBarsShown(true, false);
    addAndMakeVisible(viewport_);
    status_.setFont(theme::sans(11.0f));
    status_.setColour(juce::Label::textColourId, theme::colour::textSecondary);
    addAndMakeVisible(status_);
    if (instrument_ != nullptr)
    {
        calibrate_.setWantsKeyboardFocus(false);
        calibrate_.setTooltip("Tune the pitch output to the oscillator: steps of 0-3 V go out on Pitch Out, its pitch is measured on Audio In, "
                              "and Tracking and Offset are set");
        calibrate_.onClick = [this]
        {
            instrument_->calibrate();
            refresh();
        };
        addAndMakeVisible(calibrate_);
    }
    refresh();
    startTimerHz(10);
}

int CvEditor::preferredWidth(const devices::CvDevice& device)
{
    const int count = static_cast<int>(const_cast<devices::CvDevice&>(device).getParameters().size());
    const int columns = juce::jlimit(3, 7, (count + 1) / 2);
    return columns * ParameterControl::kWidth + 16 + kPortsWidth;
}

void CvEditor::refresh()
{
    juce::String text;
    if (!device_.isLive())
        text = "No sound card running (or rendering offline): the voltages go nowhere.";
    else
    {
        text = juce::String(device_.getSeenOutputs()) + " outputs, " + juce::String(device_.getSeenInputs()) + " inputs";
        if (const auto problems = device_.getProblems(); problems.isNotEmpty())
            text << ". " << problems;
    }
    if (instrument_ != nullptr)
    {
        const auto state = instrument_->getCalibration();
        calibrate_.setButtonText(state == devices::CvInstrument::Calibration::waiting || state == devices::CvInstrument::Calibration::running ? "Cancel"
                                                                                                                                                : "Calibrate");
        if (const auto message = instrument_->getCalibrationMessage(); message.isNotEmpty())
            text = message;
    }
    if (text != status_.getText())
        status_.setText(text, juce::dontSendNotification);
    repaint(getLocalBounds().removeFromRight(kPortsWidth));
}

void CvEditor::resized()
{
    auto area = getLocalBounds();
    auto bottom = area.removeFromBottom(26).reduced(6, 2);
    if (instrument_ != nullptr)
    {
        calibrate_.setBounds(bottom.removeFromLeft(86));
        bottom.removeFromLeft(8);
    }
    status_.setBounds(bottom);
    area.removeFromRight(kPortsWidth);
    viewport_.setBounds(area);
    const int width = viewport_.getMaximumVisibleWidth();
    const int columns = std::max(1, width / ParameterControl::kWidth);
    grid_.setSize(width, (grid_.getNumControls() + columns - 1) / columns * ParameterControl::kHeight);
}

void CvEditor::paint(juce::Graphics& g)
{
    using namespace theme;
    auto ports = getLocalBounds().withTrimmedBottom(26).removeFromRight(kPortsWidth).reduced(8, 4);
    g.setColour(colour::outline);
    g.fillRect(ports.getX() - 8, ports.getY(), 1, ports.getHeight());
    g.setColour(colour::textSecondary);
    g.setFont(label(10.5f));
    g.drawText("PORTS", ports.removeFromTop(18), juce::Justification::centredLeft, false);
    const double fullScale = device_.fullScale_->get();
    for (int p = 0; p < device_.getNumPorts(); ++p)
    {
        auto row = ports.removeFromTop(22);
        if (row.getHeight() < 22)
            break;
        const int channel = devices::cv::channelOf(device_.getPortChoice(p)->getIndex());
        const float volts = device_.getPortVolts(p);
        g.setColour(channel >= 0 ? colour::textPrimary : colour::textMuted);
        g.setFont(sans(11.0f));
        g.drawText(device_.getPortName(p), row.removeFromLeft(62), juce::Justification::centredLeft, true);
        g.setColour(colour::textMuted);
        g.drawText(channel >= 0 ? "out " + juce::String(channel + 1) : juce::String("-"), row.removeFromLeft(40), juce::Justification::centredLeft, false);
        auto value = row.removeFromRight(52);
        g.setColour(colour::textSecondary);
        g.drawText(juce::String(volts, 2) + " V", value, juce::Justification::centredRight, false);
        // A bar from 0 V, either way, to the full scale.
        auto bar = row.reduced(4, 7).toFloat();
        g.setColour(colour::control);
        g.fillRect(bar);
        const float centre = bar.getCentreX();
        const float x = centre + static_cast<float>(std::clamp(volts / fullScale, -1.0, 1.0)) * bar.getWidth() * 0.5f;
        g.setColour(channel >= 0 ? colour::gold : colour::textMuted);
        g.fillRect(juce::Rectangle<float>::leftTopRightBottom(std::min(centre, x), bar.getY(), std::max(centre, x), bar.getBottom()));
        g.setColour(colour::outlineStrong);
        g.fillRect(centre, bar.getY() - 1.0f, 1.0f, bar.getHeight() + 2.0f);
    }
}

namespace
{
/** CV In's display: the voltage and where it puts the targets. */
class CvInDisplay final : public juce::Component, private juce::Timer
{
public:
    explicit CvInDisplay(devices::CvIn& device)
        : device_(device)
    {
        startTimerHz(20);
    }

    void paint(juce::Graphics& g) override
    {
        using namespace theme;
        auto area = getLocalBounds().reduced(8, 6).toFloat();
        g.setColour(colour::panelRaised);
        g.fillRoundedRectangle(getLocalBounds().toFloat(), 4.0f);
        g.setColour(colour::textSecondary);
        g.setFont(label(10.5f));
        g.drawText("CV IN", area.removeFromTop(16.0f), juce::Justification::centredLeft, false);
        g.setFont(sans(20.0f));
        g.setColour(colour::textPrimary);
        g.drawText(device_.isLive() ? juce::String(device_.getVolts(), 2) + " V" : juce::String("--"), area.removeFromTop(30.0f),
                   juce::Justification::centredLeft, false);
        g.setFont(sans(10.5f));
        g.setColour(colour::textMuted);
        g.drawText(device_.isLive() ? "from input " + devices::cv::channelChoices()[device_.input_->getIndex()] : juce::String("no sound card"),
                   area.removeFromTop(14.0f), juce::Justification::centredLeft, false);
        auto bar = area.removeFromBottom(14.0f);
        g.setColour(colour::control);
        g.fillRect(bar);
        g.setColour(colour::gold);
        g.fillRect(bar.withWidth(bar.getWidth() * std::clamp(device_.getOutput(), 0.0f, 1.0f)));
    }

private:
    void timerCallback() override { repaint(); }
    devices::CvIn& device_;
};
} // namespace

std::unique_ptr<juce::Component> createCvInEditor(devices::CvIn& device, const DeviceEditorContext& context, int& preferredWidth)
{
    preferredWidth = LfoEditor::preferredWidth();
    return std::make_unique<LfoEditor>(device, std::make_unique<CvInDisplay>(device),
                                       juce::StringArray{"input|CV In", "minimum|Minimum", "maximum|Maximum", "fullScale|Full Scale", "smooth|Smooth",
                                                         "invert|Invert"},
                                       "CV input", context);
}

std::unique_ptr<juce::Component> createCvEditor(juce::AudioProcessor& processor, const DeviceEditorContext& context, int& preferredWidth)
{
    if (auto* in = dynamic_cast<devices::CvIn*>(&processor))
        return createCvInEditor(*in, context, preferredWidth);
    if (auto* cv = dynamic_cast<devices::CvDevice*>(&processor))
    {
        preferredWidth = CvEditor::preferredWidth(*cv);
        return std::make_unique<CvEditor>(*cv, context);
    }
    return {};
}

} // namespace dojo::ui
