#include "ui/components/DevicePanel.h"

#include "devices/BuiltinProcessor.h"
#include "ui/DeviceActions.h"
#include "ui/Latency.h"
#include "ui/PresetActions.h"
#include "ui/components/ArrangerView.h"
#include "ui/devices/ConsoleEditors.h"
#include "ui/devices/DeviceEditors.h"
#include "ui/devices/HarrisonEditors.h"
#include "ui/devices/PagedEditor.h"
#include "ui/theme/DojoTheme.h"

#include <algorithm>

namespace dojo::ui
{
namespace
{
constexpr int kHeaderHeight = 26;
constexpr int kCardHeaderHeight = 28;
constexpr int kAddCardWidth = 120;
constexpr int kNoticeMilliseconds = 4500;

} // namespace

/** A small pill with a delay ("2.27 ms"); highlighted when the delay is real (more than nothing). */
class LatencyBadge final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit LatencyBadge(juce::String prefix = {})
        : prefix_(std::move(prefix))
    {
        setInterceptsMouseClicks(true, false); // (for its tooltip)
    }

    /** Returns true when the text changed (its width may have). */
    bool set(const juce::String& value, bool active, const juce::String& tooltip)
    {
        setTooltip(tooltip);
        if (value == value_ && active == active_)
            return false;
        const bool resized = value.length() != value_.length();
        value_ = value;
        active_ = active;
        repaint();
        return resized;
    }

    [[nodiscard]] juce::String getText() const { return prefix_.isNotEmpty() ? prefix_ + " " + value_ : value_; }
    [[nodiscard]] bool isActive() const noexcept { return active_; }

    [[nodiscard]] int getPreferredWidth() const
    {
        int width = juce::GlyphArrangement::getStringWidthInt(theme::mono(10.5f), value_) + 12;
        if (prefix_.isNotEmpty())
            width += juce::GlyphArrangement::getStringWidthInt(theme::label(9.5f), prefix_) + 6;
        return width;
    }

    void paint(juce::Graphics& g) override
    {
        using namespace theme;
        const auto bounds = getLocalBounds().toFloat().reduced(0.5f, 2.5f);
        g.setColour(active_ ? colour::gold.withAlpha(0.16f) : colour::panelRaised);
        g.fillRoundedRectangle(bounds, bounds.getHeight() * 0.5f);
        g.setColour(active_ ? colour::gold.withAlpha(0.75f) : colour::outline);
        g.drawRoundedRectangle(bounds, bounds.getHeight() * 0.5f, 1.0f);
        auto text = getLocalBounds().reduced(6, 0);
        if (prefix_.isNotEmpty())
        {
            g.setColour(colour::textMuted);
            g.setFont(label(9.5f));
            const int w = juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), prefix_);
            g.drawText(prefix_, text.removeFromLeft(w), juce::Justification::centredLeft, false);
            text.removeFromLeft(6);
        }
        g.setColour(active_ ? colour::gold : colour::textMuted);
        g.setFont(mono(10.5f));
        g.drawText(value_, text, juce::Justification::centred, false);
    }

private:
    juce::String prefix_;
    juce::String value_ = "0 ms";
    bool active_ = false;
};

namespace
{
/** The card header's presets button: three lines, like a list. */
class PresetsButton final : public juce::Button
{
public:
    PresetsButton()
        : juce::Button("Presets")
    {
        setWantsKeyboardFocus(false);
    }

    void paintButton(juce::Graphics& g, bool over, bool down) override
    {
        using namespace theme;
        const auto r = getLocalBounds().toFloat().reduced(0.5f);
        g.setColour(down ? colour::accent.withAlpha(0.35f) : over ? colour::outlineStrong.withAlpha(0.45f) : colour::panelRaised);
        g.fillRoundedRectangle(r, 3.0f);
        g.setColour(colour::outline);
        g.drawRoundedRectangle(r, 3.0f, 1.0f);
        g.setColour(over || down ? colour::textPrimary : colour::textSecondary);
        const float width = std::min(11.0f, r.getWidth() - 8.0f);
        const float x = r.getCentreX() - width * 0.5f;
        for (int line = -1; line <= 1; ++line)
            g.fillRoundedRectangle(x, r.getCentreY() + static_cast<float>(line) * 3.5f - 0.75f, width, 1.5f, 0.75f);
    }
};

bool anyPresetFile(const juce::StringArray& files)
{
    return std::any_of(files.begin(), files.end(), [](const juce::String& f) { return session::presets::isPresetFile(juce::File(f)); });
}
} // namespace

//==================================================================================================
/** A hosted plug-in's own editor in a window. Keeps the node alive while open. */
/**
    The end of an insert loop (a device's accessory / effects loop) in the chain: effects dropped
    here, or added with its button, go into the loop; after it the signal returns to its host.
*/
class LoopEnd final : public juce::Component, public juce::DragAndDropTarget
{
public:
    LoopEnd(session::Project& project, plugins::PluginManager& plugins, juce::ValueTree host, juce::String returnDescription)
        : project_(project)
        , plugins_(plugins)
        , host_(std::move(host))
        , returnDescription_(std::move(returnDescription))
    {
        add_.setButtonText("+ Add to Loop");
        add_.setTooltip("Add an effect to this loop (or drag one here from the browser)");
        add_.setWantsKeyboardFocus(false);
        add_.onClick = [this] { showAddMenu(); };
        addAndMakeVisible(add_);
    }

    [[nodiscard]] const juce::ValueTree& getHost() const noexcept { return host_; }

    void showAddMenu()
    {
        juce::Component::SafePointer<LoopEnd> safe(this);
        juce::PopupMenu menu;
        deviceActions::buildAddDeviceMenu(menu, plugins_, deviceActions::MenuScope{},
                                          [safe](const deviceActions::DeviceChoice& choice)
                                          {
                                              if (safe != nullptr)
                                                  deviceActions::addDeviceToLoop(safe->project_.getSession(), safe->host_, choice);
                                          });
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&add_));
    }

    bool isInterestedInDragSource(const SourceDetails& details) override
    {
        const auto choice = deviceActions::fromDragDescription(details.description.toString(), plugins_);
        return choice.has_value() && deviceActions::fitsInLoop(*choice);
    }
    void itemDragEnter(const SourceDetails&) override
    {
        dropping_ = true;
        repaint();
    }
    void itemDragExit(const SourceDetails&) override
    {
        dropping_ = false;
        repaint();
    }
    void itemDropped(const SourceDetails& details) override
    {
        dropping_ = false;
        if (const auto choice = deviceActions::fromDragDescription(details.description.toString(), plugins_))
            deviceActions::addDeviceToLoop(project_.getSession(), host_, *choice);
        repaint();
    }

    void resized() override { add_.setBounds(getLocalBounds().reduced(8).removeFromTop(28)); }

    void paint(juce::Graphics& g) override
    {
        using namespace theme;
        auto area = getLocalBounds().toFloat().reduced(1.0f);
        g.setColour(colour::panelRaised.withAlpha(0.5f));
        g.fillRoundedRectangle(area, 5.0f);
        const float dashes[] = {5.0f, 4.0f};
        g.setColour(dropping_ ? colour::play : colour::outline);
        juce::Path outline;
        outline.addRoundedRectangle(area, 5.0f);
        juce::PathStrokeType(1.2f).createDashedStroke(outline, outline, dashes, 2);
        g.fillPath(outline);
        g.setColour(colour::textMuted);
        g.setFont(sans(11.0f));
        auto text = juce::String::fromUTF8("Drop effects here.\n\n\xe2\x86\xa9 Back into ") + session::device::getName(host_);
        if (returnDescription_.isNotEmpty())
            text << ":\n" << returnDescription_;
        g.drawFittedText(text, getLocalBounds().reduced(8).withTrimmedTop(36), juce::Justification::centredTop, 6);
    }

private:
    session::Project& project_;
    plugins::PluginManager& plugins_;
    juce::ValueTree host_;
    juce::String returnDescription_;
    juce::TextButton add_;
    bool dropping_ = false;
};

/** The frame around an insert loop's devices: its name, and whether it is in the signal path. */
class LoopFrame final : public juce::Component
{
public:
    LoopFrame(juce::ValueTree host, std::shared_ptr<engine::ProcessorNode> node)
        : host_(std::move(host))
        , node_(std::move(node))
    {
        setInterceptsMouseClicks(false, false);
    }

    void paint(juce::Graphics& g) override
    {
        using namespace theme;
        const bool engaged = node_ != nullptr && node_->isLoopEngaged();
        auto area = getLocalBounds().toFloat().reduced(1.0f);
        g.setColour((engaged ? colour::accent : colour::outline).withAlpha(0.12f));
        g.fillRoundedRectangle(area, 7.0f);
        g.setColour(engaged ? colour::accent : colour::outline);
        g.drawRoundedRectangle(area, 7.0f, 1.4f);
        g.setColour(engaged ? colour::accentBright : colour::textMuted);
        g.setFont(label(9.5f));
        const auto count = session::Session::getDevices(host_).size();
        const auto name = (node_ != nullptr && node_->getInsertLoop() != nullptr ? node_->getInsertLoop()->getLoopName() : juce::String("Insert Loop"))
                              .toUpperCase();
        const bool series = node_ == nullptr || node_->getInsertLoop() == nullptr || node_->getInsertLoop()->isLoopInSeries();
        const auto state = engaged ? (count > 0 ? juce::String("IN") : juce::String(series ? "EMPTY (NORMALLED)" : "EMPTY (NOTHING RETURNS)"))
                                   : juce::String("OUT (SWITCHED OFF)");
        const auto shortState = engaged ? (count > 0 ? juce::String("IN") : juce::String("EMPTY")) : juce::String("OFF");
        // The longest title that fits: with its host's name, without it, or just the state.
        const auto title = area.reduced(10.0f, 3.0f).removeFromTop(14.0f);
        const auto font = label(9.5f);
        juce::String text;
        for (const auto& candidate : {name + "  -  inside " + session::device::getName(host_).toUpperCase() + "  -  " + state,
                                      name + "  -  " + state, name + "  -  " + shortState})
        {
            text = candidate;
            if (juce::GlyphArrangement::getStringWidth(font, candidate) <= title.getWidth())
                break;
        }
        g.drawText(text, title, juce::Justification::centredLeft, true);
    }

private:
    juce::ValueTree host_;
    std::shared_ptr<engine::ProcessorNode> node_;
};

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
/** One device: header controls above its editor. Presets dropped on it load into the device. */
class DeviceCard final : public juce::Component,
                         public juce::DragAndDropTarget,
                         public juce::FileDragAndDropTarget,
                         private juce::Timer
{
public:
    /** `owner`: the track, the master, or (for a device in an insert loop) the loop's host device,
        whose node is `loopHost`. */
    DeviceCard(DevicePanel& panel, session::Project& project, EditorState& editor, juce::ValueTree device, juce::ValueTree owner,
               std::shared_ptr<engine::ProcessorNode> loopHost = nullptr)
        : panel_(panel)
        , project_(project)
        , device_(std::move(device))
        , owner_(std::move(owner))
        , loopHost_(std::move(loopHost))
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

        // (moves keep the chain's order: MIDI effects, the instrument, then the audio effects)
        const auto move = [this, &session](int step)
        {
            const auto devices = session::Session::getDevices(owner_);
            const int index = static_cast<int>(std::find(devices.begin(), devices.end(), device_) - devices.begin());
            if (deviceActions::canMoveDevice(devices, index, index + step))
            {
                session.beginTransaction("Move Device");
                session.moveDevice(device_, index + step);
            }
        };
        left_.onClick = [move] { move(-1); };
        right_.onClick = [move] { move(1); };
        remove_.onClick = [this, &session]
        {
            auto deviceTree = device_;
            juce::MessageManager::callAsync([&session, deviceTree]() mutable
                                            {
                                                session.beginTransaction("Remove Device");
                                                session.removeDevice(deviceTree);
                                            });
        };

        addAndMakeVisible(latency_);

        if (node_ != nullptr)
        {
            presets_.setTooltip("Presets: load one, or save this device's settings as a preset (or drop a preset here)");
            presets_.onClick = [this] { showPresetMenu(); };
            addAndMakeVisible(presets_);
        }

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
            const auto ownerTrack = session::Session::getDeviceOwner(device_); // (also from inside an insert loop)
            for (const auto& t : session.getAllTracks())
            {
                if (t == ownerTrack || (ownerTrack.hasType(session::ids::track) && !session.canFeed(t, ownerTrack)))
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
                // A native device (compressor, gate, expander, auto filter) listens to its sidechain
                // only when its External Sidechain switch is on: choosing a source switches it.
                if (auto* native = dynamic_cast<devices::BuiltinProcessor*>(&node_->getProcessor()))
                    if (auto* external = native->getParameterById("sidechain"))
                        external->setValueNotifyingHost(source != 0 ? 1.0f : 0.0f);
            };
            addAndMakeVisible(sidechain_);
        }

        if (node_ != nullptr)
        {
            editorComponent_ = createDeviceEditor(*node_, DeviceEditorContext{project_, editor, device_, owner_, panel_.getTrackTaps()}, preferredWidth_);
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

    /** Its editor's pages, when it has pages (kept across the panel's rebuilds). */
    [[nodiscard]] PagedEditor* getPagedEditor() const noexcept
    {
        auto* component = editorComponent_.get();
        if (auto* paged = dynamic_cast<PagedEditor*>(component))
            return paged;
        if (auto* console = dynamic_cast<ConsoleEditor*>(component))
            return &console->getPages();
        if (auto* harrison = dynamic_cast<HarrisonEditor*>(component))
            return &harrison->getPages();
        return nullptr;
    }

    void syncFromModel()
    {
        bypass_.setToggleState(!session::device::isBypassed(device_), juce::dontSendNotification);
        repaint();
    }

    // --- Delay ------------------------------------------------------------------------------------
    /** The delay this device adds now (none while bypassed or missing). */
    [[nodiscard]] int getActiveLatency() const
    {
        if (loopHost_ != nullptr && (!loopHost_->isLoopEngaged() || loopHost_->isBypassed()))
            return 0; // (in a loop that is out of the signal path)
        return node_ != nullptr && !session::device::isBypassed(device_) ? std::max(0, node_->getLatencySamples()) : 0;
    }
    [[nodiscard]] bool isInLoop() const noexcept { return loopHost_ != nullptr; }

    /** Shows this device's delay; `chainSoFar` is the chain's delay up to and including it. */
    void showLatency(double sampleRate, int chainSoFar)
    {
        using namespace latency;
        const int own = node_ != nullptr ? std::max(0, node_->getLatencySamples()) : 0;
        const bool bypassed = session::device::isBypassed(device_);
        const auto rate = formatRate(sampleRate);
        juce::String tooltip;
        if (node_ == nullptr)
            tooltip = "Not available: adds no delay.";
        else if (bypassed)
            tooltip = "Bypassed: adds no delay now (" + formatMilliseconds(own, sampleRate) + ", " + formatSamples(own) + ", when on).";
        else if (own == 0)
            tooltip = "Adds no delay.";
        else
            tooltip = "Adds " + formatMilliseconds(own, sampleRate) + " of delay (" + formatSamples(own) + " at " + rate + ").";
        tooltip << "\nChain delay up to here: " << formatMilliseconds(chainSoFar, sampleRate) << " (" << formatSamples(chainSoFar) << ").";
        if (latency_.set(formatMilliseconds(bypassed ? 0 : own, sampleRate), own > 0 && !bypassed, tooltip))
            resized();
    }

    [[nodiscard]] const LatencyBadge& getLatencyBadge() const noexcept { return latency_; }
    [[nodiscard]] LatencyBadge& getLatencyBadge() noexcept { return latency_; }

    // --- Presets ---------------------------------------------------------------------------------
    void showNotice(const juce::String& text, bool ok)
    {
        notice_ = text;
        noticeOk_ = ok;
        startTimer(kNoticeMilliseconds);
        repaint();
    }

    [[nodiscard]] const juce::String& getNotice() const noexcept { return notice_; }

    void buildPresetMenu(juce::PopupMenu& menu)
    {
        namespace presets = session::presets;
        const auto name = session::device::getName(device_);
        const auto fitting = presets::getFor(presets::keyOf(device_));
        juce::Component::SafePointer<DevicePanel> panel(&panel_);
        auto* project = &project_;
        const auto device = device_;
        const auto owner = owner_;

        const auto addPresets = [&](bool builtIn, const juce::String& heading)
        {
            bool first = true;
            for (const auto& preset : fitting)
            {
                if (preset.builtIn != builtIn)
                    continue;
                if (first)
                    menu.addSectionHeader(heading);
                first = false;
                menu.addItem(preset.chain ? preset.name + "  (from chain)" : preset.name, [panel, project, device, preset]
                             {
                                 const auto outcome = presetActions::load(*project, preset, device);
                                 if (panel != nullptr)
                                     panel->showNotice(device, outcome.message, outcome.ok);
                             });
            }
        };
        addPresets(true, "Built-in Presets");
        addPresets(false, "My Presets");
        if (fitting.empty())
            menu.addItem("No presets for " + name + " yet", false, false, [] {});

        const auto report = [panel, device](const presetActions::Outcome& outcome)
        {
            if (panel != nullptr)
                panel->showNotice(device, outcome.message, outcome.ok);
        };
        const auto folder = presets::getUserFolder();
        menu.addSeparator();
        menu.addItem("Save Preset...", [panel, project, device, folder, name, report]
                     {
                         if (panel != nullptr)
                             panel->choosePresetFile("Save " + name + " Preset", true, folder.getChildFile(presets::fileNameFor(name + " Preset")),
                                                     [project, device, report](const juce::File& file)
                                                     { report(presetActions::save(*project, device, file)); });
                     });
        menu.addItem("Add to Preset File...", [panel, project, device, folder, name, report]
                     {
                         if (panel != nullptr)
                             panel->choosePresetFile("Add " + name + " to a Preset File", false, folder,
                                                     [project, device, report](const juce::File& file)
                                                     { report(presetActions::addToFile(*project, device, file)); });
                     });
        menu.addItem("Save Whole Chain as Preset...", session::Session::getDevices(owner).size() > 1, false,
                     [panel, project, owner, folder, report]
                     {
                         if (panel != nullptr)
                             panel->choosePresetFile("Save the Whole Chain as a Preset", true, folder.getChildFile(presets::fileNameFor("Chain Preset")),
                                                     [project, owner, report](const juce::File& file)
                                                     { report(presetActions::saveChain(*project, owner, file)); });
                     });
        menu.addItem("Load Preset File...", [panel, project, device, folder, name, report]
                     {
                         if (panel != nullptr)
                             panel->choosePresetFile("Load a Preset into " + name, false, folder,
                                                     [project, device, report](const juce::File& file)
                                                     { report(presetActions::loadFirstFitting(*project, {file.getFullPathName()}, device)); });
                     });
        menu.addSeparator();
        menu.addItem("Open Presets Folder", [folder]
                     {
                         (void) folder.createDirectory();
                         folder.startAsProcess();
                     });
    }

    // Presets dragged from the browser...
    bool isInterestedInDragSource(const SourceDetails& details) override
    {
        return node_ != nullptr && presetActions::isPresetDrag(details.description);
    }
    void itemDragEnter(const SourceDetails& details) override { showDrop(presetActions::fromDragDescription(details.description)); }
    void itemDragExit(const SourceDetails&) override { showDrop(std::nullopt); }
    void itemDropped(const SourceDetails& details) override
    {
        showDrop(std::nullopt);
        if (const auto preset = presetActions::fromDragDescription(details.description))
            report(presetActions::load(project_, *preset, device_));
        else
            showNotice("That preset is no longer there.", false);
    }

    // ...and preset files from the file manager.
    bool isInterestedInFileDrag(const juce::StringArray& files) override { return node_ != nullptr && anyPresetFile(files); }
    void fileDragEnter(const juce::StringArray& files, int, int) override
    {
        std::optional<session::presets::Preset> shown;
        for (const auto& path : files)
        {
            juce::String error;
            if (!session::presets::isPresetFile(juce::File(path)))
                continue;
            if (auto preset = session::presets::Preset::load(juce::File(path), error))
            {
                const bool fits = preset->fits(session::presets::keyOf(device_));
                if (fits || !shown)
                    shown = std::move(preset);
                if (fits)
                    break;
            }
        }
        showDrop(shown);
    }
    void fileDragExit(const juce::StringArray&) override { showDrop(std::nullopt); }
    void filesDropped(const juce::StringArray& files, int, int) override
    {
        showDrop(std::nullopt);
        report(presetActions::loadFirstFitting(project_, files, device_));
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
        if (presets_.isVisible())
        {
            presets_.setBounds(header.removeFromRight(22));
            header.removeFromRight(4);
        }
        if (window_.isVisible())
        {
            window_.setBounds(header.removeFromRight(28));
            header.removeFromRight(4);
        }
        bypass_.setBounds(header.removeFromLeft(34));
        header.removeFromLeft(6);
        if (sidechain_.isVisible())
            sidechain_.setBounds(header.removeFromRight(std::min(130, header.getWidth() / 2)));
        if (latency_.isVisible())
        {
            header.removeFromRight(2);
            latency_.setBounds(header.removeFromRight(std::min(latency_.getPreferredWidth(), header.getWidth() / 2)));
            header.removeFromRight(4);
        }
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

    void paintOverChildren(juce::Graphics& g) override
    {
        using namespace theme;
        juce::String banner = notice_;
        bool ok = noticeOk_;
        if (drop_ != Drop::none)
        {
            ok = drop_ == Drop::fits;
            banner = dropText_;
            g.setColour(ok ? colour::play : colour::meterClip);
            g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.5f), 5.0f, 2.0f);
        }
        if (banner.isEmpty())
            return;
        auto area = getLocalBounds().withTrimmedTop(kCardHeaderHeight).reduced(8, 8).removeFromBottom(32).toFloat();
        g.setColour(colour::panelHeader.withAlpha(0.96f));
        g.fillRoundedRectangle(area, 4.0f);
        g.setColour(ok ? colour::play : colour::meterClip);
        g.drawRoundedRectangle(area, 4.0f, 1.0f);
        g.setColour(colour::textPrimary);
        g.setFont(sans(11.5f));
        g.drawFittedText(banner, area.toNearestInt().reduced(8, 3), juce::Justification::centred, 2);
    }

private:
    enum class Drop
    {
        none,
        fits,
        mismatch
    };

    void timerCallback() override
    {
        stopTimer();
        notice_.clear();
        repaint();
    }

    void report(const presetActions::Outcome& outcome) { showNotice(outcome.message, outcome.ok); }

    void showPresetMenu()
    {
        juce::PopupMenu menu;
        buildPresetMenu(menu);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&presets_));
    }

    /** Outlines the card while a preset is dragged over it: does it fit this device? */
    void showDrop(const std::optional<session::presets::Preset>& preset)
    {
        if (!preset)
        {
            drop_ = Drop::none;
            dropText_.clear();
        }
        else if (preset->fits(session::presets::keyOf(device_)))
        {
            drop_ = Drop::fits;
            dropText_ = "Drop to load \"" + preset->name + "\"";
        }
        else
        {
            drop_ = Drop::mismatch;
            dropText_ = "\"" + preset->name + "\" is for " + preset->getDeviceNames().joinIntoString(", ");
        }
        repaint();
    }

    DevicePanel& panel_;
    session::Project& project_;
    juce::ValueTree device_;
    juce::ValueTree owner_;
    std::shared_ptr<engine::ProcessorNode> loopHost_; ///< (a device in an insert loop: the loop's host)
    std::shared_ptr<engine::ProcessorNode> node_;
    juce::TextButton bypass_;
    juce::TextButton left_;
    juce::TextButton right_;
    juce::TextButton remove_;
    juce::TextButton window_;
    PresetsButton presets_;
    LatencyBadge latency_;
    juce::ComboBox sidechain_;
    std::vector<std::uint32_t> sidechainIds_;
    std::unique_ptr<juce::Component> editorComponent_;
    juce::Rectangle<int> nameArea_;
    juce::String error_;
    int preferredWidth_ = 260;
    juce::String notice_;
    bool noticeOk_ = true;
    Drop drop_ = Drop::none;
    juce::String dropText_;
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

    chainLatency_ = std::make_unique<LatencyBadge>("CHAIN DELAY");
    addAndMakeVisible(*chainLatency_);

    project_.getSession().getState().addListener(this);
    editor_.addListener(this);
    rebuild();
    startTimerHz(5);
}

DevicePanel::~DevicePanel()
{
    stopTimer();
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
    // The pages the cards' editors show stay as they are.
    std::map<std::uint32_t, std::size_t> pages;
    for (const auto& card : cards_)
        if (const auto* paged = card->getPagedEditor())
            pages[session::device::getId(card->getDevice())] = paged->getPage();
    loops_.clear();
    loopHosts_.clear();
    cards_.clear();
    shownOwner_ = getOwner();
    shownMaster_ = editor_.isMasterSelected();

    if (shownOwner_.isValid())
        for (const auto& device : session::Session::getDevices(shownOwner_))
        {
            auto card = std::make_unique<DeviceCard>(*this, project_, editor_, device, shownOwner_);
            holder_.addAndMakeVisible(*card);
            cards_.push_back(std::move(card));

            // A device with an insert loop: its loop follows it, in a frame.
            const auto node = project_.getDevices().getNode(device);
            if (node == nullptr || !node->hasInsertLoop() || shownOwner_.hasType(session::ids::master))
                continue;
            const bool shown = showsLoop(device, *node);
            loopHosts_.push_back({device, node, shown});
            if (!shown)
                continue;
            LoopView loop;
            loop.frame = std::make_unique<LoopFrame>(device, node);
            holder_.addAndMakeVisible(*loop.frame);
            loop.frame->toBack();
            loop.first = cards_.size();
            for (const auto& inner : session::Session::getDevices(device))
            {
                auto innerCard = std::make_unique<DeviceCard>(*this, project_, editor_, inner, device, node);
                holder_.addAndMakeVisible(*innerCard);
                cards_.push_back(std::move(innerCard));
            }
            loop.last = cards_.size();
            loop.end = std::make_unique<LoopEnd>(project_, plugins_, device, node->getInsertLoop()->getReturnDescription());
            holder_.addAndMakeVisible(*loop.end);
            loops_.push_back(std::move(loop));
        }

    for (const auto& card : cards_)
        if (auto* paged = card->getPagedEditor())
            if (const auto it = pages.find(session::device::getId(card->getDevice())); it != pages.end())
                paged->showPage(it->second);

    addButton_.setVisible(shownOwner_.isValid() && ownerTakesDevices(shownOwner_));
    closeStaleWindows();
    layoutCards();
    refreshLatencies();
    layoutHeader();
    repaint();
}

void DevicePanel::refreshLatencies()
{
    using namespace latency;
    const double sampleRate = project_.getDevices().getSampleRate();
    int total = 0;
    for (auto& card : cards_)
    {
        total += card->getActiveLatency();
        card->showLatency(sampleRate, total);
    }
    chainLatencySamples_ = total;

    juce::String tooltip;
    if (!shownOwner_.isValid() || !ownerTakesDevices(shownOwner_))
        tooltip = "No devices here.";
    else
    {
        const auto who = shownOwner_.hasType(session::ids::track) ? juce::String("This track's") : juce::String("The master's");
        tooltip << who << " devices delay its sound by " << formatMilliseconds(total, sampleRate) << " (" << formatSamples(total) << " at "
                << formatRate(sampleRate) << "), the sum of the delays of the devices that are on.";
        const int project = project_.getCompiler().getTotalLatency();
        tooltip << "\nThe Dojo delays the other tracks to match (delay compensation): everything plays "
                << formatMilliseconds(project, sampleRate) << " (" << formatSamples(project) << ") after the timeline.";
    }
    if (chainLatency_->set(formatMilliseconds(total, sampleRate), total > 0, tooltip))
        layoutHeader();
    chainLatency_->setVisible(shownOwner_.isValid() && ownerTakesDevices(shownOwner_));
}

juce::String DevicePanel::getHeaderTitle() const
{
    const auto owner = getOwner();
    juce::String title = "DEVICES";
    if (editor_.isMasterSelected())
        title << "  -  MASTER";
    else if (owner.isValid())
        title << "  -  " << session::track::getName(owner).toUpperCase();
    return title;
}

void DevicePanel::layoutHeader()
{
    const int titleWidth = juce::GlyphArrangement::getStringWidthInt(theme::label(11.0f), getHeaderTitle());
    const int width = chainLatency_->getPreferredWidth();
    chainLatency_->setBounds(14 + titleWidth + 14, 2, width, kHeaderHeight - 4);
}

juce::String DevicePanel::getLatencyText(const juce::ValueTree& device) const
{
    if (const auto* card = dynamic_cast<const DeviceCard*>(findCard(device)))
        return card->getLatencyBadge().getText();
    return {};
}

juce::String DevicePanel::getLatencyTooltip(const juce::ValueTree& device) const
{
    if (auto* card = dynamic_cast<DeviceCard*>(findCard(device)))
        return card->getLatencyBadge().getTooltip();
    return {};
}

juce::String DevicePanel::getChainLatencyText() const
{
    return chainLatency_->getText();
}

juce::String DevicePanel::getChainLatencyTooltip() const
{
    return chainLatency_->getTooltip();
}

void DevicePanel::layoutCards()
{
    const int height = std::max(0, viewport_.getMaximumVisibleHeight());
    constexpr int kFrameTitle = 18; // (room for a loop frame's title above its cards)
    int x = 4;
    std::size_t nextLoop = 0;
    for (std::size_t i = 0; i <= cards_.size(); ++i)
    {
        // An insert loop starts here (its frame, then its cards, then its end slot).
        while (nextLoop < loops_.size() && loops_[nextLoop].first == i)
        {
            auto& loop = loops_[nextLoop];
            const int start = x;
            x += 8;
            for (std::size_t k = loop.first; k < loop.last; ++k)
            {
                auto& card = cards_[k];
                card->setBounds(x, 4 + kFrameTitle, card->getPreferredWidth(), std::max(0, height - 12 - kFrameTitle));
                x += card->getPreferredWidth() + 6;
            }
            loop.end->setBounds(x, 4 + kFrameTitle, kAddCardWidth + 30, std::max(0, height - 12 - kFrameTitle));
            x += kAddCardWidth + 30 + 8;
            loop.frame->setBounds(start, 2, x - start, std::max(0, height - 4));
            x += 6;
            i = loop.last;
            ++nextLoop;
        }
        if (i >= cards_.size())
            break;
        auto& card = cards_[i];
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
    return kind != session::TrackKind::vca; // (a MIDI track: its MIDI effects)
}

void DevicePanel::showAddMenu()
{
    const auto owner = getOwner();
    if (!owner.isValid() || !ownerTakesDevices(owner))
        return;

    // Instrument tracks take everything; MIDI tracks their MIDI effects (and an instrument, which
    // gets an instrument track this one plays); other tracks and the master audio effects.
    const auto kind = owner.hasType(session::ids::track) ? session::track::getKind(owner) : session::TrackKind::audio;
    deviceActions::MenuScope scope;
    scope.instruments = scope.midiEffects = kind == session::TrackKind::instrument || kind == session::TrackKind::midi;
    scope.audioEffects = kind != session::TrackKind::midi;
    juce::Component::SafePointer<DevicePanel> safe(this);
    juce::PopupMenu menu;
    deviceActions::buildAddDeviceMenu(menu, plugins_, scope,
                                      [safe](const deviceActions::DeviceChoice& choice)
                                      {
                                          if (safe != nullptr)
                                              deviceActions::addDevice(safe->project_.getSession(), safe->editor_, safe->getOwner(), choice);
                                      });
    addChainItems(menu);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&addButton_));
}

void DevicePanel::addChainItems(juce::PopupMenu& menu)
{
    // Preset chains, by what they are for: each adds its whole chain to the selected track.
    std::map<juce::String, juce::PopupMenu> byCategory;
    juce::Component::SafePointer<DevicePanel> safe(this);
    for (const auto& preset : session::presets::getAll())
    {
        if (!preset.chain)
            continue;
        const auto category = preset.category.isNotEmpty() ? preset.category : juce::String(preset.builtIn ? "Other" : "My Chains");
        byCategory[category].addItem(preset.name, [safe, preset]
                                     {
                                         if (safe != nullptr)
                                             safe->dropPreset(preset);
                                     });
    }
    if (byCategory.empty())
        return;
    menu.addSectionHeader("Preset Chains");
    for (auto& [category, sub] : byCategory)
        menu.addSubMenu(category, sub);
}

juce::StringArray DevicePanel::getAddMenuChainItems()
{
    juce::StringArray items;
    juce::PopupMenu menu;
    addChainItems(menu);
    for (juce::PopupMenu::MenuItemIterator it(menu, true); it.next();)
    {
        const auto& item = it.getItem();
        if (!item.isSectionHeader && !item.isSeparator && item.subMenu == nullptr)
            items.add(item.text);
    }
    return items;
}

bool DevicePanel::isInterestedInDragSource(const SourceDetails& details)
{
    return details.description.toString().startsWith(ArrangerView::kDeviceDragPrefix) || presetActions::isPresetDrag(details.description);
}

void DevicePanel::itemDropped(const SourceDetails& details)
{
    if (presetActions::isPresetDrag(details.description))
    {
        if (const auto preset = presetActions::fromDragDescription(details.description))
            dropPreset(*preset);
        return;
    }
    if (const auto choice = deviceActions::fromDragDescription(details.description.toString(), plugins_))
        deviceActions::addDevice(project_.getSession(), editor_, getOwner(), *choice);
}

bool DevicePanel::isInterestedInFileDrag(const juce::StringArray& files)
{
    return anyPresetFile(files);
}

void DevicePanel::filesDropped(const juce::StringArray& files, int, int)
{
    juce::String error;
    for (const auto& path : files)
        if (session::presets::isPresetFile(juce::File(path)))
        {
            if (const auto preset = session::presets::Preset::load(juce::File(path), error))
            {
                dropPreset(*preset);
                return;
            }
        }
    setPanelNotice(error);
}

void DevicePanel::dropPreset(const session::presets::Preset& preset)
{
    juce::String message;
    (void) presetActions::addDevice(project_, plugins_, editor_, getOwner(), preset, &message);
    setPanelNotice(message);
}

void DevicePanel::setPanelNotice(const juce::String& text)
{
    panelNotice_ = text;
    const auto serial = ++panelNoticeSerial_;
    repaint();
    juce::Component::SafePointer<DevicePanel> safe(this);
    juce::Timer::callAfterDelay(kNoticeMilliseconds, [safe, serial]
                                {
                                    if (safe != nullptr && safe->panelNoticeSerial_ == serial)
                                    {
                                        safe->panelNotice_.clear();
                                        safe->repaint();
                                    }
                                });
}

void DevicePanel::choosePresetFile(const juce::String& title, bool forSaving, const juce::File& initial,
                                   std::function<void(const juce::File&)> onChosen)
{
    (void) (initial.isDirectory() || initial.getFileExtension().isEmpty() ? initial : initial.getParentDirectory()).createDirectory();
    chooser_ = std::make_unique<juce::FileChooser>(title, initial, session::presets::kWildcard);
    const int chooserFlags = forSaving ? juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles |
                                      juce::FileBrowserComponent::warnAboutOverwriting
                                : juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;
    chooser_->launchAsync(chooserFlags, [forSaving, done = std::move(onChosen)](const juce::FileChooser& chooser)
                          {
                              auto file = chooser.getResult();
                              if (file == juce::File())
                                  return;
                              if (forSaving && !session::presets::isPresetFile(file))
                                  file = file.withFileExtension(session::presets::kExtension);
                              done(file);
                          });
}

void DevicePanel::showNotice(const juce::ValueTree& device, const juce::String& text, bool ok)
{
    if (auto* card = dynamic_cast<DeviceCard*>(findCard(device)))
        card->showNotice(text, ok);
    else
        setPanelNotice(text);
}

juce::Component* DevicePanel::findCard(const juce::ValueTree& device) const
{
    for (const auto& card : cards_)
        if (card->getDevice() == device)
            return card.get();
    return nullptr;
}

juce::Component* DevicePanel::findLoopEnd(const juce::ValueTree& host) const
{
    for (const auto& loop : loops_)
        if (loop.end != nullptr && loop.end->getHost() == host)
            return loop.end.get();
    return nullptr;
}

juce::String DevicePanel::getNotice(const juce::ValueTree& device) const
{
    if (const auto* card = dynamic_cast<const DeviceCard*>(findCard(device)))
        return card->getNotice();
    return {};
}

juce::StringArray DevicePanel::getPresetMenuItems(const juce::ValueTree& device) const
{
    juce::StringArray items;
    if (auto* card = dynamic_cast<DeviceCard*>(findCard(device)))
    {
        juce::PopupMenu menu;
        card->buildPresetMenu(menu);
        for (juce::PopupMenu::MenuItemIterator it(menu); it.next();)
        {
            const auto& item = it.getItem();
            if (item.isSectionHeader)
                items.add("[" + item.text + "]");
            else if (!item.isSeparator)
                items.add(item.text);
        }
    }
    return items;
}

void DevicePanel::editorStateChanged()
{
    if (getOwner() != shownOwner_ || editor_.isMasterSelected() != shownMaster_)
        triggerAsyncUpdate();
}

void DevicePanel::valueTreeChildAdded(juce::ValueTree&, juce::ValueTree& child)
{
    // Devices, and tracks (the cards' sidechain lists); not the shown track's clips, sends or lanes.
    if (child.hasType(session::ids::device) || child.hasType(session::ids::track))
        triggerAsyncUpdate();
}

void DevicePanel::valueTreeChildRemoved(juce::ValueTree& parent, juce::ValueTree& child, int)
{
    if (child.hasType(session::ids::device) || child == shownOwner_ || parent == shownOwner_ || child.hasType(session::ids::track))
        triggerAsyncUpdate();
}

void DevicePanel::valueTreeChildOrderChanged(juce::ValueTree& parent, int, int)
{
    if (parent == shownOwner_ || parent.hasType(session::ids::device)) // (or inside an insert loop)
        triggerAsyncUpdate();
}

bool DevicePanel::showsLoop(const juce::ValueTree& device, const engine::ProcessorNode& node)
{
    const auto* loop = node.getInsertLoop();
    return loop != nullptr && (loop->isLoopAvailable() || !session::Session::getDevices(device).empty());
}

void DevicePanel::timerCallback()
{
    refreshLatencies();
    for (auto& loop : loops_)
        loop.frame->repaint(); // (the loop's switch)
    // A device's mode may offer its loop or not (a console's channel or monitor section).
    for (const auto& host : loopHosts_)
        if (showsLoop(host.device, *host.node) != host.shown)
        {
            triggerAsyncUpdate();
            break;
        }
}

void DevicePanel::valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& property)
{
    if (tree.hasType(session::ids::device) && property == session::ids::bypassed)
        for (auto& card : cards_)
            if (card->getDevice() == tree)
                card->syncFromModel();
    if (tree == shownOwner_ && property == session::ids::name)
    {
        layoutHeader();
        repaint();
    }
    if (tree.hasType(session::ids::device) && property == session::ids::bypassed)
        refreshLatencies();
}

void DevicePanel::resized()
{
    viewport_.setBounds(getLocalBounds().withTrimmedTop(kHeaderHeight));
    layoutCards();
    layoutHeader();
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
    g.drawText(getHeaderTitle(), header.reduced(14, 0), juce::Justification::centredLeft, false);
    g.setColour(panelNotice_.isNotEmpty() ? colour::textPrimary : colour::textMuted);
    g.setFont(sans(11.0f));
    auto hint = header.reduced(14, 0);
    if (chainLatency_->isVisible())
        hint.setLeft(chainLatency_->getRight() + 14);
    g.drawText(panelNotice_.isNotEmpty() ? panelNotice_ : juce::String("Signal flows left to right  |  drop devices, plug-ins or presets here"),
               hint, juce::Justification::centredRight, true);
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
        g.drawText("A VCA Master has no devices: it controls the faders of its member tracks.", getLocalBounds().withTrimmedTop(kHeaderHeight),
                   juce::Justification::centred, false);
    }
}

} // namespace dojo::ui
