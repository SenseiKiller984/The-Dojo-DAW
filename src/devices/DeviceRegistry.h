#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>
#include <vector>

namespace dojo::devices
{

/** A native device as offered in the browser. */
struct BuiltinDeviceInfo
{
    juce::String type; ///< "builtin:..." — stored in DEVICE trees
    juce::String name;
    juce::String category; ///< "Instrument", "Dynamics", "EQ", "Space", "Pitch"
    bool isInstrument = false;
    juce::String keywords = {}; ///< more words the browser's search finds it by
};

/** Every native device, in browser order. */
const std::vector<BuiltinDeviceInfo>& getBuiltinDevices();

/** The info for a type, or null. */
const BuiltinDeviceInfo* findBuiltinDevice(const juce::String& type);

/** True for "builtin:..." types. */
bool isBuiltinType(const juce::String& type);

/** Creates a native device (message thread). Null for unknown types. */
std::unique_ptr<juce::AudioProcessor> createBuiltinDevice(const juce::String& type);

} // namespace dojo::devices
