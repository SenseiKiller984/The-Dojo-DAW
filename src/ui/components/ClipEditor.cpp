#include "ui/components/ClipEditor.h"

#include "devices/Impulse.h"
#include "devices/Instruments.h"
#include "ui/DeviceActions.h"
#include "ui/theme/DojoTheme.h"

#include <cmath>

namespace dojo::ui
{
namespace
{
constexpr int kKeyboardWidth = 56;
constexpr int kRulerHeight = 18;
constexpr int kLaneHeight = 80;
constexpr int kExpressionLaneHeight = 62; ///< each of the three note expression lanes
constexpr int kExpressionGap = 4;
constexpr session::note::Expression kExpressionKinds[] = {session::note::Expression::pitch, session::note::Expression::pressure,
                                                         session::note::Expression::slide};
constexpr int kToolbarHeight = 30;
constexpr int kScrollBarSize = 12;
constexpr float kResizeGrab = 6.0f;

bool isBlackKey(int pitch)
{
    const int n = ((pitch % 12) + 12) % 12;
    return n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
}

/** A clip's length in beats, on the tempo map at its place. */
double clipBeats(const session::Session& session, const juce::ValueTree& clip)
{
    const auto& map = session.getTempoMap();
    return map.beatAt(session::clip::getEnd(clip)) - map.beatAt(session::clip::getStart(clip));
}

/** Content beats since a clip's start at `seconds` (on the tempo map). */
double beatsInto(const session::Session& session, const juce::ValueTree& clip, double seconds)
{
    const auto& map = session.getTempoMap();
    return map.beatAt(seconds) - map.beatAt(session::clip::getStart(clip));
}
} // namespace

//==================================================================================================
// PianoRoll

PianoRoll::PianoRoll(session::Session& session, EditorState& editor)
    : session_(session)
    , editor_(editor)
{
    setWantsKeyboardFocus(true);
    setOpaque(true);
    for (auto* bar : {&hScroll_, &vScroll_})
    {
        bar->setAutoHide(false);
        bar->addListener(this);
        addAndMakeVisible(bar);
    }
}

PianoRoll::~PianoRoll()
{
    stopPreview();
    if (clip_.isValid())
        clip_.removeListener(this);
}

void PianoRoll::startPreview(int pitch, float velocity)
{
    if (pitch == previewNote_)
        return;
    stopPreview();
    previewNote_ = juce::jlimit(0, 127, pitch);
    if (onPreview)
        onPreview(juce::MidiMessage::noteOn(1, previewNote_, juce::jlimit(0.05f, 1.0f, velocity)));
    repaint(keyboardArea());
}

void PianoRoll::stopPreview()
{
    if (previewNote_ < 0)
        return;
    if (onPreview)
        onPreview(juce::MidiMessage::noteOff(1, previewNote_));
    previewNote_ = -1;
    repaint(keyboardArea());
}

void PianoRoll::updateScrollBars()
{
    const auto grid = gridArea();
    // Time: the clip's content and a little more, and wherever the view has been taken.
    const double visibleBeats = static_cast<double>(std::max(1, grid.getWidth())) / pixelsPerBeat_;
    const double from = std::min(contentStart(), scrollBeats_);
    const double to = std::max(contentEnd() + 4.0, scrollBeats_ + visibleBeats);
    hScroll_.setRangeLimits(from, to, juce::dontSendNotification);
    hScroll_.setCurrentRange(scrollBeats_, visibleBeats, juce::dontSendNotification);
    hScroll_.setSingleStepSize(std::max(gridBeats(), visibleBeats / 16.0));

    // Pitch: 127 at the top, 0 at the bottom.
    const double visibleKeys = static_cast<double>(std::max(1, grid.getHeight())) / static_cast<double>(keyHeight_);
    vScroll_.setRangeLimits(0.0, 128.0, juce::dontSendNotification);
    vScroll_.setCurrentRange(127.0 - scrollPitch_, visibleKeys, juce::dontSendNotification);
    vScroll_.setSingleStepSize(1.0);
}

void PianoRoll::scrollBarMoved(juce::ScrollBar* bar, double newRangeStart)
{
    if (bar == &hScroll_)
        scrollBeats_ = newRangeStart;
    else
        scrollPitch_ = juce::jlimit(0, 127, 127 - juce::roundToInt(newRangeStart));
    repaint();
}

void PianoRoll::setClip(const juce::ValueTree& midiClip)
{
    if (clip_ == midiClip)
        return;
    stopPreview();
    if (clip_.isValid())
        clip_.removeListener(this);
    clip_ = midiClip;
    selection_.clear();
    if (clip_.isValid())
        clip_.addListener(this);
    zoomToFit();
}

void PianoRoll::setGridBeats(double beats)
{
    gridBeats_ = std::max(1.0 / 64.0, beats);
    repaint();
}

void PianoRoll::setPlayhead(double seconds)
{
    if (std::abs(seconds - playheadSeconds_) < 1.0e-4)
        return;
    playheadSeconds_ = seconds;
    repaint(gridArea());
}

void PianoRoll::setLowerLane(int controller)
{
    lowerLane_ = controller;
    resized(); // (the lanes' height changes with the mode)
    repaint();
}

int PianoRoll::lowerHeight() const noexcept
{
    return isExpressionMode() ? 3 * kExpressionLaneHeight + 2 * kExpressionGap : kLaneHeight;
}

double PianoRoll::gridBeats() const { return gridBeats_; }
double PianoRoll::contentStart() const { return clip_.isValid() ? session::clip::getOffset(clip_) : 0.0; }
double PianoRoll::contentEnd() const
{
    return clip_.isValid() ? contentStart() + clipBeats(session_, clip_) : 4.0;
}

juce::Rectangle<int> PianoRoll::keyboardArea() const
{
    return {0, kRulerHeight, kKeyboardWidth, std::max(0, getHeight() - kRulerHeight - lowerHeight() - 4 - kScrollBarSize)};
}
juce::Rectangle<int> PianoRoll::gridArea() const
{
    return {kKeyboardWidth, kRulerHeight, std::max(0, getWidth() - kKeyboardWidth - kScrollBarSize),
            std::max(0, getHeight() - kRulerHeight - lowerHeight() - 4 - kScrollBarSize)};
}
juce::Rectangle<int> PianoRoll::laneArea() const
{
    return {kKeyboardWidth, getHeight() - lowerHeight(), std::max(0, getWidth() - kKeyboardWidth - kScrollBarSize), lowerHeight()};
}

juce::Rectangle<int> PianoRoll::getExpressionLaneArea(session::note::Expression kind) const
{
    if (!isExpressionMode())
        return {};
    const auto lane = laneArea();
    const int index = kind == session::note::Expression::pitch ? 0 : kind == session::note::Expression::pressure ? 1 : 2;
    return {lane.getX(), lane.getY() + index * (kExpressionLaneHeight + kExpressionGap), lane.getWidth(), kExpressionLaneHeight};
}

bool PianoRoll::expressionLaneAt(float y, session::note::Expression& kind) const
{
    for (const auto k : kExpressionKinds)
    {
        const auto area = getExpressionLaneArea(k);
        if (y >= static_cast<float>(area.getY()) && y < static_cast<float>(area.getBottom()))
        {
            kind = k;
            return true;
        }
    }
    return false;
}

float PianoRoll::expressionY(session::note::Expression kind, double value) const
{
    const auto area = getExpressionLaneArea(kind).toFloat().reduced(0.0f, 5.0f);
    if (kind == session::note::Expression::pitch)
    {
        const double v = juce::jlimit(-pitchLaneRange_, pitchLaneRange_, value) / pitchLaneRange_;
        return area.getCentreY() - static_cast<float>(v) * area.getHeight() * 0.5f;
    }
    return area.getBottom() - static_cast<float>(juce::jlimit(0.0, 1.0, value)) * area.getHeight();
}

double PianoRoll::expressionValueAt(session::note::Expression kind, float y) const
{
    const auto area = getExpressionLaneArea(kind).toFloat().reduced(0.0f, 5.0f);
    if (area.getHeight() <= 0.0f)
        return 0.0;
    if (kind == session::note::Expression::pitch)
    {
        const double v = static_cast<double>((area.getCentreY() - y) / (area.getHeight() * 0.5f)) * pitchLaneRange_;
        // Near a whole semitone, exactly that (bends usually land on notes).
        const double whole = std::round(v);
        return juce::jlimit(-pitchLaneRange_, pitchLaneRange_, std::abs(v - whole) < 0.12 ? whole : v);
    }
    return juce::jlimit(0.0, 1.0, static_cast<double>((area.getBottom() - y) / area.getHeight()));
}

std::vector<juce::ValueTree> PianoRoll::expressionTargets(double beat) const
{
    if (!selection_.empty())
        return selection_;
    std::vector<juce::ValueTree> notes;
    for (const auto& n : clip_)
        if (n.hasType(session::ids::note) && session::note::getStart(n) <= beat && beat < session::note::getEnd(n))
            notes.push_back(n);
    return notes;
}

void PianoRoll::drawExpression(const juce::MouseEvent& e, bool erase)
{
    using session::note::ExpressionPoint;
    const double beat = xToBeat(e.position.x);
    const double value = expressionValueAt(dragKind_, e.position.y);
    auto* um = &session_.getUndoManager();
    const double resolution = 1.0 / 128.0; // (a point at most every 1/128 beat)

    for (auto& n : dragTargets_)
    {
        if (!n.getParent().isValid())
            continue;
        const double start = session::note::getStart(n);
        const double length = session::note::getLength(n);
        auto points = session::note::getExpression(n, dragKind_);
        const auto clampToNote = [length](double b) { return juce::jlimit(0.0, length, b); };
        const auto removeBetween = [&points](double from, double to, bool inclusive)
        {
            points.erase(std::remove_if(points.begin(), points.end(), [from, to, inclusive](const ExpressionPoint& p)
                                        { return inclusive ? (p.beat >= from && p.beat <= to) : (p.beat > from && p.beat < to); }),
                         points.end());
        };

        if (erase)
        {
            const double tolerance = 6.0 / pixelsPerBeat_;
            removeBetween(std::min(dragLastBeat_, beat) - start - tolerance, std::max(dragLastBeat_, beat) - start + tolerance, true);
        }
        else if (e.mods.isShiftDown())
        {
            // A straight ramp from where the stroke began.
            const double a = clampToNote(dragAnchorBeat_ - start);
            const double b = clampToNote(beat - start);
            removeBetween(std::min(a, b) - resolution * 0.5, std::max(a, b) + resolution * 0.5, true);
            if (points.empty() && std::min(a, b) > resolution)
                points.push_back({0.0, 0.0});
            points.push_back({a, dragAnchorValue_});
            if (std::abs(b - a) > resolution * 0.5)
                points.push_back({b, value});
        }
        else
        {
            // Freehand: the stroke replaces what was under it since the last point.
            const double at = std::round(clampToNote(beat - start) / resolution) * resolution;
            const double last = clampToNote(dragLastBeat_ - start);
            // (the point drawn last is within half a step of `last`: it stays)
            removeBetween(std::min(last, at) + resolution * 0.5, std::max(last, at) - resolution * 0.5, true);
            removeBetween(at - resolution * 0.5, at + resolution * 0.5, true);
            if (points.empty() && at > resolution)
                points.push_back({0.0, 0.0}); // (the note starts unbent: the curve begins at the neutral value)
            points.push_back({at, value});
        }
        session::note::setExpression(n, dragKind_, std::move(points), um);
    }
    dragLastBeat_ = beat;
}

float PianoRoll::beatToX(double beat) const
{
    return static_cast<float>(kKeyboardWidth) + static_cast<float>((beat - scrollBeats_) * pixelsPerBeat_);
}
double PianoRoll::xToBeat(float x) const
{
    return scrollBeats_ + static_cast<double>(x - static_cast<float>(kKeyboardWidth)) / pixelsPerBeat_;
}
float PianoRoll::pitchToY(int pitch) const
{
    return static_cast<float>(kRulerHeight) + static_cast<float>(scrollPitch_ - pitch) * keyHeight_;
}
int PianoRoll::yToPitch(float y) const
{
    return juce::jlimit(0, 127, scrollPitch_ - static_cast<int>(std::floor((y - static_cast<float>(kRulerHeight)) / keyHeight_)));
}

double PianoRoll::snapBeat(double beat, bool bypass) const
{
    if (bypass || !editor_.isSnapEnabled())
        return beat;
    const double grid = gridBeats();
    return contentStart() + std::round((beat - contentStart()) / grid) * grid;
}

void PianoRoll::zoomToFit()
{
    const double span = std::max(1.0, contentEnd() - contentStart());
    pixelsPerBeat_ = std::max(8.0, static_cast<double>(std::max(100, gridArea().getWidth() - 10)) / span);
    scrollBeats_ = contentStart();

    int highest = -1;
    int lowest = 128;
    if (clip_.isValid())
        for (const auto& n : clip_)
            if (n.hasType(session::ids::note))
            {
                highest = std::max(highest, session::note::getNote(n));
                lowest = std::min(lowest, session::note::getNote(n));
            }
    const int visibleKeys = std::max(12, static_cast<int>(static_cast<float>(gridArea().getHeight()) / keyHeight_));
    scrollPitch_ = highest >= 0 ? juce::jlimit(visibleKeys - 1, 127, (highest + lowest) / 2 + visibleKeys / 2) : 72 + visibleKeys / 4;
    updateScrollBars();
    repaint();
}

void PianoRoll::resized()
{
    const auto grid = gridArea();
    hScroll_.setBounds(grid.getX(), grid.getBottom(), grid.getWidth(), kScrollBarSize);
    vScroll_.setBounds(grid.getRight(), grid.getY(), kScrollBarSize, grid.getHeight());
    if (clip_.isValid() && pixelsPerBeat_ < 1.0)
        zoomToFit();
    updateScrollBars();
}

juce::ValueTree PianoRoll::noteAt(juce::Point<float> p, bool& onRightEdge) const
{
    onRightEdge = false;
    if (!clip_.isValid())
        return {};
    for (int i = clip_.getNumChildren() - 1; i >= 0; --i)
    {
        const auto n = clip_.getChild(i);
        if (!n.hasType(session::ids::note))
            continue;
        const float x0 = beatToX(session::note::getStart(n));
        const float x1 = beatToX(session::note::getEnd(n));
        const float y = pitchToY(session::note::getNote(n));
        if (p.x >= x0 && p.x <= x1 && p.y >= y && p.y < y + keyHeight_)
        {
            onRightEdge = x1 - p.x <= kResizeGrab && x1 - x0 > 2.0f * kResizeGrab;
            return n;
        }
    }
    return {};
}

bool PianoRoll::isSelected(const juce::ValueTree& note) const
{
    return std::find(selection_.begin(), selection_.end(), note) != selection_.end();
}

void PianoRoll::select(const juce::ValueTree& note, bool add)
{
    if (!add)
        selection_.clear();
    if (note.isValid() && !isSelected(note))
        selection_.push_back(note);
    repaint();
}

void PianoRoll::valueTreeChildRemoved(juce::ValueTree&, juce::ValueTree& child, int)
{
    selection_.erase(std::remove(selection_.begin(), selection_.end(), child), selection_.end());
    repaint();
}

void PianoRoll::deleteSelection()
{
    if (selection_.empty() || isLocked())
        return;
    session_.beginTransaction(selection_.size() == 1 ? "Delete Note" : "Delete Notes");
    const auto notes = selection_;
    for (const auto& n : notes)
        session_.removeNote(n);
    selection_.clear();
}

void PianoRoll::moveSelection(double beats, int semitones)
{
    if (selection_.empty() || isLocked())
        return;
    session_.beginTransaction("Move Notes");
    auto* um = &session_.getUndoManager();
    for (auto& n : selection_)
    {
        session::note::setStart(n, session::note::getStart(n) + beats, um);
        session::note::setNote(n, session::note::getNote(n) + semitones, um);
    }
}

bool PianoRoll::isLocked() const
{
    return clip_.isValid() && session::clip::isEditLocked(clip_);
}

void PianoRoll::quantiseSelection(double strength, bool ends)
{
    if (!clip_.isValid() || isLocked())
        return;
    session_.beginTransaction("Quantise");
    session_.quantise(clip_, selection_, gridBeats(), strength, ends);
}

void PianoRoll::paint(juce::Graphics& g)
{
    using namespace theme;
    g.fillAll(colour::windowBackground);

    if (!clip_.isValid())
        return;

    const auto grid = gridArea();
    const auto keys = keyboardArea();

    // Rows.
    g.saveState();
    g.reduceClipRegion(grid.getUnion(keys));
    for (int pitch = 127; pitch >= 0; --pitch)
    {
        const float y = pitchToY(pitch);
        if (y > static_cast<float>(grid.getBottom()) || y + keyHeight_ < static_cast<float>(grid.getY()))
            continue;
        const bool black = isBlackKey(pitch);
        g.setColour(black ? colour::laneEven : colour::laneOdd);
        g.fillRect(juce::Rectangle<float>(static_cast<float>(grid.getX()), y, static_cast<float>(grid.getWidth()), keyHeight_));
        g.setColour(pitch % 12 == 0 ? colour::gridBar : colour::gridBeat);
        g.drawHorizontalLine(juce::roundToInt(y + keyHeight_) - 1, static_cast<float>(grid.getX()), static_cast<float>(grid.getRight()));

        // Keyboard.
        auto key = juce::Rectangle<float>(0.0f, y, static_cast<float>(kKeyboardWidth), keyHeight_);
        g.setColour(pitch == previewNote_ ? colour::accent : black ? juce::Colour(0xff1b1d22) : juce::Colour(0xffd8d4cc));
        g.fillRect(black ? key.withWidth(kKeyboardWidth * 0.62f) : key);
        g.setColour(colour::outline);
        g.drawHorizontalLine(juce::roundToInt(y + keyHeight_) - 1, 0.0f, static_cast<float>(kKeyboardWidth));
        if (pitch % 12 == 0)
        {
            g.setColour(juce::Colour(0xff1b1d22)); // on a white key, in every theme
            g.setFont(mono(9.0f));
            g.drawText(juce::MidiMessage::getMidiNoteName(pitch, true, true, 3), key.reduced(3.0f, 0.0f), juce::Justification::centredRight, false);
        }
    }

    // The rest stays inside the grid (notes that start before the view must not cover the keys).
    g.restoreState();
    g.saveState();
    g.reduceClipRegion(grid);

    // Beats and bars.
    const double first = std::floor(xToBeat(static_cast<float>(grid.getX())));
    const double last = xToBeat(static_cast<float>(grid.getRight()));
    const double step = gridBeats() * pixelsPerBeat_ >= 6.0 ? gridBeats() : 1.0;
    // (bars from the tempo map at the clip's place: the arrangement's bars and signatures)
    const auto& map = session_.getTempoMap();
    const double clipBeat = clip_.isValid() ? map.beatAt(session::clip::getStart(clip_)) - contentStart() : 0.0;
    const auto isBarStart = [&](double b)
    {
        const double timeline = clipBeat + b;
        return std::abs(map.beatOfBar(map.barAt(timeline + 1.0e-6)) - timeline) < 1.0e-6;
    };
    for (double b = first; b <= last; b += step)
    {
        const bool bar = isBarStart(b);
        const bool beat = std::abs(b - std::round(b)) < 1.0e-6;
        g.setColour(bar ? colour::gridBar.brighter(0.2f) : beat ? colour::gridBar : colour::gridBeat);
        g.drawVerticalLine(juce::roundToInt(beatToX(b)), static_cast<float>(grid.getY()), static_cast<float>(grid.getBottom()));
    }

    // Outside the clip window.
    g.setColour(juce::Colours::black.withAlpha(0.45f));
    const float x0 = beatToX(contentStart());
    const float x1 = beatToX(contentEnd());
    g.fillRect(juce::Rectangle<float>(static_cast<float>(grid.getX()), static_cast<float>(grid.getY()), std::max(0.0f, x0 - static_cast<float>(grid.getX())), static_cast<float>(grid.getHeight())));
    g.fillRect(juce::Rectangle<float>(x1, static_cast<float>(grid.getY()), std::max(0.0f, static_cast<float>(grid.getRight()) - x1), static_cast<float>(grid.getHeight())));

    // Notes.
    const auto trackColour = clip_.getParent().isValid() ? session::track::getColour(clip_.getParent()) : colour::accent;
    for (const auto& n : clip_)
    {
        if (!n.hasType(session::ids::note))
            continue;
        const auto box = juce::Rectangle<float>(beatToX(session::note::getStart(n)), pitchToY(session::note::getNote(n)),
                                                std::max(3.0f, static_cast<float>(session::note::getLength(n) * pixelsPerBeat_)), keyHeight_ - 1.0f);
        const bool selected = isSelected(n);
        g.setColour(trackColour.withMultipliedBrightness(0.55f + 0.45f * session::note::getVelocity(n)));
        g.fillRoundedRectangle(box, 2.0f);
        g.setColour(selected ? colour::textPrimary : colour::windowBackground.withAlpha(0.7f));
        g.drawRoundedRectangle(box, 2.0f, selected ? 1.5f : 1.0f);

        // Its own pitch bend: a line from the middle of the note, a key per semitone.
        if (session::note::hasExpression(n, session::note::Expression::pitch))
        {
            const auto points = session::note::getExpression(n, session::note::Expression::pitch);
            const double start = session::note::getStart(n);
            const double length = session::note::getLength(n);
            const float centre = box.getCentreY();
            juce::Path bend;
            bend.startNewSubPath(box.getX(), centre - static_cast<float>(session::note::expressionAt(points, 0.0)) * keyHeight_);
            for (const auto& point : points)
                if (point.beat > 0.0 && point.beat < length)
                    bend.lineTo(beatToX(start + point.beat), centre - static_cast<float>(point.value) * keyHeight_);
            bend.lineTo(box.getRight(), centre - static_cast<float>(session::note::expressionAt(points, length)) * keyHeight_);
            g.setColour(colour::textPrimary.withAlpha(selected ? 0.95f : 0.7f));
            g.strokePath(bend, juce::PathStrokeType(selected ? 2.0f : 1.4f));
        }
    }

    // Playhead.
    const auto parent = clip_.getParent();
    const double playheadBeat = contentStart() + beatsInto(session_, clip_, playheadSeconds_);
    if (parent.isValid() && playheadBeat >= contentStart() && playheadBeat <= contentEnd())
    {
        g.setColour(colour::playhead);
        g.fillRect(beatToX(playheadBeat) - 0.5f, static_cast<float>(grid.getY()), 1.5f, static_cast<float>(grid.getHeight()));
    }
    g.restoreState();

    // Ruler.
    auto ruler = juce::Rectangle<int>(kKeyboardWidth, 0, grid.getWidth(), kRulerHeight);
    g.setColour(colour::panelHeader);
    g.fillRect(ruler.withX(0).withWidth(getWidth()));
    g.setColour(colour::textSecondary);
    g.setFont(mono(9.5f));
    g.saveState();
    g.reduceClipRegion(ruler);
    for (double b = std::ceil(first); b <= last; b += 1.0)
    {
        if (pixelsPerBeat_ < 20.0 && !isBarStart(b))
            continue;
        const auto position = map.toBarsBeatsTicks(std::max(0.0, clipBeat + b));
        g.drawText(position.beat == 1 && position.tick == 0 ? juce::String(position.bar)
                                                            : juce::String(position.bar) + "." + juce::String(position.beat),
                   juce::Rectangle<float>(beatToX(b) + 2.0f, 2.0f, 40.0f, 14.0f), juce::Justification::centredLeft, false);
    }
    g.restoreState();

    // Lower lane.
    const auto lane = laneArea();
    g.setColour(colour::panel);
    g.fillRect(lane.withX(0).withWidth(getWidth()));
    g.setColour(colour::outline);
    g.fillRect(0, lane.getY() - 4, getWidth(), 4);
    if (isExpressionMode())
    {
        paintExpressionLanes(g, trackColour);
        if (isLocked())
        {
            g.setColour(colour::textMuted);
            g.setFont(sansBold(12.0f));
            g.drawText("Edit-locked (Clip > Edit Unlock to change it)", grid.reduced(10, 6).removeFromTop(18), juce::Justification::topRight, true);
        }
        return;
    }
    g.setColour(colour::textMuted);
    g.setFont(label(8.5f));
    g.drawText(lowerLane_ < 0 ? "VELOCITY" : lowerLane_ == 128 ? "PITCH BEND" : lowerLane_ == 129 ? "PRESSURE" : "CC " + juce::String(lowerLane_),
               juce::Rectangle<int>(0, lane.getY(), kKeyboardWidth, 16), juce::Justification::centred, false);

    g.saveState();
    g.reduceClipRegion(lane);
    const auto valueToY = [&](double v) { return static_cast<float>(lane.getBottom() - 4) - static_cast<float>(v) * static_cast<float>(lane.getHeight() - 10); };
    if (lowerLane_ < 0)
    {
        for (const auto& n : clip_)
        {
            if (!n.hasType(session::ids::note))
                continue;
            const float x = beatToX(session::note::getStart(n));
            const float y = valueToY(session::note::getVelocity(n));
            g.setColour(isSelected(n) ? colour::textPrimary : trackColour);
            g.fillRect(juce::Rectangle<float>(x, y, 3.0f, static_cast<float>(lane.getBottom()) - y));
            g.fillEllipse(juce::Rectangle<float>(7.0f, 7.0f).withCentre({x + 1.5f, y}));
        }
    }
    else
    {
        juce::Path path;
        bool started = false;
        std::vector<std::pair<double, double>> points;
        for (const auto& c : clip_)
            if (c.hasType(session::ids::control) && static_cast<int>(c.getProperty(session::ids::controller)) == lowerLane_)
                points.emplace_back(static_cast<double>(c.getProperty(session::ids::beat)), static_cast<double>(c.getProperty(session::ids::value)));
        std::sort(points.begin(), points.end());
        for (const auto& [beat, value] : points)
        {
            const float x = beatToX(beat);
            const float y = valueToY(value);
            if (!started)
                path.startNewSubPath(static_cast<float>(lane.getX()), y);
            else
                path.lineTo(x, path.getCurrentPosition().y);
            path.lineTo(x, y);
            started = true;
            g.setColour(trackColour);
            g.fillEllipse(juce::Rectangle<float>(6.0f, 6.0f).withCentre({x, y}));
        }
        if (started)
        {
            path.lineTo(static_cast<float>(lane.getRight()), path.getCurrentPosition().y);
            g.setColour(trackColour.withAlpha(0.8f));
            g.strokePath(path, juce::PathStrokeType(1.5f));
        }
    }
    g.restoreState();

    if (isLocked())
    {
        g.setColour(colour::textMuted);
        g.setFont(sansBold(12.0f));
        g.drawText("Edit-locked (Clip > Edit Unlock to change it)", grid.reduced(10, 6).removeFromTop(18), juce::Justification::topRight, true);
    }
}

void PianoRoll::paintExpressionLanes(juce::Graphics& g, juce::Colour trackColour) const
{
    using namespace theme;
    using session::note::Expression;

    for (const auto kind : kExpressionKinds)
    {
        const auto area = getExpressionLaneArea(kind);
        g.setColour(colour::windowBackground.interpolatedWith(colour::panel, 0.5f));
        g.fillRect(area);

        // Its name on the left, and the scale.
        const auto name = juce::Rectangle<int>(0, area.getY(), kKeyboardWidth, area.getHeight());
        g.setColour(colour::textSecondary);
        g.setFont(label(8.5f));
        g.drawText(kind == Expression::pitch ? "PITCH" : kind == Expression::pressure ? "PRESSURE" : "SLIDE", name.withHeight(16),
                   juce::Justification::centred, false);
        g.setColour(colour::textMuted);
        g.setFont(mono(8.5f));
        g.drawText(kind == Expression::pitch ? juce::String::fromUTF8("\xc2\xb1") + juce::String(juce::roundToInt(pitchLaneRange_)) + " st"
                                             : juce::String("0 - 127"),
                   name.withTrimmedTop(16).withHeight(14), juce::Justification::centred, false);

        g.saveState();
        g.reduceClipRegion(area);
        // Reference lines: no bend (and every octave), or the floor.
        g.setColour(colour::gridBar);
        if (kind == Expression::pitch)
        {
            g.drawHorizontalLine(juce::roundToInt(expressionY(kind, 0.0)), static_cast<float>(area.getX()), static_cast<float>(area.getRight()));
            g.setColour(colour::gridBeat);
            for (double st = 12.0; st < pitchLaneRange_ + 0.5; st += 12.0)
                for (const double v : {st, -st})
                    g.drawHorizontalLine(juce::roundToInt(expressionY(kind, v)), static_cast<float>(area.getX()), static_cast<float>(area.getRight()));
        }

        for (const auto& n : clip_)
        {
            if (!n.hasType(session::ids::note))
                continue;
            const double start = session::note::getStart(n);
            const double length = session::note::getLength(n);
            const float x0 = beatToX(start);
            const float x1 = beatToX(start + length);
            if (x1 < static_cast<float>(area.getX()) || x0 > static_cast<float>(area.getRight()))
                continue;
            const bool selected = isSelected(n);
            const bool anySelected = !selection_.empty();

            // The note's span, under it.
            g.setColour(trackColour.withAlpha(selected ? 0.20f : 0.07f));
            g.fillRect(juce::Rectangle<float>(x0, static_cast<float>(area.getY()), std::max(1.0f, x1 - x0), static_cast<float>(area.getHeight())));

            const auto points = session::note::getExpression(n, kind);
            if (points.empty() && !selected)
                continue;
            juce::Path curve;
            curve.startNewSubPath(x0, expressionY(kind, session::note::expressionAt(points, 0.0)));
            for (const auto& point : points)
                if (point.beat > 0.0 && point.beat < length)
                    curve.lineTo(beatToX(start + point.beat), expressionY(kind, point.value));
            curve.lineTo(x1, expressionY(kind, session::note::expressionAt(points, length)));
            const auto stroke = selected || !anySelected ? trackColour.brighter(0.35f) : trackColour.withAlpha(0.45f);
            g.setColour(points.empty() ? stroke.withAlpha(0.35f) : stroke);
            g.strokePath(curve, juce::PathStrokeType(selected ? 2.0f : 1.3f));
            if (selected)
                for (const auto& point : points)
                    if (point.beat >= 0.0 && point.beat <= length)
                        g.fillEllipse(juce::Rectangle<float>(4.5f, 4.5f).withCentre({beatToX(start + point.beat), expressionY(kind, point.value)}));
        }
        g.restoreState();
    }
}

void PianoRoll::mouseDown(const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    drag_ = Drag::none;

    // The keys: hear the note while the mouse is held (also on an edit-locked clip).
    if (clip_.isValid() && keyboardArea().contains(e.getPosition()) && !e.mods.isPopupMenu())
    {
        drag_ = Drag::keyboard;
        startPreview(yToPitch(e.position.y), 0.35f + 0.65f * juce::jlimit(0.0f, 1.0f, e.position.x / static_cast<float>(kKeyboardWidth)));
        return;
    }

    if (!clip_.isValid() || isLocked()) // an edit-locked clip is read-only
        return;

    dragChanged_ = false;
    dragStart_ = e.position;
    dragNotes_.clear();

    if (isExpressionMode())
    {
        session::note::Expression kind = session::note::Expression::pitch;
        if (e.position.x < static_cast<float>(kKeyboardWidth) && expressionLaneAt(e.position.y, kind))
        {
            // The pitch lane's name steps through its scale.
            if (kind == session::note::Expression::pitch)
            {
                pitchLaneRange_ = pitchLaneRange_ < 2.5 ? 12.0 : pitchLaneRange_ < 12.5 ? 24.0 : pitchLaneRange_ < 24.5 ? 48.0 : 2.0;
                repaint();
            }
            return;
        }
        if (laneArea().contains(e.getPosition()) && expressionLaneAt(e.position.y, kind))
        {
            dragTargets_ = expressionTargets(xToBeat(e.position.x));
            if (dragTargets_.empty())
                return;
            dragKind_ = kind;
            dragLastBeat_ = dragAnchorBeat_ = xToBeat(e.position.x);
            dragAnchorValue_ = expressionValueAt(kind, e.position.y);
            session_.beginTransaction(e.mods.isPopupMenu() ? "Erase Note Expression" : "Draw Note Expression");
            drag_ = Drag::expression;
            drawExpression(e, e.mods.isPopupMenu());
            return;
        }
    }

    if (laneArea().contains(e.getPosition()))
    {
        session_.beginTransaction(lowerLane_ < 0 ? "Change Velocity" : "Draw Controller");
        drag_ = Drag::lane;
        editLane(e, e.mods.isPopupMenu());
        return;
    }

    if (!gridArea().contains(e.getPosition()))
        return;

    bool rightEdge = false;
    auto note = noteAt(e.position, rightEdge);

    if (e.mods.isPopupMenu())
    {
        if (note.isValid() && !isSelected(note))
            select(note, false);
        juce::Component::SafePointer<PianoRoll> safe(this);
        juce::PopupMenu menu;
        menu.addItem("Delete", !selection_.empty(), false, [safe] { if (safe != nullptr) safe->deleteSelection(); });
        menu.addItem("Quantise", [safe] { if (safe != nullptr) safe->quantiseSelection(1.0, false); });
        menu.addItem("Select All", [safe]
                     {
                         if (safe == nullptr)
                             return;
                         safe->selection_.clear();
                         for (const auto& n : safe->clip_)
                             if (n.hasType(session::ids::note))
                                 safe->selection_.push_back(n);
                         safe->repaint();
                     });
        juce::PopupMenu velocity;
        for (const float v : {0.25f, 0.5f, 0.75f, 1.0f})
            velocity.addItem(juce::String(juce::roundToInt(v * 127.0f)), [safe, v]
                             {
                                 if (safe == nullptr)
                                     return;
                                 safe->session_.beginTransaction("Change Velocity");
                                 for (auto& n : safe->selection_)
                                     session::note::setVelocity(n, v, &safe->session_.getUndoManager());
                             });
        menu.addSubMenu("Velocity", velocity, !selection_.empty());
        juce::PopupMenu expression;
        const auto clearExpression = [safe](std::vector<session::note::Expression> kinds)
        {
            return [safe, kinds]
            {
                if (safe == nullptr)
                    return;
                safe->session_.beginTransaction("Clear Note Expression");
                for (auto& n : safe->selection_)
                    for (const auto kind : kinds)
                        session::note::setExpression(n, kind, {}, &safe->session_.getUndoManager());
            };
        };
        const bool anyExpression = std::any_of(selection_.begin(), selection_.end(), [](const juce::ValueTree& n) { return session::note::hasExpression(n); });
        expression.addItem("Clear Pitch", anyExpression, false, clearExpression({session::note::Expression::pitch}));
        expression.addItem("Clear Pressure", anyExpression, false, clearExpression({session::note::Expression::pressure}));
        expression.addItem("Clear Slide", anyExpression, false, clearExpression({session::note::Expression::slide}));
        expression.addItem("Clear All", anyExpression, false,
                           clearExpression({session::note::Expression::pitch, session::note::Expression::pressure, session::note::Expression::slide}));
        menu.addSubMenu("Note Expression", expression, !selection_.empty());
        menu.showMenuAsync(juce::PopupMenu::Options().withMousePosition());
        return;
    }

    if (note.isValid())
    {
        if (e.mods.isShiftDown())
            select(note, true);
        else if (!isSelected(note))
            select(note, false);
        drag_ = rightEdge ? Drag::resize : Drag::move;
    }
    else
    {
        // Draw a new note.
        const double beat = snapBeat(xToBeat(e.position.x) - (editor_.isSnapEnabled() ? gridBeats() * 0.5 : 0.0), e.mods.isAltDown());
        session_.beginTransaction("Add Note");
        note = session_.addNote(clip_, std::max(contentStart(), beat), gridBeats(), yToPitch(e.position.y));
        select(note, false);
        drag_ = Drag::resize;
        dragChanged_ = true;
    }

    for (const auto& n : selection_)
        dragNotes_.push_back({n, session::note::getStart(n), session::note::getLength(n), session::note::getNote(n)});
}

void PianoRoll::editLane(const juce::MouseEvent& e, bool erase)
{
    const auto lane = laneArea();
    const double value = juce::jlimit(0.0, 1.0, (static_cast<double>(lane.getBottom() - 4) - e.position.y) / (lane.getHeight() - 10));
    const double beat = xToBeat(e.position.x);
    auto* um = &session_.getUndoManager();

    if (lowerLane_ < 0)
    {
        const double tolerance = std::max(4.0 / pixelsPerBeat_, gridBeats() * 0.25);
        for (auto n : clip_)
            if (n.hasType(session::ids::note) && std::abs(session::note::getStart(n) - beat) <= tolerance)
                session::note::setVelocity(n, static_cast<float>(value), um);
        return;
    }

    const double snapped = std::max(contentStart(), snapBeat(beat, e.mods.isAltDown()));
    const double tolerance = std::max(3.0 / pixelsPerBeat_, 1.0e-6);
    for (int i = clip_.getNumChildren() - 1; i >= 0; --i)
    {
        auto c = clip_.getChild(i);
        if (!c.hasType(session::ids::control) || static_cast<int>(c.getProperty(session::ids::controller)) != lowerLane_)
            continue;
        if (std::abs(static_cast<double>(c.getProperty(session::ids::beat)) - (erase ? beat : snapped)) <= (erase ? 6.0 / pixelsPerBeat_ : tolerance))
        {
            if (erase)
                session_.removeControl(c);
            else
                c.setProperty(session::ids::value, value, um);
            return;
        }
    }
    if (!erase)
        session_.addControl(clip_, lowerLane_, snapped, static_cast<float>(value));
}

void PianoRoll::mouseDrag(const juce::MouseEvent& e)
{
    if (drag_ == Drag::keyboard)
    {
        // Sliding along the keys plays each one in turn.
        startPreview(yToPitch(e.position.y), 0.35f + 0.65f * juce::jlimit(0.0f, 1.0f, e.position.x / static_cast<float>(kKeyboardWidth)));
        return;
    }
    if (drag_ == Drag::lane)
    {
        editLane(e, e.mods.isPopupMenu());
        return;
    }
    if (drag_ == Drag::expression)
    {
        drawExpression(e, e.mods.isPopupMenu());
        return;
    }
    if (dragNotes_.empty() || (drag_ != Drag::move && drag_ != Drag::resize))
        return;

    if (!dragChanged_)
    {
        if (e.position.getDistanceFrom(dragStart_) < 3.0f)
            return;
        session_.beginTransaction(drag_ == Drag::move ? "Move Notes" : "Resize Notes");
        dragChanged_ = true;
    }

    auto* um = &session_.getUndoManager();
    const double deltaBeats = static_cast<double>(e.position.x - dragStart_.x) / pixelsPerBeat_;
    const auto& anchor = dragNotes_.front();

    if (drag_ == Drag::move)
    {
        const double delta = snapBeat(anchor.start + deltaBeats, e.mods.isAltDown()) - anchor.start;
        const int semitones = -static_cast<int>(std::round((e.position.y - dragStart_.y) / keyHeight_));
        for (auto& s : dragNotes_)
        {
            session::note::setStart(s.note, s.start + delta, um);
            session::note::setNote(s.note, s.pitch + semitones, um);
        }
    }
    else
    {
        const double end = snapBeat(anchor.start + anchor.length + deltaBeats, e.mods.isAltDown());
        const double delta = end - (anchor.start + anchor.length);
        for (auto& s : dragNotes_)
            session::note::setLength(s.note, std::max(session::kMinNoteLength, s.length + delta), um);
    }
}

void PianoRoll::mouseUp(const juce::MouseEvent&)
{
    stopPreview(); // (the note sounds only while the key is held)
    drag_ = Drag::none;
    dragNotes_.clear();
    dragTargets_.clear();
}

void PianoRoll::mouseMove(const juce::MouseEvent& e)
{
    bool rightEdge = false;
    const auto note = noteAt(e.position, rightEdge);
    setMouseCursor(note.isValid() ? (rightEdge ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::DraggingHandCursor)
                                  : juce::MouseCursor::NormalCursor);
}

void PianoRoll::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (isLocked())
        return;
    session::note::Expression kind = session::note::Expression::pitch;
    if (isExpressionMode() && laneArea().contains(e.getPosition()) && expressionLaneAt(e.position.y, kind))
    {
        // Clears that curve of the notes there (or the selected ones).
        const auto targets = expressionTargets(xToBeat(e.position.x));
        if (!targets.empty())
        {
            session_.beginTransaction("Clear Note Expression");
            for (auto n : targets)
                session::note::setExpression(n, kind, {}, &session_.getUndoManager());
        }
        return;
    }
    bool rightEdge = false;
    if (auto note = noteAt(e.position, rightEdge); note.isValid())
    {
        session_.beginTransaction("Delete Note");
        session_.removeNote(note);
    }
}

void PianoRoll::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (e.mods.isCommandDown())
    {
        const double anchor = xToBeat(e.position.x);
        pixelsPerBeat_ = juce::jlimit(4.0, 800.0, pixelsPerBeat_ * std::pow(1.0025, static_cast<double>(wheel.deltaY) * 256.0));
        scrollBeats_ = anchor - static_cast<double>(e.position.x - kKeyboardWidth) / pixelsPerBeat_;
    }
    else if (e.mods.isShiftDown() || std::abs(wheel.deltaX) > std::abs(wheel.deltaY))
    {
        const float delta = std::abs(wheel.deltaX) > std::abs(wheel.deltaY) ? wheel.deltaX : wheel.deltaY;
        scrollBeats_ -= static_cast<double>(delta) * 400.0 / pixelsPerBeat_;
    }
    else
    {
        scrollPitch_ = juce::jlimit(12, 127, scrollPitch_ + juce::roundToInt(wheel.deltaY * 24.0f));
    }
    updateScrollBars();
    repaint();
}

bool PianoRoll::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
    {
        deleteSelection();
        return true;
    }
    if (key == juce::KeyPress('a', juce::ModifierKeys::commandModifier, 0))
    {
        selection_.clear();
        for (const auto& n : clip_)
            if (n.hasType(session::ids::note))
                selection_.push_back(n);
        repaint();
        return true;
    }
    if (key.getTextCharacter() == 'q' || key.getTextCharacter() == 'Q')
    {
        quantiseSelection(1.0, false);
        return true;
    }
    const int octave = key.getModifiers().isShiftDown() ? 12 : 1;
    if (key.getKeyCode() == juce::KeyPress::upKey)
    {
        moveSelection(0.0, octave);
        return true;
    }
    if (key.getKeyCode() == juce::KeyPress::downKey)
    {
        moveSelection(0.0, -octave);
        return true;
    }
    if (key.getKeyCode() == juce::KeyPress::leftKey)
    {
        moveSelection(-gridBeats(), 0);
        return true;
    }
    if (key.getKeyCode() == juce::KeyPress::rightKey)
    {
        moveSelection(gridBeats(), 0);
        return true;
    }
    return false;
}

//==================================================================================================
// StepSequencer

namespace
{
constexpr int kRowLabelWidth = 120;
constexpr int kStepRows = 16;
} // namespace

StepSequencer::StepSequencer(session::Project& project, EditorState& editor)
    : project_(project)
    , editor_(editor)
{
    setOpaque(true);
}

StepSequencer::~StepSequencer()
{
    if (previewNote_ >= 0 && onPreview)
        onPreview(juce::MidiMessage::noteOff(1, previewNote_));
    if (clip_.isValid())
        clip_.removeListener(this);
}

void StepSequencer::setClip(const juce::ValueTree& midiClip)
{
    if (clip_ == midiClip)
        return;
    if (previewNote_ >= 0 && onPreview)
        onPreview(juce::MidiMessage::noteOff(1, previewNote_));
    previewNote_ = -1;
    if (clip_.isValid())
        clip_.removeListener(this);
    clip_ = midiClip;
    if (clip_.isValid())
        clip_.addListener(this);
    repaint();
}

void StepSequencer::setStepBeats(double beats)
{
    stepBeats_ = std::max(1.0 / 32.0, beats);
    repaint();
}

void StepSequencer::setBaseNote(int note)
{
    baseNote_ = juce::jlimit(0, 127 - kStepRows, note);
    repaint();
}

void StepSequencer::setPlayhead(double seconds)
{
    if (std::abs(seconds - playheadSeconds_) < 1.0e-3)
        return;
    playheadSeconds_ = seconds;
    repaint();
}

bool StepSequencer::usesDrumRack() const
{
    if (!clip_.isValid())
        return false;
    const auto instrument = deviceActions::findInstrument(clip_.getParent());
    if (!instrument.isValid())
        return false;
    const auto type = session::device::getType(instrument);
    return type == devices::DrumRack::kTypeId || type == devices::Impulse::kTypeId;
}

int StepSequencer::rowPitch(int row) const
{
    return usesDrumRack() ? devices::DrumRack::kFirstNote + row : baseNote_ + (kStepRows - 1 - row);
}

juce::String StepSequencer::rowName(int row) const
{
    const int pitch = rowPitch(row);
    juce::String name = juce::MidiMessage::getMidiNoteName(pitch, true, true, 3);
    if (usesDrumRack())
    {
        if (auto node = project_.getDevices().findNode(session::device::getId(deviceActions::findInstrument(clip_.getParent()))))
        {
            if (auto* rack = dynamic_cast<devices::DrumRack*>(&node->getProcessor()))
                if (const auto sample = rack->getPadSample(row))
                    name << "  " << sample->file.getFileNameWithoutExtension();
            if (auto* impulse = dynamic_cast<devices::Impulse*>(&node->getProcessor()); impulse != nullptr && row < devices::Impulse::kSlots)
                if (const auto sample = impulse->getSlotSample(row))
                    name << "  " << sample->file.getFileNameWithoutExtension();
        }
    }
    return name;
}

int StepSequencer::numSteps() const
{
    if (!clip_.isValid())
        return 16;
    const double beats = clipBeats(project_.getSession(), clip_);
    return juce::jlimit(1, 256, static_cast<int>(std::floor(beats / stepBeats_ + 1.0e-6)));
}

juce::Rectangle<int> StepSequencer::cellBounds(int row, int step) const
{
    const int steps = numSteps();
    const float cellWidth = static_cast<float>(getWidth() - kRowLabelWidth) / static_cast<float>(steps);
    const float rowHeight = static_cast<float>(getHeight() - kRulerHeight) / kStepRows;
    return juce::Rectangle<float>(static_cast<float>(kRowLabelWidth) + static_cast<float>(step) * cellWidth,
                                  static_cast<float>(kRulerHeight) + static_cast<float>(row) * rowHeight, cellWidth, rowHeight)
        .toNearestInt();
}

juce::Rectangle<int> StepSequencer::getRowLabel(int row) const
{
    const auto cell = cellBounds(row, 0);
    return {0, cell.getY(), kRowLabelWidth, cell.getHeight()};
}

bool StepSequencer::cellAt(juce::Point<int> p, int& row, int& step) const
{
    if (p.x < kRowLabelWidth || p.y < kRulerHeight)
        return false;
    const int steps = numSteps();
    step = static_cast<int>(static_cast<float>(p.x - kRowLabelWidth) / (static_cast<float>(getWidth() - kRowLabelWidth) / static_cast<float>(steps)));
    row = static_cast<int>(static_cast<float>(p.y - kRulerHeight) / (static_cast<float>(getHeight() - kRulerHeight) / kStepRows));
    return step >= 0 && step < steps && row >= 0 && row < kStepRows;
}

juce::ValueTree StepSequencer::noteInCell(int row, int step) const
{
    if (!clip_.isValid())
        return {};
    const double begin = session::clip::getOffset(clip_) + step * stepBeats_;
    const int pitch = rowPitch(row);
    for (const auto& n : clip_)
        if (n.hasType(session::ids::note) && session::note::getNote(n) == pitch && session::note::getStart(n) >= begin - 1.0e-6 &&
            session::note::getStart(n) < begin + stepBeats_ - 1.0e-6)
            return n;
    return {};
}

void StepSequencer::setCell(int row, int step, bool on)
{
    auto existing = noteInCell(row, step);
    if (on && !existing.isValid())
        project_.getSession().addNote(clip_, session::clip::getOffset(clip_) + step * stepBeats_, stepBeats_, rowPitch(row), 0.8f);
    else if (!on && existing.isValid())
        project_.getSession().removeNote(existing);
}

void StepSequencer::paint(juce::Graphics& g)
{
    using namespace theme;
    g.fillAll(colour::windowBackground);
    if (!clip_.isValid())
        return;

    const int steps = numSteps();
    const auto trackColour = session::track::getColour(clip_.getParent());
    const double playBeat = beatsInto(project_.getSession(), clip_, playheadSeconds_);
    const int playStep = playBeat >= 0.0 ? static_cast<int>(std::floor(playBeat / stepBeats_)) : -1;

    g.setColour(colour::panelHeader);
    g.fillRect(0, 0, getWidth(), kRulerHeight);
    g.setFont(mono(9.0f));
    const int stepsPerBeat = std::max(1, juce::roundToInt(1.0 / stepBeats_));
    for (int s = 0; s < steps; s += stepsPerBeat)
    {
        g.setColour(colour::textSecondary);
        g.drawText(juce::String(s / stepsPerBeat + 1), cellBounds(0, s).withY(0).withHeight(kRulerHeight).translated(3, 0),
                   juce::Justification::centredLeft, false);
    }

    for (int row = 0; row < kStepRows; ++row)
    {
        const auto label = getRowLabel(row);
        g.setColour(rowPitch(row) == previewNote_ ? colour::accent.withAlpha(0.6f) : row % 2 == 0 ? colour::panelRaised : colour::panel);
        g.fillRect(label);
        g.setColour(colour::textPrimary);
        g.setFont(sans(10.5f));
        g.drawText(rowName(row), label.reduced(6, 0), juce::Justification::centredLeft, true);

        for (int s = 0; s < steps; ++s)
        {
            const auto cell = cellBounds(row, s).reduced(1).toFloat();
            const bool beatStart = s % stepsPerBeat == 0;
            const auto note = noteInCell(row, s);
            if (note.isValid())
            {
                g.setColour(trackColour.withMultipliedBrightness(0.5f + 0.5f * session::note::getVelocity(note)));
                g.fillRoundedRectangle(cell, 2.0f);
            }
            else
            {
                g.setColour(((s / stepsPerBeat) % 2 == 0) ? colour::control : colour::panelRaised);
                g.fillRoundedRectangle(cell, 2.0f);
            }
            if (s == playStep)
            {
                g.setColour(colour::textPrimary.withAlpha(0.25f));
                g.fillRoundedRectangle(cell, 2.0f);
            }
            if (beatStart)
            {
                g.setColour(colour::gridBar);
                g.drawVerticalLine(juce::roundToInt(cell.getX()) - 1, cell.getY(), cell.getBottom());
            }
        }
    }
}

void StepSequencer::mouseDown(const juce::MouseEvent& e)
{
    int row = 0;
    int step = 0;
    // A row's name: hear its note (pad) while the mouse is held.
    if (clip_.isValid() && !e.mods.isPopupMenu() && e.x < kRowLabelWidth && e.y >= kRulerHeight)
    {
        for (int r = 0; r < kStepRows; ++r)
            if (getRowLabel(r).contains(e.getPosition()))
            {
                previewNote_ = rowPitch(r);
                if (onPreview)
                    onPreview(juce::MidiMessage::noteOn(1, previewNote_, 0.8f));
                repaint();
            }
        return;
    }
    if (!clip_.isValid() || session::clip::isEditLocked(clip_) || !cellAt(e.getPosition(), row, step))
        return;

    auto& session = project_.getSession();
    editor_.selectTrack(session::track::getId(clip_.getParent()));
    if (e.mods.isPopupMenu())
    {
        if (auto note = noteInCell(row, step); note.isValid())
        {
            const float v = session::note::getVelocity(note);
            session.beginTransaction("Change Velocity");
            session::note::setVelocity(note, v > 0.9f ? 0.7f : v > 0.6f ? 0.4f : 1.0f, &session.getUndoManager());
        }
        return;
    }

    session.beginTransaction("Step Edit");
    paintOn_ = !noteInCell(row, step).isValid();
    painting_ = true;
    lastRow_ = row;
    lastStep_ = step;
    setCell(row, step, paintOn_);
}

void StepSequencer::mouseDrag(const juce::MouseEvent& e)
{
    int row = 0;
    int step = 0;
    if (!painting_ || !cellAt(e.getPosition(), row, step) || (row == lastRow_ && step == lastStep_))
        return;
    lastRow_ = row;
    lastStep_ = step;
    setCell(row, step, paintOn_);
}

void StepSequencer::mouseUp(const juce::MouseEvent&)
{
    painting_ = false;
    if (previewNote_ >= 0)
    {
        if (onPreview)
            onPreview(juce::MidiMessage::noteOff(1, previewNote_));
        previewNote_ = -1;
        repaint();
    }
}

//==================================================================================================
// AudioClipInspector

AudioClipInspector::AudioClipInspector(session::Session& session)
    : session_(session)
{
    const std::array<const char*, 7> names{"Gain (dB)", "Time-stretch (x)", "Pitch (st)", "Fade in (s)", "Fade out (s)", "Track speed (x)", "Track pitch (st)"};
    const std::array<const char*, 7> undo{"Clip Gain", "Time-stretch", "Clip Pitch", "Fade In", "Fade Out", "Track Speed", "Track Pitch"};

    for (std::size_t i = 0; i < fields_.size(); ++i)
    {
        auto& field = fields_[i];
        field.label.setText(names[i], juce::dontSendNotification);
        field.label.setFont(theme::sans(12.0f));
        field.label.setColour(juce::Label::textColourId, theme::colour::textSecondary);
        addAndMakeVisible(field.label);

        field.slider.setSliderStyle(juce::Slider::LinearHorizontal);
        field.slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 70, 22);
        field.slider.setWantsKeyboardFocus(false);
        addAndMakeVisible(field.slider);

        field.slider.onDragStart = [this, name = juce::String(undo[i])] { session_.beginTransaction(name); };
        field.slider.onValueChange = [this, i, name = juce::String(undo[i])]
        {
            if (!clip_.isValid())
                return;
            auto& slider = fields_[i].slider;
            if (slider.getThumbBeingDragged() < 0)
                session_.beginTransaction(name);
            auto* um = &session_.getUndoManager();
            const double v = slider.getValue();
            switch (i)
            {
            case 0: session::clip::setGainDb(clip_, v, um); break;
            case 1: session::clip::setStretch(clip_, v, um); break;
            case 2: session::clip::setPitch(clip_, v, um); break;
            case 3: session::clip::setFadeIn(clip_, v, um); break;
            case 4: session::clip::setFadeOut(clip_, v, um); break;
            case 5: session::track::setSpeed(track_, v, um); break;
            case 6: session::track::setPitch(track_, v, um); break;
            default: break;
            }
        };
    }

    fields_[0].slider.setRange(-60.0, 24.0, 0.1);
    fields_[0].slider.setDoubleClickReturnValue(true, 0.0);
    fields_[1].slider.setRange(session::kMinStretch, session::kMaxStretch, 0.001);
    fields_[1].slider.setSkewFactorFromMidPoint(1.0);
    fields_[1].slider.setDoubleClickReturnValue(true, 1.0);
    fields_[2].slider.setRange(-24.0, 24.0, 0.01);
    fields_[2].slider.setDoubleClickReturnValue(true, 0.0);
    fields_[5].slider.setRange(session::kMinStretch, session::kMaxStretch, 0.001);
    fields_[5].slider.setSkewFactorFromMidPoint(1.0);
    fields_[5].slider.setDoubleClickReturnValue(true, 1.0);
    fields_[6].slider.setRange(-24.0, 24.0, 0.01);
    fields_[6].slider.setDoubleClickReturnValue(true, 0.0);

    matchTempo_.setButtonText("Reset Stretch and Pitch");
    matchTempo_.onClick = [this]
    {
        if (!clip_.isValid())
            return;
        session_.beginTransaction("Reset Stretch and Pitch");
        session::clip::setStretch(clip_, 1.0, &session_.getUndoManager());
        session::clip::setPitch(clip_, 0.0, &session_.getUndoManager());
    };
    addAndMakeVisible(matchTempo_);
}

AudioClipInspector::~AudioClipInspector()
{
    if (clip_.isValid())
        clip_.removeListener(this);
    if (track_.isValid())
        track_.removeListener(this);
}

void AudioClipInspector::setClip(const juce::ValueTree& clip)
{
    if (clip_.isValid())
        clip_.removeListener(this);
    if (track_.isValid())
        track_.removeListener(this);
    clip_ = clip;
    track_ = clip.getParent();
    if (clip_.isValid())
        clip_.addListener(this);
    if (track_.isValid())
        track_.addListener(this);
    sync();
}

void AudioClipInspector::sync()
{
    if (!clip_.isValid())
        return;
    const double length = session::clip::getLength(clip_);
    fields_[3].slider.setRange(0.0, std::max(0.01, length), 0.001);
    fields_[4].slider.setRange(0.0, std::max(0.01, length), 0.001);
    fields_[0].slider.setValue(session::clip::getGainDb(clip_), juce::dontSendNotification);
    fields_[1].slider.setValue(session::clip::getStretch(clip_), juce::dontSendNotification);
    fields_[2].slider.setValue(session::clip::getPitch(clip_), juce::dontSendNotification);
    fields_[3].slider.setValue(session::clip::getFadeIn(clip_), juce::dontSendNotification);
    fields_[4].slider.setValue(session::clip::getFadeOut(clip_), juce::dontSendNotification);
    if (track_.isValid())
    {
        fields_[5].slider.setValue(session::track::getSpeed(track_), juce::dontSendNotification);
        fields_[6].slider.setValue(session::track::getPitch(track_), juce::dontSendNotification);
    }
    // An edit-locked clip is read-only (the track's own speed and pitch stay editable).
    const bool editable = session::clip::canEdit(clip_);
    for (std::size_t i = 0; i < 5; ++i)
        fields_[i].slider.setEnabled(editable);
    matchTempo_.setEnabled(editable);
    repaint();
}

void AudioClipInspector::resized()
{
    auto area = getLocalBounds().reduced(14, 34);
    auto left = area.removeFromLeft(area.getWidth() / 2).reduced(0, 0);
    auto right = area.withTrimmedLeft(20);
    for (std::size_t i = 0; i < fields_.size(); ++i)
    {
        auto& column = i < 5 ? left : right;
        auto row = column.removeFromTop(28);
        fields_[i].label.setBounds(row.removeFromLeft(130));
        fields_[i].slider.setBounds(row);
        column.removeFromTop(4);
    }
    right.removeFromTop(8);
    matchTempo_.setBounds(right.removeFromTop(28).withWidth(200));
}

void AudioClipInspector::paint(juce::Graphics& g)
{
    using namespace theme;
    g.fillAll(colour::panel);
    if (!clip_.isValid())
        return;
    g.setColour(colour::textPrimary);
    g.setFont(sansBold(14.0f));
    juce::String title = session::clip::getName(clip_);
    if (session::clip::isEditLocked(clip_))
        title << "  (edit-locked)";
    else if (session::clip::isTimeLocked(clip_))
        title << "  (time-locked)";
    g.drawText(title, getLocalBounds().reduced(14, 6).removeFromTop(20), juce::Justification::centredLeft, true);
    g.setColour(colour::textMuted);
    g.setFont(sans(11.5f));
    g.drawText(session::clip::getFile(clip_).getFullPathName() + "   start " + juce::String(session::clip::getStart(clip_), 3) + " s   length " +
                   juce::String(session::clip::getLength(clip_), 3) + " s",
               getLocalBounds().reduced(14, 6).removeFromTop(20), juce::Justification::centredRight, true);
}

//==================================================================================================
// ClipEditor

ClipEditor::ClipEditor(session::Project& project, EditorState& editor)
    : project_(project)
    , editor_(editor)
    , pianoRoll_(project.getSession(), editor)
    , steps_(project, editor)
    , inspector_(project.getSession())
{
    setOpaque(true);

    notesButton_.setButtonText("NOTES");
    stepsButton_.setButtonText("STEPS");
    for (auto* b : {&notesButton_, &stepsButton_})
    {
        b->setRadioGroupId(4242);
        b->setClickingTogglesState(true);
        b->setWantsKeyboardFocus(false);
        addAndMakeVisible(b);
    }
    notesButton_.setToggleState(true, juce::dontSendNotification);
    notesButton_.onClick = [this]
    {
        showSteps_ = false;
        showClip();
    };
    stepsButton_.onClick = [this]
    {
        showSteps_ = true;
        showClip();
    };

    const std::array<std::pair<const char*, double>, 7> grids{{{"1/4", 1.0}, {"1/8", 0.5}, {"1/16", 0.25}, {"1/32", 0.125},
                                                               {"1/8T", 1.0 / 3.0}, {"1/16T", 1.0 / 6.0}, {"1 bar", 4.0}}};
    for (std::size_t i = 0; i < grids.size(); ++i)
        gridBox_.addItem(grids[i].first, static_cast<int>(i) + 1);
    gridBox_.setSelectedId(3, juce::dontSendNotification);
    gridBox_.setTooltip("Grid (note length and quantise)");
    gridBox_.onChange = [this, grids]
    {
        const auto beats = grids[static_cast<std::size_t>(std::max(0, gridBox_.getSelectedId() - 1))].second;
        pianoRoll_.setGridBeats(beats);
        steps_.setStepBeats(beats);
    };
    addAndMakeVisible(gridBox_);

    const std::array<std::pair<const char*, int>, 9> lanes{{{"Velocity", -1},
                                                            {"Mod Wheel (CC 1)", 1},
                                                            {"Volume (CC 7)", 7},
                                                            {"Pan (CC 10)", 10},
                                                            {"Expression (CC 11)", 11},
                                                            {"Sustain (CC 64)", 64},
                                                            {"Pitch Bend", 128},
                                                            {"Channel Pressure", 129},
                                                            {"Note Expression (MPE)", PianoRoll::kNoteExpressionLane}}};
    for (std::size_t i = 0; i < lanes.size(); ++i)
        laneBox_.addItem(lanes[i].first, static_cast<int>(i) + 1);
    laneBox_.setSelectedId(1, juce::dontSendNotification);
    laneBox_.setTooltip("Lower lane: velocity, a controller, or each note's own expression (MPE: pitch, pressure, slide)");
    laneBox_.onChange = [this, lanes] { pianoRoll_.setLowerLane(lanes[static_cast<std::size_t>(std::max(0, laneBox_.getSelectedId() - 1))].second); };
    addAndMakeVisible(laneBox_);

    quantiseButton_.setButtonText("QUANTISE");
    quantiseButton_.setTooltip("Quantise the selected notes (or all) to the grid (Q)");
    quantiseButton_.onClick = [this] { pianoRoll_.quantiseSelection(strengthSlider_.getValue() / 100.0, endsToggle_.getToggleState()); };
    addAndMakeVisible(quantiseButton_);

    strengthSlider_.setSliderStyle(juce::Slider::LinearHorizontal);
    strengthSlider_.setRange(0.0, 100.0, 1.0);
    strengthSlider_.setValue(100.0, juce::dontSendNotification);
    strengthSlider_.setTextValueSuffix(" %");
    strengthSlider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 20);
    strengthSlider_.setTooltip("Quantise strength");
    addAndMakeVisible(strengthSlider_);

    endsToggle_.setButtonText("Ends");
    endsToggle_.setTooltip("Also quantise note ends");
    addAndMakeVisible(endsToggle_);

    for (int octave = 0; octave <= 8; ++octave)
        baseNoteBox_.addItem("From " + juce::MidiMessage::getMidiNoteName(octave * 12 + 12, true, true, 3), octave + 1);
    baseNoteBox_.setSelectedId(5, juce::dontSendNotification);
    baseNoteBox_.setTooltip("Lowest note of the step rows (instruments other than the Drum Rack)");
    baseNoteBox_.onChange = [this] { steps_.setBaseNote(baseNoteBox_.getSelectedId() * 12); };
    addAndMakeVisible(baseNoteBox_);

    addChildComponent(pianoRoll_);
    addChildComponent(steps_);
    addChildComponent(inspector_);
    const auto preview = [this](const juce::MidiMessage& message)
    {
        // A note-off goes where its note-on went, even if another clip was opened meanwhile.
        if (message.isNoteOn())
            previewTrack_ = clip_.isValid() && clip_.getParent().isValid() ? session::track::getId(clip_.getParent()) : 0u;
        if (onPreviewMidi && previewTrack_ != 0)
            onPreviewMidi(previewTrack_, message);
    };
    pianoRoll_.onPreview = preview;
    steps_.onPreview = preview;

    // An ARA plug-in's editor (Melodyne, VocAlign...) for the clips its track plays, or the inspector.
    for (auto* b : {&araButton_, &clipButton_})
    {
        b->setRadioGroupId(4243);
        b->setClickingTogglesState(true);
        b->setWantsKeyboardFocus(false);
        addChildComponent(b);
    }
    araButton_.setToggleState(true, juce::dontSendNotification);
    araButton_.setTooltip("The track's ARA plug-in's editor, showing this clip");
    clipButton_.setTooltip("The clip's own settings (gain, pitch, stretch...)");
    araButton_.onClick = [this]
    {
        showInspector_ = false;
        showClip();
    };
    clipButton_.onClick = [this]
    {
        showInspector_ = true;
        showClip();
    };
    araNote_.setFont(theme::sans(12.0f));
    araNote_.setColour(juce::Label::textColourId, theme::colour::textSecondary);
    addChildComponent(araNote_);

    editor_.addListener(this);
    project_.getSession().getState().addListener(this);
    project_.getAra().addChangeListener(this);
    showClip();
}

ClipEditor::~ClipEditor()
{
    project_.getAra().removeChangeListener(this);
    araEditor_.reset(); // (before the plug-in)
    project_.getSession().getState().removeListener(this);
    editor_.removeListener(this);
}

void ClipEditor::setPlayhead(double seconds)
{
    pianoRoll_.setPlayhead(seconds);
    if (steps_.isVisible())
        steps_.setPlayhead(seconds);
}

void ClipEditor::editorStateChanged()
{
    const auto clip = project_.getSession().findClip(editor_.getEditedClip());
    if (clip != clip_)
        showClip();
}

void ClipEditor::valueTreeChildRemoved(juce::ValueTree&, juce::ValueTree& child, int)
{
    if (child == clip_ || child.hasType(session::ids::track))
        juce::MessageManager::callAsync([safe = juce::Component::SafePointer<ClipEditor>(this)]
                                        {
                                            if (safe != nullptr)
                                                safe->showClip();
                                        });
}

void ClipEditor::showClip()
{
    clip_ = project_.getSession().findClip(editor_.getEditedClip());
    const bool midi = clip_.isValid() && session::clip::isMidi(clip_);
    const bool audio = clip_.isValid() && !midi;

    // An audio clip its track's ARA plug-in plays: the plug-in's editor (unless CLIP is chosen).
    const auto track = audio ? clip_.getParent() : juce::ValueTree();
    const auto ara = track.hasType(session::ids::track) ? project_.getAra().getTrackInfo(session::track::getId(track))
                                                         : session::AraHost::TrackInfo{};
    const bool araClip = audio && ara.rendering;
    showAraEditor(araClip && !showInspector_);
    araButton_.setVisible(araClip);
    clipButton_.setVisible(araClip);
    araButton_.setToggleState(!showInspector_, juce::dontSendNotification);
    clipButton_.setToggleState(showInspector_, juce::dontSendNotification);
    if (araClip)
    {
        juce::String note = ara.plugin + ": " + juce::String(ara.regions) + (ara.regions == 1 ? " clip" : " clips") + " on this track";
        if (!showInspector_ && araEditor_ == nullptr)
            note << " (its editor is open in its own window)";
        araNote_.setText(note, juce::dontSendNotification);
        project_.getAra().showSelection(session::track::getId(track), {session::clip::getId(clip_)});
    }
    araNote_.setVisible(araClip);

    pianoRoll_.setClip(midi ? clip_ : juce::ValueTree());
    steps_.setClip(midi ? clip_ : juce::ValueTree());
    inspector_.setClip(audio ? clip_ : juce::ValueTree());

    pianoRoll_.setVisible(midi && !showSteps_);
    steps_.setVisible(midi && showSteps_);
    inspector_.setVisible(audio && (!araClip || showInspector_));

    for (auto* c : std::initializer_list<juce::Component*>{&notesButton_, &stepsButton_, &gridBox_})
        c->setVisible(midi);
    for (auto* c : std::initializer_list<juce::Component*>{&laneBox_, &quantiseButton_, &strengthSlider_, &endsToggle_})
        c->setVisible(midi && !showSteps_);
    baseNoteBox_.setVisible(midi && showSteps_ && !steps_.usesDrumRack());
    resized();
    repaint();
}

void ClipEditor::showAraEditor(bool show)
{
    const auto clipTrack = clip_.isValid() ? clip_.getParent() : juce::ValueTree();
    const auto info = show && clipTrack.hasType(session::ids::track) ? project_.getAra().getTrackInfo(session::track::getId(clipTrack))
                                                                      : session::AraHost::TrackInfo{};
    if (!show || info.deviceId != araDevice_)
    {
        araEditor_.reset();
        araDevice_ = 0;
    }
    if (!show || araEditor_ != nullptr)
        return;
    araDevice_ = info.deviceId;
    if (auto node = project_.getDevices().findNode(info.deviceId))
    {
        auto& processor = node->getProcessor();
        if (processor.hasEditor() && processor.getActiveEditor() == nullptr) // (not open in a window of its own)
            araEditor_.reset(processor.createEditorAndMakeActive());
    }
    if (araEditor_ != nullptr)
        addAndMakeVisible(*araEditor_);
}

void ClipEditor::resized()
{
    auto area = getLocalBounds();
    auto toolbar = area.removeFromTop(kToolbarHeight).reduced(8, 3);
    toolbar.removeFromLeft(220);
    {
        auto araBar = toolbar;
        araButton_.setBounds(araBar.removeFromLeft(56));
        clipButton_.setBounds(araBar.removeFromLeft(56));
        araBar.removeFromLeft(12);
        araNote_.setBounds(araBar);
    }
    notesButton_.setBounds(toolbar.removeFromLeft(64));
    stepsButton_.setBounds(toolbar.removeFromLeft(64));
    toolbar.removeFromLeft(10);
    gridBox_.setBounds(toolbar.removeFromLeft(80));
    toolbar.removeFromLeft(10);
    baseNoteBox_.setBounds(toolbar.withWidth(110));
    laneBox_.setBounds(toolbar.removeFromLeft(150));
    toolbar.removeFromLeft(10);
    quantiseButton_.setBounds(toolbar.removeFromLeft(84));
    toolbar.removeFromLeft(4);
    strengthSlider_.setBounds(toolbar.removeFromLeft(150));
    toolbar.removeFromLeft(4);
    endsToggle_.setBounds(toolbar.removeFromLeft(60));

    pianoRoll_.setBounds(area);
    steps_.setBounds(area);
    inspector_.setBounds(area);
    if (araEditor_ != nullptr)
        araEditor_->setBounds(area);
}

void ClipEditor::paint(juce::Graphics& g)
{
    using namespace theme;
    g.fillAll(colour::panel);
    auto header = getLocalBounds().removeFromTop(kToolbarHeight);
    g.setColour(colour::panelHeader);
    g.fillRect(header);
    g.setColour(colour::textSecondary);
    g.setFont(label(11.0f));
    juce::String title = "EDITOR";
    if (clip_.isValid())
        title << "  -  " << session::clip::getName(clip_).toUpperCase();
    g.drawText(title, header.reduced(14, 0).withWidth(220), juce::Justification::centredLeft, true);
    g.setColour(colour::outline);
    g.fillRect(header.removeFromBottom(1));

    if (!clip_.isValid())
    {
        g.setColour(colour::textMuted);
        g.setFont(sans(12.5f));
        g.drawText("Double-click a clip in the arranger to edit it here. Double-click an instrument track's lane to create a MIDI clip.",
                   getLocalBounds().withTrimmedTop(kToolbarHeight), juce::Justification::centred, false);
    }
}

} // namespace dojo::ui
