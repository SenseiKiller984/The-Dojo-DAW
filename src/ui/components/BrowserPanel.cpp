#include "ui/components/BrowserPanel.h"

#include "ui/components/ArrangerView.h"
#include "ui/theme/DojoTheme.h"

namespace dojo::ui
{
namespace
{
constexpr int kHeaderHeight = 34;
constexpr int kTabHeight = 28;
constexpr int kChooseFolderId = 1000;
} // namespace

//==================================================================================================
/** A list of devices or plug-ins: drag source, double-click to add. */
class BrowserPanel::ChoiceList final : public juce::Component, private juce::ListBoxModel
{
public:
    explicit ChoiceList(std::function<void(const deviceActions::DeviceChoice&)>& onActivated)
        : onActivated_(onActivated)
    {
        list_.setModel(this);
        list_.setRowHeight(24);
        list_.setColour(juce::ListBox::backgroundColourId, theme::colour::panel);
        addAndMakeVisible(list_);
    }

    void setChoices(std::vector<deviceActions::DeviceChoice> choices)
    {
        choices_ = std::move(choices);
        std::stable_sort(choices_.begin(), choices_.end(), [](const auto& a, const auto& b) { return a.category < b.category; });
        list_.updateContent();
        list_.repaint();
    }

    [[nodiscard]] bool isEmpty() const noexcept { return choices_.empty(); }
    void resized() override { list_.setBounds(getLocalBounds()); }

private:
    int getNumRows() override { return static_cast<int>(choices_.size()); }

    void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override
    {
        if (row < 0 || row >= getNumRows())
            return;
        const auto& choice = choices_[static_cast<std::size_t>(row)];
        if (selected)
            g.fillAll(theme::colour::accent.withAlpha(0.3f));
        g.setColour(choice.instrument ? theme::colour::gold : theme::colour::play);
        g.fillEllipse(10.0f, static_cast<float>(height) * 0.5f - 3.0f, 6.0f, 6.0f);
        g.setColour(theme::colour::textPrimary);
        g.setFont(theme::sans(12.5f));
        g.drawText(choice.name, 24, 0, width - 28, height, juce::Justification::centredLeft, true);
        g.setColour(theme::colour::textMuted);
        g.setFont(theme::sans(10.5f));
        g.drawText(choice.category, 24, 0, width - 30, height, juce::Justification::centredRight, true);
    }

    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override
    {
        if (row >= 0 && row < getNumRows() && onActivated_)
            onActivated_(choices_[static_cast<std::size_t>(row)]);
    }

    juce::var getDragSourceDescription(const juce::SparseSet<int>& rows) override
    {
        if (rows.isEmpty() || rows[0] >= getNumRows())
            return {};
        return deviceActions::toDragDescription(choices_[static_cast<std::size_t>(rows[0])]);
    }

    juce::String getTooltipForRow(int row) override
    {
        if (row < 0 || row >= getNumRows())
            return {};
        const auto& choice = choices_[static_cast<std::size_t>(row)];
        return choice.name + (choice.instrument ? " (instrument)" : " (effect)") + " - drag onto a track, or double-click to add";
    }

    std::function<void(const deviceActions::DeviceChoice&)>& onActivated_;
    std::vector<deviceActions::DeviceChoice> choices_;
    juce::ListBox list_;
};

//==================================================================================================
BrowserPanel::BrowserPanel(const juce::String& audioWildcard, plugins::PluginManager& plugins)
    : plugins_(plugins)
{
    setOpaque(true);

    filesTab_.setButtonText("FILES");
    devicesTab_.setButtonText("DEVICES");
    pluginsTab_.setButtonText("PLUG-INS");
    for (auto* b : {&filesTab_, &devicesTab_, &pluginsTab_})
    {
        b->setRadioGroupId(7777);
        b->setClickingTogglesState(true);
        b->setWantsKeyboardFocus(false);
        b->setColour(juce::TextButton::buttonOnColourId, theme::colour::control);
        addAndMakeVisible(b);
    }
    filesTab_.setToggleState(true, juce::dontSendNotification);
    filesTab_.onClick = [this] { showTab(Tab::files); };
    devicesTab_.onClick = [this] { showTab(Tab::devices); };
    pluginsTab_.onClick = [this] { showTab(Tab::plugins); };

    deviceList_ = std::make_unique<ChoiceList>(onDeviceActivated);
    deviceList_->setChoices(deviceActions::getNativeChoices());
    addChildComponent(*deviceList_);
    pluginList_ = std::make_unique<ChoiceList>(onDeviceActivated);
    addChildComponent(*pluginList_);

    scanButton_.setButtonText("Scan");
    scanButton_.setTooltip("Look for new VST3 and CLAP plug-ins (each is opened in a separate scanner process)");
    scanButton_.onClick = [this] { scanPlugins(false); };
    addChildComponent(scanButton_);
    moreButton_.setButtonText("More...");
    moreButton_.onClick = [this]
    {
        juce::Component::SafePointer<BrowserPanel> safe(this);
        juce::PopupMenu menu;
        menu.addItem("Rescan All", [safe] { if (safe != nullptr) safe->scanPlugins(true); });
        menu.addItem("Add Search Folder...", [safe] { if (safe != nullptr) safe->addSearchFolder(); });
        const auto blacklisted = plugins_.getKnownPlugins().getBlacklistedFiles();
        juce::PopupMenu list;
        for (const auto& file : blacklisted)
            list.addItem(juce::File(file).getFileName() + "  -  " + plugins_.getBlacklistReason(file), false, false, [] {});
        menu.addSubMenu("Blacklisted (" + juce::String(blacklisted.size()) + ")", list, !blacklisted.isEmpty());
        menu.addItem("Clear Blacklist", !blacklisted.isEmpty(), false, [safe]
                     {
                         if (safe != nullptr)
                             safe->plugins_.clearBlacklist();
                     });
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&moreButton_));
    };
    addChildComponent(moreButton_);
    scanStatus_.setFont(theme::sans(11.0f));
    scanStatus_.setColour(juce::Label::textColourId, theme::colour::textMuted);
    addChildComponent(scanStatus_);
    refreshPlugins();

    filter_ = std::make_unique<juce::WildcardFileFilter>(audioWildcard, "*", "Audio files");
    contents_ = std::make_unique<juce::DirectoryContentsList>(filter_.get(), scanThread_);
    contents_->setIgnoresHiddenFiles(true);
    scanThread_.startThread(juce::Thread::Priority::low);

    tree_ = std::make_unique<juce::FileTreeComponent>(*contents_);
    tree_->setDragAndDropDescription(ArrangerView::kBrowserDragDescription);
    tree_->setMultiSelectEnabled(true);
    tree_->setItemHeight(22);
    tree_->addListener(this);
    tree_->setColour(juce::TreeView::backgroundColourId, theme::colour::panel);
    tree_->setColour(juce::FileTreeComponent::textColourId, theme::colour::textPrimary);
    tree_->setColour(juce::DirectoryContentsDisplayComponent::highlightColourId,
                     theme::colour::accent.withAlpha(0.35f));
    tree_->setColour(juce::DirectoryContentsDisplayComponent::textColourId, theme::colour::textPrimary);
    tree_->setColour(juce::DirectoryContentsDisplayComponent::highlightedTextColourId,
                     theme::colour::textPrimary);
    tree_->setColour(juce::TreeView::linesColourId, theme::colour::outlineStrong);
    tree_->setTooltip("Drag audio files onto the arranger, or double-click to add one on a new track");
    addAndMakeVisible(*tree_);

    const auto add = [this](juce::File::SpecialLocationType type, const juce::String& name)
    {
        const auto folder = juce::File::getSpecialLocation(type);
        if (folder.isDirectory() && !locations_.contains(folder))
        {
            locations_.add(folder);
            location_.addItem(name, locations_.size());
        }
    };

    add(juce::File::userMusicDirectory, "Music");
    add(juce::File::userHomeDirectory, "Home");
    add(juce::File::userDesktopDirectory, "Desktop");
    add(juce::File::userDocumentsDirectory, "Documents");
    location_.addSeparator();
    location_.addItem("Choose Folder...", kChooseFolderId);
    location_.setWantsKeyboardFocus(false);
    location_.onChange = [this]
    {
        const int id = location_.getSelectedId();
        if (id == kChooseFolderId)
            chooseFolder();
        else if (id > 0 && id <= locations_.size())
            setRoot(locations_[id - 1]);
    };
    addAndMakeVisible(location_);

    if (!locations_.isEmpty())
    {
        location_.setSelectedId(1, juce::dontSendNotification);
        setRoot(locations_.getFirst());
    }
}

BrowserPanel::~BrowserPanel()
{
    tree_->removeListener(this);
    tree_.reset();
    contents_.reset();
    scanThread_.stopThread(2000);
}

void BrowserPanel::setRoot(const juce::File& folder)
{
    if (folder.isDirectory())
        contents_->setDirectory(folder, true, true);

    if (!locations_.contains(folder))
    {
        locations_.add(folder);
        location_.addItem(folder.getFileName(), locations_.size());
    }

    location_.setSelectedId(locations_.indexOf(folder) + 1, juce::dontSendNotification);
}

void BrowserPanel::chooseFolder()
{
    chooser_ = std::make_unique<juce::FileChooser>("Choose a folder to browse", contents_->getDirectory());
    juce::Component::SafePointer<BrowserPanel> safe(this);

    chooser_->launchAsync(juce::FileBrowserComponent::openMode |
                              juce::FileBrowserComponent::canSelectDirectories,
                          [safe](const juce::FileChooser& chooser)
                          {
                              if (safe == nullptr)
                                  return;

                              const auto folder = chooser.getResult();
                              if (folder.isDirectory())
                                  safe->setRoot(folder);
                              else
                                  safe->setRoot(safe->contents_->getDirectory());
                          });
}

void BrowserPanel::showTab(Tab tab)
{
    tab_ = tab;
    tree_->setVisible(tab == Tab::files);
    location_.setVisible(tab == Tab::files);
    deviceList_->setVisible(tab == Tab::devices);
    pluginList_->setVisible(tab == Tab::plugins);
    scanButton_.setVisible(tab == Tab::plugins);
    moreButton_.setVisible(tab == Tab::plugins);
    scanStatus_.setVisible(tab == Tab::plugins);
    resized();
    repaint();
}

void BrowserPanel::refreshPlugins()
{
    pluginList_->setChoices(deviceActions::getPluginChoices(plugins_));
    const int count = plugins_.getKnownPlugins().getNumTypes();
    scanStatus_.setText(plugins_.isScanning() ? "Scanning..."
                        : count == 0         ? "No plug-ins yet: press Scan"
                                             : juce::String(count) + " plug-ins",
                        juce::dontSendNotification);
}

void BrowserPanel::scanPlugins(bool rescanAll)
{
    if (plugins_.isScanning())
        return;

    juce::Component::SafePointer<BrowserPanel> safe(this);
    plugins_.onScanFinished = [safe](const plugins::PluginManager::ScanReport& report)
    {
        if (safe == nullptr)
            return;
        safe->refreshPlugins();
        juce::String text = juce::String(report.pluginsFound) + " plug-ins found in " + juce::String(report.filesScanned) + " new files.";
        if (!report.crashed.isEmpty())
            text << "\n\nBlacklisted (crashed or hung while scanning):\n" << report.crashed.joinIntoString("\n");
        if (!report.failed.isEmpty())
            text << "\n\nSkipped:\n" << report.failed.joinIntoString("\n");
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Plug-in scan finished", text);
    };
    plugins_.startScan(rescanAll);
    scanStatus_.setText("Scanning...", juce::dontSendNotification);
}

void BrowserPanel::addSearchFolder()
{
    chooser_ = std::make_unique<juce::FileChooser>("Add a plug-in folder", juce::File::getSpecialLocation(juce::File::userHomeDirectory));
    juce::Component::SafePointer<BrowserPanel> safe(this);
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [safe](const juce::FileChooser& chooser)
                          {
                              if (safe == nullptr || !chooser.getResult().isDirectory())
                                  return;
                              auto& manager = safe->plugins_;
                              for (auto* format : manager.getFormatManager().getFormats())
                              {
                                  auto path = manager.getSearchPath(format->getName());
                                  path.addIfNotAlreadyThere(chooser.getResult());
                                  manager.setSearchPath(format->getName(), path);
                              }
                              safe->scanPlugins(false);
                          });
}

void BrowserPanel::fileDoubleClicked(const juce::File& file)
{
    if (file.existsAsFile() && onFileActivated)
        onFileActivated(file);
}

void BrowserPanel::resized()
{
    auto area = getLocalBounds().withTrimmedRight(1);
    area.removeFromTop(kHeaderHeight);
    auto tabs = area.removeFromTop(kTabHeight).reduced(6, 2);
    const int tabWidth = tabs.getWidth() / 3;
    filesTab_.setBounds(tabs.removeFromLeft(tabWidth));
    devicesTab_.setBounds(tabs.removeFromLeft(tabWidth));
    pluginsTab_.setBounds(tabs);

    if (tab_ == Tab::files)
    {
        location_.setBounds(area.removeFromTop(36).reduced(10, 5));
        area.removeFromBottom(30);
        tree_->setBounds(area);
    }
    else if (tab_ == Tab::devices)
    {
        area.removeFromBottom(30);
        deviceList_->setBounds(area.withTrimmedTop(4));
    }
    else
    {
        auto buttons = area.removeFromTop(34).reduced(8, 4);
        scanButton_.setBounds(buttons.removeFromLeft(70));
        buttons.removeFromLeft(4);
        moreButton_.setBounds(buttons.removeFromLeft(70));
        scanStatus_.setBounds(area.removeFromBottom(26).reduced(8, 0));
        pluginList_->setBounds(area);
    }
}

void BrowserPanel::paint(juce::Graphics& g)
{
    using namespace theme;

    g.fillAll(colour::panel);

    auto header = getLocalBounds().removeFromTop(kHeaderHeight);
    g.setColour(colour::panelHeader);
    g.fillRect(header);
    g.setColour(colour::textSecondary);
    g.setFont(label(11.0f));
    g.drawText("BROWSER", header.reduced(14, 0), juce::Justification::centredLeft, false);
    g.setColour(colour::outline);
    g.fillRect(header.removeFromBottom(1));

    if (tab_ != Tab::plugins)
    {
        auto footer = getLocalBounds().removeFromBottom(30).reduced(12, 4);
        g.setColour(colour::textMuted);
        g.setFont(sans(11.0f));
        g.drawFittedText(tab_ == Tab::files ? "Drag onto the arranger, or double-click" : "Drag onto a track, or double-click to add",
                         footer, juce::Justification::centredLeft, 2);
    }

    g.setColour(colour::outline);
    g.fillRect(getLocalBounds().removeFromRight(1));
}

} // namespace dojo::ui
