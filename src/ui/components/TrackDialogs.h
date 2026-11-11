#pragma once

#include "ui/EditActions.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

namespace dojo::ui
{

/**
    Track > New: rows of [count] [format] [type] [name], with + to add a row below and - to remove one,
    and whether the tracks go after the selected track or at the end. Create (or Return) adds them
    all; Escape cancels.
*/
class NewTracksPanel final : public juce::Component
{
public:
    static constexpr int kMaxRows = 8;

    NewTracksPanel();
    ~NewTracksPanel() override;

    /** One row's controls (public for tests). */
    struct Row final : public juce::Component
    {
        Row();
        void resized() override;

        juce::TextEditor count;
        juce::ComboBox format, type;
        juce::TextEditor name;
        juce::TextButton add{"+"}, remove{"-"};
    };

    [[nodiscard]] int getNumRows() const noexcept { return static_cast<int>(rows_.size()); }
    [[nodiscard]] Row& getRow(int index) { return *rows_[static_cast<std::size_t>(index)]; }
    void addRow(int after);
    void removeRow(int index);

    [[nodiscard]] std::vector<actions::NewTracks> getRows() const;
    [[nodiscard]] bool isAfterSelection() const { return afterSelection_.getToggleState(); }
    juce::ToggleButton& getAfterSelectionToggle() noexcept { return afterSelection_; }

    /** What Create does (also for tests). */
    void create();

    std::function<void(const std::vector<actions::NewTracks>&, bool afterSelection)> onCreate;

    void resized() override;
    void paint(juce::Graphics& g) override;

    /** Opens the panel in a dialog window. */
    static void show(juce::Component* centreAround, std::function<void(const std::vector<actions::NewTracks>&, bool)> onCreate);

private:
    void updateRow(Row& row);
    void layoutWindow();

    std::vector<std::unique_ptr<Row>> rows_;
    juce::ToggleButton afterSelection_{"Insert after the selected track"};
    juce::TextButton createButton_{"Create"}, cancelButton_{"Cancel"};
};

/** Track > Duplicate: how many copies, what they take along, and where they go. */
class DuplicateTracksPanel final : public juce::Component
{
public:
    DuplicateTracksPanel(int numSelected, const actions::DuplicateSettings& initial);

    [[nodiscard]] actions::DuplicateSettings getSettings() const;
    /** What Duplicate does (also for tests). */
    void duplicate();

    std::function<void(const actions::DuplicateSettings&)> onDuplicate;

    juce::TextEditor copies;
    juce::ToggleButton clips{"Clips (audio and MIDI)"}, automation{"Automation"}, devices{"Devices (with their current settings)"},
        sends{"Sends"}, afterLastSelected{"Insert after the last selected track"};

    void resized() override;
    void paint(juce::Graphics& g) override;

    static void show(juce::Component* centreAround, int numSelected, const actions::DuplicateSettings& initial,
                     std::function<void(const actions::DuplicateSettings&)> onDuplicate);

private:
    juce::Label copiesLabel_, takeLabel_;
    juce::TextButton duplicateButton_{"Duplicate"}, cancelButton_{"Cancel"};
};

/** Track > Scroll to Track: type a track number (or pick one from the list). */
class ScrollToTrackPanel final : public juce::Component, private juce::ListBoxModel
{
public:
    /** `names` in track-number order (number 1 first); `current` = the current track's number. */
    ScrollToTrackPanel(juce::StringArray names, int current);

    /** The number typed (1..count), or 0 if it is not a valid track number. */
    [[nodiscard]] int getNumber() const;
    /** What OK does (also for tests): calls onScroll with a valid number and closes. */
    void confirm();

    std::function<void(int number)> onScroll;

    juce::TextEditor number;

    void resized() override;
    void paint(juce::Graphics& g) override;

    static void show(juce::Component* centreAround, juce::StringArray names, int current, std::function<void(int)> onScroll);

private:
    int getNumRows() override { return names_.size(); }
    void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override;
    void listBoxItemClicked(int row, const juce::MouseEvent&) override;
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override;

    juce::StringArray names_;
    juce::Label label_;
    juce::ListBox list_;
    juce::TextButton okButton_{"Scroll"}, cancelButton_{"Cancel"};
};

} // namespace dojo::ui
