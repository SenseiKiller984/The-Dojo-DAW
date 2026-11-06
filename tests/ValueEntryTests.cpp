#include "devices/BuiltinProcessor.h"
#include "devices/DeviceRegistry.h"
#include "devices/Utility.h"
#include "session/Session.h"
#include "ui/EditorState.h"
#include "ui/MixerValues.h"
#include "ui/components/MixerPanel.h"
#include "ui/devices/ParameterControl.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using namespace dojo;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

TEST_CASE("Typed parameter values: numbers with or without units", "[ui][values]")
{
    using devices::parseParameterNumber;
    CHECK(parseParameterNumber("120", "Hz") == 120.0);
    CHECK(parseParameterNumber(" 120 Hz ", "Hz") == 120.0);
    CHECK(parseParameterNumber("2.5k", "Hz") == 2500.0);
    CHECK(parseParameterNumber("2.50k", "Hz") == 2500.0); // what the knob shows
    CHECK(parseParameterNumber("2.5 kHz", "Hz") == 2500.0);
    CHECK(parseParameterNumber("-6 dB", "dB") == -6.0);
    CHECK(parseParameterNumber("+3", "dB") == 3.0);
    CHECK(parseParameterNumber("50 %", "%") == 50.0);
    CHECK(parseParameterNumber("0,5", "") == 0.5);
    CHECK(parseParameterNumber("1.5 s", "ms") == 1500.0);
    CHECK(parseParameterNumber("1.5 ms", "ms") == 1.5);
    CHECK(parseParameterNumber("1.5 s", "Hz") == 1.5);
    CHECK_FALSE(parseParameterNumber("", "Hz").has_value());
    CHECK_FALSE(parseParameterNumber("abc", "Hz").has_value());
    CHECK_FALSE(parseParameterNumber("-", "dB").has_value());
}

TEST_CASE("Every native knob's displayed value types back in", "[ui][values]")
{
    for (const auto& info : devices::getBuiltinDevices())
    {
        auto processor = devices::createBuiltinDevice(info.type);
        for (auto* p : processor->getParameters())
        {
            auto* parameter = dynamic_cast<juce::AudioParameterFloat*>(p);
            if (parameter == nullptr)
                continue;
            const auto& range = parameter->getNormalisableRange();
            juce::AudioProcessorParameter& base = *parameter; // the public interface the controls use
            for (const float normalised : {0.0f, 0.37f, 0.81f, 1.0f})
            {
                const float value = range.snapToLegalValue(range.convertFrom0to1(normalised)); // what is shown
                const auto text = base.getText(normalised, 32);
                const float typed = range.convertFrom0to1(base.getValueForText(text));
                INFO(info.name << " " << base.getName(64) << ": " << text);
                const float tolerance = std::max(0.01f * std::abs(value), 0.005f * (range.end - range.start));
                CHECK(std::abs(typed - value) <= tolerance);
            }
        }
    }
}

TEST_CASE("Double-clicking a knob's value lets you type an exact value", "[ui][values]")
{
    juce::MessageManager::getInstance();
    devices::Utility utility;
    auto* gain = utility.getParameterById("gain");
    auto* bassFrequency = utility.getParameterById("bassfreq");
    REQUIRE(gain != nullptr);
    REQUIRE(bassFrequency != nullptr);

    ui::ParameterControl control(*gain, "Gain");
    control.setSize(ui::ParameterControl::kWidth, ui::ParameterControl::kHeight);
    CHECK(control.getValueArea().getBottom() == ui::ParameterControl::kHeight); // the value text under the knob

    const auto type = [&](const juce::String& text)
    {
        control.showValueEditor();
        auto* field = control.getValueEditor();
        REQUIRE(field != nullptr);
        field->setText(text, juce::dontSendNotification);
        return field;
    };
    const auto gainDb = [&] { return gain->getNormalisableRange().convertFrom0to1(gain->getValue()); };

    control.showValueEditor();
    REQUIRE(control.getValueEditor() != nullptr);
    CHECK(control.getValueEditor()->getText() == "0.00"); // starts with the current value

    // Return applies.
    type("6.5")->onReturnKey();
    CHECK_THAT(gainDb(), WithinAbs(6.5, 1.0e-4));
    CHECK(control.getValueEditor() == nullptr);

    // Units are fine; out of range clamps.
    type("-12 dB")->onReturnKey();
    CHECK_THAT(gainDb(), WithinAbs(-12.0, 1.0e-4));
    type("100")->onReturnKey();
    CHECK_THAT(gainDb(), WithinAbs(35.0, 1.0e-4));

    // Escape cancels; text without a number changes nothing.
    type("3")->onEscapeKey();
    CHECK_THAT(gainDb(), WithinAbs(35.0, 1.0e-4));
    type("loud")->onReturnKey();
    CHECK_THAT(gainDb(), WithinAbs(35.0, 1.0e-4));

    // Clicking elsewhere (focus lost) applies; a second notification changes nothing.
    auto* field = type("-3");
    field->onFocusLost();
    CHECK_THAT(gainDb(), WithinAbs(-3.0, 1.0e-4));
    field->onFocusLost();
    CHECK_THAT(gainDb(), WithinAbs(-3.0, 1.0e-4));

    // Thousands as displayed, e.g. a frequency.
    ui::ParameterControl frequency(*bassFrequency, "Bass Freq");
    frequency.setSize(ui::ParameterControl::kWidth, ui::ParameterControl::kHeight);
    frequency.showValueEditor();
    frequency.getValueEditor()->setText("0.2k", juce::dontSendNotification);
    frequency.getValueEditor()->onReturnKey();
    CHECK_THAT(bassFrequency->getNormalisableRange().convertFrom0to1(bassFrequency->getValue()), WithinAbs(200.0, 0.5));

    // Toggles and choices have no value field.
    ui::ParameterControl toggle(*utility.getParameterById("mono"), "Mono");
    toggle.setSize(ui::ParameterControl::kWidth, ui::ParameterControl::kHeight);
    toggle.showValueEditor();
    CHECK(toggle.getValueEditor() == nullptr);
}

TEST_CASE("Mixer values: typed volumes and pans", "[ui][values][mixer]")
{
    using ui::mixer::parsePan;
    using ui::mixer::parseVolume;
    CHECK(parseVolume("-6") == -6.0);
    CHECK(parseVolume("-6 dB") == -6.0);
    CHECK(parseVolume("+3.5") == 3.5);
    CHECK(parseVolume("0,5") == 0.5);
    CHECK(parseVolume("100") == session::kMaxVolumeDb);
    CHECK(parseVolume("-100") == session::kMinusInfinityDb);
    CHECK(parseVolume("-inf") == session::kMinusInfinityDb);
    CHECK(parseVolume("inf") == session::kMinusInfinityDb);
    CHECK_FALSE(parseVolume("loud").has_value());
    CHECK_FALSE(parseVolume("").has_value());

    CHECK(parsePan("C") == 0.0);
    CHECK(parsePan("center") == 0.0);
    CHECK(parsePan("0") == 0.0);
    CHECK_THAT(*parsePan("L20"), WithinAbs(-0.20, 1.0e-12));
    CHECK_THAT(*parsePan("20L"), WithinAbs(-0.20, 1.0e-12));
    CHECK_THAT(*parsePan("l 15 %"), WithinAbs(-0.15, 1.0e-12));
    CHECK_THAT(*parsePan("R35"), WithinAbs(0.35, 1.0e-12));
    CHECK_THAT(*parsePan("35"), WithinAbs(0.35, 1.0e-12));
    CHECK_THAT(*parsePan("-30"), WithinAbs(-0.30, 1.0e-12));
    CHECK(parsePan("L") == -1.0);
    CHECK(parsePan("R") == 1.0);
    CHECK(parsePan("150") == 1.0);
    CHECK_FALSE(parsePan("x").has_value());
    CHECK_FALSE(parsePan("").has_value());

    // What the strips show types back in.
    for (const double pan : {-1.0, -0.37, 0.0, 0.5, 1.0})
        CHECK_THAT(*parsePan(ui::mixer::formatPan(pan)), WithinAbs(pan, 0.006));
    for (const double db : {-60.0, -12.3, 0.0, 4.5})
        CHECK_THAT(*parseVolume(ui::mixer::formatDecibels(db)), WithinAbs(db, 0.051));
}

TEST_CASE("Double-clicking a mixer strip's volume or pan lets you type it", "[ui][values][mixer]")
{
    juce::MessageManager::getInstance();
    session::Session session;
    ui::EditorState editor;
    auto track = session.addTrack(session::TrackKind::audio, "Guitar");

    ui::ChannelStrip strip(session, editor, track, [] { return juce::StringArray{"In 1", "In 2"}; }, [] {});
    strip.setSize(104, 560);

    strip.showVolumeEditor();
    REQUIRE(strip.getValueEditor() != nullptr);
    CHECK(strip.getValueEditor()->getText() == "0.0");
    strip.getValueEditor()->setText("-6.5", juce::dontSendNotification);
    strip.getValueEditor()->onReturnKey();
    CHECK_THAT(session::track::getVolumeDb(track), WithinAbs(-6.5, 0.005));
    CHECK(strip.getValueEditor() == nullptr);

    // One undo step.
    session.getUndoManager().undo();
    CHECK_THAT(session::track::getVolumeDb(track), WithinAbs(0.0, 1.0e-9));

    strip.showPanEditor();
    REQUIRE(strip.getValueEditor() != nullptr);
    CHECK(strip.getValueEditor()->getText() == "C");
    strip.getValueEditor()->setText("L25", juce::dontSendNotification);
    strip.getValueEditor()->onReturnKey();
    CHECK_THAT(session::track::getPan(track), WithinAbs(-0.25, 1.0e-9));

    // Escape cancels; nonsense changes nothing.
    strip.showPanEditor();
    strip.getValueEditor()->setText("R80", juce::dontSendNotification);
    strip.getValueEditor()->onEscapeKey();
    CHECK_THAT(session::track::getPan(track), WithinAbs(-0.25, 1.0e-9));
    strip.showVolumeEditor();
    strip.getValueEditor()->setText("loud", juce::dontSendNotification);
    strip.getValueEditor()->onReturnKey();
    CHECK_THAT(session::track::getVolumeDb(track), WithinAbs(0.0, 1.0e-9));

    // The master's GAIN readout.
    ui::MasterStrip master(session, editor, [] {});
    master.setSize(130, 560);
    master.showGainEditor();
    REQUIRE(master.getValueEditor() != nullptr);
    master.getValueEditor()->setText("-3", juce::dontSendNotification);
    master.getValueEditor()->onReturnKey();
    CHECK_THAT(session.getMasterVolumeDb(), WithinAbs(-3.0, 0.005));
}
