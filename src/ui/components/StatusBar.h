#pragma once

#include "engine/io/AudioDeviceHost.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>

namespace dojo::ui
{

/** Bottom status bar: audio device summary on the left, engine health on the right. */
class StatusBar final : public juce::Component
{
public:
    struct Info
    {
        engine::AudioDeviceHost::Status device;
        float dspLoadPeak = 0.0f;
        std::uint64_t realtimeViolations = 0;
        bool realtimeTrapInstalled = false;
        std::uint64_t clippedSamples = 0;
        std::size_t parkedCommands = 0;
    };

    StatusBar();

    void setInfo(const Info& info);
    void paint(juce::Graphics& g) override;

private:
    [[nodiscard]] juce::String describeDevice() const;
    [[nodiscard]] juce::String describeEngine() const;

    Info info_;
    juce::String deviceText_;
    juce::String engineText_;
};

} // namespace dojo::ui
