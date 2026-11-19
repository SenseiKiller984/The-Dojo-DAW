#include "session/Exporter.h"

#include "engine/AudioEngine.h"
#include "engine/io/AdmMetadata.h"
#include "engine/io/Bw64Writer.h"
#include "engine/io/LameMp3Format.h"
#include "session/GraphCompiler.h"

#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>

namespace dojo::session
{
namespace
{
constexpr int kOfflineBlock = 512;

/**
    The ADM export's writer thread: the render thread fills blocks of audio (one channel per file
    channel) and queues them; this thread interleaves, encodes and writes them, so rendering and
    disk writing overlap. A fixed pool of blocks bounds the memory (the render thread waits when
    every block is queued).
*/
class BlockWriter final : private juce::Thread
{
public:
    struct Block
    {
        std::vector<std::vector<float>> channels;
        int frames = 0;
    };

    BlockWriter(engine::adm::Bw64Writer& file, int numChannels, int blockFrames)
        : juce::Thread("Dojo ADM writer")
        , file_(file)
    {
        for (int b = 0; b < kBlocks; ++b)
        {
            auto block = std::make_unique<Block>();
            block->channels.assign(static_cast<std::size_t>(numChannels), std::vector<float>(static_cast<std::size_t>(blockFrames), 0.0f));
            free_.push_back(block.get());
            blocks_.push_back(std::move(block));
        }
        startThread(juce::Thread::Priority::normal);
    }

    ~BlockWriter() override
    {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        changed_.notify_all();
        stopThread(10'000);
    }

    /** A block to fill (waits while every block is queued); null once writing has failed. */
    Block* acquire()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [this] { return failed_ || !free_.empty(); });
        if (failed_)
            return nullptr;
        auto* block = free_.front();
        free_.pop_front();
        return block;
    }

    void submit(Block* block)
    {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            queued_.push_back(block);
        }
        changed_.notify_all();
    }

    /** Waits for everything queued to be written. False if writing failed. */
    bool drain()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [this] { return failed_ || (queued_.empty() && !busy_); });
        return !failed_;
    }

private:
    void run() override
    {
        for (;;)
        {
            Block* block = nullptr;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                changed_.wait(lock, [this] { return stopping_ || !queued_.empty(); });
                if (queued_.empty())
                    return; // (stopping, nothing left)
                block = queued_.front();
                queued_.pop_front();
                busy_ = true;
            }
            std::vector<const float*> pointers;
            for (const auto& c : block->channels)
                pointers.push_back(c.data());
            const bool ok = file_.write(pointers.data(), block->frames);
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                busy_ = false;
                failed_ = failed_ || !ok;
                free_.push_back(block);
            }
            changed_.notify_all();
        }
    }

    static constexpr int kBlocks = 32;
    engine::adm::Bw64Writer& file_;
    std::vector<std::unique_ptr<Block>> blocks_;
    std::deque<Block*> free_, queued_;
    std::mutex mutex_;
    std::condition_variable changed_;
    bool busy_ = false;
    bool failed_ = false;
    bool stopping_ = false;
};
} // namespace

/** An ADM export under way: the file, its bed and objects, and the objects' positions block by block. */
struct Exporter::AdmExport
{
    struct Position
    {
        juce::int64 renderIndex = 0; ///< where the block started, in rendered samples
        int length = 0;
        std::array<double, 4> value{}; ///< azimuth, elevation, distance, size
    };
    struct ObjectTrack
    {
        engine::RenderObject object;
        juce::String name;
        std::array<double, 4> own{}; ///< its settings (where nothing moves it)
        std::vector<Position> positions;
        int firstFileTrack = 1;
    };

    engine::adm::Programme programme; ///< (its objects' blocks filled in at the end)
    std::vector<ObjectTrack> objects;
    int numChannels = 0;
    std::unique_ptr<engine::adm::Bw64Writer> file;
};

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
    case ExportSettings::Format::wav:
    case ExportSettings::Format::admBwf: break;
    }
    return ".wav";
}

juce::File Exporter::getStemFile(const ExportSettings& settings, const juce::String& trackName)
{
    const auto base = settings.destination.getFileNameWithoutExtension();
    return settings.destination.getParentDirectory().getChildFile(
        juce::File::createLegalFileName(base + " - " + trackName) + getFileExtension(settings.format));
}

juce::AudioChannelSet Exporter::wavChannelSet(const engine::ChannelLayout& layout)
{
    using Set = juce::AudioChannelSet;
    if (layout == engine::ChannelLayout::stereo())
        return Set::stereo();
    if (layout.numChannels == 1)
        return Set::mono();
    // Only where the WAV standard order (its channel mask's bit order) is the format's own.
    if (layout == engine::ChannelLayout::lcrLfe())
    {
        Set set;
        for (const auto type : {Set::left, Set::right, Set::centre, Set::LFE})
            set.addChannel(type);
        return set;
    }
    if (layout == engine::ChannelLayout::surround51())
        return Set::create5point1();
    if (layout == engine::ChannelLayout::surround514())
        return Set::create5point1point4();
    return Set::discreteChannels(layout.numChannels);
}

std::unique_ptr<juce::AudioFormatWriter> Exporter::createWriter(const juce::File& file, const ExportSettings& settings,
                                                                double sampleRate, int numChannels, juce::String& error,
                                                                const engine::ChannelLayout* layout)
{
    if (settings.format == ExportSettings::Format::flac && numChannels > 8)
    {
        error = "FLAC holds up to 8 channels. Export " + juce::String(numChannels) + "-channel (3D) mixes and stems as WAV.";
        return nullptr;
    }

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
        if (layout != nullptr && layout->numChannels == numChannels && numChannels > 2)
            options = options.withChannelLayout(wavChannelSet(*layout));
        else if (numChannels > 2)
            options = options.withChannelLayout(juce::AudioChannelSet::discreteChannels(numChannels));
        writer = wav.createWriterFor(stream, options);
        break;
    }
    case ExportSettings::Format::flac:
    {
        juce::FlacAudioFormat flac;
        writer = flac.createWriterFor(stream, options.withBitsPerSample(settings.bitDepth <= 16 ? 16 : 24));
        break;
    }
    case ExportSettings::Format::admBwf:
        error = "ADM BWF files are written by the exporter's own BW64 writer.";
        return nullptr;
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
        auto writer = createWriter(file, settings_, sampleRate, target.channels, error, &target.layout);
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

juce::String Exporter::prepareAdm(double sampleRate)
{
    targets_.clear();
    adm_ = std::make_unique<AdmExport>();
    auto& a = *adm_;
    const auto& session = project_.getSession();

    // The bed: the master's channels (everything that is not an object).
    auto& programme = a.programme;
    programme.name = project_.getDisplayName();
    programme.sampleRate = sampleRate;
    programme.bitDepth = settings_.bitDepth >= 32 ? 32 : (settings_.bitDepth <= 16 ? 16 : 24);
    const auto master = session.getMasterFormat();
    programme.bed = isMultichannel(master) ? getLayout(master) : engine::ChannelLayout::stereo();
    for (int c = 0; c < programme.bed.numChannels; ++c)
        programme.bedTracks.push_back(c + 1);
    int next = programme.bed.numChannels + 1;

    // The objects: each object track's own channel(s), after the bed's.
    for (const auto& object : project_.getCompiler().getObjects())
    {
        const auto t = session.findTrack(object.trackId);
        if (!t.isValid())
            continue;
        AdmExport::ObjectTrack o;
        o.object = object;
        o.name = track::getName(t);
        o.own = {track::getAzimuth(t), track::getElevation(t), track::getDistance(t), track::getSize(t)};
        o.firstFileTrack = next;
        engine::adm::ObjectGroup group;
        group.name = o.name;
        if (object.numChannels == 1)
            group.objects.push_back({o.name, next++, {}});
        else
        {
            group.objects.push_back({o.name + " L", next++, {}});
            group.objects.push_back({o.name + " R", next++, {}});
        }
        programme.groups.push_back(std::move(group));
        a.objects.push_back(std::move(o));
    }
    a.numChannels = next - 1;
    if (a.numChannels > 128)
        return "An ADM BWF file holds up to 128 channels; this one would need " + juce::String(a.numChannels) + ".";

    const auto format = programme.bitDepth == 32   ? engine::adm::Bw64Writer::Format::float32
                        : programme.bitDepth == 16 ? engine::adm::Bw64Writer::Format::pcm16
                                                   : engine::adm::Bw64Writer::Format::pcm24;
    a.file = std::make_unique<engine::adm::Bw64Writer>(settings_.destination, a.numChannels, sampleRate, format,
                                                       engine::adm::trackEntries(programme));
    if (!a.file->isOpen())
        return a.file->getError();

    Target target; // (listed, so that a failed export removes the file)
    target.file = settings_.destination;
    target.channels = a.numChannels;
    target.latency = latency_;
    targets_.push_back(std::move(target));
    longestLatency_ = latency_; // (the objects leave with the bed)
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

    // ADM BWF: offline, with the object tracks rendered apart (each its own channels).
    const bool adm = settings_.format == ExportSettings::Format::admBwf;
    if (adm)
    {
        settings_.realtime = false;
        settings_.includeMix = true;
        settings_.includeStems = false;
        objectRenderingBefore_ = static_cast<int>(project_.getCompiler().getObjectRendering());
        project_.getCompiler().setObjectRendering(GraphBuilder::ObjectRendering::apart);
    }
    const auto putObjectsBack = [this, adm]
    {
        if (adm)
            project_.getCompiler().setObjectRendering(static_cast<GraphBuilder::ObjectRendering>(objectRenderingBefore_));
    };

    // Makes sure the engine renders the current state of the document, and measures its latency.
    const auto compile = [this]
    {
        project_.getCompiler().rebuildNow();
        latency_ = project_.getCompiler().getTotalLatency();
        stemLatency_ = project_.getCompiler().getMasterInputLatency();
        graphSerial_ = controller_.getLatestGraphSerial();
    };
    compile();
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
        putObjectsBack();
        return "The audio device did not respond; try again.";
    }

    // Devices render in their offline mode (some oversample more): their latency can change, so the
    // graph is compiled again and measured with it before the files are set up.
    project_.getDevices().setNonRealtime(true);
    compile();

    if (auto error = adm ? prepareAdm(sampleRate_) : prepareTargets(sampleRate_, false); error.isNotEmpty())
    {
        project_.getDevices().setNonRealtime(false);
        engine.endOfflineRender();
        if (preparedForExport_)
            engine.release();
        preparedForExport_ = false;
        if (adm)
            settings_.destination.deleteFile();
        adm_.reset();
        targets_.clear();
        putObjectsBack();
        return error;
    }

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

    if (adm_ != nullptr)
    {
        runAdm();
        return;
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

void Exporter::runAdm()
{
    auto& engine = controller_.getEngine();
    auto& a = *adm_;
    engine.setOfflineTransport(settings_.startSeconds, true, true);

    const double timelineLength = settings_.endSeconds - settings_.startSeconds + settings_.tailSeconds;
    const auto outputFrames = static_cast<juce::int64>(std::ceil(timelineLength / speed_ * sampleRate_));
    const auto totalFrames = outputFrames + latency_; // (the objects leave aligned with the bed)
    const int block = std::min(kOfflineBlock, engine.getMaximumBlockSize());
    const int bedChannels = a.programme.bed.numChannels;

    juce::AudioBuffer<float> bed(bedChannels, block);
    {
        BlockWriter writer(*a.file, a.numChannels, block);
        for (juce::int64 rendered = 0; rendered < totalFrames && !threadShouldExit(); rendered += block)
        {
            const int n = static_cast<int>(std::min<juce::int64>(block, totalFrames - rendered));
            engine.renderOffline(bed.getArrayOfWritePointers(), bedChannels, n);

            // Where each object was this block (its own settings where nothing moves it).
            for (auto& o : a.objects)
            {
                AdmExport::Position p;
                p.renderIndex = rendered;
                p.length = n;
                for (int k = 0; k < 4; ++k)
                {
                    const float moving = engine.getMovingSpatial(o.object.slot, k);
                    p.value[static_cast<std::size_t>(k)] = std::isnan(moving) ? o.own[static_cast<std::size_t>(k)] : static_cast<double>(moving);
                }
                o.positions.push_back(p);
            }

            // The audio, without the latency's pre-roll: the bed, then each object's channels.
            const int skip = static_cast<int>(std::clamp<juce::int64>(latency_ - rendered, 0, n));
            const juce::int64 alreadyWritten = std::max<juce::int64>(0, rendered - latency_);
            const int count = static_cast<int>(std::min<juce::int64>(n - skip, outputFrames - alreadyWritten));
            if (count > 0)
            {
                auto* out = writer.acquire();
                if (out == nullptr)
                {
                    failed_ = true;
                    break;
                }
                out->frames = count;
                for (int c = 0; c < bedChannels; ++c)
                    std::copy_n(bed.getReadPointer(c, skip), count, out->channels[static_cast<std::size_t>(c)].data());
                int channel = bedChannels;
                for (const auto& o : a.objects)
                {
                    const auto& source = engine.getSlotOutput(o.object.slot);
                    for (int c = 0; c < o.object.numChannels; ++c)
                    {
                        auto& destination = out->channels[static_cast<std::size_t>(channel++)];
                        if (c < source.getNumChannels())
                            for (int i = 0; i < count; ++i)
                                destination[static_cast<std::size_t>(i)] = static_cast<float>(source.getSample(c, skip + i));
                        else
                            std::fill_n(destination.begin(), count, 0.0f);
                    }
                }
                writer.submit(out);
            }
            progress_.store(0.98 * static_cast<double>(rendered + n) / static_cast<double>(totalFrames), std::memory_order_relaxed);
        }
        if (!writer.drain())
            failed_ = true;
    }

    // The metadata: each object's blocks, where its sound is in the file (the position worked out
    // with a block belongs to the sound at the fader then: that much earlier than the output).
    if (!failed_ && !threadShouldExit())
    {
        std::size_t group = 0;
        for (auto& o : a.objects)
        {
            auto& objects = a.programme.groups[group++].objects;
            for (std::size_t side = 0; side < objects.size(); ++side)
            {
                const double offset = objects.size() == 2 ? (side == 0 ? 30.0 : -30.0) : 0.0; // (a stereo track's sides)
                std::vector<engine::adm::ObjectBlock> blocks;
                for (std::size_t k = 0; k < o.positions.size(); ++k)
                {
                    const auto& p = o.positions[k];
                    const auto start = std::max<juce::int64>(0, p.renderIndex - o.object.faderLatency);
                    const auto end = std::min<juce::int64>(outputFrames, p.renderIndex + p.length - o.object.faderLatency);
                    if (end <= start)
                        continue;
                    // A block says where the object has got to by its end: the position the next
                    // block was worked out at (the renderer moves it there across the block).
                    const auto& reached = o.positions[std::min(k + 1, o.positions.size() - 1)].value;
                    engine::adm::ObjectBlock b;
                    b.start = blocks.empty() ? 0 : start;
                    b.length = end - b.start;
                    b.position = engine::adm::polarToCartesian(reached[0] + offset, reached[1], std::clamp(reached[2], 0.0, 1.0));
                    b.size = std::clamp(reached[3], 0.0, 1.0);
                    blocks.push_back(b);
                }
                if (!blocks.empty())
                    blocks.back().length = outputFrames - blocks.back().start; // (to the end of the file)
                objects[side].blocks = engine::adm::simplify(blocks, 1.0e-4);
            }
        }
        a.programme.length = outputFrames;
        if (!a.file->finish(engine::adm::documentFor(a.programme)))
            failed_ = true;
    }
    if (failed_ && error_.isEmpty())
        error_ = a.file->getError().isNotEmpty() ? a.file->getError() : juce::String("Writing the audio file failed (is the disk full?).");
    if (threadShouldExit() && !failed_)
    {
        failed_ = true;
        error_ = "Export cancelled.";
    }
    a.file.reset(); // (closes it)
    progress_.store(1.0, std::memory_order_relaxed);

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
    if (adm_ != nullptr)
    {
        adm_.reset();
        project_.getCompiler().setObjectRendering(static_cast<GraphBuilder::ObjectRendering>(objectRenderingBefore_));
    }
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
