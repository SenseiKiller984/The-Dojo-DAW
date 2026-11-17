#pragma once

#include "engine/io/AudioDeviceHost.h"
#include "ui/surfaces/ControlSurfaces.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <map>
#include <memory>

namespace dojo::ui::surfaces
{

/**
    The application's way to the surfaces' MIDI ports: an input is taken from the instruments by the
    device host (AudioDeviceHost::routeMidiInput), an output is opened as a juce::MidiOutput and sent
    to from the message thread. Outputs stay open while this exists.
*/
class DevicePorts final : public Ports
{
public:
    explicit DevicePorts(engine::AudioDeviceHost& host)
        : host_(host)
    {
    }

    bool openInput(const juce::String& identifier, std::function<void(const juce::MidiMessage&)> receive) override
    {
        return host_.routeMidiInput(identifier, std::move(receive));
    }

    void closeInput(const juce::String& identifier) override { (void) host_.routeMidiInput(identifier, {}); }

    std::function<void(const juce::MidiMessage&)> openOutput(const juce::String& identifier) override
    {
        auto& output = outputs_[identifier];
        if (output == nullptr)
            output = juce::MidiOutput::openDevice(identifier);
        if (output == nullptr)
        {
            outputs_.erase(identifier);
            return {};
        }
        auto* device = output.get();
        return [device](const juce::MidiMessage& message) { device->sendMessageNow(message); };
    }

private:
    engine::AudioDeviceHost& host_;
    std::map<juce::String, std::unique_ptr<juce::MidiOutput>> outputs_;
};

} // namespace dojo::ui::surfaces
