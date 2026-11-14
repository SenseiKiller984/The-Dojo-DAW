#pragma once

#include "ui/EditActions.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
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
        sends{"Sends"}, vcaAssignment{"VCA assignment (otherwise the VCA's gain is baked in)"},
        afterLastSelected{"Insert after the last selected track"};

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

/** A one-line prompt (Clip > Rename, Capture and Loop): a label, a text field, OK and Cancel. */
class TextPromptPanel final : public juce::Component
{
public:
    /** `numeric`: only a number above zero is accepted. */
    TextPromptPanel(const juce::String& label, const juce::String& initial, const juce::String& okText, bool numeric);

    /** What OK (or Return) does (also for tests): calls onConfirm with the trimmed text and closes,
        unless the text is empty (or not a number above zero for a numeric prompt). */
    void confirm();

    std::function<void(const juce::String&)> onConfirm;

    juce::TextEditor text;

    void resized() override;
    void paint(juce::Graphics& g) override;

    static void show(juce::Component* centreAround, const juce::String& title, const juce::String& label, const juce::String& initial,
                     const juce::String& okText, bool numeric, std::function<void(const juce::String&)> onConfirm);

private:
    bool numeric_ = false;
    juce::Label label_;
    juce::TextButton okButton_, cancelButton_{"Cancel"};
};

/**
    Clip List > Auto Rename: Name (the root), Number (the first number), Zeros (digits at least) and
    Suffix; the example shows the first name. The clips are renamed in order, and clips later made
    from them by editing continue the numbering.
*/
class AutoRenamePanel final : public juce::Component
{
public:
    AutoRenamePanel(const juce::String& rootName, int firstNumber, int numClips);

    /** What OK does (also for tests): calls onRename and closes, unless the name is empty. */
    void confirm();
    [[nodiscard]] juce::String getExample() const;

    std::function<void(const juce::String& name, int number, int zeros, const juce::String& suffix)> onRename;

    juce::TextEditor name, number, zeros, suffix;

    void resized() override;
    void paint(juce::Graphics& g) override;

    static void show(juce::Component* centreAround, const juce::String& rootName, int firstNumber, int numClips,
                     std::function<void(const juce::String&, int, int, const juce::String&)> onRename);

private:
    void update();

    std::array<juce::Label, 4> labels_;
    juce::Label example_;
    juce::TextButton okButton_{"Rename"}, cancelButton_{"Cancel"};
    int numClips_ = 1;
};

/**
    Clip List > Time Stamp: shows the clip's original time stamp (when its material was recorded) and
    sets its user time stamp (SMPTE, 30 fps). "Current Time" takes the playhead's. With several clips,
    the others move by as much as the first.
*/
class TimeStampPanel final : public juce::Component
{
public:
    TimeStampPanel(const juce::String& clipName, int numClips, double originalSeconds, double userSeconds, double playheadSeconds);

    /** What OK does (also for tests): calls onSet with the time typed and closes, unless it is not a
        timecode. */
    void confirm();

    std::function<void(double seconds)> onSet;

    juce::TextEditor user;
    juce::TextButton currentButton{"Current Time"};

    void resized() override;
    void paint(juce::Graphics& g) override;

    static void show(juce::Component* centreAround, const juce::String& clipName, int numClips, double originalSeconds, double userSeconds,
                     double playheadSeconds, std::function<void(double)> onSet);

private:
    juce::Label title_, originalLabel_, original_, userLabel_;
    juce::TextButton okButton_{"OK"}, cancelButton_{"Cancel"};
    double playhead_ = 0.0;
};

} // namespace dojo::ui
