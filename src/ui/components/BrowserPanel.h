#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace dojo::ui
{

/**
    Left-hand file browser for audio files.

    Shows a folder tree filtered to readable audio formats. Drag files onto the arranger (onto a
    track, or below the last track to create new ones), or double-click a file to put it on a new
    track at the playhead. The location menu jumps to common folders or any folder you choose.
*/
class BrowserPanel final : public juce::Component, private juce::FileBrowserListener
{
public:
    /** `audioWildcard` is the pattern of readable formats, e.g. "*.wav;*.flac;..." */
    explicit BrowserPanel(const juce::String& audioWildcard);
    ~BrowserPanel() override;

    /** Called when a file is double-clicked. */
    std::function<void(const juce::File&)> onFileActivated;

    void setRoot(const juce::File& folder);

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void selectionChanged() override {}
    void fileClicked(const juce::File&, const juce::MouseEvent&) override {}
    void fileDoubleClicked(const juce::File& file) override;
    void browserRootChanged(const juce::File&) override {}
    void chooseFolder();

    juce::TimeSliceThread scanThread_{"Dojo file browser"};
    std::unique_ptr<juce::WildcardFileFilter> filter_;
    std::unique_ptr<juce::DirectoryContentsList> contents_;
    std::unique_ptr<juce::FileTreeComponent> tree_;
    juce::ComboBox location_;
    std::unique_ptr<juce::FileChooser> chooser_;
    juce::Array<juce::File> locations_;
};

} // namespace dojo::ui
