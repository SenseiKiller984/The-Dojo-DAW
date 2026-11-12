#pragma once

#include "session/Session.h"
#include "ui/EditActions.h"
#include "ui/EditorState.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <vector>

namespace dojo::ui
{

/**
    The Clip List (bottom panel, CLIPS tab, F6): the clips captured with Clip > Capture and the clips
    renamed with Clip > Rename (user-defined clips), and, when "Auto-Created" is on, every other clip
    made by editing. Drag a row onto a track to place a copy of it there; double-click (or the
    right-click menu) renames it; captured clips can be removed from the list.
*/
class ClipListPanel final : public juce::Component,
                            private juce::ListBoxModel,
                            private juce::ValueTree::Listener,
                            private juce::AsyncUpdater
{
public:
    /** Drag descriptions: this prefix and the clip's (or entry's) id. */
    static constexpr const char* kDragPrefix = "DojoClipList:";

    ClipListPanel(session::Session& session, EditorState& editor);
    ~ClipListPanel() override;

    [[nodiscard]] const std::vector<actions::ClipListItem>& getItems() const noexcept { return items_; }
    [[nodiscard]] bool isShowingAutoCreated() const noexcept { return autoCreated_.getToggleState(); }
    void setShowingAutoCreated(bool show);
    /** Re-reads the list from the session (done by itself when the session changes). */
    void refresh();

    [[nodiscard]] static juce::var describeDrag(const juce::ValueTree& clip);
    /** The Clip List entry or track clip a drag description names (invalid if none). */
    [[nodiscard]] static juce::ValueTree findDragged(const session::Session& session, const juce::var& description);

    /** Clip List > Rename: asks for the name (TextPromptPanel). */
    void renameItem(int row);
    /** Removes a captured clip from the list (one undo step). */
    void removeItem(int row);

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    int getNumRows() override { return static_cast<int>(items_.size()); }
    void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override;
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override;
    void listBoxItemClicked(int row, const juce::MouseEvent& e) override;
    juce::var getDragSourceDescription(const juce::SparseSet<int>& rows) override;

    void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier&) override { triggerAsyncUpdate(); }
    void valueTreeChildAdded(juce::ValueTree&, juce::ValueTree&) override { triggerAsyncUpdate(); }
    void valueTreeChildRemoved(juce::ValueTree&, juce::ValueTree&, int) override { triggerAsyncUpdate(); }
    void valueTreeRedirected(juce::ValueTree&) override { triggerAsyncUpdate(); }
    void handleAsyncUpdate() override { refresh(); }

    session::Session& session_;
    EditorState& editor_;
    std::vector<actions::ClipListItem> items_;
    juce::Label title_;
    juce::ToggleButton autoCreated_{"Auto-Created"};
    juce::ListBox list_;
};

} // namespace dojo::ui
