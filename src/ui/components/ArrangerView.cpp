#include "ui/components/ArrangerView.h"

#include "core/MusicalTime.h"
#include "ui/EditActions.h"
#include "ui/theme/DojoTheme.h"

#include <algorithm>
#include <cmath>

namespace dojo::ui
{
namespace
{
constexpr int kTrackHeight = 68;
constexpr int kScrollbarThickness = 10;
constexpr int kExtraRowsBelow = 2; // empty drop zone below the last track
constexpr int kHeaderButtonWidth = 20;
constexpr float kEdgeGrabWidth = 6.0f;
constexpr float kClipTitleHeight = 16.0f;
constexpr float kFadeHandleSize = 8.0f;
constexpr double kMinPixelsPerSecond = 2.0;
constexpr double kMaxPixelsPerSecond = 2000.0;
constexpr int kBeatsPerBar = 4;

const char* const kColourNames[] = {"Vermilion", "Persimmon", "Gold",     "Matcha",
                                    "Jade",      "Indigo",    "Wisteria", "Sakura"};

juce::String formatPan(double pan)
{
    const int percent = juce::roundToInt(std::abs(pan) * 100.0);
    if (percent == 0)
        return "C";
    return (pan < 0.0 ? "L" : "R") + juce::String(percent);
}

juce::String formatDb(double db)
{
    if (db <= session::kMinusInfinityDb + 0.001)
        return "-inf dB";
    return (db > 0.0 ? "+" : "") + juce::String(db, 1) + " dB";
}
} // namespace

ArrangerView::ArrangerView(session::Project& project, engine::EngineController& controller,
                           EditorState& editor)
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
    addTrackButton_.setTooltip("Add an audio track (Ctrl+T)");
    addTrackButton_.onClick = [this]
    {
        actions::addTrack(session_, editor_);
    };
    addAndMakeVisible(addTrackButton_);

    renameEditor_ = std::make_unique<juce::TextEditor>();
    renameEditor_->setFont(theme::sansBold(13.0f));
    renameEditor_->setJustification(juce::Justification::centredLeft);
    renameEditor_->onReturnKey = [this]
    {
        finishRename(true);
    };
    renameEditor_->onEscapeKey = [this]
    {
        finishRename(false);
    };
    renameEditor_->onFocusLost = [this]
    {
        finishRename(true);
    };
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
// Geometry

juce::Rectangle<int> ArrangerView::getRulerArea() const
{
    return {theme::metrics::trackHeaderWidth, 0,
            juce::jmax(0, getWidth() - theme::metrics::trackHeaderWidth - kScrollbarThickness),
            theme::metrics::rulerHeight};
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

juce::Rectangle<int> ArrangerView::getRowBounds(int trackIndex) const
{
    return {0, theme::metrics::rulerHeight + trackIndex * kTrackHeight - scrollY_,
            getWidth() - kScrollbarThickness, kTrackHeight};
}

int ArrangerView::getTrackIndexAt(int y) const
{
    if (y < theme::metrics::rulerHeight)
        return -1;

    const int index = (y - theme::metrics::rulerHeight + scrollY_) / kTrackHeight;
    return index < session_.getNumTracks() ? index : -1;
}

float ArrangerView::timeToX(double seconds) const noexcept
{
    return static_cast<float>(theme::metrics::trackHeaderWidth) +
           static_cast<float>((seconds - scrollSeconds_) * pixelsPerSecond_);
}

double ArrangerView::xToTime(float x) const noexcept
{
    return scrollSeconds_ +
           static_cast<double>(x - static_cast<float>(theme::metrics::trackHeaderWidth)) / pixelsPerSecond_;
}

double ArrangerView::getVisibleSeconds() const noexcept
{
    return static_cast<double>(getLaneArea().getWidth()) / pixelsPerSecond_;
}

juce::Rectangle<float> ArrangerView::getClipBounds(const juce::ValueTree& clip, int trackIndex) const
{
    const auto row = getRowBounds(trackIndex).toFloat();
    const float x0 = timeToX(session::clip::getStart(clip));
    const float x1 = timeToX(session::clip::getEnd(clip));
    return {x0, row.getY() + 3.0f, juce::jmax(2.0f, x1 - x0), row.getHeight() - 6.0f};
}

juce::Rectangle<int> ArrangerView::getMuteButtonBounds(int trackIndex) const
{
    const auto row = getRowBounds(trackIndex);
    return {theme::metrics::trackHeaderWidth - 2 * kHeaderButtonWidth - 16, row.getY() + 9,
            kHeaderButtonWidth, 18};
}

juce::Rectangle<int> ArrangerView::getSoloButtonBounds(int trackIndex) const
{
    return getMuteButtonBounds(trackIndex).translated(kHeaderButtonWidth + 4, 0);
}

juce::Rectangle<int> ArrangerView::getNameBounds(int trackIndex) const
{
    const auto row = getRowBounds(trackIndex);
    return {16, row.getY() + 8, getMuteButtonBounds(trackIndex).getX() - 22, 20};
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
    const int trackIndex = getTrackIndexAt(juce::roundToInt(position.y));
    if (trackIndex < 0 || !getLaneArea().toFloat().contains(position))
        return {};

    const auto t = session_.getTrack(trackIndex);

    // Last child is drawn on top, so test in reverse.
    for (int i = t.getNumChildren() - 1; i >= 0; --i)
    {
        const auto c = t.getChild(i);
        const auto bounds = getClipBounds(c, trackIndex);
        if (!bounds.contains(position))
            continue;

        ClipHit hit{c, trackIndex, ClipZone::body};
        const float fadeInX =
            bounds.getX() + static_cast<float>(session::clip::getFadeIn(c) * pixelsPerSecond_);
        const float fadeOutX =
            bounds.getRight() - static_cast<float>(session::clip::getFadeOut(c) * pixelsPerSecond_);
        const float handleY = bounds.getY() + kClipTitleHeight;
        const bool nearHandleRow = std::abs(position.y - handleY) <= kFadeHandleSize;

        if (nearHandleRow && std::abs(position.x - fadeInX) <= kFadeHandleSize)
            hit.zone = ClipZone::fadeInHandle;
        else if (nearHandleRow && std::abs(position.x - fadeOutX) <= kFadeHandleSize)
            hit.zone = ClipZone::fadeOutHandle;
        else if (bounds.getWidth() > 3.0f * kEdgeGrabWidth && position.x - bounds.getX() <= kEdgeGrabWidth)
            hit.zone = ClipZone::leftEdge;
        else if (bounds.getWidth() > 3.0f * kEdgeGrabWidth &&
                 bounds.getRight() - position.x <= kEdgeGrabWidth)
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
    return (session_.getNumTracks() + kExtraRowsBelow) * kTrackHeight;
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

    const int addY = theme::metrics::rulerHeight + session_.getNumTracks() * kTrackHeight - scrollY_ + 10;
    addTrackButton_.setBounds(12, addY, theme::metrics::trackHeaderWidth - 24, 28);
    addTrackButton_.setVisible(addY >= theme::metrics::rulerHeight &&
                               addY + 28 <= getHeaderArea().getBottom());

    if (renameEditor_->isVisible())
        finishRename(true);

    updateScrollbars();
    repaint();
}

void ArrangerView::updateScrollbars()
{
    const double visible = getVisibleSeconds();
    horizontalScroll_.setRangeLimits(0.0, juce::jmax(getContentDuration(), scrollSeconds_ + visible),
                                     juce::dontSendNotification);
    horizontalScroll_.setCurrentRange(scrollSeconds_, visible, juce::dontSendNotification);

    const int visibleHeight = getLaneArea().getHeight();
    verticalScroll_.setRangeLimits(0.0, juce::jmax(getContentHeight(), visibleHeight),
                                   juce::dontSendNotification);
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
    setScroll(anchorTime - static_cast<double>(anchorX - static_cast<float>(lanes.getX())) / pixelsPerSecond_,
              scrollY_);
}

void ArrangerView::zoomToFit()
{
    const double end = session_.getEndTime() > 0.0 ? session_.getEndTime() : 60.0;
    pixelsPerSecond_ = juce::jlimit(kMinPixelsPerSecond, kMaxPixelsPerSecond,
                                    static_cast<double>(getLaneArea().getWidth()) / (end * 1.05));
    setScroll(0.0, scrollY_);
}

void ArrangerView::setPlayhead(SampleCount playheadSamples, double sampleRate, bool playing)
{
    const double seconds = time::samplesToSeconds(playheadSamples, sampleRate);
    const auto lanes = getLaneArea();
    const float oldX = timeToX(playheadSeconds_);
    const float newX = timeToX(seconds);

    if (std::abs(newX - oldX) < 0.25f)
        return;

    playheadSeconds_ = seconds;

    // Page-follow while playing, but only if the playhead was on screen (don't yank a user who
    // scrolled away on purpose).
    const bool wasVisible =
        oldX >= static_cast<float>(lanes.getX()) && oldX <= static_cast<float>(lanes.getRight());
    if (playing && wasVisible && newX > static_cast<float>(lanes.getRight() - 20))
    {
        setScroll(seconds - getVisibleSeconds() * 0.05, scrollY_);
        return;
    }

    repaint(juce::roundToInt(oldX) - 8, 0, 16, getHeight());
    repaint(juce::roundToInt(newX) - 8, 0, 16, getHeight());
}

//==================================================================================================
// Session changes

void ArrangerView::sessionChanged()
{
    setScroll(scrollSeconds_, scrollY_);
}

void ArrangerView::valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier&)
{
    repaint();
}

void ArrangerView::valueTreeChildAdded(juce::ValueTree&, juce::ValueTree&)
{
    sessionChanged();
}

void ArrangerView::valueTreeChildRemoved(juce::ValueTree&, juce::ValueTree&, int)
{
    sessionChanged();
}

void ArrangerView::valueTreeChildOrderChanged(juce::ValueTree&, int, int)
{
    repaint();
}

//==================================================================================================
// Painting

void ArrangerView::paint(juce::Graphics& g)
{
    g.fillAll(theme::colour::panel);

    paintLanes(g);
    paintRuler(g);
    paintHeaders(g);
    paintDropIndicator(g);
    paintPlayhead(g);

    if (session_.getNumTracks() == 0 && !dropActive_)
        paintEmptyState(g);

    // Corner between the scrollbars.
    g.setColour(theme::colour::panel);
    g.fillRect(getWidth() - kScrollbarThickness, getHeight() - kScrollbarThickness, kScrollbarThickness,
               kScrollbarThickness);
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

    const double beatSeconds = 60.0 / getTempo();
    const double barSeconds = beatSeconds * kBeatsPerBar;
    const double barPixels = barSeconds * pixelsPerSecond_;
    const double beatPixels = beatSeconds * pixelsPerSecond_;

    int labelEvery = 1;
    while (static_cast<double>(labelEvery) * barPixels < 44.0)
        labelEvery *= 2;

    const auto firstBar =
        static_cast<int>(std::floor(xToTime(static_cast<float>(ruler.getX())) / barSeconds));
    const auto lastBar =
        static_cast<int>(std::ceil(xToTime(static_cast<float>(ruler.getRight())) / barSeconds));

    g.saveState();
    g.reduceClipRegion(ruler);

    for (int bar = juce::jmax(0, firstBar); bar <= lastBar; ++bar)
    {
        const double barStart = bar * barSeconds;
        const float x = timeToX(barStart);
        const bool labelled = bar % labelEvery == 0;

        if (labelled || barPixels >= 6.0)
        {
            g.setColour(labelled ? colour::textMuted : colour::outlineStrong);
            g.drawVerticalLine(juce::roundToInt(x),
                               static_cast<float>(ruler.getBottom() - (labelled ? 12 : 7)),
                               static_cast<float>(ruler.getBottom()));
        }

        if (labelled)
        {
            g.setColour(colour::textSecondary);
            g.setFont(mono(11.0f));
            g.drawText(
                juce::String(bar + 1),
                juce::Rectangle<float>(x + 4.0f, static_cast<float>(ruler.getY()) + 3.0f, 48.0f, 14.0f),
                juce::Justification::centredLeft, false);
        }

        if (beatPixels >= 8.0)
        {
            g.setColour(colour::outlineStrong);
            for (int beat = 1; beat < kBeatsPerBar; ++beat)
                g.drawVerticalLine(juce::roundToInt(timeToX(barStart + beat * beatSeconds)),
                                   static_cast<float>(ruler.getBottom() - 4),
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

    const auto selectedTrack = editor_.getSelectedTrack();

    for (int i = 0; i < session_.getNumTracks(); ++i)
    {
        auto row = getRowBounds(i).withWidth(metrics::trackHeaderWidth);
        if (row.getBottom() < area.getY() || row.getY() > area.getBottom())
            continue;

        const auto t = session_.getTrack(i);
        const bool selected = session::track::getId(t) == selectedTrack;
        const auto trackColour = session::track::getColour(t);

        g.setColour(selected ? colour::panelHeader.brighter(0.06f) : colour::panelRaised);
        g.fillRect(row);
        g.setColour(colour::outline);
        g.fillRect(row.removeFromBottom(1));
        g.setColour(trackColour);
        g.fillRect(row.removeFromLeft(selected ? 6 : 4));

        if (!(renameEditor_->isVisible() && renamingTrackId_ == session::track::getId(t)))
        {
            g.setColour(colour::textPrimary);
            g.setFont(sansBold(13.0f));
            g.drawText(session::track::getName(t), getNameBounds(i), juce::Justification::centredLeft, true);
        }

        const auto paintToggle =
            [&g](juce::Rectangle<int> box, const char* text, bool on, juce::Colour onColour)
        {
            g.setColour(on ? onColour : colour::control);
            g.fillRoundedRectangle(box.toFloat(), 3.0f);
            g.setColour(on ? colour::windowBackground : colour::textMuted);
            g.setFont(sansBold(10.0f));
            g.drawText(text, box, juce::Justification::centred, false);
        };

        paintToggle(getMuteButtonBounds(i), "M", session::track::isMuted(t), colour::gold);
        paintToggle(getSoloButtonBounds(i), "S", session::track::isSoloed(t), colour::play);

        const int numClips = t.getNumChildren();
        g.setColour(colour::textMuted);
        g.setFont(sans(11.0f));
        g.drawText(formatDb(session::track::getVolumeDb(t)) + "   " + formatPan(session::track::getPan(t)) +
                       "   " + juce::String(numClips) + (numClips == 1 ? " clip" : " clips") +
                       (session::track::isPhaseInverted(t) ? "   phase inv" : ""),
                   juce::Rectangle<int>(16, getRowBounds(i).getY() + 34, metrics::trackHeaderWidth - 24, 18),
                   juce::Justification::centredLeft, true);
    }

    // Reorder indicator.
    if (dragMode_ == DragMode::reorderTrack && dragMoved_ && reorderTo_ >= 0)
    {
        const int boundary = reorderTo_ > reorderFrom_ ? reorderTo_ + 1 : reorderTo_;
        g.setColour(colour::accent);
        g.fillRect(0, getRowBounds(boundary).getY() - 1, metrics::trackHeaderWidth, 3);
    }

    g.restoreState();

    g.setColour(colour::outline);
    g.fillRect(metrics::trackHeaderWidth - 1, metrics::rulerHeight, 1, area.getHeight());
}

void ArrangerView::paintLanes(juce::Graphics& g) const
{
    using namespace theme;

    const auto lanes = getLaneArea();
    g.saveState();
    g.reduceClipRegion(lanes);

    g.setColour(colour::windowBackground);
    g.fillRect(lanes);

    const auto selectedTrack = editor_.getSelectedTrack();

    for (int i = 0; i < session_.getNumTracks(); ++i)
    {
        auto row = getRowBounds(i).withLeft(lanes.getX());
        if (row.getBottom() < lanes.getY() || row.getY() > lanes.getBottom())
            continue;

        const bool selected = session::track::getId(session_.getTrack(i)) == selectedTrack;
        g.setColour(selected ? colour::laneOdd.brighter(0.05f)
                             : (i % 2 == 0 ? colour::laneEven : colour::laneOdd));
        g.fillRect(row);
        g.setColour(colour::outline.withAlpha(0.6f));
        g.fillRect(row.removeFromBottom(1));
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
        g.drawVerticalLine(juce::roundToInt(timeToX(t)), static_cast<float>(lanes.getY()),
                           static_cast<float>(lanes.getBottom()));
    }

    // Clips.
    for (int i = 0; i < session_.getNumTracks(); ++i)
    {
        const auto row = getRowBounds(i);
        if (row.getBottom() < lanes.getY() || row.getY() > lanes.getBottom())
            continue;

        const auto t = session_.getTrack(i);
        const auto trackColour = session::track::getColour(t);
        for (const auto& c : t)
            paintClip(g, c, i, trackColour);
    }

    g.restoreState();
}

void ArrangerView::paintClip(juce::Graphics& g, const juce::ValueTree& c, int trackIndex,
                             juce::Colour colour) const
{
    using namespace theme;

    const auto bounds = getClipBounds(c, trackIndex);
    if (!bounds.toNearestInt().intersects(g.getClipBounds()))
        return;

    const bool selected = editor_.isClipSelected(session::clip::getId(c));
    const auto asset = project_.getAssets().find(session::clip::getFile(c));

    g.setColour(colour.withAlpha(selected ? 0.36f : 0.22f));
    g.fillRoundedRectangle(bounds, 4.0f);

    const auto title = bounds.withHeight(kClipTitleHeight);
    g.setColour(colour.withAlpha(selected ? 1.0f : 0.85f));
    g.fillRoundedRectangle(title, 4.0f);
    g.fillRect(title.withTrimmedTop(6.0f));

    g.setColour(colour::windowBackground);
    g.setFont(sansBold(10.5f));
    g.drawText(session::clip::getName(c), title.reduced(6.0f, 0.0f), juce::Justification::centredLeft, true);

    const auto body = bounds.withTrimmedTop(kClipTitleHeight);

    if (asset != nullptr)
    {
        paintWaveform(g, c, body.reduced(0.0f, 3.0f), colour.brighter(0.25f));
    }
    else
    {
        const bool missing = !session::clip::getFile(c).existsAsFile();
        g.setColour(missing ? colour::meterClip : colour::textMuted);
        g.setFont(sans(11.0f));
        g.drawText(missing ? "Missing: " + session::clip::getFile(c).getFileName()
                           : juce::String("Loading..."),
                   body.reduced(6.0f, 2.0f), juce::Justification::centredLeft, true);
    }

    // Fades: shade the attenuated part and draw the ramp.
    const float fadeInWidth = static_cast<float>(session::clip::getFadeIn(c) * pixelsPerSecond_);
    const float fadeOutWidth = static_cast<float>(session::clip::getFadeOut(c) * pixelsPerSecond_);

    if (fadeInWidth > 0.5f)
    {
        juce::Path shade;
        shade.addTriangle(body.getX(), body.getY(), body.getX() + fadeInWidth, body.getY(), body.getX(),
                          body.getBottom());
        g.setColour(juce::Colours::black.withAlpha(0.35f));
        g.fillPath(shade);
        g.setColour(colour.brighter(0.4f));
        g.drawLine(body.getX(), body.getBottom(), body.getX() + fadeInWidth, body.getY(), 1.2f);
    }

    if (fadeOutWidth > 0.5f)
    {
        juce::Path shade;
        shade.addTriangle(body.getRight() - fadeOutWidth, body.getY(), body.getRight(), body.getY(),
                          body.getRight(), body.getBottom());
        g.setColour(juce::Colours::black.withAlpha(0.35f));
        g.fillPath(shade);
        g.setColour(colour.brighter(0.4f));
        g.drawLine(body.getRight() - fadeOutWidth, body.getY(), body.getRight(), body.getBottom(), 1.2f);
    }

    // Fade handles.
    if (bounds.getWidth() > 4.0f * kFadeHandleSize)
    {
        g.setColour(selected ? colour::textPrimary : colour::textPrimary.withAlpha(0.55f));
        for (const float x : {body.getX() + fadeInWidth, body.getRight() - fadeOutWidth})
            g.fillRect(juce::Rectangle<float>(kFadeHandleSize - 2.0f, kFadeHandleSize - 2.0f)
                           .withCentre({x, body.getY()}));
    }

    g.setColour(selected ? colour::textPrimary : colour.withAlpha(0.7f));
    g.drawRoundedRectangle(bounds.reduced(0.5f), 4.0f, selected ? 1.6f : 1.0f);
}

void ArrangerView::paintWaveform(juce::Graphics& g, const juce::ValueTree& c, juce::Rectangle<float> area,
                                 juce::Colour colour) const
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
    const double framesPerSecond = asset->sampleRate;
    const double bucketsPerSecond = framesPerSecond / session::WaveformPeaks::kFramesPerBucket;
    const float midY = area.getCentreY();
    const float halfHeight = area.getHeight() * 0.5f;

    g.setColour(colour.withAlpha(0.85f));

    const auto firstX = static_cast<int>(std::floor(visible.getX()));
    const auto lastX = static_cast<int>(std::ceil(visible.getRight()));

    for (int x = firstX; x < lastX; ++x)
    {
        const double sourceT0 = offset + (xToTime(static_cast<float>(x)) - start);
        const double sourceT1 = offset + (xToTime(static_cast<float>(x + 1)) - start);
        const int b0 =
            juce::jlimit(0, numBuckets - 1, static_cast<int>(std::floor(sourceT0 * bucketsPerSecond)));
        const int b1 =
            juce::jlimit(b0 + 1, numBuckets, static_cast<int>(std::ceil(sourceT1 * bucketsPerSecond)));

        float lo = 0.0f;
        float hi = 0.0f;
        for (int ch = 0; ch < peaks.numChannels; ++ch)
        {
            const auto chIndex = static_cast<std::size_t>(ch);
            for (int b = b0; b < b1; ++b)
            {
                lo = juce::jmin(lo, peaks.minimum[chIndex][static_cast<std::size_t>(b)]);
                hi = juce::jmax(hi, peaks.maximum[chIndex][static_cast<std::size_t>(b)]);
            }
        }

        const float top = midY - juce::jlimit(0.0f, 1.0f, hi) * halfHeight;
        const float bottom = midY - juce::jlimit(-1.0f, 0.0f, lo) * halfHeight;
        g.fillRect(static_cast<float>(x), top, 1.0f, juce::jmax(1.0f, bottom - top));
    }
}

void ArrangerView::paintDropIndicator(juce::Graphics& g) const
{
    if (!dropActive_)
        return;

    const auto lanes = getLaneArea();
    g.saveState();
    g.reduceClipRegion(lanes);

    if (dropTrackIndex_ >= 0)
    {
        g.setColour(theme::colour::accent.withAlpha(0.12f));
        g.fillRect(getRowBounds(dropTrackIndex_).withLeft(lanes.getX()));
    }
    else
    {
        const int top = theme::metrics::rulerHeight + session_.getNumTracks() * kTrackHeight - scrollY_;
        const auto zone = lanes.withTop(juce::jmax(lanes.getY(), top));
        g.setColour(theme::colour::accent.withAlpha(0.08f));
        g.fillRect(zone);
        g.setColour(theme::colour::accent);
        g.setFont(theme::sansBold(13.0f));
        g.drawText("Drop to create new tracks", zone.withHeight(kTrackHeight), juce::Justification::centred,
                   false);
    }

    g.setColour(theme::colour::accent);
    g.fillRect(timeToX(dropTime_) - 1.0f, static_cast<float>(lanes.getY()), 2.0f,
               static_cast<float>(lanes.getHeight()));
    g.restoreState();
}

void ArrangerView::paintPlayhead(juce::Graphics& g) const
{
    const auto lanes = getLaneArea();
    const float x = timeToX(playheadSeconds_);
    if (x < static_cast<float>(lanes.getX()) || x > static_cast<float>(lanes.getRight()))
        return;

    g.setColour(theme::colour::playhead);
    g.fillRect(x - 0.5f, static_cast<float>(theme::metrics::rulerHeight), 1.5f,
               static_cast<float>(lanes.getBottom() - theme::metrics::rulerHeight));

    juce::Path marker;
    const auto bottom = static_cast<float>(theme::metrics::rulerHeight);
    marker.addTriangle(x - 6.0f, bottom - 10.0f, x + 6.0f, bottom - 10.0f, x, bottom);
    g.fillPath(marker);
}

void ArrangerView::paintEmptyState(juce::Graphics& g) const
{
    auto area = getLaneArea().withSizeKeepingCentre(juce::jmin(560, getLaneArea().getWidth() - 40), 120);

    g.setColour(theme::colour::outlineStrong);
    g.drawRoundedRectangle(area.toFloat(), 8.0f, 1.0f);

    g.setColour(theme::colour::textPrimary);
    g.setFont(theme::sansBold(16.0f));
    g.drawText("Drop audio files here", area.removeFromTop(64).withTrimmedTop(16),
               juce::Justification::centred, false);

    g.setColour(theme::colour::textMuted);
    g.setFont(theme::sans(12.5f));
    g.drawText("WAV, AIFF, FLAC, MP3, OGG  |  or File > Import Audio (Ctrl+I)  |  new track: Ctrl+T",
               area.withTrimmedBottom(18), juce::Justification::centred, true);
}

//==================================================================================================
// Mouse

void ArrangerView::locateTo(double seconds)
{
    const double rate = controller_.getSampleRateOr(48000.0);
    controller_.locate(time::secondsToSamples(juce::jmax(0.0, seconds), rate));
    playheadSeconds_ = juce::jmax(0.0, seconds);
    repaint();
}

void ArrangerView::mouseDown(const juce::MouseEvent& e)
{
    grabKeyboardFocus();

    dragMode_ = DragMode::none;
    dragMoved_ = false;
    dragStart_ = e.position;
    const auto position = e.getPosition();

    if (getRulerArea().contains(position))
    {
        dragMode_ = DragMode::locate;
        locateTo(xToTime(e.position.x));
        return;
    }

    const int trackIndex = getTrackIndexAt(position.y);

    if (getHeaderArea().contains(position))
    {
        if (trackIndex < 0)
            return;

        const auto t = session_.getTrack(trackIndex);
        editor_.selectTrack(session::track::getId(t));

        if (e.mods.isPopupMenu())
            showTrackMenu(trackIndex);
        else if (getMuteButtonBounds(trackIndex).contains(position))
            toggleMute(trackIndex);
        else if (getSoloButtonBounds(trackIndex).contains(position))
            toggleSolo(trackIndex);
        else
        {
            dragMode_ = DragMode::reorderTrack;
            reorderFrom_ = trackIndex;
            reorderTo_ = trackIndex;
        }
        return;
    }

    if (!getLaneArea().contains(position))
        return;

    const auto hit = hitTestClip(e.position);

    if (hit.clip.isValid())
    {
        const auto clipId = session::clip::getId(hit.clip);

        if (e.mods.isCommandDown())
            editor_.toggleClip(clipId);
        else if (e.mods.isShiftDown())
            editor_.selectClip(clipId, true);
        else if (!editor_.isClipSelected(clipId))
            editor_.selectClip(clipId, false);

        editor_.selectTrack(session::track::getId(session_.getTrack(hit.trackIndex)));

        if (e.mods.isPopupMenu())
            showClipMenu(hit.clip);
        else if (editor_.isClipSelected(clipId))
            beginClipDrag(hit, e);

        return;
    }

    // Empty space.
    const double seconds = xToTime(e.position.x);

    if (trackIndex >= 0)
        editor_.selectTrack(session::track::getId(session_.getTrack(trackIndex)));

    if (e.mods.isPopupMenu())
    {
        showLaneMenu(trackIndex, seconds);
        return;
    }

    editor_.clearClipSelection();
    locateTo(snap(seconds, e.mods));
}

void ArrangerView::beginClipDrag(const ClipHit& hit, const juce::MouseEvent&)
{
    switch (hit.zone)
    {
    case ClipZone::body:
        dragMode_ = DragMode::moveClips;
        break;
    case ClipZone::leftEdge:
        dragMode_ = DragMode::trimStart;
        break;
    case ClipZone::rightEdge:
        dragMode_ = DragMode::trimEnd;
        break;
    case ClipZone::fadeInHandle:
        dragMode_ = DragMode::fadeIn;
        break;
    case ClipZone::fadeOutHandle:
        dragMode_ = DragMode::fadeOut;
        break;
    }

    dragAnchorId_ = session::clip::getId(hit.clip);
    dragOriginals_.clear();

    const auto snapshotOf = [this](const juce::ValueTree& c)
    {
        ClipSnapshot s;
        s.id = session::clip::getId(c);
        s.trackIndex = session_.indexOf(c.getParent());
        s.start = session::clip::getStart(c);
        s.length = session::clip::getLength(c);
        s.offset = session::clip::getOffset(c);
        s.fadeIn = session::clip::getFadeIn(c);
        s.fadeOut = session::clip::getFadeOut(c);
        return s;
    };

    dragOriginals_.push_back(snapshotOf(hit.clip)); // anchor first

    if (dragMode_ == DragMode::moveClips)
        for (const auto id : editor_.getSelectedClips())
            if (id != dragAnchorId_)
                if (auto c = session_.findClip(id); c.isValid())
                    dragOriginals_.push_back(snapshotOf(c));

    const auto asset = project_.getAssets().find(session::clip::getFile(hit.clip));
    dragSourceLength_ = asset != nullptr ? asset->getLengthSeconds() : 1.0e9;
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

    case DragMode::reorderTrack:
    {
        if (!dragMoved_ && std::abs(e.position.y - dragStart_.y) < 4.0f)
            return;

        dragMoved_ = true;
        const int numTracks = session_.getNumTracks();
        const int hovered = (e.getPosition().y - theme::metrics::rulerHeight + scrollY_) / kTrackHeight;
        reorderTo_ = juce::jlimit(0, numTracks - 1, hovered);
        repaint(getHeaderArea());
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
        case DragMode::trimEnd:
            name = "Trim Clip";
            break;
        case DragMode::fadeIn:
        case DragMode::fadeOut:
            name = "Change Fade";
            break;
        case DragMode::moveClips:
            name = dragOriginals_.size() > 1 ? "Move Clips" : "Move Clip";
            break;
        case DragMode::none:
        case DragMode::locate:
        case DragMode::reorderTrack:
            break;
        }
        session_.beginTransaction(name);
    }

    auto* um = &session_.getUndoManager();
    const double deltaSeconds = static_cast<double>(e.position.x - dragStart_.x) / pixelsPerSecond_;
    const auto& anchor = dragOriginals_.front();
    auto anchorClip = session_.findClip(anchor.id);
    if (!anchorClip.isValid())
        return;

    switch (dragMode_)
    {
    case DragMode::moveClips:
    {
        double delta = snap(anchor.start + deltaSeconds, e.mods) - anchor.start;
        int minTrack = anchor.trackIndex;
        int maxTrack = anchor.trackIndex;
        for (const auto& o : dragOriginals_)
        {
            delta = juce::jmax(delta, -o.start); // nothing may move before zero
            minTrack = juce::jmin(minTrack, o.trackIndex);
            maxTrack = juce::jmax(maxTrack, o.trackIndex);
        }

        const int numTracks = session_.getNumTracks();
        const int hovered = (e.getPosition().y - theme::metrics::rulerHeight + scrollY_) / kTrackHeight;
        const int rowDelta = juce::jlimit(-minTrack, numTracks - 1 - maxTrack, hovered - anchor.trackIndex);

        for (const auto& o : dragOriginals_)
            if (auto c = session_.findClip(o.id); c.isValid())
                session_.moveClip(c, session_.getTrack(o.trackIndex + rowDelta), o.start + delta);
        break;
    }

    case DragMode::trimStart:
    {
        const double end = anchor.start + anchor.length;
        const double earliest = juce::jmax(0.0, anchor.start - anchor.offset);
        const double newStart =
            juce::jlimit(earliest, end - session::kMinClipLength, snap(anchor.start + deltaSeconds, e.mods));
        session::clip::setStart(anchorClip, newStart, um);
        session::clip::setOffset(anchorClip, anchor.offset + (newStart - anchor.start), um);
        session::clip::setLength(anchorClip, end - newStart, um);
        session::clip::setFadeIn(anchorClip, juce::jmin(anchor.fadeIn, end - newStart), um);
        break;
    }

    case DragMode::trimEnd:
    {
        const double latest =
            anchor.start + juce::jmax(session::kMinClipLength, dragSourceLength_ - anchor.offset);
        const double newEnd = juce::jlimit(anchor.start + session::kMinClipLength, latest,
                                           snap(anchor.start + anchor.length + deltaSeconds, e.mods));
        session::clip::setLength(anchorClip, newEnd - anchor.start, um);
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
    case DragMode::reorderTrack:
        break;
    }
}

void ArrangerView::mouseUp(const juce::MouseEvent& e)
{
    if (dragMode_ == DragMode::reorderTrack && dragMoved_ && reorderTo_ >= 0 && reorderTo_ != reorderFrom_)
    {
        session_.beginTransaction("Move Track");
        session_.moveTrack(reorderFrom_, reorderTo_);
    }

    // A plain click on a clip that was part of a multi-selection selects just that clip.
    if (dragMode_ == DragMode::moveClips && !dragMoved_ && !e.mods.isAnyModifierKeyDown())
        editor_.selectClip(dragAnchorId_, false);

    dragMode_ = DragMode::none;
    dragMoved_ = false;
    dragOriginals_.clear();
    repaint();
}

void ArrangerView::mouseMove(const juce::MouseEvent& e)
{
    const auto hit = hitTestClip(e.position);
    auto newCursor = juce::MouseCursor::NormalCursor;

    if (hit.clip.isValid())
    {
        switch (hit.zone)
        {
        case ClipZone::leftEdge:
            newCursor = juce::MouseCursor::LeftEdgeResizeCursor;
            break;
        case ClipZone::rightEdge:
            newCursor = juce::MouseCursor::RightEdgeResizeCursor;
            break;
        case ClipZone::fadeInHandle:
        case ClipZone::fadeOutHandle:
            newCursor = juce::MouseCursor::LeftRightResizeCursor;
            break;
        case ClipZone::body:
            newCursor = juce::MouseCursor::DraggingHandCursor;
            break;
        }
    }
    else if (getRulerArea().contains(e.getPosition()))
    {
        newCursor = juce::MouseCursor::PointingHandCursor;
    }

    setMouseCursor(newCursor);
}

void ArrangerView::mouseDoubleClick(const juce::MouseEvent& e)
{
    const auto position = e.getPosition();
    const int trackIndex = getTrackIndexAt(position.y);

    if (getHeaderArea().contains(position) && trackIndex >= 0 && getNameBounds(trackIndex).contains(position))
    {
        startRename(trackIndex);
        return;
    }

    if (getLaneArea().contains(position) && trackIndex < 0 && !hitTestClip(e.position).clip.isValid())
        actions::addTrack(session_, editor_);
}

void ArrangerView::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (e.mods.isCommandDown())
    {
        const auto lanes = getLaneArea();
        const float anchorX = juce::jlimit(static_cast<float>(lanes.getX()),
                                           static_cast<float>(lanes.getRight()), e.position.x);
        const double anchorTime = xToTime(anchorX);
        pixelsPerSecond_ =
            juce::jlimit(kMinPixelsPerSecond, kMaxPixelsPerSecond,
                         pixelsPerSecond_ * std::pow(1.0025, static_cast<double>(wheel.deltaY) * 256.0));
        setScroll(anchorTime -
                      static_cast<double>(anchorX - static_cast<float>(lanes.getX())) / pixelsPerSecond_,
                  scrollY_);
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

//==================================================================================================
// Track header helpers

void ArrangerView::toggleMute(int trackIndex)
{
    auto t = session_.getTrack(trackIndex);
    session_.beginTransaction("Mute");
    session::track::setMuted(t, !session::track::isMuted(t), &session_.getUndoManager());
}

void ArrangerView::toggleSolo(int trackIndex)
{
    auto t = session_.getTrack(trackIndex);
    session_.beginTransaction("Solo");
    session::track::setSoloed(t, !session::track::isSoloed(t), &session_.getUndoManager());
}

void ArrangerView::startRename(int trackIndex)
{
    const auto t = session_.getTrack(trackIndex);
    renamingTrackId_ = session::track::getId(t);
    renameEditor_->setBounds(getNameBounds(trackIndex).expanded(4, 2));
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
    {
        if (auto t = session_.findTrack(trackId); t.isValid())
        {
            const auto newName = renameEditor_->getText().trim();
            if (newName.isNotEmpty() && newName != session::track::getName(t))
            {
                session_.beginTransaction("Rename Track");
                session::track::setName(t, newName, &session_.getUndoManager());
            }
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
    const bool playheadInside =
        playhead > session::clip::getStart(clipTree) && playhead < session::clip::getEnd(clipTree);

    juce::PopupMenu menu;
    menu.addSectionHeader(session::clip::getName(clipTree));
    menu.addItem("Split at Playhead", playheadInside, false,
                 [safe, playhead]
                 {
                     if (safe != nullptr)
                         actions::splitAt(safe->session_, safe->editor_, playhead);
                 });
    menu.addItem("Duplicate",
                 [safe]
                 {
                     if (safe != nullptr)
                         actions::duplicateSelectedClips(safe->session_, safe->editor_);
                 });
    menu.addItem("Remove Fades",
                 [safe, clipId]
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
    menu.addSeparator();
    menu.addItem("Delete",
                 [safe]
                 {
                     if (safe != nullptr)
                         actions::deleteSelectedClips(safe->session_, safe->editor_);
                 });

    menu.showMenuAsync(juce::PopupMenu::Options().withMousePosition());
}

void ArrangerView::showTrackMenu(int trackIndex)
{
    juce::Component::SafePointer<ArrangerView> safe(this);
    const auto t = session_.getTrack(trackIndex);
    const auto trackId = session::track::getId(t);
    const int numTracks = session_.getNumTracks();

    juce::PopupMenu colours;
    const auto& palette = session::getTrackPalette();
    for (std::size_t i = 0; i < palette.size(); ++i)
    {
        juce::PopupMenu::Item item(kColourNames[i]);
        item.colour = palette[i];
        item.isTicked = session::track::getColour(t) == palette[i];
        item.action = [safe, trackId, colour = palette[i]]
        {
            if (safe == nullptr)
                return;
            if (auto tree = safe->session_.findTrack(trackId); tree.isValid())
            {
                safe->session_.beginTransaction("Track Colour");
                session::track::setColour(tree, colour, &safe->session_.getUndoManager());
            }
        };
        colours.addItem(item);
    }

    juce::PopupMenu menu;
    menu.addSectionHeader(session::track::getName(t));
    menu.addItem("Rename",
                 [safe, trackIndex]
                 {
                     if (safe != nullptr)
                         safe->startRename(trackIndex);
                 });
    menu.addSubMenu("Colour", colours);
    menu.addSeparator();
    menu.addItem("Add Track Below",
                 [safe]
                 {
                     if (safe != nullptr)
                         actions::addTrack(safe->session_, safe->editor_);
                 });
    menu.addItem("Move Up", trackIndex > 0, false,
                 [safe, trackIndex]
                 {
                     if (safe == nullptr)
                         return;
                     safe->session_.beginTransaction("Move Track");
                     safe->session_.moveTrack(trackIndex, trackIndex - 1);
                 });
    menu.addItem("Move Down", trackIndex < numTracks - 1, false,
                 [safe, trackIndex]
                 {
                     if (safe == nullptr)
                         return;
                     safe->session_.beginTransaction("Move Track");
                     safe->session_.moveTrack(trackIndex, trackIndex + 1);
                 });
    menu.addSeparator();
    menu.addItem("Delete Track",
                 [safe]
                 {
                     if (safe != nullptr)
                         actions::deleteSelectedTrack(safe->session_, safe->editor_);
                 });

    menu.showMenuAsync(juce::PopupMenu::Options().withMousePosition());
}

void ArrangerView::showLaneMenu(int trackIndex, double seconds)
{
    juce::Component::SafePointer<ArrangerView> safe(this);

    juce::PopupMenu menu;
    menu.addItem("Add Audio Track",
                 [safe]
                 {
                     if (safe != nullptr)
                         actions::addTrack(safe->session_, safe->editor_);
                 });
    menu.addItem("Import Audio...",
                 [safe]
                 {
                     if (safe != nullptr && safe->onImportRequested)
                         safe->onImportRequested();
                 });
    menu.addItem("Split Here", trackIndex >= 0, false,
                 [safe, seconds]
                 {
                     if (safe != nullptr)
                         actions::splitAt(safe->session_, safe->editor_, seconds);
                 });

    menu.showMenuAsync(juce::PopupMenu::Options().withMousePosition());
}

//==================================================================================================
// Drag and drop

void ArrangerView::updateDropTarget(int x, int y)
{
    dropActive_ = true;
    dropTrackIndex_ = getTrackIndexAt(y);
    dropTime_ = editor_.snap(juce::jmax(0.0, xToTime(static_cast<float>(x))), getTempo());
    repaint();
}

void ArrangerView::dropFiles(const juce::StringArray& files)
{
    juce::StringArray audioFiles;
    for (const auto& path : files)
        if (project_.getAssets().canRead(juce::File(path)))
            audioFiles.add(path);

    const auto target = dropTrackIndex_ >= 0 ? session_.getTrack(dropTrackIndex_) : juce::ValueTree();
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
    updateDropTarget(x, y);
}

void ArrangerView::fileDragMove(const juce::StringArray&, int x, int y)
{
    updateDropTarget(x, y);
}

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
    return details.description.toString() == kBrowserDragDescription;
}

void ArrangerView::itemDragEnter(const SourceDetails& details)
{
    updateDropTarget(details.localPosition.x, details.localPosition.y);
}

void ArrangerView::itemDragMove(const SourceDetails& details)
{
    updateDropTarget(details.localPosition.x, details.localPosition.y);
}

void ArrangerView::itemDragExit(const SourceDetails&)
{
    dropActive_ = false;
    repaint();
}

void ArrangerView::itemDropped(const SourceDetails& details)
{
    updateDropTarget(details.localPosition.x, details.localPosition.y);

    juce::StringArray files;
    if (auto* tree = dynamic_cast<juce::FileTreeComponent*>(details.sourceComponent.get()))
        for (int i = 0; i < tree->getNumSelectedFiles(); ++i)
            if (const auto f = tree->getSelectedFile(i); f.existsAsFile())
                files.add(f.getFullPathName());

    dropFiles(files);
}

} // namespace dojo::ui
