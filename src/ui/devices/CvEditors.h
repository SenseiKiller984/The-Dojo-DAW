#pragma once

#include "devices/CvDevices.h"
#include "ui/devices/DeviceEditors.h"
#include "ui/devices/ParameterControl.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace dojo::ui
{

/**
    The CV devices' editor: every control (the ports' sound card channels among them), each port's
    voltage as it goes out, whether the sound card is there and has the channels chosen, and - for
    the CV Instrument - Calibrate and what it found.
*/
class CvEditor final : public juce::Component, private juce::Timer
{
public:
    static constexpr int kPortsWidth = 210;

    CvEditor(devices::CvDevice& device, const DeviceEditorContext& context);
    [[nodiscard]] static int preferredWidth(const devices::CvDevice& device);

    [[nodiscard]] juce::Button& getCalibrateButton() noexcept { return calibrate_; }
    [[nodiscard]] juce::String getStatus() const { return status_.getText(); }
    void refresh();

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override { refresh(); }

    devices::CvDevice& device_;
    devices::CvInstrument* instrument_ = nullptr;
    juce::Viewport viewport_;
    ParameterGrid grid_;
    juce::TextButton calibrate_{"Calibrate"};
    juce::Label status_;
};

/** CV In: its voltage and output, mapped like the LFO (see LfoEditor). */
[[nodiscard]] std::unique_ptr<juce::Component> createCvInEditor(devices::CvIn& device, const DeviceEditorContext& context, int& preferredWidth);

/** A CV device's editor, or null for other devices. */
[[nodiscard]] std::unique_ptr<juce::Component> createCvEditor(juce::AudioProcessor& processor, const DeviceEditorContext& context, int& preferredWidth);

} // namespace dojo::ui
