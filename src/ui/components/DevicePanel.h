#pragma once

#include "plugins/PluginManager.h"
#include "session/Presets.h"
#include "session/Project.h"
#include "ui/EditorState.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace dojo::ui
{

class DeviceCard;

/**
    The device chain of the selected track (or the master bus), left to right in signal order.

    Each device is a card: a header (name, bypass, sidechain source for devices that have a
    sidechain input, presets, plug-in window, move left/right, remove) above its editor. The "+"
    card adds a native device or a plug-in; devices and plug-ins can also be dropped here from the
    browser. Hosted plug-ins open their own GUI in a separate window.

    Presets (session/Presets.h): a card's presets menu loads the presets that fit its device and
    saves its settings (as a new preset file, into an existing one, or with the whole chain). A
    preset dropped onto a card (from the browser, or a .dojopreset file from the file manager) loads
    into that device; dropped anywhere else on the panel it adds a device that starts from it.
*/
class DevicePanel final : public juce::Component,
                          public juce::DragAndDropTarget,
                          public juce::FileDragAndDropTarget,
                          private juce::ValueTree::Listener,
                          private EditorState::Listener,
                          private juce::AsyncUpdater
{
public:
    DevicePanel(session::Project& project, plugins::PluginManager& plugins, EditorState& editor);
    ~DevicePanel() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    bool isInterestedInDragSource(const SourceDetails& details) override;
    void itemDropped(const SourceDetails& details) override;
    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;

    /** Opens (or brings to front) a hosted plug-in's own GUI. */
    void openPluginWindow(const juce::ValueTree& device);

    /** Shows a file chooser for a preset file (kept here so it outlives a rebuilt card). */
    void choosePresetFile(const juce::String& title, bool forSaving, const juce::File& initial,
                          std::function<void(const juce::File&)> onChosen);
    /** Shows a short message on a device's card (what a preset action did). */
    void showNotice(const juce::ValueTree& device, const juce::String& text, bool ok);

    // --- Tests -----------------------------------------------------------------------------------
    /** The card showing `device` (a DragAndDropTarget and a FileDragAndDropTarget), or nullptr. */
    [[nodiscard]] juce::Component* findCard(const juce::ValueTree& device) const;
    /** The message the card of `device` shows right now (empty when none). */
    [[nodiscard]] juce::String getNotice(const juce::ValueTree& device) const;
    /** The message the panel itself shows right now (a preset dropped on it). */
    [[nodiscard]] const juce::String& getPanelNotice() const noexcept { return panelNotice_; }
    /** What the presets menu of the card of `device` lists (section headers in brackets). */
    [[nodiscard]] juce::StringArray getPresetMenuItems(const juce::ValueTree& device) const;

private:
    class PluginWindow;

    [[nodiscard]] juce::ValueTree getOwner() const;
    void rebuild();
    void layoutCards();
    void showAddMenu();
    /** VCA Masters and MIDI tracks have no devices. */
    [[nodiscard]] static bool ownerTakesDevices(const juce::ValueTree& owner);
    void closeStaleWindows();
    void dropPreset(const session::presets::Preset& preset);
    void setPanelNotice(const juce::String& text);

    void handleAsyncUpdate() override { rebuild(); }
    void editorStateChanged() override;
    void valueTreeChildAdded(juce::ValueTree& parent, juce::ValueTree& child) override;
    void valueTreeChildRemoved(juce::ValueTree& parent, juce::ValueTree& child, int) override;
    void valueTreeChildOrderChanged(juce::ValueTree& parent, int, int) override;
    void valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& property) override;
    void valueTreeRedirected(juce::ValueTree&) override { triggerAsyncUpdate(); }

    session::Project& project_;
    plugins::PluginManager& plugins_;
    EditorState& editor_;

    juce::Viewport viewport_;
    juce::Component holder_;
    std::vector<std::unique_ptr<DeviceCard>> cards_;
    juce::TextButton addButton_;
    juce::ValueTree shownOwner_;
    bool shownMaster_ = false;
    std::map<std::uint32_t, std::unique_ptr<PluginWindow>> windows_;
    std::unique_ptr<juce::FileChooser> chooser_;
    juce::String panelNotice_;
    juce::uint32 panelNoticeSerial_ = 0;
};

} // namespace dojo::ui
