#include "ui/components/SpectralEditor.h"

#include "ui/theme/DojoTheme.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace dojo::ui
{
namespace
{
constexpr int kToolbarHeight = 30;
constexpr int kScrollBarSize = 12;
constexpr int kAxisWidth = 44;
constexpr int kRulerHeight = 16;
constexpr int kListWidth = 250;
constexpr int kRowHeight = 22;
constexpr double kLowHz = 20.0;

namespace edits = session::audioedits;
using audioedit::spectral::Kind;

/** Dark to bright: ink, indigo, magenta, vermilion, pale gold. */
juce::Colour heat(std::uint8_t level)
{
    static const std::array<juce::Colour, 256> palette = []
    {
        std::array<juce::Colour, 256> p;
        const juce::Colour stops[] = {juce::Colour(0xff06070a), juce::Colour(0xff1b1e5a), juce::Colour(0xff7a1f6b), juce::Colour(0xffe0533d),
                                      juce::Colour(0xffffe8a0)};
        for (int i = 0; i < 256; ++i)
        {
            const float x = static_cast<float>(i) / 255.0f * 4.0f;
            const int a = std::min(3, static_cast<int>(x));
            p[static_cast<std::size_t>(i)] = stops[a].interpolatedWith(stops[a + 1], x - static_cast<float>(a));
        }
        return p;
    }();
    return palette[level];
}

juce::String hzText(double hz) { return hz >= 1000.0 ? juce::String(hz / 1000.0, hz >= 10000.0 ? 0 : 1) + "k" : juce::String(juce::roundToInt(hz)); }
} // namespace

SpectralEditor::SpectralEditor(session::Project& project)
    : session_(project.getSession())
    , edits_(project.getAudioEdits())
{
    setWantsKeyboardFocus(true);
    setOpaque(true);

    gainSlider_.setSliderStyle(juce::Slider::LinearHorizontal);
    gainSlider_.setRange(-40.0, 12.0, 0.5);
    gainSlider_.setValue(-12.0, juce::dontSendNotification);
    gainSlider_.setTextValueSuffix(" dB");
    gainSlider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 20);
    gainSlider_.onValueChange = [this] { gainButton_.setButtonText(gainSlider_.getValue() > 0.0 ? "Boost" : "Attenuate"); };
    gainButton_.setTooltip("The selection quieter (or louder) by the gain shown");
    gainButton_.onClick = [this] { applyGain(gainSlider_.getValue()); };
    eraseButton_.setTooltip("Remove the selection");
    eraseButton_.onClick = [this] { erase(); };
    repairButton_.setTooltip("Rebuild the selection from the sound around it (clicks, coughs, squeaks)");
    repairButton_.onClick = [this] { repair(); };
    learnButton_.setTooltip("Take the selection's sound (noise alone) as the noise to remove");
    learnButton_.onClick = [this] { learnNoise(); };
    denoiseButton_.setTooltip("Remove the learnt noise from the selection's time (or the whole clip)");
    denoiseButton_.onClick = [this] { denoise(reductionSlider_.getValue()); };
    reductionSlider_.setSliderStyle(juce::Slider::LinearHorizontal);
    reductionSlider_.setRange(3.0, 40.0, 1.0);
    reductionSlider_.setValue(18.0, juce::dontSendNotification);
    reductionSlider_.setTextValueSuffix(" dB less");
    reductionSlider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 76, 20);
    reductionSlider_.setTooltip("De-noise: the most it takes away");
    humBox_.addItem("Hum: find", 1);
    humBox_.addItem("50 Hz", 2);
    humBox_.addItem("60 Hz", 3);
    humBox_.setSelectedId(1, juce::dontSendNotification);
    dehumButton_.setTooltip("Notch mains hum and its harmonics over the selection's time (or the whole clip)");
    dehumButton_.onClick = [this] { dehum(humBox_.getSelectedId() == 2 ? 50.0 : humBox_.getSelectedId() == 3 ? 60.0 : 0.0); };
    originalButton_.setClickingTogglesState(true);
    originalButton_.setTooltip("Play the clip as it was recorded (A/B)");
    originalButton_.onClick = [this] { edits_.setBypassed(clip_, originalButton_.getToggleState()); };
    for (auto* c : std::initializer_list<juce::Component*>{&gainButton_, &gainSlider_, &eraseButton_, &repairButton_, &learnButton_, &denoiseButton_,
                                                           &reductionSlider_, &dehumButton_, &humBox_, &originalButton_})
        addAndMakeVisible(c);
    status_.setFont(theme::sans(12.0f));
    status_.setColour(juce::Label::textColourId, theme::colour::textSecondary);
    addAndMakeVisible(status_);
    hScroll_.setAutoHide(false);
    hScroll_.addListener(this);
    addAndMakeVisible(hScroll_);

    edits_.addChangeListener(this);
    session_.getState().addListener(this);
}

SpectralEditor::~SpectralEditor()
{
    session_.getState().removeListener(this);
    edits_.removeChangeListener(this);
}

void SpectralEditor::setClip(const juce::ValueTree& clip)
{
    if (clip == clip_)
        return;
    clip_ = clip;
    selection_ = {};
    selectedOperation_ = {};
    noiseProfile_.clear();
    shown_.reset();
    image_ = {};
    viewStart_ = 0.0;
    const auto area = getSpectrumArea();
    if (area.getWidth() > 20 && clipLength() > 0.0)
        pixelsPerSecond_ = std::clamp((area.getWidth() - 10) / clipLength(), 2.0, 4000.0);
    refresh();
}

void SpectralEditor::setPlayhead(double timelineSeconds)
{
    const double t = clip_.isValid() ? timelineSeconds - session::clip::getStart(clip_) : -1.0;
    if (std::abs(t - playhead_) < 1.0e-3)
        return;
    playhead_ = t;
    repaint();
}

void SpectralEditor::setSelection(const Selection& selection)
{
    selection_ = selection;
    refresh();
}

double SpectralEditor::clipLength() const { return clip_.isValid() ? session::clip::getLength(clip_) : 1.0; }

double SpectralEditor::topHz() const { return shown_ != nullptr ? shown_->highHz : 20000.0; }

// --- Actions -------------------------------------------------------------------------------------------

audioedit::spectral::Operation SpectralEditor::fromSelection(Kind kind, bool wholeIfNone) const
{
    audioedit::spectral::Operation op;
    op.kind = kind;
    if (selection_.isEmpty())
    {
        if (!wholeIfNone)
            return op;
        op.start = 0.0;
        op.end = clipLength();
        return op;
    }
    op.start = selection_.start;
    op.end = selection_.end;
    // (a band reaching the axis's ends is open-ended there)
    op.low = selection_.low <= kLowHz * 1.01 ? 0.0 : selection_.low;
    op.high = selection_.high >= topHz() * 0.99 ? 1.0e6 : selection_.high;
    return op;
}

void SpectralEditor::add(audioedit::spectral::Operation op, const juce::String& undoName)
{
    if (!clip_.isValid() || op.end <= op.start)
    {
        status_.setText("Select a part of the spectrogram first (drag across it).", juce::dontSendNotification);
        return;
    }
    selectedOperation_ = edits_.addSpectralOperation(clip_, op, undoName);
    refresh();
}

void SpectralEditor::applyGain(double gainDb)
{
    auto op = fromSelection(Kind::gain, false);
    op.gainDb = gainDb;
    add(op, gainDb > 0.0 ? "Boost" : "Attenuate");
}

void SpectralEditor::erase() { add(fromSelection(Kind::erase, false), "Erase"); }

void SpectralEditor::repair() { add(fromSelection(Kind::repair, false), "Repair"); }

bool SpectralEditor::learnNoise()
{
    if (selection_.isEmpty())
    {
        status_.setText("Select a stretch of noise alone, then Learn Noise.", juce::dontSendNotification);
        return false;
    }
    juce::String error;
    auto profile = edits_.learnNoise(clip_, selection_.start, selection_.end, error);
    if (profile.empty())
    {
        status_.setText(error, juce::dontSendNotification);
        return false;
    }
    noiseProfile_ = std::move(profile);
    status_.setText("Noise learnt from " + juce::String(selection_.end - selection_.start, 2) + " s", juce::dontSendNotification);
    denoiseButton_.setEnabled(true);
    return true;
}

void SpectralEditor::denoise(double reductionDb)
{
    if (noiseProfile_.empty())
    {
        status_.setText("Learn the noise first (select noise alone, Learn Noise).", juce::dontSendNotification);
        return;
    }
    auto op = fromSelection(Kind::denoise, true);
    op.low = 0.0;
    op.high = 1.0e6;
    op.profile = noiseProfile_;
    op.reductionDb = reductionDb;
    add(op, "De-noise");
}

void SpectralEditor::dehum(double humHz)
{
    auto op = fromSelection(Kind::dehum, true);
    op.low = 0.0;
    op.high = 1.0e6;
    op.humHz = humHz;
    add(op, "De-hum");
}

// --- Geometry ----------------------------------------------------------------------------------------------

juce::Rectangle<int> SpectralEditor::getOperationsArea() const
{
    auto area = getLocalBounds().withTrimmedTop(kToolbarHeight);
    return area.removeFromRight(kListWidth);
}

juce::Rectangle<int> SpectralEditor::getSpectrumArea() const
{
    auto area = getLocalBounds().withTrimmedTop(kToolbarHeight);
    area.removeFromRight(kListWidth);
    area.removeFromBottom(kScrollBarSize);
    area.removeFromLeft(kAxisWidth);
    area.removeFromTop(kRulerHeight);
    return area;
}

float SpectralEditor::timeToX(double clipSeconds) const
{
    return static_cast<float>(getSpectrumArea().getX()) + static_cast<float>((clipSeconds - viewStart_) * pixelsPerSecond_);
}

double SpectralEditor::xToTime(float x) const { return viewStart_ + static_cast<double>(x - static_cast<float>(getSpectrumArea().getX())) / pixelsPerSecond_; }

float SpectralEditor::hzToY(double hz) const
{
    const auto area = getSpectrumArea().toFloat();
    const double u = std::log(std::max(hz, 1.0) / kLowHz) / std::log(topHz() / kLowHz);
    return area.getBottom() - static_cast<float>(u) * area.getHeight();
}

double SpectralEditor::yToHz(float y) const
{
    const auto area = getSpectrumArea().toFloat();
    const double u = (area.getBottom() - y) / std::max(1.0f, area.getHeight());
    return kLowHz * std::pow(topHz() / kLowHz, u);
}

int SpectralEditor::operationRowAt(juce::Point<int> p) const
{
    const auto list = getOperationsArea().withTrimmedTop(kRulerHeight + 4);
    if (!list.contains(p))
        return -1;
    const int row = (p.y - list.getY()) / kRowHeight;
    return row < static_cast<int>(edits::getSpectralOperations(clip_).size()) ? row : -1;
}

juce::Rectangle<int> SpectralEditor::getOperationTick(int row) const
{
    const auto list = getOperationsArea().withTrimmedTop(kRulerHeight + 4);
    return {list.getX() + 6, list.getY() + row * kRowHeight + 3, 16, 16};
}

juce::Rectangle<int> SpectralEditor::getOperationCross(int row) const
{
    const auto list = getOperationsArea().withTrimmedTop(kRulerHeight + 4);
    return {list.getRight() - 22, list.getY() + row * kRowHeight + 3, 16, 16};
}

// --- State ---------------------------------------------------------------------------------------------

bool SpectralEditor::concerns(const juce::ValueTree& tree) const { return clip_.isValid() && (tree == clip_ || tree.isAChildOf(clip_)); }

void SpectralEditor::valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier&)
{
    if (concerns(tree))
        refresh();
}

void SpectralEditor::valueTreeChildAdded(juce::ValueTree& parent, juce::ValueTree&)
{
    if (concerns(parent))
        refresh();
}

void SpectralEditor::valueTreeChildRemoved(juce::ValueTree& parent, juce::ValueTree& child, int)
{
    if (!concerns(parent) && parent != clip_)
        return;
    if (selectedOperation_ == child || selectedOperation_.isAChildOf(child))
        selectedOperation_ = {};
    refresh();
}

void SpectralEditor::changeListenerCallback(juce::ChangeBroadcaster*) { refresh(); }

void SpectralEditor::timerCallback() { refresh(); }

void SpectralEditor::refresh()
{
    if (!clip_.isValid())
    {
        repaint();
        return;
    }
    // (asks for the current spectrogram; the last one shows until it arrives)
    if (isShowing() || isVisible())
    {
        const auto spectrogram = edits_.getSpectrogram(clip_);
        if (spectrogram != shown_)
        {
            shown_ = spectrogram;
            shownStart_ = edits_.getSpectrogramStart(clip_);
            rebuildImage();
        }
    }
    const auto state = edits_.getState(clip_);
    const bool busy = state.status == session::AudioEdits::Status::rendering || state.status == session::AudioEdits::Status::analysing || edits_.isBusy();
    if (busy && !isTimerRunning())
        startTimerHz(8);
    else if (!busy && isTimerRunning())
        stopTimer();
    juce::String text;
    if (state.status == session::AudioEdits::Status::failed)
        text = "Failed: " + state.problem;
    else if (shown_ == nullptr)
        text = "Making the spectrogram...";
    else if (state.status == session::AudioEdits::Status::rendering)
        text = "Rendering...";
    else if (!selection_.isEmpty())
        text = juce::String(selection_.start, 2) + "-" + juce::String(selection_.end, 2) + " s   " + hzText(selection_.low) + "-" + hzText(std::min(selection_.high, topHz()))
               + " Hz";
    else
        text = juce::String(static_cast<int>(edits::getSpectralOperations(clip_).size())) + " edits";
    if (!status_.getText().startsWith("Noise learnt") || !selection_.isEmpty())
        status_.setText(text, juce::dontSendNotification);
    denoiseButton_.setEnabled(!noiseProfile_.empty());
    originalButton_.setEnabled(edits::getEdit(clip_).isValid());
    originalButton_.setToggleState(session::AudioEdits::isBypassed(clip_), juce::dontSendNotification);
    updateScrollBar();
    repaint();
}

void SpectralEditor::rebuildImage()
{
    image_ = {};
    if (shown_ == nullptr || shown_->frames <= 0)
        return;
    const int width = std::min(shown_->frames, 4096);
    image_ = juce::Image(juce::Image::RGB, width, shown_->rows, false);
    juce::Image::BitmapData pixels(image_, juce::Image::BitmapData::writeOnly);
    for (int x = 0; x < width; ++x)
    {
        // (each column the loudest of the frames it stands for)
        const int f0 = static_cast<int>(static_cast<long long>(x) * shown_->frames / width);
        const int f1 = std::max(f0 + 1, static_cast<int>(static_cast<long long>(x + 1) * shown_->frames / width));
        for (int r = 0; r < shown_->rows; ++r)
        {
            std::uint8_t level = 0;
            for (int f = f0; f < f1; ++f)
                level = std::max(level, shown_->at(f, r));
            pixels.setPixelColour(x, shown_->rows - 1 - r, heat(level));
        }
    }
}

void SpectralEditor::updateScrollBar()
{
    const double visible = getSpectrumArea().getWidth() / pixelsPerSecond_;
    hScroll_.setRangeLimits(0.0, std::max(clipLength(), viewStart_ + visible), juce::dontSendNotification);
    hScroll_.setCurrentRange(viewStart_, visible, juce::dontSendNotification);
}

void SpectralEditor::scrollBarMoved(juce::ScrollBar*, double start)
{
    viewStart_ = start;
    repaint();
}

// --- Painting ------------------------------------------------------------------------------------------

void SpectralEditor::paint(juce::Graphics& g)
{
    using namespace theme;
    g.fillAll(colour::panel);
    g.setColour(colour::panelHeader);
    g.fillRect(getLocalBounds().removeFromTop(kToolbarHeight));
    const auto area = getSpectrumArea();
    g.setColour(juce::Colour(0xff06070a));
    g.fillRect(area);
    if (!clip_.isValid())
        return;

    // The spectrogram: its time (seconds of the source from shownStart_) laid on the clip's.
    if (image_.isValid() && shown_ != nullptr)
    {
        juce::Graphics::ScopedSaveState save(g);
        g.reduceClipRegion(area);
        g.setImageResamplingQuality(juce::Graphics::lowResamplingQuality);
        const float x0 = timeToX(session::clip::clipTimeOf(clip_, shownStart_));
        const float x1 = timeToX(session::clip::clipTimeOf(clip_, shownStart_ + shown_->duration));
        const float top = hzToY(shown_->highHz);
        const float bottom = hzToY(shown_->lowHz);
        g.drawImage(image_, juce::Rectangle<float>(x0, top, std::max(1.0f, x1 - x0), bottom - top), juce::RectanglePlacement::stretchToFit);
    }
    else
    {
        g.setColour(colour::textMuted);
        g.setFont(sans(13.0f));
        g.drawText("Making the spectrogram...", area, juce::Justification::centred);
    }

    // The frequency axis and the time ruler.
    g.setFont(sans(9.5f));
    for (const double hz : {50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0})
    {
        if (hz > topHz())
            continue;
        const float y = hzToY(hz);
        g.setColour(colour::outline.withAlpha(0.6f));
        g.fillRect(static_cast<float>(area.getX()), y, static_cast<float>(area.getWidth()), 1.0f);
        g.setColour(colour::textMuted);
        g.drawText(hzText(hz), juce::Rectangle<float>(static_cast<float>(area.getX() - kAxisWidth), y - 6.0f, static_cast<float>(kAxisWidth - 4), 12.0f),
                   juce::Justification::centredRight, false);
    }
    const auto ruler = juce::Rectangle<int>(area.getX(), area.getY() - kRulerHeight, area.getWidth(), kRulerHeight);
    g.setColour(colour::panelRaised);
    g.fillRect(ruler);
    g.setColour(colour::textMuted);
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

    {
        juce::Graphics::ScopedSaveState save(g);
        g.reduceClipRegion(area);
        // The edits, where they are.
        const auto ops = edits::getSpectralOperations(clip_);
        for (const auto& op : ops)
        {
            const auto o = edits::toOperation(op, 0.0);
            const float x0 = timeToX(session::clip::clipTimeOf(clip_, o.start));
            const float x1 = timeToX(session::clip::clipTimeOf(clip_, o.end));
            const float y0 = o.kind == Kind::dehum || o.kind == Kind::denoise ? static_cast<float>(area.getY()) : hzToY(std::min(o.high, topHz()));
            const float y1 = o.kind == Kind::dehum || o.kind == Kind::denoise ? static_cast<float>(area.getBottom()) : hzToY(std::max(o.low, kLowHz));
            const auto box = juce::Rectangle<float>(x0, y0, std::max(2.0f, x1 - x0), std::max(2.0f, y1 - y0));
            const bool chosen = op == selectedOperation_;
            g.setColour((o.enabled ? colour::gold : colour::textMuted).withAlpha(chosen ? 1.0f : 0.75f));
            const float dashes[] = {4.0f, 3.0f};
            juce::Path outline;
            outline.addRectangle(box);
            juce::Path dashed;
            juce::PathStrokeType(chosen ? 2.0f : 1.0f).createDashedStroke(dashed, outline, dashes, 2);
            g.fillPath(dashed);
            g.setFont(sans(10.0f));
            g.drawText(edits::describe(op).upToFirstOccurrenceOf("   ", false, false), box.withHeight(12.0f).translated(3.0f, 2.0f).withWidth(140.0f),
                       juce::Justification::centredLeft, false);
        }
        if (!selection_.isEmpty())
        {
            const auto box = juce::Rectangle<float>::leftTopRightBottom(timeToX(selection_.start), hzToY(std::min(selection_.high, topHz())),
                                                                        timeToX(selection_.end), hzToY(std::max(selection_.low, kLowHz)));
            g.setColour(colour::accent.withAlpha(0.18f));
            g.fillRect(box);
            g.setColour(colour::accentBright);
            g.drawRect(box, 1.5f);
        }
        if (playhead_ >= 0.0)
        {
            g.setColour(colour::playhead);
            g.fillRect(timeToX(playhead_) - 0.5f, static_cast<float>(area.getY()), 1.5f, static_cast<float>(area.getHeight()));
        }
    }

    // The list of edits.
    const auto listArea = getOperationsArea();
    g.setColour(colour::panelRaised);
    g.fillRect(listArea);
    g.setColour(colour::outline);
    g.fillRect(listArea.withWidth(1));
    g.setColour(colour::textSecondary);
    g.setFont(label(10.5f));
    g.drawText("SPECTRAL EDITS", listArea.withHeight(kRulerHeight + 4).reduced(8, 0), juce::Justification::centredLeft, false);
    const auto ops = edits::getSpectralOperations(clip_);
    for (int row = 0; row < static_cast<int>(ops.size()); ++row)
    {
        const auto& op = ops[static_cast<std::size_t>(row)];
        const auto tick = getOperationTick(row);
        const auto cross = getOperationCross(row);
        const auto line = juce::Rectangle<int>(listArea.getX(), tick.getY() - 3, listArea.getWidth(), kRowHeight);
        if (op == selectedOperation_)
        {
            g.setColour(colour::control);
            g.fillRect(line);
        }
        const bool enabled = static_cast<bool>(op.getProperty(session::ids::enabled, true));
        g.setColour(colour::outlineStrong);
        g.drawRect(tick, 1);
        if (enabled)
        {
            g.setColour(colour::gold);
            g.fillRect(tick.reduced(4));
        }
        g.setColour(enabled ? colour::textPrimary : colour::textMuted);
        g.setFont(sans(11.0f));
        g.drawText(edits::describe(op), line.withTrimmedLeft(28).withTrimmedRight(26), juce::Justification::centredLeft, true);
        g.setColour(colour::textMuted);
        g.drawLine(static_cast<float>(cross.getX() + 4), static_cast<float>(cross.getY() + 4), static_cast<float>(cross.getRight() - 4),
                   static_cast<float>(cross.getBottom() - 4), 1.5f);
        g.drawLine(static_cast<float>(cross.getRight() - 4), static_cast<float>(cross.getY() + 4), static_cast<float>(cross.getX() + 4),
                   static_cast<float>(cross.getBottom() - 4), 1.5f);
    }
    if (ops.empty())
    {
        g.setColour(colour::textMuted);
        g.setFont(sans(11.0f));
        g.drawFittedText("Drag across the spectrogram to select, then Attenuate, Erase or Repair it; or Learn Noise and De-noise, or De-hum.",
                         listArea.withTrimmedTop(kRulerHeight + 8).reduced(10, 0).withHeight(80), juce::Justification::topLeft, 5);
    }
}

void SpectralEditor::resized()
{
    auto toolbar = getLocalBounds().removeFromTop(kToolbarHeight).reduced(6, 3);
    gainButton_.setBounds(toolbar.removeFromLeft(78));
    gainSlider_.setBounds(toolbar.removeFromLeft(140));
    toolbar.removeFromLeft(4);
    eraseButton_.setBounds(toolbar.removeFromLeft(56));
    repairButton_.setBounds(toolbar.removeFromLeft(62));
    toolbar.removeFromLeft(12);
    learnButton_.setBounds(toolbar.removeFromLeft(88));
    denoiseButton_.setBounds(toolbar.removeFromLeft(72));
    reductionSlider_.setBounds(toolbar.removeFromLeft(150));
    toolbar.removeFromLeft(12);
    dehumButton_.setBounds(toolbar.removeFromLeft(62));
    humBox_.setBounds(toolbar.removeFromLeft(96));
    toolbar.removeFromLeft(12);
    originalButton_.setBounds(toolbar.removeFromLeft(70));
    toolbar.removeFromLeft(8);
    status_.setBounds(toolbar);
    auto bottom = getLocalBounds().withTrimmedRight(kListWidth);
    hScroll_.setBounds(bottom.removeFromBottom(kScrollBarSize).withTrimmedLeft(kAxisWidth));
    updateScrollBar();
}

// --- Mouse ---------------------------------------------------------------------------------------------

void SpectralEditor::mouseDown(const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    if (!clip_.isValid())
        return;
    const auto p = e.getPosition();
    if (const int row = operationRowAt(p); row >= 0)
    {
        auto op = edits::getSpectralOperations(clip_)[static_cast<std::size_t>(row)];
        if (getOperationTick(row).expanded(2).contains(p))
        {
            const bool enabled = !static_cast<bool>(op.getProperty(session::ids::enabled, true));
            session_.beginTransaction(enabled ? "Enable Spectral Edit" : "Bypass Spectral Edit");
            edits::setOperationEnabled(op, enabled, &session_.getUndoManager());
        }
        else if (getOperationCross(row).expanded(2).contains(p))
            edits_.removeSpectralOperation(op);
        else
        {
            selectedOperation_ = op;
            const auto o = edits::toOperation(op, 0.0);
            selection_ = {session::clip::clipTimeOf(clip_, o.start), session::clip::clipTimeOf(clip_, o.end), std::max(o.low, kLowHz),
                          std::min(o.high, topHz())};
        }
        refresh();
        return;
    }
    if (!getSpectrumArea().contains(p))
        return;
    selecting_ = true;
    dragStart_ = e.position;
    selection_ = {};
    selectedOperation_ = {};
    refresh();
}

void SpectralEditor::mouseDrag(const juce::MouseEvent& e)
{
    if (!selecting_)
        return;
    const auto area = getSpectrumArea().toFloat();
    const auto a = dragStart_;
    const auto b = juce::Point<float>(std::clamp(e.position.x, area.getX(), area.getRight()), std::clamp(e.position.y, area.getY(), area.getBottom()));
    selection_.start = std::max(0.0, xToTime(std::min(a.x, b.x)));
    selection_.end = std::min(clipLength(), xToTime(std::max(a.x, b.x)));
    if (e.mods.isShiftDown())
    {
        selection_.low = kLowHz;
        selection_.high = topHz();
    }
    else
    {
        selection_.low = yToHz(std::max(a.y, b.y));
        selection_.high = yToHz(std::min(a.y, b.y));
    }
    refresh();
}

void SpectralEditor::mouseUp(const juce::MouseEvent&)
{
    selecting_ = false;
    if (selection_.end - selection_.start < 0.002)
        selection_ = {};
    refresh();
}

void SpectralEditor::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (e.mods.isCommandDown())
    {
        const double t = xToTime(e.position.x);
        pixelsPerSecond_ = std::clamp(pixelsPerSecond_ * (1.0 + wheel.deltaY * 0.8), 2.0, 8000.0);
        viewStart_ = std::max(0.0, t - (e.position.x - static_cast<float>(getSpectrumArea().getX())) / pixelsPerSecond_);
    }
    else
        viewStart_ = std::max(0.0, viewStart_ - (std::abs(wheel.deltaX) > 0.0f ? wheel.deltaX : wheel.deltaY) * 300.0 / pixelsPerSecond_);
    updateScrollBar();
    repaint();
}

bool SpectralEditor::keyPressed(const juce::KeyPress& key)
{
    if (key.getKeyCode() == juce::KeyPress::escapeKey)
    {
        setSelection({});
        return true;
    }
    if ((key.getKeyCode() == juce::KeyPress::deleteKey || key.getKeyCode() == juce::KeyPress::backspaceKey) && selectedOperation_.isValid())
    {
        edits_.removeSpectralOperation(selectedOperation_);
        selectedOperation_ = {};
        return true;
    }
    return false;
}

} // namespace dojo::ui
