#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace dojo::ui
{

/**
    A small text field opened over a value shown in a component (double-click a value to type it).
    Return applies the text, Escape cancels, clicking elsewhere applies it.
*/
class InlineValueEditor final
{
public:
    explicit InlineValueEditor(juce::Component& owner);
    ~InlineValueEditor();
    InlineValueEditor(const InlineValueEditor&) = delete;
    InlineValueEditor& operator=(const InlineValueEditor&) = delete;

    /** Opens over `bounds` (in the owner) with `text` selected; `apply` receives what was typed. */
    void open(juce::Rectangle<int> bounds, const juce::String& text, std::function<void(const juce::String&)> apply);
    /** The open field, or null. */
    [[nodiscard]] juce::TextEditor* getEditor() const noexcept;
    /** Closes the field, applying it or not. */
    void finish(bool apply);

private:
    juce::Component& owner_;
    std::unique_ptr<juce::TextEditor> editor_;
    std::function<void(const juce::String&)> apply_;
};

} // namespace dojo::ui
