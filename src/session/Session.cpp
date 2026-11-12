#include "session/Session.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <set>

namespace dojo::session
{
namespace
{
juce::ValueTree createEmptyState()
{
    juce::ValueTree state(ids::session);
    state.setProperty(ids::version, Session::kFileFormatVersion, nullptr);
    state.setProperty(ids::tempo, 120.0, nullptr);
    state.setProperty(ids::masterVolume, 0.0, nullptr);
    state.setProperty(ids::nextId, 1, nullptr);
    state.setProperty(ids::loopEnabled, false, nullptr);
    state.setProperty(ids::loopStart, 0.0, nullptr);
    state.setProperty(ids::loopEnd, 8.0, nullptr);
    state.setProperty(ids::speed, 1.0, nullptr);
    state.setProperty(ids::keepPitch, false, nullptr);
    state.setProperty(ids::transpose, 0.0, nullptr);
    state.addChild(juce::ValueTree(ids::master), -1, nullptr);
    return state;
}

std::uint32_t readId(const juce::ValueTree& tree)
{
    return static_cast<std::uint32_t>(static_cast<int>(tree.getProperty(ids::id, 0)));
}

void collectTracks(const juce::ValueTree& parent, std::vector<juce::ValueTree>& out)
{
    for (const auto& child : parent)
    {
        if (!child.hasType(ids::track))
            continue;
        out.push_back(child);
        collectTracks(child, out);
    }
}

int countTracks(const juce::ValueTree& parent)
{
    int count = 0;
    for (const auto& child : parent)
        if (child.hasType(ids::track))
            count += 1 + countTracks(child);
    return count;
}

/** Index among `parent`'s children at which a track may be inserted in front of `before`. */
int childIndexOf(const juce::ValueTree& parent, const juce::ValueTree& before)
{
    return before.isValid() ? parent.indexOf(before) : -1;
}

juce::String defaultTrackName(TrackKind kind, bool folder, int number)
{
    if (folder)
        return "Folder " + juce::String(number);
    switch (kind)
    {
    case TrackKind::instrument:  return "Instrument " + juce::String(number);
    case TrackKind::bus:         return "Bus " + juce::String(number);
    case TrackKind::midi:        return "MIDI " + juce::String(number);
    case TrackKind::vca:         return "VCA " + juce::String(number);
    case TrackKind::masterFader: return "Master " + juce::String(number);
    case TrackKind::video:       return "Video " + juce::String(number);
    case TrackKind::audio:       break;
    }
    return "Audio " + juce::String(number);
}
} // namespace

const std::array<juce::Colour, 8>& getTrackPalette()
{
    static const std::array<juce::Colour, 8> palette{
        juce::Colour{0xffe0533d}, // vermilion
        juce::Colour{0xffe58a3d}, // persimmon
        juce::Colour{0xffd6a645}, // gold
        juce::Colour{0xff8fb35a}, // matcha
        juce::Colour{0xff3fb59b}, // jade
        juce::Colour{0xff4f8fd9}, // indigo
        juce::Colour{0xff8a6fd6}, // wisteria
        juce::Colour{0xffd66fa7}, // sakura
    };
    return palette;
}

juce::String toString(TrackKind kind)
{
    switch (kind)
    {
    case TrackKind::instrument:  return "instrument";
    case TrackKind::bus:         return "bus";
    case TrackKind::midi:        return "midi";
    case TrackKind::vca:         return "vca";
    case TrackKind::masterFader: return "masterFader";
    case TrackKind::video:       return "video";
    case TrackKind::audio:       break;
    }
    return "audio";
}

juce::String toString(TrackFormat format)
{
    switch (format)
    {
    case TrackFormat::mono:         return "mono";
    case TrackFormat::lcrLfe:       return "3.1";
    case TrackFormat::surround51:   return "5.1";
    case TrackFormat::surround71:   return "7.1";
    case TrackFormat::dolbyDigital: return "dolbyDigital";
    case TrackFormat::dts:          return "dts";
    case TrackFormat::stereo:       break;
    }
    return "stereo";
}

TrackFormat trackFormatFromString(const juce::String& text)
{
    for (const auto format : getAllTrackFormats())
        if (text == toString(format))
            return format;
    return TrackFormat::stereo;
}

juce::String getDisplayName(TrackFormat format)
{
    switch (format)
    {
    case TrackFormat::mono:         return "Mono";
    case TrackFormat::lcrLfe:       return "3.1";
    case TrackFormat::surround51:   return "5.1";
    case TrackFormat::surround71:   return "7.1";
    case TrackFormat::dolbyDigital: return "Dolby Digital 5.1";
    case TrackFormat::dts:          return "DTS 5.1";
    case TrackFormat::stereo:       break;
    }
    return "Stereo";
}

int getNumChannels(TrackFormat format)
{
    return getLayout(format).numChannels;
}

int fileChannelFor(int fileChannels, const engine::ChannelLayout& track, int trackChannel)
{
    if (fileChannels <= 0 || trackChannel < 0 || trackChannel >= track.numChannels)
        return -1;
    const auto speaker = track.speakers[static_cast<std::size_t>(trackChannel)];
    if (fileChannels == 1)
    {
        const bool toCentre = track.has(engine::Speaker::centre);
        return (toCentre ? speaker == engine::Speaker::centre : (speaker == engine::Speaker::left || speaker == engine::Speaker::right)) ? 0 : -1;
    }
    if (fileChannels == track.numChannels)
        return trackChannel;
    switch (fileChannels)
    {
    case 2: return engine::ChannelLayout::stereo().indexOf(speaker);
    case 4: return engine::ChannelLayout::lcrLfe().indexOf(speaker);
    case 6: return engine::ChannelLayout::surround51().indexOf(speaker);
    case 8: return engine::ChannelLayout::surround71().indexOf(speaker);
    default: break;
    }
    return trackChannel < fileChannels ? trackChannel : -1;
}

std::vector<TrackFormat> getAllTrackFormats()
{
    return {TrackFormat::mono,       TrackFormat::stereo,       TrackFormat::lcrLfe, TrackFormat::surround51,
            TrackFormat::surround71, TrackFormat::dolbyDigital, TrackFormat::dts};
}

engine::ChannelLayout getLayout(TrackFormat format)
{
    switch (format)
    {
    case TrackFormat::mono:         return engine::ChannelLayout::mono();
    case TrackFormat::lcrLfe:       return engine::ChannelLayout::lcrLfe();
    case TrackFormat::surround51:   return engine::ChannelLayout::surround51();
    case TrackFormat::surround71:   return engine::ChannelLayout::surround71();
    case TrackFormat::dolbyDigital: return engine::ChannelLayout::dolbyDigital();
    case TrackFormat::dts:          return engine::ChannelLayout::dts();
    case TrackFormat::stereo:       break;
    }
    return engine::ChannelLayout::stereo();
}

TrackKind trackKindFromString(const juce::String& text)
{
    if (text == "instrument")
        return TrackKind::instrument;
    if (text == "bus")
        return TrackKind::bus;
    if (text == "midi")
        return TrackKind::midi;
    if (text == "vca")
        return TrackKind::vca;
    if (text == "masterFader")
        return TrackKind::masterFader;
    if (text == "video")
        return TrackKind::video;
    return TrackKind::audio;
}

bool holdsMidi(TrackKind kind) { return kind == TrackKind::instrument || kind == TrackKind::midi; }
bool holdsAudio(TrackKind kind) { return kind == TrackKind::audio; }
bool holdsVideo(TrackKind kind) { return kind == TrackKind::video; }
bool isRendered(TrackKind kind) { return kind != TrackKind::vca && kind != TrackKind::masterFader && kind != TrackKind::video; }

//==================================================================================================
// Track accessors

namespace track
{
std::uint32_t getId(const juce::ValueTree& t) { return readId(t); }
juce::String getName(const juce::ValueTree& t) { return t.getProperty(ids::name).toString(); }
juce::Colour getColour(const juce::ValueTree& t)
{
    return juce::Colour::fromString(t.getProperty(ids::colour, "ffe0533d").toString());
}
TrackKind getKind(const juce::ValueTree& t) { return trackKindFromString(t.getProperty(ids::kind).toString()); }
bool isFolder(const juce::ValueTree& t) { return t.getProperty(ids::folder, false); }
bool isCollapsed(const juce::ValueTree& t) { return t.getProperty(ids::collapsed, false); }
double getVolumeDb(const juce::ValueTree& t) { return t.getProperty(ids::volume, 0.0); }
double getPan(const juce::ValueTree& t) { return t.getProperty(ids::pan, 0.0); }
bool isMuted(const juce::ValueTree& t) { return t.getProperty(ids::muted, false); }
bool isSoloed(const juce::ValueTree& t) { return t.getProperty(ids::soloed, false); }
bool isPhaseInverted(const juce::ValueTree& t) { return t.getProperty(ids::phase, false); }
std::uint32_t getOutput(const juce::ValueTree& t)
{
    return static_cast<std::uint32_t>(static_cast<int>(t.getProperty(ids::output, 0)));
}
int getInput(const juce::ValueTree& t) { return t.getProperty(ids::input, -1); }
bool isInputStereo(const juce::ValueTree& t)
{
    return getFormat(t) != TrackFormat::mono && static_cast<bool>(t.getProperty(ids::inputStereo, true));
}
engine::MonitorMode getMonitor(const juce::ValueTree& t)
{
    return static_cast<engine::MonitorMode>(juce::jlimit(0, 2, static_cast<int>(t.getProperty(ids::monitor, 1))));
}
bool isArmed(const juce::ValueTree& t) { return t.getProperty(ids::armed, false); }
double getSpeed(const juce::ValueTree& t) { return t.getProperty(ids::speed, 1.0); }
double getPitch(const juce::ValueTree& t) { return t.getProperty(ids::pitch, 0.0); }
std::uint32_t getAuxSource(const juce::ValueTree& t)
{
    return static_cast<std::uint32_t>(static_cast<int>(t.getProperty(ids::auxSource, 0)));
}
int getAuxChannel(const juce::ValueTree& t) { return t.getProperty(ids::auxChannel, 0); }
bool isShowingAutomation(const juce::ValueTree& t) { return t.getProperty(ids::showAutomation, false); }
TrackFormat getFormat(const juce::ValueTree& t)
{
    if (getKind(t) != TrackKind::audio && getKind(t) != TrackKind::bus)
        return TrackFormat::stereo;
    return trackFormatFromString(t.getProperty(ids::format, "stereo").toString());
}
double getPanFrontRear(const juce::ValueTree& t) { return t.getProperty(ids::panFrontRear, -1.0); }
double getPanCentre(const juce::ValueTree& t) { return t.getProperty(ids::panCentre, 0.0); }
bool isActive(const juce::ValueTree& t) { return t.getProperty(ids::active, true); }
std::uint32_t getMidiTarget(const juce::ValueTree& t) { return static_cast<std::uint32_t>(static_cast<int>(t.getProperty(ids::midiTarget, 0))); }
std::uint32_t getVca(const juce::ValueTree& t) { return static_cast<std::uint32_t>(static_cast<int>(t.getProperty(ids::vca, 0))); }
std::uint32_t getControlledBus(const juce::ValueTree& t) { return static_cast<std::uint32_t>(static_cast<int>(t.getProperty(ids::controls, 0))); }

void setName(juce::ValueTree t, const juce::String& name, juce::UndoManager* um)
{
    t.setProperty(ids::name, name.trim().isEmpty() ? juce::String("Track") : name.trim(), um);
}
void setColour(juce::ValueTree t, juce::Colour colour, juce::UndoManager* um)
{
    t.setProperty(ids::colour, colour.toString(), um);
}
void setCollapsed(juce::ValueTree t, bool collapsed, juce::UndoManager* um) { t.setProperty(ids::collapsed, collapsed, um); }
void setVolumeDb(juce::ValueTree t, double db, juce::UndoManager* um)
{
    t.setProperty(ids::volume, juce::jlimit(kMinusInfinityDb, kMaxVolumeDb, db), um);
}
void setPan(juce::ValueTree t, double pan, juce::UndoManager* um)
{
    t.setProperty(ids::pan, juce::jlimit(-1.0, 1.0, pan), um);
}
void setMuted(juce::ValueTree t, bool muted, juce::UndoManager* um) { t.setProperty(ids::muted, muted, um); }
void setSoloed(juce::ValueTree t, bool soloed, juce::UndoManager* um) { t.setProperty(ids::soloed, soloed, um); }
void setPhaseInverted(juce::ValueTree t, bool inverted, juce::UndoManager* um)
{
    t.setProperty(ids::phase, inverted, um);
}
void setOutput(juce::ValueTree t, std::uint32_t busId, juce::UndoManager* um)
{
    t.setProperty(ids::output, static_cast<int>(busId), um);
}
void setInput(juce::ValueTree t, int firstChannel, bool stereo, juce::UndoManager* um)
{
    t.setProperty(ids::input, std::max(-1, firstChannel), um);
    t.setProperty(ids::inputStereo, stereo, um);
}
void setMonitor(juce::ValueTree t, engine::MonitorMode mode, juce::UndoManager* um)
{
    t.setProperty(ids::monitor, static_cast<int>(mode), um);
}
void setArmed(juce::ValueTree t, bool armed, juce::UndoManager* um) { t.setProperty(ids::armed, armed, um); }
void setSpeed(juce::ValueTree t, double speed, juce::UndoManager* um)
{
    t.setProperty(ids::speed, juce::jlimit(kMinStretch, kMaxStretch, speed), um);
}
void setPitch(juce::ValueTree t, double semitones, juce::UndoManager* um)
{
    t.setProperty(ids::pitch, juce::jlimit(-24.0, 24.0, semitones), um);
}
void setAuxSource(juce::ValueTree t, std::uint32_t instrumentTrackId, int firstChannel, juce::UndoManager* um)
{
    t.setProperty(ids::auxSource, static_cast<int>(instrumentTrackId), um);
    t.setProperty(ids::auxChannel, std::max(0, firstChannel), um);
}
void setShowingAutomation(juce::ValueTree t, bool show, juce::UndoManager* um)
{
    t.setProperty(ids::showAutomation, show, um);
}
void setFormat(juce::ValueTree t, TrackFormat format, juce::UndoManager* um)
{
    t.setProperty(ids::format, toString(format), um);
}
void setPanFrontRear(juce::ValueTree t, double frontRear, juce::UndoManager* um)
{
    t.setProperty(ids::panFrontRear, juce::jlimit(-1.0, 1.0, frontRear), um);
}
void setPanCentre(juce::ValueTree t, double centre, juce::UndoManager* um)
{
    t.setProperty(ids::panCentre, juce::jlimit(0.0, 1.0, centre), um);
}
void setActive(juce::ValueTree t, bool active, juce::UndoManager* um)
{
    t.setProperty(ids::active, active, um);
}
void setMidiTarget(juce::ValueTree t, std::uint32_t instrumentTrackId, juce::UndoManager* um)
{
    t.setProperty(ids::midiTarget, static_cast<int>(instrumentTrackId), um);
}
void setVca(juce::ValueTree t, std::uint32_t vcaTrackId, juce::UndoManager* um)
{
    t.setProperty(ids::vca, static_cast<int>(vcaTrackId), um);
}
void setControlledBus(juce::ValueTree t, std::uint32_t busId, juce::UndoManager* um)
{
    t.setProperty(ids::controls, static_cast<int>(busId), um);
}
} // namespace track

//==================================================================================================
// Clip accessors

namespace clip
{
std::uint32_t getId(const juce::ValueTree& c) { return readId(c); }
bool isMidi(const juce::ValueTree& c) { return c.hasType(ids::midiClip); }
bool isVideo(const juce::ValueTree& c) { return c.hasType(ids::videoClip); }
juce::String getName(const juce::ValueTree& c) { return c.getProperty(ids::name).toString(); }
juce::File getFile(const juce::ValueTree& c) { return juce::File(c.getProperty(ids::file).toString()); }
double getStart(const juce::ValueTree& c) { return c.getProperty(ids::start, 0.0); }
double getLength(const juce::ValueTree& c) { return c.getProperty(ids::length, 0.0); }
double getEnd(const juce::ValueTree& c) { return getStart(c) + getLength(c); }
double getOffset(const juce::ValueTree& c) { return c.getProperty(ids::offset, 0.0); }
double getFadeIn(const juce::ValueTree& c) { return c.getProperty(ids::fadeIn, 0.0); }
double getFadeOut(const juce::ValueTree& c) { return c.getProperty(ids::fadeOut, 0.0); }
double getGainDb(const juce::ValueTree& c) { return c.getProperty(ids::gain, 0.0); }
double getStretch(const juce::ValueTree& c) { return c.getProperty(ids::stretch, 1.0); }
double getPitch(const juce::ValueTree& c) { return c.getProperty(ids::pitch, 0.0); }
int getSourceChannel(const juce::ValueTree& c) { return std::max(-1, static_cast<int>(c.getProperty(ids::sourceChannel, -1))); }

void setStart(juce::ValueTree c, double seconds, juce::UndoManager* um)
{
    c.setProperty(ids::start, std::max(0.0, seconds), um);
}
void setLength(juce::ValueTree c, double seconds, juce::UndoManager* um)
{
    c.setProperty(ids::length, std::max(kMinClipLength, seconds), um);
}
void setOffset(juce::ValueTree c, double value, juce::UndoManager* um)
{
    c.setProperty(ids::offset, isMidi(c) ? value : std::max(0.0, value), um);
}
void setFadeIn(juce::ValueTree c, double seconds, juce::UndoManager* um)
{
    c.setProperty(ids::fadeIn, juce::jlimit(0.0, std::max(0.0, getLength(c) - getFadeOut(c)), seconds), um);
}
void setFadeOut(juce::ValueTree c, double seconds, juce::UndoManager* um)
{
    c.setProperty(ids::fadeOut, juce::jlimit(0.0, std::max(0.0, getLength(c) - getFadeIn(c)), seconds), um);
}
void setGainDb(juce::ValueTree c, double db, juce::UndoManager* um)
{
    c.setProperty(ids::gain, juce::jlimit(kMinusInfinityDb, 24.0, db), um);
}
void setStretch(juce::ValueTree c, double ratio, juce::UndoManager* um)
{
    c.setProperty(ids::stretch, juce::jlimit(kMinStretch, kMaxStretch, ratio), um);
}
void setPitch(juce::ValueTree c, double semitones, juce::UndoManager* um)
{
    c.setProperty(ids::pitch, juce::jlimit(-24.0, 24.0, semitones), um);
}
void setName(juce::ValueTree c, const juce::String& name, juce::UndoManager* um) { c.setProperty(ids::name, name, um); }
void setSourceChannel(juce::ValueTree c, int channel, juce::UndoManager* um)
{
    c.setProperty(ids::sourceChannel, std::max(-1, channel), um);
}
int getRating(const juce::ValueTree& c) { return juce::jlimit(0, 5, static_cast<int>(c.getProperty(ids::rating, 0))); }
void setRating(juce::ValueTree c, int rating, juce::UndoManager* um)
{
    rating = juce::jlimit(0, 5, rating);
    if (rating == 0)
        c.removeProperty(ids::rating, um);
    else
        c.setProperty(ids::rating, rating, um);
}

std::vector<std::uint32_t> getGroups(const juce::ValueTree& c)
{
    std::vector<std::uint32_t> groups;
    for (const auto& token : juce::StringArray::fromTokens(c.getProperty(ids::clipGroups).toString(), ",", ""))
        if (const auto id = static_cast<std::uint32_t>(token.getLargeIntValue()); id != 0)
            groups.push_back(id);
    return groups;
}
std::uint32_t getOuterGroup(const juce::ValueTree& c)
{
    const auto groups = getGroups(c);
    return groups.empty() ? 0u : groups.front();
}
void setGroups(juce::ValueTree c, const std::vector<std::uint32_t>& groups, juce::UndoManager* um)
{
    if (groups.empty())
    {
        c.removeProperty(ids::clipGroups, um);
        return;
    }
    juce::StringArray tokens;
    for (const auto id : groups)
        tokens.add(juce::String(static_cast<juce::int64>(id)));
    c.setProperty(ids::clipGroups, tokens.joinIntoString(","), um);
}

bool isLooped(const juce::ValueTree& c) { return c.getProperty(ids::looped, false); }
double getLoopLength(const juce::ValueTree& c)
{
    return isLooped(c) ? std::max(kMinClipLength, static_cast<double>(c.getProperty(ids::loopLength, getLength(c)))) : getLength(c);
}
void setLoop(juce::ValueTree c, bool looped, double iterationLength, juce::UndoManager* um)
{
    if (!looped)
    {
        c.removeProperty(ids::looped, um);
        c.removeProperty(ids::loopLength, um);
        return;
    }
    c.setProperty(ids::looped, true, um);
    c.setProperty(ids::loopLength, std::max(kMinClipLength, iterationLength), um);
}

std::optional<double> getSyncPoint(const juce::ValueTree& c)
{
    if (!c.hasProperty(ids::syncPoint))
        return std::nullopt;
    return static_cast<double>(c.getProperty(ids::syncPoint));
}
void setSyncPoint(juce::ValueTree c, std::optional<double> secondsFromStart, juce::UndoManager* um)
{
    if (secondsFromStart.has_value())
        c.setProperty(ids::syncPoint, std::max(0.0, *secondsFromStart), um);
    else
        c.removeProperty(ids::syncPoint, um);
}
double getReferenceTime(const juce::ValueTree& c) { return getStart(c) + getSyncPoint(c).value_or(0.0); }

bool isGainBypassed(const juce::ValueTree& c) { return c.getProperty(ids::gainBypassed, false); }
void setGainBypassed(juce::ValueTree c, bool bypassed, juce::UndoManager* um)
{
    if (bypassed)
        c.setProperty(ids::gainBypassed, true, um);
    else
        c.removeProperty(ids::gainBypassed, um);
}

bool isUserNamed(const juce::ValueTree& c) { return c.getProperty(ids::userNamed, false); }

bool isEditLocked(const juce::ValueTree& c) { return c.getProperty(ids::editLocked, false); }
bool isTimeLocked(const juce::ValueTree& c) { return c.getProperty(ids::timeLocked, false); }
void setEditLocked(juce::ValueTree c, bool locked, juce::UndoManager* um)
{
    if (locked)
        c.setProperty(ids::editLocked, true, um);
    else
        c.removeProperty(ids::editLocked, um);
}
void setTimeLocked(juce::ValueTree c, bool locked, juce::UndoManager* um)
{
    if (locked)
        c.setProperty(ids::timeLocked, true, um);
    else
        c.removeProperty(ids::timeLocked, um);
}
} // namespace clip

//==================================================================================================
// Note accessors

namespace note
{
double getStart(const juce::ValueTree& n) { return n.getProperty(ids::start, 0.0); }
double getLength(const juce::ValueTree& n) { return n.getProperty(ids::length, 0.25); }
double getEnd(const juce::ValueTree& n) { return getStart(n) + getLength(n); }
int getNote(const juce::ValueTree& n) { return n.getProperty(ids::noteNumber, 60); }
float getVelocity(const juce::ValueTree& n) { return static_cast<float>(static_cast<double>(n.getProperty(ids::velocity, 0.8))); }
int getChannel(const juce::ValueTree& n) { return n.getProperty(ids::channel, 1); }

void setStart(juce::ValueTree n, double beat, juce::UndoManager* um) { n.setProperty(ids::start, beat, um); }
void setLength(juce::ValueTree n, double beats, juce::UndoManager* um)
{
    n.setProperty(ids::length, std::max(kMinNoteLength, beats), um);
}
void setNote(juce::ValueTree n, int noteNumber, juce::UndoManager* um)
{
    n.setProperty(ids::noteNumber, juce::jlimit(0, 127, noteNumber), um);
}
void setVelocity(juce::ValueTree n, float velocity, juce::UndoManager* um)
{
    n.setProperty(ids::velocity, juce::jlimit(0.01, 1.0, static_cast<double>(velocity)), um);
}
} // namespace note

//==================================================================================================
// Device / send / automation accessors

namespace device
{
std::uint32_t getId(const juce::ValueTree& d) { return readId(d); }
juce::String getType(const juce::ValueTree& d) { return d.getProperty(ids::type).toString(); }
juce::String getName(const juce::ValueTree& d) { return d.getProperty(ids::name).toString(); }
bool isBypassed(const juce::ValueTree& d) { return d.getProperty(ids::bypassed, false); }
std::uint32_t getSidechain(const juce::ValueTree& d)
{
    return static_cast<std::uint32_t>(static_cast<int>(d.getProperty(ids::sidechain, 0)));
}
void setBypassed(juce::ValueTree d, bool bypassed, juce::UndoManager* um) { d.setProperty(ids::bypassed, bypassed, um); }
void setSidechain(juce::ValueTree d, std::uint32_t trackId, juce::UndoManager* um)
{
    d.setProperty(ids::sidechain, static_cast<int>(trackId), um);
}
} // namespace device

namespace send
{
std::uint32_t getId(const juce::ValueTree& s) { return readId(s); }
std::uint32_t getTarget(const juce::ValueTree& s)
{
    return static_cast<std::uint32_t>(static_cast<int>(s.getProperty(ids::target, 0)));
}
double getGainDb(const juce::ValueTree& s) { return s.getProperty(ids::gain, -6.0); }
bool isPreFader(const juce::ValueTree& s) { return s.getProperty(ids::preFader, false); }
void setGainDb(juce::ValueTree s, double db, juce::UndoManager* um)
{
    s.setProperty(ids::gain, juce::jlimit(kMinusInfinityDb, kMaxVolumeDb, db), um);
}
void setPreFader(juce::ValueTree s, bool preFader, juce::UndoManager* um) { s.setProperty(ids::preFader, preFader, um); }
} // namespace send

namespace automation
{
juce::String getTarget(const juce::ValueTree& lane) { return lane.getProperty(ids::target).toString(); }
bool isVisible(const juce::ValueTree& lane) { return lane.getProperty(ids::visible, true); }
void setVisible(juce::ValueTree lane, bool visible, juce::UndoManager* um) { lane.setProperty(ids::visible, visible, um); }

juce::String volumeTarget() { return "volume"; }
juce::String panTarget() { return "pan"; }
juce::String sendTarget(std::uint32_t sendId) { return "send:" + juce::String(static_cast<int>(sendId)); }
juce::String parameterTarget(std::uint32_t deviceId, int parameterIndex)
{
    return "param:" + juce::String(static_cast<int>(deviceId)) + ":" + juce::String(parameterIndex);
}

bool parseParameterTarget(const juce::String& target, std::uint32_t& deviceId, int& parameterIndex)
{
    if (!target.startsWith("param:"))
        return false;
    const auto rest = target.fromFirstOccurrenceOf("param:", false, false);
    const auto deviceText = rest.upToFirstOccurrenceOf(":", false, false);
    const auto indexText = rest.fromFirstOccurrenceOf(":", false, false);
    if (deviceText.isEmpty() || indexText.isEmpty())
        return false;
    deviceId = static_cast<std::uint32_t>(deviceText.getIntValue());
    parameterIndex = indexText.getIntValue();
    return deviceId != 0 && parameterIndex >= 0;
}

bool parseSendTarget(const juce::String& target, std::uint32_t& sendId)
{
    if (!target.startsWith("send:"))
        return false;
    sendId = static_cast<std::uint32_t>(target.fromFirstOccurrenceOf("send:", false, false).getIntValue());
    return sendId != 0;
}

engine::AutomationCurve toCurve(const juce::ValueTree& lane)
{
    std::vector<engine::AutomationPoint> points;
    points.reserve(static_cast<std::size_t>(lane.getNumChildren()));
    for (const auto& p : lane)
        if (p.hasType(ids::point))
            points.push_back({static_cast<double>(p.getProperty(ids::time, 0.0)),
                              juce::jlimit(0.0, 1.0, static_cast<double>(p.getProperty(ids::value, 0.0)))});

    engine::AutomationCurve curve;
    curve.setPoints(std::move(points));
    return curve;
}
} // namespace automation

//==================================================================================================
// Session

Session::Session()
    : state_(createEmptyState())
{
}

std::uint32_t Session::allocateId()
{
    const int next = state_.getProperty(ids::nextId, 1);
    state_.setProperty(ids::nextId, next + 1, nullptr); // ids are never reused, even across undo
    return static_cast<std::uint32_t>(next);
}

void Session::reassignIds(juce::ValueTree tree)
{
    if (tree.hasProperty(ids::id))
        tree.setProperty(ids::id, static_cast<int>(allocateId()), nullptr);
    for (auto child : tree)
        reassignIds(child);
}

// --- Global ---------------------------------------------------------------------------------------

double Session::getTempo() const { return state_.getProperty(ids::tempo, 120.0); }
void Session::setTempo(double bpm) { state_.setProperty(ids::tempo, juce::jlimit(kMinTempo, kMaxTempo, bpm), &undo_); }
double Session::getMasterVolumeDb() const { return state_.getProperty(ids::masterVolume, 0.0); }
void Session::setMasterVolumeDb(double db)
{
    state_.setProperty(ids::masterVolume, juce::jlimit(kMinusInfinityDb, kMaxVolumeDb, db), &undo_);
}

bool Session::isLoopEnabled() const { return state_.getProperty(ids::loopEnabled, false); }
double Session::getLoopStart() const { return state_.getProperty(ids::loopStart, 0.0); }
double Session::getLoopEnd() const { return state_.getProperty(ids::loopEnd, 8.0); }

void Session::setLoop(bool enabled, double startSeconds, double endSeconds)
{
    startSeconds = std::max(0.0, startSeconds);
    endSeconds = std::max(startSeconds + kMinClipLength, endSeconds);
    state_.setProperty(ids::loopEnabled, enabled, &undo_);
    state_.setProperty(ids::loopStart, startSeconds, &undo_);
    state_.setProperty(ids::loopEnd, endSeconds, &undo_);
}

void Session::setLoopEnabled(bool enabled) { state_.setProperty(ids::loopEnabled, enabled, &undo_); }

TrackFormat Session::getMasterFormat() const
{
    const auto format = trackFormatFromString(state_.getProperty(ids::masterFormat, "stereo").toString());
    return format == TrackFormat::mono ? TrackFormat::stereo : format;
}

void Session::setMasterFormat(TrackFormat format)
{
    if (format == TrackFormat::mono)
        format = TrackFormat::stereo;
    state_.setProperty(ids::masterFormat, toString(format), &undo_);
}

bool Session::isPunchInEnabled() const { return state_.getProperty(ids::punchIn, false); }
bool Session::isPunchOutEnabled() const { return state_.getProperty(ids::punchOut, false); }
void Session::setPunchInEnabled(bool enabled) { state_.setProperty(ids::punchIn, enabled, nullptr); }
void Session::setPunchOutEnabled(bool enabled) { state_.setProperty(ids::punchOut, enabled, nullptr); }

std::pair<double, double> Session::getPunchWindow() const
{
    return {isPunchInEnabled() ? getLoopStart() : 0.0, isPunchOutEnabled() ? getLoopEnd() : std::numeric_limits<double>::infinity()};
}

bool Session::isInputOnlyMonitoring() const { return state_.getProperty(ids::inputOnly, false); }
void Session::setInputOnlyMonitoring(bool inputOnly) { state_.setProperty(ids::inputOnly, inputOnly, nullptr); }

double Session::getSpeed() const { return state_.getProperty(ids::speed, 1.0); }
bool Session::getKeepPitch() const { return state_.getProperty(ids::keepPitch, false); }
void Session::setSpeed(double speed)
{
    state_.setProperty(ids::speed, juce::jlimit(kMinStretch, kMaxStretch, speed), &undo_);
}
void Session::setKeepPitch(bool keepPitch) { state_.setProperty(ids::keepPitch, keepPitch, &undo_); }
double Session::getTranspose() const { return state_.getProperty(ids::transpose, 0.0); }
void Session::setTranspose(double semitones)
{
    state_.setProperty(ids::transpose, juce::jlimit(-24.0, 24.0, semitones), &undo_);
}

juce::ValueTree Session::getMaster()
{
    auto masterNode = state_.getChildWithName(ids::master);
    if (!masterNode.isValid())
    {
        masterNode = juce::ValueTree(ids::master);
        state_.addChild(masterNode, 0, nullptr);
    }
    return masterNode;
}

// --- Tracks ---------------------------------------------------------------------------------------

std::vector<juce::ValueTree> Session::getAllTracks() const
{
    std::vector<juce::ValueTree> tracks;
    tracks.reserve(32);
    collectTracks(state_, tracks);
    return tracks;
}

int Session::getNumTracks() const { return countTracks(state_); }

juce::ValueTree Session::getTrack(int index) const
{
    if (index < 0)
        return {};
    const auto tracks = getAllTracks();
    return index < static_cast<int>(tracks.size()) ? tracks[static_cast<std::size_t>(index)] : juce::ValueTree();
}

int Session::indexOf(const juce::ValueTree& t) const
{
    const auto tracks = getAllTracks();
    const auto it = std::find(tracks.begin(), tracks.end(), t);
    return it != tracks.end() ? static_cast<int>(it - tracks.begin()) : -1;
}

juce::ValueTree Session::findTrack(std::uint32_t trackId) const
{
    if (trackId == 0)
        return {};
    for (const auto& t : getAllTracks())
        if (track::getId(t) == trackId)
            return t;
    return {};
}

juce::ValueTree Session::getParentFolder(const juce::ValueTree& t)
{
    const auto parent = t.getParent();
    return parent.hasType(ids::track) ? parent : juce::ValueTree();
}

int Session::getDepth(const juce::ValueTree& t)
{
    int depth = 0;
    for (auto parent = getParentFolder(t); parent.isValid(); parent = getParentFolder(parent))
        ++depth;
    return depth;
}

bool Session::isWithin(const juce::ValueTree& t, const juce::ValueTree& ancestor)
{
    for (auto node = t; node.isValid(); node = node.getParent())
        if (node == ancestor)
            return true;
    return false;
}

bool Session::isEffectivelyActive(const juce::ValueTree& t)
{
    for (auto node = t; node.isValid() && node.hasType(ids::track); node = node.getParent())
        if (!track::isActive(node))
            return false;
    return true;
}

bool Session::isVisibleInArranger(const juce::ValueTree& t)
{
    for (auto parent = getParentFolder(t); parent.isValid(); parent = getParentFolder(parent))
        if (track::isCollapsed(parent))
            return false;
    return true;
}

juce::ValueTree Session::insertTrackTree(juce::ValueTree t, int index)
{
    juce::ValueTree parent = state_;
    juce::ValueTree before;

    if (index >= 0)
    {
        before = getTrack(index);
        if (before.isValid())
            parent = before.getParent();
    }

    parent.addChild(t, childIndexOf(parent, before), &undo_);
    return t;
}

juce::ValueTree Session::addTrack(const juce::String& name, int index)
{
    return addTrack(TrackKind::audio, name, index);
}

juce::ValueTree Session::addTrack(TrackKind kind, const juce::String& name, int index)
{
    juce::ValueTree t(ids::track);
    const auto id = allocateId();
    const auto& palette = getTrackPalette();
    const int count = getNumTracks();

    t.setProperty(ids::id, static_cast<int>(id), nullptr);
    t.setProperty(ids::name, name.isNotEmpty() ? name : defaultTrackName(kind, false, count + 1), nullptr);
    t.setProperty(ids::colour, palette[static_cast<std::size_t>(count) % palette.size()].toString(), nullptr);
    t.setProperty(ids::kind, toString(kind), nullptr);
    t.setProperty(ids::volume, 0.0, nullptr);
    t.setProperty(ids::pan, 0.0, nullptr);
    t.setProperty(ids::muted, false, nullptr);
    t.setProperty(ids::soloed, false, nullptr);
    t.setProperty(ids::phase, false, nullptr);

    if (kind == TrackKind::audio)
    {
        t.setProperty(ids::input, 0, nullptr);
        t.setProperty(ids::inputStereo, true, nullptr);
    }

    if (kind == TrackKind::audio || kind == TrackKind::instrument || kind == TrackKind::midi)
        t.setProperty(ids::monitor, static_cast<int>(engine::MonitorMode::automatic), nullptr);

    return insertTrackTree(t, index);
}

juce::ValueTree Session::addFolder(const juce::String& name, int index)
{
    auto t = addTrack(TrackKind::bus, name.isNotEmpty() ? name : defaultTrackName(TrackKind::bus, true, getNumTracks() + 1),
                      index);
    t.setProperty(ids::folder, true, &undo_);
    return t;
}

std::vector<juce::ValueTree> Session::outermostInOrder(const std::vector<juce::ValueTree>& tracks) const
{
    const auto listed = [&tracks](const juce::ValueTree& t) { return std::find(tracks.begin(), tracks.end(), t) != tracks.end(); };
    std::vector<juce::ValueTree> ordered;
    for (const auto& t : getAllTracks())
    {
        if (!listed(t))
            continue;
        bool nested = false;
        for (auto parent = getParentFolder(t); parent.isValid() && !nested; parent = getParentFolder(parent))
            nested = listed(parent);
        if (!nested)
            ordered.push_back(t);
    }
    return ordered;
}

juce::ValueTree Session::groupIntoFolder(const std::vector<juce::ValueTree>& tracks, const juce::String& name)
{
    const auto ordered = outermostInOrder(tracks);
    if (ordered.empty())
        return {};

    auto parent = ordered.front().getParent();
    const int insertAt = parent.indexOf(ordered.front());

    juce::ValueTree folderTree(ids::track);
    const int count = getNumTracks();
    folderTree.setProperty(ids::id, static_cast<int>(allocateId()), nullptr);
    folderTree.setProperty(ids::name, name.isNotEmpty() ? name : defaultTrackName(TrackKind::bus, true, count + 1), nullptr);
    folderTree.setProperty(ids::colour, track::getColour(ordered.front()).toString(), nullptr);
    folderTree.setProperty(ids::kind, toString(TrackKind::bus), nullptr);
    folderTree.setProperty(ids::folder, true, nullptr);
    folderTree.setProperty(ids::volume, 0.0, nullptr);
    folderTree.setProperty(ids::pan, 0.0, nullptr);
    parent.addChild(folderTree, insertAt, &undo_);

    for (auto t : ordered)
    {
        t.getParent().removeChild(t, &undo_);
        folderTree.addChild(t, -1, &undo_);
    }
    return folderTree;
}

juce::String Session::uniqueTrackName(const juce::String& base, const juce::String& separator, bool startAtOne) const
{
    std::set<juce::String> taken;
    for (const auto& t : getAllTracks())
        taken.insert(track::getName(t));
    if (!startAtOne && taken.count(base) == 0)
        return base;
    for (int n = startAtOne ? 1 : 2;; ++n)
        if (const auto candidate = base + separator + juce::String(n); taken.count(candidate) == 0)
            return candidate;
}

juce::ValueTree Session::duplicateTrack(const juce::ValueTree& t, const DuplicateOptions& options, const juce::ValueTree& after,
                                        const juce::String& name)
{
    if (!t.isValid() || !t.hasType(ids::track) || !after.isValid() || !after.getParent().isValid())
        return {};

    auto copy = t.createCopy();

    // Leave behind what was not asked for (in every track of a copied folder).
    std::map<std::uint32_t, std::uint32_t> newId; // old id -> new id, for every node that has one
    const std::function<void(juce::ValueTree)> strip = [&](juce::ValueTree node)
    {
        std::set<std::uint32_t> droppedDevices, droppedSends;
        for (int i = node.getNumChildren(); --i >= 0;)
        {
            const auto child = node.getChild(i);
            const bool drop = (isClip(child) && !options.clips)
                              || (child.hasType(ids::device) && !options.devices) || (child.hasType(ids::send) && !options.sends)
                              || (child.hasType(ids::automation) && !options.automation);
            if (drop)
            {
                if (child.hasType(ids::device))
                    droppedDevices.insert(device::getId(child));
                if (child.hasType(ids::send))
                    droppedSends.insert(send::getId(child));
                node.removeChild(i, nullptr);
            }
        }
        // Automation of a device or send that stayed behind has nothing left to control.
        for (int i = node.getNumChildren(); --i >= 0;)
        {
            const auto lane = node.getChild(i);
            if (!lane.hasType(ids::automation))
                continue;
            std::uint32_t id = 0;
            int index = 0;
            const auto target = automation::getTarget(lane);
            if ((automation::parseParameterTarget(target, id, index) && droppedDevices.count(id) > 0)
                || (automation::parseSendTarget(target, id) && droppedSends.count(id) > 0))
                node.removeChild(i, nullptr);
        }
        for (auto child : node)
            if (child.hasType(ids::track))
                strip(child);
    };
    strip(copy);

    // Fresh ids for every node that has one.
    const std::function<void(juce::ValueTree)> renumber = [&](juce::ValueTree node)
    {
        if (node.hasProperty(ids::id))
        {
            const auto id = allocateId();
            newId[static_cast<std::uint32_t>(static_cast<int>(node.getProperty(ids::id)))] = id;
            node.setProperty(ids::id, static_cast<int>(id), nullptr);
        }
        for (auto child : node)
            renumber(child);
    };
    renumber(copy);

    // References inside the copy follow it: automation targets, outputs, sends, sidechains, aux sources.
    const auto remap = [&newId](std::uint32_t id) { const auto it = newId.find(id); return it != newId.end() ? it->second : id; };
    const std::function<void(juce::ValueTree)> relink = [&](juce::ValueTree node)
    {
        if (node.hasType(ids::automation))
        {
            std::uint32_t id = 0;
            int index = 0;
            const auto target = automation::getTarget(node);
            if (automation::parseParameterTarget(target, id, index))
                node.setProperty(ids::target, automation::parameterTarget(remap(id), index), nullptr);
            else if (automation::parseSendTarget(target, id))
                node.setProperty(ids::target, automation::sendTarget(remap(id)), nullptr);
        }
        else
        {
            for (const auto& property : {ids::output, ids::auxSource, ids::sidechain, ids::target})
                if (node.hasProperty(property))
                    node.setProperty(property, static_cast<int>(remap(static_cast<std::uint32_t>(static_cast<int>(node.getProperty(property))))), nullptr);
        }
        if (node.hasType(ids::track))
            node.setProperty(ids::armed, false, nullptr);
        for (auto child : node)
            relink(child);
    };
    relink(copy);

    copy.setProperty(ids::name, name.isNotEmpty() ? name : uniqueTrackName(track::getName(t) + ".dup", "", true), nullptr);

    auto parent = after.getParent();
    parent.addChild(copy, parent.indexOf(after) + 1, &undo_);

    // Without its VCA assignment, a copy keeps sounding the same: the VCA's gain is baked in.
    if (!options.vcaAssignment)
    {
        const std::function<void(juce::ValueTree)> leaveVcas = [&](juce::ValueTree node)
        {
            // (Nested VCAs: each level in turn; a loop of VCAs just stops.)
            for (int depth = 0; depth < 16; ++depth)
            {
                const auto v = findTrack(track::getVca(node));
                if (!v.isValid() || v == node)
                    break;
                coalesceVca(node, v);
            }
            track::setVca(node, 0, &undo_);
            for (auto child : node)
                if (child.hasType(ids::track))
                    leaveVcas(child);
        };
        leaveVcas(copy);
    }
    return copy;
}

std::vector<juce::ValueTree> Session::getVcaMembers(const juce::ValueTree& vca) const
{
    std::vector<juce::ValueTree> members;
    const auto id = track::getId(vca);
    for (const auto& t : getAllTracks())
        if (t != vca && track::getVca(t) == id)
            members.push_back(t);
    return members;
}

void Session::coalesceVca(juce::ValueTree member, const juce::ValueTree& vca)
{
    if (!member.isValid() || !vca.isValid())
        return;

    // Volume lanes are linear in dB: 0 = silence, (0, 1] = -60 dB .. +6 dB.
    constexpr double range = kMaxVolumeDb - kMinusInfinityDb;
    const auto toDb = [](double normalised) { return normalised <= 0.0 ? -1.0e9 : kMinusInfinityDb + normalised * range; };
    const auto toNormalised = [](double db) { return db <= kMinusInfinityDb ? 0.0 : juce::jlimit(0.0, 1.0, (db - kMinusInfinityDb) / range); };

    auto memberLane = getAutomation(member, automation::volumeTarget(), false, 0.0);
    const auto vcaLane = getAutomation(vca, automation::volumeTarget(), false, 0.0);
    const double memberDb = track::getVolumeDb(member);
    const double vcaDb = track::getVolumeDb(vca);

    if (!memberLane.isValid() && !vcaLane.isValid())
    {
        track::setVolumeDb(member, memberDb <= kMinusInfinityDb || vcaDb <= kMinusInfinityDb ? kMinusInfinityDb
                                                                                              : juce::jmin(kMaxVolumeDb, memberDb + vcaDb),
                           &undo_);
    }
    else
    {
        const auto memberCurve = memberLane.isValid() ? automation::toCurve(memberLane) : engine::AutomationCurve();
        const auto vcaCurve = vcaLane.isValid() ? automation::toCurve(vcaLane) : engine::AutomationCurve();
        const auto dbAt = [&](double t)
        {
            const double m = memberCurve.isEmpty() ? memberDb : toDb(memberCurve.valueAt(t));
            const double v = vcaCurve.isEmpty() ? vcaDb : toDb(vcaCurve.valueAt(t));
            return m + v;
        };

        // Sample at every breakpoint of both, and in between (the curves are splines).
        std::set<double> times;
        for (const auto* curve : {&memberCurve, &vcaCurve})
            for (const auto& point : curve->getPoints())
                times.insert(point.time);
        std::vector<double> sampled(times.begin(), times.end());
        for (std::size_t i = 0; i + 1 < sampled.size(); ++i)
            for (int k = 1; k < 8; ++k)
                times.insert(sampled[i] + (sampled[i + 1] - sampled[i]) * k / 8.0);

        juce::ValueTree lane(ids::automation);
        lane.setProperty(ids::id, static_cast<int>(allocateId()), nullptr);
        lane.setProperty(ids::target, automation::volumeTarget(), nullptr);
        lane.setProperty(ids::visible, true, nullptr);
        for (const double t : times)
        {
            juce::ValueTree p(ids::point);
            p.setProperty(ids::time, t, nullptr);
            p.setProperty(ids::value, toNormalised(dbAt(t)), nullptr);
            lane.addChild(p, -1, nullptr);
        }
        if (memberLane.isValid())
            member.removeChild(memberLane, &undo_);
        member.addChild(lane, -1, &undo_);
    }

    if (track::isMuted(vca))
        track::setMuted(member, true, &undo_);
    track::setVca(member, track::getVca(vca), &undo_);
}

void Session::removeTrack(const juce::ValueTree& t)
{
    if (!t.isValid() || !t.hasType(ids::track))
        return;

    auto parent = t.getParent();
    if (parent.isValid())
        parent.removeChild(t, &undo_);
}

void Session::moveTrack(int fromIndex, int toIndex)
{
    const auto tracks = getAllTracks();
    const int count = static_cast<int>(tracks.size());
    if (fromIndex < 0 || fromIndex >= count || fromIndex == toIndex)
        return;

    auto moving = tracks[static_cast<std::size_t>(fromIndex)];
    juce::ValueTree before = toIndex >= 0 && toIndex < count ? tracks[static_cast<std::size_t>(toIndex)] : juce::ValueTree();

    // A folder can't move into itself: skip past its own contents.
    while (before.isValid() && isWithin(before, moving))
    {
        const int next = static_cast<int>(std::find(tracks.begin(), tracks.end(), before) - tracks.begin()) + 1;
        before = next < count ? tracks[static_cast<std::size_t>(next)] : juce::ValueTree();
    }

    auto oldParent = moving.getParent();
    auto newParent = before.isValid() ? before.getParent() : state_;

    if (oldParent == newParent)
    {
        const int from = oldParent.indexOf(moving);
        int to = before.isValid() ? newParent.indexOf(before) : newParent.getNumChildren();
        if (to > from)
            --to;
        if (to != from)
            oldParent.moveChild(from, to, &undo_);
        return;
    }

    oldParent.removeChild(moving, &undo_);
    newParent.addChild(moving, before.isValid() ? newParent.indexOf(before) : -1, &undo_);
}

void Session::moveTracks(const std::vector<juce::ValueTree>& tracks, int toIndex)
{
    const auto moving = outermostInOrder(tracks);
    const auto all = getAllTracks();
    const int count = static_cast<int>(all.size());

    // Dropping onto (or inside) one of the moving tracks means just after them.
    const auto isMoving = [&moving](const juce::ValueTree& t)
    { return std::any_of(moving.begin(), moving.end(), [&t](const juce::ValueTree& m) { return isWithin(t, m); }); };
    int target = toIndex;
    while (target >= 0 && target < count && isMoving(all[static_cast<std::size_t>(target)]))
        ++target;
    const juce::ValueTree before = target >= 0 && target < count ? all[static_cast<std::size_t>(target)] : juce::ValueTree();

    for (const auto& t : moving)
        moveTrack(indexOf(t), before.isValid() ? indexOf(before) : getNumTracks());
}

bool Session::moveAfter(const juce::ValueTree& t, const juce::ValueTree& after)
{
    if (!t.isValid() || !after.isValid() || t == after || isWithin(after, t) || !after.getParent().isValid())
        return false;
    auto moving = t;
    auto newParent = after.getParent();
    if (auto oldParent = moving.getParent(); oldParent.isValid())
        oldParent.removeChild(moving, &undo_);
    newParent.addChild(moving, newParent.indexOf(after) + 1, &undo_);
    return true;
}

bool Session::moveIntoFolder(const juce::ValueTree& t, const juce::ValueTree& folderTree)
{
    if (!t.isValid() || !t.hasType(ids::track))
        return false;

    auto newParent = folderTree.isValid() ? folderTree : state_;
    if (folderTree.isValid() && (!track::isFolder(folderTree) || isWithin(folderTree, t)))
        return false;

    auto oldParent = t.getParent();
    if (oldParent == newParent)
        return true;

    auto moving = t;
    oldParent.removeChild(moving, &undo_);
    newParent.addChild(moving, -1, &undo_);
    return true;
}

std::vector<juce::ValueTree> Session::getBusses() const
{
    std::vector<juce::ValueTree> busses;
    for (const auto& t : getAllTracks())
        if (track::getKind(t) == TrackKind::bus)
            busses.push_back(t);
    return busses;
}

juce::ValueTree Session::getDestination(const juce::ValueTree& t) const
{
    if (const auto output = track::getOutput(t); output != 0)
    {
        const auto bus = findTrack(output);
        if (bus.isValid() && bus != t && track::getKind(bus) == TrackKind::bus)
            return bus;
    }

    return getParentFolder(t);
}

bool Session::canFeed(const juce::ValueTree& source, const juce::ValueTree& bus) const
{
    if (!source.isValid() || !bus.isValid() || source == bus)
        return false;

    // Does `bus` already (directly or indirectly) feed `source`? Then source -> bus closes a loop.
    const auto tracks = getAllTracks();
    std::set<std::uint32_t> visited;
    std::function<bool(const juce::ValueTree&)> reaches = [&](const juce::ValueTree& from) -> bool
    {
        if (from == source)
            return true;
        if (!visited.insert(track::getId(from)).second)
            return false;

        if (const auto dest = getDestination(from); dest.isValid() && reaches(dest))
            return true;

        for (const auto& s : getSends(from))
            if (const auto target = findTrack(send::getTarget(s)); target.isValid() && reaches(target))
                return true;

        // Tracks that use `from` as a sidechain or aux source depend on it too.
        for (const auto& other : tracks)
        {
            if (track::getAuxSource(other) == track::getId(from) && reaches(other))
                return true;
            for (const auto& d : getDevices(other))
                if (device::getSidechain(d) == track::getId(from) && reaches(other))
                    return true;
        }
        return false;
    };

    return !reaches(bus);
}

// --- Clips ----------------------------------------------------------------------------------------

juce::ValueTree Session::addClip(juce::ValueTree t, const juce::File& file, double start, double length,
                                 double offset)
{
    if (!t.isValid())
        return {};

    juce::ValueTree c(ids::clip);
    c.setProperty(ids::id, static_cast<int>(allocateId()), nullptr);
    c.setProperty(ids::name, file.getFileNameWithoutExtension(), nullptr);
    c.setProperty(ids::file, file.getFullPathName(), nullptr);
    c.setProperty(ids::start, std::max(0.0, start), nullptr);
    c.setProperty(ids::length, std::max(kMinClipLength, length), nullptr);
    c.setProperty(ids::offset, std::max(0.0, offset), nullptr);
    c.setProperty(ids::fadeIn, 0.0, nullptr);
    c.setProperty(ids::fadeOut, 0.0, nullptr);
    c.setProperty(ids::gain, 0.0, nullptr);
    c.setProperty(ids::stretch, 1.0, nullptr);
    c.setProperty(ids::pitch, 0.0, nullptr);

    t.addChild(c, -1, &undo_);
    return c;
}

juce::ValueTree Session::addVideoClip(juce::ValueTree t, const juce::File& file, double start, double length, double offset)
{
    if (!t.isValid())
        return {};

    juce::ValueTree c(ids::videoClip);
    c.setProperty(ids::id, static_cast<int>(allocateId()), nullptr);
    c.setProperty(ids::name, file.getFileNameWithoutExtension(), nullptr);
    c.setProperty(ids::file, file.getFullPathName(), nullptr);
    c.setProperty(ids::start, std::max(0.0, start), nullptr);
    c.setProperty(ids::length, std::max(kMinClipLength, length), nullptr);
    c.setProperty(ids::offset, std::max(0.0, offset), nullptr);

    t.addChild(c, -1, &undo_);
    return c;
}

juce::ValueTree Session::addMidiClip(juce::ValueTree t, double start, double length, const juce::String& name)
{
    if (!t.isValid())
        return {};

    juce::ValueTree c(ids::midiClip);
    c.setProperty(ids::id, static_cast<int>(allocateId()), nullptr);
    c.setProperty(ids::name, name.isNotEmpty() ? name : track::getName(t), nullptr);
    c.setProperty(ids::start, std::max(0.0, start), nullptr);
    c.setProperty(ids::length, std::max(kMinClipLength, length), nullptr);
    c.setProperty(ids::offset, 0.0, nullptr);

    t.addChild(c, -1, &undo_);
    return c;
}

juce::ValueTree Session::findClip(std::uint32_t clipId) const
{
    for (const auto& t : getAllTracks())
        for (const auto& c : t)
            if (isClip(c) && clip::getId(c) == clipId)
                return c;

    return {};
}

void Session::moveClip(juce::ValueTree c, juce::ValueTree targetTrack, double newStart)
{
    if (!c.isValid() || !clip::canChangeTrack(c))
        return;

    if (clip::canMoveInTime(c))
        clip::setStart(c, newStart, &undo_);

    auto currentTrack = c.getParent();
    if (!targetTrack.isValid() || targetTrack == currentTrack || !currentTrack.isValid())
        return;

    // Audio clips live on audio tracks, MIDI clips on instrument and MIDI tracks, video clips on video tracks.
    const auto kind = track::getKind(targetTrack);
    const bool compatible = clip::isMidi(c) ? holdsMidi(kind) : clip::isVideo(c) ? holdsVideo(kind) : holdsAudio(kind);
    if (!compatible)
        return;

    currentTrack.removeChild(c, &undo_);
    targetTrack.addChild(c, -1, &undo_);
}

bool Session::removeClip(juce::ValueTree c)
{
    auto parent = c.getParent();
    if (!parent.isValid() || !clip::canRemove(c))
        return false;
    parent.removeChild(c, &undo_);
    return true;
}

juce::ValueTree Session::duplicateClip(const juce::ValueTree& original)
{
    auto parent = original.getParent();
    if (!parent.isValid())
        return {};

    auto copy = original.createCopy();
    reassignIds(copy);
    copy.setProperty(ids::start, clip::getEnd(original), nullptr);
    parent.addChild(copy, -1, &undo_);
    return copy;
}

juce::ValueTree Session::pasteClip(const juce::ValueTree& original, juce::ValueTree t, double start)
{
    if (!original.isValid() || !t.isValid())
        return {};

    auto copy = original.createCopy();
    reassignIds(copy);
    copy.setProperty(ids::start, std::max(0.0, start), nullptr);
    t.addChild(copy, -1, &undo_);
    return copy;
}

void Session::clearRange(juce::ValueTree t, double start, double end)
{
    if (!t.isValid() || end <= start)
        return;

    std::vector<juce::ValueTree> clips;
    for (const auto& c : t)
        if (isClip(c))
            clips.push_back(c);

    for (auto c : clips)
    {
        const double clipStart = clip::getStart(c);
        const double clipEnd = clip::getEnd(c);
        if (clipEnd <= start || clipStart >= end || clip::isEditLocked(c) || clip::isTimeLocked(c) || clip::isLooped(c))
            continue;

        if (clipStart < start)
        {
            // Keep the part before the range (and, if the clip runs past it, the part after).
            if (clipEnd > end)
                splitClip(c, end);
            clip::setLength(c, start - clipStart, &undo_);
        }
        else if (clipEnd > end)
        {
            // Starts inside: keep the part after the range.
            if (splitClip(c, end).isValid())
                removeClip(c);
        }
        else
        {
            removeClip(c);
        }
    }
}

juce::ValueTree Session::splitClip(juce::ValueTree c, double atSeconds)
{
    auto parent = c.getParent();
    const double start = clip::getStart(c);
    const double end = clip::getEnd(c);

    if (!parent.isValid() || !clip::canEdit(c) || clip::isLooped(c) || atSeconds <= start + kMinClipLength || atSeconds >= end - kMinClipLength)
        return {}; // (a looped clip is unlooped first)

    auto right = c.createCopy(); // (a time lock carries over to the new part)
    reassignIds(right);
    const double leftLength = atSeconds - start;

    right.setProperty(ids::start, atSeconds, nullptr);
    right.setProperty(ids::length, end - atSeconds, nullptr);

    if (clip::isMidi(c))
    {
        // MIDI clips are windows onto their notes: the right part shows the content from the split on.
        const double beatsPerSecond = getTempo() / 60.0;
        right.setProperty(ids::offset, clip::getOffset(c) + leftLength * beatsPerSecond, nullptr);
    }
    else
    {
        right.setProperty(ids::offset, clip::getOffset(c) + leftLength / std::max(kMinStretch, clip::getStretch(c)), nullptr);
        right.setProperty(ids::fadeIn, 0.0, nullptr);
        right.setProperty(ids::fadeOut, std::min(clip::getFadeOut(c), end - atSeconds), nullptr);
    }

    c.setProperty(ids::length, leftLength, &undo_);
    if (!clip::isMidi(c))
    {
        c.setProperty(ids::fadeOut, 0.0, &undo_);
        c.setProperty(ids::fadeIn, std::min(clip::getFadeIn(c), leftLength), &undo_);
    }

    // The sync point stays with the part that holds it.
    if (const auto sync = clip::getSyncPoint(c))
    {
        if (*sync < leftLength)
            right.removeProperty(ids::syncPoint, nullptr);
        else
        {
            right.setProperty(ids::syncPoint, *sync - leftLength, nullptr);
            clip::setSyncPoint(c, std::nullopt, &undo_);
        }
    }

    parent.addChild(right, parent.indexOf(c) + 1, &undo_);
    return right;
}

double Session::getEndTime() const
{
    double end = 0.0;
    for (const auto& t : getAllTracks())
        for (const auto& c : t)
            if (isClip(c))
                end = std::max(end, clip::getEnd(c));
    return end;
}

// --- Clip groups, loops, sync points --------------------------------------------------------------

std::vector<juce::ValueTree> Session::getGroupMembers(std::uint32_t groupId) const
{
    std::vector<juce::ValueTree> members;
    if (groupId == 0)
        return members;
    for (const auto& t : getAllTracks())
        for (const auto& c : t)
            if (isClip(c))
            {
                const auto groups = clip::getGroups(c);
                if (std::find(groups.begin(), groups.end(), groupId) != groups.end())
                    members.push_back(c);
            }
    return members;
}

std::vector<juce::ValueTree> Session::withWholeGroups(const std::vector<juce::ValueTree>& clips) const
{
    std::vector<juce::ValueTree> result;
    const auto add = [&](const juce::ValueTree& c)
    {
        if (std::find(result.begin(), result.end(), c) == result.end())
            result.push_back(c);
    };
    for (const auto& c : clips)
    {
        if (const auto outer = clip::getOuterGroup(c); outer != 0)
            for (const auto& member : getGroupMembers(outer))
                add(member);
        else
            add(c);
    }
    return result;
}

juce::String Session::getGroupName(std::uint32_t groupId) const
{
    const auto list = state_.getChildWithName(ids::clipGroupList);
    for (const auto& g : list)
        if (readId(g) == groupId)
            return g.getProperty(ids::name).toString();
    return "Clip Group";
}

std::uint32_t Session::groupClips(const std::vector<juce::ValueTree>& clips, const juce::String& name)
{
    const auto members = withWholeGroups(clips);
    if (members.empty())
        return 0;

    auto list = state_.getChildWithName(ids::clipGroupList);
    if (!list.isValid())
    {
        list = juce::ValueTree(ids::clipGroupList);
        state_.addChild(list, -1, &undo_);
    }
    const auto id = allocateId();
    juce::String groupName = name.trim();
    if (groupName.isEmpty())
    {
        int number = list.getNumChildren() + 1;
        const auto taken = [&](const juce::String& candidate)
        {
            for (const auto& g : list)
                if (g.getProperty(ids::name).toString() == candidate)
                    return true;
            return false;
        };
        while (taken("Clip Group " + juce::String(number)))
            ++number;
        groupName = "Clip Group " + juce::String(number);
    }
    juce::ValueTree entry(ids::clipGroup);
    entry.setProperty(ids::id, static_cast<int>(id), nullptr);
    entry.setProperty(ids::name, groupName, nullptr);
    list.addChild(entry, -1, &undo_);

    for (auto c : members)
    {
        auto groups = clip::getGroups(c);
        groups.insert(groups.begin(), id);
        clip::setGroups(c, groups, &undo_);
    }
    return id;
}

namespace
{
void removeGroupEntry(juce::ValueTree& state, std::uint32_t groupId, juce::UndoManager* um)
{
    auto list = state.getChildWithName(ids::clipGroupList);
    for (int i = list.getNumChildren(); --i >= 0;)
        if (readId(list.getChild(i)) == groupId)
            list.removeChild(i, um);
}
} // namespace

std::vector<juce::ValueTree> Session::ungroup(std::uint32_t groupId)
{
    std::vector<juce::ValueTree> freed;
    for (auto c : getGroupMembers(groupId))
    {
        auto groups = clip::getGroups(c);
        if (groups.empty() || groups.front() != groupId)
            continue; // (only the top layer)
        groups.erase(groups.begin());
        clip::setGroups(c, groups, &undo_);
        freed.push_back(c);
    }
    if (!freed.empty())
        removeGroupEntry(state_, groupId, &undo_);
    return freed;
}

std::vector<juce::ValueTree> Session::ungroupAll(std::uint32_t groupId)
{
    const auto members = getGroupMembers(groupId);
    std::set<std::uint32_t> removed;
    for (auto c : members)
    {
        for (const auto id : clip::getGroups(c))
            removed.insert(id);
        clip::setGroups(c, {}, &undo_);
    }
    for (const auto id : removed)
        if (getGroupMembers(id).empty())
            removeGroupEntry(state_, id, &undo_);
    return members;
}

void Session::regroup(std::uint32_t groupId, const std::vector<juce::ValueTree>& clips, const juce::String& name)
{
    if (groupId == 0 || clips.empty())
        return;
    auto list = state_.getChildWithName(ids::clipGroupList);
    if (!list.isValid())
    {
        list = juce::ValueTree(ids::clipGroupList);
        state_.addChild(list, -1, &undo_);
    }
    bool listed = false;
    for (const auto& g : list)
        listed = listed || readId(g) == groupId;
    if (!listed)
    {
        juce::ValueTree entry(ids::clipGroup);
        entry.setProperty(ids::id, static_cast<int>(groupId), nullptr);
        entry.setProperty(ids::name, name.isNotEmpty() ? name : juce::String("Clip Group"), nullptr);
        list.addChild(entry, -1, &undo_);
    }
    for (auto c : clips)
    {
        if (!c.getParent().isValid())
            continue; // (deleted since)
        auto groups = clip::getGroups(c);
        if (!groups.empty() && groups.front() == groupId)
            continue;
        groups.insert(groups.begin(), groupId);
        clip::setGroups(c, groups, &undo_);
    }
}

void Session::renewGroups(const std::vector<juce::ValueTree>& copies)
{
    std::map<std::uint32_t, std::uint32_t> renewed;
    auto list = state_.getChildWithName(ids::clipGroupList);
    for (auto c : copies)
    {
        auto groups = clip::getGroups(c);
        if (groups.empty())
            continue;
        for (auto& id : groups)
        {
            auto it = renewed.find(id);
            if (it == renewed.end())
            {
                const auto fresh = allocateId();
                if (!list.isValid())
                {
                    list = juce::ValueTree(ids::clipGroupList);
                    state_.addChild(list, -1, &undo_);
                }
                juce::ValueTree entry(ids::clipGroup);
                entry.setProperty(ids::id, static_cast<int>(fresh), nullptr);
                entry.setProperty(ids::name, getGroupName(id), nullptr);
                list.addChild(entry, -1, &undo_);
                it = renewed.emplace(id, fresh).first;
            }
            id = it->second;
        }
        clip::setGroups(c, groups, &undo_);
    }
}

void Session::loopClip(juce::ValueTree c, double iterations)
{
    if (!c.isValid() || !clip::canEdit(c))
        return;
    const double iteration = clip::getLoopLength(c);
    clip::setLoop(c, true, iteration, &undo_);
    c.setProperty(ids::length, std::max(kMinClipLength, iteration * std::max(1.0, iterations)), &undo_);
}

std::vector<juce::ValueTree> Session::unloopClip(juce::ValueTree c)
{
    if (!c.isValid() || !clip::isLooped(c) || !clip::canEdit(c))
        return {c};

    auto parent = c.getParent();
    const double iteration = clip::getLoopLength(c);
    const double total = clip::getLength(c);
    const double start = clip::getStart(c);
    const double fadeOut = clip::isMidi(c) ? 0.0 : clip::getFadeOut(c);
    const int count = std::max(1, static_cast<int>(std::ceil(total / iteration - 1.0e-9)));

    clip::setLoop(c, false, 0.0, &undo_);
    c.setProperty(ids::length, std::min(iteration, total), &undo_);
    std::vector<juce::ValueTree> pieces{c};
    for (int k = 1; k < count; ++k)
    {
        auto copy = c.createCopy();
        reassignIds(copy);
        copy.setProperty(ids::start, start + k * iteration, nullptr);
        copy.setProperty(ids::length, std::min(iteration, total - k * iteration), nullptr);
        copy.removeProperty(ids::syncPoint, nullptr); // (the sync point stays on the first)
        if (!clip::isMidi(c))
        {
            copy.setProperty(ids::fadeIn, 0.0, nullptr);
            copy.setProperty(ids::fadeOut, k == count - 1 ? fadeOut : 0.0, nullptr);
        }
        parent.addChild(copy, parent.indexOf(pieces.back()) + 1, &undo_);
        pieces.push_back(copy);
    }
    if (count > 1 && !clip::isMidi(c))
        c.setProperty(ids::fadeOut, 0.0, &undo_);
    return pieces;
}

bool Session::identifySyncPoint(juce::ValueTree c, double seconds)
{
    if (!c.isValid() || seconds < clip::getStart(c) || seconds > clip::getEnd(c))
        return false;
    clip::setSyncPoint(c, seconds - clip::getStart(c), &undo_);
    return true;
}

// --- Clip List ------------------------------------------------------------------------------------

std::vector<juce::ValueTree> Session::getClipListEntries() const
{
    std::vector<juce::ValueTree> entries;
    for (const auto& c : state_.getChildWithName(ids::clipList))
        if (isClip(c))
            entries.push_back(c);
    return entries;
}

juce::ValueTree Session::findClipListEntry(std::uint32_t entryId) const
{
    for (const auto& c : getClipListEntries())
        if (clip::getId(c) == entryId)
            return c;
    return {};
}

namespace
{
/** A clip's own properties as a fresh clip has them: no groups, locks, loop, sync point or rating. */
void stripClipState(juce::ValueTree c)
{
    for (const auto* id : {&ids::clipGroups, &ids::editLocked, &ids::timeLocked, &ids::looped, &ids::loopLength,
                           &ids::syncPoint, &ids::rating})
        c.removeProperty(*id, nullptr);
}
} // namespace

juce::ValueTree Session::captureClip(const juce::ValueTree& track, double start, double end, const juce::String& name,
                                     juce::String& error)
{
    if (!track.isValid() || end - start < kMinClipLength)
    {
        error = "Select a range on a track first.";
        return {};
    }

    std::vector<juce::ValueTree> parts;
    for (const auto& c : track)
        if (isClip(c) && clip::getEnd(c) > start + 1.0e-9 && clip::getStart(c) < end - 1.0e-9)
            parts.push_back(c);
    std::sort(parts.begin(), parts.end(), [](const auto& a, const auto& b) { return clip::getStart(a) < clip::getStart(b); });
    if (parts.empty())
    {
        error = "There is no clip in the selection.";
        return {};
    }

    const auto& first = parts.front();
    const double from = std::max(start, clip::getStart(first));
    const double to = std::min(end, clip::getEnd(parts.back()));

    // Across clips only when they play one stretch of the same file without gaps.
    constexpr double tolerance = 1.0e-6;
    for (std::size_t k = 1; k < parts.size(); ++k)
    {
        const auto& a = parts[k - 1];
        const auto& b = parts[k];
        const bool media = !clip::isMidi(a) && a.getType() == b.getType();
        const bool contiguous = media && clip::getFile(a) == clip::getFile(b) && !clip::isLooped(a) && !clip::isLooped(b)
                                && std::abs(clip::getStretch(a) - clip::getStretch(b)) < tolerance
                                && std::abs(clip::getPitch(a) - clip::getPitch(b)) < tolerance
                                && clip::getSourceChannel(a) == clip::getSourceChannel(b)
                                && std::abs(clip::getEnd(a) - clip::getStart(b)) < tolerance
                                && std::abs(clip::getOffset(a) + clip::getLength(a) / std::max(kMinStretch, clip::getStretch(a))
                                            - clip::getOffset(b))
                                       < tolerance;
        if (!contiguous)
        {
            error = "The selection spans clips that are not contiguous material from the same file.";
            return {};
        }
    }

    // A looped clip: only within one iteration.
    double into = from - clip::getStart(first); // timeline seconds into the first clip's material
    if (clip::isLooped(first))
    {
        const double loop = clip::getLoopLength(first);
        const double iteration = std::floor(into / loop + 1.0e-9);
        if (to - clip::getStart(first) > (iteration + 1.0) * loop + tolerance)
        {
            error = "The selection runs over a loop boundary of a looped clip. Unloop the clip first.";
            return {};
        }
        into -= iteration * loop;
    }

    auto entry = first.createCopy();
    reassignIds(entry);
    stripClipState(entry);
    entry.removeProperty(ids::fadeIn, nullptr);
    entry.removeProperty(ids::fadeOut, nullptr);
    entry.setProperty(ids::start, 0.0, nullptr);
    entry.setProperty(ids::length, to - from, nullptr);
    if (clip::isMidi(first))
        entry.setProperty(ids::offset, clip::getOffset(first) + into * getTempo() / 60.0, nullptr);
    else
        entry.setProperty(ids::offset, clip::getOffset(first) + into / std::max(kMinStretch, clip::getStretch(first)), nullptr);
    entry.setProperty(ids::name, name.trim().isNotEmpty() ? name.trim() : clip::getName(first), nullptr);
    entry.setProperty(ids::userNamed, true, nullptr);

    auto list = state_.getChildWithName(ids::clipList);
    if (!list.isValid())
    {
        list = juce::ValueTree(ids::clipList);
        state_.addChild(list, -1, &undo_);
    }
    list.addChild(entry, -1, &undo_);
    return entry;
}

void Session::removeClipListEntry(std::uint32_t entryId)
{
    if (auto entry = findClipListEntry(entryId); entry.isValid())
        entry.getParent().removeChild(entry, &undo_);
}

juce::ValueTree Session::placeClipCopy(const juce::ValueTree& source, juce::ValueTree track, double seconds)
{
    if (!source.isValid() || !isClip(source) || !track.isValid())
        return {};
    const auto kind = track::getKind(track);
    const bool compatible = clip::isMidi(source) ? holdsMidi(kind) : clip::isVideo(source) ? holdsVideo(kind) : holdsAudio(kind);
    if (!compatible)
        return {};

    auto copy = source.createCopy();
    reassignIds(copy);
    stripClipState(copy);
    copy.setProperty(ids::start, std::max(0.0, seconds), nullptr);
    if (clip::isLooped(source)) // keeps playing what it played
    {
        copy.setProperty(ids::looped, true, nullptr);
        copy.setProperty(ids::loopLength, clip::getLoopLength(source), nullptr);
    }
    track.addChild(copy, -1, &undo_);
    return copy;
}

// --- MIDI -----------------------------------------------------------------------------------------

juce::ValueTree Session::addNote(juce::ValueTree midiClipTree, double startBeat, double lengthBeats, int noteNumber,
                                 float velocity, int channel)
{
    if (!midiClipTree.isValid())
        return {};

    juce::ValueTree n(ids::note);
    n.setProperty(ids::start, startBeat, nullptr);
    n.setProperty(ids::length, std::max(kMinNoteLength, lengthBeats), nullptr);
    n.setProperty(ids::noteNumber, juce::jlimit(0, 127, noteNumber), nullptr);
    n.setProperty(ids::velocity, juce::jlimit(0.01, 1.0, static_cast<double>(velocity)), nullptr);
    n.setProperty(ids::channel, juce::jlimit(1, 16, channel), nullptr);
    midiClipTree.addChild(n, -1, &undo_);
    return n;
}

void Session::removeNote(juce::ValueTree n)
{
    auto parent = n.getParent();
    if (parent.isValid())
        parent.removeChild(n, &undo_);
}

juce::ValueTree Session::addControl(juce::ValueTree midiClipTree, int controller, double beat, float value, int channel)
{
    if (!midiClipTree.isValid())
        return {};

    juce::ValueTree c(ids::control);
    c.setProperty(ids::controller, juce::jlimit(0, 128, controller), nullptr);
    c.setProperty(ids::beat, beat, nullptr);
    c.setProperty(ids::value, juce::jlimit(0.0, 1.0, static_cast<double>(value)), nullptr);
    c.setProperty(ids::channel, juce::jlimit(1, 16, channel), nullptr);
    midiClipTree.addChild(c, -1, &undo_);
    return c;
}

void Session::removeControl(juce::ValueTree c)
{
    auto parent = c.getParent();
    if (parent.isValid())
        parent.removeChild(c, &undo_);
}

void Session::quantise(juce::ValueTree midiClipTree, const std::vector<juce::ValueTree>& notes, double gridBeats,
                       double strength, bool ends)
{
    if (!midiClipTree.isValid() || gridBeats <= 0.0)
        return;

    strength = juce::jlimit(0.0, 1.0, strength);
    std::vector<juce::ValueTree> targets = notes;
    if (targets.empty())
        for (const auto& n : midiClipTree)
            if (n.hasType(ids::note))
                targets.push_back(n);

    for (auto& n : targets)
    {
        const double startBeat = note::getStart(n);
        const double endBeat = note::getEnd(n);
        const double snappedStart = std::round(startBeat / gridBeats) * gridBeats;
        const double newStart = startBeat + (snappedStart - startBeat) * strength;
        double newEnd = endBeat + (newStart - startBeat); // keep the length unless ends are quantised

        if (ends)
        {
            const double snappedEnd = std::max(snappedStart + gridBeats, std::round(endBeat / gridBeats) * gridBeats);
            newEnd = endBeat + (snappedEnd - endBeat) * strength;
        }

        n.setProperty(ids::start, newStart, &undo_);
        n.setProperty(ids::length, std::max(kMinNoteLength, newEnd - newStart), &undo_);
    }
}

juce::ValueTree Session::findNoteAt(const juce::ValueTree& midiClipTree, double beat, int noteNumber)
{
    for (const auto& n : midiClipTree)
        if (n.hasType(ids::note) && note::getNote(n) == noteNumber && beat >= note::getStart(n) && beat < note::getEnd(n))
            return n;
    return {};
}

// --- Devices --------------------------------------------------------------------------------------

juce::ValueTree Session::addDevice(juce::ValueTree owner, const juce::String& type, const juce::String& name, int index)
{
    if (!owner.isValid())
        return {};

    juce::ValueTree d(ids::device);
    d.setProperty(ids::id, static_cast<int>(allocateId()), nullptr);
    d.setProperty(ids::type, type, nullptr);
    d.setProperty(ids::name, name, nullptr);
    d.setProperty(ids::bypassed, false, nullptr);

    // Chain position `index` among the owner's devices -> child index.
    const auto devices = getDevices(owner);
    int childIndex = -1;
    if (index >= 0 && index < static_cast<int>(devices.size()))
        childIndex = owner.indexOf(devices[static_cast<std::size_t>(index)]);

    owner.addChild(d, childIndex, &undo_);
    return d;
}

void Session::removeDevice(juce::ValueTree d)
{
    auto parent = d.getParent();
    if (!parent.isValid())
        return;

    // Automation of the device's parameters goes with it.
    const auto prefix = "param:" + juce::String(static_cast<int>(device::getId(d))) + ":";
    for (auto lane : getAutomationLanes(parent))
        if (automation::getTarget(lane).startsWith(prefix))
            parent.removeChild(lane, &undo_);

    parent.removeChild(d, &undo_);
}

void Session::moveDevice(juce::ValueTree d, int newIndex)
{
    auto parent = d.getParent();
    if (!parent.isValid())
        return;

    const auto devices = getDevices(parent);
    newIndex = juce::jlimit(0, static_cast<int>(devices.size()) - 1, newIndex);
    const int from = parent.indexOf(d);
    const int to = parent.indexOf(devices[static_cast<std::size_t>(newIndex)]);
    if (from != to)
        parent.moveChild(from, to, &undo_);
}

juce::ValueTree Session::findDevice(std::uint32_t deviceId) const
{
    if (deviceId == 0)
        return {};

    for (const auto& d : getDevices(getMaster()))
        if (device::getId(d) == deviceId)
            return d;

    for (const auto& t : getAllTracks())
        for (const auto& d : t)
            if (d.hasType(ids::device) && device::getId(d) == deviceId)
                return d;

    return {};
}

std::vector<juce::ValueTree> Session::getDevices(const juce::ValueTree& owner)
{
    std::vector<juce::ValueTree> devices;
    for (const auto& child : owner)
        if (child.hasType(ids::device))
            devices.push_back(child);
    return devices;
}

// --- Sends ----------------------------------------------------------------------------------------

juce::ValueTree Session::addSend(juce::ValueTree t, const juce::ValueTree& bus, double gainDb, bool preFader)
{
    if (!t.isValid() || !bus.isValid() || !canFeed(t, bus))
        return {};

    juce::ValueTree s(ids::send);
    s.setProperty(ids::id, static_cast<int>(allocateId()), nullptr);
    s.setProperty(ids::target, static_cast<int>(track::getId(bus)), nullptr);
    s.setProperty(ids::gain, juce::jlimit(kMinusInfinityDb, kMaxVolumeDb, gainDb), nullptr);
    s.setProperty(ids::preFader, preFader, nullptr);
    t.addChild(s, -1, &undo_);
    return s;
}

void Session::removeSend(juce::ValueTree s)
{
    auto parent = s.getParent();
    if (!parent.isValid())
        return;

    if (auto lane = getAutomation(parent, automation::sendTarget(send::getId(s)), false); lane.isValid())
        parent.removeChild(lane, &undo_);
    parent.removeChild(s, &undo_);
}

std::vector<juce::ValueTree> Session::getSends(const juce::ValueTree& t)
{
    std::vector<juce::ValueTree> sends;
    for (const auto& child : t)
        if (child.hasType(ids::send))
            sends.push_back(child);
    return sends;
}

// --- Automation -----------------------------------------------------------------------------------

juce::ValueTree Session::getAutomation(juce::ValueTree owner, const juce::String& target, bool create, double initialValue)
{
    for (const auto& lane : owner)
        if (lane.hasType(ids::automation) && automation::getTarget(lane) == target)
            return lane;

    if (!create || !owner.isValid())
        return {};

    juce::ValueTree lane(ids::automation);
    lane.setProperty(ids::id, static_cast<int>(allocateId()), nullptr);
    lane.setProperty(ids::target, target, nullptr);
    lane.setProperty(ids::visible, true, nullptr);

    juce::ValueTree p(ids::point);
    p.setProperty(ids::time, 0.0, nullptr);
    p.setProperty(ids::value, juce::jlimit(0.0, 1.0, initialValue), nullptr);
    lane.addChild(p, -1, nullptr);

    owner.addChild(lane, -1, &undo_);
    return lane;
}

std::vector<juce::ValueTree> Session::getAutomationLanes(const juce::ValueTree& owner)
{
    std::vector<juce::ValueTree> lanes;
    for (const auto& child : owner)
        if (child.hasType(ids::automation))
            lanes.push_back(child);
    return lanes;
}

juce::ValueTree Session::addAutomationPoint(juce::ValueTree lane, double time, double value)
{
    if (!lane.isValid())
        return {};

    juce::ValueTree p(ids::point);
    p.setProperty(ids::time, std::max(0.0, time), nullptr);
    p.setProperty(ids::value, juce::jlimit(0.0, 1.0, value), nullptr);

    // Keep points sorted by time.
    int index = 0;
    while (index < lane.getNumChildren() && static_cast<double>(lane.getChild(index).getProperty(ids::time)) <= time)
        ++index;

    lane.addChild(p, index, &undo_);
    return p;
}

void Session::moveAutomationPoint(juce::ValueTree p, double time, double value)
{
    auto lane = p.getParent();
    if (!lane.isValid())
        return;

    // A point can't pass its neighbours (keeps the lane sorted and the drag predictable).
    const int index = lane.indexOf(p);
    double lower = 0.0;
    double upper = std::numeric_limits<double>::max();
    if (index > 0)
        lower = lane.getChild(index - 1).getProperty(ids::time);
    if (index + 1 < lane.getNumChildren())
        upper = lane.getChild(index + 1).getProperty(ids::time);

    p.setProperty(ids::time, juce::jlimit(lower, upper, std::max(0.0, time)), &undo_);
    p.setProperty(ids::value, juce::jlimit(0.0, 1.0, value), &undo_);
}

void Session::removeAutomationPoint(juce::ValueTree p)
{
    auto lane = p.getParent();
    if (lane.isValid() && lane.getNumChildren() > 1)
        lane.removeChild(p, &undo_);
}

void Session::removeAutomation(juce::ValueTree lane)
{
    auto parent = lane.getParent();
    if (parent.isValid())
        parent.removeChild(lane, &undo_);
}

// --- Document -------------------------------------------------------------------------------------

void Session::reset()
{
    state_.copyPropertiesAndChildrenFrom(createEmptyState(), nullptr);
    undo_.clearUndoHistory();
}

std::vector<juce::File> Session::getReferencedFiles() const
{
    std::vector<juce::File> files;
    for (const auto& t : getAllTracks())
        for (const auto& c : t)
            if (c.hasType(ids::clip))
                if (const auto f = clip::getFile(c); std::find(files.begin(), files.end(), f) == files.end())
                    files.push_back(f);
    return files;
}

namespace
{
/** Every clip that refers to a media file (audio and video). */
template <typename Fn>
void forEachMediaClip(juce::ValueTree parent, Fn&& fn)
{
    for (auto child : parent)
    {
        if (child.hasType(ids::clip) || child.hasType(ids::videoClip))
            fn(child);
        else if (child.hasType(ids::track) || child.hasType(ids::clipList))
            forEachMediaClip(child, fn);
    }
}
} // namespace

juce::Result Session::save(const juce::File& file) const
{
    auto copy = state_.createCopy();
    copy.setProperty(ids::version, kFileFormatVersion, nullptr);

    const auto folder = file.getParentDirectory();
    forEachMediaClip(copy, [&](juce::ValueTree c)
                     { c.setProperty(ids::fileRelative, clip::getFile(c).getRelativePathFrom(folder), nullptr); });

    const auto xml = copy.createXml();
    if (xml == nullptr)
        return juce::Result::fail("Could not serialise the session.");

    juce::TemporaryFile temp(file);
    if (!xml->writeTo(temp.getFile()) || !temp.overwriteTargetFileWithTemporary())
        return juce::Result::fail("Could not write " + file.getFullPathName());

    return juce::Result::ok();
}

juce::Result Session::load(const juce::File& file)
{
    const auto xml = juce::XmlDocument::parse(file);
    if (xml == nullptr)
        return juce::Result::fail(file.getFileName() + " is not a readable session file.");

    auto loaded = juce::ValueTree::fromXml(*xml);
    if (!loaded.hasType(ids::session))
        return juce::Result::fail(file.getFileName() + " is not a Dojo session.");

    if (static_cast<int>(loaded.getProperty(ids::version, 0)) > kFileFormatVersion)
        return juce::Result::fail(file.getFileName() + " was saved by a newer version of The Dojo.");

    // Prefer the path relative to the session file, so projects can be moved or copied as a folder.
    const auto folder = file.getParentDirectory();
    forEachMediaClip(loaded,
                     [&](juce::ValueTree c)
                     {
                         const auto relative = c.getProperty(ids::fileRelative).toString();
                         if (relative.isNotEmpty())
                         {
                             const auto candidate = folder.getChildFile(relative);
                             if (candidate.existsAsFile())
                                 c.setProperty(ids::file, candidate.getFullPathName(), nullptr);
                         }
                         c.removeProperty(ids::fileRelative, nullptr);
                     });

    // Older files: fill in what version 1 did not have.
    const auto defaults = createEmptyState();
    for (int i = 0; i < defaults.getNumProperties(); ++i)
    {
        const auto name = defaults.getPropertyName(i);
        if (!loaded.hasProperty(name))
            loaded.setProperty(name, defaults.getProperty(name), nullptr);
    }
    if (!loaded.getChildWithName(ids::master).isValid())
        loaded.addChild(juce::ValueTree(ids::master), 0, nullptr);
    loaded.setProperty(ids::version, kFileFormatVersion, nullptr);

    state_.copyPropertiesAndChildrenFrom(loaded, nullptr);
    undo_.clearUndoHistory();
    return juce::Result::ok();
}

} // namespace dojo::session
