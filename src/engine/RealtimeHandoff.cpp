#include "engine/RealtimeHandoff.h"

#include "engine/AudioEngine.h"

#include <algorithm>

namespace dojo::engine
{

//==================================================================================================
// ReleasePool

ReleasePool::ReleasePool(const AudioEngine& engine)
    : engine_(engine)
{
}

void ReleasePool::release(std::shared_ptr<const void> object)
{
    if (object != nullptr)
        pending_.push_back({std::move(object), engine_.getProcessEpoch()});
}

void ReleasePool::collect()
{
    const auto epoch = engine_.getProcessEpoch();
    const bool idle = !engine_.isProcessingActive();

    pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                  [epoch, idle](const Entry& e) { return idle || epoch >= e.epoch + 2; }),
                   pending_.end());
}

//==================================================================================================
// LiveMidiQueue

LiveMidiQueue::LiveMidiQueue() = default;

bool LiveMidiQueue::push(const juce::MidiMessage& message)
{
    const int size = message.getRawDataSize();
    if (size <= 0 || size > 3)
        return false;

    LiveMidiEvent event;
    std::copy_n(message.getRawData(), size, event.data);
    event.size = static_cast<std::uint8_t>(size);

    const std::lock_guard<std::mutex> lock(producerMutex_);
    return queue_.tryPush(event);
}

//==================================================================================================
// CaptureStream

CaptureStream::CaptureStream(std::unique_ptr<juce::AudioFormatWriter> writer, int numChannels, double sampleRate,
                             double secondsOfBuffer)
    : writer_(std::move(writer))
    , numChannels_(std::max(1, numChannels))
    , capacity_(std::max(4096, static_cast<int>(sampleRate * secondsOfBuffer)))
    , ring_(static_cast<std::size_t>(numChannels_) * static_cast<std::size_t>(capacity_), 0.0f)
{
    chunk_.setSize(numChannels_, 8192);
}

CaptureStream::~CaptureStream()
{
    finish();
}

void CaptureStream::write(const Sample* const* channels, int numChannels, int numSamples) noexcept
{
    const auto write = writePosition_.load(std::memory_order_relaxed);
    const auto read = readPosition_.load(std::memory_order_acquire);
    const auto free = static_cast<std::uint64_t>(capacity_) - (write - read);

    if (static_cast<std::uint64_t>(numSamples) > free)
    {
        overflow_.fetch_add(static_cast<std::uint64_t>(numSamples), std::memory_order_relaxed);
        return;
    }

    for (int ch = 0; ch < numChannels_; ++ch)
    {
        const Sample* source = channels[std::min(ch, numChannels - 1)];
        float* ring = ring_.data() + static_cast<std::size_t>(ch) * static_cast<std::size_t>(capacity_);

        for (int i = 0; i < numSamples; ++i)
            ring[(write + static_cast<std::uint64_t>(i)) % static_cast<std::uint64_t>(capacity_)] = static_cast<float>(source[i]);
    }

    writePosition_.store(write + static_cast<std::uint64_t>(numSamples), std::memory_order_release);
}

int CaptureStream::drainToDisk()
{
    if (writer_ == nullptr)
        return 0;

    int total = 0;

    for (;;)
    {
        const auto read = readPosition_.load(std::memory_order_relaxed);
        const auto write = writePosition_.load(std::memory_order_acquire);
        const auto available = static_cast<int>(std::min<std::uint64_t>(write - read, static_cast<std::uint64_t>(chunk_.getNumSamples())));

        if (available <= 0)
            return total;

        if (skip_ > 0)
        {
            const auto skipped = std::min<std::uint64_t>(skip_, static_cast<std::uint64_t>(available));
            readPosition_.store(read + skipped, std::memory_order_release);
            skip_ -= skipped;
            continue;
        }

        for (int ch = 0; ch < numChannels_; ++ch)
        {
            const float* ring = ring_.data() + static_cast<std::size_t>(ch) * static_cast<std::size_t>(capacity_);
            float* dest = chunk_.getWritePointer(ch);
            for (int i = 0; i < available; ++i)
                dest[i] = ring[(read + static_cast<std::uint64_t>(i)) % static_cast<std::uint64_t>(capacity_)];
        }

        writer_->writeFromAudioSampleBuffer(chunk_, 0, available);
        readPosition_.store(read + static_cast<std::uint64_t>(available), std::memory_order_release);
        framesWritten_ += static_cast<std::uint64_t>(available);
        total += available;
    }
}

void CaptureStream::finish()
{
    if (writer_ == nullptr)
        return;

    drainToDisk();
    writer_->flush();
    writer_.reset();
}

//==================================================================================================
// DiskWriterThread

DiskWriterThread::DiskWriterThread()
    : juce::Thread("Dojo disk writer")
{
    startThread(juce::Thread::Priority::high);
}

DiskWriterThread::~DiskWriterThread()
{
    stopThread(4000);

    const std::lock_guard<std::mutex> lock(mutex_);
    for (auto& stream : streams_)
        stream->finish();
}

void DiskWriterThread::add(std::shared_ptr<CaptureStream> stream)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    streams_.push_back(std::move(stream));
}

void DiskWriterThread::remove(const std::shared_ptr<CaptureStream>& stream)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    stream->finish();
    streams_.erase(std::remove(streams_.begin(), streams_.end(), stream), streams_.end());
}

void DiskWriterThread::run()
{
    while (!threadShouldExit())
    {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            for (auto& stream : streams_)
                stream->drainToDisk();
        }
        wait(5);
    }
}

} // namespace dojo::engine
