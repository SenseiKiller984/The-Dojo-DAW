#pragma once

#include "engine/EngineController.h"
#include "ui/components/LevelMeter.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace dojo::ui
{

/**
    The live master channel strip: fader (-inf .. +6 dB, smoothed in the engine), stereo peak meter
    with scale, and numeric readouts. Fully wired to the engine in Phase 1.
*/
class MasterStrip final : public juce::Component
{
public:
    explicit MasterStrip(engine::EngineController& controller);

    void pushLevels(float peakLeft, float peakRight, double elapsedSeconds);

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void updateGainReadout();

    engine::EngineController& controller_;
    juce::Slider fader_;
    LevelMeter meter_;
    juce::Rectangle<int> scaleArea_;
    juce::Rectangle<int> readoutArea_;
    float heldPeakDb_ = LevelMeter::kMinDb;
    double peakHoldAge_ = 0.0;
};

/**
    Bottom mixer panel. Placeholder channel strips (layout preview of the Phase 2/3 mixer: colour cap,
    name, pan, mute/solo/phase, fader and meter) plus the live master strip.
*/
class MixerPanel final : public juce::Component
{
public:
    explicit MixerPanel(engine::EngineController& controller);

    void pushMasterLevels(float peakLeft, float peakRight, double elapsedSeconds);

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void paintPlaceholderStrip(juce::Graphics& g, juce::Rectangle<int> area, std::size_t trackIndex) const;

    MasterStrip master_;
    juce::Rectangle<int> stripsArea_;
};

} // namespace dojo::ui
