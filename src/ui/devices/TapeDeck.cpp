#include "ui/devices/TapeDeck.h"

#include "devices/TapeA800.h"
#include "session/Session.h"
#include "ui/devices/HardwarePainting.h"
#include "ui/theme/DojoTheme.h"

#include <algorithm>
#include <cmath>

namespace dojo::ui
{
namespace tape
{
Reels reelsAt(double seconds, double lengthSeconds, double ips) noexcept
{
    const double length = std::max(lengthSeconds, 1.0e-3);
    const double hub2 = kHubRadius * kHubRadius;
    const double area = kFullRadius * kFullRadius - hub2; // (over pi) the tape's area
    Reels reels;
    reels.wound = std::clamp(seconds / length, 0.0, 1.0);
    reels.leftRadius = std::sqrt(hub2 + (1.0 - reels.wound) * area);
    reels.rightRadius = std::sqrt(hub2 + reels.wound * area);
    // Each reel turns by the tape's length over its radius: d(angle) = d(tape) / r, integrated.
    const double tapeInches = std::max(ips, 0.1) * length;
    reels.leftAngle = 2.0 * tapeInches * (kFullRadius - reels.leftRadius) / area;
    reels.rightAngle = 2.0 * tapeInches * (reels.rightRadius - kHubRadius) / area;
    return reels;
}

double standardReelSeconds(double ips) noexcept
{
    return kStandardReelFeet * 12.0 / std::max(ips, 0.1);
}

double lengthFor(const session::Session& session, const juce::ValueTree& owner, double ips)
{
    if (const double end = session.getContentEnd(owner); end > 0.0)
        return end;
    if (const double end = session.getEndTime(); end > 0.0)
        return end;
    return standardReelSeconds(ips);
}
} // namespace tape

namespace
{
constexpr float kTwoPi = juce::MathConstants<float>::twoPi;

const juce::Colour kPlate{0xffc5c4bd};
const juce::Colour kPlateEdge{0xff74736d};
const juce::Colour kInk{0xff2b2b29};
const juce::Colour kTape{0xff3e2a1c};
const juce::Colour kAmber{0xfff0b43c};
const juce::Colour kGreen{0xff4ccf72};
const juce::Colour kRed{0xffec3b2e};
const juce::Colour kWhite{0xffece7d8};

/** The tangent the tape runs along from a reel's pack to its guide roller: it leaves the pack on
    the side facing the heads and wraps the roller the other way round. */
std::pair<juce::Point<float>, juce::Point<float>> tapeTangent(juce::Point<float> pack, float packRadius, juce::Point<float> guide, float guideRadius,
                                                              bool packRightSide)
{
    const auto d = guide - pack;
    const float distance = d.getDistanceFromOrigin();
    if (distance <= packRadius + guideRadius + 0.5f)
        return {pack + juce::Point<float>(packRightSide ? packRadius : -packRadius, 0.0f), guide};
    const float base = std::atan2(d.y, d.x);
    const float spread = std::acos((packRadius + guideRadius) / distance);
    const juce::Point<float> a(std::cos(base - spread), std::sin(base - spread)), b(std::cos(base + spread), std::sin(base + spread));
    const auto n = packRightSide ? (a.x > b.x ? a : b) : (a.x < b.x ? a : b);
    return {pack + n * packRadius, guide - n * guideRadius};
}

/** The angle of `point` around `centre`, as juce::Path arcs count it (0 at twelve o'clock, clockwise). */
float arcAngle(juce::Point<float> centre, juce::Point<float> point)
{
    return std::atan2(point.x - centre.x, centre.y - point.y);
}

enum class Symbol
{
    rewind,
    forward,
    stop,
    play,
    record,
    text
};

void paintTransportLamp(juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour, bool on, Symbol symbol, const juce::String& text = {})
{
    hardware::paintLamp(g, area, colour, on, symbol == Symbol::text ? text : juce::String(), 1.5f, std::min(7.0f, area.getHeight() * 0.62f));
    if (symbol == Symbol::text)
        return;
    const float s = std::min(area.getWidth(), area.getHeight()) * 0.42f;
    const auto c = area.getCentre();
    juce::Path p;
    const auto triangle = [&p, s](juce::Point<float> at, bool right)
    {
        const float k = right ? 1.0f : -1.0f;
        p.addTriangle(at.x - k * s * 0.5f, at.y - s * 0.5f, at.x - k * s * 0.5f, at.y + s * 0.5f, at.x + k * s * 0.45f, at.y);
    };
    switch (symbol)
    {
    case Symbol::rewind:
        triangle(c.translated(-s * 0.42f, 0.0f), false);
        triangle(c.translated(s * 0.42f, 0.0f), false);
        break;
    case Symbol::forward:
        triangle(c.translated(-s * 0.42f, 0.0f), true);
        triangle(c.translated(s * 0.42f, 0.0f), true);
        break;
    case Symbol::stop:
        p.addRectangle(juce::Rectangle<float>(s * 0.8f, s * 0.8f).withCentre(c));
        break;
    case Symbol::play:
        triangle(c, true);
        break;
    case Symbol::record:
        p.addEllipse(juce::Rectangle<float>(s * 0.85f, s * 0.85f).withCentre(c));
        break;
    case Symbol::text:
        break;
    }
    g.setColour(on ? theme::textOn(colour.brighter(0.3f)) : colour.interpolatedWith(juce::Colours::white, 0.25f).withAlpha(0.75f));
    g.fillPath(p);
}
} // namespace

//==================================================================================================

TapeDeck::TapeDeck(devices::TapeA800& tape, std::function<double()> length)
    : tape_(tape)
    , length_fn_(std::move(length))
{
    setInterceptsMouseClicks(false, false);
    update(0.0);
    startTimerHz(30);
}

double TapeDeck::ips() const noexcept
{
    return devices::TapeA800::inchesPerSecond(tape_.speed_->getIndex());
}

bool TapeDeck::isAtEnd() const noexcept
{
    return shown_ >= length_ - 1.0e-6;
}

juce::String TapeDeck::timerText(double seconds)
{
    const auto tenths = static_cast<long long>(std::floor(std::max(0.0, seconds) * 10.0 + 1.0e-6));
    return juce::String::formatted("%02lld:%02lld.%lld", tenths / 600, (tenths / 10) % 60, tenths % 10);
}

void TapeDeck::update(double elapsedSeconds)
{
    bool lengthChanged = false;
    lengthCountdown_ -= elapsedSeconds;
    if (lengthCountdown_ <= 0.0)
    {
        lengthCountdown_ = 0.25;
        const double length = std::max(0.1, length_fn_ ? length_fn_() : tape::standardReelSeconds(ips()));
        lengthChanged = std::abs(length - length_) > 1.0e-9;
        length_ = length;
    }

    // Follow the transport; when it jumps (a locate), wind there.
    const double target = tape_.getTapeSeconds();
    if (!started_)
    {
        shown_ = target;
        started_ = true;
    }
    const double gap = target - shown_;
    if (std::abs(gap) <= std::max(0.25, 4.0 * elapsedSeconds))
    {
        shown_ = target;
        motion_ = !tape_.isRolling() ? Motion::stopped : tape_.isRecording() ? Motion::recording : Motion::playing;
    }
    else
    {
        // Fast at first, slowing into the locate point, and never slower than a whole tape in 3 s.
        const double eased = std::abs(gap) * (1.0 - std::exp(-elapsedSeconds / 0.16));
        const double least = std::min(std::abs(gap), length_ / 3.0 * elapsedSeconds);
        shown_ += std::copysign(std::max(eased, least), gap);
        motion_ = gap > 0.0 ? Motion::fastForward : Motion::rewinding;
    }

    const auto before = reels_;
    reels_ = tape::reelsAt(shown_, length_, ips());
    // A flange turning faster than the eye can follow blurs into a disc.
    for (int reel = 0; reel < 2; ++reel)
    {
        float goal = 0.0f;
        if (elapsedSeconds > 0.0 && !lengthChanged)
        {
            const double turned = reel == 0 ? reels_.leftAngle - before.leftAngle : reels_.rightAngle - before.rightAngle;
            goal = static_cast<float>(std::clamp((std::abs(turned) / elapsedSeconds - 18.0) / 40.0, 0.0, 1.0));
        }
        auto& blur = blur_[static_cast<std::size_t>(reel)];
        blur += (goal - blur) * 0.5f;
        if (blur < 0.01f)
            blur = 0.0f;
    }
    for (int ch = 0; ch < 2; ++ch)
        vu_[static_cast<std::size_t>(ch)] = static_cast<double>(tape_.getVuDb(ch));
    repaint();
}

//==================================================================================================

void TapeDeck::paint(juce::Graphics& g)
{
    auto area = getLocalBounds().toFloat();
    if (area.getWidth() < 60.0f || area.getHeight() < 40.0f)
        return;
    const float bridgeWidth = std::clamp(area.getWidth() * 0.29f, 90.0f, 132.0f);
    const auto bridge = area.removeFromRight(bridgeWidth);
    area.removeFromRight(3.0f);
    paintDeck(g, area);
    paintMeterBridge(g, bridge);
}

void TapeDeck::paintDeck(juce::Graphics& g, juce::Rectangle<float> area) const
{
    // The deck plate.
    hardware::paintMetal(g, area, kPlate, 5.0f, true, 800);
    g.setColour(kPlateEdge);
    g.drawRoundedRectangle(area.reduced(0.5f), 5.0f, 1.0f);
    for (const auto& corner : {area.getTopLeft().translated(6.0f, 6.0f), area.getTopRight().translated(-6.0f, 6.0f),
                               area.getBottomLeft().translated(6.0f, -6.0f), area.getBottomRight().translated(-6.0f, -6.0f)})
        hardware::paintScrew(g, corner, 2.4f, corner.x * 0.37f);

    auto inner = area.reduced(10.0f, 6.0f);
    const auto strip = inner.removeFromBottom(11.0f);

    // Where everything sits: the reels side by side, the tape's run past the heads under them.
    constexpr float pathBand = 25.0f;
    const float flange = std::max(10.0f, std::min((inner.getHeight() - pathBand) * 0.5f, inner.getWidth() * 0.19f));
    const float scale = flange / static_cast<float>(tape::kFlangeRadius); // pixels per inch
    const float cy = inner.getY() + std::max(0.0f, (inner.getHeight() - pathBand - 2.0f * flange) * 0.5f) + flange;
    const juce::Point<float> left(inner.getX() + flange, cy), right(inner.getRight() - flange, cy);
    const float tapeY = cy + flange + 13.0f;
    const float guideRadius = std::max(2.5f, flange * 0.07f);
    const juce::Point<float> leftGuide(left.x + flange * 0.92f, tapeY - guideRadius), rightGuide(right.x - flange * 0.92f, tapeY - guideRadius);
    const float leftPack = static_cast<float>(reels_.leftRadius) * scale, rightPack = static_cast<float>(reels_.rightRadius) * scale;

    // The head block: erase, record and reproduce heads under their cover, then the capstan.
    const float blockLeft = leftGuide.x + guideRadius + 5.0f, blockRight = rightGuide.x - guideRadius - 5.0f;
    const juce::Rectangle<float> cover(blockLeft, tapeY - 17.0f, std::max(4.0f, blockRight - blockLeft), 15.0f);
    juce::ColourGradient anthracite(juce::Colour(0xff45474b), 0.0f, cover.getY(), juce::Colour(0xff1d1e20), 0.0f, cover.getBottom(), false);
    g.setGradientFill(anthracite);
    g.fillRoundedRectangle(cover, 2.5f);
    g.setColour(juce::Colours::white.withAlpha(0.14f));
    g.drawHorizontalLine(juce::roundToInt(cover.getY() + 1.0f), cover.getX() + 2.0f, cover.getRight() - 2.0f);
    const float capstanX = cover.getRight() - 7.0f;
    const float headsWidth = std::max(6.0f, capstanX - 8.0f - cover.getX() - 4.0f);
    for (int head = 0; head < 3; ++head)
    {
        const float x = cover.getX() + 4.0f + headsWidth * (static_cast<float>(head) + 0.5f) / 3.0f;
        const auto block = juce::Rectangle<float>(head == 0 ? 5.0f : 7.0f, 8.0f).withCentre({x, tapeY - 4.0f});
        juce::ColourGradient metal(juce::Colour(0xffd9dce0), block.getX(), 0.0f, juce::Colour(0xff7c8288), block.getRight(), 0.0f, false);
        g.setGradientFill(metal);
        g.fillRoundedRectangle(block, 1.5f);
        g.setColour(juce::Colour(0xff222222));
        g.drawVerticalLine(juce::roundToInt(x), block.getBottom() - 3.0f, block.getBottom());
    }

    // The tape, threaded: off the left pack, round its guide, past the heads, round the right guide
    // onto the right pack.
    const auto [leftOff, leftOn] = tapeTangent(left, leftPack, leftGuide, guideRadius, true);
    const auto [rightOn, rightOff] = tapeTangent(right, rightPack, rightGuide, guideRadius, false);
    juce::Path path;
    path.startNewSubPath(leftOff);
    path.lineTo(leftOn);
    path.addCentredArc(leftGuide.x, leftGuide.y, guideRadius, guideRadius, 0.0f, arcAngle(leftGuide, leftOn), -juce::MathConstants<float>::pi);
    path.lineTo(rightGuide.x, tapeY);
    path.addCentredArc(rightGuide.x, rightGuide.y, guideRadius, guideRadius, 0.0f, juce::MathConstants<float>::pi, arcAngle(rightGuide, rightOff));
    path.lineTo(rightOn);
    g.setColour(kTape);
    g.strokePath(path, juce::PathStrokeType(1.8f));
    g.setColour(juce::Colours::white.withAlpha(0.12f));
    g.strokePath(path, juce::PathStrokeType(0.6f), juce::AffineTransform::translation(0.0f, -0.7f));

    // The capstan and its pinch roller, the guide rollers.
    g.setColour(juce::Colour(0xff8f959b));
    g.fillEllipse(juce::Rectangle<float>(5.0f, 5.0f).withCentre({capstanX, tapeY - 2.6f}));
    g.setColour(juce::Colour(0xff151515));
    g.fillEllipse(juce::Rectangle<float>(9.0f, 9.0f).withCentre({capstanX, tapeY + 4.6f}));
    g.setColour(juce::Colour(0xffb5babf));
    g.fillEllipse(juce::Rectangle<float>(3.0f, 3.0f).withCentre({capstanX, tapeY + 4.6f}));
    for (const auto& guide : {leftGuide, rightGuide})
    {
        const auto roller = juce::Rectangle<float>(guideRadius * 2.0f, guideRadius * 2.0f).withCentre(guide);
        juce::ColourGradient chrome(juce::Colour(0xfff0f2f4), roller.getX(), roller.getY(), juce::Colour(0xff70757b), roller.getRight(),
                                    roller.getBottom(), false);
        g.setGradientFill(chrome);
        g.fillEllipse(roller);
        g.setColour(juce::Colour(0xff3a3c3f));
        g.fillEllipse(roller.reduced(guideRadius * 0.6f));
    }

    // The reels.
    paintReel(g, left, flange, reels_.leftRadius, reels_.leftAngle, blur_[0]);
    paintReel(g, right, flange, reels_.rightRadius, reels_.rightAngle, blur_[1]);

    // Between the reels: the tape timer, the speed and the transport lamps.
    auto panel = juce::Rectangle<float>(left.x + flange + 3.0f, inner.getY(), 0.0f, 0.0f)
                     .withRight(right.x - flange - 3.0f)
                     .withBottom(cover.getY() - 3.0f);
    if (panel.getWidth() >= 30.0f && panel.getHeight() >= 40.0f)
    {
        auto timer = panel.removeFromTop(16.0f); // (its digits shrink to fit a narrow panel)
        g.setColour(juce::Colour(0xff0c0c0c));
        g.fillRoundedRectangle(timer, 2.0f);
        hardware::paintLedText(g, timer, timerText(shown_), kRed, std::min(10.5f, timer.getWidth() / 4.6f));
        panel.removeFromTop(2.0f);
        hardware::paintEngraved(g, panel.removeFromTop(9.0f), "OF " + timerText(length_).upToLastOccurrenceOf(".", false, false), kInk,
                                theme::label(6.5f));
        panel.removeFromTop(2.0f);
        auto speeds = panel.removeFromTop(10.0f);
        const int speed = tape_.speed_->getIndex();
        const float speedWidth = speeds.getWidth() / 3.0f;
        for (int s = 0; s < 3; ++s)
            paintTransportLamp(g, speeds.removeFromLeft(speedWidth).reduced(0.8f, 0.0f), kAmber, s == speed, Symbol::text,
                               juce::StringArray{"7.5", "15", "30"}[s]);
        panel.removeFromTop(3.0f);
        const float rowHeight = std::min(12.0f, (panel.getHeight() - 4.0f) / 3.0f);
        const auto lamp = [](juce::Rectangle<float> row, int column)
        { return row.withWidth(row.getWidth() * 0.5f).translated(static_cast<float>(column) * row.getWidth() * 0.5f, 0.0f).reduced(0.8f); };
        auto row = panel.removeFromTop(rowHeight);
        paintTransportLamp(g, lamp(row, 0), kAmber, motion_ == Motion::rewinding, Symbol::rewind);
        paintTransportLamp(g, lamp(row, 1), kAmber, motion_ == Motion::fastForward, Symbol::forward);
        panel.removeFromTop(2.0f);
        row = panel.removeFromTop(rowHeight);
        paintTransportLamp(g, lamp(row, 0), kWhite, motion_ == Motion::stopped, Symbol::stop);
        paintTransportLamp(g, lamp(row, 1), kGreen, motion_ == Motion::playing || motion_ == Motion::recording, Symbol::play);
        panel.removeFromTop(2.0f);
        row = panel.removeFromTop(rowHeight);
        paintTransportLamp(g, lamp(row, 0), kRed, motion_ == Motion::recording, Symbol::record);
        paintTransportLamp(g, lamp(row, 1), juce::Colour(0xffff7a2f), isAtEnd(), Symbol::text, "END");
    }

    // The machine's settings, printed along the front edge.
    const auto emphasis = devices::TapeA800::emphasisOf(speed(), tape_.standard_->getIndex());
    juce::String settings;
    settings << juce::String(ips(), speed() == 0 ? 1 : 0) << " IPS  " << emphasis.name << "  "
             << juce::String(devices::TapeA800::formulaOf(tape_.formula_->getIndex()).name).toUpperCase() << "  CAL "
             << tape_.calibration_->getCurrentChoiceName();
    if (tape_.group_->getIndex() > 0)
        settings << "  GROUP " << tape_.group_->getIndex();
    hardware::paintEngraved(g, strip, settings, kInk, theme::label(7.5f));
}

int TapeDeck::speed() const noexcept
{
    return tape_.speed_->getIndex();
}

void TapeDeck::paintReel(juce::Graphics& g, juce::Point<float> centre, float flange, double packInches, double angle, float blur) const
{
    const float scale = flange / static_cast<float>(tape::kFlangeRadius);
    const float pack = static_cast<float>(packInches) * scale;
    const float hub = static_cast<float>(tape::kHubRadius) * scale;
    const auto disc = [centre](float radius) { return juce::Rectangle<float>(radius * 2.0f, radius * 2.0f).withCentre(centre); };

    // Its shadow on the deck.
    g.setColour(juce::Colours::black.withAlpha(0.2f));
    g.fillEllipse(disc(flange + 1.0f).translated(2.5f, 3.5f));

    // The tape pack between the flanges: its edge, wound in rings.
    juce::ColourGradient brown(juce::Colour(0xff4f3828), centre.x, centre.y, juce::Colour(0xff281b12), centre.x + pack, centre.y, true);
    g.setGradientFill(brown);
    g.fillEllipse(disc(pack));
    for (float r = hub + 1.5f; r < pack - 0.5f; r += 2.3f)
    {
        g.setColour(juce::Colours::white.withAlpha(0.035f + 0.03f * std::sin(r * 1.7f)));
        g.drawEllipse(disc(r), 0.8f);
    }
    g.setColour(juce::Colours::black.withAlpha(0.5f));
    g.drawEllipse(disc(pack), 0.8f);

    // The front flange: spun aluminium with three windows, turning with the reel.
    const float inner = hub * 1.3f, outer = flange * 0.86f;
    juce::Path windows;
    for (int w = 0; w < 3; ++w)
    {
        const float a = static_cast<float>(std::fmod(angle, static_cast<double>(kTwoPi))) + static_cast<float>(w) * kTwoPi / 3.0f;
        juce::Path window;
        window.addPieSegment(disc(outer), a - 0.6f, a + 0.6f, inner / outer);
        windows.addPath(window.createPathWithRoundedCorners(flange * 0.07f));
    }
    juce::Path front;
    front.addEllipse(disc(flange));
    front.addPath(windows);
    front.setUsingNonZeroWinding(false);
    juce::ColourGradient aluminium(juce::Colour(0xffeef0f2), centre.x - flange, centre.y - flange, juce::Colour(0xff858b92), centre.x + flange,
                                   centre.y + flange, false);
    aluminium.addColour(0.45, juce::Colour(0xffcfd3d7));
    aluminium.addColour(0.6, juce::Colour(0xffb3b8bd));
    g.setGradientFill(aluminium);
    g.fillPath(front);
    g.saveState();
    g.reduceClipRegion(front);
    for (float r = inner; r < flange; r += 3.1f) // (the spinning's rings)
    {
        g.setColour(juce::Colours::white.withAlpha(0.05f + 0.04f * std::sin(r * 0.9f)));
        g.drawEllipse(disc(r), 0.7f);
    }
    g.restoreState();
    if (blur > 0.0f) // (turning too fast to see the windows)
    {
        g.setColour(juce::Colour(0xffbfc3c8).withAlpha(0.78f * blur));
        g.fillPath(windows);
    }
    g.setColour(juce::Colours::black.withAlpha(0.35f));
    g.strokePath(windows, juce::PathStrokeType(0.7f));
    g.setColour(juce::Colour(0xff5d6267));
    g.drawEllipse(disc(flange - 0.5f), 1.0f);

    // The NAB hub with its three drive keys and the centre cap.
    const auto hubDisc = disc(hub * 0.95f);
    juce::ColourGradient chrome(juce::Colour(0xfff4f5f6), hubDisc.getX(), hubDisc.getY(), juce::Colour(0xff6e747a), hubDisc.getRight(),
                                hubDisc.getBottom(), false);
    g.setGradientFill(chrome);
    g.fillEllipse(hubDisc);
    g.setColour(juce::Colour(0xff2b2e31));
    for (int key = 0; key < 3; ++key)
    {
        const float a = static_cast<float>(std::fmod(angle, static_cast<double>(kTwoPi))) + static_cast<float>(key) * kTwoPi / 3.0f + kTwoPi / 6.0f;
        const juce::Point<float> at(centre.x + hub * 0.62f * std::sin(a), centre.y - hub * 0.62f * std::cos(a));
        g.fillEllipse(juce::Rectangle<float>(hub * 0.2f, hub * 0.2f).withCentre(at));
    }
    g.setColour(juce::Colour(0xff1a1b1d));
    g.fillEllipse(disc(hub * 0.4f));
    g.setColour(juce::Colour(0xffc9cdd1));
    g.drawEllipse(disc(hub * 0.4f), 1.0f);
}

void TapeDeck::paintMeterBridge(juce::Graphics& g, juce::Rectangle<float> area) const
{
    hardware::paintMetal(g, area, juce::Colour(0xff2b2c2f), 5.0f, false);
    g.setColour(juce::Colour(0xff111111));
    g.drawRoundedRectangle(area.reduced(0.5f), 5.0f, 1.0f);
    auto inner = area.reduced(6.0f, 5.0f);

    auto title = inner.removeFromTop(11.0f);
    hardware::paintEngraved(g, title, "A 800", juce::Colour(0xffd8d5cc), theme::label(8.5f), juce::Justification::centredLeft);
    hardware::paintEngraved(g, title, "MULTICHANNEL", juce::Colour(0xff9a9890), theme::label(6.5f), juce::Justification::centredRight);
    inner.removeFromTop(3.0f);

    auto lamps = inner.removeFromBottom(std::min(26.0f, inner.getHeight() * 0.22f));
    inner.removeFromBottom(4.0f);
    const float meterHeight = std::min((inner.getHeight() - 4.0f) * 0.5f, inner.getWidth() * 0.62f);
    for (int ch = 0; ch < 2; ++ch)
    {
        auto face = hardware::vuFace(juce::Colour(0xfff0e2b6), "VU");
        face.caption = ch == 0 ? "L" : "R";
        const auto meter = inner.removeFromTop(meterHeight).withSizeKeepingCentre(inner.getWidth(), meterHeight);
        hardware::paintNeedleMeter(g, meter, hardware::vuPosition(vu_[static_cast<std::size_t>(ch)]), face);
        inner.removeFromTop(4.0f);
    }

    // The monitor path the channel plays, lit (the one chosen outlined while Auto Input holds Input).
    const int active = tape_.getActivePath(), chosen = tape_.path_->getIndex();
    const float half = lamps.getWidth() * 0.5f, rowHeight = lamps.getHeight() * 0.5f;
    for (int p = 0; p < 4; ++p)
    {
        const auto cell = juce::Rectangle<float>(lamps.getX() + half * static_cast<float>(p % 2), lamps.getY() + rowHeight * static_cast<float>(p / 2),
                                                 half, rowHeight)
                              .reduced(1.0f);
        hardware::paintLamp(g, cell, p == devices::TapeA800::thru ? kWhite : kAmber, p == active,
                            juce::StringArray{"INPUT", "SYNC", "REPRO", "THRU"}[p], 1.5f, std::min(7.0f, cell.getHeight() * 0.6f));
        if (p == chosen && p != active)
        {
            g.setColour(kAmber.withAlpha(0.6f));
            g.drawRoundedRectangle(cell, 2.0f, 1.0f);
        }
    }
}

} // namespace dojo::ui
