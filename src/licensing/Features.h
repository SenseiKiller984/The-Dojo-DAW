#pragma once

#include "licensing/LicenseKey.h"

#include <juce_events/juce_events.h>

#include <cstdint>

/**
    What each license allows, and the switch the rest of The Dojo asks.

        No License   nothing: The Dojo is locked (the license window), the sound card silent
        Free Trial   everything but exporting; the output carries an audible watermark and is
                     muted while recording software runs or a virtual audio device is the output
        Basic        everything but surround, Dolby Atmos (3D beds, ADM objects, ADM BWF, the
                     external renderer) and the pro devices
        Pro          everything

    The application's LicenseManager publishes its license here (gate::publish); the UI greys out
    and refuses what it does not allow, and the exporter and device factory check it again. Where
    nothing has been published (unit tests that build UI parts on their own) everything is allowed.
*/
namespace dojo::licensing
{

enum class Feature : std::uint32_t
{
    use = 1u << 0,          ///< The Dojo is unlocked at all
    exporting = 1u << 1,    ///< exports of any kind: audio, stems, ADM BWF, video, clip files, MusicXML
    surround = 1u << 2,     ///< multichannel formats, the surround panner, surround monitoring
    immersive = 1u << 3,    ///< Dolby Atmos: 3D beds, ADM objects, ADM BWF, the external renderer, ADM-OSC
    proDevices = 1u << 4,   ///< Studer A800, Harrison 4032, EMI TG12345, The Glue, Rhodes Suitcase 73
    cleanOutput = 1u << 5,  ///< the sound card's output without the trial's watermark
};

[[nodiscard]] std::uint32_t featuresOf(Tier tier) noexcept;
[[nodiscard]] constexpr bool has(std::uint32_t features, Feature feature) noexcept
{
    return (features & static_cast<std::uint32_t>(feature)) != 0;
}

/** True for the device types only Pro (and the trial) may use. */
[[nodiscard]] bool isProDevice(const juce::String& deviceType);
/** The pro devices' type ids. */
[[nodiscard]] const juce::StringArray& getProDeviceTypes();

/** Why a feature is not available under a tier (for tooltips and messages). */
[[nodiscard]] juce::String whyUnavailable(Feature feature, Tier tier);

namespace gate
{
/** Sets what is allowed (the application's LicenseManager does; message thread). Broadcasts a change. */
void publish(Tier tier);
/** Whether anything was published (false: unit tests, everything allowed). */
[[nodiscard]] bool isPublished() noexcept;
[[nodiscard]] Tier tier() noexcept;
/** Whether a feature is allowed now (any thread). */
[[nodiscard]] bool allows(Feature feature) noexcept;
/** whyUnavailable() for the published tier. */
[[nodiscard]] juce::String why(Feature feature);
/** Tells listeners when the license changes (message thread). */
[[nodiscard]] juce::ChangeBroadcaster& changes();
/** Forgets what was published (tests). */
void reset();
} // namespace gate

} // namespace dojo::licensing
