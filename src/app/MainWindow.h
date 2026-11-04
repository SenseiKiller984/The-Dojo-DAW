#pragma once

#include "engine/EngineController.h"
#include "ui/MainComponent.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace dojo::app
{

/** The single top-level window. Uses JUCE-drawn (non-native) chrome so the window looks and
    behaves identically on Windows and Linux. */
class MainWindow final : public juce::DocumentWindow
{
public:
    MainWindow(const juce::String& title, engine::EngineController& controller);
    ~MainWindow() override;

    void closeButtonPressed() override;

    [[nodiscard]] ui::MainComponent& getMainComponent() noexcept { return *mainComponent_; }

private:
    ui::MainComponent* mainComponent_ = nullptr; // owned by the DocumentWindow (setContentOwned)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainWindow)
};

} // namespace dojo::app
