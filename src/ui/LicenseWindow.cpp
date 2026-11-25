#include "ui/LicenseWindow.h"

#include "ui/theme/DojoTheme.h"

#include "licensing/Features.h"

namespace dojo::ui
{
namespace
{
constexpr int kPanelWidth = 620;
constexpr int kPanelHeight = 380;
} // namespace

//==================================================================================================
// LicensePanel

LicensePanel::LicensePanel(licensing::LicenseManager& license, bool dismissable)
    : license_(license)
    , dismissable_(dismissable)
{
    using namespace theme;
    title_.setText("The Dojo", juce::dontSendNotification);
    title_.setFont(sansBold(24.0f));
    title_.setColour(juce::Label::textColourId, colour::textPrimary);
    addAndMakeVisible(title_);

    status_.setFont(sans(13.5f));
    status_.setColour(juce::Label::textColourId, colour::textSecondary);
    status_.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(status_);

    prompt_.setText("License key", juce::dontSendNotification);
    prompt_.setFont(label(11.0f));
    prompt_.setColour(juce::Label::textColourId, colour::textMuted);
    addAndMakeVisible(prompt_);

    key_.setMultiLine(true, true);
    key_.setReturnKeyStartsNewLine(false);
    key_.setFont(mono(13.0f));
    key_.setTextToShowWhenEmpty("DOJO1-XXXXXXXX-XXXXXXXX-...  (paste the whole key)", colour::textMuted);
    key_.onReturnKey = [this] { activate(); };
    addAndMakeVisible(key_);

    message_.setFont(sans(12.5f));
    message_.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(message_);

    activate_.setButtonText("Activate");
    activate_.onClick = [this] { activate(); };
    addAndMakeVisible(activate_);

    trial_.setButtonText("Try The Dojo for free!");
    trial_.setTooltip("Opens the page with the free-trial key (7 days, once per computer)");
    trial_.onClick = [this] { openFreeTrialPage(); };
    addAndMakeVisible(trial_);

    close_.setButtonText("Close");
    close_.onClick = [this]
    {
        if (onClose)
            onClose();
    };
    addChildComponent(close_);

    remove_.setButtonText("Remove License Key");
    remove_.setTooltip("Take the license key off this computer (to move it to another one). The Dojo locks until a key is entered.");
    remove_.onClick = [this]
    {
        juce::Component::SafePointer<LicensePanel> safe(this);
        juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::WarningIcon, "Remove License Key",
                                           "Remove the license key from this computer? The Dojo locks until a key is entered again.",
                                           "Remove", "Cancel", nullptr,
                                           juce::ModalCallbackFunction::create([safe](int result)
                                                                               {
                                                                                   if (safe != nullptr && result == 1)
                                                                                       safe->license_.removeKey();
                                                                               }));
    };
    addChildComponent(remove_);

    openUrl = [](const juce::String& url) { juce::URL(url).launchInDefaultBrowser(); };
    license_.addChangeListener(this);
    update();
    setSize(kPanelWidth, kPanelHeight);
}

LicensePanel::~LicensePanel()
{
    license_.removeChangeListener(this);
}

void LicensePanel::update()
{
    if (licensing::gate::isTamperLocked())
    {
        status_.setText("This copy of The Dojo has been modified or is damaged. Please reinstall it from the official download.",
                        juce::dontSendNotification);
        key_.setVisible(false);
        prompt_.setVisible(false);
        activate_.setVisible(false);
        trial_.setVisible(false);
        remove_.setVisible(false);
        close_.setVisible(dismissable_);
        resized();
        repaint();
        return;
    }
    const auto& status = license_.getStatus();
    juce::String text;
    switch (status.tier)
    {
    case licensing::Tier::none:
        text = status.message;
        break;
    case licensing::Tier::demo:
        text = "Free trial: " + juce::String(license_.getTrialDaysLeft()) + " day" + (license_.getTrialDaysLeft() == 1 ? "" : "s")
               + " left. Exporting is off. Enter a Basic or Pro key to keep everything.";
        break;
    case licensing::Tier::basic:
        text = "Basic license (serial " + juce::String(status.serial)
               + "): everything but surround, Dolby Atmos and the pro devices. Enter a Pro key to upgrade.";
        break;
    case licensing::Tier::pro:
        text = "Pro license (serial " + juce::String(status.serial) + "): everything. Thank you!";
        break;
    }
    status_.setText(text, juce::dontSendNotification);
    close_.setVisible(dismissable_);
    remove_.setVisible(dismissable_ && status.tier != licensing::Tier::none);
    trial_.setVisible(status.tier == licensing::Tier::none);
    resized();
    repaint();
}

void LicensePanel::activate()
{
    const auto result = license_.activate(key_.getText());
    update();
    message_.setText(result.message, juce::dontSendNotification);
    message_.setColour(juce::Label::textColourId, result.ok ? theme::colour::play : theme::colour::meterClip);
    if (result.ok)
    {
        key_.clear();
        if (onActivated)
            onActivated();
    }
}

void LicensePanel::openFreeTrialPage()
{
    if (openUrl)
        openUrl(kFreeTrialUrl);
}

void LicensePanel::paint(juce::Graphics& g)
{
    using namespace theme;
    g.setColour(colour::panel);
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 8.0f);
    g.setColour(colour::accent);
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 8.0f, 1.5f);
}

void LicensePanel::resized()
{
    auto area = getLocalBounds().reduced(28, 22);
    title_.setBounds(area.removeFromTop(36));
    status_.setBounds(area.removeFromTop(54));
    area.removeFromTop(6);
    prompt_.setBounds(area.removeFromTop(18));
    key_.setBounds(area.removeFromTop(64));
    area.removeFromTop(8);
    auto buttons = area.removeFromTop(34);
    activate_.setBounds(buttons.removeFromLeft(130));
    buttons.removeFromLeft(10);
    if (trial_.isVisible())
        trial_.setBounds(buttons.removeFromLeft(200));
    if (close_.isVisible())
        close_.setBounds(buttons.removeFromRight(100));
    if (remove_.isVisible())
    {
        buttons.removeFromRight(10);
        remove_.setBounds(buttons.removeFromRight(170));
    }
    area.removeFromTop(10);
    message_.setBounds(area);
}

//==================================================================================================
// LicenseLock

LicenseLock::LicenseLock(licensing::LicenseManager& license, juce::DocumentWindow& window)
    : window_(window)
    , panel_(license, false)
{
    setWantsKeyboardFocus(false);
    addAndMakeVisible(panel_);
}

LicenseLock::~LicenseLock() = default;

void LicenseLock::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::black.withAlpha(0.62f)); // the DAW behind, dimmed
}

void LicenseLock::resized()
{
    panel_.setBounds(getLocalBounds().withSizeKeepingCentre(std::min(kPanelWidth, getWidth() - 40), std::min(kPanelHeight, getHeight() - 40)));
}

bool LicenseLock::canModalEventBeSentToComponent(const juce::Component* target)
{
    // The window's close and maximise buttons, and its title bar (move it, double-click to maximise).
    return target == &window_ || target == window_.getCloseButton() || target == window_.getMaximiseButton();
}

} // namespace dojo::ui
