#pragma once

#include "engine/EngineController.h"
#include "ui/MainComponent.h"

#include <juce_events/juce_events.h>

#include <functional>

namespace dojo::app
{

/**
    Start-up self-check used by CI (--smoke-test). After the given duration it verifies that:
      - the main window rendered frames (the UI thread and VBlank-driven refresh are alive),
      - if an audio device opened, the audio callback actually ran,
      - no real-time violations were recorded on the audio thread,
    then reports through `onFinished(exitCode)`.

    Absence of an audio device is NOT a failure (CI runners have no sound hardware); the result is
    logged so it is visible in the job output.
*/
class SmokeTest final : private juce::Timer
{
public:
    SmokeTest(int durationSeconds, const ui::MainComponent& mainComponent,
              engine::EngineController& controller, std::function<void(int exitCode)> onFinished);
    ~SmokeTest() override;

private:
    void timerCallback() override;

    const ui::MainComponent& mainComponent_;
    engine::EngineController& controller_;
    std::function<void(int)> onFinished_;
};

} // namespace dojo::app
