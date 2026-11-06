#include "ui/components/ClipEditor.h"

#include "devices/Impulse.h"
#include "devices/Instruments.h"
#include "ui/theme/DojoTheme.h"

#include <cmath>

namespace dojo::ui
{
namespace
{
constexpr int kKeyboardWidth = 56;
constexpr int kRulerHeight = 18;
constexpr int kLaneHeight = 80;
constexpr int kToolbarHeight = 30;
constexpr float kResizeGrab = 6.0f;

bool isBlackKey(int pitch)
{
    const int n = ((pitch % 12) + 12) % 12;
    return n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
}

double beatsPerSecond(const session::Session& session) { return session.getTempo() / 60.0; }
} // namespace

//==================================================================================================
// PianoRoll

PianoRoll::PianoRoll(session::Session& session, EditorState& editor)
    : session_(session)
    , editor_(editor)
{
    setWantsKeyboardFocus(true);
    setOpaque(true);
}

PianoRoll::~PianoRoll()
{
    if (clip_.isValid())
        clip_.removeListener(this);
}

void PianoRoll::setClip(const juce::ValueTree& midiClip)
{
    if (clip_ == midiClip)
        return;
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
    repaint();
}

double PianoRoll::gridBeats() const { return gridBeats_; }
double PianoRoll::contentStart() const { return clip_.isValid() ? session::clip::getOffset(clip_) : 0.0; }
double PianoRoll::contentEnd() const
{
    return clip_.isValid() ? contentStart() + session::clip::getLength(clip_) * beatsPerSecond(session_) : 4.0;
}

juce::Rectangle<int> PianoRoll::keyboardArea() const
{
    return {0, kRulerHeight, kKeyboardWidth, std::max(0, getHeight() - kRulerHeight - kLaneHeight - 4)};
}
juce::Rectangle<int> PianoRoll::gridArea() const
{
    return {kKeyboardWidth, kRulerHeight, std::max(0, getWidth() - kKeyboardWidth), std::max(0, getHeight() - kRulerHeight - kLaneHeight - 4)};
}
juce::Rectangle<int> PianoRoll::laneArea() const
{
    return {kKeyboardWidth, getHeight() - kLaneHeight, std::max(0, getWidth() - kKeyboardWidth), kLaneHeight};
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
    repaint();
}

void PianoRoll::resized()
{
    if (clip_.isValid() && pixelsPerBeat_ < 1.0)
        zoomToFit();
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
    if (selection_.empty())
        return;
    session_.beginTransaction(selection_.size() == 1 ? "Delete Note" : "Delete Notes");
    const auto notes = selection_;
    for (const auto& n : notes)
        session_.removeNote(n);
    selection_.clear();
}

void PianoRoll::moveSelection(double beats, int semitones)
{
    if (selection_.empty())
        return;
    session_.beginTransaction("Move Notes");
    auto* um = &session_.getUndoManager();
    for (auto& n : selection_)
    {
        session::note::setStart(n, session::note::getStart(n) + beats, um);
        session::note::setNote(n, session::note::getNote(n) + semitones, um);
    }
}

void PianoRoll::quantiseSelection(double strength, bool ends)
{
    if (!clip_.isValid())
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
        g.setColour(black ? juce::Colour(0xff1b1d22) : juce::Colour(0xffd8d4cc));
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

    // Beats and bars.
    const double first = std::floor(xToBeat(static_cast<float>(grid.getX())));
    const double last = xToBeat(static_cast<float>(grid.getRight()));
    const double step = gridBeats() * pixelsPerBeat_ >= 6.0 ? gridBeats() : 1.0;
    for (double b = first; b <= last; b += step)
    {
        const bool bar = std::abs(std::fmod(b, 4.0)) < 1.0e-6;
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
    }

    // Playhead.
    const auto parent = clip_.getParent();
    const double playheadBeat = contentStart() + (playheadSeconds_ - session::clip::getStart(clip_)) * beatsPerSecond(session_);
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
    for (double b = std::ceil(first); b <= last; b += 1.0)
    {
        if (pixelsPerBeat_ < 20.0 && std::abs(std::fmod(b, 4.0)) > 1.0e-6)
            continue;
        const int bar = static_cast<int>(std::floor(b / 4.0)) + 1;
        const int beatInBar = static_cast<int>(std::round(b - (bar - 1) * 4.0)) + 1;
        g.drawText(juce::String(bar) + "." + juce::String(beatInBar), juce::Rectangle<float>(beatToX(b) + 2.0f, 2.0f, 40.0f, 14.0f),
                   juce::Justification::centredLeft, false);
    }

    // Lower lane.
    const auto lane = laneArea();
    g.setColour(colour::panel);
    g.fillRect(lane.withX(0).withWidth(getWidth()));
    g.setColour(colour::outline);
    g.fillRect(0, lane.getY() - 4, getWidth(), 4);
    g.setColour(colour::textMuted);
    g.setFont(label(8.5f));
    g.drawText(lowerLane_ < 0 ? "VELOCITY" : lowerLane_ == 128 ? "PITCH BEND" : "CC " + juce::String(lowerLane_),
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
}

void PianoRoll::mouseDown(const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    if (!clip_.isValid())
        return;

    drag_ = Drag::none;
    dragChanged_ = false;
    dragStart_ = e.position;
    dragNotes_.clear();

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
    if (drag_ == Drag::lane)
    {
        editLane(e, e.mods.isPopupMenu());
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
    drag_ = Drag::none;
    dragNotes_.clear();
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
    if (clip_.isValid())
        clip_.removeListener(this);
}

void StepSequencer::setClip(const juce::ValueTree& midiClip)
{
    if (clip_ == midiClip)
        return;
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
    const auto devices = session::Session::getDevices(clip_.getParent());
    if (devices.empty())
        return false;
    const auto type = session::device::getType(devices.front());
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
        const auto devices = session::Session::getDevices(clip_.getParent());
        if (auto node = project_.getDevices().findNode(session::device::getId(devices.front())))
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
    const double beats = session::clip::getLength(clip_) * project_.getSession().getTempo() / 60.0;
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
    const double bps = project_.getSession().getTempo() / 60.0;
    const double playBeat = (playheadSeconds_ - session::clip::getStart(clip_)) * bps;
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
        const auto label = juce::Rectangle<int>(0, cellBounds(row, 0).getY(), kRowLabelWidth, cellBounds(row, 0).getHeight());
        g.setColour(row % 2 == 0 ? colour::panelRaised : colour::panel);
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
    if (!clip_.isValid() || !cellAt(e.getPosition(), row, step))
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
    g.drawText(session::clip::getName(clip_), getLocalBounds().reduced(14, 6).removeFromTop(20), juce::Justification::centredLeft, true);
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

    const std::array<std::pair<const char*, int>, 7> lanes{{{"Velocity", -1}, {"Mod Wheel (CC 1)", 1}, {"Volume (CC 7)", 7}, {"Pan (CC 10)", 10},
                                                            {"Expression (CC 11)", 11}, {"Sustain (CC 64)", 64}, {"Pitch Bend", 128}}};
    for (std::size_t i = 0; i < lanes.size(); ++i)
        laneBox_.addItem(lanes[i].first, static_cast<int>(i) + 1);
    laneBox_.setSelectedId(1, juce::dontSendNotification);
    laneBox_.setTooltip("Lower lane: velocity or a controller");
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

    editor_.addListener(this);
    project_.getSession().getState().addListener(this);
    showClip();
}

ClipEditor::~ClipEditor()
{
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

    pianoRoll_.setClip(midi ? clip_ : juce::ValueTree());
    steps_.setClip(midi ? clip_ : juce::ValueTree());
    inspector_.setClip(audio ? clip_ : juce::ValueTree());

    pianoRoll_.setVisible(midi && !showSteps_);
    steps_.setVisible(midi && showSteps_);
    inspector_.setVisible(audio);

    for (auto* c : std::initializer_list<juce::Component*>{&notesButton_, &stepsButton_, &gridBox_})
        c->setVisible(midi);
    for (auto* c : std::initializer_list<juce::Component*>{&laneBox_, &quantiseButton_, &strengthSlider_, &endsToggle_})
        c->setVisible(midi && !showSteps_);
    baseNoteBox_.setVisible(midi && showSteps_ && !steps_.usesDrumRack());
    resized();
    repaint();
}

void ClipEditor::resized()
{
    auto area = getLocalBounds();
    auto toolbar = area.removeFromTop(kToolbarHeight).reduced(8, 3);
    toolbar.removeFromLeft(220);
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
