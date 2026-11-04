#pragma once

#include "app/CommandLineOptions.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace dojo::engine
{
class AudioEngine;
class AudioDeviceHost;
class EngineController;
} // namespace dojo::engine

namespace dojo::ui
{
class DojoLookAndFeel;
} // namespace dojo::ui

namespace dojo::app
{

class MainWindow;
class SmokeTest;

/**
    Application object. initialise() runs on the main thread, which JUCE turns into the
    message (UI) thread — this is the UI thread's entry point.

    Object graph and lifetime (constructed top to bottom, destroyed bottom to top):

        DojoLookAndFeel    fonts + theme, installed as the default look-and-feel
        AudioEngine        real-time state; outlives everything that can call into it
        AudioDeviceHost    driver + callback adapter -> AudioEngine::process (audio thread entry)
        EngineController   the UI's message-thread façade over engine + device host
        MainWindow         the UI

    Shutdown tears the UI down first, then stops the audio device (after which no callback can be
    in flight), and only then destroys the engine.
*/
class DojoApplication final : public juce::JUCEApplication
{
public:
    DojoApplication();
    ~DojoApplication() override;

    const juce::String getApplicationName() override;
    const juce::String getApplicationVersion() override;
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise(const juce::String& commandLine) override;
    void shutdown() override;
    void systemRequestedQuit() override;
    void anotherInstanceStarted(const juce::String& commandLine) override;

private:
    void logStartupInformation(const juce::String& deviceError);

    CommandLineOptions options_;

    std::unique_ptr<juce::FileLogger> logger_;
    std::unique_ptr<ui::DojoLookAndFeel> lookAndFeel_;
    std::unique_ptr<engine::AudioEngine> engine_;
    std::unique_ptr<engine::AudioDeviceHost> deviceHost_;
    std::unique_ptr<engine::EngineController> controller_;
    std::unique_ptr<MainWindow> mainWindow_;
    std::unique_ptr<SmokeTest> smokeTest_;
};

} // namespace dojo::app
