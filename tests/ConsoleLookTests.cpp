// The consoles' editors look like the desks: the EMI TG12345's grey metalwork, black pointer knobs
// and red/grey buttons; the Harrison 4032's black panel, coloured knob caps, walnut cheeks, leather
// armrest and wooden meter bridge; each with the long fader of its strip.

#include "devices/ConsoleTG12345.h"
#include "devices/DeviceRegistry.h"
#include "devices/Harrison4032.h"
#include "session/Project.h"
#include "ui/EditorState.h"
#include "ui/devices/ConsoleEditors.h"
#include "ui/devices/ConsoleLookAndFeel.h"
#include "ui/devices/HardwarePainting.h"
#include "ui/devices/HarrisonEditors.h"
#include "ui/devices/ParameterControl.h"
#include "EffectTestHelpers.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace dojo;
using namespace dojo::test::devices;
using Catch::Matchers::WithinAbs;
using Look = ui::ConsoleLookAndFeel;

namespace
{
template <typename Type>
Type* find(juce::Component& in)
{
    if (auto* found = dynamic_cast<Type*>(&in))
        return found;
    for (auto* child : in.getChildren())
        if (auto* found = find<Type>(*child))
            return found;
    return nullptr;
}

juce::String idOf(juce::AudioProcessorParameter& parameter)
{
    auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*>(&parameter);
    return withId != nullptr ? withId->paramID : juce::String();
}

juce::MouseEvent mouseAt(juce::Component& c, juce::Point<float> at, juce::ModifierKeys mods = juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier),
                         juce::Point<float> down = {})
{
    const auto now = juce::Time::getCurrentTime();
    return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), at, mods, juce::MouseInputSource::defaultPressure,
                            juce::MouseInputSource::defaultOrientation, juce::MouseInputSource::defaultRotation,
                            juce::MouseInputSource::defaultTiltX, juce::MouseInputSource::defaultTiltY, &c, &c, now, down, now, 1, false);
}
} // namespace

TEST_CASE("Console looks: the TG12345's editor is grey metalwork with black knobs and its cassette's fader", "[ui][console]")
{
    juce::MessageManager::getInstance();
    devices::ConsoleTG12345 tg;
    tg.prepareToPlay(kRate, kBlock);
    ui::ConsoleEditor editor(tg);
    editor.setSize(ui::ConsoleEditor::preferredWidth(tg), 230);

    // Its look reaches every control: black printing on the grey, black legend plates.
    auto* look = dynamic_cast<Look*>(&editor.getPages().getLookAndFeel());
    REQUIRE(look != nullptr);
    CHECK(look->getStyle() == Look::Style::emi);
    auto* control = find<ui::ParameterControl>(editor.getPages());
    REQUIRE(control != nullptr);
    CHECK(&control->getLookAndFeel() == look);
    CHECK(look->findColour(ui::ParameterControl::captionColourId).getPerceivedBrightness() < 0.3f);
    CHECK(look->findColour(juce::ComboBox::backgroundColourId).getPerceivedBrightness() < 0.15f);
    // Menus keep the app's look.
    CHECK(look->getPopupMenuFont().getHeight() == juce::LookAndFeel::getDefaultLookAndFeel().getPopupMenuFont().getHeight());

    // The face carries the cassette's own level as a fader: the channel's, the echo return, the
    // control room's.
    const auto faderOf = [&editor]() -> ui::hardware::ParameterFader*
    {
        auto* display = editor.getPages().getDisplay();
        return display != nullptr ? find<ui::hardware::ParameterFader>(*display) : nullptr;
    };
    REQUIRE(faderOf() != nullptr);
    CHECK(idOf(faderOf()->getParameter()) == "fader");
    CHECK(ui::ConsoleEditor::faderFor(devices::ConsoleTG12345::mainMix) == "fader");
    for (const auto& [cassette, id] : {std::pair{3, "echoreturn"}, std::pair{4, "crlevel"}, std::pair{0, "fader"}})
    {
        setParameter(tg, "cassette", static_cast<float>(cassette));
        juce::MessageManager::getInstance()->runDispatchLoopUntil(400);
        REQUIRE(faderOf() != nullptr);
        CHECK(idOf(faderOf()->getParameter()) == juce::String(id));
    }

    // The fader: a click on the slot takes the cap there; dragging moves it with the mouse (Shift: a
    // fifth as far); a double-click returns it to its default.
    auto& fader = *faderOf();
    auto& level = fader.getParameter();
    fader.setSize(36, 160);
    const auto slot = fader.getSlot();
    const float travel = slot.getHeight() - ui::hardware::FaderStyle{}.capHeight;
    fader.mouseDown(mouseAt(fader, {slot.getCentreX(), slot.getY() + 2.0f}));
    fader.mouseUp(mouseAt(fader, {slot.getCentreX(), slot.getY() + 2.0f}));
    CHECK(level.getValue() > 0.98f);
    const float top = ui::hardware::faderCapCentre(slot, level.getValue(), ui::hardware::FaderStyle{}.capHeight);
    fader.mouseDown(mouseAt(fader, {slot.getCentreX(), top}));
    fader.mouseDrag(mouseAt(fader, {slot.getCentreX(), top + travel * 0.5f}));
    fader.mouseUp(mouseAt(fader, {slot.getCentreX(), top + travel * 0.5f}));
    CHECK_THAT(level.getValue(), WithinAbs(0.5, 0.02));
    const float middle = ui::hardware::faderCapCentre(slot, level.getValue(), ui::hardware::FaderStyle{}.capHeight);
    const float before = level.getValue();
    const juce::ModifierKeys shift(juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::shiftModifier);
    fader.mouseDown(mouseAt(fader, {slot.getCentreX(), middle}, shift));
    fader.mouseDrag(mouseAt(fader, {slot.getCentreX(), middle - travel * 0.5f}, shift));
    fader.mouseUp(mouseAt(fader, {slot.getCentreX(), middle - travel * 0.5f}, shift));
    CHECK_THAT(level.getValue() - before, WithinAbs(0.1, 0.02));
    fader.mouseDoubleClick(mouseAt(fader, {slot.getCentreX(), middle}));
    CHECK(level.getValue() == level.getDefaultValue());

    // Every page of every cassette paints.
    for (int cassette = 0; cassette < 5; ++cassette)
    {
        setParameter(tg, "cassette", static_cast<float>(cassette));
        juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
        for (std::size_t p = 0; p < editor.getPages().getNumPages(); ++p)
        {
            editor.getPages().showPage(p);
            (void) editor.createComponentSnapshot(editor.getLocalBounds());
        }
    }
}

TEST_CASE("Console looks: the Harrison's editor has its black panel, coloured caps, walnut and leather", "[ui][harrison]")
{
    juce::MessageManager::getInstance();
    // The caps say what a knob does, the switches light in their function's colour.
    const auto dominant = [](juce::Colour c)
    {
        const auto r = c.getFloatRed(), g = c.getFloatGreen(), b = c.getFloatBlue();
        return r > g && r > b ? 'r' : g > b ? 'g' : 'b';
    };
    for (const auto* id : {"lfgain", "lmfgain", "hmfgain", "hfgain", "micgain", "linetrim"})
        CHECK(dominant(Look::harrisonCap(id)) == 'r');
    for (const auto* id : {"lffreq", "lmffreq", "hmffreq", "hffreq", "hpf", "lpf"})
        CHECK(dominant(Look::harrisonCap(id)) == 'b');
    for (const auto* id : {"lmfq", "hmfq"})
        CHECK(dominant(Look::harrisonCap(id)) == 'g');
    const auto aux = Look::harrisonCap("aux3"), cue = Look::harrisonCap("cue1");
    CHECK(aux.getFloatBlue() < 0.3f * aux.getFloatRed()); // yellow
    CHECK(aux.getFloatGreen() > 0.6f * aux.getFloatRed());
    CHECK(cue.getFloatGreen() < 0.65f * cue.getFloatRed()); // orange
    CHECK(Look::harrisonCap("fader").getPerceivedBrightness() > 0.8f);
    CHECK(dominant(Look::harrisonLamp("phantom")) == 'r');
    CHECK(dominant(Look::harrisonLamp("eqin")) == 'g');
    CHECK(dominant(Look::harrisonLamp("mix")) == 'b');

    session::Project project([](std::unique_ptr<engine::RenderGraph>) {},
                             [](const juce::ValueTree& d, double, int, juce::String&) { return devices::createBuiltinDevice(session::device::getType(d)); });
    ui::EditorState editorState;
    auto track = project.getSession().addTrack(session::TrackKind::audio, "Vocal");
    auto device = project.getSession().addDevice(track, devices::Harrison4032::kTypeId, "Harrison 4032");
    auto node = project.getDevices().getNode(device);
    REQUIRE(node != nullptr);
    auto& console = dynamic_cast<devices::Harrison4032&>(node->getProcessor());
    const ui::DeviceEditorContext context{project, editorState, device, track};
    ui::HarrisonEditor editor(console, context);

    // The controls sit between the walnut cheeks, above the armrest (when there is room for it).
    editor.setSize(ui::HarrisonEditor::preferredWidth(console, context), 230);
    CHECK(editor.getPages().getX() == ui::HarrisonEditor::kCheekWidth);
    CHECK(editor.getPages().getRight() == editor.getWidth() - ui::HarrisonEditor::kCheekWidth);
    CHECK(editor.getPages().getBottom() == 230 - ui::HarrisonEditor::kArmrestHeight);
    editor.setSize(editor.getWidth(), ui::PagedEditor::kTabHeight + 4 + 2 * ui::ParameterControl::kHeight);
    CHECK(editor.getPages().getBottom() == editor.getHeight());
    editor.setSize(editor.getWidth(), 230);

    auto* look = dynamic_cast<Look*>(&editor.getPages().getLookAndFeel());
    REQUIRE(look != nullptr);
    CHECK(look->getStyle() == Look::Style::harrison);
    CHECK(look->findColour(ui::ParameterControl::captionColourId).getPerceivedBrightness() > 0.8f); // white printing
    auto* control = find<ui::ParameterControl>(editor.getPages());
    REQUIRE(control != nullptr);
    CHECK(Look::parameterIdOf(*control->getChildComponent(0)) == idOf(control->getParameter()));

    // The long fader works the channel's fader (the master fader on the other modules).
    auto* fader = find<ui::hardware::ParameterFader>(*editor.getPages().getDisplay());
    REQUIRE(fader != nullptr);
    CHECK(idOf(fader->getParameter()) == "fader");
    CHECK(fader->getHeight() > 120);
    setParameter(console, "module", 2.0f);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(400);
    fader = find<ui::hardware::ParameterFader>(*editor.getPages().getDisplay());
    REQUIRE(fader != nullptr);
    CHECK(idOf(fader->getParameter()) == "fader");
    (void) editor.createComponentSnapshot(editor.getLocalBounds());
}

TEST_CASE("Hardware painting: VU meters read in voltage, as the meters do", "[ui]")
{
    CHECK_THAT(ui::hardware::vuPosition(3.0), WithinAbs(1.0, 1e-12));
    CHECK_THAT(ui::hardware::vuPosition(0.0), WithinAbs(0.708, 1e-3));
    CHECK_THAT(ui::hardware::vuPosition(-20.0), WithinAbs(0.0708, 1e-3));
    CHECK(ui::hardware::vuPosition(-200.0) < 1e-9);
    const auto face = ui::hardware::vuFace();
    CHECK_THAT(face.redFrom, WithinAbs(ui::hardware::vuPosition(0.0), 1e-12));
    CHECK(face.marks.size() == 11);
    // Painting copes with any size.
    juce::Image image(juce::Image::ARGB, 120, 60, true);
    juce::Graphics g(image);
    for (const auto& size : {juce::Rectangle<float>(0, 0, 120, 60), juce::Rectangle<float>(0, 0, 30, 60), juce::Rectangle<float>(0, 0, 6, 6)})
        ui::hardware::paintNeedleMeter(g, size, 0.5, face);
}
