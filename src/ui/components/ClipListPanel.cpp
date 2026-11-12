#include "ui/components/ClipListPanel.h"

#include "ui/components/TrackDialogs.h"
#include "ui/theme/DojoTheme.h"

#include <algorithm>
#include <cmath>

namespace dojo::ui
{
namespace
{
juce::String formatLength(double seconds)
{
    const auto ms = static_cast<long long>(std::llround(std::max(0.0, seconds) * 1000.0));
    return juce::String::formatted("%lld:%02lld.%03lld", ms / 60000, (ms / 1000) % 60, ms % 1000);
}
} // namespace

ClipListPanel::ClipListPanel(session::Session& session, EditorState& editor)
    : session_(session)
    , editor_(editor)
{
    title_.setText("CLIP LIST", juce::dontSendNotification);
    title_.setFont(theme::label(10.5f));
    title_.setColour(juce::Label::textColourId, theme::colour::textMuted);
    addAndMakeVisible(title_);

    autoCreated_.setTooltip("Also list the clips created by editing (splits, copies, imports)");
    autoCreated_.setWantsKeyboardFocus(false);
    autoCreated_.onClick = [this] { refresh(); };
    addAndMakeVisible(autoCreated_);

    list_.setModel(this);
    list_.setRowHeight(24);
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

void ClipListPanel::setShowingAutoCreated(bool show)
{
    autoCreated_.setToggleState(show, juce::dontSendNotification);
    refresh();
}

void ClipListPanel::refresh()
{
    cancelPendingUpdate();
    items_ = actions::getClipListItems(session_, isShowingAutoCreated());
    list_.updateContent();
    list_.setVisible(!items_.empty()); // (the empty panel says what the list is for)
    list_.repaint();
    repaint();
}

juce::var ClipListPanel::describeDrag(const juce::ValueTree& clip)
{
    return juce::String(kDragPrefix) + juce::String(static_cast<int>(session::clip::getId(clip)));
}

juce::ValueTree ClipListPanel::findDragged(const session::Session& session, const juce::var& description)
{
    const auto text = description.toString();
    if (!text.startsWith(kDragPrefix))
        return {};
    const auto id = static_cast<std::uint32_t>(text.fromFirstOccurrenceOf(kDragPrefix, false, false).getIntValue());
    if (auto entry = session.findClipListEntry(id); entry.isValid())
        return entry;
    return session.findClip(id);
}

juce::var ClipListPanel::getDragSourceDescription(const juce::SparseSet<int>& rows)
{
    if (rows.isEmpty())
        return {};
    const int row = rows[0];
    if (row < 0 || row >= getNumRows())
        return {};
    return describeDrag(items_[static_cast<std::size_t>(row)].clip);
}

void ClipListPanel::renameItem(int row)
{
    if (row < 0 || row >= getNumRows())
        return;
    const auto id = session::clip::getId(items_[static_cast<std::size_t>(row)].clip);
    juce::Component::SafePointer<ClipListPanel> safe(this);
    TextPromptPanel::show(getTopLevelComponent(), "Rename Clip", "Name", session::clip::getName(items_[static_cast<std::size_t>(row)].clip),
                          "Rename", false, [safe, id](const juce::String& name)
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
    if (row < 0 || row >= getNumRows() || !items_[static_cast<std::size_t>(row)].captured)
        return;
    session_.beginTransaction("Remove Clip from Clip List");
    session_.removeClipListEntry(session::clip::getId(items_[static_cast<std::size_t>(row)].clip));
}

void ClipListPanel::listBoxItemDoubleClicked(int row, const juce::MouseEvent&)
{
    renameItem(row);
}

void ClipListPanel::listBoxItemClicked(int row, const juce::MouseEvent& e)
{
    if (row < 0 || row >= getNumRows())
        return;
    const auto& item = items_[static_cast<std::size_t>(row)];
    if (!item.captured && !e.mods.isPopupMenu())
    {
        // A clip on a track: select it there too.
        editor_.setSelectedClips({session::clip::getId(item.clip)});
        return;
    }
    if (!e.mods.isPopupMenu())
        return;

    juce::Component::SafePointer<ClipListPanel> safe(this);
    juce::PopupMenu menu;
    menu.addSectionHeader(session::clip::getName(item.clip));
    menu.addItem("Rename...", [safe, row]
                 {
                     if (safe != nullptr)
                         safe->renameItem(row);
                 });
    menu.addItem("Remove from Clip List", item.captured, false, [safe, row]
                 {
                     if (safe != nullptr)
                         safe->removeItem(row);
                 });
    menu.showMenuAsync(juce::PopupMenu::Options().withMousePosition());
}

void ClipListPanel::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected)
{
    using namespace theme;
    if (row < 0 || row >= getNumRows())
        return;
    const auto& item = items_[static_cast<std::size_t>(row)];
    const auto& c = item.clip;
    if (selected)
        g.fillAll(colour::controlHover);

    // Kind: A(udio), M(IDI) or V(ideo).
    const bool midi = session::clip::isMidi(c);
    const bool video = session::clip::isVideo(c);
    const auto badge = juce::Rectangle<float>(6.0f, 4.0f, 18.0f, static_cast<float>(height) - 8.0f);
    g.setColour(midi ? colour::accent : video ? colour::textSecondary : colour::meterLow);
    g.fillRoundedRectangle(badge, 3.0f);
    g.setColour(colour::windowBackground);
    g.setFont(sansBold(11.0f));
    g.drawText(midi ? "M" : video ? "V" : "A", badge, juce::Justification::centred, false);

    auto area = juce::Rectangle<int>(32, 0, width - 38, height);
    const auto tag = area.removeFromRight(90);
    const auto length = area.removeFromRight(90);
    const auto source = area.removeFromRight(juce::jmax(0, area.getWidth() / 2));

    g.setColour(item.userDefined ? colour::textPrimary : colour::textSecondary);
    g.setFont(item.userDefined ? sansBold(13.0f) : sans(13.0f));
    g.drawText(session::clip::getName(c), area, juce::Justification::centredLeft, true);

    g.setColour(colour::textMuted);
    g.setFont(sans(11.5f));
    g.drawText(midi ? juce::String("MIDI") : session::clip::getFile(c).getFileName(), source, juce::Justification::centredLeft, true);
    g.setFont(mono(11.0f));
    g.drawText(formatLength(session::clip::getLength(c)), length, juce::Justification::centredRight, false);
    g.setFont(label(9.5f));
    g.drawText(item.captured ? "CAPTURED" : item.userDefined ? "USER" : "AUTO", tag, juce::Justification::centredRight, false);
}

void ClipListPanel::resized()
{
    auto area = getLocalBounds().reduced(8, 4);
    auto header = area.removeFromTop(24);
    autoCreated_.setBounds(header.removeFromRight(130));
    title_.setBounds(header);
    area.removeFromTop(4);
    list_.setBounds(area);
}

void ClipListPanel::paint(juce::Graphics& g)
{
    g.fillAll(theme::colour::panel);
    if (items_.empty())
    {
        g.setColour(theme::colour::textMuted);
        g.setFont(theme::sans(13.0f));
        g.drawText("Capture a selection (Clip > Capture) or rename a clip (Clip > Rename) to list it here. "
                   "Drag a clip from here onto a track to place it.",
                   getLocalBounds().reduced(24, 40), juce::Justification::centredTop, true);
    }
}

} // namespace dojo::ui
