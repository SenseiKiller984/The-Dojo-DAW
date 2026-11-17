#include "ui/devices/MidiEffectEditors.h"

#include "devices/MidiEffects.h"
#include "ui/theme/DojoTheme.h"

#include <algorithm>
#include <functional>

namespace dojo::ui
{
namespace
{
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

juce::String noteName(int note)
{
    return note >= 0 && note <= 127 ? juce::MidiMessage::getMidiNoteName(note, true, true, 3) : juce::String("-");
}

bool isBlack(int note)
{
    const int pc = note % 12;
    return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
}

/** A keyboard from `low` to `high`; `tint` colours a key (transparent: plain). */
void paintKeyboard(juce::Graphics& g, juce::Rectangle<float> area, int low, int high, const std::function<juce::Colour(int)>& tint,
                   bool outline = true)
{
    int whites = 0;
    for (int n = low; n <= high; ++n)
        whites += isBlack(n) ? 0 : 1;
    if (whites == 0)
        return;
    const float w = area.getWidth() / static_cast<float>(whites);
    float x = area.getX();
    for (int n = low; n <= high; ++n)
    {
        if (isBlack(n))
            continue;
        const auto key = juce::Rectangle<float>(x, area.getY(), w, area.getHeight());
        g.setColour(juce::Colour(0xffe9e6df));
        g.fillRect(key);
        if (const auto c = tint(n); !c.isTransparent())
        {
            g.setColour(c);
            g.fillRect(key.reduced(w > 4.0f ? 1.0f : 0.0f, 1.0f));
        }
        if (outline)
        {
            g.setColour(juce::Colour(0xff4a4d55));
            g.drawRect(key, 0.6f);
        }
        x += w;
    }
    x = area.getX();
    for (int n = low; n <= high; ++n)
    {
        if (!isBlack(n))
        {
            x += w;
            continue;
        }
        const auto key = juce::Rectangle<float>(x - w * 0.32f, area.getY(), w * 0.64f, area.getHeight() * 0.6f);
        g.setColour(juce::Colour(0xff1b1c20));
        g.fillRect(key);
        if (const auto c = tint(n); !c.isTransparent())
        {
            g.setColour(c);
            g.fillRect(key.reduced(w > 4.0f ? 1.0f : 0.0f, 1.0f));
        }
    }
}

/** Every MIDI effect display: a dark panel with a header line, repainted as it plays. */
class MidiDisplay : public juce::Component, private juce::Timer
{
public:
    MidiDisplay() { startTimerHz(20); }

protected:
    juce::Rectangle<float> paintFrame(juce::Graphics& g, const juce::String& header) const
    {
        using namespace theme;
        auto area = getLocalBounds().toFloat();
        g.setColour(colour::meterBackground);
        g.fillRoundedRectangle(area, 3.0f);
        auto content = area.reduced(6.0f, 4.0f);
        g.setColour(colour::textSecondary);
        g.setFont(label(8.5f));
        g.drawText(header, content.removeFromTop(12.0f), juce::Justification::centredLeft, true);
        content.removeFromTop(3.0f);
        return content;
    }

private:
    void timerCallback() override
    {
        tick();
        repaint();
    }
    virtual void tick() {}
};

//==================================================================================================
class ArpDisplay final : public MidiDisplay
{
public:
    explicit ArpDisplay(devices::Arpeggiator& arp)
        : arp_(arp)
    {
    }

    void paint(juce::Graphics& g) override
    {
        using namespace theme;
        juce::String header = arp_.style_->getCurrentChoiceName().toUpperCase() + "  ";
        header << (arp_.sync_->get() ? arp_.rate_->getCurrentChoiceName() : juce::String(arp_.freeRate_->get(), 2) + " HZ") << "  "
               << arp_.octaves_->getCurrentChoiceName() << " OCT";
        auto content = paintFrame(g, header);
        if (arp_.hold_->get())
        {
            const auto lamp = getLocalBounds().toFloat().reduced(6.0f, 4.0f).removeFromTop(12.0f).removeFromRight(38.0f);
            g.setColour(colour::accent);
            g.fillRoundedRectangle(lamp, 2.0f);
            g.setColour(colour::textPrimary);
            g.setFont(label(8.0f));
            g.drawText("HOLD", lamp, juce::Justification::centred, false);
        }

        const int length = arp_.getShownLength();
        if (length == 0)
        {
            g.setColour(colour::textMuted);
            g.setFont(sans(11.0f));
            g.drawFittedText("Hold notes: the arpeggio plays them,\none after another.", content.toNearestInt(), juce::Justification::centred, 3);
            return;
        }
        int low = 127, high = 0;
        for (int i = 0; i < length; ++i)
            if (const int note = arp_.getShownNote(i); note >= 0)
            {
                low = std::min(low, note);
                high = std::max(high, note);
            }
        const int playing = arp_.getShownStep();
        const auto names = content.removeFromBottom(length <= 12 ? 11.0f : 0.0f);
        const float column = content.getWidth() / static_cast<float>(std::max(length, 8));
        for (int i = 0; i < length; ++i)
        {
            const int note = arp_.getShownNote(i);
            if (note < 0)
                continue;
            const float fraction = static_cast<float>(note - low + 1) / static_cast<float>(high - low + 1);
            const auto bar = juce::Rectangle<float>(content.getX() + column * static_cast<float>(i), content.getBottom() - content.getHeight() * fraction,
                                                    column, content.getHeight() * fraction)
                                 .reduced(std::min(1.5f, column * 0.15f), 0.0f);
            g.setColour(i == playing ? colour::accentBright : colour::controlHover);
            g.fillRoundedRectangle(bar, 1.5f);
            if (!names.isEmpty())
            {
                g.setColour(i == playing ? colour::textPrimary : colour::textMuted);
                g.setFont(mono(7.5f));
                g.drawText(noteName(note), juce::Rectangle<float>(bar.getX() - 4.0f, names.getY(), bar.getWidth() + 8.0f, names.getHeight()),
                           juce::Justification::centred, false);
            }
        }
    }

private:
    devices::Arpeggiator& arp_;
};

//==================================================================================================
class ChordDisplay final : public MidiDisplay
{
public:
    explicit ChordDisplay(devices::ChordGenerator& chord)
        : chord_(chord)
    {
    }

    void paint(juce::Graphics& g) override
    {
        using namespace theme;
        const bool diatonic = chord_.mode_->getIndex() == devices::ChordGenerator::diatonicMode;
        juce::String header = diatonic ? "DIATONIC  " + chord_.key_->getCurrentChoiceName() + " " +
                                             chord_.scale_->getCurrentChoiceName().toUpperCase() + "  " +
                                             chord_.diatonic_->getCurrentChoiceName().toUpperCase()
                                       : chord_.shape_->getCurrentChoiceName().toUpperCase() + "  " +
                                             chord_.inversion_->getCurrentChoiceName().toUpperCase() + "  " +
                                             chord_.voicing_->getCurrentChoiceName().toUpperCase();
        auto content = paintFrame(g, header);
        const auto text = content.removeFromBottom(24.0f);

        // The chord C3 gets now, and the last one played.
        std::array<int, devices::ChordGenerator::kMaxChord> preview{};
        const int count = chord_.chordFor(60, preview);
        const int shown = chord_.getShownCount();
        const auto inPreview = [&](int note) { return std::find(preview.begin(), preview.begin() + count, note) != preview.begin() + count; };
        paintKeyboard(g, content, 48, 83,
                      [&](int note) { return inPreview(note) ? colour::accent.withAlpha(0.85f) : juce::Colour(); });
        for (int i = 0; i < shown; ++i)
            if (const int note = chord_.getShownNote(i); note >= 48 && note <= 83)
                markKey(g, content, note);

        juce::String line = "C3: ";
        for (int i = 0; i < count; ++i)
            line << noteName(preview[static_cast<std::size_t>(i)]) << ' ';
        g.setColour(colour::textSecondary);
        g.setFont(mono(9.0f));
        g.drawText(line, text.withHeight(12.0f), juce::Justification::centredLeft, true);
        if (shown > 0)
        {
            juce::String last = "LAST: ";
            for (int i = 0; i < shown; ++i)
                last << noteName(chord_.getShownNote(i)) << ' ';
            g.setColour(colour::gold);
            g.drawText(last, text.withTrimmedTop(12.0f), juce::Justification::centredLeft, true);
        }
    }

private:
    /** A gold dot on a key (the last chord played). */
    static void markKey(juce::Graphics& g, juce::Rectangle<float> keyboard, int note)
    {
        int whites = 0, whiteIndex = 0;
        for (int n = 48; n <= 83; ++n)
            if (!isBlack(n))
            {
                if (n <= note)
                    whiteIndex = whites;
                ++whites;
            }
        const float w = keyboard.getWidth() / static_cast<float>(whites);
        const float x = isBlack(note) ? keyboard.getX() + w * static_cast<float>(whiteIndex + 1) : keyboard.getX() + w * (static_cast<float>(whiteIndex) + 0.5f);
        const float y = isBlack(note) ? keyboard.getY() + keyboard.getHeight() * 0.45f : keyboard.getBottom() - 6.0f;
        g.setColour(theme::colour::gold);
        g.fillEllipse(juce::Rectangle<float>(5.0f, 5.0f).withCentre({x, y}));
    }

    devices::ChordGenerator& chord_;
};

//==================================================================================================
class ScaleDisplay final : public MidiDisplay
{
public:
    explicit ScaleDisplay(devices::ScaleDevice& scale)
        : scale_(scale)
    {
    }

    void paint(juce::Graphics& g) override
    {
        using namespace theme;
        auto content = paintFrame(g, scale_.root_->getCurrentChoiceName() + " " + scale_.scale_->getCurrentChoiceName().toUpperCase() + "  " +
                                         scale_.mode_->getCurrentChoiceName().toUpperCase());
        const auto text = content.removeFromBottom(14.0f);
        const int root = scale_.root_->getIndex();
        const auto mask = scale_.getMask();
        const int lastIn = scale_.getLastIn(), lastOut = scale_.getLastOut();
        // One octave from C: the key's notes lit, the root brightest, the last note's way marked.
        paintKeyboard(g, content, 60, 71,
                      [&](int note)
                      {
                          const int pc = note % 12;
                          if (lastOut >= 0 && lastOut % 12 == pc)
                              return colour::gold;
                          if (((static_cast<unsigned>(mask) >> static_cast<unsigned>((pc - root + 12) % 12)) & 1u) == 0)
                              return juce::Colour();
                          return pc == root ? colour::accentBright : colour::accent.withAlpha(0.55f);
                      });
        if (lastIn >= 0)
        {
            g.setColour(lastOut >= 0 ? colour::textSecondary : colour::meterClip);
            g.setFont(mono(9.5f));
            g.drawText(noteName(lastIn) + juce::String::fromUTF8("  \xe2\x86\x92  ") + (lastOut >= 0 ? noteName(lastOut) : juce::String("removed")), text,
                       juce::Justification::centredLeft, false);
        }
    }

private:
    devices::ScaleDevice& scale_;
};

//==================================================================================================
class FilterDisplay final : public MidiDisplay
{
public:
    explicit FilterDisplay(devices::MidiFilter& filter)
        : filter_(filter)
    {
        tick();
    }

    void paint(juce::Graphics& g) override
    {
        using namespace theme;
        const int low = juce::roundToInt(std::min(filter_.lowNote_->get(), filter_.highNote_->get()));
        const int high = juce::roundToInt(std::max(filter_.lowNote_->get(), filter_.highNote_->get()));
        const bool outside = filter_.outside_->get();
        auto content = paintFrame(g, juce::String(outside ? "NOTES OUTSIDE " : "NOTES ") + noteName(low) + " - " + noteName(high) + "   CH " +
                                         filter_.channel_->getCurrentChoiceName().toUpperCase());

        // The notes let through on the whole keyboard.
        paintKeyboard(g, content.removeFromTop(26.0f), 0, 127,
                      [&](int note) { return ((note >= low && note <= high) != outside) ? colour::play.withAlpha(0.8f) : juce::Colour(); },
                      false);
        content.removeFromTop(6.0f);

        // The velocities let through.
        auto velocity = content.removeFromTop(12.0f);
        g.setColour(colour::textMuted);
        g.setFont(label(7.5f));
        g.drawText("VEL", velocity.removeFromLeft(22.0f), juce::Justification::centredLeft, false);
        g.setColour(colour::panelRaised);
        g.fillRoundedRectangle(velocity, 2.0f);
        const float vLow = std::min(filter_.velocityLow_->get(), filter_.velocityHigh_->get());
        const float vHigh = std::max(filter_.velocityLow_->get(), filter_.velocityHigh_->get());
        g.setColour(colour::play.withAlpha(0.8f));
        g.fillRoundedRectangle(velocity.withX(velocity.getX() + velocity.getWidth() * (vLow - 1.0f) / 126.0f)
                                   .withWidth(std::max(2.0f, velocity.getWidth() * (vHigh - vLow) / 126.0f)),
                               2.0f);
        content.removeFromTop(6.0f);

        // A lamp per kind of message: green when one got through, red when one was held back.
        static const juce::StringArray names{"NOTE", "CC", "BEND", "AT", "PROG", "SYS"};
        const float width = content.getWidth() / static_cast<float>(devices::MidiFilter::kindCount);
        auto lamps = content.removeFromTop(16.0f);
        for (int k = 0; k < devices::MidiFilter::kindCount; ++k)
        {
            const auto lamp = lamps.removeFromLeft(width).reduced(1.5f, 0.0f);
            const auto& state = lamps_[static_cast<std::size_t>(k)];
            const juce::Colour lit = state.passedFor > 0 ? colour::play : state.blockedFor > 0 ? colour::meterClip : colour::control;
            g.setColour(lit);
            g.fillRoundedRectangle(lamp, 2.0f);
            g.setColour(state.passedFor > 0 || state.blockedFor > 0 ? colour::textPrimary : colour::textMuted);
            g.setFont(label(7.5f));
            g.drawText(names[k] + (filter_.allow_[static_cast<std::size_t>(k)]->get() ? juce::String() : juce::String(" X")), lamp,
                       juce::Justification::centred, false);
        }
    }

private:
    void tick() override
    {
        for (int k = 0; k < devices::MidiFilter::kindCount; ++k)
        {
            auto& state = lamps_[static_cast<std::size_t>(k)];
            const int seen = filter_.getSeen(k), passed = filter_.getPassed(k);
            state.passedFor = passed != state.passed ? 6 : std::max(0, state.passedFor - 1);
            state.blockedFor = seen - state.seen > passed - state.passed ? 6 : std::max(0, state.blockedFor - 1);
            state.seen = seen;
            state.passed = passed;
        }
    }

    struct Lamp
    {
        int seen = 0, passed = 0, passedFor = 0, blockedFor = 0;
    };
    devices::MidiFilter& filter_;
    std::array<Lamp, devices::MidiFilter::kindCount> lamps_{};
};

template <typename Display, typename Device>
std::function<std::unique_ptr<juce::Component>()> displayOf(Device& device)
{
    return [&device] { return std::unique_ptr<juce::Component>(std::make_unique<Display>(device)); };
}
} // namespace

std::vector<PagedEditor::Page> midiEffectPages(juce::AudioProcessor& processor)
{
    if (auto* arp = dynamic_cast<devices::Arpeggiator*>(&processor))
    {
        const auto display = displayOf<ArpDisplay>(*arp);
        return {{"Pattern", ids({"style|Style", "rate|Rate", "sync|Sync", "freerate|Free Rate", "gate|Gate", "octaves|Octaves", "swing|Swing", "hold|Hold"}),
                 display, 240},
                {"Velocity & Repeats", ids({"velmode|Velocity", "velocity|Fixed Vel", "repeats|Repeats", "retrigger|Retrigger"}), display, 240}};
    }
    if (auto* chord = dynamic_cast<devices::ChordGenerator*>(&processor))
    {
        const auto display = displayOf<ChordDisplay>(*chord);
        return {{"Chord", ids({"mode|Mode", "shape|Shape", "inversion|Inversion", "voicing|Voicing", "velocity|Added Vel", "strum|Strum", "strumdir|Strum Dir"}),
                 display, 250},
                {"Custom", ids({"shift1|Shift 1", "shift2|Shift 2", "shift3|Shift 3", "shift4|Shift 4", "shift5|Shift 5", "shift6|Shift 6"}), display, 250},
                {"Diatonic", ids({"key|Key", "scale|Scale", "type|Chord"}), display, 250}};
    }
    if (auto* scale = dynamic_cast<devices::ScaleDevice*>(&processor))
        return {{"Scale", ids({"root|Root", "scale|Scale", "mode|Mode", "transpose|Transpose", "shift|Shift", "octave|Octave"}), displayOf<ScaleDisplay>(*scale),
                 210}};
    if (auto* filter = dynamic_cast<devices::MidiFilter*>(&processor))
    {
        const auto display = displayOf<FilterDisplay>(*filter);
        return {{"Notes", ids({"low|Lowest", "high|Highest", "outside|Outside", "vellow|Vel Min", "velhigh|Vel Max", "velscale|Vel Scale", "veloffset|Vel Offset"}),
                 display, 240},
                {"Messages", ids({"channel|Channel", "outchannel|Out Channel", "notes|Notes", "cc|Controllers", "pitchbend|Pitch Bend",
                                  "aftertouch|Aftertouch", "program|Program", "other|Other"}),
                 display, 240}};
    }
    return {};
}

std::unique_ptr<juce::Component> createMidiEffectEditor(juce::AudioProcessor& processor, int& preferredWidth)
{
    auto pages = midiEffectPages(processor);
    if (pages.empty())
        return nullptr;
    preferredWidth = PagedEditor::preferredWidth(pages);
    return std::make_unique<PagedEditor>(processor, std::move(pages));
}

} // namespace dojo::ui
