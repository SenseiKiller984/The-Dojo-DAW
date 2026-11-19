#pragma once

#include "engine/EngineController.h"

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <array>
#include <map>
#include <memory>
#include <vector>

namespace dojo::session
{
class Project;

/** OSC 1.0 packets (Open Sound Control): enough to send and read ADM-OSC. */
namespace osc
{
/** An argument: a 32-bit int ('i'), a 32-bit float ('f') or a string ('s'). */
struct Argument
{
    char type = 'f';
    std::int32_t i = 0;
    float f = 0.0f;
    juce::String s;

    [[nodiscard]] static Argument of(float value);
    [[nodiscard]] static Argument of(int value);
    [[nodiscard]] static Argument of(const juce::String& value);
};

struct Message
{
    juce::String address;
    std::vector<Argument> arguments;
};

/** A message as bytes: its address and type tags as zero-padded strings (4-byte steps), then its
    arguments big-endian. */
[[nodiscard]] juce::MemoryBlock encode(const Message& message);
/** Messages in one bundle ("#bundle", time tag 1: at once), each with its size before it. */
[[nodiscard]] juce::MemoryBlock encodeBundle(const std::vector<Message>& messages);
/** The messages a packet holds (a message or a bundle, bundles within bundles too); none if it is
    malformed. */
[[nodiscard]] std::vector<Message> decode(const void* data, std::size_t size);
} // namespace osc

/**
    The ADM-OSC bridge: where the object tracks are, sent to an external renderer as ADM-OSC
    messages (the open protocol for object positions that renderers such as SPAT Revolution, L-ISA,
    d&b Soundscape or Holophonix read) over UDP, as they move.

    Objects are numbered from 1 in the session's order, a stereo track's two sides (30 degrees either
    side of it) one after the other - the same order as the objects' channels on the external
    renderer outputs and in an ADM BWF export. For each object that moved: /adm/obj/n/aed (azimuth,
    elevation, distance: ADM conventions, azimuth positive to the left) and/or /adm/obj/n/xyz (the
    Cartesian room of ITU-R BS.2127), /adm/obj/n/w (its size) and /adm/obj/n/gain (0 while the
    track is silent); once a second everything again, with /adm/obj/n/name, for a renderer that
    starts listening late. A frame's messages travel in bundles of up to 1200 bytes.

    OSC carries positions only: the objects' sound reaches the renderer through the sound card (the
    external renderer outputs) or a virtual audio device. Message thread.
*/
class AdmOscBridge final : private juce::Timer
{
public:
    struct Settings
    {
        bool enabled = false;
        juce::String host = "127.0.0.1";
        int port = 9000;
        int rate = 50;          ///< updates a second (10 .. 100)
        bool polar = true;      ///< /aed
        bool cartesian = false; ///< /xyz
    };

    AdmOscBridge(Project& project, engine::EngineController& controller);
    ~AdmOscBridge() override;
    AdmOscBridge(const AdmOscBridge&) = delete;
    AdmOscBridge& operator=(const AdmOscBridge&) = delete;

    void setSettings(const Settings& settings);
    [[nodiscard]] const Settings& getSettings() const noexcept { return settings_; }
    [[nodiscard]] bool isSending() const noexcept { return socket_ != nullptr; }
    [[nodiscard]] const juce::String& getError() const noexcept { return error_; }

    /** An object as ADM-OSC numbers it, where it is now. */
    struct Object
    {
        int number = 1;
        std::uint32_t trackId = 0;
        juce::String name;
        double azimuth = 0.0, elevation = 0.0, distance = 1.0, size = 0.0;
        bool audible = true;
    };
    [[nodiscard]] std::vector<Object> currentObjects() const;

    /** The messages for now: what changed since the last call (everything when `everything`). */
    [[nodiscard]] std::vector<osc::Message> messagesNow(bool everything);
    /** Sends an update now (what changed, or everything). */
    void sendNow(bool everything);

private:
    void timerCallback() override;

    Project& project_;
    engine::EngineController& controller_;
    Settings settings_;
    std::unique_ptr<juce::DatagramSocket> socket_;
    std::map<int, std::array<double, 5>> sent_; ///< by object number: azimuth, elevation, distance, size, gain
    int ticksSinceEverything_ = 0;
    juce::String error_;
};

} // namespace dojo::session
