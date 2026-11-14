#include "ui/devices/MoreDeviceEditors.h"

#include "devices/AnalogSynth.h"
#include "devices/AutoFilter.h"
#include "devices/ChannelEq.h"
#include "devices/DriftSynth.h"
#include "devices/Drive.h"
#include "devices/Gate.h"
#include "devices/LfoDevice.h"
#include "devices/Meld.h"
#include "devices/Modulators.h"
#include "devices/MultibandDynamics.h"
#include "devices/Roar.h"
#include "engine/RenderGraph.h"
#include "ui/devices/PagedEditor.h"
#include "ui/devices/StudioDeviceEditors.h"
#include "ui/theme/DojoTheme.h"

#include <cmath>

namespace dojo::ui
{
namespace
{
using Page = PagedEditor::Page;
using Parameters = std::vector<std::pair<juce::String, juce::String>>;

constexpr double kMinFrequency = 20.0;
constexpr double kMaxFrequency = 20000.0;

/** A display's background, with a title in the corner. */
void paintPanel(juce::Graphics& g, juce::Rectangle<float> area, const juce::String& title)
{
    using namespace theme;
    g.setColour(colour::meterBackground);
    g.fillRoundedRectangle(area, 3.0f);
    if (title.isNotEmpty())
    {
        g.setColour(colour::textMuted);
        g.setFont(label(9.0f));
        g.drawText(title, area.reduced(6.0f, 3.0f), juce::Justification::topLeft, false);
    }
}

//==================================================================================================
/** A frequency response (dB over 20 Hz .. 20 kHz) with an optional marker (a modulated cutoff). */
class ResponseDisplay final : public juce::Component, private juce::Timer
{
public:
    ResponseDisplay(juce::String title, std::function<double(double)> responseDb, double rangeDb, std::function<double()> marker = {})
        : title_(std::move(title))
        , response_(std::move(responseDb))
        , range_(rangeDb)
        , marker_(std::move(marker))
    {
        startTimerHz(20);
    }

    void paint(juce::Graphics& g) override
    {
        using namespace theme;
        const auto area = getLocalBounds().toFloat();
        paintPanel(g, area, title_);
        const auto plot = area.reduced(6.0f, 14.0f);
        const auto xOf = [&](double f) { return plot.getX() + static_cast<float>(std::log(f / kMinFrequency) / std::log(kMaxFrequency / kMinFrequency)) * plot.getWidth(); };
        const auto yOf = [&](double db) { return plot.getCentreY() - static_cast<float>(std::clamp(db, -range_, range_) / range_) * plot.getHeight() * 0.5f; };

        g.setColour(colour::outline.withAlpha(0.6f));
        for (const double f : {100.0, 1000.0, 10000.0})
            g.drawVerticalLine(juce::roundToInt(xOf(f)), plot.getY(), plot.getBottom());
        g.drawHorizontalLine(juce::roundToInt(yOf(0.0)), plot.getX(), plot.getRight());

        juce::Path curve;
        const int points = std::max(2, static_cast<int>(plot.getWidth()));
        for (int x = 0; x < points; ++x)
        {
            const double f = kMinFrequency * std::pow(kMaxFrequency / kMinFrequency, static_cast<double>(x) / (points - 1));
            const float y = yOf(response_(f));
            if (x == 0)
                curve.startNewSubPath(plot.getX(), y);
            else
                curve.lineTo(plot.getX() + static_cast<float>(x), y);
        }
        g.setColour(colour::accentBright);
        g.strokePath(curve, juce::PathStrokeType(1.8f));
        if (marker_)
        {
            const double f = std::clamp(marker_(), kMinFrequency, kMaxFrequency);
            g.setColour(colour::gold);
            g.drawVerticalLine(juce::roundToInt(xOf(f)), plot.getY(), plot.getBottom());
            g.setFont(mono(9.5f));
            g.drawText(f >= 1000.0 ? juce::String(f / 1000.0, 2) + "k" : juce::String(juce::roundToInt(f)), area.reduced(6.0f, 3.0f),
                       juce::Justification::topRight, false);
        }
    }

private:
    void timerCallback() override
    {
        // Repaint when the curve (sampled at a few points) or the marker moved.
        double signature = marker_ ? marker_() : 0.0;
        for (const double f : {50.0, 200.0, 800.0, 3000.0, 12000.0})
            signature = signature * 1.37 + response_(f);
        if (std::abs(signature - signature_) > 1.0e-6)
        {
            signature_ = signature;
            repaint();
        }
    }

    juce::String title_;
    std::function<double(double)> response_;
    double range_;
    std::function<double()> marker_;
    double signature_ = 1.0e300;
};

//==================================================================================================
/** The gate: the detector level against the threshold and return, and whether it is open. */
class GateDisplay final : public juce::Component, private juce::Timer
{
public:
    explicit GateDisplay(devices::Gate& gate)
        : gate_(gate)
    {
        startTimerHz(30);
    }

    void paint(juce::Graphics& g) override
    {
        using namespace theme;
        const auto area = getLocalBounds().toFloat();
        paintPanel(g, area, "LEVEL");
        auto meter = area.reduced(8.0f, 18.0f).withTrimmedBottom(18.0f);
        const auto yOf = [&](double db) { return meter.getBottom() - static_cast<float>(std::clamp((db + 80.0) / 80.0, 0.0, 1.0)) * meter.getHeight(); };
        g.setColour(colour::control);
        g.fillRect(meter);
        g.setColour(colour::meterLow);
        g.fillRect(meter.withTop(yOf(level_)));
        g.setColour(colour::gold);
        g.fillRect(meter.getX() - 3.0f, yOf(gate_.threshold_->get()) - 1.0f, meter.getWidth() + 6.0f, 2.0f);
        g.setColour(colour::textMuted);
        g.fillRect(meter.getX() - 3.0f, yOf(gate_.threshold_->get() - gate_.return_->get()), meter.getWidth() + 6.0f, 1.0f);

        const auto lamp = area.withTop(area.getBottom() - 18.0f).reduced(8.0f, 3.0f);
        g.setColour(colour::play.withAlpha(0.15f + 0.85f * openness_));
        g.fillRoundedRectangle(lamp, 3.0f);
        g.setColour(openness_ > 0.5f ? colour::windowBackground : colour::textSecondary);
        g.setFont(label(9.0f));
        g.drawText(gate_.flip_->get() ? "DUCKED" : "OPEN", lamp, juce::Justification::centred, false);
    }

private:
    void timerCallback() override
    {
        const float peak = gate_.takeDetectorPeakDb();
        level_ = std::max(peak, level_ - 1.5f);
        openness_ = gate_.getOpenness();
        repaint();
    }

    devices::Gate& gate_;
    float level_ = -100.0f, openness_ = 0.0f;
};

//==================================================================================================
/** Multiband Dynamics: each band's level, its two thresholds and its gain change. */
class BandsDisplay final : public juce::Component, private juce::Timer
{
public:
    explicit BandsDisplay(devices::MultibandDynamics& dynamics)
        : dynamics_(dynamics)
    {
        startTimerHz(25);
    }

    void paint(juce::Graphics& g) override
    {
        using namespace theme;
        const auto area = getLocalBounds().toFloat();
        paintPanel(g, area, {});
        constexpr std::array<const char*, 3> names{"LOW", "MID", "HIGH"};
        auto columns = area.reduced(6.0f, 4.0f);
        const float width = columns.getWidth() / 3.0f;
        for (int b = 0; b < 3; ++b)
        {
            auto column = columns.removeFromLeft(width).reduced(4.0f, 0.0f);
            const auto& band = dynamics_.bands_[static_cast<std::size_t>(b)];
            g.setColour(band.on->get() ? colour::textSecondary : colour::textMuted);
            g.setFont(label(9.0f));
            g.drawText(names[static_cast<std::size_t>(b)], column.removeFromTop(14.0f), juce::Justification::centred, false);
            const auto gainText = juce::String(dynamics_.getBandGainDb(b), 1) + " dB";
            g.setFont(mono(9.0f));
            g.drawText(gainText, column.removeFromBottom(13.0f), juce::Justification::centred, false);

            auto level = column.removeFromLeft(column.getWidth() * 0.5f).reduced(2.0f, 0.0f);
            const auto yOf = [&](double db) { return level.getBottom() - static_cast<float>(std::clamp((db + 80.0) / 80.0, 0.0, 1.0)) * level.getHeight(); };
            g.setColour(colour::control);
            g.fillRect(level);
            g.setColour(colour::meterLow);
            g.fillRect(level.withTop(yOf(dynamics_.getBandLevelDb(b))));
            g.setColour(colour::gold);
            g.fillRect(level.getX() - 2.0f, yOf(band.aboveThreshold->get()) - 1.0f, level.getWidth() + 4.0f, 2.0f);
            g.setColour(colour::accentBright);
            g.fillRect(level.getX() - 2.0f, yOf(band.belowThreshold->get()) - 1.0f, level.getWidth() + 4.0f, 2.0f);

            // Gain change: up from the middle (lifted) or down (reduced), +-24 dB.
            auto change = column.reduced(3.0f, 0.0f);
            const float middle = change.getCentreY();
            const float gain = std::clamp(dynamics_.getBandGainDb(b), -24.0f, 24.0f) / 24.0f * change.getHeight() * 0.5f;
            g.setColour(colour::control);
            g.fillRect(change);
            g.setColour(gain >= 0.0f ? colour::play : colour::record);
            g.fillRect(juce::Rectangle<float>(change.getX(), std::min(middle, middle - gain), change.getWidth(), std::abs(gain)));
            g.setColour(colour::outline);
            g.drawHorizontalLine(juce::roundToInt(middle), change.getX(), change.getRight());
        }
    }

private:
    void timerCallback() override { repaint(); }
    devices::MultibandDynamics& dynamics_;
};

//==================================================================================================
/** Auto Pan: the left and right levels as they move. */
class PanDisplay final : public juce::Component, private juce::Timer
{
public:
    explicit PanDisplay(devices::AutoPan& pan)
        : pan_(pan)
    {
        startTimerHz(30);
    }

    void paint(juce::Graphics& g) override
    {
        using namespace theme;
        const auto area = getLocalBounds().toFloat();
        paintPanel(g, area, "L / R");
        auto bars = area.reduced(10.0f, 20.0f);
        for (int ch = 0; ch < 2; ++ch)
        {
            auto bar = bars.removeFromLeft(bars.getWidth() / (ch == 0 ? 2.0f : 1.0f)).reduced(6.0f, 0.0f);
            g.setColour(colour::control);
            g.fillRect(bar);
            g.setColour(colour::accentBright);
            g.fillRect(bar.withTop(bar.getBottom() - bar.getHeight() * std::clamp(pan_.getGain(ch), 0.0f, 1.0f)));
        }
    }

private:
    void timerCallback() override { repaint(); }
    devices::AutoPan& pan_;
};

//==================================================================================================
/** A modulation matrix: source, target and amount of each slot (Roar, Drift, Meld). */
class MatrixView final : public juce::Component, private juce::Timer
{
public:
    struct Slot
    {
        juce::AudioParameterChoice* source = nullptr;
        juce::AudioParameterChoice* destination = nullptr;
        juce::AudioParameterFloat* amount = nullptr;
    };

    explicit MatrixView(std::vector<Slot> slots)
        : slots_(std::move(slots))
    {
        for (auto& slot : slots_)
        {
            auto row = std::make_unique<Row>();
            int id = 1;
            for (const auto& name : slot.source->choices)
                row->source.addItem(name, id++);
            id = 1;
            for (const auto& name : slot.destination->choices)
                row->destination.addItem(name, id++);
            auto* source = slot.source;
            auto* destination = slot.destination;
            auto* amount = slot.amount;
            auto* r = row.get();
            row->source.onChange = [r, source] { setChoice(source, r->source.getSelectedId() - 1); };
            row->destination.onChange = [r, destination] { setChoice(destination, r->destination.getSelectedId() - 1); };
            row->amount.setSliderStyle(juce::Slider::LinearBar);
            row->amount.setRange(-100.0, 100.0, 0.1);
            row->amount.setTextValueSuffix(" %");
            row->amount.setDoubleClickReturnValue(true, 0.0);
            row->amount.onDragStart = [r, amount]
            {
                r->dragging = true;
                amount->beginChangeGesture();
            };
            row->amount.onDragEnd = [r, amount]
            {
                r->dragging = false;
                amount->endChangeGesture();
            };
            row->amount.onValueChange = [r, amount]
            {
                const auto value = static_cast<float>(r->amount.getValue());
                if (std::abs(value - amount->get()) < 1.0e-4f)
                    return;
                if (!r->dragging)
                    amount->beginChangeGesture();
                amount->setValueNotifyingHost(amount->convertTo0to1(value));
                if (!r->dragging)
                    amount->endChangeGesture();
            };
            for (auto* c : std::initializer_list<juce::Component*>{&row->source, &row->destination, &row->amount})
            {
                c->setWantsKeyboardFocus(false);
                addAndMakeVisible(c);
            }
            rows_.push_back(std::move(row));
        }
        timerCallback();
        startTimerHz(10);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        const int rowHeight = std::clamp(area.getHeight() / std::max(1, static_cast<int>(rows_.size())), 14, 24);
        for (auto& row : rows_)
        {
            auto line = area.removeFromTop(rowHeight).reduced(0, 1);
            row->source.setBounds(line.removeFromLeft(line.getWidth() * 30 / 100));
            line.removeFromLeft(3);
            row->destination.setBounds(line.removeFromLeft(line.getWidth() * 55 / 100));
            line.removeFromLeft(3);
            row->amount.setBounds(line);
        }
    }

private:
    struct Row
    {
        juce::ComboBox source, destination;
        juce::Slider amount;
        bool dragging = false;
    };

    static void setChoice(juce::AudioParameterChoice* parameter, int index)
    {
        if (index < 0 || index == parameter->getIndex())
            return;
        parameter->beginChangeGesture();
        parameter->setValueNotifyingHost(parameter->convertTo0to1(static_cast<float>(index)));
        parameter->endChangeGesture();
    }

    void timerCallback() override
    {
        for (std::size_t s = 0; s < rows_.size(); ++s)
        {
            auto& row = *rows_[s];
            row.source.setSelectedId(slots_[s].source->getIndex() + 1, juce::dontSendNotification);
            row.destination.setSelectedId(slots_[s].destination->getIndex() + 1, juce::dontSendNotification);
            if (!row.dragging)
                row.amount.setValue(slots_[s].amount->get(), juce::dontSendNotification);
        }
    }

    std::vector<Slot> slots_;
    std::vector<std::unique_ptr<Row>> rows_;
};

template <typename SlotArray>
std::vector<MatrixView::Slot> matrixSlots(const SlotArray& slots)
{
    std::vector<MatrixView::Slot> result;
    for (const auto& s : slots)
        result.push_back({s.source, s.destination, s.amount});
    return result;
}

//==================================================================================================
/** A Meld engine: its oscillator and what its two macros are called and set to now. */
class MeldEngineDisplay final : public juce::Component, private juce::Timer
{
public:
    MeldEngineDisplay(devices::Meld& meld, int engine)
        : meld_(meld)
        , engine_(engine)
    {
        startTimerHz(15);
    }

    void paint(juce::Graphics& g) override
    {
        using namespace theme;
        const auto area = getLocalBounds().toFloat();
        paintPanel(g, area, engine_ == 0 ? "ENGINE A" : "ENGINE B");
        const auto& p = meld_.engines_[static_cast<std::size_t>(engine_)];
        const auto oscillator = static_cast<devices::Meld::Oscillator>(p.oscillator->getIndex());
        const auto [first, second] = devices::Meld::getMacroNames(oscillator);
        auto content = area.reduced(8.0f, 16.0f);
        g.setColour(p.on->get() ? colour::accentBright : colour::textMuted);
        g.setFont(sansBold(12.0f));
        g.drawText(devices::Meld::getOscillatorNames()[p.oscillator->getIndex()], content.removeFromTop(18.0f), juce::Justification::centredLeft, false);
        const bool live = meld_.getActiveVoiceCount() > 0;
        const std::array<std::pair<juce::String, float>, 2> macros{
            std::pair{first, live ? meld_.getDisplayMacro(engine_, 0) : p.macro1->get() / 100.0f},
            std::pair{second, live ? meld_.getDisplayMacro(engine_, 1) : p.macro2->get() / 100.0f}};
        for (const auto& [name, value] : macros)
        {
            content.removeFromTop(6.0f);
            g.setColour(colour::textSecondary);
            g.setFont(label(9.0f));
            g.drawText(name.toUpperCase(), content.removeFromTop(12.0f), juce::Justification::centredLeft, false);
            const auto bar = content.removeFromTop(8.0f);
            g.setColour(colour::control);
            g.fillRoundedRectangle(bar, 2.0f);
            g.setColour(colour::accent);
            g.fillRoundedRectangle(bar.withWidth(bar.getWidth() * std::clamp(value, 0.0f, 1.0f)), 2.0f);
        }
    }

private:
    void timerCallback() override { repaint(); }
    devices::Meld& meld_;
    int engine_;
};

//==================================================================================================
// Pages

Parameters ids(std::initializer_list<const char*> idsAndCaptions)
{
    // "id|Caption" pairs.
    Parameters result;
    for (const auto* entry : idsAndCaptions)
    {
        const juce::String text(entry);
        result.emplace_back(text.upToFirstOccurrenceOf("|", false, false), text.fromFirstOccurrenceOf("|", false, false));
    }
    return result;
}

Parameters prefixed(const juce::String& prefix, std::initializer_list<const char*> idsAndCaptions)
{
    auto result = ids(idsAndCaptions);
    for (auto& [id, caption] : result)
        id = prefix + id;
    return result;
}

std::vector<Page> gatePages(devices::Gate& gate)
{
    return {{"Gate",
             ids({"threshold|Threshold", "return|Return", "attack|Attack", "hold|Hold", "release|Release", "floor|Floor", "flip|Flip",
                  "lookahead|Lookahead", "sidechain|Sidechain", "keylow|Key HP", "keyhigh|Key LP", "listen|Listen"}),
             [&gate] { return std::make_unique<GateDisplay>(gate); },
             90}};
}

std::vector<Page> multibandPages(devices::MultibandDynamics& dynamics)
{
    const auto makeDisplay = [&dynamics] { return std::unique_ptr<juce::Component>(std::make_unique<BandsDisplay>(dynamics)); };
    std::vector<Page> pages;
    for (const auto& [id, title] : {std::pair{"low", "Low"}, std::pair{"mid", "Mid"}, std::pair{"high", "High"}})
        pages.push_back({title,
                         prefixed(id, {"on|On", "athr|Above", "arat|Ratio Above", "bthr|Below", "brat|Ratio Below", "gain|Gain", "solo|Solo"}),
                         makeDisplay,
                         210});
    pages.push_back({"Global",
                     ids({"xlow|Low/Mid", "xhigh|Mid/High", "attack|Attack", "release|Release", "detection|Detection", "knee|Soft Knee",
                          "amount|Amount", "output|Output", "mix|Dry/Wet"}),
                     makeDisplay,
                     210});
    return pages;
}

std::vector<Page> autoFilterPages(devices::AutoFilter& filter)
{
    const auto makeDisplay = [&filter]
    {
        return std::unique_ptr<juce::Component>(std::make_unique<ResponseDisplay>(
            "AUTO FILTER", [&filter](double f) { return filter.getResponseDb(f, filter.getModulatedCutoff()); }, 30.0,
            [&filter] { return static_cast<double>(filter.getModulatedCutoff()); }));
    };
    return {{"Filter",
             ids({"type|Type", "slope|Slope", "circuit|Circuit", "freq|Frequency", "res|Resonance", "morph|Morph", "drive|Drive",
                  "output|Output", "mix|Dry/Wet"}),
             makeDisplay,
             210},
            {"LFO",
             ids({"lfoamt|Amount", "lforate|Rate", "lfosync|Sync", "lfodiv|Division", "lfoshape|Shape", "lfostereo|Stereo",
                  "lfooffset|Offset"}),
             makeDisplay,
             210},
            {"Envelope", ids({"envamt|Amount", "envattack|Attack", "envrelease|Release", "sidechain|Sidechain"}), makeDisplay, 210}};
}

std::vector<Page> channelEqPages(devices::ChannelEq& eq)
{
    return {{"Channel EQ",
             ids({"lowcut|Low Cut", "low|Low", "midfreq|Mid Freq", "mid|Mid", "high|High", "output|Output"}),
             [&eq]
             {
                 return std::unique_ptr<juce::Component>(
                     std::make_unique<ResponseDisplay>("CHANNEL EQ", [&eq](double f) { return eq.getResponseDb(f); }, 18.0));
             },
             220}};
}

std::vector<Page> roarPages(devices::Roar& roar)
{
    std::vector<Page> pages;
    pages.push_back({"Input & Output",
                     ids({"drive|Drive", "tone|Tone", "routing|Routing", "xlow|Low/Mid", "xhigh|Mid/High", "comp|Compressor",
                          "output|Output", "mix|Dry/Wet"}),
                     {},
                     0});
    for (int s = 1; s <= devices::Roar::kStages; ++s)
        pages.push_back({"Stage " + juce::String(s),
                         prefixed("s" + juce::String(s), {"on|On", "shape|Shape", "amount|Amount", "bias|Bias", "filter|Filter", "pre|Pre",
                                                          "freq|Frequency", "res|Resonance", "level|Level"}),
                         {},
                         0});
    pages.push_back({"Modulation",
                     ids({"l1shape|LFO 1", "l1rate|Rate 1", "l1sync|Sync 1", "l1div|Div 1", "l2shape|LFO 2", "l2rate|Rate 2", "l2sync|Sync 2",
                          "l2div|Div 2", "envattack|Env Att", "envrelease|Env Rel", "envgain|Env Gain", "noiserate|Noise", "noisesmooth|Smooth"}),
                     {},
                     0});
    pages.push_back({"Matrix", {}, [&roar] { return std::unique_ptr<juce::Component>(std::make_unique<MatrixView>(matrixSlots(roar.matrix_))); }, 420});
    pages.push_back({"Feedback",
                     ids({"fbamount|Amount", "fbtime|Time", "fbsync|Sync", "fbdiv|Division", "fbfreq|Frequency", "fbinvert|Invert"}),
                     {},
                     0});
    return pages;
}

std::vector<Page> shifterPages()
{
    return {{"Shift", ids({"mode|Mode", "pitch|Pitch", "fine|Fine", "shift|Shift Hz", "spread|Spread", "window|Window", "mix|Dry/Wet"}), {}, 0},
            {"LFO & Env",
             ids({"lfoamt|LFO", "lforate|Rate", "lfosync|Sync", "lfodiv|Division", "lfoshape|Shape", "lfostereo|Stereo", "envamt|Env",
                  "envattack|Attack", "envrelease|Release"}),
             {},
             0},
            {"Feedback", ids({"delay|Delay", "delaysync|Sync", "delaydiv|Division", "feedback|Feedback"}), {}, 0}};
}

std::vector<Page> analogPages()
{
    const auto oscillator = [](const juce::String& prefix)
    {
        return prefixed(prefix, {"on|On", "shape|Shape", "oct|Octave", "semi|Semi", "detune|Detune", "pw|Width", "pwm|PWM",
                                 "lfo|LFO Pitch", "level|Level", "f2|To F2"});
    };
    const auto filter = [](const juce::String& f, const juce::String& e)
    {
        auto parameters = prefixed(f, {"type|Type", "cutoff|Cutoff", "res|Resonance", "drive|Drive", "key|Key", "env|Env", "lfo|LFO"});
        const auto envelope = prefixed(e, {"a|Attack", "d|Decay", "s|Sustain", "r|Release"});
        parameters.insert(parameters.end(), envelope.begin(), envelope.end());
        return parameters;
    };
    auto osc2 = oscillator("o2");
    for (const auto& extra : ids({"o2sync|Sync", "noise|Noise", "noisecolor|Colour", "noisef2|Noise F2"}))
        osc2.push_back(extra);
    auto filter1 = filter("f1", "fe1");
    filter1.emplace_back("route", "Routing");
    return {{"Osc 1", oscillator("o1"), {}, 0},
            {"Osc 2 & Noise", osc2, {}, 0},
            {"Filter 1", filter1, {}, 0},
            {"Filter 2", filter("f2", "fe2"), {}, 0},
            {"Amps",
             ids({"ae1a|Attack 1", "ae1d|Decay 1", "ae1s|Sustain 1", "ae1r|Release 1", "a1level|Level 1", "a1pan|Pan 1", "a1trem|Trem 1",
                  "ae2a|Attack 2", "ae2d|Decay 2", "ae2s|Sustain 2", "ae2r|Release 2", "a2level|Level 2", "a2pan|Pan 2", "a2trem|Trem 2"}),
             {},
             0},
            {"LFOs", ids({"l1shape|LFO 1", "l1rate|Rate 1", "l1sync|Sync 1", "l1div|Div 1", "l2shape|LFO 2", "l2rate|Rate 2", "l2sync|Sync 2", "l2div|Div 2"}), {}, 0},
            {"Voice",
             ids({"voices|Voices", "legato|Legato", "glide|Glide", "unison|Unison", "unidetune|Detune", "vibamt|Vibrato", "vibrate|Vib Rate",
                  "vibdelay|Vib Delay", "velamp|Vel Amp", "velfilter|Vel Filter", "drift|Drift", "bend|Bend", "volume|Volume"}),
             {},
             0}};
}

std::vector<Page> driftPages(devices::DriftSynth& drift)
{
    return {{"Oscillators",
             ids({"o1shape|Osc 1", "o1amt|Shape", "o1oct|Octave 1", "o1modsrc|Shape Mod", "o1mod|Mod Amt", "o1level|Level 1", "o2shape|Osc 2",
                  "o2oct|Octave 2", "o2detune|Detune", "o2level|Level 2", "noise|Noise", "pitchenv|Pitch Env", "pitchlfo|Pitch LFO"}),
             {},
             0},
            {"Filter", ids({"ftype|Type", "freq|Frequency", "res|Resonance", "key|Key", "fenv|Env 2", "flfo|LFO", "hp|High Pass"}), {}, 0},
            {"Envelopes",
             ids({"e1a|Attack 1", "e1d|Decay 1", "e1s|Sustain 1", "e1r|Release 1", "e2a|Attack 2", "e2d|Decay 2", "e2s|Sustain 2", "e2r|Release 2",
                  "e2cycle|Cycle"}),
             {},
             0},
            {"LFO", ids({"lfoshape|Shape", "lforate|Rate", "lfosync|Sync", "lfodiv|Division", "lfoamt|Amount", "lforetrig|Retrigger"}), {}, 0},
            {"Mod Matrix", {}, [&drift] { return std::unique_ptr<juce::Component>(std::make_unique<MatrixView>(matrixSlots(drift.matrix_))); }, 420},
            {"Voice",
             ids({"mode|Mode", "voices|Voices", "spread|Spread", "glide|Glide", "legato|Legato", "drift|Drift", "mpe|MPE", "notebend|Note Bend",
                  "bend|Bend", "velamp|Vel Amp", "volume|Volume"}),
             {},
             0}};
}

std::vector<Page> meldPages(devices::Meld& meld)
{
    std::vector<Page> pages;
    for (int e = 0; e < 2; ++e)
    {
        const juce::String prefix = e == 0 ? "a" : "b";
        const juce::String name = e == 0 ? "A" : "B";
        const auto makeDisplay = [&meld, e] { return std::unique_ptr<juce::Component>(std::make_unique<MeldEngineDisplay>(meld, e)); };
        pages.push_back({"Engine " + name,
                         prefixed(prefix, {"on|On", "osc|Oscillator", "m1|Macro 1", "m2|Macro 2", "oct|Octave", "semi|Semi", "fine|Fine",
                                           "level|Level", "pan|Pan"}),
                         makeDisplay,
                         140});
        pages.push_back({name + " Filter & Env",
                         prefixed(prefix, {"ftype|Filter", "circuit|Circuit", "cutoff|Cutoff", "res|Resonance", "key|Key", "aa|Amp Att",
                                           "ad|Amp Dec", "as|Amp Sus", "ar|Amp Rel", "ma|Mod Att", "md|Mod Dec", "ms|Mod Sus", "mr|Mod Rel"}),
                         {},
                         0});
        pages.push_back({name + " LFOs",
                         prefixed(prefix, {"l1shape|LFO 1", "l1rate|Rate 1", "lsync|Sync 1", "ldiv|Div 1", "l2shape|LFO 2", "l2rate|Rate 2"}),
                         makeDisplay,
                         140});
    }
    pages.push_back({"Matrix", {}, [&meld] { return std::unique_ptr<juce::Component>(std::make_unique<MatrixView>(matrixSlots(meld.matrix_))); }, 440});
    pages.push_back({"Global",
                     ids({"mode|Mode", "split|Split", "voices|Voices", "glide|Glide", "mpe|MPE", "notebend|Note Bend", "bend|Bend",
                          "velamp|Vel Amp", "volume|Volume"}),
                     {},
                     0});
    return pages;
}

std::unique_ptr<juce::Component> paged(juce::AudioProcessor& processor, std::vector<Page> pages, int& preferredWidth)
{
    preferredWidth = PagedEditor::preferredWidth(pages);
    return std::make_unique<PagedEditor>(processor, std::move(pages));
}
} // namespace

//==================================================================================================
// LFO editor

/** The LFO's cycle, where it is now, and its output. */
class LfoEditor::Scope final : public juce::Component, private juce::Timer
{
public:
    explicit Scope(devices::LfoDevice& lfo)
        : lfo_(lfo)
    {
        startTimerHz(30);
    }

    void paint(juce::Graphics& g) override
    {
        using namespace theme;
        const auto area = getLocalBounds().toFloat();
        paintPanel(g, area, "LFO");
        auto plot = area.reduced(8.0f, 18.0f);
        auto bar = plot.removeFromRight(10.0f);
        plot.removeFromRight(6.0f);
        juce::Path wave;
        const int points = std::max(2, static_cast<int>(plot.getWidth()));
        for (int x = 0; x < points; ++x)
        {
            const double phase = static_cast<double>(x) / (points - 1);
            const float y = plot.getBottom() - static_cast<float>(lfo_.shapeAt(phase)) * plot.getHeight();
            if (x == 0)
                wave.startNewSubPath(plot.getX(), y);
            else
                wave.lineTo(plot.getX() + static_cast<float>(x), y);
        }
        g.setColour(colour::accent.withAlpha(0.8f));
        g.strokePath(wave, juce::PathStrokeType(1.6f));
        const float output = std::clamp(lfo_.getOutput(), 0.0f, 1.0f);
        const float x = plot.getX() + lfo_.getPhase() * plot.getWidth();
        const float y = plot.getBottom() - output * plot.getHeight();
        g.setColour(colour::gold);
        g.fillEllipse(x - 4.0f, y - 4.0f, 8.0f, 8.0f);
        g.setColour(colour::control);
        g.fillRect(bar);
        g.setColour(colour::gold);
        g.fillRect(bar.withTop(bar.getBottom() - output * bar.getHeight()));
    }

private:
    void timerCallback() override { repaint(); }
    devices::LfoDevice& lfo_;
};

/** One mapping: what it moves, its range, and remove. */
class LfoEditor::Row final : public juce::Component
{
public:
    Row(LfoEditor& owner, juce::ValueTree mapping, int index)
        : owner_(owner)
        , mapping_(std::move(mapping))
        , index_(index)
    {
        name_.setText(owner_.describe(mapping_), juce::dontSendNotification);
        name_.setFont(theme::sans(11.0f));
        name_.setColour(juce::Label::textColourId, theme::colour::textPrimary);
        name_.setTooltip(name_.getText());
        addAndMakeVisible(name_);
        for (auto* slider : {&minimum_, &maximum_})
        {
            slider->setSliderStyle(juce::Slider::LinearBar);
            slider->setRange(0.0, 100.0, 0.1);
            slider->setTextValueSuffix(" %");
            slider->setWantsKeyboardFocus(false);
            slider->onDragStart = [this] { owner_.project_.getSession().beginTransaction("LFO Range"); };
            slider->onValueChange = [this]
            {
                session::modulation::setRange(mapping_, minimum_.getValue() / 100.0, maximum_.getValue() / 100.0,
                                              &owner_.project_.getSession().getUndoManager());
            };
            addAndMakeVisible(*slider);
        }
        minimum_.setTooltip("Where the LFO's lowest point puts the target");
        maximum_.setTooltip("Where the LFO's highest point puts the target");
        remove_.setButtonText(juce::String::fromUTF8("\xc3\x97"));
        remove_.setTooltip("Remove this mapping (the target goes back to where it was)");
        remove_.setWantsKeyboardFocus(false);
        remove_.onClick = [this]
        {
            // (later: removing the mapping rebuilds the rows, this one included)
            juce::Component::SafePointer<LfoEditor> safe(&owner_);
            juce::MessageManager::callAsync([safe, i = index_] { if (safe != nullptr) safe->unmap(i); });
        };
        addAndMakeVisible(remove_);
        sync();
    }

    void sync()
    {
        minimum_.setValue(session::modulation::getMinimum(mapping_) * 100.0, juce::dontSendNotification);
        maximum_.setValue(session::modulation::getMaximum(mapping_) * 100.0, juce::dontSendNotification);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(0, 1);
        remove_.setBounds(area.removeFromRight(20));
        area.removeFromRight(3);
        maximum_.setBounds(area.removeFromRight(58));
        area.removeFromRight(2);
        minimum_.setBounds(area.removeFromRight(58));
        area.removeFromRight(4);
        name_.setBounds(area);
    }

    [[nodiscard]] const juce::ValueTree& getMapping() const noexcept { return mapping_; }

private:
    LfoEditor& owner_;
    juce::ValueTree mapping_;
    int index_;
    juce::Label name_;
    juce::Slider minimum_, maximum_;
    juce::TextButton remove_;
};

LfoEditor::LfoEditor(devices::LfoDevice& lfo, const DeviceEditorContext& context)
    : lfo_(lfo)
    , project_(context.project)
    , device_(context.device)
{
    scope_ = std::make_unique<Scope>(lfo_);
    addAndMakeVisible(*scope_);
    for (const auto& [id, caption] : ids({"shape|Shape", "rate|Rate", "sync|Sync", "division|Division", "depth|Depth", "offset|Offset",
                                          "phase|Phase", "jitter|Jitter", "smooth|Smooth", "hold|Hold", "retrigger|Retrigger", "invert|Invert"}))
        if (auto* parameter = findParameter(lfo_, id))
            grid_.add(*parameter, caption);
    addAndMakeVisible(grid_);

    mapButton.setTooltip("Map the LFO to any control: a fader, pan or send, or any device parameter, on any track");
    mapButton.onClick = [this] { showMapMenu(); };
    learnButton.setClickingTogglesState(true);
    learnButton.setTooltip("Learn: click any device control in the project to map it");
    learnButton.setColour(juce::TextButton::buttonOnColourId, theme::colour::accent);
    learnButton.onClick = [this]
    {
        if (learnButton.getToggleState())
            beginLearn();
        else
            endLearn();
    };
    for (auto* b : {&mapButton, &learnButton})
    {
        b->setWantsKeyboardFocus(false);
        addAndMakeVisible(b);
    }
    status_.setFont(theme::sans(10.5f));
    status_.setColour(juce::Label::textColourId, theme::colour::textMuted);
    addAndMakeVisible(status_);
    rowsViewport_.setViewedComponent(&rowsHolder_, false);
    rowsViewport_.setScrollBarsShown(true, false);
    addAndMakeVisible(rowsViewport_);

    device_.addListener(this);
    rebuildRows();
    startTimerHz(4);
}

LfoEditor::~LfoEditor()
{
    endLearn();
    device_.removeListener(this);
}

int LfoEditor::preferredWidth()
{
    return kScopeWidth + 6 * ParameterControl::kWidth + kMappingWidth + 24;
}

std::vector<LfoEditor::Candidate> LfoEditor::getCandidates() const
{
    std::vector<Candidate> result;
    const auto& session = project_.getSession();
    const auto addOwner = [&](const juce::ValueTree& owner, std::uint32_t trackId, const juce::String& ownerName, bool master)
    {
        const double volumeDb = master ? session.getMasterVolumeDb() : session::track::getVolumeDb(owner);
        result.push_back({trackId, session::automation::volumeTarget(), engine::gainToAutomation(juce::Decibels::decibelsToGain(volumeDb, -100.0)),
                          ownerName + " > Volume"});
        if (!master)
        {
            result.push_back({trackId, session::automation::panTarget(), 0.5 * (session::track::getPan(owner) + 1.0), ownerName + " > Pan"});
            for (const auto& send : session::Session::getSends(owner))
            {
                const auto bus = session.findTrack(session::send::getTarget(send));
                result.push_back({trackId, session::automation::sendTarget(session::send::getId(send)),
                                  engine::gainToAutomation(juce::Decibels::decibelsToGain(session::send::getGainDb(send), -100.0)),
                                  ownerName + " > Send to " + (bus.isValid() ? session::track::getName(bus) : juce::String("?"))});
            }
        }
        for (const auto& d : session::Session::getDevices(owner))
        {
            if (d == device_)
                continue; // (not its own controls)
            const auto node = project_.getDevices().findNode(session::device::getId(d));
            if (node == nullptr)
                continue;
            const auto& parameters = node->getProcessor().getParameters();
            for (int i = 0; i < parameters.size(); ++i)
                if (parameters[i]->isAutomatable())
                    result.push_back({trackId, session::automation::parameterTarget(session::device::getId(d), i), parameters[i]->getValue(),
                                      ownerName + " > " + session::device::getName(d) + " > " + parameters[i]->getName(40)});
        }
    };
    for (const auto& t : session.getAllTracks())
    {
        const auto kind = session::track::getKind(t);
        if (kind == session::TrackKind::audio || kind == session::TrackKind::instrument || kind == session::TrackKind::bus)
            addOwner(t, session::track::getId(t), session::track::getName(t), false);
    }
    if (const auto master = session.getMaster(); master.isValid())
        addOwner(master, 0, "Master", true);
    return result;
}

bool LfoEditor::map(const Candidate& candidate)
{
    auto& session = project_.getSession();
    session.beginTransaction("Map LFO");
    return session.addModulation(device_, candidate.trackId, candidate.target, candidate.base).isValid();
}

bool LfoEditor::mapParameter(juce::AudioProcessorParameter& parameter)
{
    for (const auto& candidate : getCandidates())
    {
        std::uint32_t deviceId = 0;
        int index = 0;
        if (!session::automation::parseParameterTarget(candidate.target, deviceId, index))
            continue;
        const auto node = project_.getDevices().findNode(deviceId);
        if (node != nullptr && index < node->getProcessor().getParameters().size() && node->getProcessor().getParameters()[index] == &parameter)
            return map(candidate);
    }
    return false;
}

void LfoEditor::unmap(int index)
{
    const auto mappings = session::Session::getModulations(device_);
    if (index < 0 || index >= static_cast<int>(mappings.size()))
        return;
    const auto mapping = mappings[static_cast<std::size_t>(index)];
    // A parameter goes back to where it was (faders, pans and sends never left the session).
    std::uint32_t deviceId = 0;
    int parameterIndex = 0;
    if (session::automation::parseParameterTarget(session::modulation::getTarget(mapping), deviceId, parameterIndex))
        if (const auto node = project_.getDevices().findNode(deviceId); node != nullptr && parameterIndex < node->getProcessor().getParameters().size())
        {
            auto* parameter = node->getProcessor().getParameters()[parameterIndex];
            parameter->beginChangeGesture();
            parameter->setValueNotifyingHost(static_cast<float>(session::modulation::getBase(mapping)));
            parameter->endChangeGesture();
        }
    auto& session = project_.getSession();
    session.beginTransaction("Unmap LFO");
    session.removeModulation(mapping);
}

juce::String LfoEditor::describe(const juce::ValueTree& mapping) const
{
    const auto& session = project_.getSession();
    const auto trackId = session::modulation::getTrack(mapping);
    const auto target = session::modulation::getTarget(mapping);
    std::uint32_t id = 0;
    int index = 0;
    if (session::automation::parseParameterTarget(target, id, index))
    {
        const auto d = session.findDevice(id);
        const auto owner = d.getParent();
        const juce::String ownerName = owner.hasType(session::ids::track) ? session::track::getName(owner) : juce::String("Master");
        juce::String parameterName = "Parameter " + juce::String(index + 1);
        if (const auto node = project_.getDevices().findNode(id); node != nullptr && index < node->getProcessor().getParameters().size())
            parameterName = node->getProcessor().getParameters()[index]->getName(40);
        return ownerName + " > " + (d.isValid() ? session::device::getName(d) : juce::String("?")) + " > " + parameterName;
    }
    const auto track = trackId != 0 ? session.findTrack(trackId) : juce::ValueTree();
    const juce::String ownerName = trackId == 0 ? juce::String("Master") : track.isValid() ? session::track::getName(track) : juce::String("?");
    if (target == session::automation::volumeTarget())
        return ownerName + " > Volume";
    if (target == session::automation::panTarget())
        return ownerName + " > Pan";
    if (std::uint32_t sendId = 0; session::automation::parseSendTarget(target, sendId))
        for (const auto& send : session::Session::getSends(track))
            if (session::send::getId(send) == sendId)
            {
                const auto bus = session.findTrack(session::send::getTarget(send));
                return ownerName + " > Send to " + (bus.isValid() ? session::track::getName(bus) : juce::String("?"));
            }
    return ownerName + " > " + target;
}

juce::StringArray LfoEditor::getMappingDescriptions() const
{
    juce::StringArray result;
    for (const auto& m : session::Session::getModulations(device_))
        result.add(describe(m));
    return result;
}

void LfoEditor::beginLearn()
{
    learning_ = true;
    learnButton.setToggleState(true, juce::dontSendNotification);
    status_.setText("Click any device control to map it", juce::dontSendNotification);
    juce::Component::SafePointer<LfoEditor> safe(this);
    ParameterControl::setMappingHook(
        [safe](juce::AudioProcessorParameter& parameter)
        {
            if (safe == nullptr)
                return;
            if (safe->mapParameter(parameter))
                safe->endLearn();
            else
                safe->status_.setText("That one cannot be mapped (the LFO's own, or already mapped)", juce::dontSendNotification);
        });
}

void LfoEditor::endLearn()
{
    if (!learning_)
        return;
    learning_ = false;
    ParameterControl::setMappingHook(nullptr);
    learnButton.setToggleState(false, juce::dontSendNotification);
    status_.setText({}, juce::dontSendNotification);
}

void LfoEditor::showMapMenu()
{
    const auto candidates = std::make_shared<std::vector<Candidate>>(getCandidates());
    const auto mapped = session::Session::getModulations(device_);
    const auto isMapped = [&mapped](const Candidate& c)
    {
        for (const auto& m : mapped)
            if (session::modulation::getTrack(m) == c.trackId && session::modulation::getTarget(m) == c.target)
                return true;
        return false;
    };

    // Track > (Volume, Pan, Sends, Device > Parameter).
    juce::PopupMenu menu;
    juce::String currentOwner, currentDevice;
    juce::PopupMenu ownerMenu, deviceMenu;
    const auto flushDevice = [&]
    {
        if (currentDevice.isNotEmpty())
            ownerMenu.addSubMenu(currentDevice, deviceMenu);
        deviceMenu = {};
        currentDevice = {};
    };
    const auto flushOwner = [&]
    {
        flushDevice();
        if (currentOwner.isNotEmpty())
            menu.addSubMenu(currentOwner, ownerMenu);
        ownerMenu = {};
        currentOwner = {};
    };
    for (std::size_t i = 0; i < candidates->size(); ++i)
    {
        const auto& c = (*candidates)[i];
        const auto parts = juce::StringArray::fromTokens(c.description, ">", {});
        const auto owner = parts[0].trim();
        if (owner != currentOwner)
        {
            flushOwner();
            currentOwner = owner;
        }
        const int id = static_cast<int>(i) + 1;
        if (parts.size() >= 3)
        {
            const auto deviceName = parts[1].trim();
            if (deviceName != currentDevice)
            {
                flushDevice();
                currentDevice = deviceName;
            }
            deviceMenu.addItem(id, parts[2].trim(), !isMapped(c), isMapped(c));
        }
        else
        {
            ownerMenu.addItem(id, parts[1].trim(), !isMapped(c), isMapped(c));
        }
    }
    flushOwner();
    if (static_cast<int>(mapped.size()) >= session::modulation::kMaxMappings)
    {
        menu = {};
        menu.addItem(1, "All " + juce::String(session::modulation::kMaxMappings) + " mappings are in use", false);
    }

    juce::Component::SafePointer<LfoEditor> safe(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&mapButton),
                       [safe, candidates](int result)
                       {
                           if (safe != nullptr && result > 0 && result <= static_cast<int>(candidates->size()))
                               safe->map((*candidates)[static_cast<std::size_t>(result - 1)]);
                       });
}

void LfoEditor::rebuildRows()
{
    rebuildPending_ = false;
    rows_.clear();
    const auto mappings = session::Session::getModulations(device_);
    for (std::size_t i = 0; i < mappings.size(); ++i)
    {
        auto row = std::make_unique<Row>(*this, mappings[i], static_cast<int>(i));
        rowsHolder_.addAndMakeVisible(*row);
        rows_.push_back(std::move(row));
    }
    if (status_.getText().isEmpty() || !learning_)
        status_.setText(mappings.empty() ? juce::String("Not mapped: Map... or Learn")
                                         : juce::String(static_cast<int>(mappings.size())) + " of " + juce::String(session::modulation::kMaxMappings) + " mapped",
                        juce::dontSendNotification);
    resized();
}

void LfoEditor::timerCallback()
{
    if (rebuildPending_)
        rebuildRows();
}

void LfoEditor::valueTreeChildAdded(juce::ValueTree& parent, juce::ValueTree&)
{
    if (parent == device_)
    {
        rebuildPending_ = true;
        timerCallback();
    }
}

void LfoEditor::valueTreeChildRemoved(juce::ValueTree& parent, juce::ValueTree&, int)
{
    if (parent == device_)
    {
        rebuildPending_ = true;
        // (not now: the row whose button removed it may still be on the stack)
        juce::Component::SafePointer<LfoEditor> safe(this);
        juce::MessageManager::callAsync([safe] { if (safe != nullptr) safe->timerCallback(); });
    }
}

void LfoEditor::valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier&)
{
    if (tree.getParent() == device_)
        for (auto& row : rows_)
            if (row->getMapping() == tree)
                row->sync();
}

void LfoEditor::resized()
{
    auto area = getLocalBounds();
    scope_->setBounds(area.removeFromLeft(kScopeWidth).reduced(2));
    area.removeFromLeft(6);
    grid_.setBounds(area.removeFromLeft(6 * ParameterControl::kWidth).withHeight(2 * ParameterControl::kHeight));
    area.removeFromLeft(10);
    auto header = area.removeFromTop(24);
    learnButton.setBounds(header.removeFromRight(56).reduced(0, 2));
    header.removeFromRight(4);
    mapButton.setBounds(header.removeFromRight(64).reduced(0, 2));
    status_.setBounds(area.removeFromBottom(18));
    rowsViewport_.setBounds(area);
    const int width = rowsViewport_.getMaximumVisibleWidth();
    rowsHolder_.setSize(width, static_cast<int>(rows_.size()) * kRowHeight);
    for (std::size_t i = 0; i < rows_.size(); ++i)
        rows_[i]->setBounds(0, static_cast<int>(i) * kRowHeight, width, kRowHeight);
}

void LfoEditor::paint(juce::Graphics& g)
{
    using namespace theme;
    const int mappingX = kScopeWidth + 6 + 6 * ParameterControl::kWidth + 10;
    g.setColour(colour::textSecondary);
    g.setFont(label(9.5f));
    g.drawText("MAPPINGS", juce::Rectangle<int>(mappingX, 0, 120, 24), juce::Justification::centredLeft, false);
    g.setColour(colour::outline);
    g.drawVerticalLine(mappingX - 6, 4.0f, static_cast<float>(getHeight() - 4));
}

//==================================================================================================

std::unique_ptr<juce::Component> createMoreDeviceEditor(juce::AudioProcessor& processor, const DeviceEditorContext& context, int& preferredWidth)
{
    if (auto* lfo = dynamic_cast<devices::LfoDevice*>(&processor))
    {
        preferredWidth = LfoEditor::preferredWidth();
        return std::make_unique<LfoEditor>(*lfo, context);
    }
    if (auto* gate = dynamic_cast<devices::Gate*>(&processor))
        return paged(processor, gatePages(*gate), preferredWidth);
    if (auto studio = createStudioDeviceEditor(processor, preferredWidth))
        return studio;
    if (auto* dynamics = dynamic_cast<devices::MultibandDynamics*>(&processor))
        return paged(processor, multibandPages(*dynamics), preferredWidth);
    if (auto* filter = dynamic_cast<devices::AutoFilter*>(&processor))
        return paged(processor, autoFilterPages(*filter), preferredWidth);
    if (auto* eq = dynamic_cast<devices::ChannelEq*>(&processor))
        return paged(processor, channelEqPages(*eq), preferredWidth);
    if (dynamic_cast<devices::Pedal*>(&processor) != nullptr)
        return paged(processor,
                     {{"Pedal", ids({"type|Type", "gain|Gain", "output|Output", "bass|Bass", "mid|Mid", "midhigh|Mid High", "treble|Treble", "sub|Sub", "mix|Dry/Wet"}), {}, 0}},
                     preferredWidth);
    if (dynamic_cast<devices::Overdrive*>(&processor) != nullptr)
        return paged(processor,
                     {{"Overdrive", ids({"freq|Frequency", "width|Width", "drive|Drive", "tone|Tone", "dynamics|Dynamics", "output|Output", "mix|Dry/Wet"}), {}, 0}},
                     preferredWidth);
    if (dynamic_cast<devices::Redux*>(&processor) != nullptr)
        return paged(processor,
                     {{"Downsample", ids({"rate|Rate", "jitter|Jitter", "prefilter|Pre-Filter", "postfilter|Post-Filter", "postoct|Octave"}), {}, 0},
                      {"Bits", ids({"bits|Bits", "shape|Shape", "dcshift|DC Shift", "output|Output", "mix|Dry/Wet"}), {}, 0}},
                     preferredWidth);
    if (auto* roar = dynamic_cast<devices::Roar*>(&processor))
        return paged(processor, roarPages(*roar), preferredWidth);
    if (dynamic_cast<devices::Shifter*>(&processor) != nullptr)
        return paged(processor, shifterPages(), preferredWidth);
    if (dynamic_cast<devices::ChorusEnsemble*>(&processor) != nullptr)
        return paged(processor,
                     {{"Chorus-Ensemble",
                       ids({"mode|Mode", "rate|Rate", "amount|Amount", "feedback|Feedback", "width|Width", "warmth|Warmth", "highpass|High Pass",
                            "shape|Vib Shape", "output|Output", "mix|Dry/Wet"}),
                       {},
                       0}},
                     preferredWidth);
    if (auto* pan = dynamic_cast<devices::AutoPan*>(&processor))
        return paged(processor,
                     {{"Auto Pan",
                       ids({"amount|Amount", "rate|Rate", "sync|Sync", "division|Division", "shape|Shape", "phase|Phase", "offset|Offset",
                            "invert|Invert", "square|Square"}),
                       [pan] { return std::unique_ptr<juce::Component>(std::make_unique<PanDisplay>(*pan)); },
                       90}},
                     preferredWidth);
    if (dynamic_cast<devices::AnalogSynth*>(&processor) != nullptr)
        return paged(processor, analogPages(), preferredWidth);
    if (auto* drift = dynamic_cast<devices::DriftSynth*>(&processor))
        return paged(processor, driftPages(*drift), preferredWidth);
    if (auto* meld = dynamic_cast<devices::Meld*>(&processor))
        return paged(processor, meldPages(*meld), preferredWidth);
    return nullptr;
}

} // namespace dojo::ui
