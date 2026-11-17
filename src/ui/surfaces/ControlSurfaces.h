#pragma once

#include "session/Project.h"
#include "ui/EditorState.h"
#include "ui/surfaces/SurfaceProtocol.h"

#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace dojo::ui::surfaces
{

/** The surfaces The Dojo speaks to. */
enum class Model
{
    mackieControl,  ///< Mackie Control Universal (Pro), or a unit in its mode (X-Touch, iCON, ...)
    mackieExtender, ///< Mackie Control Universal XT: eight more strips
    hui,            ///< Mackie HUI, or a unit in HUI mode
};
[[nodiscard]] juce::String modelName(Model model);

/** A surface as set up in Preferences > Control Surfaces: its model and MIDI ports (identifiers). */
struct Setup
{
    Model model = Model::mackieControl;
    juce::String input;
    juce::String output;
    bool operator==(const Setup&) const = default;
};
/** The setups as the preferences keep them, and back. */
[[nodiscard]] juce::String toString(const std::vector<Setup>& setups);
[[nodiscard]] std::vector<Setup> setupsFromString(const juce::String& text);

/** How surfaces reach their MIDI ports. The application's routes a surface's input away from the
    instruments (see AudioDeviceHost::routeMidiInput) and opens its output; tests use none. */
class Ports
{
public:
    virtual ~Ports() = default;
    /** Sends an input's messages to `receive` (on a MIDI thread) instead of the instruments. */
    virtual bool openInput(const juce::String& identifier, std::function<void(const juce::MidiMessage&)> receive) = 0;
    virtual void closeInput(const juce::String& identifier) = 0;
    /** Something that sends to an output (empty if it cannot be opened). */
    virtual std::function<void(const juce::MidiMessage&)> openOutput(const juce::String& identifier) = 0;
};

/** What the surfaces work: the application's transport, commands, selection and meters. */
struct Host
{
    std::function<void(juce::CommandID)> invoke;    ///< an application command (record, loop, save, undo, ...)
    std::function<EditorState*()> editor;           ///< the selection (null: none)
    std::function<bool()> playing;
    std::function<bool()> recording;
    std::function<double()> playhead;               ///< seconds
    std::function<void()> play, stop;
    std::function<void(double)> locate;             ///< seconds
    std::function<float(int slot)> peak;            ///< a track's peak since the last call (linear)
    std::function<float(int slot)> movingVolume;    ///< a track's fader while automation moves it (linear; NaN: its own)
};

/**
    Hardware control surfaces: Mackie Control (with extenders) and HUI.

    Each surface has eight channel strips; surfaces lie side by side in the order they are set up,
    showing the mixer's tracks from the bank's first. Per strip:
    - the motor fader is the track's volume (the master fader on a Mackie Control is the master's):
      it follows automation and every change made elsewhere, and while it is touched it is the
      surface's. A touch starts an undo step;
    - the V-Pot turns the pan (PAN), or a send's level (SEND: pressed again, the next send), with its
      LED ring; pressed, it resets them (centre, 0 dB). FLIP swaps the fader and the V-Pot;
    - REC, SOLO, MUTE and SELECT (Shift: add to the selection), lit as the tracks are;
    - the meter (post-fader peaks) and the display: the track's name, and its pan, send level, or
      volume (NAME/VALUE).
    Banks: BANK < > by a surface's eight strips, CHANNEL < > (and the cursor keys) by one.
    Transport: PLAY, STOP (stopped: back to the start), RECORD, REWIND and FAST FWD (a bar per press,
    repeating while held), CYCLE (the loop), DROP / REPLACE (punch in / out), the jog wheel (a beat
    per tick; with SCRUB a sixteenth). SAVE, UNDO (Shift: redo), F1-F8 (the views and the zoom), the
    time display (SMPTE/BEATS: time or bars and beats) and the assignment display.
    HUI is pinged once a second and is online while it answers; a Mackie Control's handshake is
    answered.

    Message thread. MIDI arrives on MIDI threads and is handled at the next timer tick (100 Hz);
    the surfaces are updated 30 times a second with what changed.
*/
class ControlSurfaces final : private juce::Timer
{
public:
    ControlSurfaces(session::Project& project, Host host, Ports* ports);
    ~ControlSurfaces() override;

    /** Replaces the surfaces (closing the old ports, opening the new). */
    void setSetups(std::vector<Setup> setups);
    [[nodiscard]] const std::vector<Setup>& getSetups() const noexcept { return setups_; }

    struct Status
    {
        bool input = false;  ///< its input is routed here
        bool output = false; ///< its output is open
        bool online = false; ///< it has answered
    };
    [[nodiscard]] Status getStatus(int index) const;
    /** "Online", "Waiting for it", "MIDI input missing", ... for the preferences. */
    [[nodiscard]] juce::String describeStatus(int index) const;

    // --- Driving it (tests; the timer does this in the application) ---------------------------------
    /** A message from surface `index`, handled now. */
    void receive(int index, const juce::MidiMessage& message);
    /** Where surface `index`'s messages go (by default its MIDI output). */
    void setSender(int index, std::function<void(const juce::MidiMessage&)> sender);
    /** Sends each surface what changed (`force`: everything). */
    void refresh(bool force = false);
    /** Time passing (`seconds` since the last call): held REWIND / FAST FWD, HUI's pings. */
    void tick(double seconds);

    // --- State ------------------------------------------------------------------------------------
    enum class VpotMode
    {
        pan,
        send,
    };
    /** The tracks the strips show, in the mixer's order (no video tracks). */
    [[nodiscard]] std::vector<juce::ValueTree> getStripTracks() const;
    [[nodiscard]] int getBank() const noexcept { return bank_; }
    [[nodiscard]] VpotMode getVpotMode() const noexcept { return vpotMode_; }
    [[nodiscard]] int getSendSlot() const noexcept { return sendSlot_; }
    [[nodiscard]] bool isFlipped() const noexcept { return flip_; }
    [[nodiscard]] int getNumStrips() const noexcept { return 8 * static_cast<int>(units_.size()); }

private:
    struct Unit;

    void timerCallback() override;
    void handle(Unit& unit, const Event& event);
    void handleKey(Unit& unit, const Event& event);
    [[nodiscard]] juce::ValueTree trackAt(const Unit& unit, int strip) const;
    [[nodiscard]] juce::ValueTree trackAt(const Unit& unit, int strip, const std::vector<juce::ValueTree>& tracks) const;
    [[nodiscard]] int unitIndex(const Unit& unit) const;
    void moveBank(int delta);
    void moveSelection(int delta);
    void shuttle(int direction);
    [[nodiscard]] double beatSeconds() const;
    void send(Unit& unit, const juce::MidiMessage& message);
    void sendLed(Unit& unit, Key key, int strip, bool on, bool force);
    void refreshUnit(Unit& unit, bool force);
    [[nodiscard]] juce::String timeText(bool hui) const;
    [[nodiscard]] juce::String valueText(const juce::ValueTree& track) const;
    [[nodiscard]] juce::ValueTree sendOf(const juce::ValueTree& track) const;
    void drainInputs();

    session::Project& project_;
    Host host_;
    Ports* ports_;
    std::vector<Setup> setups_;
    std::vector<std::unique_ptr<Unit>> units_;

    int bank_ = 0;
    VpotMode vpotMode_ = VpotMode::pan;
    int sendSlot_ = 0;
    bool flip_ = false;
    bool showValues_ = false;
    bool showBeats_ = true;
    bool shift_ = false;
    bool scrub_ = false;
    int shuttle_ = 0;
    double shuttleWait_ = 0.0;
    double sinceRefresh_ = 0.0;
    double sinceVpot_ = 1.0e9; ///< since a V-Pot last turned (its turn is one undo step)
    int lastVpot_ = -1;

    std::mutex inboxMutex_;
    std::vector<std::pair<int, juce::MidiMessage>> inbox_; ///< (from MIDI threads)
    juce::uint32 lastTick_ = 0;
};

} // namespace dojo::ui::surfaces
