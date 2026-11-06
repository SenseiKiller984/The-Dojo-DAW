#pragma once

#include "engine/Preview.h"
#include "plugins/PluginManager.h"
#include "ui/DeviceActions.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace dojo::ui
{

/**
    The left-hand browser, in two sections split by a draggable divider:

    DEVICES & PLUG-INS (top)  The Dojo's instruments and effects and every scanned VST3/CLAP
                              plug-in, grouped, with a search field. Drag one onto a track or the
                              Devices panel, or double-click (Return) to add it to the selected track.
                              Scan / rescan / search folders / blacklist live in its "..." menu.
    FOLDERS (bottom)          Starts empty: add the folders you keep samples in ("+", or drop a folder
                              from the file manager). Each is a tree of its sub-folders and audio
                              files, with its own search field (searches every added folder,
                              recursively). Selecting a file previews it (click, or walk through files
                              with the arrow keys) when the headphones button is on; the preview strip
                              at the bottom shows progress and has the preview volume. Drag files onto
                              the arranger, a Drum Rack pad, an Impulse slot or the Sampler; double-click
                              puts a file on a new track.

    Folders, preview on/off, preview volume and the divider position are saved in `settings`.
*/
class BrowserPanel final : public juce::Component, public juce::FileDragAndDropTarget, private juce::Timer
{
public:
    BrowserPanel(const juce::String& audioWildcard, plugins::PluginManager& plugins, engine::PreviewService& preview,
                 juce::PropertiesFile& settings);
    ~BrowserPanel() override;

    /** Called when a file is double-clicked. */
    std::function<void(const juce::File&)> onFileActivated;
    /** Called when a device or plug-in is double-clicked. */
    std::function<void(const deviceActions::DeviceChoice&)> onDeviceActivated;

    /** Refreshes the device and plug-in list (after a scan). */
    void refreshPlugins();
    /** Starts a background plug-in scan. */
    void scanPlugins(bool rescanAll);

    // --- Folders ---------------------------------------------------------------------------------
    void addFolder(const juce::File& folder);
    void removeFolder(const juce::File& folder);
    [[nodiscard]] const juce::Array<juce::File>& getFolders() const noexcept { return folders_; }
    /** Shows `query`'s matches in every folder (searches in the background); empty shows the tree. */
    void setFolderSearch(const juce::String& query);
    /** Files currently listed as search results (tests). */
    [[nodiscard]] juce::Array<juce::File> getSearchResults() const;
    [[nodiscard]] bool isSearching() const noexcept;

    // --- Devices ---------------------------------------------------------------------------------
    void setDeviceSearch(const juce::String& query);
    /** Names of the devices and plug-ins currently listed (tests). */
    [[nodiscard]] juce::StringArray getListedDeviceNames() const;

    // --- Preview ---------------------------------------------------------------------------------
    void setPreviewEnabled(bool enabled);
    [[nodiscard]] bool isPreviewEnabled() const noexcept { return previewEnabled_; }
    /** Previews `file` if previewing is on (what selecting a file does). */
    void fileSelected(const juce::File& file);
    /** The file being previewed right now (none when silent). */
    [[nodiscard]] juce::File getPreviewingFile() const
    {
        return preview_.isPlaying() ? preview_.getCurrentFile() : juce::File();
    }

    void paint(juce::Graphics& g) override;
    void resized() override;

    // External drops: folders become browser folders.
    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;

private:
    class DeviceList;
    class FolderTree;
    class PreviewStrip;
    class SearchThread;
    class Divider;
    void timerCallback() override;
    void searchFinished(const juce::String& query, const juce::Array<juce::File>& results);
    void saveFolders();
    void chooseFolderToAdd();
    void addSearchFolder();
    void showPluginMenu();
    void layoutSections();

    juce::String wildcard_;
    plugins::PluginManager& plugins_;
    engine::PreviewService& preview_;
    juce::PropertiesFile& settings_;

    juce::Array<juce::File> folders_;
    bool previewEnabled_ = true;
    double split_ = 0.42; ///< proportion of the height given to devices

    juce::TextEditor deviceSearch_;
    std::unique_ptr<DeviceList> deviceList_;
    juce::TextButton scanButton_;
    juce::TextButton moreButton_;
    juce::String scanStatus_;

    std::unique_ptr<Divider> divider_;

    juce::TextEditor folderSearch_;
    juce::TextButton addFolderButton_;
    juce::TextButton emptyAddButton_;
    std::unique_ptr<FolderTree> folderTree_;
    std::unique_ptr<PreviewStrip> previewStrip_;
    std::unique_ptr<SearchThread> search_;
    int searchDebounce_ = 0; ///< timer ticks until the typed search runs
    juce::String activeQuery_;
    bool searching_ = false;
    bool wasPlaying_ = false;
    juce::File shownPreviewFile_;
    int slowTicks_ = 0;

    std::unique_ptr<juce::FileChooser> chooser_;
    juce::Rectangle<int> devicesHeader_;
    juce::Rectangle<int> devicesStatus_;
    juce::Rectangle<int> foldersHeader_;
    juce::Rectangle<int> emptyArea_;
};

} // namespace dojo::ui
