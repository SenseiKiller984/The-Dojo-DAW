#pragma once

#include "session/Session.h"
#include "session/TempoDetector.h"
#include "ui/EditorState.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <functional>
#include <memory>
#include <set>
#include <vector>

namespace dojo::ui
{

/**
    The Click device's tempo detection: the tempo of the ticked tracks (the audio tracks selected in
    the arranger are ticked when the selection changes; tick others in the list), each to two
    decimal places, and their average. When the average differs from the project tempo it asks
    whether to set the project to it. The range decides between half and double time; x2 and /2
    correct it afterwards.
*/
class TempoDetectPanel final : public juce::Component,
                               private juce::ListBoxModel,
                               private EditorState::Listener,
                               private juce::ValueTree::Listener
{
public:
    static constexpr int kPreferredWidth = 470;
    static constexpr int kListWidth = 170;

    TempoDetectPanel(session::Session& session, EditorState& editor);
    ~TempoDetectPanel() override;

    /** The audio tracks that can be analysed (listed), and which are ticked. */
    [[nodiscard]] std::vector<juce::ValueTree> getAudioTracks() const;
    [[nodiscard]] bool isTicked(std::uint32_t trackId) const { return ticked_.count(trackId) > 0; }
    void setTicked(std::uint32_t trackId, bool ticked);

    /** Analyses the ticked tracks (in the background). */
    void detect();
    [[nodiscard]] bool isAnalysing() const noexcept { return job_ != nullptr; }
    /** Runs the message loop until the analysis is done (tests). */
    bool waitUntilDone(int timeoutMs);

    [[nodiscard]] const session::tempo::Combined& getResult() const noexcept { return result_; }
    /** The detected tempo (the average, times the x2 / /2 correction), to two decimals; 0 if none. */
    [[nodiscard]] double getDetectedTempo() const;
    /** x2 or /2 (or back to 1). */
    void setOctave(double factor);
    /** Sets the project tempo to the detected one (one undo step). */
    void applyTempo();

    /** Asked when the detected tempo differs from the project's: by default a dialog that sets it
        on "Set Tempo". */
    std::function<void(double detected, double project)> onDifferentTempo;
    /** "Set Click Tempo": the detected tempo for the click's own tempo (set by the Click's editor;
        the button shows only then). */
    std::function<void(double bpm)> onSetClickTempo;
    /** What "Set Click Tempo" does (also for tests). */
    void applyToClick();

    /** Range choices (min, max BPM) and the one in use. */
    void setRange(int index);
    [[nodiscard]] std::pair<double, double> getRange() const;

    juce::TextButton detectButton, halfButton{"/2"}, doubleButton{"x2"}, applyButton{"Set Project Tempo"},
        clickButton{"Set Click Tempo"};
    juce::ComboBox rangeBox;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    class Job;
    void finished(session::tempo::Combined result);
    void askToChange(double detected, double project);
    void update();
    [[nodiscard]] juce::Rectangle<int> getResultArea() const;
    [[nodiscard]] std::vector<juce::ValueTree> tickedTracks() const;
    void tickSelection();

    int getNumRows() override { return static_cast<int>(getAudioTracks().size()); }
    void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override;
    void listBoxItemClicked(int row, const juce::MouseEvent&) override;
    void editorStateChanged() override { tickSelection(); }
    void valueTreeChildAdded(juce::ValueTree&, juce::ValueTree&) override { list_.updateContent(); }
    void valueTreeChildRemoved(juce::ValueTree&, juce::ValueTree&, int) override { list_.updateContent(); }
    void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier& property) override;

    session::Session& session_;
    EditorState& editor_;
    std::set<std::uint32_t> ticked_;
    std::set<std::uint32_t> lastSelection_;
    juce::ListBox list_;
    std::unique_ptr<Job> job_;
    session::tempo::Combined result_;
    bool hasResult_ = false;
    double octave_ = 1.0;
    juce::String message_;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
};

} // namespace dojo::ui
