#include "app/MainWindow.h"

#include "core/Version.h"
#include "ui/theme/DojoTheme.h"

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
    project_.onDocumentStateChanged = nullptr;
    setMenuBar(nullptr);
}

void MainWindow::updateTitle()
{
    setName(project_.getDisplayName() + (project_.hasUnsavedChanges() ? "*" : "") + " - " +
            version::kProductName);
}

void MainWindow::closeButtonPressed()
{
    juce::JUCEApplication::getInstance()->systemRequestedQuit();
}

} // namespace dojo::app
