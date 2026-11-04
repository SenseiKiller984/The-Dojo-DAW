#include "ui/components/ArrangerView.h"

#include "core/MusicalTime.h"
#include "ui/PlaceholderSession.h"
#include "ui/theme/DojoTheme.h"

#include <cmath>

namespace dojo::ui
{
namespace
{
constexpr int kBeatsPerBar = 4;
constexpr float kFolderIndent = 14.0f;

/** Deterministic pseudo-random value in [0, 1) — lets placeholder clip contents look organic while
    rendering identically every frame and on every platform. */
float hash01(int a, int b) noexcept
{
    auto h = static_cast<std::uint32_t>(a) * 0x9E3779B1u ^ static_cast<std::uint32_t>(b) * 0x85EBCA77u;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return static_cast<float>(h & 0xFFFFu) / 65536.0f;
}
} // namespace

ArrangerView::ArrangerView(engine::EngineController& controller)
    : controller_(controller)
{
    setOpaque(true);
}

//==================================================================================================
// Geometry

juce::Rectangle<int> ArrangerView::getRulerArea() const
{
    return getLocalBounds()
        .removeFromTop(theme::metrics::rulerHeight)
        .withTrimmedLeft(theme::metrics::trackHeaderWidth);
}

juce::Rectangle<int> ArrangerView::getHeaderArea() const
{
    return getLocalBounds()
        .withTrimmedTop(theme::metrics::rulerHeight)
        .removeFromLeft(theme::metrics::trackHeaderWidth);
}

juce::Rectangle<int> ArrangerView::getLaneArea() const
{
    return getLocalBounds()
        .withTrimmedTop(theme::metrics::rulerHeight)
        .withTrimmedLeft(theme::metrics::trackHeaderWidth);
}

float ArrangerView::beatsToX(double beats) const noexcept
{
    return static_cast<float>(theme::metrics::trackHeaderWidth) + static_cast<float>(beats) * pixelsPerBeat_;
}

double ArrangerView::xToBeats(float x) const noexcept
{
    return juce::jmax(0.0, static_cast<double>((x - static_cast<float>(theme::metrics::trackHeaderWidth)) /
                                               pixelsPerBeat_));
}

juce::Rectangle<int> ArrangerView::getPlayheadRepaintArea(double beats) const
{
    const int x = juce::roundToInt(beatsToX(beats));
    return juce::Rectangle<int>(x - 7, 0, 14, getHeight());
}

//==================================================================================================
// Engine-driven updates and interaction

void ArrangerView::setPlayhead(SampleCount playheadSamples, double sampleRate)
{
    const double seconds = time::samplesToSeconds(playheadSamples, sampleRate);
    const double beats = time::secondsToQuarterNotes(seconds, time::kDefaultTempoBpm);

    if (std::abs(beats - playheadBeats_) * static_cast<double>(pixelsPerBeat_) < 0.25)
        return; // sub-pixel movement: nothing visible changed

    repaint(getPlayheadRepaintArea(playheadBeats_));
    playheadBeats_ = beats;
    repaint(getPlayheadRepaintArea(playheadBeats_));
}

void ArrangerView::mouseDown(const juce::MouseEvent& event)
{
    draggingInRuler_ = getRulerArea().contains(event.getPosition());

    if (draggingInRuler_)
        locateToX(event.position.x);
}

void ArrangerView::mouseDrag(const juce::MouseEvent& event)
{
    if (draggingInRuler_)
        locateToX(event.position.x);
}

void ArrangerView::locateToX(float x)
{
    const double rate = controller_.getSampleRateOr(48000.0);
    const double seconds = time::quarterNotesToSeconds(xToBeats(x), time::kDefaultTempoBpm);
    controller_.locate(time::secondsToSamples(seconds, rate));
}

//==================================================================================================
// Painting

void ArrangerView::paint(juce::Graphics& g)
{
    g.fillAll(theme::colour::panel);

    paintLanes(g);
    paintClips(g);
    paintRuler(g);
    paintTrackHeaders(g);
    paintPlayhead(g);
}

void ArrangerView::paintRuler(juce::Graphics& g) const
{
    using namespace theme;

    // Corner above the track headers.
    auto corner =
        getLocalBounds().removeFromTop(metrics::rulerHeight).removeFromLeft(metrics::trackHeaderWidth);
    g.setColour(colour::panelHeader);
    g.fillRect(corner);
    g.setColour(colour::textSecondary);
    g.setFont(label(11.0f));
    g.drawText("ARRANGEMENT", corner.reduced(14, 0), juce::Justification::centredLeft, false);
    g.setColour(colour::textMuted);
    g.setFont(sans(11.0f));
    g.drawText(juce::String(static_cast<int>(placeholder::getTracks().size())) + " tracks",
               corner.reduced(14, 0), juce::Justification::centredRight, false);

    const auto ruler = getRulerArea();
    g.setColour(colour::panelHeader);
    g.fillRect(ruler);

    const int firstBeat = juce::jmax(0, static_cast<int>(xToBeats(static_cast<float>(ruler.getX()))));
    const int lastBeat = static_cast<int>(xToBeats(static_cast<float>(ruler.getRight()))) + 1;

    for (int beat = firstBeat; beat <= lastBeat; ++beat)
    {
        const float x = beatsToX(beat);
        const bool isBar = beat % kBeatsPerBar == 0;

        g.setColour(isBar ? colour::textMuted : colour::outlineStrong);
        const float tickTop = static_cast<float>(ruler.getBottom()) - (isBar ? 12.0f : 5.0f);
        g.drawVerticalLine(juce::roundToInt(x), tickTop, static_cast<float>(ruler.getBottom()));

        if (isBar)
        {
            g.setColour(colour::textSecondary);
            g.setFont(mono(11.0f));
            g.drawText(
                juce::String(beat / kBeatsPerBar + 1),
                juce::Rectangle<float>(x + 4.0f, static_cast<float>(ruler.getY()) + 3.0f, 40.0f, 14.0f),
                juce::Justification::centredLeft, false);
        }
    }

    g.setColour(colour::outline);
    g.fillRect(getLocalBounds().removeFromTop(metrics::rulerHeight).removeFromBottom(1));
}

void ArrangerView::paintTrackHeaders(juce::Graphics& g) const
{
    using namespace theme;

    auto area = getHeaderArea();
    g.setColour(colour::panelRaised);
    g.fillRect(area);

    for (const auto& track : placeholder::getTracks())
    {
        auto row = area.removeFromTop(metrics::trackHeight);
        if (row.getY() > g.getClipBounds().getBottom())
            break;

        g.setColour(colour::outline);
        g.fillRect(row.removeFromBottom(1));

        // Colour tab.
        g.setColour(track.colour);
        g.fillRect(row.removeFromLeft(4));

        auto content = row.reduced(10, 7).withTrimmedLeft(
            juce::roundToInt(kFolderIndent * static_cast<float>(track.depth)));

        // Folder disclosure for busses that own children.
        if (track.kind == placeholder::TrackKind::groupBus)
        {
            const auto arrowArea = content.removeFromLeft(12).toFloat().withSizeKeepingCentre(8.0f, 8.0f);
            juce::Path arrow;
            arrow.addTriangle(arrowArea.getX(), arrowArea.getY() + 1.0f, arrowArea.getRight(),
                              arrowArea.getY() + 1.0f, arrowArea.getCentreX(), arrowArea.getBottom() - 1.0f);
            g.setColour(colour::textSecondary);
            g.fillPath(arrow);
            content.removeFromLeft(4);
        }

        // Mute / solo (inactive placeholders).
        auto buttons = content.removeFromRight(46);
        for (const auto* text : {"M", "S"})
        {
            const auto box = buttons.removeFromLeft(21).withSizeKeepingCentre(19, 17).toFloat();
            g.setColour(colour::control);
            g.fillRoundedRectangle(box, 3.0f);
            g.setColour(colour::textMuted);
            g.setFont(sansBold(10.0f));
            g.drawText(text, box, juce::Justification::centred, false);
            buttons.removeFromLeft(4);
        }

        auto nameRow = content.removeFromTop(content.getHeight() / 2);
        g.setColour(colour::textPrimary);
        g.setFont(sansBold(13.0f));
        g.drawText(track.name, nameRow, juce::Justification::bottomLeft, true);

        auto detailRow = content.withTrimmedTop(2);
        const auto kind = placeholder::getKindLabel(track.kind);
        const auto badge = detailRow.removeFromLeft(36).withHeight(14).toFloat();
        g.setColour(track.colour.withAlpha(0.18f));
        g.fillRoundedRectangle(badge, 3.0f);
        g.setColour(track.colour.brighter(0.3f));
        g.setFont(label(8.5f));
        g.drawText(kind, badge, juce::Justification::centred, false);

        g.setColour(colour::textMuted);
        g.setFont(sans(11.0f));
        g.drawText(track.detail, detailRow.withTrimmedLeft(6).withHeight(14),
                   juce::Justification::centredLeft, true);
    }

    g.setColour(colour::outline);
    g.fillRect(getHeaderArea().removeFromRight(1));
}

void ArrangerView::paintLanes(juce::Graphics& g) const
{
    using namespace theme;

    const auto lanes = getLaneArea();
    const auto& tracks = placeholder::getTracks();

    // Row backgrounds.
    auto rows = lanes;
    for (std::size_t i = 0; i < tracks.size(); ++i)
    {
        auto row = rows.removeFromTop(metrics::trackHeight);
        g.setColour(i % 2 == 0 ? colour::laneEven : colour::laneOdd);
        g.fillRect(row);
        g.setColour(colour::outline.withAlpha(0.6f));
        g.fillRect(row.removeFromBottom(1));
    }
    g.setColour(colour::windowBackground);
    g.fillRect(rows);

    // Beat / bar grid, limited to the area being repainted.
    const auto clip = g.getClipBounds().getIntersection(lanes);
    const int firstBeat = juce::jmax(0, static_cast<int>(xToBeats(static_cast<float>(clip.getX()))));
    const int lastBeat = static_cast<int>(xToBeats(static_cast<float>(clip.getRight()))) + 1;

    for (int beat = firstBeat; beat <= lastBeat; ++beat)
    {
        g.setColour(beat % kBeatsPerBar == 0 ? colour::gridBar : colour::gridBeat);
        g.drawVerticalLine(juce::roundToInt(beatsToX(beat)), static_cast<float>(lanes.getY()),
                           static_cast<float>(lanes.getBottom()));
    }

    // Empty-state hint below the last track.
    if (rows.getHeight() > 40)
    {
        g.setColour(colour::textMuted.withAlpha(0.6f));
        g.setFont(sans(12.0f));
        g.drawText("LAYOUT PREVIEW  -  clip editing, recording and track management arrive in Phase 3",
                   rows.reduced(16, 0).removeFromTop(40), juce::Justification::centredLeft, true);
    }
}

void ArrangerView::paintClips(juce::Graphics& g) const
{
    using namespace theme;

    const auto lanes = getLaneArea();
    const float pixelsPerBar = pixelsPerBeat_ * static_cast<float>(kBeatsPerBar);
    int trackIndex = 0;

    g.saveState();
    g.reduceClipRegion(lanes);

    for (const auto& track : placeholder::getTracks())
    {
        const auto row = lanes.withTrimmedTop(trackIndex * metrics::trackHeight)
                             .withHeight(metrics::trackHeight)
                             .reduced(0, 4)
                             .toFloat();
        int clipIndex = 0;

        for (const auto& clipInfo : track.clips)
        {
            const float x = beatsToX(clipInfo.startBar * kBeatsPerBar);
            const auto clipBounds = juce::Rectangle<float>(
                x + 1.0f, row.getY(), static_cast<float>(clipInfo.lengthBars) * pixelsPerBar - 2.0f,
                row.getHeight() - 1.0f);

            if (!clipBounds.toNearestInt().intersects(g.getClipBounds()))
            {
                ++clipIndex;
                continue;
            }

            g.setColour(track.colour.withAlpha(0.22f));
            g.fillRoundedRectangle(clipBounds, 3.0f);

            auto titleBar = clipBounds.withHeight(15.0f);
            g.setColour(track.colour.withAlpha(0.85f));
            g.fillRoundedRectangle(titleBar, 3.0f);
            g.fillRect(titleBar.withTrimmedTop(8.0f));

            g.setColour(colour::windowBackground);
            g.setFont(sansBold(10.5f));
            g.drawText(clipInfo.name, titleBar.reduced(5.0f, 0.0f), juce::Justification::centredLeft, true);

            const auto body = clipBounds.withTrimmedTop(17.0f).reduced(3.0f, 2.0f);
            g.setColour(track.colour.brighter(0.2f).withAlpha(0.75f));

            if (track.kind == placeholder::TrackKind::instrument)
            {
                // MIDI-style note blocks.
                const int steps = juce::roundToInt(clipInfo.lengthBars * 8.0);
                const float stepWidth = body.getWidth() / static_cast<float>(juce::jmax(1, steps));
                for (int step = 0; step < steps; ++step)
                {
                    if (hash01(trackIndex * 131 + clipIndex, step) < 0.35f)
                        continue;
                    const float pitch = hash01(step, trackIndex + clipIndex * 7);
                    const float y = body.getY() + (body.getHeight() - 3.0f) * pitch;
                    g.fillRect(body.getX() + static_cast<float>(step) * stepWidth, y, stepWidth * 0.8f, 3.0f);
                }
            }
            else
            {
                // Audio-style waveform.
                juce::Path wave;
                const float midY = body.getCentreY();
                const int columns = juce::jmax(2, static_cast<int>(body.getWidth() / 2.0f));
                for (int column = 0; column < columns; ++column)
                {
                    const float phase = static_cast<float>(column % 24) / 24.0f;
                    const float envelope = std::exp(-4.0f * phase);
                    const float amplitude =
                        (0.15f + 0.85f * envelope) * (0.55f + 0.45f * hash01(column, trackIndex));
                    const float halfHeight = body.getHeight() * 0.5f * amplitude;
                    const float x0 = body.getX() + static_cast<float>(column) * 2.0f;
                    wave.addRectangle(x0, midY - halfHeight, 1.2f, halfHeight * 2.0f);
                }
                g.fillPath(wave);
            }

            ++clipIndex;
        }

        ++trackIndex;
    }

    g.restoreState();
}

void ArrangerView::paintPlayhead(juce::Graphics& g) const
{
    const float x = beatsToX(playheadBeats_);
    if (x < static_cast<float>(theme::metrics::trackHeaderWidth))
        return;

    g.setColour(theme::colour::playhead);
    g.fillRect(x - 0.5f, static_cast<float>(theme::metrics::rulerHeight), 1.5f,
               static_cast<float>(getHeight() - theme::metrics::rulerHeight));

    juce::Path marker;
    const auto bottom = static_cast<float>(theme::metrics::rulerHeight);
    marker.addTriangle(x - 6.0f, bottom - 10.0f, x + 6.0f, bottom - 10.0f, x, bottom);
    g.fillPath(marker);
}

} // namespace dojo::ui
