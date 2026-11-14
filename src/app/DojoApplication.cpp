#include "app/DojoApplication.h"

#include "app/MainWindow.h"
#include "app/SmokeTest.h"
#include "core/rt/RealtimeGuard.h"
#include "core/Version.h"
#include "devices/DeviceRegistry.h"
#include "engine/Preview.h"
#include "ui/Preferences.h"
#include "ui/theme/Themes.h"
#include "engine/AudioEngine.h"
#include "engine/EngineController.h"
#include "engine/io/AudioDeviceHost.h"
#include "plugins/PluginManager.h"
#include "session/Exporter.h"
#include "session/Project.h"
#include "session/Recorder.h"
#include "ui/AppServices.h"
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

    // The chosen theme, before any window or component exists.
    preferences_ = std::make_unique<ui::Preferences>(*settings_->getUserSettings());
    ui::theme::applyTheme(ui::theme::findTheme(preferences_->getThemeName()));
    lookAndFeel_->refreshColours();

    // Engine, then the device that drives it. Opening the device starts the audio thread, which
    // enters The Dojo through AudioEngine::process().
    engine_ = std::make_unique<engine::AudioEngine>();
    deviceHost_ = std::make_unique<engine::AudioDeviceHost>(*engine_);
    controller_ = std::make_unique<engine::EngineController>(*engine_, *deviceHost_);
    controller_->setStopAtEnd(preferences_->getStopAtEnd());
    applySurroundMonitoring();
    preferences_->addChangeListener(this); // engine options follow the preferences

    const auto savedDevice = settings_->getUserSettings()->getXmlValue(kAudioDeviceSettingKey);
    const auto deviceError = deviceHost_->openDevice(savedDevice.get(), options_.audioApi);
    deviceHost_->getDeviceManager().addChangeListener(this); // persist later changes
    logStartupInformation(deviceError);

    // Plug-ins: the scanner executable sits next to this one.
    const auto exe = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
#if JUCE_WINDOWS
    const auto scanner = exe.getSiblingFile("dojo-plugin-scanner.exe");
#else
    const auto scanner = exe.getSiblingFile("dojo-plugin-scanner");
#endif
    plugins_ = std::make_unique<plugins::PluginManager>(
        scanner, settings_->getUserSettings()->getFile().getSiblingFile("PluginList.xml"));

    // The document: every change is compiled into a render graph and handed to the engine. Devices are
    // created by type: native devices directly, hosted plug-ins through the plug-in manager.
    project_ = std::make_unique<session::Project>(
        [controller = controller_.get()](std::unique_ptr<engine::RenderGraph> graph)
        { controller->publishGraph(std::move(graph)); },
        [pluginManager = plugins_.get()](const juce::ValueTree& device, double sampleRate, int blockSize,
                                         juce::String& error) -> std::unique_ptr<juce::AudioProcessor>
        {
            const auto type = session::device::getType(device);
            if (devices::isBuiltinType(type))
                return devices::createBuiltinDevice(type);

            if (type == plugins::PluginManager::kDeviceType)
            {
                juce::PluginDescription description;
                const auto xml = juce::parseXML(device.getProperty(session::ids::description).toString());
                if (xml == nullptr || !description.loadFromXml(*xml))
                {
                    error = "The plug-in description is missing.";
                    return nullptr;
                }
                return pluginManager->createInstance(description, sampleRate, blockSize, error);
            }

            error = "Unknown device type: " + type;
            return nullptr;
        });

    // Keep devices, clip stretchers and streaming in step with the audio device.
    auto& devices = project_->getDevices();
    devices.setPlayHead(engine_->getPlayHead());
    project_->getAssets().setEpochSource([engine = engine_.get()] { return engine->getProcessEpoch(); });
    engine_->onPrepare = [this](double sampleRate, int blockSize)
    {
        project_->getDevices().prepareAll(sampleRate, blockSize);
        juce::MessageManager::callAsync([this, sampleRate, blockSize]
                                        {
                                            if (project_ != nullptr)
                                                project_->getCompiler().setRenderFormat(sampleRate, blockSize);
                                        });
    };
    if (engine_->isPrepared())
    {
        devices.prepareAll(engine_->getSampleRate(), engine_->getMaximumBlockSize());
        project_->getCompiler().setRenderFormat(engine_->getSampleRate(), engine_->getMaximumBlockSize());
    }

    recorder_ = std::make_unique<session::Recorder>(*project_, *controller_);
    exporter_ = std::make_unique<session::Exporter>(*project_, *controller_);
    preview_ = std::make_unique<engine::PreviewService>(*engine_, project_->getAssets().getFormatManager());

    if (options_.testTone)
        controller_->setTestToneEnabled(true);

    commandManager_ = std::make_unique<juce::ApplicationCommandManager>();
    services_ = std::make_unique<ui::AppServices>(ui::AppServices{*project_, *controller_, *commandManager_, *plugins_,
                                                                   *recorder_, *exporter_, *settings_, *preview_,
                                                                   *preferences_, {}, {}, {}});
    mainWindow_ = std::make_unique<MainWindow>(*services_);

    if (options_.sessionFile != juce::File())
        mainWindow_->getMainComponent().openSessionFile(options_.sessionFile);

    if (!options_.audioFiles.isEmpty())
        mainWindow_->getMainComponent().importAudioFiles(options_.audioFiles);

    if (options_.demo)
        mainWindow_->getMainComponent().openDemoProject();

    if (options_.play)
        controller_->play();

    if (options_.smokeTestSeconds > 0)
    {
        smokeTest_ = std::make_unique<SmokeTest>(options_.smokeTestSeconds, *mainWindow_, *commandManager_,
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
    if (preferences_ != nullptr)
        preferences_->removeChangeListener(this);

    smokeTest_.reset();
    mainWindow_.reset(); // 1. UI goes first: nothing can post commands any more.
    services_.reset();
    commandManager_.reset();
    exporter_.reset();
    recorder_.reset();
    preview_.reset();
    if (engine_ != nullptr)
        engine_->onPrepare = nullptr;
    deviceHost_.reset(); // 2. Stops the driver: no audio callback can be in flight after this.
    project_.reset(); //    The document; graphs still in the engine keep their devices alive.
    plugins_.reset();
    controller_.reset();
    engine_.reset(); // 3. Now the real-time state (and the last graphs) can be destroyed.

    preferences_.reset();
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

void DojoApplication::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (source == preferences_.get())
    {
        if (controller_ != nullptr)
            controller_->setStopAtEnd(preferences_->getStopAtEnd());
        applySurroundMonitoring();
        return;
    }

    // The audio device configuration changed (e.g. in Audio Settings): remember it for next launch.
    if (deviceHost_ == nullptr || settings_ == nullptr)
        return;
    applySurroundMonitoring(); // (the channel order depends on the driver)

    if (const auto state = deviceHost_->createStateXml())
    {
        settings_->getUserSettings()->setValue(kAudioDeviceSettingKey, state.get());
        settings_->saveIfNeeded();
    }
}

void DojoApplication::applySurroundMonitoring()
{
    if (controller_ == nullptr || preferences_ == nullptr)
        return;
    using Mode = engine::command::SurroundMonitoring;
    Mode mode = Mode::foldDown;
    switch (preferences_->getSurroundMode())
    {
    case ui::Preferences::SurroundMode::standard:    mode = Mode::standard; break;
    case ui::Preferences::SurroundMode::foldDown:    mode = Mode::foldDown; break;
    case ui::Preferences::SurroundMode::virtualizer: mode = Mode::virtualizer; break;
    case ui::Preferences::SurroundMode::device:      mode = Mode::device; break;
    }
    // ALSA orders surround channels its own way; every other driver uses the WAVE order.
    const bool alsa = deviceHost_ != nullptr && deviceHost_->getStatus().deviceType == "ALSA";
    controller_->setSurroundMonitoring(mode, preferences_->getVirtualizerSpeakers(), preferences_->getVirtualizerRoom(), alsa);
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
