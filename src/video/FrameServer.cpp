#include "video/FrameServer.h"

#include <algorithm>

namespace dojo::video
{
namespace
{
constexpr std::size_t kMaxThumbnails = 2000;
constexpr std::size_t kMaxPendingThumbnails = 400;
} // namespace

FrameServer::FrameServer()
    : juce::Thread("Dojo video decoding")
{
    startThread(juce::Thread::Priority::normal);
}

FrameServer::~FrameServer()
{
    signalThreadShouldExit();
    {
        const std::lock_guard lock(mutex_);
        wake_.notify_all();
    }
    stopThread(10'000);
    cancelPendingUpdate();
}

std::optional<VideoInfo> FrameServer::getInfo(const juce::File& file, juce::String* error)
{
    const auto key = file.getFullPathName();
    {
        const std::lock_guard lock(mutex_);
        if (const auto it = infos_.find(key); it != infos_.end())
        {
            if (error != nullptr && !it->second.has_value())
                *error = errors_[key];
            return it->second;
        }
    }

    juce::String message;
    auto source = openVideo(file, message);
    std::optional<VideoInfo> info;
    if (source != nullptr)
        info = source->getInfo();

    const std::lock_guard lock(mutex_);
    infos_[key] = info;
    errors_[key] = message;
    if (error != nullptr)
        *error = message;
    return info;
}

juce::Image FrameServer::requestFrame(const juce::File& file, double seconds, int maxWidth, int maxHeight)
{
    const auto info = getInfo(file);
    if (!info.has_value() || seconds < 0.0 || (info->durationSeconds > 0.0 && seconds >= info->durationSeconds))
        return {};

    const auto index = info->frameIndexAt(seconds);
    const std::lock_guard lock(mutex_);
    const bool sameFile = playbackFrame_.file == file;
    if (!(sameFile && playbackFrame_.index == index))
    {
        playbackRequest_ = Request{file, seconds, maxWidth, maxHeight};
        wake_.notify_all();
    }
    return sameFile ? playbackFrame_.image : juce::Image();
}

juce::Image FrameServer::getThumbnail(const juce::File& file, double seconds, int height)
{
    const auto info = getInfo(file);
    if (!info.has_value() || seconds < 0.0)
        return {};

    const ThumbnailKey key{file.getFullPathName(), info->frameIndexAt(std::min(seconds, std::max(0.0, info->durationSeconds - info->getFrameDuration()))), height};
    const std::lock_guard lock(mutex_);
    if (const auto it = thumbnails_.find(key); it != thumbnails_.end())
        return it->second;

    // Newest requests first (what is on screen now); a bounded queue.
    const auto queued = std::find_if(thumbnailRequests_.begin(), thumbnailRequests_.end(), [&](const auto& r) { return r.first == key; });
    if (queued == thumbnailRequests_.end())
    {
        thumbnailRequests_.emplace_back(key, Request{file, seconds, 0, height});
        if (thumbnailRequests_.size() > kMaxPendingThumbnails)
            thumbnailRequests_.pop_front();
        wake_.notify_all();
    }
    return {};
}

bool FrameServer::waitUntilIdle(int timeoutMs)
{
    const auto deadline = juce::Time::getMillisecondCounter() + static_cast<juce::uint32>(timeoutMs);
    while (juce::Time::getMillisecondCounter() < deadline)
    {
        {
            const std::lock_guard lock(mutex_);
            if (!busy_ && !playbackRequest_.has_value() && thumbnailRequests_.empty())
                return true;
        }
        juce::Thread::sleep(2);
    }
    return false;
}

void FrameServer::clear()
{
    const std::lock_guard lock(mutex_);
    infos_.clear();
    errors_.clear();
    thumbnails_.clear();
    thumbnailOrder_.clear();
    thumbnailRequests_.clear();
    playbackFrame_ = {};
    playbackRequest_.reset();
    resetDecoders_ = true; // (the worker owns its decoders: it drops them itself)
    wake_.notify_all();
}

VideoSource* FrameServer::sourceFor(std::map<juce::String, std::unique_ptr<VideoSource>>& sources, const juce::File& file)
{
    const auto key = file.getFullPathName();
    if (const auto it = sources.find(key); it != sources.end())
        return it->second.get();
    juce::String error;
    auto source = openVideo(file, error);
    auto* raw = source.get();
    sources[key] = std::move(source);
    return raw;
}

void FrameServer::run()
{
    while (!threadShouldExit())
    {
        std::optional<Request> playback;
        std::optional<std::pair<ThumbnailKey, Request>> thumbnail;
        {
            std::unique_lock lock(mutex_);
            busy_ = false;
            wake_.wait_for(lock, std::chrono::milliseconds(100),
                           [&] { return threadShouldExit() || resetDecoders_ || playbackRequest_.has_value() || !thumbnailRequests_.empty(); });
            if (threadShouldExit())
                return;
            if (resetDecoders_)
            {
                playbackSources_.clear();
                thumbnailSources_.clear();
                resetDecoders_ = false;
            }
            if (playbackRequest_.has_value())
            {
                playback = playbackRequest_;
                playbackRequest_.reset();
            }
            else if (!thumbnailRequests_.empty())
            {
                thumbnail = thumbnailRequests_.back();
                thumbnailRequests_.pop_back();
            }
            busy_ = playback.has_value() || thumbnail.has_value();
        }

        if (playback.has_value())
        {
            juce::Image image;
            long long index = -1;
            if (auto* source = sourceFor(playbackSources_, playback->file))
            {
                image = source->getFrame(playback->seconds, playback->maxWidth, playback->maxHeight);
                index = source->getInfo().frameIndexAt(playback->seconds);
            }
            {
                const std::lock_guard lock(mutex_);
                playbackFrame_ = Frame{playback->file, index, image};
            }
            triggerAsyncUpdate();
        }
        else if (thumbnail.has_value())
        {
            juce::Image image;
            if (auto* source = sourceFor(thumbnailSources_, thumbnail->second.file))
                image = source->getFrame(thumbnail->second.seconds, 0, thumbnail->second.maxHeight);
            if (!image.isValid())
                image = juce::Image(juce::Image::ARGB, 1, 1, true); // (nothing there: remember that too)
            {
                const std::lock_guard lock(mutex_);
                thumbnails_[thumbnail->first] = image;
                thumbnailOrder_.push_back(thumbnail->first);
                while (thumbnailOrder_.size() > kMaxThumbnails)
                {
                    thumbnails_.erase(thumbnailOrder_.front());
                    thumbnailOrder_.pop_front();
                }
            }
            triggerAsyncUpdate();
        }
    }
}

void FrameServer::handleAsyncUpdate()
{
    if (onFramesReady)
        onFramesReady();
    sendSynchronousChangeMessage();
}

} // namespace dojo::video
