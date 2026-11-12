#include "app/MainWindow.h"

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
    services_.preferences.removeChangeListener(this);
    services_.commands.getKeyMappings()->removeChangeListener(this);
    preferencesWindow_.reset();
    project_.onDocumentStateChanged = nullptr;
    setMenuBar(nullptr);
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
