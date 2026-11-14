#pragma once

#include "app/MainWindow.h"
#include "engine/EngineController.h"

#include <juce_events/juce_events.h>

#include <functional>

namespace dojo::app
{

/**
    Start-up self-check used by CI (--smoke-test). After the given duration it verifies that:
      - the main window rendered frames (the UI thread and VBlank-driven refresh are alive),
      - if an audio device opened, the audio callback actually ran,
      - no real-time violations were recorded on the audio thread,
      - the window maximises and restores with the square button, goes full screen and back with
        F11 (through its shortcut), and comes back from full screen to the state it was in,
    then reports through `onFinished(exitCode)`.

    Absence of an audio device is NOT a failure (CI runners have no sound hardware); the result is
    logged so it is visible in the job output.
*/
class SmokeTest final : private juce::Timer
{
public:
    SmokeTest(int durationSeconds, MainWindow& window, juce::ApplicationCommandManager& commands,
              engine::EngineController& controller, std::function<void(int exitCode)> onFinished);
    ~SmokeTest() override;

private:
    void timerCallback() override;
    /** Checks what the previous step did and takes the next; false once the window check is done. */
    bool stepWindowCheck();
    void expectWindow(bool condition, const juce::String& what);
    void pressF11();

    MainWindow& window_;
    juce::ApplicationCommandManager& commands_;
    engine::EngineController& controller_;
    std::function<void(int)> onFinished_;
    juce::uint32 finishAt_ = 0;
    int step_ = 0;
    juce::Rectangle<int> windowed_;
    juce::StringArray windowFailures_;
};

} // namespace dojo::app
