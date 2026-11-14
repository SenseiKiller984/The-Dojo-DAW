#include "engine/Preview.h"

#include "engine/AudioEngine.h"

#include <algorithm>
#include <cmath>

namespace dojo::engine
{

//==================================================================================================
// PreviewPlayer — non-real-time side

void PreviewPlayer::play(std::shared_ptr<PreviewSound> sound)
{
    const std::lock_guard lock(mutex_);
    if (current_ != nullptr && current_ != sound)
        retired_.push_back(current_);
    current_ = std::move(sound);
    pending_.store(current_.get(), std::memory_order_release);
    // Published after pending_: the audio thread reads request_ first, so a new request always comes
    // with (at least) the sound that was set before it.
    request_.store(++serial_ << 1, std::memory_order_release);
    progress_.store(0.0f, std::memory_order_relaxed);

    const auto* inUse = acknowledged_.load(std::memory_order_acquire);
    if (inUse == pending_.load(std::memory_order_acquire))
        retired_.erase(std::remove_if(retired_.begin(), retired_.end(), [&](const auto& s) { return s.get() != inUse; }),
                       retired_.end());
}

void PreviewPlayer::stop()
{
    const std::lock_guard lock(mutex_);
    request_.store((++serial_ << 1) | 1u, std::memory_order_release);
}

std::shared_ptr<PreviewSound> PreviewPlayer::getCurrent() const
{
    const std::lock_guard lock(mutex_);
    return current_;
}

void PreviewPlayer::collectGarbage()
{
    const std::lock_guard lock(mutex_);
    const auto* inUse = acknowledged_.load(std::memory_order_acquire);
    const auto* pending = pending_.load(std::memory_order_acquire);
    // Anything the audio thread has moved on from (it acknowledged the pending sound) can go.
    if (inUse == pending)
        retired_.erase(std::remove_if(retired_.begin(), retired_.end(),
                                      [&](const auto& s) { return s.get() != inUse && s.get() != pending; }),
                       retired_.end());
}

//==================================================================================================
// PreviewPlayer — audio thread

void PreviewPlayer::addTo(float* const* outputs, int numOutputs, int numSamples) noexcept
{
    const auto request = request_.load(std::memory_order_acquire);
    PreviewSound* sound = pending_.load(std::memory_order_acquire);
    acknowledged_.store(sound, std::memory_order_release);

    if (request != seenRequest_)
    {
        seenRequest_ = request;
        if ((request & 1u) != 0)
        {
            stopping_ = true;
        }
        else
        {
            position_ = 0;
            fade_ = 0.0f;
            stopping_ = false;
            running_ = sound != nullptr;
        }
    }

    if (!running_ || sound == nullptr || numOutputs <= 0 || outputs == nullptr)
    {
        running_ = false;
        active_.store(false, std::memory_order_release);
        return;
    }

    const int ready = sound->framesReady.load(std::memory_order_acquire);
    const float gain = gain_.load(std::memory_order_relaxed);
    const float* left = sound->audio.getReadPointer(0);
    const float* right = sound->audio.getReadPointer(sound->audio.getNumChannels() > 1 ? 1 : 0);
    float* outLeft = outputs[0];
    float* outRight = numOutputs > 1 ? outputs[1] : nullptr;

    for (int i = 0; i < numSamples && position_ < ready; ++i)
    {
        if (stopping_)
        {
            fade_ -= kFadeStep;
            if (fade_ <= 0.0f)
            {
                running_ = false;
                break;
            }
        }
        else if (fade_ < 1.0f)
        {
            fade_ = std::min(1.0f, fade_ + kFadeStep);
        }

        const float g = gain * fade_;
        const float l = left[position_] * g;
        const float r = right[position_] * g;
        if (outRight != nullptr)
        {
            if (outLeft != nullptr)
                outLeft[i] += l;
            outRight[i] += r;
        }
        else if (outLeft != nullptr)
        {
            outLeft[i] += 0.5f * (l + r);
        }
        ++position_;
    }

    if (position_ >= sound->totalFrames || (sound->failed.load(std::memory_order_relaxed) && position_ >= ready))
        running_ = false;

    progress_.store(sound->totalFrames > 0 ? static_cast<float>(position_) / static_cast<float>(sound->totalFrames) : 0.0f,
                    std::memory_order_relaxed);
    active_.store(running_, std::memory_order_release);
}

//==================================================================================================
// PreviewService

PreviewService::PreviewService(AudioEngine& engine, juce::AudioFormatManager& formats)
    : juce::Thread("Dojo preview decoder")
    , engine_(engine)
    , formats_(formats)
{
    startThread(juce::Thread::Priority::normal);
}

PreviewService::~PreviewService()
{
    {
        const std::lock_guard lock(jobMutex_);
        jobPending_ = false;
        if (decoding_ != nullptr)
            decoding_->cancelled.store(true);
    }
    signalThreadShouldExit();
    notify();
    stopThread(5000);
}

bool PreviewService::preview(const juce::File& file)
{
    const double rate = engine_.getSampleRate();
    if (!engine_.isPrepared() || rate <= 0.0 || !file.existsAsFile())
        return false;
    if (!formats_.getWildcardForAllFormats().containsIgnoreCase(file.getFileExtension().toLowerCase()))
        return false;

    {
        const std::lock_guard lock(jobMutex_);
        if (decoding_ != nullptr)
            decoding_->cancelled.store(true);
        jobFile_ = file;
        jobRate_ = rate;
        jobPending_ = true;
        jobGeneration_ = ++generation_;
        decodeDone_ = false;
        currentFile_ = file;
    }
    engine_.getPreviewPlayer().stop();
    engine_.getPreviewPlayer().collectGarbage();
    notify();
    return true;
}

void PreviewService::stop()
{
    {
        const std::lock_guard lock(jobMutex_);
        jobPending_ = false;
        decodeDone_ = true;
        ++generation_;
        if (decoding_ != nullptr)
            decoding_->cancelled.store(true);
        decoding_.reset();
    }
    engine_.getPreviewPlayer().stop();
    engine_.getPreviewPlayer().collectGarbage();
}

bool PreviewService::isPlaying() const noexcept
{
    return engine_.getPreviewPlayer().isPlaying();
}

juce::File PreviewService::getCurrentFile() const
{
    const std::lock_guard lock(jobMutex_);
    return currentFile_;
}

float PreviewService::getProgress() const noexcept
{
    return engine_.getPreviewPlayer().getProgress();
}

void PreviewService::setVolume(float linearGain) noexcept
{
    engine_.getPreviewPlayer().setGain(juce::jlimit(0.0f, 2.0f, linearGain));
}

float PreviewService::getVolume() const noexcept
{
    return engine_.getPreviewPlayer().getGain();
}

void PreviewService::collectGarbage()
{
    engine_.getPreviewPlayer().collectGarbage();
}

bool PreviewService::isDecodeFinished() const
{
    const std::lock_guard lock(jobMutex_);
    return !jobPending_ && decodeDone_;
}

void PreviewService::run()
{
    while (!threadShouldExit())
    {
        juce::File file;
        double rate = 0.0;
        std::uint64_t generation = 0;
        {
            const std::lock_guard lock(jobMutex_);
            if (jobPending_)
            {
                file = jobFile_;
                rate = jobRate_;
                generation = jobGeneration_;
                jobPending_ = false;
            }
        }

        if (file == juce::File())
        {
            wait(-1);
            continue;
        }

        const auto finished = [this, generation]
        {
            const std::lock_guard lock(jobMutex_);
            if (generation == generation_)
                decodeDone_ = true;
        };

        std::unique_ptr<juce::AudioFormatReader> reader(formats_.createReaderFor(file));
        if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
        {
            finished();
            continue;
        }

        auto sound = std::make_shared<PreviewSound>();
        sound->file = file;
        sound->sampleRate = rate;
        const double seconds = static_cast<double>(reader->lengthInSamples) / reader->sampleRate;
        sound->truncated = seconds > kMaxSeconds;
        sound->totalFrames = std::max(1, static_cast<int>(std::ceil(std::min(seconds, kMaxSeconds) * rate)));
        sound->audio.setSize(2, sound->totalFrames);

        {
            const std::lock_guard lock(jobMutex_);
            if (generation != generation_) // superseded or stopped while opening the file
                continue;
            decoding_ = sound;
            engine_.getPreviewPlayer().play(sound);
        }

        decode(*sound, *reader, rate);
        finished();
    }
}

void PreviewService::decode(PreviewSound& sound, juce::AudioFormatReader& reader, double targetRate)
{
    juce::AudioFormatReaderSource source(&reader, false);
    juce::ResamplingAudioSource resampler(&source, false, 2);
    resampler.setResamplingRatio(reader.sampleRate / targetRate);

    constexpr int kChunk = 4096;
    resampler.prepareToPlay(kChunk, targetRate);

    int done = 0;
    while (done < sound.totalFrames)
    {
        if (sound.cancelled.load(std::memory_order_relaxed) || threadShouldExit())
            return;

        const int n = std::min(kChunk, sound.totalFrames - done);
        const juce::AudioSourceChannelInfo info(&sound.audio, done, n);
        resampler.getNextAudioBlock(info);
        done += n;
        sound.framesReady.store(done, std::memory_order_release);
    }
    resampler.releaseResources();
}

} // namespace dojo::engine
