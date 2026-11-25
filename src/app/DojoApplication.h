#pragma once

#include "app/CommandLineOptions.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace dojo::engine
{
class AudioEngine;
class AudioDeviceHost;
class EngineController;
class PreviewService;
} // namespace dojo::engine

namespace dojo::session
{
class Project;
class Recorder;
class Exporter;
class AdmOscBridge;
} // namespace dojo::session

namespace dojo::plugins
{
class PluginManager;
} // namespace dojo::plugins

namespace dojo::ui
{
class Preferences;
class DojoLookAndFeel;
struct AppServices;
} // namespace dojo::ui

namespace dojo::licensing
{
class LicenseManager;
}

namespace dojo::app
{

class MainWindow;
class SmokeTest;
class LicenseGuard;

/**
    Application object. initialise() runs on the main thread, which JUCE turns into the
    message (UI) thread — this is the UI thread's entry point.

    Object graph and lifetime (constructed top to bottom, destroyed bottom to top):

        DojoLookAndFeel    fonts + theme, installed as the default look-and-feel
        settings           per-user preferences file (audio device choice)
        LicenseManager     the license key (Ed25519) and the free trial's ledger: what is allowed
        AudioEngine        real-time state; outlives everything that can call into it
        AudioDeviceHost    driver + callback adapter -> AudioEngine::process (audio thread entry)
        EngineController   the UI's message-thread façade over engine + device host
        PluginManager      known plug-ins (VST3, CLAP), out-of-process scanning, instantiation
        Project            the document (session, decoded audio, devices, graph compiler -> controller)
        Recorder/Exporter  recording onto armed tracks; offline / real-time bounces
        MainWindow         the UI

    Shutdown tears the UI and the document down first, then stops the audio device (after which no
    callback can be in flight), and only then destroys the engine.
*/
class DojoApplication final : public juce::JUCEApplication, private juce::ChangeListener
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
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;
    /** Preferences > Surround, to the engine. */
    void applySurroundMonitoring();
    /** Preferences > Renderer: the objects' outputs (the compiler) and the ADM-OSC bridge. */
    void applyExternalRenderer();
    [[nodiscard]] juce::String describeExternalRenderer() const;
    void logStartupInformation(const juce::String& deviceError);
    /** The license changed (or the trial's mute): engine options, the pro devices, the status bar. */
    void licenseChanged();

    CommandLineOptions options_;

    std::unique_ptr<juce::FileLogger> logger_;
    std::unique_ptr<ui::DojoLookAndFeel> lookAndFeel_;
    std::unique_ptr<juce::ApplicationProperties> settings_;
    std::unique_ptr<ui::Preferences> preferences_;
    std::unique_ptr<licensing::LicenseManager> license_;
    std::unique_ptr<engine::AudioEngine> engine_;
    std::unique_ptr<engine::AudioDeviceHost> deviceHost_;
    std::unique_ptr<engine::EngineController> controller_;
    std::unique_ptr<plugins::PluginManager> plugins_;
    std::unique_ptr<session::Project> project_;
    std::unique_ptr<session::Recorder> recorder_;
    std::unique_ptr<session::Exporter> exporter_;
    std::unique_ptr<session::AdmOscBridge> oscBridge_;
    std::unique_ptr<engine::PreviewService> preview_;
    std::unique_ptr<LicenseGuard> licenseGuard_;
    std::unique_ptr<juce::ApplicationCommandManager> commandManager_;
    std::unique_ptr<ui::AppServices> services_;
    std::unique_ptr<MainWindow> mainWindow_;
    std::unique_ptr<SmokeTest> smokeTest_;
};

} // namespace dojo::app
