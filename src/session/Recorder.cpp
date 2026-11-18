#include "session/Recorder.h"

#include "engine/AudioEngine.h"

#include <algorithm>
#include <map>
#include <tuple>

namespace dojo::session
{
namespace
{
constexpr int kMaxMidiEvents = 65536;
constexpr juce::uint32 kStopTimeoutMs = 3000;
} // namespace

Recorder::Recorder(Project& project, engine::EngineController& controller)
    : project_(project)
    , controller_(controller)
{
}

Recorder::~Recorder()
{
    stopTimer();
    if (state_ == State::recording)
    {
        controller_.stopCapture();
        controller_.stop();
    }
    // Unacknowledged sessions may still be referenced by an engine that outlives us only in theory
    // (the application stops the device before destroying the project); keep them until the end.
}

juce::File Recorder::getRecordingsFolder() const
{
    const auto file = project_.getFile();
    if (file != juce::File())
        return file.getParentDirectory().getChildFile(file.getFileNameWithoutExtension() + " Recordings");
    return juce::File::getSpecialLocation(juce::File::userMusicDirectory).getChildFile("The Dojo").getChildFile("Recordings");
}

double Recorder::getAudioLatencySeconds() const
{
    const auto status = controller_.getDeviceStatus();
    const double rate = status.sampleRate > 0.0 ? status.sampleRate : 48000.0;
    const int samples = status.inputLatencySamples + status.outputLatencySamples + project_.getCompiler().getTotalLatency();
    return samples / rate * project_.getSession().getSpeed();
}

double Recorder::getMidiLatencySeconds() const
{
    const auto status = controller_.getDeviceStatus();
    const double rate = status.sampleRate > 0.0 ? status.sampleRate : 48000.0;
    const int samples = status.outputLatencySamples + project_.getCompiler().getTotalLatency();
    return samples / rate * project_.getSession().getSpeed();
}

juce::String Recorder::start()
{
    if (state_ != State::idle)
        return "Recording is already running.";

    auto& engine = controller_.getEngine();
    if (!engine.isPrepared())
        return "No audio device is running. Choose one in Options > Audio Settings.";

    auto& session = project_.getSession();
    const double sampleRate = engine.getSampleRate();
    const int numInputs = engine.getNumInputChannels();

    // Loop Record cycles the loop (not with punch in or out: they take the loop range as theirs).
    const bool loopRecording = session.isLoopRecording() && session.isLoopEnabled() && !session.isPunchInEnabled()
                               && !session.isPunchOutEnabled() && session.getLoopEnd() > session.getLoopStart() + kMinClipLength;

    auto capture = std::make_unique<engine::CaptureSession>();
    capture->onlyWhilePlaying = true;
    capture->suspendLoop = !loopRecording;
    std::vector<AudioTake> takes;
    std::vector<std::uint32_t> midiTracks;
    juce::StringArray problems;

    const auto folder = getRecordingsFolder();
    const auto stamp = juce::Time::getCurrentTime().formatted("%Y-%m-%d %H%M%S");

    for (const auto& t : session.getAllTracks())
    {
        if (!track::isArmed(t) || !Session::isEffectivelyActive(t))
            continue;

        if (holdsMidi(track::getKind(t)))
        {
            midiTracks.push_back(track::getId(t));
            continue;
        }

        if (track::getKind(t) != TrackKind::audio)
            continue;

        const int input = track::getInput(t);
        if (input < 0 || input >= numInputs)
        {
            problems.add(track::getName(t) + ": its input is not available on this audio device.");
            continue;
        }

        if (!folder.createDirectory())
            return "Could not create the recordings folder " + folder.getFullPathName();

        // Mono: one input; stereo: a pair; multichannel: one input per channel, consecutive.
        const auto format = track::getFormat(t);
        const bool stereo = track::isInputStereo(t) && input + 1 < numInputs;
        const int channels = isMultichannel(format) ? getNumChannels(format) : (stereo ? 2 : 1);
        if (input + channels > numInputs)
        {
            problems.add(track::getName(t) + ": a " + getDisplayName(format) + " track records " + juce::String(channels)
                         + " inputs (In " + juce::String(input + 1) + "-" + juce::String(input + channels) + "); this audio device has "
                         + juce::String(numInputs) + ".");
            continue;
        }
        const auto name = juce::File::createLegalFileName(track::getName(t) + " " + stamp);
        const auto file = folder.getNonexistentChildFile(name, ".wav", false);

        std::unique_ptr<juce::OutputStream> out = std::make_unique<juce::FileOutputStream>(file);
        juce::WavAudioFormat wav;
        auto writer = wav.createWriterFor(out, juce::AudioFormatWriterOptions{}
                                                   .withSampleRate(sampleRate)
                                                   .withNumChannels(channels)
                                                   .withBitsPerSample(24));
        if (writer == nullptr)
        {
            problems.add("Could not create " + file.getFullPathName());
            continue;
        }

        AudioTake take;
        take.trackId = track::getId(t);
        take.file = file;
        take.stream = std::make_shared<engine::CaptureStream>(std::move(writer), channels, sampleRate, 10.0);
        capture->inputs.push_back({input, channels, take.stream});
        takes.push_back(std::move(take));
    }

    if (takes.empty() && midiTracks.empty())
        return problems.isEmpty() ? juce::String("Arm a track (its R button) to record onto it.") : problems.joinIntoString("\n");

    if (!midiTracks.empty())
    {
        capture->captureMidi = true;
        capture->midiEvents.resize(kMaxMidiEvents);
    }

    for (auto& take : takes)
        diskWriter_.add(take.stream);

    audioLatency_ = getAudioLatencySeconds();
    midiLatency_ = getMidiLatencySeconds();
    std::tie(punchStart_, punchEnd_) = session.getPunchWindow();
    loopRecording_ = loopRecording;
    loopStart_ = session.getLoopStart();
    loopEnd_ = session.getLoopEnd();
    stopPasses_ = 0;
    capture_ = std::move(capture);
    takes_ = std::move(takes);
    midiTracks_ = std::move(midiTracks);
    state_ = State::recording;

    controller_.startCapture(capture_.get());
    controller_.play();

    if (onStateChanged)
        onStateChanged();

    return problems.joinIntoString("\n");
}

void Recorder::stop()
{
    if (state_ != State::recording)
        return;

    stopPosition_ = controller_.getEngine().collectTelemetry().playheadSeconds;
    stopPasses_ = capture_ != nullptr ? capture_->loopPasses.load(std::memory_order_acquire) : 0;
    stopCountBefore_ = controller_.getEngine().getCaptureStopCount();
    controller_.stopCapture();
    controller_.stop();
    stopRequestedAt_ = juce::Time::getMillisecondCounter();
    state_ = State::finishing;
    startTimer(10);

    if (onStateChanged)
        onStateChanged();
}

void Recorder::timerCallback()
{
    const bool acknowledged = controller_.getEngine().getCaptureStopCount() != stopCountBefore_;
    const bool timedOut = juce::Time::getMillisecondCounter() - stopRequestedAt_ > kStopTimeoutMs;
    if (acknowledged || timedOut)
    {
        stopTimer();
        if (!acknowledged)
            abandoned_.push_back(std::move(capture_)); // the audio thread may still look at it later
        finish();
    }
}

std::vector<Recorder::Pass> Recorder::audioPassesOf(double captureStart, double start, double length, int loopPasses) const
{
    // The whole take: the comp keeps what lies in the punch window, its take lane all of it.
    const double laneFrom = std::max(0.0, start);
    if (!loopRecording_ || loopPasses == 0)
    {
        Pass pass;
        pass.from = std::max(laneFrom, punchStart_);
        pass.to = std::min(start + length, punchEnd_);
        pass.offset = pass.from - start;
        pass.laneFrom = laneFrom;
        pass.laneTo = start + length;
        pass.laneOffset = laneFrom - start;
        if (pass.to - pass.from <= kMinClipLength)
            return {};
        return {pass};
    }

    // Loop recording: the transport played on to the loop end, then the loop over and over. What
    // the performer played at time u of pass k reached the file `latency` after the transport
    // played u.
    std::vector<Pass> passes;
    const double latency = audioLatency_;
    const double loopLength = loopEnd_ - loopStart_;
    const double firstWrap = loopEnd_ - captureStart; // seconds into the capture
    const auto add = [&passes](double from, double to, double offset)
    {
        if (to - from > kMinClipLength)
            passes.push_back({from, to, offset, from, to, offset});
    };
    const double from = std::max({loopStart_, start, 0.0});
    add(from, std::min(loopEnd_, start + length), from - start);
    for (int k = 1; k <= std::min(loopPasses, stopPasses_); ++k) // (passes begun after Stop are not kept)
    {
        const double offset = firstWrap + (k - 1) * loopLength + latency;
        add(loopStart_, loopStart_ + std::min(loopLength, length - offset), offset);
    }
    // A last pass much shorter than the loop: Stop came just after the loop came round.
    if (passes.size() > 1 && passes.back().to - passes.back().from < 0.25 * loopLength)
        passes.pop_back();
    return passes;
}

bool Recorder::recordsToLanes(const juce::ValueTree& track, double from, double to, std::size_t passes) const
{
    if (passes > 1)
        return true; // (the passes of a loop recording are takes, whatever the setting)
    const auto& session = project_.getSession();
    if (!session.isRecordingTakesToLanes())
        return false;
    if (!Session::getTakeLanes(track).empty())
        return true;
    for (const auto& c : track)
        if ((c.hasType(ids::clip) || c.hasType(ids::midiClip)) && clip::getEnd(c) > from + 1.0e-9 && clip::getStart(c) < to - 1.0e-9)
            return true; // it would replace earlier material
    return false;
}

void Recorder::finish()
{
    auto& session = project_.getSession();
    const double captureStart = controller_.getEngine().getCaptureStartSeconds();
    const bool captured = captureStart >= 0.0;
    const double sampleRate = controller_.getEngine().getSampleRate();
    // How often the loop came round (the capture may have been abandoned to the audio thread).
    const auto* capture = capture_ != nullptr ? capture_.get() : (abandoned_.empty() ? nullptr : abandoned_.back().get());
    const int loopPasses = capture != nullptr ? capture->loopPasses.load(std::memory_order_acquire) : 0;

    for (auto& take : takes_)
    {
        diskWriter_.remove(take.stream); // drains everything that is left
        take.stream->finish();
    }

    if (captured)
        session.beginTransaction("Record");

    for (auto& take : takes_)
    {
        const double length = static_cast<double>(take.stream->getFramesWritten()) / sampleRate;
        auto t = session.findTrack(take.trackId);

        if (!captured || length <= kMinClipLength || !t.isValid())
        {
            take.stream.reset();
            take.file.deleteFile();
            continue;
        }

        // The take covers [start, start + length) of the timeline (one pass after another when
        // loop recording); the comp keeps what lies in the punch window.
        const double start = captureStart - audioLatency_;
        const auto passes = audioPassesOf(captureStart, start, length, loopPasses);
        if (passes.empty())
        {
            take.stream.reset();
            take.file.deleteFile(); // nothing of it is inside the punch window
            continue;
        }

        project_.getAssets().request(take.file, {});
        session.addWholeFileClip(take.file, length, false, start); // (time stamped where it was recorded)
        double from = passes.front().from, to = passes.front().to;
        for (const auto& pass : passes)
        {
            from = std::min(from, pass.from);
            to = std::max(to, pass.to);
        }

        if (recordsToLanes(t, from, to, passes.size()))
        {
            // Each take in a take lane of its own (earlier material too), the newest comped in.
            session.keepCompInTakeLane(t, from, to);
            for (const auto& pass : passes)
            {
                auto lane = session.addTakeLane(t);
                session.addClip(lane, take.file, pass.laneFrom, pass.laneTo - pass.laneFrom, pass.laneOffset);
                session.compTake(lane, pass.from, pass.to);
            }
        }
        else
        {
            for (const auto& pass : passes)
            {
                session.clearRange(t, pass.from, pass.to); // the take replaces what was there
                session.addClip(t, take.file, pass.from, pass.to - pass.from, pass.offset);
            }
        }
    }

    if (captured && capture_ != nullptr && capture_->captureMidi)
        createMidiClips(captureStart, std::max(stopPosition_, captureStart + kMinClipLength), loopPasses);

    takes_.clear();
    midiTracks_.clear();
    capture_.reset();
    state_ = State::idle;

    if (onStateChanged)
        onStateChanged();
}

void Recorder::createMidiClips(double captureStart, double stopPosition, int loopPasses)
{
    auto& session = project_.getSession();
    const int count = std::min(capture_->midiCount.load(std::memory_order_acquire),
                               static_cast<int>(capture_->midiEvents.size()));
    const double beatsPerSecond = session.getTempo() / 60.0;
    const double loopLength = loopEnd_ - loopStart_;
    const bool looped = loopRecording_ && loopPasses > 0;

    // The passes: what each one covers of the timeline (one, unless the loop came round).
    struct Range
    {
        double from = 0.0, to = 0.0;
    };
    std::vector<Range> passes;
    const double first = std::max(0.0, captureStart - midiLatency_);
    const double stop = stopPosition - midiLatency_;
    if (!looped)
        passes.push_back({first, std::max(first + kMinClipLength, stop)});
    else
    {
        // Every pass up to the one Stop came in, which ends there (the loop may have come round
        // once more before the capture stopped).
        for (int k = 0; k <= stopPasses_; ++k)
        {
            const double from = k == 0 ? std::max(first, loopStart_) : loopStart_;
            passes.push_back({from, k < stopPasses_ ? loopEnd_ : std::clamp(stop, from, loopEnd_)});
        }
        if (passes.size() > 1 && passes.back().to - passes.back().from < 0.25 * loopLength)
            passes.pop_back(); // Stop came just after the loop came round
    }

    // Pair note-ons with note-offs, pass by pass; notes still held at a pass's end end with it.
    struct Pending
    {
        double startBeat;
        float velocity;
    };
    struct Note
    {
        double startBeat, endBeat;
        int note, channel;
        float velocity;
    };
    struct Control
    {
        double beat;
        int controller, channel;
        float value;
    };
    struct Take
    {
        double from = 0.0, to = 0.0; ///< (the punch window's part of the pass)
        std::vector<Note> notes;
        std::vector<Control> controls;
    };
    std::vector<Take> recorded;

    for (std::size_t p = 0; p < passes.size(); ++p)
    {
        const auto range = passes[p];
        std::map<int, Pending> held; // key: channel * 128 + note
        std::vector<Note> notes;
        std::vector<Control> controls;
        const auto beatOf = [&](double seconds) { return std::max(0.0, (seconds - range.from) * beatsPerSecond); };

        for (int i = 0; i < count; ++i)
        {
            const auto& e = capture_->midiEvents[static_cast<std::size_t>(i)];
            // When it was played: an event just after the loop came round was played (heard
            // late) at the end of the pass before.
            double seconds = e.seconds - midiLatency_;
            int pass = looped ? e.pass : 0;
            if (looped && pass > 0 && seconds < loopStart_ - 1.0e-9)
            {
                --pass;
                seconds += loopLength;
            }
            if (pass != static_cast<int>(p) || seconds < range.from - 1.0e-9 || seconds >= range.to)
                continue;

            const auto message = juce::MidiMessage(e.data, e.size);
            const int channel = message.getChannel();
            const double beat = beatOf(seconds);

            if (message.isNoteOn())
            {
                held[channel * 128 + message.getNoteNumber()] = {beat, message.getFloatVelocity()};
            }
            else if (message.isNoteOff())
            {
                const auto key = channel * 128 + message.getNoteNumber();
                if (const auto it = held.find(key); it != held.end())
                {
                    notes.push_back({it->second.startBeat, std::max(beat, it->second.startBeat + kMinNoteLength),
                                     message.getNoteNumber(), channel, it->second.velocity});
                    held.erase(it);
                }
            }
            else if (message.isController())
            {
                controls.push_back({beat, message.getControllerNumber(), channel, static_cast<float>(message.getControllerValue()) / 127.0f});
            }
            else if (message.isPitchWheel())
            {
                controls.push_back({beat, 128, channel, static_cast<float>(message.getPitchWheelValue()) / 16383.0f});
            }
        }

        const double endBeat = (range.to - range.from) * beatsPerSecond;
        for (const auto& [key, pending] : held)
            notes.push_back({pending.startBeat, std::max(endBeat, pending.startBeat + kMinNoteLength), key % 128, key / 128,
                             pending.velocity});

        // Keep the punch window: notes that start in it (ending by its end) and controllers in it.
        Take take;
        take.from = std::max(range.from, punchStart_);
        take.to = std::min(range.to, punchEnd_);
        if (take.to - take.from <= kMinClipLength)
            continue;
        const double fromBeat = (take.from - range.from) * beatsPerSecond;
        const double toBeat = (take.to - range.from) * beatsPerSecond;
        for (const auto& n : notes)
            if (n.startBeat >= fromBeat && n.startBeat < toBeat)
                take.notes.push_back({n.startBeat - fromBeat, std::min(n.endBeat, toBeat) - fromBeat, n.note, n.channel, n.velocity});
        for (const auto& c : controls)
            if (c.beat >= fromBeat && c.beat < toBeat)
                take.controls.push_back({c.beat - fromBeat, c.controller, c.channel, c.value});
        if (!take.notes.empty() || !take.controls.empty()) // (a pass with nothing played leaves no take)
            recorded.push_back(std::move(take));
    }

    if (recorded.empty())
        return;
    double from = recorded.front().from, to = recorded.front().to;
    for (const auto& take : recorded)
    {
        from = std::min(from, take.from);
        to = std::max(to, take.to);
    }

    for (const auto id : midiTracks_)
    {
        auto t = session.findTrack(id);
        if (!t.isValid())
            continue;

        const auto fill = [&session](juce::ValueTree clipTree, const Take& take)
        {
            for (const auto& n : take.notes)
                session.addNote(clipTree, n.startBeat, n.endBeat - n.startBeat, n.note, n.velocity, n.channel);
            for (const auto& c : take.controls)
                session.addControl(clipTree, c.controller, c.beat, c.value, c.channel);
        };

        if (recordsToLanes(t, from, to, passes.size()))
        {
            // Each take in a take lane of its own (earlier material too), the newest comped in.
            session.keepCompInTakeLane(t, from, to);
            for (const auto& take : recorded)
            {
                auto lane = session.addTakeLane(t);
                fill(session.addMidiClip(lane, take.from, take.to - take.from, track::getName(t) + " take"), take);
                session.compTake(lane, take.from, take.to);
            }
        }
        else
        {
            for (const auto& take : recorded)
            {
                session.clearRange(t, take.from, take.to); // the take replaces what was there
                fill(session.addMidiClip(t, take.from, take.to - take.from, track::getName(t) + " take"), take);
            }
        }
    }
}

} // namespace dojo::session
