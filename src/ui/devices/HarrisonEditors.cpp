#include "ui/devices/HarrisonEditors.h"

#include "devices/Harrison4032.h"
#include "session/Project.h"
#include "session/Session.h"
#include "ui/DeviceActions.h"
#include "ui/devices/HardwarePainting.h"
#include "ui/theme/DojoTheme.h"

#include <algorithm>
#include <cmath>

namespace dojo::ui
{
using Console = devices::Harrison4032;
using Outputs = Console::Outputs;

namespace harrison
{
void assignBus(session::Session& session, const juce::ValueTree& device, Console& console, int bus, bool assigned)
{
    if (bus < 1 || bus > Console::kBuses)
        return;
    console.bus_[static_cast<std::size_t>(bus - 1)]->setValueNotifyingHost(assigned ? 1.0f : 0.0f);
    const int odd = bus % 2 == 1 ? bus : bus - 1;
    const bool used = console.bus_[static_cast<std::size_t>(odd - 1)]->get() || console.bus_[static_cast<std::size_t>(odd)]->get();
    const int left = Outputs::bus(odd), right = Outputs::bus(odd + 1);
    if (used)
        (void) deviceActions::connectDeviceOutput(session, device, left, right, "Bus " + juce::String(odd) + "-" + juce::String(odd + 1));
    else
        deviceActions::disconnectDeviceOutput(session, device, left, right);
}

std::vector<Connection> connections(const juce::ValueTree&, const juce::ValueTree& track)
{
    std::vector<Connection> list;
    for (int a = 1; a <= 4; ++a)
        list.push_back({"AUX " + juce::String(a), Outputs::aux(a), Outputs::aux(a), "Aux " + juce::String(a)});
    for (int c = 1; c <= 2; ++c)
        list.push_back({"CUE " + juce::String(c), Outputs::cue(c), Outputs::cue(c) + 1, "Cue " + juce::String(c)});
    list.push_back({"QUAD 3-4", Outputs::quad, Outputs::quad + 1, "Quad 3-4"});
    list.push_back({"DIRECT", Outputs::direct, Outputs::direct + 1,
                    "Direct " + (track.isValid() ? session::track::getName(track) : juce::String("Out"))});
    return list;
}
} // namespace harrison

namespace
{
using Page = PagedEditor::Page;
using Parameters = std::vector<std::pair<juce::String, juce::String>>;

Parameters ids(std::initializer_list<const char*> idsAndCaptions)
{
    Parameters result;
    for (const auto* entry : idsAndCaptions)
    {
        const juce::String text(entry);
        result.emplace_back(text.upToFirstOccurrenceOf("|", false, false), text.fromFirstOccurrenceOf("|", false, false));
    }
    return result;
}

void styleToggle(juce::TextButton& button)
{
    button.setWantsKeyboardFocus(false);
    button.setColour(juce::TextButton::buttonOnColourId, theme::colour::accent);
}

// The desk's colours.
const juce::Colour kPanel{0xff161719};
const juce::Colour kPrint{0xffe9e5d9};
const juce::Colour kWalnut{0xff5b3920};
const juce::Colour kLeather{0xff5e3a24};
const juce::Colour kVuFace{0xfff2e4b8};
constexpr float kFaderWidth = 44.0f;

/** A black recess (a meter window, the scope) set into the panel or the bridge. */
void paintRecess(juce::Graphics& g, juce::Rectangle<float> area, juce::Colour fill)
{
    g.setColour(juce::Colours::black.withAlpha(0.6f));
    g.fillRoundedRectangle(area.expanded(1.0f), 3.0f);
    g.setColour(fill);
    g.fillRoundedRectangle(area, 2.5f);
    g.setColour(juce::Colours::white.withAlpha(0.07f));
    g.drawRoundedRectangle(area.expanded(1.0f), 3.0f, 0.8f);
}

//==================================================================================================
/** The console's face: the wooden meter bridge with the outputs' VU meters and the revision's
    badge, then on the black panel the revision's circuits, the EQ and filter curve (I/O module),
    the lamps, and the long fader. */
class HarrisonFace final : public juce::Component, private juce::Timer
{
public:
    explicit HarrisonFace(Console& console)
        : console_(console)
    {
        setInterceptsMouseClicks(false, true);
        if (auto* level = dynamic_cast<juce::RangedAudioParameter*>(findParameter(console_, "fader")))
        {
            hardware::FaderStyle style;
            style.cap = juce::Colour(0xff9da1a6);
            style.capLine = juce::Colour(0xfff6f5f0);
            style.slot = juce::Colour(0xff030303);
            style.scale = kPrint;
            style.capHeight = 20.0f;
            fader_ = std::make_unique<hardware::ParameterFader>(*level, style);
            addAndMakeVisible(*fader_);
        }
        startTimerHz(30);
    }

    void resized() override
    {
        if (fader_ != nullptr) // the long fader, the whole height of the strip (as paint lays it out)
            fader_->setBounds(getLocalBounds().toFloat().removeFromRight(kFaderWidth).reduced(3.0f, 14.0f).withTrimmedTop(4.0f).toNearestInt());
    }

    void paint(juce::Graphics& g) override
    {
        using namespace theme;
        auto area = getLocalBounds().toFloat();
        const auto& spec = Console::revisionSpec(console_.getRevision());
        const int module = console_.getModule();

        // The fader's strip of panel, its whole height.
        if (fader_ != nullptr)
        {
            auto strip = area.removeFromRight(kFaderWidth);
            hardware::paintMetal(g, strip, kPanel, 0.0f, false);
            g.setColour(juce::Colours::black);
            g.drawVerticalLine(juce::roundToInt(strip.getX()), strip.getY(), strip.getBottom());
            g.setColour(kPrint);
            g.setFont(label(7.0f));
            g.drawText(console_.getModule() == Console::ioModule ? "FADER" : "MASTER", strip.removeFromTop(16.0f).translated(0.0f, 2.0f),
                       juce::Justification::centred, false);
        }

        // The meter bridge: walnut, the two VU meters in its black window, the badge between them.
        auto bridge = area.removeFromTop(bridgeHeight(area));
        hardware::paintWood(g, bridge, kWalnut, 9, 3.0f);
        auto window = bridge.reduced(5.0f, 5.0f);
        paintRecess(g, window, juce::Colour(0xff0c0c0d));
        window.reduce(3.0f, 3.0f);
        const float badgeWidth = std::min(64.0f, window.getWidth() * 0.24f);
        const float meterWidth = (window.getWidth() - badgeWidth - 6.0f) * 0.5f;
        for (int ch = 0; ch < 2; ++ch)
        {
            auto face = hardware::vuFace(kVuFace, "VU");
            face.caption = ch == 0 ? "L" : "R";
            const auto meter = ch == 0 ? window.withWidth(meterWidth) : window.withLeft(window.getRight() - meterWidth);
            hardware::paintNeedleMeter(g, meter, hardware::vuPosition(vu_[static_cast<std::size_t>(ch)]), face);
        }
        auto badge = window.withSizeKeepingCentre(badgeWidth, window.getHeight());
        g.setColour(kPrint);
        g.setFont(sansBold(8.0f).withExtraKerningFactor(0.18f));
        g.drawText("HARRISON", badge.removeFromTop(12.0f), juce::Justification::centred, false);
        g.setFont(sansBold(14.0f));
        g.drawText("4032", badge.removeFromTop(17.0f), juce::Justification::centred, false);
        const auto revision = badge.removeFromTop(13.0f).withSizeKeepingCentre(42.0f, 12.0f);
        g.setColour(juce::Colour(0xffb6332a));
        g.fillRoundedRectangle(revision, 2.0f);
        g.setColour(juce::Colours::white);
        g.setFont(label(7.5f));
        g.drawText("REV " + juce::String(spec.name), revision, juce::Justification::centred, false);
        g.setColour(kPrint.withAlpha(0.7f));
        g.setFont(label(6.5f));
        g.drawText(spec.years, badge.removeFromTop(10.0f), juce::Justification::centred, false);

        // The black panel under it.
        hardware::paintMetal(g, area, kPanel, 0.0f, false);
        auto content = area.reduced(6.0f, 4.0f);
        g.setFont(mono(7.5f));
        g.setColour(kPrint.withAlpha(0.75f));
        g.drawText(juce::String(spec.opAmps) + "  " + spec.vca + "  +-" + juce::String(static_cast<int>(spec.railVolts)) + " V  +" +
                       juce::String(spec.maxOutputDbu, std::abs(spec.maxOutputDbu - std::round(spec.maxOutputDbu)) < 0.01 ? 0 : 1) + " dBu",
                   content.removeFromTop(10.0f), juce::Justification::centredLeft, true);
        content.removeFromTop(3.0f);
        const auto lamps = content.removeFromBottom(12.0f);
        content.removeFromBottom(4.0f);

        if (module == Console::ioModule)
        {
            paintCurve(g, content);
            const bool filters = console_.filtersEngaged();
            paintLamps(g, lamps,
                       {{"FLIP", console_.flip_->get(), ConsoleLookAndFeel::harrisonLamp("flip")},
                        {"48V", console_.phantom_->get(), ConsoleLookAndFeel::harrisonLamp("phantom")},
                        {"PAD", console_.pad_->get(), ConsoleLookAndFeel::harrisonLamp("pad")},
                        {juce::String::fromUTF8("\xc3\x98"), console_.phase_->get(), ConsoleLookAndFeel::harrisonLamp("phase")},
                        {"FILT", filters, ConsoleLookAndFeel::harrisonLamp("filtersin")},
                        {"EQ", console_.eqIn_->get(), ConsoleLookAndFeel::harrisonLamp("eqin")},
                        {"MIX", console_.mix_->get(), ConsoleLookAndFeel::harrisonLamp("mix")}});
        }
        else
        {
            g.setFont(sans(10.0f));
            g.setColour(kPrint);
            const juce::String text = module == Console::busMaster
                                          ? "SUB-GROUP / AUX / CUE MASTER\nsumming amp > master fader > line driver" +
                                                juce::String(console_.transformer_->get() ? " > Jensen transformer" : "")
                                          : "MASTER SECTION\nmix amp > master fader > line drivers\nmonitor: " +
                                                console_.monitorSource_->getCurrentChoiceName() + (console_.mono_->get() ? ", mono" : "");
            g.drawFittedText(text, content.toNearestInt(), juce::Justification::centredLeft, 5);
            if (module == Console::masterSection)
                paintLamps(g, lamps, {{"MONO", console_.mono_->get(), ConsoleLookAndFeel::harrisonLamp("mono")},
                                      {"XFMR", console_.transformer_->get(), ConsoleLookAndFeel::harrisonLamp("transformer")}});
            else
                paintLamps(g, lamps, {{"XFMR", console_.transformer_->get(), ConsoleLookAndFeel::harrisonLamp("transformer")}});
        }
    }

private:
    static float bridgeHeight(juce::Rectangle<float> area) { return std::clamp(area.getHeight() * 0.44f, 40.0f, 84.0f); }

    void paintCurve(juce::Graphics& g, juce::Rectangle<float> area) const
    {
        using namespace theme;
        // A small phosphor scope.
        paintRecess(g, area, juce::Colour(0xff040806));
        const auto inner = area.reduced(2.0f);
        const auto xOf = [&](double hz) { return inner.getX() + inner.getWidth() * static_cast<float>(std::log(hz / 20.0) / std::log(1000.0)); };
        const auto yOf = [&](double db) { return inner.getCentreY() - inner.getHeight() * 0.5f * static_cast<float>(std::clamp(db, -18.0, 18.0) / 18.0); };
        g.setColour(juce::Colour(0xff163a22));
        for (const double hz : {100.0, 1000.0, 10000.0})
            g.drawVerticalLine(juce::roundToInt(xOf(hz)), inner.getY(), inner.getBottom());
        g.drawHorizontalLine(juce::roundToInt(yOf(0.0)), inner.getX(), inner.getRight());
        const bool active = console_.eqIn_->get() || console_.filtersEngaged();
        juce::Path path;
        for (std::size_t i = 0; i < curve_.size(); ++i)
        {
            const double hz = 20.0 * std::pow(1000.0, static_cast<double>(i) / static_cast<double>(curve_.size() - 1));
            const auto point = juce::Point<float>(xOf(hz), yOf(curve_[i]));
            if (i == 0)
                path.startNewSubPath(point);
            else
                path.lineTo(point);
        }
        const juce::Colour trace = active ? juce::Colour(0xff63f08e) : juce::Colour(0xff2c6b40);
        g.setColour(trace.withAlpha(0.25f));
        g.strokePath(path, juce::PathStrokeType(3.0f));
        g.setColour(trace);
        g.strokePath(path, juce::PathStrokeType(1.2f));
        if (!active)
        {
            g.setFont(label(7.5f));
            g.setColour(juce::Colour(0xff3f8f58));
            g.drawText("EQ / FILTERS OUT", inner.reduced(4.0f, 2.0f), juce::Justification::topRight, false);
        }
    }

    struct Lamp
    {
        juce::String name;
        bool on;
        juce::Colour colour;
    };

    static void paintLamps(juce::Graphics& g, juce::Rectangle<float> area, std::initializer_list<Lamp> lamps)
    {
        const float width = area.getWidth() / static_cast<float>(std::max<std::size_t>(lamps.size(), 1));
        for (const auto& lamp : lamps)
        {
            auto cell = area.removeFromLeft(width);
            hardware::paintLed(g, {cell.getX() + 4.0f, cell.getCentreY()}, 2.6f, lamp.colour, lamp.on);
            g.setFont(theme::label(7.0f));
            g.setColour(kPrint.withAlpha(lamp.on ? 1.0f : 0.7f));
            g.drawText(lamp.name, cell.withTrimmedLeft(9.0f), juce::Justification::centredLeft, false);
        }
    }

    void timerCallback() override
    {
        // VU ballistics on the outputs (0 VU = +4 dBu): about 300 ms to settle.
        for (int ch = 0; ch < 2; ++ch)
        {
            const double target = static_cast<double>(console_.takeOutputPeakDbu(ch)) - 4.0;
            auto& vu = vu_[static_cast<std::size_t>(ch)];
            vu += (std::max(target, -40.0) - vu) * (1.0 - std::exp(-(1.0 / 30.0) / 0.065));
        }
        if (fader_ != nullptr)
            fader_->refresh();
        // The curve, when a setting changed.
        std::vector<float> signature;
        for (auto* parameter : console_.getParameters())
            signature.push_back(parameter->getValue());
        if (signature != signature_)
        {
            signature_ = std::move(signature);
            for (std::size_t i = 0; i < curve_.size(); ++i)
                curve_[i] = console_.responseDb(20.0 * std::pow(1000.0, static_cast<double>(i) / static_cast<double>(curve_.size() - 1)));
        }
        repaint();
    }

    Console& console_;
    std::unique_ptr<hardware::ParameterFader> fader_;
    std::array<double, 2> vu_{-40.0, -40.0};
    std::array<double, 96> curve_{};
    std::vector<float> signature_;
};

//==================================================================================================
/** The 32 routing switches: each assigns the channel to its bus (its pair's DAW bus is made and fed
    with the first assignment); Bus Pan divides the channel between odd and even buses. */
class RoutingMatrix final : public juce::Component, private juce::Timer
{
public:
    RoutingMatrix(Console& console, const DeviceEditorContext& context)
        : console_(console)
        , context_(context)
    {
        for (int b = 1; b <= Console::kBuses; ++b)
        {
            auto button = std::make_unique<juce::TextButton>(juce::String(b));
            styleToggle(*button);
            button->setTooltip("Assign the channel to bus " + juce::String(b) + " (its pair goes to the DAW bus \"Bus " +
                               juce::String(b % 2 == 1 ? b : b - 1) + "-" + juce::String(b % 2 == 1 ? b + 1 : b) + "\")");
            button->onClick = [this, b]
            {
                harrison::assignBus(context_.project.getSession(), context_.device, console_, b,
                                    !console_.bus_[static_cast<std::size_t>(b - 1)]->get());
                sync();
            };
            addAndMakeVisible(*button);
            buttons_.push_back(std::move(button));
        }
        sync();
        startTimerHz(4);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(4);
        area.removeFromTop(14);
        const int width = area.getWidth() / 16;
        for (int row = 0; row < 2; ++row)
        {
            auto line = area.removeFromTop(24);
            for (int column = 0; column < 16; ++column)
                buttons_[static_cast<std::size_t>(row * 16 + column)]->setBounds(line.removeFromLeft(width).reduced(1));
            area.removeFromTop(3);
        }
        status_ = area;
    }

    void paint(juce::Graphics& g) override
    {
        using namespace theme;
        paintRecess(g, getLocalBounds().toFloat().reduced(1.0f), juce::Colour(0xff101113));
        g.setColour(kPrint);
        g.setFont(label(8.5f));
        g.drawText("ROUTING  -  32 MULTITRACK BUSES", getLocalBounds().reduced(6, 3).removeFromTop(12), juce::Justification::centredLeft, false);
        g.setFont(sans(10.0f));
        g.setColour(kPrint.withAlpha(0.75f));
        g.drawFittedText(text_, status_.reduced(2), juce::Justification::topLeft, 3);
    }

private:
    void sync()
    {
        juce::StringArray busses;
        for (int b = 1; b <= Console::kBuses; ++b)
        {
            buttons_[static_cast<std::size_t>(b - 1)]->setToggleState(console_.bus_[static_cast<std::size_t>(b - 1)]->get(),
                                                                       juce::dontSendNotification);
            if (b % 2 == 1)
                if (const auto bus = deviceActions::connectedBus(context_.project.getSession(), context_.device, Outputs::bus(b), Outputs::bus(b + 1));
                    bus.isValid())
                    busses.add(session::track::getName(bus));
        }
        const juce::String text = busses.isEmpty() ? "Not assigned: the channel goes to the mix only."
                                                   : "Feeding " + busses.joinIntoString(", ") +
                                                         (console_.busPan_->get() ? " (panned odd / even)" : "");
        if (text != text_)
        {
            text_ = text;
            repaint();
        }
    }
    void timerCallback() override { sync(); }

    Console& console_;
    DeviceEditorContext context_;
    std::vector<std::unique_ptr<juce::TextButton>> buttons_;
    juce::Rectangle<int> status_;
    juce::String text_;
};

//==================================================================================================
/** Connects the module's other outputs (aux, cue, quad 3 - 4, direct) to their busses. */
class ConnectionsPanel final : public juce::Component, private juce::Timer
{
public:
    ConnectionsPanel(const DeviceEditorContext& context, std::vector<harrison::Connection> list)
        : context_(context)
        , list_(std::move(list))
    {
        for (std::size_t i = 0; i < list_.size(); ++i)
        {
            auto button = std::make_unique<juce::TextButton>();
            styleToggle(*button);
            button->onClick = [this, i]
            {
                auto& session = context_.project.getSession();
                const auto& c = list_[i];
                if (deviceActions::connectedBus(session, context_.device, c.left, c.right).isValid())
                    deviceActions::disconnectDeviceOutput(session, context_.device, c.left, c.right);
                else
                    (void) deviceActions::connectDeviceOutput(session, context_.device, c.left, c.right, c.busName);
                sync();
            };
            addAndMakeVisible(*button);
            buttons_.push_back(std::move(button));
        }
        sync();
        startTimerHz(4);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(4);
        area.removeFromTop(14);
        const int height = std::max(16, std::min(22, area.getHeight() / std::max<int>(1, static_cast<int>(buttons_.size()))));
        for (auto& button : buttons_)
            button->setBounds(area.removeFromTop(height).reduced(0, 1));
    }

    void paint(juce::Graphics& g) override
    {
        using namespace theme;
        paintRecess(g, getLocalBounds().toFloat().reduced(1.0f), juce::Colour(0xff101113));
        g.setColour(kPrint);
        g.setFont(label(8.5f));
        g.drawText("TO BUSSES", getLocalBounds().reduced(6, 3).removeFromTop(12), juce::Justification::centredLeft, false);
    }

private:
    void sync()
    {
        for (std::size_t i = 0; i < list_.size(); ++i)
        {
            const auto& c = list_[i];
            const auto bus = deviceActions::connectedBus(context_.project.getSession(), context_.device, c.left, c.right);
            buttons_[i]->setToggleState(bus.isValid(), juce::dontSendNotification);
            buttons_[i]->setButtonText(c.label + "  >  " + (bus.isValid() ? session::track::getName(bus) : c.busName + " (connect)"));
            buttons_[i]->setTooltip(bus.isValid() ? "Click to disconnect" : "Click to send this to the bus \"" + c.busName + "\" (made if needed)");
        }
    }
    void timerCallback() override { sync(); }

    DeviceEditorContext context_;
    std::vector<harrison::Connection> list_;
    std::vector<std::unique_ptr<juce::TextButton>> buttons_;
};

//==================================================================================================
/** The console's display with a row of controls under it (the bus master's solo, the monitor
    matrix's external source). */
class FaceWithRow final : public juce::Component, private juce::Timer
{
public:
    FaceWithRow(Console& console, const DeviceEditorContext& context, bool monitorPicker)
        : face_(console)
        , context_(context)
        , monitorPicker_(monitorPicker)
    {
        addAndMakeVisible(face_);
        styleToggle(button_);
        addAndMakeVisible(button_);
        button_.onClick = [this] { monitorPicker_ ? chooseSource() : toggleSolo(); };
        sync();
        startTimerHz(4);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        button_.setBounds(area.removeFromBottom(24).reduced(2, 1));
        face_.setBounds(area);
    }

private:
    void toggleSolo()
    {
        auto& session = context_.project.getSession();
        const auto track = session::Session::getDeviceOwner(context_.device);
        if (!track.hasType(session::ids::track))
            return;
        session.beginTransaction("Solo");
        session::track::setSoloed(track, !session::track::isSoloed(track), &session.getUndoManager());
        sync();
    }

    void chooseSource()
    {
        auto& session = context_.project.getSession();
        const auto self = session::Session::getDeviceOwner(context_.device);
        juce::PopupMenu menu;
        menu.addItem("None", [this] { setSource(0); });
        juce::PopupMenu tape, busses;
        for (const auto& t : session.getAllTracks())
        {
            if (t == self)
                continue;
            const auto kind = session::track::getKind(t);
            const auto id = session::track::getId(t);
            const auto name = session::track::getName(t);
            if (kind == session::TrackKind::bus)
                busses.addItem(name, [this, id] { setSource(id); });
            else if (kind == session::TrackKind::audio || kind == session::TrackKind::instrument)
                tape.addItem(name, [this, id] { setSource(id); });
        }
        menu.addSubMenu("Tape (tracks)", tape, tape.getNumItems() > 0);
        menu.addSubMenu("Busses (buses, 2-track, aux returns)", busses, busses.getNumItems() > 0);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&button_));
    }

    void setSource(std::uint32_t trackId)
    {
        auto& session = context_.project.getSession();
        session.beginTransaction("Monitor Source");
        session::device::setSidechain(context_.device, trackId, &session.getUndoManager());
        sync();
    }

    void sync()
    {
        auto& session = context_.project.getSession();
        if (monitorPicker_)
        {
            const auto source = session.findTrack(session::device::getSidechain(context_.device));
            button_.setButtonText("EXTERNAL: " + (source.isValid() ? session::track::getName(source) : juce::String("none")) + "  (choose)");
            button_.setToggleState(source.isValid(), juce::dontSendNotification);
        }
        else
        {
            const auto track = session::Session::getDeviceOwner(context_.device);
            const bool soloed = track.hasType(session::ids::track) && session::track::isSoloed(track);
            button_.setButtonText(soloed ? "SOLO (on)" : "SOLO");
            button_.setToggleState(soloed, juce::dontSendNotification);
        }
    }
    void timerCallback() override { sync(); }

    HarrisonFace face_;
    DeviceEditorContext context_;
    bool monitorPicker_;
    juce::TextButton button_;
};

constexpr int kFaceWidth = 320;
} // namespace

//==================================================================================================

HarrisonEditor::HarrisonEditor(Console& console, const DeviceEditorContext& context)
    : console_(console)
    , context_(context)
{
    setLookAndFeel(&look_);
    rebuild();
    startTimerHz(5);
}

HarrisonEditor::~HarrisonEditor()
{
    pages_.reset();
    setLookAndFeel(nullptr);
}

int HarrisonEditor::armrestHeight() const noexcept
{
    const int controls = PagedEditor::kTabHeight + 4 + PagedEditor::kRows * ParameterControl::kHeight;
    return std::clamp(getHeight() - controls - 4, 0, kArmrestHeight);
}

void HarrisonEditor::paint(juce::Graphics& g)
{
    const auto area = getLocalBounds().toFloat();
    const auto cheek = static_cast<float>(kCheekWidth);
    auto panel = area.reduced(cheek, 0.0f);

    // The black panel, the page keys' strip along its top, the channel strips' seams.
    hardware::paintMetal(g, panel, kPanel, 0.0f, false);
    g.setColour(juce::Colour(0xff0b0b0c));
    g.fillRect(panel.withHeight(static_cast<float>(PagedEditor::kTabHeight) + 2.0f));
    g.setColour(juce::Colour(0xff3a3c40));
    g.drawHorizontalLine(PagedEditor::kTabHeight + 2, panel.getX(), panel.getRight());
    if (pages_ != nullptr)
    {
        const auto grid = pages_->getGridBounds().translated(pages_->getX(), pages_->getY()).toFloat();
        for (float x = grid.getX(); x <= grid.getRight() + 0.5f; x += static_cast<float>(ParameterControl::kWidth))
        {
            g.setColour(juce::Colours::black);
            g.drawVerticalLine(juce::roundToInt(x), grid.getY(), grid.getBottom());
            g.setColour(juce::Colour(0xff2a2c30));
            g.drawVerticalLine(juce::roundToInt(x) + 1, grid.getY(), grid.getBottom());
        }
    }

    // The leather armrest along the front, between the walnut end cheeks.
    if (const int armrest = armrestHeight(); armrest >= 6)
        hardware::paintLeather(g, panel.removeFromBottom(static_cast<float>(armrest)).expanded(2.0f, 0.0f), kLeather);
    hardware::paintWood(g, area.withWidth(cheek), kWalnut, 3, 3.0f);
    hardware::paintWood(g, area.withLeft(area.getRight() - cheek), kWalnut, 4, 3.0f);
    g.setColour(juce::Colours::black.withAlpha(0.45f));
    g.drawVerticalLine(kCheekWidth, 0.0f, area.getBottom());
    g.drawVerticalLine(getWidth() - kCheekWidth - 1, 0.0f, area.getBottom());
}

std::vector<Page> HarrisonEditor::pagesFor(Console& console, const DeviceEditorContext& context, int module, int revision, bool, int panMode)
{
    const auto& spec = Console::revisionSpec(revision);
    const auto face = [&console] { return std::unique_ptr<juce::Component>(std::make_unique<HarrisonFace>(console)); };
    switch (module)
    {
    case Console::busMaster:
        return {{"Bus Master",
                 ids({"module|Module", "revision|Revision", "linetrim|Input Trim", "fader|Master", "transformer|Out Xfmr"}),
                 [&console, context] { return std::unique_ptr<juce::Component>(std::make_unique<FaceWithRow>(console, context, false)); },
                 kFaceWidth}};
    case Console::masterSection:
        return {{"Master", ids({"module|Module", "revision|Revision", "fader|Master Fader", "transformer|Out Xfmr"}), face, kFaceWidth},
                {"Monitor", ids({"crm|Source", "crmlevel|Volume", "mono|Mono"}),
                 [&console, context] { return std::unique_ptr<juce::Component>(std::make_unique<FaceWithRow>(console, context, true)); },
                 kFaceWidth}};
    default:
        break;
    }

    // The I/O module: what this revision has.
    Parameters filters = ids({"hpf|HPF", "lpf|LPF"});
    if (spec.filtersSwitch)
        filters.emplace_back("filtersin", "Filters In");
    Parameters low = ids({"lffreq|LF Freq", "lfgain|LF Gain"});
    if (spec.shelves)
        low.emplace_back("lfshelf", "LF Shelf");
    low.emplace_back("lmffreq", "LMF Freq");
    low.emplace_back("lmfgain", "LMF Gain");
    if (spec.variableQ)
        low.emplace_back("lmfq", "LMF Q");
    low.emplace_back("eqin", "EQ In");
    Parameters high = ids({"hmffreq|HMF Freq", "hmfgain|HMF Gain"});
    if (spec.variableQ)
        high.emplace_back("hmfq", "HMF Q");
    high.emplace_back("hffreq", "HF Freq");
    high.emplace_back("hfgain", "HF Gain");
    if (spec.shelves)
        high.emplace_back("hfshelf", "HF Shelf");
    high.emplace_back("eqin", "EQ In");
    Parameters pan = ids({"fader|Fader", "pan|Pan", "panmode|Pan Mode"});
    if (panMode != Console::stereoPan)
        pan.emplace_back("frontback", panMode == Console::lcrsPan ? "Surround" : "Front/Back");
    pan.emplace_back("mix", "Mix");
    pan.emplace_back("format", "Format");

    const auto all = harrison::connections(context.device, session::Session::getDeviceOwner(context.device));
    const auto panel = [context](std::vector<harrison::Connection> list)
    { return [context, list] { return std::unique_ptr<juce::Component>(std::make_unique<ConnectionsPanel>(context, list)); }; };
    const std::vector<harrison::Connection> auxes(all.begin(), all.begin() + 4);
    const std::vector<harrison::Connection> others(all.begin() + 4, all.end());

    return {{"Input",
             ids({"module|Module", "revision|Revision", "input|Input", "micgain|Mic Gain", "linetrim|Line Trim", "pad|Pad", "phase|Phase",
                  "phantom|+48V", "noise|Noise"}),
             face,
             kFaceWidth},
            {"Filters", filters, face, kFaceWidth},
            {"EQ Low", low, face, kFaceWidth},
            {"EQ High", high, face, kFaceWidth},
            {"Fader & Pan", pan, face, kFaceWidth},
            {"Monitor", ids({"flip|Flip", "monlevel|Mon Level", "monpan|Mon Pan", "cuefrom|Cue From"}), face, kFaceWidth},
            {"Aux", ids({"aux1|Aux 1", "aux1pre|Pre", "aux2|Aux 2", "aux2pre|Pre", "aux3|Aux 3", "aux3pre|Pre", "aux4|Aux 4", "aux4pre|Pre"}),
             panel(auxes), 230},
            {"Cue", ids({"cue1|Cue 1", "cue1pan|Pan", "cue2|Cue 2", "cue2pan|Pan"}), panel(others), 230},
            {"Routing", ids({"buspan|Bus Pan", "direct|Direct"}),
             [&console, context] { return std::unique_ptr<juce::Component>(std::make_unique<RoutingMatrix>(console, context)); }, 420}};
}

int HarrisonEditor::preferredWidth(Console& console, const DeviceEditorContext& context)
{
    int width = 0;
    for (int module = 0; module < 3; ++module)
        for (int revision = 0; revision < 3; ++revision)
            for (int panMode = 0; panMode < 3; ++panMode)
                width = std::max(width, PagedEditor::preferredWidth(pagesFor(console, context, module, revision, false, panMode)));
    return width + 2 * kCheekWidth;
}

void HarrisonEditor::rebuild()
{
    shown_ = {console_.getModule(), console_.getRevision(), console_.format_->getIndex(), console_.panMode_->getIndex()};
    const std::size_t page = pages_ != nullptr ? pages_->getPage() : 0;
    pages_ = std::make_unique<PagedEditor>(console_, pagesFor(console_, context_, shown_[0], shown_[1], shown_[2] == 1, shown_[3]));
    pages_->showPage(std::min(page, pages_->getNumPages() - 1));
    addAndMakeVisible(*pages_);
    resized();
}

void HarrisonEditor::timerCallback()
{
    const std::array<int, 4> now{console_.getModule(), console_.getRevision(), console_.format_->getIndex(), console_.panMode_->getIndex()};
    if (now != shown_)
        rebuild();
}

void HarrisonEditor::resized()
{
    if (pages_ != nullptr)
        pages_->setBounds(getLocalBounds().reduced(kCheekWidth, 0).withTrimmedBottom(armrestHeight()));
}

std::unique_ptr<juce::Component> createHarrisonDeviceEditor(juce::AudioProcessor& processor, const DeviceEditorContext& context, int& preferredWidth)
{
    auto* console = dynamic_cast<Console*>(&processor);
    if (console == nullptr)
        return nullptr;
    preferredWidth = HarrisonEditor::preferredWidth(*console, context);
    return std::make_unique<HarrisonEditor>(*console, context);
}

} // namespace dojo::ui
