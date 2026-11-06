#include "ui/PreferencesWindow.h"

#include "ui/theme/DojoTheme.h"
#include "ui/theme/Themes.h"

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
            std::function<void()> showTips)
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
        tabs_.addTab("Keyboard", colour::panelHeader, new KeyboardPage(commands, std::move(showTips)), true);
        addAndMakeVisible(tabs_);
        setSize(kWidth, kHeight);
    }

    void paint(juce::Graphics& g) override { g.fillAll(theme::colour::panel); }
    void resized() override { tabs_.setBounds(getLocalBounds()); }

    juce::TabbedComponent tabs_{juce::TabbedButtonBar::TabsAtTop};
};

PreferencesWindow::PreferencesWindow(Preferences& preferences, juce::AudioDeviceManager& devices, juce::ApplicationCommandManager& commands,
                                     std::function<void()> showEditingTips)
    : juce::DocumentWindow("Preferences", theme::colour::panel, juce::DocumentWindow::closeButton)
    , preferences_(preferences)
    , devices_(devices)
    , commands_(commands)
    , showEditingTips_(std::move(showEditingTips))
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
    auto content = std::make_unique<Content>(preferences_, devices_, commands_, showEditingTips_);
    content_ = content.get();
    setContentOwned(content.release(), false);
    content_->tabs_.setCurrentTabIndex(static_cast<int>(tab));
    repaint();
}

} // namespace dojo::ui
