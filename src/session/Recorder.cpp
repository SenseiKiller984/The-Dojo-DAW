#include "session/Recorder.h"

#include "engine/AudioEngine.h"

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

    auto capture = std::make_unique<engine::CaptureSession>();
    capture->onlyWhilePlaying = true;
    capture->suspendLoop = true;
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

void Recorder::finish()
{
    auto& session = project_.getSession();
    const double captureStart = controller_.getEngine().getCaptureStartSeconds();
    const bool captured = captureStart >= 0.0;
    const double sampleRate = controller_.getEngine().getSampleRate();

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

        // The take covers [start, start + length) of the timeline; keep what lies in the punch window.
        const double start = captureStart - audioLatency_;
        const double keepFrom = std::max({start, 0.0, punchStart_});
        const double keepTo = std::min(start + length, punchEnd_);

        if (keepTo - keepFrom > kMinClipLength)
        {
            project_.getAssets().request(take.file, {});
            session.clearRange(t, keepFrom, keepTo); // the take replaces what was there
            session.addClip(t, take.file, keepFrom, keepTo - keepFrom, keepFrom - start);
        }
        else
        {
            take.stream.reset();
            take.file.deleteFile(); // nothing of it is inside the punch window
        }
    }

    if (captured && capture_ != nullptr && capture_->captureMidi)
        createMidiClips(captureStart, std::max(stopPosition_, captureStart + kMinClipLength));

    takes_.clear();
    midiTracks_.clear();
    capture_.reset();
    state_ = State::idle;

    if (onStateChanged)
        onStateChanged();
}

void Recorder::createMidiClips(double captureStart, double stopPosition)
{
    auto& session = project_.getSession();
    const int count = std::min(capture_->midiCount.load(std::memory_order_acquire),
                               static_cast<int>(capture_->midiEvents.size()));
    const double beatsPerSecond = session.getTempo() / 60.0;
    const double clipStart = std::max(0.0, captureStart - midiLatency_);
    const double clipEnd = std::max(clipStart + kMinClipLength, stopPosition - midiLatency_);

    // Pair note-ons with note-offs; notes still held at the end end with the clip.
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
    std::map<int, Pending> held; // key: channel * 128 + note
    std::vector<Note> notes;
    std::vector<Control> controls;
    const auto beatOf = [&](double seconds) { return std::max(0.0, (seconds - midiLatency_ - clipStart) * beatsPerSecond); };

    for (int i = 0; i < count; ++i)
    {
        const auto& e = capture_->midiEvents[static_cast<std::size_t>(i)];
        const auto message = juce::MidiMessage(e.data, e.size);
        const int channel = message.getChannel();
        const double beat = beatOf(e.seconds);

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

    const double endBeat = (clipEnd - clipStart) * beatsPerSecond;
    for (const auto& [key, pending] : held)
        notes.push_back({pending.startBeat, std::max(endBeat, pending.startBeat + kMinNoteLength), key % 128, key / 128,
                         pending.velocity});

    // Keep the punch window: notes that start in it (ending by its end) and controllers in it.
    const double keepFrom = std::max(clipStart, punchStart_);
    const double keepTo = std::min(clipEnd, punchEnd_);
    if (keepTo - keepFrom <= kMinClipLength)
        return;
    const double fromBeat = (keepFrom - clipStart) * beatsPerSecond;
    const double toBeat = (keepTo - clipStart) * beatsPerSecond;
    std::vector<Note> keptNotes;
    for (const auto& n : notes)
        if (n.startBeat >= fromBeat && n.startBeat < toBeat)
            keptNotes.push_back({n.startBeat - fromBeat, std::min(n.endBeat, toBeat) - fromBeat, n.note, n.channel, n.velocity});
    std::vector<Control> keptControls;
    for (const auto& c : controls)
        if (c.beat >= fromBeat && c.beat < toBeat)
            keptControls.push_back({c.beat - fromBeat, c.controller, c.channel, c.value});
    notes = std::move(keptNotes);
    controls = std::move(keptControls);

    if (notes.empty() && controls.empty())
        return;

    for (const auto id : midiTracks_)
    {
        auto t = session.findTrack(id);
        if (!t.isValid())
            continue;

        session.clearRange(t, keepFrom, keepTo); // the take replaces what was there
        auto clipTree = session.addMidiClip(t, keepFrom, keepTo - keepFrom, track::getName(t) + " take");
        for (const auto& n : notes)
            session.addNote(clipTree, n.startBeat, n.endBeat - n.startBeat, n.note, n.velocity, n.channel);
        for (const auto& c : controls)
            session.addControl(clipTree, c.controller, c.beat, c.value, c.channel);
    }
}

} // namespace dojo::session
