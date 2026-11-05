#include "ui/devices/ParameterControl.h"

#include "ui/theme/DojoTheme.h"

namespace dojo::ui
{

juce::AudioProcessorParameter* findParameter(juce::AudioProcessor& processor, const juce::String& id)
{
    for (auto* parameter : processor.getParameters())
        if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*>(parameter); withId != nullptr && withId->paramID == id)
            return parameter;
    return nullptr;
}

ParameterControl::ParameterControl(juce::AudioProcessorParameter& parameter, const juce::String& caption)
    : parameter_(parameter)
    , caption_(caption.isNotEmpty() ? caption : parameter.getName(24))
{
    if (auto* choiceParameter = dynamic_cast<juce::AudioParameterChoice*>(&parameter_))
    {
        style_ = Style::choice;
        int id = 1;
        for (const auto& name : choiceParameter->choices)
            choice_.addItem(name, id++);
        choice_.setWantsKeyboardFocus(false);
        choice_.onChange = [this]
        {
            auto* p = dynamic_cast<juce::AudioParameterChoice*>(&parameter_);
            const int index = choice_.getSelectedId() - 1;
            if (p != nullptr && index >= 0)
                setFromUser(p->convertTo0to1(static_cast<float>(index)));
        };
        addAndMakeVisible(choice_);
    }
    else if (parameter_.isBoolean())
    {
        style_ = Style::toggle;
        toggle_.setButtonText(caption_.toUpperCase());
        toggle_.setClickingTogglesState(true);
        toggle_.setWantsKeyboardFocus(false);
        toggle_.setColour(juce::TextButton::buttonOnColourId, theme::colour::accent);
        toggle_.onClick = [this] { setFromUser(toggle_.getToggleState() ? 1.0f : 0.0f); };
        addAndMakeVisible(toggle_);
    }
    else
    {
        knob_.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        knob_.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
        knob_.setRange(0.0, 1.0, 0.0);
        knob_.setDoubleClickReturnValue(true, parameter_.getDefaultValue());
        knob_.setWantsKeyboardFocus(false);
        knob_.setTooltip(parameter_.getName(64));
        knob_.onDragStart = [this]
        {
            dragging_ = true;
            parameter_.beginChangeGesture();
        };
        knob_.onDragEnd = [this]
        {
            dragging_ = false;
            parameter_.endChangeGesture();
        };
        knob_.onValueChange = [this]
        {
            if (dragging_)
                parameter_.setValueNotifyingHost(static_cast<float>(knob_.getValue()));
            else
                setFromUser(static_cast<float>(knob_.getValue()));
            repaint();
        };
        addAndMakeVisible(knob_);
    }

    timerCallback();
    startTimerHz(15);
}

ParameterControl::~ParameterControl()
{
    if (dragging_)
        parameter_.endChangeGesture();
}

void ParameterControl::setFromUser(float normalised)
{
    parameter_.beginChangeGesture();
    parameter_.setValueNotifyingHost(normalised);
    parameter_.endChangeGesture();
    shown_ = normalised;
    repaint();
}

void ParameterControl::timerCallback()
{
    if (dragging_)
        return;

    const float value = parameter_.getValue();
    if (std::abs(value - shown_) < 1.0e-6f)
        return;
    shown_ = value;

    switch (style_)
    {
    case Style::knob:
        knob_.setValue(value, juce::dontSendNotification);
        break;
    case Style::toggle:
        toggle_.setToggleState(value >= 0.5f, juce::dontSendNotification);
        break;
    case Style::choice:
        if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(&parameter_))
            choice_.setSelectedId(juce::roundToInt(p->convertFrom0to1(value)) + 1, juce::dontSendNotification);
        break;
    }
    repaint();
}

void ParameterControl::resized()
{
    auto area = getLocalBounds();
    switch (style_)
    {
    case Style::knob:
        area.removeFromTop(14);
        area.removeFromBottom(14);
        knob_.setBounds(area.withSizeKeepingCentre(std::min(area.getWidth(), area.getHeight()), std::min(area.getWidth(), area.getHeight())));
        break;
    case Style::toggle:
        toggle_.setBounds(area.withSizeKeepingCentre(area.getWidth() - 4, 24));
        break;
    case Style::choice:
        area.removeFromTop(16);
        choice_.setBounds(area.withSizeKeepingCentre(area.getWidth() - 2, 24));
        break;
    }
}

void ParameterControl::paint(juce::Graphics& g)
{
    if (style_ == Style::toggle)
        return;

    auto area = getLocalBounds();
    g.setColour(theme::colour::textSecondary);
    g.setFont(theme::label(9.0f));
    g.drawText(caption_.toUpperCase(), area.removeFromTop(14), juce::Justification::centred, true);

    if (style_ == Style::knob)
    {
        const float value = parameter_.getValue();
        auto text = parameter_.getText(value, 12).trim();
        const auto unit = parameter_.getLabel().trim();
        if (unit.isNotEmpty() && !text.endsWith(unit))
            text << " " << unit;
        g.setColour(theme::colour::textPrimary);
        g.setFont(theme::mono(10.0f));
        g.drawText(text, area.removeFromBottom(14), juce::Justification::centred, true);
    }
}

//==================================================================================================

void ParameterGrid::add(juce::AudioProcessorParameter& parameter, const juce::String& caption)
{
    auto control = std::make_unique<ParameterControl>(parameter, caption);
    addAndMakeVisible(*control);
    controls_.push_back(std::move(control));
    resized();
}

void ParameterGrid::resized()
{
    const int columns = std::max(1, getWidth() / ParameterControl::kWidth);
    for (std::size_t i = 0; i < controls_.size(); ++i)
    {
        const int column = static_cast<int>(i) % columns;
        const int row = static_cast<int>(i) / columns;
        controls_[i]->setBounds(column * ParameterControl::kWidth, row * ParameterControl::kHeight, ParameterControl::kWidth,
                                ParameterControl::kHeight);
    }
}

} // namespace dojo::ui
