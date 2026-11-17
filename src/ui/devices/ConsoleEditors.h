#pragma once

#include "ui/devices/ConsoleLookAndFeel.h"
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
    The EMI TG12345's editor, looking like the desk: grey hammertone metalwork in modules, black
    pointer knobs, grey push buttons lit red, black legend plates (see ConsoleLookAndFeel). The
    controls of the cassette it is set to come in pages (rebuilt when the cassette, the model or the
    input changes), each beside the cassette's face: its row of white-faced meters (output L/R in
    dBu, the compressor's gain reduction or the main outputs' correlation), the TG12345 nameplate,
    its signal path lit where it acts, its lamps, and its fader (black slot, white cap; the main
    mix's cap red).
*/
class ConsoleEditor final : public juce::Component, private juce::Timer
{
public:
    explicit ConsoleEditor(devices::ConsoleTG12345& console);
    ~ConsoleEditor() override;

    /** The pages for a cassette (Microphone: on a Mk I or Mk II, from a stereo pair or mono). */
    [[nodiscard]] static std::vector<PagedEditor::Page> pagesFor(devices::ConsoleTG12345& console, int cassette, bool markTwo, bool stereo);
    /** Wide enough for every cassette's pages. */
    [[nodiscard]] static int preferredWidth(devices::ConsoleTG12345& console);

    /** The level the face's fader works for a cassette (a parameter ID). */
    [[nodiscard]] static juce::String faderFor(int cassette);

    [[nodiscard]] PagedEditor& getPages() noexcept { return *pages_; }
    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    void timerCallback() override;
    void rebuild();

    devices::ConsoleTG12345& console_;
    ConsoleLookAndFeel look_{ConsoleLookAndFeel::Style::emi};
    std::unique_ptr<PagedEditor> pages_;
    int cassette_ = -1, mark_ = -1, input_ = -1;
};

/** The console's editor, or null for any other processor. */
std::unique_ptr<juce::Component> createConsoleDeviceEditor(juce::AudioProcessor& processor, int& preferredWidth);

} // namespace dojo::ui
