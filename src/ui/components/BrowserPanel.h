#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace dojo::ui
{

/** Left-hand browser: native instruments, native effects, plug-ins and the sample library.
    Phase 1 shows the catalogue; drag-and-drop onto tracks arrives with the arranger. */
class BrowserPanel final : public juce::Component
{
public:
    BrowserPanel();

    void paint(juce::Graphics& g) override;
};

} // namespace dojo::ui
