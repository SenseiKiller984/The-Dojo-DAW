#pragma once

#include "licensing/LicenseManager.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace dojo::ui
{

/** Where "Try The Dojo for free!" leads: the README's free-trial section (with the trial key). */
inline constexpr const char* kFreeTrialUrl = "https://github.com/SenseiKiller984/The-Dojo-DAW#free-trial";

/**
    The license window's contents: what the license is, a field for a key, Activate, and
    "Try The Dojo for free!" (opens the free-trial page in the browser). In the lock screen it has no
    Close button; opened from Options > License... it does, and Remove License Key.
*/
class LicensePanel final : public juce::Component, private juce::ChangeListener
{
public:
    LicensePanel(licensing::LicenseManager& license, bool dismissable);
    ~LicensePanel() override;

    /** The Close button (dismissable panels). */
    std::function<void()> onClose;
    /** After a key was activated. */
    std::function<void()> onActivated;
    /** Opens a URL (default: the system browser; tests replace it). */
    std::function<void(const juce::String& url)> openUrl;

    void paint(juce::Graphics& g) override;
    void resized() override;

    // What the buttons do (also for tests).
    void setKeyText(const juce::String& text) { key_.setText(text, juce::dontSendNotification); }
    void activate();
    void openFreeTrialPage();
    [[nodiscard]] juce::String getMessage() const { return message_.getText(); }
    [[nodiscard]] juce::String getStatusText() const { return status_.getText(); }

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override { update(); }
    void update();

    licensing::LicenseManager& license_;
    bool dismissable_ = false;
    juce::Label title_, status_, prompt_, message_;
    juce::TextEditor key_;
    juce::TextButton activate_, trial_, close_, remove_;
};

/**
    The lock screen (No License): the whole window but its title bar, dimmed, with the license panel
    in the middle. It is modal: nothing behind it can be clicked, only the window's close button and
    its maximise (windowed / full screen) button, and the title bar to move the window. The commands
    are switched off as well (MainComponent), so no shortcut gets through.
*/
class LicenseLock final : public juce::Component
{
public:
    LicenseLock(licensing::LicenseManager& license, juce::DocumentWindow& window);
    ~LicenseLock() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    bool canModalEventBeSentToComponent(const juce::Component* target) override;
    void inputAttemptWhenModal() override {}

    [[nodiscard]] LicensePanel& getPanel() noexcept { return panel_; }

private:
    juce::DocumentWindow& window_;
    LicensePanel panel_;
};

} // namespace dojo::ui
