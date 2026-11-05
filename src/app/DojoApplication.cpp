#include "app/DojoApplication.h"

#include "app/MainWindow.h"
#include "app/SmokeTest.h"
#include "core/rt/RealtimeGuard.h"
#include "core/Version.h"
#include "engine/AudioEngine.h"
#include "engine/EngineController.h"
#include "engine/io/AudioDeviceHost.h"
#include "session/Project.h"
#include "ui/theme/DojoLookAndFeel.h"

#include <cstdio>

namespace dojo::app
{
namespace
{
constexpr const char* kAudioDeviceSettingKey = "audioDevice";
} // namespace

DojoApplication::DojoApplication() = default;
DojoApplication::~DojoApplication() = default;

const juce::String DojoApplication::getApplicationName()
{
    return version::kProductName;
}

const juce::String DojoApplication::getApplicationVersion()
{
    return version::kVersionString;
}

//==================================================================================================
// UI (message) thread entry point.

void DojoApplication::initialise(const juce::String&)
{
    options_ = CommandLineOptions::parse(getCommandLineParameterArray());

    if (options_.printVersion)
    {
        std::printf("%s %s\n", version::kProductName, version::kVersionString);
        setApplicationReturnValue(0);
        quit();
        return;
    }

    logger_.reset(juce::FileLogger::createDefaultAppLogger(
        "TheDojo", "TheDojo.log", juce::String(version::kProductName) + " " + version::kVersionString));
    juce::Logger::setCurrentLogger(logger_.get());

    // Theme first, so every component created afterwards picks it up.
    lookAndFeel_ = std::make_unique<ui::DojoLookAndFeel>();
    juce::LookAndFeel::setDefaultLookAndFeel(lookAndFeel_.get());

    juce::PropertiesFile::Options settingsOptions;
    settingsOptions.applicationName = "TheDojo";
    settingsOptions.filenameSuffix = ".settings";
    settingsOptions.folderName = "TheDojo";
    settingsOptions.osxLibrarySubFolder = "Application Support";
    settings_ = std::make_unique<juce::ApplicationProperties>();
    settings_->setStorageParameters(settingsOptions);

    // Engine, then the device that drives it. Opening the device starts the audio thread, which
    // enters The Dojo through AudioEngine::process().
    engine_ = std::make_unique<engine::AudioEngine>();
    deviceHost_ = std::make_unique<engine::AudioDeviceHost>(*engine_);
    controller_ = std::make_unique<engine::EngineController>(*engine_, *deviceHost_);

    const auto savedDevice = settings_->getUserSettings()->getXmlValue(kAudioDeviceSettingKey);
    const auto deviceError = deviceHost_->openDevice(savedDevice.get(), options_.audioApi);
    deviceHost_->getDeviceManager().addChangeListener(this); // persist later changes
    logStartupInformation(deviceError);

    // The document: every change is compiled into a render graph and handed to the engine.
    project_ = std::make_unique<session::Project>(
        [controller = controller_.get()](std::unique_ptr<engine::RenderGraph> graph)
        { controller->publishGraph(std::move(graph)); });

    if (options_.testTone)
        controller_->setTestToneEnabled(true);

    commandManager_ = std::make_unique<juce::ApplicationCommandManager>();
    mainWindow_ = std::make_unique<MainWindow>(*project_, *controller_, *commandManager_);

    if (options_.sessionFile != juce::File())
        mainWindow_->getMainComponent().openSessionFile(options_.sessionFile);

    if (!options_.audioFiles.isEmpty())
        mainWindow_->getMainComponent().importAudioFiles(options_.audioFiles);

    if (options_.play)
        controller_->play();

    if (options_.smokeTestSeconds > 0)
    {
        smokeTest_ = std::make_unique<SmokeTest>(options_.smokeTestSeconds, mainWindow_->getMainComponent(),
                                                 *controller_,
                                                 [this](int exitCode)
                                                 {
                                                     setApplicationReturnValue(exitCode);
                                                     quit();
                                                 });
    }
}

void DojoApplication::shutdown()
{
    if (deviceHost_ != nullptr)
        deviceHost_->getDeviceManager().removeChangeListener(this);

    smokeTest_.reset();
    mainWindow_.reset(); // 1. UI goes first: nothing can post commands any more.
    commandManager_.reset();
    project_.reset(); //    The document; graphs already in the engine keep their audio alive.
    controller_.reset();
    deviceHost_.reset(); // 2. Stops the driver: no audio callback can be in flight after this.
    engine_.reset();     // 3. Now the real-time state (and the last graphs) can be destroyed.

    settings_.reset();

    juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
    lookAndFeel_.reset();

    juce::Logger::setCurrentLogger(nullptr);
    logger_.reset();
}

void DojoApplication::systemRequestedQuit()
{
    if (mainWindow_ != nullptr && smokeTest_ == nullptr)
        mainWindow_->getMainComponent().requestQuit([] { JUCEApplicationBase::quit(); });
    else
        quit();
}

void DojoApplication::anotherInstanceStarted(const juce::String&) {}

void DojoApplication::changeListenerCallback(juce::ChangeBroadcaster*)
{
    // The audio device configuration changed (e.g. in Audio Settings): remember it for next launch.
    if (deviceHost_ == nullptr || settings_ == nullptr)
        return;

    if (const auto state = deviceHost_->createStateXml())
    {
        settings_->getUserSettings()->setValue(kAudioDeviceSettingKey, state.get());
        settings_->saveIfNeeded();
    }
}

void DojoApplication::logStartupInformation(const juce::String& deviceError)
{
    juce::Logger::writeToLog(juce::String("Platform: ") + juce::SystemStats::getOperatingSystemName() + ", " +
                             juce::String(juce::SystemStats::getNumCpus()) + " logical CPUs");
    juce::Logger::writeToLog("Audio back-ends compiled in: " +
                             deviceHost_->getAvailableDeviceTypeNames().joinIntoString(", "));
    juce::Logger::writeToLog(juce::String("RT allocation trap: ") +
                             (rt::isAllocationTrapInstalled() ? "installed" : "not installed"));

    const auto status = deviceHost_->getStatus();

    if (deviceError.isEmpty() && status.isOpen)
        juce::Logger::writeToLog("Audio device: " + status.deviceType + " / " + status.deviceName + ", " +
                                 juce::String(status.sampleRate) + " Hz, " +
                                 juce::String(status.bufferSizeSamples) + " samples");
    else
        juce::Logger::writeToLog("No audio device opened: " + deviceError);
}

} // namespace dojo::app
