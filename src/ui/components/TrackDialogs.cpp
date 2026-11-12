#include "ui/components/TrackDialogs.h"

#include "ui/theme/DojoTheme.h"

#include <array>

namespace dojo::ui
{
namespace
{
constexpr int kRowHeight = 30;
constexpr int kMargin = 14;

void closeDialog(juce::Component& content)
{
    if (auto* window = content.findParentComponentOfClass<juce::DialogWindow>())
        window->exitModalState(0);
}

void launch(std::unique_ptr<juce::Component> content, const juce::String& title, juce::Component* centreAround)
{
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(content.release());
    options.dialogTitle = title;
    options.dialogBackgroundColour = theme::colour::panel;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = false;
    options.resizable = false;
    options.componentToCentreAround = centreAround;
    options.launchAsync();
}

int parseCount(const juce::String& text, int fallback, int maximum)
{
    const auto trimmed = text.trim();
    if (trimmed.isEmpty() || !trimmed.containsOnly("0123456789"))
        return fallback;
    return juce::jlimit(1, maximum, trimmed.getIntValue());
}

void styleLabel(juce::Label& label, const juce::String& text)
{
    label.setText(text, juce::dontSendNotification);
    label.setFont(theme::label(10.5f));
    label.setColour(juce::Label::textColourId, theme::colour::textMuted);
}

constexpr std::array<actions::NewTrackType, 9> kNewTrackTypes{actions::NewTrackType::audio,       actions::NewTrackType::auxInput,
                                                             actions::NewTrackType::instrument,  actions::NewTrackType::midi,
                                                             actions::NewTrackType::vcaMaster,   actions::NewTrackType::masterFader,
                                                             actions::NewTrackType::clickTrack,  actions::NewTrackType::folder,
                                                             actions::NewTrackType::video};
} // namespace

//==================================================================================================
// New Tracks

NewTracksPanel::Row::Row()
{
    count.setText("1", juce::dontSendNotification);
    count.setInputRestrictions(2, "0123456789");
    count.setJustification(juce::Justification::centred);
    // Ids follow session::getAllTrackFormats(): 1 Mono, 2 Stereo, 3 3.1, 4 5.1, 5 7.1, 6 Dolby Digital, 7 DTS.
    const auto formats = session::getAllTrackFormats();
    for (std::size_t i = 0; i < formats.size(); ++i)
        format.addItem(session::getDisplayName(formats[i]), static_cast<int>(i) + 1);
    format.setSelectedId(2, juce::dontSendNotification);
    for (std::size_t i = 0; i < kNewTrackTypes.size(); ++i)
        type.addItem(actions::getName(kNewTrackTypes[i]), static_cast<int>(i) + 1);
    type.setSelectedId(1, juce::dontSendNotification);
    name.setTextToShowWhenEmpty("(default names)", theme::colour::textMuted);
    add.setTooltip("Add a row below");
    remove.setTooltip("Remove this row");
    for (auto* c : std::initializer_list<juce::Component*>{&count, &format, &type, &name, &add, &remove})
    {
        c->setWantsKeyboardFocus(c == &count || c == &name);
        addAndMakeVisible(c);
    }
}

void NewTracksPanel::Row::resized()
{
    auto area = getLocalBounds().reduced(0, 3);
    count.setBounds(area.removeFromLeft(48));
    area.removeFromLeft(8);
    format.setBounds(area.removeFromLeft(150));
    area.removeFromLeft(8);
    type.setBounds(area.removeFromLeft(150));
    area.removeFromLeft(8);
    remove.setBounds(area.removeFromRight(28));
    area.removeFromRight(4);
    add.setBounds(area.removeFromRight(28));
    area.removeFromRight(8);
    name.setBounds(area);
}

NewTracksPanel::NewTracksPanel()
{
    afterSelection_.setToggleState(true, juce::dontSendNotification);
    afterSelection_.setWantsKeyboardFocus(false);
    addAndMakeVisible(afterSelection_);

    createButton_.onClick = [this] { create(); };
    cancelButton_.onClick = [this] { closeDialog(*this); };
    createButton_.setWantsKeyboardFocus(false);
    cancelButton_.setWantsKeyboardFocus(false);
    addAndMakeVisible(createButton_);
    addAndMakeVisible(cancelButton_);

    addRow(-1);
}

NewTracksPanel::~NewTracksPanel() = default;

void NewTracksPanel::addRow(int after)
{
    if (getNumRows() >= kMaxRows)
        return;

    auto row = std::make_unique<Row>();
    auto* raw = row.get();
    // A new row starts like the one it is added below.
    if (after >= 0 && after < getNumRows())
    {
        const auto& source = *rows_[static_cast<std::size_t>(after)];
        raw->format.setSelectedId(source.format.getSelectedId(), juce::dontSendNotification);
        raw->type.setSelectedId(source.type.getSelectedId(), juce::dontSendNotification);
    }
    raw->type.onChange = [this, raw] { updateRow(*raw); };
    raw->add.onClick = [this, raw]
    {
        for (int i = 0; i < getNumRows(); ++i)
            if (rows_[static_cast<std::size_t>(i)].get() == raw)
            {
                addRow(i);
                return;
            }
    };
    raw->remove.onClick = [this, raw]
    {
        for (int i = 0; i < getNumRows(); ++i)
            if (rows_[static_cast<std::size_t>(i)].get() == raw)
            {
                juce::MessageManager::callAsync([safe = juce::Component::SafePointer<NewTracksPanel>(this), i]
                                                { if (safe != nullptr) safe->removeRow(i); });
                return;
            }
    };
    raw->count.onReturnKey = [this] { create(); };
    raw->name.onReturnKey = [this] { create(); };
    addAndMakeVisible(*raw);
    updateRow(*raw);

    const auto position = after >= 0 && after < getNumRows() ? rows_.begin() + after + 1 : rows_.end();
    rows_.insert(position, std::move(row));
    layoutWindow();
}

void NewTracksPanel::removeRow(int index)
{
    if (getNumRows() <= 1 || index < 0 || index >= getNumRows())
        return;
    rows_.erase(rows_.begin() + index);
    layoutWindow();
}

void NewTracksPanel::updateRow(Row& row)
{
    const auto type = kNewTrackTypes[static_cast<std::size_t>(juce::jlimit(1, static_cast<int>(kNewTrackTypes.size()), row.type.getSelectedId()) - 1)];
    const bool choice = actions::hasFormatChoice(type);
    row.format.setEnabled(choice);
    if (!choice)
        row.format.setSelectedId(2, juce::dontSendNotification);
    for (auto& r : rows_)
        r->remove.setEnabled(getNumRows() > 1);
}

std::vector<actions::NewTracks> NewTracksPanel::getRows() const
{
    std::vector<actions::NewTracks> result;
    for (const auto& row : rows_)
    {
        actions::NewTracks r;
        r.count = parseCount(row->count.getText(), 1, 99);
        r.type = kNewTrackTypes[static_cast<std::size_t>(juce::jlimit(1, static_cast<int>(kNewTrackTypes.size()), row->type.getSelectedId()) - 1)];
        const auto formats = session::getAllTrackFormats();
        const int formatIndex = juce::jlimit(1, static_cast<int>(formats.size()), row->format.getSelectedId()) - 1;
        r.format = actions::hasFormatChoice(r.type) ? formats[static_cast<std::size_t>(formatIndex)] : session::TrackFormat::stereo;
        r.name = row->name.getText().trim();
        result.push_back(r);
    }
    return result;
}

void NewTracksPanel::create()
{
    const auto rows = getRows();
    const bool after = isAfterSelection();
    auto callback = onCreate;
    closeDialog(*this); // (may delete this)
    if (callback)
        callback(rows, after);
}

void NewTracksPanel::layoutWindow()
{
    for (auto& row : rows_)
        row->remove.setEnabled(getNumRows() > 1);
    setSize(700, kMargin + 18 + getNumRows() * kRowHeight + 92);
    resized();
}

void NewTracksPanel::resized()
{
    auto area = getLocalBounds().reduced(kMargin);
    area.removeFromTop(18); // column headings
    for (auto& row : rows_)
        row->setBounds(area.removeFromTop(kRowHeight));
    area.removeFromTop(10);
    afterSelection_.setBounds(area.removeFromTop(24));
    auto buttons = area.removeFromBottom(30);
    createButton_.setBounds(buttons.removeFromRight(100));
    buttons.removeFromRight(8);
    cancelButton_.setBounds(buttons.removeFromRight(90));
}

void NewTracksPanel::paint(juce::Graphics& g)
{
    g.fillAll(theme::colour::panel);
    if (rows_.empty())
        return;
    // Column headings over the first row's controls.
    const auto& row = *rows_.front();
    g.setColour(theme::colour::textMuted);
    g.setFont(theme::label(10.5f));
    for (const auto& [control, text] : {std::pair<const juce::Component*, const char*>{&row.count, "COUNT"}, {&row.format, "FORMAT"},
                                        {&row.type, "TYPE"}, {&row.name, "NAME"}})
        g.drawText(text, row.getX() + control->getX(), kMargin, control->getWidth(), 16, juce::Justification::centredLeft, false);
}

void NewTracksPanel::show(juce::Component* centreAround, std::function<void(const std::vector<actions::NewTracks>&, bool)> onCreate)
{
    auto panel = std::make_unique<NewTracksPanel>();
    panel->onCreate = std::move(onCreate);
    launch(std::move(panel), "New Tracks", centreAround);
}

//==================================================================================================
// Duplicate Tracks

DuplicateTracksPanel::DuplicateTracksPanel(int numSelected, const actions::DuplicateSettings& initial)
{
    styleLabel(copiesLabel_, numSelected > 1 ? "Number of duplicates of each of the " + juce::String(numSelected) + " tracks"
                                             : juce::String("Number of duplicates"));
    styleLabel(takeLabel_, "Copy");
    addAndMakeVisible(copiesLabel_);
    addAndMakeVisible(takeLabel_);

    copies.setText(juce::String(initial.copies), juce::dontSendNotification);
    copies.setInputRestrictions(2, "0123456789");
    copies.setJustification(juce::Justification::centred);
    copies.onReturnKey = [this] { duplicate(); };
    addAndMakeVisible(copies);

    clips.setToggleState(initial.options.clips, juce::dontSendNotification);
    automation.setToggleState(initial.options.automation, juce::dontSendNotification);
    devices.setToggleState(initial.options.devices, juce::dontSendNotification);
    sends.setToggleState(initial.options.sends, juce::dontSendNotification);
    vcaAssignment.setToggleState(initial.options.vcaAssignment, juce::dontSendNotification);
    afterLastSelected.setToggleState(initial.afterLastSelected, juce::dontSendNotification);
    afterLastSelected.setEnabled(numSelected > 1);
    for (auto* toggle : {&clips, &automation, &devices, &sends, &vcaAssignment, &afterLastSelected})
    {
        toggle->setWantsKeyboardFocus(false);
        addAndMakeVisible(toggle);
    }

    duplicateButton_.onClick = [this] { duplicate(); };
    cancelButton_.onClick = [this] { closeDialog(*this); };
    duplicateButton_.setWantsKeyboardFocus(false);
    cancelButton_.setWantsKeyboardFocus(false);
    addAndMakeVisible(duplicateButton_);
    addAndMakeVisible(cancelButton_);

    setSize(420, 326);
}

actions::DuplicateSettings DuplicateTracksPanel::getSettings() const
{
    actions::DuplicateSettings settings;
    settings.copies = parseCount(copies.getText(), 1, 99);
    settings.options.clips = clips.getToggleState();
    settings.options.automation = automation.getToggleState();
    settings.options.devices = devices.getToggleState();
    settings.options.sends = sends.getToggleState();
    settings.options.vcaAssignment = vcaAssignment.getToggleState();
    settings.afterLastSelected = afterLastSelected.isEnabled() && afterLastSelected.getToggleState();
    return settings;
}

void DuplicateTracksPanel::duplicate()
{
    const auto settings = getSettings();
    auto callback = onDuplicate;
    closeDialog(*this);
    if (callback)
        callback(settings);
}

void DuplicateTracksPanel::resized()
{
    auto area = getLocalBounds().reduced(kMargin);
    auto top = area.removeFromTop(28);
    copies.setBounds(top.removeFromRight(56));
    copiesLabel_.setBounds(top);
    area.removeFromTop(10);
    takeLabel_.setBounds(area.removeFromTop(18));
    for (auto* toggle : {&clips, &automation, &devices, &sends, &vcaAssignment})
        toggle->setBounds(area.removeFromTop(26).withTrimmedLeft(8));
    area.removeFromTop(8);
    afterLastSelected.setBounds(area.removeFromTop(26));
    auto buttons = area.removeFromBottom(30);
    duplicateButton_.setBounds(buttons.removeFromRight(100));
    buttons.removeFromRight(8);
    cancelButton_.setBounds(buttons.removeFromRight(90));
}

void DuplicateTracksPanel::paint(juce::Graphics& g)
{
    g.fillAll(theme::colour::panel);
}

void DuplicateTracksPanel::show(juce::Component* centreAround, int numSelected, const actions::DuplicateSettings& initial,
                                std::function<void(const actions::DuplicateSettings&)> onDuplicate)
{
    auto panel = std::make_unique<DuplicateTracksPanel>(numSelected, initial);
    panel->onDuplicate = std::move(onDuplicate);
    launch(std::move(panel), numSelected > 1 ? "Duplicate Tracks" : "Duplicate Track", centreAround);
}

//==================================================================================================
// Scroll to Track

ScrollToTrackPanel::ScrollToTrackPanel(juce::StringArray names, int current)
    : names_(std::move(names))
{
    styleLabel(label_, "Track number (1 - " + juce::String(names_.size()) + ")");
    addAndMakeVisible(label_);

    number.setInputRestrictions(4, "0123456789");
    number.setJustification(juce::Justification::centred);
    if (current >= 1 && current <= names_.size())
        number.setText(juce::String(current), juce::dontSendNotification);
    number.onReturnKey = [this] { confirm(); };
    number.onTextChange = [this]
    {
        if (const int n = getNumber(); n > 0)
        {
            list_.selectRow(n - 1, false, true);
            list_.scrollToEnsureRowIsOnscreen(n - 1);
        }
    };
    addAndMakeVisible(number);

    list_.setModel(this);
    list_.setRowHeight(22);
    list_.setColour(juce::ListBox::backgroundColourId, theme::colour::panelRaised);
    list_.setWantsKeyboardFocus(false);
    addAndMakeVisible(list_);
    if (current >= 1 && current <= names_.size())
        list_.selectRow(current - 1);

    okButton_.onClick = [this] { confirm(); };
    cancelButton_.onClick = [this] { closeDialog(*this); };
    okButton_.setWantsKeyboardFocus(false);
    cancelButton_.setWantsKeyboardFocus(false);
    addAndMakeVisible(okButton_);
    addAndMakeVisible(cancelButton_);

    setSize(360, 380);
}

int ScrollToTrackPanel::getNumber() const
{
    const auto text = number.getText().trim();
    if (text.isEmpty() || !text.containsOnly("0123456789"))
        return 0;
    const int n = text.getIntValue();
    return n >= 1 && n <= names_.size() ? n : 0;
}

void ScrollToTrackPanel::confirm()
{
    const int n = getNumber();
    if (n == 0)
    {
        number.setColour(juce::TextEditor::outlineColourId, theme::colour::record);
        number.repaint();
        return;
    }
    auto callback = onScroll;
    closeDialog(*this);
    if (callback)
        callback(n);
}

void ScrollToTrackPanel::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (selected)
        g.fillAll(theme::colour::controlHover);
    g.setColour(theme::colour::textMuted);
    g.setFont(theme::mono(11.0f));
    g.drawText(juce::String(row + 1), 6, 0, 34, height, juce::Justification::centredRight, false);
    g.setColour(theme::colour::textPrimary);
    g.setFont(theme::sans(13.0f));
    g.drawText(names_[row], 50, 0, width - 56, height, juce::Justification::centredLeft, true);
}

void ScrollToTrackPanel::listBoxItemClicked(int row, const juce::MouseEvent&)
{
    number.setText(juce::String(row + 1), juce::dontSendNotification);
}

void ScrollToTrackPanel::listBoxItemDoubleClicked(int row, const juce::MouseEvent&)
{
    number.setText(juce::String(row + 1), juce::dontSendNotification);
    confirm();
}

void ScrollToTrackPanel::resized()
{
    auto area = getLocalBounds().reduced(kMargin);
    auto top = area.removeFromTop(28);
    number.setBounds(top.removeFromRight(72));
    label_.setBounds(top);
    area.removeFromTop(8);
    auto buttons = area.removeFromBottom(30);
    okButton_.setBounds(buttons.removeFromRight(100));
    buttons.removeFromRight(8);
    cancelButton_.setBounds(buttons.removeFromRight(90));
    area.removeFromBottom(10);
    list_.setBounds(area);
}

void ScrollToTrackPanel::paint(juce::Graphics& g)
{
    g.fillAll(theme::colour::panel);
}

void ScrollToTrackPanel::show(juce::Component* centreAround, juce::StringArray names, int current, std::function<void(int)> onScroll)
{
    auto panel = std::make_unique<ScrollToTrackPanel>(std::move(names), current);
    panel->onScroll = std::move(onScroll);
    auto* raw = panel.get();
    launch(std::move(panel), "Scroll to Track", centreAround);
    raw->number.grabKeyboardFocus();
    raw->number.selectAll();
}

//==================================================================================================
// Text prompt

TextPromptPanel::TextPromptPanel(const juce::String& label, const juce::String& initial, const juce::String& okText, bool numeric)
    : numeric_(numeric)
    , okButton_(okText)
{
    styleLabel(label_, label);
    addAndMakeVisible(label_);

    if (numeric_)
    {
        text.setInputRestrictions(8, "0123456789.");
        text.setJustification(juce::Justification::centred);
    }
    text.setText(initial, juce::dontSendNotification);
    text.onReturnKey = [this] { confirm(); };
    addAndMakeVisible(text);

    okButton_.onClick = [this] { confirm(); };
    cancelButton_.onClick = [this] { closeDialog(*this); };
    okButton_.setWantsKeyboardFocus(false);
    cancelButton_.setWantsKeyboardFocus(false);
    addAndMakeVisible(okButton_);
    addAndMakeVisible(cancelButton_);

    setSize(360, 130);
}

void TextPromptPanel::confirm()
{
    const auto value = text.getText().trim();
    const bool valid = numeric_ ? value.containsOnly("0123456789.") && value.getDoubleValue() > 0.0 : value.isNotEmpty();
    if (!valid)
    {
        text.setColour(juce::TextEditor::outlineColourId, theme::colour::record);
        text.repaint();
        return;
    }
    auto callback = onConfirm;
    closeDialog(*this);
    if (callback)
        callback(value);
}

void TextPromptPanel::resized()
{
    auto area = getLocalBounds().reduced(kMargin);
    label_.setBounds(area.removeFromTop(20));
    area.removeFromTop(4);
    text.setBounds(area.removeFromTop(28));
    auto buttons = area.removeFromBottom(30);
    okButton_.setBounds(buttons.removeFromRight(100));
    buttons.removeFromRight(8);
    cancelButton_.setBounds(buttons.removeFromRight(90));
}

void TextPromptPanel::paint(juce::Graphics& g)
{
    g.fillAll(theme::colour::panel);
}

void TextPromptPanel::show(juce::Component* centreAround, const juce::String& title, const juce::String& label, const juce::String& initial,
                           const juce::String& okText, bool numeric, std::function<void(const juce::String&)> onConfirm)
{
    auto panel = std::make_unique<TextPromptPanel>(label, initial, okText, numeric);
    panel->onConfirm = std::move(onConfirm);
    auto* raw = panel.get();
    launch(std::move(panel), title, centreAround);
    raw->text.grabKeyboardFocus();
    raw->text.selectAll();
}

} // namespace dojo::ui
