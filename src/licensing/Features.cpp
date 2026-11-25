#include "licensing/Features.h"

#include <atomic>

namespace dojo::licensing
{
namespace
{
constexpr std::uint32_t bit(Feature f) noexcept { return static_cast<std::uint32_t>(f); }

std::atomic<std::uint32_t> publishedFeatures{0};
std::atomic<int> publishedTier{-1}; // -1: nothing published

juce::ChangeBroadcaster& broadcaster()
{
    static juce::ChangeBroadcaster instance;
    return instance;
}
} // namespace

std::uint32_t featuresOf(Tier tier) noexcept
{
    switch (tier)
    {
    case Tier::demo:
        return bit(Feature::use) | bit(Feature::surround) | bit(Feature::immersive) | bit(Feature::proDevices);
    case Tier::basic:
        return bit(Feature::use) | bit(Feature::exporting) | bit(Feature::cleanOutput);
    case Tier::pro:
        return bit(Feature::use) | bit(Feature::exporting) | bit(Feature::surround) | bit(Feature::immersive)
               | bit(Feature::proDevices) | bit(Feature::cleanOutput);
    case Tier::none:
        break;
    }
    return 0;
}

const juce::StringArray& getProDeviceTypes()
{
    static const juce::StringArray types{"builtin:tape800", "builtin:harrison4032", "builtin:tg12345", "builtin:gluecomp",
                                         "builtin:rhodes73"};
    return types;
}

bool isProDevice(const juce::String& deviceType)
{
    return getProDeviceTypes().contains(deviceType);
}

juce::String whyUnavailable(Feature feature, Tier tier)
{
    if (tier == Tier::none)
        return "The Dojo needs a license key.";
    switch (feature)
    {
    case Feature::exporting:
        return tier == Tier::demo ? "Exporting is not part of the free trial: it needs a Basic or Pro license."
                                  : "Exporting needs a Basic or Pro license.";
    case Feature::surround: return "Surround sound needs a Pro license.";
    case Feature::immersive: return "Dolby Atmos (3D formats, ADM objects and ADM BWF) needs a Pro license.";
    case Feature::proDevices: return "This device is one of the pro devices: it needs a Pro license.";
    case Feature::cleanOutput: return "The free trial's output carries an audible watermark.";
    case Feature::use: break;
    }
    return "The Dojo needs a license key.";
}

namespace gate
{
void publish(Tier tier)
{
    const int previous = publishedTier.exchange(static_cast<int>(tier));
    publishedFeatures.store(featuresOf(tier));
    if (previous != static_cast<int>(tier))
        broadcaster().sendChangeMessage();
}

bool isPublished() noexcept
{
    return publishedTier.load() >= 0;
}

Tier tier() noexcept
{
    const int t = publishedTier.load();
    return t < 0 ? Tier::pro : static_cast<Tier>(t);
}

bool allows(Feature feature) noexcept
{
    if (!isPublished())
        return true;
    return has(publishedFeatures.load(), feature);
}

juce::String why(Feature feature)
{
    return whyUnavailable(feature, tier());
}

juce::ChangeBroadcaster& changes()
{
    return broadcaster();
}

void reset()
{
    publishedTier.store(-1);
    publishedFeatures.store(0);
}
} // namespace gate

} // namespace dojo::licensing
