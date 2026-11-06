# The Dojo

A cross-platform digital audio workstation for **Windows and Linux**, with identical features on both. It is built in C++20 on JUCE, with a 64-bit floating-point mix engine and strictly real-time-safe audio code.

![The Dojo playing its demo project: a folder with a Drum Rack beat and a sidechained bass, Sampler chords with a volume automation lane, a reverb return and the Drum Rack's editor; the browser shows the devices and a sample folder, with a kick being previewed](docs/images/main-window.png)

## What you can do

**Tracks and routing**
- Audio tracks, instrument tracks, busses (groups and effect returns) and **folders** (a folder is a bus that sums the tracks inside it; collapse it, nest folders, drag tracks in and out).
- Every track has volume, pan, mute, solo, phase invert, a meter, an **output** (the master, its folder or any bus) and **sends** (pre- or post-fader) to busses. Feedback loops are refused.
- Solo follows the routing: soloing a track keeps the busses it feeds audible, soloing a bus keeps its sources audible.
- **Plug-in delay compensation** aligns every path that meets at a bus or the master.
- The graph renders on **all CPU cores**: independent tracks run in parallel.

**Recording and MIDI**
- Pick each audio track's input (mono channel or stereo pair), arm it (R) and record (R key or the record button). Input monitoring: auto (while armed), always or off.
- MIDI keyboards are enabled automatically, also when plugged in while The Dojo runs. Armed instrument tracks play and record live MIDI (notes, controllers, pitch bend).
- Recorded clips are placed with **latency compensation** (input + output + plug-in latency).

**Editing**
- Audio clips: move (also across tracks), trim, fades, split, duplicate, cut/copy/paste; overlapping clips **crossfade** (equal power).
- Stereo clips show **both channels** (left above right), mono clips one waveform.
- **Time-stretch and pitch-shift** per clip (Signalsmith Stretch), plus per-track speed and pitch.
- Global **speed** (varispeed, or *keep pitch*) and **transpose**.
- Long files **stream from disk** instead of being loaded into memory.
- Loop range (drag in the ruler's top strip, L toggles), snap, zoom, undo/redo for everything.

**MIDI sequencing**
- **Piano roll**: draw, move, resize and delete notes; velocity lane; **controller lanes** (mod wheel, volume, pan, expression, sustain, pitch bend); **quantise** with strength and optional note ends.
- **Step sequencer** view of the same clip: rows are the Drum Rack's pads or Impulse's slots (or 16 notes), click or paint steps, right-click for velocity.

**Native devices** (64-bit, real-time safe, with dedicated editors; drag a knob, or double-click the value under it to type an exact one: "120 Hz", "2.5k", "-6 dB")
- **EQ**: 4 bands (low/high cut, shelves, peaks) with a live spectrum analyser and draggable band handles.
- **Compressor** with soft knee, makeup, mix and an **external sidechain** (pick any track); **lookahead limiter**.
- **Reverb** (feedback delay network) and tempo-synced **ping-pong delay**.
- **Echo**: two-line tape-style delay (stereo, ping-pong or mid/side; synced or free, linked or not) with resonant filter, time and filter modulation, drive, noise, wobble, ducking, reverb before/after/inside the feedback loop, width.
- **Utility**: gain ±35 dB, width 0–200 % and Mono, Bass Mono (below a crossover), phase invert left/right, channel mode (left, right, swap), balance, mute, DC filter.
- **Amp & Cabinet**: seven amp models (Clean to Heavy, and Bass) with 4x-oversampled gain stages, bass/middle/treble/presence, five cabinets and three microphone positions.
- **Pitch Shift** for live input and instruments.

**Instruments**
- **Wavetable**: two morphing wavetable oscillators (eight band-limited tables), unison up to 8, sub and noise, multimode filter, three envelopes, two LFOs and an 8-slot modulation matrix.
- **Operator**: 4-operator FM with 11 algorithms, per-operator waveform, ratio or fixed frequency, envelope and velocity, feedback, filter with envelope, pitch envelope and LFO.
- **Sampler**: polyphonic, ADSR, root note, tuning, start/end and loop; drop a file on it.
- **Drum Rack**: 16 pads (drag samples onto any pad from the browser or the file manager), per-pad volume, pan, pitch, choke groups and **outputs** — one click creates a return track for each pad output (multi-out).
- **Impulse**: 8 sample slots (drag onto any slot) with start, transpose, drive, transient, decay, filter, pan, volume and velocity per slot; slots 7 and 8 can choke each other (hi-hats).

**Automation and export**
- **Spline automation lanes** for volume, pan, sends and every device or plug-in parameter (A on the track header, or right-click > Add Automation Lane).
- **Export** (Ctrl+R): offline (faster than real time) or real-time bounce, **WAV** (16/24/32-bit float), **FLAC**, **MP3** (LAME, 128–320 kbps), the mix and/or **stems** (one file per track).

**Plug-ins**
- **VST3 and CLAP** on both Windows and Linux: parameters (automatable), state saved with the project, their own editor windows, latency reported to the delay compensation.
- **Out-of-process scanning**: each plug-in is opened by a separate scanner process, so one that crashes or hangs is blacklisted instead of taking The Dojo down.

**Preferences** (*Options > Preferences*, Ctrl+,)
- **General**: *Return the view to the start with the playhead* (Stop twice, the return button or Home also scrolls back to bar 1); *Stop at the end of the arrangement* (playback stops at the end of the last clip; a loop still loops, recording carries on).
- **Themes**: Standard Black and White; Japanese (Dojo Night, Washi, Aizome); Cherry Blossom (Sakura Night, Sakura Day); Manga (Manga Ink, Manga Noir, Shonen Pop). Applied instantly, remembered.
- **Audio** (also *Options > Audio Settings*) and **Keyboard**: every shortcut, editable and remembered (also *Options > Keyboard Shortcuts*, F1).

**Projects**: `.dojo` files with relative audio paths (move the folder freely); device and plug-in state are saved inside. *File > Open Demo Project* builds a short song that uses every kind of track and device.

## Using it

1. Drag audio files onto the arranger, or add an instrument track (**+ Add Track**, or Ctrl+Shift+T) and double-click its lane to create a MIDI clip.
2. Press **Space** to play, **R** to record onto armed tracks, **L** to loop.
3. The bottom panel has three tabs: **Mixer** (F3), **Devices** of the selected track (F4) and the **Editor** (F5) for the clip you double-clicked.
4. The browser lists devices and plug-ins at the top (with search; drag one onto a track or the Devices panel, or double-click it) and your own folders below: add a folder with **+** (or drop one from the file manager), search them, and click a sample to **preview** it (arrow keys walk through files; the headphones button turns preview off). Drag samples onto the arranger, a Drum Rack pad, an Impulse slot or the Sampler. Scan for plug-ins with **Scan** / **...**.
5. Export with **Ctrl+R**; save with **Ctrl+S**.

| Keys | Action |
|---|---|
| Space / Home | Play-stop / return to start |
| R / L | Record / loop on-off |
| Ctrl+T, Ctrl+Shift+T, Ctrl+B, Ctrl+Shift+F | Add audio track, instrument track, bus, folder |
| Ctrl+Shift+G | Put the selected track into a new folder |
| Ctrl+I, Ctrl+R | Import audio, export audio |
| Ctrl+Z, Ctrl+Shift+Z | Undo, redo |
| Ctrl+X / C / V, Delete, Ctrl+D, Ctrl+E, Ctrl+A | Cut / copy / paste, delete, duplicate, split at playhead, select all clips |
| F3 / F4 / F5 | Mixer / Devices / Editor |
| Piano roll: Q, arrows, Delete | Quantise, move / transpose selected notes, delete |
| Ctrl+G | Toggle snap |
| Ctrl+= / Ctrl+- / Ctrl+0 | Zoom in, out, to fit |
| Ctrl+N / Ctrl+O / Ctrl+S | New, open, save |
| Ctrl+, | Preferences (playback options, themes, audio, shortcuts) |
| Ctrl+Shift+A | Audio settings (device, inputs, MIDI inputs) |
| F1 | Keyboard shortcuts (change them there) |

Mouse:
- Right-click clips, track headers, automation lanes or empty lanes for context menus.
- Shift+wheel scrolls sideways; Ctrl+wheel zooms (also in the piano roll).
- Double-click a fader, knob or slider to reset it.

### Command line

```bash
TheDojo [session.dojo] [audio files...] [--demo] [--audio-api=JACK|ALSA|ASIO|"Windows Audio"] [--play] [--test-tone] [--smoke-test[=secs]] [--version]
```

The log is written to `~/.config/TheDojo/TheDojo.log` on Linux and `%APPDATA%\TheDojo\TheDojo.log` on Windows. Settings and the plug-in list (`PluginList.xml`) go next to it. Recordings go into a `<project> Recordings` folder next to the project (or `Music/The Dojo/Recordings` for unsaved projects).

## Building

### Linux

```bash
sudo ./scripts/setup-linux.sh
```

```bash
./scripts/build.sh            # Release; add `debug`, `--clang`, `--run`, `--no-tests`
```

The binary is at `build/linux-release/src/app/TheDojo_artefacts/Release/TheDojo`.

### Windows

Install Visual Studio 2022 or newer with the **Desktop development with C++** workload, which includes CMake, and Git. Then, from PowerShell:

```powershell
.\scripts\build.ps1           # Release; add -Config Debug, -Run, -NoTests
```

The binary is at `build\windows-msvc\src\app\TheDojo_artefacts\Release\TheDojo.exe`.

### Plain CMake

```bash
cmake --preset linux-release
```

```bash
cmake --build --preset linux-release
```

```bash
ctest --preset linux-release
```

Run `cmake --list-presets` for all configurations, including `linux-asan`, `linux-tsan` and `windows-msvc`. To build offline against an existing JUCE checkout, add `-DFETCHCONTENT_SOURCE_DIR_JUCE=/path/to/JUCE`.

Sanitizer presets need Clang's runtimes (`libclang-rt-<ver>-dev` on Debian/Ubuntu). On recent kernels, run TSan binaries under `setarch $(uname -m) -R`. ASan and TSan replace `operator new`/`delete` themselves, so those builds omit the real-time allocation trap, and the trap tests report SKIPPED.

## Tech stack

| | |
|---|---|
| Language | C++20 (GCC ≥ 11, Clang ≥ 14, MSVC 2022+) |
| Build | CMake ≥ 3.22 with presets, Ninja (Linux) / Visual Studio (Windows) |
| Audio, MIDI, VST3 hosting, GUI | [JUCE 9.0.4](https://github.com/juce-framework/JUCE), fetched and pinned automatically |
| CLAP hosting | Our own host on the official [CLAP SDK](https://github.com/free-audio/clap) 1.2.10 (MIT) |
| Time-stretch / pitch | [Signalsmith Stretch](https://github.com/Signalsmith-Audio/signalsmith-stretch) 1.4.0 (MIT) |
| MP3 encoding | [LAME](https://lame.sourceforge.io) 3.100 (LGPL), vendored in `third_party/lame` |
| Audio back-ends | Windows: ASIO, WASAPI, DirectSound. Linux: ALSA (incl. PulseAudio/PipeWire via ALSA), JACK (incl. PipeWire-JACK) |
| Tests | Catch2 v3 |

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the reasoning, the threading model and the real-time rules.

## Repository layout

```
cmake/        build options, compiler policy, pinned dependencies
docs/         architecture
resources/    embedded fonts
scripts/      setup and build helpers (bash + PowerShell)
src/core      real-time primitives (lock-free SPSC queue, RT allocation trap), musical time
src/engine    AudioEngine (audio thread + worker pool), render graph, routing, delay compensation,
              clip playback / time-stretch / disk streaming, capture, file preview, device host,
              MP3 encoder
src/devices   native devices: EQ, compressor, limiter, reverb, delay, Echo, Utility, Amp & Cabinet,
              pitch shift; instruments: Wavetable, Operator, Sampler, Drum Rack, Impulse
src/plugins   plug-in hosting (VST3 via JUCE, CLAP host), plug-in list and out-of-process scanning
src/scanner   dojo-plugin-scanner, the scanner child process
src/session   the document (tracks, clips, MIDI, devices, sends, automation) with undo, audio
              decoding/streaming, device instances, graph compiler, recorder, exporter, save/open
src/ui        arranger, mixer, devices panel + editors, piano roll, step sequencer, browser,
              transport, export dialog, menus, look-and-feel
src/app       application entry point, main window, settings
tests/        unit and integration tests (incl. a test CLAP plug-in)
third_party/  vendored LAME
```

## Roadmap

1. ✅ **Foundation**: stack, build system, real-time engine core, lock-free UI ↔ audio protocol
2. ✅ **Interactive baseline**: tracks, audio clips, arranger editing, mixer, undo, projects, audio settings
3. ✅ **Recording & MIDI I/O**: audio input routing and monitoring, recording, MIDI hot-plug, latency compensation
4. ✅ **Arranger depth**: busses, folders and routing, crossfades, time-stretch and pitch, multi-core graph, disk streaming
5. ✅ **MIDI sequencing**: piano roll, quantise, CC lanes, step sequencer
6. ✅ **Native DSP**: compressor, limiter, reverb, parametric EQ, delay, speed and pitch
7. ✅ **Instruments**: Drum Rack, Sampler, Impulse, Wavetable, Operator
8. ✅ **Automation and export**: spline lanes, offline/real-time bounce, WAV/FLAC/MP3, stems
9. ✅ **Plug-in hosting**: VST3 + CLAP, out-of-process scanning

## Licensing

No licence has been chosen for The Dojo yet. Note that JUCE is AGPLv3 or commercial, the bundled ASIO SDK is GPLv3 or a Steinberg licence, and LAME is LGPL (it is linked statically, which has obligations under the LGPL). See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#11-licensing-note). The embedded Inter and JetBrains Mono fonts are under the SIL Open Font License; see `resources/fonts/`.
