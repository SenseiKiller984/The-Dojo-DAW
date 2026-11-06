#include "devices/DeviceRegistry.h"
#include "engine/AudioEngine.h"
#include "engine/EngineController.h"
#include "engine/io/AudioDeviceHost.h"
#include "session/Exporter.h"
#include "session/Project.h"
#include "session/Recorder.h"
#include "ui/AppServices.h"
#include "ui/Commands.h"
#include "ui/MainComponent.h"
#include "ui/Preferences.h"
#include "ui/theme/DojoTheme.h"
#include "ui/theme/Themes.h"
#include "TestAudio.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <map>
#include <set>

using namespace dojo;

namespace
{
std::unique_ptr<juce::PropertiesFile> makeSettings(const juce::File& file)
{
    juce::PropertiesFile::Options options;
    options.applicationName = "test";
    options.storageFormat = juce::PropertiesFile::storeAsXML;
    return std::make_unique<juce::PropertiesFile>(file, options);
}

/** WCAG relative luminance and contrast ratio. */
double luminance(juce::Colour c)
{
    const auto channel = [](juce::uint8 v)
    {
        const double s = v / 255.0;
        return s <= 0.03928 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(c.getRed()) + 0.7152 * channel(c.getGreen()) + 0.0722 * channel(c.getBlue());
}

double contrast(juce::Colour a, juce::Colour b)
{
    const double la = luminance(a), lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

/** Restores the default theme when a test that applies themes ends. */
struct ThemeGuard
{
    ~ThemeGuard() { ui::theme::applyTheme(ui::theme::findTheme(ui::theme::getDefaultThemeName())); }
};
} // namespace

TEST_CASE("Preferences: defaults, persistence and change notifications", "[ui][preferences]")
{
    juce::MessageManager::getInstance();
    test::TempFolder temp;
    const auto file = temp.dir.getChildFile("settings.xml");

    {
        auto settings = makeSettings(file);
        ui::Preferences preferences(*settings);
        CHECK_FALSE(preferences.getReturnViewToStart());
        CHECK_FALSE(preferences.getStopAtEnd());
        CHECK(preferences.getThemeName() == ui::theme::getDefaultThemeName());
        CHECK(preferences.getKeyMappings() == nullptr);

        struct Counter : juce::ChangeListener
        {
            int count = 0;
            void changeListenerCallback(juce::ChangeBroadcaster*) override { ++count; }
        } counter;
        preferences.addChangeListener(&counter);

        preferences.setReturnViewToStart(true);
        preferences.setStopAtEnd(true);
        preferences.setThemeName("Sakura Night");
        REQUIRE(test::pumpMessagesUntil([&] { return counter.count > 0; }));

        juce::XmlElement mappings("KEYMAPPINGS");
        mappings.setAttribute("basedOnDefaults", 1);
        preferences.setKeyMappings(mappings);
        preferences.removeChangeListener(&counter);
    }

    // The next start of the application sees the same preferences.
    auto settings = makeSettings(file);
    ui::Preferences preferences(*settings);
    CHECK(preferences.getReturnViewToStart());
    CHECK(preferences.getStopAtEnd());
    CHECK(preferences.getThemeName() == "Sakura Night");
    REQUIRE(preferences.getKeyMappings() != nullptr);
    CHECK(preferences.getKeyMappings()->hasTagName("KEYMAPPINGS"));

    // Unknown theme names fall back to the default.
    preferences.setThemeName("No Such Theme");
    CHECK(preferences.getThemeName() == ui::theme::getDefaultThemeName());
}

TEST_CASE("Themes: every requested style, readable everywhere", "[ui][themes]")
{
    const auto& themes = ui::theme::getThemes();
    std::map<juce::String, int> perFamily;
    std::set<juce::String> names;
    for (const auto& theme : themes)
    {
        ++perFamily[theme.family];
        names.insert(theme.name);
    }
    CHECK(names.size() == themes.size()); // unique names
    CHECK(names.count("Standard Black") == 1);
    CHECK(names.count("Standard White") == 1);
    CHECK(names.count(ui::theme::getDefaultThemeName()) == 1);
    CHECK(perFamily["Japanese"] >= 2);
    CHECK(perFamily["Cherry Blossom"] >= 2);
    CHECK(perFamily["Manga"] >= 2);

    for (const auto& theme : themes)
    {
        const auto& p = theme.palette;
        INFO(theme.name);
        // Text against every surface it is drawn on.
        for (const auto surface : {p.windowBackground, p.panel, p.panelRaised, p.panelHeader, p.laneEven})
        {
            CHECK(contrast(p.textPrimary, surface) >= 7.0);
            CHECK(contrast(p.textSecondary, surface) >= 3.0);
        }
        CHECK(contrast(p.textPrimary, p.control) >= 4.5);
        CHECK(theme.dark == (luminance(p.windowBackground) < 0.2));
        // Lit buttons (accent, play, record fills) keep readable text.
        for (const auto fill : {p.accent, p.play, p.record, p.gold})
            CHECK(contrast(ui::theme::readableOn(fill, p.textPrimary), fill) >= 3.0);
        CHECK(p.playhead.getAlpha() == 255);
    }

    // The established look is kept where it reads well: light text on Dojo Night's vermilion.
    const auto& dojo = ui::theme::findTheme("Dojo Night").palette;
    CHECK(ui::theme::readableOn(dojo.accent, dojo.textPrimary) == dojo.textPrimary);

    // Text on a colour (clip titles, lit buttons) is readable for every track colour.
    for (const auto colour : ui::theme::trackPalette)
        CHECK(contrast(ui::theme::textOn(colour), colour) >= 4.5);
}

TEST_CASE("Applying a theme changes the palette the UI paints with", "[ui][themes]")
{
    ThemeGuard guard;
    const auto& washi = ui::theme::findTheme("Washi");
    ui::theme::applyTheme(washi);
    CHECK(ui::theme::getActiveThemeName() == "Washi");
    CHECK(ui::theme::colour::windowBackground == washi.palette.windowBackground);
    CHECK(ui::theme::colour::textPrimary == washi.palette.textPrimary);
    CHECK(ui::theme::colour::accent == washi.palette.accent);

    ui::theme::applyTheme(ui::theme::findTheme("Manga Noir"));
    CHECK(ui::theme::colour::windowBackground == ui::theme::findTheme("Manga Noir").palette.windowBackground);
}

TEST_CASE("Stop twice (or Home) brings the view back to the start when that preference is on", "[ui][preferences]")
{
    juce::MessageManager::getInstance();
    test::TempFolder temp;

    engine::AudioEngine engine(0);
    engine::AudioDeviceHost host(engine);
    engine::EngineController controller(engine, host);
    session::Project project([&](std::unique_ptr<engine::RenderGraph> graph) { controller.publishGraph(std::move(graph)); },
                             [](const juce::ValueTree& d, double, int, juce::String&)
                             { return devices::createBuiltinDevice(session::device::getType(d)); });
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
    ui::AppServices services{project, controller, commands, plugins, recorder, exporter, settings, preview, preferences, {}};

    {
        ui::MainComponent main(services);
        main.setSize(1400, 900);
        commands.registerAllCommandsForTarget(&main);
        commands.setFirstCommandTarget(&main);
        auto& arranger = main.getArranger();

        // Off (the default): the playhead returns, the view stays where it was.
        arranger.scrollToTime(500.0);
        REQUIRE(commands.invokeDirectly(ui::commands::returnToStart, false));
        CHECK(arranger.getScrollSeconds() > 499.0);

        // On: the view goes back to the start as well.
        preferences.setReturnViewToStart(true);
        REQUIRE(commands.invokeDirectly(ui::commands::returnToStart, false));
        CHECK(arranger.getScrollSeconds() == 0.0);

        // The transport's Stop button, pressed while stopped (the second press of a double stop).
        arranger.scrollToTime(500.0);
        juce::Button* stop = nullptr;
        std::function<void(juce::Component&)> find = [&](juce::Component& c)
        {
            for (auto* child : c.getChildren())
            {
                if (auto* button = dynamic_cast<juce::Button*>(child); button != nullptr && button->getName() == "Stop")
                    stop = button;
                find(*child);
            }
        };
        find(main);
        REQUIRE(stop != nullptr);
        stop->triggerClick();
        CHECK(test::pumpMessagesUntil([&] { return arranger.getScrollSeconds() == 0.0; }, 3000));

        commands.setFirstCommandTarget(nullptr);
    }
}
