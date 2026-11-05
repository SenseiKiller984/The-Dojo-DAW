#include "session/AudioAssetCache.h"

#include <algorithm>
#include <limits>

namespace dojo::session
{
namespace
{
constexpr double kMaxDurationSeconds = 2.0 * 60.0 * 60.0; // memory-resident limit: two hours

juce::String keyFor(const juce::File& file)
{
    return file.getFullPathName();
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

AudioAssetCache::AudioAssetCache()
{
    formats_.registerBasicFormats();
}

AudioAssetCache::~AudioAssetCache()
{
    pool_.removeAllJobs(true, 10'000);
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

    pool_.addJob(
        [weakThis, formats, file]
        {
            auto result = decode(*formats, file);
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

    auto result = decode(formats_, file);
    error = result.error;

    if (result.asset == nullptr)
        return nullptr;

    AudioAssetPtr asset = result.asset;
    assets_[keyFor(file)] = asset;
    return asset;
}

AudioAssetCache::DecodeResult AudioAssetCache::decode(juce::AudioFormatManager& formats,
                                                      const juce::File& file)
{
    if (!file.existsAsFile())
        return {nullptr, "File not found: " + file.getFullPathName()};

    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));

    if (reader == nullptr)
        return {nullptr, "Unsupported or unreadable audio file: " + file.getFileName()};

    if (reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0 || reader->numChannels == 0)
        return {nullptr, "The file contains no audio: " + file.getFileName()};

    if (static_cast<double>(reader->lengthInSamples) / reader->sampleRate > kMaxDurationSeconds ||
        reader->lengthInSamples > std::numeric_limits<int>::max())
        return {nullptr,
                "The file is longer than two hours, which is not supported yet: " + file.getFileName()};

    auto asset = std::make_shared<AudioAsset>();
    asset->file = file;
    asset->sampleRate = reader->sampleRate;

    const int numChannels = reader->numChannels > 1 ? 2 : 1;
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
        assets_[key] = asset;
    else
        failures_[key] = result.error;

    for (auto& waiter : waiters)
        if (waiter)
            waiter(asset, result.error);

    if (asset != nullptr)
        listeners_.call([&file](Listener& l) { l.audioAssetLoaded(file); });
}

} // namespace dojo::session
