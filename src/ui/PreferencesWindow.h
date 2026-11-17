#pragma once

#include "ui/Preferences.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <functional>

namespace dojo::ui
{

/**
    The Preferences window (Options > Preferences, Ctrl+,), with six tabs:

    General   playback options: return the view to the start with the playhead; stop at the end of
              the arrangement
    Themes    the colour themes, shown as previews; a click applies one
    Audio     driver, device, inputs and MIDI inputs (the same as Options > Audio Settings)
    Surround  how a surround master plays: Standard, Classic Fold-Down, the Advanced Surround
              Virtualizer (headphones or speakers, room) or the device's own 3D virtualizer
    Keyboard  every command's shortcuts, editable (Options > Keyboard Shortcuts opens this tab)
    Control Surfaces  Mackie Control units, extenders and HUI: their MIDI ports, in desk order

    Owned by the main window and kept while the application runs (closing only hides it).
*/
class PreferencesWindow final : public juce::DocumentWindow
{
public:
    /** `surroundRendering` reports what the engine does with a surround master now (an
        engine::SurroundRendering), for the Surround tab. */
    PreferencesWindow(Preferences& preferences, juce::AudioDeviceManager& devices, juce::ApplicationCommandManager& commands,
                      std::function<void()> showEditingTips, std::function<int()> surroundRendering = {},
                      std::function<juce::String(int)> surfaceStatus = {});
    ~PreferencesWindow() override;

    /** Shows the window (to front) on `tab`. */
    void showTab(Preferences::Tab tab);
    [[nodiscard]] Preferences::Tab getCurrentTab() const;

    /** Rebuilds the content in the active theme's colours (the tab stays). */
    void refreshTheme();

    void closeButtonPressed() override { setVisible(false); }

private:
    class Content;

    Preferences& preferences_;
    juce::AudioDeviceManager& devices_;
    juce::ApplicationCommandManager& commands_;
    std::function<void()> showEditingTips_;
    std::function<int()> surroundRendering_;
    std::function<juce::String(int)> surfaceStatus_; ///< (a control surface's, by its row)
    Content* content_ = nullptr; // owned by the window
};

/** The Control Surfaces tab on its own (tests): the setups kept in `preferences`, each row's status
    from `status`. */
[[nodiscard]] std::unique_ptr<juce::Component> createControlSurfacesPage(Preferences& preferences, std::function<juce::String(int)> status);

} // namespace dojo::ui
