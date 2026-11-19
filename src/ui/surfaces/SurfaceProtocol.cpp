#include "ui/surfaces/SurfaceProtocol.h"

#include <algorithm>
#include <cmath>

namespace dojo::ui::surfaces
{
namespace
{
/** The SysEx header of Mackie's devices. */
constexpr juce::uint8 kMackieId[] = {0x00, 0x00, 0x66};

juce::MidiMessage sysEx(std::initializer_list<juce::uint8> head, const std::vector<juce::uint8>& tail = {})
{
    std::vector<juce::uint8> data(head);
    data.insert(data.end(), tail.begin(), tail.end());
    return juce::MidiMessage::createSysExMessage(data.data(), static_cast<int>(data.size()));
}

/** Printable ASCII only (the displays show nothing else). */
std::vector<juce::uint8> displayText(const juce::String& text, int length)
{
    std::vector<juce::uint8> chars(static_cast<std::size_t>(std::max(0, length)), 0x20);
    for (int i = 0; i < length && i < text.length(); ++i)
    {
        const auto c = text[i];
        chars[static_cast<std::size_t>(i)] = c >= 0x20 && c < 0x7F ? static_cast<juce::uint8>(c) : juce::uint8{'?'};
    }
    return chars;
}

/** Relative encoders (V-Pots, the jog wheel): the low six bits are the ticks, bit 6 turns them back. */
int relative(int value) noexcept
{
    const int ticks = value & 0x3F;
    return (value & 0x40) != 0 ? -ticks : ticks;
}

/** A display's characters with their dots ('.' lights the dot of the character before it). */
std::vector<std::pair<juce::juce_wchar, bool>> withDots(const juce::String& text)
{
    std::vector<std::pair<juce::juce_wchar, bool>> chars;
    for (int i = 0; i < text.length(); ++i)
    {
        if (text[i] == '.' && !chars.empty() && !chars.back().second)
            chars.back().second = true;
        else
            chars.emplace_back(text[i], false);
    }
    return chars;
}

struct MackieKey
{
    Key key;
    int note;
};

/** Mackie Control's keys that are not on a strip, with their notes. */
constexpr MackieKey kMackieKeys[] = {
    {Key::assignTrack, 0x28}, {Key::assignSend, 0x29}, {Key::assignPan, 0x2A}, {Key::assignPlugin, 0x2B},
    {Key::assignEq, 0x2C}, {Key::assignInstrument, 0x2D}, {Key::bankLeft, 0x2E}, {Key::bankRight, 0x2F},
    {Key::channelLeft, 0x30}, {Key::channelRight, 0x31}, {Key::flip, 0x32}, {Key::globalView, 0x33},
    {Key::nameValue, 0x34}, {Key::smpteBeats, 0x35}, {Key::shift, 0x46}, {Key::option, 0x47},
    {Key::control, 0x48}, {Key::alt, 0x49}, {Key::read, 0x4A}, {Key::write, 0x4B}, {Key::trim, 0x4C},
    {Key::touch, 0x4D}, {Key::latch, 0x4E}, {Key::group, 0x4F}, {Key::save, 0x50}, {Key::undo, 0x51},
    {Key::cancel, 0x52}, {Key::enter, 0x53}, {Key::marker, 0x54}, {Key::nudge, 0x55}, {Key::cycle, 0x56},
    {Key::drop, 0x57}, {Key::replace, 0x58}, {Key::click, 0x59}, {Key::rudeSolo, 0x5A}, {Key::rewind, 0x5B},
    {Key::fastForward, 0x5C}, {Key::stop, 0x5D}, {Key::play, 0x5E}, {Key::record, 0x5F}, {Key::up, 0x60},
    {Key::down, 0x61}, {Key::left, 0x62}, {Key::right, 0x63}, {Key::zoom, 0x64}, {Key::scrub, 0x65},
    {Key::user, 0x66},
};

struct HuiKey
{
    Key key;
    int zone;
    int port;
};

/** HUI's keys that are not on a strip, with their zones and ports. */
constexpr HuiKey kHuiKeys[] = {
    {Key::control, hui::kZoneKeyboard, 0}, {Key::shift, hui::kZoneKeyboard, 1}, {Key::undo, hui::kZoneKeyboard, 3},
    {Key::alt, hui::kZoneKeyboard, 4}, {Key::option, hui::kZoneKeyboard, 5}, {Key::save, hui::kZoneKeyboard, 7},
    {Key::channelLeft, hui::kZoneBank, 0}, {Key::bankLeft, hui::kZoneBank, 1}, {Key::channelRight, hui::kZoneBank, 2},
    {Key::bankRight, hui::kZoneBank, 3}, {Key::assignPan, hui::kZoneAssign, 2}, {Key::assignSend, hui::kZoneAssign, 7},
    {Key::down, hui::kZoneCursor, 0}, {Key::left, hui::kZoneCursor, 1}, {Key::right, hui::kZoneCursor, 3},
    {Key::up, hui::kZoneCursor, 4}, {Key::scrub, hui::kZoneCursor, 5}, {Key::talkback, hui::kZoneTransport, 0},
    {Key::rewind, hui::kZoneTransport, 1}, {Key::fastForward, hui::kZoneTransport, 2}, {Key::stop, hui::kZoneTransport, 3},
    {Key::play, hui::kZoneTransport, 4}, {Key::record, hui::kZoneTransport, 5}, {Key::returnToZero, hui::kZoneTransport2, 0},
    {Key::end, hui::kZoneTransport2, 1}, {Key::online, hui::kZoneTransport2, 2}, {Key::cycle, hui::kZoneTransport2, 3},
    {Key::replace, hui::kZoneTransport2, 4}, {Key::audition, 0x10, 0}, {Key::drop, 0x10, 2},
    {Key::trim, hui::kZoneAutoMode, 0}, {Key::latch, hui::kZoneAutoMode, 1}, {Key::read, hui::kZoneAutoMode, 2},
    {Key::write, hui::kZoneAutoMode, 4}, {Key::touch, hui::kZoneAutoMode, 5},
};

constexpr int kHuiFunctionZone = 0x1B;
} // namespace

int meterSegment(float dbfs) noexcept
{
    constexpr float kSteps[] = {-60.0f, -50.0f, -40.0f, -30.0f, -20.0f, -14.0f, -10.0f, -8.0f, -6.0f, -4.0f, -2.0f, 0.0f};
    int segment = 0;
    for (const float step : kSteps)
        if (dbfs >= step)
            ++segment;
    return segment;
}

namespace
{
juce::NormalisableRange<double> faderLaw()
{
    juce::NormalisableRange<double> range(-60.0, 6.0);
    range.setSkewForCentre(-12.0);
    return range;
}
} // namespace

int faderPosition(double db) noexcept
{
    static const auto law = faderLaw();
    return static_cast<int>(std::lround(16383.0 * law.convertTo0to1(std::clamp(db, -60.0, 6.0))));
}

double faderDecibels(int position) noexcept
{
    static const auto law = faderLaw();
    return law.convertFrom0to1(std::clamp(position, 0, 16383) / 16383.0);
}

//==================================================================================================
namespace mackie
{

int noteOf(Key key, int strip) noexcept
{
    strip = std::clamp(strip, 0, 7);
    if (key == Key::recArm)
        return 0x00 + strip;
    if (key == Key::solo)
        return 0x08 + strip;
    if (key == Key::mute)
        return 0x10 + strip;
    if (key == Key::select)
        return 0x18 + strip;
    if (key == Key::vpotPress)
        return 0x20 + strip;
    if (key == Key::function)
        return 0x36 + strip;
    for (const auto& entry : kMackieKeys)
        if (entry.key == key)
            return entry.note;
    return -1;
}

std::pair<Key, int> keyOf(int note) noexcept
{
    if (note >= 0x00 && note < 0x08)
        return {Key::recArm, note};
    if (note >= 0x08 && note < 0x10)
        return {Key::solo, note - 0x08};
    if (note >= 0x10 && note < 0x18)
        return {Key::mute, note - 0x10};
    if (note >= 0x18 && note < 0x20)
        return {Key::select, note - 0x18};
    if (note >= 0x20 && note < 0x28)
        return {Key::vpotPress, note - 0x20};
    if (note >= 0x36 && note < 0x3E)
        return {Key::function, note - 0x36};
    for (const auto& entry : kMackieKeys)
        if (entry.note == note)
            return {entry.key, -1};
    return {Key::none, -1};
}

std::optional<Event> decode(const juce::MidiMessage& message, juce::uint8 model)
{
    Event event;
    if (message.isPitchWheel())
    {
        event.kind = Event::Kind::fader;
        event.strip = message.getChannel() - 1;
        event.value = message.getPitchWheelValue();
        return event.strip >= 0 && event.strip <= 8 ? std::optional<Event>(event) : std::nullopt;
    }
    if (message.isNoteOnOrOff())
    {
        const int note = message.getNoteNumber();
        const bool pressed = message.isNoteOn() && message.getVelocity() > 0;
        if (note >= 0x68 && note <= 0x70)
        {
            event.kind = Event::Kind::faderTouch;
            event.strip = note - 0x68;
            event.value = pressed ? 1 : 0;
            return event;
        }
        const auto [key, index] = keyOf(note);
        if (key == Key::none)
            return std::nullopt;
        event.kind = Event::Kind::key;
        event.key = key;
        event.value = pressed ? 1 : 0;
        if (key == Key::function)
            event.function = index;
        else
            event.strip = index;
        return event;
    }
    if (message.isController())
    {
        const int number = message.getControllerNumber();
        if (number >= 0x10 && number < 0x18)
        {
            event.kind = Event::Kind::vpot;
            event.strip = number - 0x10;
            event.value = relative(message.getControllerValue());
            return event;
        }
        if (number == 0x3C)
        {
            event.kind = Event::Kind::jog;
            event.value = relative(message.getControllerValue());
            return event;
        }
        return std::nullopt;
    }
    if (message.isSysEx())
    {
        const auto* data = message.getSysExData();
        const int size = message.getSysExDataSize();
        if (size < 5 || !std::equal(std::begin(kMackieId), std::end(kMackieId), data) || data[3] != model)
            return std::nullopt;
        if (data[4] == 0x01 && size >= 5 + 7 + 4)
        {
            event.kind = Event::Kind::handshake;
            std::copy_n(data + 5, 7, event.serial.begin());
            std::copy_n(data + 12, 4, event.challenge.begin());
            return event;
        }
        if (data[4] == 0x03)
        {
            event.kind = Event::Kind::online;
            return event;
        }
    }
    return std::nullopt;
}

juce::MidiMessage fader(int strip, int position)
{
    return juce::MidiMessage::pitchWheel(std::clamp(strip, 0, 8) + 1, std::clamp(position, 0, 16383));
}

juce::MidiMessage led(Key key, int strip, Led state)
{
    const int note = noteOf(key, strip);
    const auto velocity = static_cast<juce::uint8>(state == Led::on ? 0x7F : (state == Led::blink ? 0x01 : 0x00));
    return juce::MidiMessage::noteOn(1, std::max(0, note), velocity);
}

juce::MidiMessage ring(int strip, int mode, int position, bool centre)
{
    const int value = (centre ? 0x40 : 0x00) | ((mode & 0x03) << 4) | std::clamp(position, 0, 11);
    return juce::MidiMessage::controllerEvent(1, 0x30 + std::clamp(strip, 0, 7), value);
}

juce::MidiMessage meter(int strip, int segment)
{
    return juce::MidiMessage::channelPressureChange(1, (std::clamp(strip, 0, 7) << 4) | std::clamp(segment, 0, 15));
}

juce::MidiMessage meterMode(juce::uint8 model, int strip, bool signalLed, bool peakHold, bool lcdMeter)
{
    const auto mode = static_cast<juce::uint8>((signalLed ? 1 : 0) | (peakHold ? 2 : 0) | (lcdMeter ? 4 : 0));
    return sysEx({0x00, 0x00, 0x66, model, 0x20, static_cast<juce::uint8>(std::clamp(strip, 0, 7)), mode});
}

juce::MidiMessage lcd(juce::uint8 model, int position, const juce::String& text)
{
    position = std::clamp(position, 0, 2 * kLcdWidth - 1);
    const int length = std::min(text.length(), 2 * kLcdWidth - position);
    return sysEx({0x00, 0x00, 0x66, model, 0x12, static_cast<juce::uint8>(position)}, displayText(text, length));
}

namespace
{
/** A character as the seven-segment time display takes it. */
int segmentCharacter(juce::juce_wchar c, bool dot) noexcept
{
    c = juce::CharacterFunctions::toUpperCase(c);
    int value = 0x20; // (unknown: blank)
    if (c >= 0x40 && c < 0x60)
        value = static_cast<int>(c) - 0x40;
    else if (c >= 0x20 && c < 0x40)
        value = static_cast<int>(c);
    return value | (dot ? 0x40 : 0x00);
}
} // namespace

std::vector<juce::MidiMessage> timeDisplay(const juce::String& text)
{
    auto chars = withDots(text);
    while (chars.size() < 10)
        chars.insert(chars.begin(), {' ', false});
    chars.erase(chars.begin(), chars.end() - 10);
    std::vector<juce::MidiMessage> messages;
    for (int i = 0; i < 10; ++i) // (CC 64 is the rightmost digit)
    {
        const auto& [c, dot] = chars[static_cast<std::size_t>(9 - i)];
        messages.push_back(juce::MidiMessage::controllerEvent(1, 0x40 + i, segmentCharacter(c, dot)));
    }
    return messages;
}

std::vector<juce::MidiMessage> assignmentDisplay(const juce::String& text)
{
    auto chars = withDots(text);
    while (chars.size() < 2)
        chars.insert(chars.begin(), {' ', false});
    chars.erase(chars.begin(), chars.end() - 2);
    return {juce::MidiMessage::controllerEvent(1, 0x4B, segmentCharacter(chars[0].first, chars[0].second)),
            juce::MidiMessage::controllerEvent(1, 0x4A, segmentCharacter(chars[1].first, chars[1].second))};
}

juce::MidiMessage deviceQuery(juce::uint8 model)
{
    return sysEx({0x00, 0x00, 0x66, model, 0x00});
}

std::array<juce::uint8, 4> challengeResponse(const std::array<juce::uint8, 4>& challenge) noexcept
{
    // The host connection reply, as Mackie documented it for the original Logic Control.
    const int l1 = challenge[0], l2 = challenge[1], l3 = challenge[2], l4 = challenge[3];
    return {static_cast<juce::uint8>(0x7F & (l1 + (l2 ^ 0x0A) - l4)), static_cast<juce::uint8>(0x7F & ((l3 >> 4) ^ (l1 + l4))),
            static_cast<juce::uint8>(0x7F & ((l4 - (l3 << 2)) ^ (l1 | l2))), static_cast<juce::uint8>(0x7F & (l2 - l3 + (0xF0 ^ (l4 << 4))))};
}

juce::MidiMessage answerChallenge(juce::uint8 model, const std::array<juce::uint8, 7>& serial, const std::array<juce::uint8, 4>& challenge)
{
    std::vector<juce::uint8> tail(serial.begin(), serial.end());
    const auto response = challengeResponse(challenge);
    tail.insert(tail.end(), response.begin(), response.end());
    return sysEx({0x00, 0x00, 0x66, model, 0x02}, tail);
}

} // namespace mackie

//==================================================================================================
namespace hui
{

std::pair<int, int> switchOf(Key key, int strip) noexcept
{
    strip = std::clamp(strip, 0, 7);
    if (key == Key::select)
        return {strip, 1};
    if (key == Key::mute)
        return {strip, 2};
    if (key == Key::solo)
        return {strip, 3};
    if (key == Key::vpotPress)
        return {strip, 5};
    if (key == Key::recArm)
        return {strip, 7};
    if (key == Key::function)
        return {kHuiFunctionZone, strip};
    for (const auto& entry : kHuiKeys)
        if (entry.key == key)
            return {entry.zone, entry.port};
    return {-1, -1};
}

std::pair<Key, int> keyOf(int zone, int port) noexcept
{
    if (zone >= 0 && zone < 8)
    {
        switch (port)
        {
        case 1: return {Key::select, zone};
        case 2: return {Key::mute, zone};
        case 3: return {Key::solo, zone};
        case 5: return {Key::vpotPress, zone};
        case 7: return {Key::recArm, zone};
        default: return {Key::none, zone};
        }
    }
    if (zone == kHuiFunctionZone && port >= 0 && port < 8)
        return {Key::function, port};
    for (const auto& entry : kHuiKeys)
        if (entry.zone == zone && entry.port == port)
            return {entry.key, -1};
    return {Key::none, -1};
}

std::optional<Event> Decoder::decode(const juce::MidiMessage& message)
{
    Event event;
    if (message.isNoteOn() && message.getNoteNumber() == 0 && message.getVelocity() == 0x7F)
    {
        event.kind = Event::Kind::pingReply;
        return event;
    }
    if (!message.isController())
        return std::nullopt;

    const int number = message.getControllerNumber();
    const int value = message.getControllerValue();
    if (number == 0x0F)
    {
        zone_ = value;
        return std::nullopt;
    }
    if (number == 0x2F)
    {
        if (zone_ < 0)
            return std::nullopt;
        const int port = value & 0x0F;
        const bool pressed = (value & 0x40) != 0;
        if (zone_ < 8 && port == 0)
        {
            event.kind = Event::Kind::faderTouch;
            event.strip = zone_;
            event.value = pressed ? 1 : 0;
            return event;
        }
        const auto [key, index] = keyOf(zone_, port);
        if (key == Key::none)
            return std::nullopt;
        event.kind = Event::Kind::key;
        event.key = key;
        event.value = pressed ? 1 : 0;
        if (key == Key::function)
            event.function = index;
        else
            event.strip = index;
        return event;
    }
    if (number >= 0x00 && number < 0x08)
    {
        faderHigh_[static_cast<std::size_t>(number)] = value;
        return std::nullopt;
    }
    if (number >= 0x20 && number < 0x28)
    {
        event.kind = Event::Kind::fader;
        event.strip = number - 0x20;
        event.value = (faderHigh_[static_cast<std::size_t>(event.strip)] << 7) | value;
        return event;
    }
    if (number >= 0x40 && number < 0x48)
    {
        event.kind = Event::Kind::vpot;
        event.strip = number - 0x40;
        event.value = relative(value);
        return event;
    }
    if (number == 0x0D)
    {
        event.kind = Event::Kind::jog;
        event.value = relative(value);
        return event;
    }
    return std::nullopt;
}

juce::MidiMessage ping()
{
    return juce::MidiMessage::noteOn(1, 0, juce::uint8{0});
}

std::vector<juce::MidiMessage> fader(int strip, int position)
{
    strip = std::clamp(strip, 0, 7);
    position = std::clamp(position, 0, 16383);
    return {juce::MidiMessage::controllerEvent(1, strip, position >> 7), juce::MidiMessage::controllerEvent(1, 0x20 + strip, position & 0x7F)};
}

std::vector<juce::MidiMessage> led(Key key, int strip, Led state)
{
    const auto [zone, port] = switchOf(key, strip);
    if (zone < 0)
        return {};
    return {juce::MidiMessage::controllerEvent(1, 0x0C, zone), juce::MidiMessage::controllerEvent(1, 0x2C, port | (state == Led::off ? 0x00 : 0x40))};
}

juce::MidiMessage ring(int strip, int mode, int position, bool centre)
{
    const int value = (centre ? 0x40 : 0x00) | ((mode & 0x03) << 4) | std::clamp(position, 0, 11);
    return juce::MidiMessage::controllerEvent(1, 0x10 + std::clamp(strip, 0, 7), value);
}

juce::MidiMessage meter(int strip, int side, int segment)
{
    return juce::MidiMessage::aftertouchChange(1, std::clamp(strip, 0, 7), ((side != 0 ? 1 : 0) << 4) | std::clamp(segment, 0, 12));
}

juce::MidiMessage stripName(int strip, const juce::String& text)
{
    return sysEx({0x00, 0x00, 0x66, 0x05, 0x00, 0x10, static_cast<juce::uint8>(std::clamp(strip, 0, 7))}, displayText(text, 4));
}

juce::MidiMessage mainDisplay(int zone, const juce::String& text)
{
    return sysEx({0x00, 0x00, 0x66, 0x05, 0x00, 0x12, static_cast<juce::uint8>(std::clamp(zone, 0, 7))}, displayText(text, 10));
}

juce::MidiMessage timeDisplay(const juce::String& text)
{
    auto chars = withDots(text);
    while (chars.size() < 8)
        chars.insert(chars.begin(), {' ', false});
    chars.erase(chars.begin(), chars.end() - 8);
    std::vector<juce::uint8> digits;
    for (int i = 7; i >= 0; --i) // (the rightmost digit first)
    {
        const auto& [c, dot] = chars[static_cast<std::size_t>(i)];
        const int digit = c >= '0' && c <= '9' ? static_cast<int>(c - '0') : 0x0A; // (0x0A: blank)
        digits.push_back(static_cast<juce::uint8>(digit | (dot ? 0x10 : 0x00)));
    }
    return sysEx({0x00, 0x00, 0x66, 0x05, 0x00, 0x11}, digits);
}

} // namespace hui

} // namespace dojo::ui::surfaces
