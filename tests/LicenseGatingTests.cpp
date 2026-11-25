#include "devices/DeviceRegistry.h"
#include "engine/AudioEngine.h"
#include "engine/EngineController.h"
#include "engine/io/AudioDeviceHost.h"
#include "engine/io/OutputGuard.h"
#include "licensing/Features.h"
#include "licensing/LicenseManager.h"
#include "session/Exporter.h"
#include "session/Presets.h"
#include "session/Project.h"
#include "session/Recorder.h"
#include "TestAudio.h"
#include "ui/AppServices.h"
#include "ui/Commands.h"
#include "ui/DeviceActions.h"
#include "ui/EditorState.h"
#include "ui/LicenseWindow.h"
#include "ui/MainComponent.h"
#include "ui/PresetActions.h"
#include "ui/Preferences.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>

using namespace dojo;
using licensing::Feature;
using licensing::Tier;
namespace gate = licensing::gate;

namespace
{
/** Publishes a tier for one test and forgets it after (so other tests see everything allowed). */
struct ScopedTier
{
    explicit ScopedTier(Tier tier) { gate::publish(tier); }
    ~ScopedTier() { gate::reset(); }
};

const licensing::crypto::Seed kSeed = []
{
    licensing::crypto::Seed seed{};
    for (std::size_t i = 0; i < seed.size(); ++i)
        seed[i] = static_cast<std::uint8_t>(i * 7 + 3);
    return seed;
}();

std::unique_ptr<licensing::LicenseManager> makeManager(bool publish, std::int64_t& now)
{
    licensing::LicenseManager::Config config;
    config.publicKey = licensing::crypto::publicKeyOf(kSeed);
    config.keyStore = std::make_unique<licensing::MemoryMarkerStore>();
    config.ledgerStores.push_back(std::make_unique<licensing::MemoryMarkerStore>());
    config.machineId = "test";
    config.clock = [&now] { return now; };
    config.publishToGate = publish;
    return std::make_unique<licensing::LicenseManager>(std::move(config));
}

/** Runs a block of a constant signal through the guard; the peak of what came out. */
float run(engine::OutputGuard& guard, int numSamples, float level, int blockSize = 512)
{
    std::vector<float> l(static_cast<std::size_t>(blockSize)), r(static_cast<std::size_t>(blockSize));
    float* out[2] = {l.data(), r.data()};
    float peakDifference = 0.0f;
    for (int done = 0; done < numSamples; done += blockSize)
    {
        std::fill(l.begin(), l.end(), level);
        std::fill(r.begin(), r.end(), level);
        guard.process(out, 2, blockSize);
        for (int i = 0; i < blockSize; ++i)
            peakDifference = std::max({peakDifference, std::abs(l[static_cast<std::size_t>(i)] - level), std::abs(r[static_cast<std::size_t>(i)] - level)});
    }
    return peakDifference;
}
} // namespace

TEST_CASE("License gating: what reaches the sound card", "[licensing][engine]")
{
    constexpr double kRate = 48'000.0;
    engine::OutputGuard guard;
    guard.prepare(kRate);
    CHECK(guard.getPolicy() == engine::OutputGuard::Policy::mute); // (muted until the license says otherwise)

    std::vector<float> l(256, 0.5f), r(256, 0.5f);
    float* out[2] = {l.data(), r.data()};
    guard.process(out, 2, 256);
    CHECK(l[100] == 0.0f);
    CHECK(r[255] == 0.0f);

    guard.setPolicy(engine::OutputGuard::Policy::clean);
    CHECK(run(guard, 48'000, 0.5f) == 0.0f);

    SECTION("the trial's watermark: after 15 seconds of sound, then every 30 seconds")
    {
        guard.setPolicy(engine::OutputGuard::Policy::watermark);
        // Silence does not count, and never gets a burst.
        CHECK(run(guard, static_cast<int>(60 * kRate), 0.0f) == 0.0f);
        CHECK(guard.getWatermarkCount() == 0u);
        CHECK(run(guard, static_cast<int>(14 * kRate), 0.2f) == 0.0f);
        CHECK(guard.getWatermarkCount() == 0u);
        CHECK(run(guard, static_cast<int>(2 * kRate), 0.2f) > 0.05f); // the burst
        CHECK(guard.getWatermarkCount() == 1u);
        CHECK(run(guard, static_cast<int>(27 * kRate), 0.2f) == 0.0f);
        CHECK(run(guard, static_cast<int>(4 * kRate), 0.2f) > 0.05f);
        CHECK(guard.getWatermarkCount() == 2u);
        // Every channel the device plays carries it (direct outputs, sends, loopbacks alike).
        std::vector<float> a(512, 0.3f), b(512, 0.3f), c(512, 0.3f);
        float* three[3] = {a.data(), b.data(), c.data()};
        engine::OutputGuard g3;
        g3.prepare(kRate);
        g3.setPolicy(engine::OutputGuard::Policy::watermark);
        bool touched[3] = {false, false, false};
        for (int block = 0; block < static_cast<int>(17 * kRate / 512); ++block)
        {
            std::fill(a.begin(), a.end(), 0.3f);
            std::fill(b.begin(), b.end(), 0.3f);
            std::fill(c.begin(), c.end(), 0.3f);
            g3.process(three, 3, 512);
            for (int ch = 0; ch < 3; ++ch)
                for (int i = 0; i < 512; ++i)
                    touched[ch] = touched[ch] || std::abs(three[ch][i] - 0.3f) > 0.01f;
        }
        CHECK((touched[0] && touched[1] && touched[2]));
    }

    SECTION("the device host starts muted")
    {
        engine::AudioEngine engine(0);
        engine::AudioDeviceHost host(engine);
        CHECK(host.getOutputPolicy() == engine::OutputGuard::Policy::mute);
        host.setOutputPolicy(engine::OutputGuard::Policy::watermark);
        CHECK(host.getOutputPolicy() == engine::OutputGuard::Policy::watermark);
    }
}

TEST_CASE("License gating: formats, panners and pro devices under Basic", "[licensing][session]")
{
    session::Session session;
    auto track = session.addTrack(session::TrackKind::audio, "Track");
    {
        ScopedTier basic(Tier::basic);
        CHECK(session::isFormatLicensed(session::TrackFormat::stereo));
        CHECK_FALSE(session::isFormatLicensed(session::TrackFormat::surround51));
        CHECK_FALSE(session::isFormatLicensed(session::TrackFormat::surround714));
        CHECK(session::whyFormatUnlicensed(session::TrackFormat::surround714).contains("Atmos"));

        session.setMasterFormat(session::TrackFormat::surround51);
        CHECK(session.getMasterFormat() == session::TrackFormat::stereo);
        session::track::setFormat(track, session::TrackFormat::surround71, nullptr);
        CHECK(session::track::getFormat(track) != session::TrackFormat::surround71);
        session::track::setFormat(track, session::TrackFormat::mono, nullptr);
        CHECK(session::track::getFormat(track) == session::TrackFormat::mono);
        session::track::setPan3d(track, true, nullptr);
        CHECK_FALSE(session::track::isPan3d(track));
        session::track::setObject(track, true, nullptr);
        CHECK_FALSE(session::track::isObject(track));

        // The pro devices: refused when added, greyed in menus; a chain with one is refused whole.
        ui::EditorState editor;
        const auto natives = ui::deviceActions::getNativeChoices();
        int pro = 0;
        for (const auto& choice : natives)
            if (licensing::isProDevice(choice.type))
            {
                ++pro;
                CHECK_FALSE(ui::deviceActions::isLicensed(choice));
                CHECK_FALSE(ui::deviceActions::addDevice(session, editor, track, choice).isValid());
            }
        CHECK(pro == 5);
        for (const auto& preset : session::presets::getAll())
            if (preset.name == "Punchy Drum Bus") // (The Glue)
                CHECK_FALSE(ui::presetActions::isLicensed(preset));
            else if (preset.name == "Pop Vocal Polish")
                CHECK(ui::presetActions::isLicensed(preset));
    }
    {
        ScopedTier pro(Tier::pro);
        session.setMasterFormat(session::TrackFormat::surround51);
        CHECK(session.getMasterFormat() == session::TrackFormat::surround51);
        for (const auto& choice : ui::deviceActions::getNativeChoices())
            CHECK(ui::deviceActions::isLicensed(choice));
    }
    {
        ScopedTier trial(Tier::demo); // (the trial has everything but exporting)
        CHECK(session::isFormatLicensed(session::TrackFormat::surround916));
        CHECK(gate::allows(Feature::proDevices));
        CHECK_FALSE(gate::allows(Feature::exporting));
    }
}

TEST_CASE("License gating: exports, commands and the lock", "[licensing][ui]")
{
    juce::MessageManager::getInstance();
    test::TempFolder temp;
    engine::AudioEngine engine(0);
    engine.prepare(48'000.0, 256, 2);
    engine::AudioDeviceHost host(engine);
    engine::EngineController controller(engine, host);
    session::Project project([&](std::unique_ptr<engine::RenderGraph> graph) { controller.publishGraph(std::move(graph)); },
                             [](const juce::ValueTree& d, double, int, juce::String&)
                             { return devices::createBuiltinDevice(session::device::getType(d)); });
    project.getCompiler().setRenderFormat(48'000.0, 256);
    plugins::PluginManager plugins(juce::File(), temp.dir.getChildFile("plugins.xml"));
    session::Recorder recorder(project, controller);
    session::Exporter exporter(project, controller);
    juce::ApplicationProperties settings;
    juce::PropertiesFile::Options options;
    options.applicationName = "test";
    options.filenameSuffix = ".settings";
    options.folderName = temp.dir.getFullPathName();
    settings.setStorageParameters(options);
    engine::PreviewService preview(engine, project.getAssets().getFormatManager());
    ui::Preferences preferences(*settings.getUserSettings());
    juce::ApplicationCommandManager commands;
    ui::AppServices services{project, controller, commands, plugins, recorder, exporter, settings, preview, preferences, {}, {}, {}};

    session::ExportSettings exportSettings;
    exportSettings.destination = temp.dir.getChildFile("mix.wav");
    exportSettings.endSeconds = 1.0;

    SECTION("the exporter refuses what the license does not allow")
    {
        {
            ScopedTier trial(Tier::demo);
            CHECK(exporter.start(exportSettings).contains("free trial"));
        }
        {
            ScopedTier none(Tier::none);
            CHECK(exporter.start(exportSettings).isNotEmpty());
        }
        {
            ScopedTier basic(Tier::basic);
            auto adm = exportSettings;
            adm.format = session::ExportSettings::Format::admBwf;
            CHECK(exporter.start(adm).contains("Atmos"));
        }
        {
            ScopedTier pro(Tier::pro);
            project.getSession().setMasterFormat(session::TrackFormat::surround51);
        }
        {
            ScopedTier basic(Tier::basic);
            CHECK(exporter.start(exportSettings).contains("5.1"));
        }
        CHECK_FALSE(exporter.isRunning());
        CHECK_FALSE(exportSettings.destination.existsAsFile());
    }

    SECTION("locked, only full screen and the license window work; the trial cannot export")
    {
        services.toggleFullScreen = [] {};
        services.isFullScreen = [] { return false; };
        services.showLicense = [] {};
        ui::MainComponent main(services);
        main.setSize(1400, 900);
        commands.registerAllCommandsForTarget(&main);
        commands.setFirstCommandTarget(&main);
        const auto active = [&](juce::CommandID id)
        {
            juce::ApplicationCommandInfo info(id);
            main.getCommandInfo(id, info);
            return (info.flags & juce::ApplicationCommandInfo::isDisabled) == 0;
        };
        {
            ScopedTier none(Tier::none);
            for (const auto id : {ui::commands::playStop, ui::commands::record, ui::commands::exportAudio, ui::commands::newSession,
                                  ui::commands::openSession, ui::commands::preferences, ui::commands::addTrack, ui::commands::importAudio})
                CHECK_FALSE(active(id));
            CHECK(active(ui::commands::toggleFullScreen));
            CHECK(active(ui::commands::showLicense));
            // A shortcut (or a control surface) invoking a command directly does nothing either.
            const auto tracksBefore = project.getSession().getAllTracks().size();
            CHECK_FALSE(commands.invokeDirectly(ui::commands::addTrack, false));
            CHECK(project.getSession().getAllTracks().size() == tracksBefore);
        }
        {
            ScopedTier trial(Tier::demo);
            CHECK_FALSE(active(ui::commands::exportAudio));
            CHECK(active(ui::commands::playStop));
            CHECK(active(ui::commands::addTrack));
        }
        {
            ScopedTier basic(Tier::basic);
            CHECK(active(ui::commands::exportAudio));
        }
        commands.setFirstCommandTarget(nullptr);
    }

    SECTION("the license window: activation, and the free-trial page")
    {
        std::int64_t now = 1'800'000'000'000;
        auto license = makeManager(false, now);
        ui::LicensePanel panel(*license, false);
        juce::String opened;
        panel.openUrl = [&](const juce::String& url) { opened = url; };
        panel.openFreeTrialPage();
        CHECK(opened == ui::kFreeTrialUrl);
        CHECK(opened.contains("github.com"));

        panel.setKeyText("DOJO1-NOT-A-KEY");
        panel.activate();
        CHECK(license->getTier() == Tier::none);
        CHECK(panel.getMessage().isNotEmpty());

        panel.setKeyText(licensing::makeKey({Tier::basic, 321}, kSeed));
        panel.activate();
        CHECK(license->getTier() == Tier::basic);
        CHECK(panel.getStatusText().contains("Basic"));
    }

    SECTION("the lock lets through only the window's close and maximise buttons")
    {
        std::int64_t now = 1'800'000'000'000;
        auto license = makeManager(false, now);
        juce::DocumentWindow window("Test", juce::Colours::black, juce::DocumentWindow::allButtons);
        window.setUsingNativeTitleBar(false);
        window.setSize(800, 600);
        ui::LicenseLock lock(*license, window);
        CHECK(lock.canModalEventBeSentToComponent(window.getCloseButton()));
        CHECK(lock.canModalEventBeSentToComponent(window.getMaximiseButton()));
        CHECK(lock.canModalEventBeSentToComponent(&window));
        CHECK_FALSE(lock.canModalEventBeSentToComponent(window.getMinimiseButton()));
        juce::TextButton other;
        CHECK_FALSE(lock.canModalEventBeSentToComponent(&other));
    }
}

TEST_CASE("License gating: the pro devices follow the license, their settings kept", "[licensing][session]")
{
    juce::MessageManager::getInstance();
    session::Project project([](std::unique_ptr<engine::RenderGraph>) {},
                             [](const juce::ValueTree& d, double, int, juce::String&)
                             { return devices::createBuiltinDevice(session::device::getType(d)); });
    auto& session = project.getSession();
    auto track = session.addTrack(session::TrackKind::instrument, "Keys");
    auto rhodes = session.addDevice(track, "builtin:rhodes73", "Rhodes");
    auto eq = session.addDevice(track, "builtin:eq", "EQ");
    auto& devices = project.getDevices();
    const auto id = session::device::getId(rhodes);

    {
        ScopedTier basic(Tier::basic);
        CHECK(devices.getNode(rhodes) == nullptr);
        CHECK(devices.getError(id).contains("Pro"));
        CHECK(devices.getNode(eq) != nullptr); // (everything else as usual)
    }
    {
        ScopedTier pro(Tier::pro);
        devices.forgetFailed();
        REQUIRE(devices.getNode(rhodes) != nullptr);
        CHECK(devices.getError(id).isEmpty());
    }
    {
        // Downgraded: the running instance goes (its state first written into the project).
        ScopedTier basic(Tier::basic);
        devices.forgetTypes(licensing::getProDeviceTypes());
        CHECK(rhodes.getProperty(session::ids::state).toString().isNotEmpty());
        CHECK(devices.findNode(id) == nullptr);
        CHECK(devices.getNode(rhodes) == nullptr);
        CHECK(devices.getNode(eq) != nullptr);
    }
}
