#include "ui/PreferencesWindow.h"

#include "ui/surfaces/ControlSurfaces.h"
#include "ui/theme/DojoTheme.h"
#include "ui/theme/Themes.h"

#include <array>
#include <initializer_list>

namespace dojo::ui
{
namespace
{
constexpr int kWidth = 800;
constexpr int kHeight = 640;

juce::Label& styleDescription(juce::Label& label)
{
    label.setFont(theme::sans(12.0f));
    label.setColour(juce::Label::textColourId, theme::colour::textSecondary);
    label.setJustificationType(juce::Justification::topLeft);
    label.setMinimumHorizontalScale(1.0f);
    return label;
}

void styleToggle(juce::ToggleButton& toggle)
{
    toggle.setColour(juce::ToggleButton::textColourId, theme::colour::textPrimary);
    toggle.setColour(juce::ToggleButton::tickColourId, theme::colour::accent);
    toggle.setColour(juce::ToggleButton::tickDisabledColourId, theme::colour::textMuted);
    toggle.setWantsKeyboardFocus(false);
}

//==================================================================================================
/** Playback and MIDI input options. */
class GeneralPage final : public juce::Component, private juce::ChangeListener
{
public:
    explicit GeneralPage(Preferences& preferences)
        : preferences_(preferences)
    {
        returnView_.setButtonText("Return the view to the start with the playhead");
        returnViewText_.setText("When the playhead goes back to the start (press Stop twice, the Return to Start button, or "
                                "Home), the arrangement scrolls back to the start too: from bar 550 straight back to bar 1.",
                                juce::dontSendNotification);
        stopAtEnd_.setButtonText("Stop at the end of the arrangement");
        stopAtEndText_.setText("Playback stops at the end of the last audio or MIDI clip instead of running on into "
                               "empty bars. A loop still loops, and recording carries on past the end.",
                               juce::dontSendNotification);

        mpeInput_.setButtonText("MPE controllers");
        mpeInputText_.setText("Notes played one per MIDI channel 2 to 16 (an MPE controller's lower zone) record their "
                              "own pitch bend, pressure and slide as the notes' expression curves, to edit in the piano "
                              "roll's Note Expression lanes. Turn this off for a keyboard that sends on channels 2 to 16 "
                              "without being an MPE controller: its pitch bend and pressure then record as controllers.",
                              juce::dontSendNotification);
        mpeRangeLabel_.setText("Per-note pitch bend range", juce::dontSendNotification);
        mpeRangeLabel_.setFont(theme::sans(13.0f));
        mpeRangeLabel_.setColour(juce::Label::textColourId, theme::colour::textPrimary);
        for (const auto& [id, text] : {std::pair{48, "48 semitones (the MPE default)"}, std::pair{24, "24 semitones"},
                                       std::pair{12, "12 semitones"}, std::pair{2, "2 semitones"}})
            mpeRange_.addItem(text, id);
        mpeRange_.setTooltip("The bend range your MPE controller sends with (a full bend in either direction)");
        addAndMakeVisible(mpeRangeLabel_);
        addAndMakeVisible(mpeRange_);

        for (auto* toggle : {&returnView_, &stopAtEnd_, &mpeInput_})
        {
            styleToggle(*toggle);
            addAndMakeVisible(*toggle);
        }
        for (auto* label : {&returnViewText_, &stopAtEndText_, &mpeInputText_})
            addAndMakeVisible(styleDescription(*label));

        returnView_.onClick = [this] { preferences_.setReturnViewToStart(returnView_.getToggleState()); };
        stopAtEnd_.onClick = [this] { preferences_.setStopAtEnd(stopAtEnd_.getToggleState()); };
        mpeInput_.onClick = [this] { preferences_.setMpeInput(mpeInput_.getToggleState()); };
        mpeRange_.onChange = [this]
        {
            if (mpeRange_.getSelectedId() > 0)
                preferences_.setMpeBendRange(static_cast<double>(mpeRange_.getSelectedId()));
        };
        preferences_.addChangeListener(this);
        update();
    }

    ~GeneralPage() override { preferences_.removeChangeListener(this); }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(theme::colour::panel);
        g.setColour(theme::colour::textSecondary);
        g.setFont(theme::label(11.0f));
        g.drawText("PLAYBACK", getLocalBounds().reduced(24, 0).removeFromTop(48), juce::Justification::bottomLeft, false);
        g.drawText("MIDI INPUT", getLocalBounds().reduced(24, 0).withY(midiHeadingTop_).withHeight(30), juce::Justification::bottomLeft,
                   false);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(24, 0).withTrimmedTop(58);
        for (auto [toggle, text] : {std::pair{&returnView_, &returnViewText_}, std::pair{&stopAtEnd_, &stopAtEndText_}})
        {
            toggle->setBounds(area.removeFromTop(28));
            text->setBounds(area.removeFromTop(40).withTrimmedLeft(30));
            area.removeFromTop(14);
        }
        midiHeadingTop_ = area.getY();
        area.removeFromTop(40);
        mpeInput_.setBounds(area.removeFromTop(28));
        mpeInputText_.setBounds(area.removeFromTop(58).withTrimmedLeft(30));
        auto row = area.removeFromTop(28).withTrimmedLeft(30);
        mpeRangeLabel_.setBounds(row.removeFromLeft(190));
        mpeRange_.setBounds(row.removeFromLeft(260));
    }

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override { update(); }

    void update()
    {
        returnView_.setToggleState(preferences_.getReturnViewToStart(), juce::dontSendNotification);
        stopAtEnd_.setToggleState(preferences_.getStopAtEnd(), juce::dontSendNotification);
        mpeInput_.setToggleState(preferences_.getMpeInput(), juce::dontSendNotification);
        mpeRange_.setSelectedId(juce::roundToInt(preferences_.getMpeBendRange()), juce::dontSendNotification);
        mpeRange_.setEnabled(preferences_.getMpeInput());
        mpeRangeLabel_.setEnabled(preferences_.getMpeInput());
    }

    Preferences& preferences_;
    juce::ToggleButton returnView_, stopAtEnd_, mpeInput_;
    juce::Label returnViewText_, stopAtEndText_, mpeInputText_, mpeRangeLabel_;
    juce::ComboBox mpeRange_;
    int midiHeadingTop_ = 0;
};

//==================================================================================================
/** A theme drawn as a miniature of the main window, in its own colours. */
class ThemeCard final : public juce::Component, public juce::SettableTooltipClient
{
public:
    ThemeCard(const theme::Theme& theme, std::function<void()> onChosen)
        : theme_(theme)
        , onChosen_(std::move(onChosen))
    {
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
        setTitle(theme_.name);
        setTooltip(theme_.description);
    }

    void setSelected(bool selected)
    {
        selected_ = selected;
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        const auto& p = theme_.palette;
        auto bounds = getLocalBounds().toFloat().reduced(4.0f);
        auto caption = bounds.removeFromBottom(36.0f);
        const auto preview = bounds;

        // The window: transport bar, browser, lanes with clips, a playhead, a knob.
        juce::Path clip;
        clip.addRoundedRectangle(preview, 6.0f);
        g.saveState();
        g.reduceClipRegion(clip);
        g.setColour(p.windowBackground);
        g.fillRect(preview);
        auto area = preview;
        auto top = area.removeFromTop(16.0f);
        g.setColour(p.panelHeader);
        g.fillRect(top);
        g.setColour(p.accent);
        g.fillEllipse(top.removeFromLeft(16.0f).reduced(4.0f));
        g.setColour(p.play);
        g.fillRect(top.removeFromLeft(14.0f).reduced(3.0f, 4.0f));
        g.setColour(p.textMuted);
        g.fillRect(top.removeFromRight(40.0f).reduced(6.0f, 6.0f));

        auto browser = area.removeFromLeft(preview.getWidth() * 0.22f);
        g.setColour(p.panel);
        g.fillRect(browser);
        g.setColour(p.outline);
        g.fillRect(browser.getRight() - 1.0f, browser.getY(), 1.0f, browser.getHeight());
        for (int i = 0; i < 5; ++i)
        {
            g.setColour(i == 1 ? p.accent.withAlpha(0.35f) : p.textSecondary.withAlpha(0.6f));
            g.fillRect(browser.getX() + 5.0f, browser.getY() + 7.0f + static_cast<float>(i) * 11.0f, browser.getWidth() * (i == 1 ? 0.7f : 0.5f),
                       3.0f);
        }

        const float laneHeight = area.getHeight() / 3.0f;
        for (int lane = 0; lane < 3; ++lane)
        {
            const auto row = juce::Rectangle<float>(area.getX(), area.getY() + laneHeight * static_cast<float>(lane), area.getWidth(), laneHeight);
            g.setColour(lane % 2 == 0 ? p.laneEven : p.laneOdd);
            g.fillRect(row);
            const auto colour = theme::trackPalette[static_cast<std::size_t>((lane * 3) % 8)];
            const auto box = row.reduced(4.0f, 4.0f).withTrimmedLeft(static_cast<float>(lane) * 14.0f).withWidth(area.getWidth() * 0.55f);
            g.setColour(colour.withAlpha(0.3f));
            g.fillRoundedRectangle(box, 2.0f);
            g.setColour(colour);
            g.fillRoundedRectangle(box.withHeight(5.0f), 2.0f);
        }
        for (int bar = 1; bar < 6; ++bar)
        {
            g.setColour(p.gridBar.withAlpha(0.8f));
            g.fillRect(area.getX() + area.getWidth() * static_cast<float>(bar) / 6.0f, area.getY(), 1.0f, area.getHeight());
        }
        g.setColour(p.playhead);
        g.fillRect(area.getX() + area.getWidth() * 0.62f, area.getY(), 1.5f, area.getHeight());

        const auto knob = juce::Rectangle<float>(18.0f, 18.0f).withCentre({area.getRight() - 16.0f, area.getBottom() - 14.0f});
        g.setColour(p.meterBackground);
        g.fillEllipse(knob);
        juce::Path arc;
        arc.addCentredArc(knob.getCentreX(), knob.getCentreY(), 8.0f, 8.0f, 0.0f, -2.4f, 0.9f, true);
        g.setColour(p.accent);
        g.strokePath(arc, juce::PathStrokeType(2.0f));
        g.restoreState();

        g.setColour(selected_ ? theme::colour::accent : (isMouseOver() ? theme::colour::outlineStrong : theme::colour::outline));
        g.drawRoundedRectangle(preview, 6.0f, selected_ ? 2.5f : 1.0f);

        g.setColour(theme::colour::textPrimary);
        g.setFont(theme::sansBold(13.0f));
        g.drawText(theme_.name + (selected_ ? juce::String("   (current)") : juce::String()), caption.removeFromTop(18.0f),
                   juce::Justification::centredLeft, true);
        g.setColour(theme::colour::textMuted);
        g.setFont(theme::sans(11.0f));
        g.drawText(theme_.description, caption, juce::Justification::centredLeft, true);
    }

    void mouseEnter(const juce::MouseEvent&) override { repaint(); }
    void mouseExit(const juce::MouseEvent&) override { repaint(); }
    void mouseUp(const juce::MouseEvent& e) override
    {
        if (e.mouseWasClicked() && onChosen_)
            onChosen_();
    }

    [[nodiscard]] const juce::String& getThemeName() const noexcept { return theme_.name; }

private:
    const theme::Theme& theme_;
    std::function<void()> onChosen_;
    bool selected_ = false;
};

/** Every theme, grouped by family. */
class ThemesPage final : public juce::Component, private juce::ChangeListener
{
public:
    explicit ThemesPage(Preferences& preferences)
        : preferences_(preferences)
    {
        for (const auto& theme : theme::getThemes())
        {
            if (!families_.contains(theme.family))
                families_.add(theme.family);
            auto card = std::make_unique<ThemeCard>(theme, [this, name = theme.name] { preferences_.setThemeName(name); });
            holder_.addAndMakeVisible(*card);
            cards_.push_back(std::move(card));
        }
        holder_.onPaint = [this](juce::Graphics& g) { paintHeadings(g); };
        viewport_.setViewedComponent(&holder_, false);
        viewport_.setScrollBarsShown(true, false);
        addAndMakeVisible(viewport_);
        preferences_.addChangeListener(this);
        update();
    }

    ~ThemesPage() override { preferences_.removeChangeListener(this); }

    void paint(juce::Graphics& g) override { g.fillAll(theme::colour::panel); }

    void resized() override
    {
        viewport_.setBounds(getLocalBounds());
        const int width = viewport_.getMaximumVisibleWidth();
        constexpr int kColumns = 3;
        constexpr int kMargin = 20;
        const int cardWidth = (width - 2 * kMargin) / kColumns;
        const int cardHeight = cardWidth * 2 / 3 + 40;
        int y = 0;
        headings_.clear();
        for (const auto& family : families_)
        {
            headings_.push_back({family, y});
            y += 34;
            int column = 0;
            for (auto& card : cards_)
            {
                if (theme::findTheme(card->getThemeName()).family != family)
                    continue;
                card->setBounds(kMargin + column * cardWidth, y, cardWidth, cardHeight);
                if (++column == kColumns)
                {
                    column = 0;
                    y += cardHeight;
                }
            }
            if (column != 0)
                y += cardHeight;
            y += 8;
        }
        holder_.setSize(width, y + 12);
    }

private:
    /** A component whose painting is delegated (the headings between the cards). */
    struct Holder final : public juce::Component
    {
        std::function<void(juce::Graphics&)> onPaint;
        void paint(juce::Graphics& g) override
        {
            if (onPaint)
                onPaint(g);
        }
    };

    void paintHeadings(juce::Graphics& g)
    {
        g.setColour(theme::colour::textSecondary);
        g.setFont(theme::label(11.0f));
        for (const auto& [family, y] : headings_)
            g.drawText(family.toUpperCase(), 24, y + 10, holder_.getWidth() - 48, 20, juce::Justification::bottomLeft, false);
    }

    void changeListenerCallback(juce::ChangeBroadcaster*) override { update(); }

    void update()
    {
        const auto current = preferences_.getThemeName();
        for (auto& card : cards_)
            card->setSelected(card->getThemeName() == current);
    }

    Preferences& preferences_;
    juce::Viewport viewport_;
    Holder holder_;
    juce::StringArray families_;
    std::vector<std::unique_ptr<ThemeCard>> cards_;
    std::vector<std::pair<juce::String, int>> headings_;
};

//==================================================================================================
/** Audio device settings (the same component as Options > Audio Settings). */
class AudioPage final : public juce::Component
{
public:
    explicit AudioPage(juce::AudioDeviceManager& devices)
        : selector_(devices, 0, 32, 1, 2, true, false, true, false)
    {
        addAndMakeVisible(selector_);
    }

    void paint(juce::Graphics& g) override { g.fillAll(theme::colour::panel); }
    void resized() override { selector_.setBounds(getLocalBounds().reduced(8, 12)); }

private:
    juce::AudioDeviceSelectorComponent selector_;
};

//==================================================================================================
/** Surround monitoring: how a surround master plays on stereo headphones or speakers. */
class SurroundPage final : public juce::Component, private juce::ChangeListener, private juce::Timer
{
public:
    SurroundPage(Preferences& preferences, juce::AudioDeviceManager& devices, std::function<int()> rendering)
        : preferences_(preferences)
        , devices_(devices)
        , rendering_(std::move(rendering))
    {
        const std::array<std::pair<const char*, const char*>, 4> options{{
            {"Standard",
             "Plays the channels as they are, one output per channel, and only shows them on the meters: on a stereo "
             "device you hear just the front left and right."},
            {"Classic Fold-Down",
             "An output per channel when the device has enough, otherwise the ITU-R BS.775 stereo fold-down: centre and "
             "surrounds into left and right at -3 dB, the LFE left out. What The Dojo always did."},
            {"Advanced Surround Virtualizer",
             "Every speaker, the heights of 5.1.4, 7.1.4 and 9.1.6 included, becomes a virtual source around and above "
             "your head: interaural time and level differences of a spherical head model (Brown-Duda) in 3D, pinna notches "
             "that rise with elevation (4-12 kHz), a cue for sources behind, first reflections from the floor, the ceiling "
             "and the walls, and a short reverberation. On speakers, crosstalk cancellation keeps each ear's signal to that ear."},
            {"Device 3D Virtualizer",
             "Sends the surround channels to the device, in its own channel order, so that its built-in virtualizer renders "
             "them: a 7.1 gaming headset, a USB surround card, an AV receiver, or the system's spatial sound (Windows Sonic, "
             "Dolby Atmos for Headphones, DTS Headphone:X). A stereo-only device gets the Advanced Virtualizer instead."},
        }};
        for (std::size_t i = 0; i < options.size(); ++i)
        {
            auto& option = options_[i];
            option.button.setButtonText(options[i].first);
            option.button.setRadioGroupId(4242);
            styleToggle(option.button);
            option.button.onClick = [this, i]
            {
                if (options_[i].button.getToggleState())
                    preferences_.setSurroundMode(static_cast<Preferences::SurroundMode>(i));
            };
            addAndMakeVisible(option.button);
            option.text.setText(options[i].second, juce::dontSendNotification);
            addAndMakeVisible(styleDescription(option.text));
        }

        headphones_.setButtonText("Headphones");
        speakers_.setButtonText("Speakers (crosstalk cancelled, for speakers at +-30 degrees)");
        for (auto* toggle : {&headphones_, &speakers_})
        {
            toggle->setRadioGroupId(4343);
            styleToggle(*toggle);
            addAndMakeVisible(*toggle);
        }
        headphones_.onClick = [this] { preferences_.setVirtualizerSpeakers(false); };
        speakers_.onClick = [this] { preferences_.setVirtualizerSpeakers(true); };

        roomLabel_.setText("Room", juce::dontSendNotification);
        addAndMakeVisible(styleDescription(roomLabel_));
        room_.setSliderStyle(juce::Slider::LinearHorizontal);
        room_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 56, 20);
        room_.setRange(0.0, 100.0, 1.0);
        room_.setTextValueSuffix(" %");
        room_.setTooltip("Reflections and reverberation of the virtual room: 0 % is anechoic (sounds closer to the head)");
        room_.onValueChange = [this] { preferences_.setVirtualizerRoom(room_.getValue() / 100.0); };
        addAndMakeVisible(room_);

        addAndMakeVisible(styleDescription(device_));
        useOutputs_.setButtonText("Use All of Its Outputs");
        useOutputs_.setTooltip("Turns on the device's outputs 1 - 8, so that it receives every surround channel");
        useOutputs_.onClick = [this] { useAllOutputs(); };
        addAndMakeVisible(useOutputs_);
        addAndMakeVisible(styleDescription(status_));

        preferences_.addChangeListener(this);
        devices_.addChangeListener(this);
        update();
        startTimerHz(4);
    }

    ~SurroundPage() override
    {
        devices_.removeChangeListener(this);
        preferences_.removeChangeListener(this);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(theme::colour::panel);
        g.setColour(theme::colour::textSecondary);
        g.setFont(theme::label(11.0f));
        g.drawText("SURROUND MONITORING", getLocalBounds().reduced(24, 0).removeFromTop(40), juce::Justification::bottomLeft, false);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(24, 0).withTrimmedTop(48);
        for (std::size_t i = 0; i < options_.size(); ++i)
        {
            options_[i].button.setBounds(area.removeFromTop(24));
            options_[i].text.setBounds(area.removeFromTop(i == 2 || i == 3 ? 46 : 32).withTrimmedLeft(30));
            if (i == 2)
            {
                auto indent = area.removeFromTop(24).withTrimmedLeft(30);
                headphones_.setBounds(indent.removeFromLeft(130));
                speakers_.setBounds(indent);
                auto row = area.removeFromTop(26).withTrimmedLeft(30);
                roomLabel_.setBounds(row.removeFromLeft(60).withTrimmedTop(4));
                room_.setBounds(row.removeFromLeft(320));
            }
            if (i == 3)
            {
                auto row = area.removeFromTop(48).withTrimmedLeft(30);
                useOutputs_.setBounds(row.removeFromRight(180).withSizeKeepingCentre(180, 26));
                device_.setBounds(row);
            }
            area.removeFromTop(6);
        }
        status_.setBounds(area.removeFromTop(36));
    }

    /** What plays now, in words (also for tests). */
    [[nodiscard]] juce::String describeRendering(int rendering) const
    {
        switch (rendering)
        {
        case 0: return "Now: a stereo master (nothing to virtualise).";
        case 1: return "Now: an output per channel.";
        case 2: return "Now: the front left and right only.";
        case 3: return "Now: the classic stereo fold-down.";
        case 4: return juce::String("Now: the Advanced Virtualizer, for ") + (preferences_.getVirtualizerSpeakers() ? "speakers." : "headphones.");
        case 5: return "Now: the surround channels go to the device's virtualizer.";
        case 6: return "Now: an external renderer's outputs (see the Renderer tab): the bed's channels, then the objects'.";
        default: return {};
        }
    }

private:
    struct Option
    {
        juce::ToggleButton button;
        juce::Label text;
    };

    void changeListenerCallback(juce::ChangeBroadcaster*) override { update(); }
    void timerCallback() override
    {
        const auto text = rendering_ ? describeRendering(rendering_()) : juce::String();
        if (text != status_.getText())
            status_.setText(text, juce::dontSendNotification);
    }

    void update()
    {
        const auto mode = static_cast<std::size_t>(preferences_.getSurroundMode());
        for (std::size_t i = 0; i < options_.size(); ++i)
            options_[i].button.setToggleState(i == mode, juce::dontSendNotification);
        const bool speakers = preferences_.getVirtualizerSpeakers();
        headphones_.setToggleState(!speakers, juce::dontSendNotification);
        speakers_.setToggleState(speakers, juce::dontSendNotification);
        room_.setValue(preferences_.getVirtualizerRoom() * 100.0, juce::dontSendNotification);
        const bool virtualizing = mode == static_cast<std::size_t>(Preferences::SurroundMode::virtualizer)
                                  || mode == static_cast<std::size_t>(Preferences::SurroundMode::device);
        for (juce::Component* c : std::initializer_list<juce::Component*>{&headphones_, &speakers_, &room_, &roomLabel_})
            c->setEnabled(virtualizing);

        // What the device can take.
        juce::String text;
        int available = 0;
        int active = 0;
        if (auto* device = devices_.getCurrentAudioDevice())
        {
            available = device->getOutputChannelNames().size();
            active = device->getActiveOutputChannels().countNumberOfSetBits();
            text << device->getName() << " (" << device->getTypeName() << "): " << active << " of " << available << " outputs on. ";
            if (active >= 6)
                text << "It takes surround channels (" << (active >= 8 ? "7.1" : "5.1") << "): its virtualizer renders them.";
            else if (available >= 6)
                text << "It can take surround channels: turn its outputs on.";
            else
                text << "Stereo only: the Advanced Virtualizer plays instead. Headsets often offer 7.1 only with their surround "
                        "mode (or the system's spatial sound) on.";
        }
        else
            text = "No audio device is open.";
        device_.setText(text, juce::dontSendNotification);
        useOutputs_.setEnabled(available > active && active < 8);
    }

    void useAllOutputs()
    {
        auto setup = devices_.getAudioDeviceSetup();
        auto* device = devices_.getCurrentAudioDevice();
        if (device == nullptr)
            return;
        setup.useDefaultOutputChannels = false;
        setup.outputChannels.clear();
        setup.outputChannels.setRange(0, std::min(8, device->getOutputChannelNames().size()), true);
        const auto error = devices_.setAudioDeviceSetup(setup, true);
        if (error.isNotEmpty())
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Surround outputs", error);
        update();
    }

    Preferences& preferences_;
    juce::AudioDeviceManager& devices_;
    std::function<int()> rendering_;
    std::array<Option, 4> options_;
    juce::ToggleButton headphones_, speakers_;
    juce::Label roomLabel_;
    juce::Slider room_;
    juce::Label device_, status_;
    juce::TextButton useOutputs_;
};

/** Every command's keyboard shortcuts, editable; saved by the main window. */
class KeyboardPage final : public juce::Component
{
public:
    KeyboardPage(juce::ApplicationCommandManager& commands, std::function<void()> showTips)
        : editor_(*commands.getKeyMappings(), true)
        , showTips_(std::move(showTips))
    {
        editor_.setColours(theme::colour::panel, theme::colour::textPrimary);
        addAndMakeVisible(editor_);
        hint_.setText("Click + to add a key to a command, click a key to change or remove it. Changes are kept.",
                      juce::dontSendNotification);
        addAndMakeVisible(styleDescription(hint_));
        tips_.setButtonText("Mouse and editing tips...");
        tips_.onClick = [this]
        {
            if (showTips_)
                showTips_();
        };
        addAndMakeVisible(tips_);
    }

    void paint(juce::Graphics& g) override { g.fillAll(theme::colour::panel); }

    void resized() override
    {
        auto area = getLocalBounds().reduced(12, 10);
        auto bottom = area.removeFromBottom(30);
        tips_.setBounds(bottom.removeFromRight(190).reduced(0, 2));
        hint_.setBounds(bottom);
        area.removeFromBottom(6);
        editor_.setBounds(area);
    }

private:
    juce::KeyMappingEditorComponent editor_;
    juce::Label hint_;
    juce::TextButton tips_;
    std::function<void()> showTips_;
};

//==================================================================================================
/**
    Control surfaces: Mackie Control units (and extenders) and HUI, each on a pair of MIDI ports.
    The order of the rows is the order of the units on the desk (left to right).
*/
class ControlSurfacesPage final : public juce::Component, private juce::ChangeListener, private juce::Timer
{
public:
    ControlSurfacesPage(Preferences& preferences, std::function<juce::String(int)> status)
        : preferences_(preferences)
        , status_(std::move(status))
    {
        intro_.setText("Hardware controllers speaking Mackie Control or HUI over MIDI: motor faders, V-Pots, mute, solo, "
                       "record and select keys, the transport, the jog wheel, the displays and the meters. Put the unit in "
                       "its Mackie Control (or HUI) mode, add it here and pick its MIDI ports. Add extenders in the order "
                       "they stand on the desk: each one shows the next eight tracks.",
                       juce::dontSendNotification);
        eucon_.setText("EuCon (Avid's network protocol for its consoles) is not supported: Avid licenses it only with its "
                       "own software and SDK.",
                       juce::dontSendNotification);
        for (auto* label : {&intro_, &eucon_})
            addAndMakeVisible(styleDescription(*label));
        add_.setComponentID("addSurface");
        add_.onClick = [this]
        {
            auto setups = surfaces::setupsFromString(preferences_.getControlSurfaces());
            surfaces::Setup setup;
            setup.model = setups.empty() ? surfaces::Model::mackieControl : surfaces::Model::mackieExtender;
            const auto inputs = juce::MidiInput::getAvailableDevices();
            const auto outputs = juce::MidiOutput::getAvailableDevices();
            if (!inputs.isEmpty())
                setup.input = inputs[0].identifier;
            if (!outputs.isEmpty())
                setup.output = outputs[0].identifier;
            setups.push_back(setup);
            preferences_.setControlSurfaces(surfaces::toString(setups));
        };
        addAndMakeVisible(add_);
        preferences_.addChangeListener(this);
        rebuild();
        startTimerHz(2);
    }

    ~ControlSurfacesPage() override { preferences_.removeChangeListener(this); }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(theme::colour::panel);
        g.setColour(theme::colour::textSecondary);
        g.setFont(theme::label(11.0f));
        g.drawText("CONTROL SURFACES", getLocalBounds().reduced(24, 0).removeFromTop(48), juce::Justification::bottomLeft, false);
        if (rows_.empty())
        {
            g.setColour(theme::colour::textMuted);
            g.setFont(theme::sans(12.5f));
            g.drawText("No surfaces yet.", listArea_, juce::Justification::topLeft, false);
        }
        else
        {
            g.setFont(theme::label(9.5f));
            g.setColour(theme::colour::textMuted);
            auto header = listArea_.removeFromTop(16);
            header.removeFromLeft(24);
            for (const auto& [text, width] : {std::pair{"MODEL", 180}, std::pair{"MIDI IN", 160}, std::pair{"MIDI OUT", 160}, std::pair{"STATUS", 0}})
                g.drawText(text, width > 0 ? header.removeFromLeft(width + 8) : header, juce::Justification::bottomLeft, false);
        }
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(24, 0).withTrimmedTop(58);
        intro_.setBounds(area.removeFromTop(64));
        area.removeFromTop(8);
        listArea_ = area.removeFromTop(16 + 34 * static_cast<int>(std::max<std::size_t>(rows_.size(), 1)));
        auto rowsArea = listArea_.withTrimmedTop(18);
        for (auto& row : rows_)
        {
            auto r = rowsArea.removeFromTop(34).withTrimmedBottom(6);
            row->number.setBounds(r.removeFromLeft(24));
            row->model.setBounds(r.removeFromLeft(180));
            r.removeFromLeft(8);
            row->input.setBounds(r.removeFromLeft(160));
            r.removeFromLeft(8);
            row->output.setBounds(r.removeFromLeft(160));
            r.removeFromLeft(8);
            row->remove.setBounds(r.removeFromRight(74));
            r.removeFromRight(6);
            row->status.setBounds(r);
        }
        area.removeFromTop(6);
        add_.setBounds(area.removeFromTop(28).removeFromLeft(130));
        area.removeFromTop(20);
        eucon_.setBounds(area.removeFromTop(40));
    }

private:
    struct Row
    {
        juce::Label number, status;
        juce::ComboBox model, input, output;
        juce::TextButton remove{"Remove"};
    };

    void changeListenerCallback(juce::ChangeBroadcaster*) override
    {
        if (preferences_.getControlSurfaces() != shown_)
            rebuild();
    }

    void timerCallback() override
    {
        for (std::size_t i = 0; i < rows_.size(); ++i)
            rows_[i]->status.setText(status_ ? status_(static_cast<int>(i)) : juce::String(), juce::dontSendNotification);
    }

    /** Fills a port menu: the devices there now, and the chosen one if it is not. */
    static void fillPorts(juce::ComboBox& combo, const juce::Array<juce::MidiDeviceInfo>& devices, const juce::String& chosen)
    {
        combo.addItem("None", 1);
        int selected = 1;
        for (int i = 0; i < devices.size(); ++i)
        {
            combo.addItem(devices[i].name, i + 2);
            if (devices[i].identifier == chosen)
                selected = i + 2;
        }
        if (chosen.isNotEmpty() && selected == 1)
        {
            combo.addItem(chosen + " (not connected)", devices.size() + 2);
            selected = devices.size() + 2;
        }
        combo.setSelectedId(selected, juce::dontSendNotification);
    }

    void rebuild()
    {
        shown_ = preferences_.getControlSurfaces();
        const auto setups = surfaces::setupsFromString(shown_);
        const auto inputs = juce::MidiInput::getAvailableDevices();
        const auto outputs = juce::MidiOutput::getAvailableDevices();
        rows_.clear();
        for (std::size_t i = 0; i < setups.size(); ++i)
        {
            auto row = std::make_unique<Row>();
            const auto index = static_cast<int>(i);
            row->number.setText(juce::String(index + 1), juce::dontSendNotification);
            row->number.setFont(theme::mono(12.0f));
            row->number.setColour(juce::Label::textColourId, theme::colour::textMuted);
            for (const auto model : {surfaces::Model::mackieControl, surfaces::Model::mackieExtender, surfaces::Model::hui})
                row->model.addItem(surfaces::modelName(model), static_cast<int>(model) + 1);
            row->model.setSelectedId(static_cast<int>(setups[i].model) + 1, juce::dontSendNotification);
            fillPorts(row->input, inputs, setups[i].input);
            fillPorts(row->output, outputs, setups[i].output);
            row->model.setComponentID("surfaceModel" + juce::String(index));
            row->input.setComponentID("surfaceInput" + juce::String(index));
            row->output.setComponentID("surfaceOutput" + juce::String(index));
            row->status.setComponentID("surfaceStatus" + juce::String(index));
            row->remove.setComponentID("surfaceRemove" + juce::String(index));
            row->status.setFont(theme::sans(11.5f));
            row->status.setColour(juce::Label::textColourId, theme::colour::textSecondary);
            const auto change = [this, index, inputs, outputs]
            {
                auto all = surfaces::setupsFromString(preferences_.getControlSurfaces());
                if (index >= static_cast<int>(all.size()))
                    return;
                auto& current = *rows_[static_cast<std::size_t>(index)];
                auto& setup = all[static_cast<std::size_t>(index)];
                setup.model = static_cast<surfaces::Model>(std::max(0, current.model.getSelectedId() - 1));
                const auto portOf = [](const juce::ComboBox& combo, const juce::Array<juce::MidiDeviceInfo>& devices, const juce::String& previous)
                {
                    const int id = combo.getSelectedId();
                    if (id >= 2 && id - 2 < devices.size())
                        return devices[id - 2].identifier;
                    return id == 1 ? juce::String() : previous;
                };
                setup.input = portOf(current.input, inputs, setup.input);
                setup.output = portOf(current.output, outputs, setup.output);
                preferences_.setControlSurfaces(surfaces::toString(all));
            };
            row->model.onChange = change;
            row->input.onChange = change;
            row->output.onChange = change;
            row->remove.onClick = [this, index]
            {
                auto all = surfaces::setupsFromString(preferences_.getControlSurfaces());
                if (index < static_cast<int>(all.size()))
                    all.erase(all.begin() + index);
                preferences_.setControlSurfaces(surfaces::toString(all));
            };
            for (auto* c : std::initializer_list<juce::Component*>{&row->number, &row->model, &row->input, &row->output, &row->status, &row->remove})
                addAndMakeVisible(c);
            rows_.push_back(std::move(row));
        }
        timerCallback();
        resized();
        repaint();
    }

    Preferences& preferences_;
    std::function<juce::String(int)> status_;
    juce::Label intro_, eucon_;
    juce::TextButton add_{"Add Surface"};
    std::vector<std::unique_ptr<Row>> rows_;
    juce::Rectangle<int> listArea_;
    juce::String shown_;
};
} // namespace

std::unique_ptr<juce::Component> createControlSurfacesPage(Preferences& preferences, std::function<juce::String(int)> status)
{
    return std::make_unique<ControlSurfacesPage>(preferences, std::move(status));
}

//==================================================================================================
/** The Renderer tab: an external renderer given the object tracks (outputs of their own) and their
    positions (ADM-OSC). */
class RendererPage final : public juce::Component, private juce::ChangeListener, private juce::Timer
{
public:
    RendererPage(Preferences& preferences, std::function<juce::String()> status)
        : preferences_(preferences)
        , status_(std::move(status))
    {
        outputs_.setButtonText("Objects on outputs of their own");
        osc_.setButtonText("Send the objects' positions as ADM-OSC");
        for (auto* toggle : {&outputs_, &osc_})
        {
            styleToggle(*toggle);
            addAndMakeVisible(*toggle);
        }
        outputs_.onClick = [this] { apply(); };
        osc_.onClick = [this] { apply(); };
        outputsText_.setText("The bed plays as it is on the first outputs, one per channel, then each object track's channel(s) in the "
                             "session's order (a stereo track has two): for a renderer fed through the sound card, Dante, MADI or a "
                             "virtual audio device. Objects are tracks with Track > ADM Object on; they leave the bed while this is on.",
                             juce::dontSendNotification);
        oscText_.setText("ADM-OSC over UDP as the objects move (/adm/obj/n/aed or /xyz, /w, /gain; all of them with /name once a "
                         "second), numbered as on the outputs. SPAT Revolution, L-ISA, d&b Soundscape and Holophonix read it. The Dolby "
                         "Atmos Renderer takes positions from Dolby's own plug-ins instead: for it, send the bed and objects to its "
                         "inputs, or export an ADM BWF file.",
                         juce::dontSendNotification);
        for (auto* label : {&outputsText_, &oscText_, &hostLabel_, &portLabel_, &rateLabel_, &coordinatesLabel_, &statusLabel_})
            addAndMakeVisible(styleDescription(*label));
        hostLabel_.setText("Host", juce::dontSendNotification);
        portLabel_.setText("Port", juce::dontSendNotification);
        rateLabel_.setText("Rate", juce::dontSendNotification);
        coordinatesLabel_.setText("Coordinates", juce::dontSendNotification);

        for (auto* editor : {&host_, &port_})
        {
            editor->setColour(juce::TextEditor::backgroundColourId, theme::colour::meterBackground);
            editor->setColour(juce::TextEditor::textColourId, theme::colour::textPrimary);
            editor->setColour(juce::TextEditor::outlineColourId, theme::colour::outline);
            editor->onReturnKey = [this] { apply(); };
            editor->onFocusLost = [this] { apply(); };
            addAndMakeVisible(*editor);
        }
        port_.setInputRestrictions(5, "0123456789");
        host_.setTooltip("The renderer's address: 127.0.0.1 for one on this computer");
        for (const int hz : {25, 30, 50, 60, 100})
            rate_.addItem(juce::String(hz) + " times a second", hz);
        rate_.onChange = [this] { apply(); };
        coordinates_.addItem("Polar (azimuth, elevation, distance)", 1);
        coordinates_.addItem("Cartesian (x, y, z: ITU-R BS.2127)", 2);
        coordinates_.onChange = [this] { apply(); };
        addAndMakeVisible(rate_);
        addAndMakeVisible(coordinates_);

        preferences_.addChangeListener(this);
        update();
        startTimerHz(4);
    }

    ~RendererPage() override { preferences_.removeChangeListener(this); }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(theme::colour::panel);
        g.setColour(theme::colour::textSecondary);
        g.setFont(theme::label(11.0f));
        g.drawText("EXTERNAL RENDERER", getLocalBounds().reduced(24, 0).removeFromTop(40), juce::Justification::bottomLeft, false);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(24, 0).withTrimmedTop(48);
        outputs_.setBounds(area.removeFromTop(24));
        outputsText_.setBounds(area.removeFromTop(48).withTrimmedLeft(30));
        area.removeFromTop(10);
        osc_.setBounds(area.removeFromTop(24));
        oscText_.setBounds(area.removeFromTop(48).withTrimmedLeft(30));
        area.removeFromTop(4);
        const auto row = [&area](juce::Label& label, juce::Component& control, int width)
        {
            auto r = area.removeFromTop(26).withTrimmedLeft(30);
            label.setBounds(r.removeFromLeft(90));
            control.setBounds(r.removeFromLeft(width));
            area.removeFromTop(6);
        };
        row(hostLabel_, host_, 220);
        row(portLabel_, port_, 90);
        row(rateLabel_, rate_, 220);
        row(coordinatesLabel_, coordinates_, 300);
        area.removeFromTop(8);
        statusLabel_.setBounds(area.removeFromTop(40));
    }

    juce::ToggleButton outputs_, osc_;
    juce::TextEditor host_, port_;
    juce::ComboBox rate_, coordinates_;
    juce::Label statusLabel_;

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override { update(); }
    void timerCallback() override
    {
        const auto text = status_ ? status_() : juce::String();
        if (text != statusLabel_.getText())
            statusLabel_.setText(text, juce::dontSendNotification);
    }

    void update()
    {
        const auto r = preferences_.getExternalRenderer();
        outputs_.setToggleState(r.objectOutputs, juce::dontSendNotification);
        osc_.setToggleState(r.osc, juce::dontSendNotification);
        if (!host_.hasKeyboardFocus(true))
            host_.setText(r.host, juce::dontSendNotification);
        if (!port_.hasKeyboardFocus(true))
            port_.setText(juce::String(r.port), juce::dontSendNotification);
        rate_.setSelectedId(r.rate, juce::dontSendNotification);
        if (rate_.getSelectedId() == 0)
            rate_.setText(juce::String(r.rate) + " times a second", juce::dontSendNotification);
        coordinates_.setSelectedId(r.cartesian ? 2 : 1, juce::dontSendNotification);
        for (juce::Component* c : std::initializer_list<juce::Component*>{&host_, &port_, &rate_, &coordinates_})
            c->setEnabled(r.osc);
    }

    void apply()
    {
        auto r = preferences_.getExternalRenderer();
        r.objectOutputs = outputs_.getToggleState();
        r.osc = osc_.getToggleState();
        r.host = host_.getText();
        r.port = port_.getText().getIntValue();
        if (rate_.getSelectedId() > 0)
            r.rate = rate_.getSelectedId();
        r.cartesian = coordinates_.getSelectedId() == 2;
        preferences_.setExternalRenderer(r);
        update();
    }

    Preferences& preferences_;
    std::function<juce::String()> status_;
    juce::Label outputsText_, oscText_, hostLabel_, portLabel_, rateLabel_, coordinatesLabel_;
};

std::unique_ptr<juce::Component> createRendererPage(Preferences& preferences, std::function<juce::String()> status)
{
    return std::make_unique<RendererPage>(preferences, std::move(status));
}

//==================================================================================================
class PreferencesWindow::Content final : public juce::Component
{
public:
    Content(Preferences& preferences, juce::AudioDeviceManager& devices, juce::ApplicationCommandManager& commands,
            std::function<void()> showTips, std::function<int()> surroundRendering, std::function<juce::String(int)> surfaceStatus,
            std::function<juce::String()> rendererStatus)
    {
        using namespace theme;
        tabs_.setTabBarDepth(34);
        tabs_.setOutline(0);
        tabs_.setColour(juce::TabbedComponent::backgroundColourId, colour::panel);
        tabs_.setColour(juce::TabbedComponent::outlineColourId, colour::outline);
        auto& bar = tabs_.getTabbedButtonBar();
        bar.setColour(juce::TabbedButtonBar::tabOutlineColourId, colour::outline);
        bar.setColour(juce::TabbedButtonBar::tabTextColourId, colour::textSecondary);
        bar.setColour(juce::TabbedButtonBar::frontOutlineColourId, colour::accent);
        bar.setColour(juce::TabbedButtonBar::frontTextColourId, colour::textPrimary);
        tabs_.addTab("General", colour::panelHeader, new GeneralPage(preferences), true);
        tabs_.addTab("Themes", colour::panelHeader, new ThemesPage(preferences), true);
        tabs_.addTab("Audio", colour::panelHeader, new AudioPage(devices), true);
        tabs_.addTab("Surround", colour::panelHeader, new SurroundPage(preferences, devices, std::move(surroundRendering)), true);
        tabs_.addTab("Keyboard", colour::panelHeader, new KeyboardPage(commands, std::move(showTips)), true);
        tabs_.addTab("Control Surfaces", colour::panelHeader, new ControlSurfacesPage(preferences, std::move(surfaceStatus)), true);
        tabs_.addTab("Renderer", colour::panelHeader, new RendererPage(preferences, std::move(rendererStatus)), true);
        addAndMakeVisible(tabs_);
        setSize(kWidth, kHeight);
    }

    void paint(juce::Graphics& g) override { g.fillAll(theme::colour::panel); }
    void resized() override { tabs_.setBounds(getLocalBounds()); }

    juce::TabbedComponent tabs_{juce::TabbedButtonBar::TabsAtTop};
};

PreferencesWindow::PreferencesWindow(Preferences& preferences, juce::AudioDeviceManager& devices, juce::ApplicationCommandManager& commands,
                                     std::function<void()> showEditingTips, std::function<int()> surroundRendering,
                                     std::function<juce::String(int)> surfaceStatus, std::function<juce::String()> rendererStatus)
    : juce::DocumentWindow("Preferences", theme::colour::panel, juce::DocumentWindow::closeButton)
    , preferences_(preferences)
    , devices_(devices)
    , commands_(commands)
    , showEditingTips_(std::move(showEditingTips))
    , surroundRendering_(std::move(surroundRendering))
    , surfaceStatus_(std::move(surfaceStatus))
    , rendererStatus_(std::move(rendererStatus))
{
    setUsingNativeTitleBar(false);
    setTitleBarHeight(30);
    setResizable(true, false);
    setResizeLimits(640, 480, 4096, 4096);
    refreshTheme();
    centreWithSize(kWidth, kHeight);
}

PreferencesWindow::~PreferencesWindow() = default;

void PreferencesWindow::showTab(Preferences::Tab tab)
{
    if (content_ != nullptr)
        content_->tabs_.setCurrentTabIndex(static_cast<int>(tab));
    setVisible(true);
    toFront(true);
}

Preferences::Tab PreferencesWindow::getCurrentTab() const
{
    return content_ != nullptr ? static_cast<Preferences::Tab>(std::max(0, content_->tabs_.getCurrentTabIndex())) : Preferences::Tab::general;
}

void PreferencesWindow::refreshTheme()
{
    const auto tab = getCurrentTab();
    setBackgroundColour(theme::colour::panel);
    auto content = std::make_unique<Content>(preferences_, devices_, commands_, showEditingTips_, surroundRendering_, surfaceStatus_,
                                             rendererStatus_);
    content_ = content.get();
    setContentOwned(content.release(), false);
    content_->tabs_.setCurrentTabIndex(static_cast<int>(tab));
    repaint();
}

} // namespace dojo::ui
