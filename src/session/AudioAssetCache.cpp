#include "session/AudioAssetCache.h"

#include <algorithm>
#include <limits>

namespace dojo::session
{
namespace
{
constexpr double kMaxDecodedSeconds = 2.0 * 60.0 * 60.0; // memory-resident limit: two hours

juce::String keyFor(const juce::File& file)
{
    return file.getFullPathName();
}

/** Peaks of a file read sequentially in blocks, without keeping the audio (for streamed files). */
void computePeaksFromReader(AudioAsset& asset, juce::AudioFormatReader& reader)
{
    auto& peaks = asset.peaks;
    const int numChannels = asset.numChannels;
    const auto numFrames = asset.numFrames;
    const auto numBuckets = static_cast<std::size_t>((numFrames + WaveformPeaks::kFramesPerBucket - 1) /
                                                     WaveformPeaks::kFramesPerBucket);
    peaks.numChannels = numChannels;
    for (int ch = 0; ch < numChannels; ++ch)
    {
        peaks.minimum[static_cast<std::size_t>(ch)].assign(numBuckets, 0.0f);
        peaks.maximum[static_cast<std::size_t>(ch)].assign(numBuckets, 0.0f);
    }

    constexpr int kBlock = WaveformPeaks::kFramesPerBucket * 256;
    juce::AudioBuffer<float> buffer(numChannels, kBlock);

    for (juce::int64 start = 0; start < numFrames; start += kBlock)
    {
        const auto count = static_cast<int>(std::min<juce::int64>(kBlock, numFrames - start));
        reader.read(&buffer, 0, count, start, true, numChannels > 1);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float* data = buffer.getReadPointer(ch);
            for (int begin = 0; begin < count; begin += WaveformPeaks::kFramesPerBucket)
            {
                const int end = std::min(count, begin + WaveformPeaks::kFramesPerBucket);
                const auto [lo, hi] = std::minmax_element(data + begin, data + end);
                const auto bucket = static_cast<std::size_t>((start + begin) / WaveformPeaks::kFramesPerBucket);
                peaks.minimum[static_cast<std::size_t>(ch)][bucket] = *lo;
                peaks.maximum[static_cast<std::size_t>(ch)][bucket] = *hi;
            }
        }
    }
}

void computePeaks(AudioAsset& asset)
{
    auto& peaks = asset.peaks;
    const int numChannels = asset.audio.getNumChannels();
    const int numFrames = asset.audio.getNumSamples();
    const int numBuckets =
        (numFrames + WaveformPeaks::kFramesPerBucket - 1) / WaveformPeaks::kFramesPerBucket;

    peaks.numChannels = numChannels;

    for (int ch = 0; ch < numChannels; ++ch)
    {
        const auto chIndex = static_cast<std::size_t>(ch);
        peaks.minimum[chIndex].resize(static_cast<std::size_t>(numBuckets));
        peaks.maximum[chIndex].resize(static_cast<std::size_t>(numBuckets));
        const float* data = asset.audio.getReadPointer(ch);

        for (int bucket = 0; bucket < numBuckets; ++bucket)
        {
            const int begin = bucket * WaveformPeaks::kFramesPerBucket;
            const int end = std::min(numFrames, begin + WaveformPeaks::kFramesPerBucket);
            const auto [lo, hi] = std::minmax_element(data + begin, data + end);
            peaks.minimum[chIndex][static_cast<std::size_t>(bucket)] = *lo;
            peaks.maximum[chIndex][static_cast<std::size_t>(bucket)] = *hi;
        }
    }
}
} // namespace

//==================================================================================================
/** Keeps the resident regions of every streamed file up to date (every few milliseconds). */
class AudioAssetCache::StreamService final : public juce::Thread
{
public:
    StreamService()
        : juce::Thread("Dojo disk streaming")
    {
    }

    ~StreamService() override { stopThread(2000); }

    void add(std::shared_ptr<engine::StreamedAudio> audio)
    {
        {
            const std::lock_guard lock(mutex_);
            streams_.push_back(std::move(audio));
        }
        if (!isThreadRunning())
            startThread(juce::Thread::Priority::high);
    }

    void setEpochSource(std::function<std::uint64_t()> source)
    {
        const std::lock_guard lock(mutex_);
        epochSource_ = std::move(source);
    }

private:
    void run() override
    {
        std::uint64_t fallbackEpoch = 0;

        while (!threadShouldExit())
        {
            std::vector<std::shared_ptr<engine::StreamedAudio>> streams;
            std::uint64_t epoch = 0;
            {
                const std::lock_guard lock(mutex_);
                // Drop streams nobody else references any more (their clips are gone).
                streams_.erase(std::remove_if(streams_.begin(), streams_.end(),
                                              [](const auto& s) { return s.use_count() == 1; }),
                               streams_.end());
                streams = streams_;
                epoch = epochSource_ ? epochSource_() : ++fallbackEpoch;
            }

            for (auto& stream : streams)
                stream->service(epoch);

            wait(5);
        }
    }

    std::mutex mutex_;
    std::vector<std::shared_ptr<engine::StreamedAudio>> streams_;
    std::function<std::uint64_t()> epochSource_;
};

//==================================================================================================
AudioAssetCache::AudioAssetCache()
    : streamService_(std::make_unique<StreamService>())
{
    formats_.registerBasicFormats();
}

AudioAssetCache::~AudioAssetCache()
{
    pool_.removeAllJobs(true, 10'000);
}

void AudioAssetCache::setEpochSource(std::function<std::uint64_t()> source)
{
    streamService_->setEpochSource(std::move(source));
}

AudioAssetPtr AudioAssetCache::find(const juce::File& file) const
{
    const auto it = assets_.find(keyFor(file));
    return it != assets_.end() ? it->second : nullptr;
}

bool AudioAssetCache::isLoading(const juce::File& file) const
{
    return pending_.count(keyFor(file)) > 0;
}

bool AudioAssetCache::canRead(const juce::File& file) const
{
    return file.existsAsFile() && formats_.findFormatForFileExtension(file.getFileExtension()) != nullptr;
}

void AudioAssetCache::request(const juce::File& file, Completion onDone)
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (auto existing = find(file))
    {
        if (onDone)
            onDone(existing, {});
        return;
    }

    // Don't hammer the disk for a file that just failed and is still missing (graphs are rebuilt on
    // every edit); a file that has reappeared is retried.
    if (const auto failed = failures_.find(keyFor(file)); failed != failures_.end())
    {
        if (!file.existsAsFile())
        {
            if (onDone)
                onDone(nullptr, failed->second);
            return;
        }
        failures_.erase(failed);
    }

    auto& waiters = pending_[keyFor(file)];
    const bool alreadyLoading = !waiters.empty(); // a decode job for this file is already queued
    waiters.push_back(std::move(onDone));

    if (alreadyLoading)
        return;

    juce::WeakReference<AudioAssetCache> weakThis(this);
    auto* formats = &formats_;
    const double threshold = streamingThreshold_;

    pool_.addJob(
        [weakThis, formats, file, threshold]
        {
            auto result = decode(*formats, file, threshold);
            juce::MessageManager::callAsync(
                [weakThis, file, result]
                {
                    if (auto* self = weakThis.get())
                        self->finish(file, result);
                });
        });
}

AudioAssetPtr AudioAssetCache::loadNow(const juce::File& file, juce::String& error)
{
    if (auto existing = find(file))
        return existing;

    auto result = decode(formats_, file, streamingThreshold_);
    error = result.error;

    if (result.asset == nullptr)
        return nullptr;

    if (result.asset->streamed != nullptr)
        streamService_->add(result.asset->streamed);

    AudioAssetPtr asset = result.asset;
    assets_[keyFor(file)] = asset;
    return asset;
}

AudioAssetCache::DecodeResult AudioAssetCache::decode(juce::AudioFormatManager& formats, const juce::File& file,
                                                      double streamingThreshold)
{
    if (!file.existsAsFile())
        return {nullptr, "File not found: " + file.getFullPathName()};

    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));

    if (reader == nullptr)
        return {nullptr, "Unsupported or unreadable audio file: " + file.getFileName()};

    if (reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0 || reader->numChannels == 0)
        return {nullptr, "The file contains no audio: " + file.getFileName()};

    auto asset = std::make_shared<AudioAsset>();
    asset->file = file;
    asset->sampleRate = reader->sampleRate;
    asset->numChannels = std::clamp(static_cast<int>(reader->numChannels), 1, engine::kMaxChannels);
    asset->numFrames = reader->lengthInSamples;

    const double duration = static_cast<double>(reader->lengthInSamples) / reader->sampleRate;
    const bool stream = duration > streamingThreshold || duration > kMaxDecodedSeconds ||
                        reader->lengthInSamples > std::numeric_limits<int>::max();

    if (stream)
    {
        computePeaksFromReader(*asset, *reader);

        // The stream gets its own reader: only the service thread reads from it from now on.
        std::unique_ptr<juce::AudioFormatReader> streamReader(formats.createReaderFor(file));
        if (streamReader == nullptr)
            return {nullptr, "Could not open " + file.getFileName() + " for streaming"};
        asset->streamed = std::make_shared<engine::StreamedAudio>(std::move(streamReader));
        return {asset, {}};
    }

    const int numChannels = asset->numChannels;
    const auto numFrames = static_cast<int>(reader->lengthInSamples);
    asset->audio.setSize(numChannels, numFrames);

    if (!reader->read(&asset->audio, 0, numFrames, 0, true, numChannels > 1))
        return {nullptr, "Could not decode " + file.getFileName()};

    computePeaks(*asset);
    return {asset, {}};
}

void AudioAssetCache::finish(const juce::File& file, const DecodeResult& result)
{
    const auto key = keyFor(file);
    auto waiters = std::move(pending_[key]);
    pending_.erase(key);

    AudioAssetPtr asset = result.asset;
    if (asset != nullptr)
    {
        assets_[key] = asset;
        if (asset->streamed != nullptr)
            streamService_->add(asset->streamed);
    }
    else
    {
        failures_[key] = result.error;
    }

    for (auto& waiter : waiters)
        if (waiter)
            waiter(asset, result.error);

    if (asset != nullptr)
        listeners_.call([&file](Listener& l) { l.audioAssetLoaded(file); });
}

} // namespace dojo::session
