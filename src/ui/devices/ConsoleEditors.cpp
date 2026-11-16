#include "ui/devices/ConsoleEditors.h"

#include "devices/ConsoleTG12345.h"
#include "ui/theme/DojoTheme.h"

#include <algorithm>
#include <cmath>

namespace dojo::ui
{
namespace
{
using Console = devices::ConsoleTG12345;
using Page = PagedEditor::Page;
using Parameters = std::vector<std::pair<juce::String, juce::String>>;

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

//==================================================================================================
/** The console's display: the cassette's signal path (lit where it acts), its output meters (dBu),
    the compressor's gain reduction, the outputs' correlation and the cassette's lamps. */
class ConsoleFace final : public juce::Component, private juce::Timer
{
public:
    explicit ConsoleFace(Console& console)
        : console_(console)
    {
        startTimerHz(30);
    }

    void paint(juce::Graphics& g) override
    {
        using namespace theme;
        const auto area = getLocalBounds().toFloat();
        g.setColour(colour::meterBackground);
        g.fillRoundedRectangle(area, 3.0f);
        auto content = area.reduced(6.0f, 4.0f);

        // The badge: model, cassette, and the cassette's own regulated supply.
        auto header = content.removeFromTop(13.0f);
        const int cassette = console_.getCassette();
        g.setFont(label(9.0f));
        g.setColour(colour::gold);
        const juce::String model = "TG12345" + juce::String(cassette == Console::microphone ? (console_.isMarkTwo() ? " MK II" : " MK I") : "");
        g.drawText(model, header, juce::Justification::centredLeft, false);
        g.setColour(colour::textSecondary);
        g.drawText(Console::cassetteNames()[cassette].toUpperCase(), header.withTrimmedLeft(78.0f).withTrimmedRight(40.0f),
                   juce::Justification::centredLeft, true);
        g.setColour(colour::play);
        g.fillEllipse(juce::Rectangle<float>(5.0f, 5.0f).withCentre({header.getRight() - 36.0f, header.getCentreY()}));
        g.setColour(colour::textMuted);
        g.drawText("REG", header.removeFromRight(30.0f), juce::Justification::centredRight, false);

        // The signal path.
        content.removeFromTop(3.0f);
        const auto meters = content.removeFromBottom(46.0f);
        paintPath(g, content);
        paintMeters(g, meters, cassette);
    }

private:
    struct Chip
    {
        juce::String text;
        bool active = true;
    };

    [[nodiscard]] static juce::String choice(const juce::AudioParameterChoice& p) { return p.getCurrentChoiceName(); }
    [[nodiscard]] static juce::String text(juce::RangedAudioParameter& p) { return p.getText(p.getValue(), 12); }

    std::vector<Chip> chips() const
    {
        std::vector<Chip> list;
        const int cassette = console_.getCassette();
        const auto dynamics = [this]
        {
            const int mode = console_.mode_->getIndex();
            return Chip{mode == Console::comp ? "COMP 2:1" : mode == Console::limit ? "LIMIT 7:1" : "COMP OUT", mode != Console::out};
        };
        const double gain = Console::kGainSteps[static_cast<std::size_t>(console_.gain_->getIndex())] +
                            Console::kFineSteps[static_cast<std::size_t>(console_.fine_->getIndex())];
        const juce::String gainText = "GAIN " + juce::String(gain > 0.0 ? "+" : "") + juce::String(static_cast<int>(gain));
        const auto eq = [](const juce::String& name, const juce::AudioParameterChoice& p)
        { return Chip{name + " " + p.getCurrentChoiceName().upToFirstOccurrenceOf(" ", false, false), p.getIndex() != Console::kEqSteps / 2}; };
        const auto presence = [&] { return Chip{"PRES " + choice(*console_.presenceFrequency_) + " " + choice(*console_.presence_).upToFirstOccurrenceOf(" ", false, false),
                                                console_.presence_->getIndex() != Console::kEqSteps / 2}; };
        switch (cassette)
        {
        case Console::microphone:
            list = {{"MIC XFMR 1:2"}, {gainText}, {"AMP C1"}, dynamics(), eq("BASS", *console_.bass_), eq("TREBLE", *console_.treble_)};
            if (console_.isMarkTwo())
                list.push_back(presence());
            list.push_back({"AMP A"});
            list.push_back({"AMP E"});
            list.push_back({"ECHO " + text(*console_.echoSend_), console_.echoSend_->get() > -79.9f});
            list.push_back({"PAN"});
            if (!console_.isMarkTwo())
                list.push_back({"SPREAD " + juce::String(juce::roundToInt(console_.spread_->get())) + "%", std::abs(console_.spread_->get() - 100.0f) > 0.5f});
            list.push_back({"FADER " + text(*console_.fader_)});
            break;
        case Console::group:
            list = {{"MIX AMP A"}, {gainText}, dynamics(), presence(), {"FADER " + text(*console_.fader_)}};
            break;
        case Console::mainMix:
            list = {{"MIX AMP A"}, {gainText}, {"MAIN FADER " + text(*console_.fader_)}, {"ANNOUNCE", console_.announce_->get()}};
            break;
        case Console::echoAux:
            list = {{"SEND " + text(*console_.echoSend_)}, {"CHAMBER (LOOP)"}, {"RETURN " + text(*console_.echoReturn_)},
                    {"WIDTH " + juce::String(juce::roundToInt(console_.spread_->get())) + "%"}};
            break;
        default:
            if (console_.listen_->getIndex() == 0)
                list = {{"CONTROL ROOM " + text(*console_.controlRoom_)},
                        {"DIM", console_.dim_->get() || console_.talkback_->get()},
                        {"MONO", console_.mono_->get()},
                        {"CUT", console_.cut_->get()}};
            else
                list = {{"STUDIO " + text(*console_.studio_)}, {"TALKBACK " + text(*console_.talkbackLevel_), console_.talkback_->get()}};
            break;
        }
        list.push_back({"AMP B"});
        list.push_back({"OUT XFMR"});
        return list;
    }

    void paintPath(juce::Graphics& g, juce::Rectangle<float> area) const
    {
        using namespace theme;
        const auto font = mono(8.5f);
        g.setFont(font);
        float x = area.getX(), y = area.getY();
        constexpr float height = 14.0f, gap = 9.0f;
        for (const auto& chip : chips())
        {
            const float width = juce::GlyphArrangement::getStringWidth(font, chip.text) + 8.0f;
            if (x + width > area.getRight() && x > area.getX())
            {
                x = area.getX();
                y += height + 4.0f;
            }
            if (y + height > area.getBottom())
                break;
            const juce::Rectangle<float> box(x, y, width, height);
            g.setColour(chip.active ? colour::accent.withAlpha(0.22f) : colour::panelRaised);
            g.fillRoundedRectangle(box, 2.5f);
            g.setColour(chip.active ? colour::accent : colour::outlineStrong);
            g.drawRoundedRectangle(box, 2.5f, 1.0f);
            g.setColour(chip.active ? colour::textPrimary : colour::textMuted);
            g.drawText(chip.text, box, juce::Justification::centred, false);
            x += width;
            g.setColour(colour::textMuted);
            g.drawText(juce::String::fromUTF8("\xe2\x80\xba"), juce::Rectangle<float>(x, y, gap, height), juce::Justification::centred, false);
            x += gap;
        }
    }

    void paintMeters(juce::Graphics& g, juce::Rectangle<float> area, int cassette) const
    {
        using namespace theme;
        // Output, dBu: -20 .. +26, +4 (0 VU) and +24 (the drivers' maximum) marked.
        constexpr float low = -20.0f, high = 26.0f;
        const auto bar = [&](juce::Rectangle<float> row, const juce::String& name, float dbu)
        {
            g.setColour(colour::textMuted);
            g.setFont(label(8.0f));
            g.drawText(name, row.removeFromLeft(14.0f), juce::Justification::centredLeft, false);
            const auto track = row.reduced(0.0f, 2.0f);
            g.setColour(colour::panelRaised);
            g.fillRect(track);
            const float fraction = std::clamp((dbu - low) / (high - low), 0.0f, 1.0f);
            g.setColour(dbu >= 24.0f ? colour::meterClip : dbu >= 4.0f ? colour::meterMid : colour::meterLow);
            g.fillRect(track.withWidth(track.getWidth() * fraction));
            for (const float mark : {4.0f, 24.0f})
            {
                g.setColour(colour::textSecondary);
                const float mx = track.getX() + track.getWidth() * (mark - low) / (high - low);
                g.drawVerticalLine(juce::roundToInt(mx), track.getY() - 1.0f, track.getBottom() + 1.0f);
            }
        };
        auto rows = area;
        auto labels = rows.removeFromTop(9.0f);
        g.setColour(colour::textMuted);
        g.setFont(label(7.5f));
        const auto at = [&](float dbu) { return labels.getX() + 14.0f + (labels.getWidth() - 14.0f) * (dbu - low) / (high - low); };
        g.drawText("+4", juce::Rectangle<float>(at(4.0f) - 10.0f, labels.getY(), 20.0f, 9.0f), juce::Justification::centred, false);
        g.drawText("+24 dBu", juce::Rectangle<float>(at(24.0f) - 28.0f, labels.getY(), 34.0f, 9.0f), juce::Justification::centredRight, false);
        bar(rows.removeFromTop(9.0f), "L", peak_[0]);
        bar(rows.removeFromTop(9.0f), "R", peak_[1]);

        // Gain reduction (Microphone, Group), the outputs' correlation (Main), or the lamps.
        auto last = rows.reduced(0.0f, 1.0f);
        g.setFont(label(8.0f));
        if (cassette == Console::microphone || cassette == Console::group)
        {
            g.setColour(colour::textMuted);
            g.drawText("GR", last.removeFromLeft(14.0f), juce::Justification::centredLeft, false);
            const auto track = last.reduced(0.0f, 3.0f);
            g.setColour(colour::panelRaised);
            g.fillRect(track);
            const float fraction = std::clamp(gr_ / 20.0f, 0.0f, 1.0f);
            g.setColour(colour::gold);
            g.fillRect(track.withLeft(track.getRight() - track.getWidth() * fraction));
            g.setColour(colour::textSecondary);
            g.drawText(juce::String(gr_, 1) + " dB", track, juce::Justification::centredLeft, false);
        }
        else if (cassette == Console::mainMix)
        {
            g.setColour(colour::textMuted);
            g.drawText("CORR", last.removeFromLeft(26.0f), juce::Justification::centredLeft, false);
            const auto track = last.reduced(0.0f, 3.0f);
            g.setColour(colour::panelRaised);
            g.fillRect(track);
            const float centre = track.getCentreX();
            const float x = centre + track.getWidth() * 0.5f * std::clamp(correlation_, -1.0f, 1.0f);
            g.setColour(correlation_ < 0.0f ? colour::meterClip : colour::play);
            g.fillRect(juce::Rectangle<float>(std::min(centre, x), track.getY(), std::abs(x - centre), track.getHeight()));
            lamp(g, last.withTrimmedLeft(last.getWidth() - 70.0f), "ANNOUNCE", console_.announce_->get());
        }
        else if (cassette == Console::monitor)
        {
            const float w = last.getWidth() / 4.0f;
            lamp(g, last.removeFromLeft(w), "TALKBACK", console_.talkback_->get());
            lamp(g, last.removeFromLeft(w), "DIM", console_.dim_->get() || console_.talkback_->get());
            lamp(g, last.removeFromLeft(w), "MONO", console_.mono_->get());
            lamp(g, last, "CUT", console_.cut_->get());
        }
    }

    static void lamp(juce::Graphics& g, juce::Rectangle<float> area, const juce::String& name, bool on)
    {
        using namespace theme;
        const auto dot = juce::Rectangle<float>(6.0f, 6.0f).withCentre({area.getX() + 5.0f, area.getCentreY()});
        g.setColour(on ? colour::accentBright : colour::panelRaised);
        g.fillEllipse(dot);
        g.setColour(on ? colour::textPrimary : colour::textMuted);
        g.drawText(name, area.withTrimmedLeft(11.0f), juce::Justification::centredLeft, false);
    }

    void timerCallback() override
    {
        // Peak meters: rise at once, fall 20 dB/s.
        for (int ch = 0; ch < 2; ++ch)
        {
            const float now = console_.takeOutputPeakDbu(ch);
            auto& shown = peak_[static_cast<std::size_t>(ch)];
            shown = std::max(now, shown - 20.0f / 30.0f);
        }
        gr_ = console_.getGainReductionDb();
        correlation_ = console_.getCorrelation();
        repaint();
    }

    Console& console_;
    std::array<float, 2> peak_{-100.0f, -100.0f};
    float gr_ = 0.0f, correlation_ = 1.0f;
};

constexpr int kDisplayWidth = 250;
} // namespace

//==================================================================================================

ConsoleEditor::ConsoleEditor(Console& console)
    : console_(console)
{
    rebuild();
    startTimerHz(5);
}

std::vector<Page> ConsoleEditor::pagesFor(Console& console, int cassette, bool markTwo, bool stereo)
{
    const auto display = [&console] { return std::unique_ptr<juce::Component>(std::make_unique<ConsoleFace>(console)); };
    const Page quality{"Quality", ids({"osrealtime|Real-time", "osoffline|Offline"}), display, kDisplayWidth};
    const Page dynamics{"Comp / Limit", ids({"mode|Mode", "hold|Hold", "recovery|Recovery", "link|Stereo Link"}), display, kDisplayWidth};
    switch (cassette)
    {
    case Console::microphone:
    {
        Parameters eq = ids({"bass|Bass 50 Hz", "treble|Treble"});
        if (markTwo)
        {
            eq.emplace_back("presfreq", "Presence Freq");
            eq.emplace_back("presence", "Presence");
        }
        Parameters pan = ids({"pana|Pan A"});
        if (stereo)
            pan.emplace_back("panb", "Pan B");
        if (!markTwo)
            pan.emplace_back("spread", "Spreader");
        pan.emplace_back("fader", "Fader");
        return {{"Input", ids({"cassette|Cassette", "mark|Model", "input|Input", "gain|Input Gain", "fine|Fine Gain"}), display, kDisplayWidth},
                dynamics,
                {"EQ", eq, display, kDisplayWidth},
                {"Pan & Fader", pan, display, kDisplayWidth},
                {"Echo", ids({"echosend|Echo Send", "echopre|Pre-Fader", "echoreturn|Echo Return"}), display, kDisplayWidth},
                quality};
    }
    case Console::group:
        return {{"Group", ids({"cassette|Cassette", "gain|Group Gain", "fine|Fine Gain", "fader|Group Fader"}), display, kDisplayWidth},
                dynamics,
                {"Presence", ids({"presfreq|Presence Freq", "presence|Presence"}), display, kDisplayWidth},
                quality};
    case Console::mainMix:
        return {{"Main", ids({"cassette|Cassette", "gain|Mix Gain", "fine|Fine Gain", "fader|Main Fader"}), display, kDisplayWidth},
                {"Announce", ids({"announce|Track Announce", "annsource|Source", "annlevel|Level"}), display, kDisplayWidth},
                quality};
    case Console::echoAux:
        return {{"Echo / Aux", ids({"cassette|Cassette", "echosend|Send Master", "echoreturn|Return Master", "spread|Return Width"}), display,
                 kDisplayWidth},
                quality};
    default:
        return {{"Control Room", ids({"cassette|Cassette", "listen|Listen", "crlevel|CR Level", "dim|Dim", "mono|Mono", "cut|Cut"}), display,
                 kDisplayWidth},
                {"Studio & Talkback", ids({"studiolevel|Studio Level", "talkback|Talkback", "tblevel|Talkback Level"}), display, kDisplayWidth},
                quality};
    }
}

int ConsoleEditor::preferredWidth(Console& console)
{
    int width = 0;
    for (int cassette = 0; cassette < 5; ++cassette)
        for (const bool markTwo : {false, true})
            for (const bool stereo : {false, true})
                width = std::max(width, PagedEditor::preferredWidth(pagesFor(console, cassette, markTwo, stereo)));
    return width;
}

void ConsoleEditor::rebuild()
{
    cassette_ = console_.getCassette();
    mark_ = console_.mark_->getIndex();
    input_ = console_.input_->getIndex();
    const std::size_t page = pages_ != nullptr ? pages_->getPage() : 0;
    pages_ = std::make_unique<PagedEditor>(console_, pagesFor(console_, cassette_, mark_ == 1, input_ == 0));
    pages_->showPage(std::min(page, pages_->getNumPages() - 1));
    addAndMakeVisible(*pages_);
    resized();
}

void ConsoleEditor::timerCallback()
{
    if (console_.getCassette() != cassette_ || console_.mark_->getIndex() != mark_ || console_.input_->getIndex() != input_)
        rebuild();
}

void ConsoleEditor::resized()
{
    if (pages_ != nullptr)
        pages_->setBounds(getLocalBounds());
}

std::unique_ptr<juce::Component> createConsoleDeviceEditor(juce::AudioProcessor& processor, int& preferredWidth)
{
    auto* console = dynamic_cast<Console*>(&processor);
    if (console == nullptr)
        return nullptr;
    preferredWidth = ConsoleEditor::preferredWidth(*console);
    return std::make_unique<ConsoleEditor>(*console);
}

} // namespace dojo::ui
