#include "ui/components/BrowserPanel.h"

#include "ui/components/ArrangerView.h"
#include "ui/theme/DojoTheme.h"

namespace dojo::ui
{
namespace
{
constexpr int kHeaderHeight = 34;
constexpr int kChooseFolderId = 1000;
} // namespace

BrowserPanel::BrowserPanel(const juce::String& audioWildcard)
{
    setOpaque(true);

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

void BrowserPanel::fileDoubleClicked(const juce::File& file)
{
    if (file.existsAsFile() && onFileActivated)
        onFileActivated(file);
}

void BrowserPanel::resized()
{
    auto area = getLocalBounds();
    area.removeFromTop(kHeaderHeight);
    location_.setBounds(area.removeFromTop(36).reduced(10, 5));
    area.removeFromBottom(44);
    tree_->setBounds(area.withTrimmedRight(1));
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

    auto footer = getLocalBounds().removeFromBottom(44).reduced(12, 6);
    g.setColour(colour::textMuted);
    g.setFont(sans(11.0f));
    g.drawFittedText("Built-in instruments, effects and plug-ins arrive in later phases.", footer,
                     juce::Justification::centredLeft, 2);

    g.setColour(colour::outline);
    g.fillRect(getLocalBounds().removeFromRight(1));
}

} // namespace dojo::ui
