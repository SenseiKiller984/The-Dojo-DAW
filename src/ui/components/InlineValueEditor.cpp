#include "ui/components/InlineValueEditor.h"

#include "ui/theme/DojoTheme.h"

namespace dojo::ui
{

InlineValueEditor::InlineValueEditor(juce::Component& owner)
    : owner_(owner)
{
}

InlineValueEditor::~InlineValueEditor()
{
    if (editor_ != nullptr) // deleting a focused field must not apply it to a dying owner
    {
        editor_->onFocusLost = nullptr;
        editor_->onReturnKey = nullptr;
        editor_->onEscapeKey = nullptr;
    }
}

void InlineValueEditor::open(juce::Rectangle<int> bounds, const juce::String& text, std::function<void(const juce::String&)> apply)
{
    if (editor_ == nullptr)
    {
        editor_ = std::make_unique<juce::TextEditor>();
        editor_->setFont(theme::mono(10.5f));
        editor_->setJustification(juce::Justification::centred);
        editor_->setIndents(2, 1);
        editor_->setSelectAllWhenFocused(true);
        editor_->setColour(juce::TextEditor::backgroundColourId, theme::colour::meterBackground);
        editor_->setColour(juce::TextEditor::outlineColourId, theme::colour::accent);
        editor_->setColour(juce::TextEditor::focusedOutlineColourId, theme::colour::accent);
        editor_->setColour(juce::TextEditor::textColourId, theme::colour::textPrimary);
        editor_->setColour(juce::TextEditor::highlightColourId, theme::colour::accent.withAlpha(0.45f));
        editor_->setColour(juce::CaretComponent::caretColourId, theme::colour::textPrimary);
        editor_->onReturnKey = [this] { finish(true); };
        editor_->onEscapeKey = [this] { finish(false); };
        editor_->onFocusLost = [this] { finish(true); };
        owner_.addChildComponent(*editor_);
    }

    apply_ = std::move(apply);
    editor_->setText(text, juce::dontSendNotification);
    editor_->setBounds(bounds);
    editor_->setVisible(true);
    editor_->toFront(false);
    editor_->selectAll();
    if (owner_.isShowing())
        editor_->grabKeyboardFocus();
}

juce::TextEditor* InlineValueEditor::getEditor() const noexcept
{
    return editor_ != nullptr && editor_->isVisible() ? editor_.get() : nullptr;
}

void InlineValueEditor::finish(bool apply)
{
    if (getEditor() == nullptr)
        return; // already finished (hiding the field also makes it lose focus)
    const auto text = editor_->getText().trim();
    editor_->setVisible(false);
    if (apply && text.isNotEmpty() && apply_)
        apply_(text);
}

} // namespace dojo::ui
