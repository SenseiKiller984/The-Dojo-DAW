#include "ui/devices/ParameterControl.h"

#include "devices/BuiltinProcessor.h"
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

namespace
{
std::function<void(juce::AudioProcessorParameter&)>& mappingHook()
{
    static std::function<void(juce::AudioProcessorParameter&)> hook;
    return hook;
}
} // namespace

void ParameterControl::setMappingHook(std::function<void(juce::AudioProcessorParameter&)> hook)
{
    mappingHook() = std::move(hook);
}

bool ParameterControl::isMapping() noexcept
{
    return mappingHook() != nullptr;
}

void ParameterControl::mouseDown(const juce::MouseEvent&)
{
    if (learning_)
        if (auto hook = mappingHook()) // (a copy: the hook may end the mapping)
            hook(parameter_);
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
        knob_.setTooltip(parameter_.getName(64) + "  (double-click the value below to type one)");
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
    if (valueEditor_ != nullptr) // deleting a focused field must not apply it to a dying control
    {
        valueEditor_->onFocusLost = nullptr;
        valueEditor_->onReturnKey = nullptr;
        valueEditor_->onEscapeKey = nullptr;
    }
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
    // Learn: the whole control takes the click (its knob, toggle or menu does not).
    if (const bool mapping = isMapping(); mapping != learning_)
    {
        learning_ = mapping;
        for (auto* child : std::initializer_list<juce::Component*>{&knob_, &toggle_, &choice_})
            child->setInterceptsMouseClicks(!mapping, !mapping);
        setMouseCursor(mapping ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
        setRepaintsOnMouseActivity(mapping);
        repaint();
    }
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
        valueArea_ = area.removeFromBottom(14);
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

void ParameterControl::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (style_ == Style::knob && valueArea_.contains(e.getPosition()))
        showValueEditor();
}

void ParameterControl::showValueEditor()
{
    if (style_ != Style::knob)
        return;

    if (valueEditor_ == nullptr)
    {
        valueEditor_ = std::make_unique<juce::TextEditor>();
        valueEditor_->setFont(theme::mono(10.0f));
        valueEditor_->setJustification(juce::Justification::centred);
        valueEditor_->setIndents(2, 1);
        valueEditor_->setSelectAllWhenFocused(true);
        valueEditor_->setColour(juce::TextEditor::backgroundColourId, theme::colour::meterBackground);
        valueEditor_->setColour(juce::TextEditor::outlineColourId, theme::colour::accent);
        valueEditor_->setColour(juce::TextEditor::focusedOutlineColourId, theme::colour::accent);
        valueEditor_->setColour(juce::TextEditor::textColourId, theme::colour::textPrimary);
        valueEditor_->setColour(juce::TextEditor::highlightColourId, theme::colour::accent.withAlpha(0.45f));
        valueEditor_->setColour(juce::CaretComponent::caretColourId, theme::colour::textPrimary);
        valueEditor_->onReturnKey = [this] { finishValueEditor(true); };
        valueEditor_->onEscapeKey = [this] { finishValueEditor(false); };
        valueEditor_->onFocusLost = [this] { finishValueEditor(true); };
        addChildComponent(*valueEditor_);
    }

    valueEditor_->setText(parameter_.getText(parameter_.getValue(), 32).trim(), juce::dontSendNotification);
    valueEditor_->setBounds(valueArea_.withTop(valueArea_.getY() - 3).reduced(2, 0));
    valueEditor_->setVisible(true);
    valueEditor_->selectAll();
    if (isShowing())
        valueEditor_->grabKeyboardFocus();
}

void ParameterControl::finishValueEditor(bool apply)
{
    if (valueEditor_ == nullptr || !valueEditor_->isVisible())
        return; // already finished (hiding the field also makes it lose focus)

    const auto text = valueEditor_->getText().trim();
    valueEditor_->setVisible(false);

    if (apply && text.isNotEmpty())
    {
        // Native parameters need a number in the text; hosted plug-ins parse their own strings.
        const bool native = dynamic_cast<juce::RangedAudioParameter*>(&parameter_) != nullptr;
        if (!native || devices::parseParameterNumber(text, parameter_.getLabel()).has_value())
        {
            const float normalised = juce::jlimit(0.0f, 1.0f, parameter_.getValueForText(text));
            setFromUser(normalised);
            knob_.setValue(normalised, juce::dontSendNotification);
        }
    }
    repaint();
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

void ParameterControl::paintOverChildren(juce::Graphics& g)
{
    if (!learning_)
        return;
    // Learn: every control that can be mapped is outlined; click one to map it.
    const auto area = getLocalBounds().toFloat().reduced(1.5f);
    g.setColour(theme::colour::accent.withAlpha(isMouseOver() ? 0.3f : 0.12f));
    g.fillRoundedRectangle(area, 4.0f);
    g.setColour(theme::colour::accentBright);
    g.drawRoundedRectangle(area, 4.0f, isMouseOver() ? 2.0f : 1.0f);
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
