#pragma once

#include "session/Session.h"
#include "ui/ClipList.h"
#include "ui/EditorState.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

namespace dojo::ui
{

/**
    The Clip List (bottom panel, CLIPS tab, F6). Its menu (the CLIP LIST button, or a right-click)
    has:
      Show      the kinds of clips listed (Audio, MIDI, Video, Groups, Auto-Created) and the
                information shown for each (Color, File Type, Timebase, Processing State, File Name,
                Disk Name)
      Select    All, Unused, Unused Except Whole Files, Offline
      Sort By   clip type, name, length, time stamps, timebase, start/end in parent, file name,
                length and dates, disk, track format, channel name, scene and take; ascending or
                descending
      Rename..., Auto Rename..., Time Stamp..., Export Clips as Files..., Remove from Clip List
    Whole-file clips are bold; offline clips (their file cannot be found) are italic and dimmed.
    Drag rows (several at once) onto a track to place them there one after the other.
*/
class ClipListPanel final : public juce::Component,
                            private juce::ListBoxModel,
                            private juce::ValueTree::Listener,
                            private juce::AsyncUpdater
{
public:
    ClipListPanel(session::Session& session, EditorState& editor);
    ~ClipListPanel() override;

    [[nodiscard]] const std::vector<cliplist::Item>& getItems() const noexcept { return items_; }
    [[nodiscard]] const cliplist::Options& getOptions() const noexcept { return options_; }
    void setOptions(const cliplist::Options& options);
    [[nodiscard]] bool isShowingAutoCreated() const noexcept { return options_.show.autoCreated; }
    void setShowingAutoCreated(bool show);
    /** Where the options are remembered (the user settings). */
    void setSettings(juce::PropertySet* settings);
    /** Re-reads the list from the session (done by itself when the session changes). */
    void refresh();

    // Selection
    void selectRows(cliplist::Selection selection);
    void selectRow(int row, bool add);
    [[nodiscard]] std::vector<cliplist::Item> getSelectedItems() const;

    // Commands (on the selected rows)
    void renameItem(int row);
    void removeItem(int row);
    void removeSelected();
    void autoRenameSelected();
    void timeStampSelected();
    void exportSelected();
    /** The Clip List menu (Show, Select, Sort By and the commands). */
    [[nodiscard]] juce::PopupMenu buildMenu();

    /** The playhead (Time Stamp's "Current Time"). */
    std::function<double()> getPlayheadSeconds;

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    int getNumRows() override { return static_cast<int>(items_.size()); }
    void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override;
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override;
    void listBoxItemClicked(int row, const juce::MouseEvent& e) override;
    void selectedRowsChanged(int lastRowSelected) override;
    juce::var getDragSourceDescription(const juce::SparseSet<int>& rows) override;

    /** The column layout across `width` (empty rectangles for hidden columns). */
    struct Columns
    {
        juce::Rectangle<int> colour, type, name, timebase, processing, length, file, disk, tag;
    };
    [[nodiscard]] Columns layoutColumns(int width, int height) const;
    void paintHeader(juce::Graphics& g, juce::Rectangle<int> area) const;
    void saveOptions();

    void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier&) override { triggerAsyncUpdate(); }
    void valueTreeChildAdded(juce::ValueTree&, juce::ValueTree&) override { triggerAsyncUpdate(); }
    void valueTreeChildRemoved(juce::ValueTree&, juce::ValueTree&, int) override { triggerAsyncUpdate(); }
    void valueTreeRedirected(juce::ValueTree&) override { triggerAsyncUpdate(); }
    void handleAsyncUpdate() override { refresh(); }

    session::Session& session_;
    EditorState& editor_;
    cliplist::Options options_;
    cliplist::FileInfoCache files_;
    std::vector<cliplist::Item> items_;
    juce::PropertySet* settings_ = nullptr;
    juce::TextButton menuButton_;
    juce::Label status_;
    juce::ListBox list_;
    std::unique_ptr<juce::FileChooser> chooser_;
    bool syncingSelection_ = false;
};

} // namespace dojo::ui
