#include "app/LicenseGuard.h"

#include "licensing/CaptureGuard.h"
#include "licensing/Integrity.h"

namespace dojo::app
{
namespace
{
constexpr int kTickMs = 2000;
constexpr int kRefreshTicks = 30; // a minute
} // namespace

LicenseGuard::LicenseGuard(licensing::LicenseManager& license, engine::AudioDeviceHost& deviceHost, std::function<void()> onLicenseChanged)
    : license_(license)
    , deviceHost_(deviceHost)
    , onLicenseChanged_(std::move(onLicenseChanged))
{
    license_.addChangeListener(this);
    deviceHost_.getDeviceManager().addChangeListener(this);
    check();
    startTimer(kTickMs);
}

LicenseGuard::~LicenseGuard()
{
    stopTimer();
    deviceHost_.getDeviceManager().removeChangeListener(this);
    license_.removeChangeListener(this);
    license_.refresh(); // (records the time seen, for the trial)
}

void LicenseGuard::check()
{
    const bool trial = license_.getTier() == licensing::Tier::demo;
    recorders_ = trial ? licensing::capture::findRecordingSoftware() : juce::StringArray();
    if (trial && licensing::integrity::isDebuggerPresent())
        recorders_.addIfNotAlreadyThere("a debugger");
    const auto device = deviceHost_.getStatus();
    virtualDevice_ = trial && device.isOpen && licensing::capture::isVirtualAudioDevice(device.deviceName) ? device.deviceName : juce::String();
    apply();
}

void LicenseGuard::apply()
{
    using Policy = engine::AudioDeviceHost::OutputPolicy;
    auto policy = Policy::mute;
    switch (license_.getTier())
    {
    case licensing::Tier::basic:
    case licensing::Tier::pro:
        policy = Policy::clean;
        break;
    case licensing::Tier::demo:
        policy = recorders_.isEmpty() && virtualDevice_.isEmpty() ? Policy::clean : Policy::mute;
        break;
    case licensing::Tier::none:
        break;
    }
    deviceHost_.setOutputPolicy(policy);
}

juce::String LicenseGuard::describe() const
{
    switch (license_.getTier())
    {
    case licensing::Tier::pro: return "PRO";
    case licensing::Tier::basic: return "BASIC";
    case licensing::Tier::demo:
    {
        if (!recorders_.isEmpty())
            return "TRIAL: MUTED WHILE " + recorders_.joinIntoString(", ").toUpperCase() + " RUNS";
        if (virtualDevice_.isNotEmpty())
            return "TRIAL: MUTED ON A VIRTUAL AUDIO DEVICE (" + virtualDevice_.toUpperCase() + ")";
        const int days = license_.getTrialDaysLeft();
        return "TRIAL: " + juce::String(days) + (days == 1 ? " DAY" : " DAYS") + " LEFT, NO EXPORT";
    }
    case licensing::Tier::none: break;
    }
    return "NO LICENSE";
}

void LicenseGuard::timerCallback()
{
    if (++ticks_ >= kRefreshTicks)
    {
        ticks_ = 0;
        license_.refresh(); // (a change comes back through changeListenerCallback)
    }
    if (license_.getTier() == licensing::Tier::demo)
    {
        const auto before = recorders_;
        const auto deviceBefore = virtualDevice_;
        check();
        if (before != recorders_ || deviceBefore != virtualDevice_)
            if (onLicenseChanged_)
                onLicenseChanged_(); // (the status bar says why it is muted)
    }
}

void LicenseGuard::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    check();
    if (source == &license_ && onLicenseChanged_)
        onLicenseChanged_();
}

} // namespace dojo::app
