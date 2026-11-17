#include "ui/devices/ConsoleEditors.h"

#include "devices/ConsoleTG12345.h"
#include "ui/devices/HardwarePainting.h"
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

// The desk's colours.
const juce::Colour kGrey{0xffa3a7a5};        // the metalwork
const juce::Colour kMeterPanel{0xff8a8f8d};  // the meter panel
const juce::Colour kInk{0xff1b1c1c};
const juce::Colour kWhiteFace{0xfff8f6ef};
const juce::Colour kCream{0xfff0e9d6};
const juce::Colour kRed{0xffd23c2e};
constexpr float kFaderWidth = 42.0f;

/** A small white-faced meter in dBu: -20 .. +26, the +4 line-up and the drivers' +24 marked. */
hardware::MeterFace dbuFace(const juce::String& caption)
{
    hardware::MeterFace face;
    face.face = kWhiteFace;
    face.backlit = false;
    face.legend = "dBu";
    face.caption = caption;
    face.redFrom = (20.0 + 20.0) / 46.0;
    for (const auto& [dbu, text] : std::initializer_list<std::pair<double, const char*>>{
             {-20.0, "-20"}, {-10.0, "-10"}, {0.0, "0"}, {4.0, ""}, {10.0, "10"}, {20.0, "20"}, {24.0, ""}})
        face.marks.push_back({(dbu + 20.0) / 46.0, text, dbu >= 20.0});
    return face;
}

/** The compressor's gain reduction, resting on the right (0 dB) and swinging left. */
hardware::MeterFace reductionFace()
{
    hardware::MeterFace face;
    face.face = kWhiteFace;
    face.backlit = false;
    face.legend = "GR dB";
    for (const auto& [db, text] : std::initializer_list<std::pair<double, const char*>>{
             {20.0, "20"}, {15.0, ""}, {10.0, "10"}, {5.0, "5"}, {2.0, ""}, {0.0, "0"}})
        face.marks.push_back({1.0 - db / 20.0, text, false});
    return face;
}

/** The main outputs' correlation, -1 (out of phase, red) .. +1. */
hardware::MeterFace correlationFace()
{
    hardware::MeterFace face;
    face.face = kWhiteFace;
    face.backlit = false;
    face.legend = "CORR";
    for (const auto& [value, text] : std::initializer_list<std::pair<double, const char*>>{
             {-1.0, "-1"}, {-0.5, ""}, {0.0, "0"}, {0.5, ""}, {1.0, "+1"}})
        face.marks.push_back({(value + 1.0) * 0.5, text, value < 0.0});
    return face;
}

//==================================================================================================
/** The cassette's face: its meters, the nameplate, the signal path (lit where it acts), its lamps
    and its fader. */
class ConsoleFace final : public juce::Component, private juce::Timer
{
public:
    explicit ConsoleFace(Console& console)
        : console_(console)
    {
        setInterceptsMouseClicks(false, true);
        const int cassette = console_.getCassette();
        if (auto* level = dynamic_cast<juce::RangedAudioParameter*>(findParameter(console_, ConsoleEditor::faderFor(cassette))))
        {
            hardware::FaderStyle style;
            style.cap = cassette == Console::mainMix ? kRed : juce::Colour(0xfff1eee6);
            style.capLine = cassette == Console::mainMix ? juce::Colour(0xfff4f1ea) : juce::Colour(0xff121212);
            style.slot = juce::Colour(0xff050505);
            style.scale = kInk;
            fader_ = std::make_unique<hardware::ParameterFader>(*level, style);
            addAndMakeVisible(*fader_);
        }
        startTimerHz(30);
    }

    [[nodiscard]] hardware::ParameterFader* getFader() const noexcept { return fader_.get(); }

    void resized() override
    {
        if (fader_ == nullptr)
            return;
        auto content = getLocalBounds().toFloat().reduced(6.0f, 5.0f);
        const auto strip = content.removeFromRight(kFaderWidth - 6.0f); // (as paint lays it out)
        fader_->setBounds(strip.withTrimmedTop(12.0f).reduced(0.0f, 1.0f).toNearestInt());
    }

    void paint(juce::Graphics& g) override
    {
        using namespace theme;
        const auto area = getLocalBounds().toFloat();
        hardware::paintMetal(g, area, kMeterPanel, 3.0f, true, 77);
        g.setColour(juce::Colour(0xff595e5c));
        g.drawRoundedRectangle(area.reduced(0.5f), 3.0f, 1.0f);
        auto content = area.reduced(6.0f, 5.0f);
        const int cassette = console_.getCassette();

        // The fader's slot plate and its legend.
        if (fader_ != nullptr)
        {
            auto strip = content.removeFromRight(kFaderWidth - 6.0f);
            content.removeFromRight(4.0f);
            g.setColour(juce::Colour(0xff7b807e));
            g.fillRoundedRectangle(strip, 2.5f);
            hardware::paintEngraved(g, strip.removeFromTop(11.0f), faderLegend(cassette), kInk, label(7.0f));
        }

        // A row of small white-faced meters.
        const float meterHeight = std::min(content.getHeight() * 0.38f, 62.0f);
        auto meters = content.removeFromTop(meterHeight);
        const bool reduction = cassette == Console::microphone || cassette == Console::group;
        const bool correlation = cassette == Console::mainMix;
        const int count = reduction || correlation ? 3 : 2;
        const float width = (meters.getWidth() - 4.0f * static_cast<float>(count - 1)) / static_cast<float>(count);
        const auto next = [&] { auto m = meters.removeFromLeft(width); meters.removeFromLeft(4.0f); return m; };
        hardware::paintNeedleMeter(g, next(), (peak_[0] + 20.0) / 46.0, dbuFace("OUT L"));
        hardware::paintNeedleMeter(g, next(), (peak_[1] + 20.0) / 46.0, dbuFace("OUT R"));
        if (reduction)
            hardware::paintNeedleMeter(g, next(), 1.0 - static_cast<double>(gr_) / 20.0, reductionFace());
        else if (correlation)
            hardware::paintNeedleMeter(g, next(), (static_cast<double>(correlation_) + 1.0) * 0.5, correlationFace());
        content.removeFromTop(5.0f);

        // The nameplate: the model, the cassette, and its own regulated supply.
        auto plate = content.removeFromTop(19.0f);
        juce::ColourGradient chrome(juce::Colour(0xffe2e5e4), 0.0f, plate.getY(), juce::Colour(0xff7d8381), 0.0f, plate.getBottom(), false);
        g.setGradientFill(chrome);
        g.fillRoundedRectangle(plate, 2.5f);
        auto black = plate.reduced(1.5f);
        g.setColour(juce::Colour(0xff121313));
        g.fillRoundedRectangle(black, 2.0f);
        black.reduce(6.0f, 0.0f);
        g.setColour(kCream);
        g.setFont(sansBold(10.5f).withExtraKerningFactor(0.12f));
        const juce::String model = "TG12345" + juce::String(cassette == Console::microphone ? (console_.isMarkTwo() ? "  MK II" : "  MK I") : "");
        g.drawText(model, black, juce::Justification::centredLeft, false);
        hardware::paintLed(g, {black.getRight() - 3.0f, black.getCentreY()}, 2.6f, juce::Colour(0xff58d26e), true);
        g.setColour(juce::Colour(0xffb9b6ad));
        g.setFont(label(7.5f));
        g.drawText("REG", black.withTrimmedRight(9.0f), juce::Justification::centredRight, false);
        g.drawText(Console::cassetteNames()[cassette].toUpperCase(), black.withTrimmedLeft(96.0f).withTrimmedRight(30.0f),
                   juce::Justification::centredLeft, true);
        content.removeFromTop(6.0f);

        // The lamps (Main: announce; Monitor: talkback, dim, mono, cut), then the signal path.
        if (cassette == Console::mainMix || cassette == Console::monitor)
        {
            auto lamps = content.removeFromBottom(15.0f);
            content.removeFromBottom(4.0f);
            if (cassette == Console::mainMix)
                hardware::paintLamp(g, lamps.removeFromRight(76.0f), kRed, console_.announce_->get(), "ANNOUNCE", 2.0f, 7.0f);
            else
            {
                const float w = lamps.getWidth() / 4.0f;
                hardware::paintLamp(g, lamps.removeFromLeft(w).reduced(1.5f, 0.0f), kRed, console_.talkback_->get(), "TALKBACK", 2.0f, 7.0f);
                hardware::paintLamp(g, lamps.removeFromLeft(w).reduced(1.5f, 0.0f), juce::Colour(0xfff2b03a),
                                    console_.dim_->get() || console_.talkback_->get(), "DIM", 2.0f, 7.0f);
                hardware::paintLamp(g, lamps.removeFromLeft(w).reduced(1.5f, 0.0f), juce::Colour(0xfff2b03a), console_.mono_->get(), "MONO", 2.0f, 7.0f);
                hardware::paintLamp(g, lamps.reduced(1.5f, 0.0f), kRed, console_.cut_->get(), "CUT", 2.0f, 7.0f);
            }
        }
        paintPath(g, content);
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
        const auto font = label(7.5f);
        g.setFont(font);
        float x = area.getX(), y = area.getY();
        constexpr float height = 14.0f, gap = 9.0f;
        for (const auto& chip : chips())
        {
            const float width = juce::GlyphArrangement::getStringWidth(font, chip.text) + 10.0f;
            if (x + width > area.getRight() && x > area.getX())
            {
                x = area.getX();
                y += height + 4.0f;
            }
            if (y + height > area.getBottom())
                break;
            // A legend: lit where the stage acts, dark where it is switched out.
            const juce::Rectangle<float> box(x, y, width, height);
            hardware::paintLamp(g, box, kCream, chip.active, {}, 2.0f);
            g.setFont(font);
            g.setColour(chip.active ? kInk : juce::Colour(0xff9b9890));
            g.drawText(chip.text, box, juce::Justification::centred, false);
            x += width;
            g.setColour(kInk);
            g.drawText(juce::String::fromUTF8("\xe2\x80\xba"), juce::Rectangle<float>(x, y, gap, height), juce::Justification::centred, false);
            x += gap;
        }
    }

    static juce::String faderLegend(int cassette)
    {
        switch (cassette)
        {
        case Console::mainMix:
            return "MAIN";
        case Console::echoAux:
            return "RETURN";
        case Console::monitor:
            return "C.R.";
        default:
            return "FADER";
        }
    }

    void timerCallback() override
    {
        // Peak meters: rise at once, fall 20 dB/s.
        for (int ch = 0; ch < 2; ++ch)
        {
            const double now = static_cast<double>(console_.takeOutputPeakDbu(ch));
            auto& shown = peak_[static_cast<std::size_t>(ch)];
            shown = std::max(now, shown - 20.0 / 30.0);
        }
        gr_ = console_.getGainReductionDb();
        correlation_ = console_.getCorrelation();
        if (fader_ != nullptr)
            fader_->refresh();
        repaint();
    }

    Console& console_;
    std::unique_ptr<hardware::ParameterFader> fader_;
    std::array<double, 2> peak_{-100.0, -100.0};
    float gr_ = 0.0f, correlation_ = 1.0f;
};

constexpr int kDisplayWidth = 300;
} // namespace

//==================================================================================================

ConsoleEditor::ConsoleEditor(Console& console)
    : console_(console)
{
    setLookAndFeel(&look_);
    rebuild();
    startTimerHz(5);
}

ConsoleEditor::~ConsoleEditor()
{
    pages_.reset();
    setLookAndFeel(nullptr);
}

juce::String ConsoleEditor::faderFor(int cassette)
{
    switch (cassette)
    {
    case Console::echoAux:
        return "echoreturn";
    case Console::monitor:
        return "crlevel";
    default:
        return "fader";
    }
}

void ConsoleEditor::paint(juce::Graphics& g)
{
    const auto area = getLocalBounds().toFloat();
    // The desk's grey metalwork, hammertone, and the black label strip along its top.
    hardware::paintMetal(g, area, kGrey, 4.0f, true, 1912);
    hardware::paintMetal(g, area.withHeight(static_cast<float>(PagedEditor::kTabHeight) + 2.0f), juce::Colour(0xff4e5352), 4.0f, false);
    if (pages_ == nullptr)
        return;

    // A module per column of controls: its seams, and the screws that hold it (where there is room).
    const auto grid = pages_->getGridBounds().translated(pages_->getX(), pages_->getY()).toFloat();
    const float top = grid.getY() - 2.0f, bottom = area.getBottom() - 2.0f;
    const bool screws = bottom - (grid.getY() + 2.0f * static_cast<float>(ParameterControl::kHeight)) >= 9.0f;
    for (float x = grid.getX(); x <= grid.getRight() + 0.5f; x += static_cast<float>(ParameterControl::kWidth))
    {
        g.setColour(juce::Colour(0xff6c7170));
        g.drawVerticalLine(juce::roundToInt(x), top, bottom);
        g.setColour(juce::Colour(0xffc6cac8));
        g.drawVerticalLine(juce::roundToInt(x) + 1, top, bottom);
        if (screws && x + static_cast<float>(ParameterControl::kWidth) <= grid.getRight() + 0.5f)
            hardware::paintScrew(g, {x + static_cast<float>(ParameterControl::kWidth) * 0.5f, bottom - 4.5f}, 2.3f, x * 0.11f);
    }
    // The chrome trim along the front edge.
    juce::ColourGradient trim(juce::Colour(0xffe9ecea), 0.0f, area.getBottom() - 3.0f, juce::Colour(0xff6f7472), 0.0f, area.getBottom(), false);
    g.setGradientFill(trim);
    g.fillRect(area.withTop(area.getBottom() - 3.0f).reduced(3.0f, 0.0f));
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
