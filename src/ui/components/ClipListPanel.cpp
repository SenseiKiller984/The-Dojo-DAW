#include "ui/components/ClipListPanel.h"

#include "ui/EditActions.h"
#include "ui/components/TrackDialogs.h"
#include "ui/theme/DojoTheme.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <set>

namespace dojo::ui
{
namespace
{
constexpr int kHeaderHeight = 28;
constexpr int kColumnHeaderHeight = 18;
constexpr const char* kSettingsKey = "clipListOptions";

juce::String formatLength(double seconds)
{
    const auto ms = static_cast<long long>(std::llround(std::max(0.0, seconds) * 1000.0));
    return juce::String::formatted("%lld:%02lld.%03lld", ms / 60000, (ms / 1000) % 60, ms % 1000);
}

juce::Colour kindColour(cliplist::Kind kind)
{
    switch (kind)
    {
    case cliplist::Kind::audio: return theme::colour::meterLow;
    case cliplist::Kind::midi:  return theme::colour::accent;
    case cliplist::Kind::video: return theme::colour::textSecondary;
    case cliplist::Kind::group: return theme::colour::gold;
    }
    return theme::colour::textSecondary;
}

/** The Elastic Audio mark: two arrows pulling apart. */
void drawElasticMark(juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour)
{
    const auto box = area.withSizeKeepingCentre(14.0f, 8.0f);
    juce::Path p;
    p.startNewSubPath(box.getX() + 3.0f, box.getCentreY());
    p.lineTo(box.getRight() - 3.0f, box.getCentreY());
    p.addTriangle(box.getX(), box.getCentreY(), box.getX() + 4.0f, box.getY(), box.getX() + 4.0f, box.getBottom());
    p.addTriangle(box.getRight(), box.getCentreY(), box.getRight() - 4.0f, box.getY(), box.getRight() - 4.0f, box.getBottom());
    g.setColour(colour);
    g.strokePath(p, juce::PathStrokeType(1.3f));
    g.fillPath(p);
}
} // namespace

ClipListPanel::ClipListPanel(session::Session& session, EditorState& editor)
    : session_(session)
    , editor_(editor)
{
    menuButton_.setButtonText("CLIP LIST  v");
    menuButton_.setTooltip("Show, Select, Sort By, Rename, Auto Rename, Time Stamp, Export");
    menuButton_.setWantsKeyboardFocus(false);
    menuButton_.onClick = [this]
    {
        juce::Component::SafePointer<ClipListPanel> safe(this);
        buildMenu().showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&menuButton_));
    };
    addAndMakeVisible(menuButton_);

    status_.setFont(theme::label(10.0f));
    status_.setColour(juce::Label::textColourId, theme::colour::textMuted);
    addAndMakeVisible(status_);

    list_.setModel(this);
    list_.setRowHeight(24);
    list_.setMultipleSelectionEnabled(true);
    list_.setColour(juce::ListBox::backgroundColourId, theme::colour::panelRaised);
    list_.setWantsKeyboardFocus(false);
    addAndMakeVisible(list_);

    session_.getState().addListener(this);
    refresh();
}

ClipListPanel::~ClipListPanel()
{
    session_.getState().removeListener(this);
}

void ClipListPanel::setOptions(const cliplist::Options& options)
{
    options_ = options;
    saveOptions();
    refresh();
}

void ClipListPanel::setShowingAutoCreated(bool show)
{
    auto options = options_;
    options.show.autoCreated = show;
    setOptions(options);
}

void ClipListPanel::setSettings(juce::PropertySet* settings)
{
    settings_ = settings;
    if (settings_ != nullptr && settings_->containsKey(kSettingsKey))
    {
        options_ = cliplist::Options::fromString(settings_->getValue(kSettingsKey));
        refresh();
    }
}

void ClipListPanel::saveOptions()
{
    if (settings_ != nullptr)
        settings_->setValue(kSettingsKey, options_.toString());
}

void ClipListPanel::refresh()
{
    cancelPendingUpdate();

    // Keep the selection (by what the rows are) across the refresh.
    const auto keyOf = [](const cliplist::Item& item)
    { return item.kind == cliplist::Kind::group ? -static_cast<int>(item.groupId) : static_cast<int>(session::clip::getId(item.clip)); };
    std::set<int> selectedKeys;
    for (const auto& item : getSelectedItems())
        selectedKeys.insert(keyOf(item));

    files_.clear(); // (files may have appeared, gone or changed)
    items_ = cliplist::getItems(session_, options_, files_);
    list_.updateContent();

    juce::SparseSet<int> rows;
    for (std::size_t i = 0; i < items_.size(); ++i)
        if (selectedKeys.count(keyOf(items_[i])) > 0)
            rows.addRange({static_cast<int>(i), static_cast<int>(i) + 1});
    syncingSelection_ = true;
    list_.setSelectedRows(rows, juce::dontSendNotification);
    syncingSelection_ = false;

    list_.setVisible(!items_.empty()); // (the empty panel says what the list is for)
    juce::String text;
    text << static_cast<int>(items_.size()) << (items_.size() == 1 ? " clip" : " clips");
    const auto unused = cliplist::select(items_, cliplist::Selection::unused).size();
    const auto offline = cliplist::select(items_, cliplist::Selection::offline).size();
    if (unused > 0)
        text << "  |  " << static_cast<int>(unused) << " unused";
    if (offline > 0)
        text << "  |  " << static_cast<int>(offline) << " offline";
    if (options_.sort != cliplist::SortKey::none)
        text << "  |  by " << cliplist::getSortKeyName(options_.sort) << (options_.ascending ? " (ascending)" : " (descending)");
    status_.setText(text, juce::dontSendNotification);
    list_.repaint();
    repaint();
}

// --- Selection ---------------------------------------------------------------------------------------

void ClipListPanel::selectRows(cliplist::Selection selection)
{
    juce::SparseSet<int> rows;
    for (const int row : cliplist::select(items_, selection))
        rows.addRange({row, row + 1});
    list_.setSelectedRows(rows);
}

void ClipListPanel::selectRow(int row, bool add)
{
    list_.selectRow(row, false, !add);
}

std::vector<cliplist::Item> ClipListPanel::getSelectedItems() const
{
    std::vector<cliplist::Item> selected;
    const auto rows = list_.getSelectedRows();
    for (int i = 0; i < rows.size(); ++i)
        if (rows[i] >= 0 && rows[i] < static_cast<int>(items_.size()))
            selected.push_back(items_[static_cast<std::size_t>(rows[i])]);
    return selected;
}

void ClipListPanel::selectedRowsChanged(int)
{
    if (syncingSelection_)
        return;
    // Rows that are clips on tracks (or groups) select those clips in the arranger too.
    std::set<std::uint32_t> clips;
    for (const auto& item : getSelectedItems())
    {
        if (item.kind == cliplist::Kind::group)
            for (const auto& c : session_.getGroupMembers(item.groupId))
                clips.insert(session::clip::getId(c));
        else if (!item.entry)
            clips.insert(session::clip::getId(item.clip));
    }
    if (!clips.empty())
        editor_.setSelectedClips(std::move(clips));
}

// --- Commands ----------------------------------------------------------------------------------------

void ClipListPanel::renameItem(int row)
{
    if (row < 0 || row >= getNumRows())
        return;
    const auto item = items_[static_cast<std::size_t>(row)];
    if (item.kind == cliplist::Kind::group)
        return;
    const auto id = session::clip::getId(item.clip);
    juce::Component::SafePointer<ClipListPanel> safe(this);
    TextPromptPanel::show(getTopLevelComponent(), "Rename Clip", "Name", session::clip::getName(item.clip), "Rename", false,
                          [safe, id](const juce::String& name)
                          {
                              if (safe == nullptr)
                                  return;
                              auto clip = safe->session_.findClipListEntry(id);
                              if (!clip.isValid())
                                  clip = safe->session_.findClip(id);
                              actions::renameClip(safe->session_, clip, name);
                          });
}

void ClipListPanel::removeItem(int row)
{
    if (row < 0 || row >= getNumRows())
        return;
    cliplist::removeEntries(session_, {items_[static_cast<std::size_t>(row)]});
}

void ClipListPanel::removeSelected()
{
    cliplist::removeEntries(session_, getSelectedItems());
}

void ClipListPanel::autoRenameSelected()
{
    const auto clips = cliplist::clipsOf(getSelectedItems());
    if (clips.empty())
        return;
    std::vector<std::uint32_t> ids;
    for (const auto& c : clips)
        ids.push_back(session::clip::getId(c));
    const auto root = clips.front().getProperty(session::ids::autoNameRoot, session::clip::getName(clips.front()) + "_").toString();
    juce::Component::SafePointer<ClipListPanel> safe(this);
    AutoRenamePanel::show(getTopLevelComponent(), root, session_.getNextAutoNumber(root), static_cast<int>(clips.size()),
                          [safe, ids](const juce::String& name, int number, int zeros, const juce::String& suffix)
                          {
                              if (safe == nullptr)
                                  return;
                              std::vector<juce::ValueTree> targets;
                              for (const auto id : ids)
                              {
                                  auto c = safe->session_.findClipListEntry(id);
                                  if (!c.isValid())
                                      c = safe->session_.findClip(id);
                                  if (c.isValid())
                                      targets.push_back(c);
                              }
                              cliplist::autoRename(safe->session_, targets, name, number, zeros, suffix);
                          });
}

void ClipListPanel::timeStampSelected()
{
    const auto clips = cliplist::clipsOf(getSelectedItems());
    if (clips.empty())
        return;
    std::vector<std::uint32_t> ids;
    for (const auto& c : clips)
        ids.push_back(session::clip::getId(c));
    juce::Component::SafePointer<ClipListPanel> safe(this);
    TimeStampPanel::show(getTopLevelComponent(), session::clip::getName(clips.front()), static_cast<int>(clips.size()),
                         session_.getOriginalTimeStamp(clips.front()), session_.getUserTimeStamp(clips.front()),
                         getPlayheadSeconds ? getPlayheadSeconds() : 0.0,
                         [safe, ids](double seconds)
                         {
                             if (safe == nullptr)
                                 return;
                             std::vector<juce::ValueTree> targets;
                             for (const auto id : ids)
                             {
                                 auto c = safe->session_.findClipListEntry(id);
                                 if (!c.isValid())
                                     c = safe->session_.findClip(id);
                                 if (c.isValid())
                                     targets.push_back(c);
                             }
                             cliplist::setUserTimeStamps(safe->session_, targets, seconds);
                         });
}

void ClipListPanel::exportSelected()
{
    const auto items = getSelectedItems();
    if (items.empty())
        return;
    chooser_ = std::make_unique<juce::FileChooser>("Export Clips as Files to...",
                                                   juce::File::getSpecialLocation(juce::File::userMusicDirectory));
    juce::Component::SafePointer<ClipListPanel> safe(this);
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [safe, items](const juce::FileChooser& chooser)
                          {
                              if (safe == nullptr || chooser.getResult() == juce::File())
                                  return;
                              juce::String error;
                              const int written = cliplist::exportClips(safe->session_, items, chooser.getResult(), error);
                              juce::String message;
                              message << written << (written == 1 ? " clip" : " clips") << " exported to "
                                      << chooser.getResult().getFullPathName();
                              if (error.isNotEmpty())
                                  message << "\n\n" << error;
                              juce::AlertWindow::showMessageBoxAsync(error.isEmpty() ? juce::MessageBoxIconType::InfoIcon
                                                                                     : juce::MessageBoxIconType::WarningIcon,
                                                                     "Export Clips as Files", message);
                          });
}

juce::PopupMenu ClipListPanel::buildMenu()
{
    juce::Component::SafePointer<ClipListPanel> safe(this);
    const auto change = [safe](std::function<void(cliplist::Options&)> edit)
    {
        return [safe, edit]
        {
            if (safe == nullptr)
                return;
            auto options = safe->options_;
            edit(options);
            safe->setOptions(options);
        };
    };
    const auto& show = options_.show;

    juce::PopupMenu showMenu;
    showMenu.addSectionHeader("Clip Types");
    showMenu.addItem("Audio", true, show.audio, change([](auto& o) { o.show.audio = !o.show.audio; }));
    showMenu.addItem("MIDI", true, show.midi, change([](auto& o) { o.show.midi = !o.show.midi; }));
    showMenu.addItem("Video", true, show.video, change([](auto& o) { o.show.video = !o.show.video; }));
    showMenu.addItem("Groups", true, show.groups, change([](auto& o) { o.show.groups = !o.show.groups; }));
    showMenu.addItem("Auto-Created", true, show.autoCreated, change([](auto& o) { o.show.autoCreated = !o.show.autoCreated; }));
    showMenu.addSectionHeader("Clip Information");
    showMenu.addItem("Color", true, show.colour, change([](auto& o) { o.show.colour = !o.show.colour; }));
    showMenu.addItem("File Type", true, show.fileType, change([](auto& o) { o.show.fileType = !o.show.fileType; }));
    showMenu.addItem("Timebase", true, show.timebase, change([](auto& o) { o.show.timebase = !o.show.timebase; }));
    showMenu.addItem("Processing State", true, show.processing, change([](auto& o) { o.show.processing = !o.show.processing; }));
    showMenu.addItem("File Name", true, show.fileName, change([](auto& o) { o.show.fileName = !o.show.fileName; }));
    showMenu.addItem("Disk Name", true, show.diskName, change([](auto& o) { o.show.diskName = !o.show.diskName; }));

    juce::PopupMenu selectMenu;
    const auto selectItem = [&](const char* name, cliplist::Selection selection)
    {
        const bool any = !cliplist::select(items_, selection).empty();
        selectMenu.addItem(name, any, false, [safe, selection]
                           {
                               if (safe != nullptr)
                                   safe->selectRows(selection);
                           });
    };
    selectItem("All", cliplist::Selection::all);
    selectItem("Unused", cliplist::Selection::unused);
    selectItem("Unused Except Whole Files", cliplist::Selection::unusedExceptWholeFiles);
    selectItem("Offline", cliplist::Selection::offline);

    juce::PopupMenu sortMenu;
    for (int k = 0; k < cliplist::kNumSortKeys; ++k)
    {
        const auto key = static_cast<cliplist::SortKey>(k);
        if (key == cliplist::SortKey::fileName)
            sortMenu.addSectionHeader("Audio and video only");
        sortMenu.addItem(cliplist::getSortKeyName(key), true, options_.sort == key, change([key](auto& o) { o.sort = key; }));
    }
    sortMenu.addSeparator();
    sortMenu.addItem("Ascending", true, options_.ascending, change([](auto& o) { o.ascending = true; }));
    sortMenu.addItem("Descending", true, !options_.ascending, change([](auto& o) { o.ascending = false; }));

    const auto selected = getSelectedItems();
    const auto clips = cliplist::clipsOf(selected);
    const bool anyEntry = std::any_of(selected.begin(), selected.end(), [](const auto& i) { return i.entry; });

    juce::PopupMenu menu;
    menu.addSubMenu("Show", showMenu);
    menu.addSubMenu("Select", selectMenu);
    menu.addSubMenu("Sort By", sortMenu);
    menu.addSeparator();
    const int firstRow = list_.getSelectedRow(0);
    menu.addItem("Rename...", clips.size() == 1, false, [safe, firstRow]
                 {
                     if (safe != nullptr)
                         safe->renameItem(firstRow);
                 });
    menu.addItem("Auto Rename...", !clips.empty(), false, [safe] { if (safe != nullptr) safe->autoRenameSelected(); });
    menu.addItem("Time Stamp...", !clips.empty(), false, [safe] { if (safe != nullptr) safe->timeStampSelected(); });
    menu.addSeparator();
    menu.addItem("Export Clips as Files...", !clips.empty(), false, [safe] { if (safe != nullptr) safe->exportSelected(); });
    menu.addItem("Remove from Clip List", anyEntry, false, [safe] { if (safe != nullptr) safe->removeSelected(); });
    return menu;
}

// --- List ---------------------------------------------------------------------------------------------

juce::var ClipListPanel::getDragSourceDescription(const juce::SparseSet<int>&)
{
    const auto selected = getSelectedItems();
    return selected.empty() ? juce::var() : cliplist::describeDrag(selected);
}

void ClipListPanel::listBoxItemDoubleClicked(int row, const juce::MouseEvent&)
{
    renameItem(row);
}

void ClipListPanel::listBoxItemClicked(int row, const juce::MouseEvent& e)
{
    if (!e.mods.isPopupMenu() || row < 0 || row >= getNumRows())
        return;
    if (!list_.isRowSelected(row))
        list_.selectRow(row);
    buildMenu().showMenuAsync(juce::PopupMenu::Options().withMousePosition());
}

ClipListPanel::Columns ClipListPanel::layoutColumns(int width, int height) const
{
    const auto& show = options_.show;
    Columns c;
    auto area = juce::Rectangle<int>(0, 0, width, height).reduced(4, 0);
    if (show.colour)
        c.colour = area.removeFromLeft(6).reduced(0, 3);
    area.removeFromLeft(4);
    if (show.fileType)
    {
        c.type = area.removeFromLeft(50);
        area.removeFromLeft(4);
    }
    c.tag = area.removeFromRight(130);
    if (show.diskName)
        c.disk = area.removeFromRight(130);
    c.length = area.removeFromRight(80);
    if (show.processing)
        c.processing = area.removeFromRight(28);
    if (show.timebase)
        c.timebase = area.removeFromRight(70);
    if (show.fileName)
        c.file = area.removeFromRight(area.getWidth() * 2 / 5).withTrimmedLeft(8);
    c.name = area;
    return c;
}

void ClipListPanel::paintHeader(juce::Graphics& g, juce::Rectangle<int> area) const
{
    // (as wide as the rows, which leave room for the list's scroll bar)
    const auto c = layoutColumns(std::min(area.getWidth(), list_.getVisibleRowWidth()), area.getHeight());
    g.setColour(theme::colour::textMuted);
    g.setFont(theme::label(9.0f));
    const auto at = [&area](juce::Rectangle<int> r) { return r.translated(area.getX(), area.getY()); };
    if (!c.type.isEmpty())
        g.drawText("TYPE", at(c.type), juce::Justification::centredLeft, false);
    g.drawText("NAME", at(c.name), juce::Justification::centredLeft, false);
    if (!c.file.isEmpty())
        g.drawText("FILE", at(c.file), juce::Justification::centredLeft, false);
    if (!c.timebase.isEmpty())
        g.drawText("TIMEBASE", at(c.timebase), juce::Justification::centredLeft, false);
    if (!c.processing.isEmpty())
        g.drawText("PROC", at(c.processing), juce::Justification::centred, false);
    g.drawText("LENGTH", at(c.length), juce::Justification::centredRight, false);
    if (!c.disk.isEmpty())
        g.drawText("DISK", at(c.disk).withTrimmedLeft(10), juce::Justification::centredLeft, false);
    g.drawText("KIND", at(c.tag), juce::Justification::centredRight, false);
}

void ClipListPanel::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected)
{
    using namespace theme;
    if (row < 0 || row >= getNumRows())
        return;
    const auto& item = items_[static_cast<std::size_t>(row)];
    if (selected)
        g.fillAll(colour::controlHover);
    const auto c = layoutColumns(width, height);

    if (!c.colour.isEmpty())
    {
        g.setColour(cliplist::getColour(session_, item, colour::outlineStrong));
        g.fillRoundedRectangle(c.colour.toFloat(), 2.0f);
    }

    if (!c.type.isEmpty())
    {
        const auto badge = c.type.toFloat().reduced(0.0f, 4.0f);
        g.setColour(kindColour(item.kind).withAlpha(item.offline ? 0.4f : 1.0f));
        g.fillRoundedRectangle(badge, 3.0f);
        g.setColour(colour::windowBackground);
        g.setFont(sansBold(10.0f));
        g.drawText(cliplist::getFileTypeName(item).toUpperCase(), badge, juce::Justification::centred, false);
    }

    // Name: bold for whole files, italic and dimmed when offline.
    auto font = item.wholeFile ? sansBold(13.0f) : sans(13.0f);
    if (item.offline)
        font = font.italicised();
    g.setFont(font);
    g.setColour(item.offline ? colour::textMuted : (item.autoCreated ? colour::textSecondary : colour::textPrimary));
    g.drawText(cliplist::getName(session_, item), c.name, juce::Justification::centredLeft, true);

    g.setColour(colour::textMuted);
    g.setFont(sans(11.5f));
    if (!c.file.isEmpty())
    {
        const auto file = cliplist::getFile(session_, item);
        g.drawText(item.kind == cliplist::Kind::midi ? juce::String("-") : file.getFileName(), c.file, juce::Justification::centredLeft, true);
    }
    if (!c.timebase.isEmpty())
        g.drawText(cliplist::isTickBased(item) ? "Ticks" : "Samples", c.timebase, juce::Justification::centredLeft, false);
    if (!c.processing.isEmpty() && cliplist::isProcessed(item))
        drawElasticMark(g, c.processing.toFloat(), colour::gold);
    if (!c.disk.isEmpty())
        g.drawText(cliplist::getDiskName(session_, item, files_), c.disk.withTrimmedLeft(10), juce::Justification::centredLeft, true);

    g.setFont(mono(11.0f));
    g.drawText(formatLength(cliplist::getLength(session_, item)), c.length, juce::Justification::centredRight, false);

    juce::String tag = item.kind == cliplist::Kind::group ? "GROUP"
                       : item.wholeFile                   ? "WHOLE FILE"
                       : item.entry                       ? "CAPTURED"
                       : item.userDefined                 ? "USER"
                       : item.autoCreated                 ? "AUTO"
                                                          : "CLIP";
    if (item.offline)
        tag << " / OFFLINE";
    else if (!item.used)
        tag << " / UNUSED";
    g.setFont(label(9.0f));
    g.drawText(tag, c.tag, juce::Justification::centredRight, false);
}

void ClipListPanel::resized()
{
    auto area = getLocalBounds().reduced(8, 4);
    auto header = area.removeFromTop(kHeaderHeight - 4);
    menuButton_.setBounds(header.removeFromLeft(110).reduced(0, 1));
    header.removeFromLeft(10);
    status_.setBounds(header);
    area.removeFromTop(kColumnHeaderHeight);
    list_.setBounds(area);
}

void ClipListPanel::paint(juce::Graphics& g)
{
    g.fillAll(theme::colour::panel);
    if (items_.empty())
    {
        g.setColour(theme::colour::textMuted);
        g.setFont(theme::sans(13.0f));
        g.drawText("Import or record audio, capture a selection (Clip > Capture) or rename a clip (Clip > Rename) to list it here. "
                   "Drag clips from here onto a track to place them. The CLIP LIST menu shows more kinds of clips.",
                   getLocalBounds().reduced(24, 50), juce::Justification::centredTop, true);
        return;
    }
    auto area = getLocalBounds().reduced(8, 4);
    area.removeFromTop(kHeaderHeight - 4);
    paintHeader(g, area.removeFromTop(kColumnHeaderHeight));
}

} // namespace dojo::ui
