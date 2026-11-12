#include "ui/PreferencesWindow.h"

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
/** Playback options. */
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

        for (auto* toggle : {&returnView_, &stopAtEnd_})
        {
            styleToggle(*toggle);
            addAndMakeVisible(*toggle);
        }
        for (auto* label : {&returnViewText_, &stopAtEndText_})
            addAndMakeVisible(styleDescription(*label));

        returnView_.onClick = [this] { preferences_.setReturnViewToStart(returnView_.getToggleState()); };
        stopAtEnd_.onClick = [this] { preferences_.setStopAtEnd(stopAtEnd_.getToggleState()); };
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
    }

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override { update(); }

    void update()
    {
        returnView_.setToggleState(preferences_.getReturnViewToStart(), juce::dontSendNotification);
        stopAtEnd_.setToggleState(preferences_.getStopAtEnd(), juce::dontSendNotification);
    }

    Preferences& preferences_;
    juce::ToggleButton returnView_, stopAtEnd_;
    juce::Label returnViewText_, stopAtEndText_;
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
             "Every speaker becomes a virtual source around your head: interaural time and level differences of a "
             "spherical head model (Brown-Duda), a pinna cue for sources behind, first reflections of a room and a short "
             "reverberation. On speakers, crosstalk cancellation keeps each ear's signal to that ear."},
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
            options_[i].text.setBounds(area.removeFromTop(i == 3 ? 46 : 32).withTrimmedLeft(30));
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
} // namespace

//==================================================================================================
class PreferencesWindow::Content final : public juce::Component
{
public:
    Content(Preferences& preferences, juce::AudioDeviceManager& devices, juce::ApplicationCommandManager& commands,
            std::function<void()> showTips, std::function<int()> surroundRendering)
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
        addAndMakeVisible(tabs_);
        setSize(kWidth, kHeight);
    }

    void paint(juce::Graphics& g) override { g.fillAll(theme::colour::panel); }
    void resized() override { tabs_.setBounds(getLocalBounds()); }

    juce::TabbedComponent tabs_{juce::TabbedButtonBar::TabsAtTop};
};

PreferencesWindow::PreferencesWindow(Preferences& preferences, juce::AudioDeviceManager& devices, juce::ApplicationCommandManager& commands,
                                     std::function<void()> showEditingTips, std::function<int()> surroundRendering)
    : juce::DocumentWindow("Preferences", theme::colour::panel, juce::DocumentWindow::closeButton)
    , preferences_(preferences)
    , devices_(devices)
    , commands_(commands)
    , showEditingTips_(std::move(showEditingTips))
    , surroundRendering_(std::move(surroundRendering))
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
    auto content = std::make_unique<Content>(preferences_, devices_, commands_, showEditingTips_, surroundRendering_);
    content_ = content.get();
    setContentOwned(content.release(), false);
    content_->tabs_.setCurrentTabIndex(static_cast<int>(tab));
    repaint();
}

} // namespace dojo::ui
