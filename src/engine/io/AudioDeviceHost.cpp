#include "engine/io/AudioDeviceHost.h"

#include "engine/AudioEngine.h"

namespace dojo::engine
{

//==================================================================================================
/** Adapter between JUCE's driver callback interface and the engine. Kept trivially thin so that
    nothing but AudioEngine::process() ever runs on the audio thread. */
class AudioDeviceHost::EngineCallback final : public juce::AudioIODeviceCallback
{
public:
    explicit EngineCallback(AudioEngine& engine)
        : engine_(engine)
    {
    }

    // Called (on a non-audio thread) before the first callback and after every device restart.
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override
    {
        jassert(device != nullptr);
        errorReported_.store(false, std::memory_order_relaxed);
        engine_.prepare(device->getCurrentSampleRate(), device->getCurrentBufferSizeSamples(),
                        device->getActiveOutputChannels().countNumberOfSetBits());
    }

    // ===> The driver's real-time audio thread enters The Dojo here. <===
    void audioDeviceIOCallbackWithContext(const float* const* inputChannelData, int numInputChannels,
                                          float* const* outputChannelData, int numOutputChannels,
                                          int numSamples,
                                          const juce::AudioIODeviceCallbackContext& /*context*/) override
    {
        engine_.process(inputChannelData, numInputChannels, outputChannelData, numOutputChannels, numSamples);
    }

    void audioDeviceStopped() override { engine_.release(); }

    // May be invoked on any thread (possibly the audio thread), so only touch an atomic here.
    void audioDeviceError(const juce::String& /*errorMessage*/) override
    {
        errorReported_.store(true, std::memory_order_relaxed);
    }

    [[nodiscard]] bool hasReportedError() const noexcept
    {
        return errorReported_.load(std::memory_order_relaxed);
    }

private:
    AudioEngine& engine_;
    std::atomic<bool> errorReported_{false};
};

//==================================================================================================
AudioDeviceHost::AudioDeviceHost(AudioEngine& engine)
    : engine_(engine)
    , callback_(std::make_unique<EngineCallback>(engine))
{
}

AudioDeviceHost::~AudioDeviceHost()
{
    closeDevice();
}

juce::String AudioDeviceHost::openDevice(const juce::XmlElement* savedState,
                                         const juce::String& preferredDeviceType)
{
    constexpr int kNumInputChannels = 0; // inputs (recording/monitoring) are wired up in Phase 2
    constexpr int kNumOutputChannels = 2;

    // JUCE only auto-selects device *names* when initialising without saved state, so when a specific
    // back-end is requested we resolve that back-end's default output device ourselves and hand the
    // manager a complete setup. If the back-end is missing or has no devices we fall back to the
    // platform default rather than failing.
    std::unique_ptr<juce::XmlElement> requestedSetup;

    if (preferredDeviceType.isEmpty() && savedState != nullptr)
        requestedSetup = std::make_unique<juce::XmlElement>(*savedState); // the user's last choice

    if (preferredDeviceType.isNotEmpty())
    {
        for (auto* type : deviceManager_.getAvailableDeviceTypes())
        {
            if (!type->getTypeName().equalsIgnoreCase(preferredDeviceType))
                continue;

            const auto outputs = type->getDeviceNames(false);

            if (!outputs.isEmpty())
            {
                requestedSetup = std::make_unique<juce::XmlElement>("DEVICESETUP");
                requestedSetup->setAttribute("deviceType", type->getTypeName());
                requestedSetup->setAttribute("audioOutputDeviceName",
                                             outputs[juce::jmax(0, type->getDefaultDeviceIndex(false))]);
            }
        }

        if (requestedSetup == nullptr)
            juce::Logger::writeToLog("Requested audio back-end '" + preferredDeviceType +
                                     "' is unavailable or has no output devices; using the default.");
    }

    // selectDefaultDeviceOnFailure: a saved device that has since been unplugged falls back to the default.
    lastError_ = deviceManager_.initialise(kNumInputChannels, kNumOutputChannels, requestedSetup.get(), true);

    if (lastError_.isEmpty() && deviceManager_.getCurrentAudioDevice() == nullptr)
        lastError_ = "No audio output device is available";

    // Registering the callback triggers audioDeviceAboutToStart() -> AudioEngine::prepare() if a device
    // is open. If none is, the engine simply stays unprepared until one appears.
    deviceManager_.addAudioCallback(callback_.get());

    return lastError_;
}

std::unique_ptr<juce::XmlElement> AudioDeviceHost::createStateXml() const
{
    return deviceManager_.createStateXml();
}

void AudioDeviceHost::closeDevice()
{
    deviceManager_.removeAudioCallback(callback_.get());
    deviceManager_.closeAudioDevice();
    engine_.release();
}

AudioDeviceHost::Status AudioDeviceHost::getStatus() const
{
    Status status;
    status.lastError = lastError_;
    status.driverReportedError = callback_->hasReportedError();

    if (auto* device = deviceManager_.getCurrentAudioDevice(); device != nullptr && device->isOpen())
    {
        status.isOpen = true;
        status.deviceType = device->getTypeName();
        status.deviceName = device->getName();
        status.sampleRate = device->getCurrentSampleRate();
        status.bufferSizeSamples = device->getCurrentBufferSizeSamples();
        status.outputLatencySamples = device->getOutputLatencyInSamples();
        status.xrunCount = device->getXRunCount();
    }

    return status;
}

juce::StringArray AudioDeviceHost::getAvailableDeviceTypeNames()
{
    juce::StringArray names;

    for (auto* type : deviceManager_.getAvailableDeviceTypes())
        names.add(type->getTypeName());

    return names;
}

} // namespace dojo::engine
