#include "ui/components/ArrangerView.h"

#include "ui/BrowserDrag.h"
#include "ui/WaveformPainter.h"

#include "ui/Commands.h"
#include "ui/EditActions.h"
#include "ui/PresetActions.h"
#include "ui/ClipList.h"
#include "ui/components/VideoWindow.h"
#include "ui/theme/DojoTheme.h"
#include "video/VideoSource.h"

#include <algorithm>
#include <cmath>

namespace dojo::ui
{
namespace
{
constexpr int kTrackHeight = 64;
constexpr int kLaneHeight = 48;
constexpr int kTakeHeight = 44;
constexpr int kScrollbarThickness = 10;
constexpr int kExtraSpaceBelow = 2 * kTrackHeight; // empty drop zone below the last track
constexpr int kHeaderButtonWidth = 20;
constexpr int kIndent = 14;
constexpr int kLoopStripHeight = 11;
constexpr float kEdgeGrabWidth = 6.0f;
constexpr float kClipTitleHeight = 16.0f;
constexpr double kSmpteFrameRate = 30.0; ///< the timecode rate of sync point locations
constexpr float kFadeHandleSize = 8.0f;
constexpr float kWarpGrab = 4.0f; ///< (pixels either side of a warp marker or a hitpoint)
constexpr float kHitpointTickHeight = 7.0f;
constexpr float kPointRadius = 4.5f;
constexpr double kMinPixelsPerSecond = 2.0;
constexpr double kMaxPixelsPerSecond = 2000.0;

const char* const kColourNames[] = {"Vermilion", "Persimmon", "Gold", "Matcha", "Jade", "Indigo", "Wisteria", "Sakura"};

/** Edit Lock badge: a padlock. */
void drawPadlock(juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour)
{
    const auto box = area.withSizeKeepingCentre(8.0f, 10.0f);
    const auto body = box.withTrimmedTop(4.0f);
    juce::Path shackle;
    shackle.addCentredArc(box.getCentreX(), body.getY(), 2.6f, 3.4f, 0.0f, -juce::MathConstants<float>::halfPi,
                          juce::MathConstants<float>::halfPi, true);
    g.setColour(colour);
    g.strokePath(shackle, juce::PathStrokeType(1.4f));
    g.fillRoundedRectangle(body, 1.2f);
}

/** Clip rating: `rating` small filled stars (of five). */
void drawStars(juce::Graphics& g, juce::Rectangle<float> area, int rating, juce::Colour colour)
{
    const float size = std::min(9.0f, area.getHeight());
    for (int k = 0; k < rating; ++k)
    {
        juce::Path star;
        const auto centre = juce::Point<float>(area.getX() + size * 0.5f + static_cast<float>(k) * (size + 1.0f), area.getCentreY());
        star.addStar(centre, 5, size * 0.22f, size * 0.5f, 0.0f);
        g.setColour(colour);
        g.fillPath(star);
    }
}

/** Looped clip badge: a circular arrow. */
void drawLoopBadge(juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour)
{
    const auto box = area.withSizeKeepingCentre(10.0f, 10.0f);
    juce::Path arc;
    arc.addCentredArc(box.getCentreX(), box.getCentreY(), 4.0f, 4.0f, 0.0f, 0.5f, juce::MathConstants<float>::twoPi - 0.3f, true);
    g.setColour(colour);
    g.strokePath(arc, juce::PathStrokeType(1.3f));
    juce::Path head;
    const float x = box.getCentreX() + 4.0f * std::sin(0.5f);
    const float y = box.getCentreY() - 4.0f * std::cos(0.5f);
    head.addTriangle(x - 2.5f, y - 2.0f, x + 2.0f, y - 2.5f, x + 0.5f, y + 2.0f);
    g.fillPath(head);
}

/** Time Lock badge: a clock face. */
void drawClockBadge(juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour)
{
    const auto face = area.withSizeKeepingCentre(10.0f, 10.0f);
    const auto centre = face.getCentre();
    g.setColour(colour);
    g.drawEllipse(face.reduced(0.6f), 1.3f);
    g.drawLine(centre.x, centre.y, centre.x, face.getY() + 2.4f, 1.3f);
    g.drawLine(centre.x, centre.y, face.getRight() - 2.6f, centre.y, 1.3f);
}

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

/** Tracks that can be record-enabled (they record audio or MIDI). */
bool canArm(session::TrackKind kind)
{
    return kind == session::TrackKind::audio || session::holdsMidi(kind);
}

juce::String kindLabel(const juce::ValueTree& t)
{
    if (session::track::isFolder(t))
        return "Folder";
    switch (session::track::getKind(t))
    {
    case session::TrackKind::instrument:  return "Instrument";
    case session::TrackKind::bus:         return "Bus";
    case session::TrackKind::midi:        return "MIDI";
    case session::TrackKind::vca:         return "VCA";
    case session::TrackKind::masterFader: return "Master Fader";
    case session::TrackKind::video:       return "Video";
    case session::TrackKind::audio:       break;
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
    setVideoFrames(nullptr);
    project_.getAssets().removeListener(this);
    editor_.removeListener(this);
    session_.getState().removeListener(this);
}

void ArrangerView::setVideoFrames(video::FrameServer* frames)
{
    if (videoFrames_ != nullptr)
        videoFrames_->removeChangeListener(this);
    videoFrames_ = frames;
    if (videoFrames_ != nullptr)
        videoFrames_->addChangeListener(this);
    repaint();
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

        if (session::track::isShowingTakes(t))
            for (const auto& lane : session::Session::getTakeLanes(t))
            {
                Row takeRow;
                takeRow.kind = Row::Kind::take;
                takeRow.track = t;
                takeRow.lane = lane;
                takeRow.depth = row.depth;
                takeRow.top = y;
                takeRow.height = kTakeHeight;
                rows_.push_back(takeRow);
                y += kTakeHeight;
            }

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
    if (y < topHeight())
        return -1;
    const int contentY = y - topHeight() + scrollY_;
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

juce::Rectangle<int> ArrangerView::getTakesButtonArea(const juce::ValueTree& track) const
{
    const int row = rowOfTrack(track);
    return row >= 0 ? getTakesButton(row) : juce::Rectangle<int>();
}

int ArrangerView::rowOfTakeLane(const juce::ValueTree& lane) const
{
    const auto& all = rows();
    for (std::size_t i = 0; i < all.size(); ++i)
        if (all[i].kind == Row::Kind::take && all[i].lane == lane)
            return static_cast<int>(i);
    return -1;
}

juce::Rectangle<int> ArrangerView::getTakeLaneArea(const juce::ValueTree& lane) const
{
    const int row = rowOfTakeLane(lane);
    return row >= 0 ? getRowBounds(row) : juce::Rectangle<int>();
}

juce::Rectangle<int> ArrangerView::getTakeAuditionButtonArea(const juce::ValueTree& lane) const
{
    const int row = rowOfTakeLane(lane);
    return row >= 0 ? getHeaderButton(row, 0) : juce::Rectangle<int>();
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

// --- Tempo track ------------------------------------------------------------------------------------

namespace
{
constexpr int kTempoTrackHeight = 72;
constexpr int kSignatureStripHeight = 16;
constexpr float kTempoPointRadius = 4.5f;

juce::String shapeLabel(time::TempoShape shape, double bend)
{
    switch (shape)
    {
        case time::TempoShape::linear: return "ramp";
        case time::TempoShape::curve: return bend >= 0.0 ? "curve, slow start" : "curve, fast start";
        case time::TempoShape::jump: break;
    }
    return "jump";
}
} // namespace

int ArrangerView::topHeight() const
{
    return theme::metrics::rulerHeight + (editor_.isShowingTempoTrack() ? kTempoTrackHeight : 0);
}

void ArrangerView::editorStateChanged()
{
    const int top = topHeight();
    if (top != shownTopHeight_)
    {
        shownTopHeight_ = top;
        rowsDirty_ = true;
        resized();
    }
    repaint();
}

juce::Rectangle<int> ArrangerView::getTempoTrackArea() const
{
    if (!editor_.isShowingTempoTrack())
        return {};
    return {theme::metrics::trackHeaderWidth, theme::metrics::rulerHeight,
            juce::jmax(0, getWidth() - theme::metrics::trackHeaderWidth - kScrollbarThickness), kTempoTrackHeight};
}

juce::Rectangle<int> ArrangerView::getSignatureStripArea() const { return getTempoTrackArea().withHeight(kSignatureStripHeight); }

juce::Rectangle<int> ArrangerView::getTempoCurveArea() const
{
    return getTempoTrackArea().withTrimmedTop(kSignatureStripHeight).reduced(0, 7);
}

std::pair<double, double> ArrangerView::tempoRange() const
{
    if (dragMode_ == DragMode::tempoPoint && tempoDragRange_.second > tempoDragRange_.first)
        return tempoDragRange_; // (the scale holds still under the mouse)
    double lo = 1.0e9, hi = 0.0;
    for (const auto& p : session_.getTempoMap().getPoints())
    {
        lo = std::min(lo, p.bpm);
        hi = std::max(hi, p.bpm);
    }
    const double middle = (lo + hi) * 0.5;
    const double half = std::max(40.0, (hi - lo) * 0.5 * 1.3);
    return {std::max(session::kMinTempo, middle - half), std::min(session::kMaxTempo, middle + half)};
}

float ArrangerView::tempoToY(double bpm) const
{
    const auto area = getTempoCurveArea().toFloat();
    const auto [lo, hi] = tempoRange();
    const double x = hi > lo ? (bpm - lo) / (hi - lo) : 0.5;
    return area.getBottom() - static_cast<float>(std::clamp(x, 0.0, 1.0)) * area.getHeight();
}

double ArrangerView::yToTempo(float y) const
{
    const auto area = getTempoCurveArea().toFloat();
    const auto [lo, hi] = tempoRange();
    const double x = area.getHeight() > 0.0f ? (area.getBottom() - y) / area.getHeight() : 0.5;
    return juce::jlimit(session::kMinTempo, session::kMaxTempo, lo + (hi - lo) * std::clamp(x, 0.0, 1.0));
}

juce::Point<float> ArrangerView::getTempoPointPosition(int index) const
{
    const auto& map = session_.getTempoMap();
    const auto& points = map.getPoints();
    if (index < 0 || index >= static_cast<int>(points.size()))
        return {};
    const auto& p = points[static_cast<std::size_t>(index)];
    return {timeToX(map.secondsAt(p.beat)), tempoToY(p.bpm)};
}

int ArrangerView::tempoPointAt(juce::Point<float> position) const
{
    if (!getTempoCurveArea().expanded(0, 7).toFloat().contains(position))
        return -1;
    const auto count = static_cast<int>(session_.getTempoMap().getPoints().size());
    int best = -1;
    float bestDistance = kTempoPointRadius + 3.0f;
    for (int i = 0; i < count; ++i)
    {
        const float d = getTempoPointPosition(i).getDistanceFrom(position);
        if (d <= bestDistance)
        {
            best = i;
            bestDistance = d;
        }
    }
    return best;
}

juce::ValueTree ArrangerView::tempoChangeOfPoint(int index) const
{
    const auto& points = session_.getTempoMap().getPoints();
    if (index <= 0 || index >= static_cast<int>(points.size()))
        return {};
    juce::ValueTree found;
    for (const auto& change : session_.getTempoChanges())
        if (std::abs(static_cast<double>(change.getProperty(session::ids::beat)) - points[static_cast<std::size_t>(index)].beat) < 1.0e-9)
            found = change; // (the later of two at a beat is the one in force)
    return found;
}

juce::ValueTree ArrangerView::signatureChangeAt(juce::Point<int> position) const
{
    if (!getSignatureStripArea().contains(position))
        return {};
    const auto& map = session_.getTempoMap();
    for (const auto& change : session_.getSignatureChanges())
    {
        const int bar = static_cast<int>(change.getProperty(session::ids::bar)) - 1;
        const float x = timeToX(map.secondsAt(map.beatOfBar(bar)));
        const auto px = static_cast<float>(position.x);
        if (px >= x - 3.0f && px <= x + 34.0f)
            return change;
    }
    return {};
}

void ArrangerView::paintTempoTrack(juce::Graphics& g) const
{
    if (!editor_.isShowingTempoTrack())
        return;
    using namespace theme;
    const auto area = getTempoTrackArea();
    const auto map = session_.getTempoMapShared();

    // Header: the tempo and signature at the playhead.
    const juce::Rectangle<int> corner(0, area.getY(), metrics::trackHeaderWidth, area.getHeight());
    g.setColour(colour::panelHeader);
    g.fillRect(corner);
    auto text = corner.reduced(14, 8);
    g.setColour(colour::textSecondary);
    g.setFont(label(11.0f));
    g.drawText("TEMPO", text.removeFromTop(16), juce::Justification::centredLeft, false);
    const double beat = map->beatAt(playheadSeconds_);
    const auto signature = map->signatureAt(beat);
    g.setColour(colour::gold);
    g.setFont(mono(13.0f));
    g.drawText(juce::String(map->bpmAtBeat(beat), 2) + " BPM", text.removeFromTop(18), juce::Justification::centredLeft, false);
    g.setColour(colour::textMuted);
    g.setFont(sans(11.0f));
    g.drawText(juce::String(signature.numerator) + "/" + juce::String(signature.denominator) + "   double-click to add",
               text.removeFromTop(16), juce::Justification::centredLeft, false);

    g.setColour(colour::panelRaised);
    g.fillRect(area.withWidth(area.getWidth() + kScrollbarThickness));
    g.saveState();
    g.reduceClipRegion(area);

    // Time signatures, where their bars start.
    const auto strip = getSignatureStripArea();
    g.setColour(colour::panel);
    g.fillRect(strip);
    for (const auto& s : map->getSignatures())
    {
        const float x = timeToX(map->secondsAt(map->beatOfBar(s.bar)));
        g.setColour(colour::gold.withAlpha(0.8f));
        g.fillRect(x, static_cast<float>(strip.getY()) + 2.0f, 1.5f, static_cast<float>(strip.getHeight()) - 2.0f);
        g.setFont(mono(10.5f));
        g.drawText(juce::String(s.signature.numerator) + "/" + juce::String(s.signature.denominator),
                   juce::Rectangle<float>(x + 4.0f, static_cast<float>(strip.getY()), 40.0f, static_cast<float>(strip.getHeight())),
                   juce::Justification::centredLeft, false);
    }

    // The tempo over time: the curve, filled below; the points; the scale.
    const auto curve = getTempoCurveArea();
    juce::Path line;
    for (int x = area.getX() - 2; x <= area.getRight() + 2; x += 2)
    {
        const double t = std::max(0.0, xToTime(static_cast<float>(x)));
        const float y = tempoToY(map->bpmAt(t));
        if (line.isEmpty())
            line.startNewSubPath(static_cast<float>(x), y);
        else
            line.lineTo(static_cast<float>(x), y);
    }
    juce::Path fill(line);
    fill.lineTo(static_cast<float>(area.getRight() + 2), static_cast<float>(area.getBottom()));
    fill.lineTo(static_cast<float>(area.getX() - 2), static_cast<float>(area.getBottom()));
    fill.closeSubPath();
    g.setColour(colour::gold.withAlpha(0.08f));
    g.fillPath(fill);
    g.setColour(colour::gold.withAlpha(0.85f));
    g.strokePath(line, juce::PathStrokeType(1.5f));

    const auto& points = map->getPoints();
    for (int i = 0; i < static_cast<int>(points.size()); ++i)
    {
        const auto position = getTempoPointPosition(i);
        const auto& p = points[static_cast<std::size_t>(i)];
        const bool dragged = dragMode_ == DragMode::tempoPoint && dragTempoIndex_ == i;
        g.setColour(dragged ? colour::accentBright : colour::gold);
        if (p.shape == time::TempoShape::jump)
            g.fillEllipse(position.x - kTempoPointRadius, position.y - kTempoPointRadius, kTempoPointRadius * 2.0f, kTempoPointRadius * 2.0f);
        else
            g.drawEllipse(position.x - kTempoPointRadius, position.y - kTempoPointRadius, kTempoPointRadius * 2.0f, kTempoPointRadius * 2.0f,
                          1.8f);
        g.setColour(colour::textSecondary);
        g.setFont(mono(10.0f));
        const float labelY = position.y < static_cast<float>(curve.getCentreY()) ? position.y + 4.0f : position.y - 16.0f;
        g.drawText(juce::String(p.bpm, p.bpm == std::round(p.bpm) ? 0 : 2), juce::Rectangle<float>(position.x + 6.0f, labelY, 60.0f, 12.0f),
                   juce::Justification::centredLeft, false);
    }

    const auto [lo, hi] = tempoRange();
    g.setColour(colour::textMuted);
    g.setFont(mono(9.0f));
    g.drawText(juce::String(juce::roundToInt(hi)), juce::Rectangle<int>(area.getRight() - 34, curve.getY() - 6, 30, 11),
               juce::Justification::centredRight, false);
    g.drawText(juce::String(juce::roundToInt(lo)), juce::Rectangle<int>(area.getRight() - 34, curve.getBottom() - 6, 30, 11),
               juce::Justification::centredRight, false);
    g.restoreState();

    g.setColour(colour::outline);
    g.fillRect(0, area.getBottom() - 1, getWidth(), 1);
}

juce::PopupMenu ArrangerView::buildSignatureMenu(int bar)
{
    juce::Component::SafePointer<ArrangerView> safe(this);
    const auto current = session_.getTempoMap().signatureOfBar(bar - 1);
    juce::PopupMenu menu;
    menu.addSectionHeader("Time Signature from Bar " + juce::String(bar));
    for (const auto& offered : std::vector<std::pair<int, int>>{{2, 4}, {3, 4}, {4, 4}, {5, 4}, {6, 4}, {7, 4}, {3, 8}, {5, 8}, {6, 8},
                                                                {7, 8}, {9, 8}, {11, 8}, {12, 8}, {2, 2}, {3, 2}})
    {
        const int n = offered.first, d = offered.second;
        menu.addItem(juce::String(n) + "/" + juce::String(d), true, current.numerator == n && current.denominator == d,
                     [safe, bar, n, d]
                     {
                         if (safe == nullptr)
                             return;
                         safe->session_.beginTransaction("Time Signature");
                         safe->session_.addSignatureChange(bar, n, d);
                     });
    }
    return menu;
}

juce::PopupMenu ArrangerView::buildTempoMenu(juce::Point<int> position)
{
    juce::Component::SafePointer<ArrangerView> safe(this);
    juce::PopupMenu menu;
    const auto& map = session_.getTempoMap();

    // A signature change: change it, or remove it.
    if (auto signature = signatureChangeAt(position); signature.isValid())
    {
        const int bar = signature.getProperty(session::ids::bar);
        menu = buildSignatureMenu(bar);
        menu.addSeparator();
        menu.addItem("Remove Time Signature", [safe, signature]
                     {
                         if (safe == nullptr)
                             return;
                         safe->session_.beginTransaction("Remove Time Signature");
                         safe->session_.removeSignatureChange(signature);
                     });
        return menu;
    }

    const int index = tempoPointAt(position.toFloat());
    if (index >= 0)
    {
        // A tempo point: how it goes to the next one, its tempo, removing it.
        const auto& p = map.getPoints()[static_cast<std::size_t>(index)];
        auto change = tempoChangeOfPoint(index);
        menu.addSectionHeader((index == 0 ? juce::String("Start Tempo") : "Tempo Change") + ": " + juce::String(p.bpm, 2) + " BPM, "
                              + shapeLabel(p.shape, p.bend));
        const auto setShape = [safe, index, change](time::TempoShape shape, double bend)
        {
            return [safe, index, change, shape, bend]
            {
                if (safe == nullptr)
                    return;
                safe->session_.beginTransaction("Tempo Shape");
                if (index == 0)
                    safe->session_.setStartTempoShape(shape, bend);
                else
                    safe->session_.setTempoChangeShape(change, shape, bend);
            };
        };
        const bool last = index + 1 >= static_cast<int>(map.getPoints().size());
        menu.addItem("Jump (hold, then change)", true, p.shape == time::TempoShape::jump, setShape(time::TempoShape::jump, 0.0));
        menu.addItem("Linear Ramp to the Next", !last, p.shape == time::TempoShape::linear, setShape(time::TempoShape::linear, 0.0));
        menu.addItem("Curve to the Next: Slow Start", !last, p.shape == time::TempoShape::curve && p.bend > 0.0 && p.bend < 0.75,
                     setShape(time::TempoShape::curve, 0.5));
        menu.addItem("Curve to the Next: Very Slow Start", !last, p.shape == time::TempoShape::curve && p.bend >= 0.75,
                     setShape(time::TempoShape::curve, 0.9));
        menu.addItem("Curve to the Next: Fast Start", !last, p.shape == time::TempoShape::curve && p.bend < 0.0 && p.bend > -0.75,
                     setShape(time::TempoShape::curve, -0.5));
        menu.addItem("Curve to the Next: Very Fast Start", !last, p.shape == time::TempoShape::curve && p.bend <= -0.75,
                     setShape(time::TempoShape::curve, -0.9));
        menu.addSeparator();
        menu.addItem("Set Tempo...", [safe, index] { if (safe != nullptr) safe->showTempoValueEditor(index); });
        menu.addItem("Remove Tempo Change", index > 0, false, [safe, change]
                     {
                         if (safe == nullptr)
                             return;
                         safe->session_.beginTransaction("Remove Tempo Change");
                         safe->session_.removeTempoChange(change);
                     });
        return menu;
    }

    // Empty space: a tempo change or a time signature here.
    const double seconds = std::max(0.0, xToTime(static_cast<float>(position.x)));
    const double snapped = editor_.snap(seconds, map);
    const double beat = map.beatAt(snapped);
    const double bpm = getTempoCurveArea().contains(position) ? std::round(yToTempo(static_cast<float>(position.y)) * 100.0) / 100.0
                                                              : map.bpmAtBeat(beat);
    menu.addItem("Add Tempo Change Here (" + juce::String(bpm, 2) + " BPM)", beat > 0.0, false, [safe, beat, bpm]
                 {
                     if (safe == nullptr)
                         return;
                     safe->session_.beginTransaction("Add Tempo Change");
                     safe->session_.addTempoChange(beat, bpm);
                 });
    const int bar = static_cast<int>(map.barAt(map.beatAt(seconds))) + 1;
    menu.addSubMenu("Time Signature from Bar " + juce::String(bar), buildSignatureMenu(bar));
    menu.addSeparator();
    menu.addItem("Clear Tempo Map", !map.isConstant(), false, [safe]
                 {
                     if (safe == nullptr)
                         return;
                     safe->session_.beginTransaction("Clear Tempo Map");
                     safe->session_.clearTempoMap();
                 });
    return menu;
}

void ArrangerView::showTempoValueEditor(int index)
{
    const auto& points = session_.getTempoMap().getPoints();
    if (index < 0 || index >= static_cast<int>(points.size()))
        return;
    auto* window = new juce::AlertWindow("Set Tempo", index == 0 ? "The start tempo (BPM):" : "This tempo change (BPM):",
                                         juce::MessageBoxIconType::NoIcon, this);
    window->addTextEditor("bpm", juce::String(points[static_cast<std::size_t>(index)].bpm, 2));
    window->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<ArrangerView> safe(this);
    auto change = tempoChangeOfPoint(index);
    const double beat = points[static_cast<std::size_t>(index)].beat;
    window->enterModalState(true, juce::ModalCallbackFunction::create(
                                      [safe, window, index, change, beat](int result)
                                      {
                                          const double bpm = window->getTextEditorContents("bpm").getDoubleValue();
                                          if (safe == nullptr || result != 1 || bpm <= 0.0)
                                              return;
                                          safe->session_.beginTransaction("Set Tempo");
                                          if (index == 0)
                                              safe->session_.setTempo(bpm);
                                          else
                                              safe->session_.setTempoChange(change, beat, bpm);
                                      }),
                            true);
}

void ArrangerView::mouseDownOnTempoTrack(const juce::MouseEvent& e)
{
    const auto position = e.getPosition();
    if (e.mods.isPopupMenu())
    {
        buildTempoMenu(position).showMenuAsync(juce::PopupMenu::Options().withMousePosition());
        return;
    }
    if (auto signature = signatureChangeAt(position); signature.isValid())
    {
        dragSignature_ = signature;
        dragMode_ = DragMode::signatureMarker;
        session_.beginTransaction("Move Time Signature");
        return;
    }
    const int index = tempoPointAt(e.position);
    if (index < 0)
        return;
    dragTempoIndex_ = index;
    dragTempoChange_ = tempoChangeOfPoint(index);
    tempoDragMap_ = session_.getTempoMapShared();
    tempoDragRange_ = {0.0, 0.0};
    tempoDragRange_ = tempoRange();
    dragMode_ = DragMode::tempoPoint;
    session_.beginTransaction(index == 0 ? "Change Tempo" : "Move Tempo Change");
}

void ArrangerView::dragTempoTrack(const juce::MouseEvent& e)
{
    if (dragMode_ == DragMode::signatureMarker)
    {
        if (!dragSignature_.getParent().isValid())
            return;
        const auto& map = session_.getTempoMap();
        const auto bar = map.barAt(map.beatAt(std::max(0.0, xToTime(e.position.x)))) + 1;
        if (bar != static_cast<int>(dragSignature_.getProperty(session::ids::bar)))
            session_.setSignatureChange(dragSignature_, static_cast<int>(bar), dragSignature_.getProperty(session::ids::numerator),
                                        dragSignature_.getProperty(session::ids::denominator));
        dragMoved_ = true;
        return;
    }
    if (dragMode_ != DragMode::tempoPoint || tempoDragMap_ == nullptr)
        return;
    dragMoved_ = true;
    // Up and down: the tempo (Shift: finer); along: the beat, on the grid (Alt: free). The start
    // tempo only goes up and down.
    const double y = e.mods.isShiftDown() ? dragStart_.y + (e.position.y - dragStart_.y) * 0.2f : e.position.y;
    const double bpm = std::round(yToTempo(static_cast<float>(y)) * 100.0) / 100.0;
    if (dragTempoIndex_ == 0)
    {
        session_.setTempo(bpm);
        return;
    }
    if (!dragTempoChange_.getParent().isValid())
        return;
    const double seconds = std::max(0.0, xToTime(e.position.x));
    const double snapped = editor_.snap(seconds, *tempoDragMap_, e.mods.isAltDown());
    const double beat = std::max(1.0 / 16.0, tempoDragMap_->beatAt(snapped));
    session_.setTempoChange(dragTempoChange_, beat, bpm);
}

juce::Rectangle<int> ArrangerView::getHeaderArea() const
{
    return {0, topHeight(), theme::metrics::trackHeaderWidth,
            juce::jmax(0, getHeight() - topHeight() - kScrollbarThickness)};
}

juce::Rectangle<int> ArrangerView::getLaneArea() const
{
    return {theme::metrics::trackHeaderWidth, topHeight(),
            juce::jmax(0, getWidth() - theme::metrics::trackHeaderWidth - kScrollbarThickness),
            juce::jmax(0, getHeight() - topHeight() - kScrollbarThickness)};
}

juce::Rectangle<int> ArrangerView::getRowBounds(int row) const
{
    const auto& all = rows();
    if (row < 0 || row >= static_cast<int>(all.size()))
    {
        const int bottom = all.empty() ? 0 : all.back().top + all.back().height;
        return {0, topHeight() + bottom - scrollY_, getWidth() - kScrollbarThickness, kTrackHeight};
    }
    const auto& r = all[static_cast<std::size_t>(row)];
    return {0, topHeight() + r.top - scrollY_, getWidth() - kScrollbarThickness, r.height};
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
    // A clip in a group stands for its whole (outermost) group.
    const auto idsOf = [](const std::vector<juce::ValueTree>& clips)
    {
        std::set<std::uint32_t> result;
        for (const auto& c : clips)
            result.insert(session::clip::getId(c));
        return result;
    };
    const auto whole = idsOf(session_.withWholeGroups({clip}));

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
        std::vector<juce::ValueTree> range;
        for (const auto& c : clip.getParent())
            if (session::Session::isClip(c) && session::clip::getStart(c) >= from - 1.0e-9 && session::clip::getStart(c) <= to + 1.0e-9)
                range.push_back(c);
        for (const auto rangeId : idsOf(session_.withWholeGroups(range)))
            selection.insert(rangeId);
        editor_.setSelectedClips(std::move(selection));
        return;
    }

    if (mods.isCommandDown())
    {
        auto selection = editor_.getSelectedClips();
        const bool wasSelected = editor_.isClipSelected(id);
        for (const auto member : whole)
            if (wasSelected)
                selection.erase(member);
            else
                selection.insert(member);
        editor_.setSelectedClips(std::move(selection));
    }
    else if (!editor_.isClipSelected(id)) // a selected clip keeps the selection: it may be dragged
        editor_.setSelectedClips(whole);
    selectionAnchor_ = id;
}

void ArrangerView::selectClipsInArea(juce::Rectangle<int> area, const std::set<std::uint32_t>& base)
{
    const double t0 = xToTime(static_cast<float>(area.getX()));
    const double t1 = xToTime(static_cast<float>(area.getRight()));
    const int y0 = area.getY() - topHeight() + scrollY_;
    const int y1 = area.getBottom() - topHeight() + scrollY_;

    auto selection = base;
    std::vector<juce::ValueTree> touched;
    for (const auto& row : rows())
    {
        if (row.kind != Row::Kind::track)
            continue;
        // Clips are drawn 3 px inside their row.
        if (row.top + row.height - 3 <= y0 || row.top + 3 >= y1)
            continue;
        for (const auto& c : row.track)
            if (session::Session::isClip(c) && session::clip::getEnd(c) > t0 && session::clip::getStart(c) < t1)
                touched.push_back(c);
    }
    for (const auto& c : session_.withWholeGroups(touched)) // a group is selected as a whole
        selection.insert(session::clip::getId(c));
    if (selection != editor_.getSelectedClips())
        editor_.setSelectedClips(std::move(selection));
}

juce::Rectangle<int> ArrangerView::getMarqueeArea() const
{
    if (!marqueeActive_)
        return {};
    const auto toScreen = [this](const TimelinePoint& p)
    {
        return juce::Point<int>(juce::roundToInt(timeToX(p.seconds)), p.contentY + topHeight() - scrollY_);
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
    marqueeEnd_ = {xToTime(x), y - topHeight() + scrollY_};
    selectClipsInArea(getMarqueeArea(), marqueeBase_);

    // The range dragged over is also the time selection (on the tracks it touches): Clip > Capture.
    EditorState::TimeSelection selection;
    selection.start = std::max(0.0, std::min(marqueeAnchor_.seconds, marqueeEnd_.seconds));
    selection.end = std::max(marqueeAnchor_.seconds, marqueeEnd_.seconds);
    const int y0 = std::min(marqueeAnchor_.contentY, marqueeEnd_.contentY);
    const int y1 = std::max(marqueeAnchor_.contentY, marqueeEnd_.contentY);
    for (const auto& row : rows())
        if (row.kind == Row::Kind::track && row.top + row.height > y0 && row.top < y1)
            selection.trackIds.insert(session::track::getId(row.track));
    editor_.setTimeSelection(std::move(selection));
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

juce::Rectangle<int> ArrangerView::getTakesButton(int row) const
{
    const auto& r = rows()[static_cast<std::size_t>(row)];
    if (r.kind != Row::Kind::track || session::Session::getTakeLanes(r.track).empty())
        return {};
    const auto button = getHeaderButton(row, 0);
    return {button.getX(), getRowBounds(row).getY() + 37, button.getWidth(), 16};
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
    return editor_.snap(seconds, session_.getTempoMap(), mods.isAltDown() || mods.isShiftDown());
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
        if (session::clip::isEditLocked(c) || session::clip::isGrouped(c))
            return hit; // no trims or fades on an edit-locked clip, nor inside a clip group
        const bool audio = !session::clip::isMidi(c);
        const float fadeInX = bounds.getX() + static_cast<float>(session::clip::getFadeIn(c) * pixelsPerSecond_);
        const float fadeOutX = bounds.getRight() - static_cast<float>(session::clip::getFadeOut(c) * pixelsPerSecond_);
        const float handleY = bounds.getY() + kClipTitleHeight;
        const bool nearHandleRow = std::abs(position.y - handleY) <= kFadeHandleSize;

        // Warp markers: a few pixels either side of their line, below the title.
        if (audio && position.y > bounds.getY() + kClipTitleHeight)
        {
            const auto markers = session::clip::getWarpMarkers(c);
            for (std::size_t m = 0; m < markers.size(); ++m)
                if (std::abs(position.x - timeToX(session::clip::getStart(c) + markers[m].clipTime)) <= kWarpGrab)
                {
                    hit.zone = ClipZone::warpMarker;
                    hit.warpIndex = static_cast<int>(m);
                    return hit;
                }
        }

        if (audio && nearHandleRow && std::abs(position.x - fadeInX) <= kFadeHandleSize)
            hit.zone = ClipZone::fadeInHandle;
        else if (audio && nearHandleRow && std::abs(position.x - fadeOutX) <= kFadeHandleSize)
            hit.zone = ClipZone::fadeOutHandle;
        else if (bounds.getWidth() > 3.0f * kEdgeGrabWidth && position.x - bounds.getX() <= kEdgeGrabWidth && !session::clip::isLooped(c))
            hit.zone = ClipZone::leftEdge; // (a looped clip is lengthened or shortened at its end)
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
    if (target == session::automation::azimuthTarget())
        return "3D Azimuth";
    if (target == session::automation::elevationTarget())
        return "3D Elevation";
    if (target == session::automation::distanceTarget())
        return "3D Distance";
    if (target == session::automation::sizeTarget())
        return "3D Size";

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
    if (target == session::automation::azimuthTarget() || target == session::automation::elevationTarget())
        return juce::String(session::automation::spatialFromNormalised(target, value), 1) + juce::String::fromUTF8("\xc2\xb0");
    if (target == session::automation::distanceTarget())
        return juce::String(value, 2);
    if (target == session::automation::sizeTarget())
        return juce::String(value * 100.0, 0) + " %";
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
    if (target == session::automation::azimuthTarget())
        return session::automation::spatialToNormalised(target, session::track::getAzimuth(t));
    if (target == session::automation::elevationTarget())
        return session::automation::spatialToNormalised(target, session::track::getElevation(t));
    if (target == session::automation::distanceTarget())
        return session::track::getDistance(t);
    if (target == session::automation::sizeTarget())
        return session::track::getSize(t);

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
    const int addY = topHeight() + bottom - scrollY_ + 10;
    addTrackButton_.setBounds(12, addY, theme::metrics::trackHeaderWidth - 24, 28);
    addTrackButton_.setVisible(addY >= topHeight() && addY + 28 <= getHeaderArea().getBottom());

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
    if (property == session::ids::collapsed || property == session::ids::showAutomation || property == session::ids::visible
        || property == session::ids::showTakes)
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
    paintTempoTrack(g);
    paintHeaders(g);
    paintInactiveTracks(g);
    paintDropIndicator(g);
    paintPlayhead(g);

    if (session_.getNumTracks() == 0 && !dropActive_)
        paintEmptyState(g);

    g.setColour(theme::colour::panel);
    g.fillRect(getWidth() - kScrollbarThickness, getHeight() - kScrollbarThickness, kScrollbarThickness, kScrollbarThickness);
}

void ArrangerView::paintInactiveTracks(juce::Graphics& g) const
{
    using namespace theme;
    const auto& all = rows();
    const juce::Rectangle<int> content(0, topHeight(), getWidth() - kScrollbarThickness, getLaneArea().getHeight());
    g.saveState();
    g.reduceClipRegion(content);
    for (std::size_t i = 0; i < all.size(); ++i)
    {
        if (session::Session::isEffectivelyActive(all[i].track))
            continue;
        const auto row = getRowBounds(static_cast<int>(i)).withLeft(0).withRight(content.getRight());
        if (!row.intersects(content))
            continue;
        g.setColour(colour::windowBackground.withAlpha(0.6f));
        g.fillRect(row);
        if (all[i].kind == Row::Kind::track)
        {
            const auto badge = juce::Rectangle<int>(metrics::trackHeaderWidth - 74, row.getY() + 33, 64, 16);
            g.setColour(colour::control);
            g.fillRoundedRectangle(badge.toFloat(), 3.0f);
            g.setColour(colour::textSecondary);
            g.setFont(label(9.5f));
            g.drawText("INACTIVE", badge, juce::Justification::centred, false);
        }
    }
    g.restoreState();
}

void ArrangerView::scrollToTrack(const juce::ValueTree& track)
{
    for (auto folder = session::Session::getParentFolder(track); folder.isValid(); folder = session::Session::getParentFolder(folder))
        if (session::track::isCollapsed(folder))
            session::track::setCollapsed(folder, false, nullptr);
    rowsDirty_ = true;
    const int row = rowOfTrack(track);
    if (row < 0)
        return;
    setScroll(scrollSeconds_, rows()[static_cast<std::size_t>(row)].top);
    updateScrollbars();
    repaint();
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

    // Punch in / out: red brackets at the loop edges (recording keeps only what lies between them).
    const auto punchMark = [&](float x, bool in)
    {
        const auto top = static_cast<float>(strip.getY());
        const auto height = static_cast<float>(ruler.getBottom()) - top;
        g.setColour(colour::record);
        g.fillRect(in ? x : x - 2.0f, top, 2.0f, height);
        g.fillRect(in ? x : x - 7.0f, top, 7.0f, 2.0f);
        g.fillRect(in ? x : x - 7.0f, top + height - 2.0f, 7.0f, 2.0f);
    };
    if (session_.isPunchInEnabled())
        punchMark(loopX0, true);
    if (session_.isPunchOutEnabled())
        punchMark(loopX1, false);

    // Bars and beats from the tempo map (signature changes, tempo changes and ramps).
    const auto& map = session_.getTempoMap();
    const double tStart = xToTime(static_cast<float>(ruler.getX()));
    const double tEnd = xToTime(static_cast<float>(ruler.getRight()));
    const auto firstBar = std::max<std::int64_t>(0, map.barAt(map.beatAt(std::max(0.0, tStart))));
    const auto lastBar = map.barAt(map.beatAt(std::max(0.0, tEnd))) + 1;
    // The narrowest bar on screen decides how often bars are numbered.
    double narrowest = 1.0e9;
    for (auto bar = firstBar; bar <= lastBar && bar - firstBar < 4096; ++bar)
        narrowest = std::min(narrowest, (map.secondsAt(map.beatOfBar(bar + 1)) - map.secondsAt(map.beatOfBar(bar))) * pixelsPerSecond_);
    int labelEvery = 1;
    while (static_cast<double>(labelEvery) * narrowest < 44.0 && labelEvery < (1 << 20))
        labelEvery *= 2;

    for (auto bar = firstBar; bar <= lastBar && bar - firstBar < 65536; ++bar)
    {
        const double barBeat = map.beatOfBar(bar);
        const double barStart = map.secondsAt(barBeat);
        const double barPixels = (map.secondsAt(map.beatOfBar(bar + 1)) - barStart) * pixelsPerSecond_;
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

        const auto signature = map.signatureOfBar(bar);
        const double unit = time::beatLengthInQuarterNotes(signature);
        if (barPixels / signature.numerator >= 8.0)
        {
            g.setColour(colour::outlineStrong);
            for (int beat = 1; beat < signature.numerator; ++beat)
                g.drawVerticalLine(juce::roundToInt(timeToX(map.secondsAt(barBeat + beat * unit))), static_cast<float>(ruler.getBottom() - 4),
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
        else if (all[i].kind == Row::Kind::take)
            paintTakeHeader(g, static_cast<int>(i), all[i]);
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
    g.fillRect(metrics::trackHeaderWidth - 1, topHeight(), 1, area.getHeight());
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
        else if (kind == session::TrackKind::midi)
        {
            // A 5-pin DIN socket.
            g.drawEllipse(box.reduced(0.5f), 1.3f);
            for (int k = 0; k < 3; ++k)
                g.fillEllipse(box.getX() + 2.5f + static_cast<float>(k) * 3.0f, box.getCentreY() - 1.0f, 2.0f, 2.0f);
        }
        else if (kind == session::TrackKind::video)
        {
            // A strip of film: a frame with sprocket holes.
            g.drawRect(box, 1.2f);
            for (int k = 0; k < 3; ++k)
            {
                const float y = box.getY() + 1.5f + static_cast<float>(k) * (box.getHeight() - 3.0f) / 2.0f - 0.75f;
                g.fillRect(box.getX() + 1.5f, y, 1.5f, 1.5f);
                g.fillRect(box.getRight() - 3.0f, y, 1.5f, 1.5f);
            }
        }
        else if (kind == session::TrackKind::vca || kind == session::TrackKind::masterFader)
        {
            // A fader: a slot and a cap (two caps for a VCA's group).
            g.fillRect(box.getCentreX() - 0.75f, box.getY(), 1.5f, box.getHeight());
            g.fillRoundedRectangle(box.getX(), box.getY() + box.getHeight() * 0.3f, box.getWidth(), 3.0f, 1.0f);
            if (kind == session::TrackKind::vca)
                g.fillRoundedRectangle(box.getX(), box.getY() + box.getHeight() * 0.7f, box.getWidth(), 3.0f, 1.0f);
        }
        else
            for (int k = 0; k < 4; ++k)
            {
                const float h = box.getHeight() * (k % 2 == 0 ? 0.9f : 0.5f);
                g.fillRect(box.getX() + static_cast<float>(k) * 3.0f, box.getCentreY() - h * 0.5f, 2.0f, h);
            }
    }

    if (!(renameEditor_->isVisible() && !renamingTake_ && renamingTrackId_ == session::track::getId(t)))
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

    if (kind != session::TrackKind::video) // (video has nothing to automate, nothing to solo)
        paintToggle(getHeaderButton(rowIndex, 0), "A", session::track::isShowingAutomation(t), colour::textSecondary);
    if (kind != session::TrackKind::masterFader && kind != session::TrackKind::video) // (a master fader is not soloed)
        paintToggle(getHeaderButton(rowIndex, 1), "S", session::track::isSoloed(t), colour::play);
    paintToggle(getHeaderButton(rowIndex, 2), "M", session::track::isMuted(t), colour::gold);
    if (canArm(kind))
        paintToggle(getHeaderButton(rowIndex, 3), "R", session::track::isArmed(t), colour::record);
    // T: the take lanes (once the track has some), with how many there are.
    const auto takesButton = getTakesButton(rowIndex);
    if (!takesButton.isEmpty())
    {
        const auto count = session::Session::getTakeLanes(t).size();
        const auto label = "T" + (count < 10 ? juce::String(static_cast<int>(count)) : juce::String("+"));
        paintToggle(takesButton, label.toRawUTF8(), session::track::isShowingTakes(t), colour::accent);
    }

    // Second line: routing summary.
    const auto format = session::track::getFormat(t);
    const bool mono = kind != session::TrackKind::instrument && format == session::TrackFormat::mono;
    juce::String info = kindLabel(t) + (mono ? " mono" : session::isMultichannel(format) ? " " + session::getDisplayName(format) : juce::String())
                        + "  " + formatDb(session::track::getVolumeDb(t)) + "  " + formatPan(session::track::getPan(t));
    if (kind == session::TrackKind::audio && session::track::getInput(t) >= 0)
    {
        const int first = session::track::getInput(t) + 1;
        if (session::isMultichannel(format))
            info << "  In " << first << "-" << (first + session::getNumChannels(format) - 1);
        else
            info << "  In " << first << (session::track::isInputStereo(t) ? "/" + juce::String(first + 1) : juce::String());
    }
    if (const auto dest = session_.getDestination(t); dest.isValid() && !folder && session::track::getOutput(t) != 0)
        info << "  > " << session::track::getName(dest);
    if (kind == session::TrackKind::midi)
    {
        const auto target = session_.findTrack(session::track::getMidiTarget(t));
        info = "MIDI  > " + (target.isValid() ? session::track::getName(target) : juce::String("(no instrument)"));
    }
    else if (kind == session::TrackKind::vca)
    {
        const auto members = session_.getVcaMembers(t).size();
        info = "VCA  " + formatDb(session::track::getVolumeDb(t)) + "  " + juce::String(members) + (members == 1 ? " track" : " tracks");
    }
    else if (kind == session::TrackKind::video)
    {
        // The picture format of its first clip: "Video  1920x1080  25 fps  h264".
        info = "Video";
        const auto first = t.getChildWithName(session::ids::videoClip);
        if (first.isValid() && videoFrames_ != nullptr)
            if (const auto videoInfo = videoFrames_->getInfo(session::clip::getFile(first)))
                info << "  " << videoInfo->width << "x" << videoInfo->height << "  " << juce::String(videoInfo->frameRate, 2).trimCharactersAtEnd("0").trimCharactersAtEnd(".")
                     << " fps  " << videoInfo->codec;
        if (session::track::isMuted(t))
            info << "  (hidden)";
    }
    else if (kind == session::TrackKind::masterFader)
    {
        const auto bus = session_.findTrack(session::track::getControlledBus(t));
        info = "Master Fader  " + formatDb(session::track::getVolumeDb(t)) + "  > " + (bus.isValid() ? session::track::getName(bus) : juce::String("Main"));
    }
    if (const auto vca = session_.findTrack(session::track::getVca(t)); vca.isValid())
        info << "  VCA " << session::track::getName(vca);
    if (const auto devices = session::Session::getDevices(t); !devices.empty())
        info << "  [" << session::device::getName(devices.front()) << (devices.size() > 1 ? " +" + juce::String(devices.size() - 1) : juce::String()) << "]";

    g.setColour(colour::textMuted);
    g.setFont(sans(11.0f));
    const int infoRight = takesButton.isEmpty() ? metrics::trackHeaderWidth - 8 : takesButton.getX() - 4;
    g.drawText(info, juce::Rectangle<int>(getNameBounds(rowIndex).getX(), getRowBounds(rowIndex).getY() + 34,
                                          infoRight - getNameBounds(rowIndex).getX(), 18),
               juce::Justification::centredLeft, true);
}

void ArrangerView::paintTakeHeader(juce::Graphics& g, int rowIndex, const Row& row) const
{
    using namespace theme;

    const auto laneId = session::takelane::getId(row.lane);
    const bool auditioned = session::track::getAuditionedTake(row.track) == laneId;
    auto bounds = getRowBounds(rowIndex).withWidth(metrics::trackHeaderWidth);
    g.setColour(colour::panel);
    g.fillRect(bounds);
    g.setColour(colour::outline.withAlpha(0.6f));
    g.fillRect(bounds.removeFromBottom(1));
    g.setColour(session::track::getColour(row.track).withAlpha(0.5f));
    g.fillRect(bounds.removeFromLeft(2));

    const int left = 26 + row.depth * kIndent;
    const auto button = getHeaderButton(rowIndex, 0);
    if (!(renameEditor_->isVisible() && renamingTake_ && renamingTrackId_ == laneId))
    {
        g.setColour(colour::textSecondary);
        g.setFont(sansBold(11.5f));
        g.drawText(session::takelane::getName(row.lane), juce::Rectangle<int>(left, getRowBounds(rowIndex).getY() + 6, button.getX() - left - 6, 16),
                   juce::Justification::centredLeft, true);
    }

    // How much of it is in the comp.
    double comped = 0.0;
    for (const auto& [from, to] : session::Session::getCompedRanges(row.lane))
        comped += to - from;
    g.setColour(colour::textMuted);
    g.setFont(sans(10.5f));
    g.drawText(auditioned ? juce::String("auditioning (plays instead of the comp)")
                          : comped > 0.0 ? juce::String(comped, 1) + " s in the comp" : juce::String("not in the comp"),
               juce::Rectangle<int>(left, getRowBounds(rowIndex).getY() + 24, metrics::trackHeaderWidth - left - 8, 14),
               juce::Justification::centredLeft, true);

    // S: audition the lane (it plays instead of the comp).
    g.setColour(auditioned ? colour::play : colour::control);
    g.fillRoundedRectangle(button.toFloat(), 3.0f);
    g.setColour(auditioned ? textOn(colour::play) : colour::textMuted);
    g.setFont(sansBold(10.0f));
    g.drawText("S", button, juce::Justification::centred, false);
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
        else if (all[i].kind == Row::Kind::take)
        {
            g.setColour(colour::windowBackground.interpolatedWith(colour::panel, 0.5f));
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

    // Grid: bars, the signature's beats, and the grid's parts of them, on the tempo map.
    {
        const auto& map = session_.getTempoMap();
        const double t0 = std::max(0.0, xToTime(static_cast<float>(lanes.getX())));
        const double t1 = xToTime(static_cast<float>(lanes.getRight()));
        const auto lastBar = map.barAt(map.beatAt(t1)) + 1;
        for (auto bar = std::max<std::int64_t>(0, map.barAt(map.beatAt(t0))); bar <= lastBar; ++bar)
        {
            const double barBeat = map.beatOfBar(bar);
            const auto signature = map.signatureOfBar(bar);
            const double unit = time::beatLengthInQuarterNotes(signature);
            const double barPixels = (map.secondsAt(map.beatOfBar(bar + 1)) - map.secondsAt(barBeat)) * pixelsPerSecond_;
            const double beatPixels = barPixels / signature.numerator;
            const double gridUnit = editor_.getGrid() == EditorState::Grid::bar ? unit * signature.numerator : unit * editor_.getGridBeats();
            const double gridPixels = beatPixels * gridUnit / unit;
            const double step = gridPixels >= 8.0 ? gridUnit : (beatPixels >= 8.0 ? unit : unit * signature.numerator);
            const int count = std::max(1, static_cast<int>(std::lround(unit * signature.numerator / step)));
            for (int k = 0; k < count; ++k)
            {
                const double beat = barBeat + k * step;
                const double inBeats = k * step / unit;
                const bool isBar = k == 0;
                const bool isBeat = std::abs(inBeats - std::round(inBeats)) < 1.0e-6;
                g.setColour(isBar ? colour::gridBar : (isBeat ? colour::gridBeat.brighter(0.15f) : colour::gridBeat));
                g.drawVerticalLine(juce::roundToInt(timeToX(map.secondsAt(beat))), static_cast<float>(lanes.getY()),
                                   static_cast<float>(lanes.getBottom()));
            }
        }
    }

    // Recording region on armed tracks (when loop recording has come round: this pass).
    if (recording_)
    {
        const bool cameRound = playheadSeconds_ < recordStart_ && session_.isLoopRecording() && session_.isLoopEnabled();
        const float x0 = timeToX(cameRound ? session_.getLoopStart() : recordStart_);
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
        if (all[i].kind == Row::Kind::take)
        {
            paintTakeLane(g, static_cast<int>(i), all[i]);
            continue;
        }

        const auto trackColour = session::track::getColour(all[i].track);
        for (const auto& c : all[i].track)
            if (session::Session::isClip(c))
                paintClip(g, c, static_cast<int>(i), trackColour);
        paintClipGroups(g, static_cast<int>(i), all[i].track);

        // A take lane auditioned: the comp is silent meanwhile.
        if (const auto lane = session_.findTakeLane(session::track::getAuditionedTake(all[i].track)); lane.isValid())
        {
            const auto row = bounds.withLeft(lanes.getX()).reduced(0, 1);
            g.setColour(colour::windowBackground.withAlpha(0.6f));
            g.fillRect(row);
            g.setColour(colour::play);
            g.setFont(sansBold(11.0f));
            g.drawText("Auditioning " + session::takelane::getName(lane) + " (S on its lane: back to the comp)", row.reduced(10, 0),
                       juce::Justification::centredLeft, true);
        }
    }

    // The time selection.
    if (const auto& selection = editor_.getTimeSelection(); !selection.isEmpty() && !marqueeActive_)
    {
        const float x0 = timeToX(selection.start);
        const float x1 = timeToX(selection.end);
        for (std::size_t i = 0; i < all.size(); ++i)
            if (all[i].kind == Row::Kind::track && selection.trackIds.count(session::track::getId(all[i].track)) > 0)
            {
                const auto row = getRowBounds(static_cast<int>(i)).toFloat();
                const juce::Rectangle<float> band(x0, row.getY() + 1.0f, std::max(1.0f, x1 - x0), row.getHeight() - 2.0f);
                g.setColour(colour::accent.withAlpha(0.16f));
                g.fillRect(band);
                g.setColour(colour::accent.withAlpha(0.7f));
                g.drawVerticalLine(juce::roundToInt(x0), band.getY(), band.getBottom());
                g.drawVerticalLine(juce::roundToInt(x1), band.getY(), band.getBottom());
            }
    }

    g.restoreState();
}

void ArrangerView::paintTakeLane(juce::Graphics& g, int rowIndex, const Row& row) const
{
    using namespace theme;

    const auto trackColour = session::track::getColour(row.track);
    const auto comped = session::Session::getCompedRanges(row.lane);
    const bool auditioned = session::track::getAuditionedTake(row.track) == session::takelane::getId(row.lane);

    for (const auto& c : row.lane)
    {
        if (!session::Session::isClip(c))
            continue;
        const auto bounds = getClipBounds(c, rowIndex);
        if (!bounds.toNearestInt().intersects(g.getClipBounds()))
            continue;

        // The take, dimmed; drawn again at full strength where it is in the comp.
        const auto paintBody = [&](juce::Colour colour, float fill)
        {
            g.setColour(colour.withAlpha(fill));
            g.fillRoundedRectangle(bounds, 3.0f);
            if (session::clip::isMidi(c))
                paintMidiNotes(g, c, bounds.reduced(2.0f, 3.0f), colour);
            else if (project_.getAssets().find(session::clip::getFile(c)) != nullptr)
                paintWaveform(g, c, bounds.reduced(0.0f, 3.0f), colour);
        };
        paintBody(trackColour.withMultipliedSaturation(0.4f).withAlpha(auditioned ? 0.8f : 0.5f), 0.08f);
        for (const auto& [from, to] : comped)
        {
            const float x0 = std::max(bounds.getX(), timeToX(from));
            const float x1 = std::min(bounds.getRight(), timeToX(to));
            if (x1 <= x0)
                continue;
            juce::Graphics::ScopedSaveState state(g);
            g.reduceClipRegion(juce::Rectangle<float>(x0, bounds.getY(), x1 - x0, bounds.getHeight()).getSmallestIntegerContainer());
            paintBody(trackColour.brighter(0.25f), 0.30f);
        }
        g.setColour(trackColour.withAlpha(auditioned ? 0.9f : 0.55f));
        g.drawRoundedRectangle(bounds.reduced(0.5f), 3.0f, auditioned ? 1.5f : 1.0f);
    }

    // The swipe being dragged.
    if (dragMode_ == DragMode::swipe && swipeRow_ == rowIndex && dragMoved_)
    {
        const auto bounds = getRowBounds(rowIndex).toFloat();
        const float x0 = timeToX(std::min(swipeAnchor_, swipeEnd_));
        const float x1 = timeToX(std::max(swipeAnchor_, swipeEnd_));
        const juce::Rectangle<float> band(x0, bounds.getY() + 2.0f, std::max(1.0f, x1 - x0), bounds.getHeight() - 4.0f);
        g.setColour(colour::accent.withAlpha(0.22f));
        g.fillRect(band);
        g.setColour(colour::accent.withAlpha(0.9f));
        g.drawRect(band, 1.0f);
    }
}

void ArrangerView::paintClipGroups(juce::Graphics& g, int row, const juce::ValueTree& track) const
{
    using namespace theme;

    // Each outermost group's extent on this row (its clips may continue on other tracks).
    struct Extent
    {
        std::uint32_t id = 0;
        juce::Rectangle<float> bounds;
        bool selected = false;
        int rating = 0; ///< the members' rating when they share one, else -1
    };
    std::vector<Extent> extents;
    for (const auto& c : track)
    {
        if (!session::Session::isClip(c))
            continue;
        const auto group = session::clip::getOuterGroup(c);
        if (group == 0)
            continue;
        const auto bounds = getClipBounds(c, row);
        const bool selected = editor_.isClipSelected(session::clip::getId(c));
        auto it = std::find_if(extents.begin(), extents.end(), [group](const Extent& x) { return x.id == group; });
        const int rating = session::clip::getRating(c);
        if (it == extents.end())
            extents.push_back({group, bounds, selected, rating});
        else
        {
            it->bounds = it->bounds.getUnion(bounds);
            it->selected = it->selected || selected;
            if (it->rating != rating)
                it->rating = -1;
        }
    }

    for (const auto& x : extents)
    {
        if (!x.bounds.toNearestInt().intersects(g.getClipBounds()))
            continue;
        const auto frame = x.bounds.expanded(1.0f, 0.0f);
        const auto edge = x.selected ? colour::textPrimary : colour::accent;

        // A title bar across the group, with a group mark and its name.
        auto title = frame.withHeight(kClipTitleHeight);
        juce::Path bar;
        bar.addRoundedRectangle(title.getX(), title.getY(), title.getWidth(), title.getHeight(), 4.0f, 4.0f, true, true, false, false);
        g.setColour(colour::accent.darker(0.45f));
        g.fillPath(bar);
        auto text = title.reduced(5.0f, 0.0f);
        // (the group bar covers its clips' title bars: their shared rating shows on it)
        if (x.rating > 0 && editor_.isShowingClipRatings() && text.getWidth() > 40.0f + static_cast<float>(x.rating) * 10.0f)
            drawStars(g, text.removeFromRight(static_cast<float>(x.rating) * 10.0f + 2.0f), x.rating, colour::textPrimary);
        if (text.getWidth() > 14.0f)
        {
            const auto mark = text.removeFromLeft(10.0f).withSizeKeepingCentre(9.0f, 8.0f);
            g.setColour(colour::textPrimary);
            g.drawRect(mark.translated(1.5f, -1.5f), 1.0f);
            g.fillRect(mark.withTrimmedRight(3.0f).withTrimmedTop(3.0f));
            text.removeFromLeft(4.0f);
        }
        g.setFont(sansBold(11.0f));
        g.setColour(colour::textPrimary);
        g.drawText(session_.getGroupName(x.id), text, juce::Justification::centredLeft, true);

        g.setColour(edge);
        g.drawRoundedRectangle(frame.reduced(0.5f), 4.0f, x.selected ? 2.0f : 1.4f);
    }
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

void ArrangerView::paintVideoFrames(juce::Graphics& g, const juce::ValueTree& c, juce::Rectangle<float> body, juce::Colour colour) const
{
    using namespace theme;
    const auto file = session::clip::getFile(c);
    const auto message = [&](const juce::String& text, juce::Colour textColour)
    {
        g.setColour(textColour);
        g.setFont(sans(11.0f));
        g.drawText(text, body.reduced(6.0f, 2.0f), juce::Justification::centredLeft, true);
    };
    if (!file.existsAsFile())
        return message("Missing: " + file.getFileName(), colour::meterClip);
    if (videoFrames_ == nullptr)
        return message(file.getFileName(), colour::textMuted);

    juce::String error;
    const auto info = videoFrames_->getInfo(file, &error);
    if (!info.has_value())
        return message(error, colour::meterClip);

    // Frames side by side from the clip start, each showing the moment at its left edge.
    const int height = juce::roundToInt(body.getHeight()) - 2;
    if (height < 8 || info->width <= 0 || info->height <= 0)
        return;
    const float tileWidth = static_cast<float>(height) * static_cast<float>(info->width) / static_cast<float>(info->height);
    const auto visible = body.getIntersection(g.getClipBounds().toFloat());
    if (visible.isEmpty())
        return;

    g.saveState();
    g.reduceClipRegion(body.toNearestInt());
    const double offset = session::clip::getOffset(c);
    for (float x = body.getX() + std::floor((visible.getX() - body.getX()) / tileWidth) * tileWidth; x < visible.getRight(); x += tileWidth)
    {
        const auto tile = juce::Rectangle<float>(x, body.getY() + 1.0f, tileWidth, static_cast<float>(height));
        double inClip = static_cast<double>(x - body.getX()) / pixelsPerSecond_;
        if (session::clip::isLooped(c))
            inClip = std::fmod(inClip, session::clip::getLoopLength(c));
        const auto image = videoFrames_->getThumbnail(file, offset + inClip, height);
        if (image.isValid() && image.getWidth() > 1)
            g.drawImage(image, tile.reduced(0.5f, 0.0f), juce::RectanglePlacement::stretchToFit);
        else
        {
            g.setColour(colour.withAlpha(0.12f));
            g.fillRect(tile.reduced(1.0f));
        }
    }
    g.restoreState();
}

bool ArrangerView::isOffline(const juce::ValueTree& c) const
{
    if (session::clip::isMidi(c))
        return false;
    // (checked at most every two seconds per file: painting must stay cheap)
    const auto path = session::clip::getFile(c).getFullPathName();
    const auto now = juce::Time::getMillisecondCounter();
    auto& entry = fileExists_[path];
    if (entry.second == 0 || now - entry.second > 2000)
        entry = {juce::File(path).existsAsFile(), now == 0 ? 1u : now};
    return !entry.first;
}

void ArrangerView::paintClip(juce::Graphics& g, const juce::ValueTree& c, int row, juce::Colour trackColour) const
{
    using namespace theme;

    const auto bounds = getClipBounds(c, row);
    if (!bounds.toNearestInt().intersects(g.getClipBounds()))
        return;

    const bool selected = editor_.isClipSelected(session::clip::getId(c));
    const bool midi = session::clip::isMidi(c);
    // Offline clips (their file cannot be found): light blue, the name in italics.
    const bool offline = isOffline(c);
    const auto colour = offline ? juce::Colour(0xff86b9e8) : trackColour;

    g.setColour(colour.withAlpha(selected ? 0.36f : 0.22f));
    g.fillRoundedRectangle(bounds, 4.0f);

    const auto title = bounds.withHeight(kClipTitleHeight);
    g.setColour(colour.withAlpha(selected ? 1.0f : 0.85f));
    g.fillRoundedRectangle(title, 4.0f);
    g.fillRect(title.withTrimmedTop(6.0f));

    juce::String name = session::clip::getName(c);
    if (!midi && c.hasType(session::ids::clip))
    {
        if (std::abs(session::clip::getStretch(c) - 1.0) > 1.0e-6)
            name << "  x" << juce::String(session::clip::getStretch(c), 2);
        if (std::abs(session::clip::getPitch(c)) > 1.0e-6)
            name << "  " << (session::clip::getPitch(c) > 0 ? "+" : "") << juce::String(session::clip::getPitch(c), 1) << " st";
        if (const double gainDb = session::clip::getGainDb(c); std::abs(gainDb) > 1.0e-6)
            name << "  " << (gainDb > 0 ? "+" : "") << juce::String(gainDb, 1) << " dB" << (session::clip::isGainBypassed(c) ? " (bypassed)" : "");
    }
    // A comp clip: the take it came from.
    if (const auto lane = session_.getTakeLaneOf(c); lane.isValid())
        name << "  [" << session::takelane::getName(lane) << "]";
    // Badges on the right of the title bar: locks, loop, rating.
    auto titleText = title.reduced(6.0f, 0.0f);
    const bool editLocked = session::clip::isEditLocked(c);
    const bool grouped = session::clip::isGrouped(c);
    if (session::clip::isTimeLocked(c) && titleText.getWidth() > 30.0f)
        drawClockBadge(g, titleText.removeFromRight(12.0f), textOn(colour));
    if (editLocked && titleText.getWidth() > 30.0f)
        drawPadlock(g, titleText.removeFromRight(12.0f), textOn(colour));
    if (session::clip::isLooped(c) && titleText.getWidth() > 30.0f)
        drawLoopBadge(g, titleText.removeFromRight(12.0f), textOn(colour));
    if (const int rating = session::clip::getRating(c); rating > 0 && editor_.isShowingClipRatings() && titleText.getWidth() > 30.0f + static_cast<float>(rating) * 10.0f)
        drawStars(g, titleText.removeFromRight(static_cast<float>(rating) * 10.0f + 2.0f), rating, textOn(colour));
    g.setColour(textOn(colour));
    g.setFont(offline ? sansBold(10.5f).italicised() : sansBold(10.5f));
    g.drawText(offline ? name + "  (offline)" : name, titleText, juce::Justification::centredLeft, true);

    const auto body = bounds.withTrimmedTop(kClipTitleHeight);
    const bool video = session::clip::isVideo(c);

    if (video)
    {
        paintVideoFrames(g, c, body, colour);
    }
    else if (midi)
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

    if (!midi && !video)
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

        if (bounds.getWidth() > 4.0f * kFadeHandleSize && !editLocked && !grouped)
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

    // A looped clip: a notch where each iteration starts.
    if (session::clip::isLooped(c))
    {
        const double loop = session::clip::getLoopLength(c);
        g.setColour(colour::textPrimary.withAlpha(0.5f));
        for (double t = session::clip::getStart(c) + loop; t < session::clip::getEnd(c) - 1.0e-9; t += loop)
        {
            const float x = timeToX(t);
            g.drawLine(x, body.getY(), x, body.getBottom(), 1.0f);
            juce::Path notch;
            notch.addTriangle(x - 3.0f, body.getY(), x + 3.0f, body.getY(), x, body.getY() + 4.0f);
            g.fillPath(notch);
        }
    }

    // Hitpoints (on a selected clip): short ticks along its bottom, where each attack plays.
    if (!session::clip::isMidi(c))
    {
        const double clipStart = session::clip::getStart(c);
        const double clipLength = session::clip::getLength(c);
        if (selected)
        {
            g.setColour(colour::textPrimary.withAlpha(0.55f));
            for (const double h : session::clip::getHitpoints(c))
                if (const double t = session::clip::clipTimeOf(c, h); t >= 0.0 && t <= clipLength)
                {
                    const float x = timeToX(clipStart + t);
                    g.drawLine(x, bounds.getBottom() - kHitpointTickHeight, x, bounds.getBottom() - 1.0f, 1.0f);
                }
        }
        // Warp markers: a gold line through the clip with a handle under its title.
        for (const auto& marker : session::clip::getWarpMarkers(c))
        {
            if (marker.clipTime > clipLength)
                continue;
            const float x = timeToX(clipStart + marker.clipTime);
            g.setColour(colour::gold.withAlpha(0.75f));
            g.drawLine(x, body.getY(), x, bounds.getBottom() - 1.0f, 1.0f);
            juce::Path handle;
            handle.addTriangle(x - 4.0f, body.getY(), x + 4.0f, body.getY(), x, body.getY() + 6.0f);
            g.setColour(colour::gold);
            g.fillPath(handle);
        }
    }

    // The sync point: a light grey line and a small down arrow at the bottom of the clip.
    if (const auto sync = session::clip::getSyncPoint(c))
    {
        const float x = timeToX(session::clip::getStart(c) + *sync);
        g.setColour(juce::Colour(0xffd0d0d0).withAlpha(0.8f));
        g.drawLine(x, body.getY(), x, bounds.getBottom() - 6.0f, 1.0f);
        juce::Path arrow;
        arrow.addTriangle(x - 4.0f, bounds.getBottom() - 7.0f, x + 4.0f, bounds.getBottom() - 7.0f, x, bounds.getBottom() - 1.5f);
        g.fillPath(arrow);
        // Its SMPTE location, on a selected clip with room for it.
        if (selected && bounds.getRight() - x > 90.0f && bounds.getHeight() > 34.0f)
        {
            g.setFont(mono(10.0f));
            g.drawText("SYNC " + formatTimecode(session::clip::getReferenceTime(c), kSmpteFrameRate),
                       juce::Rectangle<float>(x + 6.0f, bounds.getBottom() - 14.0f, 100.0f, 12.0f), juce::Justification::centredLeft, false);
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
    const auto markers = session::clip::getWarpMarkers(c);
    const auto sourceAt = [&](double clipTime) { return warp::sourceAt(markers.data(), markers.size(), offset, stretch, clipTime); };
    const double bucketsPerSecond = asset->sampleRate / session::WaveformPeaks::kFramesPerBucket;
    // A looped clip shows its first loop-length again in every iteration.
    const bool looped = session::clip::isLooped(c);
    const double loop = session::clip::getLoopLength(c);
    const auto inClip = [&](float x)
    {
        const double t = xToTime(x) - start;
        return looped && t >= 0.0 ? std::fmod(t, loop) : t;
    };

    // Stereo files show their two channels (left above right), mono files one; a clip that plays one
    // channel of its file (a track split into mono) shows that channel.
    const int chosen = session::clip::getSourceChannel(c);
    const auto firstX = static_cast<int>(std::floor(visible.getX()));
    const auto lastX = static_cast<int>(std::ceil(visible.getRight()));
    waveform::paintChannels(g, area, chosen >= 0 ? 1 : peaks.numChannels, firstX, lastX, colour.withAlpha(0.85f),
                            [&](int lane, int x)
                            {
                                const int channel = chosen >= 0 ? std::min(chosen, peaks.numChannels - 1) : lane;
                                const double t0 = inClip(static_cast<float>(x));
                                const double sourceT0 = sourceAt(t0);
                                const double sourceT1 = sourceAt(std::max(t0, inClip(static_cast<float>(x + 1))));
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
    const auto& map = session_.getTempoMap(); // (notes follow the tempo map)
    const double clipStart = session::clip::getStart(c);
    const double contentStart = session::clip::getOffset(c);
    const double clipBeat = map.beatAt(clipStart);

    // A looped clip repeats its first loop of notes in every iteration (a loop is so many beats).
    const double clipEnd = session::clip::getEnd(c);
    const double loopBeats = map.beatAt(clipStart + session::clip::getLoopLength(c)) - clipBeat;
    const int iterations = session::clip::isLooped(c)
                               ? std::max(1, static_cast<int>(std::ceil((map.beatAt(clipEnd) - clipBeat) / loopBeats - 1.0e-9)))
                               : 1;
    g.setColour(colour);
    for (int k = 0; k < iterations; ++k)
    {
        const double baseBeat = clipBeat + k * loopBeats;
        const double windowEnd = iterations > 1 ? std::min(map.secondsAt(baseBeat + loopBeats), clipEnd) : clipEnd;
        for (const auto& n : c)
        {
            if (!n.hasType(session::ids::note))
                continue;
            const double startTime = map.secondsAt(baseBeat + session::note::getStart(n) - contentStart);
            const double endTime = std::min(windowEnd, map.secondsAt(baseBeat + session::note::getEnd(n) - contentStart));
            if (startTime >= windowEnd)
                continue;
            const float x0 = std::max(area.getX(), timeToX(startTime));
            const float x1 = std::min(area.getRight(), timeToX(endTime));
            if (x1 <= x0)
                continue;
            const float y = area.getBottom() - static_cast<float>(session::note::getNote(n) - lowest + 1) * noteHeight;
            g.fillRect(x0, y, std::max(1.0f, x1 - x0), std::max(1.0f, noteHeight - 0.5f));
        }
    }
}

void ArrangerView::paintDropIndicator(juce::Graphics& g) const
{
    if (!dropActive_)
        return;

    const auto lanes = getLaneArea();
    g.saveState();
    g.reduceClipRegion(getLocalBounds().withTop(topHeight()));

    if (dropRow_ >= 0)
    {
        g.setColour(theme::colour::accent.withAlpha(0.12f));
        g.fillRect(getRowBounds(dropRow_).withRight(lanes.getRight()));
    }
    else if (!dropIsDevice_)
    {
        const auto& all = rows();
        const int bottom = all.empty() ? 0 : all.back().top + all.back().height;
        const int top = topHeight() + bottom - scrollY_;
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

    if (getTempoTrackArea().contains(position))
    {
        mouseDownOnTempoTrack(e);
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
    if (row >= 0 && rows()[static_cast<std::size_t>(row)].kind == Row::Kind::take)
    {
        mouseDownOnTake(e, row);
        return;
    }

    const auto hit = hitTestClip(e.position);
    if (!e.mods.isPopupMenu())
        editor_.clearTimeSelection(); // a click starts a new selection
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
    marqueeAnchor_ = {seconds, juce::roundToInt(e.position.y) - topHeight() + scrollY_};
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

    if (row.kind == Row::Kind::take)
    {
        editor_.selectTrack(trackId);
        if (e.mods.isPopupMenu())
        {
            showTakeMenu(row.lane);
            return;
        }
        if (getHeaderButton(rowIndex, 0).contains(position))
        {
            // S: the lane plays instead of the comp (again: the comp plays).
            const auto laneId = session::takelane::getId(row.lane);
            session::track::setAuditionedTake(t, session::track::getAuditionedTake(t) == laneId ? 0u : laneId, nullptr);
        }
        return;
    }

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
    if (const auto takesButton = getTakesButton(rowIndex); takesButton.contains(position))
    {
        session::track::setShowingTakes(t, !session::track::isShowingTakes(t), nullptr);
        return;
    }

    // An inactive track's solo, mute and arm do nothing (it does not play).
    if (!session::Session::isEffectivelyActive(t))
        for (const int button : {1, 2, 3})
            if (getHeaderButton(rowIndex, button).contains(position))
                return;

    // Solo, mute and arm switch every selected track when the clicked one is part of the selection.
    const auto targets = inSelection ? selectedTracksInOrder(false) : std::vector<juce::ValueTree>{t};
    const auto kind = session::track::getKind(t);
    if (kind != session::TrackKind::video && getHeaderButton(rowIndex, 0).contains(position))
    {
        if (session::Session::getAutomationLanes(t).empty())
            session_.getAutomation(t, session::automation::volumeTarget(), true, currentValueFor(t, session::automation::volumeTarget()));
        session::track::setShowingAutomation(t, !session::track::isShowingAutomation(t), nullptr);
        for (auto lane : session::Session::getAutomationLanes(t))
            session::automation::setVisible(lane, true, nullptr);
        return;
    }
    if (kind != session::TrackKind::masterFader && kind != session::TrackKind::video && getHeaderButton(rowIndex, 1).contains(position))
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
    if (canArm(kind) && getHeaderButton(rowIndex, 3).contains(position))
    {
        session_.beginTransaction("Arm Track");
        const bool on = !session::track::isArmed(t);
        for (auto target : targets)
            if (canArm(session::track::getKind(target)))
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
    case ClipZone::warpMarker:    dragMode_ = DragMode::warpMarker; break;
    }
    dragWarpIndex_ = hit.warpIndex;

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
        s.warp = session::clip::getWarpMarkers(c);
        s.midi = session::clip::isMidi(c);
        s.looped = session::clip::isLooped(c);
        s.sync = session::clip::getSyncPoint(c);
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

    case DragMode::tempoPoint:
    case DragMode::signatureMarker:
        dragTempoTrack(e);
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

    case DragMode::swipe:
        if (!dragMoved_ && std::abs(e.position.x - dragStart_.x) < 3.0f)
            return;
        dragMoved_ = true;
        swipeEnd_ = snap(std::max(0.0, xToTime(e.position.x)), e.mods);
        repaint();
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
    case DragMode::warpMarker:
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
    case DragMode::warpMarker:
    case DragMode::swipe:
    case DragMode::tempoPoint:
    case DragMode::signatureMarker:
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
        case DragMode::warpMarker: name = "Move Warp Marker"; break;
        case DragMode::none:
        case DragMode::locate:
        case DragMode::loopCreate:
        case DragMode::loopMove:
        case DragMode::loopStart:
        case DragMode::loopEnd:
        case DragMode::reorderTrack:
        case DragMode::marquee:
        case DragMode::automationPoint:
        case DragMode::swipe:
        case DragMode::tempoPoint:
        case DragMode::signatureMarker: break;
        }
        session_.beginTransaction(name);
    }

    auto* um = &session_.getUndoManager();
    const double deltaSeconds = static_cast<double>(e.position.x - dragStart_.x) / pixelsPerSecond_;
    const auto& anchor = dragOriginals_.front();
    auto anchorClip = session_.findClip(anchor.id);
    if (!anchorClip.isValid() || (dragMode_ != DragMode::moveClips && !session::clip::canEdit(anchorClip)))
        return;
    const auto& tempoMap = session_.getTempoMap();

    switch (dragMode_)
    {
    case DragMode::moveClips:
    {
        // The anchor's sync point (else its start) is what snaps to the grid.
        const double reference = anchor.start + anchor.sync.value_or(0.0);
        double delta = snap(reference + deltaSeconds, e.mods) - reference;
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
            session::clip::setOffset(anchorClip, anchor.offset + tempoMap.beatAt(newStart) - tempoMap.beatAt(anchor.start), um);
        else
        {
            // The audio stays where it is: the offset follows the (warped) map, and the warp markers
            // keep their places (the old start becomes one when the clip grows to the left).
            const double delta = newStart - anchor.start;
            session::clip::setOffset(anchorClip, warp::sourceAt(anchor.warp.data(), anchor.warp.size(), anchor.offset, anchor.stretch, delta), um);
            if (!anchor.warp.empty())
            {
                std::vector<WarpPoint> markers;
                if (delta < 0.0)
                    markers.push_back({-delta, anchor.offset});
                for (const auto& marker : anchor.warp)
                    if (marker.clipTime > delta)
                        markers.push_back({marker.clipTime - delta, marker.source});
                session::clip::setWarpMarkers(anchorClip, markers, um);
            }
            session::clip::setFadeIn(anchorClip, juce::jmin(anchor.fadeIn, end - newStart), um);
        }
        if (anchor.sync.has_value()) // the sync point stays with the material
        {
            const double sync = *anchor.sync - (newStart - anchor.start);
            session::clip::setSyncPoint(anchorClip, sync >= 0.0 ? std::optional<double>(sync) : std::nullopt, um);
        }
        break;
    }

    case DragMode::trimEnd:
    {
        // (a looped clip repeats for as long as it is drawn out)
        const double latest = anchor.midi || anchor.looped
                                  ? 1.0e9
                                  : anchor.start + juce::jmax(session::kMinClipLength, warp::clipTimeOf(anchor.warp.data(), anchor.warp.size(), anchor.offset,
                                                                                                         anchor.stretch, dragSourceLength_));
        const double newEnd = juce::jlimit(anchor.start + session::kMinClipLength, latest, snap(anchor.start + anchor.length + deltaSeconds, e.mods));
        session::clip::setLength(anchorClip, newEnd - anchor.start, um);
        if (anchor.sync.has_value())
            session::clip::setSyncPoint(anchorClip, *anchor.sync <= newEnd - anchor.start ? anchor.sync : std::nullopt, um);
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

    case DragMode::warpMarker:
    {
        // A warp marker moves its moment of the source to another time in the clip; the audio on
        // either side stretches to fit (never faster than warp::kMaxRate, never past its neighbours).
        if (dragWarpIndex_ < 0 || dragWarpIndex_ >= static_cast<int>(anchor.warp.size()))
            break;
        auto markers = anchor.warp;
        const auto index = static_cast<std::size_t>(dragWarpIndex_);
        const auto& marker = markers[index];
        const double previousTime = index > 0 ? markers[index - 1].clipTime : 0.0;
        const double previousSource = index > 0 ? markers[index - 1].source : anchor.offset;
        double lowest = std::max(previousTime + 0.002, previousTime + (marker.source - previousSource) / warp::kMaxRate + 0.0005);
        double highest = anchor.length - 0.002;
        if (index + 1 < markers.size())
        {
            const auto& next = markers[index + 1];
            highest = std::min(next.clipTime - 0.002, next.clipTime - (next.source - marker.source) / warp::kMaxRate - 0.0005);
        }
        if (highest < lowest)
            break;
        const double wanted = snap(anchor.start + marker.clipTime + deltaSeconds, e.mods) - anchor.start;
        markers[index].clipTime = juce::jlimit(lowest, highest, wanted);
        session::clip::setWarpMarkers(anchorClip, markers, um);
        break;
    }

    case DragMode::none:
    case DragMode::locate:
    case DragMode::loopCreate:
    case DragMode::loopMove:
    case DragMode::loopStart:
    case DragMode::loopEnd:
    case DragMode::reorderTrack:
    case DragMode::marquee:
    case DragMode::automationPoint:
    case DragMode::swipe:
    case DragMode::tempoPoint:
    case DragMode::signatureMarker:
        break;
    }
}

void ArrangerView::mouseDownOnTake(const juce::MouseEvent& e, int rowIndex)
{
    const auto& row = rows()[static_cast<std::size_t>(rowIndex)];
    editor_.selectTrack(session::track::getId(row.track));
    if (e.mods.isPopupMenu())
    {
        showTakeMenu(row.lane);
        return;
    }

    // A swipe across the lane comps what it covers; a click comps the take under the mouse.
    dragMode_ = DragMode::swipe;
    swipeLane_ = row.lane;
    swipeRow_ = rowIndex;
    swipeClick_ = std::max(0.0, xToTime(e.position.x));
    swipeAnchor_ = swipeEnd_ = snap(swipeClick_, e.mods);
}

void ArrangerView::mouseUp(const juce::MouseEvent& e)
{
    if (dragMode_ == DragMode::tempoPoint || dragMode_ == DragMode::signatureMarker)
    {
        dragMode_ = DragMode::none;
        dragTempoIndex_ = -1;
        dragTempoChange_ = {};
        dragSignature_ = {};
        tempoDragMap_.reset();
        repaint();
        return;
    }

    if (dragMode_ == DragMode::swipe && swipeLane_.isValid() && swipeLane_.getParent().isValid())
    {
        double from = std::min(swipeAnchor_, swipeEnd_);
        double to = std::max(swipeAnchor_, swipeEnd_);
        if (!dragMoved_ || to - from < session::kMinClipLength)
        {
            from = to = 0.0;
            for (const auto& c : session::takelane::getClips(swipeLane_))
                if (session::clip::getStart(c) <= swipeClick_ && swipeClick_ < session::clip::getEnd(c))
                {
                    from = session::clip::getStart(c);
                    to = session::clip::getEnd(c);
                }
        }
        if (to - from >= session::kMinClipLength)
        {
            session_.beginTransaction("Comp Take");
            session_.compTake(swipeLane_, from, to);
        }
    }
    swipeLane_ = {};
    swipeRow_ = -1;

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

    // A plain click on a clip that was part of a multi-selection selects just that clip (and its group).
    if (dragMode_ == DragMode::moveClips && !dragMoved_ && !e.mods.isAnyModifierKeyDown())
    {
        std::set<std::uint32_t> selection{dragAnchorId_};
        if (const auto anchor = session_.findClip(dragAnchorId_); anchor.isValid())
            for (const auto& c : session_.withWholeGroups({anchor}))
                selection.insert(session::clip::getId(c));
        editor_.setSelectedClips(std::move(selection));
    }
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

    if (getTempoTrackArea().contains(position))
    {
        if (signatureChangeAt(position).isValid())
            newCursor = juce::MouseCursor::LeftRightResizeCursor;
        else if (const int index = tempoPointAt(e.position); index == 0)
            newCursor = juce::MouseCursor::UpDownResizeCursor;
        else if (index > 0)
            newCursor = juce::MouseCursor::DraggingHandCursor;
        setMouseCursor(newCursor);
        return;
    }

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
    else if (const int takeRow = rowAt(position.y); takeRow >= 0 && getLaneArea().contains(position) &&
                                                    rows()[static_cast<std::size_t>(takeRow)].kind == Row::Kind::take)
    {
        newCursor = juce::MouseCursor::IBeamCursor; // (swipe to comp)
    }
    else if (const auto hit = hitTestClip(e.position); hit.clip.isValid())
    {
        switch (hit.zone)
        {
        case ClipZone::leftEdge: newCursor = juce::MouseCursor::LeftEdgeResizeCursor; break;
        case ClipZone::rightEdge: newCursor = juce::MouseCursor::RightEdgeResizeCursor; break;
        case ClipZone::fadeInHandle:
        case ClipZone::fadeOutHandle:
        case ClipZone::warpMarker: newCursor = juce::MouseCursor::LeftRightResizeCursor; break;
        case ClipZone::body:
            newCursor = session::clip::isEditLocked(hit.clip)   ? juce::MouseCursor::NormalCursor
                        : session::clip::isTimeLocked(hit.clip) ? juce::MouseCursor::UpDownResizeCursor
                                                                : juce::MouseCursor::DraggingHandCursor;
            break;
        }
    }

    setMouseCursor(newCursor);
}

void ArrangerView::mouseDoubleClick(const juce::MouseEvent& e)
{
    const auto position = e.getPosition();
    const int row = rowAt(position.y);

    // The tempo track: a time signature from the bar (signature strip), a tempo change (curve), or
    // a point's exact tempo.
    if (getSignatureStripArea().contains(position))
    {
        const auto& map = session_.getTempoMap();
        const int bar = static_cast<int>(map.barAt(map.beatAt(std::max(0.0, xToTime(e.position.x))))) + 1;
        buildSignatureMenu(bar).showMenuAsync(juce::PopupMenu::Options().withMousePosition());
        return;
    }
    if (getTempoCurveArea().expanded(0, 7).contains(position))
    {
        if (const int index = tempoPointAt(e.position); index >= 0)
        {
            showTempoValueEditor(index);
            return;
        }
        const auto& map = session_.getTempoMap();
        const double beat = map.beatAt(editor_.snap(std::max(0.0, xToTime(e.position.x)), map, e.mods.isAltDown()));
        if (beat > 0.0)
        {
            session_.beginTransaction("Add Tempo Change");
            session_.addTempoChange(beat, std::round(yToTempo(e.position.y) * 100.0) / 100.0);
        }
        return;
    }

    if (getLoopStrip().contains(position))
    {
        session_.beginTransaction("Toggle Loop");
        session_.setLoopEnabled(!session_.isLoopEnabled());
        return;
    }

    if (getHeaderArea().contains(position) && row >= 0 && rows()[static_cast<std::size_t>(row)].kind != Row::Kind::automation &&
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
    if (row >= 0 && rows()[static_cast<std::size_t>(row)].kind == Row::Kind::take)
        return; // (its clicks comp)

    if (const auto hit = hitTestClip(e.position); hit.clip.isValid())
    {
        auto c = hit.clip;
        const bool audio = !session::clip::isMidi(c);
        // On a warp marker: it goes. On a hitpoint (or anywhere with Shift): a warp marker there.
        if (hit.zone == ClipZone::warpMarker && session::clip::canEdit(c))
        {
            auto markers = session::clip::getWarpMarkers(c);
            markers.erase(markers.begin() + hit.warpIndex);
            session_.beginTransaction("Delete Warp Marker");
            session::clip::setWarpMarkers(c, markers, &session_.getUndoManager());
            return;
        }
        if (audio && session::clip::canEdit(c) && !session::clip::isLooped(c))
        {
            const double start = session::clip::getStart(c);
            std::optional<double> at;
            if (const auto bounds = getClipBounds(c, hit.row); e.position.y > bounds.getBottom() - kHitpointTickHeight - 4.0f)
                for (const double h : session::clip::getHitpoints(c))
                    if (const double t = session::clip::clipTimeOf(c, h); std::abs(e.position.x - timeToX(start + t)) <= kWarpGrab)
                        at = t;
            if (!at.has_value() && e.mods.isShiftDown())
                at = xToTime(e.position.x) - start;
            if (at.has_value() && *at > 0.0 && *at < session::clip::getLength(c))
            {
                auto markers = session::clip::getWarpMarkers(c);
                markers.push_back({*at, session::clip::sourceAt(c, *at)});
                session_.beginTransaction("Add Warp Marker");
                session::clip::setWarpMarkers(c, markers, &session_.getUndoManager());
                return;
            }
        }
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
    if (session::holdsMidi(session::track::getKind(t)))
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
    // A bar long (the bar it starts in, on the tempo map).
    const auto& map = session_.getTempoMap();
    const double beat = map.beatAt(seconds);
    const double barSeconds = map.secondsAt(beat + map.barLength(map.barAt(beat))) - seconds;
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
    const auto& r = rows()[static_cast<std::size_t>(row)];
    renamingTake_ = r.kind == Row::Kind::take;
    renamingTrackId_ = renamingTake_ ? session::takelane::getId(r.lane) : session::track::getId(r.track);
    renameEditor_->setBounds(renamingTake_ ? juce::Rectangle<int>(getNameBounds(row).getX(), getRowBounds(row).getY() + 4,
                                                                  getHeaderButton(row, 0).getX() - getNameBounds(row).getX() - 6, 20)
                                           : getNameBounds(row).expanded(4, 2));
    renameEditor_->setText(renamingTake_ ? session::takelane::getName(r.lane) : session::track::getName(r.track), false);
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

    if (renamingTake_)
    {
        renamingTake_ = false;
        if (auto lane = session_.findTakeLane(trackId); commit && lane.isValid())
        {
            const auto newName = renameEditor_->getText().trim();
            if (newName.isNotEmpty() && newName != session::takelane::getName(lane))
            {
                session_.beginTransaction("Rename Take Lane");
                session::takelane::setName(lane, newName, &session_.getUndoManager());
            }
        }
        repaint();
        return;
    }

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
    const bool editable = session::clip::canEdit(clipTree);

    juce::PopupMenu menu;
    menu.addSectionHeader(session::clip::getName(clipTree));
    menu.addItem(midi ? "Edit Notes..." : "Clip Properties...", [safe, clipId]
                 {
                     if (safe != nullptr && safe->onOpenClip)
                         safe->onOpenClip(clipId);
                 });
    menu.addItem("Split at Playhead", playheadInside && editable, false, [safe, playhead]
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
        menu.addItem("Quantise to Grid", editable, false, [safe, clipId]
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
        menu.addItem("Remove Fades", editable, false, [safe, clipId]
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
        menu.addItem("Reset Stretch and Pitch", editable, false, [safe, clipId]
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
    const auto command = [safe](int id)
    {
        return [safe, id]
        {
            if (safe != nullptr && safe->onCommand)
                safe->onCommand(id);
        };
    };
    {
        const int rating = actions::getSelectedClipsRating(session_, editor_);
        juce::PopupMenu ratings;
        ratings.addItem("None", true, rating == 0, command(commands::clipRatingNone));
        for (int r = 1; r <= 5; ++r)
            ratings.addItem(juce::String(r), true, rating == r, command(commands::clipRatingNone + r));
        menu.addSubMenu("Rating", ratings);
    }
    const bool hasGroups = actions::selectionHasGroups(session_, editor_);
    menu.addItem("Group", command(commands::groupClips));
    menu.addItem("Ungroup", hasGroups, false, command(commands::ungroupClips));
    menu.addItem("Ungroup All", hasGroups, false, command(commands::ungroupAllClips));
    menu.addItem("Regroup", !editor_.getLastUngroup().empty(), false, command(commands::regroupClips));
    menu.addSeparator();
    menu.addItem("Loop...", editable, false, command(commands::loopClips));
    menu.addItem("Unloop", actions::selectionHasLoopedClips(session_, editor_), false, command(commands::unloopClips));
    menu.addSeparator();
    menu.addItem("Capture...", command(commands::captureClip));
    menu.addItem("Rename...", command(commands::renameClip));
    menu.addItem("Identify Sync Point", playheadInside && editable, false, command(commands::identifySyncPoint));
    menu.addItem("Remove Sync Point", actions::selectionHasSyncPoints(session_, editor_), false, command(commands::removeSyncPoint));
    menu.addItem("Quantize to Grid", session::clip::canMoveInTime(clipTree), false, command(commands::quantizeClipsToGrid));
    if (clipTree.hasType(session::ids::clip))
    {
        juce::PopupMenu gain;
        gain.addItem("Bypass Clip Gain", editable, session::clip::isGainBypassed(clipTree), command(commands::bypassClipGain));
        gain.addItem("Render Clip Gain", editable && std::abs(session::clip::getGainDb(clipTree)) > 1.0e-6, false, command(commands::renderClipGain));
        menu.addSubMenu("Clip Gain", gain);
    }

    menu.addSeparator();
    const bool editLocked = actions::selectedClipsLocked(session_, editor_, actions::ClipLock::edit);
    const bool timeLocked = actions::selectedClipsLocked(session_, editor_, actions::ClipLock::time);
    menu.addItem(editLocked ? "Edit Unlock" : "Edit Lock", true, editLocked, [safe]
                 {
                     if (safe != nullptr)
                         actions::toggleSelectedClipsLock(safe->session_, safe->editor_, actions::ClipLock::edit);
                 });
    menu.addItem(timeLocked ? "Time Unlock" : "Time Lock", true, timeLocked, [safe]
                 {
                     if (safe != nullptr)
                         actions::toggleSelectedClipsLock(safe->session_, safe->editor_, actions::ClipLock::time);
                 });

    menu.addSeparator();
    menu.addItem("Delete", session::clip::canRemove(clipTree), false, [safe]
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
    if (session::track::isPan3d(trackTree))
    {
        juce::PopupMenu spatial;
        spatial.addItem("Azimuth", addLane(session::automation::azimuthTarget()));
        spatial.addItem("Elevation", addLane(session::automation::elevationTarget()));
        spatial.addItem("Distance", addLane(session::automation::distanceTarget()));
        spatial.addItem("Size", addLane(session::automation::sizeTarget()));
        menu.addSubMenu("3D Panner", spatial);
    }

    for (const auto& s : session::Session::getSends(trackTree))
        if (const auto bus = session_.findTrack(session::send::getTarget(s)); bus.isValid())
            menu.addItem("Send > " + session::track::getName(bus), addLane(session::automation::sendTarget(session::send::getId(s))));

    for (const auto& d : session::Session::getAllDevices(trackTree))
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

void ArrangerView::showTakeMenu(const juce::ValueTree& lane)
{
    juce::Component::SafePointer<ArrangerView> safe(this);
    const auto laneId = session::takelane::getId(lane);
    const auto track = lane.getParent();
    const bool auditioned = session::track::getAuditionedTake(track) == laneId;
    const auto withLane = [safe, laneId](auto fn)
    {
        return [safe, laneId, fn]
        {
            if (safe == nullptr)
                return;
            if (auto tree = safe->session_.findTakeLane(laneId); tree.isValid())
                fn(*safe, tree);
        };
    };

    juce::PopupMenu menu;
    menu.addSectionHeader(session::takelane::getName(lane));
    menu.addItem("Comp Whole Take", !session::takelane::getClips(lane).empty(), false,
                 withLane([](ArrangerView& self, juce::ValueTree tree)
                          {
                              const auto [from, to] = session::takelane::getExtent(tree);
                              self.session_.beginTransaction("Comp Take");
                              self.session_.compTake(tree, from, to);
                          }));
    menu.addItem("Audition (plays instead of the comp)", true, auditioned,
                 withLane([auditioned](ArrangerView&, juce::ValueTree tree)
                          { session::track::setAuditionedTake(tree.getParent(), auditioned ? 0u : session::takelane::getId(tree), nullptr); }));
    menu.addItem("Rename", withLane([](ArrangerView& self, juce::ValueTree tree)
                                    {
                                        if (const int row = self.rowOfTakeLane(tree); row >= 0)
                                            self.startRename(row);
                                    }));
    menu.addSeparator();
    menu.addItem("Delete Take Lane", withLane([](ArrangerView& self, juce::ValueTree tree)
                                              {
                                                  self.session_.beginTransaction("Delete Take Lane");
                                                  self.session_.removeTakeLane(tree);
                                              }));
    menu.showMenuAsync(juce::PopupMenu::Options().withMousePosition());
}

juce::PopupMenu ArrangerView::buildAraMenu(const juce::ValueTree& track)
{
    juce::Component::SafePointer<ArrangerView> safe(this);
    const auto trackId = session::track::getId(track);
    const auto current = session::Session::getAraDevice(track);
    juce::String currentId;
    if (current.isValid())
        if (const auto xml = juce::parseXML(current.getProperty(session::ids::description).toString()))
        {
            juce::PluginDescription d;
            if (d.loadFromXml(*xml))
                currentId = d.createIdentifierString();
        }
    const auto choose = [safe, trackId](const juce::String& descriptionXml, const juce::String& name)
    {
        return [safe, trackId, descriptionXml, name]
        {
            if (safe == nullptr)
                return;
            auto tree = safe->session_.findTrack(trackId);
            if (!tree.isValid())
                return;
            safe->session_.beginTransaction(name.isEmpty() ? "No ARA Plug-in" : "ARA Plug-in " + name);
            safe->session_.setAraPlugin(tree, descriptionXml, name);
        };
    };

    juce::PopupMenu ara;
    ara.addItem("None", true, !current.isValid(), choose({}, {}));
    const auto plugins = getAraPlugins ? getAraPlugins() : juce::Array<juce::PluginDescription>();
    if (plugins.isEmpty())
        ara.addItem("No ARA plug-ins found (scan for plug-ins in the browser)", false, false, [] {});
    for (const auto& d : plugins)
        ara.addItem(d.name + (d.manufacturerName.isNotEmpty() ? " (" + d.manufacturerName + ")" : juce::String()), true,
                    d.createIdentifierString() == currentId, choose(d.createXml()->toString(), d.name));
    return ara;
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
    const auto kind = session::track::getKind(t);
    if (kind != session::TrackKind::midi && kind != session::TrackKind::vca && kind != session::TrackKind::masterFader)
        menu.addSubMenu("Output", output);

    // Audio tracks and busses (folders too): mono, stereo or a multichannel format.
    if (kind == session::TrackKind::audio || kind == session::TrackKind::bus)
    {
        juce::PopupMenu formats;
        const auto current = session::track::getFormat(t);
        for (const auto format : session::getAllTrackFormats())
            formats.addItem(session::getDisplayName(format), true, current == format,
                            withTracks(idsOf(selection), [format](ArrangerView& self, const std::vector<juce::ValueTree>& trees)
                                       {
                                           self.session_.beginTransaction("Track Format");
                                           for (auto tree : trees)
                                           {
                                               const auto k = session::track::getKind(tree);
                                               if (k != session::TrackKind::audio && k != session::TrackKind::bus)
                                                   continue;
                                               session::track::setFormat(tree, format, &self.session_.getUndoManager());
                                               if (k == session::TrackKind::audio)
                                                   session::track::setInput(tree, session::track::getInput(tree), format != session::TrackFormat::mono,
                                                                            &self.session_.getUndoManager());
                                           }
                                       }));
        menu.addSubMenu("Format", formats);
    }

    // The 3D panner: a mono or stereo track into a surround or 3D bus or master.
    {
        const auto destination = session_.getDestination(t);
        const auto destinationFormat = destination.isValid() ? session::track::getFormat(destination) : session_.getMasterFormat();
        const bool canPan3d = (kind == session::TrackKind::audio || kind == session::TrackKind::instrument || kind == session::TrackKind::bus)
                              && !session::isMultichannel(session::track::getFormat(t)) && session::isMultichannel(destinationFormat);
        if (canPan3d)
        {
            const bool on = session::track::isPan3d(t);
            menu.addItem("3D Panner", true, on, withTracks(idsOf(selection), [on](ArrangerView& self, const std::vector<juce::ValueTree>& trees)
                                                           {
                                                               self.session_.beginTransaction(on ? "3D Panner Off" : "3D Panner On");
                                                               for (auto tree : trees)
                                                                   session::track::setPan3d(tree, !on, &self.session_.getUndoManager());
                                                           }));
            menu.addItem("Open 3D Panner...", [safe, trackId]
                         {
                             if (safe == nullptr)
                                 return;
                             auto tree = safe->session_.findTrack(trackId);
                             if (!tree.isValid())
                                 return;
                             if (!session::track::isPan3d(tree))
                             {
                                 safe->session_.beginTransaction("3D Panner On");
                                 session::track::setPan3d(tree, true, &safe->session_.getUndoManager());
                             }
                             if (safe->onOpenSpatialPanner)
                                 safe->onOpenSpatialPanner(tree);
                         });
            // An ADM object (Dolby Atmos): its own channel(s) with its position in an ADM BWF
            // export and on the external renderer's outputs, instead of panned into the bed.
            const bool object = session::track::isObject(t);
            menu.addItem("ADM Object (Dolby Atmos)", on, object,
                         withTracks(idsOf(selection), [object](ArrangerView& self, const std::vector<juce::ValueTree>& trees)
                                    {
                                        self.session_.beginTransaction(object ? "ADM Object Off" : "ADM Object On");
                                        for (auto tree : trees)
                                            if (session::track::isPan3d(tree) || object)
                                                session::track::setObject(tree, !object, &self.session_.getUndoManager());
                                    }));
        }
    }

    // ARA (Melodyne, VocAlign...): an audio track's ARA plug-in, in its first slot, plays its clips
    // from the audio it reads itself; its editor opens in the clip editor.
    if (kind == session::TrackKind::audio && session::AraHost::isAvailable())
        menu.addSubMenu("ARA Plug-in", buildAraMenu(t));

    // MIDI tracks: the instrument track their MIDI plays.
    if (kind == session::TrackKind::midi)
    {
        juce::PopupMenu midiOut;
        const auto current = session::track::getMidiTarget(t);
        midiOut.addItem("None", true, current == 0, withTracks(idsOf(selection), [](ArrangerView& self, const std::vector<juce::ValueTree>& trees)
                                                                {
                                                                    self.session_.beginTransaction("MIDI Output");
                                                                    for (auto tree : trees)
                                                                        if (session::track::getKind(tree) == session::TrackKind::midi)
                                                                            session::track::setMidiTarget(tree, 0, &self.session_.getUndoManager());
                                                                }));
        for (const auto& candidate : session_.getAllTracks())
            if (session::track::getKind(candidate) == session::TrackKind::instrument)
            {
                const auto targetId = session::track::getId(candidate);
                midiOut.addItem(session::track::getName(candidate), true, current == targetId,
                                withTracks(idsOf(selection), [targetId](ArrangerView& self, const std::vector<juce::ValueTree>& trees)
                                           {
                                               self.session_.beginTransaction("MIDI Output");
                                               for (auto tree : trees)
                                                   if (session::track::getKind(tree) == session::TrackKind::midi)
                                                       session::track::setMidiTarget(tree, targetId, &self.session_.getUndoManager());
                                           }));
            }
        menu.addSubMenu("MIDI Output", midiOut);
    }

    // Master Faders: the path they control (one master fader per path).
    if (kind == session::TrackKind::masterFader)
    {
        juce::PopupMenu path;
        const auto current = session::track::getControlledBus(t);
        std::set<std::uint32_t> taken;
        for (const auto& other : session_.getAllTracks())
            if (other != t && session::track::getKind(other) == session::TrackKind::masterFader)
                taken.insert(session::track::getControlledBus(other));
        const auto setPath = [trackId, safe](std::uint32_t busId)
        {
            return [trackId, safe, busId]
            {
                if (safe == nullptr)
                    return;
                if (auto tree = safe->session_.findTrack(trackId); tree.isValid())
                {
                    safe->session_.beginTransaction("Master Fader Path");
                    session::track::setControlledBus(tree, busId, &safe->session_.getUndoManager());
                }
            };
        };
        path.addItem("Main output", taken.count(0) == 0, current == 0, setPath(0));
        for (const auto& bus : session_.getBusses())
            if (!session::track::isFolder(bus))
            {
                const auto busId = session::track::getId(bus);
                path.addItem(session::track::getName(bus), taken.count(busId) == 0, current == busId, setPath(busId));
            }
        menu.addSubMenu("Controls", path);
    }

    // VCA Masters: membership for every selected track (not a VCA into itself or a loop of VCAs).
    {
        juce::PopupMenu vcas;
        const auto commonVca = std::all_of(selection.begin(), selection.end(), [&](const juce::ValueTree& tree)
                                           { return session::track::getVca(tree) == session::track::getVca(t); })
                                   ? session::track::getVca(t)
                                   : std::uint32_t{0xffffffffu};
        vcas.addItem("None", true, commonVca == 0, withTracks(idsOf(selection), [](ArrangerView& self, const std::vector<juce::ValueTree>& trees)
                                                             {
                                                                 self.session_.beginTransaction("VCA Assignment");
                                                                 for (auto tree : trees)
                                                                     session::track::setVca(tree, 0, &self.session_.getUndoManager());
                                                             }));
        for (const auto& candidate : session_.getAllTracks())
        {
            if (session::track::getKind(candidate) != session::TrackKind::vca)
                continue;
            // Would a member end up controlling itself?
            bool loops = false;
            for (const auto& tree : selection)
            {
                std::set<std::uint32_t> visited;
                for (auto v = candidate; v.isValid() && visited.insert(session::track::getId(v)).second;
                     v = session_.findTrack(session::track::getVca(v)))
                    loops = loops || v == tree;
            }
            const auto vcaId = session::track::getId(candidate);
            vcas.addItem(session::track::getName(candidate), !loops, commonVca == vcaId,
                         withTracks(idsOf(selection), [vcaId](ArrangerView& self, const std::vector<juce::ValueTree>& trees)
                                    {
                                        self.session_.beginTransaction("VCA Assignment");
                                        for (auto tree : trees)
                                            session::track::setVca(tree, vcaId, &self.session_.getUndoManager());
                                    }));
        }
        menu.addSubMenu("VCA", vcas, vcas.getNumItems() > 1);
    }
    menu.addSubMenu("Add Automation Lane", automationMenu);
    if (session::holdsAudio(kind) || session::holdsMidi(kind))
    {
        const auto command = [safe](int id)
        {
            return [safe, id]
            {
                if (safe != nullptr && safe->onCommand)
                    safe->onCommand(id);
            };
        };
        const bool hasLanes = !session::Session::getTakeLanes(t).empty();
        juce::PopupMenu takes;
        takes.addItem("Show Take Lanes", true, actions::selectedTracksShowTakes(session_, editor_), command(commands::toggleTakeLanes));
        takes.addItem("New Take Lane from Comp", command(commands::newTakeLaneFromComp));
        takes.addSeparator();
        takes.addItem("Delete Unused Take Lanes", hasLanes, false, command(commands::deleteUnusedTakeLanes));
        takes.addItem("Delete All Take Lanes", hasLanes, false, command(commands::deleteAllTakeLanes));
        menu.addSubMenu("Take Lanes", takes);
    }
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
    const auto runCommand = [safe](int id)
    {
        return [safe, id]
        {
            if (safe != nullptr && safe->onCommand)
                safe->onCommand(id);
        };
    };
    menu.addItem(outermost.size() > 1 ? "Duplicate " + juce::String(outermost.size()) + " Tracks..." : juce::String("Duplicate..."),
                 runCommand(commands::duplicateTracks));
    menu.addItem("Split into Mono", !actions::getSplittableTracks(session_, editor_).empty(), false, runCommand(commands::splitIntoMono));
    const bool anyActive = std::any_of(selection.begin(), selection.end(), [](const juce::ValueTree& tree) { return session::track::isActive(tree); });
    menu.addItem(anyActive ? "Make Inactive" : "Make Active", withTracks(idsOf(selection), [anyActive](ArrangerView& self, const std::vector<juce::ValueTree>& trees)
                                                                         {
                                                                             self.session_.beginTransaction(anyActive ? "Make Inactive" : "Make Active");
                                                                             for (auto tree : trees)
                                                                                 session::track::setActive(tree, !anyActive, &self.session_.getUndoManager());
                                                                         }));
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
    const bool instrument = t.isValid() && session::holdsMidi(session::track::getKind(t));

    juce::PopupMenu menu;
    if (instrument)
        menu.addItem("New MIDI Clip Here", [safe, trackId, seconds]
                     {
                         if (safe == nullptr)
                             return;
                         if (auto tree = safe->session_.findTrack(trackId); tree.isValid())
                             safe->createMidiClipAt(tree, safe->editor_.snap(seconds, safe->session_.getTempoMap()));
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
                     const auto& map = safe->session_.getTempoMap();
                     const auto bar = map.barAt(map.beatAt(seconds));
                     const double start = map.secondsAt(map.beatOfBar(bar));
                     const double end = map.secondsAt(map.beatOfBar(bar + 1));
                     safe->session_.beginTransaction("Set Loop");
                     safe->session_.setLoop(true, start, end);
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
    dropTime_ = editor_.snap(juce::jmax(0.0, xToTime(static_cast<float>(x))), session_.getTempoMap());
    repaint();
}

void ArrangerView::dropFiles(const juce::StringArray& files)
{
    juce::StringArray audioFiles;
    juce::StringArray videoFiles;
    for (const auto& path : files)
    {
        if (video::isVideoFile(juce::File(path)))
            videoFiles.add(path);
        else if (project_.getAssets().canRead(juce::File(path)))
            audioFiles.add(path);
    }

    // Videos go onto a video track (the one they were dropped on, or the first, or a new one).
    auto target = dropRow_ >= 0 ? rows()[static_cast<std::size_t>(dropRow_)].track : juce::ValueTree();
    bool importedVideo = false;
    for (const auto& path : videoFiles)
    {
        juce::String error;
        if (actions::importVideo(session_, editor_, juce::File(path), dropTime_, target, error).isValid())
            importedVideo = true;
        else
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Import Video", error);
    }
    if (importedVideo && onVideoImported)
        onVideoImported();

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
        if (project_.getAssets().canRead(juce::File(path)) || video::isVideoFile(juce::File(path)))
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
    return browserDrag::isFileDrag(details.description) || description.startsWith(kDeviceDragPrefix)
           || description.startsWith(presetActions::kDragPrefix) || description.startsWith(cliplist::kDragPrefix);
}

void ArrangerView::itemDragEnter(const SourceDetails& details)
{
    const auto description = details.description.toString();
    dropIsDevice_ = description.startsWith(kDeviceDragPrefix) || description.startsWith(presetActions::kDragPrefix);
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

    if (description.startsWith(kDeviceDragPrefix) || description.startsWith(presetActions::kDragPrefix))
    {
        dropActive_ = false;
        const auto target = dropRow_ >= 0 ? rows()[static_cast<std::size_t>(dropRow_)].track : juce::ValueTree();
        if (onDeviceDropped)
            onDeviceDropped(target, description);
        repaint();
        return;
    }

    if (description.startsWith(cliplist::kDragPrefix))
    {
        // Clips from the Clip List: copies from the drop point on, one after the other (on a new track
        // when this one cannot hold them).
        dropActive_ = false;
        const auto items = cliplist::findDragged(session_, details.description);
        const auto target = dropRow_ >= 0 ? rows()[static_cast<std::size_t>(dropRow_)].track : juce::ValueTree();
        cliplist::place(session_, editor_, items, target, dropTime_);
        repaint();
        return;
    }

    dropFiles(browserDrag::getFiles(details.description));
}

} // namespace dojo::ui
