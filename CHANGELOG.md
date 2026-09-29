# Changelog

The project was released as **T5ynth** through v2.5.3. From 3.0.0 it is
**akróasys**; the repository, the preset format and the version line continue
unbroken.

## 3.1.0 — shipped as v3.1.0-beta.0, v3.1.0-beta.1

### The instrument

- **Every part of a written instrument is a whole instrument, and has its own
  fader.** An "a > b" transition and an "a through b" pair used to collapse into
  one part: the second end was written as whatever fed the crossfade, so its
  parameter lines went missing and only the first end reached a mix channel.
  Measured on "a vibraphone > a saw wave", the saw end came back as a single
  oscillator with one dead control where the library entry has five. The
  contract now asks for every part to be built in full, with every one of its
  parameter lines, and scales each by its own level — and a control frozen to a
  constant that nothing reads is handed back to the author like a compiler
  error, so the collapse cannot pass silently. Pitch stays shared for those two
  relations; they are one voice, and only the mixing is per part. Instruments
  written before this sound exactly as they did.
- **An LRO orchestra no longer stops after 100 hours.** The always-on voices
  were scored for 360 000 s; past that the performance ended and the render path
  kept reading Csound's frozen output — a held note became a standing 64-sample
  buzz, an idle synth permanent silence, until the next regeneration. The score
  now runs 2·10⁹ s (~63 years), and a preset written before today has its stored
  score rewritten as it is compiled, so it does not carry the old cliff either.
- **The Reading field says when an orchestra stopped its own performance.** A
  body can end the Csound performance from inside; the only symptom used to be
  silence. The engine latches that moment and the RUNNING station reports it with
  a warning dot, and regenerating the same text really does recompile instead of
  reporting success for an engine that can no longer sound.
- **The clarinet is a clarinet bore.** The library entry the LRO reaches for on
  a clarinet prompt is Csound's `wgclar` — reed, bore and bell — where it was FM
  standing in for one. Its first control is named for the blowing pressure it
  sets, not for the model's own argument, and the blown bottle's is corrected
  the same way. `wgclar`'s measured facts were re-read at the rate the plugin
  actually runs (4× oversampled), because the figures on record had been taken
  at 44100, which the oscillator never renders at.
- **Envelope curves became a continuous travel.** Each stage's shape was a
  five-entry choice (Log … Exp); it is now one bend control that passes through
  those five as named anchors and reaches beyond them on the release. Presets
  and event-log tapes written before this migrate to the matching anchor
  (`CalibrationMigration.h`, epoch 9). The ADSR display's three stages stopped
  sharing one width, so it draws what is set.
- **Shift is fine adjustment on every control that is dragged** — knobs,
  sliders, and the waveform brackets. It used to be on some and not others.
- **The LRO is at the level of the engines it is A/B'd against.** It was 12.4 dB
  quieter, so every comparison was decided by loudness before it was decided by
  sound.
- **Louder, and one level at every voice count.** The output gain no longer
  depends on the voice-count switch, so a single note is as loud at 16 voices
  as at Mono. Dense chords reach the standalone's output ceiling sooner.
- **MPE:** the zone layout is `juce::MPEZoneLayout`'s now, and an NRPN can no
  longer declare a zone or have its data byte read as a bend range. A parity
  suite freezes what the hand-written path could do.
- **The in-app manual** describes the instrument instead of defending it, says
  what the player sees rather than what the code does, and the Re-Prompt section
  says that its ear hears the bare oscillator, not the speakers.
- **A cache take records the same run on a fast and on a slow computer.** Filling
  a cache used to sample the Drift wherever the machine happened to be ready: a
  render in flight let the cadence point pass unused while the sweep carried on,
  so a quick computer caught the trajectory every bar and a slow one every few —
  and Resynth's carry and the Re-Prompt rewrite skipped the same steps with it.
  The new **A/S** switch in the CACHE row holds the generation parameters at each
  cache point until the render lands and then steps them on by exactly one
  Regenerate interval, so the recording is a property of the settings and not of
  the hardware. Everything audible the Drift reaches keeps moving throughout. A
  finished take plays back like any other cache, now including with a Re-Prompt
  stance engaged: the recording is that evolution, so it repeats rather than
  being re-rendered differently every cycle. The switch has the cells the 32- and
  64-deep caches used to hold — presets that deep ran to hundreds of megabytes —
  and the remaining depths gained the room to be readable at any window size. A
  preset saved at 32 or 64 still loads, with its cache kept at 16.
- **A Snap recall no longer crackles.** The voice sum fades out over 5 ms, the
  recall is written into that gap, and the sound fades back in over 5 ms. A new
  sample then arrives through the Regen XFade crossfade, as on a regeneration;
  an LRO slot's orchestra arrives through the engine's crossfaded swap. Delay
  and reverb tails ring on across the fade. The cost is a dip in the dry sound
  of roughly 13–17 ms on every recall, and the recall lands about one host
  period later than before. An offline render gets no fade. The digit keys 1–4
  recall on key-down only, so auto-repeat no longer recalls a slot again and
  again, and the synth panel lays itself out once per recall instead of once per
  changed choice box.
- **A setting that changes under a sounding note ramps instead of stepping.**
  A Snap recall, a preset, automation or a MIDI CC used to step these at a block
  or sub-block boundary, and the step clicked. Cutoff, key tracking, resonance,
  mix and drive, an envelope's target and amount, and master volume now reach a
  playing voice over 1 ms. A change of filter model, slope, type or on/off
  crossfades from the old filter to the new one, which first runs over the last
  10 ms of the voice's input so that it does not start from silence. The output
  gain moves along a smooth curve, and every gain ramp starts from the level
  last heard, also after bypass, after idle and after the host re-prepares the
  plugin.
- **Changing WT Frames no longer drops out.** The re-slice held the audio for
  its whole computation: 19–35 ms of silence on a recall that changed the frame
  count. It now computes beside the audio and runs once per change, whether the
  change comes from the box, automation, a MIDI CC, a Snap recall or a preset,
  and whether the editor is open or not. Opening the editor no longer re-slices,
  and a MIDI CC mapped to WT Frames can no longer lock the audio and the editor
  against each other.
- **The SVF's 6 and 18 dB slopes work at the top of the cutoff range.** With the
  cutoff at 20 kHz — the knob's default, and where key tracking clamps — they
  ran on an unset coefficient until the cutoff first moved on that voice: silent
  as a low-pass, the full band as a high-pass.
- **The reverb no longer runs away on a short block.** When a host called with
  fewer samples than it had prepared for, the reverb read its own previous
  output back in as input, and the algorithmic reverb grew without bound within
  about a second.
- **All Sound Off, All Notes Off and Reset All Controllers do what the MIDI
  spec asks.** A DAW sends them on transport stop and on locate, so this is heard
  in ordinary use, not only from a panic button.
  - All Sound Off (CC 120) cuts every voice within a few milliseconds. It used
    to release over the patch's own release time, up to 10 s.
  - All Notes Off (CC 123) is a key-up for every note. It resets no controller,
    and the pedals keep what they hold: with the sustain pedal down, the notes
    ring until it comes up.
  - Reset All Controllers (CC 121) leaves channel volume alone; with CC 7 at 40
    the instrument used to jump +10 dB. It no longer retunes or silences a note
    that is still sounding, or detaches it from its MPE channel.

  The panic button still cuts everything.
- **A panic leaves no arpeggiator or sequencer note behind.** Switching the
  arpeggiator on while a sequencer note sounded could leave that note droning
  until the next panic. After a panic, the sequencer's, the arpeggiator's and a
  drone's notes no longer answer the mod wheel or pressure through their
  release, and a sliding step no longer carries on a note the panic has ended.
  A sequencer step no longer cuts a key held on the same pitch.
- **Per-note MPE expression stays with the finger that plays it.** A controller
  that rotates its member channels hands a new key a channel on which a released
  note may still be sounding. That key's pressure, bend and slide used to drive
  the old note as well: it swelled back up, brightened again, or its bend
  jumped. Poly pressure now ends with the key: a note pressed hard, or pressure
  that arrived just after its key-up, no longer holds that note's pressure up
  for the rest of the session. A note held by a pedal, taken over by the
  arpeggiator or continued by a sequencer slide no longer answers someone else's
  finger. Still open: releasing a key on the pitch a drone holds can leave the
  drone at that key's last pressure.
- **Every row of the Expression column picks its own source.** Each target now
  has a source beside its depth: velocity, per-note bend (X), MPE Y (CC 74) or
  pressure. Y used to be wired to the cutoff alone, at ±4 octaves around an
  assumed centre, so on a controller that rests Y at the bottom (measured on an
  Osmose) every MPE note sat about four octaves dark. Y is now read as the travel
  from where the note began, so the same movement means the same thing whether
  a controller rests Y at the bottom or in the middle. A fresh patch routes level
  to pressure and cutoff and scan to Y, all at depth zero, and the pitch row
  routes X to the per-note bend; moving that row to another source switches the
  bend off. A preset written before sources existed plays as it did, because all
  of its depths meant pressure. LRO instruments that read the timbre control
  now see its rest at 0 rather than at the middle, so they can sound different
  at the same settings. An aftertouch bar's first drag no longer starts in the
  negative direction.
- **An MPE tab shows what the controller sends.** A controller announces its
  zone and bend range once, at power-on or when its MPE mode is chosen, and a
  plugin opened later never hears it. The new tab beside Settings shows which
  channels carry notes, whether a zone and a bend range were transmitted, how far
  the lateral lean reaches, CC 74's span and which kinds of pressure arrive. It
  sets the per-note and the master bend range by hand, and what a full lateral
  lean is worth as a modulation source. All three belong to the machine, not to
  the preset. Without a transmitted range, the per-note bend range is now the
  MPE spec's 48 semitones instead of 24, so such a controller bends over the
  whole interval it means instead of half of it.
- **Aftertouch travels the cache, one position per note.** The Cache row steps
  through the cache positions the way Re-Prompt steps, and with several notes
  held each follows its own position instead of all changing sample together.
  The LRO has a cache of its own: a slot holds an authored orchestra with its
  prompt, reading and knobs, so sixteen of them take a few kilobytes. The cache
  depth follows the Duration — 16 positions up to 12 s, 8 up to 24 s, 4 up to
  48 s, 2 up to 96 s — and a depth the Duration cannot afford is dimmed. While a
  control is dragged, the positions rebuild once the hand has rested for 250 ms,
  and never later than 2 s. A preset stores a cache only when it holds entries,
  and reopening akróasys no longer brings back the last sitting's cache. With
  several notes on different positions, the waveform and the Cache row's
  highlight show no single one.
- **The Expression column and the effect lamps show only what is active.** A
  row reads OFF while its depth is zero and it still stands where it shipped;
  a preset written before expression sources used to light fifteen rows. Delay
  and reverb at Mix 0 read OFF, a modulated Mix counts as in play, and the
  cell's fill still shows the switch a click toggles.
- **On the computer keyboard, a key struck again plays again, and no note starts
  by itself.** A key released and struck again within 20 ms made no second note.
  On macOS the system can report a key as held that nobody presses (measured:
  the key for C); that note used to sound and hang whenever another key was
  played, and go dead after a panic. A note now starts only on a keystroke.
- **Smaller fixes.** Letting the sostenuto pedal up no longer silences a note
  the sustain pedal still holds. In step recording on macOS, a modifier change
  while Space is held no longer adds a rest, and the Space rest no longer goes
  dead after its key-up went to another window; a Command tap while Space is
  held still adds one. The known-tags cloud in the preset drawer uses the room
  the drawer has instead of two rows.

### Under the hood

- **The 27 STK opcodes are available to Csound bodies on macOS.** `libstkops`
  was in the payload all along and declined to register because STK's data files
  had no `RAWWAVE_PATH` to be found through; the 41 `.raw` files now ship as
  `Contents/libs/rawwaves/` and both the plugin and the backend point the
  variable at them. The bundler and the verifier keep module and data together,
  because a file STK asks for and cannot find aborts the host process. No
  library entry uses one yet. What they are:
  [`docs/STK_OPCODES.html`](docs/STK_OPCODES.html).
- The LRO measurement tools now measure the Csound the app ships rather than
  whichever one the machine happens to have installed.
- The Windows backend smoke-test in CI no longer reads a still-locked log file
  as a backend that failed to start — the failure that broke the v3.0.0-beta.2
  tag run on a commit that had passed on `main` four minutes earlier.
- **Under CLAP, the audio thread and the editor no longer race.** The CLAP
  wrapper calls the audio callback without JUCE's callback lock, which
  everything that hands samples and wavetables to the audio thread relied on.
  The audio callback now takes the lock itself, and the sampler's audio path no
  longer takes a mutex to read a snapshot.
- **A note-on no longer allocates memory on the audio thread.** A switched-off
  debug log still built its message, eleven allocations per note. A new test
  counts allocations through the real audio callback, and it reads zero.

## 3.0.0 — shipped as v3.0.0-beta.0 … v3.0.0-beta.2

Two things make this a major version: the instrument is renamed, and it gains a
second oscillator built on a different principle from the first.

### New: the Language-Resonant Oscillator (LRO)

A second oscillator, peer to the existing one, in which **the sounding program
itself is generated at run time by a language model from a description in
ordinary language, then compiled and executed as the instrument's voice
source.** First landed 2026-07-22 (`3728a42f`).

The architecture:

1. The player types a description of an instrument ("a bowed cello", "bright
   shimmer degrading to a dark rumble"). No code, no parameters, no keyword
   vocabulary.
2. A **curated, parametrised library of synthesis code** is held beside the
   model — currently 30 instrument bodies, 51 sound-character words and 17
   motions, each entry carrying real Csound source, the synthesis method it
   implements, a published source for that method, and named parameters with
   ranges and one-line glosses (`backend/lco_library.json`).
3. **Two-turn consultation.** In the first turn the model is shown the library's
   index and *names the entries it wants opened*; nothing is matched to it by
   keyword and nothing restricts it to what it opened. In the second turn it is
   shown those entries in full and writes a complete Csound orchestra
   (`backend/lco_write.py`).
4. A **fixed host scaffold** is the only contract: sixteen voice channels, gate,
   frequency, velocity, pressure, timbre and trigger per voice, `ksmps = 64`,
   and the rule that the body shapes spectrum and timbre while the host's
   amplitude envelope owns loudness. The model writes the instrument; it never
   writes the transport, the polyphony or the envelope
   (`src/dsp/CsoundEngine.cpp`).
5. The orchestra is **compiled and performance-checked before it is allowed to
   play** — it must compile and produce non-silent output for a quarter of a
   second. A failure is fed back to the model as a repair round rather than
   falling back to anything.
6. **The player's control surface is derived from the generated program.** A
   parameter line from an opened library entry that survives into the body the
   model actually wrote becomes a knob on the panel, carrying the library's
   name, gloss and range and the model's chosen value; a line the body never
   reads does not become a knob (`src/dsp/LroControls.h`).
7. The compiled orchestra is **hot-swapped into the running audio engine**
   without an audible break, and travels inside the preset as source text.

The library orients the model rather than
constraining it: it may combine any number of methods in one program, layer up
to three bodies, morph one into another across a note, drive one with another,
or write something the library does not contain at all.

The full technical description, in the form a third party can read and cite, is
[`docs/LRO_TECHNICAL_DISCLOSURE.md`](docs/LRO_TECHNICAL_DISCLOSURE.md).

Milestones, dated: Csound detection and engine wrapper 2026-07-16 (`3d7663a4`);
voice bridge and playable engine mode 2026-07-17 (`cf21475a`); **the language
model writes the orchestra** 2026-07-22 (`3728a42f`); the panel derives its
knobs from the written body 2026-07-30 (`a8b45824`, `0e7f9709`); per-part levels
2026-08-01 (`8c36dc13`).

### New: the authoring trace

The LRO's panel shows the whole authoring path rather than a progress bar —
nine stations covering what the model was given, which library entries it asked
to have opened, its reasoning as it streams, the orchestra it wrote, a repair
round if the first attempt failed, and whether the result compiled and played.
A station that does not appear is itself information. Hold the panel to read the
Csound the model wrote.

### New: Csound ships inside the app

Csound 6.18.1 travels in the bundle on macOS, Windows and Linux, linked
dynamically as LGPL 2.1 requires. Nothing needs installing. Linux takes the
distribution's `libcsound64-6.0` in the Debian package and has no scanned
synthesis (`scanu`/`scanu2`/`scans`).

### New: one language model for the whole instrument

Gemma 4 12B (QAT, 4-bit GGUF, run through llama.cpp) writes the LRO's Csound,
translates prompts to English and drives Re-Prompt. Alternatively an external
provider — OpenRouter, Mistral AI, IONOS, Mammouth AI, Anthropic, OpenAI, a
local Ollama, or any OpenAI-compatible endpoint — with only the text step
leaving the machine. The separately-installed 1.5B translator is gone.

### Also in 3.0.0

- **CLAP plug-in format on macOS.** The `.pkg` now installs a fourth artefact,
  `akroasys.clap`, into `/Library/Audio/Plug-Ins/CLAP` — selectable in the
  installer like the VST3 and the AU. It is the same instrument: one processor,
  one parameter set, one preset format, built from the same shared code by
  [clap-juce-extensions](https://github.com/free-audio/clap-juce-extensions).
  Windows and Linux keep Standalone + VST3. (Not to be confused with CLAP the
  audio-text model that Re-Prompt listens with — same acronym, unrelated thing.)
- **MPE is now declared, not just implemented.** The synth has always parsed a
  full MPE zone from raw MIDI; it never told the host so. CLAP hosts and Logic
  now see it and send per-note channels. The zone layout is read from the MPE
  Configuration Message as well, so channel 16 is treated as the member channel
  it is in the fifteen-channel lower zone every host defaults to — one note in
  fifteen no longer sends its pressure and slide to every voice.
- **Five envelopes** instead of three, with COPY/PASTE between the tabs.
- **Amp effects chain** — distortion as an overdriven amplifier, tremolo with
  four shapes, chorus and phaser, behind the voices.
- **Advanced/Easy view removed.** The toggle that remains switches oscillators.
- **Every engine's shared controls in one top bar**, with the tuning selector
  and the octave switchbox.
- **Re-Prompt listens on both oscillators** — for the LRO it renders a probe of
  the compiled orchestra and hands that to CLAP, rather than re-reading its own
  text.
- **A preset restores what it does not say.** A preset file that leaves a
  setting out now loads that setting's default. Before, a missing entry was read
  as a literal zero — so a hand-edited, converted or older-format file could
  arrive with the filter closed down to 20 Hz, the amplitude envelope at no
  depth, the sequencer at 0 BPM, the loop reduced to a sliver, or the output
  three decibels down, none of which looks like a gap once loaded. Files this
  instrument writes itself carry every entry and load exactly as before; the
  rule third-party writers can rely on is now written down in
  `docs/PRESET_FORMAT.md`.
- **A preset's loop window no longer depends on which preset you loaded
  before it.** The two loop markers were applied one at a time, and each was
  held back by the other one's leftover position from the previous patch — so a
  preset whose loop begins later than the last one ended started somewhere it
  had never been saved, and stayed there, because the lock that preserves a
  preset's markers also stops the automatic re-bracketing that would have
  corrected it. Ten of the 46 presets in the UCDCAE AI Lab bank were affected.
  They now load the loop they were saved with, every time. Nothing about the
  files changes — they always held the right window.
- **Session log** (`.t5evt`) — opt-in recording and replay of a playing session.
- **The in-app manual rewritten** against the code, with the two oscillators as
  peers and background chapters on Csound's MUSIC-N lineage and on the code
  library.

---

## v2.5.3-beta.1 — 2026-06-30

- **Fix: CUDA on small-VRAM cards.** The prompt translator is pinned to CPU where VRAM is tight, and `T5YNTH_CUDA_FP32` forces fp32 when a card's fp16 path misbehaves.
- **Presets: cross-device presets are flagged** on the detail card, so a preset made on another machine's device announces itself before it is loaded.
- **Delay: `Tp4`** restores the legacy additive tape wobble beside the newer voicings.

## v2.5.3-beta.0 — 2026-06-29

- **Delay: Tape and BBD character presets.** The delay-type buttons become a combo box; each family carries named characters instead of numbered variants.

## v2.5.2-beta.1 / v2.5.2-beta.0 — 2026-06-26 / 2026-06-25

- **New: BBD delay mode**, with tape playback rolloff that darkens with delay time rather than only with age.
- **Delay: per-mode Damp** with an intrinsic tape baseline and an honest percentage label.
- **New: step recording.** Double-click *Step* to play notes into the grid; Space or a sustain pedal enters a rest.
- **Keyboard: the typing map reaches ~2 octaves** (`o l p ö ä ü # +`).

## v2.5.1-beta.0 — 2026-06-23

- **New: CORE MONITOR.** The generative sequencer's Strand 1 slot holds a read-only phosphor-green readout that names each pattern mutation as it fires — the operation, the result, and the rule that chose it.
- **Generative sequencer: Range as a compact switchbox**, per-strand group cards, roles and tempo multipliers in one row.

## v2.5.0-beta.1 / v2.5.0-beta.0 — 2026-06-22

- **New: the Delta panel.** Semantic Axes and the Dimension Explorer share one box behind a two-segment switch; the DimExplorer inherits the freed band and gains a binned |A−B| focus-spectrum mini-view.
- **New: looping envelopes** as a self-retriggering A→D→Hold→R cycle, with a Loop toggle in each envelope header. In loop mode Sustain becomes the hold time.
- **New: tabbed settings overlay** with the filter-oversampling control.
- **Drift: its own BPM-sync division range**, 1/4 … 64/1 — slower than the LFOs and the delay.
- **Filter: LP/HP/BP as a type toggle.**
- **Re-Prompt and Translate are gated** on the translation model actually being installed.

## v2.4.0-beta.0 — 2026-06-19

- **New: the delay is reworked** into Digital / Ping-Pong / Tape multi-head voicings.
- **New: graphical ADSR editor**, replacing the faders. Clicking a segment body cycles that stage's curve.
- **New: continuous signed per-stage velocity sensitivity.**
- **UI: module cards** for the FX Delay/Reverb sections and the generative sequencer's Euclidean controls.

## v2.3.0-beta.0 — 2026-06-12

- **New: Resynth.** A generation can start from a waveform instead of from pure noise and denoise away from there — an audio-conditioned feedback loop with an anti-convergence controller, an Off→Full slider, drift as a target, and preset/snapshot persistence.
- **Presets: a maintainer checkout writes bank presets directly**, so the live preset directory is the git checkout.

## v2.2.0-beta.1 / v2.2.0-beta.0 — 2026-06-06 / 2026-06-04

- **New: Stable Audio 3.** SA3 Small Music and Small SFX with the t5gemma text encoder, a model-metadata IPC channel, and the layer-split slider clamped to each model's own DiT block count. First landed 2026-05-26.
- **Filter: the Warp algorithm becomes true ZDF**, with per-style resonance recalibration and output makeup that holds level parity across styles.
- **Setup: per-row inline download progress**, cancel, and a `~/Downloads` pre-scan.
- **Fix: preset JSON is decoded as UTF-8**, not Latin-1 — the cause of prompt mojibake after a restart.

## v2.1.0-beta.1 / v2.1.0-beta.0 — 2026-06-03 / 2026-06-02

- **Held voices follow a new generation.** Granular and Freeze voices crossfade-morph live when inference returns, rather than being cut or frozen on the old buffer.
- **New: MIDI Panic** in the status bar.
- **Presets: "+ Bank" is decoupled from Save**, and a name that exists in another bank blocks the save rather than silently shadowing it.
- **UI: the scan playhead** is drawn in its own colour across the full waveform height.

## v2.0.2-beta.0 — 2026-05-23

- **New: curated tag vocabulary** with a click-to-add cloud and autocomplete; long-press a tag row to drag it onto the detail card.
- **Presets: edits to the UCDCAE bank fork to My Presets** with a mandatory rename, so the shipped bank cannot be edited in place.
- **Presets: session snapshots persist in `.t5p`**, and the FLAC v4 write/read path is active.

## v2.0.1-beta.0 — 2026-05-20

- **New: Semantic Axes master Amount**, a single attenuator over all three axis slots.
- **Fix: embedding-noise sigma halved** so the slider has usable resolution across its range.

## v2.0.0-beta.1 / v2.0.0-beta.0 — 2026-05-17

- **New: Easy view.** A second, compact layout for the whole instrument — generation column, oscillator, filter, LFOs and modulation — switched from the engine title. (Removed again in 3.0.0, where a single layout replaces both.)
- **New: true L/R stereo filter path.**
- **New: in-plugin sync of the UCDCAE AI Lab preset bank** from GitHub.

## v1.9.0-beta.1 — 2026-05-16

- **New: Granular engine.** Adds a third engine beside Sampler and Wavetable. Granular reuses loaded/generated sample material but renders through its own texture engine with pitch-independent grain timing, conservative texture macros, stereo spread, and sample-near defaults.
- **UI: Granular integration.** Engine selection now exposes Sampler / Wavetable / Granular, with the Granular controls aligned to the existing one-line engine menu style.
- **Fix: Granular normalisation and modulation targets.** Granular `Norm` now normalises the mono freeze input that the engine actually renders from, and Env/LFO target order is aligned around Filter / Scan / Pitch / Delay / Reverb / Noise.
- **UI: compact startup layout.** LFO, Drift, and Regenerate controls now size their menu/button cells from actual label text so the default window width does not crush Scan/Magnitude/max controls.

## v1.8.0-beta.1 — 2026-05-10

- **New: sequencer one-shot sample slots.** Each Step Sequencer step now has three compact sample slots for drum-style one-shots. Samples are copied physically from the current P1→P3 waveform region, each slot has Normal / Accent / Mute cycling, and the slots are saved inside presets together with their WAV data.
- **New: T5 oscillator snapshots.** The Generation column now has a `SNAP OFF / 1 / 2` switchbox beside the inference cache controls. Long-clicking slot 1 or 2 stores the current main sound and T5-Osc state from the beginning of the long click; short-clicking recalls the snapshot.
- **New: preset-persistent inference cache.** Presets now save cache capacity and already generated cache audio entries by default, then restore them on load so cached alternate generations survive the preset round-trip.
- **UI: compact Snap / Cache row.** Snapshot and cache controls share one switchbox-style row using the same bordered button format as the Engine controls.

## v1.7.0-beta.1 — 2026-05-01

- **New: BPM-sync for LFOs, Drift, and Delay.** Each of the three LFOs, three Drift LFOs, and the Delay-Time control now has a clock-icon toggle that swaps the free-rate slider for a musical division (`16/1` … `1/32`, including triplet `T` and quintuplet `Q` variants). The slow end (`16/1`, `8/1`, `4/1`, `2/1`) gives drift cycles spanning multiple bars — suited to slow harmonic motion and long auto-regen swings. Sync follows the host transport when the DAW is playing; in standalone or with the host transport stopped, the in-app sequencer (step or generative) takes over; if neither is running, the rate freezes at the last-seen host BPM and falls back to the Seq-BPM field as a last resort. The UI swaps the rate/time row for a division stepper when Sync is on.
- **Fix: clock-mode persisted across preset loads.** Loading a pre-v1.7 preset would inherit whatever clock state was last touched in the host process — Drift 1's clock would mysteriously stay on Sync after loading a vanilla preset. The state restore now patches missing clock-param defaults straight into the loaded ValueTree, so the swap is atomic and old presets land at Off / `1/4` as intended. Old `.t5p` JSON imports get the same explicit-default treatment.
- **Fix: injection mode persisted across `.t5p` loads.** Pre-injection presets (saved before v1.6) didn't carry the injection-mode field, so loading one would leave the panel on whatever mode (e.g. Combo 3) was active before — and the resulting audio no longer matched the preset's source. Old presets now default to `linear / 0.75 / 4 / 16` on load, reproducing the original A↔B crossfade behaviour they were generated with.
- **Fix: LFO Free / Trigger mode.** Trigger mode previously had no audible effect — per-voice LFOs were reset on note-on but their output was never read by the voice rendering, so all voices kept tracking the same free-running global LFO regardless of mode. Voices now consume their own retriggered LFO for per-voice modulation (filter, pitch, scan) when Trigger is selected. Free mode is unchanged. Global FX targets (Dly Time/FB/Mix, Rev Mix) keep using the global LFO since the FX bus is shared across voices.
- **Fix: delay dry/wet curve.** The Delay's mix knob was a hybrid that attenuated the dry path by 30 % at full wet (mislabelled as "parallel send-bus"); cranking the mix could feel like the delay was eating the signal, especially in external loopbacks. The mix is now a true crossfade — `out = dry × (1 − mix) + wet × mix`, identical to the Reverb's curve. At mix = 1 the dry path vanishes and only the delay tail (with its own feedback) remains.

## v1.6.0-beta.1 — 2026-04-30

- **New: polyphonic generative sequencer.** The Generative Sequencer grows from a single Euclidean / Turing-Machine voice to up to **five independent strands**, each with its own Div×, octave offset, dominance, role, and panning. Strands draw from a shared **Pitch Field** (12-bit pitch-class set with Static / Drift / Transform / Pivot evolution modes) so harmonic coherence is provided at the field level rather than per-event. Coordination is handled via priority displacement (DensityBudget) caps. New in-app Manual sections cover strand roles, the pitch field, and the principles behind ensemble-aware coordination. State is persisted in `.t5p` JSON and `.t5seq` files; the older single-strand format is restored as Strand 1 with the others muted.
- **New: Injection Modes.** The Generation panel grows a six-button mode row above the Alpha slider. The classical A↔B movement is now one of six modes — *Linear*, *Step-in*, *Layer*, *Combo 1*, *Combo 2*, *Combo 3* — that select different ways Impulse B enters Impulse A inside the diffusion pipeline. Linear preserves bit-for-bit compatibility with prior versions; the other five modes operate on the diffusion sampler steps and on individual DiT block cross-attention layers. See the in-app Manual §1 for the user-facing description and ARCHITECTURE.md §6.5 for the mechanics.
- **Step-in mode** — early sampler steps see Impulse A only; after a transition step the cross-attention conditioning swaps to a Step-in-controlled blend of A and B. Drives a single intensity slider (0–1) coupled to both the transition point and the late-phase blend amount. Implemented as a `DiTWrapper.forward` kwargs swap on the inner sampler.
- **Layer mode** — a two-thumb range slider defines a B-zone over the 16 DiT blocks. Per-block forward_pre_hooks override each block's cross-attention context with a sigmoid top-hat blend of A and B. Built for surgical "this impulse only on these layers" experiments.
- **Combo 1 / 2 / 3** — preset combinations of step phase × layer band, each with a hardcoded layer range and a Step-in-style intensity slider. Combo 1 = surface (blocks 0..4), Combo 2 = broad mid (blocks 4..12), Combo 3 = narrow center (blocks 6..10). Hard layer mask (no edge softening) so slider=1 is genuinely 100 % B in the band's blocks.
- **Per-mode slider memory.** Step-in and the three Combo modes each remember their own intensity-slider value, so A/B-ing them by clicking buttons does not destroy the last-used position of any individual mode. Linear and Layer already had independent state.
- **Mode buttons trigger regeneration.** Clicking any of the six mode buttons fires a fresh inference with the newly selected mode, matching the existing slider/drift auto-regen UX.
- **Slider-scale fix.** The Step-in/Combo intensity slider now displays 0–1 (was 0.5–1.0) — internal mapping onto the audible region of `injection_transition_at` / `late_phase_alpha` is unchanged. Old presets reload their stored value into the corresponding mode slot, but their effective rendering may differ since the slider value is no longer remapped before being sent to the backend.

## v1.3.0-beta.1 - 2026-04-24

- Expanded the instrument from an early beta into a fuller text-to-sound workflow with independent wavetable extraction regions, shared P1/P2/P3 traversal controls, and a clearer wavetable start-point model.
- Added session persistence for the standalone app so working states survive quit/relaunch instead of behaving like disposable test sessions.
- Reworked the preset workflow with factory presets, `INIT`, explicit P1/P2/P3 locks, auto-trim support, and cleaner preset migration/loading behavior.
- Broadened the musical control surface with a microtuning system, non-Western scales, Shruti support, sampler-mode tuning support, and a more expressive generative sequencer.
- Extended modulation and motion design with drift random waveforms, additional drift targets, sample-and-hold LFO support, ghost sliders, and a denser but more legible sequencer layout.
- Moved the user-facing setup flow toward installer-first distribution on macOS, added a Windows installer path, and tightened model/preset placement around per-user defaults with system-wide scan fallbacks.
- Improved the guided onboarding and in-app documentation with the embedded manual overlay, clearer install guidance, and a more robust download/setup flow for model assets.
- Hardened backend startup, model discovery, and download handling so failures surface more clearly and model paths behave consistently across packaged installs.
- Stabilized playback and interaction around generate/regenerate, active-voice sampler behavior, modulation editing, shutdown, and repaint-heavy GUI paths.
- Kept the core dependency baseline unchanged across the beta line: `backend/requirements.txt` and the top-level CMake `FetchContent` pins for `nlohmann/json` and `signalsmith-stretch` did not change between `v1.0.0-beta.1`, `v1.0.0-beta.2`, and `v1.0.0-beta.3`.

## Notes

- `v1.3.0-beta.1` keeps the release on the beta line while the broader rollout (VST3/AU and additional public platforms) is still unfinished; the current tagged release asset remains the macOS installer only.
- The version number stays monotonic with the older internal `1.2.x` build line and avoids another downgrade in bundle / installer versioning.
