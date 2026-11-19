#include "devices/Harrison4032.h"

#include <algorithm>
#include <cmath>

namespace dojo::devices
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kDbToNeper = 0.11512925464970229; // ln(10) / 20
constexpr double kOffDb = -80.0;
constexpr double kVoltsPerUnit = 13.79;     // +4 dBu (1.228 V rms, 1.736 V peak) is 0.1259 here
constexpr double kPopDbu = -60.0;           // +48 V switching, at the mic input
constexpr double kFeedthroughSeconds = 0.02; // the VCA's control-voltage thump dies away

double dbToGain(double db) noexcept
{
    return std::exp(db * kDbToNeper);
}

double levelGain(double db) noexcept
{
    return db <= kOffDb + 0.05 ? 0.0 : dbToGain(db);
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

juce::String frontBackText(float value)
{
    const int position = juce::roundToInt(value);
    if (position == 0)
        return "Middle";
    return (position < 0 ? "F" : "B") + juce::String(std::abs(position));
}

float frontBackFromText(const juce::String& text)
{
    const auto t = text.trim().toUpperCase();
    if (t.startsWith("M"))
        return 0.0f;
    const auto number = static_cast<float>(std::abs(parseParameterText(t.retainCharacters("0123456789.,-"), {})));
    if (t.startsWith("F"))
        return -number;
    if (t.startsWith("B") || t.startsWith("S"))
        return number;
    return static_cast<float>(parseParameterText(text, {}));
}

/** A first-order high-pass (bilinear, prewarped). */
dsp::BiquadCoefficients firstOrderHighPass(double rate, double hz) noexcept
{
    const double k = std::tan(kPi * std::min(hz, rate * 0.49) / rate);
    return {1.0 / (1.0 + k), -1.0 / (1.0 + k), 0.0, (k - 1.0) / (k + 1.0), 0.0};
}

const std::array<Harrison4032::RevisionSpec, 3>& revisions()
{
    // Feedthrough: a 40 dB fader move in a few milliseconds thumps at about -50 dBFS on the dbx
    // 202 / EGC-101 generation, 20 dB less with the dbx 2150's control-voltage isolation.
    static const double feedthroughOld = std::pow(10.0, -50.0 / 20.0) / 40.0;
    static const std::array<Harrison4032::RevisionSpec, 3> list{{
        {"A", "1975 - 77", "LM301A + discrete pairs", "dbx 202 / EGC-101", 15.0, 22.0, 0.00048, 0.0075, 1.0, -126.0, 0.0010,
         feedthroughOld, -62.0, false, false, false, 2, 25.0},
        {"B", "1977 - 79", "TL071 / TL072 (FET)", "dbx 202 / EGC-101", 18.0, 24.5, 0.00042, 0.0067, 13.0, -127.0, 0.0010,
         feedthroughOld, -85.0, true, false, false, 2, 25.0},
        {"C", "1979 - 84", "NE5534 / TL071", "dbx 2150", 18.0, 26.0, 0.00025, 0.0051, 13.0, -128.0, 0.0005, feedthroughOld * 0.1,
         -90.0, true, true, true, 3, 20.0},
    }};
    return list;
}
} // namespace

//==================================================================================================

const Harrison4032::RevisionSpec& Harrison4032::revisionSpec(int revision) noexcept
{
    return revisions()[static_cast<std::size_t>(std::clamp(revision, 0, 2))];
}

juce::StringArray Harrison4032::moduleNames()
{
    return {"I/O Module", "Bus Master", "Master Section"};
}

std::pair<double, double> Harrison4032::panGains(double pan) noexcept
{
    const double angle = (std::clamp(pan, -1.0, 1.0) + 1.0) * kPi * 0.25;
    return {std::cos(angle), std::sin(angle)};
}

double Harrison4032::proportionalQ(double q, double gainDb) noexcept
{
    // Proportional Q: the band narrows as it is boosted or cut, to its Q at the full +-15 dB.
    return q * (0.4 + 0.6 * std::min(1.0, std::abs(gainDb) / kEqRangeDb));
}

double Harrison4032::Stage::process(double x, const StageModel& model) noexcept
{
    // The op-amp's small curvature, its rails (a firm knee: an op-amp is clean until it clips),
    // and, on the LM301A, its slew rate.
    const double u = std::clamp(x / model.rail, -2.0, 2.0);
    double y = u + model.second * u * u - model.third * u * u * u;
    constexpr double knee = 0.9;
    const double magnitude = std::abs(y);
    if (magnitude > knee)
        y = std::copysign(knee + (1.0 - knee) * std::tanh((magnitude - knee) / (1.0 - knee)), y);
    y = std::clamp(y * model.rail, previous - model.maxStep, previous + model.maxStep);
    previous = y;
    return y;
}

Harrison4032::Harrison4032()
    : BuiltinProcessor(kTypeId, "Harrison 4032", [] {
        auto buses = BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)
                         .withInput("Tape Return", juce::AudioChannelSet::stereo(), true);
        // The extra outputs, in pairs: buses 1 - 32, aux 1 - 4, cue 1 and 2, quad 3 - 4, direct.
        for (int pair = 0; pair < Outputs::count / 2; ++pair)
            buses = buses.withOutput("Send " + juce::String(pair + 1), juce::AudioChannelSet::stereo(), true);
        return buses;
    }(), false)
{
    module_ = addChoice("module", "Module", moduleNames(), ioModule, false);
    revision_ = addChoice("revision", "Revision", {"A (1975)", "B (1977)", "C (1979)"}, revisionC, false);
    format_ = addChoice("format", "Format", {"Stereo", "Mono"}, 0, false);
    input_ = addChoice("input", "Input", {"Mic", "Line"}, 0);
    micGain_ = addFloat("micgain", "Mic Gain", {10.0f, 60.0f, 0.1f}, static_cast<float>(kMicReferenceGainDb), "dB");
    lineTrim_ = addFloat("linetrim", "Line Trim", {-10.0f, 10.0f, 0.1f}, 0.0f, "dB");
    pad_ = addBool("pad", "Pad", false);
    phase_ = addBool("phase", "Phase", false);
    phantom_ = addBool("phantom", "+48V", false);
    flip_ = addBool("flip", "Flip", false);
    hpf_ = addFloat("hpf", "HPF", skewedRange(20.0f, static_cast<float>(kHpfMaxHz), 200.0f, 1.0f), 20.0f, "Hz");
    lpf_ = addFloat("lpf", "LPF", skewedRange(static_cast<float>(kLpfMinHz), static_cast<float>(kLpfMaxHz), 2000.0f, 1.0f), 20000.0f, "Hz");
    filtersIn_ = addBool("filtersin", "Filters In", false);
    lfFreq_ = addFloat("lffreq", "LF Freq", skewedRange(40.0f, 800.0f, 160.0f, 1.0f), 100.0f, "Hz");
    lfGain_ = addFloat("lfgain", "LF Gain", {-15.0f, 15.0f, 0.1f}, 0.0f, "dB");
    lfShelf_ = addBool("lfshelf", "LF Shelf", false);
    lmfFreq_ = addFloat("lmffreq", "LMF Freq", skewedRange(80.0f, 1500.0f, 350.0f, 1.0f), 400.0f, "Hz");
    lmfGain_ = addFloat("lmfgain", "LMF Gain", {-15.0f, 15.0f, 0.1f}, 0.0f, "dB");
    lmfQ_ = addFloat("lmfq", "LMF Q", {0.5f, 3.0f, 0.01f}, static_cast<float>(kFixedMidQ));
    hmfFreq_ = addFloat("hmffreq", "HMF Freq", skewedRange(800.0f, 15000.0f, 3500.0f, 1.0f), 3000.0f, "Hz");
    hmfGain_ = addFloat("hmfgain", "HMF Gain", {-15.0f, 15.0f, 0.1f}, 0.0f, "dB");
    hmfQ_ = addFloat("hmfq", "HMF Q", {0.5f, 3.0f, 0.01f}, static_cast<float>(kFixedMidQ));
    hfFreq_ = addFloat("hffreq", "HF Freq", skewedRange(800.0f, 18000.0f, 4000.0f, 1.0f), 10000.0f, "Hz");
    hfGain_ = addFloat("hfgain", "HF Gain", {-15.0f, 15.0f, 0.1f}, 0.0f, "dB");
    hfShelf_ = addBool("hfshelf", "HF Shelf", false);
    eqIn_ = addBool("eqin", "EQ In", false); // (out, as on a fresh channel: flat until switched in)
    const juce::NormalisableRange<float> level(static_cast<float>(kOffDb), 10.0f, 0.1f, 2.5f);
    fader_ = addFloatWithText("fader", "Fader", level, 0.0f, "dB", levelText, levelFromText);
    pan_ = addFloatWithText("pan", "Pan", {-100.0f, 100.0f, 1.0f}, 0.0f, {}, panText, panFromText);
    panMode_ = addChoice("panmode", "Pan Mode", {"Stereo", "Quad (Front / Back)", "LCRS"}, stereoPan, false);
    frontBack_ = addFloatWithText("frontback", "Front / Back", {-100.0f, 100.0f, 1.0f}, -100.0f, {}, frontBackText, frontBackFromText);
    mix_ = addBool("mix", "Mix", true);
    monitorLevel_ = addFloatWithText("monlevel", "Monitor Level", level, 0.0f, "dB", levelText, levelFromText);
    monitorPan_ = addFloatWithText("monpan", "Monitor Pan", {-100.0f, 100.0f, 1.0f}, 0.0f, {}, panText, panFromText);
    for (int i = 0; i < 4; ++i)
    {
        aux_[static_cast<std::size_t>(i)] = addFloatWithText("aux" + juce::String(i + 1), "Aux " + juce::String(i + 1), level,
                                                             static_cast<float>(kOffDb), "dB", levelText, levelFromText);
        auxPre_[static_cast<std::size_t>(i)] = addBool("aux" + juce::String(i + 1) + "pre", "Aux " + juce::String(i + 1) + " Pre", false);
    }
    for (int i = 0; i < 2; ++i)
    {
        cue_[static_cast<std::size_t>(i)] = addFloatWithText("cue" + juce::String(i + 1), "Cue " + juce::String(i + 1), level,
                                                             static_cast<float>(kOffDb), "dB", levelText, levelFromText);
        cuePan_[static_cast<std::size_t>(i)] =
            addFloatWithText("cue" + juce::String(i + 1) + "pan", "Cue " + juce::String(i + 1) + " Pan", {-100.0f, 100.0f, 1.0f}, 0.0f, {}, panText,
                             panFromText);
    }
    cueFrom_ = addChoice("cuefrom", "Cue From", {"Monitor", "Channel"}, 0);
    for (int i = 0; i < kBuses; ++i)
        bus_[static_cast<std::size_t>(i)] = addBool("bus" + juce::String(i + 1), "Bus " + juce::String(i + 1), false);
    busPan_ = addBool("buspan", "Bus Pan", false);
    direct_ = addBool("direct", "Direct Out", false);
    monitorSource_ = addChoice("crm", "Monitor", {"Mix", "External", "Mix + External"}, monitorMix);
    monitorVolume_ = addFloatWithText("crmlevel", "Monitor Volume", level, 0.0f, "dB", levelText, levelFromText);
    mono_ = addBool("mono", "Mono", false);
    transformer_ = addBool("transformer", "Output Transformer", false);
    noise_ = addBool("noise", "Noise", false);
    for (auto& peak : outputPeak_)
        peak.store(-100.0f);
    revision_->addListener(this);
}

Harrison4032::~Harrison4032()
{
    revision_->removeListener(this);
}

void Harrison4032::parameterValueChanged(int, float)
{
    // (a revision's controls differ: the editor follows; nothing to do on the audio side)
}

bool Harrison4032::filtersEngaged() const noexcept
{
    return revisionSpec(revision_->getIndex()).filtersSwitch ? filtersIn_->get() : eqIn_->get();
}

//==================================================================================================

Harrison4032::Design Harrison4032::design(double rate) const
{
    using Shape = dsp::BiquadCoefficients::Shape;
    const auto& spec = revisionSpec(revision_->getIndex());
    Design d;
    const double hpf = std::max(static_cast<double>(hpf_->get()), spec.hpfMinHz);
    if (spec.hpfPoles == 3)
    {
        // A 3-pole Butterworth: a first-order section and a second-order one with Q 1.
        d.highPass[0] = firstOrderHighPass(rate, hpf);
        d.highPass[1] = dsp::BiquadCoefficients::make(Shape::lowCut, rate, hpf, 1.0, 0.0);
    }
    else
    {
        d.highPass[1] = dsp::BiquadCoefficients::make(Shape::lowCut, rate, hpf, 0.7071067811865476, 0.0);
    }
    d.lowPass = dsp::BiquadCoefficients::make(Shape::highCut, rate, lpf_->get(), 0.7071067811865476, 0.0);

    const auto bell = [rate](double hz, double q, double gain)
    { return dsp::BiquadCoefficients::make(Shape::peak, rate, hz, proportionalQ(q, gain), gain); };
    const double lf = lfGain_->get(), lmf = lmfGain_->get(), hmf = hmfGain_->get(), hf = hfGain_->get();
    d.bands[0] = spec.shelves && lfShelf_->get() ? dsp::BiquadCoefficients::make(Shape::lowShelf, rate, lfFreq_->get(), 0.7071067811865476, lf)
                                                 : bell(lfFreq_->get(), kBellQ, lf);
    d.bands[1] = bell(lmfFreq_->get(), spec.variableQ ? lmfQ_->get() : kFixedMidQ, lmf);
    d.bands[2] = bell(hmfFreq_->get(), spec.variableQ ? hmfQ_->get() : kFixedMidQ, hmf);
    d.bands[3] = spec.shelves && hfShelf_->get() ? dsp::BiquadCoefficients::make(Shape::highShelf, rate, hfFreq_->get(), 0.7071067811865476, hf)
                                                 : bell(hfFreq_->get(), kBellQ, hf);
    for (auto& band : d.bands)
        if (std::abs(band.b0 - 1.0) < 1.0e-12 && std::abs(band.b1 - band.a1) < 1.0e-12 && std::abs(band.b2 - band.a2) < 1.0e-12)
            band = {}; // (a flat band: exactly nothing)
    return d;
}

double Harrison4032::responseDb(double hz) const
{
    constexpr double rate = 48000.0;
    const auto d = design(rate);
    double magnitude = 1.0;
    if (filtersEngaged())
    {
        for (const auto& section : d.highPass)
            magnitude *= section.magnitudeAt(hz, rate);
        magnitude *= d.lowPass.magnitudeAt(hz, rate);
    }
    if (eqIn_->get())
        for (const auto& band : d.bands)
            magnitude *= band.magnitudeAt(hz, rate);
    return 20.0 * std::log10(std::max(magnitude, 1.0e-12));
}

void Harrison4032::prepareDsp(double sampleRate, int maximumBlockSize)
{
    sidechain_.setSize(2, maximumBlockSize, false, true, false);
    sides_ = {};
    outputTransformer_ = {};
    summing_ = {};
    driver_ = {};
    designSignature_.fill(-1.0e9f);
    glide_ = 1.0 - std::exp(-1.0 / (kGlideSeconds * sampleRate));
    flipGlide_ = 1.0 - std::exp(-1.0 / (kFlipSeconds * sampleRate));
    feedthroughDecay_ = std::exp(-1.0 / (kFeedthroughSeconds * sampleRate));
    popDecay_ = std::exp(-1.0 / (0.025 * sampleRate));
    thump_ = pop_ = 0.0;
    phantomWas_ = phantom_->get();
    noiseHasSpare_ = false;

    using Shape = dsp::BiquadCoefficients::Shape;
    // The Jensen JT-115-K-E: 1 % THD at 20 Hz with 0 dBu in (+18 dBFS on this input's scale); its
    // low end reaches far below the audio band. The output transformer (when fitted): 1 % at 20 Hz
    // at +24 dBu.
    micModel_.highPass = dsp::BiquadCoefficients::make(Shape::lowCut, sampleRate, 5.0, 0.7071067811865476, 0.0);
    outputModel_.highPass = dsp::BiquadCoefficients::make(Shape::lowCut, sampleRate, 5.0, 0.7071067811865476, 0.0);
    for (auto* model : {&micModel_, &outputModel_})
    {
        model->leak = std::exp(-2.0 * kPi * 1.0 / sampleRate);
        model->integrate = 2.0 * kPi * 20.0 / sampleRate;
    }
    micModel_.saturation = 0.04 / std::pow(dbToGain(kMicOffsetDb), 2.0);
    outputModel_.saturation = 0.04 / std::pow(dbToGain(24.0 + kDbuToDbfs), 2.0);

    update();
    for (auto* glide : {&gGain_, &gTrim_, &gFader_, &gPanAL_, &gPanAR_, &gPanBL_, &gPanBR_, &gMonL_, &gMonR_, &gFront_, &gRear_, &gFlip_,
                        &gMix_, &gFiltersIn_, &gEqIn_, &gMonitorVolume_, &gMono_, &gExternal_, &gProgram_, &gLine_})
        glide->value = glide->target;
    for (auto& g : gAux_)
        g.value = g.target;
    for (std::size_t i = 0; i < gCueL_.size(); ++i)
    {
        gCueL_[i].value = gCueL_[i].target;
        gCueR_[i].value = gCueR_[i].target;
    }
    for (auto& g : gBus_)
        g.value = g.target;
    faderDbPrevious_ = gFader_.value;
    setLatencySamples(0);
}

void Harrison4032::update() noexcept
{
    const auto& spec = revisionSpec(revision_->getIndex());
    const double rate = sampleRate_;
    stageModel_.rail = dbToGain(spec.maxOutputDbu + kDbuToDbfs);
    stageModel_.second = spec.second;
    stageModel_.third = spec.third;
    stageModel_.maxStep = spec.slewVoltsPerUs * 1.0e6 / rate / kVoltsPerUnit;
    vcaCurvature_ = spec.vcaCurvature;
    feedthrough_ = spec.feedthrough;
    crosstalk_ = dbToGain(spec.crosstalkDb);

    const std::array<float, 20> signature{static_cast<float>(revision_->getIndex()), hpf_->get(),    lpf_->get(),     lfFreq_->get(),
                                          lfGain_->get(),                         lmfFreq_->get(), lmfGain_->get(), lmfQ_->get(),
                                          hmfFreq_->get(),                        hmfGain_->get(), hmfQ_->get(),    hfFreq_->get(),
                                          hfGain_->get(),                         lfShelf_->get() ? 1.0f : 0.0f,
                                          hfShelf_->get() ? 1.0f : 0.0f,          static_cast<float>(rate),
                                          0.0f,                                   0.0f,
                                          0.0f,                                   0.0f};
    if (signature != designSignature_)
    {
        designSignature_ = signature;
        design_ = design(rate);
    }

    const bool mono = format_->getIndex() == 1;
    const bool microphone = input_->getIndex() == 0;
    gLine_.target = microphone ? 0.0 : 1.0;
    gGain_.target = dbToGain(micGain_->get() - kMicReferenceGainDb) * (pad_->get() ? dbToGain(kPadDb) : 1.0) * (phase_->get() ? -1.0 : 1.0);
    gTrim_.target = dbToGain(lineTrim_->get());
    gFader_.target = std::max(static_cast<double>(fader_->get()), kOffDb);
    gFlip_.target = flip_->get() ? 1.0 : 0.0;
    gMix_.target = mix_->get() ? 1.0 : 0.0;
    gFiltersIn_.target = filtersEngaged() ? 1.0 : 0.0;
    gEqIn_.target = eqIn_->get() ? 1.0 : 0.0;

    // The pan pot: a balance for the stereo pair, constant power for the mono module.
    const double pan = pan_->get() / 100.0;
    if (mono)
    {
        const auto [l, r] = panGains(pan);
        gPanAL_.target = l;
        gPanAR_.target = r;
        gPanBL_.target = gPanBR_.target = 0.0;
    }
    else
    {
        gPanAL_.target = std::min(1.0, 1.0 - pan);
        gPanAR_.target = 0.0;
        gPanBL_.target = 0.0;
        gPanBR_.target = std::min(1.0, 1.0 + pan);
    }
    const double monitorPan = monitorPan_->get() / 100.0;
    if (mono)
    {
        const auto [l, r] = panGains(monitorPan);
        gMonL_.target = l * levelGain(monitorLevel_->get());
        gMonR_.target = r * levelGain(monitorLevel_->get());
    }
    else
    {
        gMonL_.target = std::min(1.0, 1.0 - monitorPan) * levelGain(monitorLevel_->get());
        gMonR_.target = std::min(1.0, 1.0 + monitorPan) * levelGain(monitorLevel_->get());
    }
    // Quad / LCRS: front and back (or surround) by constant power.
    const auto [front, rear] = panGains(frontBack_->get() / 100.0);
    const bool quad = panMode_->getIndex() != stereoPan;
    gFront_.target = quad ? front : 1.0;
    gRear_.target = quad ? rear : 0.0;

    for (std::size_t i = 0; i < 4; ++i)
        gAux_[i].target = levelGain(aux_[i]->get());
    for (std::size_t i = 0; i < 2; ++i)
    {
        const auto [l, r] = panGains(cuePan_[i]->get() / 100.0);
        gCueL_[i].target = l * levelGain(cue_[i]->get());
        gCueR_[i].target = r * levelGain(cue_[i]->get());
    }
    const auto [busL, busR] = panGains(pan);
    const bool busPan = busPan_->get();
    for (std::size_t i = 0; i < static_cast<std::size_t>(kBuses); ++i)
        gBus_[i].target = bus_[i]->get() ? (busPan ? ((i % 2) == 0 ? busL : busR) : 1.0) : 0.0;

    // The master section's monitor matrix.
    const int source = monitorSource_->getIndex();
    gProgram_.target = source != monitorExternal ? 1.0 : 0.0;
    gExternal_.target = source != monitorMix ? 1.0 : 0.0;
    gMono_.target = mono_->get() ? 1.0 : 0.0;
    gMonitorVolume_.target = levelGain(monitorVolume_->get());

    // +48 V: switching it puts a pop on the microphone input.
    if (phantom_->get() != phantomWas_)
    {
        phantomWas_ = phantom_->get();
        pop_ += (phantomWas_ ? 1.0 : -1.0) * dbToGain(kPopDbu + kMicOffsetDb);
    }
}

double Harrison4032::noiseSample() noexcept
{
    // Gaussian (Box-Muller on a xorshift generator).
    if (noiseHasSpare_)
    {
        noiseHasSpare_ = false;
        return noiseSpare_;
    }
    const auto uniform = [this]
    {
        noiseState_ ^= noiseState_ << 13;
        noiseState_ ^= noiseState_ >> 17;
        noiseState_ ^= noiseState_ << 5;
        return (static_cast<double>(noiseState_) + 1.0) / 4294967297.0;
    };
    const double radius = std::sqrt(-2.0 * std::log(uniform()));
    const double angle = 2.0 * kPi * uniform();
    noiseSpare_ = radius * std::sin(angle);
    noiseHasSpare_ = true;
    return radius * std::cos(angle);
}

void Harrison4032::meter(const double* left, const double* right, int n) noexcept
{
    double peakL = 0.0, peakR = 0.0;
    for (int i = 0; i < n; ++i)
    {
        peakL = std::max(peakL, std::abs(left[i]));
        peakR = std::max(peakR, std::abs(right[i]));
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
}

//==================================================================================================

void Harrison4032::processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer&) noexcept
{
    const int n = std::min(buffer.getNumSamples(), sidechain_.getNumSamples());
    if (n <= 0 || buffer.getNumChannels() < 2)
        return;
    update();

    // The sidechain input (the tape return, or the monitor matrix's external source) shares its
    // channels with the extra outputs: read it first.
    const bool sidechain = buffer.getNumChannels() >= 4;
    for (int ch = 0; ch < 2; ++ch)
    {
        if (sidechain)
            sidechain_.copyFrom(ch, 0, buffer, 2 + ch, 0, n);
        else
            sidechain_.clear(ch, 0, n);
    }
    for (int ch = 2; ch < buffer.getNumChannels(); ++ch)
        buffer.clear(ch, 0, n);

    double* left = buffer.getWritePointer(0);
    double* right = buffer.getWritePointer(1);
    switch (module_->getIndex())
    {
    case ioModule:
        runModule(buffer);
        break;
    case busMaster:
        runBusMaster(left, right, n);
        break;
    default:
        runMasterSection(left, right, sidechain_.getReadPointer(0), sidechain_.getReadPointer(1), n);
        break;
    }
    meter(left, right, n);
}

void Harrison4032::runModule(juce::AudioBuffer<double>& buffer) noexcept
{
    const int n = std::min(buffer.getNumSamples(), sidechain_.getNumSamples());
    double* left = buffer.getWritePointer(0);
    double* right = buffer.getWritePointer(1);
    const double* tapeLeft = sidechain_.getReadPointer(0);
    const double* tapeRight = sidechain_.getReadPointer(1);
    const double lcrsPanPosition = pan_->get() / 100.0;
    const int extra = buffer.getNumChannels() - 2; // (the extra outputs, from channel 2)
    const auto out = [&](int index) -> double* { return index < extra ? buffer.getWritePointer(2 + index) : nullptr; };

    const bool mono = format_->getIndex() == 1;
    const int sides = mono ? 1 : 2;
    const bool lcrs = panMode_->getIndex() == lcrsPan;
    const bool noise = noise_->get();
    const auto& spec = revisionSpec(revision_->getIndex());
    const double noiseLevel = dbToGain(spec.einDbu + kMicOffsetDb) * std::sqrt(0.5 * sampleRate_ / 20000.0);
    const bool cueFromChannel = cueFrom_->getIndex() == 1;
    std::array<bool, 4> pre{};
    for (std::size_t a = 0; a < 4; ++a)
        pre[a] = auxPre_[a]->get();
    std::array<double*, kBuses> busOut{};
    bool anyBus = false;
    for (int b = 0; b < kBuses; ++b)
    {
        busOut[static_cast<std::size_t>(b)] = out(Outputs::bus(b + 1));
        anyBus = anyBus || gBus_[static_cast<std::size_t>(b)].target > 0.0 || gBus_[static_cast<std::size_t>(b)].value > 1.0e-9;
    }
    std::array<double*, 4> auxOut{out(Outputs::aux(1)), out(Outputs::aux(2)), out(Outputs::aux(3)), out(Outputs::aux(4))};
    std::array<double*, 4> cueOut{out(Outputs::cue(1)), out(Outputs::cue(1) + 1), out(Outputs::cue(2)), out(Outputs::cue(2) + 1)};
    double* quadA = out(Outputs::quad);
    double* quadB = out(Outputs::quad + 1);
    double* directA = out(Outputs::direct);
    double* directB = out(Outputs::direct + 1);
    const bool direct = direct_->get();

    for (int i = 0; i < n; ++i)
    {
        const double line = gLine_.next(glide_);
        const double micGain = gGain_.next(glide_);
        const double trim = gTrim_.next(glide_);
        const double flip = gFlip_.next(flipGlide_);
        const double filtersIn = gFiltersIn_.next(glide_);
        const double eqIn = gEqIn_.next(glide_);
        pop_ *= popDecay_;

        // The inputs: the track at the microphone input or the line input (the channel input), and
        // the tape return (the sidechain). FLIP swaps them between the channel and monitor paths.
        std::array<double, 2> channel{}, monitor{};
        const std::array<double, 2> trackIn{mono ? 0.5 * (left[i] + right[i]) : left[i], right[i]};
        const std::array<double, 2> tapeIn{mono ? 0.5 * (tapeLeft[i] + tapeRight[i]) : tapeLeft[i], tapeRight[i]};
        for (int s = 0; s < sides; ++s)
        {
            auto& side = sides_[static_cast<std::size_t>(s)];
            double mic = 0.0, lineIn = 0.0;
            if (line < 1.0)
            {
                double x = trackIn[static_cast<std::size_t>(s)] + pop_;
                if (noise)
                    x += noiseLevel * noiseSample();
                mic = side.micPreamp.process(side.mic.process(x, micModel_) * micGain, stageModel_);
            }
            if (line > 0.0)
                lineIn = side.lineReceiver.process(trackIn[static_cast<std::size_t>(s)] * trim, stageModel_);
            const double input = mic + line * (lineIn - mic);
            const double tape = side.tapeReceiver.process(tapeIn[static_cast<std::size_t>(s)], stageModel_);
            channel[static_cast<std::size_t>(s)] = input + flip * (tape - input);
            monitor[static_cast<std::size_t>(s)] = tape + flip * (input - tape);
        }

        // The channel path: filters, EQ (hard-wire bypassed when out), the VCA fader.
        const double faderDb = gFader_.next(glide_);
        const double vcaGain = faderDb <= kOffDb + 0.05 ? 0.0 : dbToGain(faderDb);
        const double controlDb = std::max(faderDb, -60.0);
        thump_ = thump_ * feedthroughDecay_ + feedthrough_ * (controlDb - faderDbPrevious_);
        faderDbPrevious_ = controlDb;
        std::array<double, 2> preFader{}, postFader{};
        for (int s = 0; s < sides; ++s)
        {
            auto& eq = sides_[static_cast<std::size_t>(s)].eq;
            double x = channel[static_cast<std::size_t>(s)];
            if (filtersIn > 1.0e-9)
            {
                double f = x;
                for (std::size_t k = 0; k < 2; ++k)
                    f = eq.highPass[k].process(f, design_.highPass[k]);
                f = eq.filterStage.process(eq.lowPass.process(f, design_.lowPass), stageModel_);
                x += filtersIn * (f - x);
            }
            if (eqIn > 1.0e-9)
            {
                double e = x;
                for (std::size_t k = 0; k < 4; ++k)
                    e = eq.bands[k].process(e, design_.bands[k]);
                e = eq.eqStage.process(e, stageModel_);
                x += eqIn * (e - x);
            }
            preFader[static_cast<std::size_t>(s)] = x;
            // The VCA: a little 2nd harmonic (most linear at unity), and its control voltage's
            // feedthrough.
            double y = x * vcaGain;
            y += vcaCurvature_ * (1.0 + std::abs(faderDb) / 20.0) * y * y / stageModel_.rail;
            postFader[static_cast<std::size_t>(s)] = y + thump_;
        }

        // To the mix: the channel path (pan, quad / LCRS) and the monitor path (level, pan).
        const double mix = gMix_.next(glide_);
        const double al = gPanAL_.next(glide_), ar = gPanAR_.next(glide_), bl = gPanBL_.next(glide_), br = gPanBR_.next(glide_);
        const double frontGain = gFront_.next(glide_), rearGain = gRear_.next(glide_);
        const double monL = gMonL_.next(glide_), monR = gMonR_.next(glide_);
        double chL = al * postFader[0] + bl * postFader[1];
        double chR = ar * postFader[0] + br * postFader[1];
        double extraA = 0.0, extraB = 0.0;
        if (lcrs)
        {
            // LCRS: the pan pot across left, centre and right; Front / Back is the surround.
            const double m = mono ? postFader[0] : 0.5 * (postFader[0] + postFader[1]);
            if (mono)
            {
                const double p = lcrsPanPosition;
                const double angle = (p <= 0.0 ? p + 1.0 : p) * kPi * 0.5;
                const double first = std::cos(angle), second = std::sin(angle);
                chL = p <= 0.0 ? first * m : 0.0;
                chR = p > 0.0 ? second * m : 0.0;
                extraA = p <= 0.0 ? second * m : first * m; // centre
            }
            extraB = rearGain * m; // surround
            chL *= frontGain;
            chR *= frontGain;
            extraA *= frontGain;
        }
        else
        {
            extraA = rearGain * chL;
            extraB = rearGain * chR;
            chL *= frontGain;
            chR *= frontGain;
        }
        const std::array<double, 2> monitorIn{sides_[0].monitorStage.process(monitor[0], stageModel_),
                                              mono ? 0.0 : sides_[1].monitorStage.process(monitor[1], stageModel_)};
        left[i] = mix * chL + monL * monitorIn[0];
        right[i] = mix * chR + monR * (mono ? monitorIn[0] : monitorIn[1]);
        if (quadA != nullptr)
            quadA[i] = mix * extraA;
        if (quadB != nullptr)
            quadB[i] = mix * extraB;

        // The sends: aux 1 - 4 (mono, pre or post fader), cue 1 - 2 (stereo), the buses, direct.
        const double preMono = mono ? preFader[0] : 0.5 * (preFader[0] + preFader[1]);
        const double postMono = mono ? postFader[0] : 0.5 * (postFader[0] + postFader[1]);
        for (std::size_t a = 0; a < 4; ++a)
        {
            const double g = gAux_[a].next(glide_);
            if (auxOut[a] != nullptr)
                auxOut[a][i] = g * (pre[a] ? preMono : postMono);
        }
        const double cueSource = cueFromChannel ? preMono : (mono ? monitorIn[0] : 0.5 * (monitorIn[0] + monitorIn[1]));
        for (std::size_t c = 0; c < 2; ++c)
        {
            const double gl = gCueL_[c].next(glide_), gr = gCueR_[c].next(glide_);
            if (cueOut[2 * c] != nullptr)
                cueOut[2 * c][i] = gl * cueSource;
            if (cueOut[2 * c + 1] != nullptr)
                cueOut[2 * c + 1][i] = gr * cueSource;
        }
        if (anyBus)
        {
            for (std::size_t b = 0; b < static_cast<std::size_t>(kBuses); ++b)
                busScratch_[b] = gBus_[b].next(glide_) * postMono;
            // Crosstalk: each bus leaks into its neighbours (inverting summing on revision A).
            for (std::size_t b = 0; b < static_cast<std::size_t>(kBuses); ++b)
            {
                if (busOut[b] == nullptr)
                    continue;
                double x = busScratch_[b];
                if (b > 0)
                    x += crosstalk_ * busScratch_[b - 1];
                if (b + 1 < static_cast<std::size_t>(kBuses))
                    x += crosstalk_ * busScratch_[b + 1];
                busOut[b][i] = x;
            }
        }
        if (direct && directA != nullptr && directB != nullptr)
            directA[i] = directB[i] = postMono;
    }
}

void Harrison4032::runBusMaster(double* left, double* right, int n) noexcept
{
    // A sub-group, aux or cue master: the summing amplifier, its fader, the line driver.
    const bool transformer = transformer_->get();
    for (int i = 0; i < n; ++i)
    {
        const double trim = gTrim_.next(glide_);
        const double faderDb = gFader_.next(glide_);
        const double fader = faderDb <= kOffDb + 0.05 ? 0.0 : dbToGain(faderDb);
        std::array<double*, 2> io{left, right};
        for (std::size_t ch = 0; ch < 2; ++ch)
        {
            double x = summing_[ch].process(io[ch][i] * trim, stageModel_) * fader;
            x = driver_[ch].process(x, stageModel_);
            if (transformer)
                x = outputTransformer_[ch].process(x, outputModel_);
            io[ch][i] = x;
        }
    }
}

void Harrison4032::runMasterSection(double* left, double* right, const double* externalLeft, const double* externalRight, int n) noexcept
{
    // The mix's summing amplifier, master fader and line drivers; then the control room monitor
    // matrix (not for offline exports: they are the mix).
    const bool transformer = transformer_->get();
    const bool monitoring = !isNonRealtime();
    for (int i = 0; i < n; ++i)
    {
        const double faderDb = gFader_.next(glide_);
        const double fader = faderDb <= kOffDb + 0.05 ? 0.0 : dbToGain(faderDb);
        std::array<double*, 2> io{left, right};
        for (std::size_t ch = 0; ch < 2; ++ch)
        {
            double x = summing_[ch].process(io[ch][i], stageModel_) * fader;
            x = driver_[ch].process(x, stageModel_);
            if (transformer)
                x = outputTransformer_[ch].process(x, outputModel_);
            io[ch][i] = x;
        }
        const double program = gProgram_.next(glide_);
        const double external = gExternal_.next(glide_);
        const double mono = gMono_.next(glide_);
        const double volume = gMonitorVolume_.next(glide_);
        if (!monitoring)
            continue;
        double l = program * left[i] + external * externalLeft[i];
        double r = program * right[i] + external * externalRight[i];
        const double mid = 0.5 * (l + r);
        l += mono * (mid - l);
        r += mono * (mid - r);
        left[i] = volume * l;
        right[i] = volume * r;
    }
}

} // namespace dojo::devices
