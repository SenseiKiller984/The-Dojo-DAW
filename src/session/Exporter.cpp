#include "session/Exporter.h"

#include "engine/AudioEngine.h"
#include "engine/io/LameMp3Format.h"

#include <cmath>

namespace dojo::session
{
namespace
{
constexpr int kOfflineBlock = 512;
} // namespace

Exporter::Exporter(Project& project, engine::EngineController& controller)
    : juce::Thread("Dojo export")
    , project_(project)
    , controller_(controller)
{
}

Exporter::~Exporter()
{
    stopTimer();
    if (isThreadRunning())
        stopThread(10'000);
    if (running_ && !settings_.realtime)
    {
        project_.getDevices().setNonRealtime(false);
        controller_.getEngine().endOfflineRender();
    }
}

juce::String Exporter::getFileExtension(ExportSettings::Format format)
{
    switch (format)
    {
    case ExportSettings::Format::flac: return ".flac";
    case ExportSettings::Format::mp3:  return ".mp3";
    case ExportSettings::Format::wav:  break;
    }
    return ".wav";
}

juce::File Exporter::getStemFile(const ExportSettings& settings, const juce::String& trackName)
{
    const auto base = settings.destination.getFileNameWithoutExtension();
    return settings.destination.getParentDirectory().getChildFile(
        juce::File::createLegalFileName(base + " - " + trackName) + getFileExtension(settings.format));
}

std::unique_ptr<juce::AudioFormatWriter> Exporter::createWriter(const juce::File& file, const ExportSettings& settings,
                                                                double sampleRate, int numChannels, juce::String& error)
{
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(file);
    if (static_cast<juce::FileOutputStream*>(stream.get())->failedToOpen())
    {
        error = "Could not write " + file.getFullPathName();
        return nullptr;
    }

    auto options = juce::AudioFormatWriterOptions{}.withSampleRate(sampleRate).withNumChannels(numChannels);
    std::unique_ptr<juce::AudioFormatWriter> writer;

    switch (settings.format)
    {
    case ExportSettings::Format::wav:
    {
        juce::WavAudioFormat wav;
        if (settings.bitDepth >= 32)
            options = options.withBitsPerSample(32).withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
        else
            options = options.withBitsPerSample(settings.bitDepth <= 16 ? 16 : 24);
        writer = wav.createWriterFor(stream, options);
        break;
    }
    case ExportSettings::Format::flac:
    {
        juce::FlacAudioFormat flac;
        writer = flac.createWriterFor(stream, options.withBitsPerSample(settings.bitDepth <= 16 ? 16 : 24));
        break;
    }
    case ExportSettings::Format::mp3:
    {
        engine::LameMp3Format mp3;
        if (!mp3.getPossibleSampleRates().contains(static_cast<int>(sampleRate)))
        {
            error = "MP3 supports sample rates up to 48 kHz; the audio device runs at " + juce::String(sampleRate, 0) + " Hz.";
            return nullptr;
        }
        writer = mp3.createWriterFor(stream, options.withBitsPerSample(16).withQualityOptionIndex(settings.mp3QualityIndex));
        break;
    }
    }

    if (writer == nullptr && error.isEmpty())
        error = "Could not create the encoder for " + file.getFileName();
    return writer;
}

juce::String Exporter::prepareTargets(double sampleRate, bool forStreams)
{
    targets_.clear();
    juce::String error;

    // Surround mixes and stems keep their channels (one per speaker, in the format's order), except
    // in MP3, which is stereo: there they are folded down.
    const bool stereoOnly = settings_.format == ExportSettings::Format::mp3;
    const auto& session = project_.getSession();
    const auto layoutOf = [](TrackFormat format) { return isMultichannel(format) ? getLayout(format) : engine::ChannelLayout::stereo(); };

    const auto add = [&](int slot, const juce::File& file, const engine::ChannelLayout& layout, int latency) -> bool
    {
        Target target;
        target.slot = slot;
        target.latency = std::max(0, latency);
        target.file = file;
        target.layout = layout;
        target.channels = stereoOnly ? 2 : layout.numChannels;
        target.foldDown = layout.numChannels > target.channels;
        auto writer = createWriter(file, settings_, sampleRate, target.channels, error);
        if (writer == nullptr)
            return false;

        if (forStreams)
        {
            target.stream = std::make_shared<engine::CaptureStream>(std::move(writer), target.channels, sampleRate, 10.0);
            target.stream->skipFirstFrames(static_cast<std::uint64_t>(target.latency));
        }
        else
        {
            target.writer = std::move(writer);
        }
        targets_.push_back(std::move(target));
        return true;
    };

    if (settings_.includeMix && !add(-1, settings_.destination, layoutOf(session.getMasterFormat()), latency_))
        return error;

    if (settings_.includeStems)
    {
        for (const auto& t : project_.getSession().getAllTracks())
        {
            const int slot = project_.getCompiler().getSlot(track::getId(t));
            const auto destination = session.getDestination(t);
            const auto layout = layoutOf(destination.isValid() ? track::getFormat(destination) : session.getMasterFormat());
            // A stem is its track's output where it meets its destination (a folder or bus may be
            // aligned earlier than the master's input): skip exactly that track's delay.
            const int own = project_.getCompiler().getOutputLatency(track::getId(t));
            if (slot >= 0 && !add(slot, getStemFile(settings_, track::getName(t)), layout, own >= 0 ? own : stemLatency_))
                return error;
        }
    }

    if (targets_.empty())
        return "Nothing to export: choose the mix and/or stems.";
    longestLatency_ = 0;
    for (const auto& target : targets_)
        longestLatency_ = std::max(longestLatency_, target.latency);
    return {};
}

juce::String Exporter::start(const ExportSettings& settings)
{
    if (running_)
        return "An export is already running.";
    if (settings.endSeconds <= settings.startSeconds)
        return "The export range is empty.";

    settings_ = settings;
    auto& engine = controller_.getEngine();
    failed_ = false;
    error_.clear();
    progress_ = 0.0;
    speed_ = project_.getSession().getSpeed();

    // Make sure the engine renders the current state of the document.
    project_.getCompiler().rebuildNow();
    latency_ = project_.getCompiler().getTotalLatency();
    stemLatency_ = project_.getCompiler().getMasterInputLatency();
    graphSerial_ = controller_.getLatestGraphSerial();
    previousPosition_ = engine.collectTelemetry().playheadSeconds;
    controller_.stop();

    if (settings_.realtime)
    {
        if (!engine.isPrepared())
            return "Real-time export needs a running audio device.";

        sampleRate_ = engine.getSampleRate();
        if (auto error = prepareTargets(sampleRate_, true); error.isNotEmpty())
            return error;

        capture_ = std::make_unique<engine::CaptureSession>();
        capture_->onlyWhilePlaying = true;
        capture_->suspendLoop = true;
        for (auto& target : targets_)
        {
            if (target.slot < 0)
                capture_->master = target.stream;
            else
            {
                capture_->trackOutputs[static_cast<std::size_t>(target.slot)] = target.stream.get();
                capture_->ownedTrackStreams.push_back(target.stream);
            }
            diskWriter_.add(target.stream);
        }

        controller_.locateSeconds(settings_.startSeconds);
        controller_.startCapture(capture_.get());
        controller_.play();
        running_ = true;
        realtimeStopping_ = false;
        startTimerHz(20);
        return {};
    }

    // Offline: pause live processing and render on our own thread.
    if (!engine.isPrepared())
    {
        engine.prepare(48000.0, kOfflineBlock, 2, 0); // no device: render at 48 kHz
        preparedForExport_ = true;
    }
    sampleRate_ = engine.getSampleRate();

    if (!engine.beginOfflineRender())
    {
        if (preparedForExport_)
            engine.release();
        preparedForExport_ = false;
        return "The audio device did not respond; try again.";
    }

    if (auto error = prepareTargets(sampleRate_, false); error.isNotEmpty())
    {
        engine.endOfflineRender();
        if (preparedForExport_)
            engine.release();
        preparedForExport_ = false;
        return error;
    }

    project_.getDevices().setNonRealtime(true);
    running_ = true;
    startThread(juce::Thread::Priority::high);
    startTimerHz(30); // keeps the controller pumping graphs while the export thread waits for them
    return {};
}

void Exporter::cancel()
{
    if (!running_)
        return;

    if (settings_.realtime)
    {
        failed_ = true;
        error_ = "Export cancelled.";
        finishRealtime();
    }
    else
    {
        signalThreadShouldExit();
    }
}

void Exporter::run()
{
    auto& engine = controller_.getEngine();

    // Wait for the newest graph to be applied (the controller posts it from the message thread).
    const auto deadline = juce::Time::getMillisecondCounter() + 5000;
    while (engine.getAppliedGraphSerial() < graphSerial_ && juce::Time::getMillisecondCounter() < deadline && !threadShouldExit())
    {
        engine.applyCommandsOffline();
        wait(2);
    }

    engine.setOfflineTransport(settings_.startSeconds, true, true);

    const double timelineLength = settings_.endSeconds - settings_.startSeconds + settings_.tailSeconds;
    const auto outputFrames = static_cast<juce::int64>(std::ceil(timelineLength / speed_ * sampleRate_));
    const auto totalFrames = outputFrames + longestLatency_;
    const int block = std::min(kOfflineBlock, engine.getMaximumBlockSize());

    // The mix: as many outputs as the file has channels (the engine folds a surround master down
    // when there are fewer).
    int mixChannels = 2;
    for (const auto& target : targets_)
        if (target.slot < 0)
            mixChannels = target.channels;
    juce::AudioBuffer<float> mix(mixChannels, block);
    juce::AudioBuffer<float> stem(engine::kMaxChannels, block);

    for (juce::int64 rendered = 0; rendered < totalFrames && !threadShouldExit(); rendered += block)
    {
        const int n = static_cast<int>(std::min<juce::int64>(block, totalFrames - rendered));
        engine.renderOffline(mix.getArrayOfWritePointers(), mixChannels, n);

        for (auto& target : targets_)
        {
            // Skip the latency pre-roll so the file lines up with the timeline: the whole graph's for
            // the mix, the track's own output delay for a stem.
            const int latency = target.latency;
            const juce::int64 alreadyWritten = rendered - latency;
            const int skip = static_cast<int>(std::clamp<juce::int64>(latency - rendered, 0, n));
            const int count = static_cast<int>(std::min<juce::int64>(n - skip, outputFrames - std::max<juce::int64>(0, alreadyWritten)));

            if (count > 0)
            {
                if (target.slot < 0)
                {
                    std::array<const float*, engine::kMaxChannels> channels{};
                    for (int ch = 0; ch < mixChannels; ++ch)
                        channels[static_cast<std::size_t>(ch)] = mix.getReadPointer(ch, skip);
                    if (!target.writer->writeFromFloatArrays(channels.data(), mixChannels, count))
                        failed_ = true;
                }
                else
                {
                    // A stem is the track's output: its destination's channels (or their fold-down).
                    const auto& source = engine.getSlotOutput(target.slot);
                    const auto fold = engine::conversionMatrix(target.layout, engine::ChannelLayout::stereo());
                    for (int ch = 0; ch < target.channels; ++ch)
                        for (int i = 0; i < count; ++i)
                        {
                            double value = 0.0;
                            if (target.foldDown)
                            {
                                for (int in = 0; in < std::min(target.layout.numChannels, source.getNumChannels()); ++in)
                                    value += fold.gain[static_cast<std::size_t>(ch)][static_cast<std::size_t>(in)] * source.getSample(in, skip + i);
                            }
                            else if (ch < source.getNumChannels())
                            {
                                value = source.getSample(ch, skip + i);
                            }
                            stem.setSample(ch, i, static_cast<float>(value));
                        }
                    std::array<const float*, engine::kMaxChannels> channels{};
                    for (int ch = 0; ch < target.channels; ++ch)
                        channels[static_cast<std::size_t>(ch)] = stem.getReadPointer(ch);
                    if (!target.writer->writeFromFloatArrays(channels.data(), target.channels, count))
                        failed_ = true;
                }
            }
        }

        progress_.store(static_cast<double>(rendered + n) / static_cast<double>(totalFrames), std::memory_order_relaxed);
        if (failed_)
        {
            error_ = "Writing the audio file failed (is the disk full?).";
            break;
        }
    }

    if (threadShouldExit() && !failed_)
    {
        failed_ = true;
        error_ = "Export cancelled.";
    }

    for (auto& target : targets_)
        target.writer.reset(); // flushes and closes the file

    engine.setOfflineTransport(previousPosition_, false, false);
    engine.applyCommandsOffline();

    juce::MessageManager::callAsync([this, alive = std::weak_ptr<int>(alive_)]
                                    {
                                        if (!alive.expired())
                                            finishOffline();
                                    });
}

void Exporter::finishOffline()
{
    waitForThreadToExit(10'000);
    stopTimer();
    project_.getDevices().setNonRealtime(false);
    auto& engine = controller_.getEngine();
    engine.endOfflineRender();
    if (preparedForExport_)
        engine.release();
    preparedForExport_ = false;
    complete(failed_ ? juce::Result::fail(error_) : juce::Result::ok());
}

void Exporter::timerCallback()
{
    if (!settings_.realtime)
    {
        controller_.pump();
        return;
    }

    auto& engine = controller_.getEngine();
    if (!realtimeStopping_)
    {
        const double position = engine.collectTelemetry().playheadSeconds;
        const double end = settings_.endSeconds + settings_.tailSeconds + longestLatency_ / sampleRate_ * speed_;
        progress_ = juce::jlimit(0.0, 1.0, (position - settings_.startSeconds) / (end - settings_.startSeconds));
        if (position >= end)
            finishRealtime();
        return;
    }

    if (engine.getCaptureStopCount() != stopCountBefore_)
    {
        stopTimer();
        for (auto& target : targets_)
            diskWriter_.remove(target.stream); // drains and closes
        for (auto& target : targets_)
            if (target.stream->getOverflowCount() > 0)
            {
                failed_ = true;
                error_ = "The disk could not keep up; some audio was lost.";
            }
        capture_.reset();
        complete(failed_ ? juce::Result::fail(error_) : juce::Result::ok());
    }
}

void Exporter::finishRealtime()
{
    if (realtimeStopping_)
        return;
    realtimeStopping_ = true;
    stopCountBefore_ = controller_.getEngine().getCaptureStopCount();
    controller_.stopCapture();
    controller_.stop();
    controller_.locateSeconds(previousPosition_);
}

void Exporter::complete(const juce::Result& result)
{
    juce::Array<juce::File> files;
    for (auto& target : targets_)
    {
        if (result.failed())
            target.file.deleteFile();
        else
            files.add(target.file);
    }
    targets_.clear();
    running_ = false;
    progress_ = result.wasOk() ? 1.0 : 0.0;

    if (onFinished)
        onFinished(result, files);
}

} // namespace dojo::session
