#pragma once

#include "core/Types.h"
#include "engine/EngineController.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace dojo::ui
{

/**
    The arranger: bar/beat ruler, track header column and the clip lanes.

    Phase 1 renders the placeholder session as a layout preview. Live behaviour already wired to the
    engine: the playhead follows the transport, and clicking or dragging in the ruler relocates it.
    Clip editing, zoom/scroll, snapping and the real track model arrive in Phase 3.
*/
class ArrangerView final : public juce::Component
{
public:
    explicit ArrangerView(engine::EngineController& controller);

    /** Called once per UI frame. Repaints only the strips the playhead left and entered. */
    void setPlayhead(SampleCount playheadSamples, double sampleRate);

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;

private:
    [[nodiscard]] juce::Rectangle<int> getRulerArea() const;
    [[nodiscard]] juce::Rectangle<int> getHeaderArea() const;
    [[nodiscard]] juce::Rectangle<int> getLaneArea() const;
    [[nodiscard]] float beatsToX(double beats) const noexcept;
    [[nodiscard]] double xToBeats(float x) const noexcept;
    [[nodiscard]] juce::Rectangle<int> getPlayheadRepaintArea(double beats) const;

    void paintRuler(juce::Graphics& g) const;
    void paintTrackHeaders(juce::Graphics& g) const;
    void paintLanes(juce::Graphics& g) const;
    void paintClips(juce::Graphics& g) const;
    void paintPlayhead(juce::Graphics& g) const;
    void locateToX(float x);

    engine::EngineController& controller_;
    double playheadBeats_ = 0.0;
    float pixelsPerBeat_ = 24.0f;
    bool draggingInRuler_ = false;
};

} // namespace dojo::ui
