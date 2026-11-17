#include "ui/devices/HardwarePainting.h"

#include "ui/theme/DojoTheme.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace dojo::ui::hardware
{
namespace
{
/** Rendered textures, by look, size and pixel scale (freed with the GUI). */
class TextureCache final : public juce::DeletedAtShutdown
{
public:
    ~TextureCache() override { clearSingletonInstance(); }

    juce::Image& get(const juce::String& key, int width, int height, float scale,
                     const std::function<void(juce::Graphics&, juce::Rectangle<float>)>& render)
    {
        const auto id = key + "|" + juce::String(width) + "x" + juce::String(height) + "@" + juce::String(scale, 2);
        auto found = images_.find(id);
        if (found == images_.end())
        {
            if (images_.size() > 128)
                images_.clear();
            juce::Image image(juce::Image::ARGB, std::max(1, juce::roundToInt(static_cast<float>(width) * scale)),
                              std::max(1, juce::roundToInt(static_cast<float>(height) * scale)), true);
            {
                juce::Graphics painter(image);
                painter.addTransform(juce::AffineTransform::scale(scale));
                render(painter, juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)));
            }
            found = images_.emplace(id, image).first;
        }
        return found->second;
    }

    JUCE_DECLARE_SINGLETON_SINGLETHREADED_INLINE(TextureCache, false)

private:
    std::map<juce::String, juce::Image> images_;
};

void drawCached(juce::Graphics& g, juce::Rectangle<float> area, const juce::String& key,
                const std::function<void(juce::Graphics&, juce::Rectangle<float>)>& render)
{
    const auto bounds = area.getSmallestIntegerContainer();
    if (bounds.isEmpty())
        return;
    const float scale = std::clamp(g.getInternalContext().getPhysicalPixelScaleFactor(), 1.0f, 4.0f);
    auto& image = TextureCache::getInstance()->get(key, bounds.getWidth(), bounds.getHeight(), scale, render);
    g.drawImage(image, bounds.toFloat());
}

juce::String keyOf(juce::Colour colour)
{
    return colour.toString();
}

float angleOf(double position, float halfSwing)
{
    return -halfSwing + 2.0f * halfSwing * static_cast<float>(position);
}

juce::Point<float> pointAt(juce::Point<float> pivot, float angle, float radius)
{
    return {pivot.x + radius * std::sin(angle), pivot.y - radius * std::cos(angle)};
}
} // namespace

//==================================================================================================
// Meters

double vuPosition(double vu) noexcept
{
    return std::pow(10.0, (vu - 3.0) / 20.0);
}

MeterFace vuFace(juce::Colour face, const juce::String& legend)
{
    MeterFace f;
    f.face = face;
    f.legend = legend;
    f.redFrom = vuPosition(0.0);
    for (const auto& [vu, text] : std::initializer_list<std::pair<double, const char*>>{
             {-20.0, "20"}, {-10.0, "10"}, {-7.0, ""}, {-5.0, "5"}, {-3.0, "3"}, {-2.0, ""}, {-1.0, ""}, {0.0, "0"}, {1.0, ""}, {2.0, ""}, {3.0, "+3"}})
        f.marks.push_back({vuPosition(vu), text, vu >= 0.0});
    return f;
}

void paintNeedleMeter(juce::Graphics& g, juce::Rectangle<float> area, double position, const MeterFace& f)
{
    if (area.getWidth() < 8.0f || area.getHeight() < 8.0f)
        return;
    const float small = std::min(area.getWidth(), area.getHeight());
    const float corner = small * 0.07f;

    // The case.
    g.setColour(f.bezel);
    g.fillRoundedRectangle(area, corner + 1.5f);
    g.setColour(juce::Colours::white.withAlpha(0.12f));
    g.drawRoundedRectangle(area.reduced(0.5f), corner + 1.5f, 1.0f);
    const auto face = area.reduced(std::max(2.0f, small * 0.06f));

    // The face, lit from behind (VU) or plain.
    juce::Path facePath;
    facePath.addRoundedRectangle(face, corner);
    if (f.backlit)
    {
        juce::ColourGradient lamp(f.face.brighter(0.12f), face.getCentreX(), face.getBottom(), f.face.darker(0.22f), face.getX(),
                                  face.getY(), true);
        g.setGradientFill(lamp);
    }
    else
    {
        juce::ColourGradient plain(f.face.brighter(0.05f), 0.0f, face.getY(), f.face.darker(0.08f), 0.0f, face.getBottom(), false);
        g.setGradientFill(plain);
    }
    g.fillPath(facePath);

    g.saveState();
    g.reduceClipRegion(facePath);

    // The scale: as large an arc as the face's height allows, its swing narrowed until the end
    // labels fit across a narrow face.
    const float textHeight = std::clamp(face.getHeight() * 0.15f, 5.5f, 10.0f);
    const juce::Point<float> pivot(face.getCentreX(), face.getBottom() + face.getHeight() * 0.3f);
    const float radius = pivot.y - face.getY() - textHeight * 1.7f;
    const float fit = (face.getWidth() * 0.5f - textHeight * 1.15f) / (radius + textHeight * 1.3f);
    const float halfSwing = std::clamp(std::asin(std::clamp(fit, 0.0f, 1.0f)), 0.3f, 0.8f);
    const float housing = face.getHeight() * 0.17f;
    const float stroke = std::max(0.8f, small * 0.018f);
    juce::Path arc;
    arc.addCentredArc(pivot.x, pivot.y, radius, radius, 0.0f, angleOf(0.0, halfSwing), angleOf(std::min(f.redFrom, 1.0), halfSwing), true);
    g.setColour(f.ink);
    g.strokePath(arc, juce::PathStrokeType(stroke));
    if (f.redFrom < 1.0)
    {
        juce::Path red;
        red.addCentredArc(pivot.x, pivot.y, radius + stroke, radius + stroke, 0.0f, angleOf(f.redFrom, halfSwing), angleOf(1.0, halfSwing), true);
        g.setColour(f.red);
        g.strokePath(red, juce::PathStrokeType(stroke * 2.6f));
    }
    g.setFont(theme::sansBold(textHeight));
    for (const auto& mark : f.marks)
    {
        const float a = angleOf(mark.position, halfSwing);
        const float length = mark.text.isEmpty() ? textHeight * 0.35f : textHeight * 0.6f;
        g.setColour(mark.red ? f.red : f.ink);
        g.drawLine(juce::Line<float>(pointAt(pivot, a, radius), pointAt(pivot, a, radius + length)), stroke);
        if (mark.text.isNotEmpty())
        {
            const auto at = pointAt(pivot, a, radius + length + textHeight * 0.7f);
            g.drawText(mark.text, juce::Rectangle<float>(textHeight * 2.6f, textHeight * 1.2f).withCentre(at), juce::Justification::centred, false);
        }
    }
    g.setColour(f.ink);
    g.setFont(theme::sansBold(textHeight * 1.3f));
    const auto legendCentre = juce::Point<float>(pivot.x, face.getBottom() - housing * 1.45f - textHeight * (f.caption.isEmpty() ? 0.8f : 1.7f));
    g.drawText(f.legend, juce::Rectangle<float>(textHeight * 6.0f, textHeight * 1.5f).withCentre(legendCentre), juce::Justification::centred, false);
    if (f.caption.isNotEmpty())
    {
        g.setFont(theme::label(textHeight * 0.9f));
        g.drawText(f.caption, juce::Rectangle<float>(textHeight * 8.0f, textHeight * 1.2f).withCentre(legendCentre.translated(0.0f, textHeight * 1.25f)),
                   juce::Justification::centred, false);
    }

    // The needle, its pivot under the housing.
    const float a = angleOf(std::clamp(position, -0.04, 1.05), halfSwing);
    const auto tip = pointAt(pivot, a, radius + textHeight * 0.55f);
    g.setColour(juce::Colours::black.withAlpha(0.2f));
    g.drawLine(juce::Line<float>(pivot.translated(1.4f, 1.0f), tip.translated(1.4f, 1.0f)), stroke * 1.2f);
    g.setColour(f.needle);
    g.drawLine(juce::Line<float>(pivot, tip), stroke * 1.2f);
    juce::Path cover;
    cover.startNewSubPath(face.getX(), face.getBottom());
    cover.lineTo(face.getX(), face.getBottom() - housing * 0.55f);
    cover.quadraticTo(face.getCentreX(), face.getBottom() - housing * 1.45f, face.getRight(), face.getBottom() - housing * 0.55f);
    cover.lineTo(face.getRight(), face.getBottom());
    cover.closeSubPath();
    g.setColour(f.bezel.brighter(0.15f));
    g.fillPath(cover);
    g.restoreState();

    // The glass.
    juce::ColourGradient glass(juce::Colours::white.withAlpha(0.2f), face.getX(), face.getY(), juce::Colours::transparentWhite,
                               face.getX() + face.getWidth() * 0.45f, face.getY() + face.getHeight() * 0.6f, false);
    g.setGradientFill(glass);
    g.fillPath(facePath);
}

//==================================================================================================
// Lamps, LEDs, digits, screws

void paintLamp(juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour, bool on, const juce::String& text, float corner,
               float textHeight)
{
    if (area.isEmpty())
        return;
    g.setColour(juce::Colour(0xff0b0b0b));
    g.fillRoundedRectangle(area, corner + 1.0f);
    const auto lens = area.reduced(1.2f);
    if (on)
    {
        juce::ColourGradient lit(colour.brighter(0.55f), lens.getCentreX(), lens.getCentreY(), colour.darker(0.1f), lens.getX(), lens.getY(), true);
        g.setGradientFill(lit);
        g.fillRoundedRectangle(lens, corner);
        g.setColour(colour.withAlpha(0.28f));
        g.drawRoundedRectangle(area.expanded(1.0f), corner + 2.0f, 2.0f);
    }
    else
    {
        const auto dim = colour.interpolatedWith(juce::Colours::black, 0.74f);
        juce::ColourGradient dark(dim.brighter(0.3f), 0.0f, lens.getY(), dim.darker(0.35f), 0.0f, lens.getBottom(), false);
        g.setGradientFill(dark);
        g.fillRoundedRectangle(lens, corner);
    }
    g.setColour(juce::Colours::white.withAlpha(on ? 0.32f : 0.1f));
    g.fillRoundedRectangle(lens.withHeight(lens.getHeight() * 0.36f).reduced(1.0f, 0.6f), std::max(0.5f, corner * 0.8f));
    if (text.isNotEmpty())
    {
        g.setFont(theme::label(textHeight));
        g.setColour(on ? theme::textOn(colour.brighter(0.3f)) : colour.interpolatedWith(juce::Colours::white, 0.3f).withAlpha(0.7f));
        g.drawFittedText(text, lens.toNearestInt().reduced(1, 0), juce::Justification::centred, 1, 0.8f);
    }
}

void paintLed(juce::Graphics& g, juce::Point<float> centre, float radius, juce::Colour colour, bool on)
{
    g.setColour(juce::Colour(0xff0a0a0a));
    g.fillEllipse(juce::Rectangle<float>(radius * 2.0f + 2.0f, radius * 2.0f + 2.0f).withCentre(centre));
    const auto led = juce::Rectangle<float>(radius * 2.0f, radius * 2.0f).withCentre(centre);
    if (on)
    {
        g.setColour(colour.withAlpha(0.3f));
        g.fillEllipse(led.expanded(radius * 0.9f));
    }
    const auto body = on ? colour : colour.interpolatedWith(juce::Colours::black, 0.75f);
    juce::ColourGradient lens(body.brighter(on ? 0.8f : 0.3f), led.getX() + radius * 0.6f, led.getY() + radius * 0.5f, body.darker(0.3f),
                              led.getRight(), led.getBottom(), true);
    g.setGradientFill(lens);
    g.fillEllipse(led);
}

void paintLedText(juce::Graphics& g, juce::Rectangle<float> area, const juce::String& text, juce::Colour colour, float height,
                  juce::Justification justification)
{
    g.setFont(theme::mono(height).boldened());
    juce::String ghost;
    for (const auto c : text)
        ghost << (juce::CharacterFunctions::isDigit(c) ? juce::juce_wchar('8') : c);
    g.setColour(colour.withAlpha(0.1f));
    g.drawText(ghost, area, justification, false);
    g.setColour(colour.withAlpha(0.22f));
    for (const auto& offset : {juce::Point<float>(-0.7f, 0.0f), juce::Point<float>(0.7f, 0.0f), juce::Point<float>(0.0f, -0.7f), juce::Point<float>(0.0f, 0.7f)})
        g.drawText(text, area.translated(offset.x, offset.y), justification, false);
    g.setColour(colour.brighter(0.25f));
    g.drawText(text, area, justification, false);
}

void paintScrew(juce::Graphics& g, juce::Point<float> centre, float radius, float slotAngle)
{
    const auto head = juce::Rectangle<float>(radius * 2.0f, radius * 2.0f).withCentre(centre);
    g.setColour(juce::Colours::black.withAlpha(0.35f));
    g.fillEllipse(head.translated(0.4f, 0.6f).expanded(0.4f));
    juce::ColourGradient metal(juce::Colour(0xffe4e6e8), head.getX(), head.getY(), juce::Colour(0xff6c7075), head.getRight(), head.getBottom(), false);
    g.setGradientFill(metal);
    g.fillEllipse(head);
    const auto dx = std::cos(slotAngle) * radius * 0.8f, dy = std::sin(slotAngle) * radius * 0.8f;
    g.setColour(juce::Colour(0xff2a2c2f));
    g.drawLine(centre.x - dx, centre.y - dy, centre.x + dx, centre.y + dy, std::max(0.8f, radius * 0.38f));
}

//==================================================================================================
// Surfaces

void paintMetal(juce::Graphics& g, juce::Rectangle<float> area, juce::Colour base, float corner, bool speckle, int seed)
{
    if (area.isEmpty())
        return;
    if (!speckle)
    {
        juce::ColourGradient light(base.brighter(0.07f), 0.0f, area.getY(), base.darker(0.09f), 0.0f, area.getBottom(), false);
        g.setGradientFill(light);
        g.fillRoundedRectangle(area, corner);
        return;
    }
    drawCached(g, area, "metal|" + keyOf(base) + "|" + juce::String(seed) + "|" + juce::String(corner, 1),
               [base, corner, seed](juce::Graphics& t, juce::Rectangle<float> r)
               {
                   juce::Path shape;
                   shape.addRoundedRectangle(r, corner);
                   t.reduceClipRegion(shape);
                   juce::ColourGradient light(base.brighter(0.07f), 0.0f, r.getY(), base.darker(0.09f), 0.0f, r.getBottom(), false);
                   t.setGradientFill(light);
                   t.fillRect(r);
                   juce::Random random(seed);
                   const int count = static_cast<int>(r.getWidth() * r.getHeight() / 14.0f);
                   for (int i = 0; i < count; ++i)
                   {
                       const float x = random.nextFloat() * r.getWidth(), y = random.nextFloat() * r.getHeight();
                       const float size = 0.7f + random.nextFloat() * 1.3f;
                       const bool bright = random.nextBool();
                       t.setColour((bright ? juce::Colours::white : juce::Colours::black).withAlpha(0.025f + random.nextFloat() * 0.05f));
                       t.fillEllipse(x, y, size, size);
                   }
               });
}

void paintWood(juce::Graphics& g, juce::Rectangle<float> area, juce::Colour base, int seed, float corner)
{
    drawCached(g, area, "wood|" + keyOf(base) + "|" + juce::String(seed) + "|" + juce::String(corner, 1),
               [base, seed, corner](juce::Graphics& t, juce::Rectangle<float> r)
               {
                   juce::Path shape;
                   shape.addRoundedRectangle(r, corner);
                   t.reduceClipRegion(shape);
                   t.setColour(base);
                   t.fillRect(r);
                   const bool vertical = r.getHeight() > r.getWidth();
                   const float length = vertical ? r.getHeight() : r.getWidth();
                   const float across = vertical ? r.getWidth() : r.getHeight();
                   const auto at = [vertical](float along, float offset) { return vertical ? juce::Point<float>(offset, along) : juce::Point<float>(along, offset); };
                   juce::Random random(seed * 7919 + 17);

                   // The figure: broad, slowly wandering bands of lighter and darker wood.
                   for (int band = 0; band < static_cast<int>(across / 5.0f) + 3; ++band)
                   {
                       const float centre = random.nextFloat() * across;
                       const float width = 1.5f + random.nextFloat() * 5.0f;
                       const float phase = random.nextFloat() * 6.28f, frequency = 0.004f + random.nextFloat() * 0.01f;
                       juce::Path path;
                       for (float s = -4.0f; s <= length + 4.0f; s += 5.0f)
                       {
                           const auto p = at(s, centre + 3.0f * std::sin(s * frequency + phase));
                           s < 0.0f ? path.startNewSubPath(p) : path.lineTo(p);
                       }
                       t.setColour((random.nextBool() ? base.brighter(0.18f) : base.darker(0.25f)).withAlpha(0.3f));
                       t.strokePath(path, juce::PathStrokeType(width));
                   }
                   // The grain.
                   for (float offset = random.nextFloat(); offset < across; offset += 0.8f + random.nextFloat() * 1.7f)
                   {
                       const float amplitude = 0.4f + random.nextFloat() * 1.8f;
                       const float f1 = 0.008f + random.nextFloat() * 0.03f, f2 = 0.05f + random.nextFloat() * 0.08f;
                       const float p1 = random.nextFloat() * 6.28f, p2 = random.nextFloat() * 6.28f;
                       juce::Path path;
                       for (float s = -4.0f; s <= length + 4.0f; s += 4.0f)
                       {
                           const auto p = at(s, offset + amplitude * std::sin(s * f1 + p1) + 0.35f * std::sin(s * f2 + p2));
                           s < 0.0f ? path.startNewSubPath(p) : path.lineTo(p);
                       }
                       t.setColour(base.darker(0.4f + random.nextFloat() * 0.6f).withAlpha(0.16f + random.nextFloat() * 0.3f));
                       t.strokePath(path, juce::PathStrokeType(0.45f + random.nextFloat() * 0.7f));
                   }
                   // The varnish: a sheen along the length, darker edges.
                   const auto sheenFrom = at(0.0f, 0.0f), sheenTo = at(0.0f, across);
                   juce::ColourGradient sheen(juce::Colours::black.withAlpha(0.28f), sheenFrom.x, sheenFrom.y, juce::Colours::black.withAlpha(0.3f),
                                              vertical ? sheenTo.x : sheenFrom.x, vertical ? sheenFrom.y : sheenTo.y, false);
                   sheen.addColour(0.3, juce::Colours::white.withAlpha(0.1f));
                   sheen.addColour(0.55, juce::Colours::transparentWhite);
                   t.setGradientFill(sheen);
                   t.fillRect(r);
               });
}

void paintLeather(juce::Graphics& g, juce::Rectangle<float> area, juce::Colour base)
{
    drawCached(g, area, "leather|" + keyOf(base),
               [base](juce::Graphics& t, juce::Rectangle<float> r)
               {
                   juce::Path roll;
                   roll.addRoundedRectangle(r, std::min(6.0f, r.getHeight() * 0.45f));
                   t.reduceClipRegion(roll);
                   juce::ColourGradient padded(base.brighter(0.35f), 0.0f, r.getY(), base.darker(0.55f), 0.0f, r.getBottom(), false);
                   padded.addColour(0.35, base.brighter(0.1f));
                   padded.addColour(0.7, base.darker(0.15f));
                   t.setGradientFill(padded);
                   t.fillRect(r);
                   juce::Random random(4711);
                   const int pores = static_cast<int>(r.getWidth() * r.getHeight() / 9.0f);
                   for (int i = 0; i < pores; ++i)
                   {
                       t.setColour((random.nextBool() ? juce::Colours::white : juce::Colours::black).withAlpha(0.03f + random.nextFloat() * 0.05f));
                       t.fillEllipse(random.nextFloat() * r.getWidth(), random.nextFloat() * r.getHeight(), 1.1f, 0.8f);
                   }
                   // The seam: stitches along the upper edge.
                   const float seam = r.getY() + std::max(2.5f, r.getHeight() * 0.26f);
                   t.setColour(base.darker(0.6f).withAlpha(0.7f));
                   t.drawHorizontalLine(juce::roundToInt(seam + 1.6f), r.getX(), r.getRight());
                   t.setColour(base.brighter(0.75f).withAlpha(0.75f));
                   for (float x = r.getX() + 2.0f; x < r.getRight() - 2.0f; x += 5.0f)
                       t.fillRect(juce::Rectangle<float>(x, seam - 0.5f, 3.0f, 1.0f));
               });
}

void paintEngraved(juce::Graphics& g, juce::Rectangle<float> area, const juce::String& text, juce::Colour ink, const juce::Font& font,
                   juce::Justification justification)
{
    g.setFont(font);
    const bool darkInk = ink.getPerceivedBrightness() < 0.5f;
    g.setColour(darkInk ? juce::Colours::white.withAlpha(0.4f) : juce::Colours::black.withAlpha(0.65f));
    g.drawText(text, area.translated(0.0f, 0.8f), justification, false);
    g.setColour(ink);
    g.drawText(text, area, justification, false);
}

//==================================================================================================
// Faders

float faderCapCentre(juce::Rectangle<float> area, float position, float capHeight) noexcept
{
    const float top = area.getY() + capHeight * 0.5f, bottom = area.getBottom() - capHeight * 0.5f;
    return bottom - (bottom - top) * std::clamp(position, 0.0f, 1.0f);
}

void paintFader(juce::Graphics& g, juce::Rectangle<float> area, float position, const FaderStyle& style)
{
    const float slotWidth = std::max(3.0f, area.getWidth() * 0.12f);
    const auto slot = juce::Rectangle<float>(slotWidth, std::max(0.0f, area.getHeight() - style.capHeight * 0.6f)).withCentre(area.getCentre());
    g.setColour(style.slot);
    g.fillRoundedRectangle(slot, slotWidth * 0.5f);
    g.setColour(juce::Colours::white.withAlpha(0.1f));
    g.drawRoundedRectangle(slot.expanded(0.6f), slotWidth * 0.5f, 0.8f);

    const float centre = faderCapCentre(area, position, style.capHeight);
    const auto cap = juce::Rectangle<float>(area.getWidth() * 0.9f, style.capHeight).withCentre({area.getCentreX(), centre});
    g.setColour(juce::Colours::black.withAlpha(0.4f));
    g.fillRoundedRectangle(cap.translated(1.0f, 2.0f), 2.5f);
    juce::ColourGradient body(style.cap.brighter(0.2f), 0.0f, cap.getY(), style.cap.darker(0.4f), 0.0f, cap.getBottom(), false);
    body.addColour(0.45, style.cap);
    body.addColour(0.55, style.cap.darker(0.22f));
    g.setGradientFill(body);
    g.fillRoundedRectangle(cap, 2.5f);
    g.setColour(style.cap.darker(0.45f).withAlpha(0.6f));
    for (const float k : {0.22f, 0.78f})
        g.drawHorizontalLine(juce::roundToInt(cap.getY() + cap.getHeight() * k), cap.getX() + 2.0f, cap.getRight() - 2.0f);
    g.setColour(style.capLine);
    g.fillRect(cap.withSizeKeepingCentre(cap.getWidth() - 2.0f, 1.8f));
}

ParameterFader::ParameterFader(juce::RangedAudioParameter& parameter, FaderStyle style, std::vector<float> marks)
    : parameter_(parameter)
    , style_(style)
    , marks_(std::move(marks))
{
    setRepaintsOnMouseActivity(false);
    refresh();
}

ParameterFader::~ParameterFader()
{
    if (dragging_)
        parameter_.endChangeGesture();
}

void ParameterFader::refresh()
{
    const float value = parameter_.getValue();
    if (std::abs(value - shown_) < 1.0e-6f)
        return;
    shown_ = value;
    setTooltip(parameter_.getName(32) + ": " + parameter_.getText(value, 12).trim() + " " + parameter_.getLabel() +
               "  (drag; Shift for fine moves; double-click resets)");
    repaint();
}

juce::Rectangle<float> ParameterFader::getSlot() const noexcept
{
    auto area = getLocalBounds().toFloat().reduced(1.0f, 2.0f);
    area.removeFromLeft(std::min(16.0f, area.getWidth() * 0.42f));
    return area;
}

void ParameterFader::paint(juce::Graphics& g)
{
    const auto slot = getSlot();
    g.setFont(theme::mono(7.0f));
    for (const float db : marks_)
    {
        const float y = faderCapCentre(slot, parameter_.convertTo0to1(db), style_.capHeight);
        g.setColour(style_.scale);
        g.drawHorizontalLine(juce::roundToInt(y), slot.getX() + 1.0f, slot.getX() + slot.getWidth() * 0.3f);
        g.drawHorizontalLine(juce::roundToInt(y), slot.getRight() - slot.getWidth() * 0.3f, slot.getRight() - 1.0f);
        const juce::String text = db > 0.0f ? "+" + juce::String(static_cast<int>(db)) : juce::String(static_cast<int>(std::abs(db)));
        g.drawText(text, juce::Rectangle<float>(0.0f, y - 5.0f, slot.getX() - 1.0f, 10.0f), juce::Justification::centredRight, false);
    }
    paintFader(g, slot, shown_, style_);
}

float ParameterFader::positionAt(float y) const noexcept
{
    const auto slot = getSlot();
    const float top = slot.getY() + style_.capHeight * 0.5f, bottom = slot.getBottom() - style_.capHeight * 0.5f;
    return std::clamp((bottom - y) / std::max(1.0f, bottom - top), 0.0f, 1.0f);
}

void ParameterFader::mouseDown(const juce::MouseEvent& e)
{
    const auto slot = getSlot();
    const float y = e.position.y;
    parameter_.beginChangeGesture();
    dragging_ = true;
    lastY_ = y;
    dragValue_ = parameter_.getValue();
    if (std::abs(y - faderCapCentre(slot, shown_, style_.capHeight)) > style_.capHeight * 0.5f) // (off the cap: it jumps there)
    {
        dragValue_ = positionAt(y);
        parameter_.setValueNotifyingHost(dragValue_);
        refresh();
    }
}

void ParameterFader::mouseDrag(const juce::MouseEvent& e)
{
    if (!dragging_)
        return;
    const auto slot = getSlot();
    const float travel = std::max(1.0f, slot.getHeight() - style_.capHeight);
    const float moved = (lastY_ - e.position.y) / travel;
    lastY_ = e.position.y;
    dragValue_ = std::clamp(dragValue_ + moved * (e.mods.isShiftDown() ? 0.2f : 1.0f), 0.0f, 1.0f); // Shift: a fifth of the movement
    parameter_.setValueNotifyingHost(dragValue_);
    refresh();
}

void ParameterFader::mouseUp(const juce::MouseEvent&)
{
    if (dragging_)
        parameter_.endChangeGesture();
    dragging_ = false;
}

void ParameterFader::mouseDoubleClick(const juce::MouseEvent&)
{
    parameter_.beginChangeGesture();
    parameter_.setValueNotifyingHost(parameter_.getDefaultValue());
    parameter_.endChangeGesture();
    refresh();
}

} // namespace dojo::ui::hardware
