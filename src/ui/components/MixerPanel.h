#pragma once

#include "engine/EngineController.h"
#include "session/Project.h"
#include "ui/components/InlineValueEditor.h"
#include "ui/components/LevelMeter.h"
#include "ui/EditorState.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

namespace dojo::ui
{

/**
    One track's channel strip: name, input (audio tracks), output routing, record arm and input
    monitoring, sends (one level slider per send, "+" adds one), the device count (opens the
    Devices panel), pan, mute/solo/phase, fader and meter. Every control edits the session
    (undoably) and follows external changes such as undo or the arranger's buttons.
*/
class ChannelStrip final : public juce::Component,
                           private juce::ValueTree::Listener,
                           private EditorState::Listener
{
public:
    ChannelStrip(session::Session& session, EditorState& editor, juce::ValueTree track,
                 std::function<juce::StringArray()> inputNames, std::function<void()> showDevices);
    ~ChannelStrip() override;

    [[nodiscard]] const juce::ValueTree& getTrack() const noexcept { return track_; }
    void pushLevels(float peakLeft, float peakRight, double elapsedSeconds);

    /** Double-clicking the volume or pan value under the fader opens a field to type it
        ("-6", "-inf"; "C", "L20", "R35"). */
    void showVolumeEditor();
    void showPanEditor();
    [[nodiscard]] juce::TextEditor* getValueEditor() const noexcept { return valueEditor_.getEditor(); }
    void mouseDoubleClick(const juce::MouseEvent& e) override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;

private:
    struct SendControl
    {
        juce::ValueTree send;
        juce::Slider level;
    };

    void syncFromModel();
    void rebuildRouting();
    void rebuildSends();
    void showSendMenu();
    void valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier&) override;
    void valueTreeChildAdded(juce::ValueTree& parent, juce::ValueTree& child) override;
    void valueTreeChildRemoved(juce::ValueTree& parent, juce::ValueTree& child, int) override;
    void editorStateChanged() override { repaint(); }

    session::Session& session_;
    EditorState& editor_;
    juce::ValueTree track_;
    std::function<juce::StringArray()> inputNames_;
    std::function<void()> showDevices_;
    juce::ComboBox input_;
    juce::ComboBox output_;
    std::vector<std::uint32_t> outputIds_;
    juce::TextButton arm_;
    juce::TextButton monitor_;
    juce::TextButton devices_;
    juce::TextButton addSend_;
    std::vector<std::unique_ptr<SendControl>> sends_;

    juce::Slider pan_;
    juce::TextButton mute_;
    juce::TextButton solo_;
    juce::TextButton phase_;
    juce::Slider fader_;
    LevelMeter meter_;
    juce::Rectangle<int> nameArea_;
    juce::Rectangle<int> readoutArea_;
    juce::Rectangle<int> volumeArea_, panArea_; ///< the two halves of the readout
    InlineValueEditor valueEditor_{*this};
    bool panDragging_ = false;
    bool faderDragging_ = false;
};

/** The master strip: fader (session master volume, undoable), meter with scale, gain/peak readouts. */
class MasterStrip final : public juce::Component, private juce::ValueTree::Listener
{
public:
    MasterStrip(session::Session& session, EditorState& editor, std::function<void()> showDevices);
    ~MasterStrip() override;

    void pushLevels(float peakLeft, float peakRight, double elapsedSeconds);

    /** Double-clicking the GAIN readout opens a field to type the master volume. */
    void showGainEditor();
    [[nodiscard]] juce::TextEditor* getValueEditor() const noexcept { return valueEditor_.getEditor(); }
    void mouseDoubleClick(const juce::MouseEvent& e) override;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& property) override;
    void valueTreeRedirected(juce::ValueTree&) override;

    session::Session& session_;
    EditorState& editor_;
    std::function<void()> showDevices_;
    juce::TextButton devices_;
    juce::Slider fader_;
    LevelMeter meter_;
    juce::Rectangle<int> scaleArea_;
    juce::Rectangle<int> readoutArea_;
    juce::Rectangle<int> gainArea_;
    InlineValueEditor valueEditor_{*this};
    float heldPeakDb_ = LevelMeter::kMinDb;
    double peakHoldAge_ = 0.0;
    bool dragging_ = false;
};

/** Bottom mixer panel: a scrollable row of channel strips and the master strip. */
class MixerPanel final : public juce::Component, private juce::ValueTree::Listener, private juce::AsyncUpdater
{
public:
    MixerPanel(session::Project& project, engine::EngineController& controller, EditorState& editor);

    /** Shows the Devices panel (a strip's FX button was clicked). */
    std::function<void()> onShowDevices;
    ~MixerPanel() override;

    /** Pulls every track's post-fader peaks from the engine and feeds the meters. Once per UI frame. */
    void updateMeters(const engine::TelemetrySnapshot& telemetry, double elapsedSeconds);

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void rebuildStrips();
    void layoutStrips();
    void valueTreeChildAdded(juce::ValueTree& parent, juce::ValueTree& child) override;
    void valueTreeChildRemoved(juce::ValueTree& parent, juce::ValueTree& child, int) override;
    void valueTreeChildOrderChanged(juce::ValueTree& parent, int, int) override;
    void valueTreeRedirected(juce::ValueTree&) override { rebuildStrips(); }
    void handleAsyncUpdate() override { rebuildStrips(); }

    session::Project& project_;
    engine::EngineController& controller_;
    EditorState& editor_;

    juce::Viewport viewport_;
    juce::Component stripHolder_;
    std::vector<std::unique_ptr<ChannelStrip>> strips_;
    MasterStrip master_;
};

} // namespace dojo::ui
