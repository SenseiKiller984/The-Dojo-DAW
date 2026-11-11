#pragma once

#include "plugins/PluginManager.h"
#include "session/Project.h"
#include "ui/EditorState.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <map>
#include <memory>
#include <vector>

namespace dojo::ui
{

class DeviceCard;

/**
    The device chain of the selected track (or the master bus), left to right in signal order.

    Each device is a card: a header (name, bypass, sidechain source for devices that have a
    sidechain input, plug-in window, move left/right, remove) above its editor. The "+" card adds a
    native device or a plug-in; devices and plug-ins can also be dropped here from the browser.
    Hosted plug-ins open their own GUI in a separate window.
*/
class DevicePanel final : public juce::Component,
                          public juce::DragAndDropTarget,
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

    /** Opens (or brings to front) a hosted plug-in's own GUI. */
    void openPluginWindow(const juce::ValueTree& device);

private:
    class PluginWindow;

    [[nodiscard]] juce::ValueTree getOwner() const;
    void rebuild();
    void layoutCards();
    void showAddMenu();
    /** VCA Masters and MIDI tracks have no devices. */
    [[nodiscard]] static bool ownerTakesDevices(const juce::ValueTree& owner);
    void closeStaleWindows();

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
};

} // namespace dojo::ui
