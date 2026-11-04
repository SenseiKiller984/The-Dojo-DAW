#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace dojo::ui
{

/** A flat button that draws a vector glyph. Used for transport and channel-strip controls. */
class IconButton final : public juce::Button
{
public:
    enum class Icon
    {
        play,
        pause,
        stop,
        record,
        loop,
        returnToStart,
    };

    IconButton(const juce::String& name, Icon icon, juce::Colour activeColour);

    void setIcon(Icon icon);
    [[nodiscard]] Icon getIcon() const noexcept { return icon_; }

    /** When "lit", the glyph and background take the active colour (e.g. while playing). */
    void setLit(bool lit);

    void paintButton(juce::Graphics& g, bool shouldDrawButtonAsHighlighted,
                     bool shouldDrawButtonAsDown) override;

    static juce::Path createGlyph(Icon icon, juce::Rectangle<float> area);

private:
    Icon icon_;
    juce::Colour activeColour_;
    bool lit_ = false;
};

} // namespace dojo::ui
