#include "ui/devices/DeviceEditors.h"

#include "devices/Click.h"
#include "devices/Dynamics.h"
#include "devices/Echo.h"
#include "devices/Equaliser.h"
#include "devices/Impulse.h"
#include "devices/Instruments.h"
#include "devices/Operator.h"
#include "devices/TimeEffects.h"
#include "devices/WavetableSynth.h"
#include "ui/BrowserDrag.h"
#include "ui/WaveformPainter.h"
#include "ui/devices/MoreDeviceEditors.h"
#include "ui/devices/PagedEditor.h"
#include "ui/devices/ParameterControl.h"
#include "ui/devices/TempoDetectPanel.h"
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

/** The Click: its parameters (with its own tempo: halve, double, take the project's), and tempo
    detection of the selected tracks beside them. */
class ClickEditor final : public juce::Component, private juce::Timer
{
public:
    static constexpr int kParametersWidth = 4 * ParameterControl::kWidth + 16;

    ClickEditor(devices::Click& click, const DeviceEditorContext& context)
        : click_(click)
        , session_(context.project.getSession())
        , parameters_(click, 16)
        , tempo_(context.project.getSession(), context.editor)
    {
        addAndMakeVisible(parameters_);
        addAndMakeVisible(tempo_);
        half_.setTooltip("Half the click's tempo (turns Own Tempo on)");
        double_.setTooltip("Double the click's tempo (turns Own Tempo on)");
        project_.setTooltip("The click's tempo becomes the project's");
        half_.onClick = [this] { click_.setOwnTempo(click_.getEffectiveTempo(session_.getTempo()) * 0.5, true); };
        double_.onClick = [this] { click_.setOwnTempo(click_.getEffectiveTempo(session_.getTempo()) * 2.0, true); };
        project_.onClick = [this] { click_.setOwnTempo(session_.getTempo(), false); };
        for (auto* b : {&half_, &double_, &project_})
        {
            b->setWantsKeyboardFocus(false);
            addAndMakeVisible(b);
        }
        status_.setFont(theme::sans(11.5f));
        status_.setColour(juce::Label::textColourId, theme::colour::textSecondary);
        addAndMakeVisible(status_);
        tempo_.onSetClickTempo = [this](double bpm) { click_.setOwnTempo(bpm, true); };
        updateStatus();
        startTimerHz(5);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        tempo_.setBounds(area.removeFromRight(TempoDetectPanel::kPreferredWidth));
        area.removeFromRight(6);
        auto bottom = area.removeFromBottom(26).reduced(6, 0);
        half_.setBounds(bottom.removeFromLeft(36));
        bottom.removeFromLeft(4);
        double_.setBounds(bottom.removeFromLeft(36));
        bottom.removeFromLeft(4);
        project_.setBounds(bottom.removeFromLeft(80));
        bottom.removeFromLeft(8);
        status_.setBounds(bottom);
        area.removeFromBottom(4);
        parameters_.setBounds(area);
    }

private:
    void timerCallback() override { updateStatus(); }
    void updateStatus()
    {
        const double project = session_.getTempo();
        const auto text = click_.ownTempo_->get()
                              ? "Own tempo: " + juce::String(click_.getEffectiveTempo(project), 2) + " BPM"
                              : "Follows the project: " + juce::String(project, 2) + " BPM";
        if (text != status_.getText())
            status_.setText(text, juce::dontSendNotification);
    }

    devices::Click& click_;
    session::Session& session_;
    GenericEditor parameters_;
    TempoDetectPanel tempo_;
    juce::TextButton half_{"/2"}, double_{"x2"}, project_{"= Project"};
    juce::Label status_;
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
            g.setColour(textOn(kBandColours[static_cast<std::size_t>(b)]));
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

/**
    Limiter: two rows of controls and the gain-reduction meter.

        Maximize   Threshold  Input      Ceiling   Mode
        Auto Rel.  Release    Lookahead  Link      Channels

    Controls that do nothing in the current setting are dimmed and disabled: Input while Maximize
    is on, Threshold while it is off, Release under Auto release, Link in Stereo.
*/
class LimiterEditor final : public juce::Component, private juce::Timer
{
public:
    static constexpr int kColumns = 5;
    static constexpr int kMeterWidth = 30;
    /** Toggles, three knobs, and drop-downs wide enough for "True Peak". */
    static constexpr std::array<int, kColumns> kColumnWidths{84, ParameterControl::kWidth + 4, ParameterControl::kWidth + 4,
                                                             ParameterControl::kWidth + 4, 100};
    static constexpr int kPreferredWidth = kColumnWidths[0] + kColumnWidths[1] + kColumnWidths[2] + kColumnWidths[3]
                                           + kColumnWidths[4] + kMeterWidth + 16;

    explicit LimiterEditor(devices::Limiter& limiter)
        : limiter_(limiter)
        , meter_([&limiter] { return limiter.takeGainReductionDb(); })
    {
        const std::array<std::pair<juce::AudioProcessorParameter*, const char*>, 2 * kColumns> layout{{
            {limiter.maximize_, "Maximize"},
            {limiter.threshold_, "Threshold"},
            {limiter.input_, "Input"},
            {limiter.ceiling_, "Ceiling"},
            {limiter.mode_, "Mode"},
            {limiter.autoRelease_, "Auto Rel."},
            {limiter.release_, "Release"},
            {limiter.lookahead_, "Lookahead"},
            {limiter.link_, "Link"},
            {limiter.channels_, "Channels"},
        }};
        for (const auto& [parameter, caption] : layout)
        {
            controls_.push_back(std::make_unique<ParameterControl>(*parameter, caption));
            addAndMakeVisible(*controls_.back());
        }
        addAndMakeVisible(meter_);
        updateActiveControls();
        startTimerHz(10);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        meter_.setBounds(area.removeFromRight(kMeterWidth).reduced(2, 4));
        // The columns share out any extra width; the two rows sit in the middle.
        const int natural = kPreferredWidth - kMeterWidth - 16;
        const double scale = std::max(0.5, static_cast<double>(area.getWidth()) / natural);
        const int height = std::min(ParameterControl::kHeight + 8, area.getHeight() / 2);
        const int top = area.getY() + std::max(0, (area.getHeight() - 2 * height) / 2);
        for (int row = 0; row < 2; ++row)
        {
            double x = area.getX();
            for (int column = 0; column < kColumns; ++column)
            {
                const double width = kColumnWidths[static_cast<std::size_t>(column)] * scale;
                controls_[static_cast<std::size_t>(row * kColumns + column)]->setBounds(
                    juce::roundToInt(x), top + row * height, juce::roundToInt(x + width) - juce::roundToInt(x), height);
                x += width;
            }
        }
    }

private:
    void timerCallback() override { updateActiveControls(); }

    void updateActiveControls()
    {
        const bool maximize = limiter_.maximize_->get();
        setActive(*limiter_.input_, !maximize);
        setActive(*limiter_.threshold_, maximize);
        setActive(*limiter_.release_, !limiter_.autoRelease_->get());
        setActive(*limiter_.link_, limiter_.channels_->getIndex() == 1);
    }

    void setActive(juce::AudioProcessorParameter& parameter, bool active)
    {
        for (auto& control : controls_)
            if (&control->getParameter() == &parameter && control->isEnabled() != active)
            {
                control->setEnabled(active);
                control->setAlpha(active ? 1.0f : 0.35f);
            }
    }

    devices::Limiter& limiter_;
    std::vector<std::unique_ptr<ParameterControl>> controls_;
    ReductionMeter meter_;
};

//==================================================================================================
/** Draws a sample's waveform (min/max per pixel column; a stereo sample shows both channels). */
void paintSampleWaveform(juce::Graphics& g, const devices::SampleData& sample, juce::Rectangle<float> area, juce::Colour colour)
{
    const int frames = sample.getNumFrames();
    const int columns = static_cast<int>(area.getWidth());
    if (frames <= 0 || columns < 1)
        return;
    const auto firstX = static_cast<int>(area.getX());
    waveform::paintChannels(g, area, std::min(sample.audio.getNumChannels(), 2), firstX, firstX + columns, colour,
                            [&](int channel, int x)
                            {
                                const float* data = sample.audio.getReadPointer(channel);
                                const auto column = static_cast<juce::int64>(x - firstX);
                                const int begin = static_cast<int>(column * frames / columns);
                                const int end = std::max(begin + 1, static_cast<int>((column + 1) * frames / columns));
                                float lo = 0.0f;
                                float hi = 0.0f;
                                for (int i = begin; i < std::min(end, frames); ++i)
                                {
                                    lo = std::min(lo, data[i]);
                                    hi = std::max(hi, data[i]);
                                }
                                return std::pair{lo, hi};
                            });
}

/** Sampler: waveform with start/end (drop a file or Load...), ADSR and tuning. */
class SamplerEditor final : public juce::Component,
                            public juce::FileDragAndDropTarget,
                            public juce::DragAndDropTarget,
                            private juce::Timer
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

    // Files dragged from the browser.
    bool isInterestedInDragSource(const SourceDetails& details) override { return browserDrag::isFileDrag(details.description); }
    void itemDragEnter(const SourceDetails&) override
    {
        dropping_ = true;
        repaint();
    }
    void itemDragExit(const SourceDetails&) override
    {
        dropping_ = false;
        repaint();
    }
    void itemDropped(const SourceDetails& details) override
    {
        dropping_ = false;
        if (const auto files = browserDrag::getFiles(details.description); !files.isEmpty())
            load(juce::File(files[0]));
        repaint();
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
class DrumRackEditor final : public juce::Component,
                             public juce::FileDragAndDropTarget,
                             public juce::DragAndDropTarget,
                             private juce::Timer
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
    void filesDropped(const juce::StringArray& files, int x, int y) override { dropFiles(files, {x, y}); }

    // Files dragged from the browser: onto any pad.
    bool isInterestedInDragSource(const SourceDetails& details) override { return browserDrag::isFileDrag(details.description); }
    void itemDragMove(const SourceDetails& details) override
    {
        dropPad_ = padAt(details.localPosition);
        repaint();
    }
    void itemDragExit(const SourceDetails&) override
    {
        dropPad_ = -1;
        repaint();
    }
    void itemDropped(const SourceDetails& details) override { dropFiles(browserDrag::getFiles(details.description), details.localPosition); }

    /** Loads `files` onto the pad at `position` (the selected pad if none); several files fill
        consecutive pads. The pad that received the first file is selected. */
    void dropFiles(const juce::StringArray& files, juce::Point<int> position)
    {
        dropPad_ = -1;
        int pad = padAt(position);
        if (pad < 0)
            pad = selectedPad_;
        const int first = pad;
        for (const auto& path : files)
        {
            if (pad >= devices::DrumRack::kPads)
                break;
            load(pad++, juce::File(path));
        }
        if (!files.isEmpty())
            selectPad(first);
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
//==================================================================================================
/** A choice parameter set from the UI as one gesture. */
void setChoiceFromUser(juce::AudioParameterChoice* parameter, int index)
{
    if (parameter == nullptr || index < 0)
        return;
    parameter->beginChangeGesture();
    parameter->setValueNotifyingHost(parameter->convertTo0to1(static_cast<float>(index)));
    parameter->endChangeGesture();
}

//==================================================================================================
//==================================================================================================
/** One Wavetable oscillator's current frame (follows Position and its modulation). */
class WavetableDisplay final : public juce::Component, private juce::Timer
{
public:
    WavetableDisplay(devices::WavetableSynth& synth, int oscillator)
        : synth_(synth)
        , oscillator_(oscillator)
    {
        startTimerHz(20);
        timerCallback();
    }

    void paint(juce::Graphics& g) override
    {
        using namespace theme;
        const auto area = getLocalBounds().toFloat();
        g.setColour(colour::meterBackground);
        g.fillRoundedRectangle(area, 3.0f);

        const auto& tables = devices::WavetableSynth::getTables();
        const auto& p = synth_.oscillators_[static_cast<std::size_t>(oscillator_)];
        const auto& table = tables[static_cast<std::size_t>(std::clamp(p.table->getIndex(), 0, static_cast<int>(tables.size()) - 1))];
        const auto wave = area.reduced(6.0f, 16.0f);

        // A few neighbouring frames behind the current one, for depth.
        for (int ghost = 3; ghost >= 0; --ghost)
        {
            const double position = std::clamp(shown_ + (ghost - 1.5) * 0.08 * (ghost == 0 ? 0.0 : 1.0), 0.0, 1.0);
            juce::Path path;
            const int points = std::max(2, static_cast<int>(wave.getWidth()));
            for (int x = 0; x < points; ++x)
            {
                const double phase = static_cast<double>(x) / (points - 1);
                const float y = wave.getCentreY() - table.read(phase, ghost == 0 ? shown_ : position, 0) * wave.getHeight() * 0.48f;
                if (x == 0)
                    path.startNewSubPath(wave.getX(), y);
                else
                    path.lineTo(wave.getX() + static_cast<float>(x), y);
            }
            g.setColour(ghost == 0 ? colour::accentBright : colour::accent.withAlpha(0.15f));
            g.strokePath(path, juce::PathStrokeType(ghost == 0 ? 1.8f : 1.0f));
        }

        g.setColour(colour::textSecondary);
        g.setFont(sans(10.5f));
        g.drawText(juce::String(table.getName()) + "  " + juce::String(juce::roundToInt(shown_ * 100.0)) + " %",
                   area.reduced(6.0f, 2.0f), juce::Justification::topLeft, true);
    }

private:
    void timerCallback() override
    {
        const auto& p = synth_.oscillators_[static_cast<std::size_t>(oscillator_)];
        const double position = synth_.getActiveVoiceCount() > 0 ? synth_.getDisplayPosition(oscillator_) : p.position->get() / 100.0;
        const int table = p.table->getIndex();
        if (std::abs(position - shown_) > 1.0e-4 || table != shownTable_)
        {
            shown_ = position;
            shownTable_ = table;
            repaint();
        }
    }

    devices::WavetableSynth& synth_;
    int oscillator_;
    double shown_ = -1.0;
    int shownTable_ = -1;
};

//==================================================================================================
/** The Wavetable modulation matrix: source, target and amount for each slot. */
class MatrixView final : public juce::Component, private juce::Timer
{
public:
    explicit MatrixView(devices::WavetableSynth& synth)
        : synth_(synth)
    {
        for (std::size_t s = 0; s < rows_.size(); ++s)
        {
            auto& row = rows_[s];
            auto& slot = synth_.matrix_[s];
            int id = 1;
            for (const auto& name : devices::WavetableSynth::getSourceNames())
                row.source.addItem(name, id++);
            id = 1;
            for (const auto& name : devices::WavetableSynth::getDestinationNames())
                row.destination.addItem(name, id++);
            row.source.onChange = [&row, &slot] { setChoiceFromUser(slot.source, row.source.getSelectedId() - 1); };
            row.destination.onChange = [&row, &slot] { setChoiceFromUser(slot.destination, row.destination.getSelectedId() - 1); };
            row.amount.setSliderStyle(juce::Slider::LinearBar);
            row.amount.setRange(-100.0, 100.0, 0.1);
            row.amount.setTextValueSuffix(" %");
            row.amount.setDoubleClickReturnValue(true, 0.0);
            row.amount.onDragStart = [&row, &slot]
            {
                row.dragging = true;
                slot.amount->beginChangeGesture();
            };
            row.amount.onDragEnd = [&row, &slot]
            {
                row.dragging = false;
                slot.amount->endChangeGesture();
            };
            row.amount.onValueChange = [&row, &slot]
            {
                const float value = static_cast<float>(row.amount.getValue());
                if (std::abs(value - slot.amount->get()) < 1.0e-4f)
                    return;
                if (!row.dragging)
                    slot.amount->beginChangeGesture();
                *slot.amount = value;
                if (!row.dragging)
                    slot.amount->endChangeGesture();
            };
            for (auto* c : std::initializer_list<juce::Component*>{&row.source, &row.destination, &row.amount})
            {
                c->setWantsKeyboardFocus(false);
                addAndMakeVisible(c);
            }
        }
        timerCallback();
        startTimerHz(10);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        const int rowHeight = std::max(16, area.getHeight() / static_cast<int>(rows_.size()));
        for (auto& row : rows_)
        {
            auto line = area.removeFromTop(rowHeight).reduced(0, 1);
            row.source.setBounds(line.removeFromLeft(line.getWidth() * 30 / 100));
            line.removeFromLeft(3);
            row.destination.setBounds(line.removeFromLeft(line.getWidth() * 50 / 100));
            line.removeFromLeft(3);
            row.amount.setBounds(line);
        }
    }

private:
    struct Row
    {
        juce::ComboBox source, destination;
        juce::Slider amount;
        bool dragging = false;
    };

    void timerCallback() override
    {
        for (std::size_t s = 0; s < rows_.size(); ++s)
        {
            auto& row = rows_[s];
            const auto& slot = synth_.matrix_[s];
            row.source.setSelectedId(slot.source->getIndex() + 1, juce::dontSendNotification);
            row.destination.setSelectedId(slot.destination->getIndex() + 1, juce::dontSendNotification);
            if (!row.dragging)
                row.amount.setValue(slot.amount->get(), juce::dontSendNotification);
        }
    }

    devices::WavetableSynth& synth_;
    std::array<Row, devices::WavetableSynth::kMatrixSlots> rows_;
};

//==================================================================================================
/** Operator's current algorithm: modulators above what they modulate, carriers at the bottom. */
class AlgorithmDisplay final : public juce::Component, private juce::Timer
{
public:
    explicit AlgorithmDisplay(devices::Operator& fm)
        : fm_(fm)
    {
        startTimerHz(10);
    }

    void paint(juce::Graphics& g) override
    {
        using namespace theme;
        const auto area = getLocalBounds().toFloat();
        g.setColour(colour::meterBackground);
        g.fillRoundedRectangle(area, 3.0f);

        const int index = std::clamp(fm_.algorithm_->getIndex(), 0, devices::Operator::kAlgorithms - 1);
        const auto& algorithm = devices::Operator::getAlgorithms()[static_cast<std::size_t>(index)];

        // Depth of each operator: carriers 0, a modulator one above the deepest thing it modulates.
        std::array<int, devices::Operator::kOperators> depth{};
        for (int pass = 0; pass < 4; ++pass)
            for (int target = 0; target < devices::Operator::kOperators; ++target)
                for (int source = 0; source < devices::Operator::kOperators; ++source)
                    if (((algorithm.modulators[static_cast<std::size_t>(target)] >> source) & 1) != 0)
                        depth[static_cast<std::size_t>(source)] = std::max(depth[static_cast<std::size_t>(source)],
                                                                           depth[static_cast<std::size_t>(target)] + 1);
        const int maxDepth = *std::max_element(depth.begin(), depth.end());

        const auto content = area.reduced(10.0f, 22.0f).withTrimmedBottom(10.0f);
        const float rowHeight = content.getHeight() / static_cast<float>(maxDepth + 1);
        std::array<juce::Rectangle<float>, devices::Operator::kOperators> boxes;
        for (int level = 0; level <= maxDepth; ++level)
        {
            std::vector<int> members;
            for (int o = 0; o < devices::Operator::kOperators; ++o)
                if (depth[static_cast<std::size_t>(o)] == level)
                    members.push_back(o);
            const float columnWidth = content.getWidth() / static_cast<float>(std::max<std::size_t>(1, members.size()));
            for (std::size_t m = 0; m < members.size(); ++m)
            {
                const float cx = content.getX() + columnWidth * (static_cast<float>(m) + 0.5f);
                const float cy = content.getBottom() - rowHeight * (static_cast<float>(level) + 0.5f);
                boxes[static_cast<std::size_t>(members[m])] = juce::Rectangle<float>(26.0f, 18.0f).withCentre({cx, cy});
            }
        }

        g.setColour(colour::textSecondary);
        for (int target = 0; target < devices::Operator::kOperators; ++target)
            for (int source = 0; source < devices::Operator::kOperators; ++source)
                if (((algorithm.modulators[static_cast<std::size_t>(target)] >> source) & 1) != 0)
                {
                    const auto from = boxes[static_cast<std::size_t>(source)];
                    const auto to = boxes[static_cast<std::size_t>(target)];
                    g.drawLine(from.getCentreX(), from.getBottom(), to.getCentreX(), to.getY(), 1.5f);
                }

        const float outY = area.getBottom() - 8.0f;
        for (int o = 0; o < devices::Operator::kOperators; ++o)
        {
            const auto box = boxes[static_cast<std::size_t>(o)];
            const bool carrier = ((algorithm.carriers >> o) & 1) != 0;
            if (carrier)
            {
                g.setColour(colour::accent);
                g.drawLine(box.getCentreX(), box.getBottom(), box.getCentreX(), outY, 1.5f);
            }
            if (o == devices::Operator::kOperators - 1 && fm_.feedback_->get() > 0.0f)
            {
                g.setColour(colour::gold);
                g.drawEllipse(box.getRight() - 2.0f, box.getY() - 8.0f, 12.0f, 12.0f, 1.2f);
            }
            g.setColour(carrier ? colour::accent.withAlpha(0.35f) : colour::control);
            g.fillRoundedRectangle(box, 3.0f);
            g.setColour(colour::textPrimary);
            g.setFont(sansBold(11.0f));
            g.drawText(juce::String::charToString(static_cast<juce::juce_wchar>('A' + o)), box, juce::Justification::centred, false);
        }
        g.setColour(colour::accent);
        g.fillRect(content.getX(), outY, content.getWidth(), 1.5f);

        g.setColour(colour::textSecondary);
        g.setFont(sans(10.5f));
        g.drawText("Algorithm " + juce::String(index + 1) + ":  " + algorithm.description, area.reduced(6.0f, 3.0f),
                   juce::Justification::topLeft, true);
    }

private:
    void timerCallback() override
    {
        const int algorithm = fm_.algorithm_->getIndex();
        const bool feedback = fm_.feedback_->get() > 0.0f;
        if (algorithm != shownAlgorithm_ || feedback != shownFeedback_)
        {
            shownAlgorithm_ = algorithm;
            shownFeedback_ = feedback;
            repaint();
        }
    }

    devices::Operator& fm_;
    int shownAlgorithm_ = -1;
    bool shownFeedback_ = false;
};

//==================================================================================================
/** Impulse: eight sample slots (drop files from the browser or the file manager) and the selected
    slot's settings. */
class ImpulseEditor final : public juce::Component,
                            public juce::FileDragAndDropTarget,
                            public juce::DragAndDropTarget,
                            private juce::Timer
{
public:
    explicit ImpulseEditor(devices::Impulse& impulse)
        : impulse_(impulse)
    {
        loadButton_.setButtonText("Load...");
        loadButton_.onClick = [this] { chooseFile(selected_); };
        addAndMakeVisible(loadButton_);
        globals_.add(*impulse_.link_, "Link 7/8");
        globals_.add(*impulse_.transpose_, "Transpose");
        globals_.add(*impulse_.volume_, "Volume");
        addAndMakeVisible(globals_);
        addAndMakeVisible(slotGrid_);
        selectSlot(0);
        startTimerHz(30);
    }

    static constexpr int kSlotAreaWidth = 300;
    static int preferredWidth() { return kSlotAreaWidth + 8 * ParameterControl::kWidth + 40; }

    void resized() override
    {
        auto area = getLocalBounds();
        slots_ = area.removeFromLeft(kSlotAreaWidth).reduced(2);
        area.removeFromLeft(6);
        globals_.setBounds(area.removeFromRight(2 * ParameterControl::kWidth).withTrimmedTop(24));
        area.removeFromRight(8);
        auto header = area.removeFromTop(24);
        loadButton_.setBounds(header.removeFromRight(70).reduced(2, 1));
        title_ = header;
        slotGrid_.setBounds(area);
    }

    void paint(juce::Graphics& g) override
    {
        using namespace theme;
        for (int slot = 0; slot < devices::Impulse::kSlots; ++slot)
        {
            const auto box = slotBounds(slot).toFloat().reduced(2.0f);
            const auto sample = impulse_.getSlotSample(slot);
            g.setColour(slot == selected_ ? colour::controlHover : colour::control);
            g.fillRoundedRectangle(box, 4.0f);
            const float light = activity_[static_cast<std::size_t>(slot)];
            if (light > 0.01f)
            {
                g.setColour(colour::accent.withAlpha(juce::jlimit(0.0f, 0.8f, light)));
                g.fillRoundedRectangle(box, 4.0f);
            }
            if (slot == dropSlot_)
            {
                g.setColour(colour::gold);
                g.drawRoundedRectangle(box, 4.0f, 2.0f);
            }
            g.setColour(colour::textMuted);
            g.setFont(mono(9.0f));
            g.drawText(juce::String(slot + 1) + "  " + devices::Impulse::getSlotNoteName(slot), box.reduced(5.0f, 3.0f),
                       juce::Justification::topLeft, false);
            g.setColour(sample != nullptr ? colour::textPrimary : colour::textMuted);
            g.setFont(sans(10.5f));
            g.drawFittedText(sample != nullptr ? sample->file.getFileNameWithoutExtension() : juce::String("drop a sample"),
                             box.reduced(5.0f, 3.0f).toNearestInt(), juce::Justification::bottomLeft, 2);
        }

        g.setColour(colour::textPrimary);
        g.setFont(sansBold(12.0f));
        const auto sample = impulse_.getSlotSample(selected_);
        g.drawText("Slot " + juce::String(selected_ + 1) + (sample != nullptr ? "  " + sample->file.getFileName() : juce::String()),
                   title_, juce::Justification::centredLeft, true);
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        const int slot = slotAt(e.getPosition());
        if (slot < 0)
            return;
        selectSlot(slot);
        if (e.mods.isPopupMenu())
        {
            juce::Component::SafePointer<ImpulseEditor> safe(this);
            juce::PopupMenu menu;
            menu.addItem("Load Sample...", [safe, slot] { if (safe != nullptr) safe->chooseFile(slot); });
            menu.addItem("Clear Slot", impulse_.getSlotSample(slot) != nullptr, false, [safe, slot]
                         {
                             if (safe != nullptr)
                             {
                                 safe->impulse_.setSlotSample(slot, nullptr);
                                 safe->repaint();
                             }
                         });
            menu.showMenuAsync(juce::PopupMenu::Options().withMousePosition());
            return;
        }
        impulse_.triggerSlot(slot, 0.9f);
    }

    // Files from the file manager...
    bool isInterestedInFileDrag(const juce::StringArray& files) override { return !files.isEmpty(); }
    void fileDragMove(const juce::StringArray&, int x, int y) override
    {
        dropSlot_ = slotAt({x, y});
        repaint();
    }
    void fileDragExit(const juce::StringArray&) override
    {
        dropSlot_ = -1;
        repaint();
    }
    void filesDropped(const juce::StringArray& files, int x, int y) override { dropFiles(files, {x, y}); }

    // ...and from the browser: onto any slot.
    bool isInterestedInDragSource(const SourceDetails& details) override { return browserDrag::isFileDrag(details.description); }
    void itemDragMove(const SourceDetails& details) override
    {
        dropSlot_ = slotAt(details.localPosition);
        repaint();
    }
    void itemDragExit(const SourceDetails&) override
    {
        dropSlot_ = -1;
        repaint();
    }
    void itemDropped(const SourceDetails& details) override { dropFiles(browserDrag::getFiles(details.description), details.localPosition); }

    void dropFiles(const juce::StringArray& files, juce::Point<int> position)
    {
        dropSlot_ = -1;
        int slot = slotAt(position);
        if (slot < 0)
            slot = selected_;
        const int first = slot;
        for (const auto& path : files)
        {
            if (slot >= devices::Impulse::kSlots)
                break;
            load(slot++, juce::File(path));
        }
        if (!files.isEmpty())
            selectSlot(first);
        repaint();
    }

    [[nodiscard]] juce::Rectangle<int> slotBounds(int slot) const
    {
        const int column = slot % 4;
        const int row = slot / 4;
        const int w = slots_.getWidth() / 4;
        const int h = slots_.getHeight() / 2;
        return {slots_.getX() + column * w, slots_.getY() + row * h, w, h};
    }

    [[nodiscard]] int slotAt(juce::Point<int> p) const
    {
        for (int slot = 0; slot < devices::Impulse::kSlots; ++slot)
            if (slotBounds(slot).contains(p))
                return slot;
        return -1;
    }

    [[nodiscard]] int getSelectedSlot() const noexcept { return selected_; }

private:
    void selectSlot(int slot)
    {
        selected_ = slot;
        slotGrid_.clear();
        const auto& p = impulse_.getSlot(slot);
        slotGrid_.add(*p.start, "Start");
        slotGrid_.add(*p.transpose, "Transpose");
        slotGrid_.add(*p.drive, "Drive");
        slotGrid_.add(*p.transient, "Transient");
        slotGrid_.add(*p.decay, "Decay");
        slotGrid_.add(*p.volume, "Volume");
        slotGrid_.add(*p.filter, "Filter");
        slotGrid_.add(*p.cutoff, "Cutoff");
        slotGrid_.add(*p.resonance, "Resonance");
        slotGrid_.add(*p.pan, "Pan");
        slotGrid_.add(*p.velocity, "Velocity");
        slotGrid_.resized();
        repaint();
    }

    void load(int slot, const juce::File& file)
    {
        juce::String error;
        if (!impulse_.loadSlotSample(slot, file, error))
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Could not load the sample", error);
    }

    void chooseFile(int slot)
    {
        chooser_ = std::make_unique<juce::FileChooser>("Load Slot Sample", juce::File::getSpecialLocation(juce::File::userMusicDirectory),
                                                       "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
        juce::Component::SafePointer<ImpulseEditor> safe(this);
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [safe, slot](const juce::FileChooser& chooser)
                              {
                                  if (safe != nullptr && chooser.getResult().existsAsFile())
                                  {
                                      safe->load(slot, chooser.getResult());
                                      safe->repaint();
                                  }
                              });
    }

    void timerCallback() override
    {
        bool changed = false;
        for (int slot = 0; slot < devices::Impulse::kSlots; ++slot)
        {
            auto& shown = activity_[static_cast<std::size_t>(slot)];
            const float next = std::max(impulse_.takeSlotActivity(slot), shown * 0.8f);
            changed = changed || std::abs(next - shown) > 0.005f;
            shown = next;
        }
        if (changed)
            repaint(slots_);
    }

    devices::Impulse& impulse_;
    juce::TextButton loadButton_;
    ParameterGrid slotGrid_;
    ParameterGrid globals_;
    juce::Rectangle<int> slots_;
    juce::Rectangle<int> title_;
    int selected_ = 0;
    int dropSlot_ = -1;
    std::array<float, devices::Impulse::kSlots> activity_{};
    std::unique_ptr<juce::FileChooser> chooser_;
};

//==================================================================================================
// Page layouts of the new devices.

std::vector<PagedEditor::Page> wavetablePages(devices::WavetableSynth& synth)
{
    std::vector<PagedEditor::Page> pages;
    for (int o = 0; o < 2; ++o)
    {
        const juce::String id = "o" + juce::String(o + 1);
        pages.push_back({"Osc " + juce::String(o + 1),
                         {{id + "on", "On"}, {id + "table", "Table"}, {id + "pos", "Position"}, {id + "level", "Level"},
                          {id + "oct", "Octave"}, {id + "semi", "Semi"}, {id + "fine", "Fine"}},
                         [&synth, o] { return std::make_unique<WavetableDisplay>(synth, o); },
                         180});
    }
    pages.push_back({"Voice",
                     {{"unison", "Unison"}, {"unidetune", "Detune"}, {"unispread", "Spread"}, {"glide", "Glide"},
                      {"sub", "Sub"}, {"noise", "Noise"}, {"velamp", "Velocity"}, {"volume", "Volume"}},
                     {},
                     0});
    pages.push_back({"Filter", {{"ftype", "Type"}, {"cutoff", "Cutoff"}, {"res", "Resonance"}, {"drive", "Drive"}, {"keytrack", "Key Track"}}, {}, 0});
    pages.push_back({"Amp Env", {{"a1", "Attack"}, {"d1", "Decay"}, {"s1", "Sustain"}, {"r1", "Release"}}, {}, 0});
    pages.push_back({"Env 2/3",
                     {{"a2", "Attack 2"}, {"d2", "Decay 2"}, {"s2", "Sustain 2"}, {"r2", "Release 2"},
                      {"a3", "Attack 3"}, {"d3", "Decay 3"}, {"s3", "Sustain 3"}, {"r3", "Release 3"}},
                     {},
                     0});
    pages.push_back({"LFOs", {{"l1shape", "LFO 1"}, {"l1rate", "Rate 1"}, {"l2shape", "LFO 2"}, {"l2rate", "Rate 2"}}, {}, 0});
    pages.push_back({"Matrix", {}, [&synth] { return std::make_unique<MatrixView>(synth); }, 460});
    return pages;
}

std::vector<PagedEditor::Page> operatorPages(devices::Operator& fm)
{
    std::vector<PagedEditor::Page> pages;
    pages.push_back({"Algorithm",
                     {{"algo", "Algorithm"}, {"feedback", "Feedback"}, {"transpose", "Transpose"}, {"glide", "Glide"}, {"volume", "Volume"}},
                     [&fm] { return std::make_unique<AlgorithmDisplay>(fm); },
                     200});
    for (int o = 0; o < devices::Operator::kOperators; ++o)
    {
        const juce::String letter = juce::String::charToString(static_cast<juce::juce_wchar>('A' + o));
        const juce::String id = letter.toLowerCase();
        pages.push_back({"Osc " + letter,
                         {{id + "wave", "Wave"}, {id + "coarse", "Coarse"}, {id + "fine", "Fine"}, {id + "fixed", "Fixed"},
                          {id + "freq", "Fixed Hz"}, {id + "level", "Level"}, {id + "a", "Attack"}, {id + "d", "Decay"},
                          {id + "s", "Sustain"}, {id + "r", "Release"}, {id + "vel", "Velocity"}},
                         {},
                         0});
    }
    pages.push_back({"Filter",
                     {{"fon", "On"}, {"ftype", "Type"}, {"cutoff", "Cutoff"}, {"res", "Resonance"}, {"fenv", "Env Amt"},
                      {"fa", "Attack"}, {"fd", "Decay"}, {"fs", "Sustain"}, {"fr", "Release"}},
                     {},
                     0});
    pages.push_back({"Pitch/LFO",
                     {{"penv", "Pitch Env"}, {"pdecay", "Decay"}, {"lshape", "LFO"}, {"lrate", "Rate"}, {"ldest", "Target"}, {"lamount", "Amount"}},
                     {},
                     0});
    return pages;
}

std::vector<PagedEditor::Page> echoPages()
{
    return {
        {"Echo",
         {{"mode", "Mode"}, {"sync", "Sync"}, {"divl", "Left"}, {"divr", "Right"}, {"timel", "Left ms"}, {"timer", "Right ms"},
          {"link", "Link"}, {"feedback", "Feedback"}, {"invert", "Invert"}, {"input", "Input"}, {"width", "Width"}, {"mix", "Dry/Wet"}},
         {},
         0},
        {"Filter & Mod",
         {{"filter", "Filter"}, {"hp", "High Pass"}, {"lp", "Low Pass"}, {"res", "Resonance"}, {"modrate", "Mod Rate"},
          {"moddepth", "Mod Time"}, {"modfilter", "Mod Filter"}},
         {},
         0},
        {"Character",
         {{"drive", "Drive"}, {"noise", "Noise"}, {"wobble", "Wobble"}, {"duck", "Duck"}, {"reverb", "Reverb"},
          {"reverbdecay", "Decay"}, {"reverbloc", "Place"}},
         {},
         0},
    };
}

} // namespace

std::unique_ptr<juce::Component> createDeviceEditor(engine::ProcessorNode& node, const DeviceEditorContext& context, int& preferredWidth)
{
    auto& processor = node.getProcessor();
    if (auto more = createMoreDeviceEditor(processor, context, preferredWidth))
        return more;

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
        preferredWidth = LimiterEditor::kPreferredWidth;
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
    if (auto* impulse = dynamic_cast<devices::Impulse*>(&processor))
    {
        preferredWidth = ImpulseEditor::preferredWidth();
        return std::make_unique<ImpulseEditor>(*impulse);
    }
    if (auto* synth = dynamic_cast<devices::WavetableSynth*>(&processor))
    {
        auto pages = wavetablePages(*synth);
        preferredWidth = PagedEditor::preferredWidth(pages);
        return std::make_unique<PagedEditor>(processor, std::move(pages));
    }
    if (auto* fm = dynamic_cast<devices::Operator*>(&processor))
    {
        auto pages = operatorPages(*fm);
        preferredWidth = PagedEditor::preferredWidth(pages);
        return std::make_unique<PagedEditor>(processor, std::move(pages));
    }
    if (auto* click = dynamic_cast<devices::Click*>(&processor))
    {
        preferredWidth = ClickEditor::kParametersWidth + 6 + TempoDetectPanel::kPreferredWidth;
        return std::make_unique<ClickEditor>(*click, context);
    }
    if (dynamic_cast<devices::Echo*>(&processor) != nullptr)
    {
        auto pages = echoPages();
        preferredWidth = PagedEditor::preferredWidth(pages);
        return std::make_unique<PagedEditor>(processor, std::move(pages));
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
