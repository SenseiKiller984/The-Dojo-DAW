#pragma once

#include "ui/devices/DeviceEditors.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace dojo::ui
{

/** Editors of the keyboard instruments: the Rhodes Suitcase. Null for any other processor. */
std::unique_ptr<juce::Component> createKeysDeviceEditor(juce::AudioProcessor& processor, const DeviceEditorContext& context, int& preferredWidth);

} // namespace dojo::ui
