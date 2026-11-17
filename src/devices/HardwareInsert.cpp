#include "devices/HardwareInsert.h"

#include "core/rt/AtomicUtils.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace dojo::devices
{

//==================================================================================================
// Measuring the round trip

namespace loopback
{

std::vector<float> makePing(double sampleRate, float level)
{
    const int length = std::max(64, static_cast<int>(std::lround(0.15 * sampleRate)));
    const double low = 40.0;
    const double high = std::min(16000.0, 0.4 * sampleRate);
    const double duration = static_cast<double>(length) / sampleRate;
    const double rate = std::log(high / low);
    const int fade = std::max(8, length / 20);

    std::vector<float> ping(static_cast<std::size_t>(length));
    for (int i = 0; i < length; ++i)
    {
        const double t = static_cast<double>(i) / sampleRate;
        // An exponential sweep: the frequency rises from `low` to `high` at a constant rate per octave.
        const double phase = juce::MathConstants<double>::twoPi * low * duration / rate * (std::exp(t / duration * rate) - 1.0);
        double gain = 1.0;
        if (i < fade)
            gain = 0.5 - 0.5 * std::cos(juce::MathConstants<double>::pi * i / fade);
        else if (i >= length - fade)
            gain = 0.5 - 0.5 * std::cos(juce::MathConstants<double>::pi * (length - 1 - i) / fade);
        ping[static_cast<std::size_t>(i)] = static_cast<float>(level * gain * std::sin(phase));
    }
    return ping;
}

Measurement measure(const std::vector<float>& ping, const float* recorded, int recordedLength, int minimumView)
{
    Measurement result;
    const int pingLength = static_cast<int>(ping.size());
    if (pingLength == 0 || recorded == nullptr || recordedLength < pingLength)
        return result;

    double pingEnergy = 0.0;
    for (const float v : ping)
        pingEnergy += static_cast<double>(v) * static_cast<double>(v);
    if (pingEnergy <= 0.0)
        return result;

    // The cross-correlation of the recording with the ping at every round trip, through FFTs long
    // enough that nothing wraps around.
    int order = 1;
    while ((1 << order) < recordedLength + pingLength)
        ++order;
    const int size = 1 << order;
    juce::dsp::FFT fft(order);
    std::vector<float> a(static_cast<std::size_t>(size) * 2, 0.0f);
    std::vector<float> b(static_cast<std::size_t>(size) * 2, 0.0f);
    std::copy_n(recorded, recordedLength, a.begin());
    std::copy(ping.begin(), ping.end(), b.begin());
    fft.performRealOnlyForwardTransform(a.data());
    fft.performRealOnlyForwardTransform(b.data());
    for (int k = 0; k < size; ++k)
    {
        const auto re = static_cast<std::size_t>(2 * k), im = re + 1;
        const float ar = a[re], ai = a[im], br = b[re], bi = b[im];
        a[re] = ar * br + ai * bi; // a * conj(b)
        a[im] = ai * br - ar * bi;
    }
    fft.performRealOnlyInverseTransform(a.data());

    // The strongest correlation where the whole ping fits in the recording.
    const int lags = recordedLength - pingLength + 1;
    int best = 0;
    float bestMagnitude = 0.0f;
    for (int k = 0; k < lags; ++k)
        if (const float magnitude = std::abs(a[static_cast<std::size_t>(k)]); magnitude > bestMagnitude)
        {
            bestMagnitude = magnitude;
            best = k;
        }

    // The correlation for the display: its magnitude, the peak at 1, in at most 512 points, from no
    // delay to four times the round trip found (so the peak and what follows it are in view).
    const int view = std::clamp(std::max(4 * best, minimumView), std::min(lags, 512), lags);
    const int points = std::min(512, view);
    result.correlationLags = view;
    result.correlation.assign(static_cast<std::size_t>(points), 0.0f);
    if (bestMagnitude > 0.0f)
        for (int k = 0; k < view; ++k)
        {
            auto& point = result.correlation[static_cast<std::size_t>(static_cast<std::int64_t>(k) * points / view)];
            point = std::max(point, std::abs(a[static_cast<std::size_t>(k)]) / bestMagnitude);
        }

    // How the peak stands out: against the correlation's RMS away from it.
    const int guard = std::max(64, pingLength / 50);
    double rest = 0.0;
    int restCount = 0;
    for (int k = 0; k < lags; ++k)
        if (std::abs(k - best) > guard)
        {
            const double v = a[static_cast<std::size_t>(k)];
            rest += v * v;
            ++restCount;
        }
    const double restRms = restCount > 0 ? std::sqrt(rest / restCount) : 0.0;

    // The exact correlation at the peak (independent of the FFT's scaling): the loop's gain and sign.
    double correlation = 0.0;
    for (int i = 0; i < pingLength; ++i)
        correlation += static_cast<double>(recorded[best + i]) * static_cast<double>(ping[static_cast<std::size_t>(i)]);
    const double gain = correlation / pingEnergy;

    result.latency = best;
    result.inverted = gain < 0.0;
    result.gainDb = static_cast<float>(juce::Decibels::gainToDecibels(std::abs(gain), -100.0));
    result.clarityDb = restRms > 0.0 ? static_cast<float>(juce::Decibels::gainToDecibels(bestMagnitude / restRms, -100.0))
                                     : (bestMagnitude > 0.0f ? 100.0f : 0.0f);
    result.found = bestMagnitude > 0.0f && result.gainDb >= kMinGainDb && result.clarityDb >= kMinClarityDb;
    return result;
}

} // namespace loopback

//==================================================================================================

const juce::StringArray& HardwareInsert::channelChoices()
{
    static const juce::StringArray choices = []
    {
        juce::StringArray names{"None"};
        for (int pair = 0; pair < 32; ++pair)
            names.add(juce::String(2 * pair + 1) + "/" + juce::String(2 * pair + 2));
        for (int channel = 1; channel <= 64; ++channel)
            names.add(juce::String(channel));
        return names;
    }();
    return choices;
}

HardwareInsert::Channels HardwareInsert::channelsOf(int choice) noexcept
{
    if (choice >= 1 && choice <= 32)
        return {2 * (choice - 1), 2};
    if (choice >= 33 && choice <= 96)
        return {choice - 33, 1};
    return {};
}

int HardwareInsert::choiceOf(Channels channels) noexcept
{
    if (channels.count == 2 && channels.first >= 0 && channels.first % 2 == 0 && channels.first < 64)
        return 1 + channels.first / 2;
    if (channels.count == 1 && channels.first >= 0 && channels.first < 64)
        return 33 + channels.first;
    return 0;
}

HardwareInsert::HardwareInsert()
    : BuiltinProcessor(kTypeId, "HW Insert",
                       BusesProperties()
                           .withInput("Input", juce::AudioChannelSet::stereo(), true)
                           .withOutput("Output", juce::AudioChannelSet::stereo(), true),
                       false)
{
    send_ = addChoice("send", "Send To", channelChoices(), choiceOf({2, 2}), false);
    return_ = addChoice("return", "Return From", channelChoices(), choiceOf({2, 2}), false);
    sendGain_ = addFloat("sendgain", "Send", {-24.0f, 24.0f, 0.01f}, 0.0f, "dB");
    returnGain_ = addFloat("returngain", "Return", {-24.0f, 24.0f, 0.01f}, 0.0f, "dB");
    mix_ = addFloat("mix", "Dry/Wet", {0.0f, 100.0f, 0.1f}, 100.0f, "%");
    phase_ = addBool("phase", "Phase", false);
    offset_ = addFloat("offset", "Offset", {-1000.0f, 1000.0f, 1.0f}, 0.0f, "smp", false, 0);
    autoPing_ = addBool("auto", "Auto Ping", true);
    pingLevel_ = addChoice("pinglevel", "Ping Level", {"-30 dBFS", "-24 dBFS", "-18 dBFS", "-12 dBFS", "-6 dBFS"}, 2, false);
}

HardwareInsert::Channels HardwareInsert::getSendChannels() const noexcept
{
    return channelsOf(send_->getIndex());
}

HardwareInsert::Channels HardwareInsert::getReturnChannels() const noexcept
{
    return channelsOf(return_->getIndex());
}

float HardwareInsert::pingLevelGain() const
{
    return static_cast<float>(juce::Decibels::decibelsToGain(-30.0 + 6.0 * pingLevel_->getIndex()));
}

void HardwareInsert::prepareDsp(double sampleRate, int maximumBlockSize)
{
    sendBuffer_.setSize(2, std::max(1, maximumBlockSize), false, true, false);
    sendBuffer_.clear();
    sendSamples_ = 0;
    dryLine_.setSize(2, kMaxLatency + std::max(1, maximumBlockSize) + 1, false, true, false);
    dryLine_.clear();
    dryWrite_ = 0;
    ping_ = loopback::makePing(sampleRate);
    listenSamples_ = static_cast<int>(std::ceil(kListenSeconds * sampleRate));
    capture_.assign(ping_.size() + static_cast<std::size_t>(listenSamples_), 0.0f);
    pingPosition_ = 0;
    capturePosition_ = 0;
    smoothing_ = 1.0 - std::exp(-1.0 / (0.01 * sampleRate)); // 10 ms
    muteStep_ = 1.0 / (0.005 * sampleRate);
    sendGainNow_ = juce::Decibels::decibelsToGain(static_cast<double>(sendGain_->get()));
    returnGainNow_ = juce::Decibels::decibelsToGain(static_cast<double>(returnGain_->get())) * (phase_->get() ? -1.0 : 1.0);
    mixNow_ = mix_->get() / 100.0;
    muteNow_ = 1.0;
    heldLeft_ = heldRight_ = 0.0;
    // A measurement cut short by the restart is lost (its recording no longer fits).
    if (const int state = pingState_.load(std::memory_order_acquire);
        state == static_cast<int>(PingState::running) || state == static_cast<int>(PingState::analysing))
        pingState_.store(static_cast<int>(PingState::waiting), std::memory_order_release);
}

void HardwareInsert::setHardwarePorts(const HardwarePorts& ports) noexcept
{
    ports_ = ports;
}

void HardwareInsert::processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer&) noexcept
{
    const int numSamples = buffer.getNumSamples();
    // The ports hold for this sub-block only: a device the engine did not give them to has none.
    const HardwarePorts ports = std::exchange(ports_, HardwarePorts{});
    sendSamples_ = 0;
    if (numSamples <= 0 || buffer.getNumChannels() < 1)
        return;

    processed_.fetch_add(1, std::memory_order_relaxed);
    seenLive_.store(ports.live, std::memory_order_relaxed);
    if (ports.live)
    {
        seenInputs_.store(ports.numInputs, std::memory_order_relaxed);
        seenOutputs_.store(ports.numOutputs, std::memory_order_relaxed);
        seenBuffer_.store(ports.bufferSize, std::memory_order_relaxed);
        seenReported_.store(ports.reportedLatency, std::memory_order_relaxed);
    }
    if (auto* head = getPlayHead())
        if (const auto position = head->getPosition())
            playing_.store(position->getIsPlaying(), std::memory_order_relaxed);

    const Channels send = getSendChannels();
    const Channels ret = getReturnChannels();
    const bool sendOk = ports.live && send.count > 0 && send.first + send.count <= ports.numOutputs;
    const bool returnOk = ports.live && ports.inputs != nullptr && ret.count > 0 && ret.first + ret.count <= ports.numInputs;

    // A measurement asked for starts here, with the sound card's channels there.
    int state = pingState_.load(std::memory_order_acquire);
    if (state == static_cast<int>(PingState::waiting) && ports.live)
    {
        if (sendOk && returnOk)
        {
            pingPosition_ = 0;
            capturePosition_ = 0;
            pingGain_ = pingLevelAsked_.load(std::memory_order_relaxed);
            state = static_cast<int>(PingState::running);
        }
        else
            state = static_cast<int>(PingState::unavailable);
        pingState_.store(state, std::memory_order_release);
    }
    const bool pinging = state == static_cast<int>(PingState::running);

    double* left = buffer.getWritePointer(0);
    double* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : nullptr;
    double* lineLeft = dryLine_.getWritePointer(0);
    double* lineRight = dryLine_.getWritePointer(1);
    const int lineSize = dryLine_.getNumSamples();
    const int latency = std::clamp(appliedLatency_.load(std::memory_order_relaxed), 0, kMaxLatency);
    double* sendLeft = sendBuffer_.getWritePointer(0);
    double* sendRight = sendBuffer_.getWritePointer(1);
    const int sendable = std::min(numSamples, sendBuffer_.getNumSamples());
    const Sample* returnLeft = returnOk ? ports.inputs[ret.first] : nullptr;
    const Sample* returnRight = returnOk ? ports.inputs[ret.first + ret.count - 1] : nullptr;

    const double sendTarget = juce::Decibels::decibelsToGain(static_cast<double>(sendGain_->get()));
    const double returnTarget = juce::Decibels::decibelsToGain(static_cast<double>(returnGain_->get())) * (phase_->get() ? -1.0 : 1.0);
    const double mixTarget = mix_->get() / 100.0;
    const int pingLength = static_cast<int>(ping_.size());
    const int captureLength = static_cast<int>(capture_.size());

    float sendPeak = 0.0f, returnPeak = 0.0f;
    for (int i = 0; i < numSamples; ++i)
    {
        const double inLeft = left[i];
        const double inRight = right != nullptr ? right[i] : inLeft;

        // The dry signal, delayed to meet the return (the line runs all the time, so it stays whole).
        lineLeft[dryWrite_] = inLeft;
        lineRight[dryWrite_] = inRight;
        int readAt = dryWrite_ - latency;
        if (readAt < 0)
            readAt += lineSize;
        const double dryLeft = lineLeft[readAt];
        const double dryRight = lineRight[readAt];
        if (++dryWrite_ >= lineSize)
            dryWrite_ = 0;

        sendGainNow_ += (sendTarget - sendGainNow_) * smoothing_;
        returnGainNow_ += (returnTarget - returnGainNow_) * smoothing_;
        mixNow_ += (mixTarget - mixNow_) * smoothing_;

        double outLeft = 0.0, outRight = 0.0;
        if (pinging)
        {
            // The sweep goes out on the send; the return is recorded as it comes; the device's
            // output fades out from where it was.
            const float sample = pingPosition_ < pingLength ? ping_[static_cast<std::size_t>(pingPosition_)] * pingGain_ : 0.0f;
            ++pingPosition_;
            if (i < sendable)
                sendLeft[i] = sendRight[i] = sample;
            if (capturePosition_ < captureLength)
                capture_[static_cast<std::size_t>(capturePosition_++)] =
                    returnLeft != nullptr ? static_cast<float>(0.5 * (returnLeft[i] + returnRight[i])) : 0.0f;
            muteNow_ = std::max(0.0, muteNow_ - muteStep_);
            outLeft = heldLeft_ * muteNow_;
            outRight = heldRight_ * muteNow_;
        }
        else
        {
            if (sendOk && i < sendable)
            {
                const double l = inLeft * sendGainNow_;
                const double r = inRight * sendGainNow_;
                if (send.count == 1)
                    sendLeft[i] = 0.5 * (l + r);
                else
                {
                    sendLeft[i] = l;
                    sendRight[i] = r;
                }
                sendPeak = std::max(sendPeak, static_cast<float>(std::max(std::abs(l), std::abs(r))));
            }
            if (returnOk)
            {
                const double l = returnLeft[i] * returnGainNow_;
                const double r = returnRight[i] * returnGainNow_;
                returnPeak = std::max(returnPeak, static_cast<float>(std::max(std::abs(l), std::abs(r))));
                outLeft = dryLeft + (l - dryLeft) * mixNow_;
                outRight = dryRight + (r - dryRight) * mixNow_;
            }
            else
            {
                // No return on this sound card (or no sound card): the signal passes, in time.
                outLeft = dryLeft;
                outRight = dryRight;
            }
            heldLeft_ = outLeft;
            heldRight_ = outRight;
            muteNow_ = std::min(1.0, muteNow_ + muteStep_);
            outLeft *= muteNow_;
            outRight *= muteNow_;
        }

        left[i] = outLeft;
        if (right != nullptr)
            right[i] = outRight;
    }

    if (pinging && capturePosition_ >= captureLength)
        pingState_.store(static_cast<int>(PingState::analysing), std::memory_order_release);

    if (pinging || sendOk)
    {
        sendSamples_ = sendable;
        sentTo_ = send;
    }
    rt::atomicStoreMax(sendPeak_, sendPeak);
    rt::atomicStoreMax(returnPeak_, returnPeak);
}

void HardwareInsert::addSends(float* const* outputs, int numOutputs, int offset, int numSamples) noexcept
{
    const int count = std::min(sendSamples_, numSamples);
    sendSamples_ = 0;
    if (count <= 0 || outputs == nullptr)
        return;
    for (int c = 0; c < sentTo_.count; ++c)
    {
        const int channel = sentTo_.first + c;
        if (channel < 0 || channel >= numOutputs || outputs[channel] == nullptr)
            continue;
        const double* source = sendBuffer_.getReadPointer(c);
        float* destination = outputs[channel] + offset;
        for (int i = 0; i < count; ++i)
            destination[i] += static_cast<float>(source[i]);
    }
}

//==================================================================================================
// Message thread

HardwareInsert::Hardware HardwareInsert::getHardware() const noexcept
{
    Hardware hardware;
    hardware.live = seenLive_.load(std::memory_order_relaxed) && idleServices_ < 5;
    hardware.numInputs = seenInputs_.load(std::memory_order_relaxed);
    hardware.numOutputs = seenOutputs_.load(std::memory_order_relaxed);
    hardware.bufferSize = seenBuffer_.load(std::memory_order_relaxed);
    hardware.reportedLatency = seenReported_.load(std::memory_order_relaxed);
    return hardware;
}

std::pair<float, float> HardwareInsert::takePeaks() noexcept
{
    return {sendPeak_.exchange(0.0f, std::memory_order_relaxed), returnPeak_.exchange(0.0f, std::memory_order_relaxed)};
}

HardwareInsert::Setup HardwareInsert::currentSetup() const
{
    return {static_cast<int>(std::lround(sampleRate_)), seenBuffer_.load(std::memory_order_relaxed), send_->getIndex(), return_->getIndex()};
}

bool HardwareInsert::isCalibrationCurrent() const
{
    const auto setup = currentSetup();
    return calibration_.valid && setup.bufferSize > 0
           && Setup{static_cast<int>(std::lround(calibration_.sampleRate)), calibration_.bufferSize, calibration_.send, calibration_.ret} == setup;
}

void HardwareInsert::ping()
{
    auto expected = static_cast<int>(PingState::waiting);
    if (pingState_.compare_exchange_strong(expected, static_cast<int>(PingState::idle), std::memory_order_acq_rel))
    {
        message_ = "Measurement cancelled.";
        return;
    }
    if (expected != static_cast<int>(PingState::idle))
        return; // (one is running)
    pingLevelAsked_.store(pingLevelGain(), std::memory_order_relaxed);
    message_ = "Measuring the round trip...";
    pingState_.store(static_cast<int>(PingState::waiting), std::memory_order_release);
}

void HardwareInsert::analyse()
{
    const float level = pingLevelAsked_.load(std::memory_order_relaxed);
    std::vector<float> sent(ping_.size());
    for (std::size_t i = 0; i < sent.size(); ++i)
        sent[i] = ping_[i] * level;
    auto measurement = loopback::measure(sent, capture_.data(), static_cast<int>(capture_.size()),
                                         static_cast<int>(std::lround(0.02 * sampleRate_))); // (at least 20 ms in view)
    correlation_ = std::move(measurement.correlation);
    correlationLags_ = measurement.correlationLags;

    const auto setup = currentSetup();
    const auto returnName = channelChoices()[return_->getIndex()];
    if (measurement.found)
    {
        calibration_ = {true, measurement.latency, measurement.gainDb, measurement.inverted, measurement.clarityDb,
                        static_cast<double>(setup.sampleRateHz), setup.bufferSize, setup.send, setup.ret};
        if (phase_->get() != measurement.inverted)
            phase_->setValueNotifyingHost(measurement.inverted ? 1.0f : 0.0f);
        message_ = "Round trip " + juce::String(measurement.latency) + " samples ("
                   + juce::String(1000.0 * measurement.latency / sampleRate_, 2) + " ms), back at "
                   + juce::String(measurement.gainDb, 1) + " dB" + (measurement.inverted ? ", inverted (Phase on)" : "") + ".";
        if (measurement.latency > kMaxLatency)
            message_ << " Longer than the delay compensation reaches (" << kMaxLatency << " samples): a smaller buffer helps.";
    }
    else if (measurement.gainDb < loopback::kMinGainDb)
        message_ = "Nothing came back on input " + returnName + ": check the cables and the hardware.";
    else
        message_ = "The return was too noisy or distorted to measure: try a louder ping.";

    pingState_.store(static_cast<int>(PingState::idle), std::memory_order_release);
    updateHostDisplay(juce::AudioProcessor::ChangeDetails().withNonParameterStateChanged(true));
}

void HardwareInsert::updateLatency()
{
    const auto hardware = getHardware();
    int base = 0;
    if (calibration_.valid && (isCalibrationCurrent() || !hardware.live))
        base = calibration_.latency;
    else if (hardware.reportedLatency > 0)
        base = hardware.reportedLatency;
    else if (hardware.bufferSize > 0)
        base = 2 * hardware.bufferSize;
    else if (calibration_.valid)
        base = calibration_.latency;
    const int total = std::clamp(base + static_cast<int>(std::lround(offset_->get())), 0, kMaxLatency);
    if (total != appliedLatency_.load(std::memory_order_relaxed) || total != getLatencySamples())
    {
        appliedLatency_.store(total, std::memory_order_relaxed);
        setLatencySamples(total);
    }
}

void HardwareInsert::service()
{
    const auto processed = processed_.load(std::memory_order_relaxed);
    const bool running = processed != lastProcessed_;
    lastProcessed_ = processed;
    idleServices_ = running ? 0 : std::min(idleServices_ + 1, 1000);

    switch (getPingState())
    {
    case PingState::analysing:
        analyse();
        break;
    case PingState::unavailable:
    {
        const auto hardware = getHardware();
        message_ = "Can't measure: the sound card has " + juce::String(hardware.numOutputs) + " outputs and "
                   + juce::String(hardware.numInputs) + " inputs (more can be switched on in Preferences > Audio).";
        pingState_.store(static_cast<int>(PingState::idle), std::memory_order_release);
        break;
    }
    case PingState::idle:
    case PingState::waiting:
    case PingState::running:
        break;
    }

    // Auto Ping: once per setup (rate, buffer, channels) not measured yet, while stopped.
    const auto hardware = getHardware();
    if (autoPing_->get() && getPingState() == PingState::idle && running && hardware.live && !playing_.load(std::memory_order_relaxed))
    {
        const auto send = getSendChannels();
        const auto ret = getReturnChannels();
        const bool there = send.count > 0 && ret.count > 0 && send.first + send.count <= hardware.numOutputs
                           && ret.first + ret.count <= hardware.numInputs;
        const auto setup = currentSetup();
        if (there && !isCalibrationCurrent() && autoTried_ != setup)
        {
            autoTried_ = setup;
            ping();
        }
    }

    updateLatency();
}

void HardwareInsert::matchLevels()
{
    if (!calibration_.valid)
        return;
    // The ping went out without the Send level: Send + loop + Return = 0 dB.
    const float target = juce::jlimit(-24.0f, 24.0f, -(sendGain_->get() + calibration_.gainDb));
    returnGain_->setValueNotifyingHost(returnGain_->convertTo0to1(target));
}

void HardwareInsert::writeExtraState(juce::XmlElement& xml) const
{
    if (!calibration_.valid)
        return;
    auto* element = xml.createNewChildElement("ROUNDTRIP");
    element->setAttribute("latency", calibration_.latency);
    element->setAttribute("gainDb", static_cast<double>(calibration_.gainDb));
    element->setAttribute("inverted", calibration_.inverted);
    element->setAttribute("clarityDb", static_cast<double>(calibration_.clarityDb));
    element->setAttribute("sampleRate", calibration_.sampleRate);
    element->setAttribute("bufferSize", calibration_.bufferSize);
    element->setAttribute("send", calibration_.send);
    element->setAttribute("return", calibration_.ret);
}

void HardwareInsert::readExtraState(const juce::XmlElement& xml)
{
    calibration_ = {};
    if (const auto* element = xml.getChildByName("ROUNDTRIP"))
    {
        calibration_.valid = true;
        calibration_.latency = std::max(0, element->getIntAttribute("latency"));
        calibration_.gainDb = static_cast<float>(element->getDoubleAttribute("gainDb"));
        calibration_.inverted = element->getBoolAttribute("inverted");
        calibration_.clarityDb = static_cast<float>(element->getDoubleAttribute("clarityDb"));
        calibration_.sampleRate = element->getDoubleAttribute("sampleRate");
        calibration_.bufferSize = element->getIntAttribute("bufferSize");
        calibration_.send = element->getIntAttribute("send");
        calibration_.ret = element->getIntAttribute("return");
    }
    updateLatency();
}

} // namespace dojo::devices
