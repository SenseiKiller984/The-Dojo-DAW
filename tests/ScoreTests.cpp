// The score editor: staff notation of MIDI clips (spelling, keys, values, rests, ties, beams, staves),
// MusicXML export, and editing on the staff.

#include "core/TempoMap.h"
#include "devices/DeviceRegistry.h"
#include "engine/AudioEngine.h"
#include "engine/EngineController.h"
#include "engine/io/AudioDeviceHost.h"
#include "session/Project.h"
#include "session/Session.h"
#include "ui/EditorState.h"
#include "ui/components/ClipEditor.h"
#include "ui/score/MusicXml.h"
#include "ui/score/Notation.h"
#include "ui/score/ScoreView.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <tuple>

using namespace dojo;
using Catch::Matchers::WithinAbs;

namespace
{
std::vector<score::SourceNote> sourceNotes(std::initializer_list<std::tuple<double, double, int>> list)
{
    std::vector<score::SourceNote> out;
    std::uint32_t id = 0;
    for (const auto& [start, end, midi] : list)
        out.push_back({start, end, midi, id++});
    return out;
}

score::Options inKey(int fifths, score::ClefMode clef = score::ClefMode::automatic)
{
    score::Options o;
    o.key = fifths;
    o.clef = clef;
    return o;
}

/** "type/dots" of each event, "r" for rests, "R" for whole-bar rests. */
std::vector<std::string> shape(const score::Staff& staff)
{
    std::vector<std::string> out;
    for (const auto& e : staff.events)
        out.push_back((e.wholeBarRest ? std::string("R") : e.rest ? std::string("r") : std::string()) + std::to_string(e.type)
                      + (e.dots > 0 ? "." : ""));
    return out;
}

std::unique_ptr<juce::XmlElement> parse(const juce::String& text)
{
    juce::XmlDocument doc(text);
    auto xml = doc.getDocumentElement();
    INFO(doc.getLastParseError().toStdString());
    REQUIRE(xml != nullptr);
    return xml;
}
} // namespace

TEST_CASE("Score: pitches spelled in the key, key signatures, key detection and note values", "[score]")
{
    const auto check = [](int midi, int key, int step, int alter, int octave)
    {
        const auto s = score::spell(midi, key);
        INFO("midi " << midi << " key " << key);
        CHECK(s.step == step);
        CHECK(s.alter == alter);
        CHECK(s.octave == octave);
    };
    check(60, 0, 0, 0, 4);  // C4
    check(61, 0, 0, 1, 4);  // C#4 in C
    check(61, -2, 1, -1, 4); // Db4 in Bb
    check(70, 0, 6, -1, 4); // Bb4 in C
    check(70, 2, 5, 1, 4);  // A#4 in D
    check(65, 6, 2, 1, 4);  // E#4 in F# major
    check(60, 7, 6, 1, 3);  // B#3 in C# major (the octave of the B)
    check(59, -6, 0, -1, 4); // Cb4 in Gb major
    check(64, -7, 3, -1, 4); // Fb4 in Cb major
    for (int key = -7; key <= 7; ++key)
        for (int midi = 0; midi <= 127; ++midi)
            REQUIRE(score::midiOf(score::spell(midi, key)) == midi);

    CHECK(score::keyAlteration(3, 1) == 1);   // F# in G
    CHECK(score::keyAlteration(0, 1) == 0);
    CHECK(score::keyAlteration(0, 2) == 1);   // C# in D
    CHECK(score::keyAlteration(6, -1) == -1); // Bb in F
    CHECK(score::keyAlteration(3, -7) == -1);
    CHECK(score::diatonic({0, 4, 0}) == 28);
    CHECK(std::string(score::keyName(1)) == "G major / E minor");
    CHECK(std::string(score::keyName(-3)) == "Eb major / C minor");

    // Detection: scales with their tonic triads weighted.
    const auto scale = [](int tonic)
    {
        std::vector<score::SourceNote> notes;
        const int steps[] = {0, 2, 4, 5, 7, 9, 11};
        double t = 0.0;
        for (const int s : steps)
        {
            const double length = (s == 0 || s == 4 || s == 7) ? 2.0 : 0.5;
            notes.push_back({t, t + length, 60 + tonic + s, 0});
            t += length;
        }
        return notes;
    };
    CHECK(score::detectKey(scale(0)) == 0);
    CHECK(score::detectKey(scale(7)) == 1);
    CHECK(score::detectKey(scale(10)) == -2);
    CHECK(score::detectKey(scale(4)) == 4);
    CHECK(score::detectKey({}) == 0);
    CHECK(score::detectKey(sourceNotes({{0, 1, 67}, {1, 2, 66}, {2, 3, 71}})) == 0); // (too little to tell)

    CHECK(score::valueLength(0, 0) == 4.0);
    CHECK(score::valueLength(2, 0) == 1.0);
    CHECK(score::valueLength(1, 1) == 3.0);
    CHECK(score::valueLength(3, 1) == 0.75);
    CHECK(score::valueLength(5, 0) == 0.125);
}

TEST_CASE("Score layout in 4/4: values, dots, rests by the beat, ties over the bar line, beams and stems", "[score]")
{
    const time::TempoMap map;
    const auto notes = sourceNotes({{0.0, 1.0, 60}, {1.0, 2.5, 64}, {2.5, 3.0, 67}, {3.0, 3.5, 69}, {3.5, 4.5, 71}});
    const auto s = score::layout(notes, 0.0, 8.0, map, inKey(0));
    REQUIRE(s.bars.size() == 2);
    CHECK(s.bars[0].showSignature);
    CHECK_FALSE(s.bars[1].showSignature);
    CHECK(s.bars[1].number == 1);
    CHECK(s.bars[1].start == 4.0);
    REQUIRE(s.staves.size() == 1);
    const auto& staff = s.staves.front();
    CHECK(staff.clef == score::Clef::treble);
    // quarter, dotted quarter, eighth (off the beat: not dotted across it), eighth, eighth tied to an
    // eighth, then rests by the beat: eighth, quarter, half.
    CHECK(shape(staff) == std::vector<std::string>{"2", "2.", "3", "3", "3", "3", "r3", "r2", "r1"});
    const auto& e = staff.events;
    CHECK(e[1].beat == 1.0);
    CHECK(e[1].length == 1.5);
    CHECK(e[4].heads.front().tieStart);
    CHECK_FALSE(e[4].heads.front().tieEnd);
    CHECK(e[5].heads.front().tieEnd);
    CHECK(e[5].bar == 1);
    CHECK(e[5].heads.front().id == 4);
    // Beams: the eighths of beat 4 together; the off-beat eighth of beat 3 and the tied one alone.
    CHECK(e[2].beam < 0);
    CHECK(e[3].beam >= 0);
    CHECK(e[3].beam == e[4].beam);
    CHECK(e[5].beam < 0);
    // Stems: up below the middle line.
    CHECK(e[0].stemUp);
    CHECK(e[3].stemUp);
    for (const auto& ev : e)
        for (const auto& h : ev.heads)
            CHECK(h.accidental == score::Accidental::none);

    // Off the beat: an eighth then a quarter, not a dotted quarter across the beat.
    const auto offBeat = score::layout(sourceNotes({{0.5, 2.0, 72}}), 0.0, 4.0, map, inKey(0));
    CHECK(shape(offBeat.staves.front()) == std::vector<std::string>{"r3", "3", "2", "r1"});
    CHECK(offBeat.staves.front().events[1].heads.front().tieStart);
    CHECK(offBeat.staves.front().events[2].heads.front().tieEnd);
    CHECK(offBeat.staves.front().events[1].stemUp == false); // C6: above the middle line

    // Sixteenths beamed by the beat; an empty bar is one whole-bar rest.
    const auto sixteenths = score::layout(sourceNotes({{0.0, 0.25, 60}, {0.25, 0.5, 62}, {0.5, 0.75, 64}, {0.75, 1.0, 65}}), 0.0, 8.0, map, inKey(0));
    const auto& st = sixteenths.staves.front();
    CHECK(shape(st) == std::vector<std::string>{"4", "4", "4", "4", "r2", "r1", "R0"});
    CHECK(st.events[0].beam == st.events[3].beam);
    CHECK(st.events[6].wholeBarRest);
    CHECK(st.events[6].length == 4.0);

    // The grid: the notation rounds (the notes are not changed).
    const auto loose = score::layout(sourceNotes({{0.07, 0.96, 60}}), 0.0, 4.0, map, inKey(0));
    CHECK(loose.staves.front().events.front().beat == 0.0);
    CHECK(loose.staves.front().events.front().type == 2);
    score::Options coarse = inKey(0);
    coarse.grid = 0.5;
    const auto eighthGrid = score::layout(sourceNotes({{0.3, 1.2, 60}}), 0.0, 4.0, map, coarse);
    CHECK(eighthGrid.staves.front().events[1].beat == 0.5);

    // Chords: one event, heads low to high; a note lasts until the next onset.
    const auto chord = score::layout(sourceNotes({{0.0, 2.0, 67}, {0.0, 2.0, 60}, {0.0, 2.0, 64}, {1.0, 2.0, 72}}), 0.0, 4.0, map, inKey(0));
    const auto& chordEvent = chord.staves.front().events.front();
    REQUIRE(chordEvent.heads.size() == 3);
    CHECK(chordEvent.heads[0].midi == 60);
    CHECK(chordEvent.heads[2].midi == 67);
    CHECK(chordEvent.type == 2); // cut at beat 1, where the C5 starts
}

TEST_CASE("Score layout: accidentals in the key and the bar, compound meters, signatures, clefs and the grand staff", "[score]")
{
    const time::TempoMap map;
    // F major: Bb in the key; B natural shows its natural, the Bb after it its flat again; Gb its
    // flat; the next bar starts afresh.
    const auto f = score::layout(sourceNotes({{0, 1, 70}, {1, 2, 71}, {2, 3, 70}, {3, 4, 66}, {4, 5, 71}, {5, 6, 70}}), 0.0, 8.0, map, inKey(-1));
    CHECK(f.keyFifths == -1);
    const auto& fe = f.staves.front().events;
    CHECK(fe[0].heads[0].accidental == score::Accidental::none);
    CHECK(fe[1].heads[0].accidental == score::Accidental::natural);
    CHECK(fe[2].heads[0].accidental == score::Accidental::flat);
    CHECK(fe[3].heads[0].accidental == score::Accidental::flat);
    CHECK(fe[3].heads[0].spelling.step == 4); // G flat, not F sharp
    CHECK(fe[4].heads[0].accidental == score::Accidental::natural);
    CHECK(fe[5].heads[0].accidental == score::Accidental::flat);
    // A tied note does not repeat its accidental.
    const auto tied = score::layout(sourceNotes({{3, 5, 61}, {5, 6, 61}}), 0.0, 8.0, map, inKey(0));
    const auto& te = tied.staves.front().events;
    CHECK(te[1].heads[0].accidental == score::Accidental::sharp);
    CHECK(te[2].heads[0].tieEnd);
    CHECK(te[2].heads[0].accidental == score::Accidental::none);
    CHECK(te[3].heads[0].accidental == score::Accidental::none); // (still sharp in that bar)

    // 6/8: eighths beamed in threes, a dotted quarter, a dotted quarter rest.
    const time::TempoMap sixEight({{0.0, 120.0}}, {{0, {6, 8}}});
    const auto c = score::layout(sourceNotes({{0, 0.5, 60}, {0.5, 1, 62}, {1, 1.5, 64}, {1.5, 2, 65}, {2, 2.5, 67}, {2.5, 3, 69}, {3, 4.5, 72}}),
                                 0.0, 6.0, sixEight, inKey(0));
    REQUIRE(c.bars.size() == 2);
    CHECK(c.bars[0].signature.denominator == 8);
    CHECK(c.bars[1].start == 3.0);
    const auto& ce = c.staves.front().events;
    CHECK(shape(c.staves.front()) == std::vector<std::string>{"3", "3", "3", "3", "3", "3", "2.", "r2."});
    CHECK(ce[0].beam == ce[2].beam);
    CHECK(ce[3].beam == ce[5].beam);
    CHECK(ce[2].beam != ce[3].beam);

    // A signature change: bars follow the map, the change is shown.
    const time::TempoMap change({{0.0, 120.0}}, {{0, {4, 4}}, {1, {3, 4}}});
    const auto ch = score::layout({}, 0.0, 10.0, change, inKey(0));
    REQUIRE(ch.bars.size() == 3);
    CHECK(ch.bars[1].showSignature);
    CHECK(ch.bars[1].signature.numerator == 3);
    CHECK_FALSE(ch.bars[2].showSignature);
    CHECK(ch.bars[2].start == 7.0);
    CHECK(ch.staves.front().events[1].wholeBarRest);
    CHECK(ch.staves.front().events[1].length == 3.0);

    // Clefs: treble for high notes, bass for low ones, a grand staff for both (split at middle C).
    CHECK(score::layout(sourceNotes({{0, 1, 40}, {1, 2, 52}}), 0, 4, map, {}).staves.front().clef == score::Clef::bass);
    const auto grand = score::layout(sourceNotes({{0, 1, 48}, {0, 1, 72}, {1, 2, 60}}), 0, 4, map, {});
    REQUIRE(grand.staves.size() == 2);
    CHECK(grand.staves[0].clef == score::Clef::treble);
    CHECK(grand.staves[1].clef == score::Clef::bass);
    CHECK(grand.staves[0].events[0].heads[0].midi == 72);
    CHECK(grand.staves[0].events[1].heads[0].midi == 60);
    CHECK(grand.staves[1].events[0].heads[0].midi == 48);
    CHECK(grand.staves[1].events[1].rest);
    CHECK(score::layout(sourceNotes({{0, 1, 48}}), 0, 4, map, inKey(0, score::ClefMode::treble)).staves.front().clef == score::Clef::treble);
    CHECK(score::layout(sourceNotes({{0, 1, 72}}), 0, 4, map, inKey(0, score::ClefMode::grand)).staves.size() == 2);
}

TEST_CASE("MusicXML export: key, time, clefs, pitches, durations, dots, ties, beams, rests, two staves", "[score][musicxml]")
{
    const time::TempoMap map;
    const auto s = score::layout(sourceNotes({{0.0, 1.0, 60}, {1.0, 2.5, 66}, {2.5, 3.0, 67}, {3.0, 3.5, 69}, {3.5, 4.5, 71}}), 0.0, 8.0, map, inKey(1));
    const auto text = score::toMusicXml(s, "Theme", "Flute");
    CHECK(text.startsWith("<?xml version=\"1.0\" encoding=\"UTF-8\""));
    CHECK(text.contains("<!DOCTYPE score-partwise PUBLIC \"-//Recordare//DTD MusicXML 4.0 Partwise//EN\""));
    const auto xml = parse(text);
    CHECK(xml->getTagName() == "score-partwise");
    CHECK(xml->getStringAttribute("version") == "4.0");
    CHECK(xml->getChildByName("work")->getChildElementAllSubText("work-title", {}) == "Theme");
    CHECK(xml->getChildByName("part-list")->getChildByName("score-part")->getChildElementAllSubText("part-name", {}) == "Flute");
    const auto* part = xml->getChildByName("part");
    REQUIRE(part != nullptr);
    std::vector<juce::XmlElement*> measures;
    for (auto* m : part->getChildWithTagNameIterator("measure"))
        measures.push_back(m);
    REQUIRE(measures.size() == 2);
    CHECK(measures[0]->getStringAttribute("number") == "1");
    const auto* attributes = measures[0]->getChildByName("attributes");
    REQUIRE(attributes != nullptr);
    CHECK(attributes->getChildElementAllSubText("divisions", {}) == "16");
    CHECK(attributes->getChildByName("key")->getChildElementAllSubText("fifths", {}) == "1");
    CHECK(attributes->getChildByName("time")->getChildElementAllSubText("beats", {}) == "4");
    CHECK(attributes->getChildByName("time")->getChildElementAllSubText("beat-type", {}) == "4");
    CHECK(attributes->getChildByName("clef")->getChildElementAllSubText("sign", {}) == "G");

    std::vector<juce::XmlElement*> notes;
    for (auto* n : measures[0]->getChildWithTagNameIterator("note"))
        notes.push_back(n);
    REQUIRE(notes.size() == 5);
    CHECK(notes[0]->getChildByName("pitch")->getChildElementAllSubText("step", {}) == "C");
    CHECK(notes[0]->getChildByName("pitch")->getChildElementAllSubText("octave", {}) == "4");
    CHECK(notes[0]->getChildElementAllSubText("duration", {}) == "16");
    CHECK(notes[0]->getChildElementAllSubText("type", {}) == "quarter");
    // F# in G: alter 1, no accidental shown; dotted.
    CHECK(notes[1]->getChildByName("pitch")->getChildElementAllSubText("step", {}) == "F");
    CHECK(notes[1]->getChildByName("pitch")->getChildElementAllSubText("alter", {}) == "1");
    CHECK(notes[1]->getChildByName("accidental") == nullptr);
    CHECK(notes[1]->getChildByName("dot") != nullptr);
    CHECK(notes[1]->getChildElementAllSubText("duration", {}) == "24");
    // The beamed pair, and the tie over the bar line.
    CHECK(notes[3]->getChildElementAllSubText("beam", {}) == "begin");
    CHECK(notes[4]->getChildElementAllSubText("beam", {}) == "end");
    CHECK(notes[4]->getChildByName("tie")->getStringAttribute("type") == "start");
    CHECK(notes[4]->getChildByName("notations")->getChildByName("tied")->getStringAttribute("type") == "start");
    const auto* second = measures[1]->getChildByName("note");
    CHECK(second->getChildByName("tie")->getStringAttribute("type") == "stop");
    int rests = 0;
    for (auto* n : measures[1]->getChildWithTagNameIterator("note"))
        rests += n->getChildByName("rest") != nullptr ? 1 : 0;
    CHECK(rests == 3);
    CHECK(measures[1]->getChildByName("barline")->getChildElementAllSubText("bar-style", {}) == "light-heavy");
    // Durations add up to each bar.
    for (auto* m : measures)
    {
        int total = 0;
        for (auto* n : m->getChildWithTagNameIterator("note"))
            if (n->getChildByName("chord") == nullptr)
                total += n->getChildElementAllSubText("duration", {}).getIntValue();
        CHECK(total == 64);
    }

    // A grand staff: two staves, the second after a backup, each note on its staff; measure rests.
    const auto g = score::layout(sourceNotes({{0, 4, 48}, {0, 4, 72}}), 0.0, 8.0, map, inKey(0));
    const auto gx = parse(score::toMusicXml(g, {}, {}));
    const auto* gm = gx->getChildByName("part")->getChildByName("measure");
    CHECK(gm->getChildByName("attributes")->getChildElementAllSubText("staves", {}) == "2");
    CHECK(gm->getChildByName("backup")->getChildElementAllSubText("duration", {}) == "64");
    std::vector<juce::String> staffNumbers;
    for (auto* n : gm->getChildWithTagNameIterator("note"))
        staffNumbers.push_back(n->getChildElementAllSubText("staff", {}));
    CHECK(staffNumbers == std::vector<juce::String>{"1", "2"});
    const auto* restBar = gx->getChildByName("part")->getChildElement(1)->getChildByName("note");
    CHECK(restBar->getChildByName("rest")->getStringAttribute("measure") == "yes");

    const auto file = juce::File::createTempFile(".musicxml");
    CHECK(score::writeMusicXml(s, file, "Theme", "Flute"));
    CHECK(file.loadFileAsString() == text);
    file.deleteFile();
}

TEST_CASE("The score editor: notes put in on the staff, selected, dragged, retyped, transposed and deleted", "[score][ui]")
{
    struct MessageThread
    {
        MessageThread() { juce::MessageManager::getInstance(); }
    } messageThread;
    engine::AudioEngine engine;
    engine::AudioDeviceHost host(engine);
    engine::EngineController controller(engine, host);
    session::Project project([&controller](std::unique_ptr<engine::RenderGraph> graph) { controller.publishGraph(std::move(graph)); },
                             [](const juce::ValueTree& d, double, int, juce::String&)
                             { return devices::createBuiltinDevice(session::device::getType(d)); });
    auto& session = project.getSession();
    auto& undo = session.getUndoManager();
    auto track = session.addTrack(session::TrackKind::instrument, "Piano");
    auto clip = session.addMidiClip(track, 0.0, 4.0, "Theme"); // two bars at 120

    ui::ScoreView view(session);
    view.setSize(1400, 320);
    std::vector<juce::MidiMessage> previews;
    view.onPreview = [&previews](const juce::MidiMessage& m) { previews.push_back(m); };
    view.setClip(clip);

    const auto event = [&view](juce::Point<float> p, juce::Point<float> down, juce::ModifierKeys extra = {})
    {
        const auto now = juce::Time::getCurrentTime();
        const auto mods = juce::ModifierKeys((extra.isPopupMenu() ? 0 : juce::ModifierKeys::leftButtonModifier) | extra.getRawFlags());
        return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), p, mods, juce::MouseInputSource::defaultPressure,
                                juce::MouseInputSource::defaultOrientation, juce::MouseInputSource::defaultRotation,
                                juce::MouseInputSource::defaultTiltX, juce::MouseInputSource::defaultTiltY, &view, &view, now, down, now, 1,
                                p != down);
    };
    const auto click = [&](juce::Point<float> p, juce::ModifierKeys mods = {})
    {
        view.mouseDown(event(p, p, mods));
        view.mouseUp(event(p, p, mods));
    };
    const auto notes = [&clip]
    {
        std::vector<juce::ValueTree> out;
        for (int i = 0; i < clip.getNumChildren(); ++i)
            if (clip.getChild(i).hasType(session::ids::note))
                out.push_back(clip.getChild(i));
        return out;
    };
    const auto paint = [&view]
    {
        juce::Image image(juce::Image::RGB, view.getWidth(), view.getHeight(), true);
        juce::Graphics g(image);
        view.paintEntireComponent(g, false);
    };

    // An empty clip: two bars of whole-bar rests in treble clef.
    const auto& empty = view.getScore();
    REQUIRE(empty.bars.size() == 2);
    REQUIRE(empty.staves.size() == 1);
    CHECK(empty.staves[0].events[0].wholeBarRest);
    paint();

    // The Note tool puts the chosen value on the grid at the staff position's pitch, in the key.
    CHECK(view.getTool() == ui::ScoreView::Tool::note);
    CHECK(view.getNoteType() == 2);
    click({view.beatToX(1.0), view.diatonicToY(0, 32)}); // G4 on beat 2
    REQUIRE(notes().size() == 1);
    auto g4 = notes()[0];
    CHECK(session::note::getNote(g4) == 67);
    CHECK_THAT(session::note::getStart(g4), WithinAbs(1.0, 1e-9));
    CHECK_THAT(session::note::getLength(g4), WithinAbs(1.0, 1e-9));
    CHECK(undo.getUndoDescription() == "Add Note");
    CHECK(view.getSelection() == std::vector<juce::ValueTree>{g4});
    REQUIRE(previews.size() == 2);
    CHECK(previews[0].isNoteOn());
    CHECK(previews[0].getNoteNumber() == 67);
    CHECK(previews[1].isNoteOff());

    // Shift: a semitone up (F#5); Alt: down (Eb4); an eighth, dotted.
    view.setNoteValue(3, true);
    click({view.beatToX(2.0), view.diatonicToY(0, 38)}, juce::ModifierKeys::shiftModifier);
    view.setNoteValue(2, false);
    click({view.beatToX(3.0) + 3.0f, view.diatonicToY(0, 30) + 1.5f}, juce::ModifierKeys::altModifier);
    REQUIRE(notes().size() == 3);
    CHECK(session::note::getNote(notes()[1]) == 78);
    CHECK_THAT(session::note::getLength(notes()[1]), WithinAbs(0.75, 1e-9));
    CHECK(session::note::getNote(notes()[2]) == 63);
    CHECK_THAT(session::note::getStart(notes()[2]), WithinAbs(3.0, 1e-9));
    const auto& written = view.getScore();
    CHECK(written.keyFifths == 0);
    paint();

    // Heads are where the view says; the Select tool picks them.
    const auto head = view.headPosition(g4);
    CHECK_THAT(static_cast<double>(head.y), WithinAbs(static_cast<double>(view.diatonicToY(0, 32)), 0.01));
    CHECK_THAT(static_cast<double>(head.x), WithinAbs(static_cast<double>(view.beatToX(1.0)), 0.01));
    view.setTool(ui::ScoreView::Tool::select);
    click({200.0f, view.diatonicToY(0, 20)}); // nothing there
    CHECK(view.getSelection().empty());
    click(head);
    CHECK(view.getSelection() == std::vector<juce::ValueTree>{g4});

    // Dragging: a beat later on the grid, two steps up the staff in the key (G4 -> B4); one step.
    const auto target = juce::Point<float>(view.beatToX(2.0), head.y - view.lineSpacing());
    view.mouseDown(event(head, head));
    view.mouseDrag(event({head.x + 5.0f, head.y - 2.0f}, head));
    view.mouseDrag(event(target, head));
    view.mouseUp(event(target, head));
    CHECK(session::note::getNote(g4) == 71);
    CHECK_THAT(session::note::getStart(g4), WithinAbs(2.0, 1e-9));
    CHECK(undo.getUndoDescription() == "Move Note");
    undo.undo();
    CHECK(session::note::getNote(g4) == 67);
    CHECK_THAT(session::note::getStart(g4), WithinAbs(1.0, 1e-9));
    view.setSelection({g4});

    // Keys: 4 makes it an eighth, "." dots it, Up a semitone, Ctrl+Down an octave, Right a grid step.
    CHECK(view.keyPressed(juce::KeyPress('4')));
    CHECK_THAT(session::note::getLength(g4), WithinAbs(0.5, 1e-9));
    CHECK(view.getNoteType() == 3);
    CHECK(view.keyPressed(juce::KeyPress('.')));
    CHECK_THAT(session::note::getLength(g4), WithinAbs(0.75, 1e-9));
    CHECK(view.isDotted());
    CHECK(view.keyPressed(juce::KeyPress(juce::KeyPress::upKey)));
    CHECK(session::note::getNote(g4) == 68);
    CHECK(view.keyPressed(juce::KeyPress(juce::KeyPress::downKey, juce::ModifierKeys::commandModifier, 0)));
    CHECK(session::note::getNote(g4) == 56);
    CHECK(view.keyPressed(juce::KeyPress(juce::KeyPress::downKey, juce::ModifierKeys::commandModifier, 0)));
    CHECK(session::note::getNote(g4) == 44);
    CHECK(view.keyPressed(juce::KeyPress(juce::KeyPress::rightKey)));
    CHECK_THAT(session::note::getStart(g4), WithinAbs(1.25, 1e-9));
    CHECK(undo.getUndoDescription() == "Move Note");
    CHECK(view.keyPressed(juce::KeyPress('n')));
    CHECK(view.getTool() == ui::ScoreView::Tool::note);
    CHECK(view.keyPressed(juce::KeyPress('s')));
    CHECK(view.getTool() == ui::ScoreView::Tool::select);
    CHECK_FALSE(view.keyPressed(juce::KeyPress(juce::KeyPress::spaceKey))); // (the transport's)

    // The low note made a grand staff (automatic clef); a lasso over everything selects it all.
    CHECK(view.getScore().staves.size() == 2);
    paint();
    const float right = view.beatToX(7.9);
    view.mouseDown(event({view.beatToX(0.0) - 20.0f, 34.0f}, {view.beatToX(0.0) - 20.0f, 34.0f}));
    view.mouseDrag(event({right, static_cast<float>(view.getHeight() - 14)}, {view.beatToX(0.0) - 20.0f, 34.0f}));
    view.mouseUp(event({right, static_cast<float>(view.getHeight() - 14)}, {view.beatToX(0.0) - 20.0f, 34.0f}));
    CHECK(view.getSelection().size() == 3);

    // Delete removes the selection (one step); right-click deletes a note under the mouse.
    CHECK(view.keyPressed(juce::KeyPress(juce::KeyPress::deleteKey)));
    CHECK(notes().empty());
    CHECK(undo.getUndoDescription() == "Delete Notes");
    undo.undo();
    REQUIRE(notes().size() == 3);
    const auto f5 = notes()[1];
    view.mouseDown(event(view.headPosition(f5), view.headPosition(f5), juce::ModifierKeys::rightButtonModifier));
    CHECK(notes().size() == 2);
    CHECK(undo.getUndoDescription() == "Delete Note");

    // The clip's settings: clef, key, grid (undoable, saved on the clip).
    view.setClefMode(score::ClefMode::bass);
    CHECK(clip.getProperty(session::ids::scoreClef).toString() == "bass");
    CHECK(view.getScore().staves.size() == 1);
    CHECK(view.getScore().staves[0].clef == score::Clef::bass);
    CHECK(undo.getUndoDescription() == "Score Clef");
    undo.undo();
    CHECK(view.getOptions().clef == score::ClefMode::automatic);
    view.setKey(-3);
    CHECK(view.getScore().keyFifths == -3);
    CHECK(static_cast<int>(clip.getProperty(session::ids::scoreKey)) == -3);
    view.setKey(std::nullopt);
    CHECK_FALSE(clip.hasProperty(session::ids::scoreKey));
    view.setGrid(0.5);
    CHECK(view.getOptions().grid == 0.5);
    // Note-tool clicks land on the eighth grid.
    view.setTool(ui::ScoreView::Tool::note);
    view.setNoteValue(2, false);
    click({view.beatToX(5.3), view.diatonicToY(0, 34)});
    CHECK_THAT(session::note::getStart(notes().back()), WithinAbs(5.5, 1e-9));

    // The tempo map's bars: a 3/4 bar from bar 2 on.
    session.addSignatureChange(2, 3, 4);
    const auto& retimed = view.getScore();
    REQUIRE(retimed.bars.size() >= 2);
    CHECK(retimed.bars[1].signature.numerator == 3);
    CHECK(retimed.bars[1].showSignature);
    paint();

    // MusicXML: named after the clip and its track.
    const auto file = juce::File::createTempFile(".musicxml");
    REQUIRE(view.exportMusicXml(file));
    const auto xml = parse(file.loadFileAsString());
    CHECK(xml->getChildByName("work")->getChildElementAllSubText("work-title", {}) == "Theme");
    CHECK(xml->getChildByName("part-list")->getChildByName("score-part")->getChildElementAllSubText("part-name", {}) == "Piano");
    file.deleteFile();
}

TEST_CASE("The clip editor's SCORE view shows the edited MIDI clip", "[score][ui]")
{
    struct MessageThread
    {
        MessageThread() { juce::MessageManager::getInstance(); }
    } messageThread;
    engine::AudioEngine engine;
    engine::AudioDeviceHost host(engine);
    engine::EngineController controller(engine, host);
    session::Project project([&controller](std::unique_ptr<engine::RenderGraph> graph) { controller.publishGraph(std::move(graph)); },
                             [](const juce::ValueTree& d, double, int, juce::String&)
                             { return devices::createBuiltinDevice(session::device::getType(d)); });
    auto& session = project.getSession();
    auto track = session.addTrack(session::TrackKind::instrument, "Keys");
    auto clip = session.addMidiClip(track, 2.0, 4.0, "Chords");
    session.addNote(clip, 0.0, 1.0, 60);
    session.addNote(clip, 0.0, 1.0, 64);
    ui::EditorState editor;
    ui::ClipEditor clipEditor(project, editor);
    clipEditor.setSize(1400, 360);
    editor.setEditedClip(session::clip::getId(clip));
    CHECK(clipEditor.getPianoRoll().isVisible());
    CHECK_FALSE(clipEditor.getScoreView().isVisible());
    clipEditor.showMidiView(ui::ClipEditor::MidiView::score);
    CHECK(clipEditor.getScoreView().isVisible());
    CHECK_FALSE(clipEditor.getPianoRoll().isVisible());
    auto& view = clipEditor.getScoreView();
    CHECK(view.getClip() == clip);
    // The clip starts at second 2 (beat 4, bar 2): its notes are in the timeline's bar 2.
    CHECK_THAT(view.contentOrigin(), WithinAbs(4.0, 1e-9));
    const auto& s = view.getScore();
    REQUIRE_FALSE(s.bars.empty());
    CHECK(s.bars.front().number == 1);
    CHECK(s.staves[0].events.front().heads.size() == 2);
    juce::Image image(juce::Image::RGB, 1400, 360, true);
    {
        juce::Graphics g(image);
        clipEditor.paintEntireComponent(g, false);
    }

    // DOJO_SCORE_SNAPSHOT=<file.png>: a melody with chords, accidentals, beams, ties and rests on a
    // grand staff, drawn for a look.
    if (const auto path = juce::SystemStats::getEnvironmentVariable("DOJO_SCORE_SNAPSHOT", {}); path.isNotEmpty())
    {
        const std::tuple<double, double, int> melody[] = {
            {0.0, 0.5, 72}, {0.5, 1.0, 74}, {1.0, 1.5, 76}, {1.5, 2.0, 77}, {2.0, 3.5, 79}, {3.5, 4.0, 78},
            {4.0, 4.25, 79}, {4.25, 4.5, 81}, {4.5, 4.75, 79}, {4.75, 5.0, 77}, {5.0, 6.0, 76}, {6.0, 7.5, 70}, {7.5, 9.0, 72},
            {12.0, 16.0, 84}};
        for (const auto& [start, end, midi] : melody)
            session.addNote(clip, start, end - start, midi);
        for (const int midi : {48, 52, 55})
            session.addNote(clip, 0.0, 2.0, midi);
        for (const int midi : {53, 57, 60})
            session.addNote(clip, 2.0, 2.0, midi);
        session.addNote(clip, 4.0, 3.0, 43);
        session.addNote(clip, 8.0, 0.75, 41);
        session.addNote(clip, 8.75, 0.25, 42);
        session::clip::setLength(clip, 8.0, &session.getUndoManager());
        clipEditor.setSize(1600, 380);
        view.setPlayhead(3.1);
        juce::Image shot(juce::Image::RGB, 1600, 380, true);
        {
            juce::Graphics g(shot);
            clipEditor.paintEntireComponent(g, false);
        }
        const juce::File file(path);
        file.deleteFile();
        juce::FileOutputStream out(file);
        juce::PNGImageFormat().writeImageToStream(shot, out);
    }
    clipEditor.showMidiView(ui::ClipEditor::MidiView::notes);
    CHECK(clipEditor.getPianoRoll().isVisible());
}
