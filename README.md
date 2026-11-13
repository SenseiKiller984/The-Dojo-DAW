# The Dojo

A cross-platform digital audio workstation for **Windows and Linux**, with identical features on both. It is built in C++20 on JUCE, with a 64-bit floating-point mix engine and strictly real-time-safe audio code.

![The Dojo playing its demo project: a folder with a Drum Rack beat and a sidechained bass, Sampler chords with a volume automation lane, a reverb return and the Drum Rack's editor; the browser shows the devices and a sample folder, with a kick being previewed](docs/images/main-window.png)

## What you can do

**Tracks and routing**
- Audio tracks, instrument tracks, busses (groups and effect returns) and **folders** (a folder is a bus that sums the tracks inside it; collapse it, nest folders, drag tracks in and out).
- Every track has volume, pan, mute, solo, phase invert, a meter, an **output** (the master, its folder or any bus) and **sends** (pre- or post-fader) to busses. Feedback loops are refused.
- Solo follows the routing: soloing a track keeps the busses it feeds audible, soloing a bus keeps its sources audible.
- **Plug-in delay compensation** aligns every path that meets at a bus, a folder or the master. Whatever arrives earlier waits for the latest arrival: a track by its own compensation delay, each send (pre- or post-fader) and each instrument aux output by an alignment delay of its own.
  - Sends leave their track before its compensation delay. So a track that plays to the master and also sends to a return with latency (a reverb return with a limiter, a parallel compression bus) lines up exactly with that return.
  - **Automation moves with the sound it shapes.** A track's fader, pan, VCA and send automation, and each device's parameter automation, are read as late as the sound at that point (behind a 10 ms lookahead, 10 ms later).
  - **Exports line up with the timeline.** The mix skips the whole project's delay. Each stem skips its own track's delay (a track inside a folder with latency meets that folder earlier than the master). The render runs long enough that nothing is cut at the end.
  - **The playhead and time show what you hear**: while playing, the position minus the delay to the speakers (the delay compensation plus the audio device's output latency). On a loop wrap they show the end of the loop for that moment.
  - Recording places takes where they were played (input, output and compensation latency removed).
- **Delay readouts**: you can see how much delay (latency) each device adds and what a whole chain adds up to.
  - **Each device** shows its delay in milliseconds in a badge in its header (in gold when it adds any). Hover the badge to see the samples and the chain's delay up to and including that device.
  - **The Devices panel's header** shows the whole chain's delay (CHAIN DELAY), the sum over the devices that are on. Its tooltip also gives the whole project's delay after compensation.
  - **The status bar** shows the project's delay compensation (PDC) in milliseconds and samples.
  - Everything follows changes as you make them: a lookahead or mode that changes a device's delay, bypassing a device (a bypassed device adds no delay), and the sample rate.
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
  - How a surround master reaches your ears is set in **Preferences > Surround** (see *Surround monitoring* below).
- **Surround panner**: a mono or stereo track that feeds a surround bus or master gets a panner pad in its mixer strip. Drag to place the track (left/right is its pan, up/down front to rear). Right-click to set how much of a centred signal plays from the centre speaker (dialogue: 100 %). Double-click to reset.
- **Effects on multichannel tracks**: every device and plug-in runs as linked instances, one per channel group. The speaker pairs (L/R, Ls/Rs, Lrs/Rrs) run as stereo pairs; the centre and LFE run on their own. All instances follow the first one's settings.
- Meters show one bar per channel, and the arranger draws every channel of a multichannel clip when the track is tall enough.
- **Export**:
  - A surround master exports as a multichannel WAV or FLAC. Stems have their destination's channels.
  - MP3 gets the stereo fold-down. (Surround monitoring never changes what is exported.)

**Surround monitoring** (*Options > Preferences > Surround*): how a surround master plays on stereo headphones or speakers. The meters always show every channel.
- **Standard**: the channels as they are, one output per channel. On a stereo device you hear only the front left and right.
- **Classic Fold-Down** (the default, what earlier versions did): an output per channel when the device has enough; otherwise the ITU-R BS.775 stereo fold-down (centre and surrounds at -3 dB, the LFE left out).
- **Advanced Surround Virtualizer**: binaural rendering of every speaker at its standard place (L/R at ±30°, C at 0°, surrounds at ±110° in 5.1 and ±90° in 7.1, rears at ±150°, 2 m away in a 5 × 6 m room). It plays on outputs 1 and 2, also on a device with more outputs.
  - Each speaker reaches each ear four ways: directly, and as first reflections off the floor, the nearest side wall and the front or back wall.
  - Every path uses the Brown–Duda spherical-head model:
    - The interaural time difference follows Woodworth's formula (0.26 ms for the front speakers, 0.66 ms at the side), as a fractional delay.
    - The head shadow is a first-order filter: up to +6 dB of treble at the near ear, about -20 dB in the head's shadow.
  - Sources behind get a pinna cue (a high shelf, down to -8 dB at 4 kHz), and a short four-line feedback-delay-network reverberation (0.35 s) puts the sound outside the head.
  - The LFE plays on both sides below 120 Hz. A full mix plays at the fold-down's level.
  - **Headphones** or **Speakers**: for speakers, a recursive crosstalk canceller (for speakers at ±30°, band-limited to 250 Hz – 6 kHz) keeps each ear's signal to that ear.
  - **Room** (0–100 %): 0 % is anechoic (closer, inside the head); more gives more reflections and reverberation.
- **Device 3D Virtualizer**: sends the surround channels to the device in its own channel order, so the device's built-in virtualizer renders them. That can be a 7.1 gaming headset, a USB surround card, an AV receiver, or the system's spatial sound (Windows Sonic, Dolby Atmos for Headphones, DTS Headphone:X).
  - Channel order: WAVE/WASAPI order (L R C LFE BL BR SL SR) for most drivers, ALSA's own (L R RL RR C LFE SL SR) on ALSA.
  - The tab shows how many outputs the current device has and has on; **Use All of Its Outputs** turns on outputs 1–8.
  - A stereo-only device gets the Advanced Virtualizer instead. Headsets often offer 7.1 only with their surround mode, or the system's spatial sound, switched on.
- The tab also says what plays right now (for example "the Advanced Virtualizer, for headphones").

**Track menu**
- **New...** (Ctrl+Shift+N) adds one or more tracks in one go. Each row is a count, a format (mono or stereo), a type and a name; + and - add and remove rows. The tracks go after the selected track (inside its folder) or at the end. The types:
  - audio track, aux input (a bus), instrument track or folder (audio tracks, aux inputs and folders in any format: mono, stereo, 3.1, 5.1, 7.1, Dolby Digital or DTS);
  - **MIDI track**: MIDI clips (and live MIDI when armed) that play the instrument track picked in its menu (*MIDI Output*). Muting it silences its notes; soloing the instrument keeps it playing.
  - **VCA Master**: no audio of its own. Its fader (and volume automation) scales its member tracks, its mute mutes them and its solo solos them; VCAs can be nested. Assign tracks with *VCA* in the track menu (it applies to the whole selection).
  - **Master Fader**: one last fader, with mute, volume automation and post-fader devices, on the main output or on a bus (*Controls* in its menu; one per path).
  - **Video Track**: video clips for scoring to picture (see *Video* below).
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

**Video**
- **File > Import Video...** (Ctrl+Shift+I), or drop a video file on the arranger.
  - The video goes onto a video track (the one it's dropped on, or the first one; a new one at the top if there is none), at the playhead or the drop point.
  - The **Video window** (*View > Video*, Ctrl+Shift+V) opens. It shows the picture at the playhead while playing, locating and scrubbing, with the timecode (hh:mm:ss:ff at the video's frame rate) and the clip's name.
- Video clips show a **filmstrip** of their frames and edit like audio clips: move, trim, split, copy and paste, Edit and Time Lock. Muting a video track hides its picture, and the topmost video track with a clip at the playhead is the one shown.
- Frames are decoded in the background, so the picture never holds up the interface or the audio.
- Formats:
  - **Motion-JPEG AVI** on every platform (built in).
  - **MP4, MOV, MKV, WebM, AVI** (H.264, HEVC, VP9, ProRes, ...) with **FFmpeg** on Linux, found at build time (`libavformat-dev libavcodec-dev libswscale-dev`, installed by `scripts/setup-linux.sh`).
  - **Windows Media Foundation** on Windows (MP4, MOV, AVI, WMV with H.264, HEVC, ...).
- The demo project has a short picture track.

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

**Clip menu** (also in the clip's right-click menu; each command applies to all selected clips, and selecting a clip of a clip group selects the whole group)
- **Edit Lock / Edit Unlock** (Ctrl+Shift+L): an edit-locked clip (padlock in its title bar) can't be cut, deleted, split, trimmed, faded or moved (in time or to another track). Its notes, gain, stretch and pitch are read-only in the editor. Copies of it, pasted to any track or time, are edit-locked too. (It locks the selected clips, or unlocks them if all are locked; Time Lock likewise.)
- **Time Lock / Time Unlock** (Ctrl+Alt+L): a time-locked clip (clock in its title bar) stays at its time location. It can't be moved in time or deleted, but it can be trimmed, split or dragged to another track at the same time. When it is split, both parts stay time-locked.
- Recording over a locked clip leaves it as it is. Delete and cut skip locked clips, which stay selected.
- **Rating** (None, 1–5; Ctrl+Alt+0…5): rate takes from 1 (lowest) to 5 (highest); none is the default. Ratings show as stars in the clip's title bar (or on its group's bar), and *View > Clip Ratings* shows or hides them.
- **Group** (Ctrl+Alt+G): makes a clip group of the selection: audio, MIDI and video clips on one or more tracks, and other clip groups (which nest inside it). A group has one title bar with its name over its clips; it is selected, moved, copied and deleted as a whole, and its clips can't be trimmed or faded until it is ungrouped.
- **Ungroup** (Ctrl+Alt+U): unpacks the top layer of the selected groups only; nested groups stay. **Regroup** (Ctrl+Alt+R) puts back what the last Ungroup took apart (after you edited its clips). **Ungroup All** removes every layer at once.
- **Loop...** (Ctrl+Alt+O): an audio, MIDI or video clip repeats a number of times (a loop arrow in its title bar, a notch where each iteration starts). Drag its right edge to draw out more (or fewer) loops; its left edge moves it. **Unloop** turns it back into one standard clip per iteration, which can be edited one by one.
- **Capture...** (Ctrl+Alt+C): the selection becomes a new clip in the **Clip List**: a range dragged over in an empty part of the lanes (the marquee is also the time selection, shown as a band on the tracks it covers), or else the selected clips. It works on one track, and across clips only when they play contiguous material of the same file (as the parts of a split clip do), or within one loop of a looped clip. Drag the clip from the Clip List onto a track to place it there.
- **Rename...** (Ctrl+Shift+R): renames the clip. A renamed clip that editing created (a split part, a copy, an import) becomes a user-defined clip, listed in the Clip List even when auto-created clips are hidden.
- **Identify Sync Point** (Ctrl+Alt+S) at the playhead: a small down arrow at the bottom of the clip with a light grey line, and the sync point's SMPTE time (30 fps) when the clip is selected. Moving the clip snaps the sync point, not the start, to the grid, and trimming the start keeps it on the same audio. **Remove Sync Point** takes it away.
- **Quantize to Grid** (Ctrl+Alt+Q): moves the selected clips so that their sync point (or start, when there is none) is on the nearest line of the grid chosen in the transport's grid pop-up. Time- and edit-locked clips stay.
- **Clip Gain** > **Bypass Clip Gain** (Ctrl+Alt+B): the clip plays without its clip gain; the setting is kept (shown as "(bypassed)" next to the gain). **Render Clip Gain** writes the clip's audio with its gain applied to a new file (in "<session> Rendered" next to the session), plays that instead and resets the gain to 0 dB. Undo brings back the original.

**Clip List** (the **CLIPS** tab, F6). It lists the **whole-file clips** (one per imported, recorded or rendered file, in **bold**; a clip on a track that plays a whole file is that clip), captured clips, user-defined (renamed) clips, the MIDI clips on tracks and the clip groups. **Auto-created** clips, made as a by-product of splitting, copying, pasting or unlooping (and audio clips that play part of a file), are listed only when asked for. Drag rows (several at once) onto a track to place them there one after the other; a clip dropped on a track that can't hold it, or a group's clip past the last track, gets a new track of the right kind; groups keep their layout. Double-click a row to rename it. Clips selected in the list are selected in the arranger too. The **CLIP LIST** button (or a right-click) opens its menu:
- **Show**
  - Clip types: **Audio**, **MIDI**, **Video**, **Groups**, **Auto-Created**.
  - Clip information: **Color** (the colour of the track the clip is on), **File Type** (audio, MIDI, video or group), **Timebase** (MIDI is tick-based, audio and video sample-based), **Processing State** (an Elastic Audio mark on time-stretched or pitch-shifted clips), **File Name**, **Disk Name** (the volume label and drive on Windows, the mount point on Linux).
- **Select**: **All**; **Unused** (not placed on any track); **Unused Except Whole Files**; **Offline** (their files can't be found). Offline clips are italic and dimmed in the list, and light blue with italic names in the arranger.
- **Sort By**: Clip Type, Name (natural order: "Take 2" before "Take 10"), Length, Original Time Stamp, User Time Stamp, Timebase, Start in Parent, End in Parent; for audio and video also File Name, File Length, File Creation Date, File Modification Date, Disk Name, Track Format/Width, Channel Name and Scene and Take (from a file's iXML, or names such as "Sc12 Tk3"). **Ascending** or **Descending**. MIDI clips have no file values and go last.
- **Auto Rename...**: Name (the root), Number (to start from), Zeros (digits at least) and Suffix. The selected clips are renamed in order ("Verse_007.v", "Verse_008.v", ...), and clips later made from them by splitting, duplicating, pasting or unlooping continue the numbering. It changes clip names only, never the files.
- **Time Stamp...**: shows the clip's **original time stamp** (when its first sample was recorded: a file's BWF time reference, or where it was recorded or first placed, plus the clip's start in the file) and sets its **user time stamp** (SMPTE, 30 fps; *Current Time* takes the playhead's). With several clips, the others move by as much as the first. Both are saved with the session, and splits keep them consistent.
- **Rename...**, **Export Clips as Files...** (audio as 32-bit float WAV of the clip's part of the file, MIDI as a Standard MIDI File; video is skipped), **Remove from Clip List** (whole-file and captured clips; the clips on tracks stay).
- The options are remembered.
- Not available yet: Pro Tools-style **playlists** (alternate takes on a track and its playlist lanes), so there is no showing or hiding of playlist lanes by rating. Ratings are saved with each clip, ready for that.

**MIDI sequencing**
- **Piano roll**: draw, move, resize and delete notes; velocity lane; **controller lanes** (mod wheel, volume, pan, expression, sustain, pitch bend); **quantise** with strength and optional note ends.
  - **Scroll bars** move left and right through the clip (and up and down through the notes); also Shift+wheel, and Ctrl+wheel zooms.
  - **Click a key** on the keyboard to hear its note through the track's instrument, for as long as you hold the mouse button: long samples and sustained synths keep sounding until you let go. Further right on a key plays louder; slide up or down to play other keys. It works with the transport stopped and the track not armed, also on edit-locked clips (MIDI tracks play through their instrument track), and nothing is recorded.
- **Step sequencer** view of the same clip: rows are the Drum Rack's pads or Impulse's slots (or 16 notes), click or paint steps, right-click for velocity. Click a row's name to hear it while held.

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
  - **Own Tempo**: off (the default), the click always plays at the project tempo, whatever its own **Tempo** says. On, it plays at its own Tempo (20–400 BPM, two decimals), independently of the project: **x2** and **/2** double or halve it (and turn Own Tempo on), **= Project** copies the project tempo into it. The panel shows which tempo the click is playing at. Its beats count from the start of the timeline at its own tempo.
  - **Tempo detection**: the Click's panel lists the audio tracks. Tick one, or several (kick, snare, overheads...); selecting audio tracks in the arranger ticks them. **Detect Tempo** measures each track to two decimal places (for example Kick 121.12, Snare 121.48) and averages them.
    - If the average differs from the project tempo, it asks whether to set the project to it. **Set Project Tempo** does that later (one undo step). **Set Click Tempo** gives the detected tempo to the click alone (Own Tempo), leaving the project as it is.
    - The **range** (70–180 BPM by default) decides between half and double time; **/2** and **x2** correct the result afterwards. A track read at half or double the others' tempo is folded to agree before averaging, and shown as such.
    - How it works:
      - Onsets come from the spectral flux of the whole spectrum and of the band below 300 Hz (kick and snare body).
      - A comb over the onsets' autocorrelation, with a tempo prior around 120 BPM, gives the beat.
      - Each attack is timed to the sample on the energy envelope and numbered on the sixteenth-note grid.
      - A least-squares line through (number, time), weighted by onset strength and refitted without outliers, gives the tempo. Over 20 s of a steady part it is accurate to well under 0.01 BPM.
    - Clips count as placed: offsets, stretch and loops included. MIDI tracks follow the project tempo, so they are not measured.

- **Channel EQ**: a console channel strip: Low Cut (80 Hz), Low shelf (100 Hz), a Mid bell sweepable from 120 Hz to 7.5 kHz with console-style proportional Q (broad for small moves, narrower for big ones), High shelf (8 kHz) and output, with its response curve.
- **Auto Filter**: an analog-modelled filter swept by an LFO and an envelope follower.
  - Low-, high-, band-pass, notch, or **Morph** (from low-pass through band-pass and high-pass to notch); 12 or 24 dB.
  - Circuits: **Clean** (exact), **Warm** (OTA, its resonance saturates), **Ladder** (four-pole transistor ladder, 2x oversampled) and **Diode** (screaming resonance); Drive pushes them.
  - LFO: eight shapes, free or synced (1/64 to 8 bars), offset and stereo phase (180 degrees: one side opens as the other closes). The envelope follower opens or closes it with the level, of the input or of a **sidechain** track. The curve shows the cutoff moving.
- **Gate**: Threshold, Return (hysteresis, so a signal at the threshold does not chatter), Attack, Hold, Release, Floor, **Flip** (passes only what is quiet), **Lookahead** (opens before a transient; reported as latency), an external **sidechain** through high- and low-pass key filters, and Listen.
- **Multiband Dynamics**: three bands (Linkwitz-Riley crossovers that add back up flat), each with an **Above** threshold and ratio (over 1:1 compresses downward, under 1:1 expands upward) and a **Below** threshold and ratio (over 1:1 expands downward, under 1:1 compresses upward), gain, solo and on/off. Attack, release, peak or RMS detection, soft knee, Amount, output and dry/wet; the display shows each band's level, thresholds and gain change.
- **Roar**: three saturation stages for anything from subtle warmth to wild degradation.
  - Eleven shapes per stage (Soft Sine, Diode Clipper, Tube Preamp, Polynomial, Hard Clip, Rectifier, Tri Fold, Sine Fold, Bit Crusher, Fractal, Noise Injection), with amount, bias, a filter before or after it (low-, high-, band-pass, notch, peak) and a level; 2x oversampled.
  - Routing: **Single**, **Serial**, **Parallel**, **Mid/Side** (stage 1 on the mid, 2 on the side) or **Multiband** (one stage per band).
  - Modulation: two LFOs, an envelope follower and a smoothed random source through an 8-slot matrix (drive, tone, every stage's amount, bias and filter, feedback, dry/wet).
  - Feedback through a delay and a band-pass (time or synced, invertible), a glue compressor, output and dry/wet.
- **Pedal**: Overdrive (the clipping stage sees only the mids, blended with the clean signal), Distortion (harder, more gain) and Fuzz (biased transistor clipping that sputters); Bass, Mid (500 Hz or 1.5 kHz), Treble, Sub, output and dry/wet; 4x oversampled.
- **Overdrive**: a 1970s-style overdrive pedal: a band-pass in front (frequency and width), drive into soft diode clipping, tone, and **Dynamics** (how much of the input's attacks and swells come back through the clipping).
- **Redux**: sample-rate reduction (a jittering clock, with pre- and post-filters that can be switched off for full aliasing) and bit reduction (fractional bits, Shape for more resolution near zero, DC Shift for truncation grit).
- **Shifter**: pitch shifting (+-48 semitones, window size), single-sideband **frequency shifting** (inharmonic, bell-like; negative shifts down) and **ring modulation**; stereo spread, an LFO (free or synced, stereo phase), an envelope follower, and a feedback delay for rising or falling spirals.
- **Chorus-Ensemble**: **Classic** (two voices), **Ensemble** (three voices 120 degrees apart with a fast shimmer, string-machine style) or **Vibrato**; rate, amount, feedback (or inverted), width, warmth, a high-pass on the wet signal, output and dry/wet.
- **Auto Pan**: rhythmic volume and panning: amount, rate or synced division, eight shapes, Phase (180 degrees pans, 0 is a tremolo), Offset, Invert and Square (hard switching for choppy gating).
- **LFO**: moves any knob or slider in the project, and they move on screen as it plays.
  - **Map...** lists every track and the master: its fader, pan and sends, and every parameter of every device and plug-in on it. **Learn** outlines every device control in the project; click one to map it.
  - Up to eight targets, each with its own range (min and max). Removing a mapping puts the target back where it was; mappings are saved with the project and undoable.
  - Eight shapes, rate or synced division (locked to the song position), depth, offset, phase, jitter, smooth, hold, retrigger and invert. Bypassing it hands the faders back to their own settings.
  - The mixer's faders and pan knobs follow while an LFO, or automation, moves them; their values show in the accent colour.

**Instruments**
- **Wavetable**: two morphing wavetable oscillators (eight band-limited tables), unison up to 8, sub and noise, multimode filter, three envelopes, two LFOs and an 8-slot modulation matrix.
- **Operator**: 4-operator FM with 11 algorithms, per-operator waveform, ratio or fixed frequency, envelope and velocity, feedback, filter with envelope, pitch envelope and LFO.
- **Sampler**: polyphonic, ADSR, root note, tuning, start/end and loop; drop a file on it.
- **Drum Rack**: 16 pads (drag samples onto any pad from the browser or the file manager), per-pad volume, pan, pitch, choke groups and **outputs** — one click creates a return track for each pad output (multi-out).
- **Impulse**: 8 sample slots (drag onto any slot) with start, transpose, drive, transient, decay, filter, pan, volume and velocity per slot; slots 7 and 8 can choke each other (hi-hats).
- **Analog**: vintage subtractive synthesis.
  - Two band-limited oscillators (sine, saw, rectangle with width and PWM, noise) with octave, semitone, fine tuning, pitch modulation by LFO 1, and a balance between the two filters; oscillator 2 can hard-sync to oscillator 1. Noise with colour.
  - Two filters (12 dB state-variable and 24 dB transistor-ladder low-, band- and high-pass, notch) with drive, key tracking, an envelope each and LFO 2, in parallel or in series.
  - Two amplifiers with their own envelopes, levels, pans and tremolo; two LFOs (free or synced) per voice.
  - Delayed vibrato, unison (up to 4), up to 16 voices or mono with legato, glide, velocity to level and filter, and **Drift**: every voice slightly, and slowly changingly, out of tune.
- **Drift**: a compact, **MPE**-ready analog-modelled synth.
  - Oscillator 1 (Sine, Triangle, Shark Tooth, Saturated, Saw, Pulse) with a Shape control modulated by Env 2 or the LFO; oscillator 2 (sine, triangle, saw, square) with octave and detune; noise.
  - A filter (LP I 12 dB, LP II 24 dB ladder, high-pass) and a high-pass after it; Env 1 for the level and Env 2 for modulation, or **cycling** as a loop.
  - An LFO with nine shapes (including one-shot envelopes), a 3-slot modulation matrix (Env 1, Env 2, LFO, velocity, key, pressure, slide, mod wheel, random), voice modes Poly, Mono, Stereo and Unison with Spread, glide and legato.
  - **Drift** gives each voice its own slightly-off, wandering pitch and filter. MPE: each note's own bend (48 semitones by default), pressure and slide.
- **Meld**: a bi-timbral, MPE-capable macro-oscillator synth for textures, drones and harmonic pads.
  - Two engines, **A** and **B**, **layered** on every note or **split** across the keyboard. Each has a macro oscillator, tuning, a filter (four circuits), amplitude and modulation envelopes, two LFOs, level and pan.
  - Thirteen macro oscillators, each with two macros: Basic Shapes, Swarm, Harmonic FM, Inharmonic FM, Fold FM, Squelch, Pluck, Bitgrunge, Noise Loop, Shepard's Tone (endlessly rising or falling), Chip, Additive and Rain (granular droplets).
  - A 12-slot modulation matrix: both engines' envelopes and LFOs, velocity, key, pressure, slide, pitch bend, mod wheel and a random value per note, to either engine's macros, pitch, cutoff, resonance, level, pan and LFO rate. MPE: each note's own bend, pressure and slide.

**Automation and export**
- **Spline automation lanes** for volume, pan, sends and every device or plug-in parameter (A on the track header, or right-click > Add Automation Lane).
- **Export** (Ctrl+R): offline (faster than real time) or real-time bounce, **WAV** (16/24/32-bit float), **FLAC**, **MP3** (LAME, 128–320 kbps), the mix and/or **stems** (one file per track).

**Plug-ins**
- **VST3 and CLAP** on both Windows and Linux: parameters (automatable), state saved with the project, their own editor windows, latency reported to the delay compensation.
- **Out-of-process scanning**: each plug-in is opened by a separate scanner process, so one that crashes or hangs is blacklisted instead of taking The Dojo down.

**Preferences** (*Options > Preferences*, Ctrl+,)
- **General**: *Return the view to the start with the playhead* (Stop twice, the return button or Home also scrolls back to bar 1); *Stop at the end of the arrangement* (playback stops at the end of the last clip, and the next Play starts again from bar 1, the view too with the first option; a loop still loops, recording carries on).
- **Themes**: Standard Black and White; Japanese (Dojo Night, Washi, Aizome); Cherry Blossom (Sakura Night, Sakura Day); Manga (Manga Ink, Manga Noir, Shonen Pop); Sour, inspired by an album cover's pastel pop-punk look (Sour Lavender: lavender, sticker pink, smiley yellow and lime; Sour Midnight: the same after dark, with a baby-blue playhead). Applied instantly, remembered.
- **Audio** (also *Options > Audio Settings*) and **Keyboard**: every shortcut, editable and remembered (also *Options > Keyboard Shortcuts*, F1).
- **Surround**: surround monitoring: Standard, Classic Fold-Down, the Advanced Surround Virtualizer (headphones or speakers, room) or the Device 3D Virtualizer (see *Surround monitoring* above).

**Presets** (`.dojopreset` files): every device, native or plug-in, can save its settings as a preset and load one back.
- A preset keeps **all** of a device's settings at the moment it is saved (for a native device every parameter by name and value, readable as XML like a `.dojo` session; for a plug-in its own state).
- One preset file can hold settings for **several devices** that serve the same purpose, one entry per kind of device. A device loading it takes **its own** entry (an EQ the EQ settings, a Channel EQ the Channel EQ settings), and a preset with nothing for a device says what it is for instead.
- The **≡ button** in a device's header opens its presets menu:
  - the **built-in presets** and **My Presets** that fit that device (click one to load it);
  - **Save Preset...**: this device's settings as a new preset file;
  - **Add to Preset File...**: adds this device's settings to an existing preset file (replacing the settings for this kind of device if it already has some), so one file can serve several devices;
  - **Save Whole Chain as Preset...**: the track's whole device chain, in order, as a chain preset (see below);
  - **Load Preset File...** and **Open Presets Folder**.
- **Drag and drop**: drop a preset onto a device (from the browser's **PRESETS** list, or a `.dojopreset` file from the file manager) to load it into that device. The device is outlined green when the preset fits, red when it does not. Dropped on the Devices panel's empty space, a preset adds a device that starts from it; dropped on a track in the arranger (or double-clicked in the browser), it loads into the first device on that track it fits, or else adds one.
- Loading a preset is one undo step.
- Your presets live in *Documents/The Dojo/Presets* (sub-folders too) and show up in the browser and the presets menus as soon as they are saved or copied there.
- **Built-in preset: Synth Bite** (for the EQ and the Channel EQ). Makes a sound more aggressive, present and upfront in the mix: use it when an instrument is too soft, too dark, or masked by the tracks around it. One file, two versions, each tuned for its device to the same result:
  - **EQ**: low cut at 90 Hz (no sub rumble), -3 dB at 350 Hz (Q 1.1, less boxy mud where parts mask each other), +6 dB at 3 kHz (Q 0.8, the bite), a +4.5 dB high shelf from 7 kHz (no longer dark) and -1.5 dB output, so the change is not just "louder".
  - **Channel EQ**: low cut (80 Hz), low shelf -3 dB, mid +6 dB at 3 kHz, high shelf +5.5 dB, output -1.5 dB.
  - Net result on either device: about -18 dB at 35 Hz, +4.5 dB at 3 kHz and +2.5 to +3.5 dB from 10 kHz up.

**Preset chains**: a chain preset is a whole device chain in one `.dojopreset` file: every device in order, each with its own settings and its role in the chain as its name ("Subtractive EQ", "Slapback Delay"). A chain can use the same kind of device more than once.
- To add one, double-click it in the browser's **PRESET CHAINS** list, drag it onto a track or the Devices panel, or pick it in **+ Add Device > Preset Chains**. Its devices go after the track's own, and adding them is one undo step.
- Dropped onto one device, a chain loads only that device's part. The device takes the entry of its kind at its own position, so the second EQ on a track takes the chain's second EQ.
- Your own chains come from **Save Whole Chain as Preset...** and show up next to the built-in ones.
- **Built-in chains** (each device's settings are in the file, readable):

  | Chain | For | Devices |
  | --- | --- | --- |
  | **Punchy Drum Bus** | Drums | Drum EQ (low cut 30 Hz, +2.5 dB at 60 Hz, -3 dB at 420 Hz, +2.5 dB shelf from 8 kHz) > Glue Compressor (4:1, 25 ms attack, 65 % parallel) > Saturation (Overdrive, 30 %) > Bass Mono (below 120 Hz) > Soft Clipper (Limiter, -1 dBFS) |
  | **Dusty Lo-Fi Drums** | Drums | Sampler Grit (Redux, 26 kHz, 12 bit) > Crunch (Roar, tube preamp) > Smash (10:1, 40 % parallel) > Dusty Filter (Auto Filter, warm 6.5 kHz low-pass, slow wander) > Thump EQ (+3 dB low shelf, -2 dB at 250 Hz) |
  | **Pop Vocal Polish** | Vocals | Expander (Gate, -20 dB floor) > Clean-Up EQ (low cut 100 Hz, -2.5 dB at 300 Hz, +2.5 dB at 3.5 kHz, +3 dB air) > Vocal Compressor (3.5:1) > De-Esser (Multiband, above 6 kHz) > Quarter Echo (ping-pong, ducked) > Vocal Plate |
  | **Upfront Rap Vocal** | Vocals | Tight Gate > Vocal EQ (low cut 110 Hz, -3 dB at 250 Hz, +3.5 dB at 2.8 kHz) > Fast Compressor (8:1, 2 ms) > Grit (Pedal, 25 %) > De-Esser > Eighth Throw (Delay, 1/8 ping-pong) > Level Lock (Limiter, -1 dBFS) |
  | **Drivers License Vocal Saturation** | Vocals | see below |
  | **Dreamy Wide Space** | General | Clean Lows (low cut 120 Hz) > Ensemble (Chorus-Ensemble) > Dream Echo (dotted-quarter ping-pong, wobble, into reverb) > Big Hall (6 s) > Wider (140 %) |
  | **Old Radio** | General | Band-Pass (350 Hz - 3.8 kHz, honk at 1.5 kHz) > Tube Distortion (Overdrive) > Low Rate (Redux, 11 kHz, 10 bit) > Mono |
  | **Gritty Bass Weight** | Bass | Bass Compressor (4:1) > Mid Grit (Roar multiband: clean lows, tube-driven mids, tamed highs) > Weight & Growl (+2 dB at 80 Hz and 700 Hz) > Mono Lows (below 150 Hz) |
  | **Shimmer Bloom** | Pads & Keys | Clean Lows (low cut 150 Hz) > Octave Shimmer (Shifter +12, feeding back on itself) > Chorus > Shimmer Hall (9 s) |

- **Drivers License Vocal Saturation** is the intimate, saturated bedroom-pop vocal, in seven stages:
  1. **Noise Gate** (Gate): -50 dB threshold, 1 ms lookahead, 60 ms hold, fully closed between phrases. It shuts out computer fans, AC hum and street noise the moment you stop singing.
  2. **Subtractive EQ**: a low cut at 90 Hz against room rumble, and -3.5 dB at 450 Hz (Q 1.4) against the "boxy" sound of a bedroom.
  3. **Fast Compressor**: 6:1 with a 1 ms attack and 70 ms release pins the vocal down. Whispered verses stay intimate and belted choruses stay in check (34 dB of input range comes out about 20 dB apart).
  4. **Saturation** (Overdrive, the core sound): drive 20 %, mixed in at 25 %. It is subtle: a little warmth and harmonics.
  5. **Air EQ (Red Light)**: +5.5 dB high shelf from 10 kHz, the "expensive preamp" air.
  6. **Slapback Delay** (Echo): 92 ms on the left, 118 ms on the right, almost no feedback, filtered and at 18 %. Instead of a long reverb that gives the small room away, it adds space and width without pushing the vocal back.
  7. **Short Plate** (Reverb): small, bright (little damping) and 0.9 s long at 14 %. A controlled, vintage studio decay that glues the saturation and the delay together.

**Projects**: `.dojo` files with relative audio paths (move the folder freely); device and plug-in state are saved inside. *File > Open Demo Project* builds a short song that uses every kind of track and device.

## Using it

1. Drag audio files onto the arranger, or add an instrument track (**+ Add Track**, or Ctrl+Shift+T) and double-click its lane to create a MIDI clip.
2. Press **Space** to play, **R** to record onto armed tracks, **L** to loop.
3. The bottom panel has four tabs: **Mixer** (F3), **Devices** of the selected track (F4), the **Editor** (F5) for the clip you double-clicked and the **Clip List** (F6). In the mixer, double-click the volume or pan value under a fader (or the master's GAIN) to type it: `-6`, `-inf`, `C`, `L20`, `R35`.
4. The browser lists devices, plug-ins and presets at the top (with search; drag one onto a track, a device or the Devices panel, or double-click it) and your own folders below: add a folder with **+** (or drop one from the file manager), search them, and click a sample to **preview** it (arrow keys walk through files; the headphones button turns preview off). Drag samples onto the arranger, a Drum Rack pad, an Impulse slot or the Sampler. Scan for plug-ins with **Scan** / **...**.
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
| F3 / F4 / F5 / F6 | Mixer / Devices / Editor / Clip List |
| Ctrl+Alt+G / U / R | Group, ungroup, regroup clips |
| Ctrl+Alt+O, Ctrl+Alt+C, Ctrl+Shift+R | Loop, capture, rename clips |
| Ctrl+Alt+S, Ctrl+Alt+Q, Ctrl+Alt+B | Identify sync point, quantize clips to grid, bypass clip gain |
| Ctrl+Alt+0 … 5 | Clip rating (none, 1–5) |
| Piano roll: Q, arrows, Delete | Quantise, move / transpose selected notes, delete |
| Ctrl+G | Toggle snap |
| Ctrl+= / Ctrl+- / Ctrl+0 | Zoom in, out, to fit |
| Ctrl+N / Ctrl+O / Ctrl+S | New, open, save |
| Ctrl+, | Preferences (playback options, themes, audio, shortcuts) |
| Ctrl+Shift+A | Audio settings (device, inputs, MIDI inputs) |
| F1 | Keyboard shortcuts (change them there) |
| F11 | Full screen on-off (also *View > Full Screen*) |

Mouse:
- Right-click clips, track headers, automation lanes or empty lanes for context menus.
- Drag in an empty part of the lanes for a rubber band: it selects the clips it touches and is the time selection (for Capture). Ctrl or Shift add to the selection.
- Shift+wheel scrolls sideways; Ctrl+wheel zooms (also in the piano roll).
- Double-click a fader, knob or slider to reset it.
- The window's square button (or a double-click on its title bar) maximises it to the screen's work area and restores it again. From full screen (F11), F11 or the square button goes back to the window as it was, maximised or not.
  - On Linux this goes through the window manager (the EWMH maximised and full-screen states), so it also works on KDE, GNOME, XWayland and other window managers that maximise a window themselves: a window the window manager has maximised (at start, by a shortcut or by its own title-bar double-click) is restored by the square button to a normal-sized window.

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
| Video decoding | Built-in Motion-JPEG AVI reader; [FFmpeg](https://ffmpeg.org) (LGPL, the system's shared libraries, optional) on Linux; Media Foundation on Windows |
| Audio back-ends | Windows: ASIO, WASAPI, DirectSound. Linux: ALSA (incl. PulseAudio/PipeWire via ALSA), JACK (incl. PipeWire-JACK) |
| Tests | Catch2 v3 |

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the reasoning, the threading model and the real-time rules.

## Repository layout

```
cmake/        build options, compiler policy, pinned dependencies
docs/         architecture
resources/    embedded fonts and built-in presets (presets/*.dojopreset)
scripts/      setup and build helpers (bash + PowerShell)
src/core      real-time primitives (lock-free SPSC queue, RT allocation trap), musical time
src/engine    AudioEngine (audio thread + worker pool), render graph, routing, delay compensation,
              clip playback / time-stretch / disk streaming, capture, file preview, device host,
              MP3 encoder
src/devices   native devices: EQ, Channel EQ, Auto Filter, compressor, limiter, Gate, Multiband
              Dynamics, reverb, delay, Echo, Utility, Amp & Cabinet, Roar, Pedal, Overdrive, Redux,
              pitch shift, Shifter, Chorus-Ensemble, Auto Pan, LFO, Click; instruments: Wavetable,
              Operator, Meld, Analog, Drift, Sampler, Drum Rack, Impulse
src/plugins   plug-in hosting (VST3 via JUCE, CLAP host), plug-in list and out-of-process scanning
src/scanner   dojo-plugin-scanner, the scanner child process
src/video     video decoding for video tracks: built-in Motion-JPEG AVI, FFmpeg (Linux),
              Media Foundation (Windows); background frame server for the Video window and thumbnails
src/session   the document (tracks, clips, MIDI, devices, sends, automation) with undo, audio
              decoding/streaming, device instances, graph compiler, recorder, exporter, save/open,
              device presets (.dojopreset)
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
10. ✅ **Post-production**: surround formats (3.1, 5.1, 7.1, Dolby Digital, DTS) with a surround panner, video tracks and a Video window

## Licensing

No licence has been chosen for The Dojo yet. Note that JUCE is AGPLv3 or commercial, the bundled ASIO SDK is GPLv3 or a Steinberg licence, and LAME is LGPL (it is linked statically, which has obligations under the LGPL). See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#11-licensing-note). The embedded Inter and JetBrains Mono fonts are under the SIL Open Font License; see `resources/fonts/`.
