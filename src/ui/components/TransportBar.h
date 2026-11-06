#pragma once

#include "core/Types.h"
#include "engine/EngineController.h"
#include "session/Session.h"
#include "ui/components/IconButton.h"
#include "ui/EditorState.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace dojo::ui
{

/**
    Top bar: brand, transport, position (bars|beats|ticks and clock), tempo, meter, grid snapping,
    the test tone, audio settings and the DSP load meter.

    The tempo, speed and transpose boxes are editable: drag up/down (Shift for fine steps) or
    double-click to type a value. Record arms recording onto the armed tracks (see Recorder); loop
    toggles the loop range set in the arranger's ruler.
*/
class TransportBar final : public juce::Component, private EditorState::Listener
{
public:
    TransportBar(engine::EngineController& controller, session::Session& session, EditorState& editor);
    ~TransportBar() override;

    /** Called once per UI frame with fresh engine telemetry. Repaints only what changed. */
    void update(const engine::TelemetrySnapshot& telemetry, double sampleRate);

    /** Opens the audio device settings (provided by MainComponent). */
    std::function<void()> onAudioSettingsRequested;
    /** The record button was clicked (MainComponent starts / stops the Recorder). */
    /** Return to start (the return button, or Stop while stopped). Defaults to the controller's.
        The main component adds "scroll the view back" when that preference is on. */
    std::function<void()> onReturnToStart;
    /** Play / stop (the play button). Defaults to the controller's; the main component adds "after a
        stop at the end, the view returns to the start" when that preference is on. */
    std::function<void()> onTogglePlayback;
    std::function<void()> onRecordClicked;

    /** Lights the record button while recording. */
    void setRecording(bool recording);

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    class ValueField;

    void editorStateChanged() override;
    void paintBrand(juce::Graphics& g) const;
    void paintPositionDisplay(juce::Graphics& g) const;
    void paintSignature(juce::Graphics& g) const;
    void paintLoadMeter(juce::Graphics& g) const;

    engine::EngineController& controller_;
    session::Session& session_;
    EditorState& editor_;

    IconButton returnButton_;
    IconButton playButton_;
    IconButton stopButton_;
    IconButton recordButton_;
    IconButton loopButton_;
    std::unique_ptr<ValueField> tempoField_;
    std::unique_ptr<ValueField> speedField_;
    std::unique_ptr<ValueField> transposeField_;
    juce::TextButton keepPitchButton_;
    juce::TextButton snapButton_;
    juce::ComboBox gridBox_;
    juce::TextButton testToneButton_;
    juce::TextButton audioSettingsButton_;

    juce::Rectangle<int> brandArea_;
    juce::Rectangle<int> positionArea_;
    juce::Rectangle<int> signatureArea_;
    juce::Rectangle<int> loadArea_;
    bool compactBrand_ = false;

    SampleCount playheadSamples_ = -1;
    double sampleRate_ = 0.0;
    double tempoShown_ = 0.0;
    bool playing_ = false;
    bool recording_ = false;
    bool loopShown_ = false;
    float dspLoad_ = 0.0f;
};

} // namespace dojo::ui
