#include "ui/surfaces/ControlSurfaces.h"

#include "ui/Commands.h"
#include "ui/MixerValues.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace dojo::ui::surfaces
{
namespace
{
constexpr int kStrips = 8;
constexpr int kMaster = 8;                 ///< (a Mackie Control's master fader: pitch bend channel 9)
constexpr double kRefreshSeconds = 1.0 / 30.0;
constexpr double kPingSeconds = 1.0;       ///< HUI is pinged once a second...
constexpr double kOfflineSeconds = 3.0;    ///< ...and offline when it has not answered for this long
constexpr double kShuttleDelay = 0.4;      ///< REWIND / FAST FWD held: repeating after this...
constexpr double kShuttleRepeat = 0.15;    ///< ...this often
constexpr double kMeterRepeat = 0.25;      ///< a lit meter is sent again this often (it decays by itself)
constexpr double kEditGapSeconds = 1.0;    ///< a V-Pot turned after a pause starts a new undo step
constexpr double kMinDb = -60.0;
constexpr double kMaxDb = 6.0;

/** `text` in exactly `width` characters, cut or padded with spaces, centred if `centre`. */
juce::String fit(const juce::String& text, int width, bool centre = false)
{
    auto cut = text.substring(0, width);
    if (!centre)
        return cut.paddedRight(' ', width);
    const int left = (width - cut.length()) / 2;
    return juce::String::repeatedString(" ", left) + cut + juce::String::repeatedString(" ", width - left - cut.length());
}

bool canRecord(const juce::ValueTree& track)
{
    const auto kind = session::track::getKind(track);
    return kind == session::TrackKind::audio || kind == session::TrackKind::instrument || kind == session::TrackKind::midi;
}
} // namespace

juce::String modelName(Model model)
{
    switch (model)
    {
    case Model::mackieControl: return "Mackie Control";
    case Model::mackieExtender: return "Mackie Control Extender";
    case Model::hui: return "HUI";
    }
    return {};
}

juce::String toString(const std::vector<Setup>& setups)
{
    juce::Array<juce::var> list;
    for (const auto& setup : setups)
    {
        auto* object = new juce::DynamicObject();
        object->setProperty("model", static_cast<int>(setup.model));
        object->setProperty("input", setup.input);
        object->setProperty("output", setup.output);
        list.add(juce::var(object));
    }
    return juce::JSON::toString(juce::var(list), true);
}

std::vector<Setup> setupsFromString(const juce::String& text)
{
    std::vector<Setup> setups;
    const auto parsed = juce::JSON::parse(text);
    if (const auto* list = parsed.getArray())
        for (const auto& item : *list)
        {
            Setup setup;
            setup.model = static_cast<Model>(std::clamp(static_cast<int>(item.getProperty("model", 0)), 0, 2));
            setup.input = item.getProperty("input", {}).toString();
            setup.output = item.getProperty("output", {}).toString();
            setups.push_back(setup);
        }
    return setups;
}

//==================================================================================================
struct ControlSurfaces::Unit
{
    Setup setup;
    std::function<void(const juce::MidiMessage&)> output;
    bool inputOpen = false;
    bool online = false;
    bool everHeard = false;
    double sinceHeard = 1.0e9;
    double sincePing = 1.0e9;
    hui::Decoder decoder;
    std::array<bool, 9> touched{};

    // What the surface shows (to send only what changes); -1: not known.
    std::array<int, 9> fader{};
    std::array<std::array<int, 4>, kStrips> stripLeds{}; ///< rec, solo, mute, select
    std::array<int, kStrips> ring{};
    std::array<int, kStrips> meter{};
    std::array<double, kStrips> meterAge{};
    juce::String lcdTop, lcdBottom;
    std::array<juce::String, kStrips> names;
    std::array<juce::String, 8> zones;
    juce::String time, assignment;
    std::map<int, int> leds; ///< global keys' LEDs by note (Mackie) or zone/port

    Unit()
    {
        forget();
    }

    void forget()
    {
        fader.fill(-1);
        for (auto& strip : stripLeds)
            strip.fill(-1);
        ring.fill(-1);
        meter.fill(-1);
        meterAge.fill(0.0);
        lcdTop = lcdBottom = time = assignment = "?";
        names.fill("?");
        zones.fill("?");
        leds.clear();
    }

    [[nodiscard]] bool isHui() const noexcept { return setup.model == Model::hui; }
    [[nodiscard]] bool isMain() const noexcept { return setup.model == Model::mackieControl; }
    [[nodiscard]] juce::uint8 sysExModel() const noexcept { return setup.model == Model::mackieExtender ? mackie::kExtender : mackie::kMain; }
};

ControlSurfaces::ControlSurfaces(session::Project& project, Host host, Ports* ports)
    : project_(project)
    , host_(std::move(host))
    , ports_(ports)
{
    lastTick_ = juce::Time::getMillisecondCounter();
    startTimerHz(100);
}

ControlSurfaces::~ControlSurfaces()
{
    stopTimer();
    setSetups({}); // (closes the inputs: no MIDI thread calls in any more)
}

void ControlSurfaces::setSetups(std::vector<Setup> setups)
{
    if (ports_ != nullptr)
        for (const auto& unit : units_)
            if (unit->inputOpen)
                ports_->closeInput(unit->setup.input);
    {
        const std::lock_guard lock(inboxMutex_);
        inbox_.clear();
    }
    units_.clear();
    setups_ = std::move(setups);

    for (std::size_t i = 0; i < setups_.size(); ++i)
    {
        auto unit = std::make_unique<Unit>();
        unit->setup = setups_[i];
        if (ports_ != nullptr)
        {
            const auto index = static_cast<int>(i);
            unit->inputOpen = unit->setup.input.isNotEmpty()
                              && ports_->openInput(unit->setup.input,
                                                   [this, index](const juce::MidiMessage& message)
                                                   {
                                                       const std::lock_guard lock(inboxMutex_);
                                                       inbox_.emplace_back(index, message);
                                                   });
            if (unit->setup.output.isNotEmpty())
                unit->output = ports_->openOutput(unit->setup.output);
        }
        units_.push_back(std::move(unit));
    }
    bank_ = std::clamp(bank_, 0, std::max(0, static_cast<int>(getStripTracks().size()) - 1));

    // Greet them: a Mackie Control is asked to introduce itself, HUI is pinged; then everything.
    for (auto& unit : units_)
    {
        if (unit->isHui())
        {
            send(*unit, hui::ping());
            unit->sincePing = 0.0;
        }
        else
            send(*unit, mackie::deviceQuery(unit->sysExModel()));
    }
    refresh(true);
}

ControlSurfaces::Status ControlSurfaces::getStatus(int index) const
{
    Status status;
    if (index < 0 || index >= static_cast<int>(units_.size()))
        return status;
    const auto& unit = *units_[static_cast<std::size_t>(index)];
    status.input = unit.inputOpen;
    status.output = static_cast<bool>(unit.output);
    status.online = unit.isHui() ? unit.online && unit.sinceHeard < kOfflineSeconds : unit.online || unit.everHeard;
    return status;
}

juce::String ControlSurfaces::describeStatus(int index) const
{
    const auto status = getStatus(index);
    if (index < 0 || index >= static_cast<int>(units_.size()))
        return {};
    if (!status.input && !status.output)
        return "Pick its MIDI ports";
    if (!status.input)
        return "MIDI input missing";
    if (!status.output)
        return "MIDI output missing";
    return status.online ? "Online" : "Waiting for it";
}

//==================================================================================================
// Incoming

void ControlSurfaces::timerCallback()
{
    const auto now = juce::Time::getMillisecondCounter();
    const double seconds = std::clamp(static_cast<double>(now - lastTick_) / 1000.0, 0.0, 1.0);
    lastTick_ = now;
    drainInputs();
    tick(seconds);
    sinceRefresh_ += seconds;
    if (sinceRefresh_ >= kRefreshSeconds)
    {
        sinceRefresh_ = 0.0;
        refresh(false);
    }
}

void ControlSurfaces::drainInputs()
{
    std::vector<std::pair<int, juce::MidiMessage>> messages;
    {
        const std::lock_guard lock(inboxMutex_);
        messages.swap(inbox_);
    }
    for (const auto& [index, message] : messages)
        receive(index, message);
}

void ControlSurfaces::receive(int index, const juce::MidiMessage& message)
{
    if (index < 0 || index >= static_cast<int>(units_.size()))
        return;
    auto& unit = *units_[static_cast<std::size_t>(index)];
    unit.sinceHeard = 0.0;
    unit.everHeard = true;
    const auto event = unit.isHui() ? unit.decoder.decode(message) : mackie::decode(message, unit.sysExModel());
    if (event.has_value())
        handle(unit, *event);
}

void ControlSurfaces::handle(Unit& unit, const Event& event)
{
    auto& session = project_.getSession();
    auto& undo = session.getUndoManager();
    switch (event.kind)
    {
    case Event::Kind::pingReply:
        unit.online = true;
        return;
    case Event::Kind::handshake:
        send(unit, mackie::answerChallenge(unit.sysExModel(), event.serial, event.challenge));
        return;
    case Event::Kind::online:
        unit.online = true;
        unit.forget();
        refreshUnit(unit, true);
        return;
    case Event::Kind::faderTouch:
        if (event.strip < 0 || event.strip > kMaster)
            return;
        unit.touched[static_cast<std::size_t>(event.strip)] = event.value != 0;
        if (event.value != 0)
            undo.beginNewTransaction("Fader");
        else
            unit.fader[static_cast<std::size_t>(event.strip)] = -1; // (released: show where the track really is)
        return;
    case Event::Kind::fader:
    {
        if (event.strip < 0 || event.strip > kMaster)
            return;
        unit.fader[static_cast<std::size_t>(event.strip)] = event.value; // (that is where it is now)
        if (event.strip == kMaster)
        {
            if (unit.isMain())
                session.setMasterVolumeDb(faderDecibels(event.value));
            return;
        }
        auto track = trackAt(unit, event.strip);
        if (!track.isValid())
            return;
        if (!flip_)
            session::track::setVolumeDb(track, faderDecibels(event.value), &undo);
        else if (vpotMode_ == VpotMode::pan)
            session::track::setPan(track, std::clamp(event.value / 16383.0 * 2.0 - 1.0, -1.0, 1.0), &undo);
        else if (auto s = sendOf(track); s.isValid())
            session::send::setGainDb(s, faderDecibels(event.value), &undo);
        return;
    }
    case Event::Kind::vpot:
    {
        auto track = trackAt(unit, event.strip);
        if (!track.isValid() || event.value == 0)
            return;
        // A turn is one undo step: a new one after a pause, or on another V-Pot.
        const int vpot = unitIndex(unit) * kStrips + event.strip;
        if (sinceVpot_ >= kEditGapSeconds || vpot != lastVpot_)
            undo.beginNewTransaction("V-Pot");
        sinceVpot_ = 0.0;
        lastVpot_ = vpot;
        if (flip_)
            session::track::setVolumeDb(track, std::clamp(session::track::getVolumeDb(track) + 0.5 * event.value, kMinDb, kMaxDb), &undo);
        else if (vpotMode_ == VpotMode::pan)
            session::track::setPan(track, std::clamp(session::track::getPan(track) + 0.02 * event.value, -1.0, 1.0), &undo);
        else if (auto s = sendOf(track); s.isValid())
            session::send::setGainDb(s, std::clamp(session::send::getGainDb(s) + 0.5 * event.value, kMinDb, kMaxDb), &undo);
        return;
    }
    case Event::Kind::jog:
        if (host_.locate && host_.playhead)
            host_.locate(std::max(0.0, host_.playhead() + event.value * beatSeconds() * (scrub_ ? 0.25 : 1.0)));
        return;
    case Event::Kind::key:
        handleKey(unit, event);
        return;
    }
}

void ControlSurfaces::handleKey(Unit& unit, const Event& event)
{
    const bool pressed = event.value != 0;
    if (event.key == Key::shift)
    {
        shift_ = pressed;
        return;
    }
    if (event.key == Key::rewind || event.key == Key::fastForward)
    {
        if (pressed)
            shuttle(event.key == Key::rewind ? -1 : 1);
        else
            shuttle_ = 0;
        return;
    }
    if (!pressed)
        return;

    auto& session = project_.getSession();
    auto& undo = session.getUndoManager();
    const auto invoke = [this](juce::CommandID command)
    {
        if (host_.invoke)
            host_.invoke(command);
    };
    const bool playing = host_.playing && host_.playing();

    switch (event.key)
    {
    case Key::recArm:
    case Key::solo:
    case Key::mute:
    case Key::select:
    case Key::vpotPress:
    {
        auto track = trackAt(unit, event.strip);
        if (!track.isValid())
            return;
        undo.beginNewTransaction();
        if (event.key == Key::recArm && canRecord(track))
            session::track::setArmed(track, !session::track::isArmed(track), &undo);
        else if (event.key == Key::solo)
            session::track::setSoloed(track, !session::track::isSoloed(track), &undo);
        else if (event.key == Key::mute)
            session::track::setMuted(track, !session::track::isMuted(track), &undo);
        else if (event.key == Key::select)
        {
            if (auto* editor = host_.editor ? host_.editor() : nullptr)
            {
                const auto id = session::track::getId(track);
                if (shift_)
                {
                    std::vector<std::uint32_t> order;
                    for (const auto& t : getStripTracks())
                        order.push_back(session::track::getId(t));
                    editor->selectTrackWithModifiers(id, true, false, order);
                }
                else
                    editor->selectTrack(id);
            }
        }
        else if (event.key == Key::vpotPress)
        {
            // Back to the default: the volume (flipped), the centre, or a send at 0 dB.
            if (flip_)
                session::track::setVolumeDb(track, 0.0, &undo);
            else if (vpotMode_ == VpotMode::pan)
                session::track::setPan(track, 0.0, &undo);
            else if (auto s = sendOf(track); s.isValid())
                session::send::setGainDb(s, 0.0, &undo);
        }
        return;
    }
    case Key::bankLeft: moveBank(-getNumStrips()); return;
    case Key::bankRight: moveBank(getNumStrips()); return;
    case Key::channelLeft:
    case Key::left: moveBank(-1); return;
    case Key::channelRight:
    case Key::right: moveBank(1); return;
    case Key::up: moveSelection(-1); return;
    case Key::down: moveSelection(1); return;
    case Key::assignPan:
        vpotMode_ = VpotMode::pan;
        return;
    case Key::assignSend:
    {
        if (vpotMode_ == VpotMode::send)
        {
            std::size_t most = 1;
            for (const auto& t : getStripTracks())
                most = std::max(most, session::Session::getSends(t).size());
            sendSlot_ = (sendSlot_ + 1) % static_cast<int>(most);
        }
        else
        {
            vpotMode_ = VpotMode::send;
            sendSlot_ = 0;
        }
        return;
    }
    case Key::flip: flip_ = !flip_; return;
    case Key::nameValue: showValues_ = !showValues_; return;
    case Key::smpteBeats: showBeats_ = !showBeats_; return;
    case Key::scrub: scrub_ = !scrub_; return;
    case Key::play:
        if (!playing)
            invoke(commands::playStop);
        return;
    case Key::stop:
        invoke(playing ? commands::playStop : commands::returnToStart);
        return;
    case Key::record: invoke(commands::record); return;
    case Key::cycle: invoke(commands::toggleLoop); return;
    case Key::drop: invoke(commands::togglePunchIn); return;
    case Key::replace: invoke(commands::togglePunchOut); return;
    case Key::save: invoke(commands::saveSession); return;
    case Key::undo: invoke(shift_ ? commands::redo : commands::undo); return;
    case Key::returnToZero: invoke(commands::returnToStart); return;
    case Key::end:
        if (host_.locate)
            host_.locate(session.getContentEnd(session.getMaster()));
        return;
    case Key::rudeSolo:
    {
        undo.beginNewTransaction("Unsolo All");
        for (auto t : session.getAllTracks())
            if (session::track::isSoloed(t))
                session::track::setSoloed(t, false, &undo);
        return;
    }
    case Key::function:
    {
        static constexpr juce::CommandID kFunctions[] = {commands::showMixer, commands::showDevices, commands::showEditor, commands::toggleSnap,
                                                         commands::zoomIn,    commands::zoomOut,     commands::zoomToFit,  commands::addTrack};
        if (event.function >= 0 && event.function < 8)
            invoke(kFunctions[event.function]);
        return;
    }
    case Key::none:
    case Key::assignTrack:
    case Key::assignPlugin:
    case Key::assignEq:
    case Key::assignInstrument:
    case Key::globalView:
    case Key::shift:
    case Key::option:
    case Key::control:
    case Key::alt:
    case Key::read:
    case Key::write:
    case Key::trim:
    case Key::touch:
    case Key::latch:
    case Key::group:
    case Key::cancel:
    case Key::enter:
    case Key::marker:
    case Key::nudge:
    case Key::click:
    case Key::rewind:
    case Key::fastForward:
    case Key::zoom:
    case Key::online:
    case Key::audition:
    case Key::talkback:
    case Key::user:
        return;
    }
}

//==================================================================================================
// State

std::vector<juce::ValueTree> ControlSurfaces::getStripTracks() const
{
    std::vector<juce::ValueTree> tracks;
    for (const auto& t : project_.getSession().getAllTracks())
        if (session::track::getKind(t) != session::TrackKind::video)
            tracks.push_back(t);
    return tracks;
}

int ControlSurfaces::unitIndex(const Unit& unit) const
{
    for (std::size_t i = 0; i < units_.size(); ++i)
        if (units_[i].get() == &unit)
            return static_cast<int>(i);
    return 0;
}

juce::ValueTree ControlSurfaces::trackAt(const Unit& unit, int strip) const
{
    return trackAt(unit, strip, getStripTracks());
}

juce::ValueTree ControlSurfaces::trackAt(const Unit& unit, int strip, const std::vector<juce::ValueTree>& tracks) const
{
    if (strip < 0 || strip >= kStrips)
        return {};
    const int index = bank_ + unitIndex(unit) * kStrips + strip;
    return index >= 0 && index < static_cast<int>(tracks.size()) ? tracks[static_cast<std::size_t>(index)] : juce::ValueTree();
}

juce::ValueTree ControlSurfaces::sendOf(const juce::ValueTree& track) const
{
    const auto sends = session::Session::getSends(track);
    return sendSlot_ >= 0 && sendSlot_ < static_cast<int>(sends.size()) ? sends[static_cast<std::size_t>(sendSlot_)] : juce::ValueTree();
}

void ControlSurfaces::moveBank(int delta)
{
    const int count = static_cast<int>(getStripTracks().size());
    const int last = std::max(0, count - std::max(1, getNumStrips()));
    bank_ = std::clamp(bank_ + delta, 0, last);
}

void ControlSurfaces::moveSelection(int delta)
{
    auto* editor = host_.editor ? host_.editor() : nullptr;
    const auto tracks = getStripTracks();
    if (editor == nullptr || tracks.empty())
        return;
    int index = 0;
    for (std::size_t i = 0; i < tracks.size(); ++i)
        if (session::track::getId(tracks[i]) == editor->getSelectedTrack())
            index = static_cast<int>(i) + delta;
    index = std::clamp(index, 0, static_cast<int>(tracks.size()) - 1);
    editor->selectTrack(session::track::getId(tracks[static_cast<std::size_t>(index)]));
    // Keep it on a strip.
    if (index < bank_)
        bank_ = index;
    else if (index >= bank_ + getNumStrips())
        bank_ = index - getNumStrips() + 1;
}

double ControlSurfaces::beatSeconds() const
{
    const double at = host_.playhead ? std::max(0.0, host_.playhead()) : 0.0;
    return 60.0 / std::max(1.0, project_.getSession().getTempoAt(at));
}

double ControlSurfaces::barSeconds() const
{
    // The bar at the play head, on the tempo map.
    const auto& session = project_.getSession();
    const double at = host_.playhead ? std::max(0.0, host_.playhead()) : 0.0;
    const auto& map = session.getTempoMap();
    const auto bar = map.barAt(map.beatAt(at));
    return map.secondsAt(map.beatOfBar(bar + 1)) - map.secondsAt(map.beatOfBar(bar));
}

void ControlSurfaces::shuttle(int direction)
{
    shuttle_ = direction;
    shuttleWait_ = kShuttleDelay;
    if (host_.locate && host_.playhead)
        host_.locate(std::max(0.0, host_.playhead() + direction * barSeconds()));
}

void ControlSurfaces::tick(double seconds)
{
    sinceVpot_ += seconds;
    if (shuttle_ != 0)
    {
        shuttleWait_ -= seconds;
        while (shuttleWait_ <= 0.0)
        {
            if (host_.locate && host_.playhead)
                host_.locate(std::max(0.0, host_.playhead() + shuttle_ * barSeconds()));
            shuttleWait_ += kShuttleRepeat;
        }
    }
    for (auto& unit : units_)
    {
        unit->sinceHeard += seconds;
        for (auto& age : unit->meterAge)
            age += seconds;
        if (unit->isHui())
        {
            unit->sincePing += seconds;
            if (unit->sincePing >= kPingSeconds)
            {
                send(*unit, hui::ping());
                unit->sincePing = 0.0;
            }
            if (unit->sinceHeard >= kOfflineSeconds)
                unit->online = false;
        }
    }
}

//==================================================================================================
// Outgoing

void ControlSurfaces::setSender(int index, std::function<void(const juce::MidiMessage&)> sender)
{
    if (index >= 0 && index < static_cast<int>(units_.size()))
        units_[static_cast<std::size_t>(index)]->output = std::move(sender);
}

void ControlSurfaces::send(Unit& unit, const juce::MidiMessage& message)
{
    if (unit.output)
        unit.output(message);
}

void ControlSurfaces::sendLed(Unit& unit, Key key, int strip, bool on, bool force)
{
    int id = -1;
    if (unit.isHui())
    {
        const auto [zone, port] = hui::switchOf(key, strip);
        id = zone < 0 ? -1 : zone * 16 + port;
    }
    else
        id = mackie::noteOf(key, strip);
    if (id < 0)
        return;
    const int state = on ? 1 : 0;
    if (const auto it = unit.leds.find(id); !force && it != unit.leds.end() && it->second == state)
        return;
    unit.leds[id] = state;
    if (unit.isHui())
        for (const auto& message : hui::led(key, strip, on ? Led::on : Led::off))
            send(unit, message);
    else
        send(unit, mackie::led(key, strip, on ? Led::on : Led::off));
}

juce::String ControlSurfaces::valueText(const juce::ValueTree& track) const
{
    if (!track.isValid())
        return {};
    if (showValues_ || flip_)
        return mixer::formatDecibels(session::track::getVolumeDb(track));
    if (vpotMode_ == VpotMode::pan)
    {
        const auto pan = mixer::formatPan(session::track::getPan(track));
        return pan == "C" ? "<C>" : pan;
    }
    const auto s = sendOf(track);
    return s.isValid() ? mixer::formatDecibels(session::send::getGainDb(s)) : "--";
}

juce::String ControlSurfaces::timeText(bool hui) const
{
    const double seconds = host_.playhead ? std::max(0.0, host_.playhead()) : 0.0;
    if (showBeats_)
    {
        // Bars and beats from the tempo map (beats in the signature's unit), sixteenths of a beat.
        const auto position = project_.getSession().getTempoMap().toBarsBeatsTicks(project_.getSession().beatAtSeconds(seconds));
        const auto bar = static_cast<int>(position.bar);
        const auto beat = position.beat;
        const auto ticksPerBeat = std::max(1, static_cast<int>(std::lround(
                                                   time::kTicksPerQuarterNote
                                                   * time::beatLengthInQuarterNotes(project_.getSession().getSignatureAt(seconds)))));
        const double inBeat = static_cast<double>(position.tick) / ticksPerBeat;
        const auto sixteenth = static_cast<int>(inBeat * 4.0) + 1;
        const auto ticks = static_cast<int>((inBeat * 4.0 - std::floor(inBeat * 4.0)) * 240.0);
        if (hui)
            return juce::String(bar).paddedLeft('0', 4) + "." + juce::String(beat).paddedLeft('0', 2) + "." + juce::String(sixteenth).paddedLeft('0', 2);
        return juce::String(bar).paddedLeft(' ', 3) + "." + juce::String(beat).paddedLeft(' ', 2) + "." + juce::String(sixteenth).paddedLeft(' ', 2) + "."
               + juce::String(ticks).paddedLeft('0', 3);
    }
    const auto whole = static_cast<int>(seconds);
    const int hours = whole / 3600, minutes = (whole / 60) % 60, secs = whole % 60;
    if (hui)
        return juce::String(hours).paddedLeft('0', 2) + "." + juce::String(minutes).paddedLeft('0', 2) + "." + juce::String(secs).paddedLeft('0', 2) + "."
               + juce::String(static_cast<int>((seconds - whole) * 30.0)).paddedLeft('0', 2);
    return juce::String(hours).paddedLeft(' ', 3) + "." + juce::String(minutes).paddedLeft('0', 2) + "." + juce::String(secs).paddedLeft('0', 2) + "."
           + juce::String(static_cast<int>((seconds - whole) * 1000.0)).paddedLeft('0', 3);
}

void ControlSurfaces::refresh(bool force)
{
    for (auto& unit : units_)
        refreshUnit(*unit, force);
}

void ControlSurfaces::refreshUnit(Unit& unit, bool force)
{
    auto& session = project_.getSession();
    auto* editor = host_.editor ? host_.editor() : nullptr;
    const auto tracks = getStripTracks();
    juce::String top, bottom;
    bool anySolo = false;
    for (const auto& t : session.getAllTracks())
        anySolo = anySolo || session::track::isSoloed(t);

    for (int s = 0; s < kStrips; ++s)
    {
        const auto i = static_cast<std::size_t>(s);
        const auto track = trackAt(unit, s, tracks);
        const bool valid = track.isValid();
        const auto id = valid ? session::track::getId(track) : 0u;
        const int slot = valid ? project_.getCompiler().getSlot(id) : -1;

        // The fader (unless a hand is on it).
        if (!unit.touched[i])
        {
            int position = 0;
            if (valid)
            {
                if (flip_ && vpotMode_ == VpotMode::pan)
                    position = static_cast<int>(std::lround((session::track::getPan(track) + 1.0) * 0.5 * 16383.0));
                else if (flip_)
                    position = sendOf(track).isValid() ? faderPosition(session::send::getGainDb(sendOf(track))) : 0;
                else
                {
                    double db = session::track::getVolumeDb(track);
                    if (const float moving = host_.movingVolume ? host_.movingVolume(slot) : std::numeric_limits<float>::quiet_NaN(); !std::isnan(moving))
                        db = juce::Decibels::gainToDecibels(static_cast<double>(moving), kMinDb);
                    position = faderPosition(db);
                }
            }
            if (force || position != unit.fader[i])
            {
                unit.fader[i] = position;
                if (unit.isHui())
                    for (const auto& message : hui::fader(s, position))
                        send(unit, message);
                else
                    send(unit, mackie::fader(s, position));
            }
        }

        // The strip's keys.
        const std::array<bool, 4> lit{valid && session::track::isArmed(track), valid && session::track::isSoloed(track),
                                      valid && session::track::isMuted(track), valid && editor != nullptr && editor->isTrackSelected(id)};
        constexpr std::array<Key, 4> keys{Key::recArm, Key::solo, Key::mute, Key::select};
        for (std::size_t k = 0; k < 4; ++k)
            if (force || unit.stripLeds[i][k] != static_cast<int>(lit[k]))
            {
                unit.stripLeds[i][k] = static_cast<int>(lit[k]);
                if (unit.isHui())
                    for (const auto& message : hui::led(keys[k], s, lit[k] ? Led::on : Led::off))
                        send(unit, message);
                else
                    send(unit, mackie::led(keys[k], s, lit[k] ? Led::on : Led::off));
            }

        // The V-Pot's ring: the pan as a dot, a send or the (flipped) volume filling up.
        int ring = 0;
        if (valid)
        {
            if (flip_)
                ring = 0x20 | std::clamp(static_cast<int>(std::lround(faderPosition(session::track::getVolumeDb(track)) / 16383.0 * 11.0)), 0, 11);
            else if (vpotMode_ == VpotMode::pan)
                ring = std::clamp(static_cast<int>(std::lround(session::track::getPan(track) * 5.0)) + 6, 1, 11);
            else if (const auto sendTree = sendOf(track); sendTree.isValid())
                ring = 0x20 | std::clamp(static_cast<int>(std::lround(faderPosition(session::send::getGainDb(sendTree)) / 16383.0 * 11.0)), 0, 11);
        }
        if (force || ring != unit.ring[i])
        {
            unit.ring[i] = ring;
            const int mode = (ring >> 4) & 0x03;
            send(unit, unit.isHui() ? hui::ring(s, mode, ring & 0x0F) : mackie::ring(s, mode, ring & 0x0F));
        }

        // The meter: sent when it changes, and again while lit (it falls by itself).
        const float peak = slot >= 0 && host_.peak ? host_.peak(slot) : 0.0f;
        const int segment = meterSegment(juce::Decibels::gainToDecibels(peak, -100.0f));
        if (force || segment != unit.meter[i] || (segment > 0 && unit.meterAge[i] >= kMeterRepeat))
        {
            unit.meter[i] = segment;
            unit.meterAge[i] = 0.0;
            if (unit.isHui())
            {
                send(unit, hui::meter(s, 0, segment));
                send(unit, hui::meter(s, 1, segment));
            }
            else
                send(unit, mackie::meter(s, segment));
        }

        // Names and values.
        const auto name = valid ? session::track::getName(track) : juce::String();
        if (unit.isHui())
        {
            const auto shortName = fit(name, 4);
            if (force || shortName != unit.names[i])
            {
                unit.names[i] = shortName;
                send(unit, hui::stripName(s, shortName));
            }
            top += fit(name, 5);
            bottom += fit(valueText(track), 5, true);
        }
        else
        {
            top += fit(name, 6) + " ";
            bottom += fit(valueText(track), 7, true);
        }
    }

    if (unit.isHui())
    {
        // The main display: two strips' names (top) and values (bottom) per ten-character zone.
        for (int zone = 0; zone < 8; ++zone)
        {
            const auto text = (zone < 4 ? top : bottom).substring((zone % 4) * 10, (zone % 4) * 10 + 10);
            if (force || text != unit.zones[static_cast<std::size_t>(zone)])
            {
                unit.zones[static_cast<std::size_t>(zone)] = text;
                send(unit, hui::mainDisplay(zone, text));
            }
        }
    }
    else
    {
        if (force || top != unit.lcdTop)
        {
            unit.lcdTop = top;
            send(unit, mackie::lcd(unit.sysExModel(), 0, top));
        }
        if (force || bottom != unit.lcdBottom)
        {
            unit.lcdBottom = bottom;
            send(unit, mackie::lcd(unit.sysExModel(), mackie::kLcdWidth, bottom));
        }
    }

    // The master section: on a Mackie Control (or a HUI: its transport keys) only.
    if (unit.isMain() && !unit.touched[kMaster])
    {
        const int position = faderPosition(session.getMasterVolumeDb());
        if (force || position != unit.fader[kMaster])
        {
            unit.fader[kMaster] = position;
            send(unit, mackie::fader(kMaster, position));
        }
    }
    if (unit.isMain() || unit.isHui())
    {
        const bool playing = host_.playing && host_.playing();
        const bool recording = host_.recording && host_.recording();
        sendLed(unit, Key::play, 0, playing, force);
        sendLed(unit, Key::stop, 0, !playing, force);
        sendLed(unit, Key::record, 0, recording, force);
        sendLed(unit, Key::cycle, 0, session.isLoopEnabled(), force);
        sendLed(unit, Key::rewind, 0, shuttle_ < 0, force);
        sendLed(unit, Key::fastForward, 0, shuttle_ > 0, force);
        sendLed(unit, Key::scrub, 0, scrub_, force);
        sendLed(unit, Key::assignPan, 0, vpotMode_ == VpotMode::pan, force);
        sendLed(unit, Key::assignSend, 0, vpotMode_ == VpotMode::send, force);
        if (unit.isMain())
        {
            sendLed(unit, Key::flip, 0, flip_, force);
            sendLed(unit, Key::nameValue, 0, showValues_, force);
            // SMPTE / BEATS and RUDE SOLO are lamps of their own (notes 0x71 - 0x73).
            for (const auto& [note, on] : {std::pair{0x71, !showBeats_}, std::pair{0x72, showBeats_}, std::pair{0x73, anySolo}})
                if (const auto it = unit.leds.find(note); force || it == unit.leds.end() || it->second != static_cast<int>(on))
                {
                    unit.leds[note] = static_cast<int>(on);
                    send(unit, juce::MidiMessage::noteOn(1, note, static_cast<juce::uint8>(on ? 0x7F : 0x00)));
                }
        }

        const auto time = timeText(unit.isHui());
        if (force || time != unit.time)
        {
            unit.time = time;
            if (unit.isHui())
                send(unit, hui::timeDisplay(time));
            else
                for (const auto& message : mackie::timeDisplay(time))
                    send(unit, message);
        }
        if (unit.isMain())
        {
            const auto assignment = flip_ ? juce::String("FL") : (vpotMode_ == VpotMode::pan ? juce::String("PN") : "S" + juce::String(sendSlot_ + 1));
            if (force || assignment != unit.assignment)
            {
                unit.assignment = assignment;
                for (const auto& message : mackie::assignmentDisplay(assignment))
                    send(unit, message);
            }
        }
    }
}

} // namespace dojo::ui::surfaces
