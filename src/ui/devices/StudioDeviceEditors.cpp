#include "ui/devices/StudioDeviceEditors.h"

#include "devices/PlateReverb.h"
#include "devices/StudioDynamics.h"
#include "ui/devices/PagedEditor.h"
#include "ui/theme/DojoTheme.h"

#include <array>
#include <cmath>
#include <functional>

namespace dojo::ui
{
namespace
{
using Page = PagedEditor::Page;
using Parameters = std::vector<std::pair<juce::String, juce::String>>;

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

/** "id|Caption" pairs. */
Parameters ids(std::initializer_list<const char*> idsAndCaptions)
{
    Parameters result;
    for (const auto* entry : idsAndCaptions)
    {
        const juce::String text(entry);
        result.emplace_back(text.upToFirstOccurrenceOf("|", false, false), text.fromFirstOccurrenceOf("|", false, false));
    }
    return result;
}

template <typename Display, typename... Args>
std::function<std::unique_ptr<juce::Component>()> make(Args&&... args)
{
    return [=] { return std::unique_ptr<juce::Component>(std::make_unique<Display>(args...)); };
}

//==================================================================================================
/**
    A scrolling history (about four seconds) of a detector's level against the threshold, and of
    the gain change it caused (reduction hanging from the top, a boost in green).
*/
class HistoryDisplay final : public juce::Component, private juce::Timer
{
public:
    struct Source
    {
        juce::String title;
        std::function<float()> takeLevelDb; ///< the detector's level since the last call
        std::function<double()> thresholdDb;
        std::function<float()> takeGainDb; ///< the gain change since the last call (< 0 down, > 0 up)
        double levelFloorDb = -80.0;
        double gainRangeDb = 24.0;
    };

    explicit HistoryDisplay(Source source)
        : source_(std::move(source))
    {
        levels_.fill(-200.0f);
        gains_.fill(0.0f);
        startTimerHz(30);
    }

    void paint(juce::Graphics& g) override
    {
        using namespace theme;
        const auto area = getLocalBounds().toFloat();
        paintPanel(g, area, source_.title);
        const auto plot = area.reduced(6.0f, 4.0f).withTrimmedTop(12.0f);
        const auto yOfLevel = [&](double db)
        { return plot.getBottom() - static_cast<float>(std::clamp((db - source_.levelFloorDb) / -source_.levelFloorDb, 0.0, 1.0)) * plot.getHeight(); };
        const float step = plot.getWidth() / static_cast<float>(kPoints - 1);

        // The gain change: bars from the top.
        for (std::size_t k = 0; k < kPoints; ++k)
        {
            const float gain = gains_[(write_ + k) % kPoints];
            if (std::abs(gain) < 0.05f)
                continue;
            const float h = plot.getHeight() * static_cast<float>(std::min(1.0, std::abs(gain) / source_.gainRangeDb));
            g.setColour(gain < 0.0f ? colour::accent.withAlpha(0.55f) : colour::meterLow.withAlpha(0.55f));
            g.fillRect(plot.getX() + step * static_cast<float>(k) - step * 0.5f, plot.getY(), step + 0.5f, h);
        }

        // The threshold, and the level.
        if (source_.thresholdDb)
        {
            g.setColour(colour::gold.withAlpha(0.85f));
            const float y = yOfLevel(source_.thresholdDb());
            const float dashes[] = {4.0f, 3.0f};
            g.drawDashedLine({plot.getX(), y, plot.getRight(), y}, dashes, 2, 1.0f);
        }
        juce::Path path;
        bool started = false;
        for (std::size_t k = 0; k < kPoints; ++k)
        {
            const float level = levels_[(write_ + k) % kPoints];
            const float x = plot.getX() + step * static_cast<float>(k);
            const float y = yOfLevel(level);
            if (!started)
                path.startNewSubPath(x, y), started = true;
            else
                path.lineTo(x, y);
        }
        g.setColour(colour::textPrimary.withAlpha(0.85f));
        g.strokePath(path, juce::PathStrokeType(1.2f));

        const float now = gains_[(write_ + kPoints - 1) % kPoints];
        g.setColour(colour::textSecondary);
        g.setFont(mono(9.5f));
        g.drawText((now > 0.05f ? "+" : "") + juce::String(now, 1) + " dB", area.reduced(6.0f, 3.0f), juce::Justification::topRight, false);
    }

private:
    static constexpr std::size_t kPoints = 120;

    void timerCallback() override
    {
        levels_[write_] = source_.takeLevelDb ? source_.takeLevelDb() : -200.0f;
        gains_[write_] = source_.takeGainDb ? source_.takeGainDb() : 0.0f;
        write_ = (write_ + 1) % kPoints;
        repaint();
    }

    Source source_;
    std::array<float, kPoints> levels_{}, gains_{};
    std::size_t write_ = 0;
};

//==================================================================================================
/** De-Esser: the band the detector listens to (over 1 .. 20 kHz) above the reduction history. */
class DeEsserDisplay final : public juce::Component, private juce::Timer
{
public:
    explicit DeEsserDisplay(devices::DeEsser& deesser)
        : deesser_(deesser)
        , history_(HistoryDisplay::Source{"GR", [&deesser] { return deesser.takeDetectorDb(); },
                                          [&deesser] { return static_cast<double>(deesser.threshold_->get()); },
                                          [&deesser] { return -deesser.takeReductionDb(); }, -60.0, 24.0})
    {
        addAndMakeVisible(history_);
        startTimerHz(10);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        band_ = area.removeFromTop(juce::roundToInt(static_cast<float>(area.getHeight()) * 0.42f)).toFloat();
        area.removeFromTop(3);
        history_.setBounds(area);
    }

    void paint(juce::Graphics& g) override
    {
        using namespace theme;
        paintPanel(g, band_, deesser_.band_->getIndex() == 1 ? "BAND" : "ABOVE");
        const auto plot = band_.reduced(6.0f, 4.0f).withTrimmedTop(10.0f);
        constexpr double lo = 1000.0, hi = 20000.0;
        const auto yOf = [&](double db) { return plot.getY() - static_cast<float>(std::clamp(db, -30.0, 0.0) / 30.0) * plot.getHeight(); };
        juce::Path curve;
        const int points = std::max(2, static_cast<int>(plot.getWidth()));
        for (int x = 0; x < points; ++x)
        {
            const double f = lo * std::pow(hi / lo, static_cast<double>(x) / (points - 1));
            const float y = yOf(deesser_.detectorResponseDb(f));
            if (x == 0)
                curve.startNewSubPath(plot.getX(), y);
            else
                curve.lineTo(plot.getX() + static_cast<float>(x), y);
        }
        juce::Path fill(curve);
        fill.lineTo(plot.getRight(), plot.getBottom());
        fill.lineTo(plot.getX(), plot.getBottom());
        fill.closeSubPath();
        g.setColour(colour::accent.withAlpha(0.25f));
        g.fillPath(fill);
        g.setColour(colour::accentBright);
        g.strokePath(curve, juce::PathStrokeType(1.5f));
        g.setColour(colour::textSecondary);
        g.setFont(mono(9.0f));
        const auto khz = [](double f) { return juce::String(f / 1000.0, f < 10000.0 ? 1 : 0) + "k"; };
        juce::String text = khz(deesser_.frequency_->get());
        if (deesser_.band_->getIndex() == 1)
            text << "-" << khz(deesser_.high_->get());
        if (deesser_.detection_->getIndex() == 1)
            text << " REL";
        g.drawText(text, band_.reduced(6.0f, 2.0f), juce::Justification::topRight, false);
    }

private:
    void timerCallback() override
    {
        const double signature = deesser_.frequency_->get() + deesser_.high_->get() * 3.1 + deesser_.band_->getIndex() * 7.0 +
                                 deesser_.detection_->getIndex() * 13.0;
        if (std::abs(signature - signature_) > 1.0e-9)
        {
            signature_ = signature;
            repaint(band_.toNearestInt());
        }
    }

    devices::DeEsser& deesser_;
    HistoryDisplay history_;
    juce::Rectangle<float> band_;
    double signature_ = -1.0;
};

//==================================================================================================
/** A transfer curve: output against input, with the current input level marked (optional). */
class TransferDisplay final : public juce::Component, private juce::Timer
{
public:
    /** `curve` maps an input to an output, both from `minimum` to `maximum` (dB, or linear). */
    TransferDisplay(juce::String title, std::function<double(double)> curve, double minimum, double maximum,
                    std::function<float()> takeLevel = {}, std::function<float()> takeActivity = {})
        : title_(std::move(title))
        , curve_(std::move(curve))
        , minimum_(minimum)
        , maximum_(maximum)
        , takeLevel_(std::move(takeLevel))
        , takeActivity_(std::move(takeActivity))
    {
        startTimerHz(30);
    }

    void paint(juce::Graphics& g) override
    {
        using namespace theme;
        const auto area = getLocalBounds().toFloat();
        paintPanel(g, area, title_);
        auto plot = area.reduced(8.0f, 16.0f);
        plot = plot.withSizeKeepingCentre(std::min(plot.getWidth(), plot.getHeight()), std::min(plot.getWidth(), plot.getHeight()));
        const auto xOf = [&](double v) { return plot.getX() + static_cast<float>((v - minimum_) / (maximum_ - minimum_)) * plot.getWidth(); };
        const auto yOf = [&](double v) { return plot.getBottom() - static_cast<float>((std::clamp(v, minimum_, maximum_) - minimum_) / (maximum_ - minimum_)) * plot.getHeight(); };
        g.setColour(colour::outline.withAlpha(0.7f));
        g.drawLine(xOf(minimum_), yOf(minimum_), xOf(maximum_), yOf(maximum_), 1.0f); // (unity)
        g.drawRect(plot, 1.0f);
        juce::Path path;
        const int points = std::max(2, static_cast<int>(plot.getWidth()));
        for (int i = 0; i < points; ++i)
        {
            const double v = minimum_ + (maximum_ - minimum_) * i / (points - 1);
            const float x = xOf(v), y = yOf(curve_(v));
            if (i == 0)
                path.startNewSubPath(x, y);
            else
                path.lineTo(x, y);
        }
        g.setColour(colour::accentBright);
        g.strokePath(path, juce::PathStrokeType(1.8f));
        if (takeLevel_ && level_ > minimum_)
        {
            g.setColour(colour::gold);
            g.fillEllipse(xOf(level_) - 3.5f, yOf(curve_(level_)) - 3.5f, 7.0f, 7.0f);
        }
        if (takeActivity_)
        {
            const auto lamp = area.withTop(area.getBottom() - 14.0f).reduced(8.0f, 2.0f);
            g.setColour(colour::meterClip.withAlpha(0.15f + 0.85f * std::clamp(activity_ / 6.0f, 0.0f, 1.0f)));
            g.fillRoundedRectangle(lamp, 2.0f);
            g.setColour(colour::textPrimary);
            g.setFont(label(8.5f));
            g.drawText("CLIP  -" + juce::String(activity_, 1) + " dB", lamp, juce::Justification::centred, false);
        }
    }

private:
    void timerCallback() override
    {
        if (takeLevel_)
            level_ = std::max(static_cast<double>(takeLevel_()), level_ - 1.5);
        if (takeActivity_)
            activity_ = std::max(takeActivity_(), activity_ * 0.85f);
        repaint();
    }

    juce::String title_;
    std::function<double(double)> curve_;
    double minimum_, maximum_;
    std::function<float()> takeLevel_, takeActivity_;
    double level_ = -200.0;
    float activity_ = 0.0f;
};

//==================================================================================================
/** Lines of text that update (e.g. the oversampling's latency now and offline). */
class InfoDisplay final : public juce::Component, private juce::Timer
{
public:
    InfoDisplay(juce::String title, std::function<juce::StringArray()> lines)
        : title_(std::move(title))
        , lines_(std::move(lines))
    {
        startTimerHz(4);
    }

    void paint(juce::Graphics& g) override
    {
        using namespace theme;
        const auto area = getLocalBounds().toFloat();
        paintPanel(g, area, title_);
        auto text = area.reduced(8.0f, 4.0f).withTrimmedTop(14.0f);
        g.setFont(mono(9.5f));
        for (const auto& line : lines_())
        {
            const bool heading = line.startsWith("#");
            g.setColour(heading ? colour::textMuted : colour::textPrimary);
            g.drawText(heading ? line.substring(1) : line, text.removeFromTop(14.0f), juce::Justification::centredLeft, true);
        }
    }

private:
    void timerCallback() override
    {
        const auto lines = lines_().joinIntoString("\n");
        if (lines != last_)
        {
            last_ = lines;
            repaint();
        }
    }

    juce::String title_;
    std::function<juce::StringArray()> lines_;
    juce::String last_;
};

/** What the oversampling costs in latency, now and in offline renders. */
std::function<juce::StringArray()> oversamplingLines(juce::AudioProcessor& processor, std::function<int(bool)> latencyFor,
                                                     juce::AudioParameterChoice& realtime, juce::AudioParameterChoice& offline,
                                                     juce::AudioParameterChoice& phase)
{
    return [&processor, latencyFor, &realtime, &offline, &phase]
    {
        const double rate = processor.getSampleRate() > 0.0 ? processor.getSampleRate() : 48000.0;
        const auto describe = [&](bool isOffline, const juce::AudioParameterChoice& choice)
        {
            const int samples = latencyFor(isOffline);
            return choice.getCurrentChoiceName() + "  " + juce::String(samples) + " smp (" + juce::String(samples * 1000.0 / rate, 2) + " ms)";
        };
        return juce::StringArray{"#REAL-TIME", describe(false, realtime), "#OFFLINE RENDER", describe(true, offline), "#FILTERS",
                                 phase.getCurrentChoiceName()};
    };
}

//==================================================================================================
/** Plate Reverb: the decay envelope (pre-delay, then the fall to -60 dB) of the lows, mids and highs. */
class DecayDisplay final : public juce::Component, private juce::Timer
{
public:
    explicit DecayDisplay(devices::PlateReverb& plate)
        : plate_(plate)
    {
        startTimerHz(10);
    }

    void paint(juce::Graphics& g) override
    {
        using namespace theme;
        const auto area = getLocalBounds().toFloat();
        paintPanel(g, area, "DECAY");
        const auto plot = area.reduced(8.0f, 18.0f);
        const bool frozen = plate_.freeze_->get();
        const double decay = plate_.decay_->get();
        const double lows = plate_.getLowDecaySeconds();
        const double preDelay = plate_.getPreDelayMs() / 1000.0;
        const double span = preDelay + std::max(decay, lows) * 1.1;
        // Damping shortens the highs (a rough guide: at 1 kHz damping a third of the decay).
        const double damping = plate_.damping_->get() * devices::PlateReverb::characterOf(plate_.plate_->getIndex()).damping;
        const double highs = decay * std::clamp(std::pow(damping / 20000.0, 0.35), 0.3, 1.0);
        const auto xOf = [&](double t) { return plot.getX() + static_cast<float>(t / span) * plot.getWidth(); };
        const auto yOf = [&](double db) { return plot.getY() - static_cast<float>(std::clamp(db, -60.0, 0.0) / 60.0) * plot.getHeight(); };
        const auto envelope = [&](double rt, juce::Colour colour)
        {
            juce::Path path;
            path.startNewSubPath(xOf(0.0), yOf(-60.0));
            path.lineTo(xOf(preDelay), yOf(-60.0));
            path.lineTo(xOf(preDelay), yOf(0.0));
            if (frozen)
                path.lineTo(xOf(span), yOf(0.0));
            else
                path.lineTo(xOf(std::min(span, preDelay + rt)), yOf(-60.0 * std::min(1.0, (span - preDelay) / rt)));
            g.setColour(colour);
            g.strokePath(path, juce::PathStrokeType(1.6f));
        };
        g.setColour(colour::outline.withAlpha(0.6f));
        g.drawHorizontalLine(juce::roundToInt(yOf(-30.0)), plot.getX(), plot.getRight());
        envelope(lows, colour::meterLow.withAlpha(0.8f));
        envelope(highs, colour::gold.withAlpha(0.8f));
        envelope(decay, colour::accentBright);
        g.setFont(mono(9.5f));
        g.setColour(colour::textSecondary);
        g.drawText(frozen ? juce::String("FROZEN") : juce::String(decay, 2) + " s", area.reduced(6.0f, 3.0f), juce::Justification::topRight, false);
        g.setFont(label(8.5f));
        auto legend = area.reduced(6.0f, 3.0f).removeFromBottom(12.0f);
        g.setColour(colour::meterLow);
        g.drawText("LOWS", legend.removeFromLeft(legend.getWidth() / 3.0f), juce::Justification::centredLeft, false);
        g.setColour(colour::accentBright);
        g.drawText("MIDS", legend.removeFromLeft(legend.getWidth() / 2.0f), juce::Justification::centred, false);
        g.setColour(colour::gold);
        g.drawText("HIGHS", legend, juce::Justification::centredRight, false);
    }

private:
    void timerCallback() override
    {
        const double signature = plate_.decay_->get() * 7.0 + plate_.getPreDelayMs() + plate_.damping_->get() * 0.001 + plate_.getLowDecaySeconds() * 3.0 +
                                 plate_.plate_->getIndex() * 11.0 + (plate_.freeze_->get() ? 17.0 : 0.0);
        if (std::abs(signature - signature_) > 1.0e-9)
        {
            signature_ = signature;
            repaint();
        }
    }

    devices::PlateReverb& plate_;
    double signature_ = -1.0;
};

//==================================================================================================
// Pages

std::vector<Page> gluePages(devices::TheGlue& glue)
{
    const auto vu = [&glue] { return std::unique_ptr<juce::Component>(std::make_unique<GlueVuMeter>(glue)); };
    return {{"The Glue",
             ids({"threshold|Threshold", "ratio|Ratio", "attack|Attack", "release|Release", "makeup|Makeup", "mix|Mix", "range|Range",
                  "schp|SC High Pass"}),
             vu,
             180},
            {"Sidechain & Clip", ids({"sidechain|External SC", "schp|SC High Pass", "range|Range", "peakclip|Peak Clip"}), vu, 180},
            {"Oversampling",
             ids({"osrealtime|Real-time", "osoffline|Offline", "osphase|Phase"}),
             make<InfoDisplay>(juce::String("LATENCY"), oversamplingLines(glue, [&glue](bool offline) { return glue.latencyFor(offline); },
                                                                         *glue.osRealtime_, *glue.osOffline_, *glue.osPhase_)),
             180}};
}

std::vector<Page> deEsserPages(devices::DeEsser& deesser)
{
    const auto display = [&deesser] { return std::unique_ptr<juce::Component>(std::make_unique<DeEsserDisplay>(deesser)); };
    return {{"De-Esser",
             ids({"freq|Frequency", "high|High", "band|Band", "detect|Detection", "threshold|Threshold", "range|Range", "release|Release",
                  "mode|Mode"}),
             display,
             190},
            {"Options", ids({"lookahead|Lookahead", "stereo|Stereo", "listen|Listen"}), display, 190}};
}

std::vector<Page> expanderPages(devices::Expander& expander)
{
    const auto transfer = [&expander]
    {
        return std::unique_ptr<juce::Component>(std::make_unique<TransferDisplay>(
            "IN > OUT", [&expander](double db) { return db + expander.gainChangeFor(db); }, -80.0, 0.0,
            [&expander] { return expander.takeDetectorDb(); }));
    };
    const auto history = [&expander]
    {
        return std::unique_ptr<juce::Component>(std::make_unique<HistoryDisplay>(HistoryDisplay::Source{
            "GAIN", [&expander] { return expander.takeDetectorDb(); }, [&expander] { return static_cast<double>(expander.threshold_->get()); },
            [&expander] { return expander.takeGainChangeDb(); }, -80.0, 40.0}));
    };
    return {{"Expander",
             ids({"mode|Mode", "threshold|Threshold", "ratio|Ratio", "range|Range", "knee|Knee", "hysteresis|Hysteresis", "attack|Attack",
                  "hold|Hold", "release|Release", "output|Output"}),
             transfer,
             130},
            {"Detector",
             ids({"detect|Detector", "lookahead|Lookahead", "sidechain|External SC", "keylisten|Key Listen", "keylow|Key HP", "keyhigh|Key LP",
                  "mix|Dry/Wet"}),
             history,
             190}};
}

std::vector<Page> softClipperPages(devices::SoftClipper& clipper)
{
    const auto curve = [&clipper]
    {
        return std::unique_ptr<juce::Component>(std::make_unique<TransferDisplay>(
            "CURVE", [&clipper](double x) { return clipper.shape(x); }, -1.5, 1.5, std::function<float()>{}, [&clipper] { return clipper.takeClipDb(); }));
    };
    return {{"Soft Clipper",
             ids({"drive|Input", "ceiling|Ceiling", "shape|Shape", "softness|Softness", "asymmetry|Asymmetry", "link|Stereo Link", "autogain|Auto Gain",
                  "output|Output", "mix|Dry/Wet"}),
             curve,
             130},
            {"Listen & Quality",
             ids({"listen|Listen", "osrealtime|Real-time", "osoffline|Offline", "osphase|Phase"}),
             make<InfoDisplay>(juce::String("LATENCY"), oversamplingLines(clipper, [&clipper](bool offline) { return clipper.latencyFor(offline); },
                                                                         *clipper.osRealtime_, *clipper.osOffline_, *clipper.osPhase_)),
             180}};
}

std::vector<Page> plateReverbPages(devices::PlateReverb& plate)
{
    const auto display = [&plate] { return std::unique_ptr<juce::Component>(std::make_unique<DecayDisplay>(plate)); };
    return {{"Plate",
             ids({"plate|Plate", "predelay|Pre-delay", "sync|Sync", "division|Division", "decay|Decay", "size|Size", "damping|Damping", "mix|Dry/Wet"}),
             display,
             150},
            {"Tone", ids({"lowdecay|Low Decay", "lowfreq|Low Freq", "bandwidth|Bandwidth", "lowcut|Low Cut", "highcut|High Cut", "width|Width"}), display, 150},
            {"Motion", ids({"diffusion|Diffusion", "modrate|Mod Rate", "modulation|Mod Depth", "drive|Drive", "ducking|Ducking", "freeze|Freeze"}), display, 150}};
}

std::unique_ptr<juce::Component> paged(juce::AudioProcessor& processor, std::vector<Page> pages, int& preferredWidth)
{
    preferredWidth = PagedEditor::preferredWidth(pages);
    return std::make_unique<PagedEditor>(processor, std::move(pages));
}
} // namespace

//==================================================================================================
// The Glue's VU meter

GlueVuMeter::GlueVuMeter(devices::TheGlue& glue)
    : glue_(glue)
{
    setRepaintsOnMouseActivity(true);
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
    setTooltip("Click to show or hide the Peak needle. Hover to read the RMS and Peak compression.");
    startTimerHz(30);
}

bool GlueVuMeter::isPeakVisible() const noexcept
{
    return glue_.isPeakNeedleShown();
}

void GlueVuMeter::setPeakVisible(bool visible)
{
    glue_.setPeakNeedleShown(visible);
    repaint();
}

void GlueVuMeter::mouseDown(const juce::MouseEvent&)
{
    setPeakVisible(!isPeakVisible());
}

juce::String GlueVuMeter::getReadout() const
{
    return "RMS " + juce::String(rms_, 1) + " dB   PEAK " + juce::String(peak_, 1) + " dB";
}

void GlueVuMeter::update(double elapsedSeconds)
{
    const double before = rms_ + peak_ * 3.7;
    rms_ = std::max(0.0, static_cast<double>(glue_.getRmsReductionDb()));
    // The peak needle: jumps up, holds, then falls.
    const double peak = glue_.takePeakReductionDb();
    if (peak >= peak_)
    {
        peak_ = peak;
        peakHeld_ = kPeakHoldSeconds;
    }
    else if (peakHeld_ > 0.0)
    {
        peakHeld_ -= elapsedSeconds;
    }
    else
    {
        peak_ = std::max(peak, peak_ - kPeakFallDbPerSecond * elapsedSeconds);
    }
    if (std::abs(rms_ + peak_ * 3.7 - before) > 1.0e-4)
        repaint();
}

void GlueVuMeter::paint(juce::Graphics& g)
{
    using namespace theme;
    const auto area = getLocalBounds().toFloat();
    paintPanel(g, area, "COMPRESSION");

    auto content = area.reduced(6.0f, 4.0f).withTrimmedTop(12.0f);
    // A meter face wider than tall, as on the console; the numbers go under it.
    const float faceHeight = std::min(content.getHeight() - 18.0f, content.getWidth() * 0.72f);
    const auto face = content.removeFromTop(faceHeight).withTrimmedTop(2.0f);
    const auto readout = content.removeFromTop(18.0f);

    // A classic meter face (dark whatever the theme, so the white needle reads).
    g.setColour(juce::Colour(0xff17150f));
    g.fillRoundedRectangle(face, 4.0f);
    g.setColour(juce::Colour(0xff3a3426));
    g.drawRoundedRectangle(face, 4.0f, 1.0f);

    constexpr float kSweep = 0.85f; // radians either side of vertical
    const juce::Point<float> pivot(face.getCentreX(), face.getBottom() + face.getHeight() * 0.12f);
    const float radius = std::min((face.getWidth() * 0.5f - 8.0f) / (0.9f * std::sin(kSweep)), (face.getHeight() * 1.12f - 18.0f) / 0.9f);
    const auto angleOf = [](double db) { return kSweep - 2.0f * kSweep * static_cast<float>(std::clamp(db / kScaleDb, 0.0, 1.0)); };
    const auto pointAt = [&](float angle, float r) { return juce::Point<float>(pivot.x + r * std::sin(angle), pivot.y - r * std::cos(angle)); };

    g.saveState();
    g.reduceClipRegion(face.toNearestInt());
    const auto scaleColour = juce::Colour(0xffe9dfc5);
    g.setFont(mono(8.5f));
    for (int db = 0; db <= static_cast<int>(kScaleDb); db += 2)
    {
        const float angle = angleOf(db);
        const bool major = db % 4 == 0;
        g.setColour(db >= 12 ? juce::Colour(0xffd9603b) : scaleColour);
        g.drawLine({pointAt(angle, radius * (major ? 0.80f : 0.84f)), pointAt(angle, radius * 0.90f)}, major ? 1.4f : 0.9f);
        if (major)
        {
            const auto at = pointAt(angle, radius * 0.70f);
            g.drawText(juce::String(db), juce::Rectangle<float>(26.0f, 11.0f).withCentre(at), juce::Justification::centred, false);
        }
    }
    // The arc.
    juce::Path arc;
    arc.addCentredArc(pivot.x, pivot.y, radius * 0.90f, radius * 0.90f, 0.0f, -kSweep, kSweep, true);
    g.setColour(scaleColour.withAlpha(0.7f));
    g.strokePath(arc, juce::PathStrokeType(1.0f));
    g.setFont(label(8.0f));
    g.drawText("dB GR", juce::Rectangle<float>(60.0f, 12.0f).withCentre(pointAt(0.0f, radius * 0.45f)), juce::Justification::centred, false);

    // The needles: Peak (yellow, when shown) behind RMS (white).
    const auto needle = [&](double db, juce::Colour colour, float thickness)
    {
        const float angle = angleOf(db);
        g.setColour(colour);
        g.drawLine({pointAt(angle, radius * 0.12f), pointAt(angle, radius * 0.94f)}, thickness);
    };
    const bool peakVisible = isPeakVisible();
    if (peakVisible)
        needle(peak_, juce::Colour(0xffffd23f), 1.4f);
    needle(rms_, juce::Colours::white, 1.8f);
    g.restoreState();

    if (peakVisible)
    {
        g.setColour(juce::Colour(0xffffd23f));
        g.setFont(label(8.0f));
        g.drawText("PEAK", face.reduced(5.0f, 3.0f), juce::Justification::topRight, false);
    }

    // Hover: the numbers below the meter.
    if (isMouseOver(true))
    {
        g.setColour(colour::textPrimary);
        g.setFont(mono(9.5f));
        g.drawText(getReadout(), readout, juce::Justification::centred, false);
    }
}

//==================================================================================================

std::unique_ptr<juce::Component> createStudioDeviceEditor(juce::AudioProcessor& processor, int& preferredWidth)
{
    if (auto* glue = dynamic_cast<devices::TheGlue*>(&processor))
        return paged(processor, gluePages(*glue), preferredWidth);
    if (auto* deesser = dynamic_cast<devices::DeEsser*>(&processor))
        return paged(processor, deEsserPages(*deesser), preferredWidth);
    if (auto* expander = dynamic_cast<devices::Expander*>(&processor))
        return paged(processor, expanderPages(*expander), preferredWidth);
    if (auto* clipper = dynamic_cast<devices::SoftClipper*>(&processor))
        return paged(processor, softClipperPages(*clipper), preferredWidth);
    if (auto* plate = dynamic_cast<devices::PlateReverb*>(&processor))
        return paged(processor, plateReverbPages(*plate), preferredWidth);
    return nullptr;
}

} // namespace dojo::ui
