#pragma once

#include "video/VideoSource.h"

#include <juce_events/juce_events.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <tuple>

namespace dojo::video
{

/**
    Decodes video frames on a background thread, for the video window (the frame at the playhead)
    and the arranger (thumbnails of video clips), so the message thread never waits for a decoder.

    The message thread asks; if the answer is not ready yet it gets the best it has (the previous
    playback frame, or no thumbnail) and onFramesReady is called once the worker has more. Playback
    requests always win over thumbnails, and only the newest one counts. Each file gets one decoder
    for playback and one for thumbnails, so scrubbing never disturbs the thumbnail strip.
*/
class FrameServer final : public juce::ChangeBroadcaster, private juce::Thread, private juce::AsyncUpdater
{
public:
    FrameServer();
    ~FrameServer() override;

    /** What a file holds (opened once and remembered); nullopt with `error` if it cannot be decoded.
        Message thread (it may open the file there the first time). */
    std::optional<VideoInfo> getInfo(const juce::File& file, juce::String* error = nullptr);

    /** The frame of `file` at `seconds` in a box (0 = full size). Returns the newest frame decoded
        for this file so far (exactly this one once ready) and asks for this one. */
    juce::Image requestFrame(const juce::File& file, double seconds, int maxWidth, int maxHeight);

    /** A thumbnail `height` pixels high of the frame at `seconds`; invalid until decoded. */
    juce::Image getThumbnail(const juce::File& file, double seconds, int height);

    /** Called on the message thread after the worker produced frames or thumbnails (listeners also
        get a change message). */
    std::function<void()> onFramesReady;

    /** Tests: waits (up to `timeoutMs`) until the worker has nothing left to do. */
    bool waitUntilIdle(int timeoutMs);

    /** Forgets every decoder, frame and thumbnail (e.g. when a file changed on disk). */
    void clear();

private:
    struct Request
    {
        juce::File file;
        double seconds = 0.0;
        int maxWidth = 0;
        int maxHeight = 0;
    };
    struct Frame
    {
        juce::File file;
        long long index = -1;
        juce::Image image;
    };
    using ThumbnailKey = std::tuple<juce::String, long long, int>;

    void run() override;
    void handleAsyncUpdate() override;
    VideoSource* sourceFor(std::map<juce::String, std::unique_ptr<VideoSource>>& sources, const juce::File& file);

    std::mutex mutex_;
    std::condition_variable wake_;
    bool busy_ = false;
    bool resetDecoders_ = false;
    std::optional<Request> playbackRequest_;
    Frame playbackFrame_;
    std::deque<std::pair<ThumbnailKey, Request>> thumbnailRequests_;
    std::map<ThumbnailKey, juce::Image> thumbnails_;
    std::deque<ThumbnailKey> thumbnailOrder_; ///< oldest first, to bound the cache
    std::map<juce::String, std::optional<VideoInfo>> infos_;
    std::map<juce::String, juce::String> errors_;

    // Worker thread only.
    std::map<juce::String, std::unique_ptr<VideoSource>> playbackSources_;
    std::map<juce::String, std::unique_ptr<VideoSource>> thumbnailSources_;
};

} // namespace dojo::video
