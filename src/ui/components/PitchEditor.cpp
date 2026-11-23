#include "ui/components/PitchEditor.h"

#include "ui/theme/DojoTheme.h"

#include <algorithm>
#include <cmath>

namespace dojo::ui
{
namespace
{
constexpr int kToolbarHeight = 30;
constexpr int kInspectorHeight = 26;
constexpr int kScrollBarSize = 12;
constexpr int kKeysWidth = 46;
constexpr int kRulerHeight = 16;
constexpr double kHighestPitch = 112.0;
constexpr double kLowestPitch = 20.0;

using audioedit::pitch::Mode;
namespace edits = session::audioedits;

juce::String noteName(double midi)
{
    const int nearest = static_cast<int>(std::lround(midi));
    const int cents = static_cast<int>(std::lround((midi - nearest) * 100.0));
    juce::String name = juce::MidiMessage::getMidiNoteName(nearest, true, true, 4);
    if (cents != 0)
        name << (cents > 0 ? " +" : " ") << cents << " ct";
    return name;
}

bool isBlackKey(int pitch)
{
    const int n = ((pitch % 12) + 12) % 12;
    return n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
}
} // namespace

PitchEditor::PitchEditor(session::Project& project)
    : session_(project.getSession())
    , edits_(project.getAudioEdits())
{
    setWantsKeyboardFocus(true);
    setOpaque(true);

    modeBox_.addItem("Melodic", 1);
    modeBox_.addItem("Polyphonic", 2);
    modeBox_.setSelectedId(1, juce::dontSendNotification);
    modeBox_.setTooltip("Melodic: one voice (vocals, bass, a solo); Polyphonic: chords (piano, guitar)");
    addAndMakeVisible(modeBox_);

    analyseButton_.setTooltip("Find the clip's notes");
    analyseButton_.onClick = [this] { analyse(modeBox_.getSelectedId() == 2 ? Mode::polyphonic : Mode::melodic); };
    addAndMakeVisible(analyseButton_);

    correctButton_.setTooltip("Move the selected notes (or all) towards the scale by Pitch, and straighten their drift by Drift");
    correctButton_.onClick = [this] { correct(pitchAmount_.getValue() / 100.0, driftAmount_.getValue() / 100.0); };
    addAndMakeVisible(correctButton_);
    for (auto* s : {&pitchAmount_, &driftAmount_})
    {
        s->setSliderStyle(juce::Slider::LinearHorizontal);
        s->setRange(0.0, 100.0, 1.0);
        s->setTextBoxStyle(juce::Slider::TextBoxRight, false, 70, 20);
        addAndMakeVisible(s);
    }
    pitchAmount_.setTextValueSuffix(" % pitch");
    driftAmount_.setTextValueSuffix(" % drift");
    pitchAmount_.setValue(100.0, juce::dontSendNotification);
    driftAmount_.setValue(50.0, juce::dontSendNotification);
    pitchAmount_.setTooltip("Correct: how far each note's centre moves to the scale");
    driftAmount_.setTooltip("Correct: how much each note's drift is straightened");

    for (int pc = 0; pc < 12; ++pc)
        rootBox_.addItem(juce::MidiMessage::getMidiNoteName(pc, true, false, 4), pc + 1);
    rootBox_.setSelectedId(1, juce::dontSendNotification);
    rootBox_.setTooltip("The scale's root");
    const char* scales[] = {"Chromatic", "Major", "Natural minor", "Harmonic minor", "Major pentatonic", "Minor pentatonic"};
    for (int i = 0; i < 6; ++i)
        scaleBox_.addItem(scales[i], i + 1);
    scaleBox_.setSelectedId(1, juce::dontSendNotification);
    scaleBox_.setTooltip("Notes snap and correct to this scale");
    const auto scaleChanged = [this]
    {
        setScale({rootBox_.getSelectedId() - 1, static_cast<edits::ScaleKind>(std::max(0, scaleBox_.getSelectedId() - 1))});
    };
    rootBox_.onChange = scaleChanged;
    scaleBox_.onChange = scaleChanged;
    addAndMakeVisible(rootBox_);
    addAndMakeVisible(scaleBox_);

    formantsButton_.setTooltip("Keep a voice's formants (its character) where they are when its pitch moves");
    formantsButton_.onClick = [this]
    {
        if (!clip_.isValid())
            return;
        session_.beginTransaction("Keep Formants");
        edits::setKeepsFormants(clip_, formantsButton_.getToggleState(), &session_.getUndoManager());
    };
    addAndMakeVisible(formantsButton_);

    originalButton_.setClickingTogglesState(true);
    originalButton_.setTooltip("Play the clip as it was recorded (A/B)");
    originalButton_.onClick = [this] { edits_.setBypassed(clip_, originalButton_.getToggleState()); };
    addAndMakeVisible(originalButton_);

    removeButton_.setTooltip("Remove the pitch edits: the clip plays its own file again");
    removeButton_.onClick = [this]
    {
        edits_.removePitch(clip_);
        selection_.clear();
    };
    addAndMakeVisible(removeButton_);

    for (auto* l : {&status_, &selectionInfo_})
    {
        l->setFont(theme::sans(12.0f));
        l->setColour(juce::Label::textColourId, theme::colour::textSecondary);
        addAndMakeVisible(l);
    }

    driftSlider_.setRange(0.0, 200.0, 1.0);
    driftSlider_.setTextValueSuffix(" % drift");
    driftSlider_.setTooltip("The selected notes' pitch drift: 0 % straight, 100 % as performed, 200 % exaggerated");
    driftSlider_.onDragStart = [this] { session_.beginTransaction("Change Drift"); };
    driftSlider_.onValueChange = [this]
    {
        if (!driftSlider_.isMouseButtonDown())
            session_.beginTransaction("Change Drift");
        setDriftOfSelection(driftSlider_.getValue() / 100.0);
    };
    gainSlider_.setRange(-24.0, 12.0, 0.1);
    gainSlider_.setTextValueSuffix(" dB");
    gainSlider_.setTooltip("The selected notes' level");
    gainSlider_.onDragStart = [this] { session_.beginTransaction("Change Note Level"); };
    gainSlider_.onValueChange = [this]
    {
        if (!gainSlider_.isMouseButtonDown())
            session_.beginTransaction("Change Note Level");
        setGainOfSelection(gainSlider_.getValue());
    };
    for (auto* s : {&driftSlider_, &gainSlider_})
    {
        s->setSliderStyle(juce::Slider::LinearHorizontal);
        s->setTextBoxStyle(juce::Slider::TextBoxRight, false, 86, 20);
        addAndMakeVisible(s);
    }

    for (auto* bar : {&hScroll_, &vScroll_})
    {
        bar->setAutoHide(false);
        bar->addListener(this);
        addAndMakeVisible(bar);
    }

    edits_.addChangeListener(this);
    session_.getState().addListener(this);
    updateControls();
}

PitchEditor::~PitchEditor()
{
    session_.getState().removeListener(this);
    edits_.removeChangeListener(this);
}

void PitchEditor::setClip(const juce::ValueTree& clip)
{
    if (clip == clip_)
        return;
    clip_ = clip;
    selection_.clear();
    drag_ = Drag::none;
    if (clip_.isValid())
        modeBox_.setSelectedId(edits::hasPitch(clip_) && edits::getPitchMode(clip_) == Mode::polyphonic ? 2 : 1, juce::dontSendNotification);
    fitView();
    updateControls();
    repaint();
}

void PitchEditor::setPlayhead(double timelineSeconds)
{
    const double t = clip_.isValid() ? timelineSeconds - session::clip::getStart(clip_) : -1.0;
    if (std::abs(t - playhead_) < 1.0e-3)
        return;
    playhead_ = t;
    repaint();
}

void PitchEditor::analyse(Mode mode)
{
    juce::String error;
    if (!edits_.analysePitch(clip_, mode, error))
    {
        status_.setText(error, juce::dontSendNotification);
        return;
    }
    selection_.clear();
    updateControls();
}

void PitchEditor::correct(double pitchAmount, double driftAmount)
{
    const auto notes = targets();
    if (notes.empty())
        return;
    session_.beginTransaction("Correct Pitch");
    edits::correctPitch(notes, pitchAmount, driftAmount, scale_, &session_.getUndoManager());
}

void PitchEditor::setDriftOfSelection(double drift)
{
    for (auto& n : selection_)
        edits::setNoteDrift(n, drift, &session_.getUndoManager());
}

void PitchEditor::setGainOfSelection(double gainDb)
{
    for (auto& n : selection_)
        edits::setNoteGain(n, gainDb, &session_.getUndoManager());
}

void PitchEditor::setScale(const session::audioedits::Scale& scale)
{
    scale_ = scale;
    rootBox_.setSelectedId(scale.root + 1, juce::dontSendNotification);
    scaleBox_.setSelectedId(static_cast<int>(scale.kind) + 1, juce::dontSendNotification);
    repaint();
}

void PitchEditor::setSelection(std::vector<juce::ValueTree> notes)
{
    selection_ = std::move(notes);
    updateControls();
    repaint();
}

std::vector<juce::ValueTree> PitchEditor::notes() const { return clip_.isValid() ? edits::getPitchNotes(clip_) : std::vector<juce::ValueTree>{}; }

std::vector<juce::ValueTree> PitchEditor::targets() const { return selection_.empty() ? notes() : selection_; }

double PitchEditor::clipLength() const { return clip_.isValid() ? session::clip::getLength(clip_) : 1.0; }

// --- Geometry ----------------------------------------------------------------------------------------------

juce::Rectangle<int> PitchEditor::getNoteArea() const
{
    auto area = getLocalBounds();
    area.removeFromTop(kToolbarHeight);
    area.removeFromBottom(kInspectorHeight);
    area.removeFromBottom(kScrollBarSize);
    area.removeFromLeft(kKeysWidth);
    area.removeFromRight(kScrollBarSize);
    area.removeFromTop(kRulerHeight);
    return area;
}

float PitchEditor::timeToX(double clipSeconds) const
{
    return static_cast<float>(getNoteArea().getX()) + static_cast<float>((clipSeconds - viewStart_) * pixelsPerSecond_);
}

double PitchEditor::xToTime(float x) const { return viewStart_ + static_cast<double>(x - static_cast<float>(getNoteArea().getX())) / pixelsPerSecond_; }

float PitchEditor::pitchToY(double midi) const
{
    return static_cast<float>(getNoteArea().getY()) + static_cast<float>(topPitch_ - midi) * semitoneHeight_;
}

double PitchEditor::yToPitch(float y) const { return topPitch_ - static_cast<double>((y - static_cast<float>(getNoteArea().getY())) / semitoneHeight_); }

juce::Rectangle<float> PitchEditor::getNoteBounds(const juce::ValueTree& note) const
{
    const auto e = edits::toEdit(note);
    const float x0 = timeToX(edits::clipTimeOf(clip_, e.newStart));
    const float x1 = timeToX(edits::clipTimeOf(clip_, e.newEnd));
    const float y = pitchToY(e.newPitch);
    const float half = semitoneHeight_ * 0.5f;
    return {x0, y - half, std::max(2.0f, x1 - x0), 2.0f * half};
}

juce::ValueTree PitchEditor::noteAt(juce::Point<float> p) const
{
    juce::ValueTree best;
    float bestDistance = 1.0e9f;
    for (const auto& n : notes())
    {
        const auto bounds = getNoteBounds(n).expanded(3.0f, 2.0f);
        if (!bounds.contains(p))
            continue;
        const float d = std::abs(bounds.getCentreY() - p.y);
        if (d < bestDistance)
        {
            bestDistance = d;
            best = n;
        }
    }
    return best;
}

PitchEditor::Drag PitchEditor::dragAt(const juce::ValueTree& note, juce::Point<float> p) const
{
    if (edits::getPitchMode(clip_) != Mode::melodic)
        return Drag::move;
    const auto bounds = getNoteBounds(note);
    if (bounds.getWidth() > 20.0f)
    {
        if (p.x < bounds.getX() + 6.0f)
            return Drag::start;
        if (p.x > bounds.getRight() - 6.0f)
            return Drag::end;
    }
    return Drag::move;
}

void PitchEditor::fitView()
{
    const auto area = getNoteArea();
    const double length = clipLength();
    if (area.getWidth() > 20 && length > 0.0)
        pixelsPerSecond_ = std::clamp((area.getWidth() - 10) / length, 4.0, 2000.0);
    viewStart_ = 0.0;
    double lo = 60.0, hi = 72.0;
    const auto all = notes();
    if (!all.empty())
    {
        lo = 200.0;
        hi = -1.0;
        for (const auto& n : all)
        {
            const auto e = edits::toEdit(n);
            lo = std::min(lo, e.newPitch);
            hi = std::max(hi, e.newPitch);
        }
    }
    const double rows = std::max(1.0, area.getHeight() / static_cast<double>(semitoneHeight_));
    topPitch_ = std::clamp((lo + hi) * 0.5 + rows * 0.5, kLowestPitch + rows, kHighestPitch);
    updateScrollBars();
}

void PitchEditor::updateScrollBars()
{
    const auto area = getNoteArea();
    const double visibleSeconds = area.getWidth() / pixelsPerSecond_;
    hScroll_.setRangeLimits(0.0, std::max(clipLength(), viewStart_ + visibleSeconds), juce::dontSendNotification);
    hScroll_.setCurrentRange(viewStart_, visibleSeconds, juce::dontSendNotification);
    const double rows = area.getHeight() / static_cast<double>(semitoneHeight_);
    vScroll_.setRangeLimits(0.0, kHighestPitch - kLowestPitch, juce::dontSendNotification);
    vScroll_.setCurrentRange(kHighestPitch - topPitch_, rows, juce::dontSendNotification);
}

void PitchEditor::scrollBarMoved(juce::ScrollBar* bar, double start)
{
    if (bar == &hScroll_)
        viewStart_ = start;
    else
        topPitch_ = kHighestPitch - start;
    repaint();
}

// --- State ---------------------------------------------------------------------------------------------

bool PitchEditor::concerns(const juce::ValueTree& tree) const { return clip_.isValid() && (tree == clip_ || tree.isAChildOf(clip_)); }

void PitchEditor::valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier&)
{
    if (concerns(tree))
    {
        updateControls();
        repaint();
    }
}

void PitchEditor::valueTreeChildAdded(juce::ValueTree& parent, juce::ValueTree&)
{
    if (concerns(parent) || parent == clip_)
    {
        if (selection_.empty() && drag_ == Drag::none)
            fitView();
        updateControls();
        repaint();
    }
}

void PitchEditor::valueTreeChildRemoved(juce::ValueTree& parent, juce::ValueTree& child, int)
{
    if (!concerns(parent) && parent != clip_)
        return;
    selection_.erase(std::remove_if(selection_.begin(), selection_.end(),
                                    [&child](const juce::ValueTree& n) { return n == child || n.isAChildOf(child); }),
                     selection_.end());
    updateControls();
    repaint();
}

void PitchEditor::changeListenerCallback(juce::ChangeBroadcaster*)
{
    updateControls();
    repaint();
}

void PitchEditor::timerCallback()
{
    updateControls();
    repaint();
}

void PitchEditor::updateControls()
{
    const bool analysed = clip_.isValid() && edits::hasPitch(clip_);
    const auto state = edits_.getState(clip_);
    juce::String text;
    switch (state.status)
    {
        case session::AudioEdits::Status::analysing:
            text = "Analysing " + juce::String(juce::roundToInt(state.progress * 100.0f)) + " %";
            break;
        case session::AudioEdits::Status::rendering:
            text = "Rendering...";
            break;
        case session::AudioEdits::Status::failed:
            text = "Failed: " + state.problem;
            break;
        case session::AudioEdits::Status::idle:
            text = analysed ? juce::String(static_cast<int>(notes().size())) + " notes" : juce::String("Not analysed");
            break;
    }
    status_.setText(text, juce::dontSendNotification);
    const bool busy = state.status == session::AudioEdits::Status::analysing || state.status == session::AudioEdits::Status::rendering;
    if (busy && !isTimerRunning())
        startTimerHz(10);
    else if (!busy && isTimerRunning())
        stopTimer();

    analyseButton_.setEnabled(clip_.isValid() && state.status != session::AudioEdits::Status::analysing);
    for (auto* c : std::initializer_list<juce::Component*>{&correctButton_, &pitchAmount_, &driftAmount_, &formantsButton_, &originalButton_,
                                                           &removeButton_})
        c->setEnabled(analysed);
    formantsButton_.setToggleState(analysed && edits::keepsFormants(clip_), juce::dontSendNotification);
    formantsButton_.setVisible(!analysed || edits::getPitchMode(clip_) == Mode::melodic);
    originalButton_.setToggleState(analysed && session::AudioEdits::isBypassed(clip_), juce::dontSendNotification);

    // The selection's own values.
    const bool any = !selection_.empty();
    driftSlider_.setEnabled(any);
    gainSlider_.setEnabled(any);
    if (any)
    {
        const auto e = edits::toEdit(selection_.front());
        if (!driftSlider_.isMouseButtonDown())
            driftSlider_.setValue(e.drift * 100.0, juce::dontSendNotification);
        if (!gainSlider_.isMouseButtonDown())
            gainSlider_.setValue(e.gainDb, juce::dontSendNotification);
        selectionInfo_.setText(selection_.size() == 1 ? noteName(e.newPitch) + "   (sung " + noteName(e.note.pitch) + ")"
                                                      : juce::String(static_cast<int>(selection_.size())) + " notes",
                               juce::dontSendNotification);
    }
    else
        selectionInfo_.setText(analysed ? "Click a note (Shift: more), or drag around notes" : juce::String(), juce::dontSendNotification);
}

// --- Painting ------------------------------------------------------------------------------------------

void PitchEditor::paint(juce::Graphics& g)
{
    using namespace theme;
    g.fillAll(colour::panel);
    g.setColour(colour::panelHeader);
    g.fillRect(getLocalBounds().removeFromTop(kToolbarHeight));
    g.fillRect(getLocalBounds().removeFromBottom(kInspectorHeight + kScrollBarSize).removeFromTop(kInspectorHeight));

    const auto area = getNoteArea();
    const auto keys = juce::Rectangle<int>(area.getX() - kKeysWidth, area.getY(), kKeysWidth, area.getHeight());
    const auto ruler = juce::Rectangle<int>(area.getX(), area.getY() - kRulerHeight, area.getWidth(), kRulerHeight);
    if (!clip_.isValid())
        return;

    {
        juce::Graphics::ScopedSaveState save(g);
        g.reduceClipRegion(area.getUnion(keys));
        // Pitch rows, the scale's notes lighter; keys with C names.
        const int lowRow = static_cast<int>(std::floor(yToPitch(static_cast<float>(area.getBottom())))) - 1;
        const int highRow = static_cast<int>(std::ceil(topPitch_)) + 1;
        for (int p = lowRow; p <= highRow; ++p)
        {
            const float y0 = pitchToY(p + 0.5);
            const auto row = juce::Rectangle<float>(static_cast<float>(area.getX()), y0, static_cast<float>(area.getWidth()), semitoneHeight_);
            const bool inScale = session::audioedits::nearestInScale(p, scale_) == p;
            g.setColour(inScale ? (isBlackKey(p) ? colour::laneOdd : colour::laneEven) : colour::panel.darker(0.25f));
            g.fillRect(row);
            g.setColour(colour::gridBeat);
            g.fillRect(row.withHeight(1.0f));
            const auto key = juce::Rectangle<float>(static_cast<float>(keys.getX()), y0, static_cast<float>(kKeysWidth - 2), semitoneHeight_);
            g.setColour(isBlackKey(p) ? colour::control : colour::textSecondary.withAlpha(0.85f));
            g.fillRect(key.reduced(0.0f, 0.5f));
            if (p % 12 == 0)
            {
                g.setColour(colour::panel);
                g.setFont(sans(9.5f));
                g.drawText(juce::MidiMessage::getMidiNoteName(p, true, true, 4), key.withTrimmedLeft(3.0f), juce::Justification::centredLeft, false);
                g.setColour(colour::gridBar);
                g.fillRect(row.withY(row.getBottom() - 1.0f).withHeight(1.0f));
            }
        }
    }

    // The ruler: seconds of the clip.
    g.setColour(colour::panelRaised);
    g.fillRect(ruler);
    g.setColour(colour::textMuted);
    g.setFont(sans(9.5f));
    const double step = pixelsPerSecond_ > 200.0 ? 0.25 : pixelsPerSecond_ > 60.0 ? 1.0 : pixelsPerSecond_ > 15.0 ? 5.0 : 15.0;
    for (double t = std::floor(viewStart_ / step) * step; timeToX(t) < static_cast<float>(area.getRight()); t += step)
    {
        const float x = timeToX(t);
        if (x < static_cast<float>(area.getX()))
            continue;
        g.fillRect(x, static_cast<float>(ruler.getY() + 8), 1.0f, 8.0f);
        g.drawText(juce::String(t, step < 1.0 ? 2 : 0) + " s", juce::Rectangle<float>(x + 2.0f, static_cast<float>(ruler.getY()), 60.0f, 10.0f),
                   juce::Justification::centredLeft, false);
    }

    juce::Graphics::ScopedSaveState save(g);
    g.reduceClipRegion(area);
    // The clip's end.
    g.setColour(colour::outlineStrong);
    g.fillRect(timeToX(clipLength()), static_cast<float>(area.getY()), 1.0f, static_cast<float>(area.getHeight()));

    if (!edits::hasPitch(clip_))
    {
        g.setColour(colour::textMuted);
        g.setFont(sans(13.0f));
        g.drawText("Choose Melodic (one voice) or Polyphonic (chords) and press Analyse to find the notes.", area.toFloat(),
                   juce::Justification::centred, true);
    }
    else
    {
        for (const auto& n : notes())
            paintNote(g, n, std::find(selection_.begin(), selection_.end(), n) != selection_.end());
    }

    if (playhead_ >= 0.0)
    {
        g.setColour(colour::playhead);
        g.fillRect(timeToX(playhead_) - 0.5f, static_cast<float>(area.getY()), 1.5f, static_cast<float>(area.getHeight()));
    }
    if (drag_ == Drag::lasso)
    {
        g.setColour(colour::accent.withAlpha(0.15f));
        g.fillRect(lasso_);
        g.setColour(colour::accent);
        g.drawRect(lasso_, 1.0f);
    }
}

void PitchEditor::paintNote(juce::Graphics& g, const juce::ValueTree& note, bool selected) const
{
    using namespace theme;
    const auto e = edits::toEdit(note);
    const double newLength = std::max(1.0e-6, e.newEnd - e.newStart);
    const double length = std::max(1.0e-6, e.note.end - e.note.start);
    const float x0 = timeToX(edits::clipTimeOf(clip_, e.newStart));
    const float x1 = timeToX(edits::clipTimeOf(clip_, e.newEnd));
    const float centre = pitchToY(e.newPitch);

    // Where it was, once moved.
    if (e.pitchEdited() || e.timeEdited())
    {
        const float ox0 = timeToX(edits::clipTimeOf(clip_, e.note.start));
        const float ox1 = timeToX(edits::clipTimeOf(clip_, e.note.end));
        g.setColour(colour::textMuted.withAlpha(0.6f));
        g.drawRoundedRectangle(juce::Rectangle<float>(ox0, pitchToY(e.note.pitch) - semitoneHeight_ * 0.35f, std::max(2.0f, ox1 - ox0), semitoneHeight_ * 0.7f),
                               3.0f, 1.0f);
    }

    // The blob: as thick as it is loud (the square root, so quiet notes still show).
    const int points = std::clamp(static_cast<int>((x1 - x0) / 3.0f), 2, 400);
    juce::Path blob, curve;
    std::vector<juce::Point<float>> lower;
    const float gain = static_cast<float>(juce::Decibels::decibelsToGain(e.gainDb));
    for (int i = 0; i <= points; ++i)
    {
        const double u = static_cast<double>(i) / points;
        const double original = e.note.start + u * length;
        const float x = timeToX(edits::clipTimeOf(clip_, e.newStart + u * newLength));
        const float half = std::max(1.5f, semitoneHeight_ * 0.55f * std::sqrt(std::clamp(static_cast<float>(e.note.levelAt(original)) * gain, 0.0f, 1.5f)));
        if (i == 0)
            blob.startNewSubPath(x, centre - half);
        else
            blob.lineTo(x, centre - half);
        lower.push_back({x, centre + half});
        const float y = pitchToY(e.newPitch + e.drift * e.note.driftAt(original));
        if (i == 0)
            curve.startNewSubPath(x, y);
        else
            curve.lineTo(x, y);
    }
    for (auto it = lower.rbegin(); it != lower.rend(); ++it)
        blob.lineTo(*it);
    blob.closeSubPath();
    g.setColour((selected ? colour::accentBright : colour::accent).withAlpha(selected ? 0.85f : 0.55f));
    g.fillPath(blob);
    g.setColour(selected ? colour::textPrimary : colour::accent.brighter(0.2f));
    g.strokePath(blob, juce::PathStrokeType(1.0f));
    g.setColour(colour::gold);
    g.strokePath(curve, juce::PathStrokeType(1.3f));
    if (selected || x1 - x0 > 70.0f)
    {
        g.setColour(colour::textPrimary.withAlpha(selected ? 1.0f : 0.7f));
        g.setFont(sans(10.0f));
        g.drawText(noteName(e.newPitch), juce::Rectangle<float>(x0, centre - semitoneHeight_ * 1.6f, 90.0f, 11.0f), juce::Justification::centredLeft,
                   false);
    }
}

void PitchEditor::resized()
{
    auto toolbar = getLocalBounds().removeFromTop(kToolbarHeight).reduced(6, 3);
    modeBox_.setBounds(toolbar.removeFromLeft(104));
    toolbar.removeFromLeft(4);
    analyseButton_.setBounds(toolbar.removeFromLeft(70));
    toolbar.removeFromLeft(12);
    correctButton_.setBounds(toolbar.removeFromLeft(66));
    toolbar.removeFromLeft(4);
    pitchAmount_.setBounds(toolbar.removeFromLeft(140));
    driftAmount_.setBounds(toolbar.removeFromLeft(140));
    toolbar.removeFromLeft(6);
    rootBox_.setBounds(toolbar.removeFromLeft(56));
    scaleBox_.setBounds(toolbar.removeFromLeft(130));
    toolbar.removeFromLeft(10);
    formantsButton_.setBounds(toolbar.removeFromLeft(86));
    originalButton_.setBounds(toolbar.removeFromLeft(70));
    toolbar.removeFromLeft(4);
    removeButton_.setBounds(toolbar.removeFromLeft(66));
    toolbar.removeFromLeft(8);
    status_.setBounds(toolbar);

    auto bottom = getLocalBounds();
    hScroll_.setBounds(bottom.removeFromBottom(kScrollBarSize).withTrimmedLeft(kKeysWidth).withTrimmedRight(kScrollBarSize));
    auto inspector = bottom.removeFromBottom(kInspectorHeight).reduced(8, 3);
    selectionInfo_.setBounds(inspector.removeFromLeft(260));
    driftSlider_.setBounds(inspector.removeFromLeft(230));
    inspector.removeFromLeft(12);
    gainSlider_.setBounds(inspector.removeFromLeft(230));
    const auto area = getNoteArea();
    vScroll_.setBounds(area.getRight(), area.getY(), kScrollBarSize, area.getHeight());
    fitView();
}

// --- Editing -------------------------------------------------------------------------------------------

void PitchEditor::mouseDown(const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    if (!clip_.isValid() || !getNoteArea().contains(e.getPosition()))
        return;
    dragStart_ = e.position;
    moved_ = false;
    const auto hit = noteAt(e.position);
    if (!hit.isValid())
    {
        drag_ = Drag::lasso;
        lasso_ = {e.position, e.position};
        lassoBase_ = e.mods.isShiftDown() || e.mods.isCommandDown() ? selection_ : std::vector<juce::ValueTree>{};
        selection_ = lassoBase_;
        updateControls();
        repaint();
        return;
    }
    const bool selected = std::find(selection_.begin(), selection_.end(), hit) != selection_.end();
    if (e.mods.isShiftDown() || e.mods.isCommandDown())
    {
        if (selected)
            selection_.erase(std::remove(selection_.begin(), selection_.end(), hit), selection_.end());
        else
            selection_.push_back(hit);
    }
    else if (!selected)
        selection_ = {hit};
    drag_ = std::find(selection_.begin(), selection_.end(), hit) != selection_.end() ? dragAt(hit, e.position) : Drag::none;
    if (drag_ == Drag::start || drag_ == Drag::end)
        selection_ = {hit};
    originals_.clear();
    for (const auto& n : selection_)
    {
        const auto edit = edits::toEdit(n);
        originals_.push_back({n, edit.newPitch, edit.newStart, edit.newEnd});
    }
    updateControls();
    repaint();
}

void PitchEditor::mouseDrag(const juce::MouseEvent& e)
{
    if (drag_ == Drag::lasso)
    {
        lasso_ = juce::Rectangle<float>(dragStart_, e.position);
        auto selection = lassoBase_;
        for (const auto& n : notes())
            if (lasso_.intersects(getNoteBounds(n)) && std::find(selection.begin(), selection.end(), n) == selection.end())
                selection.push_back(n);
        selection_ = std::move(selection);
        updateControls();
        repaint();
        return;
    }
    if (drag_ == Drag::none || originals_.empty())
        return;
    auto* um = &session_.getUndoManager();
    const float dx = e.position.x - dragStart_.x;
    const float dy = e.position.y - dragStart_.y;
    const bool timing = edits::getPitchMode(clip_) == Mode::melodic;
    if (!moved_ && std::abs(dx) < 3.0f && std::abs(dy) < 3.0f)
        return;
    // Seconds of the clip moved, as note seconds (the clip may be stretched).
    const auto noteDelta = [this, dx](double noteTime)
    {
        const double clipTime = edits::clipTimeOf(clip_, noteTime);
        return edits::noteTimeOf(clip_, clipTime + dx / pixelsPerSecond_) - noteTime;
    };
    if (drag_ == Drag::move)
    {
        double semitones = -dy / semitoneHeight_;
        if (!e.mods.isAltDown())
            semitones = std::round(semitones);
        const bool moveTime = timing && std::abs(dx) > 4.0f;
        if (!moved_)
            session_.beginTransaction(moveTime ? "Move Notes" : "Change Pitch");
        moved_ = true;
        auto order = originals_;
        const double sample = noteDelta(order.front().start);
        std::sort(order.begin(), order.end(), [sample](const Original& a, const Original& b) { return sample > 0.0 ? a.start > b.start : a.start < b.start; });
        for (auto& o : order)
        {
            edits::setNotePitch(o.note, o.pitch + semitones, um);
            if (moveTime)
            {
                const double d = noteDelta(o.start);
                edits::setNoteTiming(o.note, o.start + d, o.end + d, um);
            }
            else if (timing)
                edits::setNoteTiming(o.note, o.start, o.end, um);
        }
    }
    else
    {
        if (!moved_)
            session_.beginTransaction(drag_ == Drag::start ? "Change Note Start" : "Change Note End");
        moved_ = true;
        const auto& o = originals_.front();
        if (drag_ == Drag::start)
            edits::setNoteTiming(o.note, o.start + noteDelta(o.start), o.end, um);
        else
            edits::setNoteTiming(o.note, o.start, o.end + noteDelta(o.end), um);
    }
}

void PitchEditor::mouseUp(const juce::MouseEvent&)
{
    drag_ = Drag::none;
    originals_.clear();
    repaint();
}

void PitchEditor::mouseDoubleClick(const juce::MouseEvent& e)
{
    const auto hit = noteAt(e.position);
    if (!hit.isValid())
        return;
    session_.beginTransaction("Snap Pitch");
    edits::correctPitch({hit}, 1.0, 0.0, scale_, &session_.getUndoManager());
}

void PitchEditor::mouseMove(const juce::MouseEvent& e)
{
    const auto hit = clip_.isValid() ? noteAt(e.position) : juce::ValueTree();
    const auto drag = hit.isValid() ? dragAt(hit, e.position) : Drag::none;
    setMouseCursor(drag == Drag::start || drag == Drag::end ? juce::MouseCursor::LeftRightResizeCursor
                   : drag == Drag::move                     ? juce::MouseCursor::UpDownLeftRightResizeCursor
                                                            : juce::MouseCursor::NormalCursor);
}

void PitchEditor::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (e.mods.isCommandDown())
    {
        const double t = xToTime(e.position.x);
        pixelsPerSecond_ = std::clamp(pixelsPerSecond_ * (1.0 + wheel.deltaY * 0.8), 4.0, 4000.0);
        viewStart_ = std::max(0.0, t - (e.position.x - static_cast<float>(getNoteArea().getX())) / pixelsPerSecond_);
    }
    else if (e.mods.isShiftDown() || std::abs(wheel.deltaX) > std::abs(wheel.deltaY))
        viewStart_ = std::max(0.0, viewStart_ - (std::abs(wheel.deltaX) > 0.0f ? wheel.deltaX : wheel.deltaY) * 300.0 / pixelsPerSecond_);
    else
    {
        const double rows = getNoteArea().getHeight() / static_cast<double>(semitoneHeight_);
        topPitch_ = std::clamp(topPitch_ + wheel.deltaY * 12.0, kLowestPitch + rows, kHighestPitch);
    }
    updateScrollBars();
    repaint();
}

bool PitchEditor::keyPressed(const juce::KeyPress& key)
{
    const int code = key.getKeyCode();
    auto* um = &session_.getUndoManager();
    if (code == juce::KeyPress::upKey || code == juce::KeyPress::downKey)
    {
        if (selection_.empty())
            return false;
        const double step = (code == juce::KeyPress::upKey ? 1.0 : -1.0) * (key.getModifiers().isShiftDown() ? 0.1 : 1.0);
        session_.beginTransaction("Change Pitch");
        for (auto& n : selection_)
            edits::setNotePitch(n, edits::toEdit(n).newPitch + step, um);
        return true;
    }
    if (code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey)
    {
        if (selection_.empty())
            return false;
        session_.beginTransaction(selection_.size() == 1 ? "Reset Note" : "Reset Notes");
        for (auto& n : selection_)
            edits::resetNote(n, um);
        return true;
    }
    if (code == juce::KeyPress::escapeKey)
    {
        setSelection({});
        return true;
    }
    if (key.getModifiers().isCommandDown() && (code == 'A' || code == 'a'))
    {
        setSelection(notes());
        return true;
    }
    return false;
}

} // namespace dojo::ui
