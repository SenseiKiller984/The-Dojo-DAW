#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace dojo::ui
{

/**
    A control bound to one juce::AudioProcessorParameter of a device or hosted plug-in: a rotary
    knob for continuous parameters, a toggle for booleans, a drop-down for choices.

    Edits are sent as proper change gestures (setValueNotifyingHost between begin/endChangeGesture).
    The control follows the parameter by polling it (15 Hz), so automation, presets and plug-in
    GUIs are reflected without any listener running on the audio thread. Values are normalised
    (0..1); the text comes from the parameter itself.
*/
class ParameterControl final : public juce::Component, private juce::Timer
{
public:
    explicit ParameterControl(juce::AudioProcessorParameter& parameter, const juce::String& caption = {});
    ~ParameterControl() override;

    void resized() override;
    void paint(juce::Graphics& g) override;

    [[nodiscard]] juce::AudioProcessorParameter& getParameter() noexcept { return parameter_; }

    /** Preferred size for grids. */
    static constexpr int kWidth = 64;
    static constexpr int kHeight = 72;

private:
    enum class Style
    {
        knob,
        toggle,
        choice,
    };

    void timerCallback() override;
    void setFromUser(float normalised);

    juce::AudioProcessorParameter& parameter_;
    juce::String caption_;
    Style style_ = Style::knob;
    juce::Slider knob_;
    juce::TextButton toggle_;
    juce::ComboBox choice_;
    bool dragging_ = false;
    float shown_ = -1.0f;
};

/** A grid of ParameterControls (wraps to the available width). */
class ParameterGrid final : public juce::Component
{
public:
    void add(juce::AudioProcessorParameter& parameter, const juce::String& caption = {});
    void clear() { controls_.clear(); }
    void resized() override;
    [[nodiscard]] int getNumControls() const noexcept { return static_cast<int>(controls_.size()); }

private:
    std::vector<std::unique_ptr<ParameterControl>> controls_;
};

/** Finds a parameter of a processor by its parameter ID (built-in devices). */
juce::AudioProcessorParameter* findParameter(juce::AudioProcessor& processor, const juce::String& id);

} // namespace dojo::ui
