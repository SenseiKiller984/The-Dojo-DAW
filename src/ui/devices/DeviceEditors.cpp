#include "ui/devices/DeviceEditors.h"

#include "devices/Dynamics.h"
#include "devices/Equaliser.h"
#include "devices/Instruments.h"
#include "devices/TimeEffects.h"
#include "ui/devices/ParameterControl.h"
#include "ui/theme/DojoTheme.h"

#include <juce_dsp/juce_dsp.h>

#include <cmath>

namespace dojo::ui
{
namespace
{
constexpr double kMinFrequency = 20.0;
constexpr double kMaxFrequency = 20000.0;
constexpr double kEqRangeDb = 24.0;

const std::array<juce::Colour, 4> kBandColours{juce::Colour{0xffe58a3d}, juce::Colour{0xffd6a645}, juce::Colour{0xff3fb59b},
                                               juce::Colour{0xff4f8fd9}};

void setParameter(juce::AudioProcessorParameter* parameter, float normalised)
{
    if (parameter == nullptr)
        return;
    parameter->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, normalised));
}


//==================================================================================================
/** Every automatable parameter as a grid (plug-ins: the first 48). */
class GenericEditor final : public juce::Component
{
public:
    GenericEditor(juce::AudioProcessor& processor, int maxParameters)
    {
        int count = 0;
        for (auto* parameter : processor.getParameters())
        {
            if (!parameter->isAutomatable() || count >= maxParameters)
                continue;
            grid_.add(*parameter);
            ++count;
        }
        addAndMakeVisible(viewport_);
        viewport_.setViewedComponent(&grid_, false);
        viewport_.setScrollBarsShown(true, false);
    }

    void resized() override
    {
        viewport_.setBounds(getLocalBounds());
        const int width = viewport_.getMaximumVisibleWidth();
        const int columns = std::max(1, width / ParameterControl::kWidth);
        const int rows = (grid_.getNumControls() + columns - 1) / columns;
        grid_.setSize(width, rows * ParameterControl::kHeight);
    }

    void paint(juce::Graphics& g) override
    {
        if (grid_.getNumControls() == 0)
        {
            g.setColour(theme::colour::textMuted);
            g.setFont(theme::sans(12.0f));
            g.drawText("No automatable parameters", getLocalBounds(), juce::Justification::centred, false);
        }
    }

private:
    juce::Viewport viewport_;
    ParameterGrid grid_;
};

//==================================================================================================
/** A vertical gain-reduction meter (dB, 0 at the top). */
class ReductionMeter final : public juce::Component, private juce::Timer
{
public:
    explicit ReductionMeter(std::function<float()> take)
        : take_(std::move(take))
    {
        startTimerHz(30);
    }

    void paint(juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();
        g.setColour(theme::colour::textMuted);
        g.setFont(theme::label(8.5f));
        g.drawText("GR", area.removeFromTop(12.0f), juce::Justification::centred, false);
        auto text = area.removeFromBottom(14.0f);
        g.setColour(theme::colour::meterBackground);
        g.fillRoundedRectangle(area, 2.0f);
        const float proportion = juce::jlimit(0.0f, 1.0f, shown_ / 24.0f);
        g.setColour(theme::colour::gold);
        g.fillRoundedRectangle(area.withHeight(area.getHeight() * proportion), 2.0f);
        g.setColour(theme::colour::textPrimary);
        g.setFont(theme::mono(10.0f));
        g.drawText(juce::String(shown_, 1), text, juce::Justification::centred, false);
    }

private:
    void timerCallback() override
    {
        const float reduction = take_();
        shown_ = reduction > shown_ ? reduction : shown_ * 0.85f + reduction * 0.15f;
        repaint();
    }

    std::function<float()> take_;
    float shown_ = 0.0f;
};

//==================================================================================================
/** EQ: response curve, spectrum analyser and draggable band handles above per-band controls. */
class EqEditor final : public juce::Component, private juce::Timer
{
public:
    explicit EqEditor(devices::Equaliser& eq)
        : eq_(eq)
        , fft_(11)
    {
        for (int b = 0; b < devices::Equaliser::kNumBands; ++b)
        {
            const auto& band = eq_.getBand(b);
            for (auto* parameter : std::initializer_list<juce::AudioProcessorParameter*>{band.enabled, band.shape, band.frequency, band.gain, band.q})
            {
                auto control = std::make_unique<ParameterControl>(*parameter, parameter == band.enabled ? "B" + juce::String(b + 1)
                                                                               : parameter == band.shape     ? "Shape"
                                                                               : parameter == band.frequency ? "Freq"
                                                                               : parameter == band.gain      ? "Gain"
                                                                                                             : "Q");
                addAndMakeVisible(*control);
                controls_.push_back(std::move(control));
            }
        }
        output_ = std::make_unique<ParameterControl>(eq_.getOutputGain(), "Output");
        addAndMakeVisible(*output_);
        startTimerHz(30);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        graph_ = area.removeFromTop(std::max(80, area.getHeight() - 112)).reduced(2);
        area.removeFromTop(2);
        const int bandWidth = (area.getWidth() - 60) / devices::Equaliser::kNumBands;
        for (int b = 0; b < devices::Equaliser::kNumBands; ++b)
        {
            auto column = area.withX(area.getX() + b * bandWidth).withWidth(bandWidth).reduced(2, 0);
            auto top = column.removeFromTop(40);
            controls_[static_cast<std::size_t>(b * 5)]->setBounds(top.removeFromLeft(top.getWidth() / 3).withTrimmedTop(14));
            controls_[static_cast<std::size_t>(b * 5 + 1)]->setBounds(top);
            const int knobWidth = column.getWidth() / 3;
            for (int k = 0; k < 3; ++k)
                controls_[static_cast<std::size_t>(b * 5 + 2 + k)]->setBounds(column.removeFromLeft(knobWidth));
        }
        output_->setBounds(area.removeFromRight(60));
    }

    void paint(juce::Graphics& g) override
    {
        using namespace theme;
        const auto area = graph_.toFloat();
        g.setColour(colour::meterBackground);
        g.fillRoundedRectangle(area, 3.0f);

        // Grid: decades and +-12 dB.
        g.setColour(colour::outline);
        for (const double f : {50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0})
            g.drawVerticalLine(juce::roundToInt(frequencyToX(f)), area.getY(), area.getBottom());
        for (const double db : {-12.0, 0.0, 12.0})
            g.drawHorizontalLine(juce::roundToInt(gainToY(db)), area.getX(), area.getRight());
        g.setColour(colour::textMuted);
        g.setFont(mono(9.0f));
        for (const auto& [f, text] : {std::pair{100.0, "100"}, {1000.0, "1k"}, {10000.0, "10k"}})
            g.drawText(text, juce::Rectangle<float>(frequencyToX(f) + 2.0f, area.getBottom() - 12.0f, 30.0f, 11.0f),
                       juce::Justification::centredLeft, false);

        // Analyser.
        if (!spectrum_.isEmpty())
        {
            g.setColour(colour::textMuted.withAlpha(0.35f));
            g.fillPath(spectrum_);
        }

        // Response curve.
        juce::Path curve;
        for (float x = area.getX(); x <= area.getRight(); x += 2.0f)
        {
            const double db = juce::Decibels::gainToDecibels(eq_.getMagnitudeAt(xToFrequency(x)), -48.0);
            const float y = gainToY(juce::jlimit(-kEqRangeDb, kEqRangeDb, db));
            if (x <= area.getX())
                curve.startNewSubPath(x, y);
            else
                curve.lineTo(x, y);
        }
        g.setColour(colour::accent);
        g.strokePath(curve, juce::PathStrokeType(2.0f));

        // Band handles.
        for (int b = 0; b < devices::Equaliser::kNumBands; ++b)
        {
            const auto centre = handlePosition(b);
            const bool on = eq_.getBand(b).enabled->get();
            g.setColour(kBandColours[static_cast<std::size_t>(b)].withAlpha(on ? 1.0f : 0.35f));
            g.fillEllipse(juce::Rectangle<float>(12.0f, 12.0f).withCentre(centre));
            g.setColour(colour::windowBackground);
            g.setFont(sansBold(9.0f));
            g.drawText(juce::String(b + 1), juce::Rectangle<float>(12.0f, 12.0f).withCentre(centre), juce::Justification::centred, false);
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        dragBand_ = -1;
        for (int b = 0; b < devices::Equaliser::kNumBands; ++b)
            if (handlePosition(b).getDistanceFrom(e.position) < 10.0f)
                dragBand_ = b;
        if (dragBand_ >= 0)
        {
            const auto& band = eq_.getBand(dragBand_);
            band.frequency->beginChangeGesture();
            band.gain->beginChangeGesture();
        }
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (dragBand_ < 0)
            return;
        const auto& band = eq_.getBand(dragBand_);
        const auto frequency = static_cast<float>(xToFrequency(juce::jlimit(static_cast<float>(graph_.getX()), static_cast<float>(graph_.getRight()), e.position.x)));
        setParameter(band.frequency, band.frequency->convertTo0to1(frequency));
        const int shape = band.shape->getIndex();
        if (shape != 0 && shape != 4) // cuts have no gain
        {
            const auto gain = static_cast<float>(yToGain(e.position.y));
            setParameter(band.gain, band.gain->convertTo0to1(gain));
        }
        repaint();
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        if (dragBand_ >= 0)
        {
            const auto& band = eq_.getBand(dragBand_);
            band.frequency->endChangeGesture();
            band.gain->endChangeGesture();
        }
        dragBand_ = -1;
    }

    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        for (int b = 0; b < devices::Equaliser::kNumBands; ++b)
            if (handlePosition(b).getDistanceFrom(e.position) < 12.0f)
            {
                auto* q = eq_.getBand(b).q;
                q->beginChangeGesture();
                setParameter(q, static_cast<juce::AudioProcessorParameter*>(q)->getValue() + wheel.deltaY * 0.1f);
                q->endChangeGesture();
                repaint();
                return;
            }
    }

private:
    [[nodiscard]] float frequencyToX(double f) const
    {
        const double proportion = std::log(f / kMinFrequency) / std::log(kMaxFrequency / kMinFrequency);
        return static_cast<float>(graph_.getX()) + static_cast<float>(proportion) * static_cast<float>(graph_.getWidth());
    }
    [[nodiscard]] double xToFrequency(float x) const
    {
        const double proportion = (x - static_cast<float>(graph_.getX())) / std::max(1.0f, static_cast<float>(graph_.getWidth()));
        return kMinFrequency * std::pow(kMaxFrequency / kMinFrequency, juce::jlimit(0.0, 1.0, proportion));
    }
    [[nodiscard]] float gainToY(double db) const
    {
        return static_cast<float>(graph_.getCentreY()) - static_cast<float>(db / kEqRangeDb) * static_cast<float>(graph_.getHeight()) * 0.5f;
    }
    [[nodiscard]] double yToGain(float y) const
    {
        return juce::jlimit(-kEqRangeDb, kEqRangeDb,
                            (static_cast<float>(graph_.getCentreY()) - y) / (static_cast<float>(graph_.getHeight()) * 0.5f) * kEqRangeDb);
    }
    [[nodiscard]] juce::Point<float> handlePosition(int b) const
    {
        const auto& band = eq_.getBand(b);
        const int shape = band.shape->getIndex();
        const double gain = (shape == 0 || shape == 4) ? 0.0 : static_cast<double>(band.gain->get());
        return {frequencyToX(band.frequency->get()), gainToY(gain)};
    }

    void timerCallback() override
    {
        // Analyser: Hann-windowed FFT of the newest post-EQ samples.
        constexpr int size = 2048;
        std::array<float, size * 2> data{};
        eq_.copyAnalyserSamples(data.data(), size);
        for (int i = 0; i < size; ++i)
            data[static_cast<std::size_t>(i)] *= 0.5f - 0.5f * std::cos(2.0f * juce::MathConstants<float>::pi * static_cast<float>(i) / (size - 1));
        fft_.performFrequencyOnlyForwardTransform(data.data());

        const double sampleRate = eq_.getSampleRate() > 0.0 ? eq_.getSampleRate() : 48000.0;
        const auto area = graph_.toFloat();
        juce::Path path;
        path.startNewSubPath(area.getX(), area.getBottom());
        for (float x = area.getX(); x <= area.getRight(); x += 3.0f)
        {
            const double f = xToFrequency(x);
            const auto bin = static_cast<std::size_t>(juce::jlimit(1, size / 2 - 1, juce::roundToInt(f / sampleRate * size)));
            const float magnitude = data[bin] / (size * 0.25f);
            const float db = juce::Decibels::gainToDecibels(magnitude, -90.0f);
            const float y = juce::jmap(db, -90.0f, 0.0f, area.getBottom(), area.getY());
            path.lineTo(x, y);
        }
        path.lineTo(area.getRight(), area.getBottom());
        path.closeSubPath();
        spectrum_ = path;
        repaint(graph_);
    }

    devices::Equaliser& eq_;
    juce::dsp::FFT fft_;
    std::vector<std::unique_ptr<ParameterControl>> controls_;
    std::unique_ptr<ParameterControl> output_;
    juce::Rectangle<int> graph_;
    juce::Path spectrum_;
    int dragBand_ = -1;
};

//==================================================================================================
/** Compressor: knobs, transfer curve and gain-reduction meter. */
class CompressorEditor final : public juce::Component, private juce::Timer
{
public:
    explicit CompressorEditor(devices::Compressor& compressor)
        : compressor_(compressor)
        , meter_([&compressor] { return compressor.takeGainReductionDb(); })
    {
        for (const char* id : {"threshold", "ratio", "attack", "release", "knee", "makeup", "mix", "sidechain"})
            if (auto* parameter = findParameter(compressor_, id))
                grid_.add(*parameter);
        addAndMakeVisible(grid_);
        addAndMakeVisible(meter_);
        startTimerHz(10);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        meter_.setBounds(area.removeFromRight(30).reduced(2, 4));
        curve_ = area.removeFromRight(std::min(120, area.getHeight())).reduced(4);
        grid_.setBounds(area);
    }

    void paint(juce::Graphics& g) override
    {
        using namespace theme;
        const auto area = curve_.toFloat();
        g.setColour(colour::meterBackground);
        g.fillRoundedRectangle(area, 3.0f);
        g.setColour(colour::outline);
        g.drawLine(area.getX(), area.getBottom(), area.getRight(), area.getY(), 1.0f);

        juce::Path path;
        for (int i = 0; i <= 60; ++i)
        {
            const double in = -60.0 + i;
            const double out = juce::jlimit(-60.0, 12.0, compressor_.getOutputLevelDb(in));
            const float x = area.getX() + static_cast<float>(i) / 60.0f * area.getWidth();
            const float y = area.getBottom() - static_cast<float>((out + 60.0) / 60.0) * area.getHeight();
            if (i == 0)
                path.startNewSubPath(x, y);
            else
                path.lineTo(x, y);
        }
        g.setColour(colour::accent);
        g.strokePath(path, juce::PathStrokeType(1.8f));
        g.setColour(colour::textMuted);
        g.setFont(label(8.5f));
        g.drawText("IN > OUT", area.reduced(4.0f), juce::Justification::topLeft, false);
    }

private:
    void timerCallback() override { repaint(curve_); }

    devices::Compressor& compressor_;
    ParameterGrid grid_;
    ReductionMeter meter_;
    juce::Rectangle<int> curve_;
};

/** Limiter: knobs and gain-reduction meter. */
class LimiterEditor final : public juce::Component
{
public:
    explicit LimiterEditor(devices::Limiter& limiter)
        : meter_([&limiter] { return limiter.takeGainReductionDb(); })
    {
        for (const char* id : {"input", "ceiling", "release", "lookahead"})
            if (auto* parameter = findParameter(limiter, id))
                grid_.add(*parameter);
        addAndMakeVisible(grid_);
        addAndMakeVisible(meter_);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        meter_.setBounds(area.removeFromRight(30).reduced(2, 4));
        grid_.setBounds(area);
    }

private:
    ParameterGrid grid_;
    ReductionMeter meter_;
};

//==================================================================================================
/** Draws a sample's waveform (min/max per pixel column). */
void paintSampleWaveform(juce::Graphics& g, const devices::SampleData& sample, juce::Rectangle<float> area, juce::Colour colour)
{
    const int frames = sample.getNumFrames();
    if (frames <= 0 || area.getWidth() < 1.0f)
        return;
    const float* data = sample.audio.getReadPointer(0);
    const float mid = area.getCentreY();
    const float half = area.getHeight() * 0.5f;
    g.setColour(colour);
    const int columns = static_cast<int>(area.getWidth());
    for (int x = 0; x < columns; ++x)
    {
        const int begin = static_cast<int>(static_cast<juce::int64>(x) * frames / columns);
        const int end = std::max(begin + 1, static_cast<int>(static_cast<juce::int64>(x + 1) * frames / columns));
        float lo = 0.0f;
        float hi = 0.0f;
        for (int i = begin; i < std::min(end, frames); ++i)
        {
            lo = std::min(lo, data[i]);
            hi = std::max(hi, data[i]);
        }
        g.fillRect(area.getX() + static_cast<float>(x), mid - hi * half, 1.0f, std::max(1.0f, (hi - lo) * half));
    }
}

/** Sampler: waveform with start/end (drop a file or Load...), ADSR and tuning. */
class SamplerEditor final : public juce::Component, public juce::FileDragAndDropTarget, private juce::Timer
{
public:
    explicit SamplerEditor(devices::Sampler& sampler)
        : sampler_(sampler)
    {
        for (auto* parameter : std::initializer_list<juce::AudioProcessorParameter*>{sampler_.root_, sampler_.tune_, sampler_.fine_, sampler_.gain_,
                                                                                     sampler_.velocity_, sampler_.attack_, sampler_.decay_,
                                                                                     sampler_.sustain_, sampler_.release_, sampler_.loop_})
            grid_.add(*parameter);
        addAndMakeVisible(grid_);

        loadButton_.setButtonText("Load Sample...");
        loadButton_.onClick = [this] { chooseFile(); };
        addAndMakeVisible(loadButton_);
        startTimerHz(10);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        auto top = area.removeFromTop(std::max(70, area.getHeight() - 150));
        loadButton_.setBounds(top.removeFromRight(110).removeFromTop(26).reduced(2, 0));
        waveform_ = top.reduced(2);
        grid_.setBounds(area);
    }

    void paint(juce::Graphics& g) override
    {
        using namespace theme;
        const auto area = waveform_.toFloat();
        g.setColour(dropping_ ? colour::accent.withAlpha(0.2f) : colour::meterBackground);
        g.fillRoundedRectangle(area, 3.0f);

        const auto sample = sampler_.getSample();
        if (sample == nullptr)
        {
            g.setColour(colour::textMuted);
            g.setFont(sans(12.0f));
            g.drawText("Drop an audio file here", area, juce::Justification::centred, false);
            return;
        }

        const float startX = area.getX() + sampler_.start_->get() * area.getWidth();
        const float endX = area.getX() + sampler_.end_->get() * area.getWidth();
        g.setColour(colour::panelRaised);
        g.fillRect(juce::Rectangle<float>(startX, area.getY(), endX - startX, area.getHeight()));
        paintSampleWaveform(g, *sample, area.reduced(0.0f, 4.0f), colour::accent.withAlpha(0.85f));

        g.setColour(colour::gold);
        g.fillRect(startX - 1.0f, area.getY(), 2.0f, area.getHeight());
        g.fillRect(endX - 1.0f, area.getY(), 2.0f, area.getHeight());
        g.setColour(colour::textSecondary);
        g.setFont(sans(11.0f));
        g.drawText(sample->file.getFileName() + "   " + juce::String(sample->getNumFrames() / sample->sampleRate, 2) + " s   root " +
                       juce::MidiMessage::getMidiNoteName(juce::roundToInt(sampler_.root_->get()), true, true, 3) +
                       "   voices " + juce::String(sampler_.getActiveVoiceCount()),
                   area.reduced(6.0f, 2.0f), juce::Justification::topLeft, true);
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (!waveform_.contains(e.getPosition()) || sampler_.getSample() == nullptr)
            return;
        const float proportion = (e.position.x - static_cast<float>(waveform_.getX())) / static_cast<float>(waveform_.getWidth());
        dragging_ = std::abs(proportion - sampler_.start_->get()) < std::abs(proportion - sampler_.end_->get()) ? sampler_.start_ : sampler_.end_;
        dragging_->beginChangeGesture();
        mouseDrag(e);
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (dragging_ == nullptr)
            return;
        const float proportion = juce::jlimit(0.0f, 1.0f, (e.position.x - static_cast<float>(waveform_.getX())) / static_cast<float>(waveform_.getWidth()));
        dragging_->setValueNotifyingHost(dragging_->convertTo0to1(proportion));
        repaint();
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        if (dragging_ != nullptr)
            dragging_->endChangeGesture();
        dragging_ = nullptr;
    }

    bool isInterestedInFileDrag(const juce::StringArray& files) override { return !files.isEmpty(); }
    void fileDragEnter(const juce::StringArray&, int, int) override
    {
        dropping_ = true;
        repaint();
    }
    void fileDragExit(const juce::StringArray&) override
    {
        dropping_ = false;
        repaint();
    }
    void filesDropped(const juce::StringArray& files, int, int) override
    {
        dropping_ = false;
        load(juce::File(files[0]));
    }

private:
    void timerCallback() override { repaint(waveform_); }

    void load(const juce::File& file)
    {
        juce::String error;
        if (!sampler_.loadSample(file, error))
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Could not load the sample", error);
        repaint();
    }

    void chooseFile()
    {
        chooser_ = std::make_unique<juce::FileChooser>("Load Sample", juce::File::getSpecialLocation(juce::File::userMusicDirectory),
                                                       "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
        juce::Component::SafePointer<SamplerEditor> safe(this);
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [safe](const juce::FileChooser& chooser)
                              {
                                  if (safe != nullptr && chooser.getResult().existsAsFile())
                                      safe->load(chooser.getResult());
                              });
    }

    devices::Sampler& sampler_;
    ParameterGrid grid_;
    juce::TextButton loadButton_;
    juce::Rectangle<int> waveform_;
    juce::AudioParameterFloat* dragging_ = nullptr;
    bool dropping_ = false;
    std::unique_ptr<juce::FileChooser> chooser_;
};

//==================================================================================================
/** Drum Rack: a 4x4 pad grid and the selected pad's settings. */
class DrumRackEditor final : public juce::Component, public juce::FileDragAndDropTarget, private juce::Timer
{
public:
    DrumRackEditor(devices::DrumRack& rack, const DeviceEditorContext& context)
        : rack_(rack)
        , context_(context)
    {
        loadButton_.setButtonText("Load...");
        loadButton_.onClick = [this] { chooseFile(selectedPad_); };
        addAndMakeVisible(loadButton_);

        returnsButton_.setButtonText("Create Return Tracks");
        returnsButton_.setTooltip("Adds a bus for every pad output (Out 1..16) in use, fed by this Drum Rack (multi-out)");
        returnsButton_.onClick = [this] { createReturnTracks(); };
        addAndMakeVisible(returnsButton_);

        if (auto* master = findParameter(rack_, "master"))
        {
            master_ = std::make_unique<ParameterControl>(*master, "Master");
            addAndMakeVisible(*master_);
        }

        addAndMakeVisible(padGrid_);
        selectPad(0);
        startTimerHz(30);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        pads_ = area.removeFromLeft(std::min(area.getWidth() / 2, area.getHeight() * 2)).reduced(2);
        area.removeFromLeft(6);
        auto header = area.removeFromTop(26);
        returnsButton_.setBounds(header.removeFromRight(150).reduced(2, 0));
        loadButton_.setBounds(header.removeFromRight(70).reduced(2, 0));
        padTitle_ = header;
        if (master_ != nullptr)
            master_->setBounds(area.removeFromRight(ParameterControl::kWidth).removeFromTop(ParameterControl::kHeight));
        padGrid_.setBounds(area);
    }

    void paint(juce::Graphics& g) override
    {
        using namespace theme;
        for (int pad = 0; pad < devices::DrumRack::kPads; ++pad)
        {
            const auto box = padBounds(pad).toFloat().reduced(2.0f);
            const auto sample = rack_.getPadSample(pad);
            const float light = activity_[static_cast<std::size_t>(pad)];
            g.setColour(pad == selectedPad_ ? colour::controlHover : colour::control);
            g.fillRoundedRectangle(box, 4.0f);
            if (light > 0.01f)
            {
                g.setColour(colour::accent.withAlpha(juce::jlimit(0.0f, 0.8f, light)));
                g.fillRoundedRectangle(box, 4.0f);
            }
            if (pad == dropPad_)
            {
                g.setColour(colour::gold);
                g.drawRoundedRectangle(box, 4.0f, 2.0f);
            }
            g.setColour(colour::textMuted);
            g.setFont(mono(9.0f));
            g.drawText(devices::DrumRack::getPadNoteName(pad), box.reduced(4.0f, 2.0f), juce::Justification::topLeft, false);
            g.setColour(sample != nullptr ? colour::textPrimary : colour::textMuted);
            g.setFont(sans(10.5f));
            g.drawText(sample != nullptr ? sample->file.getFileNameWithoutExtension() : juce::String("empty"), box.reduced(4.0f, 2.0f),
                       juce::Justification::bottomLeft, true);
        }

        g.setColour(colour::textPrimary);
        g.setFont(sansBold(12.0f));
        const auto sample = rack_.getPadSample(selectedPad_);
        g.drawText("Pad " + juce::String(selectedPad_ + 1) + " (" + devices::DrumRack::getPadNoteName(selectedPad_) + ")" +
                       (sample != nullptr ? "  " + sample->file.getFileName() : juce::String()),
                   padTitle_, juce::Justification::centredLeft, true);
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        const int pad = padAt(e.getPosition());
        if (pad < 0)
            return;
        selectPad(pad);
        if (e.mods.isPopupMenu())
        {
            juce::Component::SafePointer<DrumRackEditor> safe(this);
            juce::PopupMenu menu;
            menu.addItem("Load Sample...", [safe, pad] { if (safe != nullptr) safe->chooseFile(pad); });
            menu.addItem("Clear Pad", rack_.getPadSample(pad) != nullptr, false, [safe, pad] { if (safe != nullptr) safe->rack_.setPadSample(pad, nullptr); });
            menu.showMenuAsync(juce::PopupMenu::Options().withMousePosition());
            return;
        }
        rack_.triggerPad(pad, 0.8f + 0.2f * (1.0f - e.position.y / static_cast<float>(getHeight())));
    }

    bool isInterestedInFileDrag(const juce::StringArray& files) override { return !files.isEmpty(); }
    void fileDragMove(const juce::StringArray&, int x, int y) override
    {
        dropPad_ = padAt({x, y});
        repaint();
    }
    void fileDragExit(const juce::StringArray&) override
    {
        dropPad_ = -1;
        repaint();
    }
    void filesDropped(const juce::StringArray& files, int x, int y) override
    {
        dropPad_ = -1;
        int pad = padAt({x, y});
        if (pad < 0)
            pad = selectedPad_;
        // Several files fill consecutive pads.
        for (const auto& path : files)
        {
            if (pad >= devices::DrumRack::kPads)
                break;
            load(pad++, juce::File(path));
        }
        repaint();
    }

private:
    [[nodiscard]] juce::Rectangle<int> padBounds(int pad) const
    {
        const int column = pad % 4;
        const int row = 3 - pad / 4; // pad 1 bottom-left, like hardware
        const int w = pads_.getWidth() / 4;
        const int h = pads_.getHeight() / 4;
        return {pads_.getX() + column * w, pads_.getY() + row * h, w, h};
    }

    [[nodiscard]] int padAt(juce::Point<int> p) const
    {
        for (int pad = 0; pad < devices::DrumRack::kPads; ++pad)
            if (padBounds(pad).contains(p))
                return pad;
        return -1;
    }

    void selectPad(int pad)
    {
        selectedPad_ = pad;
        padGrid_.clear();
        const auto& p = rack_.getPad(pad);
        padGrid_.add(*p.volume, "Volume");
        padGrid_.add(*p.pan, "Pan");
        padGrid_.add(*p.pitch, "Pitch");
        padGrid_.add(*p.output, "Output");
        padGrid_.add(*p.choke, "Choke");
        repaint();
    }

    void load(int pad, const juce::File& file)
    {
        juce::String error;
        if (!rack_.loadPadSample(pad, file, error))
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Could not load the sample", error);
    }

    void chooseFile(int pad)
    {
        chooser_ = std::make_unique<juce::FileChooser>("Load Pad Sample", juce::File::getSpecialLocation(juce::File::userMusicDirectory),
                                                       "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
        juce::Component::SafePointer<DrumRackEditor> safe(this);
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [safe, pad](const juce::FileChooser& chooser)
                              {
                                  if (safe != nullptr && chooser.getResult().existsAsFile())
                                  {
                                      safe->load(pad, chooser.getResult());
                                      safe->repaint();
                                  }
                              });
    }

    void createReturnTracks()
    {
        auto& session = context_.project.getSession();
        auto owner = context_.owner;
        if (!owner.hasType(session::ids::track))
            return;

        // Which aux outputs are in use, and which already have a return track?
        std::set<int> outputs;
        for (int pad = 0; pad < devices::DrumRack::kPads; ++pad)
            if (const int output = rack_.getPad(pad).output->getIndex(); output > 0)
                outputs.insert(output);
        for (const auto& t : session.getAllTracks())
            if (session::track::getAuxSource(t) == session::track::getId(owner))
                outputs.erase(session::track::getAuxChannel(t) / 2 + 1);

        if (outputs.empty())
        {
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Drum Rack outputs",
                                                   "Set a pad's Output to \"Out 1\".. \"Out 16\" first; every output in use then gets "
                                                   "its own return track.");
            return;
        }

        session.beginTransaction("Create Return Tracks");
        const int index = session.indexOf(owner) + 1;
        int offset = 0;
        for (const int output : outputs)
        {
            auto bus = session.addTrack(session::TrackKind::bus, session::track::getName(owner) + " Out " + juce::String(output),
                                        index + offset < session.getNumTracks() ? index + offset : -1);
            session::track::setAuxSource(bus, session::track::getId(owner), (output - 1) * 2, &session.getUndoManager());
            session::track::setColour(bus, session::track::getColour(owner), &session.getUndoManager());
            ++offset;
        }
    }

    void timerCallback() override
    {
        bool changed = false;
        for (int pad = 0; pad < devices::DrumRack::kPads; ++pad)
        {
            const float level = rack_.takePadActivity(pad);
            auto& shown = activity_[static_cast<std::size_t>(pad)];
            const float next = std::max(level, shown * 0.8f);
            changed = changed || std::abs(next - shown) > 0.005f;
            shown = next;
        }
        if (changed)
            repaint(pads_);
    }

    devices::DrumRack& rack_;
    DeviceEditorContext context_;
    juce::TextButton loadButton_;
    juce::TextButton returnsButton_;
    std::unique_ptr<ParameterControl> master_;
    ParameterGrid padGrid_;
    juce::Rectangle<int> pads_;
    juce::Rectangle<int> padTitle_;
    int selectedPad_ = 0;
    int dropPad_ = -1;
    std::array<float, devices::DrumRack::kPads> activity_{};
    std::unique_ptr<juce::FileChooser> chooser_;
};
} // namespace

std::unique_ptr<juce::Component> createDeviceEditor(engine::ProcessorNode& node, const DeviceEditorContext& context, int& preferredWidth)
{
    auto& processor = node.getProcessor();

    if (auto* eq = dynamic_cast<devices::Equaliser*>(&processor))
    {
        preferredWidth = 560;
        return std::make_unique<EqEditor>(*eq);
    }
    if (auto* compressor = dynamic_cast<devices::Compressor*>(&processor))
    {
        preferredWidth = 430;
        return std::make_unique<CompressorEditor>(*compressor);
    }
    if (auto* limiter = dynamic_cast<devices::Limiter*>(&processor))
    {
        preferredWidth = 2 * ParameterControl::kWidth + 40;
        return std::make_unique<LimiterEditor>(*limiter);
    }
    if (auto* sampler = dynamic_cast<devices::Sampler*>(&processor))
    {
        preferredWidth = 5 * ParameterControl::kWidth + 20;
        return std::make_unique<SamplerEditor>(*sampler);
    }
    if (auto* rack = dynamic_cast<devices::DrumRack*>(&processor))
    {
        preferredWidth = 640;
        return std::make_unique<DrumRackEditor>(*rack, context);
    }

    const bool native = dynamic_cast<devices::BuiltinProcessor*>(&processor) != nullptr;
    int count = 0;
    for (auto* parameter : processor.getParameters())
        count += parameter->isAutomatable() ? 1 : 0;
    const int maxParameters = native ? 64 : 48;
    const int columns = juce::jlimit(2, 6, (std::min(count, maxParameters) + 1) / 2);
    preferredWidth = columns * ParameterControl::kWidth + 16;
    return std::make_unique<GenericEditor>(processor, maxParameters);
}

} // namespace dojo::ui
