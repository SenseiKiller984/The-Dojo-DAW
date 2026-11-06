#pragma once

#include "ui/Preferences.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <functional>

namespace dojo::ui
{

/**
    The Preferences window (Options > Preferences, Ctrl+,), with four tabs:

    General   playback options: return the view to the start with the playhead; stop at the end of
              the arrangement
    Themes    the colour themes, shown as previews; a click applies one
    Audio     driver, device, inputs and MIDI inputs (the same as Options > Audio Settings)
    Keyboard  every command's shortcuts, editable (Options > Keyboard Shortcuts opens this tab)

    Owned by the main window and kept while the application runs (closing only hides it).
*/
class PreferencesWindow final : public juce::DocumentWindow
{
public:
    PreferencesWindow(Preferences& preferences, juce::AudioDeviceManager& devices, juce::ApplicationCommandManager& commands,
                      std::function<void()> showEditingTips);
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
    Content* content_ = nullptr; // owned by the window
};

} // namespace dojo::ui
