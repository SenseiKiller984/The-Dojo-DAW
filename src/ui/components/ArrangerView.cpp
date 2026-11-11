#include "ui/components/ArrangerView.h"

#include "ui/BrowserDrag.h"
#include "ui/WaveformPainter.h"

#include "ui/EditActions.h"
#include "ui/theme/DojoTheme.h"

#include <algorithm>
#include <cmath>

namespace dojo::ui
{
namespace
{
constexpr int kTrackHeight = 64;
constexpr int kLaneHeight = 48;
constexpr int kScrollbarThickness = 10;
constexpr int kExtraSpaceBelow = 2 * kTrackHeight; // empty drop zone below the last track
constexpr int kHeaderButtonWidth = 20;
constexpr int kIndent = 14;
constexpr int kLoopStripHeight = 11;
constexpr float kEdgeGrabWidth = 6.0f;
constexpr float kClipTitleHeight = 16.0f;
constexpr float kFadeHandleSize = 8.0f;
constexpr float kPointRadius = 4.5f;
constexpr double kMinPixelsPerSecond = 2.0;
constexpr double kMaxPixelsPerSecond = 2000.0;
constexpr int kBeatsPerBar = 4;

const char* const kColourNames[] = {"Vermilion", "Persimmon", "Gold", "Matcha", "Jade", "Indigo", "Wisteria", "Sakura"};

juce::String formatDb(double db)
{
    if (db <= session::kMinusInfinityDb + 0.001)
        return "-inf dB";
    return (db > 0.0 ? "+" : "") + juce::String(db, 1) + " dB";
}

juce::String formatPan(double pan)
{
    const int percent = juce::roundToInt(std::abs(pan) * 100.0);
    if (percent == 0)
        return "C";
    return (pan < 0.0 ? "L" : "R") + juce::String(percent);
}

juce::String kindLabel(const juce::ValueTree& t)
{
    if (session::track::isFolder(t))
        return "Folder";
    switch (session::track::getKind(t))
    {
    case session::TrackKind::instrument: return "Instrument";
    case session::TrackKind::bus:        return "Bus";
    case session::TrackKind::audio:      break;
    }
    return "Audio";
}
} // namespace

ArrangerView::ArrangerView(session::Project& project, engine::EngineController& controller, EditorState& editor)
    : project_(project)
    , session_(project.getSession())
    , controller_(controller)
    , editor_(editor)
{
    setOpaque(true);
    setWantsKeyboardFocus(true);

    for (auto* bar : {&horizontalScroll_, &verticalScroll_})
    {
        bar->setAutoHide(false);
        bar->addListener(this);
        addAndMakeVisible(bar);
    }

    addTrackButton_.setButtonText("+  Add Track");
    addTrackButton_.setWantsKeyboardFocus(false);
    addTrackButton_.setTooltip("Add a track or folder");
    addTrackButton_.onClick = [this]
    {
        juce::Component::SafePointer<ArrangerView> safe(this);
        juce::PopupMenu menu;
        menu.addItem("Audio Track", [safe] { if (safe != nullptr) actions::addTrack(safe->session_, safe->editor_); });
        menu.addItem("Instrument Track (Drum Rack)", [safe]
                     { if (safe != nullptr) actions::addInstrumentTrack(safe->session_, safe->editor_, "builtin:drumrack", "Drum Rack"); });
        menu.addItem("Instrument Track (Sampler)", [safe]
                     { if (safe != nullptr) actions::addInstrumentTrack(safe->session_, safe->editor_, "builtin:sampler", "Sampler"); });
        menu.addItem("Empty Instrument Track", [safe]
                     { if (safe != nullptr) actions::addTrack(safe->session_, safe->editor_, session::TrackKind::instrument); });
        menu.addItem("Bus (Group / Return)", [safe]
                     { if (safe != nullptr) actions::addTrack(safe->session_, safe->editor_, session::TrackKind::bus); });
        menu.addItem("Folder", [safe] { if (safe != nullptr) actions::addFolder(safe->session_, safe->editor_); });
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&addTrackButton_));
    };
    addAndMakeVisible(addTrackButton_);

    renameEditor_ = std::make_unique<juce::TextEditor>();
    renameEditor_->setFont(theme::sansBold(13.0f));
    renameEditor_->setJustification(juce::Justification::centredLeft);
    renameEditor_->onReturnKey = [this] { finishRename(true); };
    renameEditor_->onEscapeKey = [this] { finishRename(false); };
    renameEditor_->onFocusLost = [this] { finishRename(true); };
    addChildComponent(*renameEditor_);

    session_.getState().addListener(this);
    editor_.addListener(this);
    project_.getAssets().addListener(this);
}

ArrangerView::~ArrangerView()
{
    project_.getAssets().removeListener(this);
    editor_.removeListener(this);
    session_.getState().removeListener(this);
}

//==================================================================================================
// Rows

void ArrangerView::ensureRows() const
{
    if (!rowsDirty_)
        return;
    rowsDirty_ = false;
    rows_.clear();

    int y = 0;
    for (const auto& t : session_.getAllTracks())
    {
        if (!session::Session::isVisibleInArranger(t))
            continue;

        Row row;
        row.kind = Row::Kind::track;
        row.track = t;
        row.depth = session::Session::getDepth(t);
        row.top = y;
        row.height = kTrackHeight;
        rows_.push_back(row);
        y += kTrackHeight;

        if (!session::track::isShowingAutomation(t))
            continue;

        for (const auto& lane : session::Session::getAutomationLanes(t))
        {
            if (!session::automation::isVisible(lane))
                continue;
            Row laneRow;
            laneRow.kind = Row::Kind::automation;
            laneRow.track = t;
            laneRow.lane = lane;
            laneRow.depth = row.depth;
            laneRow.top = y;
            laneRow.height = kLaneHeight;
            rows_.push_back(laneRow);
            y += kLaneHeight;
        }
    }
}

int ArrangerView::rowAt(int y) const
{
    if (y < theme::metrics::rulerHeight)
        return -1;
    const int contentY = y - theme::metrics::rulerHeight + scrollY_;
    const auto& all = rows();
    for (std::size_t i = 0; i < all.size(); ++i)
        if (contentY >= all[i].top && contentY < all[i].top + all[i].height)
            return static_cast<int>(i);
    return -1;
}

int ArrangerView::rowOfTrack(const juce::ValueTree& t) const
{
    const auto& all = rows();
    for (std::size_t i = 0; i < all.size(); ++i)
        if (all[i].kind == Row::Kind::track && all[i].track == t)
            return static_cast<int>(i);
    return -1;
}

std::vector<std::uint32_t> ArrangerView::shownTrackOrder() const
{
    std::vector<std::uint32_t> order;
    for (const int r : trackRows())
        order.push_back(session::track::getId(rows()[static_cast<std::size_t>(r)].track));
    return order;
}

std::vector<juce::ValueTree> ArrangerView::selectedTracksInOrder(bool outermostOnly) const
{
    if (outermostOnly)
        return actions::getSelectedTracks(session_, editor_);
    std::vector<juce::ValueTree> selected;
    for (const auto& t : session_.getAllTracks())
        if (editor_.isTrackSelected(session::track::getId(t)))
            selected.push_back(t);
    return selected;
}

void ArrangerView::selectTrackWithModifiers(const juce::ValueTree& track, juce::ModifierKeys mods)
{
    editor_.selectTrackWithModifiers(session::track::getId(track), mods.isCommandDown(), mods.isShiftDown(), shownTrackOrder());
}

juce::Rectangle<int> ArrangerView::getTrackHeaderArea(const juce::ValueTree& track) const
{
    const int row = rowOfTrack(track);
    return row >= 0 ? getRowBounds(row).withWidth(theme::metrics::trackHeaderWidth) : juce::Rectangle<int>();
}

juce::Rectangle<int> ArrangerView::getTrackButtonArea(const juce::ValueTree& track, int indexFromRight) const
{
    const int row = rowOfTrack(track);
    return row >= 0 ? getHeaderButton(row, indexFromRight) : juce::Rectangle<int>();
}

std::vector<int> ArrangerView::trackRows() const
{
    std::vector<int> result;
    const auto& all = rows();
    for (std::size_t i = 0; i < all.size(); ++i)
        if (all[i].kind == Row::Kind::track)
            result.push_back(static_cast<int>(i));
    return result;
}

void ArrangerView::structureChanged()
{
    rowsDirty_ = true;
    setScroll(scrollSeconds_, scrollY_);
}

//==================================================================================================
// Geometry

juce::Rectangle<int> ArrangerView::getRulerArea() const
{
    return {theme::metrics::trackHeaderWidth, 0, juce::jmax(0, getWidth() - theme::metrics::trackHeaderWidth - kScrollbarThickness),
            theme::metrics::rulerHeight};
}

juce::Rectangle<int> ArrangerView::getLoopStrip() const
{
    return getRulerArea().withHeight(kLoopStripHeight);
}

juce::Rectangle<int> ArrangerView::getHeaderArea() const
{
    return {0, theme::metrics::rulerHeight, theme::metrics::trackHeaderWidth,
            juce::jmax(0, getHeight() - theme::metrics::rulerHeight - kScrollbarThickness)};
}

juce::Rectangle<int> ArrangerView::getLaneArea() const
{
    return {theme::metrics::trackHeaderWidth, theme::metrics::rulerHeight,
            juce::jmax(0, getWidth() - theme::metrics::trackHeaderWidth - kScrollbarThickness),
            juce::jmax(0, getHeight() - theme::metrics::rulerHeight - kScrollbarThickness)};
}

juce::Rectangle<int> ArrangerView::getRowBounds(int row) const
{
    const auto& all = rows();
    if (row < 0 || row >= static_cast<int>(all.size()))
    {
        const int bottom = all.empty() ? 0 : all.back().top + all.back().height;
        return {0, theme::metrics::rulerHeight + bottom - scrollY_, getWidth() - kScrollbarThickness, kTrackHeight};
    }
    const auto& r = all[static_cast<std::size_t>(row)];
    return {0, theme::metrics::rulerHeight + r.top - scrollY_, getWidth() - kScrollbarThickness, r.height};
}

float ArrangerView::timeToX(double seconds) const noexcept
{
    return static_cast<float>(theme::metrics::trackHeaderWidth) + static_cast<float>((seconds - scrollSeconds_) * pixelsPerSecond_);
}

double ArrangerView::xToTime(float x) const noexcept
{
    return scrollSeconds_ + static_cast<double>(x - static_cast<float>(theme::metrics::trackHeaderWidth)) / pixelsPerSecond_;
}

double ArrangerView::getVisibleSeconds() const noexcept
{
    return static_cast<double>(getLaneArea().getWidth()) / pixelsPerSecond_;
}

juce::Rectangle<float> ArrangerView::getClipBounds(const juce::ValueTree& clip, int row) const
{
    const auto bounds = getRowBounds(row).toFloat();
    const float x0 = timeToX(session::clip::getStart(clip));
    const float x1 = timeToX(session::clip::getEnd(clip));
    return {x0, bounds.getY() + 3.0f, juce::jmax(2.0f, x1 - x0), bounds.getHeight() - 6.0f};
}

void ArrangerView::selectClipWithModifiers(const juce::ValueTree& clip, juce::ModifierKeys mods)
{
    const auto id = session::clip::getId(clip);

    if (mods.isShiftDown())
    {
        // Explorer-style range: every clip on this track between the anchor and this one (by start
        // time; an anchor on another track contributes its time). The anchor stays.
        auto anchor = session_.findClip(selectionAnchor_);
        if (!anchor.isValid())
        {
            anchor = clip;
            selectionAnchor_ = id;
        }
        const double from = std::min(session::clip::getStart(anchor), session::clip::getStart(clip));
        const double to = std::max(session::clip::getStart(anchor), session::clip::getStart(clip));
        auto selection = mods.isCommandDown() ? editor_.getSelectedClips() : std::set<std::uint32_t>{};
        for (const auto& c : clip.getParent())
            if (session::Session::isClip(c) && session::clip::getStart(c) >= from - 1.0e-9 && session::clip::getStart(c) <= to + 1.0e-9)
                selection.insert(session::clip::getId(c));
        editor_.setSelectedClips(std::move(selection));
        return;
    }

    if (mods.isCommandDown())
        editor_.toggleClip(id);
    else if (!editor_.isClipSelected(id)) // a selected clip keeps the selection: it may be dragged
        editor_.selectClip(id, false);
    selectionAnchor_ = id;
}

void ArrangerView::selectClipsInArea(juce::Rectangle<int> area, const std::set<std::uint32_t>& base)
{
    const double t0 = xToTime(static_cast<float>(area.getX()));
    const double t1 = xToTime(static_cast<float>(area.getRight()));
    const int y0 = area.getY() - theme::metrics::rulerHeight + scrollY_;
    const int y1 = area.getBottom() - theme::metrics::rulerHeight + scrollY_;

    auto selection = base;
    for (const auto& row : rows())
    {
        if (row.kind != Row::Kind::track)
            continue;
        // Clips are drawn 3 px inside their row.
        if (row.top + row.height - 3 <= y0 || row.top + 3 >= y1)
            continue;
        for (const auto& c : row.track)
            if (session::Session::isClip(c) && session::clip::getEnd(c) > t0 && session::clip::getStart(c) < t1)
                selection.insert(session::clip::getId(c));
    }
    if (selection != editor_.getSelectedClips())
        editor_.setSelectedClips(std::move(selection));
}

juce::Rectangle<int> ArrangerView::getMarqueeArea() const
{
    if (!marqueeActive_)
        return {};
    const auto toScreen = [this](const TimelinePoint& p)
    {
        return juce::Point<int>(juce::roundToInt(timeToX(p.seconds)), p.contentY + theme::metrics::rulerHeight - scrollY_);
    };
    return juce::Rectangle<int>(toScreen(marqueeAnchor_), toScreen(marqueeEnd_));
}

void ArrangerView::dragMarquee(const juce::MouseEvent& e)
{
    if (!marqueeActive_)
    {
        if (e.position.getDistanceFrom(dragStart_) < 4.0f)
            return;
        marqueeActive_ = true;
        beginDragAutoRepeat(40); // keeps scrolling while the mouse rests beyond an edge
    }

    // Scroll when the mouse goes past the lanes.
    const auto lanes = getLaneArea();
    float dx = 0.0f;
    int dy = 0;
    if (e.position.x > static_cast<float>(lanes.getRight()))
        dx = e.position.x - static_cast<float>(lanes.getRight());
    else if (e.position.x < static_cast<float>(lanes.getX()))
        dx = e.position.x - static_cast<float>(lanes.getX());
    if (e.position.y > static_cast<float>(lanes.getBottom()))
        dy = juce::roundToInt(e.position.y) - lanes.getBottom();
    else if (e.position.y < static_cast<float>(lanes.getY()))
        dy = juce::roundToInt(e.position.y) - lanes.getY();
    if (std::abs(dx) > 0.0f || dy != 0)
    {
        setScroll(scrollSeconds_ + static_cast<double>(dx) * 0.5 / pixelsPerSecond_, scrollY_ + dy / 2);
        updateScrollbars();
    }

    const float x = juce::jlimit(static_cast<float>(lanes.getX()), static_cast<float>(lanes.getRight()), e.position.x);
    const int y = juce::jlimit(lanes.getY(), lanes.getBottom(), juce::roundToInt(e.position.y));
    marqueeEnd_ = {xToTime(x), y - theme::metrics::rulerHeight + scrollY_};
    selectClipsInArea(getMarqueeArea(), marqueeBase_);
    repaint();
}

void ArrangerView::paintMarquee(juce::Graphics& g) const
{
    const auto area = getMarqueeArea();
    if (area.isEmpty())
        return;
    g.saveState();
    g.reduceClipRegion(getLaneArea());
    g.setColour(theme::colour::accent.withAlpha(0.12f));
    g.fillRect(area);
    g.setColour(theme::colour::accent.withAlpha(0.85f));
    g.drawRect(area, 1);
    g.restoreState();
}

void ArrangerView::setViewState(const ViewState& state)
{
    pixelsPerSecond_ = state.pixelsPerSecond;
    setScroll(state.scrollSeconds, state.scrollY);
    updateScrollbars();
    repaint();
}

void ArrangerView::scrollToTime(double seconds)
{
    setScroll(seconds, scrollY_);
    updateScrollbars();
    repaint();
}

juce::Rectangle<int> ArrangerView::getClipArea(const juce::ValueTree& clip) const
{
    const auto& all = rows();
    for (std::size_t i = 0; i < all.size(); ++i)
        if (all[i].kind == Row::Kind::track && all[i].track == clip.getParent())
            return getClipBounds(clip, static_cast<int>(i)).toNearestInt();
    return {};
}

juce::Rectangle<int> ArrangerView::getHeaderButton(int row, int indexFromRight) const
{
    const auto bounds = getRowBounds(row);
    const int right = theme::metrics::trackHeaderWidth - 10;
    return {right - (indexFromRight + 1) * (kHeaderButtonWidth + 3), bounds.getY() + 8, kHeaderButtonWidth, 18};
}

juce::Rectangle<int> ArrangerView::getFoldButton(int row) const
{
    const auto bounds = getRowBounds(row);
    const int depth = rows()[static_cast<std::size_t>(row)].depth;
    return {8 + depth * kIndent, bounds.getY() + 9, 14, 16};
}

juce::Rectangle<int> ArrangerView::getNameBounds(int row) const
{
    const auto bounds = getRowBounds(row);
    const int depth = rows()[static_cast<std::size_t>(row)].depth;
    const int left = 26 + depth * kIndent;
    return {left, bounds.getY() + 8, juce::jmax(20, getHeaderButton(row, 3).getX() - left - 6), 20};
}

double ArrangerView::getTempo() const
{
    return session_.getTempo();
}

double ArrangerView::snap(double seconds, const juce::ModifierKeys& mods) const
{
    return editor_.snap(seconds, getTempo(), mods.isAltDown() || mods.isShiftDown());
}

ArrangerView::ClipHit ArrangerView::hitTestClip(juce::Point<float> position) const
{
    const int row = rowAt(juce::roundToInt(position.y));
    if (row < 0 || !getLaneArea().toFloat().contains(position))
        return {};

    const auto& r = rows()[static_cast<std::size_t>(row)];
    if (r.kind != Row::Kind::track)
        return {};

    // Last child is drawn on top, so test in reverse.
    for (int i = r.track.getNumChildren() - 1; i >= 0; --i)
    {
        const auto c = r.track.getChild(i);
        if (!session::Session::isClip(c))
            continue;

        const auto bounds = getClipBounds(c, row);
        if (!bounds.contains(position))
            continue;

        ClipHit hit{c, row, ClipZone::body};
        const bool audio = !session::clip::isMidi(c);
        const float fadeInX = bounds.getX() + static_cast<float>(session::clip::getFadeIn(c) * pixelsPerSecond_);
        const float fadeOutX = bounds.getRight() - static_cast<float>(session::clip::getFadeOut(c) * pixelsPerSecond_);
        const float handleY = bounds.getY() + kClipTitleHeight;
        const bool nearHandleRow = std::abs(position.y - handleY) <= kFadeHandleSize;

        if (audio && nearHandleRow && std::abs(position.x - fadeInX) <= kFadeHandleSize)
            hit.zone = ClipZone::fadeInHandle;
        else if (audio && nearHandleRow && std::abs(position.x - fadeOutX) <= kFadeHandleSize)
            hit.zone = ClipZone::fadeOutHandle;
        else if (bounds.getWidth() > 3.0f * kEdgeGrabWidth && position.x - bounds.getX() <= kEdgeGrabWidth)
            hit.zone = ClipZone::leftEdge;
        else if (bounds.getWidth() > 3.0f * kEdgeGrabWidth && bounds.getRight() - position.x <= kEdgeGrabWidth)
            hit.zone = ClipZone::rightEdge;

        return hit;
    }

    return {};
}

double ArrangerView::getContentDuration() const
{
    return juce::jmax(120.0, juce::jmax(session_.getEndTime(), playheadSeconds_) + 30.0);
}

int ArrangerView::getContentHeight() const
{
    const auto& all = rows();
    const int bottom = all.empty() ? 0 : all.back().top + all.back().height;
    return bottom + kExtraSpaceBelow;
}

//==================================================================================================
// Automation geometry and text

float ArrangerView::valueToY(const juce::Rectangle<int>& row, double value) const
{
    const float top = static_cast<float>(row.getY()) + 6.0f;
    const float height = static_cast<float>(row.getHeight()) - 12.0f;
    return top + static_cast<float>(1.0 - juce::jlimit(0.0, 1.0, value)) * height;
}

double ArrangerView::yToValue(const juce::Rectangle<int>& row, float y) const
{
    const float top = static_cast<float>(row.getY()) + 6.0f;
    const float height = static_cast<float>(row.getHeight()) - 12.0f;
    return juce::jlimit(0.0, 1.0, 1.0 - static_cast<double>((y - top) / height));
}

juce::ValueTree ArrangerView::hitTestPoint(int row, juce::Point<float> position) const
{
    const auto& r = rows()[static_cast<std::size_t>(row)];
    const auto bounds = getRowBounds(row);
    for (const auto& p : r.lane)
    {
        const juce::Point<float> centre{timeToX(p.getProperty(session::ids::time)),
                                        valueToY(bounds, p.getProperty(session::ids::value))};
        if (centre.getDistanceFrom(position) <= kPointRadius + 3.0f)
            return p;
    }
    return {};
}

juce::String ArrangerView::describeTarget(const juce::ValueTree& lane) const
{
    const auto target = session::automation::getTarget(lane);
    if (target == session::automation::volumeTarget())
        return "Volume";
    if (target == session::automation::panTarget())
        return "Pan";

    std::uint32_t id = 0;
    if (session::automation::parseSendTarget(target, id))
    {
        const auto owner = lane.getParent();
        for (const auto& s : session::Session::getSends(owner))
            if (session::send::getId(s) == id)
                if (const auto bus = session_.findTrack(session::send::getTarget(s)); bus.isValid())
                    return "Send > " + session::track::getName(bus);
        return "Send";
    }

    int index = 0;
    if (session::automation::parseParameterTarget(target, id, index))
    {
        const auto deviceTree = session_.findDevice(id);
        juce::String name = deviceTree.isValid() ? session::device::getName(deviceTree) : juce::String("Device");
        if (auto node = project_.getDevices().findNode(id))
        {
            const auto& parameters = node->getProcessor().getParameters();
            if (index < parameters.size())
                name << ": " << parameters[index]->getName(40);
        }
        return name;
    }
    return target;
}

juce::String ArrangerView::describeValue(const juce::ValueTree& lane, double value) const
{
    const auto target = session::automation::getTarget(lane);
    std::uint32_t id = 0;
    int index = 0;

    if (target == session::automation::volumeTarget() || session::automation::parseSendTarget(target, id))
    {
        const double gain = engine::automationToGain(value);
        return gain <= 0.0 ? juce::String("-inf dB") : formatDb(juce::Decibels::gainToDecibels(gain));
    }
    if (target == session::automation::panTarget())
        return formatPan(value * 2.0 - 1.0);
    if (session::automation::parseParameterTarget(target, id, index))
        if (auto node = project_.getDevices().findNode(id))
        {
            const auto& parameters = node->getProcessor().getParameters();
            if (index < parameters.size())
                return parameters[index]->getText(static_cast<float>(value), 24) + " " + parameters[index]->getLabel();
        }
    return juce::String(value, 2);
}

double ArrangerView::currentValueFor(const juce::ValueTree& t, const juce::String& target) const
{
    if (target == session::automation::volumeTarget())
        return engine::gainToAutomation(juce::Decibels::decibelsToGain(session::track::getVolumeDb(t), session::kMinusInfinityDb));
    if (target == session::automation::panTarget())
        return (session::track::getPan(t) + 1.0) * 0.5;

    std::uint32_t id = 0;
    int index = 0;
    if (session::automation::parseSendTarget(target, id))
        for (const auto& s : session::Session::getSends(t))
            if (session::send::getId(s) == id)
                return engine::gainToAutomation(juce::Decibels::decibelsToGain(session::send::getGainDb(s), session::kMinusInfinityDb));
    if (session::automation::parseParameterTarget(target, id, index))
        if (auto node = project_.getDevices().findNode(id))
        {
            const auto& parameters = node->getProcessor().getParameters();
            if (index < parameters.size())
                return parameters[index]->getValue();
        }
    return 0.5;
}

//==================================================================================================
// Layout and scrolling

void ArrangerView::resized()
{
    const auto lanes = getLaneArea();
    horizontalScroll_.setBounds(lanes.getX(), lanes.getBottom(), lanes.getWidth(), kScrollbarThickness);
    verticalScroll_.setBounds(lanes.getRight(), lanes.getY(), kScrollbarThickness, lanes.getHeight());
    setScroll(scrollSeconds_, scrollY_);
}

void ArrangerView::setScroll(double seconds, int y)
{
    const int maxY = juce::jmax(0, getContentHeight() - getLaneArea().getHeight());
    scrollSeconds_ = juce::jmax(0.0, seconds);
    scrollY_ = juce::jlimit(0, maxY, y);

    const auto& all = rows();
    const int bottom = all.empty() ? 0 : all.back().top + all.back().height;
    const int addY = theme::metrics::rulerHeight + bottom - scrollY_ + 10;
    addTrackButton_.setBounds(12, addY, theme::metrics::trackHeaderWidth - 24, 28);
    addTrackButton_.setVisible(addY >= theme::metrics::rulerHeight && addY + 28 <= getHeaderArea().getBottom());

    if (renameEditor_->isVisible())
        finishRename(true);

    updateScrollbars();
    repaint();
}

void ArrangerView::updateScrollbars()
{
    const double visible = getVisibleSeconds();
    horizontalScroll_.setRangeLimits(0.0, juce::jmax(getContentDuration(), scrollSeconds_ + visible), juce::dontSendNotification);
    horizontalScroll_.setCurrentRange(scrollSeconds_, visible, juce::dontSendNotification);

    const int visibleHeight = getLaneArea().getHeight();
    verticalScroll_.setRangeLimits(0.0, juce::jmax(getContentHeight(), visibleHeight), juce::dontSendNotification);
    verticalScroll_.setCurrentRange(scrollY_, visibleHeight, juce::dontSendNotification);
}

void ArrangerView::scrollBarMoved(juce::ScrollBar* bar, double newRangeStart)
{
    if (bar == &horizontalScroll_)
        setScroll(newRangeStart, scrollY_);
    else
        setScroll(scrollSeconds_, juce::roundToInt(newRangeStart));
}

void ArrangerView::zoomBy(double factor)
{
    const auto lanes = getLaneArea();
    float anchorX = timeToX(playheadSeconds_);
    if (anchorX < static_cast<float>(lanes.getX()) || anchorX > static_cast<float>(lanes.getRight()))
        anchorX = static_cast<float>(lanes.getCentreX());

    const double anchorTime = xToTime(anchorX);
    pixelsPerSecond_ = juce::jlimit(kMinPixelsPerSecond, kMaxPixelsPerSecond, pixelsPerSecond_ * factor);
    setScroll(anchorTime - static_cast<double>(anchorX - static_cast<float>(lanes.getX())) / pixelsPerSecond_, scrollY_);
}

void ArrangerView::zoomToFit()
{
    const double end = session_.getEndTime() > 0.0 ? session_.getEndTime() : 60.0;
    pixelsPerSecond_ = juce::jlimit(kMinPixelsPerSecond, kMaxPixelsPerSecond, static_cast<double>(getLaneArea().getWidth()) / (end * 1.05));
    setScroll(0.0, scrollY_);
}

void ArrangerView::setPlayhead(double seconds, bool playing)
{
    const auto lanes = getLaneArea();
    const float oldX = timeToX(playheadSeconds_);
    const float newX = timeToX(seconds);

    if (std::abs(newX - oldX) < 0.25f)
        return;

    playheadSeconds_ = seconds;

    // Page-follow while playing, but only if the playhead was on screen.
    const bool wasVisible = oldX >= static_cast<float>(lanes.getX()) && oldX <= static_cast<float>(lanes.getRight());
    if (playing && wasVisible && newX > static_cast<float>(lanes.getRight() - 20))
    {
        setScroll(seconds - getVisibleSeconds() * 0.05, scrollY_);
        return;
    }

    if (recording_)
    {
        repaint();
        return;
    }

    // Automation lane headers show the value at the playhead.
    for (const auto& row : rows())
        if (row.kind == Row::Kind::automation)
        {
            repaint(getHeaderArea());
            break;
        }

    repaint(juce::roundToInt(oldX) - 8, 0, 16, getHeight());
    repaint(juce::roundToInt(newX) - 8, 0, 16, getHeight());
}

void ArrangerView::setRecording(bool recording, double startSeconds)
{
    if (recording == recording_ && std::abs(startSeconds - recordStart_) < 1.0e-9)
        return;
    recording_ = recording;
    recordStart_ = startSeconds;
    repaint();
}

//==================================================================================================
// Session changes

void ArrangerView::valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier& property)
{
    if (property == session::ids::collapsed || property == session::ids::showAutomation || property == session::ids::visible)
        structureChanged();
    else
        repaint();
}

void ArrangerView::valueTreeChildAdded(juce::ValueTree&, juce::ValueTree&) { structureChanged(); }
void ArrangerView::valueTreeChildRemoved(juce::ValueTree&, juce::ValueTree&, int) { structureChanged(); }
void ArrangerView::valueTreeChildOrderChanged(juce::ValueTree&, int, int) { structureChanged(); }

//==================================================================================================
// Painting

void ArrangerView::paint(juce::Graphics& g)
{
    g.fillAll(theme::colour::panel);

    paintLanes(g);
    paintMarquee(g);
    paintRuler(g);
    paintHeaders(g);
    paintDropIndicator(g);
    paintPlayhead(g);

    if (session_.getNumTracks() == 0 && !dropActive_)
        paintEmptyState(g);

    g.setColour(theme::colour::panel);
    g.fillRect(getWidth() - kScrollbarThickness, getHeight() - kScrollbarThickness, kScrollbarThickness, kScrollbarThickness);
}

void ArrangerView::paintRuler(juce::Graphics& g) const
{
    using namespace theme;

    auto corner = juce::Rectangle<int>(0, 0, metrics::trackHeaderWidth, metrics::rulerHeight);
    g.setColour(colour::panelHeader);
    g.fillRect(corner);
    g.setColour(colour::textSecondary);
    g.setFont(label(11.0f));
    g.drawText("ARRANGEMENT", corner.reduced(14, 0), juce::Justification::centredLeft, false);
    g.setColour(colour::textMuted);
    g.setFont(sans(11.0f));
    const int numTracks = session_.getNumTracks();
    g.drawText(juce::String(numTracks) + (numTracks == 1 ? " track" : " tracks"), corner.reduced(14, 0),
               juce::Justification::centredRight, false);

    const auto ruler = getRulerArea();
    g.setColour(colour::panelHeader);
    g.fillRect(ruler.withWidth(ruler.getWidth() + kScrollbarThickness));

    g.saveState();
    g.reduceClipRegion(ruler);

    // Loop range.
    const auto strip = getLoopStrip();
    const bool loopOn = session_.isLoopEnabled();
    const float loopX0 = timeToX(session_.getLoopStart());
    const float loopX1 = timeToX(session_.getLoopEnd());
    g.setColour(colour::panelRaised);
    g.fillRect(strip);
    g.setColour(loopOn ? colour::gold.withAlpha(0.85f) : colour::textMuted.withAlpha(0.45f));
    g.fillRoundedRectangle(juce::Rectangle<float>(loopX0, static_cast<float>(strip.getY()) + 1.0f, juce::jmax(2.0f, loopX1 - loopX0),
                                                  static_cast<float>(strip.getHeight()) - 2.0f),
                           3.0f);

    const double beatSeconds = 60.0 / getTempo();
    const double barSeconds = beatSeconds * kBeatsPerBar;
    const double barPixels = barSeconds * pixelsPerSecond_;
    const double beatPixels = beatSeconds * pixelsPerSecond_;

    int labelEvery = 1;
    while (static_cast<double>(labelEvery) * barPixels < 44.0)
        labelEvery *= 2;

    const auto firstBar = static_cast<int>(std::floor(xToTime(static_cast<float>(ruler.getX())) / barSeconds));
    const auto lastBar = static_cast<int>(std::ceil(xToTime(static_cast<float>(ruler.getRight())) / barSeconds));

    for (int bar = juce::jmax(0, firstBar); bar <= lastBar; ++bar)
    {
        const double barStart = bar * barSeconds;
        const float x = timeToX(barStart);
        const bool labelled = bar % labelEvery == 0;

        if (labelled || barPixels >= 6.0)
        {
            g.setColour(labelled ? colour::textMuted : colour::outlineStrong);
            g.drawVerticalLine(juce::roundToInt(x), static_cast<float>(ruler.getBottom() - (labelled ? 10 : 6)),
                               static_cast<float>(ruler.getBottom()));
        }

        if (labelled)
        {
            g.setColour(colour::textSecondary);
            g.setFont(mono(10.5f));
            g.drawText(juce::String(bar + 1),
                       juce::Rectangle<float>(x + 4.0f, static_cast<float>(strip.getBottom()) + 1.0f, 48.0f, 14.0f),
                       juce::Justification::centredLeft, false);
        }

        if (beatPixels >= 8.0)
        {
            g.setColour(colour::outlineStrong);
            for (int beat = 1; beat < kBeatsPerBar; ++beat)
                g.drawVerticalLine(juce::roundToInt(timeToX(barStart + beat * beatSeconds)), static_cast<float>(ruler.getBottom() - 4),
                                   static_cast<float>(ruler.getBottom()));
        }
    }

    g.restoreState();

    g.setColour(colour::outline);
    g.fillRect(0, metrics::rulerHeight - 1, getWidth(), 1);
}

void ArrangerView::paintHeaders(juce::Graphics& g) const
{
    using namespace theme;

    const auto area = getHeaderArea();
    g.saveState();
    g.reduceClipRegion(area);
    g.setColour(colour::windowBackground);
    g.fillRect(area);

    const auto& all = rows();
    for (std::size_t i = 0; i < all.size(); ++i)
    {
        const auto bounds = getRowBounds(static_cast<int>(i));
        if (bounds.getBottom() < area.getY() || bounds.getY() > area.getBottom())
            continue;
        if (all[i].kind == Row::Kind::track)
            paintTrackHeader(g, static_cast<int>(i), all[i]);
        else
            paintLaneHeader(g, static_cast<int>(i), all[i]);
    }

    // Reorder indicator.
    if (dragMode_ == DragMode::reorderTrack && dragMoved_ && reorderTargetRow_ >= 0)
    {
        g.setColour(colour::accent);
        g.fillRect(0, getRowBounds(reorderTargetRow_).getY() - 1, metrics::trackHeaderWidth, 3);
    }

    g.restoreState();

    g.setColour(colour::outline);
    g.fillRect(metrics::trackHeaderWidth - 1, metrics::rulerHeight, 1, area.getHeight());
}

void ArrangerView::paintTrackHeader(juce::Graphics& g, int rowIndex, const Row& row) const
{
    using namespace theme;

    const auto& t = row.track;
    const bool selected = editor_.isTrackSelected(session::track::getId(t));
    const auto trackColour = session::track::getColour(t);
    const bool folder = session::track::isFolder(t);
    const auto kind = session::track::getKind(t);
    auto bounds = getRowBounds(rowIndex).withWidth(metrics::trackHeaderWidth);

    g.setColour(selected ? colour::controlHover : (folder ? colour::panelHeader : colour::panelRaised));
    g.fillRect(bounds);
    if (selected) // clearly marked, so a multi-selection reads at a glance
    {
        g.setColour(colour::accent.withAlpha(0.75f));
        g.drawRect(bounds.withTrimmedLeft(4).withTrimmedBottom(1), 1);
    }
    g.setColour(colour::outline);
    g.fillRect(bounds.removeFromBottom(1));
    g.setColour(trackColour);
    g.fillRect(bounds.removeFromLeft(selected ? 6 : 4));

    // Nesting guide.
    for (int d = 0; d < row.depth; ++d)
    {
        g.setColour(colour::outlineStrong);
        g.fillRect(12 + d * kIndent, bounds.getY(), 1, bounds.getHeight());
    }

    if (folder)
    {
        const auto box = getFoldButton(rowIndex).toFloat();
        juce::Path arrow;
        if (session::track::isCollapsed(t))
            arrow.addTriangle(box.getX() + 3.0f, box.getY() + 3.0f, box.getRight() - 2.0f, box.getCentreY(), box.getX() + 3.0f, box.getBottom() - 3.0f);
        else
            arrow.addTriangle(box.getX() + 2.0f, box.getY() + 4.0f, box.getRight() - 2.0f, box.getY() + 4.0f, box.getCentreX(), box.getBottom() - 3.0f);
        g.setColour(colour::textSecondary);
        g.fillPath(arrow);
    }
    else
    {
        // Kind glyph: waveform / keys / bus.
        const auto box = getFoldButton(rowIndex).toFloat().reduced(1.0f, 3.0f);
        g.setColour(trackColour.withAlpha(0.9f));
        if (kind == session::TrackKind::instrument)
            for (int k = 0; k < 3; ++k)
                g.fillRect(box.getX() + static_cast<float>(k) * 4.0f, box.getY(), 3.0f, box.getHeight());
        else if (kind == session::TrackKind::bus)
            g.drawEllipse(box.reduced(1.0f), 1.5f);
        else
            for (int k = 0; k < 4; ++k)
            {
                const float h = box.getHeight() * (k % 2 == 0 ? 0.9f : 0.5f);
                g.fillRect(box.getX() + static_cast<float>(k) * 3.0f, box.getCentreY() - h * 0.5f, 2.0f, h);
            }
    }

    if (!(renameEditor_->isVisible() && renamingTrackId_ == session::track::getId(t)))
    {
        g.setColour(colour::textPrimary);
        g.setFont(sansBold(13.0f));
        g.drawText(session::track::getName(t), getNameBounds(rowIndex), juce::Justification::centredLeft, true);
    }

    const auto paintToggle = [&g](juce::Rectangle<int> box, const char* text, bool on, juce::Colour onColour)
    {
        g.setColour(on ? onColour : colour::control);
        g.fillRoundedRectangle(box.toFloat(), 3.0f);
        g.setColour(on ? textOn(onColour) : colour::textMuted);
        g.setFont(sansBold(10.0f));
        g.drawText(text, box, juce::Justification::centred, false);
    };

    paintToggle(getHeaderButton(rowIndex, 0), "A", session::track::isShowingAutomation(t), colour::textSecondary);
    paintToggle(getHeaderButton(rowIndex, 1), "S", session::track::isSoloed(t), colour::play);
    paintToggle(getHeaderButton(rowIndex, 2), "M", session::track::isMuted(t), colour::gold);
    if (kind != session::TrackKind::bus)
        paintToggle(getHeaderButton(rowIndex, 3), "R", session::track::isArmed(t), colour::record);

    // Second line: routing summary.
    juce::String info = kindLabel(t) + "  " + formatDb(session::track::getVolumeDb(t)) + "  " + formatPan(session::track::getPan(t));
    if (kind == session::TrackKind::audio && session::track::getInput(t) >= 0)
        info << "  In " << (session::track::getInput(t) + 1) << (session::track::isInputStereo(t) ? "/" + juce::String(session::track::getInput(t) + 2) : juce::String());
    if (const auto dest = session_.getDestination(t); dest.isValid() && !folder && session::track::getOutput(t) != 0)
        info << "  > " << session::track::getName(dest);
    if (const auto devices = session::Session::getDevices(t); !devices.empty())
        info << "  [" << session::device::getName(devices.front()) << (devices.size() > 1 ? " +" + juce::String(devices.size() - 1) : juce::String()) << "]";

    g.setColour(colour::textMuted);
    g.setFont(sans(11.0f));
    g.drawText(info, juce::Rectangle<int>(getNameBounds(rowIndex).getX(), getRowBounds(rowIndex).getY() + 34,
                                          metrics::trackHeaderWidth - getNameBounds(rowIndex).getX() - 8, 18),
               juce::Justification::centredLeft, true);
}

void ArrangerView::paintLaneHeader(juce::Graphics& g, int rowIndex, const Row& row) const
{
    using namespace theme;

    auto bounds = getRowBounds(rowIndex).withWidth(metrics::trackHeaderWidth);
    g.setColour(colour::panel);
    g.fillRect(bounds);
    g.setColour(colour::outline.withAlpha(0.6f));
    g.fillRect(bounds.removeFromBottom(1));
    g.setColour(session::track::getColour(row.track).withAlpha(0.5f));
    g.fillRect(bounds.removeFromLeft(2));

    const int left = 26 + row.depth * kIndent;
    g.setColour(colour::textSecondary);
    g.setFont(sansBold(11.5f));
    g.drawText(describeTarget(row.lane) + "  v", juce::Rectangle<int>(left, getRowBounds(rowIndex).getY() + 6, metrics::trackHeaderWidth - left - 32, 16),
               juce::Justification::centredLeft, true);

    const double value = session::automation::toCurve(row.lane).valueAt(playheadSeconds_, 0.5);
    g.setColour(colour::textMuted);
    g.setFont(mono(10.5f));
    g.drawText(describeValue(row.lane, value), juce::Rectangle<int>(left, getRowBounds(rowIndex).getY() + 24, metrics::trackHeaderWidth - left - 32, 16),
               juce::Justification::centredLeft, true);

    // Close (hide) button.
    const auto close = juce::Rectangle<int>(metrics::trackHeaderWidth - 26, getRowBounds(rowIndex).getY() + 6, 16, 16);
    g.setColour(colour::textMuted);
    g.drawLine(static_cast<float>(close.getX() + 4), static_cast<float>(close.getY() + 4), static_cast<float>(close.getRight() - 4),
               static_cast<float>(close.getBottom() - 4), 1.5f);
    g.drawLine(static_cast<float>(close.getRight() - 4), static_cast<float>(close.getY() + 4), static_cast<float>(close.getX() + 4),
               static_cast<float>(close.getBottom() - 4), 1.5f);
}

void ArrangerView::paintLanes(juce::Graphics& g) const
{
    using namespace theme;

    const auto lanes = getLaneArea();
    g.saveState();
    g.reduceClipRegion(lanes);

    g.setColour(colour::windowBackground);
    g.fillRect(lanes);

    const auto& all = rows();

    int trackOrdinal = 0;
    for (std::size_t i = 0; i < all.size(); ++i)
    {
        auto bounds = getRowBounds(static_cast<int>(i)).withLeft(lanes.getX());
        if (all[i].kind == Row::Kind::track)
            ++trackOrdinal;
        if (bounds.getBottom() < lanes.getY() || bounds.getY() > lanes.getBottom())
            continue;

        if (all[i].kind == Row::Kind::automation)
        {
            g.setColour(colour::panel);
            g.fillRect(bounds);
        }
        else
        {
            const bool selected = editor_.isTrackSelected(session::track::getId(all[i].track));
            const bool folder = session::track::isFolder(all[i].track);
            g.setColour(selected ? colour::laneOdd.brighter(0.05f)
                                 : folder ? colour::panelRaised
                                          : (trackOrdinal % 2 == 0 ? colour::laneEven : colour::laneOdd));
            g.fillRect(bounds);
        }
        g.setColour(colour::outline.withAlpha(0.6f));
        g.fillRect(bounds.removeFromBottom(1));
    }

    // Loop range shading.
    if (session_.isLoopEnabled())
    {
        const float x0 = timeToX(session_.getLoopStart());
        const float x1 = timeToX(session_.getLoopEnd());
        g.setColour(colour::gold.withAlpha(0.05f));
        g.fillRect(juce::Rectangle<float>(x0, static_cast<float>(lanes.getY()), x1 - x0, static_cast<float>(lanes.getHeight())));
    }

    // Grid.
    const double beatSeconds = 60.0 / getTempo();
    const double gridSeconds = editor_.getGridBeats() * beatSeconds;
    const double beatPixels = beatSeconds * pixelsPerSecond_;
    const double gridPixels = gridSeconds * pixelsPerSecond_;
    const double barSeconds = beatSeconds * kBeatsPerBar;

    const double t0 = xToTime(static_cast<float>(lanes.getX()));
    const double t1 = xToTime(static_cast<float>(lanes.getRight()));
    const double step = gridPixels >= 8.0 ? gridSeconds : (beatPixels >= 8.0 ? beatSeconds : barSeconds);

    for (auto n = static_cast<long long>(std::floor(t0 / step)); static_cast<double>(n) * step <= t1; ++n)
    {
        if (n < 0)
            continue;
        const double t = static_cast<double>(n) * step;
        const double barPosition = t / barSeconds;
        const bool isBar = std::abs(barPosition - std::round(barPosition)) < 1.0e-6;
        const double beatPosition = t / beatSeconds;
        const bool isBeat = std::abs(beatPosition - std::round(beatPosition)) < 1.0e-6;
        g.setColour(isBar ? colour::gridBar : (isBeat ? colour::gridBeat.brighter(0.15f) : colour::gridBeat));
        g.drawVerticalLine(juce::roundToInt(timeToX(t)), static_cast<float>(lanes.getY()), static_cast<float>(lanes.getBottom()));
    }

    // Recording region on armed tracks.
    if (recording_)
    {
        const float x0 = timeToX(recordStart_);
        const float x1 = timeToX(playheadSeconds_);
        for (std::size_t i = 0; i < all.size(); ++i)
            if (all[i].kind == Row::Kind::track && session::track::isArmed(all[i].track))
            {
                const auto bounds = getRowBounds(static_cast<int>(i)).toFloat();
                g.setColour(colour::record.withAlpha(0.25f));
                g.fillRoundedRectangle(juce::Rectangle<float>(x0, bounds.getY() + 3.0f, juce::jmax(2.0f, x1 - x0), bounds.getHeight() - 6.0f), 4.0f);
            }
    }

    // Clips and automation.
    for (std::size_t i = 0; i < all.size(); ++i)
    {
        const auto bounds = getRowBounds(static_cast<int>(i));
        if (bounds.getBottom() < lanes.getY() || bounds.getY() > lanes.getBottom())
            continue;

        if (all[i].kind == Row::Kind::automation)
        {
            paintAutomation(g, static_cast<int>(i), all[i]);
            continue;
        }

        const auto trackColour = session::track::getColour(all[i].track);
        for (const auto& c : all[i].track)
            if (session::Session::isClip(c))
                paintClip(g, c, static_cast<int>(i), trackColour);
    }

    g.restoreState();
}

void ArrangerView::paintAutomation(juce::Graphics& g, int rowIndex, const Row& row) const
{
    using namespace theme;

    const auto bounds = getRowBounds(rowIndex);
    const auto lanes = getLaneArea();
    const auto curve = session::automation::toCurve(row.lane);
    const auto colour = session::track::getColour(row.track);

    // The curve, sampled every two pixels.
    juce::Path path;
    juce::Path fill;
    const int x0 = juce::jmax(lanes.getX(), 0);
    const int x1 = lanes.getRight();
    bool first = true;
    for (int x = x0; x <= x1; x += 2)
    {
        const float y = valueToY(bounds, curve.valueAt(xToTime(static_cast<float>(x)), 0.5));
        if (first)
        {
            path.startNewSubPath(static_cast<float>(x), y);
            fill.startNewSubPath(static_cast<float>(x), static_cast<float>(bounds.getBottom()));
            fill.lineTo(static_cast<float>(x), y);
            first = false;
        }
        else
        {
            path.lineTo(static_cast<float>(x), y);
            fill.lineTo(static_cast<float>(x), y);
        }
    }
    fill.lineTo(static_cast<float>(x1), static_cast<float>(bounds.getBottom()));
    fill.closeSubPath();

    g.setColour(colour.withAlpha(0.10f));
    g.fillPath(fill);
    g.setColour(colour.withAlpha(0.9f));
    g.strokePath(path, juce::PathStrokeType(1.6f));

    for (const auto& p : row.lane)
    {
        const juce::Point<float> centre{timeToX(p.getProperty(session::ids::time)), valueToY(bounds, p.getProperty(session::ids::value))};
        const bool active = p == dragPoint_;
        g.setColour(active ? colour::textPrimary : colour.brighter(0.3f));
        g.fillEllipse(juce::Rectangle<float>(kPointRadius * 2.0f, kPointRadius * 2.0f).withCentre(centre));
        g.setColour(colour::windowBackground);
        g.drawEllipse(juce::Rectangle<float>(kPointRadius * 2.0f, kPointRadius * 2.0f).withCentre(centre), 1.0f);
    }
}

void ArrangerView::paintClip(juce::Graphics& g, const juce::ValueTree& c, int row, juce::Colour colour) const
{
    using namespace theme;

    const auto bounds = getClipBounds(c, row);
    if (!bounds.toNearestInt().intersects(g.getClipBounds()))
        return;

    const bool selected = editor_.isClipSelected(session::clip::getId(c));
    const bool midi = session::clip::isMidi(c);

    g.setColour(colour.withAlpha(selected ? 0.36f : 0.22f));
    g.fillRoundedRectangle(bounds, 4.0f);

    const auto title = bounds.withHeight(kClipTitleHeight);
    g.setColour(colour.withAlpha(selected ? 1.0f : 0.85f));
    g.fillRoundedRectangle(title, 4.0f);
    g.fillRect(title.withTrimmedTop(6.0f));

    juce::String name = session::clip::getName(c);
    if (!midi)
    {
        if (std::abs(session::clip::getStretch(c) - 1.0) > 1.0e-6)
            name << "  x" << juce::String(session::clip::getStretch(c), 2);
        if (std::abs(session::clip::getPitch(c)) > 1.0e-6)
            name << "  " << (session::clip::getPitch(c) > 0 ? "+" : "") << juce::String(session::clip::getPitch(c), 1) << " st";
    }
    g.setColour(textOn(colour));
    g.setFont(sansBold(10.5f));
    g.drawText(name, title.reduced(6.0f, 0.0f), juce::Justification::centredLeft, true);

    const auto body = bounds.withTrimmedTop(kClipTitleHeight);

    if (midi)
    {
        paintMidiNotes(g, c, body.reduced(2.0f, 3.0f), colour.brighter(0.3f));
    }
    else if (project_.getAssets().find(session::clip::getFile(c)) != nullptr)
    {
        paintWaveform(g, c, body.reduced(0.0f, 3.0f), colour.brighter(0.25f));
    }
    else
    {
        const bool missing = !session::clip::getFile(c).existsAsFile();
        g.setColour(missing ? colour::meterClip : colour::textMuted);
        g.setFont(sans(11.0f));
        g.drawText(missing ? "Missing: " + session::clip::getFile(c).getFileName() : juce::String("Loading..."), body.reduced(6.0f, 2.0f),
                   juce::Justification::centredLeft, true);
    }

    if (!midi)
    {
        // Fades: shade the attenuated part and draw the ramp.
        const float fadeInWidth = static_cast<float>(session::clip::getFadeIn(c) * pixelsPerSecond_);
        const float fadeOutWidth = static_cast<float>(session::clip::getFadeOut(c) * pixelsPerSecond_);

        if (fadeInWidth > 0.5f)
        {
            juce::Path shade;
            shade.addTriangle(body.getX(), body.getY(), body.getX() + fadeInWidth, body.getY(), body.getX(), body.getBottom());
            g.setColour(juce::Colours::black.withAlpha(0.35f));
            g.fillPath(shade);
            g.setColour(colour.brighter(0.4f));
            g.drawLine(body.getX(), body.getBottom(), body.getX() + fadeInWidth, body.getY(), 1.2f);
        }

        if (fadeOutWidth > 0.5f)
        {
            juce::Path shade;
            shade.addTriangle(body.getRight() - fadeOutWidth, body.getY(), body.getRight(), body.getY(), body.getRight(), body.getBottom());
            g.setColour(juce::Colours::black.withAlpha(0.35f));
            g.fillPath(shade);
            g.setColour(colour.brighter(0.4f));
            g.drawLine(body.getRight() - fadeOutWidth, body.getY(), body.getRight(), body.getBottom(), 1.2f);
        }

        if (bounds.getWidth() > 4.0f * kFadeHandleSize)
        {
            g.setColour(selected ? colour::textPrimary : colour::textPrimary.withAlpha(0.55f));
            for (const float x : {body.getX() + fadeInWidth, body.getRight() - fadeOutWidth})
                g.fillRect(juce::Rectangle<float>(kFadeHandleSize - 2.0f, kFadeHandleSize - 2.0f).withCentre({x, body.getY()}));
        }

        // Crossfade with the next clip on the track (X marks the overlap).
        const auto parent = c.getParent();
        for (const auto& other : parent)
        {
            if (other == c || !other.hasType(session::ids::clip))
                continue;
            const double overlapStart = std::max(session::clip::getStart(c), session::clip::getStart(other));
            const double overlapEnd = std::min(session::clip::getEnd(c), session::clip::getEnd(other));
            if (overlapEnd > overlapStart && session::clip::getStart(other) > session::clip::getStart(c))
            {
                const float xa = timeToX(overlapStart);
                const float xb = timeToX(overlapEnd);
                g.setColour(colour::textPrimary.withAlpha(0.6f));
                g.drawLine(xa, body.getY(), xb, body.getBottom(), 1.0f);
                g.drawLine(xa, body.getBottom(), xb, body.getY(), 1.0f);
            }
        }
    }

    g.setColour(selected ? colour::textPrimary : colour.withAlpha(0.7f));
    g.drawRoundedRectangle(bounds.reduced(0.5f), 4.0f, selected ? 1.6f : 1.0f);
}

void ArrangerView::paintWaveform(juce::Graphics& g, const juce::ValueTree& c, juce::Rectangle<float> area, juce::Colour colour) const
{
    const auto asset = project_.getAssets().find(session::clip::getFile(c));
    if (asset == nullptr || area.getHeight() < 4.0f)
        return;

    const auto& peaks = asset->peaks;
    const int numBuckets = peaks.getNumBuckets();
    if (numBuckets == 0)
        return;

    const auto visible = area.getIntersection(g.getClipBounds().toFloat());
    if (visible.isEmpty())
        return;

    const double start = session::clip::getStart(c);
    const double offset = session::clip::getOffset(c);
    const double stretch = std::max(session::kMinStretch, session::clip::getStretch(c));
    const double bucketsPerSecond = asset->sampleRate / session::WaveformPeaks::kFramesPerBucket;

    // Stereo files show their two channels (left above right), mono files one.
    const auto firstX = static_cast<int>(std::floor(visible.getX()));
    const auto lastX = static_cast<int>(std::ceil(visible.getRight()));
    waveform::paintChannels(g, area, std::min(peaks.numChannels, 2), firstX, lastX, colour.withAlpha(0.85f),
                            [&](int channel, int x)
                            {
                                const double sourceT0 = offset + (xToTime(static_cast<float>(x)) - start) / stretch;
                                const double sourceT1 = offset + (xToTime(static_cast<float>(x + 1)) - start) / stretch;
                                const int b0 = juce::jlimit(0, numBuckets - 1, static_cast<int>(std::floor(sourceT0 * bucketsPerSecond)));
                                const int b1 = juce::jlimit(b0 + 1, numBuckets, static_cast<int>(std::ceil(sourceT1 * bucketsPerSecond)));
                                const auto& minimum = peaks.minimum[static_cast<std::size_t>(channel)];
                                const auto& maximum = peaks.maximum[static_cast<std::size_t>(channel)];
                                float lo = 0.0f;
                                float hi = 0.0f;
                                for (int b = b0; b < b1; ++b)
                                {
                                    lo = juce::jmin(lo, minimum[static_cast<std::size_t>(b)]);
                                    hi = juce::jmax(hi, maximum[static_cast<std::size_t>(b)]);
                                }
                                return std::pair{lo, hi};
                            });
}

void ArrangerView::paintMidiNotes(juce::Graphics& g, const juce::ValueTree& c, juce::Rectangle<float> area, juce::Colour colour) const
{
    int lowest = 127;
    int highest = 0;
    int count = 0;
    for (const auto& n : c)
        if (n.hasType(session::ids::note))
        {
            lowest = std::min(lowest, session::note::getNote(n));
            highest = std::max(highest, session::note::getNote(n));
            ++count;
        }

    if (count == 0)
    {
        g.setColour(theme::colour::textMuted);
        g.setFont(theme::sans(11.0f));
        g.drawText("Empty MIDI clip - double-click to edit", area.reduced(4.0f, 0.0f), juce::Justification::centredLeft, true);
        return;
    }

    lowest = std::max(0, lowest - 2);
    highest = std::min(127, highest + 2);
    const float noteHeight = std::max(1.5f, area.getHeight() / static_cast<float>(highest - lowest + 1));
    const double beatsPerSecond = getTempo() / 60.0;
    const double clipStart = session::clip::getStart(c);
    const double contentStart = session::clip::getOffset(c);

    g.setColour(colour);
    for (const auto& n : c)
    {
        if (!n.hasType(session::ids::note))
            continue;
        const double startTime = clipStart + (session::note::getStart(n) - contentStart) / beatsPerSecond;
        const double endTime = clipStart + (session::note::getEnd(n) - contentStart) / beatsPerSecond;
        const float x0 = std::max(area.getX(), timeToX(startTime));
        const float x1 = std::min(area.getRight(), timeToX(endTime));
        if (x1 <= x0)
            continue;
        const float y = area.getBottom() - static_cast<float>(session::note::getNote(n) - lowest + 1) * noteHeight;
        g.fillRect(x0, y, std::max(1.0f, x1 - x0), std::max(1.0f, noteHeight - 0.5f));
    }
}

void ArrangerView::paintDropIndicator(juce::Graphics& g) const
{
    if (!dropActive_)
        return;

    const auto lanes = getLaneArea();
    g.saveState();
    g.reduceClipRegion(getLocalBounds().withTop(theme::metrics::rulerHeight));

    if (dropRow_ >= 0)
    {
        g.setColour(theme::colour::accent.withAlpha(0.12f));
        g.fillRect(getRowBounds(dropRow_).withRight(lanes.getRight()));
    }
    else if (!dropIsDevice_)
    {
        const auto& all = rows();
        const int bottom = all.empty() ? 0 : all.back().top + all.back().height;
        const int top = theme::metrics::rulerHeight + bottom - scrollY_;
        const auto zone = lanes.withTop(juce::jmax(lanes.getY(), top));
        g.setColour(theme::colour::accent.withAlpha(0.08f));
        g.fillRect(zone);
        g.setColour(theme::colour::accent);
        g.setFont(theme::sansBold(13.0f));
        g.drawText("Drop to create new tracks", zone.withHeight(kTrackHeight), juce::Justification::centred, false);
    }

    if (!dropIsDevice_)
    {
        g.setColour(theme::colour::accent);
        g.fillRect(timeToX(dropTime_) - 1.0f, static_cast<float>(lanes.getY()), 2.0f, static_cast<float>(lanes.getHeight()));
    }
    g.restoreState();
}

void ArrangerView::paintPlayhead(juce::Graphics& g) const
{
    const auto lanes = getLaneArea();
    const float x = timeToX(playheadSeconds_);
    if (x < static_cast<float>(lanes.getX()) || x > static_cast<float>(lanes.getRight()))
        return;

    g.setColour(recording_ ? theme::colour::record : theme::colour::playhead);
    g.fillRect(x - 0.5f, static_cast<float>(theme::metrics::rulerHeight), 1.5f, static_cast<float>(lanes.getBottom() - theme::metrics::rulerHeight));

    juce::Path marker;
    const auto bottom = static_cast<float>(theme::metrics::rulerHeight);
    marker.addTriangle(x - 6.0f, bottom - 10.0f, x + 6.0f, bottom - 10.0f, x, bottom);
    g.fillPath(marker);
}

void ArrangerView::paintEmptyState(juce::Graphics& g) const
{
    auto area = getLaneArea().withSizeKeepingCentre(juce::jmin(600, getLaneArea().getWidth() - 40), 120);

    g.setColour(theme::colour::outlineStrong);
    g.drawRoundedRectangle(area.toFloat(), 8.0f, 1.0f);

    g.setColour(theme::colour::textPrimary);
    g.setFont(theme::sansBold(16.0f));
    g.drawText("Drop audio files here", area.removeFromTop(64).withTrimmedTop(16), juce::Justification::centred, false);

    g.setColour(theme::colour::textMuted);
    g.setFont(theme::sans(12.5f));
    g.drawText("or + Add Track: audio, instruments (Drum Rack, Sampler), busses and folders", area.withTrimmedBottom(18),
               juce::Justification::centred, true);
}

//==================================================================================================
// Mouse

void ArrangerView::locateTo(double seconds)
{
    controller_.locateSeconds(juce::jmax(0.0, seconds));
    playheadSeconds_ = juce::jmax(0.0, seconds);
    repaint();
}

void ArrangerView::mouseDown(const juce::MouseEvent& e)
{
    if (isShowing()) // (a component off screen cannot take the focus)
        grabKeyboardFocus();

    dragMode_ = DragMode::none;
    dragMoved_ = false;
    dragStart_ = e.position;
    const auto position = e.getPosition();

    if (getLoopStrip().contains(position))
    {
        const double t = xToTime(e.position.x);
        const float x0 = timeToX(session_.getLoopStart());
        const float x1 = timeToX(session_.getLoopEnd());
        loopDragStart_ = session_.getLoopStart();
        loopDragEnd_ = session_.getLoopEnd();
        loopAnchor_ = snap(t, e.mods);

        if (std::abs(e.position.x - x0) <= 5.0f)
            dragMode_ = DragMode::loopStart;
        else if (std::abs(e.position.x - x1) <= 5.0f)
            dragMode_ = DragMode::loopEnd;
        else if (e.position.x > x0 && e.position.x < x1)
            dragMode_ = DragMode::loopMove;
        else
            dragMode_ = DragMode::loopCreate;
        return;
    }

    if (getRulerArea().contains(position))
    {
        dragMode_ = DragMode::locate;
        locateTo(xToTime(e.position.x));
        return;
    }

    const int row = rowAt(position.y);

    if (getHeaderArea().contains(position))
    {
        if (row >= 0)
            mouseDownOnHeader(e, row);
        return;
    }

    if (!getLaneArea().contains(position))
        return;

    if (row >= 0 && rows()[static_cast<std::size_t>(row)].kind == Row::Kind::automation)
    {
        mouseDownOnAutomation(e, row);
        return;
    }

    const auto hit = hitTestClip(e.position);
    if (hit.clip.isValid())
    {
        const auto clipId = session::clip::getId(hit.clip);
        selectClipWithModifiers(hit.clip, e.mods);

        editor_.selectTrack(session::track::getId(rows()[static_cast<std::size_t>(hit.row)].track));

        if (e.mods.isPopupMenu())
            showClipMenu(hit.clip);
        else if (editor_.isClipSelected(clipId))
            beginClipDrag(hit);
        return;
    }

    // Empty space.
    const double seconds = xToTime(e.position.x);
    if (row >= 0)
        editor_.selectTrack(session::track::getId(rows()[static_cast<std::size_t>(row)].track));

    if (e.mods.isPopupMenu())
    {
        showLaneMenu(row, seconds);
        return;
    }

    // A click moves the playhead (on release); a drag draws a rubber band that selects the clips it
    // touches. Ctrl or Shift add to the selection instead of replacing it.
    const bool additive = e.mods.isCommandDown() || e.mods.isShiftDown();
    if (!additive)
    {
        editor_.clearClipSelection();
        selectionAnchor_ = 0;
    }
    marqueeBase_ = additive ? editor_.getSelectedClips() : std::set<std::uint32_t>{};
    marqueeAnchor_ = {seconds, juce::roundToInt(e.position.y) - theme::metrics::rulerHeight + scrollY_};
    marqueeEnd_ = marqueeAnchor_;
    marqueeActive_ = false;
    pendingLocate_ = snap(seconds, e.mods);
    dragMode_ = DragMode::marquee;
}

void ArrangerView::mouseDownOnHeader(const juce::MouseEvent& e, int rowIndex)
{
    const auto& row = rows()[static_cast<std::size_t>(rowIndex)];
    const auto position = e.getPosition();
    auto t = row.track;
    auto* um = &session_.getUndoManager();
    const auto trackId = session::track::getId(t);

    if (row.kind == Row::Kind::automation)
    {
        editor_.selectTrack(trackId);
        const auto close = juce::Rectangle<int>(theme::metrics::trackHeaderWidth - 26, getRowBounds(rowIndex).getY() + 6, 16, 16);
        if (close.contains(position))
        {
            session_.beginTransaction("Hide Automation Lane");
            session::automation::setVisible(row.lane, false, um);
            return;
        }
        showAutomationMenu(row.lane, {});
        return;
    }

    // A track of a multi-selection keeps the selection for its buttons, its menu and drags; any
    // other track is selected on its own.
    const bool inSelection = editor_.isTrackSelected(trackId) && editor_.getSelectedTracks().size() > 1;
    if (!inSelection && !(e.mods.isCommandDown() || e.mods.isShiftDown()))
        editor_.selectTrack(trackId);

    if (e.mods.isPopupMenu())
    {
        if (!inSelection)
            editor_.selectTrack(trackId);
        showTrackMenu(t);
        return;
    }

    if (session::track::isFolder(t) && getFoldButton(rowIndex).contains(position))
    {
        session::track::setCollapsed(t, !session::track::isCollapsed(t), nullptr);
        return;
    }

    // Solo, mute and arm switch every selected track when the clicked one is part of the selection.
    const auto targets = inSelection ? selectedTracksInOrder(false) : std::vector<juce::ValueTree>{t};
    const auto kind = session::track::getKind(t);
    if (getHeaderButton(rowIndex, 0).contains(position))
    {
        if (session::Session::getAutomationLanes(t).empty())
            session_.getAutomation(t, session::automation::volumeTarget(), true, currentValueFor(t, session::automation::volumeTarget()));
        session::track::setShowingAutomation(t, !session::track::isShowingAutomation(t), nullptr);
        for (auto lane : session::Session::getAutomationLanes(t))
            session::automation::setVisible(lane, true, nullptr);
        return;
    }
    if (getHeaderButton(rowIndex, 1).contains(position))
    {
        session_.beginTransaction("Solo");
        const bool on = !session::track::isSoloed(t);
        for (auto target : targets)
            session::track::setSoloed(target, on, um);
        return;
    }
    if (getHeaderButton(rowIndex, 2).contains(position))
    {
        session_.beginTransaction("Mute");
        const bool on = !session::track::isMuted(t);
        for (auto target : targets)
            session::track::setMuted(target, on, um);
        return;
    }
    if (kind != session::TrackKind::bus && getHeaderButton(rowIndex, 3).contains(position))
    {
        session_.beginTransaction("Arm Track");
        const bool on = !session::track::isArmed(t);
        for (auto target : targets)
            if (session::track::getKind(target) != session::TrackKind::bus)
                session::track::setArmed(target, on, um);
        return;
    }

    // Ctrl / Shift: select like files in Explorer.
    if (e.mods.isCommandDown() || e.mods.isShiftDown())
    {
        selectTrackWithModifiers(t, e.mods);
        return;
    }

    // Dragging the header moves the track (or every selected track); a click without a drag on a
    // track of a multi-selection selects only that track (on release, so the drag can happen first).
    pendingTrackClick_ = inSelection ? trackId : 0u;
    dragMode_ = DragMode::reorderTrack;
    reorderTrack_ = t;
    reorderTargetRow_ = rowIndex;
}

void ArrangerView::mouseDownOnAutomation(const juce::MouseEvent& e, int rowIndex)
{
    const auto& row = rows()[static_cast<std::size_t>(rowIndex)];
    editor_.selectTrack(session::track::getId(row.track));
    auto point = hitTestPoint(rowIndex, e.position);

    if (e.mods.isPopupMenu())
    {
        showAutomationMenu(row.lane, point);
        return;
    }

    if (!point.isValid())
    {
        session_.beginTransaction("Add Automation Point");
        point = session_.addAutomationPoint(row.lane, snap(xToTime(e.position.x), e.mods), yToValue(getRowBounds(rowIndex), e.position.y));
        dragMoved_ = true; // part of this transaction
    }

    dragMode_ = DragMode::automationPoint;
    dragPoint_ = point;
    dragPointRow_ = rowIndex;
    repaint();
}

void ArrangerView::beginClipDrag(const ClipHit& hit)
{
    switch (hit.zone)
    {
    case ClipZone::body:          dragMode_ = DragMode::moveClips; break;
    case ClipZone::leftEdge:      dragMode_ = DragMode::trimStart; break;
    case ClipZone::rightEdge:     dragMode_ = DragMode::trimEnd; break;
    case ClipZone::fadeInHandle:  dragMode_ = DragMode::fadeIn; break;
    case ClipZone::fadeOutHandle: dragMode_ = DragMode::fadeOut; break;
    }

    dragAnchorId_ = session::clip::getId(hit.clip);
    dragOriginals_.clear();

    const auto trackRowList = trackRows();
    const auto snapshotOf = [&](const juce::ValueTree& c)
    {
        ClipSnapshot s;
        s.id = session::clip::getId(c);
        const int row = rowOfTrack(c.getParent());
        s.trackOrdinal = static_cast<int>(std::find(trackRowList.begin(), trackRowList.end(), row) - trackRowList.begin());
        s.start = session::clip::getStart(c);
        s.length = session::clip::getLength(c);
        s.offset = session::clip::getOffset(c);
        s.fadeIn = session::clip::getFadeIn(c);
        s.fadeOut = session::clip::getFadeOut(c);
        s.stretch = std::max(session::kMinStretch, session::clip::getStretch(c));
        s.midi = session::clip::isMidi(c);
        return s;
    };

    dragOriginals_.push_back(snapshotOf(hit.clip)); // anchor first

    if (dragMode_ == DragMode::moveClips)
        for (const auto id : editor_.getSelectedClips())
            if (id != dragAnchorId_)
                if (auto c = session_.findClip(id); c.isValid() && rowOfTrack(c.getParent()) >= 0)
                    dragOriginals_.push_back(snapshotOf(c));

    if (session::clip::isMidi(hit.clip))
        dragSourceLength_ = 1.0e9;
    else
    {
        const auto asset = project_.getAssets().find(session::clip::getFile(hit.clip));
        dragSourceLength_ = asset != nullptr ? asset->getLengthSeconds() : 1.0e9;
    }
}

void ArrangerView::mouseDrag(const juce::MouseEvent& e)
{
    switch (dragMode_)
    {
    case DragMode::none:
        return;

    case DragMode::locate:
        locateTo(xToTime(e.position.x));
        return;

    case DragMode::loopCreate:
    case DragMode::loopMove:
    case DragMode::loopStart:
    case DragMode::loopEnd:
        dragLoop(e);
        return;

    case DragMode::reorderTrack:
    {
        if (!dragMoved_ && std::abs(e.position.y - dragStart_.y) < 4.0f)
            return;
        dragMoved_ = true;
        const auto list = trackRows();
        int target = static_cast<int>(rows().size()); // below everything
        for (const int r : list)
            if (e.position.y < static_cast<float>(getRowBounds(r).getCentreY()))
            {
                target = r;
                break;
            }
        reorderTargetRow_ = target;
        repaint(getHeaderArea());
        return;
    }

    case DragMode::marquee:
        dragMarquee(e);
        return;

    case DragMode::automationPoint:
    {
        if (!dragPoint_.isValid())
            return;
        if (!dragMoved_)
        {
            if (e.position.getDistanceFrom(dragStart_) < 2.0f)
                return;
            session_.beginTransaction("Move Automation Point");
            dragMoved_ = true;
        }
        session_.moveAutomationPoint(dragPoint_, snap(xToTime(e.position.x), e.mods), yToValue(getRowBounds(dragPointRow_), e.position.y));
        return;
    }

    case DragMode::moveClips:
    case DragMode::trimStart:
    case DragMode::trimEnd:
    case DragMode::fadeIn:
    case DragMode::fadeOut:
        dragClips(e);
        return;
    }
}

void ArrangerView::dragLoop(const juce::MouseEvent& e)
{
    const double t = snap(juce::jmax(0.0, xToTime(e.position.x)), e.mods);
    const bool wasEnabled = session_.isLoopEnabled();

    if (!dragMoved_)
    {
        if (std::abs(e.position.x - dragStart_.x) < 3.0f)
            return;
        dragMoved_ = true;
        session_.beginTransaction("Set Loop");
    }

    double start = loopDragStart_;
    double end = loopDragEnd_;
    switch (dragMode_)
    {
    case DragMode::loopCreate:
        start = std::min(loopAnchor_, t);
        end = std::max(loopAnchor_, t);
        break;
    case DragMode::loopMove:
    {
        const double delta = std::max(-loopDragStart_, t - loopAnchor_);
        start = loopDragStart_ + delta;
        end = loopDragEnd_ + delta;
        break;
    }
    case DragMode::loopStart:
        start = std::min(t, loopDragEnd_ - 0.01);
        break;
    case DragMode::loopEnd:
        end = std::max(t, loopDragStart_ + 0.01);
        break;
    case DragMode::none:
    case DragMode::locate:
    case DragMode::moveClips:
    case DragMode::trimStart:
    case DragMode::trimEnd:
    case DragMode::fadeIn:
    case DragMode::fadeOut:
    case DragMode::reorderTrack:
    case DragMode::marquee:
    case DragMode::automationPoint:
        break;
    }

    if (end - start > 0.01)
        session_.setLoop(dragMode_ == DragMode::loopCreate ? true : wasEnabled, start, end);
}

void ArrangerView::dragClips(const juce::MouseEvent& e)
{
    if (dragOriginals_.empty())
        return;

    if (!dragMoved_)
    {
        if (e.position.getDistanceFrom(dragStart_) < 3.0f)
            return;
        dragMoved_ = true;

        const char* name = "Move Clip";
        switch (dragMode_)
        {
        case DragMode::trimStart:
        case DragMode::trimEnd: name = "Trim Clip"; break;
        case DragMode::fadeIn:
        case DragMode::fadeOut: name = "Change Fade"; break;
        case DragMode::moveClips: name = dragOriginals_.size() > 1 ? "Move Clips" : "Move Clip"; break;
        case DragMode::none:
        case DragMode::locate:
        case DragMode::loopCreate:
        case DragMode::loopMove:
        case DragMode::loopStart:
        case DragMode::loopEnd:
        case DragMode::reorderTrack:
        case DragMode::marquee:
        case DragMode::automationPoint: break;
        }
        session_.beginTransaction(name);
    }

    auto* um = &session_.getUndoManager();
    const double deltaSeconds = static_cast<double>(e.position.x - dragStart_.x) / pixelsPerSecond_;
    const auto& anchor = dragOriginals_.front();
    auto anchorClip = session_.findClip(anchor.id);
    if (!anchorClip.isValid())
        return;
    const double beatsPerSecond = getTempo() / 60.0;

    switch (dragMode_)
    {
    case DragMode::moveClips:
    {
        double delta = snap(anchor.start + deltaSeconds, e.mods) - anchor.start;
        int minOrdinal = anchor.trackOrdinal;
        int maxOrdinal = anchor.trackOrdinal;
        for (const auto& o : dragOriginals_)
        {
            delta = juce::jmax(delta, -o.start); // nothing may move before zero
            minOrdinal = juce::jmin(minOrdinal, o.trackOrdinal);
            maxOrdinal = juce::jmax(maxOrdinal, o.trackOrdinal);
        }

        const auto list = trackRows();
        const int hoveredRow = rowAt(e.getPosition().y);
        int hoveredOrdinal = anchor.trackOrdinal;
        for (std::size_t k = 0; k < list.size(); ++k)
            if (list[k] == hoveredRow)
                hoveredOrdinal = static_cast<int>(k);
        const int count = static_cast<int>(list.size());
        const int ordinalDelta = juce::jlimit(-minOrdinal, count - 1 - maxOrdinal, hoveredOrdinal - anchor.trackOrdinal);

        for (const auto& o : dragOriginals_)
            if (auto c = session_.findClip(o.id); c.isValid())
            {
                const int targetOrdinal = juce::jlimit(0, count - 1, o.trackOrdinal + ordinalDelta);
                const auto target = rows()[static_cast<std::size_t>(list[static_cast<std::size_t>(targetOrdinal)])].track;
                session_.moveClip(c, target, o.start + delta);
            }
        break;
    }

    case DragMode::trimStart:
    {
        const double end = anchor.start + anchor.length;
        const double earliest = anchor.midi ? 0.0 : juce::jmax(0.0, anchor.start - anchor.offset * anchor.stretch);
        const double newStart = juce::jlimit(earliest, end - session::kMinClipLength, snap(anchor.start + deltaSeconds, e.mods));
        session::clip::setStart(anchorClip, newStart, um);
        session::clip::setLength(anchorClip, end - newStart, um);
        if (anchor.midi)
            session::clip::setOffset(anchorClip, anchor.offset + (newStart - anchor.start) * beatsPerSecond, um);
        else
        {
            session::clip::setOffset(anchorClip, anchor.offset + (newStart - anchor.start) / anchor.stretch, um);
            session::clip::setFadeIn(anchorClip, juce::jmin(anchor.fadeIn, end - newStart), um);
        }
        break;
    }

    case DragMode::trimEnd:
    {
        const double latest = anchor.midi ? 1.0e9
                                          : anchor.start + juce::jmax(session::kMinClipLength, (dragSourceLength_ - anchor.offset) * anchor.stretch);
        const double newEnd = juce::jlimit(anchor.start + session::kMinClipLength, latest, snap(anchor.start + anchor.length + deltaSeconds, e.mods));
        session::clip::setLength(anchorClip, newEnd - anchor.start, um);
        if (!anchor.midi)
            session::clip::setFadeOut(anchorClip, juce::jmin(anchor.fadeOut, newEnd - anchor.start), um);
        break;
    }

    case DragMode::fadeIn:
        session::clip::setFadeIn(anchorClip, xToTime(e.position.x) - anchor.start, um);
        break;

    case DragMode::fadeOut:
        session::clip::setFadeOut(anchorClip, anchor.start + anchor.length - xToTime(e.position.x), um);
        break;

    case DragMode::none:
    case DragMode::locate:
    case DragMode::loopCreate:
    case DragMode::loopMove:
    case DragMode::loopStart:
    case DragMode::loopEnd:
    case DragMode::reorderTrack:
    case DragMode::marquee:
    case DragMode::automationPoint:
        break;
    }
}

void ArrangerView::mouseUp(const juce::MouseEvent& e)
{
    if (dragMode_ == DragMode::reorderTrack && dragMoved_ && reorderTrack_.isValid())
    {
        const auto& all = rows();
        const int from = session_.indexOf(reorderTrack_);
        int to = session_.getNumTracks();
        if (reorderTargetRow_ >= 0 && reorderTargetRow_ < static_cast<int>(all.size()))
            to = session_.indexOf(all[static_cast<std::size_t>(reorderTargetRow_)].track);

        if (editor_.isTrackSelected(session::track::getId(reorderTrack_)) && editor_.getSelectedTracks().size() > 1)
        {
            // The whole selection moves, in order (into a folder if dropped inside one).
            session_.beginTransaction("Move Tracks");
            session_.moveTracks(selectedTracksInOrder(true), to);
        }
        else if (from >= 0 && to != from && to != from + 1)
        {
            session_.beginTransaction("Move Track");
            session_.moveTrack(from, to);
        }
        else if (from >= 0 && to == session_.getNumTracks() && session::Session::getParentFolder(reorderTrack_).isValid())
        {
            session_.beginTransaction("Move Track");
            session_.moveIntoFolder(reorderTrack_, {});
        }
    }

    if (dragMode_ == DragMode::loopStart || dragMode_ == DragMode::loopEnd || dragMode_ == DragMode::loopMove)
        if (!dragMoved_ && e.getNumberOfClicks() == 1)
            locateTo(xToTime(e.position.x));

    if (dragMode_ == DragMode::marquee)
    {
        if (!marqueeActive_) // a click, not a drag
            locateTo(pendingLocate_);
        marqueeActive_ = false;
        repaint();
    }

    // A plain click on a clip that was part of a multi-selection selects just that clip.
    if (dragMode_ == DragMode::moveClips && !dragMoved_ && !e.mods.isAnyModifierKeyDown())
        editor_.selectClip(dragAnchorId_, false);
    // Likewise for a track header.
    if (dragMode_ == DragMode::reorderTrack && !dragMoved_ && pendingTrackClick_ != 0 && !e.mods.isAnyModifierKeyDown())
        editor_.selectTrack(pendingTrackClick_);
    pendingTrackClick_ = 0;

    dragMode_ = DragMode::none;
    dragMoved_ = false;
    dragOriginals_.clear();
    reorderTrack_ = {};
    reorderTargetRow_ = -1;
    dragPoint_ = {};
    repaint();
}

void ArrangerView::mouseMove(const juce::MouseEvent& e)
{
    auto newCursor = juce::MouseCursor::NormalCursor;
    const auto position = e.getPosition();

    if (getLoopStrip().contains(position))
    {
        const float x0 = timeToX(session_.getLoopStart());
        const float x1 = timeToX(session_.getLoopEnd());
        if (std::abs(e.position.x - x0) <= 5.0f || std::abs(e.position.x - x1) <= 5.0f)
            newCursor = juce::MouseCursor::LeftRightResizeCursor;
        else if (e.position.x > x0 && e.position.x < x1)
            newCursor = juce::MouseCursor::DraggingHandCursor;
        else
            newCursor = juce::MouseCursor::IBeamCursor;
    }
    else if (getRulerArea().contains(position))
    {
        newCursor = juce::MouseCursor::PointingHandCursor;
    }
    else if (const int row = rowAt(position.y); row >= 0 && getLaneArea().contains(position) &&
                                                rows()[static_cast<std::size_t>(row)].kind == Row::Kind::automation)
    {
        newCursor = hitTestPoint(row, e.position).isValid() ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::CrosshairCursor;
    }
    else if (const auto hit = hitTestClip(e.position); hit.clip.isValid())
    {
        switch (hit.zone)
        {
        case ClipZone::leftEdge: newCursor = juce::MouseCursor::LeftEdgeResizeCursor; break;
        case ClipZone::rightEdge: newCursor = juce::MouseCursor::RightEdgeResizeCursor; break;
        case ClipZone::fadeInHandle:
        case ClipZone::fadeOutHandle: newCursor = juce::MouseCursor::LeftRightResizeCursor; break;
        case ClipZone::body: newCursor = juce::MouseCursor::DraggingHandCursor; break;
        }
    }

    setMouseCursor(newCursor);
}

void ArrangerView::mouseDoubleClick(const juce::MouseEvent& e)
{
    const auto position = e.getPosition();
    const int row = rowAt(position.y);

    if (getLoopStrip().contains(position))
    {
        session_.beginTransaction("Toggle Loop");
        session_.setLoopEnabled(!session_.isLoopEnabled());
        return;
    }

    if (getHeaderArea().contains(position) && row >= 0 && rows()[static_cast<std::size_t>(row)].kind == Row::Kind::track &&
        getNameBounds(row).contains(position))
    {
        startRename(row);
        return;
    }

    if (!getLaneArea().contains(position))
        return;

    if (row >= 0 && rows()[static_cast<std::size_t>(row)].kind == Row::Kind::automation)
    {
        if (auto point = hitTestPoint(row, e.position); point.isValid())
        {
            session_.beginTransaction("Delete Automation Point");
            session_.removeAutomationPoint(point);
        }
        return;
    }

    if (const auto hit = hitTestClip(e.position); hit.clip.isValid())
    {
        if (onOpenClip)
            onOpenClip(session::clip::getId(hit.clip));
        return;
    }

    if (row < 0)
    {
        actions::addTrack(session_, editor_);
        return;
    }

    const auto t = rows()[static_cast<std::size_t>(row)].track;
    if (session::track::getKind(t) == session::TrackKind::instrument)
        createMidiClipAt(t, snap(xToTime(e.position.x), e.mods));
}

void ArrangerView::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (e.mods.isCommandDown())
    {
        const auto lanes = getLaneArea();
        const float anchorX = juce::jlimit(static_cast<float>(lanes.getX()), static_cast<float>(lanes.getRight()), e.position.x);
        const double anchorTime = xToTime(anchorX);
        pixelsPerSecond_ = juce::jlimit(kMinPixelsPerSecond, kMaxPixelsPerSecond,
                                        pixelsPerSecond_ * std::pow(1.0025, static_cast<double>(wheel.deltaY) * 256.0));
        setScroll(anchorTime - static_cast<double>(anchorX - static_cast<float>(lanes.getX())) / pixelsPerSecond_, scrollY_);
        return;
    }

    const bool horizontal = e.mods.isShiftDown() || std::abs(wheel.deltaX) > std::abs(wheel.deltaY);
    if (horizontal)
    {
        const float delta = std::abs(wheel.deltaX) > std::abs(wheel.deltaY) ? wheel.deltaX : wheel.deltaY;
        setScroll(scrollSeconds_ - static_cast<double>(delta) * getVisibleSeconds() * 0.25, scrollY_);
    }
    else
    {
        setScroll(scrollSeconds_, scrollY_ - juce::roundToInt(wheel.deltaY * 160.0f));
    }
}

void ArrangerView::createMidiClipAt(const juce::ValueTree& t, double seconds)
{
    const double barSeconds = kBeatsPerBar * 60.0 / getTempo();
    session_.beginTransaction("New MIDI Clip");
    auto c = session_.addMidiClip(t, seconds, barSeconds);
    editor_.selectClip(session::clip::getId(c), false);
    if (onOpenClip)
        onOpenClip(session::clip::getId(c));
}

//==================================================================================================
// Rename

void ArrangerView::startRename(int row)
{
    const auto t = rows()[static_cast<std::size_t>(row)].track;
    renamingTrackId_ = session::track::getId(t);
    renameEditor_->setBounds(getNameBounds(row).expanded(4, 2));
    renameEditor_->setText(session::track::getName(t), false);
    renameEditor_->setVisible(true);
    renameEditor_->selectAll();
    renameEditor_->grabKeyboardFocus();
    repaint();
}

void ArrangerView::finishRename(bool commit)
{
    if (renamingTrackId_ == 0)
        return;

    const auto trackId = renamingTrackId_;
    renamingTrackId_ = 0; // guards against re-entry from onFocusLost
    renameEditor_->setVisible(false);

    if (commit)
        if (auto t = session_.findTrack(trackId); t.isValid())
        {
            const auto newName = renameEditor_->getText().trim();
            if (newName.isNotEmpty() && newName != session::track::getName(t))
            {
                session_.beginTransaction("Rename Track");
                session::track::setName(t, newName, &session_.getUndoManager());
            }
        }

    repaint();
}

//==================================================================================================
// Context menus

void ArrangerView::showClipMenu(const juce::ValueTree& clipTree)
{
    juce::Component::SafePointer<ArrangerView> safe(this);
    const auto clipId = session::clip::getId(clipTree);
    const double playhead = playheadSeconds_;
    const bool playheadInside = playhead > session::clip::getStart(clipTree) && playhead < session::clip::getEnd(clipTree);
    const bool midi = session::clip::isMidi(clipTree);

    juce::PopupMenu menu;
    menu.addSectionHeader(session::clip::getName(clipTree));
    menu.addItem(midi ? "Edit Notes..." : "Clip Properties...", [safe, clipId]
                 {
                     if (safe != nullptr && safe->onOpenClip)
                         safe->onOpenClip(clipId);
                 });
    menu.addItem("Split at Playhead", playheadInside, false, [safe, playhead]
                 {
                     if (safe != nullptr)
                         actions::splitAt(safe->session_, safe->editor_, playhead);
                 });
    menu.addItem("Duplicate", [safe]
                 {
                     if (safe != nullptr)
                         actions::duplicateSelectedClips(safe->session_, safe->editor_);
                 });

    if (midi)
    {
        menu.addItem("Quantise to Grid", [safe, clipId]
                     {
                         if (safe == nullptr)
                             return;
                         if (auto c = safe->session_.findClip(clipId); c.isValid())
                         {
                             safe->session_.beginTransaction("Quantise");
                             safe->session_.quantise(c, {}, safe->editor_.getGridBeats(), 1.0, false);
                         }
                     });
    }
    else
    {
        menu.addItem("Remove Fades", [safe, clipId]
                     {
                         if (safe == nullptr)
                             return;
                         if (auto c = safe->session_.findClip(clipId); c.isValid())
                         {
                             safe->session_.beginTransaction("Remove Fades");
                             session::clip::setFadeIn(c, 0.0, &safe->session_.getUndoManager());
                             session::clip::setFadeOut(c, 0.0, &safe->session_.getUndoManager());
                         }
                     });
        menu.addItem("Reset Stretch and Pitch", [safe, clipId]
                     {
                         if (safe == nullptr)
                             return;
                         if (auto c = safe->session_.findClip(clipId); c.isValid())
                         {
                             safe->session_.beginTransaction("Reset Stretch and Pitch");
                             session::clip::setStretch(c, 1.0, &safe->session_.getUndoManager());
                             session::clip::setPitch(c, 0.0, &safe->session_.getUndoManager());
                         }
                     });
    }

    menu.addSeparator();
    menu.addItem("Delete", [safe]
                 {
                     if (safe != nullptr)
                         actions::deleteSelectedClips(safe->session_, safe->editor_);
                 });

    menu.showMenuAsync(juce::PopupMenu::Options().withMousePosition());
}

void ArrangerView::addAutomationMenuItems(juce::PopupMenu& menu, const juce::ValueTree& trackTree)
{
    juce::Component::SafePointer<ArrangerView> safe(this);
    const auto trackId = session::track::getId(trackTree);

    const auto addLane = [safe, trackId](const juce::String& target)
    {
        return [safe, trackId, target]
        {
            if (safe == nullptr)
                return;
            auto t = safe->session_.findTrack(trackId);
            if (!t.isValid())
                return;
            safe->session_.beginTransaction("Add Automation Lane");
            auto lane = safe->session_.getAutomation(t, target, true, safe->currentValueFor(t, target));
            session::automation::setVisible(lane, true, &safe->session_.getUndoManager());
            session::track::setShowingAutomation(t, true, nullptr);
        };
    };

    menu.addItem("Volume", addLane(session::automation::volumeTarget()));
    menu.addItem("Pan", addLane(session::automation::panTarget()));

    for (const auto& s : session::Session::getSends(trackTree))
        if (const auto bus = session_.findTrack(session::send::getTarget(s)); bus.isValid())
            menu.addItem("Send > " + session::track::getName(bus), addLane(session::automation::sendTarget(session::send::getId(s))));

    for (const auto& d : session::Session::getDevices(trackTree))
    {
        auto node = project_.getDevices().findNode(session::device::getId(d));
        if (node == nullptr)
            continue;
        juce::PopupMenu parameters;
        const auto& list = node->getProcessor().getParameters();
        for (int i = 0; i < list.size() && i < 256; ++i)
            if (list[i]->isAutomatable())
                parameters.addItem(list[i]->getName(40), addLane(session::automation::parameterTarget(session::device::getId(d), i)));
        menu.addSubMenu(session::device::getName(d), parameters, parameters.getNumItems() > 0);
    }
}

void ArrangerView::showTrackMenu(const juce::ValueTree& t)
{
    juce::Component::SafePointer<ArrangerView> safe(this);
    const auto trackId = session::track::getId(t);
    auto* um = &session_.getUndoManager();
    juce::ignoreUnused(um);

    const auto withTrack = [safe, trackId](auto fn)
    {
        return [safe, trackId, fn]
        {
            if (safe == nullptr)
                return;
            if (auto tree = safe->session_.findTrack(trackId); tree.isValid())
                fn(*safe, tree);
        };
    };

    // Colour, output, folder moves, grouping and deleting apply to every selected track (the clicked
    // one is always among them: a right-click elsewhere selects it alone).
    auto selection = selectedTracksInOrder(false);
    if (std::find(selection.begin(), selection.end(), t) == selection.end())
        selection = {t};
    auto outermost = session_.outermostInOrder(selection);
    const bool multiple = selection.size() > 1;
    const auto idsOf = [](const std::vector<juce::ValueTree>& tracks)
    {
        std::vector<std::uint32_t> ids;
        for (const auto& tree : tracks)
            ids.push_back(session::track::getId(tree));
        return ids;
    };
    const auto withTracks = [safe](std::vector<std::uint32_t> ids, auto fn)
    {
        return [safe, ids, fn]
        {
            if (safe == nullptr)
                return;
            std::vector<juce::ValueTree> trees;
            for (const auto id : ids)
                if (auto tree = safe->session_.findTrack(id); tree.isValid())
                    trees.push_back(tree);
            if (!trees.empty())
                fn(*safe, trees);
        };
    };
    const auto all = [](const std::vector<juce::ValueTree>& tracks, auto predicate)
    { return std::all_of(tracks.begin(), tracks.end(), predicate); };

    juce::PopupMenu colours;
    const auto& palette = session::getTrackPalette();
    for (std::size_t i = 0; i < palette.size(); ++i)
    {
        juce::PopupMenu::Item item(kColourNames[i]);
        item.colour = palette[i];
        item.isTicked = all(selection, [&](const juce::ValueTree& tree) { return session::track::getColour(tree) == palette[i]; });
        item.action = withTracks(idsOf(selection), [colour = palette[i]](ArrangerView& self, const std::vector<juce::ValueTree>& trees)
                                 {
                                     self.session_.beginTransaction("Track Colour");
                                     for (auto tree : trees)
                                         session::track::setColour(tree, colour, &self.session_.getUndoManager());
                                 });
        colours.addItem(item);
    }

    // Output routing: master (or the folder), or any bus that would not create a loop.
    juce::PopupMenu output;
    const auto folderTree = session::Session::getParentFolder(t);
    const juce::String direct = multiple ? juce::String("Master / Folder")
                                : folderTree.isValid() ? "Folder (" + session::track::getName(folderTree) + ")"
                                                       : juce::String("Master");
    output.addItem(direct, true, all(selection, [](const juce::ValueTree& tree) { return session::track::getOutput(tree) == 0; }),
                   withTracks(idsOf(selection), [](ArrangerView& self, const std::vector<juce::ValueTree>& trees)
                              {
                                  self.session_.beginTransaction("Route Track");
                                  for (auto tree : trees)
                                      session::track::setOutput(tree, 0, &self.session_.getUndoManager());
                              }));
    for (const auto& bus : session_.getBusses())
    {
        const bool own = std::any_of(selection.begin(), selection.end(), [&bus](const juce::ValueTree& tree)
                                     { return bus == tree || bus == session::Session::getParentFolder(tree); });
        if (own)
            continue;
        const auto busId = session::track::getId(bus);
        output.addItem(session::track::getName(bus), all(selection, [&](const juce::ValueTree& tree) { return session_.canFeed(tree, bus); }),
                       all(selection, [busId](const juce::ValueTree& tree) { return session::track::getOutput(tree) == busId; }),
                       withTracks(idsOf(selection), [busId](ArrangerView& self, const std::vector<juce::ValueTree>& trees)
                                  {
                                      self.session_.beginTransaction("Route Track");
                                      for (auto tree : trees)
                                          session::track::setOutput(tree, busId, &self.session_.getUndoManager());
                                  }));
    }

    // Folders to move into (not one of the tracks moving, nor inside one).
    juce::PopupMenu folders;
    for (const auto& candidate : session_.getAllTracks())
    {
        if (!session::track::isFolder(candidate))
            continue;
        const bool inside = std::any_of(outermost.begin(), outermost.end(), [&candidate](const juce::ValueTree& tree)
                                        { return session::Session::isWithin(candidate, tree); });
        const bool already = all(outermost, [&candidate](const juce::ValueTree& tree) { return session::Session::getParentFolder(tree) == candidate; });
        if (inside || already)
            continue;
        const auto folderId = session::track::getId(candidate);
        folders.addItem(session::track::getName(candidate),
                        withTracks(idsOf(outermost), [folderId](ArrangerView& self, const std::vector<juce::ValueTree>& trees)
                                   {
                                       self.session_.beginTransaction("Move into Folder");
                                       for (const auto& tree : trees)
                                           self.session_.moveIntoFolder(tree, self.session_.findTrack(folderId));
                                   }));
    }
    const bool anyInFolder = std::any_of(outermost.begin(), outermost.end(), [](const juce::ValueTree& tree)
                                         { return session::Session::getParentFolder(tree).isValid(); });

    juce::PopupMenu automationMenu;
    addAutomationMenuItems(automationMenu, t);

    juce::PopupMenu menu;
    menu.addSectionHeader(multiple ? juce::String(selection.size()) + " tracks selected"
                                   : session::track::getName(t) + "  (" + kindLabel(t) + ")");
    menu.addItem("Rename", withTrack([](ArrangerView& self, juce::ValueTree tree)
                                     {
                                         const int row = self.rowOfTrack(tree);
                                         if (row >= 0)
                                             self.startRename(row);
                                     }));
    menu.addSubMenu("Colour", colours);
    menu.addSubMenu("Output", output);
    menu.addSubMenu("Add Automation Lane", automationMenu);
    menu.addSeparator();
    menu.addSubMenu("Move into Folder", folders, folders.getNumItems() > 0);
    menu.addItem("Move out of Folder", anyInFolder, false,
                 withTracks(idsOf(outermost), [](ArrangerView& self, const std::vector<juce::ValueTree>& trees)
                            {
                                self.session_.beginTransaction("Move out of Folder");
                                for (const auto& tree : trees)
                                    self.session_.moveIntoFolder(tree, {});
                            }));
    menu.addItem(outermost.size() > 1 ? "Group " + juce::String(outermost.size()) + " Tracks into New Folder" : juce::String("Group into New Folder"), [safe]
                 {
                     if (safe != nullptr)
                         actions::groupSelectedTracksIntoFolder(safe->session_, safe->editor_);
                 });
    if (session::track::isFolder(t))
        menu.addItem(session::track::isCollapsed(t) ? "Expand Folder" : "Collapse Folder",
                     withTrack([](ArrangerView&, juce::ValueTree tree)
                               { session::track::setCollapsed(tree, !session::track::isCollapsed(tree), nullptr); }));
    menu.addSeparator();
    menu.addItem("Add Audio Track", [safe] { if (safe != nullptr) actions::addTrack(safe->session_, safe->editor_); });
    menu.addItem("Add Instrument Track (Drum Rack)", [safe]
                 { if (safe != nullptr) actions::addInstrumentTrack(safe->session_, safe->editor_, "builtin:drumrack", "Drum Rack"); });
    menu.addItem("Add Instrument Track (Sampler)", [safe]
                 { if (safe != nullptr) actions::addInstrumentTrack(safe->session_, safe->editor_, "builtin:sampler", "Sampler"); });
    menu.addItem("Add Bus", [safe] { if (safe != nullptr) actions::addTrack(safe->session_, safe->editor_, session::TrackKind::bus); });
    menu.addItem("Add Folder", [safe] { if (safe != nullptr) actions::addFolder(safe->session_, safe->editor_); });
    menu.addSeparator();
    menu.addItem(outermost.size() > 1 ? "Delete " + juce::String(outermost.size()) + " Tracks" : juce::String("Delete Track"), [safe]
                 {
                     if (safe != nullptr)
                         actions::deleteSelectedTracks(safe->session_, safe->editor_);
                 });

    menu.showMenuAsync(juce::PopupMenu::Options().withMousePosition());
}

void ArrangerView::showLaneMenu(int row, double seconds)
{
    juce::Component::SafePointer<ArrangerView> safe(this);
    const auto t = row >= 0 ? rows()[static_cast<std::size_t>(row)].track : juce::ValueTree();
    const auto trackId = t.isValid() ? session::track::getId(t) : 0u;
    const bool instrument = t.isValid() && session::track::getKind(t) == session::TrackKind::instrument;

    juce::PopupMenu menu;
    if (instrument)
        menu.addItem("New MIDI Clip Here", [safe, trackId, seconds]
                     {
                         if (safe == nullptr)
                             return;
                         if (auto tree = safe->session_.findTrack(trackId); tree.isValid())
                             safe->createMidiClipAt(tree, safe->editor_.snap(seconds, safe->getTempo()));
                     });
    menu.addItem("Add Audio Track", [safe] { if (safe != nullptr) actions::addTrack(safe->session_, safe->editor_); });
    menu.addItem("Import Audio...", [safe]
                 {
                     if (safe != nullptr && safe->onImportRequested)
                         safe->onImportRequested();
                 });
    menu.addItem("Split Here", row >= 0, false, [safe, seconds]
                 {
                     if (safe != nullptr)
                         actions::splitAt(safe->session_, safe->editor_, seconds);
                 });
    menu.addItem("Set Loop to This Bar", [safe, seconds]
                 {
                     if (safe == nullptr)
                         return;
                     const double bar = kBeatsPerBar * 60.0 / safe->getTempo();
                     const double start = std::floor(seconds / bar) * bar;
                     safe->session_.beginTransaction("Set Loop");
                     safe->session_.setLoop(true, start, start + bar);
                 });

    menu.showMenuAsync(juce::PopupMenu::Options().withMousePosition());
}

void ArrangerView::showAutomationMenu(const juce::ValueTree& lane, const juce::ValueTree& point)
{
    juce::Component::SafePointer<ArrangerView> safe(this);
    auto laneCopy = lane;
    auto pointCopy = point;

    juce::PopupMenu menu;
    menu.addSectionHeader(describeTarget(lane));
    if (point.isValid())
        menu.addItem("Delete Point", lane.getNumChildren() > 1, false, [safe, pointCopy]
                     {
                         if (safe == nullptr)
                             return;
                         safe->session_.beginTransaction("Delete Automation Point");
                         safe->session_.removeAutomationPoint(pointCopy);
                     });

    juce::PopupMenu other;
    addAutomationMenuItems(other, lane.getParent());
    menu.addSubMenu("Show Another Parameter", other);
    menu.addItem("Hide Lane", [safe, laneCopy]
                 {
                     if (safe == nullptr)
                         return;
                     safe->session_.beginTransaction("Hide Automation Lane");
                     session::automation::setVisible(laneCopy, false, &safe->session_.getUndoManager());
                 });
    menu.addItem("Delete Automation", [safe, laneCopy]
                 {
                     if (safe == nullptr)
                         return;
                     safe->session_.beginTransaction("Delete Automation");
                     safe->session_.removeAutomation(laneCopy);
                 });

    menu.showMenuAsync(juce::PopupMenu::Options().withMousePosition());
}

//==================================================================================================
// Drag and drop

void ArrangerView::updateDropTarget(int x, int y)
{
    dropActive_ = true;
    dropRow_ = rowAt(y);
    if (dropRow_ >= 0 && rows()[static_cast<std::size_t>(dropRow_)].kind != Row::Kind::track)
        dropRow_ = rowOfTrack(rows()[static_cast<std::size_t>(dropRow_)].track);
    dropTime_ = editor_.snap(juce::jmax(0.0, xToTime(static_cast<float>(x))), getTempo());
    repaint();
}

void ArrangerView::dropFiles(const juce::StringArray& files)
{
    juce::StringArray audioFiles;
    for (const auto& path : files)
        if (project_.getAssets().canRead(juce::File(path)))
            audioFiles.add(path);

    auto target = dropRow_ >= 0 ? rows()[static_cast<std::size_t>(dropRow_)].track : juce::ValueTree();
    if (target.isValid() && session::track::getKind(target) != session::TrackKind::audio)
        target = {};
    if (target.isValid())
        editor_.selectTrack(session::track::getId(target));

    if (!audioFiles.isEmpty())
        project_.importFiles(audioFiles, target, dropTime_);

    dropActive_ = false;
    repaint();
}

bool ArrangerView::isInterestedInFileDrag(const juce::StringArray& files)
{
    for (const auto& path : files)
        if (project_.getAssets().canRead(juce::File(path)))
            return true;
    return false;
}

void ArrangerView::fileDragEnter(const juce::StringArray&, int x, int y)
{
    dropIsDevice_ = false;
    updateDropTarget(x, y);
}

void ArrangerView::fileDragMove(const juce::StringArray&, int x, int y) { updateDropTarget(x, y); }

void ArrangerView::fileDragExit(const juce::StringArray&)
{
    dropActive_ = false;
    repaint();
}

void ArrangerView::filesDropped(const juce::StringArray& files, int x, int y)
{
    updateDropTarget(x, y);
    dropFiles(files);
}

bool ArrangerView::isInterestedInDragSource(const SourceDetails& details)
{
    const auto description = details.description.toString();
    return browserDrag::isFileDrag(details.description) || description.startsWith(kDeviceDragPrefix);
}

void ArrangerView::itemDragEnter(const SourceDetails& details)
{
    dropIsDevice_ = details.description.toString().startsWith(kDeviceDragPrefix);
    updateDropTarget(details.localPosition.x, details.localPosition.y);
}

void ArrangerView::itemDragMove(const SourceDetails& details) { updateDropTarget(details.localPosition.x, details.localPosition.y); }

void ArrangerView::itemDragExit(const SourceDetails&)
{
    dropActive_ = false;
    repaint();
}

void ArrangerView::itemDropped(const SourceDetails& details)
{
    updateDropTarget(details.localPosition.x, details.localPosition.y);
    const auto description = details.description.toString();

    if (description.startsWith(kDeviceDragPrefix))
    {
        dropActive_ = false;
        const auto target = dropRow_ >= 0 ? rows()[static_cast<std::size_t>(dropRow_)].track : juce::ValueTree();
        if (onDeviceDropped)
            onDeviceDropped(target, description);
        repaint();
        return;
    }

    dropFiles(browserDrag::getFiles(details.description));
}

} // namespace dojo::ui
