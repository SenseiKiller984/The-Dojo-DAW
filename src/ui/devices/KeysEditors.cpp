#include "ui/devices/KeysEditors.h"

#include "devices/RhodesSuitcase.h"
#include "session/Project.h"
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
/** The Rhodes' top: its LED, the keyboard with the sounding keys, and the accessory loop's state
    (with a button that adds the loop's return, the Suitcase Base). */
class RhodesFace final : public juce::Component, private juce::Timer
{
public:
    RhodesFace(devices::RhodesSuitcase& rhodes, const DeviceEditorContext& context, bool showLoop)
        : rhodes_(rhodes)
        , project_(context.project)
        , device_(context.device)
        , showLoop_(showLoop)
    {
        addReturn_.setTooltip("Adds a Suitcase Base at the end of this track: put your loop effects between it and the Rhodes.");
        addReturn_.onClick = [this]
        {
            addSuitcaseBase(project_, device_, rhodes_);
            updateButton();
        };
        addChildComponent(addReturn_);
        updateButton();
        startTimerHz(30);
    }

    void resized() override
    {
        addReturn_.setBounds(getLocalBounds().removeFromBottom(26).reduced(8, 2).removeFromRight(150));
    }

    void paint(juce::Graphics& g) override
    {
        using namespace theme;
        const auto area = getLocalBounds().toFloat();
        paintPanel(g, area, "SUITCASE 73 MK II");
        auto content = area.reduced(8.0f, 4.0f).withTrimmedTop(14.0f);

        // The LED (power on, and the vibrato's rate).
        const bool vibrato = rhodes_.stage.intensity->get() > 0.01f && !rhodes_.loop_->get();
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
                blacks.emplace_back(juce::Rectangle<float>(keyboard.getX() + static_cast<float>(whiteIndex) * white - white * 0.32f, keyboard.getY(), white * 0.64f,
                                                           keyboard.getHeight() * 0.6f),
                                    sounding);
                continue;
            }
            const auto key = juce::Rectangle<float>(keyboard.getX() + static_cast<float>(whiteIndex) * white, keyboard.getY(), white, keyboard.getHeight()).reduced(0.5f, 0.0f);
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
        if (showLoop_ && rhodes_.loop_->get())
        {
            g.setColour(colour::gold);
            g.drawText(hasSuitcaseBaseAfter(device_) ? "LOOP: send -> your effects -> Suitcase Base" : "LOOP: add effects, then a Suitcase Base",
                       content.removeFromTop(13.0f), juce::Justification::centredLeft, false);
        }
    }

    [[nodiscard]] juce::Button& getAddReturnButton() noexcept { return addReturn_; }

private:
    void updateButton()
    {
        addReturn_.setVisible(showLoop_ && rhodes_.loop_->get() && !hasSuitcaseBaseAfter(device_));
    }

    void timerCallback() override
    {
        updateButton();
        repaint();
    }

    devices::RhodesSuitcase& rhodes_;
    session::Project& project_;
    juce::ValueTree device_;
    bool showLoop_;
    juce::TextButton addReturn_{"Add Suitcase Base"};
};

/** The Suitcase Base: its LED and what it plays through. */
class BaseFace final : public juce::Component, private juce::Timer
{
public:
    explicit BaseFace(devices::SuitcaseBase& base)
        : base_(base)
    {
        startTimerHz(30);
    }

    void paint(juce::Graphics& g) override
    {
        using namespace theme;
        const auto area = getLocalBounds().toFloat();
        paintPanel(g, area, "SUITCASE BASE");
        paintLed(g, {area.getRight() - 14.0f, area.getY() + 10.0f}, base_.getLfo(), base_.stage.intensity->get() > 0.01f);
        auto content = area.reduced(8.0f, 4.0f).withTrimmedTop(16.0f);
        // The speaker grille.
        const auto grille = content.removeFromTop(content.getHeight() - 16.0f);
        g.setColour(juce::Colour(0xff2a2826));
        g.fillRoundedRectangle(grille, 3.0f);
        const bool tens = base_.stage.cabinet->getIndex() == 0;
        const int count = tens ? 4 : 2;
        const float size = std::min(grille.getHeight() * (tens ? 0.42f : 0.8f), grille.getWidth() / (tens ? 2.4f : 2.4f));
        for (int s = 0; s < count; ++s)
        {
            const float column = tens ? static_cast<float>(s % 2) : static_cast<float>(s);
            const float row = tens ? static_cast<float>(s / 2) : 0.5f;
            const juce::Point<float> centre(grille.getX() + grille.getWidth() * (0.28f + 0.44f * column),
                                            grille.getY() + grille.getHeight() * (tens ? 0.27f + 0.46f * row : 0.5f));
            g.setColour(juce::Colour(0xff4a4744));
            g.drawEllipse(juce::Rectangle<float>(size, size).withCentre(centre), 1.5f);
            g.drawEllipse(juce::Rectangle<float>(size * 0.35f, size * 0.35f).withCentre(centre), 1.0f);
        }
        g.setFont(mono(9.0f));
        g.setColour(colour::textSecondary);
        g.drawText(base_.stage.output->getIndex() == 0 ? base_.stage.cabinet->getCurrentChoiceName() + "  2 x 40 W" : juce::String("LINE OUT"), content,
                   juce::Justification::centredLeft, false);
    }

private:
    void timerCallback() override { repaint(); }

    devices::SuitcaseBase& base_;
};

std::unique_ptr<juce::Component> paged(juce::AudioProcessor& processor, std::vector<Page> pages, int& preferredWidth)
{
    preferredWidth = PagedEditor::preferredWidth(pages);
    return std::make_unique<PagedEditor>(processor, std::move(pages));
}
} // namespace

bool hasSuitcaseBaseAfter(const juce::ValueTree& rhodesDevice)
{
    const auto owner = rhodesDevice.getParent();
    if (!owner.isValid())
        return false;
    bool after = false;
    for (const auto& device : session::Session::getDevices(owner))
    {
        if (device == rhodesDevice)
            after = true;
        else if (after && session::device::getType(device) == devices::SuitcaseBase::kTypeId)
            return true;
    }
    return false;
}

juce::ValueTree addSuitcaseBase(session::Project& project, const juce::ValueTree& rhodesDevice, devices::RhodesSuitcase& rhodes)
{
    auto& session = project.getSession();
    session.beginTransaction("Add Suitcase Base");
    const auto base = session.addDevice(rhodesDevice.getParent(), devices::SuitcaseBase::kTypeId, "Suitcase Base");
    if (const auto node = project.getDevices().getNode(base))
        if (auto* processor = dynamic_cast<devices::SuitcaseBase*>(&node->getProcessor()))
        {
            // Set up like the Rhodes' own suitcase, so the sound does not change.
            const auto copy = [](juce::RangedAudioParameter* from, juce::RangedAudioParameter* to)
            { to->setValueNotifyingHost(from->getValue()); };
            copy(rhodes.stage.speed, processor->stage.speed);
            copy(rhodes.stage.intensity, processor->stage.intensity);
            copy(rhodes.stage.modulator, processor->stage.modulator);
            copy(rhodes.stage.output, processor->stage.output);
            copy(rhodes.stage.cabinet, processor->stage.cabinet);
            copy(rhodes.stage.master, processor->stage.master);
        }
    return base;
}

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
    if (auto* base = dynamic_cast<devices::SuitcaseBase*>(&processor))
        return paged(processor,
                     {{"Suitcase Base",
                       ids({"speed|Speed", "intensity|Intensity", "modulator|Modulator", "out|Output", "cabinet|Speakers", "master|Master"}),
                       [base] { return std::unique_ptr<juce::Component>(std::make_unique<BaseFace>(*base)); },
                       200}},
                     preferredWidth);
    return nullptr;
}

} // namespace dojo::ui
