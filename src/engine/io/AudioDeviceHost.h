#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

#include <atomic>
#include <cstdint>
#include <memory>

namespace dojo::engine
{

class AudioEngine;

/**
    Connects the AudioEngine to an audio driver.

    Owns the juce::AudioDeviceManager and installs a thin juce::AudioIODeviceCallback adapter whose
    audioDeviceIOCallbackWithContext() is the driver's high-priority audio thread entering The Dojo.

    Back-ends compiled in:
      Windows : ASIO, WASAPI (shared + exclusive), DirectSound
      Linux   : ALSA (incl. the PulseAudio/PipeWire ALSA PCM plug-ins), JACK (incl. PipeWire-JACK)

    Driver/device/sample-rate/buffer/input-channel selection is done with JUCE's
    AudioDeviceSelectorComponent on this host's AudioDeviceManager (Options > Audio Settings) and
    persisted by the application.

    MIDI: every MIDI input is enabled and feeds the engine's live MIDI queue. Devices plugged in
    while the application runs are enabled automatically (hot-plug).

    All functions are message-thread only.
*/
class AudioDeviceHost final
{
public:
    struct Status
    {
        bool isOpen = false;
        juce::String deviceType; ///< e.g. "ALSA", "JACK", "ASIO", "Windows Audio"
        juce::String deviceName;
        double sampleRate = 0.0;
        int bufferSizeSamples = 0;
        int outputLatencySamples = 0;
        int inputLatencySamples = 0;
        int numInputChannels = 0;
        int xrunCount = -1; ///< -1 when the driver can't report dropouts
        bool driverReportedError = false;
        juce::String lastError;
    };

    explicit AudioDeviceHost(AudioEngine& engine);
    ~AudioDeviceHost();

    AudioDeviceHost(const AudioDeviceHost&) = delete;
    AudioDeviceHost& operator=(const AudioDeviceHost&) = delete;

    /** Opens a stereo output: the device described by `savedState` (from createStateXml(), e.g.
        restored from the user's settings) if given, otherwise the system default. If
        `preferredDeviceType` names a compiled-in back-end ("JACK", "ASIO", ...), that back-end's
        default device is used instead. Returns an empty string on success, otherwise a
        human-readable error. Failure is non-fatal: the application keeps running without audio. */
    juce::String openDevice(const juce::XmlElement* savedState = nullptr,
                            const juce::String& preferredDeviceType = {});

    /** The current device configuration, for persisting across launches (null if none was chosen). */
    [[nodiscard]] std::unique_ptr<juce::XmlElement> createStateXml() const;

    /** Stops the audio callback and closes the device. */
    void closeDevice();

    [[nodiscard]] Status getStatus() const;

    /** Names of every back-end compiled into this build, in the manager's priority order. */
    [[nodiscard]] juce::StringArray getAvailableDeviceTypeNames();

    [[nodiscard]] juce::AudioDeviceManager& getDeviceManager() noexcept { return deviceManager_; }

    /** Names of the active input channels, in engine input order (for track input selectors). */
    [[nodiscard]] juce::StringArray getActiveInputChannelNames() const;

    /** Names of the enabled MIDI inputs. */
    [[nodiscard]] juce::StringArray getMidiInputNames() const;

    /** Number of MIDI messages received so far (any thread). */
    [[nodiscard]] std::uint64_t getMidiMessageCount() const noexcept;

    /** Enables every MIDI input that has appeared since the last call (hot-plug). */
    void refreshMidiInputs();

private:
    class EngineCallback;
    class MidiCallback;

    AudioEngine& engine_;
    std::unique_ptr<EngineCallback> callback_; // declared before the manager so it outlives it
    std::unique_ptr<MidiCallback> midiCallback_;
    juce::AudioDeviceManager deviceManager_;
    juce::String lastError_;
    juce::StringArray knownMidiInputs_;
    juce::MidiDeviceListConnection midiDeviceListConnection_;
};

} // namespace dojo::engine
