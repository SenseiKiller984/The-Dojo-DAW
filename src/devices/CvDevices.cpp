#include "devices/CvDevices.h"

#include "devices/HardwareInsert.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace dojo::devices
{
namespace cv
{
const juce::StringArray& channelChoices()
{
    static const juce::StringArray choices = []
    {
        juce::StringArray names{"None"};
        for (int channel = 1; channel <= 64; ++channel)
            names.add(juce::String(channel));
        return names;
    }();
    return choices;
}

double measurePitch(const float* samples, int numSamples, double sampleRate)
{
    // Normalised autocorrelation (as YIN's difference): the first lag well inside the 30 Hz - 4 kHz
    // range where it dips under 0.2, at its local minimum, refined by a parabola.
    const int maxLag = std::min(numSamples / 2, static_cast<int>(sampleRate / 30.0));
    const int minLag = std::max(2, static_cast<int>(sampleRate / 4000.0));
    const int window = numSamples - maxLag - 1;
    if (window < minLag * 4)
        return 0.0;
    std::vector<double> d(static_cast<std::size_t>(maxLag + 2), 0.0);
    double running = 0.0;
    d[0] = 1.0;
    for (int lag = 1; lag <= maxLag + 1; ++lag)
    {
        double sum = 0.0;
        for (int i = 0; i < window; ++i)
        {
            const double diff = static_cast<double>(samples[i]) - static_cast<double>(samples[i + lag]);
            sum += diff * diff;
        }
        running += sum;
        d[static_cast<std::size_t>(lag)] = running > 0.0 ? sum * lag / running : 1.0;
    }
    int best = -1;
    for (int lag = minLag; lag < maxLag; ++lag)
        if (d[static_cast<std::size_t>(lag)] < 0.2)
        {
            while (lag + 1 < maxLag && d[static_cast<std::size_t>(lag + 1)] < d[static_cast<std::size_t>(lag)])
                ++lag;
            best = lag;
            break;
        }
    if (best < 0)
        return 0.0;
    const double s0 = d[static_cast<std::size_t>(best - 1)], s1 = d[static_cast<std::size_t>(best)], s2 = d[static_cast<std::size_t>(best + 1)];
    const double denominator = s0 - 2.0 * s1 + s2;
    const double lag = best + (denominator > 1.0e-12 ? std::clamp(0.5 * (s0 - s2) / denominator, -1.0, 1.0) : 0.0);
    return sampleRate / lag;
}
} // namespace cv

//==================================================================================================
// CvDevice

juce::AudioProcessor::BusesProperties CvDevice::busesFor(bool instrument)
{
    if (instrument)
        return BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true);
    return BusesProperties().withInput("Input", juce::AudioChannelSet::stereo(), true).withOutput("Output", juce::AudioChannelSet::stereo(), true);
}

CvDevice::CvDevice(juce::String typeId, juce::String name, bool instrument)
    : BuiltinProcessor(std::move(typeId), std::move(name), busesFor(instrument), instrument)
{
}

void CvDevice::addPort(const juce::String& name, int defaultChoice)
{
    const int index = numPorts_++;
    portNames_[static_cast<std::size_t>(index)] = name;
    portChoices_[static_cast<std::size_t>(index)] =
        addChoice("out" + juce::String(index + 1), name + " Out", cv::channelChoices(), defaultChoice, false);
}

void CvDevice::addAudioIn()
{
    audioIn_ = addChoice("audioIn", "Audio In", HardwareInsert::channelChoices(), 0, false);
    inputGain_ = addFloat("inputGain", "Input Gain", {-24.0f, 24.0f, 0.1f}, 0.0f, "dB", true, 1);
}

void CvDevice::addCommon()
{
    fullScale_ = addFloat("fullScale", "Full Scale", {1.0f, 24.0f, 0.1f}, 10.0f, "V", false, 1);
    delay_ = addFloat("delay", "Delay", {0.0f, static_cast<float>(kMaxDelayMs), 0.1f}, 0.0f, "ms", false, 1);
}

juce::String CvDevice::getProblems() const
{
    if (!isLive())
        return {};
    juce::StringArray missing;
    for (int p = 0; p < numPorts_; ++p)
    {
        const int channel = cv::channelOf(portChoices_[static_cast<std::size_t>(p)]->getIndex());
        if (channel >= getSeenOutputs())
            missing.add(portNames_[static_cast<std::size_t>(p)] + " (" + juce::String(channel + 1) + ")");
    }
    if (audioIn_ != nullptr)
    {
        const auto in = HardwareInsert::channelsOf(audioIn_->getIndex());
        if (in.count > 0 && in.first + in.count > getSeenInputs())
            missing.add("Audio In (" + HardwareInsert::channelChoices()[audioIn_->getIndex()] + ")");
    }
    return missing.isEmpty() ? juce::String() : "Not on this sound card: " + missing.joinIntoString(", ");
}

void CvDevice::prepareDsp(double sampleRate, int maximumBlockSize)
{
    const int block = std::max(1, maximumBlockSize);
    volts_.setSize(std::max(1, numPorts_), block, false, true, false);
    send_.setSize(std::max(1, numPorts_), block, false, true, false);
    const int lineLength = static_cast<int>(std::ceil(kMaxDelayMs / 1000.0 * sampleRate)) + block + 1;
    line_.setSize(std::max(1, numPorts_), lineLength, false, true, false);
    line_.clear();
    write_ = 0;
    sendSamples_ = 0;
    prepareCv(sampleRate, maximumBlockSize);
}

void CvDevice::setHardwarePorts(const HardwarePorts& ports) noexcept { ports_ = ports; }

CvDevice::Transport CvDevice::transport() const noexcept
{
    Transport t;
    if (auto* head = getPlayHead())
        if (const auto position = head->getPosition())
        {
            t.playing = position->getIsPlaying();
            t.ppq = position->getPpqPosition().orFallback(0.0);
            t.bpm = position->getBpm().orFallback(120.0);
        }
    return t;
}

void CvDevice::processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept
{
    const int numSamples = buffer.getNumSamples();
    // The ports hold for this sub-block only.
    const HardwarePorts ports = std::exchange(ports_, HardwarePorts{});
    sendSamples_ = 0;
    if (numSamples <= 0)
        return;
    live_.store(ports.live, std::memory_order_relaxed);
    if (ports.live)
    {
        seenOutputs_.store(ports.numOutputs, std::memory_order_relaxed);
        seenInputs_.store(ports.numInputs, std::memory_order_relaxed);
    }

    // The device's own audio: an instrument plays the modular back in from Audio In (or nothing);
    // the others pass the track's audio through.
    if (acceptsMidi())
    {
        buffer.clear();
        if (audioIn_ != nullptr && ports.live && ports.inputs != nullptr)
        {
            const auto in = HardwareInsert::channelsOf(audioIn_->getIndex());
            const double gain = juce::Decibels::decibelsToGain(static_cast<double>(inputGain_->get()));
            if (in.count > 0 && in.first + in.count <= ports.numInputs)
                for (int ch = 0; ch < std::min(2, buffer.getNumChannels()); ++ch)
                {
                    const Sample* source = ports.inputs[in.first + std::min(ch, in.count - 1)];
                    if (source != nullptr)
                        for (int i = 0; i < numSamples; ++i)
                            buffer.setSample(ch, i, static_cast<double>(source[i]) * gain);
                }
        }
    }

    double* volts[kMaxPorts]{};
    for (int p = 0; p < numPorts_; ++p)
    {
        volts[p] = volts_.getWritePointer(p);
        std::fill(volts[p], volts[p] + numSamples, 0.0);
    }
    generate(volts, numSamples, midi, ports);

    // Volts to samples, through the delay line, into what goes out after the graph.
    const double scale = 1.0 / std::max(0.1, static_cast<double>(fullScale_->get()));
    const int length = line_.getNumSamples();
    const int delay = std::clamp(static_cast<int>(std::lround(delay_->get() / 1000.0 * sampleRate_)), 0, length - numSamples - 1);
    for (int p = 0; p < numPorts_; ++p)
    {
        double* line = line_.getWritePointer(p);
        double* out = send_.getWritePointer(p);
        for (int i = 0; i < numSamples; ++i)
        {
            const int w = (write_ + i) % length;
            line[w] = std::clamp(volts[p][i] * scale, -1.0, 1.0);
            out[i] = line[(w - delay + length) % length];
        }
        portVolts_[static_cast<std::size_t>(p)].store(static_cast<float>(out[numSamples - 1] / scale), std::memory_order_relaxed);
        sentTo_[static_cast<std::size_t>(p)] = cv::channelOf(portChoices_[static_cast<std::size_t>(p)]->getIndex());
    }
    write_ = (write_ + numSamples) % length;
    sendSamples_ = ports.live ? numSamples : 0;
}

void CvDevice::addSends(float* const* outputs, int numOutputs, int offset, int numSamples) noexcept
{
    const int count = std::min(sendSamples_, numSamples);
    sendSamples_ = 0;
    if (count <= 0 || outputs == nullptr)
        return;
    for (int p = 0; p < numPorts_; ++p)
    {
        const int channel = sentTo_[static_cast<std::size_t>(p)];
        if (channel < 0 || channel >= numOutputs || outputs[channel] == nullptr)
            continue;
        const double* source = send_.getReadPointer(p);
        float* destination = outputs[channel] + offset;
        for (int i = 0; i < count; ++i)
            destination[i] += static_cast<float>(source[i]);
    }
}

//==================================================================================================
// CvInstrument

CvInstrument::CvInstrument()
    : CvDevice(kTypeId, "CV Instrument", true)
{
    addPort("Pitch", 1);
    addPort("Gate", 2);
    addPort("Velocity", 0);
    addPort("Mod", 0);
    addAudioIn();
    zeroNote_ = addFloat("zeroNote", "0 V Note", {0.0f, 127.0f, 1.0f}, 36.0f, {}, false, 0);
    transpose_ = addFloat("transpose", "Transpose", {-48.0f, 48.0f, 1.0f}, 0.0f, "st", true, 0);
    fine_ = addFloat("fine", "Fine", {-100.0f, 100.0f, 0.1f}, 0.0f, "ct", true, 1);
    tracking_ = addFloat("tracking", "Tracking", {80.0f, 120.0f, 0.001f}, 100.0f, "%", false, 3);
    offset_ = addFloat("offset", "Offset", {-2000.0f, 2000.0f, 0.1f}, 0.0f, "mV", false, 1);
    bendRange_ = addFloat("bendRange", "Bend Range", {0.0f, 24.0f, 1.0f}, 2.0f, "st", false, 0);
    glide_ = addFloat("glide", "Glide", skewedRange(0.0f, 2000.0f, 150.0f), 0.0f, "ms");
    glideMode_ = addChoice("glideMode", "Glide Mode", {"Always", "Legato"}, 0);
    priority_ = addChoice("priority", "Priority", {"Last", "Lowest", "Highest"}, 0, false);
    gateVolts_ = addFloat("gateVolts", "Gate Level", {1.0f, 12.0f, 0.1f}, 5.0f, "V", false, 1);
    retrigger_ = addBool("retrigger", "Retrigger", true);
    velocityVolts_ = addFloat("velocityVolts", "Velocity Range", {0.0f, 10.0f, 0.1f}, 5.0f, "V", false, 1);
    modVolts_ = addFloat("modVolts", "Mod Range", {0.0f, 10.0f, 0.1f}, 5.0f, "V", false, 1);
    addCommon();
}

void CvInstrument::prepareCv(double sampleRate, int)
{
    held_.fill(false);
    sounding_ = -1;
    glideLeft_ = 0.0;
    retriggerLeft_ = 0;
    currentNote_.store(-1, std::memory_order_relaxed);
    capture_.assign(static_cast<std::size_t>(kCalibrationSteps * kStepSeconds * sampleRate * 0.5) + 16, 0.0f);
    calibrationPosition_ = 0;
    if (getCalibration() == Calibration::running)
        calibration_.store(static_cast<int>(Calibration::waiting), std::memory_order_release);
}

int CvInstrument::chooseNote() const noexcept
{
    int chosen = -1;
    const int priority = priority_->getIndex(); // last, lowest, highest
    for (int n = 0; n < 128; ++n)
    {
        if (!held_[static_cast<std::size_t>(n)])
            continue;
        if (chosen < 0 || priority == 2 || (priority == 0 && order_[static_cast<std::size_t>(n)] > order_[static_cast<std::size_t>(chosen)]))
            chosen = n;
    }
    return chosen;
}

void CvInstrument::noteOn(int note, float velocity) noexcept
{
    const bool legato = sounding_ >= 0;
    held_[static_cast<std::size_t>(note)] = true;
    velocities_[static_cast<std::size_t>(note)] = velocity;
    order_[static_cast<std::size_t>(note)] = ++counter_;
    const int chosen = chooseNote();
    if (chosen == sounding_)
        return;
    sounding_ = chosen;
    velocity_ = velocities_[static_cast<std::size_t>(chosen)];
    // Glide: always (from wherever the pitch was), or only between notes that overlap.
    if (glide_->get() > 0.5f && (glideMode_->getIndex() == 0 || legato))
    {
        glideFrom_ = pitch_;
        glideTarget_ = static_cast<double>(chosen);
        glideTotal_ = glideLeft_ = glide_->get() / 1000.0 * sampleRate_;
    }
    else
    {
        pitch_ = glideTarget_ = static_cast<double>(chosen);
        glideLeft_ = 0.0;
    }
    if (legato && retrigger_->get())
        retriggerLeft_ = static_cast<int>(0.002 * sampleRate_);
}

void CvInstrument::noteOff(int note) noexcept
{
    held_[static_cast<std::size_t>(note)] = false;
    const int chosen = chooseNote();
    if (chosen == sounding_)
        return;
    if (chosen < 0)
    {
        sounding_ = -1; // (the pitch stays: the envelope's release plays on it)
        return;
    }
    // Back to a note still held.
    sounding_ = chosen;
    if (glide_->get() > 0.5f)
    {
        glideFrom_ = pitch_;
        glideTarget_ = chosen;
        glideTotal_ = glideLeft_ = glide_->get() / 1000.0 * sampleRate_;
    }
    else
        pitch_ = glideTarget_ = chosen;
    if (retrigger_->get())
        retriggerLeft_ = static_cast<int>(0.002 * sampleRate_);
}

void CvInstrument::generate(double* const* volts, int numSamples, juce::MidiBuffer& midi, const HardwarePorts& ports) noexcept
{
    // Calibration takes the outputs over: steps of 0..3 V on Pitch, the gate open.
    const int calibration = calibration_.load(std::memory_order_acquire);
    if (calibration == static_cast<int>(Calibration::waiting) && ports.live)
    {
        calibrationPosition_ = 0;
        calibration_.store(static_cast<int>(Calibration::running), std::memory_order_release);
    }
    if (calibration_.load(std::memory_order_acquire) == static_cast<int>(Calibration::running))
    {
        const auto in = HardwareInsert::channelsOf(audioIn_->getIndex());
        const auto stepLength = static_cast<int>(kStepSeconds * sampleRate_);
        const int total = stepLength * kCalibrationSteps;
        for (int i = 0; i < numSamples; ++i)
        {
            const int position = calibrationPosition_ + i;
            const int step = std::min(kCalibrationSteps - 1, position / stepLength);
            volts[pitchPort][i] = static_cast<double>(step);
            volts[gatePort][i] = gateVolts_->get();
            // The second half of each step is recorded (the oscillator has settled).
            const int into = position - step * stepLength;
            if (into >= stepLength / 2 && position < total && ports.inputs != nullptr && in.count > 0 && in.first < ports.numInputs)
            {
                const auto index = static_cast<std::size_t>(step * (stepLength / 2) + (into - stepLength / 2));
                if (index < capture_.size())
                    capture_[index] = static_cast<float>(ports.inputs[in.first][i]);
            }
        }
        calibrationPosition_ += numSamples;
        if (calibrationPosition_ >= total)
            calibration_.store(static_cast<int>(Calibration::analysing), std::memory_order_release);
        midi.clear();
        return;
    }

    const double zero = zeroNote_->get();
    const double tracking = tracking_->get() / 100.0;
    const double offset = offset_->get() / 1000.0;
    const double shift = transpose_->get() + fine_->get() / 100.0;
    int position = 0;
    const auto render = [&](int until)
    {
        for (int i = position; i < until; ++i)
        {
            if (glideLeft_ > 0.0)
            {
                glideLeft_ -= 1.0;
                pitch_ = glideTarget_ + (glideFrom_ - glideTarget_) * std::max(0.0, glideLeft_) / std::max(1.0, glideTotal_);
            }
            volts[pitchPort][i] = cv::pitchVolts(pitch_ + shift + bend_ * bendRange_->get(), zero, tracking, offset);
            const bool open = sounding_ >= 0 && retriggerLeft_ <= 0;
            if (retriggerLeft_ > 0)
                --retriggerLeft_;
            volts[gatePort][i] = open ? gateVolts_->get() : 0.0;
            volts[velocityPort][i] = velocity_ * velocityVolts_->get();
            volts[modPort][i] = mod_ * modVolts_->get();
        }
        position = until;
    };
    for (const auto metadata : midi)
    {
        render(std::clamp(metadata.samplePosition, position, numSamples));
        const auto m = metadata.getMessage();
        if (m.isNoteOn())
            noteOn(m.getNoteNumber(), m.getFloatVelocity());
        else if (m.isNoteOff())
            noteOff(m.getNoteNumber());
        else if (m.isPitchWheel())
            bend_ = (m.getPitchWheelValue() - 8192) / 8192.0;
        else if (m.isControllerOfType(1))
            mod_ = m.getControllerValue() / 127.0;
        else if (m.isAllNotesOff() || m.isAllSoundOff())
        {
            held_.fill(false);
            sounding_ = -1;
        }
    }
    render(numSamples);
    currentNote_.store(sounding_, std::memory_order_relaxed);
    midi.clear();
}

void CvInstrument::calibrate()
{
    const auto state = getCalibration();
    if (state == Calibration::waiting || state == Calibration::running)
    {
        calibration_.store(static_cast<int>(Calibration::idle), std::memory_order_release);
        calibrationMessage_ = "Cancelled.";
        return;
    }
    if (cv::channelOf(getPortChoice(pitchPort)->getIndex()) < 0 || HardwareInsert::channelsOf(audioIn_->getIndex()).count == 0)
    {
        calibration_.store(static_cast<int>(Calibration::failed), std::memory_order_release);
        calibrationMessage_ = "Choose Pitch Out (to the oscillator's 1 V/oct input) and Audio In (from its output) first.";
        return;
    }
    std::fill(capture_.begin(), capture_.end(), 0.0f);
    calibrationMessage_ = "Waiting for the sound card...";
    calibration_.store(static_cast<int>(Calibration::waiting), std::memory_order_release);
}

juce::String CvInstrument::getCalibrationMessage() const
{
    switch (getCalibration())
    {
        case Calibration::running: return "Calibrating: listening to the oscillator...";
        case Calibration::analysing: return "Measuring...";
        case Calibration::idle:
        case Calibration::waiting:
        case Calibration::done:
        case Calibration::failed: break;
    }
    return calibrationMessage_;
}

std::vector<double> CvInstrument::getMeasuredPitches() const { return measured_; }

void CvInstrument::service()
{
    if (getCalibration() != Calibration::analysing)
        return;
    const auto stepLength = static_cast<int>(kStepSeconds * sampleRate_);
    const int half = stepLength / 2;
    measured_.clear();
    for (int step = 0; step < kCalibrationSteps; ++step)
    {
        const auto first = static_cast<std::size_t>(step * half);
        if (first + static_cast<std::size_t>(half) > capture_.size())
            break;
        measured_.push_back(cv::measurePitch(capture_.data() + first, half, sampleRate_));
    }
    if (applyCalibration(measured_))
        calibration_.store(static_cast<int>(Calibration::done), std::memory_order_release);
    else
        calibration_.store(static_cast<int>(Calibration::failed), std::memory_order_release);
}

bool CvInstrument::applyCalibration(const std::vector<double>& hz)
{
    if (hz.size() < 2 || std::any_of(hz.begin(), hz.end(), [](double f) { return f <= 0.0; }))
    {
        calibrationMessage_ = "No steady pitch came back: check the patch (Pitch Out to the oscillator, its output to Audio In).";
        return false;
    }
    // Semitones against volts: a least-squares line.
    const auto n = static_cast<double>(hz.size());
    double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
    for (std::size_t v = 0; v < hz.size(); ++v)
    {
        const double x = static_cast<double>(v);
        const double y = 69.0 + 12.0 * std::log2(hz[v] / 440.0);
        sx += x;
        sy += y;
        sxx += x * x;
        sxy += x * y;
    }
    const double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx); // semitones a volt
    const double noteAtZero = (sy - slope * sx) / n;                // the note the oscillator plays at 0 V
    const double tracking = 12.0 / slope;
    if (!(tracking > 0.8 && tracking < 1.2))
    {
        calibrationMessage_ = "The oscillator moved " + juce::String(slope, 2) + " semitones a volt: not 1 V/octave (or not the patched input).";
        return false;
    }
    // Volts for note N: (N - zero) / 12 * tracking + offset = (N - noteAtZero) / slope.
    const double zero = zeroNote_->get();
    const double offset = (zero - noteAtZero) * tracking / 12.0;
    if (std::abs(offset) > 2.0)
    {
        calibrationMessage_ = "The oscillator is tuned " + juce::String(noteAtZero - zero, 1) + " semitones from the 0 V note: tune it nearer, or change 0 V Note.";
        return false;
    }
    tracking_->setValueNotifyingHost(tracking_->convertTo0to1(static_cast<float>(tracking * 100.0)));
    offset_->setValueNotifyingHost(offset_->convertTo0to1(static_cast<float>(offset * 1000.0)));
    calibrationMessage_ = "Calibrated: " + juce::String(slope, 3) + " semitones a volt; tracking " + juce::String(tracking * 100.0, 2) + " %, offset "
                          + juce::String(offset * 1000.0, 1) + " mV.";
    return true;
}

//==================================================================================================
// CvTriggers

CvTriggers::CvTriggers()
    : CvDevice(kTypeId, "CV Triggers", true)
{
    for (int s = 0; s < kSlots; ++s)
        addPort("Trigger " + juce::String(s + 1), s == 0 ? 1 : 0);
    for (int s = 0; s < kSlots; ++s)
        notes_[static_cast<std::size_t>(s)] = addFloat("note" + juce::String(s + 1), "Note " + juce::String(s + 1), {0.0f, 127.0f, 1.0f},
                                                       static_cast<float>(36 + s), {}, false, 0);
    mode_ = addChoice("mode", "Mode", {"Trigger", "Gate"}, 0, false);
    length_ = addFloat("length", "Trigger Length", {1.0f, 100.0f, 0.1f}, 10.0f, "ms", false, 1);
    volts_ = addFloat("volts", "Level", {1.0f, 12.0f, 0.1f}, 5.0f, "V", false, 1);
    velocity_ = addBool("velocity", "Velocity Level", false);
    addAudioIn();
    addCommon();
}

void CvTriggers::prepareCv(double, int)
{
    left_.fill(0);
    holding_.fill(false);
    level_.fill(0.0);
}

void CvTriggers::generate(double* const* volts, int numSamples, juce::MidiBuffer& midi, const HardwarePorts&) noexcept
{
    const bool gates = mode_->getIndex() == 1;
    const int length = std::max(1, static_cast<int>(length_->get() / 1000.0 * sampleRate_));
    int position = 0;
    const auto render = [&](int until)
    {
        for (int s = 0; s < kSlots; ++s)
        {
            const auto su = static_cast<std::size_t>(s);
            for (int i = position; i < until; ++i)
            {
                const bool high = gates ? holding_[su] : left_[su] > 0;
                if (!gates && left_[su] > 0)
                    --left_[su];
                volts[s][i] = high ? level_[su] : 0.0;
            }
        }
        position = until;
    };
    for (const auto metadata : midi)
    {
        render(std::clamp(metadata.samplePosition, position, numSamples));
        const auto m = metadata.getMessage();
        if (!m.isNoteOnOrOff())
            continue;
        for (int s = 0; s < kSlots; ++s)
        {
            const auto su = static_cast<std::size_t>(s);
            if (std::lround(notes_[su]->get()) != m.getNoteNumber())
                continue;
            if (m.isNoteOn())
            {
                level_[su] = volts_->get() * (velocity_->get() ? m.getFloatVelocity() : 1.0);
                left_[su] = length;
                holding_[su] = true;
            }
            else
                holding_[su] = false;
        }
    }
    render(numSamples);
    midi.clear();
}

//==================================================================================================
// CvClock

CvClock::CvClock()
    : CvDevice(kTypeId, "CV Clock", false)
{
    addPort("Clock", 1);
    addPort("Run", 0);
    addPort("Reset", 0);
    rate_ = addChoice("rate", "Pulses / Quarter", {"1", "2", "4", "8", "12", "16", "24", "48"}, 2, false);
    width_ = addFloat("width", "Width", {1.0f, 90.0f, 0.1f}, 50.0f, "%", true, 1);
    swing_ = addFloat("swing", "Swing", {0.0f, 75.0f, 0.1f}, 0.0f, "%", true, 1);
    volts_ = addFloat("volts", "Level", {1.0f, 12.0f, 0.1f}, 5.0f, "V", false, 1);
    addCommon();
}

int CvClock::pulsesPerQuarter(int choice) noexcept
{
    static constexpr int kRates[] = {1, 2, 4, 8, 12, 16, 24, 48};
    return kRates[std::clamp(choice, 0, 7)];
}

void CvClock::prepareCv(double, int)
{
    wasPlaying_ = false;
    resetLeft_ = 0;
    lastPulse_ = -1.0;
}

void CvClock::generate(double* const* volts, int numSamples, juce::MidiBuffer&, const HardwarePorts&) noexcept
{
    const auto t = transport();
    const double level = volts_->get();
    if (t.playing && !wasPlaying_)
    {
        resetLeft_ = static_cast<int>(0.01 * sampleRate_); // a 10 ms reset as the transport starts
        pulses_.store(0, std::memory_order_relaxed);
        lastPulse_ = -1.0;
    }
    wasPlaying_ = t.playing;
    const int ppqn = pulsesPerQuarter(rate_->getIndex());
    const double width = width_->get() / 100.0;
    const double swing = swing_->get() / 100.0;
    const double quartersPerSample = t.bpm / 60.0 / sampleRate_;
    for (int i = 0; i < numSamples; ++i)
    {
        double high = 0.0;
        if (t.playing)
        {
            // Where this sample is, in pulses; every second pulse later by the swing.
            const double pulses = (t.ppq + i * quartersPerSample) * ppqn;
            const double pair = std::floor(pulses / 2.0);
            const double into = pulses - pair * 2.0; // 0..2
            const double second = 1.0 + swing;       // where the second pulse of a pair starts
            const bool inSecond = into >= second;
            const double start = inSecond ? second : 0.0;
            const double span = inSecond ? 2.0 - second : second;
            const double phase = (into - start) / std::max(1.0e-9, span);
            high = phase < width ? level : 0.0;
            const double number = pair * 2.0 + (inSecond ? 1.0 : 0.0);
            if (high > 0.0 && number > lastPulse_)
            {
                lastPulse_ = number;
                pulses_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        volts[clockPort][i] = high;
        volts[runPort][i] = t.playing ? level : 0.0;
        volts[resetPort][i] = resetLeft_ > 0 ? level : 0.0;
        if (resetLeft_ > 0)
            --resetLeft_;
    }
}

//==================================================================================================
// CvUtility

CvUtility::CvUtility()
    : CvDevice(kTypeId, "CV Utility", false)
{
    addPort("CV", 1);
    value_ = addFloat("value", "Value", {0.0f, 100.0f, 0.01f}, 0.0f, "%", true, 2);
    minimum_ = addFloat("minimum", "Minimum", {-10.0f, 10.0f, 0.01f}, 0.0f, "V", false, 2);
    maximum_ = addFloat("maximum", "Maximum", {-10.0f, 10.0f, 0.01f}, 5.0f, "V", false, 2);
    shape_ = addChoice("shape", "LFO", {"Off", "Sine", "Triangle", "Saw", "Square", "Random"}, 0);
    rate_ = addFloat("rate", "LFO Rate", skewedRange(0.01f, 100.0f, 2.0f), 1.0f, "Hz");
    depth_ = addFloat("depth", "LFO Depth", {0.0f, 10.0f, 0.01f}, 1.0f, "V", true, 2);
    slew_ = addFloat("slew", "Slew", skewedRange(0.0f, 2000.0f, 100.0f), 0.0f, "ms");
    addCommon();
}

void CvUtility::prepareCv(double, int)
{
    phase_ = 0.0;
    held_ = 0.0;
    out_ = minimum_->get() + (maximum_->get() - minimum_->get()) * value_->get() / 100.0;
}

void CvUtility::generate(double* const* volts, int numSamples, juce::MidiBuffer&, const HardwarePorts&) noexcept
{
    const double target = minimum_->get() + (maximum_->get() - minimum_->get()) * value_->get() / 100.0;
    const double step = rate_->get() / sampleRate_;
    const int shape = shape_->getIndex();
    const double depth = depth_->get();
    const double slewSamples = slew_->get() / 1000.0 * sampleRate_;
    const double coefficient = slewSamples > 1.0 ? 1.0 - std::exp(-1.0 / (slewSamples / 5.0)) : 1.0;
    for (int i = 0; i < numSamples; ++i)
    {
        double lfo = 0.0;
        switch (shape)
        {
            case 1: lfo = std::sin(2.0 * std::numbers::pi * phase_); break;
            case 2: lfo = phase_ < 0.5 ? 4.0 * phase_ - 1.0 : 3.0 - 4.0 * phase_; break;
            case 3: lfo = 2.0 * phase_ - 1.0; break;
            case 4: lfo = phase_ < 0.5 ? 1.0 : -1.0; break;
            case 5: lfo = held_; break;
            default: break;
        }
        phase_ += step;
        if (phase_ >= 1.0)
        {
            phase_ -= std::floor(phase_);
            random_ = random_ * 1664525u + 1013904223u;
            held_ = static_cast<double>(random_ >> 8) / static_cast<double>(1u << 24) * 2.0 - 1.0;
        }
        out_ += (target + lfo * depth - out_) * coefficient;
        volts[0][i] = out_;
    }
}

//==================================================================================================
// CvIn

CvIn::CvIn()
    : BuiltinProcessor(kTypeId, "CV In",
                       BusesProperties()
                           .withInput("Input", juce::AudioChannelSet::stereo(), true)
                           .withOutput("Output", juce::AudioChannelSet::stereo(), true),
                       false)
{
    input_ = addChoice("input", "CV In", cv::channelChoices(), 1, false);
    minimum_ = addFloat("minimum", "Minimum", {-10.0f, 10.0f, 0.01f}, 0.0f, "V", false, 2);
    maximum_ = addFloat("maximum", "Maximum", {-10.0f, 10.0f, 0.01f}, 5.0f, "V", false, 2);
    fullScale_ = addFloat("fullScale", "Full Scale", {1.0f, 24.0f, 0.1f}, 10.0f, "V", false, 1);
    smooth_ = addFloat("smooth", "Smooth", skewedRange(0.0f, 1000.0f, 50.0f), 5.0f, "ms");
    invert_ = addBool("invert", "Invert", false);
}

void CvIn::prepareDsp(double, int)
{
    reading_ = smoothed_ = 0.0;
}

void CvIn::setHardwarePorts(const HardwarePorts& ports) noexcept { ports_ = ports; }

void CvIn::processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer&) noexcept
{
    // (the track's audio passes through; the input is read for the next block's modulation)
    const HardwarePorts ports = std::exchange(ports_, HardwarePorts{});
    live_.store(ports.live, std::memory_order_relaxed);
    const int channel = cv::channelOf(input_->getIndex());
    const int count = buffer.getNumSamples();
    if (!ports.live || ports.inputs == nullptr || channel < 0 || channel >= ports.numInputs || ports.inputs[channel] == nullptr || count <= 0)
        return;
    // The mean of what came in (a voltage changes slowly against a sub-block).
    double sum = 0.0;
    for (int i = 0; i < count; ++i)
        sum += static_cast<double>(ports.inputs[channel][i]);
    reading_ = sum / count * static_cast<double>(fullScale_->get());
}

double CvIn::advanceModulation(int numSamples, double sampleRate, double, double, bool) noexcept
{
    const double smoothing = smooth_->get() / 1000.0 * sampleRate;
    const double coefficient = smoothing > 1.0 ? 1.0 - std::exp(-numSamples / smoothing) : 1.0;
    smoothed_ += (reading_ - smoothed_) * coefficient;
    const double lo = minimum_->get();
    const double hi = maximum_->get();
    double out = std::abs(hi - lo) > 1.0e-6 ? (smoothed_ - lo) / (hi - lo) : 0.0;
    out = std::clamp(out, 0.0, 1.0);
    if (invert_->get())
        out = 1.0 - out;
    volts_.store(static_cast<float>(smoothed_), std::memory_order_relaxed);
    output_.store(static_cast<float>(out), std::memory_order_relaxed);
    return out;
}

} // namespace dojo::devices
