#include "ui/score/ScoreView.h"

#include "ui/score/MusicXml.h"
#include "ui/theme/DojoTheme.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace dojo::ui
{
namespace
{
constexpr int kToolbarHeight = 30;
constexpr int kScrollBarHeight = 12;
constexpr double kEpsilon = 1.0e-6;
constexpr float kStaffDistance = 12.0f; ///< staff top to staff top, in spaces
constexpr float kTimeSignatureWidth = 2.8f;

juce::Colour paperColour() { return juce::Colour(0xfff6f1e7); }
juce::Colour inkColour() { return juce::Colour(0xff1d1b19); }
juce::Colour faintColour() { return juce::Colour(0xff9a9386); }

constexpr std::array<const char*, 4> kClefIds{"automatic", "treble", "bass", "grand"};
constexpr std::array<const char*, 4> kClefLabels{"Clef: Auto", "Treble", "Bass", "Grand Staff"};
constexpr std::array<double, 4> kGridBeats{1.0, 0.5, 0.25, 0.125};
constexpr std::array<const char*, 4> kGridLabels{"Grid 1/4", "Grid 1/8", "Grid 1/16", "Grid 1/32"};
constexpr std::array<const char*, 6> kValueLabels{"1/1", "1/2", "1/4", "1/8", "1/16", "1/32"};
constexpr std::array<const char*, 6> kValueNames{"whole", "half", "quarter", "eighth", "sixteenth", "32nd"};

score::ClefMode clefModeOf(const juce::ValueTree& clip)
{
    const auto text = clip.getProperty(session::ids::scoreClef).toString();
    for (std::size_t i = 0; i < kClefIds.size(); ++i)
        if (text == kClefIds[i])
            return static_cast<score::ClefMode>(i);
    return score::ClefMode::automatic;
}

/** The written value nearest a length: {type 0..5, dotted}. */
std::pair<int, bool> valueOf(double length)
{
    std::pair<int, bool> best{2, false};
    double bestError = 1.0e9;
    for (int type = 0; type <= 5; ++type)
        for (int dots = 0; dots <= 1; ++dots)
            if (const double error = std::abs(score::valueLength(type, dots) - length); error < bestError - 1.0e-9)
            {
                bestError = error;
                best = {type, dots == 1};
            }
    return best;
}

juce::String pitchName(int midi, int keyFifths)
{
    const auto s = score::spell(midi, keyFifths);
    static constexpr std::array<const char*, 7> steps{"C", "D", "E", "F", "G", "A", "B"};
    juce::String name = steps[static_cast<std::size_t>(s.step)];
    for (int i = 0; i < std::abs(s.alter); ++i)
        name << (s.alter > 0 ? "#" : "b");
    return name + juce::String(s.octave);
}

// --- Glyphs (drawn in staff spaces: y down, `ls` pixels a space) ------------------------------------

juce::AffineTransform placeAt(float x, float y, float ls) { return juce::AffineTransform::scale(ls).translated(x, y); }

void strokeGlyph(juce::Graphics& g, const juce::Path& p, float x, float y, float ls, float width)
{
    g.strokePath(p, juce::PathStrokeType(width * ls, juce::PathStrokeType::curved, juce::PathStrokeType::rounded), placeAt(x, y, ls));
}

void fillCircle(juce::Graphics& g, float x, float y, float radius) { g.fillEllipse(x - radius, y - radius, radius * 2.0f, radius * 2.0f); }

/** The G clef, its spiral round the G line (y 0). */
juce::Path trebleClefPath()
{
    juce::Path p;
    p.startNewSubPath(0.1f, 0.05f);
    p.cubicTo(-0.4f, -0.05f, -0.45f, 0.7f, 0.15f, 0.72f);
    p.cubicTo(0.75f, 0.74f, 0.95f, 0.05f, 0.6f, -0.45f);
    p.cubicTo(0.3f, -0.9f, -0.65f, -0.85f, -0.8f, -0.05f);
    p.cubicTo(-0.95f, 0.85f, -0.15f, 1.35f, 0.55f, 1.05f);
    p.cubicTo(1.25f, 0.75f, 1.1f, -0.35f, 0.55f, -1.25f);
    p.cubicTo(0.1f, -2.0f, -0.45f, -2.6f, -0.3f, -3.5f);
    p.cubicTo(-0.2f, -4.3f, 0.55f, -4.75f, 0.62f, -4.0f);
    p.cubicTo(0.7f, -3.25f, -0.15f, -2.9f, -0.2f, -2.0f);
    p.cubicTo(-0.25f, -1.0f, 0.4f, 1.3f, 0.38f, 2.4f);
    p.cubicTo(0.36f, 3.05f, -0.35f, 3.15f, -0.5f, 2.65f);
    return p;
}

void drawTrebleClef(juce::Graphics& g, float x, float gLineY, float ls)
{
    strokeGlyph(g, trebleClefPath(), x, gLineY, ls, 0.17f);
    fillCircle(g, x - 0.25f * ls, gLineY + 2.6f * ls, 0.32f * ls);
}

/** The F clef, its head on the F line (y 0). */
void drawBassClef(juce::Graphics& g, float x, float fLineY, float ls)
{
    juce::Path p;
    p.startNewSubPath(-0.05f, 0.15f);
    p.cubicTo(-0.1f, -1.0f, 1.6f, -1.25f, 1.65f, 0.15f);
    p.cubicTo(1.7f, 1.4f, 0.7f, 2.3f, -0.35f, 2.85f);
    strokeGlyph(g, p, x, fLineY, ls, 0.22f);
    fillCircle(g, x + 0.12f * ls, fLineY + 0.1f * ls, 0.4f * ls);
    fillCircle(g, x + 2.15f * ls, fLineY - 0.5f * ls, 0.17f * ls);
    fillCircle(g, x + 2.15f * ls, fLineY + 0.5f * ls, 0.17f * ls);
}

void drawHead(juce::Graphics& g, float x, float y, float ls, int type)
{
    juce::Path p;
    if (type == 0)
    {
        p.addEllipse(-0.8f, -0.5f, 1.6f, 1.0f);
        juce::Path hole;
        hole.addEllipse(-0.42f, -0.27f, 0.84f, 0.54f);
        p.addPath(hole, juce::AffineTransform::rotation(0.9f));
    }
    else
    {
        p.addEllipse(-0.64f, -0.44f, 1.28f, 0.88f);
        p.applyTransform(juce::AffineTransform::rotation(-0.35f));
        if (type == 1)
        {
            juce::Path hole;
            hole.addEllipse(-0.52f, -0.2f, 1.04f, 0.4f);
            p.addPath(hole, juce::AffineTransform::rotation(-0.55f));
        }
    }
    p.setUsingNonZeroWinding(false);
    g.fillPath(p, placeAt(x, y, ls));
}

void drawAccidental(juce::Graphics& g, score::Accidental a, float x, float y, float ls)
{
    const auto thin = 0.13f * ls;
    const auto thick = 0.3f * ls;
    switch (a)
    {
        case score::Accidental::sharp:
            g.drawLine(x - 0.2f * ls, y - 1.25f * ls, x - 0.2f * ls, y + 1.4f * ls, thin);
            g.drawLine(x + 0.2f * ls, y - 1.4f * ls, x + 0.2f * ls, y + 1.25f * ls, thin);
            g.drawLine(x - 0.5f * ls, y - 0.3f * ls, x + 0.5f * ls, y - 0.6f * ls, thick);
            g.drawLine(x - 0.5f * ls, y + 0.6f * ls, x + 0.5f * ls, y + 0.3f * ls, thick);
            break;
        case score::Accidental::natural:
            g.drawLine(x - 0.25f * ls, y - 1.35f * ls, x - 0.25f * ls, y + 0.6f * ls, thin);
            g.drawLine(x + 0.25f * ls, y - 0.6f * ls, x + 0.25f * ls, y + 1.35f * ls, thin);
            g.drawLine(x - 0.25f * ls, y - 0.3f * ls, x + 0.25f * ls, y - 0.55f * ls, thick);
            g.drawLine(x - 0.25f * ls, y + 0.55f * ls, x + 0.25f * ls, y + 0.3f * ls, thick);
            break;
        case score::Accidental::flat:
        case score::Accidental::doubleFlat:
        {
            const int count = a == score::Accidental::flat ? 1 : 2;
            for (int i = 0; i < count; ++i)
            {
                const float fx = x - 0.15f * ls - static_cast<float>(count - 1 - i) * 0.6f * ls + static_cast<float>(count - 1) * 0.3f * ls;
                g.drawLine(fx - 0.25f * ls, y - 1.8f * ls, fx - 0.25f * ls, y + 0.5f * ls, thin);
                juce::Path bowl;
                bowl.startNewSubPath(-0.25f, 0.5f);
                bowl.cubicTo(0.65f, -0.05f, 0.55f, -0.85f, -0.25f, -0.2f);
                strokeGlyph(g, bowl, fx, y, ls, 0.2f);
            }
            break;
        }
        case score::Accidental::doubleSharp:
            g.drawLine(x - 0.45f * ls, y - 0.45f * ls, x + 0.45f * ls, y + 0.45f * ls, 0.16f * ls);
            g.drawLine(x - 0.45f * ls, y + 0.45f * ls, x + 0.45f * ls, y - 0.45f * ls, 0.16f * ls);
            for (const float dx : {-0.45f, 0.45f})
                for (const float dy : {-0.45f, 0.45f})
                    g.fillRect(x + dx * ls - 0.15f * ls, y + dy * ls - 0.15f * ls, 0.3f * ls, 0.3f * ls);
            break;
        case score::Accidental::none:
            break;
    }
}

/** A rest of `type` centred at x on a staff whose top line is at `top`. */
void drawRest(juce::Graphics& g, int type, int dots, float x, float top, float ls)
{
    const float middle = top + 2.0f * ls;
    float right = x + 0.6f * ls;
    if (type == 0)
        g.fillRect(x - 0.65f * ls, top + ls, 1.3f * ls, 0.5f * ls); // hangs from the fourth line
    else if (type == 1)
        g.fillRect(x - 0.65f * ls, middle - 0.5f * ls, 1.3f * ls, 0.5f * ls); // sits on the middle line
    else if (type == 2)
    {
        juce::Path p;
        p.startNewSubPath(-0.2f, -1.5f);
        p.lineTo(0.4f, -0.8f);
        p.lineTo(-0.15f, -0.15f);
        p.lineTo(0.4f, 0.5f);
        p.cubicTo(-0.35f, 0.2f, -0.45f, 0.95f, 0.05f, 1.35f);
        strokeGlyph(g, p, x, middle, ls, 0.26f);
        juce::Path thickPart;
        thickPart.startNewSubPath(0.35f, -0.78f);
        thickPart.lineTo(-0.1f, -0.15f);
        strokeGlyph(g, thickPart, x, middle, ls, 0.42f);
    }
    else
    {
        const int flags = type - 2;
        const float stemTop = top + 1.15f * ls;
        const float stemBottom = top + (2.3f + static_cast<float>(flags)) * ls;
        const float slant = 0.32f;
        const auto stemX = [&](float y) { return x + 0.45f * ls - (y - stemTop) * slant; };
        g.drawLine(stemX(stemTop), stemTop, stemX(stemBottom), stemBottom, 0.14f * ls);
        for (int k = 0; k < flags; ++k)
        {
            const float y = stemTop + static_cast<float>(k) * ls + 0.1f * ls;
            const float dotX = stemX(y) - 0.95f * ls;
            fillCircle(g, dotX, y + 0.25f * ls, 0.27f * ls);
            juce::Path curve;
            curve.startNewSubPath(dotX, y + 0.3f * ls);
            curve.quadraticTo(dotX + 0.5f * ls, y + 0.55f * ls, stemX(y), y);
            g.strokePath(curve, juce::PathStrokeType(0.13f * ls));
        }
        right = x + 0.9f * ls;
    }
    for (int d = 0; d < dots; ++d)
        fillCircle(g, right + 0.3f * ls + static_cast<float>(d) * 0.5f * ls, top + 1.5f * ls, 0.17f * ls);
}

/** A flag (or several) at the end of a stem. */
void drawFlags(juce::Graphics& g, float stemX, float endY, bool up, int count, float ls)
{
    for (int k = 0; k < count; ++k)
    {
        const float dir = up ? 1.0f : -1.0f;
        const float y = endY + dir * static_cast<float>(k) * 0.8f * ls;
        juce::Path p;
        p.startNewSubPath(0.0f, 0.0f);
        p.cubicTo(0.15f, dir * 0.9f, 1.25f, dir * 1.3f, 0.75f, dir * 2.7f);
        strokeGlyph(g, p, stemX, y, ls, 0.24f);
    }
}

void fillBeam(juce::Graphics& g, float x0, float y0, float x1, float y1, float thickness)
{
    juce::Path p;
    p.startNewSubPath(x0, y0);
    p.lineTo(x1, y1);
    p.lineTo(x1, y1 + thickness);
    p.lineTo(x0, y0 + thickness);
    p.closeSubPath();
    g.fillPath(p);
}

/** Heads a second apart go either side of the stem; accidentals stack leftwards clear of each other. */
struct Arrangement
{
    std::vector<int> shift;            ///< -1 left of the stem, 0, +1 right
    std::vector<int> accidentalColumn; ///< -1: none
    int columns = 0;
    bool shiftedLeft = false;
    bool shiftedRight = false;
};

Arrangement arrange(const score::Event& e)
{
    Arrangement a;
    const auto n = e.heads.size();
    a.shift.assign(n, 0);
    a.accidentalColumn.assign(n, -1);
    if (n > 1)
    {
        if (e.stemUp)
        {
            for (std::size_t i = 1; i < n; ++i)
                if (a.shift[i - 1] == 0 && score::diatonic(e.heads[i].spelling) - score::diatonic(e.heads[i - 1].spelling) <= 1)
                    a.shift[i] = 1;
        }
        else
        {
            for (std::size_t i = n - 1; i-- > 0;)
                if (a.shift[i + 1] == 0 && score::diatonic(e.heads[i + 1].spelling) - score::diatonic(e.heads[i].spelling) <= 1)
                    a.shift[i] = -1;
        }
    }
    std::vector<std::pair<int, int>> placed; // (diatonic, column)
    for (std::size_t i = n; i-- > 0;)
    {
        if (e.heads[i].accidental == score::Accidental::none)
            continue;
        const int d = score::diatonic(e.heads[i].spelling);
        int column = 0;
        for (bool clash = true; clash;)
        {
            clash = false;
            for (const auto& other : placed)
                if (other.second == column && std::abs(other.first - d) < 6)
                {
                    clash = true;
                    ++column;
                    break;
                }
        }
        placed.emplace_back(d, column);
        a.accidentalColumn[i] = column;
        a.columns = std::max(a.columns, column + 1);
    }
    for (const int s : a.shift)
    {
        a.shiftedLeft = a.shiftedLeft || s < 0;
        a.shiftedRight = a.shiftedRight || s > 0;
    }
    return a;
}

/** The key signature's positions (diatonic, treble clef; two octaves lower in the bass). */
constexpr std::array<int, 7> kSharpPositions{38, 35, 39, 36, 33, 37, 34};
constexpr std::array<int, 7> kFlatPositions{34, 37, 33, 36, 32, 35, 31};
} // namespace

//==================================================================================================

ScoreView::ScoreView(session::Session& session)
    : session_(session)
{
    setWantsKeyboardFocus(true);
    setOpaque(true);

    for (auto* b : {&selectTool_, &noteTool_})
    {
        b->setRadioGroupId(4251);
        b->setClickingTogglesState(true);
        b->setWantsKeyboardFocus(false);
        addAndMakeVisible(b);
    }
    selectTool_.setTooltip("Select tool (S): click or lasso notes, drag them along the grid and up or down the staff");
    noteTool_.setTooltip("Note tool (N): click to put in the chosen value (Shift: a semitone up, Alt: down); right-click deletes");
    selectTool_.onClick = [this] { setTool(Tool::select); };
    noteTool_.onClick = [this] { setTool(Tool::note); };

    for (std::size_t i = 0; i < valueButtons_.size(); ++i)
    {
        auto& b = valueButtons_[i];
        b.setButtonText(kValueLabels[i]);
        b.setRadioGroupId(4252);
        b.setClickingTogglesState(true);
        b.setWantsKeyboardFocus(false);
        b.setTooltip(juce::String("A ") + kValueNames[i] + " note (" + juce::String(static_cast<int>(i) + 1)
                     + "): the selected notes', or the Note tool's");
        b.onClick = [this, i] { setSelectionValue(static_cast<int>(i), std::nullopt); };
        addAndMakeVisible(b);
    }
    dotButton_.setClickingTogglesState(true);
    dotButton_.setWantsKeyboardFocus(false);
    dotButton_.setTooltip("Dotted (.)");
    dotButton_.onClick = [this] { setSelectionValue(-1, dotButton_.getToggleState()); };
    addAndMakeVisible(dotButton_);

    for (std::size_t i = 0; i < kClefLabels.size(); ++i)
        clefBox_.addItem(kClefLabels[i], static_cast<int>(i) + 1);
    clefBox_.setTooltip("Clef: automatic (treble, bass or a grand staff from the notes' range), or fixed");
    clefBox_.onChange = [this] { setClefMode(static_cast<score::ClefMode>(std::max(0, clefBox_.getSelectedId() - 1))); };
    addAndMakeVisible(clefBox_);

    keyBox_.addItem("Key: detected", 1);
    for (int fifths = -7; fifths <= 7; ++fifths)
        keyBox_.addItem(score::keyName(fifths), fifths + 9);
    keyBox_.setTooltip("Key signature: detected from the notes, or chosen");
    keyBox_.onChange = [this]
    {
        const int id = keyBox_.getSelectedId();
        setKey(id <= 1 ? std::optional<int>() : std::optional<int>(id - 9));
    };
    addAndMakeVisible(keyBox_);

    for (std::size_t i = 0; i < kGridLabels.size(); ++i)
        gridBox_.addItem(kGridLabels[i], static_cast<int>(i) + 1);
    gridBox_.setTooltip("Notation grid: notes are shown rounded to it (they are not changed) and put in on it");
    gridBox_.onChange = [this] { setGrid(kGridBeats[static_cast<std::size_t>(std::clamp(gridBox_.getSelectedId() - 1, 0, 3))]); };
    addAndMakeVisible(gridBox_);

    exportButton_.setWantsKeyboardFocus(false);
    exportButton_.setTooltip("Export the score as MusicXML (for Dorico, Sibelius, Finale, MuseScore...)");
    exportButton_.onClick = [this]
    {
        if (!clip_.isValid())
            return;
        const auto name = session::clip::getName(clip_);
        chooser_ = std::make_unique<juce::FileChooser>(
            "Export MusicXML",
            juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile(juce::File::createLegalFileName(name) + ".musicxml"),
            "*.musicxml;*.xml");
        chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::warnAboutOverwriting,
                              [safe = juce::Component::SafePointer<ScoreView>(this)](const juce::FileChooser& fc)
                              {
                                  const auto file = fc.getResult();
                                  if (safe == nullptr || file == juce::File())
                                      return;
                                  if (!safe->exportMusicXml(file))
                                      juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Export MusicXML",
                                                                             "Could not write " + file.getFullPathName());
                              });
    };
    addAndMakeVisible(exportButton_);

    hScroll_.setAutoHide(false);
    hScroll_.addListener(this);
    addAndMakeVisible(hScroll_);

    session_.getState().addListener(this);
    updateToolbar();
}

ScoreView::~ScoreView()
{
    stopPreview();
    session_.getState().removeListener(this);
}

void ScoreView::setClip(const juce::ValueTree& clip)
{
    if (clip == clip_)
        return;
    stopPreview();
    clip_ = clip;
    selection_.clear();
    dragOriginals_.clear();
    dragging_ = lassoing_ = false;
    scrollX_ = 0.0f;
    dirty_ = true;
    updateToolbar();
    updateScrollBar();
    repaint();
}

void ScoreView::setPlayhead(double seconds)
{
    const double beat = session_.getTempoMap().beatAt(seconds);
    if (std::abs(beat - playheadBeat_) < 1.0e-4)
        return;
    playheadBeat_ = beat;
    repaint();
}

void ScoreView::setTool(Tool tool)
{
    tool_ = tool;
    updateToolbar();
    repaint();
}

void ScoreView::setNoteValue(int type, bool dotted)
{
    noteType_ = std::clamp(type, 0, 5);
    dotted_ = dotted;
    updateToolbar();
}

void ScoreView::setClefMode(score::ClefMode mode)
{
    if (!clip_.isValid() || clefModeOf(clip_) == mode)
        return;
    session_.beginTransaction("Score Clef");
    clip_.setProperty(session::ids::scoreClef, kClefIds[static_cast<std::size_t>(mode)], &session_.getUndoManager());
}

void ScoreView::setKey(std::optional<int> fifths)
{
    if (!clip_.isValid() || getOptions().key == fifths)
        return;
    session_.beginTransaction("Score Key");
    if (fifths.has_value())
        clip_.setProperty(session::ids::scoreKey, std::clamp(*fifths, -7, 7), &session_.getUndoManager());
    else
        clip_.removeProperty(session::ids::scoreKey, &session_.getUndoManager());
}

void ScoreView::setGrid(double beats)
{
    if (!clip_.isValid() || std::abs(getOptions().grid - beats) < kEpsilon)
        return;
    session_.beginTransaction("Score Grid");
    clip_.setProperty(session::ids::scoreGrid, std::max(0.0625, beats), &session_.getUndoManager());
}

score::Options ScoreView::getOptions() const
{
    score::Options options;
    if (!clip_.isValid())
        return options;
    options.clef = clefModeOf(clip_);
    if (clip_.hasProperty(session::ids::scoreKey))
        options.key = std::clamp(static_cast<int>(clip_.getProperty(session::ids::scoreKey)), -7, 7);
    if (clip_.hasProperty(session::ids::scoreGrid))
        options.grid = std::max(0.0625, static_cast<double>(clip_.getProperty(session::ids::scoreGrid)));
    return options;
}

const score::Score& ScoreView::getScore() const
{
    relayout();
    return score_;
}

bool ScoreView::exportMusicXml(const juce::File& file) const
{
    if (!clip_.isValid())
        return false;
    const auto track = clip_.getParent();
    const auto part = track.hasType(session::ids::track) ? session::track::getName(track) : juce::String("Part");
    return score::writeMusicXml(getScore(), file, session::clip::getName(clip_), part);
}

double ScoreView::contentOrigin() const
{
    relayout();
    return origin_;
}

void ScoreView::setSelection(std::vector<juce::ValueTree> notes)
{
    selection_ = std::move(notes);
    repaint();
}

// --- Layout --------------------------------------------------------------------------------------------

void ScoreView::invalidate()
{
    dirty_ = true;
    triggerAsyncUpdate();
    repaint();
}

void ScoreView::handleAsyncUpdate()
{
    updateToolbar();
    updateScrollBar();
    repaint();
}

void ScoreView::relayout() const
{
    if (!dirty_)
        return;
    dirty_ = false;
    columns_.clear();
    barLineX_.clear();
    signatureX_.clear();

    const auto map = session_.getTempoMapShared();
    std::vector<score::SourceNote> notes;
    if (clip_.isValid())
    {
        clipFrom_ = map->beatAt(session::clip::getStart(clip_));
        clipTo_ = std::max(clipFrom_ + 0.0625, map->beatAt(session::clip::getEnd(clip_)));
        origin_ = clipFrom_ - session::clip::getOffset(clip_);
        for (int i = 0; i < clip_.getNumChildren(); ++i)
        {
            const auto n = clip_.getChild(i);
            if (!n.hasType(session::ids::note))
                continue;
            const double s = origin_ + session::note::getStart(n);
            const double e = s + session::note::getLength(n);
            if (e <= clipFrom_ + kEpsilon || s >= clipTo_ - kEpsilon)
                continue;
            notes.push_back({std::max(s, clipFrom_), std::min(e, clipTo_), session::note::getNote(n), static_cast<std::uint32_t>(i)});
        }
    }
    else
    {
        clipFrom_ = 0.0;
        clipTo_ = 4.0;
        origin_ = 0.0;
    }
    score_ = score::layout(notes, clipFrom_, clipTo_, *map, getOptions());

    // Columns: time-proportional, with room for each column's accidentals, heads, dots and flags.
    const float ls = lineSpacing();
    const float headW = 1.3f * ls;
    const float accidentalW = 1.05f * ls;
    leftMargin_ = (score_.staves.size() > 1 ? 1.8f : 0.3f) * ls;
    float x = leftMargin_ + 0.3f * ls + 3.6f * ls;
    x += static_cast<float>(std::abs(score_.keyFifths)) * 0.95f * ls + 0.8f * ls;
    headerWidth_ = x;
    struct Need
    {
        double beat = 0.0;
        float left = 0.0f;
        float right = 0.0f;
    };
    for (std::size_t b = 0; b < score_.bars.size(); ++b)
    {
        const auto& bar = score_.bars[b];
        barLineX_.push_back(x);
        x += (b == 0 ? 0.4f : 1.1f) * ls;
        signatureX_.push_back(bar.showSignature ? x : -1.0f);
        if (bar.showSignature)
            x += (kTimeSignatureWidth + 0.8f) * ls;

        std::map<long long, Need> needs;
        for (const auto& staff : score_.staves)
            for (const auto& e : staff.events)
            {
                if (e.bar != static_cast<int>(b))
                    continue;
                auto& need = needs[std::llround(e.beat * 1024.0)];
                need.beat = e.beat;
                float left = 0.0f;
                float right = 0.5f * ls;
                if (e.rest)
                    right += 1.4f * ls + static_cast<float>(e.dots) * 0.5f * ls;
                else
                {
                    const auto a = arrange(e);
                    left = static_cast<float>(a.columns) * accidentalW + (a.shiftedLeft ? headW : 0.0f);
                    right += headW * (a.shiftedRight ? 1.5f : 0.5f) + static_cast<float>(e.dots) * 0.55f * ls + 0.3f * ls;
                    if (e.beam < 0 && e.type >= 3 && e.stemUp)
                        right += 0.9f * ls;
                }
                need.left = std::max(need.left, left);
                need.right = std::max(need.right, right);
            }
        if (needs.empty())
            needs[std::llround(bar.start * 1024.0)] = {bar.start, 0.0f, 1.5f * ls};
        std::vector<Need> list;
        for (const auto& entry : needs)
            list.push_back(entry.second);
        for (std::size_t i = 0; i < list.size(); ++i)
        {
            x += list[i].left;
            columns_.push_back({list[i].beat, x});
            const double next = i + 1 < list.size() ? list[i + 1].beat : bar.end;
            const float nextLeft = i + 1 < list.size() ? list[i + 1].left : 0.0f;
            x += std::max(static_cast<float>(next - list[i].beat) * pixelsPerBeat_ - nextLeft, list[i].right);
        }
        columns_.push_back({bar.end, x});
    }
    barLineX_.push_back(x);
    contentWidth_ = x + 2.0f * ls;
}

float ScoreView::contentX(double beat) const
{
    relayout();
    if (columns_.empty())
        return headerWidth_ + static_cast<float>(beat - clipFrom_) * pixelsPerBeat_;
    const auto it = std::upper_bound(columns_.begin(), columns_.end(), beat + kEpsilon, [](double b, const Column& c) { return b < c.beat; });
    if (it == columns_.begin())
        return it->x - static_cast<float>(it->beat - beat) * pixelsPerBeat_;
    const auto& a = *std::prev(it);
    if (it == columns_.end())
        return a.x + static_cast<float>(beat - a.beat) * pixelsPerBeat_;
    if (it->beat - a.beat < kEpsilon)
        return a.x;
    return a.x + static_cast<float>((beat - a.beat) / (it->beat - a.beat)) * (it->x - a.x);
}

namespace
{
template <typename Columns>
double beatOnColumns(const Columns& columns, float x, float pixelsPerBeat, double fallbackBeat, float fallbackX)
{
    if (columns.empty())
        return fallbackBeat + static_cast<double>((x - fallbackX) / pixelsPerBeat);
    const auto it = std::upper_bound(columns.begin(), columns.end(), x, [](float v, const auto& c) { return v < c.x; });
    if (it == columns.begin())
        return it->beat - static_cast<double>((it->x - x) / pixelsPerBeat);
    const auto& a = *std::prev(it);
    if (it == columns.end())
        return a.beat + static_cast<double>((x - a.x) / pixelsPerBeat);
    if (it->x - a.x < 1.0e-3f)
        return a.beat;
    return a.beat + static_cast<double>((x - a.x) / (it->x - a.x)) * (it->beat - a.beat);
}
} // namespace

double ScoreView::contentBeat(float x) const
{
    relayout();
    return beatOnColumns(columns_, x, pixelsPerBeat_, clipFrom_, headerWidth_);
}

juce::Rectangle<int> ScoreView::scoreArea() const
{
    return getLocalBounds().withTrimmedTop(kToolbarHeight).withTrimmedBottom(kScrollBarHeight);
}

float ScoreView::beatToX(double beat) const { return static_cast<float>(scoreArea().getX()) + contentX(beat) - scrollX_; }

double ScoreView::xToBeat(float x) const { return contentBeat(x - static_cast<float>(scoreArea().getX()) + scrollX_); }

float ScoreView::staffTop(int staff) const
{
    relayout();
    const auto area = scoreArea().toFloat();
    const float ls = lineSpacing();
    const int staves = std::max(1, static_cast<int>(score_.staves.size()));
    const float system = static_cast<float>(staves - 1) * kStaffDistance * ls + 4.0f * ls;
    const float top = std::max(6.0f * ls, (area.getHeight() - system) * 0.5f);
    return area.getY() + top + static_cast<float>(staff) * kStaffDistance * ls;
}

namespace
{
score::Clef clefOfStaff(const score::Score& s, int staff)
{
    return staff >= 0 && staff < static_cast<int>(s.staves.size()) ? s.staves[static_cast<std::size_t>(staff)].clef : score::Clef::treble;
}
} // namespace

float ScoreView::diatonicToY(int staff, int diatonic) const
{
    relayout();
    return staffTop(staff) + static_cast<float>(score::topLine(clefOfStaff(score_, staff)) - diatonic) * lineSpacing() * 0.5f;
}

int ScoreView::yToDiatonic(int staff, float y) const
{
    relayout();
    return score::topLine(clefOfStaff(score_, staff)) - juce::roundToInt((y - staffTop(staff)) / (lineSpacing() * 0.5f));
}

int ScoreView::staffAt(float y) const
{
    relayout();
    int best = 0;
    float bestDistance = 1.0e9f;
    for (int s = 0; s < std::max(1, static_cast<int>(score_.staves.size())); ++s)
        if (const float d = std::abs(y - (staffTop(s) + 2.0f * lineSpacing())); d < bestDistance)
        {
            bestDistance = d;
            best = s;
        }
    return best;
}

ScoreView::Placed ScoreView::place(int staffIndex, const score::Event& e) const
{
    Placed p;
    const float ls = lineSpacing();
    const float headW = 1.3f * ls;
    float x = beatToX(e.beat);
    if (e.wholeBarRest && e.bar >= 0 && static_cast<std::size_t>(e.bar) + 1 < barLineX_.size())
    {
        const auto b = static_cast<std::size_t>(e.bar);
        const float from = signatureX_[b] >= 0.0f ? signatureX_[b] + kTimeSignatureWidth * ls : barLineX_[b];
        x = static_cast<float>(scoreArea().getX()) - scrollX_ + (from + barLineX_[b + 1]) * 0.5f;
    }
    const auto a = arrange(e);
    for (std::size_t i = 0; i < e.heads.size(); ++i)
        p.heads.push_back({x + static_cast<float>(a.shift[i]) * (headW - 1.0f), diatonicToY(staffIndex, score::diatonic(e.heads[i].spelling))});
    if (e.heads.empty())
        p.heads.push_back({x, staffTop(staffIndex) + 2.0f * ls});
    p.accidentalColumn = a.accidentalColumn;
    p.stemX = e.stemUp ? x + headW * 0.5f - 0.6f : x - headW * 0.5f + 0.6f;
    return p;
}

juce::Point<float> ScoreView::headPosition(const juce::ValueTree& note) const
{
    relayout();
    const int index = clip_.indexOf(note);
    if (index < 0)
        return {-1.0f, -1.0f};
    for (std::size_t s = 0; s < score_.staves.size(); ++s)
        for (const auto& e : score_.staves[s].events)
            for (std::size_t h = 0; h < e.heads.size(); ++h)
                if (e.heads[h].id == static_cast<std::uint32_t>(index))
                    return place(static_cast<int>(s), e).heads[h];
    return {-1.0f, -1.0f};
}

juce::ValueTree ScoreView::noteAt(juce::Point<float> p) const
{
    relayout();
    const float ls = lineSpacing();
    for (std::size_t s = 0; s < score_.staves.size(); ++s)
        for (const auto& e : score_.staves[s].events)
        {
            if (e.rest)
                continue;
            const auto placed = place(static_cast<int>(s), e);
            for (std::size_t h = 0; h < e.heads.size(); ++h)
                if (std::abs(placed.heads[h].x - p.x) <= 0.8f * ls && std::abs(placed.heads[h].y - p.y) <= 0.55f * ls)
                    return clip_.getChild(static_cast<int>(e.heads[h].id));
        }
    return {};
}

bool ScoreView::isSelected(std::uint32_t id) const
{
    const auto note = clip_.getChild(static_cast<int>(id));
    return std::find(selection_.begin(), selection_.end(), note) != selection_.end();
}

int ScoreView::pitchAt(int staff, float y, juce::ModifierKeys mods) const
{
    relayout();
    const int d = yToDiatonic(staff, y);
    const int step = ((d % 7) + 7) % 7;
    score::Spelling s{step, (d - step) / 7, 0};
    s.alter = score::keyAlteration(step, score_.keyFifths) + (mods.isShiftDown() ? 1 : 0) - (mods.isAltDown() ? 1 : 0);
    return std::clamp(score::midiOf(s), 0, 127);
}

double ScoreView::snapBeat(double beat) const
{
    const auto map = session_.getTempoMapShared();
    const double grid = getOptions().grid;
    const double barStart = map->beatOfBar(map->barAt(beat + kEpsilon));
    return barStart + std::round((beat - barStart) / grid) * grid;
}

// --- Painting ------------------------------------------------------------------------------------------

void ScoreView::paint(juce::Graphics& g)
{
    using namespace theme;
    relayout();
    g.fillAll(colour::panel);
    g.setColour(colour::panelHeader);
    g.fillRect(getLocalBounds().removeFromTop(kToolbarHeight));
    const auto area = scoreArea();
    g.setColour(paperColour());
    g.fillRect(area);
    if (!clip_.isValid())
    {
        g.setColour(faintColour());
        g.setFont(sans(12.5f));
        g.drawText("No MIDI clip", area, juce::Justification::centred);
        return;
    }

    juce::Graphics::ScopedSaveState save(g);
    g.reduceClipRegion(area);
    const float ls = lineSpacing();
    const float originX = static_cast<float>(area.getX()) - scrollX_;
    const int staves = static_cast<int>(score_.staves.size());
    for (int s = 0; s < staves; ++s)
        paintStaff(g, s);

    // Bar lines through the system, a brace for a grand staff, the final double bar.
    const float systemTop = staffTop(0);
    const float systemBottom = staffTop(staves - 1) + 4.0f * ls;
    g.setColour(inkColour());
    for (std::size_t b = 1; b + 1 < barLineX_.size(); ++b)
        g.fillRect(originX + barLineX_[b] - 0.6f, systemTop, 1.2f, systemBottom - systemTop);
    if (!barLineX_.empty())
    {
        const float end = originX + barLineX_.back();
        g.fillRect(end - 0.5f * ls, systemTop, 0.5f * ls, systemBottom - systemTop);
        g.fillRect(end - 1.2f * ls, systemTop, 1.2f, systemBottom - systemTop);
    }
    if (staves > 1)
    {
        const float left = originX + leftMargin_;
        g.fillRect(left, systemTop, 1.2f, systemBottom - systemTop);
        juce::Path brace;
        const float h = systemBottom - systemTop;
        const float bx = left - 0.35f * ls;
        brace.startNewSubPath(bx, systemTop);
        brace.cubicTo(bx - 1.1f * ls, systemTop + h * 0.15f, bx + 0.2f * ls, systemTop + h * 0.4f, bx - 0.9f * ls, systemTop + h * 0.5f);
        brace.cubicTo(bx + 0.2f * ls, systemTop + h * 0.6f, bx - 1.1f * ls, systemTop + h * 0.85f, bx, systemBottom);
        g.strokePath(brace, juce::PathStrokeType(0.28f * ls, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // Bar numbers (the timeline's) above the system.
    g.setColour(faintColour());
    g.setFont(sans(10.0f));
    for (std::size_t b = 0; b < score_.bars.size() && b < barLineX_.size(); ++b)
        g.drawText(juce::String(score_.bars[b].number + 1), juce::Rectangle<float>(originX + barLineX_[b] + 2.0f, systemTop - 3.4f * ls, 40.0f, 12.0f),
                   juce::Justification::centredLeft, false);

    // The playhead.
    if (playheadBeat_ >= clipFrom_ - kEpsilon && playheadBeat_ <= clipTo_ + kEpsilon)
    {
        g.setColour(colour::playhead);
        g.fillRect(beatToX(playheadBeat_) - 0.75f, systemTop - 2.0f * ls, 1.5f, systemBottom - systemTop + 4.0f * ls);
    }

    // The Note tool's ghost: where a click puts a note, and which.
    if (tool_ == Tool::note && !dragging_ && area.toFloat().contains(hover_) && !noteAt(hover_).isValid())
    {
        const int staff = staffAt(hover_.y);
        const double beat = snapBeat(xToBeat(hover_.x));
        if (beat >= clipFrom_ - kEpsilon && beat < clipTo_ - kEpsilon)
        {
            const int pitch = pitchAt(staff, hover_.y, hoverMods_);
            const int d = yToDiatonic(staff, hover_.y);
            const float x = beatToX(beat);
            const float y = diatonicToY(staff, d);
            g.setColour(colour::accent.withAlpha(0.55f));
            const int top = score::topLine(clefOfStaff(score_, staff));
            for (int line = top + 2; line <= d; line += 2)
                g.fillRect(x - 0.95f * ls, diatonicToY(staff, line) - 0.5f, 1.9f * ls, 1.0f);
            for (int line = top - 10; line >= d; line -= 2)
                g.fillRect(x - 0.95f * ls, diatonicToY(staff, line) - 0.5f, 1.9f * ls, 1.0f);
            drawHead(g, x, y, ls, std::min(noteType_, 2));
            g.setFont(sans(10.5f));
            g.drawText(pitchName(pitch, score_.keyFifths), juce::Rectangle<float>(x + ls, y - 1.6f * ls, 50.0f, 12.0f),
                       juce::Justification::centredLeft, false);
        }
    }

    if (lassoing_)
    {
        g.setColour(colour::accent.withAlpha(0.12f));
        g.fillRect(lasso_);
        g.setColour(colour::accent.withAlpha(0.7f));
        g.drawRect(lasso_, 1.0f);
    }
}

void ScoreView::paintStaff(juce::Graphics& g, int staffIndex) const
{
    using namespace theme;
    const auto& staff = score_.staves[static_cast<std::size_t>(staffIndex)];
    const float ls = lineSpacing();
    const float headW = 1.3f * ls;
    const float originX = static_cast<float>(scoreArea().getX()) - scrollX_;
    const float top = staffTop(staffIndex);
    const int topLine = score::topLine(staff.clef);
    const int bottomLine = topLine - 8;
    const auto ink = inkColour();

    // Lines, clef, key signature, time signatures.
    g.setColour(ink.withAlpha(0.75f));
    const float lineEnd = originX + (barLineX_.empty() ? contentWidth_ : barLineX_.back());
    for (int l = 0; l < 5; ++l)
        g.fillRect(originX + leftMargin_, top + static_cast<float>(l) * ls - 0.5f, lineEnd - originX - leftMargin_, 1.0f);
    g.setColour(ink);
    if (staff.clef == score::Clef::treble)
        drawTrebleClef(g, originX + leftMargin_ + 1.7f * ls, diatonicToY(staffIndex, 32), ls);
    else
        drawBassClef(g, originX + leftMargin_ + 0.9f * ls, diatonicToY(staffIndex, 24), ls);
    const int count = std::abs(score_.keyFifths);
    const int shiftDown = staff.clef == score::Clef::bass ? 14 : 0;
    for (int i = 0; i < count; ++i)
    {
        const auto position = (score_.keyFifths > 0 ? kSharpPositions : kFlatPositions)[static_cast<std::size_t>(i)] - shiftDown;
        drawAccidental(g, score_.keyFifths > 0 ? score::Accidental::sharp : score::Accidental::flat,
                       originX + leftMargin_ + 4.1f * ls + static_cast<float>(i) * 0.95f * ls, diatonicToY(staffIndex, position), ls);
    }
    g.setFont(theme::sansBold(2.9f * ls));
    for (std::size_t b = 0; b < score_.bars.size() && b < signatureX_.size(); ++b)
        if (signatureX_[b] >= 0.0f)
        {
            const float x = originX + signatureX_[b];
            const auto sig = score_.bars[b].signature;
            g.drawText(juce::String(sig.numerator), juce::Rectangle<float>(x, top - 0.25f * ls, kTimeSignatureWidth * ls, 2.3f * ls),
                       juce::Justification::centred, false);
            g.drawText(juce::String(sig.denominator), juce::Rectangle<float>(x, top + 1.85f * ls, kTimeSignatureWidth * ls, 2.3f * ls),
                       juce::Justification::centred, false);
        }

    // Where everything goes.
    std::vector<Placed> placed;
    placed.reserve(staff.events.size());
    for (const auto& e : staff.events)
        placed.push_back(place(staffIndex, e));

    // Beams first: they decide their stems' lengths.
    std::vector<float> stemEnd(staff.events.size(), 0.0f);
    std::map<int, std::vector<std::size_t>> groups;
    for (std::size_t i = 0; i < staff.events.size(); ++i)
        if (!staff.events[i].rest && staff.events[i].beam >= 0)
            groups[staff.events[i].beam].push_back(i);
    struct BeamLine
    {
        std::vector<std::size_t> members;
        float x0, y0, x1, y1;
        bool up;
    };
    std::vector<BeamLine> beams;
    for (const auto& [id, members] : groups)
    {
        const auto& first = staff.events[members.front()];
        const bool up = first.stemUp;
        const auto naturalEnd = [&](std::size_t i)
        {
            const auto& heads = placed[i].heads;
            return up ? heads.back().y - 3.5f * ls : heads.front().y + 3.5f * ls;
        };
        // (for a stem up the last head is the highest; heads are low to high)
        BeamLine beam{members, placed[members.front()].stemX, naturalEnd(members.front()), placed[members.back()].stemX, naturalEnd(members.back()), up};
        const float rise = std::clamp(beam.y1 - beam.y0, -ls, ls);
        beam.y1 = beam.y0 + rise;
        const auto yAt = [&beam](float x) { return beam.x1 - beam.x0 < 0.5f ? beam.y0 : beam.y0 + (beam.y1 - beam.y0) * (x - beam.x0) / (beam.x1 - beam.x0); };
        float shift = 0.0f;
        for (const auto i : members)
        {
            const auto& heads = placed[i].heads;
            const float x = placed[i].stemX;
            if (up)
                shift = std::max(shift, yAt(x) - (heads.back().y - 2.8f * ls));
            else
                shift = std::max(shift, (heads.front().y + 2.8f * ls) - yAt(x));
        }
        beam.y0 += up ? -shift : shift;
        beam.y1 += up ? -shift : shift;
        for (const auto i : members)
            stemEnd[i] = yAt(placed[i].stemX) + (up ? -shift : shift);
        beams.push_back(beam);
    }

    std::set<std::uint32_t> selectedIds;
    for (const auto& note : selection_)
        if (const int index = clip_.indexOf(note); index >= 0)
            selectedIds.insert(static_cast<std::uint32_t>(index));

    for (std::size_t i = 0; i < staff.events.size(); ++i)
    {
        const auto& e = staff.events[i];
        const auto& p = placed[i];
        g.setColour(ink);
        if (e.rest)
        {
            drawRest(g, e.type, e.dots, p.heads.front().x, top, ls);
            continue;
        }
        const bool eventSelected = std::any_of(e.heads.begin(), e.heads.end(), [&](const score::Head& h) { return selectedIds.count(h.id) > 0; });
        const float baseX = beatToX(e.beat);

        // Ledger lines.
        const int high = score::diatonic(e.heads.back().spelling);
        const int low = score::diatonic(e.heads.front().spelling);
        float ledgerLeft = baseX - 0.95f * ls, ledgerRight = baseX + 0.95f * ls;
        for (const auto& h : p.heads)
        {
            ledgerLeft = std::min(ledgerLeft, h.x - 0.95f * ls);
            ledgerRight = std::max(ledgerRight, h.x + 0.95f * ls);
        }
        for (int line = topLine + 2; line <= high; line += 2)
            g.fillRect(ledgerLeft, diatonicToY(staffIndex, line) - 0.5f, ledgerRight - ledgerLeft, 1.0f);
        for (int line = bottomLine - 2; line >= low; line -= 2)
            g.fillRect(ledgerLeft, diatonicToY(staffIndex, line) - 0.5f, ledgerRight - ledgerLeft, 1.0f);

        // Heads, accidentals, dots.
        float leftmost = baseX;
        float rightmost = baseX;
        for (const auto& h : p.heads)
        {
            leftmost = std::min(leftmost, h.x);
            rightmost = std::max(rightmost, h.x);
        }
        for (std::size_t h = 0; h < e.heads.size(); ++h)
        {
            const auto& head = e.heads[h];
            g.setColour(selectedIds.count(head.id) > 0 ? colour::accent : ink);
            drawHead(g, p.heads[h].x, p.heads[h].y, ls, e.type);
            if (head.accidental != score::Accidental::none)
                drawAccidental(g, head.accidental, leftmost - headW * 0.5f - 0.55f * ls - static_cast<float>(p.accidentalColumn[h]) * 1.05f * ls,
                               p.heads[h].y, ls);
            const int d = score::diatonic(head.spelling);
            const float dotY = (d - topLine) % 2 == 0 ? diatonicToY(staffIndex, d + 1) : p.heads[h].y;
            for (int k = 0; k < e.dots; ++k)
                fillCircle(g, rightmost + headW * 0.5f + 0.45f * ls + static_cast<float>(k) * 0.55f * ls, dotY, 0.18f * ls);
        }

        // The stem and flags.
        g.setColour(eventSelected ? colour::accent : ink);
        if (e.type >= 1)
        {
            const int flagCount = std::max(0, e.type - 2);
            const float from = e.stemUp ? p.heads.front().y - 0.2f * ls : p.heads.back().y + 0.2f * ls;
            float to = e.stemUp ? p.heads.back().y - (3.5f + 0.5f * static_cast<float>(std::max(0, flagCount - 1))) * ls
                                : p.heads.front().y + (3.5f + 0.5f * static_cast<float>(std::max(0, flagCount - 1))) * ls;
            if (e.beam >= 0)
                to = stemEnd[i];
            g.drawLine(p.stemX, from, p.stemX, to, 0.14f * ls);
            if (e.beam < 0 && flagCount > 0)
                drawFlags(g, p.stemX, to, e.stemUp, flagCount, ls);
        }

        // Ties to the next event's same notes.
        for (std::size_t h = 0; h < e.heads.size(); ++h)
        {
            if (!e.heads[h].tieStart)
                continue;
            for (std::size_t j = i + 1; j < staff.events.size(); ++j)
            {
                const auto& next = staff.events[j];
                const auto found = std::find_if(next.heads.begin(), next.heads.end(),
                                                [&](const score::Head& o) { return o.tieEnd && o.midi == e.heads[h].midi; });
                if (found == next.heads.end())
                    continue;
                const auto k = static_cast<std::size_t>(std::distance(next.heads.begin(), found));
                const float x0 = p.heads[h].x + 0.6f * ls;
                const float x1 = placed[j].heads[k].x - 0.6f * ls;
                const float dir = e.stemUp ? 1.0f : -1.0f;
                const float y = p.heads[h].y + dir * 0.6f * ls;
                juce::Path tie;
                tie.startNewSubPath(x0, y);
                tie.cubicTo(x0 + (x1 - x0) * 0.25f, y + dir * 0.9f * ls, x0 + (x1 - x0) * 0.75f, y + dir * 0.9f * ls, x1, y);
                tie.cubicTo(x0 + (x1 - x0) * 0.75f, y + dir * 0.6f * ls, x0 + (x1 - x0) * 0.25f, y + dir * 0.6f * ls, x0, y);
                tie.closeSubPath();
                g.setColour(selectedIds.count(e.heads[h].id) > 0 ? colour::accent : ink);
                g.fillPath(tie);
                break;
            }
        }
    }

    // The beams (and the second and third ones of sixteenths and 32nds, with hooks).
    for (const auto& beam : beams)
    {
        const bool selected = std::any_of(beam.members.begin(), beam.members.end(),
                                          [&](std::size_t i) {
                                              return std::any_of(staff.events[i].heads.begin(), staff.events[i].heads.end(),
                                                                 [&](const score::Head& h) { return selectedIds.count(h.id) > 0; });
                                          });
        g.setColour(selected ? colour::accent : ink);
        const float thickness = (beam.up ? 1.0f : -1.0f) * 0.5f * ls;
        const auto yAt = [&beam](float x) { return beam.x1 - beam.x0 < 0.5f ? beam.y0 : beam.y0 + (beam.y1 - beam.y0) * (x - beam.x0) / (beam.x1 - beam.x0); };
        fillBeam(g, beam.x0 - 0.07f * ls, beam.y0, beam.x1 + 0.07f * ls, beam.y1, thickness);
        for (int level = 2; level <= 3; ++level)
        {
            const float offset = (beam.up ? 1.0f : -1.0f) * static_cast<float>(level - 1) * 0.75f * ls;
            const auto levels = [&](std::size_t k) { return staff.events[beam.members[k]].type - 2; };
            for (std::size_t k = 0; k < beam.members.size(); ++k)
            {
                if (levels(k) < level)
                    continue;
                const float x = placed[beam.members[k]].stemX;
                if (k + 1 < beam.members.size() && levels(k + 1) >= level)
                {
                    const float xn = placed[beam.members[k + 1]].stemX;
                    fillBeam(g, x, yAt(x) + offset, xn, yAt(xn) + offset, thickness);
                }
                else if (k == 0 || levels(k - 1) < level)
                {
                    const float hook = (k == 0 ? 1.0f : -1.0f) * 1.0f * ls;
                    fillBeam(g, x, yAt(x) + offset, x + hook, yAt(x + hook) + offset, thickness);
                }
            }
        }
    }
}

// --- Editing -------------------------------------------------------------------------------------------

void ScoreView::preview(int pitch)
{
    if (pitch == previewNote_)
        return;
    stopPreview();
    if (onPreview)
        onPreview(juce::MidiMessage::noteOn(1, pitch, 0.8f));
    previewNote_ = pitch;
}

void ScoreView::stopPreview()
{
    if (previewNote_ >= 0 && onPreview)
        onPreview(juce::MidiMessage::noteOff(1, previewNote_));
    previewNote_ = -1;
}

void ScoreView::insertNoteAt(juce::Point<float> p, juce::ModifierKeys mods)
{
    if (!clip_.isValid())
        return;
    relayout();
    const int staff = staffAt(p.y);
    const double beat = snapBeat(xToBeat(p.x));
    if (beat < clipFrom_ - kEpsilon || beat >= clipTo_ - kEpsilon)
        return;
    const int pitch = pitchAt(staff, p.y, mods);
    session_.beginTransaction("Add Note");
    const auto note = session_.addNote(clip_, beat - origin_, score::valueLength(noteType_, dotted_ ? 1 : 0), pitch, 0.8f);
    selection_ = {note};
    inserted_ = true;
    preview(pitch);
    repaint();
}

void ScoreView::transposeSelection(int semitones)
{
    if (selection_.empty())
        return;
    session_.beginTransaction("Transpose Notes");
    for (auto& n : selection_)
        session::note::setNote(n, std::clamp(session::note::getNote(n) + semitones, 0, 127), &session_.getUndoManager());
}

void ScoreView::moveSelection(double beats)
{
    if (selection_.empty())
        return;
    session_.beginTransaction(selection_.size() == 1 ? "Move Note" : "Move Notes");
    for (auto& n : selection_)
        session::note::setStart(n, std::max(0.0, session::note::getStart(n) + beats), &session_.getUndoManager());
}

void ScoreView::setSelectionValue(int type, std::optional<bool> dotted)
{
    if (type >= 0)
        noteType_ = std::clamp(type, 0, 5);
    if (dotted.has_value())
        dotted_ = *dotted;
    if (!selection_.empty())
    {
        session_.beginTransaction("Change Note Value");
        for (auto& n : selection_)
        {
            const auto [currentType, currentDotted] = valueOf(session::note::getLength(n));
            const int newType = type >= 0 ? noteType_ : currentType;
            const bool newDotted = dotted.has_value() ? *dotted : currentDotted;
            session::note::setLength(n, score::valueLength(newType, newDotted ? 1 : 0), &session_.getUndoManager());
        }
    }
    updateToolbar();
}

void ScoreView::mouseDown(const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    inserted_ = moved_ = false;
    dragBeats_ = 0.0;
    dragSteps_ = 0;
    if (!clip_.isValid() || !scoreArea().contains(e.getPosition()))
        return;
    const auto p = e.position;
    auto hit = noteAt(p);
    if (e.mods.isPopupMenu())
    {
        if (hit.isValid())
        {
            session_.beginTransaction("Delete Note");
            selection_.erase(std::remove(selection_.begin(), selection_.end(), hit), selection_.end());
            session_.removeNote(hit);
        }
        return;
    }
    if (hit.isValid())
    {
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
        dragging_ = std::find(selection_.begin(), selection_.end(), hit) != selection_.end();
        preview(session::note::getNote(hit));
    }
    else if (tool_ == Tool::note)
    {
        insertNoteAt(p, e.mods);
        dragging_ = inserted_;
    }
    else
    {
        lassoing_ = true;
        lasso_ = {p, p};
        lassoBase_ = e.mods.isShiftDown() || e.mods.isCommandDown() ? selection_ : std::vector<juce::ValueTree>{};
        selection_ = lassoBase_;
    }
    if (dragging_)
    {
        relayout();
        dragStart_ = p;
        dragColumns_ = columns_;
        dragOriginals_.clear();
        for (const auto& n : selection_)
            dragOriginals_.push_back({n, session::note::getStart(n), session::note::getNote(n)});
    }
    repaint();
}

void ScoreView::mouseDrag(const juce::MouseEvent& e)
{
    if (lassoing_)
    {
        relayout();
        lasso_ = juce::Rectangle<float>(e.mouseDownPosition, e.position);
        auto selection = lassoBase_;
        for (std::size_t s = 0; s < score_.staves.size(); ++s)
            for (const auto& ev : score_.staves[s].events)
            {
                if (ev.rest)
                    continue;
                const auto placed = place(static_cast<int>(s), ev);
                for (std::size_t h = 0; h < ev.heads.size(); ++h)
                    if (lasso_.contains(placed.heads[h]))
                    {
                        const auto note = clip_.getChild(static_cast<int>(ev.heads[h].id));
                        if (std::find(selection.begin(), selection.end(), note) == selection.end())
                            selection.push_back(note);
                    }
            }
        selection_ = std::move(selection);
        repaint();
        return;
    }
    if (!dragging_ || dragOriginals_.empty())
        return;
    const double grid = getOptions().grid;
    const float toContent = scrollX_ - static_cast<float>(scoreArea().getX());
    const double beats = std::round((beatOnColumns(dragColumns_, e.position.x + toContent, pixelsPerBeat_, clipFrom_, headerWidth_)
                                     - beatOnColumns(dragColumns_, dragStart_.x + toContent, pixelsPerBeat_, clipFrom_, headerWidth_))
                                    / grid)
                         * grid;
    const int steps = juce::roundToInt((dragStart_.y - e.position.y) / (lineSpacing() * 0.5f));
    if (std::abs(beats - dragBeats_) < kEpsilon && steps == dragSteps_)
        return;
    dragBeats_ = beats;
    dragSteps_ = steps;
    if (!moved_ && !inserted_)
        session_.beginTransaction(dragOriginals_.size() == 1 ? "Move Note" : "Move Notes");
    moved_ = true;
    const int key = getScore().keyFifths;
    for (const auto& o : dragOriginals_)
    {
        int pitch = o.pitch;
        if (steps != 0)
        {
            const auto spelling = score::spell(o.pitch, key);
            const int d = score::diatonic(spelling) + steps;
            const int step = ((d % 7) + 7) % 7;
            pitch = std::clamp(score::midiOf({step, (d - step) / 7, score::keyAlteration(step, key)}), 0, 127);
        }
        auto note = o.note;
        session::note::setStart(note, std::max(0.0, o.start + beats), &session_.getUndoManager());
        session::note::setNote(note, pitch, &session_.getUndoManager());
    }
    preview(session::note::getNote(dragOriginals_.front().note));
}

void ScoreView::mouseUp(const juce::MouseEvent&)
{
    stopPreview();
    dragging_ = lassoing_ = false;
    inserted_ = moved_ = false;
    dragOriginals_.clear();
    repaint();
}

void ScoreView::mouseMove(const juce::MouseEvent& e)
{
    hover_ = e.position;
    hoverMods_ = e.mods;
    if (tool_ == Tool::note)
        repaint();
}

void ScoreView::mouseExit(const juce::MouseEvent&)
{
    hover_ = {-1.0f, -1.0f};
    repaint();
}

void ScoreView::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (e.mods.isCommandDown())
    {
        const double beat = xToBeat(e.position.x);
        pixelsPerBeat_ = std::clamp(pixelsPerBeat_ * (1.0f + wheel.deltaY * 0.6f), 24.0f, 320.0f);
        dirty_ = true;
        relayout();
        scrollX_ = contentX(beat) - (e.position.x - static_cast<float>(scoreArea().getX()));
        updateScrollBar();
        repaint();
        return;
    }
    const float delta = std::abs(wheel.deltaX) > std::abs(wheel.deltaY) ? wheel.deltaX : wheel.deltaY;
    scrollX_ -= delta * 300.0f;
    updateScrollBar();
    repaint();
}

bool ScoreView::keyPressed(const juce::KeyPress& key)
{
    const int code = key.getKeyCode();
    const auto mods = key.getModifiers();
    const auto ch = key.getTextCharacter() != 0 ? key.getTextCharacter() : static_cast<juce::juce_wchar>(code);
    if (code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey)
    {
        if (selection_.empty())
            return false;
        session_.beginTransaction(selection_.size() == 1 ? "Delete Note" : "Delete Notes");
        const auto notes = selection_;
        selection_.clear();
        for (const auto& n : notes)
            session_.removeNote(n);
        return true;
    }
    if (code == juce::KeyPress::upKey || code == juce::KeyPress::downKey)
    {
        if (selection_.empty())
            return false;
        transposeSelection((code == juce::KeyPress::upKey ? 1 : -1) * (mods.isCommandDown() ? 12 : 1));
        return true;
    }
    if (code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey)
    {
        if (selection_.empty())
            return false;
        moveSelection((code == juce::KeyPress::rightKey ? 1.0 : -1.0) * getOptions().grid);
        return true;
    }
    if (code == juce::KeyPress::escapeKey)
    {
        selection_.clear();
        repaint();
        return true;
    }
    if (mods.isCommandDown() && (code == 'A' || code == 'a'))
    {
        selection_.clear();
        for (int i = 0; i < clip_.getNumChildren(); ++i)
            if (clip_.getChild(i).hasType(session::ids::note))
                selection_.push_back(clip_.getChild(i));
        repaint();
        return true;
    }
    if (mods.isCommandDown() || mods.isAltDown())
        return false;
    if (ch >= '1' && ch <= '6')
    {
        setSelectionValue(static_cast<int>(ch - '1'), std::nullopt);
        return true;
    }
    if (ch == '.')
    {
        const bool dotted = selection_.empty() ? !dotted_ : !valueOf(session::note::getLength(selection_.front())).second;
        setSelectionValue(-1, dotted);
        return true;
    }
    if (ch == 'n' || ch == 'N')
    {
        setTool(Tool::note);
        return true;
    }
    if (ch == 's' || ch == 'S')
    {
        setTool(Tool::select);
        return true;
    }
    return false;
}

// --- Plumbing -------------------------------------------------------------------------------------------

bool ScoreView::concerns(const juce::ValueTree& tree) const
{
    if (!clip_.isValid())
        return false;
    return tree == clip_ || tree.isAChildOf(clip_) || tree.hasType(session::ids::tempoMap) || tree.getParent().hasType(session::ids::tempoMap)
           || tree == session_.getState();
}

void ScoreView::valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier&)
{
    if (concerns(tree))
        invalidate();
}

void ScoreView::valueTreeChildAdded(juce::ValueTree& parent, juce::ValueTree&)
{
    if (concerns(parent))
        invalidate();
}

void ScoreView::valueTreeChildRemoved(juce::ValueTree& parent, juce::ValueTree& child, int)
{
    if (!concerns(parent))
        return;
    selection_.erase(std::remove(selection_.begin(), selection_.end(), child), selection_.end());
    invalidate();
}

void ScoreView::scrollBarMoved(juce::ScrollBar*, double start)
{
    scrollX_ = static_cast<float>(start);
    repaint();
}

void ScoreView::updateScrollBar()
{
    relayout();
    const float visible = static_cast<float>(scoreArea().getWidth());
    scrollX_ = std::clamp(scrollX_, 0.0f, std::max(0.0f, contentWidth_ - visible));
    hScroll_.setRangeLimits(0.0, std::max(static_cast<double>(contentWidth_), static_cast<double>(visible)), juce::dontSendNotification);
    hScroll_.setCurrentRange(scrollX_, visible, juce::dontSendNotification);
}

void ScoreView::updateToolbar()
{
    selectTool_.setToggleState(tool_ == Tool::select, juce::dontSendNotification);
    noteTool_.setToggleState(tool_ == Tool::note, juce::dontSendNotification);
    for (std::size_t i = 0; i < valueButtons_.size(); ++i)
        valueButtons_[i].setToggleState(static_cast<int>(i) == noteType_, juce::dontSendNotification);
    dotButton_.setToggleState(dotted_, juce::dontSendNotification);
    const auto options = getOptions();
    clefBox_.setSelectedId(static_cast<int>(options.clef) + 1, juce::dontSendNotification);
    keyBox_.changeItemText(1, juce::String("Key: detected (") + score::keyName(getScore().keyFifths) + ")");
    keyBox_.setSelectedId(options.key.has_value() ? *options.key + 9 : 1, juce::dontSendNotification);
    int grid = 3;
    for (std::size_t i = 0; i < kGridBeats.size(); ++i)
        if (std::abs(kGridBeats[i] - options.grid) < kEpsilon)
            grid = static_cast<int>(i) + 1;
    gridBox_.setSelectedId(grid, juce::dontSendNotification);
    for (auto* c : std::initializer_list<juce::Component*>{&clefBox_, &keyBox_, &gridBox_, &exportButton_, &dotButton_})
        c->setEnabled(clip_.isValid());
}

void ScoreView::resized()
{
    auto toolbar = getLocalBounds().removeFromTop(kToolbarHeight).reduced(6, 3);
    selectTool_.setBounds(toolbar.removeFromLeft(56));
    noteTool_.setBounds(toolbar.removeFromLeft(50));
    toolbar.removeFromLeft(10);
    for (auto& b : valueButtons_)
        b.setBounds(toolbar.removeFromLeft(40));
    dotButton_.setBounds(toolbar.removeFromLeft(26));
    toolbar.removeFromLeft(10);
    clefBox_.setBounds(toolbar.removeFromLeft(112));
    toolbar.removeFromLeft(6);
    keyBox_.setBounds(toolbar.removeFromLeft(220));
    toolbar.removeFromLeft(6);
    gridBox_.setBounds(toolbar.removeFromLeft(92));
    toolbar.removeFromLeft(10);
    exportButton_.setBounds(toolbar.removeFromLeft(96));
    hScroll_.setBounds(getLocalBounds().removeFromBottom(kScrollBarHeight));
    updateScrollBar();
}

} // namespace dojo::ui
