#pragma once

#include "engine/ProcessorNode.h"
#include "engine/TrackTaps.h"
#include "session/Project.h"
#include "ui/EditorState.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <utility>
#include <vector>

namespace dojo::ui
{

/** What a device editor may need besides the processor. */
struct DeviceEditorContext
{
    session::Project& project;
    EditorState& editor;
    juce::ValueTree device; ///< the DEVICE tree
    juce::ValueTree owner;  ///< its track (or the MASTER node)
    engine::TrackTaps* taps = nullptr; ///< other tracks' sound for spectrum displays (none in tests)
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

/** The EQ editor's spectrum overlay, from outside (tests). */
namespace eqEditor
{
/** Overlays these tracks' spectra (as its OVERLAY menu does; kept on the device). */
void setOverlays(juce::Component& editor, const std::vector<std::uint32_t>& tracks);
/** Takes the newest sound; returns the names of the spectra over its own and its masking readout. */
std::pair<juce::StringArray, juce::String> inspect(juce::Component& editor);
} // namespace eqEditor

} // namespace dojo::ui
