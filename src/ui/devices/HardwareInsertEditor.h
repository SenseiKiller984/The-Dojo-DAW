#pragma once

#include "ui/devices/DeviceEditors.h"

namespace dojo::devices
{
class HardwareInsert;
}

namespace dojo::ui
{

/**
    The HW Insert's editor: the sound card's channels to send to and return from (by name, those
    the sound card lacks marked), the Send and Return levels with their meters, Phase, Dry/Wet,
    and the latency panel: the round trip in samples and milliseconds, where it came from
    (measured, or the driver's figure until then), the last measurement's correlation with its
    peak, PING, Auto Ping, MATCH (Return for unity through the loop), Offset and the ping's level.
*/
std::unique_ptr<juce::Component> createHardwareInsertEditor(devices::HardwareInsert& insert, const DeviceEditorContext& context,
                                                            int& preferredWidth);

} // namespace dojo::ui
