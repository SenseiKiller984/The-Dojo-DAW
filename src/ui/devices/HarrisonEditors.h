#pragma once

#include "ui/devices/ConsoleLookAndFeel.h"
#include "ui/devices/DeviceEditors.h"
#include "ui/devices/PagedEditor.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

namespace dojo::devices
{
class Harrison4032;
}

namespace dojo::ui
{

namespace harrison
{
/** Assigns the I/O module to bus `bus` (1 - 32), or not, and keeps the bus pair's device send to the
    DAW bus "Bus <odd>-<even>" in step: made with the pair's first assignment, removed with its last. */
void assignBus(session::Session& session, const juce::ValueTree& device, devices::Harrison4032& console, int bus, bool assigned);

/** One of the I/O module's other outputs (Aux 1 - 4, Cue 1 - 2, Quad 3 - 4, Direct): its channels,
    and the bus it connects to. */
struct Connection
{
    juce::String label;
    int left = 0, right = 0;
    juce::String busName;
};
[[nodiscard]] std::vector<Connection> connections(const juce::ValueTree& device, const juce::ValueTree& track);
} // namespace harrison

/**
    The Harrison 4032's editor, looking like the desk: a black panel between walnut end cheeks, a
    leather armrest along the front, grey knobs with coloured caps and illuminated switches (see
    ConsoleLookAndFeel). The controls of the module and revision it is set to come in pages (rebuilt
    when the module, the revision, the format or the pan mode changes). Beside them: the wooden meter
    bridge with the outputs' cream VU meters and the revision's badge, the revision's circuits, the EQ
    and filter curve, the lamps, and the long fader. The Routing page has the 32 bus switches, the Aux
    and Cue pages connect their sends to busses, and the Master Section picks its monitor source.
*/
class HarrisonEditor final : public juce::Component, private juce::Timer
{
public:
    static constexpr int kCheekWidth = 10; ///< the walnut end cheeks
    static constexpr int kArmrestHeight = 14;

    HarrisonEditor(devices::Harrison4032& console, const DeviceEditorContext& context);
    ~HarrisonEditor() override;

    [[nodiscard]] static std::vector<PagedEditor::Page> pagesFor(devices::Harrison4032& console, const DeviceEditorContext& context,
                                                                 int module, int revision, bool mono, int panMode);
    [[nodiscard]] static int preferredWidth(devices::Harrison4032& console, const DeviceEditorContext& context);

    [[nodiscard]] PagedEditor& getPages() noexcept { return *pages_; }
    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    void timerCallback() override;
    void rebuild();
    /** The armrest's height: what the controls leave of the editor's height, up to kArmrestHeight. */
    [[nodiscard]] int armrestHeight() const noexcept;

    devices::Harrison4032& console_;
    DeviceEditorContext context_;
    ConsoleLookAndFeel look_{ConsoleLookAndFeel::Style::harrison};
    std::unique_ptr<PagedEditor> pages_;
    std::array<int, 4> shown_{-1, -1, -1, -1};
};

/** The console's editor, or null for any other processor. */
std::unique_ptr<juce::Component> createHarrisonDeviceEditor(juce::AudioProcessor& processor, const DeviceEditorContext& context,
                                                            int& preferredWidth);

} // namespace dojo::ui
