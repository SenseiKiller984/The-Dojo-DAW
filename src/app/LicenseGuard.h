#pragma once

#include "engine/io/AudioDeviceHost.h"
#include "licensing/LicenseManager.h"

#include <juce_events/juce_events.h>

#include <functional>

namespace dojo::app
{

/**
    Keeps what the sound card plays in line with the license (engine::OutputGuard): silent without a
    license, clean with Basic or Pro, watermarked in the free trial, and silent in the trial while a
    recording program runs (OBS, Voicemeeter...) or a virtual audio device is the output. Looks at
    the running programs every two seconds in the trial, and asks the license again every minute (the
    trial's end, the clock). Message thread only.
*/
class LicenseGuard final : private juce::Timer, private juce::ChangeListener
{
public:
    LicenseGuard(licensing::LicenseManager& license, engine::AudioDeviceHost& deviceHost, std::function<void()> onLicenseChanged);
    ~LicenseGuard() override;

    /** The license in a few words for the status bar. */
    [[nodiscard]] juce::String describe() const;
    /** Recording programs seen running (the trial mutes while there are any). */
    [[nodiscard]] const juce::StringArray& getRecordingSoftware() const noexcept { return recorders_; }

    /** Looks at the programs and the output device now, and applies the result. */
    void check();

private:
    void timerCallback() override;
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;
    void apply();

    licensing::LicenseManager& license_;
    engine::AudioDeviceHost& deviceHost_;
    std::function<void()> onLicenseChanged_;
    juce::StringArray recorders_;
    juce::String virtualDevice_; ///< the output device's name when it is a virtual one
    int ticks_ = 0;
};

} // namespace dojo::app
