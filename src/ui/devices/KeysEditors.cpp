#include "ui/devices/KeysEditors.h"

#include "devices/RhodesSuitcase.h"
#include "session/Session.h"
#include "ui/devices/PagedEditor.h"
#include "ui/theme/DojoTheme.h"

#include <cmath>

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

/** The front panel's LED: on with the power, pulsing with the vibrato's LFO. */
void paintLed(juce::Graphics& g, juce::Point<float> centre, float lfo, bool vibrato)
{
    const float brightness = vibrato ? 0.25f + 0.75f * (0.5f + 0.5f * lfo) : 1.0f;
    g.setColour(juce::Colour(0xffff3b30).withAlpha(0.25f * brightness));
    g.fillEllipse(juce::Rectangle<float>(16.0f, 16.0f).withCentre(centre));
    g.setColour(juce::Colour(0xffff3b30).withMultipliedBrightness(0.4f + 0.6f * brightness));
    g.fillEllipse(juce::Rectangle<float>(8.0f, 8.0f).withCentre(centre));
}

//==================================================================================================
/** The Rhodes' top: its LED, the keyboard with the sounding keys, and its accessory loop's state. */
class RhodesFace final : public juce::Component, private juce::Timer
{
public:
    RhodesFace(devices::RhodesSuitcase& rhodes, const DeviceEditorContext& context, bool showLoop)
        : rhodes_(rhodes)
        , device_(context.device)
        , showLoop_(showLoop)
    {
        startTimerHz(30);
    }

    void paint(juce::Graphics& g) override
    {
        using namespace theme;
        const auto area = getLocalBounds().toFloat();
        paintPanel(g, area, "SUITCASE 73 MK II");
        auto content = area.reduced(8.0f, 4.0f).withTrimmedTop(14.0f);

        // The LED (power on, and the vibrato's rate).
        const bool vibrato = rhodes_.stage.intensity->get() > 0.01f;
        paintLed(g, {area.getRight() - 14.0f, area.getY() + 10.0f}, rhodes_.getLfo(), vibrato);

        // The keyboard: 73 keys, E1 to E7, the sounding ones lit.
        auto keyboard = content.removeFromTop(std::min(46.0f, content.getHeight() * 0.5f));
        constexpr int kWhiteKeys = 43;
        const float white = keyboard.getWidth() / kWhiteKeys;
        const auto isBlack = [](int note) { const int n = note % 12; return n == 1 || n == 3 || n == 6 || n == 8 || n == 10; };
        int whiteIndex = 0;
        std::vector<std::pair<juce::Rectangle<float>, bool>> blacks;
        for (int k = 0; k < devices::RhodesSuitcase::kKeys; ++k)
        {
            const int note = devices::RhodesSuitcase::kLowestNote + k;
            const bool sounding = rhodes_.isKeySounding(k);
            if (isBlack(note))
            {
                blacks.emplace_back(juce::Rectangle<float>(keyboard.getX() + static_cast<float>(whiteIndex) * white - white * 0.32f, keyboard.getY(),
                                                           white * 0.64f, keyboard.getHeight() * 0.6f),
                                    sounding);
                continue;
            }
            const auto key =
                juce::Rectangle<float>(keyboard.getX() + static_cast<float>(whiteIndex) * white, keyboard.getY(), white, keyboard.getHeight()).reduced(0.5f, 0.0f);
            g.setColour(sounding ? colour::accentBright : juce::Colour(0xffe8e4da));
            g.fillRect(key);
            ++whiteIndex;
        }
        for (const auto& [key, sounding] : blacks)
        {
            g.setColour(sounding ? colour::accent : juce::Colour(0xff1c1b1f));
            g.fillRect(key);
        }

        // The settings, and the accessory loop.
        content.removeFromTop(6.0f);
        g.setFont(mono(9.0f));
        g.setColour(colour::textSecondary);
        g.drawText(juce::String(rhodes_.getSoundingKeys()) + " sounding   " + rhodes_.load_->getCurrentChoiceName() + "   " +
                       (rhodes_.stage.output->getIndex() == 0 ? rhodes_.stage.cabinet->getCurrentChoiceName() + " speakers" : juce::String("line out")),
                   content.removeFromTop(13.0f), juce::Justification::centredLeft, false);
        if (showLoop_)
        {
            const auto effects = session::Session::getDevices(device_);
            juce::String text = "LOOP: ";
            if (!rhodes_.loop_->get())
                text << "out (switched off)";
            else if (effects.empty())
                text << "empty: drop effects into the loop after this card";
            else
            {
                juce::StringArray names;
                for (const auto& e : effects)
                    names.add(session::device::getName(e));
                text << "send > " << names.joinIntoString(" > ") << " > return";
            }
            g.setColour(rhodes_.loop_->get() && !effects.empty() ? colour::gold : colour::textMuted);
            g.drawFittedText(text, content.removeFromTop(26.0f).toNearestInt(), juce::Justification::topLeft, 2);
        }
    }

private:
    void timerCallback() override { repaint(); }

    devices::RhodesSuitcase& rhodes_;
    juce::ValueTree device_;
    bool showLoop_;
};

std::unique_ptr<juce::Component> paged(juce::AudioProcessor& processor, std::vector<Page> pages, int& preferredWidth)
{
    preferredWidth = PagedEditor::preferredWidth(pages);
    return std::make_unique<PagedEditor>(processor, std::move(pages));
}
} // namespace

std::unique_ptr<juce::Component> createKeysDeviceEditor(juce::AudioProcessor& processor, const DeviceEditorContext& context, int& preferredWidth)
{
    if (auto* rhodes = dynamic_cast<devices::RhodesSuitcase*>(&processor))
    {
        const auto face = [rhodes, context](bool loop)
        { return [rhodes, context, loop] { return std::unique_ptr<juce::Component>(std::make_unique<RhodesFace>(*rhodes, context, loop)); }; };
        return paged(processor,
                     {{"Harp",
                       ids({"voicing|Voicing", "hammer|Hammer", "tine|Tine", "decay|Decay", "release|Release", "velcurve|Velocity", "age|Age",
                            "tuning|Tuning", "load|Harp Load"}),
                       face(false),
                       300},
                      {"Janus Preamp",
                       ids({"volume|Volume", "treble|Treble", "bass|Bass", "loop|Acc. Loop", "speed|Speed", "intensity|Intensity",
                            "modulator|Modulator"}),
                       face(true),
                       300},
                      {"Power Base", ids({"out|Output", "cabinet|Speakers", "master|Master"}), face(true), 300}},
                     preferredWidth);
    }
    return nullptr;
}

} // namespace dojo::ui
