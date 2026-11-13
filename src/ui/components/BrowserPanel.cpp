#include "ui/components/BrowserPanel.h"

#include "devices/DeviceRegistry.h"
#include "ui/BrowserDrag.h"
#include "ui/PresetActions.h"
#include "ui/theme/DojoTheme.h"

#include <algorithm>

namespace dojo::ui
{
namespace
{
constexpr int kHeaderHeight = 34;
constexpr int kSectionHeader = 28;
constexpr int kSearchHeight = 30;
constexpr int kStripHeight = 40;
constexpr int kDividerHeight = 7;
constexpr int kStatusHeight = 20;
constexpr int kMaxSearchResults = 2000;
constexpr int kTimerHz = 20;

constexpr const char* kFoldersKey = "browserFolders";
constexpr const char* kPreviewKey = "browserPreview";
constexpr const char* kPreviewVolumeKey = "browserPreviewVolume";
constexpr const char* kSplitKey = "browserSplit";

juce::StringArray termsOf(const juce::String& query)
{
    juce::StringArray terms;
    terms.addTokens(query, " ", "\"");
    terms.trim();
    terms.removeEmptyStrings();
    return terms;
}

bool matchesAll(const juce::String& text, const juce::StringArray& terms)
{
    return std::all_of(terms.begin(), terms.end(), [&](const juce::String& term) { return text.containsIgnoreCase(term); });
}

void styleSearch(juce::TextEditor& editor, const juce::String& placeholder)
{
    editor.setTextToShowWhenEmpty(placeholder, theme::colour::textMuted);
    editor.setFont(theme::sans(12.5f));
    editor.setIndents(8, 5);
    editor.setColour(juce::TextEditor::backgroundColourId, theme::colour::meterBackground);
    editor.setColour(juce::TextEditor::outlineColourId, theme::colour::outline);
    editor.setColour(juce::TextEditor::focusedOutlineColourId, theme::colour::accent.withAlpha(0.7f));
    editor.setColour(juce::TextEditor::textColourId, theme::colour::textPrimary);
    editor.setColour(juce::CaretComponent::caretColourId, theme::colour::textPrimary);
}

void styleSmallButton(juce::TextButton& button, const juce::String& text, const juce::String& tooltip)
{
    button.setButtonText(text);
    button.setTooltip(tooltip);
    button.setWantsKeyboardFocus(false);
}

void sortByName(juce::Array<juce::File>& files)
{
    std::sort(files.begin(), files.end(),
              [](const juce::File& a, const juce::File& b) { return a.getFileName().compareNatural(b.getFileName()) < 0; });
}

/** One row of the folder tree: an added folder, a sub-folder, a file, or a search result. */
class FileItem final : public juce::TreeViewItem
{
public:
    enum class Kind
    {
        root,
        folder,
        file,
        result,
    };

    FileItem(BrowserPanel& browser, juce::File file, juce::String wildcard, Kind kind, juce::String detail = {})
        : browser_(browser)
        , file_(std::move(file))
        , wildcard_(std::move(wildcard))
        , kind_(kind)
        , detail_(std::move(detail))
    {
    }

    [[nodiscard]] const juce::File& getFile() const noexcept { return file_; }
    [[nodiscard]] bool isAudioFile() const noexcept { return kind_ == Kind::file || kind_ == Kind::result; }

    bool mightContainSubItems() override { return !isAudioFile(); }
    juce::String getUniqueName() const override { return file_.getFullPathName(); }
    int getItemHeight() const override { return 22; }
    juce::String getTooltip() override { return file_.getFullPathName(); }

    void itemOpennessChanged(bool isNowOpen) override
    {
        if (isNowOpen && !loaded_)
            load();
    }

    void refresh()
    {
        clearSubItems();
        loaded_ = false;
        if (isOpen())
            load();
    }

    void paintItem(juce::Graphics& g, int width, int height) override
    {
        using namespace theme;
        if (isSelected())
            g.fillAll(colour::accent.withAlpha(0.28f));

        const bool previewing = isAudioFile() && browser_.isPreviewEnabled() && file_ == previewFile();
        auto icon = juce::Rectangle<float>(2.0f, 0.0f, 14.0f, static_cast<float>(height)).withSizeKeepingCentre(14.0f, 11.0f);
        if (isAudioFile())
        {
            g.setColour(previewing ? colour::accentBright : colour::play.withAlpha(0.8f));
            const float bar = icon.getWidth() / 7.0f;
            const float heights[4] = {0.45f, 1.0f, 0.65f, 0.3f};
            for (int i = 0; i < 4; ++i)
            {
                const float h = icon.getHeight() * heights[i];
                g.fillRect(icon.getX() + bar * (2.0f * static_cast<float>(i)) + bar * 0.5f, icon.getCentreY() - h * 0.5f, bar, h);
            }
        }
        else
        {
            g.setColour(kind_ == Kind::root ? colour::gold : colour::gold.withAlpha(0.6f));
            g.fillRoundedRectangle(icon.withTrimmedTop(2.0f), 1.5f);
            g.fillRoundedRectangle(icon.withWidth(6.0f).withHeight(4.0f), 1.0f);
        }

        const auto text = juce::Rectangle<int>(22, 0, width - 24, height);
        g.setColour(kind_ == Kind::root && !file_.isDirectory() ? colour::textMuted : colour::textPrimary);
        g.setFont(kind_ == Kind::root ? sansBold(12.5f) : sans(12.5f));
        const auto name = kind_ == Kind::root ? file_.getFileName() + (file_.isDirectory() ? "" : " (missing)") : file_.getFileName();
        if (detail_.isEmpty())
        {
            g.drawText(name, text, juce::Justification::centredLeft, true);
            return;
        }

        const int nameWidth = std::min(text.getWidth(), juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), name) + 8);
        g.drawText(name, text.withWidth(nameWidth), juce::Justification::centredLeft, true);
        g.setColour(colour::textMuted);
        g.setFont(sans(11.0f));
        g.drawText(detail_, text.withTrimmedLeft(nameWidth), juce::Justification::centredLeft, true);
    }

    void itemClicked(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
            showMenu();
    }

    void itemDoubleClicked(const juce::MouseEvent&) override
    {
        if (isAudioFile())
        {
            if (browser_.onFileActivated)
                browser_.onFileActivated(file_);
        }
        else
        {
            setOpen(!isOpen());
        }
    }

    void itemSelectionChanged(bool isNowSelected) override
    {
        if (isNowSelected && isAudioFile() && getOwnerView() != nullptr && getOwnerView()->getNumSelectedItems() == 1)
            browser_.fileSelected(file_);
    }

    juce::var getDragSourceDescription() override
    {
        if (!isAudioFile() || getOwnerView() == nullptr)
            return {};

        juce::StringArray paths;
        auto* view = getOwnerView();
        for (int i = 0; i < view->getNumSelectedItems(); ++i)
            if (auto* item = dynamic_cast<FileItem*>(view->getSelectedItem(i)); item != nullptr && item->isAudioFile())
                paths.add(item->file_.getFullPathName());
        if (!paths.contains(file_.getFullPathName()))
            paths.insert(0, file_.getFullPathName());
        return browserDrag::describe(paths);
    }

private:
    [[nodiscard]] juce::File previewFile() const;

    void load()
    {
        loaded_ = true;
        if (!file_.isDirectory())
            return;

        auto folders = file_.findChildFiles(juce::File::findDirectories | juce::File::ignoreHiddenFiles, false);
        auto files = file_.findChildFiles(juce::File::findFiles | juce::File::ignoreHiddenFiles, false, wildcard_);
        sortByName(folders);
        sortByName(files);
        for (const auto& folder : folders)
            addSubItem(new FileItem(browser_, folder, wildcard_, Kind::folder));
        for (const auto& file : files)
            addSubItem(new FileItem(browser_, file, wildcard_, Kind::file));
    }

    void showMenu()
    {
        juce::PopupMenu menu;
        auto& browser = browser_;
        const auto file = file_;
        if (isAudioFile())
        {
            menu.addItem("Preview", [&browser, file] { browser.fileSelected(file); });
            menu.addItem("Add to New Track", [&browser, file]
                         {
                             if (browser.onFileActivated)
                                 browser.onFileActivated(file);
                         });
        }
        else
        {
            menu.addItem("Refresh", [this] { refresh(); });
        }
        menu.addItem("Show in File Manager", [file] { file.revealToUser(); });
        if (kind_ == Kind::root)
        {
            menu.addSeparator();
            juce::Component::SafePointer<BrowserPanel> safe(&browser_);
            menu.addItem("Remove from Browser", [safe, file]
                         {
                             if (safe != nullptr)
                                 safe->removeFolder(file);
                         });
        }
        menu.showMenuAsync(juce::PopupMenu::Options().withMousePosition());
    }

    BrowserPanel& browser_;
    juce::File file_;
    juce::String wildcard_;
    Kind kind_;
    juce::String detail_;
    bool loaded_ = false;
};

/** The invisible root whose children are the added folders (or the search results). */
class RootItem final : public juce::TreeViewItem
{
public:
    bool mightContainSubItems() override { return true; }
    juce::String getUniqueName() const override { return "root"; }
    void paintItem(juce::Graphics&, int, int) override {}
};
} // namespace

//==================================================================================================
/** The added folders as a tree, or the results of a search as a flat list. */
class BrowserPanel::FolderTree final : public juce::TreeView
{
public:
    explicit FolderTree(BrowserPanel& browser)
        : browser_(browser)
    {
        setRootItem(&root_);
        setRootItemVisible(false);
        setMultiSelectEnabled(true);
        setIndentSize(14);
        setColour(juce::TreeView::backgroundColourId, theme::colour::panel);
        setColour(juce::TreeView::linesColourId, theme::colour::outlineStrong);
        setColour(juce::TreeView::selectedItemBackgroundColourId, juce::Colours::transparentBlack);
    }

    ~FolderTree() override
    {
        setRootItem(nullptr);
        root_.clearSubItems();
    }

    void showFolders(const juce::Array<juce::File>& folders)
    {
        auto openness = searching_ ? std::move(folderOpenness_) : getOpennessState(true);
        searching_ = false;
        root_.clearSubItems();
        for (const auto& folder : folders)
            root_.addSubItem(new FileItem(browser_, folder, browser_.wildcard_, FileItem::Kind::root));
        root_.setOpen(true);
        if (openness != nullptr)
            restoreOpennessState(*openness, true);
    }

    void showResults(const juce::Array<juce::File>& folders, const juce::Array<juce::File>& results)
    {
        if (!searching_)
            folderOpenness_ = getOpennessState(true);
        searching_ = true;
        root_.clearSubItems();
        results_ = results;
        for (const auto& file : results)
        {
            juce::String detail;
            for (const auto& folder : folders)
                if (file.isAChildOf(folder))
                {
                    detail = folder.getFileName() + "/" + file.getParentDirectory().getRelativePathFrom(folder);
                    detail = detail.trimCharactersAtEnd("/.");
                    break;
                }
            root_.addSubItem(new FileItem(browser_, file, browser_.wildcard_, FileItem::Kind::result, detail));
        }
        root_.setOpen(true);
    }

    [[nodiscard]] bool isShowingResults() const noexcept { return searching_; }
    [[nodiscard]] const juce::Array<juce::File>& getResults() const noexcept { return results_; }

    void paint(juce::Graphics& g) override
    {
        juce::TreeView::paint(g);
        if (searching_ && root_.getNumSubItems() == 0)
        {
            g.setColour(theme::colour::textMuted);
            g.setFont(theme::sans(12.0f));
            g.drawFittedText(browser_.isSearching() ? "Searching..." : "No matching audio files", getLocalBounds().reduced(12).removeFromTop(40),
                             juce::Justification::centred, 2);
        }
    }

private:
    BrowserPanel& browser_;
    RootItem root_;
    bool searching_ = false;
    juce::Array<juce::File> results_;
    std::unique_ptr<juce::XmlElement> folderOpenness_;
};

juce::File FileItem::previewFile() const
{
    return browser_.getPreviewingFile();
}

//==================================================================================================
/** Devices and plug-ins, grouped under headings and filtered by the search text. */
class BrowserPanel::DeviceList final : public juce::Component, private juce::ListBoxModel
{
public:
    explicit DeviceList(BrowserPanel& browser)
        : browser_(browser)
    {
        list_.setModel(this);
        list_.setRowHeight(22);
        list_.setOutlineThickness(0);
        list_.setColour(juce::ListBox::backgroundColourId, theme::colour::panel);
        addAndMakeVisible(list_);
    }

    void setChoices(std::vector<deviceActions::DeviceChoice> choices)
    {
        all_ = std::move(choices);
        std::stable_sort(all_.begin(), all_.end(), [](const auto& a, const auto& b)
                         {
                             const bool pluginA = a.type == plugins::PluginManager::kDeviceType;
                             const bool pluginB = b.type == plugins::PluginManager::kDeviceType;
                             return pluginA && pluginB ? a.name.compareNatural(b.name) < 0 : false;
                         });
        rebuild();
    }

    void setPresets(std::vector<session::presets::Preset> presets)
    {
        presets_ = std::move(presets);
        rebuild();
    }

    void setQuery(const juce::String& query)
    {
        query_ = query;
        rebuild();
    }

    [[nodiscard]] juce::StringArray getListedNames() const
    {
        juce::StringArray names;
        for (const auto& row : rows_)
            if (row.choice != nullptr)
                names.add(row.choice->name);
        return names;
    }

    [[nodiscard]] juce::StringArray getListedPresetNames() const
    {
        juce::StringArray names;
        for (const auto& row : rows_)
            if (row.preset != nullptr)
                names.add(row.preset->name);
        return names;
    }

    [[nodiscard]] juce::String getPresetDragDescription(const juce::String& name) const
    {
        for (const auto& row : rows_)
            if (row.preset != nullptr && row.preset->name == name)
                return presetActions::toDragDescription(*row.preset);
        return {};
    }

    void resized() override { list_.setBounds(getLocalBounds()); }

private:
    struct Row
    {
        juce::String heading;
        const deviceActions::DeviceChoice* choice = nullptr;
        const session::presets::Preset* preset = nullptr;
    };

    void rebuild()
    {
        rows_.clear();
        const auto terms = termsOf(query_);
        const auto addGroup = [&](const juce::String& heading, bool plugin, bool instrument)
        {
            bool first = true;
            for (const auto& choice : all_)
            {
                if ((choice.type == plugins::PluginManager::kDeviceType) != plugin || choice.instrument != instrument)
                    continue;
                if (!matchesAll(choice.name + " " + choice.category, terms))
                    continue;
                if (first)
                    rows_.push_back({heading, nullptr, nullptr});
                first = false;
                rows_.push_back({{}, &choice, nullptr});
            }
        };
        addGroup("INSTRUMENTS", false, true);
        addGroup("AUDIO EFFECTS", false, false);
        const auto addPresets = [&](const juce::String& heading, bool chains)
        {
            bool first = true;
            for (const auto& preset : presets_)
            {
                if (preset.chain != chains)
                    continue;
                // (searchable by name, what it is for, its devices' roles and kinds)
                juce::String text = preset.name + (chains ? " chain " : " preset ") + preset.category;
                for (const auto& d : preset.devices)
                {
                    text << ' ' << d.deviceName << ' ' << d.key.fromLastOccurrenceOf(":", false, false);
                    if (const auto* info = devices::findBuiltinDevice(d.key)) // ("Plate Reverb", "Glue Compressor")
                        text << ' ' << info->name;
                }
                if (!matchesAll(text, terms))
                    continue;
                if (first)
                    rows_.push_back({heading, nullptr, nullptr});
                first = false;
                rows_.push_back({{}, nullptr, &preset});
            }
        };
        addPresets("PRESET CHAINS", true);
        addPresets("PRESETS", false);
        addGroup("PLUG-IN INSTRUMENTS", true, true);
        addGroup("PLUG-IN EFFECTS", true, false);
        list_.deselectAllRows();
        list_.updateContent();
        list_.repaint();
    }

    int getNumRows() override { return static_cast<int>(rows_.size()); }

    [[nodiscard]] const deviceActions::DeviceChoice* choiceAt(int row) const
    {
        return row >= 0 && row < static_cast<int>(rows_.size()) ? rows_[static_cast<std::size_t>(row)].choice : nullptr;
    }

    void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override
    {
        using namespace theme;
        if (row < 0 || row >= getNumRows())
            return;
        const auto& r = rows_[static_cast<std::size_t>(row)];
        if (r.preset != nullptr)
        {
            if (selected)
                g.fillAll(colour::accent.withAlpha(0.28f));
            const float cy = static_cast<float>(height) * 0.5f;
            g.setColour(r.preset->builtIn ? colour::accent : colour::textSecondary);
            if (r.preset->chain)
            {
                // Three linked devices.
                for (int k = 0; k < 3; ++k)
                    g.fillRoundedRectangle(9.0f + static_cast<float>(k) * 4.5f, cy - 2.0f, 3.5f, 4.0f, 0.8f);
            }
            else
            {
                juce::Path diamond;
                diamond.addQuadrilateral(15.0f, cy - 4.0f, 19.0f, cy, 15.0f, cy + 4.0f, 11.0f, cy);
                g.fillPath(diamond);
            }
            const auto detail = r.preset->chain ? (r.preset->category.isNotEmpty() ? r.preset->category + ", " : juce::String()) +
                                                      juce::String(static_cast<int>(r.preset->devices.size())) + " devices"
                                                : r.preset->getDeviceNames().joinIntoString(", ");
            g.setFont(sans(10.5f));
            const int detailWidth = std::min(width / 2, juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), detail) + 4);
            g.setColour(colour::textPrimary);
            g.setFont(sans(12.5f));
            g.drawText(r.preset->name, 26, 0, width - 34 - detailWidth, height, juce::Justification::centredLeft, true);
            g.setColour(colour::textMuted);
            g.setFont(sans(10.5f));
            g.drawText(detail, width - 6 - detailWidth, 0, detailWidth, height, juce::Justification::centredRight, true);
            return;
        }
        if (r.choice == nullptr)
        {
            g.setColour(colour::textMuted);
            g.setFont(label(10.0f));
            g.drawText(r.heading, 10, 0, width - 12, height, juce::Justification::bottomLeft, true);
            return;
        }

        if (selected)
            g.fillAll(colour::accent.withAlpha(0.28f));
        g.setColour(r.choice->instrument ? colour::gold : colour::play);
        g.fillEllipse(12.0f, static_cast<float>(height) * 0.5f - 3.0f, 6.0f, 6.0f);
        g.setColour(colour::textPrimary);
        g.setFont(sans(12.5f));
        g.drawText(r.choice->name, 26, 0, width - 30, height, juce::Justification::centredLeft, true);
        g.setColour(colour::textMuted);
        g.setFont(sans(10.5f));
        g.drawText(r.choice->category, 26, 0, width - 32, height, juce::Justification::centredRight, true);
    }

    [[nodiscard]] const session::presets::Preset* presetAt(int row) const
    {
        return row >= 0 && row < static_cast<int>(rows_.size()) ? rows_[static_cast<std::size_t>(row)].preset : nullptr;
    }

    void activate(int row)
    {
        if (const auto* choice = choiceAt(row); choice != nullptr && browser_.onDeviceActivated)
            browser_.onDeviceActivated(*choice);
        if (const auto* preset = presetAt(row); preset != nullptr && browser_.onPresetActivated)
            browser_.onPresetActivated(*preset);
    }

    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override { activate(row); }
    void returnKeyPressed(int row) override { activate(row); }

    juce::var getDragSourceDescription(const juce::SparseSet<int>& rows) override
    {
        if (rows.isEmpty())
            return {};
        if (const auto* preset = presetAt(rows[0]))
            return presetActions::toDragDescription(*preset);
        const auto* choice = choiceAt(rows[0]);
        return choice != nullptr ? juce::var(deviceActions::toDragDescription(*choice)) : juce::var();
    }

    juce::String getTooltipForRow(int row) override
    {
        if (const auto* preset = presetAt(row))
        {
            const auto description = preset->description.isNotEmpty() ? preset->description + "\n" : juce::String();
            if (preset->chain)
                return description + "Chain: " + preset->getDeviceNames().joinIntoString(" > ") +
                       "\nDrag onto a track or the Devices panel to add the whole chain (or double-click to add it to the "
                       "selected track); drop it on one device to load that device's part";
            return description + "Preset for " + preset->getDeviceNames().joinIntoString(", ") +
                   " - drag onto a device to load it, or double-click to use it on the selected track";
        }
        const auto* choice = choiceAt(row);
        if (choice == nullptr)
            return {};
        return choice->name + (choice->instrument ? " (instrument)" : " (effect)") + " - drag onto a track, or double-click to add";
    }

    BrowserPanel& browser_;
    std::vector<deviceActions::DeviceChoice> all_;
    std::vector<session::presets::Preset> presets_;
    std::vector<Row> rows_;
    juce::String query_;
    juce::ListBox list_;
};

//==================================================================================================
/** The preview on/off switch, the previewed file with its progress, and the preview volume. */
class BrowserPanel::PreviewStrip final : public juce::Component
{
public:
    explicit PreviewStrip(BrowserPanel& browser)
        : browser_(browser)
    {
        juce::Path arc;
        arc.addCentredArc(9.0f, 10.0f, 7.0f, 7.0f, 0.0f, -juce::MathConstants<float>::halfPi, juce::MathConstants<float>::halfPi, true);
        juce::Path shape;
        juce::PathStrokeType(2.0f).createStrokedPath(shape, arc);
        shape.addRoundedRectangle(0.5f, 9.0f, 4.5f, 8.0f, 1.5f);
        shape.addRoundedRectangle(13.0f, 9.0f, 4.5f, 8.0f, 1.5f);
        headphones_.setShape(shape, false, true, false);
        headphones_.setClickingTogglesState(true);
        headphones_.shouldUseOnColours(true);
        headphones_.setOnColours(theme::colour::accentBright, theme::colour::accentBright.brighter(0.2f), theme::colour::accent);
        headphones_.setToggleState(browser_.previewEnabled_, juce::dontSendNotification);
        headphones_.setTooltip("Preview files when they are selected");
        headphones_.onClick = [this] { browser_.setPreviewEnabled(headphones_.getToggleState()); };
        addAndMakeVisible(headphones_);

        volume_.setSliderStyle(juce::Slider::LinearHorizontal);
        volume_.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        volume_.setRange(0.0, 1.0, 0.0);
        volume_.setValue(browser_.preview_.getVolume(), juce::dontSendNotification);
        volume_.setTooltip("Preview volume");
        volume_.setColour(juce::Slider::trackColourId, theme::colour::textSecondary);
        volume_.onValueChange = [this]
        {
            browser_.preview_.setVolume(static_cast<float>(volume_.getValue()));
            browser_.settings_.setValue(kPreviewVolumeKey, volume_.getValue());
        };
        addAndMakeVisible(volume_);
    }

    void setPreviewEnabled(bool enabled) { headphones_.setToggleState(enabled, juce::dontSendNotification); }

    void resized() override
    {
        auto area = getLocalBounds().reduced(8, 0);
        headphones_.setBounds(area.removeFromLeft(20).withSizeKeepingCentre(18, 18));
        area.removeFromLeft(8);
        volume_.setBounds(area.removeFromRight(64).withSizeKeepingCentre(64, 20));
        area.removeFromRight(6);
        info_ = area;
    }

    void paint(juce::Graphics& g) override
    {
        using namespace theme;
        g.fillAll(colour::panelHeader);
        g.setColour(colour::outline);
        g.fillRect(getLocalBounds().removeFromTop(1));

        const bool playing = browser_.preview_.isPlaying();
        const auto file = browser_.preview_.getCurrentFile();
        auto area = info_;
        g.setColour(playing ? colour::textPrimary : colour::textMuted);
        g.setFont(sans(11.5f));
        const auto text = !browser_.previewEnabled_ ? juce::String("Preview off")
                          : file == juce::File()   ? juce::String("Select a file to preview")
                                                   : file.getFileName();
        g.drawText(text, area.removeFromTop(area.getHeight() - 10), juce::Justification::bottomLeft, true);

        auto bar = area.removeFromTop(3).toFloat().withY(static_cast<float>(area.getY()) + 1.0f);
        g.setColour(colour::meterBackground);
        g.fillRoundedRectangle(bar, 1.5f);
        if (playing)
        {
            g.setColour(colour::accent);
            g.fillRoundedRectangle(bar.withWidth(bar.getWidth() * juce::jlimit(0.0f, 1.0f, browser_.preview_.getProgress())), 1.5f);
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (info_.contains(e.getPosition()))
            browser_.preview_.stop(); // click the strip to stop a long preview
    }

private:
    BrowserPanel& browser_;
    juce::ShapeButton headphones_{"Preview", theme::colour::textMuted, theme::colour::textSecondary, theme::colour::textPrimary};
    juce::Slider volume_;
    juce::Rectangle<int> info_;
};

//==================================================================================================
/** Searches every added folder recursively for audio files whose path matches all search terms. */
class BrowserPanel::SearchThread final : public juce::Thread
{
public:
    explicit SearchThread(BrowserPanel& browser)
        : juce::Thread("Dojo browser search")
        , browser_(browser)
    {
    }

    ~SearchThread() override { stopThread(4000); }

    void search(const juce::Array<juce::File>& folders, const juce::String& query, const juce::String& wildcard)
    {
        stopThread(4000);
        folders_ = folders;
        query_ = query;
        wildcard_ = wildcard;
        startThread(juce::Thread::Priority::low);
    }

private:
    void run() override
    {
        const auto terms = termsOf(query_);
        juce::Array<juce::File> found;
        for (const auto& folder : folders_)
        {
            for (const auto& entry : juce::RangedDirectoryIterator(folder, true, wildcard_, juce::File::findFiles | juce::File::ignoreHiddenFiles))
            {
                if (threadShouldExit())
                    return;
                const auto file = entry.getFile();
                if (matchesAll(file.getRelativePathFrom(folder), terms) && !found.contains(file))
                    found.add(file);
                if (found.size() >= kMaxSearchResults)
                    break;
            }
            if (found.size() >= kMaxSearchResults)
                break;
        }
        sortByName(found);

        juce::Component::SafePointer<BrowserPanel> safe(&browser_);
        juce::MessageManager::callAsync([safe, query = query_, found]
                                        {
                                            if (safe != nullptr)
                                                safe->searchFinished(query, found);
                                        });
    }

    BrowserPanel& browser_;
    juce::Array<juce::File> folders_;
    juce::String query_;
    juce::String wildcard_;
};

//==================================================================================================
/** The draggable bar between the two sections. */
class BrowserPanel::Divider final : public juce::Component
{
public:
    explicit Divider(BrowserPanel& browser)
        : browser_(browser)
    {
        setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
        setRepaintsOnMouseActivity(true);
    }

    void paint(juce::Graphics& g) override
    {
        g.setColour(isMouseOverOrDragging() ? theme::colour::accent.withAlpha(0.6f) : theme::colour::outlineStrong);
        g.fillRect(getLocalBounds().withSizeKeepingCentre(getWidth(), 1));
        g.fillRoundedRectangle(getLocalBounds().toFloat().withSizeKeepingCentre(28.0f, 3.0f), 1.5f);
    }

    void mouseDown(const juce::MouseEvent&) override { start_ = browser_.split_; }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        const int available = std::max(1, browser_.getHeight() - kHeaderHeight - kStripHeight - kDividerHeight);
        browser_.split_ = juce::jlimit(0.12, 0.85, start_ + static_cast<double>(e.getDistanceFromDragStartY()) / available);
        browser_.layoutSections();
    }

    void mouseUp(const juce::MouseEvent&) override { browser_.settings_.setValue(kSplitKey, browser_.split_); }

private:
    BrowserPanel& browser_;
    double start_ = 0.0;
};

//==================================================================================================
BrowserPanel::BrowserPanel(const juce::String& audioWildcard, plugins::PluginManager& plugins, engine::PreviewService& preview,
                           juce::PropertiesFile& settings)
    : wildcard_(audioWildcard)
    , plugins_(plugins)
    , preview_(preview)
    , settings_(settings)
{
    setOpaque(true);

    for (const auto& path : juce::StringArray::fromLines(settings_.getValue(kFoldersKey)))
        if (path.isNotEmpty() && juce::File::isAbsolutePath(path) && !folders_.contains(juce::File(path)))
            folders_.add(juce::File(path));
    previewEnabled_ = settings_.getBoolValue(kPreviewKey, true);
    preview_.setVolume(static_cast<float>(settings_.getDoubleValue(kPreviewVolumeKey, 0.7)));
    split_ = juce::jlimit(0.12, 0.85, settings_.getDoubleValue(kSplitKey, 0.42));

    // --- Devices & plug-ins ----------------------------------------------------------------------
    styleSearch(deviceSearch_, "Search devices and plug-ins");
    deviceSearch_.onTextChange = [this] { deviceList_->setQuery(deviceSearch_.getText()); };
    deviceSearch_.onEscapeKey = [this]
    {
        deviceSearch_.clear();
        deviceList_->setQuery({});
        deviceSearch_.giveAwayKeyboardFocus();
    };
    deviceSearch_.onReturnKey = [this] { deviceSearch_.giveAwayKeyboardFocus(); };
    addAndMakeVisible(deviceSearch_);

    deviceList_ = std::make_unique<DeviceList>(*this);
    addAndMakeVisible(*deviceList_);

    styleSmallButton(scanButton_, "Scan", "Look for new VST3 and CLAP plug-ins (each is opened in a separate scanner process)");
    scanButton_.onClick = [this] { scanPlugins(false); };
    addAndMakeVisible(scanButton_);
    styleSmallButton(moreButton_, "...", "Rescan, plug-in folders, blacklist");
    moreButton_.onClick = [this] { showPluginMenu(); };
    addAndMakeVisible(moreButton_);

    divider_ = std::make_unique<Divider>(*this);
    addAndMakeVisible(*divider_);

    // --- Folders ---------------------------------------------------------------------------------
    styleSearch(folderSearch_, "Search your folders");
    folderSearch_.onTextChange = [this] { searchDebounce_ = 5; };
    folderSearch_.onReturnKey = [this] { searchDebounce_ = 0; setFolderSearch(folderSearch_.getText()); };
    folderSearch_.onEscapeKey = [this]
    {
        folderSearch_.clear();
        setFolderSearch({});
        folderSearch_.giveAwayKeyboardFocus();
    };
    addAndMakeVisible(folderSearch_);

    styleSmallButton(addFolderButton_, "+", "Add a folder to the browser");
    addFolderButton_.onClick = [this] { chooseFolderToAdd(); };
    addAndMakeVisible(addFolderButton_);
    styleSmallButton(emptyAddButton_, "Add Folder...", "Add a folder you keep samples in");
    emptyAddButton_.onClick = [this] { chooseFolderToAdd(); };
    addChildComponent(emptyAddButton_);

    folderTree_ = std::make_unique<FolderTree>(*this);
    folderTree_->showFolders(folders_);
    addChildComponent(*folderTree_);

    previewStrip_ = std::make_unique<PreviewStrip>(*this);
    addAndMakeVisible(*previewStrip_);

    search_ = std::make_unique<SearchThread>(*this);

    refreshPlugins();
    refreshPresets();
    startTimerHz(kTimerHz);
}

BrowserPanel::~BrowserPanel()
{
    stopTimer();
    search_.reset();
    folderTree_.reset();
}

//==================================================================================================
// Folders

void BrowserPanel::addFolder(const juce::File& folder)
{
    if (!folder.isDirectory() || folders_.contains(folder))
        return;
    folders_.add(folder);
    saveFolders();
    if (folderTree_->isShowingResults())
        setFolderSearch(folderSearch_.getText());
    else
        folderTree_->showFolders(folders_);
    layoutSections();
    repaint();
}

void BrowserPanel::removeFolder(const juce::File& folder)
{
    if (!folders_.contains(folder))
        return;
    folders_.removeAllInstancesOf(folder);
    saveFolders();
    if (folderTree_->isShowingResults() && !folders_.isEmpty())
        setFolderSearch(folderSearch_.getText());
    else
        folderTree_->showFolders(folders_);
    layoutSections();
    repaint();
}

void BrowserPanel::saveFolders()
{
    juce::StringArray paths;
    for (const auto& folder : folders_)
        paths.add(folder.getFullPathName());
    settings_.setValue(kFoldersKey, paths.joinIntoString("\n"));
}

void BrowserPanel::chooseFolderToAdd()
{
    chooser_ = std::make_unique<juce::FileChooser>("Add a folder to the browser",
                                                   juce::File::getSpecialLocation(juce::File::userMusicDirectory));
    juce::Component::SafePointer<BrowserPanel> safe(this);
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [safe](const juce::FileChooser& chooser)
                          {
                              if (safe != nullptr && chooser.getResult().isDirectory())
                                  safe->addFolder(chooser.getResult());
                          });
}

void BrowserPanel::setFolderSearch(const juce::String& query)
{
    activeQuery_ = query.trim();
    if (activeQuery_.isEmpty() || folders_.isEmpty())
    {
        searching_ = false;
        search_->stopThread(4000);
        folderTree_->showFolders(folders_);
        return;
    }

    searching_ = true;
    folderTree_->showResults(folders_, {});
    search_->search(folders_, activeQuery_, wildcard_);
    folderTree_->repaint();
}

void BrowserPanel::searchFinished(const juce::String& query, const juce::Array<juce::File>& results)
{
    if (query != activeQuery_ || activeQuery_.isEmpty())
        return; // an older search
    searching_ = false;
    folderTree_->showResults(folders_, results);
    folderTree_->repaint();
}

juce::Array<juce::File> BrowserPanel::getSearchResults() const
{
    return folderTree_->isShowingResults() ? folderTree_->getResults() : juce::Array<juce::File>();
}

bool BrowserPanel::isSearching() const noexcept
{
    return searching_ || searchDebounce_ > 0;
}

bool BrowserPanel::isInterestedInFileDrag(const juce::StringArray& files)
{
    return std::any_of(files.begin(), files.end(), [](const juce::String& path) { return juce::File(path).isDirectory(); });
}

void BrowserPanel::filesDropped(const juce::StringArray& files, int, int)
{
    for (const auto& path : files)
        if (juce::File(path).isDirectory())
            addFolder(juce::File(path));
}

//==================================================================================================
// Devices and plug-ins

void BrowserPanel::setDeviceSearch(const juce::String& query)
{
    deviceSearch_.setText(query, juce::dontSendNotification);
    deviceList_->setQuery(query);
}

juce::StringArray BrowserPanel::getListedDeviceNames() const
{
    return deviceList_->getListedNames();
}

namespace
{
/** The preset files in the presets folder, with their sizes and times (to notice changes). */
juce::String presetFolderSignature()
{
    juce::String signature;
    const auto folder = session::presets::getUserFolder();
    if (folder.isDirectory())
        for (const auto& entry : juce::RangedDirectoryIterator(folder, true, session::presets::kWildcard, juce::File::findFiles))
            signature << entry.getFile().getFullPathName() << '|' << entry.getFileSize() << '|'
                      << entry.getModificationTime().toMilliseconds() << '\n';
    return signature;
}
} // namespace

void BrowserPanel::refreshPresets()
{
    presetFolderSignature_ = presetFolderSignature();
    deviceList_->setPresets(session::presets::getAll());
}

juce::StringArray BrowserPanel::getListedPresetNames() const
{
    return deviceList_->getListedPresetNames();
}

juce::String BrowserPanel::getPresetDragDescription(const juce::String& name) const
{
    return deviceList_->getPresetDragDescription(name);
}

void BrowserPanel::refreshPlugins()
{
    auto choices = deviceActions::getNativeChoices();
    for (auto& choice : deviceActions::getPluginChoices(plugins_))
        choices.push_back(std::move(choice));
    deviceList_->setChoices(std::move(choices));

    const int count = plugins_.getKnownPlugins().getNumTypes();
    scanStatus_ = plugins_.isScanning() ? "Scanning plug-ins..." : count == 0 ? "No plug-ins yet: press Scan" : juce::String();
    layoutSections();
    repaint();
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
    scanStatus_ = "Scanning plug-ins...";
    layoutSections();
    repaint();
}

void BrowserPanel::showPluginMenu()
{
    juce::Component::SafePointer<BrowserPanel> safe(this);
    juce::PopupMenu menu;
    menu.addItem("Scan for New Plug-ins", !plugins_.isScanning(), false, [safe] { if (safe != nullptr) safe->scanPlugins(false); });
    menu.addItem("Rescan All", !plugins_.isScanning(), false, [safe] { if (safe != nullptr) safe->scanPlugins(true); });
    menu.addItem("Add Plug-in Folder...", [safe] { if (safe != nullptr) safe->addSearchFolder(); });
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

//==================================================================================================
// Preview

void BrowserPanel::setPreviewEnabled(bool enabled)
{
    previewEnabled_ = enabled;
    settings_.setValue(kPreviewKey, enabled);
    previewStrip_->setPreviewEnabled(enabled);
    if (!enabled)
        preview_.stop();
    previewStrip_->repaint();
    folderTree_->repaint();
}

void BrowserPanel::fileSelected(const juce::File& file)
{
    if (!previewEnabled_ || !file.existsAsFile())
        return;
    preview_.preview(file);
    previewStrip_->repaint();
}

void BrowserPanel::timerCallback()
{
    if (searchDebounce_ > 0 && --searchDebounce_ == 0)
        setFolderSearch(folderSearch_.getText());

    const bool playing = preview_.isPlaying();
    const auto file = playing ? preview_.getCurrentFile() : juce::File();
    if (playing || playing != wasPlaying_)
        previewStrip_->repaint();
    if (file != shownPreviewFile_)
        folderTree_->repaint();
    wasPlaying_ = playing;
    shownPreviewFile_ = file;

    if (++slowTicks_ >= kTimerHz)
    {
        slowTicks_ = 0;
        preview_.collectGarbage();
        if (presetFolderSignature() != presetFolderSignature_)
            refreshPresets();
        if (!plugins_.isScanning() && scanStatus_.startsWith("Scanning"))
            refreshPlugins();
        else if (plugins_.isScanning())
        {
            scanStatus_ = "Scanning plug-ins... " + juce::String(juce::roundToInt(plugins_.getScanProgress() * 100.0f)) + "%";
            repaint(devicesStatus_);
        }
    }
}

//==================================================================================================
// Layout and painting

void BrowserPanel::resized()
{
    layoutSections();
}

void BrowserPanel::layoutSections()
{
    auto area = getLocalBounds().withTrimmedRight(1);
    area.removeFromTop(kHeaderHeight);
    previewStrip_->setBounds(area.removeFromBottom(kStripHeight));

    const int minimum = kSectionHeader + kSearchHeight + 40;
    const int available = std::max(0, area.getHeight() - kDividerHeight);
    const int devicesHeight = juce::jlimit(std::min(minimum, available), std::max(minimum, available - minimum),
                                           juce::roundToInt(available * split_));
    auto devices = area.removeFromTop(devicesHeight);
    divider_->setBounds(area.removeFromTop(kDividerHeight));
    auto folders = area;

    devicesHeader_ = devices.removeFromTop(kSectionHeader);
    {
        auto buttons = devicesHeader_.reduced(6, 4);
        moreButton_.setBounds(buttons.removeFromRight(26));
        buttons.removeFromRight(4);
        scanButton_.setBounds(buttons.removeFromRight(44));
    }
    deviceSearch_.setBounds(devices.removeFromTop(kSearchHeight).reduced(8, 3));
    devicesStatus_ = scanStatus_.isEmpty() ? juce::Rectangle<int>() : devices.removeFromBottom(kStatusHeight);
    deviceList_->setBounds(devices.withTrimmedTop(2));

    foldersHeader_ = folders.removeFromTop(kSectionHeader);
    addFolderButton_.setBounds(foldersHeader_.reduced(6, 4).removeFromRight(26));
    folderSearch_.setBounds(folders.removeFromTop(kSearchHeight).reduced(8, 3));
    folderSearch_.setEnabled(!folders_.isEmpty());
    emptyArea_ = folders;

    const bool empty = folders_.isEmpty();
    folderTree_->setVisible(!empty);
    folderTree_->setBounds(folders.withTrimmedTop(2));
    emptyAddButton_.setVisible(empty);
    emptyAddButton_.setBounds(folders.withSizeKeepingCentre(120, 26).translated(0, 24));
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

    g.setColour(colour::textSecondary);
    g.setFont(label(10.0f));
    g.drawText("DEVICES & PLUG-INS", devicesHeader_.withTrimmedLeft(12), juce::Justification::centredLeft, true);
    g.drawText("FOLDERS", foldersHeader_.withTrimmedLeft(12), juce::Justification::centredLeft, true);

    if (!devicesStatus_.isEmpty())
    {
        g.setColour(colour::textMuted);
        g.setFont(sans(11.0f));
        g.drawText(scanStatus_, devicesStatus_.reduced(12, 0), juce::Justification::centredLeft, true);
    }

    if (folders_.isEmpty())
    {
        g.setColour(colour::textMuted);
        g.setFont(sans(12.0f));
        g.drawFittedText("Add the folders you keep samples in.\nYou can also drop a folder here.",
                         emptyArea_.withSizeKeepingCentre(emptyArea_.getWidth() - 24, 40).translated(0, -18),
                         juce::Justification::centred, 3);
    }

    g.setColour(colour::outline);
    g.fillRect(getLocalBounds().removeFromRight(1));
}

} // namespace dojo::ui
