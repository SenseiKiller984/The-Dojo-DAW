#include "ui/devices/TempoDetectPanel.h"

#include "ui/theme/DojoTheme.h"

#include <array>
#include <cmath>

namespace dojo::ui
{
namespace
{
constexpr std::array<std::pair<double, double>, 5> kRanges{{{70.0, 180.0}, {60.0, 120.0}, {80.0, 160.0}, {90.0, 180.0}, {100.0, 200.0}}};
constexpr int kTitleHeight = 24;
constexpr int kRowHeight = 18;

juce::String formatBpm(double bpm) { return juce::String(session::tempo::roundBpm(bpm), 2); }
} // namespace

/** The analysis, on its own thread; the result goes back to the message thread. */
class TempoDetectPanel::Job final : public juce::Thread
{
public:
    Job(TempoDetectPanel& owner, std::vector<session::tempo::TrackMaterial> tracks, std::pair<double, double> range)
        : juce::Thread("Tempo detection")
        , safe_(&owner)
        , alive_(owner.alive_)
        , tracks_(std::move(tracks))
        , range_(range)
    {
    }

    ~Job() override
    {
        cancel_.store(true);
        stopThread(10'000);
    }

    void run() override
    {
        auto analysed = session::tempo::analyse(tracks_, range_.first, range_.second, &cancel_);
        if (cancel_.load())
            return;
        auto safe = safe_;
        auto alive = alive_;
        juce::MessageManager::callAsync([safe, alive, result = std::move(analysed)]() mutable
                                        {
                                            if (alive->load() && safe != nullptr)
                                                safe->finished(std::move(result));
                                        });
    }

private:
    juce::Component::SafePointer<TempoDetectPanel> safe_;
    std::shared_ptr<std::atomic<bool>> alive_;
    std::vector<session::tempo::TrackMaterial> tracks_;
    std::pair<double, double> range_;
    std::atomic<bool> cancel_{false};
};

TempoDetectPanel::TempoDetectPanel(session::Session& session, EditorState& editor)
    : session_(session)
    , editor_(editor)
{
    detectButton.setButtonText("Detect Tempo");
    detectButton.setTooltip("Measures the ticked tracks (the audio tracks selected in the arranger are ticked)");
    detectButton.onClick = [this] { detect(); };
    halfButton.setTooltip("Half time: half the tempo found");
    doubleButton.setTooltip("Double time: twice the tempo found");
    halfButton.onClick = [this] { setOctave(octave_ * 0.5); };
    doubleButton.onClick = [this] { setOctave(octave_ * 2.0); };
    applyButton.onClick = [this] { applyTempo(); };
    for (auto* b : {&detectButton, &halfButton, &doubleButton, &applyButton})
    {
        b->setWantsKeyboardFocus(false);
        addAndMakeVisible(b);
    }

    for (std::size_t i = 0; i < kRanges.size(); ++i)
        rangeBox.addItem(juce::String(juce::roundToInt(kRanges[i].first)) + " - " + juce::String(juce::roundToInt(kRanges[i].second)) + " BPM",
                         static_cast<int>(i) + 1);
    rangeBox.setSelectedId(1, juce::dontSendNotification);
    rangeBox.setTooltip("The range of tempos to look in: it decides between half and double time");
    addAndMakeVisible(rangeBox);

    list_.setModel(this);
    list_.setRowHeight(20);
    list_.setColour(juce::ListBox::backgroundColourId, theme::colour::panel);
    list_.setWantsKeyboardFocus(false);
    addAndMakeVisible(list_);

    onDifferentTempo = [this](double detected, double project) { askToChange(detected, project); };
    editor_.addListener(this);
    session_.getState().addListener(this);
    tickSelection();
    update();
}

TempoDetectPanel::~TempoDetectPanel()
{
    session_.getState().removeListener(this);
    editor_.removeListener(this);
    alive_->store(false);
    job_.reset();
}

std::vector<juce::ValueTree> TempoDetectPanel::getAudioTracks() const
{
    std::vector<juce::ValueTree> tracks;
    for (const auto& t : session_.getAllTracks())
        if (session::holdsAudio(session::track::getKind(t)))
            tracks.push_back(t);
    return tracks;
}

void TempoDetectPanel::setTicked(std::uint32_t trackId, bool ticked)
{
    if (ticked)
        ticked_.insert(trackId);
    else
        ticked_.erase(trackId);
    list_.repaint();
    update();
}

void TempoDetectPanel::tickSelection()
{
    // When the arranger's selection changes and holds audio tracks, they become the ticked ones.
    std::set<std::uint32_t> selected;
    for (const auto& t : getAudioTracks())
        if (editor_.isTrackSelected(session::track::getId(t)))
            selected.insert(session::track::getId(t));
    if (selected == lastSelection_)
        return;
    lastSelection_ = selected;
    if (!selected.empty())
    {
        ticked_ = selected;
        list_.repaint();
        update();
    }
}

void TempoDetectPanel::valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& property)
{
    if (property == session::ids::tempo || (tree.hasType(session::ids::track) && property == session::ids::name))
    {
        list_.repaint();
        update();
    }
}

void TempoDetectPanel::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool)
{
    const auto tracks = getAudioTracks();
    if (row < 0 || row >= static_cast<int>(tracks.size()))
        return;
    const auto& t = tracks[static_cast<std::size_t>(row)];
    const bool on = isTicked(session::track::getId(t));
    const auto box = juce::Rectangle<float>(4.0f, (static_cast<float>(height) - 12.0f) / 2.0f, 12.0f, 12.0f);
    g.setColour(on ? theme::colour::accent : theme::colour::outlineStrong);
    if (on)
        g.fillRoundedRectangle(box, 2.0f);
    else
        g.drawRoundedRectangle(box, 2.0f, 1.2f);
    g.setColour(session::track::getColour(t));
    g.fillRect(juce::Rectangle<float>(22.0f, 4.0f, 3.0f, static_cast<float>(height) - 8.0f));
    g.setColour(on ? theme::colour::textPrimary : theme::colour::textSecondary);
    g.setFont(theme::sans(12.0f));
    g.drawText(session::track::getName(t), 30, 0, width - 32, height, juce::Justification::centredLeft, true);
}

void TempoDetectPanel::listBoxItemClicked(int row, const juce::MouseEvent&)
{
    const auto tracks = getAudioTracks();
    if (row < 0 || row >= static_cast<int>(tracks.size()))
        return;
    const auto id = session::track::getId(tracks[static_cast<std::size_t>(row)]);
    setTicked(id, !isTicked(id));
}

void TempoDetectPanel::setRange(int index)
{
    rangeBox.setSelectedId(juce::jlimit(1, static_cast<int>(kRanges.size()), index + 1), juce::dontSendNotification);
}

std::pair<double, double> TempoDetectPanel::getRange() const
{
    return kRanges[static_cast<std::size_t>(juce::jlimit(0, static_cast<int>(kRanges.size()) - 1, rangeBox.getSelectedId() - 1))];
}

std::vector<juce::ValueTree> TempoDetectPanel::tickedTracks() const
{
    // In arranger order.
    std::vector<juce::ValueTree> tracks;
    for (const auto& t : getAudioTracks())
        if (isTicked(session::track::getId(t)))
            tracks.push_back(t);
    return tracks;
}

void TempoDetectPanel::detect()
{
    if (job_ != nullptr)
        return;
    const auto tracks = tickedTracks();
    if (tracks.empty())
    {
        message_ = "Tick the tracks to measure (or select them in the arranger).";
        update();
        return;
    }
    std::vector<session::tempo::TrackMaterial> materials;
    for (const auto& t : tracks)
        materials.push_back(session::tempo::takeMaterial(session_, t));

    message_ = "Analysing " + juce::String(static_cast<int>(materials.size())) + (materials.size() == 1 ? " track..." : " tracks...");
    hasResult_ = false;
    octave_ = 1.0;
    job_ = std::make_unique<Job>(*this, std::move(materials), getRange());
    job_->startThread();
    update();
}

bool TempoDetectPanel::waitUntilDone(int timeoutMs)
{
    const auto end = juce::Time::getMillisecondCounter() + static_cast<juce::uint32>(timeoutMs);
    while (job_ != nullptr)
    {
        if (juce::Time::getMillisecondCounter() > end)
            return false;
        juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
    }
    return true;
}

void TempoDetectPanel::finished(session::tempo::Combined result)
{
    job_.reset();
    result_ = std::move(result);
    hasResult_ = true;
    octave_ = 1.0;
    message_ = result_.valid ? juce::String() : juce::String("No tempo found in the selected tracks.");
    update();

    if (result_.valid)
    {
        const double detected = getDetectedTempo();
        const double project = session_.getTempo();
        if (std::abs(detected - project) >= 0.005 && onDifferentTempo)
            onDifferentTempo(detected, project);
    }
}

double TempoDetectPanel::getDetectedTempo() const
{
    if (!hasResult_ || !result_.valid)
        return 0.0;
    return session::tempo::roundBpm(result_.average * octave_);
}

void TempoDetectPanel::setOctave(double factor)
{
    if (!hasResult_ || !result_.valid)
        return;
    const double bpm = result_.average * factor;
    if (bpm < session::kMinTempo || bpm > session::kMaxTempo)
        return;
    octave_ = factor;
    update();
}

void TempoDetectPanel::applyTempo()
{
    const double bpm = getDetectedTempo();
    if (bpm <= 0.0)
        return;
    session_.beginTransaction("Set Tempo to " + formatBpm(bpm) + " BPM");
    session_.setTempo(bpm);
    update();
}

void TempoDetectPanel::askToChange(double detected, double project)
{
    juce::Component::SafePointer<TempoDetectPanel> safe(this);
    juce::String text;
    text << "The selected tracks play at " << formatBpm(detected) << " BPM";
    if (result_.counted > 1)
        text << " (the average of " << result_.counted << " tracks)";
    text << ". The project is at " << formatBpm(project) << " BPM.\n\nSet the project tempo to " << formatBpm(detected) << " BPM?";
    juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                                     .withIconType(juce::MessageBoxIconType::QuestionIcon)
                                     .withTitle("Change the Project Tempo?")
                                     .withMessage(text)
                                     .withButton("Set " + formatBpm(detected) + " BPM")
                                     .withButton("Keep " + formatBpm(project) + " BPM")
                                     .withAssociatedComponent(this),
                                 [safe](int button)
                                 {
                                     if (button == 1 && safe != nullptr)
                                         safe->applyTempo();
                                 });
}

void TempoDetectPanel::update()
{
    const bool busy = job_ != nullptr;
    detectButton.setEnabled(!busy && !ticked_.empty());
    const bool valid = hasResult_ && result_.valid;
    halfButton.setEnabled(valid && !busy);
    doubleButton.setEnabled(valid && !busy);
    applyButton.setEnabled(valid && !busy && std::abs(getDetectedTempo() - session_.getTempo()) >= 0.005);
    repaint();
}

juce::Rectangle<int> TempoDetectPanel::getResultArea() const
{
    auto area = getLocalBounds().reduced(8, 6);
    area.removeFromTop(kTitleHeight);
    area.removeFromLeft(kListWidth + 10);
    return area;
}

void TempoDetectPanel::resized()
{
    auto left = getLocalBounds().reduced(8, 6);
    left.removeFromTop(kTitleHeight);
    left = left.removeFromLeft(kListWidth);
    rangeBox.setBounds(left.removeFromBottom(24));
    left.removeFromBottom(4);
    list_.setBounds(left);

    auto area = getResultArea();
    detectButton.setBounds(area.removeFromTop(26));
    auto bottom = area.removeFromBottom(26);
    halfButton.setBounds(bottom.removeFromLeft(40));
    bottom.removeFromLeft(4);
    doubleButton.setBounds(bottom.removeFromLeft(40));
    bottom.removeFromLeft(8);
    applyButton.setBounds(bottom);
}

void TempoDetectPanel::paint(juce::Graphics& g)
{
    using namespace theme;
    g.fillAll(colour::panelRaised);
    g.setColour(colour::textSecondary);
    g.setFont(label(10.0f));
    const auto title = getLocalBounds().reduced(8, 6).removeFromTop(kTitleHeight);
    g.drawText("TEMPO DETECTION", title, juce::Justification::centredLeft, false);
    g.setColour(colour::textMuted);
    g.drawText("TRACKS", title.withTrimmedLeft(kListWidth - 50).withWidth(50), juce::Justification::centredRight, false);
    auto area = getResultArea();
    area.removeFromTop(26 + 6);
    area.removeFromBottom(26 + 6);

    // Project tempo at the bottom.
    g.setColour(colour::textMuted);
    g.setFont(sans(11.0f));
    g.drawText("Project: " + formatBpm(session_.getTempo()) + " BPM", area.removeFromBottom(16), juce::Justification::centredLeft, false);

    if (!hasResult_ || !result_.valid || message_.isNotEmpty())
    {
        g.setColour(colour::textSecondary);
        g.setFont(sans(12.0f));
        g.drawFittedText(message_.isNotEmpty() ? message_
                                               : juce::String("Tick a track (or several: kick, snare, overheads...), or select them in "
                                                              "the arranger: each is measured to two decimal places, then averaged."),
                         area.removeFromTop(64), juce::Justification::topLeft, 4);
    }
    if (!hasResult_)
        return;

    // One row per track.
    for (const auto& t : result_.tracks)
    {
        if (area.getHeight() < kRowHeight * 3)
            break;
        auto row = area.removeFromTop(kRowHeight);
        g.setFont(sans(12.0f));
        g.setColour(colour::textPrimary);
        g.drawText(t.name, row.removeFromLeft(row.getWidth() / 2), juce::Justification::centredLeft, true);
        if (t.estimate.valid)
        {
            juce::String value = formatBpm(t.estimate.bpm);
            if (std::abs(t.folded - t.estimate.bpm) > 1.0e-6)
                value << " -> " << formatBpm(t.folded);
            g.setFont(mono(12.0f));
            g.drawText(value, row, juce::Justification::centredRight, false);
        }
        else
        {
            g.setColour(colour::textMuted);
            g.setFont(sans(11.0f));
            g.drawText(t.estimate.problem, row, juce::Justification::centredRight, true);
        }
    }

    if (result_.valid && area.getHeight() >= 34)
    {
        area.removeFromTop(4);
        auto summary = area.removeFromTop(34);
        g.setColour(colour::outline);
        g.drawHorizontalLine(summary.getY(), static_cast<float>(summary.getX()), static_cast<float>(summary.getRight()));
        g.setColour(colour::textSecondary);
        g.setFont(label(9.5f));
        g.drawText(result_.counted > 1 ? "AVERAGE OF " + juce::String(result_.counted) : juce::String("TEMPO"),
                   summary.removeFromLeft(110), juce::Justification::centredLeft, false);
        g.setColour(colour::accentBright);
        g.setFont(mono(22.0f));
        juce::String text = formatBpm(getDetectedTempo()) + " BPM";
        if (octave_ > 1.0)
            text = "x2  " + text;
        else if (octave_ < 1.0)
            text = "/2  " + text;
        g.drawText(text, summary, juce::Justification::centredRight, false);
    }
}

} // namespace dojo::ui
