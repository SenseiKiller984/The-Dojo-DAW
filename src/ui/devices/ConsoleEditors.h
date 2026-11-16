#pragma once

#include "ui/devices/PagedEditor.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

namespace dojo::devices
{
class ConsoleTG12345;
}

namespace dojo::ui
{

/**
    The EMI TG12345's editor: the controls of the cassette it is set to, in pages (rebuilt when the
    cassette, the model or the input changes), each beside the console's display: the cassette's
    signal path, its output meters, the compressor's gain reduction and its lamps.
*/
class ConsoleEditor final : public juce::Component, private juce::Timer
{
public:
    explicit ConsoleEditor(devices::ConsoleTG12345& console);

    /** The pages for a cassette (Microphone: on a Mk I or Mk II, from a stereo pair or mono). */
    [[nodiscard]] static std::vector<PagedEditor::Page> pagesFor(devices::ConsoleTG12345& console, int cassette, bool markTwo, bool stereo);
    /** Wide enough for every cassette's pages. */
    [[nodiscard]] static int preferredWidth(devices::ConsoleTG12345& console);

    [[nodiscard]] PagedEditor& getPages() noexcept { return *pages_; }
    void resized() override;

private:
    void timerCallback() override;
    void rebuild();

    devices::ConsoleTG12345& console_;
    std::unique_ptr<PagedEditor> pages_;
    int cassette_ = -1, mark_ = -1, input_ = -1;
};

/** The console's editor, or null for any other processor. */
std::unique_ptr<juce::Component> createConsoleDeviceEditor(juce::AudioProcessor& processor, int& preferredWidth);

} // namespace dojo::ui
