#include "devices/DeviceRegistry.h"
#include "engine/AudioEngine.h"
#include "engine/EngineController.h"
#include "engine/io/AdmMetadata.h"
#include "engine/io/AudioDeviceHost.h"
#include "session/AdmOscBridge.h"
#include "session/Exporter.h"
#include "session/Project.h"
#include "TestAudio.h"
#include "TestSupport.h"
#include "ui/components/ExportDialog.h"
#include "ui/components/SpatialPannerView.h"
#include "ui/Preferences.h"
#include "ui/PreferencesWindow.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <map>

using namespace dojo;
using namespace dojo::session;
using Catch::Matchers::WithinAbs;

namespace
{
struct MessageThread
{
    MessageThread() { juce::MessageManager::getInstance(); }
};

struct App
{
    App()
        : host(engine)
        , controller(engine, host)
        , project([this](std::unique_ptr<engine::RenderGraph> graph) { controller.publishGraph(std::move(graph)); },
                  [](const juce::ValueTree& d, double, int, juce::String&) { return devices::createBuiltinDevice(device::getType(d)); })
    {
    }

    MessageThread messageThread;
    engine::AudioEngine engine;
    engine::AudioDeviceHost host;
    engine::EngineController controller;
    Project project;
};

std::vector<std::uint8_t> bytes(const juce::MemoryBlock& block)
{
    const auto* p = static_cast<const std::uint8_t*>(block.getData());
    return {p, p + block.getSize()};
}

/** The messages by address (the last of each). */
std::map<juce::String, osc::Message> byAddress(const std::vector<osc::Message>& messages)
{
    std::map<juce::String, osc::Message> map;
    for (const auto& m : messages)
        map[m.address] = m;
    return map;
}

template <typename T>
T* findChild(juce::Component& root, const std::function<bool(T&)>& matches)
{
    for (auto* c : root.getChildren())
    {
        if (auto* t = dynamic_cast<T*>(c); t != nullptr && matches(*t))
            return t;
        if (auto* found = findChild<T>(*c, matches))
            return found;
    }
    return nullptr;
}
} // namespace

TEST_CASE("OSC 1.0 packets: messages and bundles, byte for byte", "[adm][osc]")
{
    // "/adm/obj/1/aed" (14 characters, a zero, padded to 16), ",fff" (a zero, padded to 8), three
    // big-endian floats.
    const auto packet = bytes(osc::encode({"/adm/obj/1/aed", {osc::Argument::of(30.0f), osc::Argument::of(-90.0f), osc::Argument::of(0.5f)}}));
    REQUIRE(packet.size() == 36);
    CHECK(juce::String(reinterpret_cast<const char*>(packet.data()), 14) == "/adm/obj/1/aed");
    CHECK(packet[14] == 0);
    CHECK(packet[15] == 0);
    CHECK(juce::String(reinterpret_cast<const char*>(packet.data() + 16), 4) == ",fff");
    for (std::size_t i = 20; i < 24; ++i)
        CHECK(packet[i] == 0);
    CHECK(std::vector<std::uint8_t>(packet.begin() + 24, packet.begin() + 28) == std::vector<std::uint8_t>{0x41, 0xF0, 0x00, 0x00}); // 30.0f
    CHECK(std::vector<std::uint8_t>(packet.begin() + 28, packet.begin() + 32) == std::vector<std::uint8_t>{0xC2, 0xB4, 0x00, 0x00}); // -90.0f
    CHECK(std::vector<std::uint8_t>(packet.begin() + 32, packet.begin() + 36) == std::vector<std::uint8_t>{0x3F, 0x00, 0x00, 0x00}); // 0.5f

    // Round trips: ints, floats, strings; a bundle of two.
    const osc::Message name{"/adm/obj/2/name", {osc::Argument::of(juce::String("Bird L"))}};
    const osc::Message count{"/x", {osc::Argument::of(-5), osc::Argument::of(2.25f)}};
    const auto bundle = osc::encodeBundle({name, count});
    const auto raw = bytes(bundle);
    CHECK(juce::String(reinterpret_cast<const char*>(raw.data()), 7) == "#bundle");
    CHECK(raw[7] == 0);
    CHECK(std::vector<std::uint8_t>(raw.begin() + 8, raw.begin() + 16) == std::vector<std::uint8_t>{0, 0, 0, 0, 0, 0, 0, 1}); // (at once)
    const auto decoded = osc::decode(bundle.getData(), bundle.getSize());
    REQUIRE(decoded.size() == 2);
    CHECK(decoded[0].address == "/adm/obj/2/name");
    REQUIRE(decoded[0].arguments.size() == 1);
    CHECK(decoded[0].arguments[0].type == 's');
    CHECK(decoded[0].arguments[0].s == "Bird L");
    REQUIRE(decoded[1].arguments.size() == 2);
    CHECK(decoded[1].arguments[0].i == -5);
    CHECK(decoded[1].arguments[1].f == 2.25f);

    // A bundle inside a bundle; nonsense reads as nothing.
    juce::MemoryOutputStream nested;
    nested.write("#bundle", 8);
    for (const std::uint8_t b : {0, 0, 0, 0, 0, 0, 0, 1})
        nested.writeByte(static_cast<char>(b));
    const auto size = static_cast<std::uint32_t>(bundle.getSize());
    for (const int shift : {24, 16, 8, 0})
        nested.writeByte(static_cast<char>((size >> shift) & 0xff));
    nested.write(bundle.getData(), bundle.getSize());
    CHECK(osc::decode(nested.getData(), nested.getDataSize()).size() == 2);
    CHECK(osc::decode(packet.data(), 30).empty()); // (cut short)
    const char junk[] = "not an osc packet";
    CHECK(osc::decode(junk, sizeof(junk)).empty());
}

TEST_CASE("ADM-OSC bridge: objects numbered as on the outputs, what moved, everything once a second, over UDP", "[adm][osc]")
{
    App app;
    auto& session = app.project.getSession();
    session.setMasterFormat(TrackFormat::surround714);
    auto bird = session.addTrack("Bird");
    track::setFormat(bird, TrackFormat::mono, nullptr);
    track::setPan3d(bird, true, nullptr);
    track::setAzimuth(bird, 30.0, nullptr);
    track::setElevation(bird, 10.0, nullptr);
    track::setDistance(bird, 0.8, nullptr);
    track::setSize(bird, 0.25, nullptr);
    track::setObject(bird, true, nullptr);
    auto plain = session.addTrack("Not an object"); // (3D-panned, in the bed)
    track::setPan3d(plain, true, nullptr);
    auto pad = session.addTrack("Pad"); // (stereo: two objects)
    track::setPan3d(pad, true, nullptr);
    track::setObject(pad, true, nullptr);
    auto flat = session.addTrack("Flat"); // (an object needs the 3D panner)
    track::setObject(flat, true, nullptr);
    app.project.getCompiler().rebuildNow();

    AdmOscBridge bridge(app.project, app.controller);
    const auto objects = bridge.currentObjects();
    REQUIRE(objects.size() == 3);
    CHECK(objects[0].number == 1);
    CHECK(objects[0].name == "Bird");
    CHECK_THAT(objects[0].azimuth, WithinAbs(30.0, 1e-9));
    CHECK(objects[1].name == "Pad L");
    CHECK_THAT(objects[1].azimuth, WithinAbs(30.0, 1e-9));
    CHECK(objects[2].name == "Pad R");
    CHECK_THAT(objects[2].azimuth, WithinAbs(-30.0, 1e-9));

    // Everything: positions, sizes, gains and names.
    auto all = byAddress(bridge.messagesNow(true));
    REQUIRE(all.count("/adm/obj/1/aed") == 1);
    const auto& aed = all["/adm/obj/1/aed"].arguments;
    REQUIRE(aed.size() == 3);
    CHECK_THAT(aed[0].f, WithinAbs(30.0, 1e-5));
    CHECK_THAT(aed[1].f, WithinAbs(10.0, 1e-5));
    CHECK_THAT(aed[2].f, WithinAbs(0.8, 1e-5));
    CHECK_THAT(all["/adm/obj/1/w"].arguments[0].f, WithinAbs(0.25, 1e-6));
    CHECK_THAT(all["/adm/obj/1/gain"].arguments[0].f, WithinAbs(1.0, 1e-6));
    CHECK(all["/adm/obj/1/name"].arguments[0].s == "Bird");
    CHECK(all["/adm/obj/3/name"].arguments[0].s == "Pad R");
    CHECK(all.count("/adm/obj/4/aed") == 0);
    CHECK(all.count("/adm/obj/1/xyz") == 0); // (polar unless asked)

    // Then only what moves.
    CHECK(bridge.messagesNow(false).empty());
    track::setAzimuth(bird, -45.0, nullptr);
    auto moved = byAddress(bridge.messagesNow(false));
    CHECK(moved.count("/adm/obj/1/aed") == 1);
    CHECK_THAT(moved["/adm/obj/1/aed"].arguments[0].f, WithinAbs(-45.0, 1e-5));
    CHECK(moved.count("/adm/obj/1/name") == 0);
    CHECK(moved.count("/adm/obj/2/aed") == 0);
    track::setMuted(pad, true, nullptr);
    auto muted = byAddress(bridge.messagesNow(false));
    CHECK_THAT(muted["/adm/obj/2/gain"].arguments[0].f, WithinAbs(0.0, 1e-9));
    CHECK_THAT(muted["/adm/obj/3/gain"].arguments[0].f, WithinAbs(0.0, 1e-9));

    // Cartesian: the room of ITU-R BS.2127.
    AdmOscBridge::Settings cartesian;
    cartesian.polar = false;
    cartesian.cartesian = true;
    bridge.setSettings(cartesian);
    auto xyz = byAddress(bridge.messagesNow(true));
    REQUIRE(xyz.count("/adm/obj/1/xyz") == 1);
    CHECK(xyz.count("/adm/obj/1/aed") == 0);
    const auto expected = engine::adm::polarToCartesian(-45.0, 10.0, 0.8);
    CHECK_THAT(xyz["/adm/obj/1/xyz"].arguments[0].f, WithinAbs(expected.x, 1e-5));
    CHECK_THAT(xyz["/adm/obj/1/xyz"].arguments[1].f, WithinAbs(expected.y, 1e-5));
    CHECK_THAT(xyz["/adm/obj/1/xyz"].arguments[2].f, WithinAbs(expected.z, 1e-5));

    // Over UDP, to a receiver on this computer.
    juce::DatagramSocket receiver(false);
    REQUIRE(receiver.bindToPort(0, "127.0.0.1"));
    AdmOscBridge::Settings udp;
    udp.enabled = true;
    udp.host = "127.0.0.1";
    udp.port = receiver.getBoundPort();
    bridge.setSettings(udp);
    CHECK(bridge.isSending());
    bridge.sendNow(true);
    std::vector<osc::Message> received;
    char buffer[4096];
    while (receiver.waitUntilReady(true, 1000) == 1)
    {
        const int n = receiver.read(buffer, sizeof(buffer), false);
        if (n <= 0)
            break;
        for (auto& m : osc::decode(buffer, static_cast<std::size_t>(n)))
            received.push_back(std::move(m));
        if (received.size() >= 12)
            break;
    }
    const auto got = byAddress(received);
    REQUIRE(got.count("/adm/obj/1/aed") == 1);
    CHECK_THAT(got.at("/adm/obj/1/aed").arguments[0].f, WithinAbs(-45.0, 1e-5));
    CHECK(got.count("/adm/obj/3/name") == 1);
    CHECK(bridge.getError().isEmpty());
}

TEST_CASE("External renderer preferences are remembered; the Renderer tab sets them", "[adm][preferences]")
{
    MessageThread messageThread;
    test::TempFolder temp;
    juce::PropertiesFile::Options options;
    options.applicationName = "DojoRendererTest";
    options.folderName = temp.dir.getFullPathName();
    juce::PropertiesFile file(temp.dir.getChildFile("prefs.settings"), options);
    ui::Preferences preferences(file);

    const auto defaults = preferences.getExternalRenderer();
    CHECK_FALSE(defaults.objectOutputs);
    CHECK_FALSE(defaults.osc);
    CHECK(defaults.host == "127.0.0.1");
    CHECK(defaults.port == 9000);
    CHECK(defaults.rate == 50);
    CHECK_FALSE(defaults.cartesian);

    ui::Preferences::ExternalRenderer r;
    r.objectOutputs = true;
    r.osc = true;
    r.host = "  192.168.1.20 ";
    r.port = 70000;
    r.rate = 5;
    r.cartesian = true;
    preferences.setExternalRenderer(r);
    const auto back = ui::Preferences(file).getExternalRenderer();
    CHECK(back.objectOutputs);
    CHECK(back.osc);
    CHECK(back.host == "192.168.1.20");
    CHECK(back.port == 65535);
    CHECK(back.rate == 10);
    CHECK(back.cartesian);

    // The tab: its controls write the preferences; the status line says what happens.
    preferences.setExternalRenderer({});
    auto page = ui::createRendererPage(preferences, [] { return juce::String("Sending 2 objects"); });
    page->setSize(800, 600);
    auto* outputs = findChild<juce::ToggleButton>(*page, [](juce::ToggleButton& b) { return b.getButtonText().startsWith("Objects on outputs"); });
    auto* osc = findChild<juce::ToggleButton>(*page, [](juce::ToggleButton& b) { return b.getButtonText().contains("ADM-OSC"); });
    REQUIRE(outputs != nullptr);
    REQUIRE(osc != nullptr);
    outputs->setToggleState(true, juce::sendNotificationSync);
    outputs->onClick();
    CHECK(preferences.getExternalRenderer().objectOutputs);
    osc->setToggleState(true, juce::sendNotificationSync);
    osc->onClick();
    CHECK(preferences.getExternalRenderer().osc);
    auto* port = findChild<juce::TextEditor>(*page, [](juce::TextEditor& e) { return e.getText() == "9000"; });
    REQUIRE(port != nullptr);
    port->setText("7400", juce::dontSendNotification);
    port->onReturnKey();
    CHECK(preferences.getExternalRenderer().port == 7400);
    test::pumpMessagesUntil([&] { return findChild<juce::Label>(*page, [](juce::Label& l) { return l.getText() == "Sending 2 objects"; }) != nullptr; },
                            2'000);
    CHECK(findChild<juce::Label>(*page, [](juce::Label& l) { return l.getText() == "Sending 2 objects"; }) != nullptr);
}

TEST_CASE("The export dialog offers ADM BWF: offline, one file, its objects counted; the mixer marks objects", "[adm][ui]")
{
    App app;
    auto& session = app.project.getSession();
    session.setMasterFormat(TrackFormat::surround714);
    auto bird = session.addTrack("Bird");
    track::setPan3d(bird, true, nullptr);
    track::setObject(bird, true, nullptr);
    Exporter exporter(app.project, app.controller);

    ui::ExportDialog dialog(app.project, exporter);
    auto* format = findChild<juce::ComboBox>(dialog, [](juce::ComboBox& c) { return c.getItemText(0) == "WAV"; });
    REQUIRE(format != nullptr);
    CHECK(format->getItemText(3).startsWith("ADM BWF"));
    auto* stems = findChild<juce::ToggleButton>(dialog, [](juce::ToggleButton& b) { return b.getButtonText().startsWith("Stems"); });
    REQUIRE(stems != nullptr);
    CHECK(stems->isVisible());
    format->setSelectedId(4, juce::sendNotificationSync);
    CHECK_FALSE(stems->isVisible());
    auto* hint = findChild<juce::Label>(dialog, [](juce::Label& l) { return l.getText().contains("BW64"); });
    REQUIRE(hint != nullptr);
    CHECK(hint->isVisible());
    CHECK(hint->getText().contains("1 object track"));
    CHECK(hint->getText().contains("7.1.4"));
    auto* mode = findChild<juce::ComboBox>(dialog, [](juce::ComboBox& c) { return c.getItemText(0).startsWith("Offline"); });
    REQUIRE(mode != nullptr);
    CHECK(mode->getSelectedId() == 1);
    CHECK_FALSE(mode->isItemEnabled(2)); // (no real-time ADM)
    format->setSelectedId(1, juce::sendNotificationSync);
    CHECK(stems->isVisible());
    CHECK(mode->isItemEnabled(2));

    // The panner's view tags an object.
    ui::SpatialPannerView view(true);
    CHECK(view.getBadge().isEmpty());
    view.setBadge("OBJ");
    CHECK(view.getBadge() == "OBJ");
    juce::Image image(juce::Image::ARGB, 80, 80, true);
    view.setSize(80, 80);
    {
        juce::Graphics g(image);
        view.paint(g);
    }
    CHECK(image.getPixelAt(6, 6).getAlpha() > 0); // (the tag, top left)
}
