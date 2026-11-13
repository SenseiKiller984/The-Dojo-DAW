#include "app/MainWindow.h"

#include "app/WindowManager.h"

#include <optional>

#include "core/Version.h"
#include "ui/theme/DojoLookAndFeel.h"
#include "ui/theme/DojoTheme.h"
#include "ui/theme/Themes.h"

namespace dojo::app
{
namespace
{
constexpr int kDefaultWidth = 1440;
constexpr int kDefaultHeight = 900;
constexpr int kMinWidth = 1180;
constexpr int kMinHeight = 680;
} // namespace

MainWindow::MainWindow(ui::AppServices& services)
    : juce::DocumentWindow(version::kProductName, ui::theme::colour::windowBackground,
                           juce::DocumentWindow::allButtons)
    , services_(services)
    , project_(services.project)
    , menuModel_(services.commands)
{
    setUsingNativeTitleBar(false);
    setTitleBarHeight(30);
    setResizable(true, false);
    setResizeLimits(kMinWidth, kMinHeight, 16384, 16384);

    auto& commandManager = services.commands;
    auto component = std::make_unique<ui::MainComponent>(services);
    mainComponent_ = component.get();
    setContentOwned(component.release(), false);

    // Commands and their default shortcuts.
    commandManager.registerAllCommandsForTarget(mainComponent_);
    commandManager.setFirstCommandTarget(mainComponent_);
    addKeyListener(commandManager.getKeyMappings());
    setMenuBar(&menuModel_, 26);

    // Customised shortcuts are kept between sessions.
    if (const auto mappings = services.preferences.getKeyMappings())
        commandManager.getKeyMappings()->restoreFromXml(*mappings);
    commandManager.getKeyMappings()->addChangeListener(this);
    services.preferences.addChangeListener(this);
    services.showPreferences = [this](ui::Preferences::Tab tab) { showPreferences(tab); };
    services.toggleFullScreen = [this] { setFullScreenMode(!fullScreenMode_); };
    services.isFullScreen = [this] { return fullScreenMode_; };

    // Fit the default size onto the current display (minus a margin for task bars / docks).
    const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
    const auto userArea = display != nullptr ? display->userBounds.toNearestInt()
                                             : juce::Rectangle<int>(0, 0, kDefaultWidth, kDefaultHeight);
    const int width = juce::jlimit(kMinWidth, juce::jmax(kMinWidth, userArea.getWidth() - 40), kDefaultWidth);
    const int height =
        juce::jlimit(kMinHeight, juce::jmax(kMinHeight, userArea.getHeight() - 40), kDefaultHeight);
    centreWithSize(width, height);

    project_.onDocumentStateChanged = [this]
    {
        updateTitle();
    };
    updateTitle();

    setVisible(true);
    mainComponent_->grabKeyboardFocus();
}

MainWindow::~MainWindow()
{
    services_.showPreferences = nullptr;
    services_.toggleFullScreen = nullptr;
    services_.isFullScreen = nullptr;
    services_.preferences.removeChangeListener(this);
    services_.commands.getKeyMappings()->removeChangeListener(this);
    preferencesWindow_.reset();
    project_.onDocumentStateChanged = nullptr;
    setMenuBar(nullptr);
}

juce::Rectangle<int> MainWindow::getScreenArea(bool whole) const
{
    const auto& displays = juce::Desktop::getInstance().getDisplays();
    const auto* display = displays.getDisplayForRect(getScreenBounds());
    if (display == nullptr)
        display = displays.getPrimaryDisplay();
    if (display == nullptr)
        return getBounds();
    return (whole ? display->logicalBounds : display->userBounds).toNearestInt();
}

void MainWindow::setOwnBounds(juce::Rectangle<int> bounds)
{
    const juce::ScopedValueSetter<bool> setting(settingBounds_, true);
    // (a screen smaller than the usual minimum size: the maximised window still fits it)
    if (bounds.getWidth() < kMinWidth || bounds.getHeight() < kMinHeight)
        setResizeLimits(std::min(kMinWidth, bounds.getWidth()), std::min(kMinHeight, bounds.getHeight()), 16384,
                        16384);
    zoomedBounds_ = bounds;
    setBounds(bounds);
}

juce::Rectangle<int> MainWindow::defaultBounds() const
{
    const auto area = getScreenArea(false);
    const int width = juce::jlimit(kMinWidth, juce::jmax(kMinWidth, area.getWidth() - 40), kDefaultWidth);
    const int height = juce::jlimit(kMinHeight, juce::jmax(kMinHeight, area.getHeight() - 40), kDefaultHeight);
    return juce::Rectangle<int>(width, height).withCentre(area.getCentre());
}

juce::Rectangle<int> MainWindow::sensibleRestoreBounds(juce::Rectangle<int> bounds) const
{
    // Restoring to something that covers the screen would look like nothing happened.
    const auto area = getScreenArea(false);
    if (bounds.isEmpty() || (bounds.getWidth() >= area.getWidth() - 8 && bounds.getHeight() >= area.getHeight() - 8))
        return defaultBounds();
    return bounds;
}

void MainWindow::applyRestoreBounds()
{
    const juce::ScopedValueSetter<bool> setting(settingBounds_, true);
    setBounds(sensibleRestoreBounds(restoreBounds_));
}

void MainWindow::leaveWindowManagerZoom()
{
    // Ask the window manager to let go first: while it holds the window maximised (or full
    // screen) it ignores the window's own size. Then set the size, and once more a moment later
    // for window managers that put back their own idea of it.
    ignoreManagerUntil_ = juce::Time::getMillisecondCounter() + 800;
    windowmanager::requestFullScreen(getPeer(), false);
    windowmanager::requestMaximised(getPeer(), false);
    applyRestoreBounds();
    juce::Component::SafePointer<MainWindow> safe(this);
    juce::Timer::callAfterDelay(250,
                                [safe]
                                {
                                    if (safe == nullptr || safe->maximised_ || safe->fullScreenMode_)
                                        return;
                                    const auto area = safe->getScreenArea(false);
                                    if (safe->getWidth() >= area.getWidth() - 8 && safe->getHeight() >= area.getHeight() - 8)
                                    {
                                        safe->ignoreManagerUntil_ = juce::Time::getMillisecondCounter() + 800;
                                        windowmanager::requestMaximised(safe->getPeer(), false);
                                        safe->applyRestoreBounds();
                                    }
                                });
}

void MainWindow::maximiseButtonPressed()
{
    if (fullScreenMode_)
        setFullScreenMode(false);
    else
        setMaximised(!(maximised_ || windowmanager::isMaximised(getPeer())));
}

void MainWindow::setMaximised(bool shouldBeMaximised)
{
    if (fullScreenMode_)
        setFullScreenMode(false);
    if (shouldBeMaximised)
    {
        if (!maximised_)
            restoreBounds_ = sensibleRestoreBounds(getBounds());
        maximised_ = true;
        windowmanager::requestMaximised(getPeer(), true);
        setOwnBounds(getScreenArea(false));
    }
    else
    {
        if (!maximised_ && !windowmanager::isMaximised(getPeer()))
            return;
        maximised_ = false;
        leaveWindowManagerZoom();
    }
    resized();
}

void MainWindow::setFullScreenMode(bool shouldBeFullScreen)
{
    if (shouldBeFullScreen == fullScreenMode_)
        return;
    if (shouldBeFullScreen)
    {
        maximisedBeforeFullScreen_ = maximised_ || windowmanager::isMaximised(getPeer());
        if (!maximisedBeforeFullScreen_)
            restoreBounds_ = sensibleRestoreBounds(getBounds());
        maximised_ = false;
        fullScreenMode_ = true;
        setTitleBarHeight(0);
        windowmanager::requestFullScreen(getPeer(), true);
        setOwnBounds(getScreenArea(true));
    }
    else
    {
        fullScreenMode_ = false;
        setTitleBarHeight(30);
        if (maximisedBeforeFullScreen_)
        {
            maximised_ = true;
            windowmanager::requestFullScreen(getPeer(), false);
            windowmanager::requestMaximised(getPeer(), true);
            setOwnBounds(getScreenArea(false));
        }
        else
        {
            leaveWindowManagerZoom();
        }
    }
    services_.commands.commandStatusChanged();
    resized();
}

void MainWindow::checkUserGeometry()
{
    if (settingBounds_ || fullScreenMode_)
        return;
    // The window manager maximised the window by itself (its placement policy, a shortcut, a
    // double-click it handled): take it as maximised, so that the square button restores it.
    if (juce::Time::getMillisecondCounter() >= ignoreManagerUntil_ && windowmanager::isMaximised(getPeer()))
    {
        if (!maximised_)
        {
            maximised_ = true;
            restoreBounds_ = sensibleRestoreBounds(restoreBounds_);
        }
        zoomedBounds_ = getBounds();
        return;
    }
    if (maximised_ && getBounds() != zoomedBounds_ && !windowmanager::isMaximised(getPeer()))
        maximised_ = false; // (dragged or resized by the user: an ordinary window now)
}

void MainWindow::resized()
{
    juce::DocumentWindow::resized();
    checkUserGeometry();
    // The square shows "restore" while maximised (or full screen).
    if (auto* button = getMaximiseButton())
        button->setToggleState(maximised_ || fullScreenMode_, juce::dontSendNotification);
}

void MainWindow::moved()
{
    juce::DocumentWindow::moved();
    checkUserGeometry();
    if (auto* button = getMaximiseButton())
        button->setToggleState(maximised_ || fullScreenMode_, juce::dontSendNotification);
}

void MainWindow::updateTitle()
{
    setName(project_.getDisplayName() + (project_.hasUnsavedChanges() ? "*" : "") + " - " +
            version::kProductName);
}

void MainWindow::showPreferences(ui::Preferences::Tab tab)
{
    if (preferencesWindow_ == nullptr)
        preferencesWindow_ = std::make_unique<ui::PreferencesWindow>(
            services_.preferences, services_.controller.getDeviceHost().getDeviceManager(), services_.commands,
            [this]
            {
                if (mainComponent_ != nullptr)
                    mainComponent_->showShortcutHelp();
            },
            [this] { return static_cast<int>(services_.controller.getLastTelemetry().surroundRendering); });
    preferencesWindow_->showTab(tab);
}

void MainWindow::applyTheme()
{
    ui::theme::applyTheme(ui::theme::findTheme(services_.preferences.getThemeName()));
    if (auto* dojoLookAndFeel = dynamic_cast<ui::DojoLookAndFeel*>(&juce::LookAndFeel::getDefaultLookAndFeel()))
        dojoLookAndFeel->refreshColours();
    setBackgroundColour(ui::theme::colour::windowBackground);
    rebuildMainComponent();
    if (preferencesWindow_ != nullptr)
        preferencesWindow_->refreshTheme();

    auto& desktop = juce::Desktop::getInstance();
    for (int i = 0; i < desktop.getNumComponents(); ++i)
        if (auto* component = desktop.getComponent(i))
        {
            component->sendLookAndFeelChange();
            component->repaint();
        }
}

void MainWindow::rebuildMainComponent()
{
    // The old component goes first: its destructor unhooks the callbacks the new one installs.
    auto& commandManager = services_.commands;
    const auto view = mainComponent_ != nullptr ? std::optional(mainComponent_->getViewState()) : std::nullopt;
    commandManager.setFirstCommandTarget(nullptr);
    clearContentComponent();
    mainComponent_ = nullptr;

    auto component = std::make_unique<ui::MainComponent>(services_);
    mainComponent_ = component.get();
    setContentOwned(component.release(), false);
    if (view.has_value())
        mainComponent_->setViewState(*view);
    commandManager.registerAllCommandsForTarget(mainComponent_);
    commandManager.setFirstCommandTarget(mainComponent_);
    menuModel_.menuItemsChanged();
    mainComponent_->grabKeyboardFocus();
}

void MainWindow::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (source == services_.commands.getKeyMappings())
    {
        if (const auto mappings = services_.commands.getKeyMappings()->createXml(true))
            services_.preferences.setKeyMappings(*mappings);
        return;
    }

    if (source == &services_.preferences && services_.preferences.getThemeName() != ui::theme::getActiveThemeName())
        applyTheme();
}

void MainWindow::closeButtonPressed()
{
    juce::JUCEApplication::getInstance()->systemRequestedQuit();
}

} // namespace dojo::app
