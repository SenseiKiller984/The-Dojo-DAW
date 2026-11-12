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
- Audio tracks and aux inputs (busses) are **mono or stereo**. A mono track plays its clips as one signal, or one chosen channel of each clip's file. It also records one input channel.

**Surround (multichannel)**
- Audio tracks, aux inputs, folders and the master can also be **3.1, 5.1, 7.1, Dolby Digital or DTS**. Each format has one channel per speaker, in this order:

  | Format | Channels |
  | --- | --- |
  | 3.1 | L R C LFE |
  | 5.1 | L R C LFE Ls Rs (SMPTE/ITU) |
  | 7.1 | L R C LFE Ls Rs Lrs Rrs |
  | Dolby Digital | L C R Ls Rs LFE (5.1 in AC-3 order) |
  | DTS | L R Ls Rs C LFE (5.1 in DTS order) |

- Choose the format in *Track > New*, or later in the track's right-click *Format* menu. The **master's format** is set in the master strip of the mixer.
- A multichannel track plays multichannel files and records one input per channel (*In 3-8* for a 5.1 track on input 3).
- Signals move between formats **by speaker**, so a Dolby Digital track's centre plays from a 5.1 master's centre:
  - A track that feeds a smaller format is folded down (ITU style): the centre and surrounds go to the sides at -3 dB, and the LFE is dropped.
  - On an audio device with fewer outputs than the master has channels, you hear the master's stereo fold-down.
- **Surround panner**: a mono or stereo track that feeds a surround bus or master gets a panner pad in its mixer strip. Drag to place the track (left/right is its pan, up/down front to rear). Right-click to set how much of a centred signal plays from the centre speaker (dialogue: 100 %). Double-click to reset.
- **Effects on multichannel tracks**: every device and plug-in runs as linked instances, one per channel group. The speaker pairs (L/R, Ls/Rs, Lrs/Rrs) run as stereo pairs; the centre and LFE run on their own. All instances follow the first one's settings.
- Meters show one bar per channel, and the arranger draws every channel of a multichannel clip when the track is tall enough.
- **Export**:
  - A surround master exports as a multichannel WAV or FLAC. Stems have their destination's channels.
  - MP3 gets the stereo fold-down.

**Track menu**
- **New...** (Ctrl+Shift+N) adds one or more tracks in one go. Each row is a count, a format (mono or stereo), a type and a name; + and - add and remove rows. The tracks go after the selected track (inside its folder) or at the end. The types:
  - audio track, aux input (a bus), instrument track or folder (audio tracks, aux inputs and folders in any format: mono, stereo, 3.1, 5.1, 7.1, Dolby Digital or DTS);
  - **MIDI track**: MIDI clips (and live MIDI when armed) that play the instrument track picked in its menu (*MIDI Output*). Muting it silences its notes; soloing the instrument keeps it playing.
  - **VCA Master**: no audio of its own. Its fader (and volume automation) scales its member tracks, its mute mutes them and its solo solos them; VCAs can be nested. Assign tracks with *VCA* in the track menu (it applies to the whole selection).
  - **Master Fader**: one last fader, with mute, volume automation and post-fader devices, on the main output or on a bus (*Controls* in its menu; one per path).
  - **Click Track**: a mono aux input with the **Click** device already inserted (also *Track > Create Click Track*).
- **Duplicate...** (Alt+Shift+D) makes any number of copies of the selected tracks. You choose whether the copies get the clips, automation, devices (with their current settings) and sends. Each copy goes right after its source, or all of them after the last selected track. A folder is copied with its contents; routing inside it points at the copies.
- **Delete** removes the selected tracks (undoable). Deleting a VCA Master coalesces it: its fader and automation are added into each member's own volume (and a muted VCA mutes them), so they sound exactly as before.
- Duplicating a VCA member without *VCA assignment* bakes the VCA's gain into the copy the same way.
- **Split into Mono** gives each selected stereo or multichannel audio track one mono track per channel ("Name.L", "Name.R", and for surround "Name.C", "Name.LFE", "Name.Ls" ...).
  - Each one is placed at its speaker: pan, front/rear, and the centre speaker for C.
  - Each plays and records that channel.
  - Each gets the source's devices, sends and automation; the source track stays.
- **Make Inactive / Make Active**: an inactive track (or everything in an inactive folder) is not played and its devices use no CPU. Tracks routed into an inactive bus are silent. Inactive tracks are dimmed and can't be soloed, muted or recorded.
- **Clear All Clip Indicators** (Alt+C) clears the red clip lights of every meter, in the mixer and on the master. They light when a signal goes over 0 dBFS and stay lit until cleared.
- **Scroll to Track...** takes a track number (or a pick from the list), selects that track and brings it to the top of the arranger (opening collapsed folders) and the left of the mixer.

**Recording and MIDI**
- Pick each audio track's input (mono channel or stereo pair), arm it (R) and record (R key or the record button). Each track's monitoring is AUTO, IN (always the input) or OFF.
- **Track > Set Record Tracks to Input Only / Auto Input** decides what record-enabled tracks on AUTO hear:
  - **Input Only**: always their input.
  - **Auto Input** (tape-machine style, the default): the input while stopped. While playing, the track's own clips. While recording, the input from the punch-in point to the punch-out point, and the clips before and after.
- **Punch In / Punch Out** (the buttons next to the loop button, or the Transport menu) use the loop range. A take keeps only what lies between the punch points; red brackets in the ruler show them.
- A take replaces what was on its track where it lies; one undo brings the old take back whole.
- MIDI keyboards are enabled automatically, also when plugged in while The Dojo runs. Armed instrument tracks play and record live MIDI (notes, controllers, pitch bend).
- Recorded clips are placed with **latency compensation** (input + output + plug-in latency).

**Editing**
- Audio clips: move (also across tracks), trim, fades, split, duplicate, cut/copy/paste; overlapping clips **crossfade** (equal power).
- Selecting clips like files in Explorer: **drag a box** in empty space (Ctrl/Shift adds to the selection), **Ctrl+click** adds or removes a clip, **Shift+click** selects every clip on that track from the last clicked one (Ctrl+Shift+click adds that range).
- Selecting tracks the same way, in the track headers (or on mixer strips): **Shift+click** selects every track from the last one clicked to this one, **Ctrl+click** adds or removes one track, **Ctrl+Shift+click** adds a range. Then:
  - **Ctrl+Shift+G** or *Group into New Folder* puts all of them into one new folder, in their order. Tracks from different folders are moved in too.
  - Dragging one of their headers moves them all (drop them inside a folder to move them into it).
  - M, S and R on one of them switch all of them.
  - The right-click menu's colour, output, move into or out of a folder and delete apply to all of them.
- Stereo clips show **both channels** (left above right), mono clips one waveform.
- **Time-stretch and pitch-shift** per clip (Signalsmith Stretch), plus per-track speed and pitch.
- Global **speed** (varispeed, or *keep pitch*) and **transpose**.
- Long files **stream from disk** instead of being loaded into memory.
- Loop range (drag in the ruler's top strip, L toggles), snap, zoom, undo/redo for everything.

**Clip menu** (also in the clip's right-click menu; each applies to all selected clips: it locks them, or unlocks them if all are locked)
- **Edit Lock / Edit Unlock** (Ctrl+Shift+L): an edit-locked clip (padlock in its title bar) can't be cut, deleted, split, trimmed, faded or moved (in time or to another track). Its notes, gain, stretch and pitch are read-only in the editor. Copies of it, pasted to any track or time, are edit-locked too.
- **Time Lock / Time Unlock** (Ctrl+Alt+L): a time-locked clip (clock in its title bar) stays at its time location. It can't be moved in time or deleted, but it can be trimmed, split or dragged to another track at the same time. When it is split, both parts stay time-locked.
- Recording over a locked clip leaves it as it is. Delete and cut skip locked clips, which stay selected.

**MIDI sequencing**
- **Piano roll**: draw, move, resize and delete notes; velocity lane; **controller lanes** (mod wheel, volume, pan, expression, sustain, pitch bend); **quantise** with strength and optional note ends.
- **Step sequencer** view of the same clip: rows are the Drum Rack's pads or Impulse's slots (or 16 notes), click or paint steps, right-click for velocity.

**Native devices** (64-bit, real-time safe, with dedicated editors; drag a knob, or double-click the value under it to type an exact one: "120 Hz", "2.5k", "-6 dB")
- **EQ**: 4 bands (low/high cut, shelves, peaks) with a live spectrum analyser and draggable band handles.
- **Compressor** with soft knee, makeup, mix and an **external sidechain** (pick any track).
- **Limiter** (lookahead brickwall):
  - **Maximize**: Threshold replaces Input Gain. Material at the threshold comes out at the ceiling, so lowering the threshold reduces the dynamic range and makes the track louder.
  - **Auto Release**: a fast path recovers within ~20 ms after short peaks. A slow path only engages under sustained limiting, so dense material does not pump.
  - **Stereo** or **L/R**: Stereo gives both channels the same gain reduction. L/R limits each channel on its own peaks, and **Link** blends between the two (100 % = Stereo).
  - Modes:
    - **Standard**: transparent.
    - **Soft Clip**: lets peaks up to 6 dB over the ceiling reach a saturation curve, which rounds them off below it for warmth and crunch.
    - **True Peak**: an 8x-oversampling detector keeps the peaks between samples under the ceiling too, for streaming and CD delivery. It adds 8 samples of latency, compensated automatically.
- **Reverb** (feedback delay network) and tempo-synced **ping-pong delay**.
- **Echo**: two-line tape-style delay (stereo, ping-pong or mid/side; synced or free, linked or not) with resonant filter, time and filter modulation, drive, noise, wobble, ducking, reverb before/after/inside the feedback loop, width.
- **Utility**: gain ±35 dB, width 0–200 % and Mono, Bass Mono (below a crossover), phase invert left/right, channel mode (left, right, swap), balance, mute, DC filter.
- **Amp & Cabinet**: seven amp models (Clean to Heavy, and Bass) with 4x-oversampled gain stages, bass/middle/treble/presence, five cabinets and three microphone positions.
- **Pitch Shift** for live input and instruments.
- **Click**: a metronome that follows the transport's tempo, bar position and time signature, sample-accurately. It has four sounds (Beep, Woodblock, Rim, Cowbell), volume, an accented downbeat, and quarter, eighth or sixteenth clicks. It plays during playback and recording, or only while recording.

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
- **General**: *Return the view to the start with the playhead* (Stop twice, the return button or Home also scrolls back to bar 1); *Stop at the end of the arrangement* (playback stops at the end of the last clip, and the next Play starts again from bar 1, the view too with the first option; a loop still loops, recording carries on).
- **Themes**: Standard Black and White; Japanese (Dojo Night, Washi, Aizome); Cherry Blossom (Sakura Night, Sakura Day); Manga (Manga Ink, Manga Noir, Shonen Pop). Applied instantly, remembered.
- **Audio** (also *Options > Audio Settings*) and **Keyboard**: every shortcut, editable and remembered (also *Options > Keyboard Shortcuts*, F1).

**Projects**: `.dojo` files with relative audio paths (move the folder freely); device and plug-in state are saved inside. *File > Open Demo Project* builds a short song that uses every kind of track and device.

## Using it

1. Drag audio files onto the arranger, or add an instrument track (**+ Add Track**, or Ctrl+Shift+T) and double-click its lane to create a MIDI clip.
2. Press **Space** to play, **R** to record onto armed tracks, **L** to loop.
3. The bottom panel has three tabs: **Mixer** (F3), **Devices** of the selected track (F4) and the **Editor** (F5) for the clip you double-clicked. In the mixer, double-click the volume or pan value under a fader (or the master's GAIN) to type it: `-6`, `-inf`, `C`, `L20`, `R35`.
4. The browser lists devices and plug-ins at the top (with search; drag one onto a track or the Devices panel, or double-click it) and your own folders below: add a folder with **+** (or drop one from the file manager), search them, and click a sample to **preview** it (arrow keys walk through files; the headphones button turns preview off). Drag samples onto the arranger, a Drum Rack pad, an Impulse slot or the Sampler. Scan for plug-ins with **Scan** / **...**.
5. Export with **Ctrl+R**; save with **Ctrl+S**.

| Keys | Action |
|---|---|
| Space / Home | Play-stop / return to start |
| R / L | Record / loop on-off |
| Ctrl+T, Ctrl+Shift+T, Ctrl+B, Ctrl+Shift+F | Add audio track, instrument track, bus, folder |
| Ctrl+Shift+N, Alt+Shift+D | New tracks, duplicate tracks |
| Ctrl+Shift+G | Put the selected tracks into a new folder |
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
              pitch shift, Click; instruments: Wavetable, Operator, Sampler, Drum Rack, Impulse
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
