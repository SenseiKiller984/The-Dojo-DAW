#pragma once

#include "core/Types.h"
#include "engine/EngineController.h"
#include "ui/components/IconButton.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace dojo::ui
{

/**
    Top bar: brand, transport controls, position display (bars|beats|ticks + clock), tempo and
    time signature, the diagnostic test-tone switch and the DSP load meter.

    Live in Phase 1: play/pause, stop (press twice to return to start), return-to-start, the
    position display, test tone and DSP load. Record and loop are present but disabled until their
    phases land.
*/
class TransportBar final : public juce::Component
{
public:
    explicit TransportBar(engine::EngineController& controller);
    ~TransportBar() override;

    /** Called once per UI frame with fresh engine telemetry. Repaints only what changed. */
    void update(const engine::TelemetrySnapshot& telemetry, double sampleRate);

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void paintBrand(juce::Graphics& g) const;
    void paintPositionDisplay(juce::Graphics& g) const;
    void paintTempoAndSignature(juce::Graphics& g) const;
    void paintLoadMeter(juce::Graphics& g) const;

    engine::EngineController& controller_;

    IconButton returnButton_;
    IconButton playButton_;
    IconButton stopButton_;
    IconButton recordButton_;
    IconButton loopButton_;
    juce::TextButton testToneButton_;

    juce::Rectangle<int> brandArea_;
    juce::Rectangle<int> positionArea_;
    juce::Rectangle<int> tempoArea_;
    juce::Rectangle<int> signatureArea_;
    juce::Rectangle<int> loadArea_;

    SampleCount playheadSamples_ = -1;
    double sampleRate_ = 0.0;
    bool playing_ = false;
    float dspLoad_ = 0.0f;
};

} // namespace dojo::ui
