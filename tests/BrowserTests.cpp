#include "engine/AudioEngine.h"
#include "session/Presets.h"
#include "ui/BrowserDrag.h"
#include "ui/components/BrowserPanel.h"
#include "TestAudio.h"

#include <catch2/catch_test_macros.hpp>

using namespace dojo;

namespace
{
/** A browser with its own settings file, plug-in list and (prepared) engine. */
struct BrowserRig
{
    BrowserRig()
    {
        juce::MessageManager::getInstance();
        formats.registerBasicFormats();
        engine.prepare(48'000.0, 256, 2);
        juce::PropertiesFile::Options options;
        options.applicationName = "settings";
        options.filenameSuffix = ".xml";
        options.storageFormat = juce::PropertiesFile::storeAsXML;
        settings = std::make_unique<juce::PropertiesFile>(temp.dir.getChildFile("settings.xml"), options);
        plugins = std::make_unique<plugins::PluginManager>(juce::File(), temp.dir.getChildFile("plugins.xml"));
        preview = std::make_unique<engine::PreviewService>(engine, formats);
    }

    ~BrowserRig()
    {
        preview.reset();
        engine.release();
    }

    std::unique_ptr<ui::BrowserPanel> makeBrowser()
    {
        auto browser = std::make_unique<ui::BrowserPanel>(formats.getWildcardForAllFormats(), *plugins, *preview, *settings);
        browser->setSize(240, 800);
        return browser;
    }

    juce::File makeSampleFolder()
    {
        const auto root = temp.dir.getChildFile("Samples");
        root.getChildFile("Kicks").createDirectory();
        root.getChildFile("Snares").createDirectory();
        test::writeWav(root.getChildFile("Kicks/Kick 01.wav"), test::sine(60.0, 48'000.0, 4800), 48'000.0);
        test::writeWav(root.getChildFile("Kicks/Kick 02 Hard.wav"), test::sine(60.0, 48'000.0, 4800), 48'000.0);
        test::writeWav(root.getChildFile("Snares/Snare Tight.wav"), test::sine(200.0, 48'000.0, 4800), 48'000.0);
        root.getChildFile("Snares/notes.txt").replaceWithText("not audio");
        return root;
    }

    test::TempFolder temp;
    juce::AudioFormatManager formats;
    engine::AudioEngine engine{0};
    std::unique_ptr<juce::PropertiesFile> settings;
    std::unique_ptr<plugins::PluginManager> plugins;
    std::unique_ptr<engine::PreviewService> preview;
};

bool waitForSearch(ui::BrowserPanel& browser)
{
    return test::pumpMessagesUntil([&] { return !browser.isSearching(); });
}
} // namespace

TEST_CASE("Browser starts without folders and remembers the ones added", "[browser]")
{
    BrowserRig rig;
    const auto samples = rig.makeSampleFolder();

    auto browser = rig.makeBrowser();
    CHECK(browser->getFolders().isEmpty());

    browser->addFolder(samples);
    browser->addFolder(samples); // no duplicates
    browser->addFolder(rig.temp.dir.getChildFile("does not exist"));
    REQUIRE(browser->getFolders().size() == 1);

    // A new browser (next start of the application) has the same folders.
    browser.reset();
    browser = rig.makeBrowser();
    REQUIRE(browser->getFolders().size() == 1);
    CHECK(browser->getFolders()[0] == samples);

    browser->removeFolder(samples);
    CHECK(browser->getFolders().isEmpty());
    browser.reset();
    CHECK(rig.makeBrowser()->getFolders().isEmpty());
}

TEST_CASE("Browser folder search finds audio files in every added folder", "[browser]")
{
    BrowserRig rig;
    const auto samples = rig.makeSampleFolder();
    const auto more = rig.temp.dir.getChildFile("More");
    more.createDirectory();
    test::writeWav(more.getChildFile("kick deep.wav"), test::sine(50.0, 48'000.0, 4800), 48'000.0);

    auto browser = rig.makeBrowser();
    browser->addFolder(samples);
    browser->addFolder(more);

    browser->setFolderSearch("kick");
    REQUIRE(waitForSearch(*browser));
    CHECK(browser->getSearchResults().size() == 3);

    browser->setFolderSearch("kick hard"); // every word must match
    REQUIRE(waitForSearch(*browser));
    REQUIRE(browser->getSearchResults().size() == 1);
    CHECK(browser->getSearchResults()[0].getFileName() == "Kick 02 Hard.wav");

    browser->setFolderSearch("snares"); // folder names count
    REQUIRE(waitForSearch(*browser));
    REQUIRE(browser->getSearchResults().size() == 1); // not notes.txt
    CHECK(browser->getSearchResults()[0].getFileName() == "Snare Tight.wav");

    browser->setFolderSearch("nothing like this");
    REQUIRE(waitForSearch(*browser));
    CHECK(browser->getSearchResults().isEmpty());

    browser->setFolderSearch({});
    CHECK(browser->getSearchResults().isEmpty());
}

TEST_CASE("Browser device search filters devices and plug-ins", "[browser]")
{
    BrowserRig rig;
    auto browser = rig.makeBrowser();

    const auto all = browser->getListedDeviceNames();
    CHECK(all.contains("Drum Rack"));
    CHECK(all.contains("EQ"));
    CHECK(all.contains("Compressor"));

    browser->setDeviceSearch("comp");
    CHECK(browser->getListedDeviceNames() == juce::StringArray{"Compressor", "The Glue"}); // (The Glue: a bus compressor)

    browser->setDeviceSearch("sibilance"); // more words each device is known by
    CHECK(browser->getListedDeviceNames() == juce::StringArray{"De-Esser"});

    browser->setDeviceSearch("dynamics"); // categories count
    CHECK(browser->getListedDeviceNames().contains("Limiter"));

    browser->setDeviceSearch("no such device");
    CHECK(browser->getListedDeviceNames().isEmpty());

    browser->setDeviceSearch({});
    CHECK(browser->getListedDeviceNames().size() == all.size());
}

TEST_CASE("The browser lists presets (built-in and the user's), searchable and draggable", "[browser][presets]")
{
    BrowserRig rig;
    const auto folder = rig.temp.dir.getChildFile("Presets");
    session::presets::setUserFolder(folder);
    auto browser = rig.makeBrowser();
    // Chains first, then device presets.
    const auto builtIn = browser->getListedPresetNames();
    REQUIRE(builtIn.size() == 10);
    CHECK(builtIn[0] == "Dreamy Wide Space");
    CHECK(builtIn.contains("Drivers License Vocal Saturation"));
    CHECK(builtIn[9] == "Synth Bite");
    CHECK(browser->getPresetDragDescription("Synth Bite") == "DojoPreset:builtin:Synth Bite");
    CHECK(browser->getPresetDragDescription("Old Radio") == "DojoPreset:builtin:Old Radio");

    // A preset saved into the presets folder (by The Dojo or copied there) shows up.
    session::presets::Preset mine;
    mine.name = "Glassy Pad";
    session::presets::DeviceSettings settings;
    settings.key = "builtin:reverb";
    settings.deviceName = "Reverb";
    juce::MemoryBlock state;
    juce::AudioProcessor::copyXmlToBinary(juce::XmlElement("DOJO_DEVICE"), state);
    settings.state = state;
    mine.put(settings);
    REQUIRE(mine.save(folder.getChildFile("Pads/Glassy Pad")).wasOk());
    browser->refreshPresets();
    auto listed = browser->getListedPresetNames();
    REQUIRE(listed.size() == 11);
    CHECK(listed[9] == "Synth Bite");
    CHECK(listed[10] == "Glassy Pad");
    CHECK(browser->getPresetDragDescription("Glassy Pad") ==
          "DojoPreset:" + folder.getChildFile("Pads/Glassy Pad.dojopreset").getFullPathName());

    // Search: by name, what it is for, the devices in it (by role or kind), "preset" or "chain".
    browser->setDeviceSearch("bite");
    CHECK(browser->getListedPresetNames() == juce::StringArray{"Synth Bite"});
    browser->setDeviceSearch("channel eq");
    CHECK(browser->getListedPresetNames() == juce::StringArray{"Synth Bite"});
    CHECK(browser->getListedDeviceNames() == juce::StringArray{"Channel EQ"});
    browser->setDeviceSearch("reverb");
    listed = browser->getListedPresetNames();
    CHECK(listed.contains("Glassy Pad"));
    CHECK(listed.contains("Drivers License Vocal Saturation"));
    CHECK_FALSE(listed.contains("Old Radio"));
    browser->setDeviceSearch("slapback");
    CHECK(browser->getListedPresetNames() == juce::StringArray{"Drivers License Vocal Saturation"});
    browser->setDeviceSearch("vocals");
    CHECK(browser->getListedPresetNames() ==
          juce::StringArray{"Drivers License Vocal Saturation", "Pop Vocal Polish", "Upfront Rap Vocal"});
    browser->setDeviceSearch("drums chain");
    CHECK(browser->getListedPresetNames() == juce::StringArray{"Dusty Lo-Fi Drums", "Punchy Drum Bus"});
    browser->setDeviceSearch("preset");
    CHECK(browser->getListedPresetNames() == juce::StringArray{"Synth Bite", "Glassy Pad"});
    browser->setDeviceSearch({});
    session::presets::setUserFolder({});
}

TEST_CASE("Selecting a file previews it unless preview is switched off", "[browser]")
{
    BrowserRig rig;
    const auto samples = rig.makeSampleFolder();
    auto browser = rig.makeBrowser();
    browser->addFolder(samples);

    const auto kick = samples.getChildFile("Kicks/Kick 01.wav");
    browser->fileSelected(kick);
    CHECK(rig.preview->getCurrentFile() == kick);

    browser->setPreviewEnabled(false);
    rig.preview->stop();
    browser->fileSelected(samples.getChildFile("Snares/Snare Tight.wav"));
    CHECK(rig.preview->getCurrentFile() == kick); // unchanged

    // The setting is remembered.
    browser.reset();
    CHECK_FALSE(rig.makeBrowser()->isPreviewEnabled());
}

TEST_CASE("Browser drags carry the dragged files", "[browser]")
{
    test::TempFolder temp;
    const auto a = test::writeWav(temp.dir.getChildFile("a.wav"), test::sine(100.0, 48'000.0, 480), 48'000.0);
    const auto b = test::writeWav(temp.dir.getChildFile("b.wav"), test::sine(100.0, 48'000.0, 480), 48'000.0);
    const auto missing = temp.dir.getChildFile("gone.wav");

    const auto description = ui::browserDrag::describe({a.getFullPathName(), missing.getFullPathName(), b.getFullPathName()});
    CHECK(ui::browserDrag::isFileDrag(description));
    CHECK_FALSE(ui::browserDrag::isFileDrag(juce::var("DojoDevice:builtin:eq")));
    CHECK(ui::browserDrag::getFiles(description) == juce::StringArray{a.getFullPathName(), b.getFullPathName()});
}
