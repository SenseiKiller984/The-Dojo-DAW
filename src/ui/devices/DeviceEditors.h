#pragma once

#include "engine/ProcessorNode.h"
#include "session/Project.h"
#include "ui/EditorState.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace dojo::ui
{

/** What a device editor may need besides the processor. */
struct DeviceEditorContext
{
    session::Project& project;
    EditorState& editor;
    juce::ValueTree device; ///< the DEVICE tree
    juce::ValueTree owner;  ///< its track (or the MASTER node)
};

/**
    Creates the editor shown inside a device's card in the Devices panel.

    Native devices get dedicated editors: EQ (response curve, analyser, draggable bands), compressor
    and limiter (gain-reduction meters, transfer curve), Sampler (waveform with start/end, ADSR,
    drop a file to load) and Drum Rack (4x4 pads with drag-and-drop, audition, per-pad settings,
    return tracks for its outputs). Everything else, including hosted plug-ins, gets a grid of its
    automatable parameters. `preferredWidth` receives the width the editor would like.
*/
std::unique_ptr<juce::Component> createDeviceEditor(engine::ProcessorNode& node, const DeviceEditorContext& context,
                                                    int& preferredWidth);

} // namespace dojo::ui
