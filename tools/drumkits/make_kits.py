#!/usr/bin/env python3
"""
Builds The Dojo's built-in drum kits from the Linn and Roland sample sets.

    python3 tools/drumkits/make_kits.py <folder with the original sample folders> [<another> ...]

The folders hold the sets as they came ("Linn LM-1/", "Linn LM-2/", "Linn Linndrum/",
"Linn AdrenaLinn1/", "Linn 9000/", "Roland MC-202/", "Roland MC-303/", "Roland MC-808/",
"Roland R8 MKII/", "Roland R-70/": folders of WAV files, possibly one level down, the R-70's with
sub-folders). Writes:

  resources/kits/<Machine>.zip              the Linn machines' samples, renamed readably (the WAV
                                            files' bytes as they came), compiled into the app
  resources/drum-machines/<Machine>.zip     the Roland machines' samples, likewise (WAV, as they
                                            came): too big to compile in (88 MB), so they are
                                            copied beside the application ("Drum Machines")
  resources/presets/kits/<Kit>.dojopreset   the kits: Drum Rack presets with every pad
                                            arranged (sample, level, pan, choke) and a demo beat

Besides the playable kits (below and tools/drumkits/roland.py), every machine gets selector kits
for the samples no playable kit uses, one kind of sound each ("R-70 Kicks 2"), so that every sample
is on a pad. Their demo plays each pad in turn.

and checks that every sample of every machine is on a pad of at least one kit. A kit's pads hold
"kit:<Machine>/<Sample>.wav" references; The Dojo unpacks a machine's samples into its library
folder the first time one of its kits is used (devices/DrumKits.h).

Pad layout (Drum Rack pads 1-16 on notes C1-D#2, as General MIDI where it can be):
   1 Kick       2 Rim/Side Stick  3 Snare      4 Clap
   5 Snare 2    6 Low Tom         7 Closed Hat 8 Mid Tom
   9 Hat 2      10 High Tom       11 Open Hat  12 Percussion (low)
   13 Percussion (high) 14 Crash  15 Percussion 16 Ride
Hats on pads 7, 9 and 11 choke each other (choke group 1). Percussion and the extra kits of a
machine (so that every sample has a pad) use the same places where they can.
"""
import os
import re
import sys
import zipfile
from xml.sax.saxutils import quoteattr

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import roland  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

# --- The machines: their source folder and every sample's readable name --------------------------
MACHINES = {
    "Linn LM-1": ("Linn LM-1", {
        "Kick1": "Kick 1", "Kick2": "Kick 2", "Sst": "Side Stick", "SnareDrum1": "Snare 1",
        "SnareDrum2": "Snare 2", "Clap": "Clap", "Lotom": "Tom Low", "Midtom": "Tom Mid",
        "Hitom": "Tom High", "Chh": "Hat Closed", "Ohh": "Hat Open", "Cabasa": "Cabasa",
        "Tamb": "Tambourine", "Conga": "Conga High", "Congal": "Conga Low", "Cowb": "Cowbell",
        "Crash": "Crash", "Ride": "Ride", "Linn_Bp": "Beep",
    }),
    "Linn LM-2": ("Linn LM-2", {
        "Kick Hard": "Kick Hard", "Kick Soft": "Kick Soft", "Sidestick": "Side Stick",
        "Snare Hard": "Snare Hard", "Snare Mid": "Snare Mid", "Snare Soft": "Snare Soft",
        "Clap": "Clap", "Lo Tom": "Tom Low", "Mid Tom": "Tom Mid", "High Tom": "Tom High",
        "Cl Hat": "Hat Closed", "Med Hat": "Hat Medium", "Open Hat": "Hat Open",
        "Low Conga": "Conga Low", "Hi Conga": "Conga High", "Cowbell": "Cowbell", "Crash": "Crash",
        "Ride Hard": "Ride Hard", "Ride Soft": "Ride Soft", "Cabasa Hard": "Cabasa Hard",
        "Cabasa Soft": "Cabasa Soft", "Tambourine Hard": "Tambourine Hard",
        "Tambourine Soft": "Tambourine Soft",
    }),
    "LinnDrum": ("Linn Linndrum", {
        "Kick": "Kick", "Kickme": "Kick 2", "Sst": "Side Stick", "Ssth": "Side Stick High",
        "Sstl": "Side Stick Low", "SnareDrum": "Snare", "SnareDrumh": "Snare High",
        "SnareDruml": "Snare Low", "Clap": "Clap", "Chh": "Hat Closed", "Chhs": "Hat Closed Short",
        "Chhl": "Hat Closed Long", "Ohh": "Hat Open", "Tomll": "Tom Lowest", "Toml": "Tom Low",
        "Tom": "Tom Mid", "Tomh": "Tom High", "Tomhh": "Tom Highest", "Congalll": "Conga Lowest",
        "Congall": "Conga Lower", "Congal": "Conga Low", "Conga": "Conga Mid", "Congah": "Conga High",
        "Congahh": "Conga Highest", "Cabasa": "Cabasa", "Tamb": "Tambourine", "Cowb": "Cowbell",
        "Crash": "Crash", "Ride": "Ride", "Reallinn": "Real Linn",
    }),
    "AdrenaLinn": ("Linn AdrenaLinn1", dict(
        [(f"BassDrum_{i}", f"Kick {i}") for i in range(1, 10)]
        + [(f"SnareDrum_{i}", f"Snare {i}") for i in range(1, 9)]
        + [(f"HH_{i}", f"Hat {i}") for i in range(1, 7)]
        + [(f"Perc{i}", f"Perc {i}") for i in range(1, 13)]
        + [("HHop", "Hat Open"), ("Ride", "Ride"), ("Rim", "Rim"), ("Tomhi", "Tom High"), ("Tomlo", "Tom Low")]
    )),
    "Linn 9000": ("Linn 9000", {
        "Kick": "Kick 1", "Kick 2": "Kick 2", "Kick_1": "Kick 3", "BassDrumrum1": "Kick 4",
        "BassDrumrum1kai": "Kick 5", "BassDrumrum2": "Kick 6", "BassDrumrum3": "Kick 7",
        "BassDrumrum4": "Kick 8",
        "Snare": "Snare 1", "Snare 2": "Snare 2", "Snr": "Snare 3", "Snare1": "Snare 4",
        "Snare2": "Snare 5", "Snare3": "Snare 6", "Snare4": "Snare 7", "Snare5": "Snare 8",
        "Snare6": "Snare 9",
        "Rim": "Rim 1", "Rim 2": "Rim 2", "Rim1": "Rim 3", "Rim2": "Rim 4", "Stick": "Side Stick",
        "Clap": "Clap 1", "Clap 1": "Clap 2", "Clap 2": "Clap 3",
        "Clhh": "Hat Closed 1", "Clhh 2": "Hat Closed 2", "Clhh_1": "Hat Closed 3",
        "Hhclose1": "Hat Closed 4", "Hhclose2": "Hat Closed 5",
        "Ophh": "Hat Open 1", "Ophh 2": "Hat Open 2", "Ophh 3": "Hat Open 3", "Ophh_1": "Hat Open 4",
        "Oph2": "Hat Open 5", "Hhopen1": "Hat Open 6", "Hhopen2": "Hat Open 7",
        # (toms ordered by pitch: 55, 78, 116, 140, 160 Hz; 51, 79, 105, 135, 160 Hz)
        "Tom 4": "Tom Floor", "Tom 3": "Tom Low", "Tom": "Tom Mid", "Tom 2": "Tom High",
        "Tom 1": "Tom Higher", "Tom4": "Tom Floor 2", "Tom5": "Tom Low 2", "Tom3": "Tom Mid 2",
        "Tom2": "Tom High 2", "Tom1": "Tom Higher 2", "Etom": "Electronic Tom",
        "Conga 1": "Conga 1", "Lo-Cong": "Conga Low", "Hi-Cong": "Conga High", "Conga 2": "Conga 2",
        "Lowconga": "Conga Low 2", "Midconga": "Conga Mid", "Hiconga": "Conga High 2",
        "Cowbell": "Cowbell 1", "Cowbell1": "Cowbell 2", "Cowbell2": "Cowbell 3",
        "Cabasa": "Cabasa 1", "Cabasa2": "Cabasa 2",
        "Tamb": "Tambourine 1", "Tamb 2": "Tambourine 2", "Tamb1": "Tambourine 3", "Tamb2": "Tambourine 4",
        "Crash": "Crash 1", "Crash 2": "Crash 2", "Crash 3": "Crash 3", "Crash1": "Crash 4",
        "Crash2": "Crash 5", "Crash3": "Crash 6", "Crsh": "Crash 7",
        "Ride": "Ride 1", "Ride1": "Ride 2", "Ride2": "Ride 3", "Ride3": "Ride 4",
        "Pec1": "Perc 1", "Pec2": "Perc 2", "Pec3": "Perc 3", "Ping": "Ping",
    }),
}

# --- Demo beats: two bars of sixteenths per lane, by pad (1-16). X loud, x normal, o soft -------------
G = {}
G["pop"] = {  # straight eighties pop
    1: "X.......X.x.....X.......X.....x.",
    3: "....X.......X.......X.......X...",
    4: "............X...............X...",
    7: "x.x.x.x.x.x.x.x.x.x.x.x.x.x.....",
    11: "..............................x.",
    9: "..o...o...o...o...o...o...o.....",
    14: "X...............................",
}
G["ballad"] = {  # slow, sidestick and ride
    1: "X.........x.....X.......x.......",
    2: "....X.......X.......X.......X...",
    16: "x...x...x...x...x...x...x...x...",
    7: "..o...o...o...o...o...o...o...o.",
    6: "..........................x.o.oo",
    10: "........................x.x.....",
}
G["funk"] = {  # syncopated sixteenths
    1: "X..x..x...X.....X..x....x.x.....",
    3: "....X..o.o..X..o....X..o.o..X...",
    7: "xoxoxoxoxoxoxoX.xoxoxoxoxoxoxoX.",
    11: "..............x...............x.",
    4: "............X...............X...",
    15: "x.......x.......x.......x...x...",
}
G["electro"] = {  # four on the floor
    1: "X...X...X...X...X...X...X...X...",
    4: "....X.......X.......X.......X...",
    7: "..x...x...x...x...x...x...x...x.",
    9: "x.o.x.o.x.o.x.o.x.o.x.o.x.o.x.o.",
    11: "..x...x...x...x...x...x...x...x.",
    12: "......x.......x.......x...x.x...",
    14: "X...............................",
}
G["rock"] = {  # driving eighths with a tom fill
    1: "X.....X.X.......X.....X.X.......",
    3: "....X.......X.......X.......",
    7: "x.x.x.x.x.x.x.x.x.x.x.x.",
    10: "............................X.x.",
    8: ".............................xx.",
    6: "..............................xX",
    14: "X...............................",
    16: "........................x.x.x.x.",
}
G["shuffle"] = {  # swung, rim and hats
    1: "X.....x...X.......x.X...x.......",
    2: "....X.......X.......X.......X...",
    3: "............X...............X...",
    7: "x.xxx.xxx.xxx.xxx.xxx.xxx.xxx.xx",
    11: "..............x...............x.",
}
G["latin"] = {  # congas, cowbell, shaker
    1: "X.......X.......X.......X.......",
    12: "..x..x......x.....x..x......x.x.",
    13: "x...x..x.x..x...x...x..x.x..x...",
    15: "X...x.x.X.x.x...X...x.x.X.x.x...",
    9: "xoxoxoxoxoxoxoxoxoxoxoxoxoxoxoxo",
    2: "....X.......X.......X.......X...",
    4: "..............................X.",
}
G["percussion"] = {  # the percussion kits' own pads
    1: "X.......X.......X.......X.......",
    6: "x..x....x..x....x..x....x..x.xx.",
    8: "...x..x....x..x....x..x....x..x.",
    10: "x.....x...x.....x.....x...x...x.",
    12: "..x.......x...x...x.......x...x.",
    13: "......x.......x.......x.....x.x.",
    5: "x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.",
    9: "....x.......x.......x.......x...",
    15: "x..x..x...x.x...x..x..x...x.x...",
    16: "........................X.......",
}
G["selector"] = {  # kits of variants: each kick and snare heard in turn
    1: "X.......x.......................",
    2: "..x.............................",
    3: "....X.......................X...",
    5: "............X...................",
    6: "................X.......x.......",
    8: "..................x.............",
    10: "..........................x.....",
    12: "....................X...........",
    13: "............................x...",
    7: "x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.",
    11: "..............x...............x.",
}

G["bassline"] = {  # the MC-202: hits on the beat, the bass notes in sixteenths
    1: "X...X...X...X...X...X...X...X...",
    2: "..x...x...x...x...x...x...x...x.",
    3: "....X.......X.......X.......X...",
    9: "X..x......x.....X..x............",
    10: "......x.......x.......x.........",
    11: "....................x..x......x.",
    12: "............x.................x.",
    13: "...............................x",
}
G["breakbeat"] = {  # fast broken beat for the jungle kit
    1: "X.........X.....X.....X...X.....",
    3: "....X.......X.o.....X..o....X...",
    12: "..o...o..o.........o.....o..o.o.",
    7: "x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.",
    11: "..............x...............x.",
    14: "X...............................",
}

# --- The kits: name, machine, tempo, swing, groove, pads (1-16 -> sample), description ----------
STD_HATS = (7, 9, 11)
KITS = [
    ("LM-1 Kit", "Linn LM-1", 108, 0.0, "pop",
     ["Kick 1", "Side Stick", "Snare 1", "Clap", "Snare 2", "Tom Low", "Hat Closed", "Tom Mid",
      "Cabasa", "Tom High", "Hat Open", "Conga Low", "Conga High", "Crash", "Cowbell", "Ride"],
     "The first sampling drum machine (1980): its punchy kick, crisp snare and clap, the classic eighties pop kit."),
    ("LM-1 Alt Kit", "Linn LM-1", 96, 0.18, "shuffle",
     ["Kick 2", "Side Stick", "Snare 2", "Clap", "Snare 1", "Tom Low", "Hat Closed", "Tom Mid",
      "Tambourine", "Tom High", "Hat Open", "Conga Low", "Conga High", "Crash", "Beep", "Ride"],
     "The LM-1's second kick and snare, tambourine and its beep, for a lighter, swung groove."),
    ("LM-2 Kit", "Linn LM-2", 116, 0.0, "rock",
     ["Kick Hard", "Side Stick", "Snare Hard", "Clap", "Snare Mid", "Tom Low", "Hat Closed", "Tom Mid",
      "Hat Medium", "Tom High", "Hat Open", "Conga Low", "Conga High", "Crash", "Cowbell", "Ride Hard"],
     "The LinnDrum's hard-hit sounds (LM-2): a bright, driving kit for rock and pop."),
    ("LM-2 Soft Kit", "Linn LM-2", 84, 0.0, "ballad",
     ["Kick Soft", "Side Stick", "Snare Soft", "Clap", "Snare Mid", "Tom Low", "Hat Closed", "Tom Mid",
      "Cabasa Soft", "Tom High", "Hat Open", "Cabasa Hard", "Tambourine Soft", "Crash", "Tambourine Hard", "Ride Soft"],
     "The LM-2's soft-hit sounds with cabasa and tambourine: gentle, for ballads."),
    ("LinnDrum Kit", "LinnDrum", 120, 0.0, "funk",
     ["Kick", "Side Stick", "Snare", "Clap", "Snare High", "Tom Low", "Hat Closed", "Tom Mid",
      "Hat Closed Short", "Tom High", "Hat Open", "Conga Low", "Conga High", "Crash", "Cowbell", "Ride"],
     "The LinnDrum as it came: the sound of countless eighties records."),
    ("LinnDrum Tuned Kit", "LinnDrum", 100, 0.0, "pop",
     ["Kick 2", "Side Stick Low", "Snare Low", "Clap", "Side Stick High", "Tom Lowest", "Hat Closed Long", "Tom Mid",
      "Cabasa", "Tom Highest", "Hat Open", "Conga Lower", "Conga Highest", "Crash", "Tambourine", "Ride"],
     "The LinnDrum with its sounds tuned down and up: a deeper snare, long hats and wide toms."),
    ("LinnDrum Percussion Kit", "LinnDrum", 104, 0.0, "percussion",
     ["Kick", "Side Stick", "Snare", "Clap", "Cabasa", "Conga Lowest", "Hat Closed", "Conga Lower",
      "Tambourine", "Conga Low", "Hat Open", "Conga Mid", "Conga High", "Conga Highest", "Cowbell", "Real Linn"],
     "Every conga tuning of the LinnDrum, with cabasa, tambourine and cowbell, for percussion parts."),
    ("AdrenaLinn Kit", "AdrenaLinn", 96, 0.0, "funk",
     ["Kick 1", "Rim", "Snare 1", "Perc 11", "Snare 2", "Tom Low", "Hat 1", "Perc 4",
      "Hat 2", "Tom High", "Hat Open", "Perc 5", "Perc 10", "Perc 8", "Perc 12", "Ride"],
     "Drum sounds of the AdrenaLinn guitar effects processor: tight and modern."),
    ("AdrenaLinn Kit 2", "AdrenaLinn", 124, 0.0, "electro",
     ["Kick 2", "Rim", "Snare 3", "Perc 6", "Snare 4", "Tom Low", "Hat 3", "Kick 3",
      "Hat 4", "Tom High", "Hat Open", "Kick 4", "Perc 7", "Perc 1", "Perc 3", "Ride"],
     "More AdrenaLinn kicks, snares and hats, for dance grooves."),
    ("AdrenaLinn Sub Kit", "AdrenaLinn", 88, 0.0, "selector",
     ["Kick 8", "Kick 6", "Snare 5", "Perc 9", "Snare 6", "Kick 7", "Hat 5", "Kick 5",
      "Hat 6", "Kick 9", "Hat Open", "Snare 7", "Snare 8", "Perc 2", "Perc 12", "Ride"],
     "The AdrenaLinn's sub-bass kicks and the rest of its kicks and snares, one per pad."),
    ("Linn 9000 Kit", "Linn 9000", 112, 0.0, "pop",
     ["Kick 1", "Side Stick", "Snare 1", "Clap 1", "Snare 2", "Tom Low", "Hat Closed 1", "Tom Mid",
      "Hat Closed 3", "Tom High", "Hat Open 1", "Conga Low", "Conga High", "Crash 1", "Cowbell 1", "Ride 1"],
     "The Linn 9000 (1984), sampling drum machine and sequencer: big, polished eighties drums."),
    ("Linn 9000 Kit 2", "Linn 9000", 118, 0.0, "rock",
     ["Kick 4", "Rim 3", "Snare 4", "Clap 3", "Snare 5", "Tom Low 2", "Hat Closed 4", "Tom Mid 2",
      "Hat Closed 5", "Tom High 2", "Hat Open 6", "Conga Low 2", "Conga High 2", "Crash 4", "Cowbell 2", "Ride 2"],
     "The Linn 9000's second set of sounds: shorter, punchier drums."),
    ("Linn 9000 Kit 3", "Linn 9000", 98, 0.16, "shuffle",
     ["Kick 2", "Rim 2", "Snare 3", "Clap 2", "Snare 6", "Tom Floor", "Hat Closed 2", "Tom Higher",
      "Tambourine 1", "Electronic Tom", "Hat Open 2", "Conga 1", "Conga 2", "Crash 2", "Cabasa 1", "Ride 3"],
     "Fat snare and floor tom, electronic tom and a swung feel."),
    ("Linn 9000 Kit 4", "Linn 9000", 104, 0.0, "latin",
     ["Kick 3", "Rim 4", "Snare 7", "Clap 3", "Snare 8", "Tom Floor 2", "Hat Closed 4", "Tom Higher 2",
      "Cabasa 2", "Tom Mid 2", "Hat Open 3", "Conga Mid", "Conga High 2", "Crash 3", "Cowbell 3", "Ride 4"],
     "Congas, cabasa and cowbell with the 9000's drums: latin and percussive grooves."),
    ("Linn 9000 Kit 5", "Linn 9000", 126, 0.0, "electro",
     ["Kick 6", "Rim 1", "Snare 9", "Clap 1", "Snare 1", "Tom Low", "Hat Closed 2", "Tambourine 2",
      "Hat Closed 3", "Tambourine 3", "Hat Open 4", "Perc 1", "Perc 2", "Crash 5", "Perc 3", "Ping"],
     "Kicks and snares for dance music, with tambourines and the 9000's percussion."),
    ("Linn 9000 Selector Kit", "Linn 9000", 92, 0.0, "selector",
     ["Kick 7", "Kick 5", "Snare 2", "Kick 8", "Snare 3", "Crash 6", "Hat Closed 5", "Crash 7",
      "Tambourine 4", "Hat Open 5", "Hat Open 7", "Snare 6", "Snare 8", "Crash 2", "Tom High", "Ride 1"],
     "The rest of the Linn 9000: its other kicks, crashes, open hats and tambourine, one per pad."),
]

PAD_ROLE_LEVEL = {7: -3.0, 9: -4.0, 11: -4.0, 14: -6.0, 16: -6.0, 12: -2.0, 13: -2.0, 15: -3.0, 2: -1.0}
PAD_ROLE_PAN = {6: -0.3, 8: 0.0, 10: 0.3, 7: 0.15, 9: 0.15, 11: 0.15, 12: -0.25, 13: 0.25, 15: 0.2, 14: -0.2, 16: 0.25}


def is_hat(name):
    return name.startswith("Hat")


def source_files(source_roots, folder):
    """ A set's WAV files, by their path in its folder without the extension ("BD/kick - (1)")."""
    for source_root in source_roots:
        for base in (os.path.join(source_root, folder), os.path.join(source_root, folder.replace(" ", "_"), folder),
                     os.path.join(source_root, folder.replace(" ", "_"))):
            if not os.path.isdir(base):
                continue
            found = {}
            for directory, _, files in os.walk(base):
                for f in files:
                    if f.lower().endswith(".wav"):
                        path = os.path.join(directory, f)
                        found[os.path.splitext(os.path.relpath(path, base))[0].replace(os.sep, "/")] = path
            if found:
                return found
    sys.exit(f"no WAV files for {folder} under {source_roots}")


def category(name):
    """ What kind of sound a sample is, from its name (the selector kits group by it)."""
    n = name.lower()
    words = set(re.findall(r"[a-z0-9]+", n))
    if "kick" in words or "bass drum" in n:
        return "Kicks"
    if words & {"snare", "roll"}:
        return "Snares"
    if words & {"clap", "snap", "snaps"}:
        return "Claps"
    if "rim" in words or "side stick" in n or "sticks" in words:
        return "Rims"
    if "tom" in words:
        return "Toms"
    if "hat" in words:
        return "Hats"
    if words & {"crash", "ride", "cymbal", "china", "splash", "gong"}:
        return "Cymbals"
    if "bass" in words or "sub" in words:
        return "Bass"
    if words & {"scratch", "noise", "gun", "fx", "reverse", "shot", "blip", "heartbeat", "spark", "squash", "thriller",
                "shadow", "air", "click", "ring", "pipe", "plate", "wheel", "ambience", "q", "rub", "can", "hit"}:
        return "FX"
    return "Percussion"


# The selector kits' groups of sounds, in order.
SELECTOR_GROUPS = [("Kicks", {"Kicks"}), ("Snares & Claps", {"Snares", "Claps", "Rims"}), ("Toms", {"Toms"}),
                   ("Hats & Cymbals", {"Hats", "Cymbals"}), ("Percussion", {"Percussion"}), ("Bass & FX", {"Bass", "FX"})]


def natural_key(text):
    return [int(t) if t.isdigit() else t.lower() for t in re.split(r"(\d+)", text)]


def selector_kits(machine, short, names, used):
    """ Kits of the samples no playable kit uses, sixteen of one kind each, so that every one is on a pad."""
    kits = []
    left = sorted(names - used, key=natural_key)
    for label, kinds in SELECTOR_GROUPS:
        group = [n for n in left if category(n) in kinds]
        chunks = [group[i:i + 16] for i in range(0, len(group), 16)]
        for k, chunk in enumerate(chunks):
            name = f"{short} {label}" + (f" {k + 1}" if len(chunks) > 1 else "")
            pads = chunk + [None] * (16 - len(chunk))
            kits.append((name, machine, 110, 0.0, "audition", pads,
                         f"More of the {machine}'s {label.lower()}, one per pad: its demo plays each in turn."))
    return kits


def demo_xml(groove, pads, tempo, swing):
    if groove == "audition":  # each pad in turn, in eighths
        lanes = [f'    <LANE pad="{pad}" steps="{"." * (2 * pad) + "x" + "." * (31 - 2 * pad)}"/>'
                 for pad in range(16) if pads[pad] is not None]
        return f'  <DEMO tempo="{tempo}" bars="2" swing="{swing}">\n' + "\n".join(lanes) + "\n  </DEMO>\n"
    lanes = []
    for pad, steps in sorted(G[groove].items()):
        steps = (steps + "." * 32)[:32]
        if pads[pad - 1] is None or not any(c in "Xxo" for c in steps):
            continue
        lanes.append(f'    <LANE pad="{pad - 1}" steps="{steps}"/>')
    return f'  <DEMO tempo="{tempo}" bars="2" swing="{swing}">\n' + "\n".join(lanes) + "\n  </DEMO>\n"


def kit_xml(name, machine, tempo, swing, groove, pads, description):
    out = ['<?xml version="1.0" encoding="UTF-8"?>\n',
           f"<!-- Built-in drum kit (tools/drumkits/make_kits.py): the {machine}'s samples, arranged on the\n"
           "     Drum Rack's pads, with a demo beat the browser plays when the kit is selected. -->\n",
           f'<DOJO_PRESET version="1" name={quoteattr(name)} category="Drums" kit="1" machine={quoteattr(machine)}\n'
           f'             description={quoteattr(description)}>\n',
           '  <DEVICE key="builtin:drumrack" name="Drum Rack">\n',
           '    <DOJO_DEVICE type="builtin:drumrack" version="1">\n']
    selector = groove == "audition"
    for pad in range(1, 17):
        sample = pads[pad - 1] or ""
        level = 0.0 if selector else PAD_ROLE_LEVEL.get(pad, 0.0)
        if category(sample) == "Cymbals":
            level = -6.0
        elif selector and category(sample) == "Hats":
            level = -3.0
        pan = 0.0 if selector else PAD_ROLE_PAN.get(pad, 0.0)
        choke = 1 if not selector and pad in STD_HATS and is_hat(sample) else 0
        out.append(f'      <PARAM id="pad{pad}Volume" value="{level}"/>\n')
        out.append(f'      <PARAM id="pad{pad}Pan" value="{pan}"/>\n')
        out.append(f'      <PARAM id="pad{pad}Pitch" value="0.0"/>\n')
        out.append(f'      <PARAM id="pad{pad}Output" value="0.0"/>\n')
        out.append(f'      <PARAM id="pad{pad}Choke" value="{float(choke)}"/>\n')
    out.append('      <PARAM id="master" value="-3.0"/>\n')
    out.append("      <EXTRA>\n")
    for pad in range(1, 17):
        if pads[pad - 1] is not None:
            out.append(f'        <PAD index="{pad - 1}" sample={quoteattr("kit:" + machine + "/" + pads[pad - 1] + ".wav")}/>\n')
    out.append("      </EXTRA>\n    </DOJO_DEVICE>\n  </DEVICE>\n")
    out.append(demo_xml(groove, pads, tempo, swing))
    out.append("</DOJO_PRESET>\n")
    return "".join(out)


SHORT_NAMES = {"Roland MC-202": "MC-202", "Roland MC-303": "MC-303", "Roland MC-808": "MC-808",
               "Roland R-8 MKII": "R-8 MKII", "Roland R-70": "R-70", "Linn LM-1": "LM-1", "Linn LM-2": "LM-2",
               "LinnDrum": "LinnDrum", "AdrenaLinn": "AdrenaLinn", "Linn 9000": "Linn 9000"}


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    source_roots = sys.argv[1:]
    kits_dir = os.path.join(ROOT, "resources", "kits")
    external_dir = os.path.join(ROOT, "resources", "drum-machines")
    presets_dir = os.path.join(ROOT, "resources", "presets", "kits")
    for d in (kits_dir, external_dir, presets_dir):
        os.makedirs(d, exist_ok=True)
    for old in os.listdir(presets_dir):  # (kits are written afresh)
        if old.endswith(".dojopreset"):
            os.remove(os.path.join(presets_dir, old))

    machines = dict(MACHINES)
    machines.update(roland.MACHINES)
    names = {}
    for machine, (folder, renames) in machines.items():
        files = source_files(source_roots, folder)
        missing = set(files) - set(renames)
        unknown = set(renames) - set(files)
        if missing or unknown:
            sys.exit(f"{machine}: not renamed {sorted(missing)}, not found {sorted(unknown)}")
        if len(set(renames.values())) != len(renames):
            sys.exit(f"{machine}: two samples with the same name")
        names[machine] = set(renames.values())
        archive_dir = external_dir if machine in roland.MACHINES else kits_dir
        with zipfile.ZipFile(os.path.join(archive_dir, machine + ".zip"), "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for original in sorted(files, key=lambda o: renames[o]):
                info = zipfile.ZipInfo(renames[original] + ".wav", date_time=(2024, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o644 << 16
                with open(files[original], "rb") as f:
                    z.writestr(info, f.read())

    used = {m: set() for m in machines}
    written = []
    playable = KITS + roland.KITS
    for name, machine, tempo, swing, groove, pads, description in playable:
        for sample in pads:
            if sample is not None:
                used[machine].add(sample)
    selectors = []
    for machine in machines:
        selectors += selector_kits(machine, SHORT_NAMES[machine], names[machine], used[machine])
    for name, machine, tempo, swing, groove, pads, description in playable + selectors:
        if len(pads) != 16:
            sys.exit(f"{name}: {len(pads)} pads")
        for sample in pads:
            if sample is None:
                continue
            if sample not in names[machine]:
                sys.exit(f"{name}: no sample {sample} in {machine}")
            used[machine].add(sample)
        path = os.path.join(presets_dir, name + ".dojopreset")
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(kit_xml(name, machine, tempo, swing, groove, pads, description))
        written.append(path)

    for machine in machines:
        unused = sorted(names[machine] - used[machine])
        if unused:
            sys.exit(f"{machine}: samples on no pad: {unused}")
        kits = sum(1 for k in playable + selectors if k[1] == machine)
        print(f"{machine}: {len(names[machine])} samples, all on pads, {kits} kits")
    print(f"{len(written)} kits")


if __name__ == "__main__":
    main()
