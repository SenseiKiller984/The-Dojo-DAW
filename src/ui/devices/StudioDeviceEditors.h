#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace dojo::devices
{
class TheGlue;
}

namespace dojo::ui
{

/**
    Editors of the studio devices: The Glue, De-Esser, Expander, Soft Clipper and Plate Reverb.
    Null for any other processor.
*/
std::unique_ptr<juce::Component> createStudioDeviceEditor(juce::AudioProcessor& processor, int& preferredWidth);

/**
    The Glue's VU meter of the gain reduction.
    - The white needle shows the RMS compression (VU ballistics).
    - Click the meter to show (or hide) the yellow Peak needle: the peak compression, held briefly.
    - Hover over it to read the RMS and Peak compression in dB below the meter.
*/
class GlueVuMeter final : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    static constexpr double kScaleDb = 20.0;   ///< full scale of the meter
    static constexpr double kPeakHoldSeconds = 0.6;
    static constexpr double kPeakFallDbPerSecond = 20.0;

    explicit GlueVuMeter(devices::TheGlue& glue);

    /** (The choice is kept by the device: it survives page changes and is saved with it.) */
    [[nodiscard]] bool isPeakVisible() const noexcept;
    void setPeakVisible(bool visible);
    /** The needles' readings (dB of compression, >= 0). */
    [[nodiscard]] double getRmsDb() const noexcept { return rms_; }
    [[nodiscard]] double getPeakDb() const noexcept { return peak_; }
    /** What hovering shows below the meter. */
    [[nodiscard]] juce::String getReadout() const;
    /** Reads the device's meters (the timer does this 30 times a second; tests call it directly). */
    void update(double elapsedSeconds);

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent&) override;

private:
    void timerCallback() override { update(1.0 / 30.0); }

    devices::TheGlue& glue_;
    double rms_ = 0.0, peak_ = 0.0, peakHeld_ = 0.0;
};

} // namespace dojo::ui
