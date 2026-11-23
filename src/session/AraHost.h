#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace dojo::session
{
class AudioAssetCache;
class DeviceManager;
class Session;

/**
    ARA 2 hosting (Audio Random Access: Melodyne, VocAlign, ...), for VST3 plug-ins with the ARA
    extension, on Windows and Linux alike.

    An audio track's ARA plug-in is a hosted plug-in in its first device slot, marked as the track's
    ARA device (Track > ARA Plug-in). When its instance is created, before it is prepared, it is bound
    to its plug-in's document (one document per plug-in factory, for the whole project) as playback
    renderer, editor renderer and editor view. The track's audio clips are then its playback regions:
    the engine leaves them out (rendersClipsOf) and the plug-in plays them from the audio it reads
    through the host. Bypassed, the plug-in passes the track's own clips through as usual.

    The documents follow the session (coalesced after each change, or synchronise()):
      - a musical context with the session's tempo and 4/4 (as tempo entries and bar signatures);
      - a region sequence per ARA track, named after it;
      - an audio source per audio file, its samples read from the decoded asset or the file;
      - an audio modification and a playback region per clip: its place on the timeline, its part of
        the file (time-stretched clips as such where the plug-in can), named after the clip. The
        plug-in plays each clip's part of its file as it is: a clip's gain, fades, pitch, looping and
        source channel are not applied on these tracks (ARA regions carry none of them; levels,
        fades and pitch are edited in the plug-in).
    A plug-in's set of regions only changes with the plug-in out of play (released, the regions
    swapped, prepared again: ProcessorNode::reconfigure), so the audio thread never waits. Each
    document's archive (the plug-in's edits) is written into the session before it is saved and
    given back to the plug-in once its regions exist again after the session is opened. A document
    lives while plug-ins are bound to it (ARA lets a plug-in outlive its document, not the module
    the document's code is in), and is taken down with the last of them.

    Message thread only (the plug-ins read audio from any thread; that side is thread-safe).
    Without ARA in the build (DOJO_WITH_ARA off) nothing is ever bound.
*/
class AraHost final : public juce::ChangeBroadcaster
{
public:
    AraHost(Session& session, AudioAssetCache& assets, DeviceManager& devices);
    ~AraHost();

    AraHost(const AraHost&) = delete;
    AraHost& operator=(const AraHost&) = delete;

    /** Whether ARA hosting is in this build. */
    [[nodiscard]] static bool isAvailable() noexcept;
    /** Whether a plug-in has the ARA extension (and ARA hosting is in this build). */
    [[nodiscard]] static bool isAraPlugin(const juce::PluginDescription& description) noexcept;

    /** A device instance has just been created, before it is prepared (DeviceManager): binds it to
        its document if it is an audio track's ARA device. */
    void deviceCreated(const juce::ValueTree& device, juce::AudioProcessor& processor);
    /** A device instance is about to be dropped (still alive): its regions are taken from it. If it
        was the last plug-in of its document, the document's archive goes into the session (undoing
        the removal brings the edits back) and the document is taken down, while the plug-in's module
        is surely still loaded. */
    void deviceRemoved(std::uint32_t deviceId);

    /** Whether a track's clips are played by its ARA plug-in (bound, first, not bypassed). */
    [[nodiscard]] bool rendersClipsOf(std::uint32_t trackId) const;

    /** Brings the documents in line with the session now (it also happens by itself, coalesced). */
    void synchronise();

    /** Writes each document's archive into the session (call before saving). */
    void storeArchives();

    /** The session is about to be replaced (new or opened): the documents are taken down. */
    void reset();

    /** Tells a track's ARA plug-in which of its clips are selected (its editor shows them). */
    void showSelection(std::uint32_t trackId, const std::vector<std::uint32_t>& clipIds);

    struct TrackInfo
    {
        bool bound = false;          ///< its ARA plug-in is bound to a document
        bool rendering = false;      ///< ... and plays the track's clips (rendersClipsOf)
        juce::String plugin;         ///< the plug-in's name
        std::uint32_t deviceId = 0;  ///< its device
        int regions = 0;             ///< the playback regions it renders
        juce::String problem;        ///< why it is not bound, if it should be
    };
    [[nodiscard]] TrackInfo getTrackInfo(std::uint32_t trackId) const;

    /** Called (message thread) when which tracks the plug-ins play changed: compile the graph again. */
    std::function<void()> onRenderingChanged;
    // (as a ChangeBroadcaster it tells listeners when a plug-in is bound, released or starts or stops
    //  playing its track: synchronously before a plug-in goes, so that its editor can go first)
    /** A plug-in asks the host to start (true) or stop (false) playing (message thread). */
    std::function<void(bool play)> onRequestPlayback;
    /** A plug-in asks the host to move the playhead (seconds, message thread). */
    std::function<void(double seconds)> onRequestLocate;
    // (its requests for the cycle set the session's loop range and switch it on or off, each an
    //  undo step, with no callback needed)

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace dojo::session
