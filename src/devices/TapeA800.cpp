#include "devices/TapeA800.h"

#include <algorithm>
#include <cmath>
#include <mutex>

namespace dojo::devices
{
namespace
{
using Shape = dsp::BiquadCoefficients::Shape;
constexpr double kPi = 3.14159265358979323846;
constexpr double kRecordHighShelf = 4.0;  ///< the record pre-emphasis's largest HF boost (+12 dB)
constexpr double kRecordLowShelf = 0.2;   ///< NAB's LF boost limit (+14 dB at DC)
constexpr double kTransformerCore = 0.035; ///< LF core saturation (about 0.5 % THD at 40 Hz, 0 dBFS)
constexpr double kLineAmpHardness = 8.0;

/** Every A800 in the process, for groups (message thread). */
std::mutex& groupMutex()
{
    static std::mutex mutex;
    return mutex;
}
std::vector<TapeA800*>& machines()
{
    static std::vector<TapeA800*> list;
    return list;
}

/** A smooth saturation, linear at first: x / (1 + |x|^p)^(1/p). */
inline double saturate(double x, double hardness) noexcept
{
    const double magnitude = std::abs(x);
    if (magnitude < 1.0e-6)
        return x;
    return x / std::pow(1.0 + std::pow(magnitude, hardness), 1.0 / hardness);
}

/** A line amplifier: clean until close to its clip level, then rounded into it. */
inline double lineAmp(double x, double clip) noexcept
{
    return clip * saturate(x / clip, kLineAmpHardness);
}

struct HeadResponse
{
    double highPass, highPassQ, bump, bumpGain, bumpQ, dip, dipGain, dipQ, lowPass, lowPassQ;
};

/** The heads and tape at each speed (small signal), tuned to the machine's specification. */
HeadResponse headResponse(int speed, bool syncHead, const TapeA800::Formula& formula) noexcept
{
    constexpr std::array<HeadResponse, 3> repro{{
        {20.0, 0.62, 38.0, 1.1, 1.1, 85.0, -0.5, 1.4, 23500.0, 0.62},    // 7.5 ips
        {15.0, 0.65, 55.0, 1.0, 1.1, 125.0, -0.45, 1.4, 32000.0, 0.62},  // 15 ips
        {27.0, 0.62, 100.0, 0.9, 1.1, 230.0, -0.4, 1.4, 34000.0, 0.62},  // 30 ips
    }};
    auto r = repro[static_cast<std::size_t>(std::clamp(speed, 0, 2))];
    if (syncHead)
    {
        // The record head as a playback head: a wider gap (fewer highs) and a stronger contour
        // ripple in the lows.
        r.highPass *= 0.9;
        r.bumpGain += 0.6;
        r.dipGain -= 0.4;
        r.lowPass *= 0.5;
    }
    r.bumpGain *= formula.bump;
    r.dipGain *= formula.bump;
    r.lowPass *= formula.top;
    return r;
}

/** A first-order analog section (n1 s + n0) / (d1 s + d0), bilinear at `rate`. */
struct Analog
{
    double n1, n0, d1, d0;
};
} // namespace

void TapeA800::Channel::reset() noexcept
{
    *this = Channel{};
}

const TapeA800::Formula& TapeA800::formulaOf(int index) noexcept
{
    //                                           MOL  hard  knee    hiss  bump  top
    static const std::array<Formula, 4> formulas{{{"Vintage", 8.0, 1.6, 0.4124, -58.0, 1.3, 0.85},
                                                  {"Classic", 11.0, 2.0, 0.5254, -62.0, 1.0, 1.0},
                                                  {"Hi-Output", 14.0, 2.3, 0.5943, -64.0, 0.9, 1.05},
                                                  {"Mastering", 16.0, 2.6, 0.6520, -66.0, 0.8, 1.1}}};
    return formulas[static_cast<std::size_t>(std::clamp(index, 0, 3))];
}

TapeA800::Emphasis TapeA800::emphasisOf(int speed, int standard) noexcept
{
    if (speed >= 2)
        return {35.0e-6, 0.0, "AES"};
    if (standard == 0)
        return {50.0e-6, 3180.0e-6, "NAB"};
    return {speed == 1 ? 35.0e-6 : 70.0e-6, 0.0, "CCIR"};
}

double TapeA800::inchesPerSecond(int speed) noexcept
{
    constexpr std::array<double, 3> ips{7.5, 15.0, 30.0};
    return ips[static_cast<std::size_t>(std::clamp(speed, 0, 2))];
}

TapeA800::TapeA800()
    : BuiltinProcessor(kTypeId, "Studer A800 Multichannel Tape Recorder",
                       BusesProperties()
                           .withInput("Input", juce::AudioChannelSet::stereo(), true)
                           .withOutput("Output", juce::AudioChannelSet::stereo(), true),
                       false)
{
    path_ = addChoice("path", "Path", {"Input", "Sync", "Repro", "Thru"}, repro);
    autoInput_ = addBool("autoinput", "Auto Input", false);
    speed_ = addChoice("speed", "Tape Speed", {"7.5 ips", "15 ips", "30 ips"}, 1);
    standard_ = addChoice("eq", "EQ Standard", {"NAB", "CCIR"}, 0);
    formula_ = addChoice("formula", "Tape Formula", {"Vintage", "Classic", "Hi-Output", "Mastering"}, 1);
    calibration_ = addChoice("cal", "Calibration", {"+3 dB", "+6 dB", "+9 dB"}, 1);
    input_ = addFloat("input", "Input", {-12.0f, 18.0f, 0.1f}, 0.0f, "dB");
    output_ = addFloat("output", "Output", {-18.0f, 12.0f, 0.1f}, 0.0f, "dB");
    bias_ = addFloat("bias", "Bias", {-3.0f, 3.0f, 0.1f}, 0.0f, "dB");
    reproHigh_ = addFloat("hf", "Repro HF", {-3.0f, 3.0f, 0.1f}, 0.0f, "dB");
    reproLow_ = addFloat("lf", "Repro LF", {-3.0f, 3.0f, 0.1f}, 0.0f, "dB");
    noise_ = addBool("noise", "Noise", false);
    group_ = addChoice("group", "Group", {"Off", "1", "2", "3", "4"}, 0, false);
    osRealtime_ = addChoice("osrealtime", "Oversampling", dsp::PhaseOversampler::factorNames(), 1, false);
    osOffline_ = addChoice("osoffline", "Offline Oversampling", dsp::PhaseOversampler::factorNames(), 2, false);
    for (auto* parameter : getParameters())
        parameter->addListener(this);
    const std::lock_guard lock(groupMutex());
    machines().push_back(this);
}

TapeA800::~TapeA800()
{
    {
        const std::lock_guard lock(groupMutex());
        auto& list = machines();
        list.erase(std::remove(list.begin(), list.end(), this), list.end());
    }
    for (auto* parameter : getParameters())
        parameter->removeListener(this);
}

int TapeA800::factorChoice() const noexcept
{
    return isNonRealtime() ? osOffline_->getIndex() : osRealtime_->getIndex();
}

int TapeA800::currentLatency() const noexcept
{
    return oversampler_.latencyOf(factorChoice(), 0); // (minimum phase: the lowest latency)
}

int TapeA800::latencyFor(bool offline) const noexcept
{
    return oversampler_.latencyOf((offline ? osOffline_ : osRealtime_)->getIndex(), 0);
}

void TapeA800::setNonRealtime(bool isNonRealtime) noexcept
{
    BuiltinProcessor::setNonRealtime(isNonRealtime);
    setLatencySamples(currentLatency());
}

void TapeA800::setStateInformation(const void* data, int sizeInBytes)
{
    loading_ = true; // (a saved machine keeps its own settings: nothing passes to its group)
    BuiltinProcessor::setStateInformation(data, sizeInBytes);
    loading_ = false;
}

void TapeA800::copySettingsFrom(const TapeA800& other)
{
    applyingGroup_ = true;
    const auto& mine = getParameters();
    const auto& theirs = other.getParameters();
    for (int i = 0; i < std::min(mine.size(), theirs.size()); ++i)
        if (mine[i] != group_)
            mine[i]->setValueNotifyingHost(theirs[i]->getValue());
    applyingGroup_ = false;
}

void TapeA800::parameterValueChanged(int index, float value)
{
    auto* parameter = getParameters()[index];
    if (parameter == osRealtime_ || parameter == osOffline_)
        setLatencySamples(currentLatency());

    // Groups: an edit on the message thread passes to every machine in the same group (automation
    // on the audio thread stays with its own machine).
    if (applyingGroup_ || loading_ || !juce::MessageManager::existsAndIsCurrentThread())
        return;
    const int group = group_->getIndex();
    if (group == 0)
        return;
    const std::lock_guard lock(groupMutex());
    for (auto* other : machines())
    {
        if (other == this || other->group_->getIndex() != group)
            continue;
        if (parameter == group_)
        {
            copySettingsFrom(*other); // joining a group: take its settings
            return;
        }
        other->applyingGroup_ = true;
        other->getParameters()[index]->setValueNotifyingHost(value);
        other->applyingGroup_ = false;
    }
}

void TapeA800::prepareDsp(double, int maximumBlockSize)
{
    oversampler_.prepare(maximumBlockSize);
    oversampler_.select(factorChoice(), 0);
    oversampler_.reset();
    for (auto& channel : channels_)
        channel.reset();
    filters_ = Filters{};
    for (auto& ramp : weightRamp_)
        ramp.assign(static_cast<std::size_t>(maximumBlockSize), 0.0);
    weights_ = {0.0, 0.0, 0.0, 0.0};
    weights_[static_cast<std::size_t>(path_->getIndex())] = 1.0;
    vuSquare_ = {0.0, 0.0};
    setLatencySamples(currentLatency());
}

void TapeA800::updateFilters(double rate) noexcept
{
    const int speed = speed_->getIndex();
    const int standard = standard_->getIndex();
    const int formulaIndex = formula_->getIndex();
    const float bias = bias_->get(), high = reproHigh_->get(), low = reproLow_->get();
    auto& f = filters_;
    const auto same = [](double a, double b) { return std::abs(a - b) < 1.0e-9; };
    if (same(f.rate, rate) && f.speed == speed && f.standard == standard && f.formula == formulaIndex && same(f.bias, bias) && same(f.high, high) &&
        same(f.low, low))
        return;
    f.rate = rate;
    f.speed = speed;
    f.standard = standard;
    f.formula = formulaIndex;
    f.bias = bias;
    f.high = high;
    f.low = low;

    const double k = 2.0 * rate;
    const auto bilinear = [k](Analog a)
    {
        const double norm = a.d1 * k + a.d0;
        return FirstOrder{(a.n1 * k + a.n0) / norm, (a.n0 - a.n1 * k) / norm, (a.d0 - a.d1 * k) / norm};
    };
    const auto corner = [](double hz) { return 2.0 * kPi * hz; };

    // The electronics: an input high-pass, the transformer core's low-frequency band, and the
    // line amplifier's ultrasonic low-pass.
    f.inputHighPass = bilinear({1.0, 0.0, 1.0, corner(4.0)});
    f.core = bilinear({0.0, corner(60.0), 1.0, corner(60.0)});
    f.ultrasonicOn = rate > 80000.0; // (at the base rate the band ends first)
    f.ultrasonic = dsp::BiquadCoefficients::make(Shape::highCut, rate, 0.45 * rate, 0.7071, 0.0);

    // Record pre-emphasis (HF, and NAB's LF), and its exact inverse for playback.
    const auto emphasis = emphasisOf(speed, standard);
    const double tauHigh = emphasis.highSeconds;
    f.recordHigh = bilinear({tauHigh, 1.0, tauHigh / kRecordHighShelf, 1.0});
    f.recordLow = emphasis.lowSeconds > 0.0 ? bilinear({emphasis.lowSeconds, 1.0, emphasis.lowSeconds, kRecordLowShelf}) : FirstOrder{};
    f.playHigh = f.recordHigh.inverse();
    f.playLow = f.recordLow.inverse();

    // The heads.
    const auto& formula = formulaOf(formulaIndex);
    const auto heads = [&](bool syncHead, std::array<dsp::BiquadCoefficients, 4>& out, bool& gapLoss)
    {
        const auto r = headResponse(speed, syncHead, formula);
        out[0] = dsp::BiquadCoefficients::make(Shape::lowCut, rate, r.highPass, r.highPassQ, 0.0);
        out[1] = dsp::BiquadCoefficients::make(Shape::peak, rate, r.bump, r.bumpQ, r.bumpGain);
        out[2] = dsp::BiquadCoefficients::make(Shape::peak, rate, r.dip, r.dipQ, r.dipGain);
        gapLoss = r.lowPass < 0.4 * rate; // (above, the band ends first)
        out[3] = gapLoss ? dsp::BiquadCoefficients::make(Shape::highCut, rate, r.lowPass, r.lowPassQ, 0.0) : dsp::BiquadCoefficients{};
    };
    heads(false, f.reproHead, f.reproGapLoss);
    heads(true, f.syncHead, f.syncGapLoss);

    // Bias: over-bias loses highs (more at lower speeds), under-bias gives a little more.
    constexpr std::array<double, 3> shelfHz{6000.0, 8000.0, 10000.0};
    constexpr std::array<double, 3> lossPerDb{2.0, 1.2, 0.7};
    const auto s = static_cast<std::size_t>(std::clamp(speed, 0, 2));
    const double biasGain = bias > 0.0f ? -bias * lossPerDb[s] : -bias * 0.5 * lossPerDb[s];
    f.biasShelf = dsp::BiquadCoefficients::make(Shape::highShelf, rate, std::min(shelfHz[s], 0.4 * rate), 0.7071, biasGain);
    f.trimHigh = dsp::BiquadCoefficients::make(Shape::highShelf, rate, std::min(10000.0, 0.4 * rate), 0.7071, high);
    f.trimLow = dsp::BiquadCoefficients::make(Shape::lowShelf, rate, 100.0, 0.7071, low);
}

void TapeA800::processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer&) noexcept
{
    const int numSamples = buffer.getNumSamples();
    if (numSamples <= 0)
        return;
    double* channels[2] = {buffer.getWritePointer(0), buffer.getWritePointer(1)};

    // The transport (Auto Input switches the tape paths to Input while it is stopped).
    bool rolling = false;
    if (auto* head = getPlayHead())
        if (const auto position = head->getPosition())
            rolling = position->getIsPlaying();
    rolling_.store(rolling, std::memory_order_relaxed);
    int target = path_->getIndex();
    if (autoInput_->get() && !rolling && (target == sync || target == repro))
        target = input;
    activePath_.store(target, std::memory_order_relaxed);

    // Each path's share, sample by sample (20 ms crossfades).
    const double step = 1.0 / (kCrossfadeSeconds * sampleRate_);
    for (int i = 0; i < numSamples; ++i)
    {
        double total = 0.0;
        for (std::size_t p = 0; p < 4; ++p)
        {
            const double goal = static_cast<int>(p) == target ? 1.0 : 0.0;
            weights_[p] = goal > weights_[p] ? std::min(goal, weights_[p] + step) : std::max(goal, weights_[p] - step);
            total += weights_[p];
        }
        for (std::size_t p = 0; p < 4; ++p)
            weightRamp_[p][static_cast<std::size_t>(i)] = weights_[p] / std::max(total, 1.0e-12);
    }

    oversampler_.select(factorChoice(), 0);
    const int multiple = oversampler_.getRateMultiple();
    const double rate = sampleRate_ * multiple;
    updateFilters(rate);
    const auto& f = filters_;

    const auto& formula = formulaOf(formula_->getIndex());
    const double calibration = 3.0 * (calibration_->getIndex() + 1);
    const double bias = bias_->get();
    // The tape: 3 % THD where the flux reaches the formula's maximum output level (under-bias
    // lowers it, over-bias raises it a little).
    const double molDbfs = kOperatingLevelDbfs + formula.molDb - calibration + (bias < 0.0 ? 1.5 * bias : 0.5 * bias);
    const double tapeScale = formula.knee / dsp::dbToGain(molDbfs);
    const double inputGain = dsp::dbToGain(input_->get());
    const double outputGain = dsp::dbToGain(output_->get());
    const double inputClip = dsp::dbToGain(kInputClipDbfs + 1.0), outputClip = dsp::dbToGain(kOutputClipDbfs + 1.0);
    const bool noise = noise_->get();
    constexpr std::array<double, 3> noiseBySpeed{7.0, 4.0, 2.0}; // (calibrated: about -88 dBFS at 15 ips, Classic, +6 dB)
    const double hiss = dsp::dbToGain(kOperatingLevelDbfs + formula.noiseDb - calibration + noiseBySpeed[static_cast<std::size_t>(speed_->getIndex())]) *
                        std::sqrt(3.0 * multiple); // (white noise over the oversampled band)
    const bool anyProcessing = weightRamp_[thru][0] < 1.0 || weightRamp_[thru][static_cast<std::size_t>(numSamples - 1)] < 1.0;

    const auto process = [&](double** data, std::size_t count) noexcept
    {
        if (!anyProcessing)
            return;
        for (std::size_t j = 0; j < count; ++j)
        {
            const auto base = j / static_cast<std::size_t>(multiple);
            const double wInput = weightRamp_[input][base], wSync = weightRamp_[sync][base], wRepro = weightRamp_[repro][base];
            const double processed = wInput + wSync + wRepro;
            if (processed <= 0.0)
                continue;
            const bool tape = wSync > 0.0 || wRepro > 0.0;
            for (std::size_t ch = 0; ch < 2; ++ch)
            {
                auto& c = channels_[ch];
                // Input electronics: transformer (its core saturates on the lows first), line amp.
                double x = c.inputHighPass.process(data[ch][j] * inputGain, f.inputHighPass);
                const double lows = c.inputCore.process(x, f.core);
                x -= kTransformerCore * lows * lows * lows;
                x = lineAmp(x, inputClip);
                if (f.ultrasonicOn)
                    x = c.ultrasonic.process(x, f.ultrasonic);

                double y = x * wInput;
                if (tape)
                {
                    // Record: pre-emphasis, bias and the tape's magnetisation, then (with Noise) hiss.
                    double r = c.recordLow.process(c.recordHigh.process(x, f.recordHigh), f.recordLow);
                    r = saturate(r * tapeScale, formula.hardness) / tapeScale;
                    if (noise)
                    {
                        noiseState_ = noiseState_ * 1664525u + 1013904223u;
                        r += hiss * (static_cast<double>(noiseState_) / 4294967295.0 * 2.0 - 1.0);
                    }
                    r = c.biasShelf.process(r, f.biasShelf);
                    // Playback off the record head (Sync) or the reproduce head (Repro).
                    const auto play = [&](std::array<dsp::Biquad, 4>& head, const std::array<dsp::BiquadCoefficients, 4>& response, bool gapLoss)
                    {
                        double p = head[2].process(head[1].process(head[0].process(r, response[0]), response[1]), response[2]);
                        if (gapLoss)
                            p = head[3].process(p, response[3]);
                        return p;
                    };
                    // (The playback EQ is linear: one de-emphasis serves both heads' mix while
                    // they crossfade.)
                    double heads = 0.0;
                    if (wSync > 0.0)
                        heads += wSync * play(c.syncHead, f.syncHead, f.syncGapLoss);
                    if (wRepro > 0.0)
                    {
                        const double p = play(c.reproHead, f.reproHead, f.reproGapLoss);
                        heads += wRepro * c.trimLow.process(c.trimHigh.process(p, f.trimHigh), f.trimLow);
                    }
                    y += c.playLow.process(c.playHigh.process(heads, f.playHigh), f.playLow);
                }
                // Output: playback gain, line driver and transformer.
                y = y / processed * outputGain;
                const double outLows = c.outputCore.process(y, f.core);
                y -= kTransformerCore * 0.5 * outLows * outLows * outLows;
                data[ch][j] = lineAmp(y, outputClip);
            }
        }
    };

    oversampler_.process(channels, numSamples, process);

    // Thru: the input itself (delayed like the rest), blended in while switching to or from it.
    for (int ch = 0; ch < 2; ++ch)
    {
        double* out = channels[ch];
        for (int i = 0; i < numSamples; ++i)
        {
            const double wThru = weightRamp_[thru][static_cast<std::size_t>(i)];
            if (wThru > 0.0)
                out[i] = wThru >= 1.0 ? oversampler_.dry(ch, i) : out[i] * (1.0 - wThru) + oversampler_.dry(ch, i) * wThru;
        }
    }

    // VU meters (0 VU = -18 dBFS, a sine's level; VU ballistics).
    const double vu = 1.0 - std::exp(-1.0 / (0.065 * sampleRate_));
    for (std::size_t ch = 0; ch < 2; ++ch)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const double s = channels[ch][i];
            vuSquare_[ch] += (s * s - vuSquare_[ch]) * vu;
        }
        vu_[ch].store(static_cast<float>(dsp::gainToDb(std::sqrt(2.0 * std::max(0.0, vuSquare_[ch]))) - kOperatingLevelDbfs), std::memory_order_relaxed);
    }
}

} // namespace dojo::devices
