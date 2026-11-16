#include "devices/ConsoleTG12345.h"

#include <algorithm>
#include <cmath>

namespace dojo::devices
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kDbToNeper = 0.11512925464970229; // ln(10) / 20
constexpr double kOffDb = -80.0;                    // (a level control's bottom: off)
constexpr double kVcaCurvature = 0.05;              // the gain cell's 2nd harmonic, by the gain reduction
constexpr double kToneHz = 1000.0;

double dbToGain(double db) noexcept
{
    return std::exp(db * kDbToNeper);
}

double levelGain(const juce::AudioParameterFloat& parameter) noexcept
{
    const double db = parameter.get();
    return db <= kOffDb + 0.05 ? 0.0 : dbToGain(db);
}

/** The peak (dBFS-linear) of a sine at `dbu`. */
double railOf(double dbu) noexcept
{
    return dbToGain(dbu + ConsoleTG12345::kDbuToDbfs);
}

/** A card's transfer: its curvature, then the soft approach to its rails. */
double shape(double x, double rail, double second, double third) noexcept
{
    const double u = std::clamp(x / rail, -2.5, 2.5);
    double y = u + second * u * u - third * u * u * u;
    constexpr double knee = 0.75;
    const double magnitude = std::abs(y);
    if (magnitude > knee)
        y = std::copysign(knee + (1.0 - knee) * std::tanh((magnitude - knee) / (1.0 - knee)), y);
    return y * rail;
}

juce::String levelText(float value)
{
    if (value <= static_cast<float>(kOffDb) + 0.05f)
        return "Off";
    return (value > 0.0f ? "+" : "") + juce::String(value, 1) + " dB";
}

float levelFromText(const juce::String& text)
{
    const auto t = text.trim().toLowerCase();
    if (t.startsWith("off") || t.contains("inf"))
        return static_cast<float>(kOffDb);
    return static_cast<float>(parseParameterText(text, "dB"));
}

juce::String panText(float value)
{
    const int position = juce::roundToInt(value);
    if (position == 0)
        return "C";
    return (position < 0 ? "L" : "R") + juce::String(std::abs(position));
}

float panFromText(const juce::String& text)
{
    const auto t = text.trim().toUpperCase();
    if (t == "C" || t.startsWith("CENT"))
        return 0.0f;
    const auto number = static_cast<float>(std::abs(parseParameterText(t.retainCharacters("0123456789.,-"), {})));
    if (t.startsWith("L"))
        return -number;
    if (t.startsWith("R"))
        return number;
    return static_cast<float>(parseParameterText(text, {}));
}

juce::StringArray stepNames(const double* steps, std::size_t count)
{
    juce::StringArray names;
    for (std::size_t i = 0; i < count; ++i)
        names.add((steps[i] > 0.0 ? "+" : "") + juce::String(static_cast<int>(steps[i])) + " dB");
    return names;
}

juce::StringArray eqNames()
{
    juce::StringArray names;
    for (int i = 0; i < ConsoleTG12345::kEqSteps; ++i)
    {
        const double db = ConsoleTG12345::eqStepDb(i);
        names.add((db > 0.0 ? "+" : "") + juce::String(static_cast<int>(db)) + " dB");
    }
    return names;
}
} // namespace

//==================================================================================================

double ConsoleTG12345::amplify(double x, const AmpCard& card) noexcept
{
    return shape(x, railOf(card.railDbu), card.second, card.third);
}

std::pair<double, double> ConsoleTG12345::panGains(double pan) noexcept
{
    // Each side: an 11 kOhm pot as a divider (the wiper at `a`), into a 22 kOhm mixing resistor
    // to the bus's virtual earth; the two pots turn in opposite directions.
    constexpr double r = kMixingOhms / kPanPotOhms;
    const auto side = [](double a) { return a * r / (r + a * (1.0 - a)); };
    pan = std::clamp(pan, -1.0, 1.0);
    return {side(0.5 * (1.0 - pan)), side(0.5 * (1.0 + pan))};
}

juce::StringArray ConsoleTG12345::cassetteNames()
{
    return {"Microphone", "Group", "Main", "Echo / Aux", "Control Room & Studio"};
}

ConsoleTG12345::ConsoleTG12345()
    : BuiltinProcessor(kTypeId, "EMI TG12345",
                       BusesProperties()
                           .withInput("Input", juce::AudioChannelSet::stereo(), true)
                           .withOutput("Output", juce::AudioChannelSet::stereo(), true)
                           .withInput("Talkback", juce::AudioChannelSet::stereo(), true),
                       false)
{
    cassette_ = addChoice("cassette", "Cassette", cassetteNames(), microphone, false);
    mark_ = addChoice("mark", "Model", {"Mk I", "Mk II"}, 0, false);
    input_ = addChoice("input", "Input", {"Stereo Pair", "Mono"}, 0, false);
    gain_ = addChoice("gain", "Input Gain", stepNames(kGainSteps.data(), kGainSteps.size()), 3);
    fine_ = addChoice("fine", "Fine Gain", stepNames(kFineSteps.data(), kFineSteps.size()), 2);
    mode_ = addChoice("mode", "Comp / Limit", {"Out", "Comp", "Limit"}, out);
    hold_ = addFloat("hold", "Hold", {-20.0f, 10.0f, 0.1f}, 0.0f, "dB");
    recovery_ = addChoice("recovery", "Recovery", {"100 ms", "250 ms", "500 ms", "1 s", "2 s", "5 s"}, 2);
    link_ = addBool("link", "Stereo Link", true);
    bass_ = addChoice("bass", "Bass", eqNames(), kEqSteps / 2);
    treble_ = addChoice("treble", "Treble", eqNames(), kEqSteps / 2);
    presenceFrequency_ = addChoice("presfreq", "Presence Freq",
                                   {"0.5 kHz", "0.8 kHz", "1.2 kHz", "1.8 kHz", "2.8 kHz", "4.2 kHz", "6.5 kHz", "10 kHz"}, 4);
    presence_ = addChoice("presence", "Presence", eqNames(), kEqSteps / 2);
    panA_ = addFloatWithText("pana", "Pan A", {-100.0f, 100.0f, 1.0f}, -100.0f, {}, panText, panFromText);
    panB_ = addFloatWithText("panb", "Pan B", {-100.0f, 100.0f, 1.0f}, 100.0f, {}, panText, panFromText);
    spread_ = addFloat("spread", "Spreader", {0.0f, 200.0f, 1.0f}, 100.0f, "%");
    const juce::NormalisableRange<float> level(static_cast<float>(kOffDb), 10.0f, 0.1f, 2.5f);
    fader_ = addFloatWithText("fader", "Fader", level, 0.0f, "dB", levelText, levelFromText);
    echoSend_ = addFloatWithText("echosend", "Echo Send", level, -10.0f, "dB", levelText, levelFromText);
    echoPre_ = addBool("echopre", "Echo Pre-Fader", false);
    echoReturn_ = addFloatWithText("echoreturn", "Echo Return", level, 0.0f, "dB", levelText, levelFromText);
    announce_ = addBool("announce", "Track Announce", false);
    announceSource_ = addChoice("annsource", "Announce Source", {"Talkback Mic", "Tone 1 kHz"}, 0);
    announceLevel_ = addFloatWithText("annlevel", "Announce Level", level, 0.0f, "dB", levelText, levelFromText);
    listen_ = addChoice("listen", "Listen", {"Control Room", "Studio"}, 0);
    controlRoom_ = addFloatWithText("crlevel", "Control Room", level, 0.0f, "dB", levelText, levelFromText);
    dim_ = addBool("dim", "Dim", false);
    mono_ = addBool("mono", "Mono", false);
    cut_ = addBool("cut", "Cut", false);
    studio_ = addFloatWithText("studiolevel", "Studio Playback", level, 0.0f, "dB", levelText, levelFromText);
    talkback_ = addBool("talkback", "Talkback", false);
    talkbackLevel_ = addFloatWithText("tblevel", "Talkback Level", level, 0.0f, "dB", levelText, levelFromText);
    osRealtime_ = addChoice("osrealtime", "Oversampling", dsp::PhaseOversampler::factorNames(), 1, false);
    osOffline_ = addChoice("osoffline", "Offline Oversampling", dsp::PhaseOversampler::factorNames(), 2, false);
    for (auto* parameter : {static_cast<juce::AudioProcessorParameter*>(cassette_), static_cast<juce::AudioProcessorParameter*>(osRealtime_),
                            static_cast<juce::AudioProcessorParameter*>(osOffline_)})
        parameter->addListener(this);
    for (auto& peak : outputPeak_)
        peak.store(-100.0f);
}

ConsoleTG12345::~ConsoleTG12345()
{
    for (auto* parameter : {static_cast<juce::AudioProcessorParameter*>(cassette_), static_cast<juce::AudioProcessorParameter*>(osRealtime_),
                            static_cast<juce::AudioProcessorParameter*>(osOffline_)})
        parameter->removeListener(this);
}

int ConsoleTG12345::factorChoice() const noexcept
{
    return isNonRealtime() ? osOffline_->getIndex() : osRealtime_->getIndex();
}

int ConsoleTG12345::currentLatency() const noexcept
{
    return oversampler_.latencyOf(factorChoice(), 0); // (minimum phase: the lowest latency)
}

int ConsoleTG12345::latencyFor(bool offline) const noexcept
{
    return oversampler_.latencyOf((offline ? osOffline_ : osRealtime_)->getIndex(), 0);
}

void ConsoleTG12345::setNonRealtime(bool isNonRealtime) noexcept
{
    BuiltinProcessor::setNonRealtime(isNonRealtime);
    setLatencySamples(currentLatency());
}

void ConsoleTG12345::parameterValueChanged(int, float)
{
    setLatencySamples(currentLatency());
    // The cassette decides whether the insert loop is in the signal path: the graph follows.
    updateHostDisplay(ChangeDetails().withLatencyChanged(true));
}

bool ConsoleTG12345::isLoopAvailable() const noexcept
{
    const int cassette = cassette_->getIndex();
    return cassette == microphone || cassette == echoAux;
}

juce::String ConsoleTG12345::getLoopName() const
{
    return cassette_->getIndex() == echoAux ? "Echo Chamber" : "Echo Send";
}

juce::String ConsoleTG12345::getReturnDescription() const
{
    return cassette_->getIndex() == echoAux ? "Return Master, the return's width, the output"
                                            : "Echo Return, mixed into the channel's output";
}

//==================================================================================================

void ConsoleTG12345::prepareDsp(double sampleRate, int maximumBlockSize)
{
    oversampler_.prepare(maximumBlockSize);
    echoOversampler_.prepare(maximumBlockSize);
    oversampler_.select(factorChoice(), 0);
    echoOversampler_.select(factorChoice(), 0);
    oversampler_.reset();
    echoOversampler_.reset();
    echoOversampled_.assign(static_cast<std::size_t>(maximumBlockSize) * 8, 0.0);
    for (auto& e : echoBase_)
        e.assign(static_cast<std::size_t>(maximumBlockSize), 0.0);
    for (auto& r : mainRing_)
        r.assign(static_cast<std::size_t>(kMaxLoopLatency + maximumBlockSize + 1), 0.0);
    ringWrite_ = 0;
    inputTransformer_ = {};
    outputTransformer_ = {};
    eq_ = {};
    gr_ = {0.0, 0.0};
    rectified_ = {0.0, 0.0};
    tonePhase_ = 0.0;
    sumLL_ = sumRR_ = sumLR_ = 0.0;
    rate_ = 0.0;
    eqSignature_.fill(-1.0e9f);
    baseGlide_ = 1.0 - std::exp(-1.0 / (kGlideSeconds * sampleRate));
    update(sampleRate * oversampler_.getRateMultiple());
    for (auto* glide : {&gIn_, &gFader_, &gSpread_, &gEchoSend_, &gPanAL_, &gPanAR_, &gPanBL_, &gPanBR_, &gReturnMaster_, &gSendReturn_,
                        &bEchoReturn_, &bSendMaster_, &bAnnounce_, &bProgram_, &bTalkback_, &bMono_})
        glide->value = glide->target;
    setLatencySamples(currentLatency());
}

ConsoleTG12345::FirstOrder ConsoleTG12345::shelf(bool high, double rate, double turnoverHz, double calibrationHz, double gainDb) noexcept
{
    if (std::abs(gainDb) < 1.0e-9)
        return {};
    // A first-order passive shelf (bilinear, prewarped at its turnover) whose plateau is set so the
    // response at the calibration frequency is exactly `gainDb`, as the stud switch is marked.
    const double wc = 2.0 * kPi * std::min(turnoverHz, rate * 0.45);
    const double k = wc / std::tan(wc / (2.0 * rate));
    const auto design = [&](double plateauDb)
    {
        const double root = std::sqrt(dbToGain(plateauDb));
        const double n1 = high ? root : 1.0, n0 = high ? wc : wc * root;
        const double d1 = high ? 1.0 / root : 1.0, d0 = high ? wc : wc / root;
        const double d = d1 * k + d0;
        return FirstOrder{(n1 * k + n0) / d, (n0 - n1 * k) / d, (d0 - d1 * k) / d};
    };
    const auto responseDb = [&](const FirstOrder& c)
    {
        const std::complex<double> z = std::polar(1.0, -2.0 * kPi * calibrationHz / rate);
        return 20.0 * std::log10(std::abs((c.b0 + c.b1 * z) / (1.0 + c.a1 * z)));
    };
    double lo = gainDb > 0.0 ? 0.0 : -60.0, hi = gainDb > 0.0 ? 60.0 : 0.0;
    for (int i = 0; i < 60; ++i)
    {
        const double mid = 0.5 * (lo + hi);
        if (responseDb(design(mid)) < gainDb)
            lo = mid;
        else
            hi = mid;
    }
    return design(0.5 * (lo + hi));
}

void ConsoleTG12345::update(double rate) noexcept
{
    const int cassette = cassette_->getIndex();
    const bool markTwo = mark_->getIndex() == 1;

    if (std::abs(rate - rate_) > 0.5)
    {
        rate_ = rate;
        glide_ = 1.0 - std::exp(-1.0 / (kGlideSeconds * rate));
        ripple_ = std::exp(-1.0 / (0.01 * rate)); // (the rectifier's smoothing: 10 ms)
        using Shape = dsp::BiquadCoefficients::Shape;
        // The mic input transformer (1:2): 1 % THD at 50 Hz with the input at -2 dBFS (16 dB over
        // +4 dBu); the 1:1 output transformer: 1 % at 50 Hz at +24 dBu. Flux: the integral of the
        // voltage, scaled to the signal's amplitude at 50 Hz, leaking below 2 Hz.
        micModel_.highPass = dsp::BiquadCoefficients::make(Shape::lowCut, rate, 8.0, 0.7071, 0.0);
        outputModel_.highPass = dsp::BiquadCoefficients::make(Shape::lowCut, rate, 5.0, 0.7071, 0.0);
        for (auto* model : {&micModel_, &outputModel_})
        {
            model->leak = std::exp(-2.0 * kPi * 2.0 / rate);
            model->integrate = 2.0 * kPi * 50.0 / rate;
        }
        micModel_.saturation = 0.04 / std::pow(dbToGain(-2.0), 2.0);
        outputModel_.saturation = 0.04 / std::pow(railOf(kMaxOutputDbu), 2.0);
        eqSignature_.fill(-1.0e9f);
    }

    // The EQ networks (only those the cassette has).
    const float bass = cassette == microphone ? static_cast<float>(eqStepDb(bass_->getIndex())) : 0.0f;
    const float treble = cassette == microphone ? static_cast<float>(eqStepDb(treble_->getIndex())) : 0.0f;
    const bool presenceFitted = cassette == group || (cassette == microphone && markTwo);
    const float presence = presenceFitted ? static_cast<float>(eqStepDb(presence_->getIndex())) : 0.0f;
    const std::array<float, 8> signature{bass, treble, presence, static_cast<float>(presenceFrequency_->getIndex()),
                                         static_cast<float>(rate), 0.0f, 0.0f, 0.0f};
    if (signature != eqSignature_)
    {
        eqSignature_ = signature;
        using Shape = dsp::BiquadCoefficients::Shape;
        bassShelf_ = shelf(false, rate, 150.0, kBassHz, bass);
        trebleCutShelf_ = shelf(true, rate, 6000.0, kTrebleCutHz, std::min(0.0f, treble));
        trebleBoostBell_ = dsp::BiquadCoefficients::make(Shape::peak, rate, kTrebleBoostHz, 0.8, std::max(0.0f, treble));
        presenceBell_ = dsp::BiquadCoefficients::make(Shape::peak, rate, kPresenceHz[static_cast<std::size_t>(presenceFrequency_->getIndex())],
                                                      1.4, presence);
    }

    // The compressor / limiter (on the Microphone and Group cassettes).
    dynamicsMode_ = cassette == microphone || cassette == group ? mode_->getIndex() : out;
    ratio_ = dynamicsMode_ == limit ? kLimitRatio : dynamicsMode_ == comp ? kCompRatio : 1.0;
    // The loop's feedback speeds the detector up by the ratio: the closed loop attacks in 1 ms.
    attack_ = 1.0 - std::exp(-1.0 / (ratio_ * kAttackSeconds * rate));
    // Recovery: 90 % of the gain reduction is recovered in the switch's time.
    release_ = 1.0 - std::exp(-std::log(10.0) / (kRecoverySeconds[static_cast<std::size_t>(recovery_->getIndex())] * rate));
    thresholdDb_ = kNominalDbfs + hold_->get();
    thresholdLinear_ = dbToGain(thresholdDb_);
    linked_ = link_->get();

    // Gains.
    gIn_.target = dbToGain(kGainSteps[static_cast<std::size_t>(gain_->getIndex())] + kFineSteps[static_cast<std::size_t>(fine_->getIndex())]);
    gFader_.target = levelGain(*fader_);
    const bool spreader = (cassette == microphone && !markTwo) || cassette == echoAux;
    gSpread_.target = spreader ? spread_->get() / 100.0 : 1.0;
    gEchoSend_.target = cassette == microphone ? levelGain(*echoSend_) : 0.0;
    const bool mono = input_->getIndex() == 1;
    const auto [al, ar] = panGains(panA_->get() / 100.0);
    const auto [bl, br] = panGains(panB_->get() / 100.0);
    gPanAL_.target = al;
    gPanAR_.target = ar;
    gPanBL_.target = mono ? 0.0 : bl;
    gPanBR_.target = mono ? 0.0 : br;
    gReturnMaster_.target = levelGain(*echoReturn_);
    gSendReturn_.target = levelGain(*echoSend_) * levelGain(*echoReturn_);
    bEchoReturn_.target = levelGain(*echoReturn_);
    bSendMaster_.target = levelGain(*echoSend_);
    bAnnounce_.target = cassette == mainMix && announce_->get() ? levelGain(*announceLevel_) : 0.0;
    const bool studio = listen_->getIndex() == 1;
    const bool talkback = talkback_->get();
    if (studio)
        bProgram_.target = levelGain(*studio_);
    else
        bProgram_.target = cut_->get() ? 0.0 : levelGain(*controlRoom_) * (dim_->get() || talkback ? dbToGain(kDimDb) : 1.0);
    bTalkback_.target = studio && talkback ? levelGain(*talkbackLevel_) : 0.0;
    bMono_.target = !studio && mono_->get() ? 1.0 : 0.0;
}

//==================================================================================================

void ConsoleTG12345::dynamics(double& a, double& b, bool stereo) noexcept
{
    if (dynamicsMode_ == out && gr_[0] < 1.0e-6 && gr_[1] < 1.0e-6)
    {
        gr_ = {0.0, 0.0};
        rectified_ = {0.0, 0.0};
        return;
    }
    // The gain cell (a transistor VCA: a little 2nd harmonic, growing with the gain reduction).
    static const double rail = railOf(kTypeC1.railDbu);
    const auto cell = [](double x, double gain) { const double y = x * gain; return y + kVcaCurvature * (1.0 - gain) * y * y / rail; };
    const double gainA = dbToGain(-gr_[0]);
    const double gainB = linked_ ? gainA : dbToGain(-gr_[1]);
    const double inA = std::abs(a), inB = stereo ? std::abs(b) : 0.0;
    a = cell(a, gainA);
    if (stereo)
        b = cell(b, gainB);

    // The sidechain hears the output: rectified to a control voltage (the rectifier's capacitor
    // charges at once and bridges the waveform's troughs, decaying in 10 ms), the gain cell's
    // changes showing in it as they happen. Above the Hold threshold it pulls the gain down by
    // (ratio - 1) times the output's excess: the feedback law, the output rising 1/ratio as fast.
    const auto target = [this](double level)
    {
        if (dynamicsMode_ == out || level <= thresholdLinear_)
            return 0.0;
        return (ratio_ - 1.0) * 20.0 * std::log10(level / thresholdLinear_);
    };
    const auto follow = [this](double& gr, double goal)
    {
        const double coefficient = dynamicsMode_ == out ? glide_ : (goal > gr ? attack_ : release_);
        gr += coefficient * (goal - gr);
    };
    if (linked_ || !stereo)
    {
        rectified_[0] = std::max(std::max(inA, inB), rectified_[0] * ripple_);
        follow(gr_[0], target(rectified_[0] * gainA));
        gr_[1] = gr_[0];
        rectified_[1] = rectified_[0];
    }
    else
    {
        rectified_[0] = std::max(inA, rectified_[0] * ripple_);
        rectified_[1] = std::max(inB, rectified_[1] * ripple_);
        follow(gr_[0], target(rectified_[0] * gainA));
        follow(gr_[1], target(rectified_[1] * gainB));
    }
}

double ConsoleTG12345::equalise(EqChannel& channel, double x) noexcept
{
    x = channel.bass.process(x, bassShelf_);
    x = channel.trebleBoost.process(x, trebleBoostBell_);
    x = channel.trebleCut.process(x, trebleCutShelf_);
    x = channel.presence.process(x, presenceBell_);
    return x;
}

void ConsoleTG12345::runCassette(double* left, double* right, int n, const double* sidechainLeft, const double* sidechainRight,
                                 bool captureEcho) noexcept
{
    const int cassette = cassette_->getIndex();
    oversampler_.select(factorChoice(), 0);
    update(sampleRate_ * oversampler_.getRateMultiple());
    const bool offline = isNonRealtime();

    // Before the desk's circuits (base rate): the announce microphone or tone into the main mix;
    // the monitor section's program levels and the talkback into the studio feed.
    if (cassette == mainMix)
    {
        const bool tone = announceSource_->getIndex() == 1;
        const double step = 2.0 * kPi * kToneHz / sampleRate_;
        const double toneLevel = dbToGain(kNominalDbfs);
        for (int i = 0; i < n; ++i)
        {
            const double g = bAnnounce_.next(baseGlide_);
            double source = 0.0;
            if (tone)
                source = toneLevel * std::sin(tonePhase_);
            else if (sidechainLeft != nullptr && sidechainRight != nullptr)
                source = 0.5 * (sidechainLeft[i] + sidechainRight[i]);
            tonePhase_ = std::fmod(tonePhase_ + step, 2.0 * kPi);
            left[i] += g * source;
            right[i] += g * source;
        }
    }
    else if (cassette == monitor && !offline)
    {
        for (int i = 0; i < n; ++i)
        {
            const double program = bProgram_.next(baseGlide_);
            const double talkback = bTalkback_.next(baseGlide_);
            const double mono = bMono_.next(baseGlide_);
            const double mid = 0.5 * (left[i] + right[i]);
            const double l = left[i] + mono * (mid - left[i]);
            const double r = right[i] + mono * (mid - right[i]);
            const double voice = sidechainLeft != nullptr && sidechainRight != nullptr ? 0.5 * (sidechainLeft[i] + sidechainRight[i]) : 0.0;
            left[i] = program * l + talkback * voice;
            right[i] = program * r + talkback * voice;
        }
    }

    static const double railA = railOf(kTypeA.railDbu), railB = railOf(kTypeB.railDbu), railC1 = railOf(kTypeC1.railDbu),
                        railE = railOf(kTypeE.railDbu);
    const auto ampA = [](double x) { return shape(x, railA, kTypeA.second, kTypeA.third); };
    const auto ampB = [](double x) { return shape(x, railB, kTypeB.second, kTypeB.third); };
    const auto ampC1 = [](double x) { return shape(x, railC1, kTypeC1.second, kTypeC1.third); };
    const auto ampE = [](double x) { return shape(x, railE, kTypeE.second, kTypeE.third); };
    const auto outputStage = [&](double& l, double& r)
    {
        l = outputTransformer_[0].process(ampB(l), outputModel_);
        r = outputTransformer_[1].process(ampB(r), outputModel_);
    };
    const auto widen = [&](double& l, double& r)
    {
        const double width = gSpread_.next(glide_);
        const double mid = 0.5 * (l + r), side = 0.5 * (l - r) * width;
        l = mid + side;
        r = mid - side;
    };

    const bool stereo = input_->getIndex() == 0;
    const bool echoPre = echoPre_->get();
    const bool capture = captureEcho && cassette == microphone;
    double grPeak = 0.0;
    double* channels[2] = {left, right};
    oversampler_.process(channels, n,
                         [&](double** ch, std::size_t count)
                         {
                             double* l = ch[0];
                             double* r = ch[1];
                             switch (cassette)
                             {
                             case microphone:
                                 for (std::size_t i = 0; i < count; ++i)
                                 {
                                     double a = stereo ? l[i] : 0.5 * (l[i] + r[i]);
                                     double b = stereo ? r[i] : 0.0;
                                     // Mic transformer, the stud switches, Amplifier C1. (The
                                     // transformer's 1:2 step-up is part of the switches'
                                     // calibration: they read the net gain.)
                                     const double gain = gIn_.next(glide_);
                                     a = ampC1(inputTransformer_[0].process(a, micModel_) * gain);
                                     if (stereo)
                                         b = ampC1(inputTransformer_[1].process(b, micModel_) * gain);
                                     dynamics(a, b, stereo);
                                     grPeak = std::max(grPeak, std::max(gr_[0], gr_[1]));
                                     // EQ networks and their make-up amplifier (Type A), then Type E,
                                     // which feeds the echo send and the pan-pots in parallel.
                                     a = ampE(ampA(equalise(eq_[0], a)));
                                     if (stereo)
                                         b = ampE(ampA(equalise(eq_[1], b)));
                                     const double fader = gFader_.next(glide_);
                                     const double send = gEchoSend_.next(glide_) * (a + b) * (echoPre ? 1.0 : fader);
                                     if (capture && i < echoOversampled_.size())
                                         echoOversampled_[i] = send;
                                     double outL = gPanAL_.next(glide_) * a + gPanBL_.next(glide_) * b;
                                     double outR = gPanAR_.next(glide_) * a + gPanBR_.next(glide_) * b;
                                     widen(outL, outR);
                                     outL *= fader;
                                     outR *= fader;
                                     outputStage(outL, outR);
                                     l[i] = outL;
                                     r[i] = outR;
                                 }
                                 break;
                             case group:
                                 for (std::size_t i = 0; i < count; ++i)
                                 {
                                     // The sub-group's mix amplifier, its compressor / limiter, the
                                     // presence network and its make-up amplifier, the group fader.
                                     const double gain = gIn_.next(glide_);
                                     double a = ampA(l[i] * gain);
                                     double b = ampA(r[i] * gain);
                                     dynamics(a, b, true);
                                     grPeak = std::max(grPeak, std::max(gr_[0], gr_[1]));
                                     a = ampA(equalise(eq_[0], a));
                                     b = ampA(equalise(eq_[1], b));
                                     const double fader = gFader_.next(glide_);
                                     a *= fader;
                                     b *= fader;
                                     outputStage(a, b);
                                     l[i] = a;
                                     r[i] = b;
                                 }
                                 break;
                             case mainMix:
                                 for (std::size_t i = 0; i < count; ++i)
                                 {
                                     const double gain = gIn_.next(glide_);
                                     const double fader = gFader_.next(glide_);
                                     double a = ampA(l[i] * gain) * fader;
                                     double b = ampA(r[i] * gain) * fader;
                                     outputStage(a, b);
                                     l[i] = a;
                                     r[i] = b;
                                 }
                                 break;
                             case echoAux:
                                 for (std::size_t i = 0; i < count; ++i)
                                 {
                                     // Whole: the send goes straight to the return (nothing in the
                                     // loop); the loop's return: the send master was applied before.
                                     const double send = gSendReturn_.next(glide_);
                                     const double ret = gReturnMaster_.next(glide_);
                                     const double g = captureEcho ? ret : send;
                                     double a = l[i] * g;
                                     double b = r[i] * g;
                                     widen(a, b);
                                     outputStage(a, b);
                                     l[i] = a;
                                     r[i] = b;
                                 }
                                 break;
                             default: // the monitor section
                                 if (offline)
                                     break; // (monitoring: exports pass through)
                                 for (std::size_t i = 0; i < count; ++i)
                                 {
                                     double a = l[i];
                                     double b = r[i];
                                     outputStage(a, b);
                                     l[i] = a;
                                     r[i] = b;
                                 }
                                 break;
                             }
                         });
    grDisplay_.store(static_cast<float>(grPeak), std::memory_order_relaxed);

    // The echo send back to the base rate (through filters like the channel's own).
    if (capture)
    {
        echoOversampler_.select(factorChoice(), 0);
        std::fill_n(echoBase_[0].data(), n, 0.0);
        std::fill_n(echoBase_[1].data(), n, 0.0);
        double* echo[2] = {echoBase_[0].data(), echoBase_[1].data()};
        echoOversampler_.process(echo, n,
                                 [this](double** ch, std::size_t count)
                                 {
                                     for (std::size_t i = 0; i < count && i < echoOversampled_.size(); ++i)
                                         ch[0][i] = ch[1][i] = echoOversampled_[i];
                                 });
    }
}

void ConsoleTG12345::meter(const double* left, const double* right, int n) noexcept
{
    double peakL = 0.0, peakR = 0.0, ll = 0.0, rr = 0.0, lr = 0.0;
    for (int i = 0; i < n; ++i)
    {
        peakL = std::max(peakL, std::abs(left[i]));
        peakR = std::max(peakR, std::abs(right[i]));
        ll += left[i] * left[i];
        rr += right[i] * right[i];
        lr += left[i] * right[i];
    }
    const auto store = [](std::atomic<float>& slot, double peak)
    {
        const auto dbu = static_cast<float>(20.0 * std::log10(peak + 1.0e-12) - kDbuToDbfs);
        float current = slot.load(std::memory_order_relaxed);
        while (dbu > current && !slot.compare_exchange_weak(current, dbu, std::memory_order_acq_rel))
        {
        }
    };
    store(outputPeak_[0], peakL);
    store(outputPeak_[1], peakR);
    const double k = 1.0 - std::exp(-static_cast<double>(n) / (0.3 * sampleRate_));
    sumLL_ += k * (ll - sumLL_);
    sumRR_ += k * (rr - sumRR_);
    sumLR_ += k * (lr - sumLR_);
    const double norm = std::sqrt(sumLL_ * sumRR_);
    correlation_.store(norm > 1.0e-18 ? static_cast<float>(std::clamp(sumLR_ / norm, -1.0, 1.0)) : 1.0f, std::memory_order_relaxed);
}

void ConsoleTG12345::processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer&) noexcept
{
    const int n = buffer.getNumSamples();
    if (n <= 0 || buffer.getNumChannels() < 2)
        return;
    double* left = buffer.getWritePointer(0);
    double* right = buffer.getWritePointer(1);
    const bool talkback = buffer.getNumChannels() >= 4;
    runCassette(left, right, n, talkback ? buffer.getReadPointer(2) : nullptr, talkback ? buffer.getReadPointer(3) : nullptr, false);
    meter(left, right, n);
}

void ConsoleTG12345::processSend(juce::AudioBuffer<double>& buffer, juce::MidiBuffer&) noexcept
{
    const int n = std::min(buffer.getNumSamples(), static_cast<int>(echoBase_[0].size()));
    if (n <= 0 || buffer.getNumChannels() < 2)
        return;
    double* left = buffer.getWritePointer(0);
    double* right = buffer.getWritePointer(1);
    const int cassette = cassette_->getIndex();
    if (cassette == echoAux)
    {
        // The master echo send into the chamber.
        bSendMaster_.target = levelGain(*echoSend_);
        for (int i = 0; i < n; ++i)
        {
            const double g = bSendMaster_.next(baseGlide_);
            left[i] *= g;
            right[i] *= g;
        }
        return;
    }

    // The channel itself (kept for the return, where it meets what comes back), and its echo send.
    runCassette(left, right, n, nullptr, nullptr, true);
    const int size = static_cast<int>(mainRing_[0].size());
    for (int i = 0; i < n; ++i)
    {
        const auto slot = static_cast<std::size_t>((ringWrite_ + i) % size);
        mainRing_[0][slot] = left[i];
        mainRing_[1][slot] = right[i];
    }
    const bool echo = cassette == microphone;
    for (int i = 0; i < n; ++i)
        left[i] = right[i] = echo ? echoBase_[0][static_cast<std::size_t>(i)] : 0.0; // (the echo bus is mono)
}

void ConsoleTG12345::processReturn(juce::AudioBuffer<double>& buffer, int loopLatency) noexcept
{
    const int n = std::min(buffer.getNumSamples(), static_cast<int>(echoBase_[0].size()));
    if (n <= 0 || buffer.getNumChannels() < 2)
        return;
    double* left = buffer.getWritePointer(0);
    double* right = buffer.getWritePointer(1);
    const int cassette = cassette_->getIndex();
    if (cassette == echoAux)
    {
        // The chamber's return: Return Master, its width, the output stage.
        runCassette(left, right, n, nullptr, nullptr, true);
        meter(left, right, n);
        return;
    }

    // The channel, delayed by the loop's latency so that it meets its echo in time, plus the return.
    const int size = static_cast<int>(mainRing_[0].size());
    const int latency = std::clamp(loopLatency, 0, kMaxLoopLatency);
    bEchoReturn_.target = cassette == microphone ? levelGain(*echoReturn_) : 0.0;
    for (int i = 0; i < n; ++i)
    {
        const auto slot = static_cast<std::size_t>(((ringWrite_ + i - latency) % size + size) % size);
        const double g = bEchoReturn_.next(baseGlide_);
        left[i] = mainRing_[0][slot] + g * left[i];
        right[i] = mainRing_[1][slot] + g * right[i];
    }
    ringWrite_ = (ringWrite_ + n) % size;
    meter(left, right, n);
}

} // namespace dojo::devices
