#include "devices/DeviceRegistry.h"
#include "engine/AudioEngine.h"
#include "engine/EngineController.h"
#include "engine/io/AudioDeviceHost.h"
#include "session/AraHost.h"
#include "session/Project.h"
#include "ui/EditorState.h"
#include "ui/components/ArrangerView.h"
#include "ui/components/ClipEditor.h"
#include "TestAudio.h"
#include "TestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace dojo;
using Catch::Matchers::WithinAbs;

TEST_CASE("ARA hosting is in the build", "[ara]")
{
#if JUCE_PLUGINHOST_ARA
    CHECK(session::AraHost::isAvailable());
    juce::PluginDescription description;
    description.hasARAExtension = true;
    CHECK(session::AraHost::isAraPlugin(description));
    description.hasARAExtension = false;
    CHECK_FALSE(session::AraHost::isAraPlugin(description));
#else
    CHECK_FALSE(session::AraHost::isAvailable());
#endif
}

#if JUCE_PLUGINHOST_ARA && defined(DOJO_TEST_ARA_PATH)
namespace
{
constexpr double kRate = 48'000.0;
constexpr int kBlock = 256;
constexpr double kTone = 440.0;
constexpr float kLevel = 0.2f;

std::size_t at(double seconds) { return static_cast<std::size_t>(std::llround(seconds * kRate)); }

/** The application's object graph with plug-in hosting (VST3), the engine driven by the test. */
struct App
{
    App()
        : host(engine)
        , controller(engine, host)
        , project([this](std::unique_ptr<engine::RenderGraph> graph) { controller.publishGraph(std::move(graph)); },
                  [this](const juce::ValueTree& d, double rate, int block, juce::String& error) -> std::unique_ptr<juce::AudioProcessor>
                  {
                      if (session::device::getType(d) != "plugin")
                          return devices::createBuiltinDevice(session::device::getType(d));
                      juce::PluginDescription description;
                      const auto xml = juce::parseXML(d.getProperty(session::ids::description).toString());
                      if (xml == nullptr || !description.loadFromXml(*xml))
                      {
                          error = "no description";
                          return nullptr;
                      }
                      return formats.createPluginInstance(description, rate, block, error);
                  })
    {
        formats.addFormat(std::make_unique<juce::VST3PluginFormat>());
        engine.prepare(kRate, kBlock, 2, 0);
        project.getDevices().setPlayHead(engine.getPlayHead());
        project.getDevices().prepareAll(kRate, kBlock);
        project.getCompiler().setRenderFormat(kRate, kBlock);
    }

    /** The test ARA plug-in's description, as a scan finds it. */
    juce::PluginDescription testPlugin()
    {
        juce::OwnedArray<juce::PluginDescription> found;
        juce::VST3PluginFormat format;
        format.findAllTypesForFile(found, DOJO_TEST_ARA_PATH);
        REQUIRE(found.size() == 1);
        return *found[0];
    }

    /** Waits until the track's ARA plug-in plays `regions` regions, and the graph is in the engine. */
    void settle(std::uint32_t trackId, bool rendering, int regions)
    {
        auto& ara = project.getAra();
        const auto ready = [&]
        {
            const auto info = ara.getTrackInfo(trackId);
            return info.rendering == rendering && info.regions == regions;
        };
        REQUIRE(test::pumpMessagesUntil(ready, 10'000));
        project.getCompiler().rebuildNow();
        idle(8);
    }

    /** A few blocks while stopped: the latest graph reaches the engine. */
    void idle(int blocks)
    {
        std::vector<float> l(kBlock), r(kBlock);
        float* outputs[2] = {l.data(), r.data()};
        for (int i = 0; i < blocks; ++i)
        {
            controller.pump();
            engine.process(nullptr, 0, outputs, 2, kBlock);
            engine.releaseRetiredGraphs();
        }
    }

    /** Plays from the start for `seconds`: the left output. */
    std::vector<float> play(double seconds)
    {
        std::vector<float> left;
        std::vector<float> l(kBlock), r(kBlock);
        float* outputs[2] = {l.data(), r.data()};
        controller.locateSeconds(0.0);
        controller.play();
        for (int done = 0; done < static_cast<int>(seconds * kRate); done += kBlock)
        {
            controller.pump();
            engine.process(nullptr, 0, outputs, 2, kBlock);
            engine.releaseRetiredGraphs();
            left.insert(left.end(), l.begin(), l.end());
        }
        controller.stop();
        idle(2);
        return left;
    }

    /** A parameter of the track's ARA plug-in (the test plug-in's counts: value * 64). */
    int count(std::uint32_t deviceId, int parameter)
    {
        auto node = project.getDevices().findNode(deviceId);
        REQUIRE(node != nullptr);
        return static_cast<int>(std::lround(node->getProcessor().getParameters()[parameter]->getValue() * 64.0f));
    }

    struct MessageThread
    {
        MessageThread() { juce::MessageManager::getInstance(); }
    } messageThread;
    juce::AudioPluginFormatManager formats;
    engine::AudioEngine engine;
    engine::AudioDeviceHost host;
    engine::EngineController controller;
    session::Project project;
};

/** What the test plug-in should play at timeline time `t` for a clip at `start` showing the tone
    from `offset`: the tone normalised to 0.5 with the peak `peak` it knows. */
float expected(double t, double start, double offset, float peak)
{
    const auto source = static_cast<double>(std::llround((t - start + offset) * kRate));
    return static_cast<float>(kLevel * std::sin(2.0 * test::kPi * kTone * source / kRate)) * (0.5f / peak);
}

/** The largest difference from what the plug-in should play over [from, to). */
double worst(const std::vector<float>& out, double from, double to, double start, double offset, float peak)
{
    double error = 0.0;
    for (auto i = at(from); i < at(to) && i < out.size(); ++i)
        error = std::max(error, static_cast<double>(std::abs(out[i] - expected(static_cast<double>(i) / kRate, start, offset, peak))));
    return error;
}

constexpr int kSelected = 0, kAnalysed = 1, kRestored = 2;
} // namespace

TEST_CASE("An ARA plug-in plays its track's clips from the audio it reads through the host", "[ara][plugins]")
{
    test::TempFolder temp;
    App app;
    auto& session = app.project.getSession();
    const auto description = app.testPlugin();
    REQUIRE(description.hasARAExtension);
    CHECK(session::AraHost::isAraPlugin(description));

    // A 3 s stereo tone at 0.2; a clip of it at 1.0 s on the timeline from 0.5 s into the file, 1 s long.
    const auto tone = test::sine(kTone, kRate, static_cast<int>(at(3.0)), kLevel);
    const auto file = test::writeWav(temp.dir.getChildFile("tone.wav"), tone, kRate, 2);
    juce::String error;
    REQUIRE(app.project.getAssets().loadNow(file, error) != nullptr);
    auto vocal = session.addTrack(session::TrackKind::audio, "Vocal");
    auto clip = session.addClip(vocal, file, 1.0, 1.0, 0.5);
    const auto trackId = session::track::getId(vocal);

    // Without an ARA plug-in the engine plays the clip.
    app.project.getCompiler().rebuildNow();
    app.idle(8);
    auto out = app.play(2.5);
    CHECK(test::peak(out, at(1.1), at(1.9)) > 0.15);
    CHECK(test::peak(out, at(1.1), at(1.9)) < 0.25);

    // Track > ARA Plug-in: bound before it is prepared, the clip becomes its playback region.
    auto device = session.setAraPlugin(vocal, description.createXml()->toString(), description.name);
    REQUIRE(device.isValid());
    CHECK(session::Session::getAraDevice(vocal) == device);
    app.project.getCompiler().rebuildNow(); // (creates the instance: bound there)
    app.settle(trackId, true, 1);
    const auto deviceId = session::device::getId(device);
    auto info = app.project.getAra().getTrackInfo(trackId);
    CHECK(info.bound);
    CHECK(info.plugin == "DojoTestAra");
    CHECK(info.deviceId == deviceId);
    CHECK(app.project.getAra().rendersClipsOf(trackId));

    // It read the whole file through the host (its analysis), and plays the region: the tone from
    // 0.5 s into the file at 1.0 s on the timeline, normalised to 0.5 - the engine plays nothing.
    out = app.play(2.5);
    CHECK(app.count(deviceId, kAnalysed) == 1);
    CHECK(worst(out, 1.0, 2.0, 1.0, 0.5, kLevel) < 2.0e-3);
    CHECK(test::peak(out, at(1.05), at(1.95)) > 0.45);
    CHECK(test::peak(out, 0, at(0.99)) < 1.0e-6);
    CHECK(test::peak(out, at(2.01), at(2.5)) < 1.0e-6);

    // Edits follow: moved to 1.5 s and showing the file from 1.0 s.
    session.moveClip(clip, vocal, 1.5);
    session::clip::setOffset(clip, 1.0, nullptr);
    app.project.getAra().synchronise();
    app.idle(4);
    out = app.play(3.0);
    CHECK(worst(out, 1.5, 2.5, 1.5, 1.0, kLevel) < 2.0e-3);
    CHECK(test::peak(out, at(1.0), at(1.49)) < 1.0e-6);

    // A second clip: a second region.
    auto second = session.addClip(vocal, file, 0.0, 0.5, 2.0);
    app.settle(trackId, true, 2);
    out = app.play(3.0);
    CHECK(worst(out, 0.0, 0.5, 0.0, 2.0, kLevel) < 2.0e-3);
    CHECK(worst(out, 1.5, 2.5, 1.5, 1.0, kLevel) < 2.0e-3);
    session.removeClip(second);
    app.settle(trackId, true, 1);

    // The clip editor's selection reaches the plug-in's editor view.
    app.project.getAra().showSelection(trackId, {session::clip::getId(clip)});
    CHECK(test::pumpMessagesUntil([&] { return app.count(deviceId, kSelected) == 1; }, 2'000));

    // Bypassed, the plug-in passes the track's own clips through.
    session::device::setBypassed(device, true, nullptr);
    app.settle(trackId, false, 1);
    CHECK_FALSE(app.project.getAra().rendersClipsOf(trackId));
    out = app.play(3.0);
    CHECK(test::peak(out, at(1.6), at(2.4)) > 0.15);
    CHECK(test::peak(out, at(1.6), at(2.4)) < 0.25); // (the clip as it is, not normalised)
    session::device::setBypassed(device, false, nullptr);
    app.settle(trackId, true, 1);

    // Moved out of the first slot, it is left out: the clips play as they are, it gets no regions.
    auto eq = session.addDevice(vocal, "eq", "EQ", 0);
    app.settle(trackId, false, 0);
    CHECK_FALSE(session::Session::getAraDevice(vocal).isValid());
    out = app.play(3.0);
    CHECK(test::peak(out, at(1.6), at(2.4)) > 0.15);
    CHECK(test::peak(out, at(1.6), at(2.4)) < 0.25);
    session.removeDevice(eq);
    app.settle(trackId, true, 1);
}

TEST_CASE("An ARA plug-in's edits are saved with the session and given back when it is opened", "[ara][plugins]")
{
    test::TempFolder temp;
    const auto file = test::writeWav(temp.dir.getChildFile("tone.wav"), test::sine(kTone, kRate, static_cast<int>(at(3.0)), kLevel), kRate, 2);
    const auto sessionFile = temp.dir.getChildFile("ara.dojo");
    juce::String archive;
    {
        App app;
        auto& session = app.project.getSession();
        juce::String error;
        REQUIRE(app.project.getAssets().loadNow(file, error) != nullptr);
        auto vocal = session.addTrack(session::TrackKind::audio, "Vocal");
        session.addClip(vocal, file, 1.0, 1.0, 0.5);
        const auto description = app.testPlugin();
        session.setAraPlugin(vocal, description.createXml()->toString(), description.name);
        app.project.getCompiler().rebuildNow();
        app.settle(session::track::getId(vocal), true, 1);
        REQUIRE(app.project.saveAs(sessionFile).wasOk());

        // The archive: the plug-in's own bytes (the peak it measured, by the source's persistent id).
        const auto stored = session.getState().getChildWithName(session::ids::araDocuments).getChild(0);
        REQUIRE(stored.isValid());
        CHECK(stored.getProperty(session::ids::araFactory).toString().startsWith("dev.thedojo.tests.ara"));
        archive = stored.getProperty(session::ids::araArchive).toString();
        juce::MemoryBlock bytes;
        REQUIRE(bytes.fromBase64Encoding(archive));
        juce::MemoryInputStream in(bytes, false);
        CHECK(in.readInt() == 1);
        CHECK(in.readString() == "tone.wav#" + juce::String(static_cast<int>(at(3.0))));
        CHECK_THAT(in.readFloat(), WithinAbs(kLevel, 1.0e-3));
    }

    // The session file, with the archive changed: the plug-in now "knows" a peak of 0.4.
    {
        auto xml = juce::XmlDocument::parse(sessionFile);
        REQUIRE(xml != nullptr);
        auto* documents = xml->getChildByName(session::ids::araDocuments.toString());
        REQUIRE(documents != nullptr);
        juce::MemoryOutputStream out;
        out.writeInt(1);
        out.writeString("tone.wav#" + juce::String(static_cast<int>(at(3.0))));
        out.writeFloat(0.4f);
        documents->getChildElement(0)->setAttribute(session::ids::araArchive.toString(), out.getMemoryBlock().toBase64Encoding());
        REQUIRE(xml->writeTo(sessionFile));
    }

    App app;
    REQUIRE(app.project.open(sessionFile).wasOk());
    auto vocal = app.project.getSession().getTrack(0);
    const auto trackId = session::track::getId(vocal);
    juce::String error;
    REQUIRE(app.project.getAssets().loadNow(file, error) != nullptr);
    app.project.getCompiler().rebuildNow();
    app.settle(trackId, true, 1);
    const auto deviceId = session::device::getId(session::Session::getAraDevice(vocal));
    // Restored (not measured again), and played with it: 0.2 * 0.5 / 0.4 = 0.25 at the peak.
    CHECK(test::pumpMessagesUntil([&] { return app.count(deviceId, kRestored) == 1; }, 2'000));
    CHECK(app.count(deviceId, kAnalysed) == 0);
    const auto out = app.play(2.5);
    CHECK(worst(out, 1.0, 2.0, 1.0, 0.5, 0.4f) < 2.0e-3);
    CHECK_THAT(test::peak(out, at(1.05), at(1.95)), WithinAbs(0.25, 0.01));
}

namespace
{
struct MenuItem
{
    juce::String text;
    bool enabled = false;
    bool ticked = false;
    std::function<void()> action;
};

std::vector<MenuItem> itemsOf(const juce::PopupMenu& menu)
{
    std::vector<MenuItem> items;
    for (juce::PopupMenu::MenuItemIterator it(menu); it.next();)
        items.push_back({it.getItem().text, it.getItem().isEnabled, it.getItem().isTicked, it.getItem().action});
    return items;
}

juce::Button* findButton(juce::Component& parent, const juce::String& text)
{
    for (auto* child : parent.getChildren())
        if (auto* button = dynamic_cast<juce::Button*>(child); button != nullptr && button->getButtonText() == text)
            return button;
    return nullptr;
}
} // namespace

TEST_CASE("Track > ARA Plug-in, and the plug-in's own editor for the clip in the clip editor", "[ara][plugins][ui]")
{
    test::TempFolder temp;
    App app;
    auto& session = app.project.getSession();
    const auto description = app.testPlugin();
    const auto file = test::writeWav(temp.dir.getChildFile("tone.wav"), test::sine(kTone, kRate, static_cast<int>(at(3.0)), kLevel), kRate, 2);
    juce::String error;
    REQUIRE(app.project.getAssets().loadNow(file, error) != nullptr);
    auto vocal = session.addTrack(session::TrackKind::audio, "Vocal");
    const auto verse = session::clip::getId(session.addClip(vocal, file, 0.0, 1.0, 0.0));
    auto chorusClip = session.addClip(vocal, file, 1.5, 1.0, 1.0);
    session::clip::setName(chorusClip, "play from 2", nullptr); // (the test plug-in's transport request)
    const auto chorus = session::clip::getId(chorusClip);
    const auto trackId = session::track::getId(vocal);

    ui::EditorState editor;
    ui::ArrangerView arranger(app.project, app.controller, editor);

    // No ARA plug-in installed: None (ticked) and a pointer to the scan.
    auto items = itemsOf(arranger.buildAraMenu(vocal));
    REQUIRE(items.size() == 2);
    CHECK(items[0].text == "None");
    CHECK(items[0].ticked);
    CHECK(items[1].text.startsWith("No ARA plug-ins found"));
    CHECK_FALSE(items[1].enabled);

    // The test plug-in installed: choosing it makes it the track's ARA plug-in, in one undo step.
    arranger.getAraPlugins = [&] { return juce::Array<juce::PluginDescription>{description}; };
    items = itemsOf(arranger.buildAraMenu(vocal));
    REQUIRE(items.size() == 2);
    CHECK(items[1].text == "DojoTestAra (The Dojo Tests)");
    CHECK(items[1].enabled);
    CHECK_FALSE(items[1].ticked);
    items[1].action();
    const auto device = session::Session::getAraDevice(vocal);
    REQUIRE(device.isValid());
    CHECK(session::device::getName(device) == "DojoTestAra");
    CHECK(session.getUndoManager().getUndoDescription() == "ARA Plug-in DojoTestAra");
    app.project.getCompiler().rebuildNow();
    app.settle(trackId, true, 2);
    items = itemsOf(arranger.buildAraMenu(vocal));
    CHECK_FALSE(items[0].ticked);
    CHECK(items[1].ticked);

    // The clip editor: for a clip the plug-in plays, the plug-in's own editor, told which clip.
    const auto deviceId = session::device::getId(device);
    ui::ClipEditor clipEditor(app.project, editor);
    clipEditor.setSize(900, 400);
    editor.setEditedClip(verse);
    auto* view = clipEditor.getAraEditor();
    REQUIRE(view != nullptr);
    CHECK(view->isVisible());
    CHECK(view->getWidth() == 900);
    CHECK(view->getBottom() == 400);
    CHECK(app.project.getDevices().findNode(deviceId)->getProcessor().getActiveEditor() == view);
    CHECK(test::pumpMessagesUntil([&] { return app.count(deviceId, kSelected) == 1; }, 2'000));
    auto* araButton = findButton(clipEditor, "ARA");
    auto* clipButton = findButton(clipEditor, "CLIP");
    REQUIRE(araButton != nullptr);
    REQUIRE(clipButton != nullptr);
    CHECK(araButton->isVisible());
    CHECK(araButton->getToggleState());

    // The other clip: the same editor, now showing that one. The test plug-in answers a region named
    // "play from 2" as its own transport controls would: loop 2-4 s, locate to 2 s, play. The
    // requests reach the app on the message thread; the loop is the session's (one undo step each).
    std::vector<juce::String> requests;
    app.project.getAra().onRequestLocate = [&](double seconds) { requests.push_back("locate " + juce::String(seconds, 2)); };
    app.project.getAra().onRequestPlayback = [&](bool play) { requests.push_back(play ? "play" : "stop"); };
    editor.setEditedClip(chorus);
    CHECK(clipEditor.getAraEditor() == view);
    CHECK(app.count(deviceId, kSelected) == 1);
    CHECK(test::pumpMessagesUntil([&] { return requests.size() == 2; }, 2'000));
    CHECK(requests == std::vector<juce::String>{"locate 2.00", "play"});
    CHECK(session.isLoopEnabled());
    CHECK_THAT(session.getLoopStart(), WithinAbs(2.0, 1.0e-9));
    CHECK_THAT(session.getLoopEnd(), WithinAbs(4.0, 1.0e-9));
    CHECK(session.getUndoManager().getUndoDescription() == "Toggle Loop");
    app.project.getAra().onRequestLocate = nullptr;
    app.project.getAra().onRequestPlayback = nullptr;

    // CLIP: the clip's own settings (the plug-in's editor closed); ARA: the plug-in again.
    clipButton->triggerClick();
    CHECK(test::pumpMessagesUntil([&] { return clipEditor.getAraEditor() == nullptr; }, 2'000));
    CHECK(app.project.getDevices().findNode(deviceId)->getProcessor().getActiveEditor() == nullptr);
    CHECK(clipButton->getToggleState());
    araButton->triggerClick();
    CHECK(test::pumpMessagesUntil([&] { return clipEditor.getAraEditor() != nullptr; }, 2'000));

    // None: the editor goes first, while its plug-in is still there; the clip gets its settings back.
    itemsOf(arranger.buildAraMenu(vocal))[0].action();
    CHECK(session.getUndoManager().getUndoDescription() == "No ARA Plug-in");
    app.project.getCompiler().rebuildNow();
    app.settle(trackId, false, 0);
    CHECK(clipEditor.getAraEditor() == nullptr);
    CHECK_FALSE(araButton->isVisible());
    CHECK_FALSE(clipButton->isVisible());
    CHECK(app.project.getDevices().findNode(deviceId) == nullptr);

    // Undo: the plug-in is back (bound again), and so is its editor.
    REQUIRE(session.getUndoManager().undo());
    app.project.getCompiler().rebuildNow();
    app.settle(trackId, true, 2);
    CHECK(test::pumpMessagesUntil([&] { return clipEditor.getAraEditor() != nullptr; }, 2'000));
    CHECK(araButton->isVisible());
}
#endif
