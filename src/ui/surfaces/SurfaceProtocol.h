#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <optional>
#include <vector>

/**
    The MIDI protocols of hardware control surfaces: Mackie Control Universal (with its extenders)
    and HUI. Both put eight channel strips (a motor fader, a V-Pot, rec/solo/mute/select keys and a
    meter) and a transport on MIDI; they differ in how.

    - Mackie Control: stateless. Faders are pitch bends (14 bits) on channels 1-8 (9: the master),
      keys are notes (velocity 127 pressed, 0 released; the same notes light their LEDs), V-Pots
      are relative CCs 16-23, the jog wheel CC 60. The 2 x 56 character LCD, the meters' modes and
      the handshake are SysEx (F0 00 00 66 <model> ...), the meters channel pressure, the time
      display CCs 64-73.
    - HUI: keys come as a "zone" (CC 15) followed by a "port" (CC 47, +64 when pressed); LEDs go
      back the same way (CC 12 / CC 44). Faders are a CC pair (MSB 0-7, LSB 32-39), V-Pots CCs
      64-71, meters polyphonic aftertouch, and the displays SysEx (F0 00 00 66 05 00 ...). The host
      pings it once a second (a note-on of note 0) to keep it online.

    These are pure encoders and decoders (no MIDI ports, no session); see ControlSurfaces.
*/
namespace dojo::ui::surfaces
{

/** A surface's keys, by function (strip keys carry their strip in Event::strip). */
enum class Key
{
    none,
    // Per strip.
    recArm, solo, mute, select, vpotPress,
    // Assignment and banking.
    assignTrack, assignSend, assignPan, assignPlugin, assignEq, assignInstrument,
    bankLeft, bankRight, channelLeft, channelRight, flip, globalView, nameValue, smpteBeats,
    function, ///< F1-F8 (Event::value: 0-7)
    // Modifiers.
    shift, option, control, alt,
    // Automation.
    read, write, trim, touch, latch, group,
    // Utilities.
    save, undo, cancel, enter,
    // Transport.
    marker, nudge, cycle, drop, replace, click, rudeSolo, rewind, fastForward, stop, play, record,
    // Navigation.
    up, down, left, right, zoom, scrub,
    // HUI's own.
    returnToZero, end, online, audition, talkback,
    user,
};

/** What a surface's control did. */
struct Event
{
    enum class Kind
    {
        fader,      ///< value: 0 .. 16383; strip 8 = the master fader
        faderTouch, ///< value: 1 touched, 0 released
        vpot,       ///< value: ticks turned (+ clockwise)
        key,        ///< value: 1 pressed, 0 released (F-keys: function in `function`)
        jog,        ///< value: ticks turned (+ clockwise)
        pingReply,  ///< HUI answered a ping
        handshake,  ///< Mackie Control: a challenge to answer (see MackieControl::answerChallenge)
        online,     ///< Mackie Control: the host connection was confirmed
    };

    Kind kind = Kind::key;
    int strip = -1;
    int value = 0;
    Key key = Key::none;
    int function = -1;
    std::array<juce::uint8, 7> serial{};   ///< (handshake)
    std::array<juce::uint8, 4> challenge{}; ///< (handshake)
};

/** LED states. */
enum class Led
{
    off,
    on,
    blink, ///< (Mackie Control only; HUI shows it lit)
};

/** The meter segment (0 .. 12) for a level in dBFS, as the surfaces' meters are printed:
    -60 -50 -40 -30 -20 -14 -10 -8 -6 -4 -2 0. */
[[nodiscard]] int meterSegment(float dbfs) noexcept;

/** A fader position (0 .. 16383) for a volume in dB, and back, on the mixer's own fader law
    (-60 dB = off at the bottom, -12 dB half way, +6 dB at the top). */
[[nodiscard]] int faderPosition(double db) noexcept;
[[nodiscard]] double faderDecibels(int position) noexcept;

//==================================================================================================
namespace mackie
{

inline constexpr juce::uint8 kMain = 0x14;     ///< Mackie Control Universal (Pro)
inline constexpr juce::uint8 kExtender = 0x15; ///< Mackie Control Universal XT
inline constexpr int kLcdWidth = 56;           ///< characters per LCD line (7 per strip)

/** A key's note, and back (strip keys need their strip; F-keys their number). -1 / none: no such key. */
[[nodiscard]] int noteOf(Key key, int strip = 0) noexcept;
[[nodiscard]] std::pair<Key, int> keyOf(int note) noexcept; ///< (key, strip or F-key index)

/** Decodes a message from the surface (`model`: the SysEx id the unit answers to). */
[[nodiscard]] std::optional<Event> decode(const juce::MidiMessage& message, juce::uint8 model = kMain);

[[nodiscard]] juce::MidiMessage fader(int strip, int position);
[[nodiscard]] juce::MidiMessage led(Key key, int strip, Led state);
/** A V-Pot's LED ring: `mode` 0 dot, 1 boost/cut, 2 wrap, 3 spread; `position` 0 (off) .. 11. */
[[nodiscard]] juce::MidiMessage ring(int strip, int mode, int position, bool centre = false);
/** A meter: `segment` 0 .. 12 (14 lights the overload LED, 15 clears it). */
[[nodiscard]] juce::MidiMessage meter(int strip, int segment);
/** Meter modes: the signal LED, peak hold, and the LCD's level meter. */
[[nodiscard]] juce::MidiMessage meterMode(juce::uint8 model, int strip, bool signalLed, bool peakHold, bool lcdMeter);
/** Text at a position of the LCD (0 .. 55 the top line, 56 .. 111 the bottom one). */
[[nodiscard]] juce::MidiMessage lcd(juce::uint8 model, int position, const juce::String& text);
/** The time display's ten digits, left to right (spaces, digits, letters; a '.' after a character
    lights its dot). */
[[nodiscard]] std::vector<juce::MidiMessage> timeDisplay(const juce::String& text);
/** The two-character assignment display. */
[[nodiscard]] std::vector<juce::MidiMessage> assignmentDisplay(const juce::String& text);
/** Asks the unit to introduce itself (it answers with a challenge). */
[[nodiscard]] juce::MidiMessage deviceQuery(juce::uint8 model);
/** The answer to a challenge (the host connection reply). */
[[nodiscard]] juce::MidiMessage answerChallenge(juce::uint8 model, const std::array<juce::uint8, 7>& serial,
                                                const std::array<juce::uint8, 4>& challenge);
[[nodiscard]] std::array<juce::uint8, 4> challengeResponse(const std::array<juce::uint8, 4>& challenge) noexcept;

} // namespace mackie

//==================================================================================================
namespace hui
{

inline constexpr int kZoneTransport = 0x0E;
inline constexpr int kZoneBank = 0x0A;
inline constexpr int kZoneAssign = 0x0B;
inline constexpr int kZoneCursor = 0x0D;
inline constexpr int kZoneTransport2 = 0x0F;
inline constexpr int kZoneKeyboard = 0x08;
inline constexpr int kZoneAutoMode = 0x18;

/** A key's zone and port, and back. {-1, -1}: no such key on a HUI. */
[[nodiscard]] std::pair<int, int> switchOf(Key key, int strip = 0) noexcept;
[[nodiscard]] std::pair<Key, int> keyOf(int zone, int port) noexcept;

/** HUI's messages depend on the ones before them (a zone, then its port; a fader's high bits,
    then its low ones), so its decoder keeps that state. */
class Decoder
{
public:
    [[nodiscard]] std::optional<Event> decode(const juce::MidiMessage& message);

private:
    int zone_ = -1;
    std::array<int, 8> faderHigh_{};
};

[[nodiscard]] juce::MidiMessage ping();
[[nodiscard]] std::vector<juce::MidiMessage> fader(int strip, int position);
[[nodiscard]] std::vector<juce::MidiMessage> led(Key key, int strip, Led state);
/** A V-Pot's ring: `mode` 0 dot, 1 boost/cut, 2 wrap, 3 spread; `position` 0 (off) .. 11. */
[[nodiscard]] juce::MidiMessage ring(int strip, int mode, int position, bool centre = false);
/** A meter: `side` 0 left, 1 right; `segment` 0 .. 12. */
[[nodiscard]] juce::MidiMessage meter(int strip, int side, int segment);
/** A strip's four-character name display. */
[[nodiscard]] juce::MidiMessage stripName(int strip, const juce::String& text);
/** The main display, in eight zones of ten characters (0 .. 3 the top line, 4 .. 7 the bottom). */
[[nodiscard]] juce::MidiMessage mainDisplay(int zone, const juce::String& text);
/** The time display's eight digits, left to right (a '.' after a digit lights its dot). */
[[nodiscard]] juce::MidiMessage timeDisplay(const juce::String& text);

} // namespace hui

} // namespace dojo::ui::surfaces
