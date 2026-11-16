#pragma once

#include "ui/devices/DeviceEditors.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace dojo::devices
{
class RhodesSuitcase;
}

namespace dojo::ui
{

/** Editors of the keyboard instruments' devices: the Rhodes Suitcase and its Suitcase Base. Null
    for any other processor. */
std::unique_ptr<juce::Component> createKeysDeviceEditor(juce::AudioProcessor& processor, const DeviceEditorContext& context, int& preferredWidth);

/** Whether the track of a Rhodes Suitcase (its DEVICE tree) has a Suitcase Base after it. */
[[nodiscard]] bool hasSuitcaseBaseAfter(const juce::ValueTree& rhodesDevice);

/** Adds a Suitcase Base at the end of the Rhodes Suitcase's track, set up like the Rhodes' own
    suitcase (one undo step). Returns its DEVICE tree. */
juce::ValueTree addSuitcaseBase(session::Project& project, const juce::ValueTree& rhodesDevice, devices::RhodesSuitcase& rhodes);

} // namespace dojo::ui
