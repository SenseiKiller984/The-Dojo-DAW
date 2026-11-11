#include "ui/components/DevicePanel.h"

#include "devices/Dynamics.h"
#include "ui/DeviceActions.h"
#include "ui/components/ArrangerView.h"
#include "ui/devices/DeviceEditors.h"
#include "ui/theme/DojoTheme.h"

namespace dojo::ui
{
namespace
{
constexpr int kHeaderHeight = 26;
constexpr int kCardHeaderHeight = 28;
constexpr int kAddCardWidth = 120;
} // namespace

//==================================================================================================
/** A hosted plug-in's own editor in a window. Keeps the node alive while open. */
class DevicePanel::PluginWindow final : public juce::DocumentWindow
{
public:
    PluginWindow(std::shared_ptr<engine::ProcessorNode> node, const juce::String& title, std::function<void()> onClose)
        : juce::DocumentWindow(title, theme::colour::panel, juce::DocumentWindow::closeButton)
        , node_(std::move(node))
        , onClose_(std::move(onClose))
    {
        // Native title bar: the plug-in's own window is placed at the top-left of the client area.
        setUsingNativeTitleBar(true);
        auto* editor = node_->getProcessor().createEditorAndMakeActive();
        if (editor != nullptr)
        {
            setContentNonOwned(editor, true);
            setResizable(editor->isResizable(), false);
        }
        centreWithSize(std::max(200, getWidth()), std::max(100, getHeight()));
        setVisible(true);
    }

    ~PluginWindow() override
    {
        if (auto* editor = node_->getProcessor().getActiveEditor())
        {
            clearContentComponent();
            delete editor; // tells the processor (editorBeingDeleted) itself
        }
    }

    void closeButtonPressed() override
    {
        if (onClose_)
            juce::MessageManager::callAsync(onClose_);
    }

    [[nodiscard]] bool hasEditor() const { return getContentComponent() != nullptr; }

private:
    std::shared_ptr<engine::ProcessorNode> node_;
    std::function<void()> onClose_;
};

//==================================================================================================
/** One device: header controls above its editor. */
class DeviceCard final : public juce::Component
{
public:
    DeviceCard(DevicePanel& panel, session::Project& project, EditorState& editor, juce::ValueTree device, juce::ValueTree owner)
        : project_(project)
        , device_(std::move(device))
        , owner_(std::move(owner))
    {
        auto& session = project_.getSession();
        auto* um = &session.getUndoManager();
        const auto id = session::device::getId(device_);
        node_ = project_.getDevices().getNode(device_);

        bypass_.setButtonText("ON");
        bypass_.setClickingTogglesState(true);
        bypass_.setWantsKeyboardFocus(false);
        bypass_.setTooltip("Device on / bypassed");
        bypass_.setColour(juce::TextButton::buttonOnColourId, theme::colour::play);
        bypass_.setToggleState(!session::device::isBypassed(device_), juce::dontSendNotification);
        bypass_.onClick = [this, &session, um]
        {
            session.beginTransaction("Bypass Device");
            session::device::setBypassed(device_, !bypass_.getToggleState(), um);
        };

        left_.setButtonText("<");
        right_.setButtonText(">");
        remove_.setButtonText("X");
        left_.setTooltip("Move earlier in the chain");
        right_.setTooltip("Move later in the chain");
        remove_.setTooltip("Remove device");
        for (auto* b : {&left_, &right_, &remove_, &bypass_})
        {
            b->setWantsKeyboardFocus(false);
            addAndMakeVisible(b);
        }

        left_.onClick = [this, &session]
        {
            const auto devices = session::Session::getDevices(owner_);
            const int index = static_cast<int>(std::find(devices.begin(), devices.end(), device_) - devices.begin());
            if (index > 0)
            {
                session.beginTransaction("Move Device");
                session.moveDevice(device_, index - 1);
            }
        };
        right_.onClick = [this, &session]
        {
            const auto devices = session::Session::getDevices(owner_);
            const int index = static_cast<int>(std::find(devices.begin(), devices.end(), device_) - devices.begin());
            if (index + 1 < static_cast<int>(devices.size()))
            {
                session.beginTransaction("Move Device");
                session.moveDevice(device_, index + 1);
            }
        };
        remove_.onClick = [this, &session]
        {
            auto deviceTree = device_;
            juce::MessageManager::callAsync([&session, deviceTree]() mutable
                                            {
                                                session.beginTransaction("Remove Device");
                                                session.removeDevice(deviceTree);
                                            });
        };

        if (node_ != nullptr && node_->getProcessor().hasEditor() && session::device::getType(device_) == plugins::PluginManager::kDeviceType)
        {
            window_.setButtonText("UI");
            window_.setTooltip("Open the plug-in's own window");
            window_.setWantsKeyboardFocus(false);
            window_.onClick = [&panel, this] { panel.openPluginWindow(device_); };
            addAndMakeVisible(window_);
        }

        if (node_ != nullptr && node_->hasSidechainInput())
        {
            sidechain_.setTooltip("Sidechain source");
            sidechain_.setWantsKeyboardFocus(false);
            sidechain_.addItem("No sidechain", 1);
            int itemId = 2;
            for (const auto& t : session.getAllTracks())
            {
                if (t == owner_ || (owner_.hasType(session::ids::track) && !session.canFeed(t, owner_)))
                    continue;
                sidechainIds_.push_back(session::track::getId(t));
                sidechain_.addItem("SC: " + session::track::getName(t), itemId++);
            }
            const auto current = session::device::getSidechain(device_);
            const auto it = std::find(sidechainIds_.begin(), sidechainIds_.end(), current);
            sidechain_.setSelectedId(it != sidechainIds_.end() ? static_cast<int>(it - sidechainIds_.begin()) + 2 : 1, juce::dontSendNotification);
            sidechain_.onChange = [this, &session, um]
            {
                const int index = sidechain_.getSelectedId() - 2;
                const auto source = index >= 0 ? sidechainIds_[static_cast<std::size_t>(index)] : 0u;
                session.beginTransaction("Sidechain");
                session::device::setSidechain(device_, source, um);
                // A compressor listens to its sidechain only when asked to.
                if (auto* compressor = dynamic_cast<devices::Compressor*>(&node_->getProcessor()))
                    if (auto* external = compressor->getParameterById("sidechain"))
                        external->setValueNotifyingHost(source != 0 ? 1.0f : 0.0f);
            };
            addAndMakeVisible(sidechain_);
        }

        if (node_ != nullptr)
        {
            editorComponent_ = createDeviceEditor(*node_, DeviceEditorContext{project_, editor, device_, owner_}, preferredWidth_);
            addAndMakeVisible(*editorComponent_);
        }
        else
        {
            error_ = project_.getDevices().getError(id);
            preferredWidth_ = 260;
        }

        preferredWidth_ = std::max(preferredWidth_, 220);
    }

    [[nodiscard]] int getPreferredWidth() const noexcept { return preferredWidth_; }
    [[nodiscard]] const juce::ValueTree& getDevice() const noexcept { return device_; }

    void syncFromModel()
    {
        bypass_.setToggleState(!session::device::isBypassed(device_), juce::dontSendNotification);
        repaint();
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(1);
        auto header = area.removeFromTop(kCardHeaderHeight).reduced(4, 3);
        remove_.setBounds(header.removeFromRight(22));
        header.removeFromRight(2);
        right_.setBounds(header.removeFromRight(20));
        left_.setBounds(header.removeFromRight(20));
        header.removeFromRight(4);
        if (window_.isVisible())
        {
            window_.setBounds(header.removeFromRight(28));
            header.removeFromRight(4);
        }
        bypass_.setBounds(header.removeFromLeft(34));
        header.removeFromLeft(6);
        if (sidechain_.isVisible())
            sidechain_.setBounds(header.removeFromRight(std::min(130, header.getWidth() / 2)));
        nameArea_ = header;
        if (editorComponent_ != nullptr)
            editorComponent_->setBounds(area.reduced(6, 4));
    }

    void paint(juce::Graphics& g) override
    {
        using namespace theme;
        const bool on = !session::device::isBypassed(device_);
        auto bounds = getLocalBounds().toFloat().reduced(1.0f);
        g.setColour(colour::panelRaised);
        g.fillRoundedRectangle(bounds, 5.0f);
        g.setColour(on ? colour::outlineStrong : colour::outline);
        g.drawRoundedRectangle(bounds, 5.0f, 1.0f);
        g.setColour(colour::panelHeader);
        g.fillRoundedRectangle(bounds.removeFromTop(kCardHeaderHeight), 5.0f);

        g.setColour(on ? colour::textPrimary : colour::textMuted);
        g.setFont(sansBold(12.0f));
        g.drawText(session::device::getName(device_), nameArea_, juce::Justification::centredLeft, true);

        if (node_ == nullptr)
        {
            g.setColour(colour::meterClip);
            g.setFont(sans(12.0f));
            g.drawFittedText("Not available: " + error_, getLocalBounds().withTrimmedTop(kCardHeaderHeight).reduced(10), juce::Justification::centred, 4);
        }
        else if (!on)
        {
            g.setColour(colour::windowBackground.withAlpha(0.45f));
            g.fillRect(getLocalBounds().withTrimmedTop(kCardHeaderHeight + 1).reduced(2));
        }
    }

private:
    session::Project& project_;
    juce::ValueTree device_;
    juce::ValueTree owner_;
    std::shared_ptr<engine::ProcessorNode> node_;
    juce::TextButton bypass_;
    juce::TextButton left_;
    juce::TextButton right_;
    juce::TextButton remove_;
    juce::TextButton window_;
    juce::ComboBox sidechain_;
    std::vector<std::uint32_t> sidechainIds_;
    std::unique_ptr<juce::Component> editorComponent_;
    juce::Rectangle<int> nameArea_;
    juce::String error_;
    int preferredWidth_ = 260;
};

//==================================================================================================
DevicePanel::DevicePanel(session::Project& project, plugins::PluginManager& plugins, EditorState& editor)
    : project_(project)
    , plugins_(plugins)
    , editor_(editor)
{
    setOpaque(true);
    viewport_.setViewedComponent(&holder_, false);
    viewport_.setScrollBarsShown(false, true);
    viewport_.setScrollBarThickness(10);
    addAndMakeVisible(viewport_);

    addButton_.setButtonText("+  Add Device");
    addButton_.setTooltip("Add a native device or a plug-in (or drag one here from the browser)");
    addButton_.onClick = [this] { showAddMenu(); };
    holder_.addAndMakeVisible(addButton_);

    project_.getSession().getState().addListener(this);
    editor_.addListener(this);
    rebuild();
}

DevicePanel::~DevicePanel()
{
    windows_.clear();
    editor_.removeListener(this);
    project_.getSession().getState().removeListener(this);
}

juce::ValueTree DevicePanel::getOwner() const
{
    auto& session = project_.getSession();
    if (editor_.isMasterSelected())
        return session.getMaster();
    return session.findTrack(editor_.getSelectedTrack());
}

void DevicePanel::rebuild()
{
    cancelPendingUpdate();
    cards_.clear();
    shownOwner_ = getOwner();
    shownMaster_ = editor_.isMasterSelected();

    if (shownOwner_.isValid())
        for (const auto& device : session::Session::getDevices(shownOwner_))
        {
            auto card = std::make_unique<DeviceCard>(*this, project_, editor_, device, shownOwner_);
            holder_.addAndMakeVisible(*card);
            cards_.push_back(std::move(card));
        }

    addButton_.setVisible(shownOwner_.isValid() && ownerTakesDevices(shownOwner_));
    closeStaleWindows();
    layoutCards();
    repaint();
}

void DevicePanel::layoutCards()
{
    const int height = std::max(0, viewport_.getMaximumVisibleHeight());
    int x = 4;
    for (auto& card : cards_)
    {
        card->setBounds(x, 4, card->getPreferredWidth(), std::max(0, height - 8));
        x += card->getPreferredWidth() + 6;
    }
    addButton_.setBounds(x, 4, kAddCardWidth, std::max(0, height - 8));
    x += kAddCardWidth + 4;
    holder_.setSize(std::max(x, viewport_.getMaximumVisibleWidth()), std::max(0, height));
}

void DevicePanel::closeStaleWindows()
{
    for (auto it = windows_.begin(); it != windows_.end();)
        it = project_.getSession().findDevice(it->first).isValid() ? std::next(it) : windows_.erase(it);
}

void DevicePanel::openPluginWindow(const juce::ValueTree& device)
{
    const auto id = session::device::getId(device);
    if (const auto it = windows_.find(id); it != windows_.end())
    {
        it->second->toFront(true);
        return;
    }

    auto node = project_.getDevices().findNode(id);
    if (node == nullptr)
        return;

    juce::Component::SafePointer<DevicePanel> safe(this);
    auto window = std::make_unique<PluginWindow>(node, session::device::getName(device),
                                                 [safe, id]
                                                 {
                                                     if (safe != nullptr)
                                                         safe->windows_.erase(id);
                                                 });
    if (window->hasEditor())
        windows_[id] = std::move(window);
}

bool DevicePanel::ownerTakesDevices(const juce::ValueTree& owner)
{
    if (!owner.hasType(session::ids::track))
        return owner.isValid(); // the master
    const auto kind = session::track::getKind(owner);
    return kind != session::TrackKind::vca && kind != session::TrackKind::midi;
}

void DevicePanel::showAddMenu()
{
    const auto owner = getOwner();
    if (!owner.isValid() || !ownerTakesDevices(owner))
        return;

    const bool instrumentsAllowed = owner.hasType(session::ids::track) && session::track::getKind(owner) == session::TrackKind::instrument;
    juce::Component::SafePointer<DevicePanel> safe(this);
    juce::PopupMenu menu;
    deviceActions::buildAddDeviceMenu(menu, plugins_, instrumentsAllowed,
                                      [safe](const deviceActions::DeviceChoice& choice)
                                      {
                                          if (safe != nullptr)
                                              deviceActions::addDevice(safe->project_.getSession(), safe->editor_, safe->getOwner(), choice);
                                      });
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&addButton_));
}

bool DevicePanel::isInterestedInDragSource(const SourceDetails& details)
{
    return details.description.toString().startsWith(ArrangerView::kDeviceDragPrefix);
}

void DevicePanel::itemDropped(const SourceDetails& details)
{
    if (const auto choice = deviceActions::fromDragDescription(details.description.toString(), plugins_))
        deviceActions::addDevice(project_.getSession(), editor_, getOwner(), *choice);
}

void DevicePanel::editorStateChanged()
{
    if (getOwner() != shownOwner_ || editor_.isMasterSelected() != shownMaster_)
        triggerAsyncUpdate();
}

void DevicePanel::valueTreeChildAdded(juce::ValueTree& parent, juce::ValueTree& child)
{
    if (child.hasType(session::ids::device) || parent == shownOwner_)
        triggerAsyncUpdate();
}

void DevicePanel::valueTreeChildRemoved(juce::ValueTree& parent, juce::ValueTree& child, int)
{
    if (child.hasType(session::ids::device) || child == shownOwner_ || parent == shownOwner_ || child.hasType(session::ids::track))
        triggerAsyncUpdate();
}

void DevicePanel::valueTreeChildOrderChanged(juce::ValueTree& parent, int, int)
{
    if (parent == shownOwner_)
        triggerAsyncUpdate();
}

void DevicePanel::valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& property)
{
    if (tree.hasType(session::ids::device) && property == session::ids::bypassed)
        for (auto& card : cards_)
            if (card->getDevice() == tree)
                card->syncFromModel();
    if (tree == shownOwner_ && property == session::ids::name)
        repaint();
}

void DevicePanel::resized()
{
    viewport_.setBounds(getLocalBounds().withTrimmedTop(kHeaderHeight));
    layoutCards();
}

void DevicePanel::paint(juce::Graphics& g)
{
    using namespace theme;
    g.fillAll(colour::panel);
    auto header = getLocalBounds().removeFromTop(kHeaderHeight);
    g.setColour(colour::panelHeader);
    g.fillRect(header);
    g.setColour(colour::textSecondary);
    g.setFont(label(11.0f));
    const auto owner = getOwner();
    juce::String title = "DEVICES";
    if (editor_.isMasterSelected())
        title << "  -  MASTER";
    else if (owner.isValid())
        title << "  -  " << session::track::getName(owner).toUpperCase();
    g.drawText(title, header.reduced(14, 0), juce::Justification::centredLeft, false);
    g.setColour(colour::textMuted);
    g.setFont(sans(11.0f));
    g.drawText("Signal flows left to right  |  drop devices or plug-ins here", header.reduced(14, 0), juce::Justification::centredRight, false);
    g.setColour(colour::outline);
    g.fillRect(header.removeFromBottom(1));

    if (!owner.isValid())
    {
        g.setColour(colour::textMuted);
        g.setFont(sans(12.5f));
        g.drawText("Select a track (or the master) to see its devices.", getLocalBounds().withTrimmedTop(kHeaderHeight),
                   juce::Justification::centred, false);
    }
    else if (!ownerTakesDevices(owner))
    {
        g.setColour(colour::textMuted);
        g.setFont(sans(12.5f));
        g.drawText(session::track::getKind(owner) == session::TrackKind::vca
                       ? "A VCA Master has no devices: it controls the faders of its member tracks."
                       : "A MIDI track has no devices: its MIDI plays the instrument track chosen in its menu (MIDI Output).",
                   getLocalBounds().withTrimmedTop(kHeaderHeight), juce::Justification::centred, false);
    }
}

} // namespace dojo::ui
