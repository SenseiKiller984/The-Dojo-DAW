#include "ui/score/MusicXml.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace dojo::score
{
namespace
{
constexpr int kDivisions = 16; // per quarter note: a 64th is one

const char* typeName(int type)
{
    static constexpr const char* names[] = {"whole", "half", "quarter", "eighth", "16th", "32nd", "64th"};
    return names[std::clamp(type, 0, 6)];
}

const char* stepName(int step)
{
    static constexpr const char* names[] = {"C", "D", "E", "F", "G", "A", "B"};
    return names[std::clamp(step, 0, 6)];
}

const char* accidentalName(Accidental a)
{
    switch (a)
    {
        case Accidental::natural: return "natural";
        case Accidental::sharp: return "sharp";
        case Accidental::flat: return "flat";
        case Accidental::doubleSharp: return "double-sharp";
        case Accidental::doubleFlat: return "flat-flat";
        case Accidental::none: break;
    }
    return "";
}

int divisionsOf(double beats) { return static_cast<int>(std::lround(beats * kDivisions)); }

juce::XmlElement* add(juce::XmlElement& parent, const char* name, const juce::String& text = {})
{
    auto* e = parent.createNewChildElement(name);
    if (text.isNotEmpty())
        e->addTextElement(text);
    return e;
}

/** The beam values (number 1 and 2) of each event of a staff. */
std::vector<std::pair<juce::String, juce::String>> beams(const Staff& staff)
{
    std::vector<std::pair<juce::String, juce::String>> out(staff.events.size());
    for (std::size_t i = 0; i < staff.events.size(); ++i)
    {
        const auto& e = staff.events[i];
        if (e.beam < 0)
            continue;
        const bool prevSame = i > 0 && staff.events[i - 1].beam == e.beam;
        const bool nextSame = i + 1 < staff.events.size() && staff.events[i + 1].beam == e.beam;
        out[i].first = !prevSame ? "begin" : !nextSame ? "end" : "continue";
        if (e.type >= 4)
        {
            const bool prev16 = prevSame && staff.events[i - 1].type >= 4;
            const bool next16 = nextSame && staff.events[i + 1].type >= 4;
            out[i].second = prev16 && next16 ? "continue" : next16 ? "begin" : prev16 ? "end" : !prevSame ? "forward hook" : "backward hook";
        }
    }
    return out;
}
} // namespace

juce::String toMusicXml(const Score& score, const juce::String& title, const juce::String& partName)
{
    juce::XmlElement root("score-partwise");
    root.setAttribute("version", "4.0");
    add(*add(root, "work"), "work-title", title.isNotEmpty() ? title : juce::String("Untitled"));
    auto* identification = add(root, "identification");
    add(*add(*identification, "encoding"), "software", "The Dojo");
    auto* parts = add(root, "part-list");
    auto* scorePart = add(*parts, "score-part");
    scorePart->setAttribute("id", "P1");
    add(*scorePart, "part-name", partName.isNotEmpty() ? partName : juce::String("Part"));

    auto* part = add(root, "part");
    part->setAttribute("id", "P1");
    const bool grand = score.staves.size() > 1;
    std::vector<std::vector<std::pair<juce::String, juce::String>>> beamValues;
    for (const auto& staff : score.staves)
        beamValues.push_back(beams(staff));

    for (std::size_t b = 0; b < score.bars.size(); ++b)
    {
        const auto& bar = score.bars[b];
        auto* measure = add(*part, "measure");
        measure->setAttribute("number", juce::String(bar.number + 1));
        if (b == 0 || bar.showSignature)
        {
            auto* attributes = add(*measure, "attributes");
            if (b == 0)
            {
                add(*attributes, "divisions", juce::String(kDivisions));
                add(*add(*attributes, "key"), "fifths", juce::String(score.keyFifths));
            }
            auto* timeElement = add(*attributes, "time");
            add(*timeElement, "beats", juce::String(bar.signature.numerator));
            add(*timeElement, "beat-type", juce::String(bar.signature.denominator));
            if (b == 0)
            {
                if (grand)
                    add(*attributes, "staves", "2");
                for (std::size_t s = 0; s < score.staves.size(); ++s)
                {
                    auto* clef = add(*attributes, "clef");
                    if (grand)
                        clef->setAttribute("number", static_cast<int>(s) + 1);
                    const bool treble = score.staves[s].clef == Clef::treble;
                    add(*clef, "sign", treble ? "G" : "F");
                    add(*clef, "line", treble ? "2" : "4");
                }
            }
        }

        for (std::size_t s = 0; s < score.staves.size(); ++s)
        {
            const auto& staff = score.staves[s];
            if (s > 0)
                add(*add(*measure, "backup"), "duration", juce::String(divisionsOf(bar.end - bar.start)));
            for (std::size_t i = 0; i < staff.events.size(); ++i)
            {
                const auto& e = staff.events[i];
                if (e.bar != static_cast<int>(b))
                    continue;
                const int heads = e.rest ? 1 : static_cast<int>(e.heads.size());
                for (int h = 0; h < heads; ++h)
                {
                    auto* note = add(*measure, "note");
                    if (h > 0)
                        add(*note, "chord");
                    if (e.rest)
                    {
                        auto* rest = add(*note, "rest");
                        if (e.wholeBarRest)
                            rest->setAttribute("measure", "yes");
                    }
                    else
                    {
                        const auto& head = e.heads[static_cast<std::size_t>(h)];
                        auto* pitch = add(*note, "pitch");
                        add(*pitch, "step", stepName(head.spelling.step));
                        if (head.spelling.alter != 0)
                            add(*pitch, "alter", juce::String(head.spelling.alter));
                        add(*pitch, "octave", juce::String(head.spelling.octave));
                    }
                    add(*note, "duration", juce::String(divisionsOf(e.length)));
                    if (!e.rest)
                    {
                        const auto& head = e.heads[static_cast<std::size_t>(h)];
                        if (head.tieEnd)
                            add(*note, "tie")->setAttribute("type", "stop");
                        if (head.tieStart)
                            add(*note, "tie")->setAttribute("type", "start");
                    }
                    add(*note, "voice", grand && s == 1 ? "5" : "1");
                    if (!e.wholeBarRest)
                        add(*note, "type", typeName(e.type));
                    for (int d = 0; d < e.dots; ++d)
                        add(*note, "dot");
                    if (!e.rest)
                    {
                        const auto& head = e.heads[static_cast<std::size_t>(h)];
                        if (head.accidental != Accidental::none)
                            add(*note, "accidental", accidentalName(head.accidental));
                        add(*note, "stem", e.stemUp ? "up" : "down");
                    }
                    if (grand)
                        add(*note, "staff", juce::String(static_cast<int>(s) + 1));
                    if (!e.rest && h == 0)
                    {
                        const auto& [beam1, beam2] = beamValues[s][i];
                        if (beam1.isNotEmpty())
                            add(*note, "beam", beam1)->setAttribute("number", 1);
                        if (beam2.isNotEmpty())
                            add(*note, "beam", beam2)->setAttribute("number", 2);
                    }
                    if (!e.rest)
                    {
                        const auto& head = e.heads[static_cast<std::size_t>(h)];
                        if (head.tieStart || head.tieEnd)
                        {
                            auto* notations = add(*note, "notations");
                            if (head.tieEnd)
                                add(*notations, "tied")->setAttribute("type", "stop");
                            if (head.tieStart)
                                add(*notations, "tied")->setAttribute("type", "start");
                        }
                    }
                }
            }
        }
        if (b + 1 == score.bars.size())
        {
            auto* barline = add(*measure, "barline");
            barline->setAttribute("location", "right");
            add(*barline, "bar-style", "light-heavy");
        }
    }

    juce::XmlElement::TextFormat format;
    format.newLineChars = "\n";
    format.customHeader = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\"?>";
    format.dtd = "<!DOCTYPE score-partwise PUBLIC \"-//Recordare//DTD MusicXML 4.0 Partwise//EN\" "
                 "\"http://www.musicxml.org/dtds/partwise.dtd\">";
    return root.toString(format);
}

bool writeMusicXml(const Score& score, const juce::File& file, const juce::String& title, const juce::String& partName)
{
    return file.replaceWithText(toMusicXml(score, title, partName), false, false, "\n");
}

} // namespace dojo::score
