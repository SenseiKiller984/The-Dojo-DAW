#include "devices/TheGlue.h"

#include "devices/dsp/Biquad.h"
#include "devices/dsp/DynamicsMath.h"

#include <algorithm>
#include <cmath>

namespace dojo::devices
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kDiodeSoftness = 0.25; ///< dB: the diode's soft turn-on
constexpr double kAutoHoldSeconds = 0.5; ///< Auto release: the slow capacitor charges over this ...
constexpr double kAutoReleaseSeconds = 1.2; ///< ... and lets go over this
constexpr double kVuSeconds = 0.065;        ///< VU ballistics (99 % in 300 ms)
constexpr double kConductingSeconds = 0.015; ///< Auto release: compression counts as demanded this long after the diode conducted
constexpr double kLn10Over20 = 0.11512925464970228;

/** The diode: no current below zero, a soft turn-on, then (almost) linear. */
inline double diode(double e) noexcept
{
    return e > 0.0 ? e * e / (e + kDiodeSoftness) : 0.0;
}
inline double diodeSlope(double e) noexcept
{
    if (e <= 0.0)
        return 0.0;
    const double d = e + kDiodeSoftness;
    return e * (e + 2.0 * kDiodeSoftness) / (d * d);
}

} // namespace

TheGlue::TheGlue()
    : BuiltinProcessor(kTypeId, "The Glue",
                       BusesProperties()
                           .withInput("Input", juce::AudioChannelSet::stereo(), true)
                           .withOutput("Output", juce::AudioChannelSet::stereo(), true)
                           .withInput("Sidechain", juce::AudioChannelSet::stereo(), true),
                       false)
{
    threshold_ = addFloat("threshold", "Threshold", {-40.0f, 0.0f, 0.1f}, -10.0f, "dB");
    ratio_ = addChoice("ratio", "Ratio", {"2:1", "4:1", "10:1"}, 1);
    attack_ = addChoice("attack", "Attack", {"0.01 ms", "0.1 ms", "0.3 ms", "1 ms", "3 ms", "10 ms", "30 ms"}, 5);
    release_ = addChoice("release", "Release", {"0.1 s", "0.2 s", "0.4 s", "0.6 s", "0.8 s", "1.2 s", "Auto"}, 6);
    makeup_ = addFloat("makeup", "Makeup", {0.0f, 20.0f, 0.1f}, 0.0f, "dB");
    mix_ = addFloatWithText("mix", "Mix", {0.0f, 100.0f, 0.1f}, 100.0f, {}, mixText, mixFromText);
    range_ = addFloatWithText(
        "range", "Range", {0.0f, static_cast<float>(kRangeFull), 0.1f}, static_cast<float>(kRangeFull), {},
        [](float value) { return value >= kRangeFull - 0.05 ? juce::String("Full") : juce::String(value, 1) + " dB"; },
        [](const juce::String& text)
        {
            if (text.trim().startsWithIgnoreCase("full"))
                return static_cast<float>(kRangeFull);
            return static_cast<float>(std::clamp(parseParameterText(text, "dB"), 0.0, kRangeFull));
        });
    scHighPass_ = addFloatWithText(
        "schp", "SC High Pass", skewedRange(20.0f, 2000.0f, 150.0f), 20.0f, {},
        [](float value)
        {
            if (value <= 20.5f)
                return juce::String("Off");
            return value >= 1000.0f ? juce::String(value / 1000.0f, 2) + " kHz" : juce::String(juce::roundToInt(value)) + " Hz";
        },
        [](const juce::String& text)
        {
            if (text.trim().startsWithIgnoreCase("off"))
                return 20.0f;
            return static_cast<float>(std::clamp(parseParameterText(text, "Hz"), 20.0, 2000.0));
        });
    external_ = addBool("sidechain", "External Sidechain", false);
    peakClip_ = addBool("peakclip", "Peak Clip", false);
    osRealtime_ = addChoice("osrealtime", "Oversampling", dsp::PhaseOversampler::factorNames(), 1, false);
    osOffline_ = addChoice("osoffline", "Offline Oversampling", dsp::PhaseOversampler::factorNames(), 3, false);
    osPhase_ = addChoice("osphase", "Oversampling Phase", dsp::PhaseOversampler::phaseNames(), 2, false);
    for (auto* parameter : {static_cast<juce::AudioProcessorParameter*>(osRealtime_), static_cast<juce::AudioProcessorParameter*>(osOffline_),
                            static_cast<juce::AudioProcessorParameter*>(osPhase_)})
        parameter->addListener(this);
}

TheGlue::~TheGlue()
{
    for (auto* parameter : {static_cast<juce::AudioProcessorParameter*>(osRealtime_), static_cast<juce::AudioProcessorParameter*>(osOffline_),
                            static_cast<juce::AudioProcessorParameter*>(osPhase_)})
        parameter->removeListener(this);
}

juce::String TheGlue::mixText(float percent)
{
    if (percent <= 0.05f)
        return "-inf dB";
    const double db = 20.0 * std::log10(percent / 100.0);
    return juce::String(std::abs(db) < 0.05 ? 0.0 : db, 1) + " dB";
}

float TheGlue::mixFromText(const juce::String& text)
{
    const auto t = text.trim().toLowerCase();
    if (t.contains("inf"))
        return 0.0f;
    const double number = parseParameterText(t, "dB");
    if (t.contains("%"))
        return static_cast<float>(std::clamp(number, 0.0, 100.0));
    return static_cast<float>(std::clamp(100.0 * std::pow(10.0, number / 20.0), 0.0, 100.0));
}

double TheGlue::ratioOf(int choice) noexcept
{
    constexpr std::array<double, 3> ratios{2.0, 4.0, 10.0};
    return ratios[static_cast<std::size_t>(std::clamp(choice, 0, 2))];
}

double TheGlue::attackMsOf(int choice) noexcept
{
    constexpr std::array<double, 7> ms{0.01, 0.1, 0.3, 1.0, 3.0, 10.0, 30.0};
    return ms[static_cast<std::size_t>(std::clamp(choice, 0, 6))];
}

double TheGlue::releaseSecondsOf(int choice) noexcept
{
    constexpr std::array<double, 7> seconds{0.1, 0.2, 0.4, 0.6, 0.8, 1.2, -1.0};
    return seconds[static_cast<std::size_t>(std::clamp(choice, 0, 6))];
}

double TheGlue::peakClip(double x) noexcept
{
    const double magnitude = std::abs(x);
    if (magnitude <= kPeakClipKnee)
        return x;
    const double span = kPeakClipCeiling - kPeakClipKnee;
    return std::copysign(kPeakClipKnee + span * std::tanh((magnitude - kPeakClipKnee) / span), x);
}

double TheGlue::reductionForDetector(double detectorDb) const noexcept
{
    return (ratioOf(ratio_->getIndex()) - 1.0) * softOver(detectorDb - threshold_->get(), kKneeDb);
}

double TheGlue::staticOutputDb(double inputDb) const noexcept
{
    // Feedback: the reduction u satisfies u = reduction(input - u); u - reduction(input - u) rises
    // with u, so bisect.
    double lo = 0.0, hi = std::max(0.0, (inputDb - threshold_->get() + kKneeDb) * (ratioOf(ratio_->getIndex()) - 1.0));
    for (int i = 0; i < 50; ++i)
    {
        const double u = 0.5 * (lo + hi);
        if (u - reductionForDetector(inputDb - u) > 0.0)
            hi = u;
        else
            lo = u;
    }
    return inputDb - 0.5 * (lo + hi);
}

void TheGlue::writeExtraState(juce::XmlElement& extra) const
{
    extra.setAttribute("peakNeedle", isPeakNeedleShown());
}

void TheGlue::readExtraState(const juce::XmlElement& extra)
{
    setPeakNeedleShown(extra.getBoolAttribute("peakNeedle", false));
}

int TheGlue::factorChoice() const noexcept
{
    return isNonRealtime() ? osOffline_->getIndex() : osRealtime_->getIndex();
}

int TheGlue::currentLatency() const noexcept
{
    return oversampler_.latencyOf(factorChoice(), osPhase_->getIndex());
}

void TheGlue::setNonRealtime(bool isNonRealtime) noexcept
{
    BuiltinProcessor::setNonRealtime(isNonRealtime);
    setLatencySamples(currentLatency()); // (offline renders may use another oversampling)
}

void TheGlue::parameterValueChanged(int, float)
{
    setLatencySamples(currentLatency());
}

void TheGlue::prepareDsp(double, int maximumBlockSize)
{
    oversampler_.prepare(maximumBlockSize);
    oversampler_.select(factorChoice(), osPhase_->getIndex());
    oversampler_.reset();
    hpState_.fill(0.0);
    hpOut_.fill(0.0);
    sidePrevious_.fill(0.0);
    envelope_ = slowEnvelope_ = reductionSquare_ = conducting_ = 0.0;
    setLatencySamples(currentLatency());
}

void TheGlue::processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer&) noexcept
{
    const int numSamples = buffer.getNumSamples();
    if (numSamples <= 0)
        return;
    double* channels[2] = {buffer.getWritePointer(0), buffer.getWritePointer(1)};
    const bool external = external_->get() && buffer.getNumChannels() >= 4;
    const double* side[2] = {external ? buffer.getReadPointer(2) : nullptr, external ? buffer.getReadPointer(3) : nullptr};

    oversampler_.select(factorChoice(), osPhase_->getIndex());
    const int multiple = oversampler_.getRateMultiple();
    const double rate = sampleRate_ * multiple;
    const double h = 1.0 / rate;

    const double slope = ratioOf(ratio_->getIndex()) - 1.0;
    const double threshold = threshold_->get();
    const double tauA = attackMsOf(attack_->getIndex()) / 1000.0;
    const double releaseSeconds = releaseSecondsOf(release_->getIndex());
    const bool autoRelease = releaseSeconds < 0.0;
    const double tauR = autoRelease ? 0.1 : releaseSeconds;
    const double holdCharge = 1.0 - std::exp(-h / kAutoHoldSeconds);
    const double holdRelease = 1.0 - std::exp(-h / kAutoReleaseSeconds);
    const double vu = 1.0 - std::exp(-h / kVuSeconds);

    // Range: the sidechain saturates at the level that gives Range dB of reduction.
    const double rangeDb = range_->get();
    const bool saturate = rangeDb < kRangeFull - 0.05;
    // (Where the knee gives exactly Range dB: invert the soft knee.)
    const double rangeOver = rangeDb / slope;
    const double clipOver = rangeOver >= 0.5 * kKneeDb ? rangeOver : std::sqrt(2.0 * kKneeDb * rangeOver) - 0.5 * kKneeDb;
    const double clipLevel = dsp::dbToGain(threshold + clipOver);
    // (Below the knee's start nothing happens: no logarithms needed.)
    const double kneeStart = dsp::dbToGain(threshold - 0.5 * kKneeDb);

    // Sidechain high-pass: first order (-6 dB/oct), bilinear.
    const bool highPass = scHighPass_->get() > 20.5f;
    const double k = std::tan(kPi * std::min(static_cast<double>(scHighPass_->get()), 0.45 * rate) / rate);
    const double hpB0 = 1.0 / (1.0 + k);
    const double hpA1 = (k - 1.0) / (1.0 + k);

    const double makeup = dsp::dbToGain(makeup_->get());
    const double wet = mix_->get() / 100.0;
    const bool clip = peakClip_->get();
    double peakReduction = 0.0;

    /** The detector's target (dB of reduction) when the gain cell is at `reduction` dB, and its slope. */
    const auto target = [&](double magnitude, double reduction, double& derivative) noexcept
    {
        derivative = 0.0;
        const double z = magnitude * std::exp(-kLn10Over20 * reduction);
        if (z <= kneeStart)
            return 0.0;
        double level = z, levelSlope = 1.0; // d(dB(level)) / d(dB(z))
        if (saturate)
        {
            // A soft clip of the detected signal (rounding only near the clip level).
            const double r2 = (z / clipLevel) * (z / clipLevel);
            const double r4 = r2 * r2;
            const double q = r4 * r4;
            level = z / std::sqrt(std::sqrt(std::sqrt(1.0 + q)));
            levelSlope = 1.0 / (1.0 + q);
        }
        const double over = dsp::gainToDb(level) - threshold;
        derivative = -slope * softOverSlope(over, kKneeDb) * levelSlope;
        return slope * softOver(over, kKneeDb);
    };

    const auto shape = [&](double** data, std::size_t count) noexcept
    {
        for (std::size_t j = 0; j < count; ++j)
        {
            const double x[2] = {data[0][j], data[1][j]};
            double s[2] = {x[0], x[1]};
            if (external)
            {
                // The sidechain comes in at the base rate: interpolate it.
                const auto base = static_cast<int>(j) / multiple;
                const double t = static_cast<double>(static_cast<int>(j) % multiple + 1) / multiple;
                for (std::size_t ch = 0; ch < 2; ++ch)
                {
                    const double previous = base == 0 ? sidePrevious_[ch] : side[ch][base - 1];
                    s[ch] = previous + (side[ch][base] - previous) * t;
                }
            }
            if (highPass)
                for (std::size_t ch = 0; ch < 2; ++ch)
                {
                    const double y = hpB0 * (s[ch] - hpState_[ch]) - hpA1 * hpOut_[ch];
                    hpState_[ch] = s[ch];
                    hpOut_[ch] = y;
                    s[ch] = y;
                }
            const double magnitude = std::max(std::abs(s[0]), std::abs(s[1]));

            // The detector capacitor (holding the control voltage, in dB of reduction): charged
            // through the diode towards the target, discharged through the release. The loop is
            // closed (the target depends on the reduction it sets), so solve it implicitly:
            //   u - u0 = h * (diode(target(u) - u) / tauA - u / tauR)
            const double u0 = envelope_;
            const double held = autoRelease ? slowEnvelope_ : 0.0;
            const double released = u0 / (1.0 + h / tauR);
            double unused = 0.0;
            double u = released;
            if (magnitude > kneeStart && target(magnitude, std::max(released, held), unused) > released)
            {
                conducting_ = kConductingSeconds;
                double lo = released;
                double hi = std::max(released, target(magnitude, std::max(lo, held), unused));
                for (int iteration = 0; iteration < 8; ++iteration)
                {
                    double derivative = 0.0;
                    const double t = target(magnitude, std::max(u, held), derivative);
                    if (u < held)
                        derivative = 0.0;
                    const double e = t - u;
                    const double f = u - u0 - h * (diode(e) / tauA - u / tauR);
                    const double fSlope = 1.0 + h * (diodeSlope(e) * (1.0 - derivative) / tauA + 1.0 / tauR);
                    if (f < 0.0)
                        lo = u;
                    else
                        hi = u;
                    double next = u - f / fSlope;
                    if (!(next > lo && next < hi))
                        next = 0.5 * (lo + hi);
                    const bool done = std::abs(next - u) < 1.0e-9;
                    u = next;
                    if (done)
                        break;
                }
            }
            envelope_ = u;
            conducting_ -= h;
            // Auto release: the slow capacitor charges only while compression is being demanded
            // (the diode conducted within the last cycles), and lets go slowly.
            if (autoRelease)
            {
                if (u > slowEnvelope_)
                {
                    if (conducting_ > 0.0)
                        slowEnvelope_ += (u - slowEnvelope_) * holdCharge;
                }
                else
                {
                    slowEnvelope_ += (u - slowEnvelope_) * holdRelease;
                }
            }
            const double reduction = std::max(u, held);
            peakReduction = std::max(peakReduction, reduction);
            reductionSquare_ += (reduction * reduction - reductionSquare_) * vu;

            const double gain = std::exp(-kLn10Over20 * reduction) * makeup;
            for (std::size_t ch = 0; ch < 2; ++ch)
            {
                double y = x[ch] * (1.0 - wet) + x[ch] * gain * wet; // (phase accurate: same domain)
                if (clip)
                    y = peakClip(y);
                data[ch][j] = y;
            }
        }
    };

    oversampler_.process(channels, numSamples, shape);
    if (external)
        for (std::size_t ch = 0; ch < 2; ++ch)
            sidePrevious_[ch] = side[ch][numSamples - 1];

    rmsReduction_.store(static_cast<float>(std::sqrt(std::max(0.0, reductionSquare_))), std::memory_order_relaxed);
    storeMax(peakReduction_, static_cast<float>(peakReduction));
}

} // namespace dojo::devices
