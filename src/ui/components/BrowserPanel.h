#pragma once

#include "plugins/PluginManager.h"
#include "ui/DeviceActions.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace dojo::ui
{

/**
    Left-hand browser with three tabs:

    FILES     a folder tree filtered to readable audio formats. Drag files onto the arranger (onto a
              track, or below the last track to create new ones), or double-click a file to put it
              on a new track at the playhead. The location menu jumps to common folders.
    DEVICES   The Dojo's instruments and effects: drag onto a track or the Devices panel, or
              double-click to add to the selected track.
    PLUG-INS  installed VST3 and CLAP plug-ins (same gestures), plus scanning: scan new files,
              rescan everything, add search folders, clear the blacklist of plug-ins that crashed.
*/
class BrowserPanel final : public juce::Component, private juce::FileBrowserListener
{
public:
    /** `audioWildcard` is the pattern of readable formats, e.g. "*.wav;*.flac;..." */
    BrowserPanel(const juce::String& audioWildcard, plugins::PluginManager& plugins);
    ~BrowserPanel() override;

    /** Called when a file is double-clicked. */
    std::function<void(const juce::File&)> onFileActivated;
    /** Called when a device or plug-in is double-clicked. */
    std::function<void(const deviceActions::DeviceChoice&)> onDeviceActivated;

    /** Refreshes the plug-in list (after a scan). */
    void refreshPlugins();
    /** Starts a background plug-in scan. */
    void scanPlugins(bool rescanAll);

    void setRoot(const juce::File& folder);

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    class ChoiceList;

    enum class Tab
    {
        files,
        devices,
        plugins,
    };

    void showTab(Tab tab);
    void addSearchFolder();

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

    plugins::PluginManager& plugins_;
    Tab tab_ = Tab::files;
    juce::TextButton filesTab_;
    juce::TextButton devicesTab_;
    juce::TextButton pluginsTab_;
    std::unique_ptr<ChoiceList> deviceList_;
    std::unique_ptr<ChoiceList> pluginList_;
    juce::TextButton scanButton_;
    juce::TextButton moreButton_;
    juce::Label scanStatus_;
};

} // namespace dojo::ui
