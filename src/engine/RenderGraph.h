#pragma once

#include "core/Types.h"
#include "engine/Automation.h"
#include "engine/ChannelLayout.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace juce
{
class AudioProcessorParameter;
}

namespace dojo::engine
{

class ProcessorNode;
class ClipStretcher;
class StreamedAudio;

/** Upper bound on simultaneously rendered tracks. Per-track engine state (buffers, gain smoothing,
    latency-compensation delay lines, meters) lives in fixed arrays of this size so the audio thread
    never allocates. */
inline constexpr int kMaxTracks = 256;

/** Longest plug-in delay compensation per track, in samples (~170 ms at 48 kHz). */
inline constexpr int kMaxCompensationSamples = 8192;

/** Slots that carry up to kMaxChannels (the last ones): tracks that are multichannel themselves or
    feed a multichannel bus or master. The others are stereo. */
inline constexpr int kMaxWideSlots = 32;
inline constexpr int kFirstWideSlot = kMaxTracks - kMaxWideSlots;

enum class TrackKind : std::uint8_t
{
    audio,      ///< plays audio clips (and/or a monitored input) through its device chain
    instrument, ///< MIDI clips (and live MIDI) drive the first device, an instrument
    bus,        ///< sums other tracks (folder children, routed tracks, sends) or an instrument's aux outs
    midi,       ///< MIDI clips (and live MIDI), sent to an instrument track's instrument; no audio
    vca,        ///< VCA Master: its fader, mute and solo control its member tracks; no audio of its own
    masterFader, ///< a last gain stage (fader, mute, post-fader devices) on the main output or a bus
};

enum class MonitorMode : std::uint8_t
{
    off,
    automatic, ///< hear the input while the track is armed
    always,
};

/**
    One audio clip as the audio thread sees it: a window onto decoded (or disk-streamed) sample data,
    placed on the timeline. Times are in seconds, so a graph stays valid across device sample-rate
    changes; the engine converts to samples per block and resamples when the source rate differs.
*/
struct RenderClip
{
    double startSeconds = 0.0;        ///< timeline position of the clip's first sample
    double endSeconds = 0.0;          ///< timeline position just past its last sample
    double sourceOffsetSeconds = 0.0; ///< source time that plays at startSeconds (slip editing)
    double fadeInSeconds = 0.0;
    double fadeOutSeconds = 0.0;
    bool fadeInEqualPower = false;  ///< crossfades use equal-power curves, user fades are linear
    bool fadeOutEqualPower = false;
    double gain = 1.0;

    double stretch = 1.0;        ///< timeline seconds per source second (2 = half speed)
    double pitchSemitones = 0.0; ///< independent of stretch
    ClipStretcher* stretcher = nullptr; ///< set when stretch != 1 or pitch != 0

    const float* const* channels = nullptr; ///< in-memory audio (kept alive by RenderGraph::keepAlive)
    const StreamedAudio* streamed = nullptr; ///< or disk-streamed audio
    int numChannels = 0;                    ///< source channels (1 = mono, 2 = stereo, up to 8)
    int sourceChannel = -1;                 ///< play only this source channel (on every output); -1 = all

    /** Which source channel each of the track's channels plays (-1 = none). With numOutputs = 0, the
        stereo default: a mono source (or sourceChannel) on both sides, otherwise channels 0 and 1. */
    int numOutputs = 0;
    std::array<std::int8_t, kMaxChannels> channelMap{};
    SampleCount sourceLength = 0;           ///< frames in the source
    double sourceSampleRate = 48000.0;
};

/** A note relative to its clip's content, in beats (quarter notes). */
struct RenderNote
{
    double startBeat = 0.0;
    double endBeat = 0.0;
    int note = 60;
    float velocity = 0.8f;
    int channel = 1;
};

/** A continuous controller value at a beat. controller 0..127 = CC; 128 = pitch bend (value 0..1). */
struct RenderControl
{
    double beat = 0.0;
    int controller = 1;
    float value = 0.0f; ///< normalised 0..1
    int channel = 1;
};

struct RenderMidiClip
{
    double startSeconds = 0.0;
    double endSeconds = 0.0;
    double contentStartBeat = 0.0; ///< content beat that plays at startSeconds
    double rate = 1.0;             ///< content beats advance `rate` x the tempo (per-track speed)
    std::vector<RenderNote> notes; ///< sorted by startBeat
    std::vector<RenderControl> controls; ///< sorted by beat
};

/** Automation of one processor parameter. */
struct ParameterLane
{
    juce::AudioProcessorParameter* parameter = nullptr;
    AutomationCurve curve;
};

/** One device in a chain: its node plus where its sidechain comes from. */
struct RenderDevice
{
    ProcessorNode* node = nullptr;
    int sidechainSource = -1; ///< graph index of the track whose output feeds the sidechain (-1 = none)
    /** On a multichannel signal: linked instances for channel groups 1.. (group 0 is `node`); each
        block they take over `node`'s parameter values and bypass. */
    std::array<ProcessorNode*, kMaxChannelGroups - 1> companions{};
};

/** An incoming send, stored on the *receiving* bus (busses pull, so parallel tracks never write to
    shared buffers). */
struct RenderSend
{
    int sourceIndex = -1;
    double gain = 1.0;
    bool preFader = false;
    AutomationCurve gainAutomation; ///< normalised like volume automation; overrides `gain` when set
    /** From the source's (pre- or post-fader) channels to the bus's, when they differ. */
    bool convert = false;
    ChannelMatrix matrix;
};

struct RenderTrack
{
    std::uint32_t trackId = 0; ///< stable identity, so smoothing/delay state survives graph swaps
    int slot = 0;              ///< index into the engine's per-track state, buffers and meters
    TrackKind kind = TrackKind::audio;
    bool mono = false; ///< a mono track: its sources are folded to mono before its devices

    /** The channels the track's sources and devices work in: stereo for mono, stereo and instrument
        tracks, one per speaker for multichannel tracks. */
    ChannelLayout layout = ChannelLayout::stereo();
    /** The channels of its destination (the bus it feeds, or the master): the fader converts to these
        (surround panner, or by speaker). */
    ChannelLayout outputLayout = ChannelLayout::stereo();
    ChannelGroups groups;       ///< device channel groups of `layout`
    ChannelGroups outputGroups; ///< ... of `outputLayout` (a Master Fader's post-fader devices)
    double panFrontRear = -1.0; ///< surround panner (see surroundPanMatrix)
    double panCentre = 0.0;
    /** Needs a wide slot (layout or outputLayout has more than two channels). */
    [[nodiscard]] bool isWide() const noexcept { return layout.isMultichannel() || outputLayout.isMultichannel(); }

    // Mix
    double volume = 1.0; ///< linear; ignored while volumeAutomation has points
    double pan = 0.0;    ///< -1..1; ignored while panAutomation has points
    bool audible = true; ///< mute/solo resolved
    bool phaseInverted = false;
    AutomationCurve volumeAutomation; ///< normalised 0..1 = -60 dB .. +6 dB (see automationToGain)
    AutomationCurve panAutomation;    ///< normalised 0..1 = left .. right
    std::vector<ParameterLane> parameterLanes;

    // Sources
    std::vector<RenderClip> clips;         ///< audio tracks; sorted by start
    std::vector<RenderMidiClip> midiClips; ///< instrument tracks; sorted by start
    std::vector<int> busSources;           ///< bus tracks: graph indices whose main output feeds this bus
    std::vector<RenderSend> sends;         ///< bus tracks: incoming sends
    int auxSourceIndex = -1;               ///< bus fed from an instrument's aux outputs (multi-out)
    int auxChannel = 0;                    ///< first aux channel (a stereo pair)
    ChannelMatrix auxMatrix;               ///< the stereo pair into a multichannel bus

    // Inputs
    int inputChannel = -1; ///< first device input channel, -1 = none
    bool inputStereo = true;
    int inputCount = 0; ///< multichannel tracks: consecutive input channels (one per layout channel)
    MonitorMode monitor = MonitorMode::automatic;
    bool armed = false;
    bool receivesLiveMidi = false;

    // Processing
    std::vector<RenderDevice> devices; ///< instrument first on instrument tracks
    bool needsPreFaderCopy = false;    ///< some bus pulls a pre-fader send from this track

    // Routing
    int outputIndex = -1;           ///< graph index of the bus this track feeds; -1 = master; -2 = none
    int compensationDelay = 0;      ///< plug-in delay compensation applied to the post-fader output

    /** Instrument tracks: the MIDI tracks (graph indices) whose MIDI their instrument also plays. */
    std::vector<int> midiSources;

    /** VCA Masters this track belongs to (directly or through nested VCAs): each multiplies its
        volume by its gain, or by its automation when that has points. */
    struct VcaGain
    {
        double gain = 1.0;
        AutomationCurve automation; ///< normalised like volume automation
    };
    std::vector<VcaGain> vcaGains;

    /** A Master Fader on this bus: one more fader (with mute) and post-fader devices on its output. */
    bool hasPostFader = false;
    double postVolume = 1.0;
    bool postAudible = true;
    AutomationCurve postVolumeAutomation;
    std::vector<RenderDevice> postDevices;
};

/**
    An immutable snapshot of everything the audio thread renders.

    Built on the message thread whenever the session changes, handed to the audio thread by pointer
    (command::SwapGraph) and never modified afterwards. The graph it replaces is returned through a
    lock-free queue and destroyed on the message thread, so the audio thread never frees memory.

    `tracks` is in dependency order and grouped into `levels`: every track in a level depends only on
    tracks in earlier levels, so the tracks of one level are rendered in parallel.
*/
struct RenderGraph
{
    std::uint64_t serial = 0; ///< assigned by EngineController; echoed back via telemetry
    double tempo = 120.0;
    double speed = 1.0; ///< global varispeed (timeline seconds per real second)
    bool loopEnabled = false;
    double loopStart = 0.0;
    double loopEnd = 0.0;
    bool hasTransportSettings = false; ///< apply speed/loop from this graph when it is swapped in
    double arrangementEnd = 0.0;       ///< end of the last clip (for "stop at the end"); 0 = no clips
    // Recording: what armed audio tracks with automatic monitoring hear.
    bool inputOnlyMonitoring = false; ///< always their input; otherwise Auto Input (tape-style)
    double punchStart = 0.0;          ///< while recording, the input is heard from here...
    double punchEnd = 1.0e300;        ///< ...to here (the track's clips before and after)

    double masterGain = 1.0;
    /** The master's channels (stereo, or a surround format), its device channel groups, and how it
        is folded down to stereo for an audio device with fewer outputs. */
    ChannelLayout masterLayout = ChannelLayout::stereo();
    ChannelGroups masterGroups;
    ChannelMatrix masterFoldDown;
    AutomationCurve masterVolumeAutomation;
    std::vector<RenderDevice> masterDevices;
    std::vector<ParameterLane> masterParameterLanes;
    /** A Master Fader on the main output: after the master bus, one more fader (with mute) and its
        post-fader devices. */
    bool hasMasterFader = false;
    double masterFaderVolume = 1.0;
    bool masterFaderAudible = true;
    AutomationCurve masterFaderAutomation;
    std::vector<RenderDevice> masterFaderDevices;
    std::vector<ParameterLane> masterFaderParameterLanes;

    std::vector<RenderTrack> tracks;
    std::vector<std::vector<int>> levels; ///< graph indices per dependency level
    std::vector<int> masterSources;       ///< graph indices feeding the master bus

    int totalLatency = 0; ///< latency of the whole graph at the master output (for offline renders)

    std::vector<std::shared_ptr<const void>> keepAlive; ///< owners of sample data, nodes, stretchers
};

/** Volume automation mapping, linear in dB: 0 -> -inf (silence), (0, 1] -> -60 dB .. +6 dB, so 0 dB
    sits at 60/66 ≈ 0.909. */
[[nodiscard]] double automationToGain(double normalised) noexcept;
[[nodiscard]] double gainToAutomation(double linearGain) noexcept;

/** Balance pan law: centre is unity on both sides; panning attenuates the opposite side linearly. */
[[nodiscard]] inline std::pair<double, double> balancePanGains(double pan) noexcept
{
    pan = pan < -1.0 ? -1.0 : (pan > 1.0 ? 1.0 : pan);
    return {pan <= 0.0 ? 1.0 : 1.0 - pan, pan >= 0.0 ? 1.0 : 1.0 + pan};
}

} // namespace dojo::engine
