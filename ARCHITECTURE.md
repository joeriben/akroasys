# T5ynth Architecture

Code-level walkthrough for C++/JUCE developers new to the project. This document
describes *where code lives* and *how the C++ layers are wired together*. It
deliberately does **not** cover the audio signal flow — that is documented in
`resources/akroasys_Guide.html`, section 16 ("Signal flow reference"), which is
shipped as `BinaryData::akroasys_Guide_html` and reachable from the plugin's
`Manual` button.

For sibling docs written alongside this one, see the cross-reference list at
the end.

---

## 1. Top-level directory layout

```
t5ynth/
├── CMakeLists.txt              # Single top-level CMake file — only build entry point
├── src/                        # All C++ sources
│   ├── PluginProcessor.{h,cpp} # juce::AudioProcessor root — owns APVTS + DSP + inference
│   ├── PluginEditor.{h,cpp}    # juce::AudioProcessorEditor — thin wrapper around MainPanel
│   ├── dsp/                    # Voice, oscillators, filters, envelopes, effects
│   ├── gui/                    # All juce::Component panels + LookAndFeel
│   ├── inference/              # PipeInference — C++ side of the Python IPC
│   ├── sequencer/              # Step, phrase and generative sequencers, arpeggiator
│   └── presets/                # .t5p binary preset format serialization
├── backend/                    # Python inference subprocess
│   ├── pipe_inference.py       # Script form (dev mode)
│   ├── pipe_inference.spec     # PyInstaller spec
│   ├── runtime_hook.py         # PyInstaller runtime hook
│   ├── requirements.txt        # Python deps (diffusers, torch, transformers, …)
│   ├── services/               # Model backends (stable_audio_backend, cross_aesthetic_backend)
│   ├── routes/                 # Legacy HTTP routes (kept but unused by the plugin)
│   ├── config.py               # Device/model enumeration logic
│   └── dist/pipe_inference/    # PyInstaller one-folder output (populated by `pyinstaller`)
├── JUCE/                       # Vendored JUCE framework, see §10
├── resources/
│   ├── ir/                     # Convolution reverb impulse responses (.wav)
│   ├── presets/                # Bundled .t5p presets (seeded into the UCDCAE AI Lab bank on first launch)
│   ├── logos/                  # App icon, about-dialog logos
│   └── akroasys_Guide.html       # HTML user manual (rendered by WebBrowserComponent)
├── docs/                       # Developer documentation (you are here's siblings)
├── tools/                      # Standalone Python + C++ validation/comparison scripts
└── .github/workflows/build.yml # CI: Linux/macOS/Windows build + PyInstaller bundling
```

### `src/` subtree

- **`src/PluginProcessor.{h,cpp}`** — root of the plugin, see §2.
- **`src/PluginEditor.{h,cpp}`** — 31 lines total. Installs the custom
  `T5ynthLookAndFeel`, instantiates a `MainPanel`, sets window size
  `1300×867` with a `3:2` fixed aspect ratio. Real UI lives in `gui/`.
- **`src/dsp/`** — per-voice and global DSP classes. Notable entries:
  - `SynthVoice.{h,cpp}` — single monophonic voice; owns its `WavetableOscillator`,
    `SamplePlayer`, three `ADSREnvelope`s (amp + 2 mod), two per-voice `LFO`s,
    `NoiseGenerator`, and `StateVariableFilter` (`T5ynthFilter`).
    See `src/dsp/SynthVoice.h:17`.
  - `VoiceManager.{h,cpp}` — fixed `std::array<SynthVoice, 16>` pool with
    runtime `voiceLimit` (1/4/6/8/12/16) and oldest-note stealing.
    See `src/dsp/VoiceManager.h:14`.
  - `WavetableOscillator`, `SamplePlayer`, `WavetableBank` — the two
    oscillator types fed by the generator output. Both are owned by
    `T5ynthProcessor` as *master* instances (`masterOsc`, `masterSampler`) and
    the per-voice copies share frame/buffer data. A third, unrelated
    oscillator source sits beside them: `CsoundEngine.{h,cpp}`, the LCO's
    live Csound orchestra — see §5.
  - `LFO`, `DriftLFO`, `ADSREnvelope`, `StateVariableFilter`,
    `NoiseGenerator`.
  - `DelayLine`, `ConvolutionReverb`, `AlgorithmicReverb`, `Limiter` —
    global post-sum effects.
  - `BlockParams.h` — plain-old-data struct carrying every per-block
    parameter value from APVTS down into `VoiceManager::renderBlock`.
  - `ModulationMatrix.{h,cpp}` — **stub**, see §6.
- **`src/gui/`** — every `juce::Component` subclass, plus `T5ynthLookAndFeel`
  and `GuiHelpers.h` (shared `SliderRow`, `SwitchBox` primitives). See §3.
- **`src/inference/`** — `PipeInference.{h,cpp}` only. C++ side of the IPC to
  Python. See §7.
- **`src/sequencer/`** — `StepSequencer`, `Arpeggiator`,
  `PhraseSequencer` + `PhraseModel` (motif, harmony and form for
  conventional lines, `docs/PHRASE_SEQ_CONCEPT.md`; the model is pure and
  runs in `tools/phrase_seq_print.cpp` without JUCE),
  `GenerativeSequencer` (Euclidean + scale-quantized note generator).
  The three sequencers share one transport (`seq_running`); `seq_engine`
  (Step / Phrase / Gen) picks which one plays. Driven from `processBlock`
  and exposed via `PluginProcessor` accessors.
- **`src/presets/PresetFormat.{h,cpp}`** — .t5p binary serialization with
  embedded audio. Magic `T5YN`, current version `2`. See
  `src/presets/PresetFormat.h:8` and `docs/PRESET_FORMAT.md` for the
  wire format.

---

## 2. Plugin lifecycle orchestration

`T5ynthProcessor` (`src/PluginProcessor.h:17`) is the root object owned by the
host (DAW or standalone wrapper). It owns everything with lifetime bound to
the plugin instance:

- the `juce::AudioProcessorValueTreeState parameters` tree, built in
  `createParameterLayout()` (`src/PluginProcessor.cpp:19` — returns at line
  467);
- the DSP pipeline (`voiceManager`, `masterOsc`, `masterSampler`, two global
  `LFO`s, `driftLfo`, `postFilter`, `delay`, `reverb`, `algoReverb`,
  `limiter`);
- the sequencers (`stepSequencer`, `phraseSequencer`, `generativeSequencer`)
  and the `arpeggiator`;
- the `PipeInference pipeInference` client that manages the Python
  subprocess;
- generated-audio buffers (`generatedAudioFull`, `generatedAudioRaw`) plus
  the last known `sampleRate`, seed, prompts, and prompt embeddings —
  these are *GUI-visible state that is not in APVTS*. See §8.

### APVTS as single source of truth

`createParameterLayout()` is the only place where APVTS parameters are
registered. Every exposed control in the GUI ultimately resolves to an
`AudioProcessorValueTreeState::SliderAttachment`, `ButtonAttachment`, or
`ComboBoxAttachment` against an ID declared here. When adding a new
parameter you touch three places: this function, the DSP class that reads
it (typically via `BlockParams` populated in `processBlock`), and the panel
that exposes it.

### `prepareToPlay`

`src/PluginProcessor.cpp:470`. Calls `prepare(sampleRate, samplesPerBlock)`
on every DSP member, loads the default convolution-reverb IR from
`BinaryData::emt_140_plate_medium_wav` (§9), resizes pre-allocated scratch
buffers (`lfo1Buffer`, `lfo2Buffer`, `reverbSendBuffer`), and computes
`tailBlocks` for deep-idle detection (≈10 seconds of reverb tail at the
current block size).

### `processBlock`

`src/PluginProcessor.cpp:505`. High-level structure:

1. **Idle gating.** Track `silentBlockCount` — once no voices are active
   and no MIDI has arrived for `tailBlocks` blocks, skip the entire body
   and just advance LFO phases (`audioIdle` becomes `true`).
2. **Voice count update.** Read `voice_count` choice and resize the
   active pool.
3. **Parameter snapshot.** Read every APVTS value into a `BlockParams bp`
   local (`src/dsp/BlockParams.h`). This happens once per block — voice
   rendering never touches APVTS directly.
4. **Modulation targets resolved.** Drift LFO target, ENV 2/3 targets
   (`mod1_target`, `mod2_target`), LFO1/LFO2 targets are all stored as
   integer choices in APVTS; `processBlock` dispatches them onto the
   correct DSP destinations. See §6.
5. **Global LFOs ticked into `lfo1Buffer` / `lfo2Buffer`.**
6. **MIDI routed** through sequencer / arpeggiator / direct note-on.
7. **`voiceManager.renderBlock(...)`** sums polyphonic voices into
   `buffer` with `1/sqrt(N)` scaling.
8. **Post-sum chain.** Global filter → delay → parallel reverb send (via
   `reverbSendBuffer`) → limiter → master gain.
9. **GUI telemetry.** Modulated parameter snapshots (`modulatedValues`),
   `lastMidiNote`, `waveformSnapshot`, `audioIdle` are stored with relaxed
   atomics for the GUI thread to read on its timers.

### `releaseResources`

Resets the master oscillator, sampler, and voice manager.
`src/PluginProcessor.cpp:498`.

### State (`getStateInformation` / `setStateInformation`)

`src/PluginProcessor.cpp:1516` — APVTS state is serialized as XML via JUCE's
`copyXmlToBinary`. On load, the transport flag `seq_running` is explicitly
forced to `0` so sessions never auto-start audio on restore. The engine
choice `seq_engine` is not a transport and stays as saved; a state from
before it carries the bool `gen_seq_running`, which the restore migrates
(on → Gen, off → Step). The phrase sequencer's stored motif is not a
parameter and travels as the XML attribute `phraseMotif` (JSON, the format
of `docs/PRESET_FORMAT.md` §3.4).

Beyond APVTS, the session state carries only a few XML attributes like that
one (MIDI output and clock settings, the phrase motif, the Csound orchestra
in LRO mode). Prompts, seed, generated audio, axes slot state, and prompt
embeddings live outside APVTS and are persisted separately via the .t5p
preset format (§8).

---

## 3. GUI hierarchy

`MainPanel` (`src/gui/MainPanel.h:15`) is the GUI root. `T5ynthEditor`
contains exactly one `MainPanel` member (`src/PluginEditor.h:20`). All
layout is driven by `MainPanel::resized()` — no JUCE `Viewport` or
`ResizableWindow` wrappers.

`MainPanel` owns the major fixed panels as direct members:

| Member                 | Type                     | Source                           |
|------------------------|--------------------------|----------------------------------|
| `promptPanel`          | `PromptPanel`            | `src/gui/PromptPanel.h:15`       |
| `axesPanel`            | `AxesPanel`              | `src/gui/AxesPanel.h:16`         |
| `synthPanel`           | `SynthPanel`             | `src/gui/SynthPanel.h:14`        |
| `fxPanel`              | `FxPanel`                | `src/gui/FxPanel.h:10`           |
| `sequencerPanel`       | `SequencerPanel`         | `src/gui/SequencerPanel.h:17`    |
| `statusBar`            | `StatusBar`              | `src/gui/StatusBar.h:5`          |
| `dimensionExplorer`    | `DimensionExplorer`      | `src/gui/DimensionExplorer.h:13` |
| `settingsPage`         | `SettingsPage`           | `src/gui/SetupWizard.h:12`       |
| `manualWeb`            | `juce::WebBrowserComponent` | inline in MainPanel           |

### Layout columns

`MainPanel::resized()` divides the window into three columns (generation,
engine/filter/modulation, FX) plus a full-width sequencer row and a top/
bottom status bar. Each panel is a self-contained `juce::Component` that
binds to `T5ynthProcessor::getValueTreeState()` via APVTS attachments. There
is no cross-panel state — panels communicate through APVTS or via
`std::function` callbacks set by `MainPanel` (e.g.
`PromptPanel::onEmbeddingsReady`, `StatusBar::onSavePreset`).

### Overlay panels (Scrim pattern)

Three features render as full-window overlays instead of being inlined:

1. **Dimension Explorer** — launched from the mini preview in
   `MainPanel`. Uses the nested `Scrim` component
   (`src/gui/MainPanel.h:56`) to darken the background and swallow
   click-outside events.
2. **Settings** — `SettingsPage` (the legacy name of the file is
   `SetupWizard.{h,cpp}`; the class is `SettingsPage`). Shows model
   status, Smart Auto-Scan, HuggingFace download buttons, and backend
   connection state.
3. **Manual** — renders `BinaryData::akroasys_Guide_html` via a JUCE
   `WebBrowserComponent`. The HTML is extracted to a temp file on first
   open (`manualHtmlOnDisk`, `MainPanel.h:94`) because
   `WebBrowserComponent` loads URLs rather than raw HTML strings.

Each overlay has its own `Scrim` member instance and a `visible` flag.

### LookAndFeel

`src/gui/T5ynthLookAndFeel.{h,cpp}` is a single `juce::LookAndFeel_V4`
subclass installed once by `T5ynthEditor`. Shared slider/button/header
primitives live in `src/gui/GuiHelpers.h` (`SliderRow`, `SwitchBox`,
header-bar helpers) — every panel uses these so that spacing, colors and
header treatment stay consistent.

---

## 4. Voice chain

Polyphony is managed by `VoiceManager` (`src/dsp/VoiceManager.h:14`), which
holds a fixed `std::array<SynthVoice, 16>` pool (`MAX_VOICES = 16`) and a
runtime `voiceLimit` from APVTS `voice_count` (choices:
`Mono / 4 / 6 / 8 / 12 / 16`). Policy:

- **Allocation order:** find free voice first, else steal by *oldest note*
  (`noteOnCounter` monotonic, `stealVoice()` at `VoiceManager.h:85`).
- **Equal-power gain scaling:** each voice multiplied by `1/sqrt(N)` where
  `N` = number of active voices, ramped over `GAIN_RAMP_MS = 5ms` on any
  voice count change to prevent clicks.
- **Mono mode** (`voiceLimit == 1`): a single voice with legato glide
  configured via `glideToNote`.

Each `SynthVoice` (`src/dsp/SynthVoice.h:17`) owns its own instance of:

- `WavetableOscillator osc` and `SamplePlayer sampler` (mode is stored in
  APVTS `engine_mode`, not as a class member — see
  `PluginProcessor::isWavetableMode()` at `src/PluginProcessor.h:61`).
  These are two of FOUR per-voice engines selected by `engine_mode`
  (`SynthVoice::EngineMode`, `src/dsp/SynthVoice.h:122`): the others are
  the `FreezeTextureEngine freezeEngine` granular engine
  (`SynthVoice.h:169`) and, in Csound mode, a live orchestra rendered by a
  processor-owned `CsoundEngine` entirely outside the voice and read in via
  `csoundBuf_` — see §5.
- `NoiseGenerator noise` for the sub-oscillator noise source.
- Three `ADSREnvelope`s: `ampEnv` (amplifier/ENV1), `modEnv1` (ENV2),
  `modEnv2` (ENV3).
- Two per-voice `LFO`s (`perVoiceLfo1`, `perVoiceLfo2`) used only when
  the corresponding global LFO is in *Trigger* mode — in *Free* mode the
  global `T5ynthProcessor::lfo1` / `lfo2` values are used instead.
- Three filter algorithms, switched per voice by `BlockParams::filterAlgorithm`:
  - `T5ynthFilter` (linear TPT State Variable Filter,
    `src/dsp/StateVariableFilter.h`) — low-CPU default.
  - `LadderFilter` (Huovilainen non-linear 4-pole ladder with
    half-sample-delay-compensated feedback, `src/dsp/LadderFilter.h`).
  - `CutoffWarpFilter` (Surge-XT-inspired ZDF 4-pole ladder with
    style-switchable per-feedback saturation, `src/dsp/CutoffWarpFilter.h`).
  Only the active model processes audio; the other two sit idle.

### Engine data distribution

The oscillator frame data lives once on the *master* instances owned by
`T5ynthProcessor` (`masterOsc`, `masterSampler`,
`src/PluginProcessor.h:122`). After a new generation lands via
`loadGeneratedAudio`, `VoiceManager::distributeSamplerBuffer` and
`distributeWavetableFrames` push pointers/copies to every voice. This keeps
the hot render path lock-free while allowing a single source of truth for
the loaded audio.

### Sub-block filter updates

`SynthVoice::renderBlock` processes audio in chunks of
`SUB_BLOCK_SIZE = 32` samples so filter coefficients can be updated at
sub-block rate without paying the cost of per-sample recomputation
(`src/dsp/SynthVoice.h:49`).

---

## 5. The LCO — the language-controlled oscillator

Alongside the two sample-fed engines in §4, `engine_mode` can select a third
kind of oscillator that is fed by neither of them: for every prompt, one
inference against a curated library makes a language model **write** a
Csound orchestra, which is compiled and run live as the voice source. One
prompt → one inference → one orchestra → it runs. The **authoring** path has
no fallback: if the model cannot produce an orchestra that compiles, the LCO
stays silent — never a keyword-matched or default one. (The **engine** has one
exception to that, described at the end of this section, and it is a known
conflict with the rule rather than a designed escape hatch.)

### Where it sits

`SynthVoice::EngineMode` (`src/dsp/SynthVoice.h:122`) is four-valued:
`Sampler`, `Wavetable`, `Freeze` (the granular `FreezeTextureEngine`,
`SynthVoice.h:169`) and `Csound`. In Csound mode a voice's raw signal is not
produced by `osc`/`sampler`/`freezeEngine` at all — it is read
sample-by-sample from a processor-owned `CsoundEngine`'s per-voice output
buffer (`SynthVoice.cpp:1043-1056`); noise mix, drive, filter and the
VCA/envelope chain downstream are the same shared path every other engine
uses, untouched.

`engine_mode` itself (`EngineMode` in `src/dsp/BlockParams.h:503`) is
five-valued: `Sampler`/`Wavetable`/`Freeze` are the neural "T5osc" paradigm;
`Lco` is a legacy value kept only so a v5 preset's baked-wavetable DCO still
loads (it falls through to the Wavetable DSP path,
`PluginProcessor.cpp:3337-3339`); `Csound` is the live value described in
this section. The
"» LCO / » T5osc" toggle in `MainPanel` owns the switch
(`MainPanel::applyOscModeToEngine`, `MainPanel.cpp:1645`): leaving Csound
for T5osc calls `T5ynthProcessor::restoreNeuralEngineMode()`; returning to
the LCO restores whichever language mode was stashed
(`restoreLanguageEngineMode()`) or, failing that, re-forces Csound if an
orchestra has already been authored this session
(`forceCsoundEngineMode()`, `PluginProcessor.cpp:2134`) — a session with
nothing baked yet leaves the engine on whatever was already sounding until
the first bake.

### Triggering an authoring pass

`PromptPanel::triggerDcoBake()` (`PromptPanel.cpp:2301`) is the LCO's
GENERATE entry point — the same button, Cmd/Return shortcut and Launch
Control XL "Generate" CC that drive the neural path route here whenever the
panel is in Advanced (LCO) mode. It gates on the language model being
installed, the inference pipe being idle, and the prompt editor being
non-empty, then calls `PipeInference::authorCsoundOrchestra()`
(`PromptPanel.cpp:2656`) on a detached background thread.
`setLcoStatus()` (`PromptPanel.cpp:2161`) is the single channel ordinary
status text ("Writing the instrument", "Still generating", …) and the busy
state flow through, both funnelled into `LcoTraceView`
(`src/gui/LcoTraceView.h`). With a Re-Prompt stance engaged, the same
GENERATE press instead runs `triggerDcoReprompt()` (`PromptPanel.cpp:2713`),
which rereads the previous bake's own reading and prompt under the selected
stance and rewrites the prompt before calling `triggerDcoBake()` again.

On success, the panel builds an `LcoTraceView::Trace`
(`PromptPanel.cpp:2425-2444`) entirely from fields the backend already
computed — nothing is composed and the author is asked for no account of
itself. The view renders
it as five stations (`LcoTraceView.h:11-17`): **HEARD** (the authored
prompt), **LOOKED UP** (which library instruments/adjectives/motions the
prompt's own words reached, and which orientation entries the author saw
anyway), **WROTE** (which model, and its own one-line reading), **REPAIRED**
(the Csound errors it had to fix, absent when it compiled on the first
attempt) and **RUNNING** (the live compile state). The authored Csound body
is the flip side of the same card, not printed inline. A single model tab
names whichever model would author next, or — once a bake has claimed one —
the model that actually wrote the running orchestra (`lcoResolvedModel_` /
`lcoAuthorClaim_`, `PromptPanel.h:617-618`); it is the one language model in
the app, shared by authoring, translation and Re-Prompt.

### The IPC hop

`authorCsoundOrchestra()` (`PipeInference.h:263`, implemented
`PipeInference.cpp:1276`) sends a `mode:"csound"` request over the same
stdin/stdout binary protocol as generation — see `docs/IPC_PROTOCOL.md`
§3.3 for the request shape and §4.5/§4.6 for the text-result and
interim-frame responses it reads. On the Python side,
`backend/pipe_inference.py`'s `request.get("mode") == "csound"` branch
(`pipe_inference.py:3667`) resolves the author model via
`_resolve_coder_model_dir()` (`:938` — one directory, no fallback to any
other installed model; the shipped slot is `gemma-4-12b-it-qat-q4_0`,
`:913`), builds a single `csound_llm` callable around it (GGUF via
llama.cpp when the install ships one, else a `transformers` directory) and
calls `lco_write.build_csound_response()` (`:3721`). When the request
carries `"stream": true`, the author's reasoning is sent back live as
`\x04` interim frames as it is written, decoded on the C++ side and shown
in the WROTE station while authoring is still running.

### The host scaffold — the model's one contract

`lco_write.wrap()` (`backend/lco_write.py:922`) puts the authored body
between a fixed `_HEAD` (`:844`) and `_TAIL`+`_SCORE` (`:907`, `:918`). The
body's only obligation is to write its signal into **`asig`**; everything
else is the host's:

- `sr` substituted from the engine's real sample rate at compile time,
  `ksmps = 64`, `nchnls = 16` (one channel per voice, matching
  `CsoundEngine::kMaxVoices`), `0dbfs = 1`.
- ftables `giSine` (1, sine), `giCos` (2, cosine — `gbuzz`'s harmonics are
  cosines), `giCheb` (3, a GEN13 Chebyshev transfer function) and `giImp`
  (4, a strike impulse for models that take their excitation as a table
  number).
- one numeric `instr 1` with `ivoice = p4` and sixteen always-on score
  instances (`i 1 0 2000000000 <v>` for v = 1..16, ~63 years) — there is
  no note-off and no per-note score event; every struck idiom in the
  library is continuously driven (noise, `dust`) rather than one-shot,
  because the synth's own envelope owns amplitude shape, never the
  oscillator.
- six per-voice control channels read via `sprintf`+`chnget`
  (`gate`/`freq`/`vel`/`pres`/`timb`/`trig`), `kgate` declicked with
  `portk`, `kfreq` limited to 20..12000 Hz, and the player's `kvol1-3`/
  `koct1-3` mix/octave knobs.
- `knote`, seconds since the current note began — `init 0`, reset on
  `changed2(ktrig)` — for anything the authored body wants to shape over
  the note's lifetime.
- `aout = asig * kgate * kpresGain * 0.32` (`HEADROOM`), then a
  final `clip aout, 0, 0.95, 0.85` before `outch ivoice, aout`. No `kvel`
  factor here: the voice envelope's peak already tracks velocity
  (`velPeakScale`), so one applied in the tail made the engine scale as
  vel² where every other engine is linear (removed 2026-07-25; presets
  saved earlier keep the old line in their stored orchestra until re-baked).

### The repair loop

`build_csound_response()` (`lco_write.py:1276`) assembles the prompt from
`select()` + `render_library()` — which library entries the prompt's words
reached versus a `_STARTER` orientation set when they reach too few
(`:147`, `:220`) — calls the model, and hands the reply to `sanitize()`
(`:768`) to split the plain-language reasoning from the fenced `csound`
body and its `READING:` line. `wrap()` + `syntax_check()` (`:1183`) then
compile the result through the real Csound compiler — the CLI's
`--syntax-check-only`, or CsoundLib64 via `ctypes` as a fallback, the same
compiler `CsoundEngine.cpp` itself runs. A failure does not end the
attempt: `_continue()` (`:412`) puts the original request, the model's own
failed reply, and every distinct error seen so far — not only the latest —
back to the model as a continuation of the same conversation, so a retry
edits what it wrote rather than re-authoring from nothing. This runs for up
to `MAX_TRIES` (`T5YNTH_LCO_MAX_TRIES`, default 6) attempts, stopping early
if a reply repeats with no new error. Exhausting the budget returns
`{"ok": false, ...}` and the LCO stays silent — never a keyword-matched or
default orchestra.

### Compiling, swapping and rendering (`CsoundEngine`)

`src/dsp/CsoundEngine.{h,cpp}` wraps one `CSOUND*` instance behind
`prepare()` / `setVoiceControls()` / `renderUpTo()` / `voiceBuffer()`.
`T5ynthProcessor` owns TWO instances (`csoundEngines_[2]`,
`PluginProcessor.h:528-529`) so a newly authored orchestra can compile on a
background thread while the previously active one keeps sounding.
`requestCsoundOrchestra()` (`PluginProcessor.cpp:2073`) snapshots every
voice's retrigger epoch and frequency, queues the orchestra text, and wakes
`handleAsyncUpdate()` (`:7686`) on the message thread, which compiles the
text into whichever engine is currently inactive
(`CsoundEngine::prepare(sampleRate, blockSize, orchestraText)`) and then
calls `primeForTakeover()` — silently pumping ~0.25 s of `ksmps` at a closed
gate so a HELD note's later crossfade does not audibly re-strike — before
the swap is armed. The crossfade itself runs over `driftCrossfade` ("Regen
XFade", `PluginProcessor.cpp:4133`), the same control the platform's other
regenerating engines use. If no engine is ready yet at all (the first bake
of a session), the new orchestra compiles directly into the active slot
instead, with no fade. On a compile failure, `csoundCompileErrorText_` is
set and nothing swaps — the previously active orchestra keeps playing
unchanged (`PluginProcessor.cpp:7888-7905`); `PromptPanel::
pollCsoundCompile()` (`PromptPanel.cpp:2213`) polls this state and reports
compiling → ok/error into the RUNNING station.

A `prepare()` call with `orchestraText == nullptr` compiles
`CsoundEngine.cpp`'s own hard-wired 12-partial standing-tone orchestra
(`:46-189`) instead of an authored one. It is reached when Csound mode is
selected before anything has been authored in the session — the bootstrap
compile at `PluginProcessor.cpp:7798`, and the restore path at `:1989` when
the active engine carries no orchestra text.

**This conflicts with a documented platform rule.** The rule is that the
oscillator is authored by the language model and that no deterministic
fallback tone stands in for it; this orchestra is exactly such a tone, and
`CsoundEngine.cpp`'s own comments call it "the fallback
tone". Whether it should sound at all, or whether selecting Csound mode with
nothing authored should be silent, is open and belongs to the maintainer.
Do not extend or rely on it in the meantime.

### Where the library lives

`backend/dco_lexicon.json` (`lexicon_version` 21) is the curated source: 29
instruments, 51 adjectives, 17 motions, of which 12 instruments
(`analog_osc`, `blown_bottle`, `divider_organ`, `driven_metal`, `drum_head`,
`fm3`, `fm_bell`, `fm_stack`, `metallic_fm`, `plucked_wire`, `string`,
`supersaw`) carry measured parameters with named, glossed anchors.
`tools/lco_build_library.py` assembles it into `backend/lco_library.json`,
which `lco_write.render_library()` renders into the author's prompt;
`--check` regenerates the library in memory and exits 1 on drift against
the committed file, so the two cannot silently fall out of step.

### The Csound dependency is optional

`CMakeLists.txt:70-78` finds `CsoundLib64.framework` only on Apple, only in
a Homebrew or `/Library/Frameworks` prefix (`NO_DEFAULT_PATH` — never a
system default search), and is never required; it is not searched for at
all on Linux/Windows. `src/dsp/CsoundEngine.cpp` is added to
`target_sources(T5ynth ...)` only inside that `if(T5YNTH_CSOUND_FOUND)`
block (`:216-217`); everywhere else the class compiles from the header-only
inert stub at the bottom of `CsoundEngine.h` (every call a no-op / false /
silence). `T5YNTH_HAS_CSOUND` is nonetheless always defined, 0 or 1
(`CMakeLists.txt:313`), so every call site branches at runtime rather than
needing a build-level `#ifdef` — but the practical consequence is that a
build made on a machine without Csound installed (every CI runner, and any
developer machine that has not `brew install`ed it) links and runs cleanly
with `engine_mode = Csound` selecting an engine that is never ready: the
LCO compiles green and is silent.

---

## 6. Modulation routing

**Important:** `src/dsp/ModulationMatrix.{h,cpp}` exists in the tree and is
compiled, but it is currently a **stub** — `ModulationMatrix::process()`
does nothing, `getModulationValue()` returns 0. See
`src/dsp/ModulationMatrix.cpp:13`. The planned central routing matrix is
not implemented.

Actual modulation routing today uses a **target-dropdown-per-source**
pattern. Every modulation source has a single `*_target` APVTS choice that
picks its destination at runtime. `processBlock` reads these choices into
`BlockParams`, then in the voice/global mix applies the source to the
selected destination.

Current sources and their target parameters (see
`PluginProcessor.cpp:294-314`):

- `mod1_target` — ENV 2 (Mod 1). Choices:
  `--- / DCA / Filter / Scan / Pitch / Dly Time / Dly FB / Dly Mix / Rev Mix / LFO1 Rate / LFO1 Depth / LFO2 Rate / LFO2 Depth`.
- `mod2_target` — ENV 3 (Mod 2). Same choices as ENV 2.
- `lfo1_target`, `lfo2_target` — Choices:
  `--- / Filter / Scan / Pitch / Dly Time / Dly FB / Dly Mix / Rev Mix / ENV1 Amt / ENV2 Amt / ENV3 Amt`.
- `drift1_target`, `drift2_target`, `drift3_target` — Drift LFO targets
  driving the oscillator generation parameters (Alpha, Noise, Magnitude,
  Axes) and a subset of DSP targets.

### APVTS prefix convention

- `amp_*` — ENV 1 (Amplifier envelope).
- `mod1_*` — ENV 2 (configurable modulation envelope).
- `mod2_*` — ENV 3 (configurable modulation envelope).

ENV 1 is fixed as the amplifier, ENV 2 and ENV 3 are fully assignable.
The naming (`amp_` vs `mod1_`/`mod2_`) preserves the ENV 1 = DCA convention
but all three envelopes use the same `ADSREnvelope` class.

### Ghost indicators

Any slider whose target is currently being modulated paints a small "ghost"
position marker indicating the *modulated* value versus the *set* value.
The audio thread writes the modulated values atomically to
`T5ynthProcessor::modulatedValues` (`src/PluginProcessor.h:212`) with
`NaN` as the "no modulation" sentinel. GUI panels read these on their
timer tick and draw the ghost only when the value is non-NaN. See the
`ModulatedValues` struct definition for the full set of tracked targets.

### Adding a new modulation target

See `docs/ADDING_A_MODULATION_TARGET.md`. In short: add the string to the
relevant `*_target` choice array in `createParameterLayout`, then handle
the new target index in the `processBlock` dispatch and (if it is a
"continuous" target) add a matching atomic to `ModulatedValues`.

### BPM-sync for LFO / Drift / Delay

Each of the three LFOs, three Drift LFOs, and the Delay carries a pair of
APVTS choice params: `*_clock_mode` (Off / Sync) and `*_clock_division`
(`1/1` … `1/32`, including triplet `T` and quintuplet `Q` variants). The
canonical division → factor table lives in `ClockDivision::kFactor[]`
(`src/dsp/BlockParams.h`); never duplicate it elsewhere.

`T5ynthProcessor::resolveSyncBpm()` is the single source of truth for the
sync BPM, with priority: MIDI clock (when enabled and valid) > live host
transport > running in-app sequencer (any engine) > frozen `hostBpmLastSeen`
> `seq_bpm` fallback. The
host transport is snapshotted at the top of `processBlock` into the
`hostBpmLastSeen` / `hostPlayingNow` atomics; both are written and read
exclusively from the audio thread, so relaxed ordering is sufficient.

Effective values are derived by `ClockSync::computeRate(bpm, divIdx)`
(LFO/Drift Hz) and `ClockSync::computeDelayMs(bpm, divIdx)`
(delay milliseconds). When the corresponding `*_clock_mode` is Sync, the
synced value is written back into `BlockParams` (LFO) or fed directly to
the DSP setter (Drift, Delay), so downstream modulation that scales the
"base rate" (e.g. envelope → LFO rate mod) operates relative to the
synced rate naturally.

The UI mirrors this shape: each LFO/Drift/Delay row carries a clock-icon
toggle that swaps the rate/time slider for a division stepper. The
visibility swap is a `GUI`-side `onChange` on the hidden mode parameter,
not a separate visibility flag.

---

## 7. Inference IPC

C++ side is `src/inference/PipeInference.{h,cpp}`; the single instance
lives in `T5ynthProcessor::pipeInference` (`src/PluginProcessor.h:144`) and
is launched via `launchPipeInference(const juce::File& backendDir)` called
from the editor on startup.

Python side is `backend/pipe_inference.py`, which is also the entry point
for the PyInstaller bundle. In a release build the PyInstaller one-folder
output at `backend/dist/pipe_inference/pipe_inference` is shipped inside
the plugin bundle; `PipeInference::findBundledBinary` at
`src/inference/PipeInference.cpp:15` searches both that path and sibling
locations inside the app/.vst3/.component resources. In a dev build (no
bundled binary present) the client falls back to invoking
`pipe_inference.py` via a system Python (`PipeInference.cpp:128`).

Key characteristics:

- **Persistent subprocess.** One Python process per plugin instance; it
  stays alive across generations so model-load cost is paid once.
- **Binary protocol** over child stdin/stdout. The platform-specific
  handles are in `PipeInference::`-private members (`stdinFd_` /
  `stdoutFd_` on POSIX, `hChildStdinWr_` / `hChildStdoutRd_` on Windows).
- **Blocking `generate(Request)`.** Called from a JUCE
  `Thread`/`ThreadPoolJob` in `PromptPanel` — not from `processBlock`.
- **Auto-restart.** `generate()` checks `isChildAlive()` and will call
  `tryRestart()` once if the child has died.
- **Startup handshake** exposes the available devices (`mps`, `cuda`,
  `cpu`) and available models; these populate the Settings overlay.

The `Request` struct (`src/inference/PipeInference.h:45`) carries prompts,
alpha/magnitude/noiseSigma, duration, steps, cfgScale, seed,
device/model selection, per-dimension offsets from Dimension Explorer,
and the semantic axes map. The `Result` struct carries the audio buffer,
generation time, final seed, and the two mean-pooled 768-dim T5 prompt
embeddings.

The same client also carries the LCO's authoring call
(`authorCsoundOrchestra()`, `mode:"csound"` on the wire) — a separate
request/response shape from `generate()`, described in §5.

**Wire format details (flag bytes, header layout, error frames) are
documented separately in `docs/IPC_PROTOCOL.md`.** This file intentionally
does not duplicate the protocol spec.

### 7.5 Prompt-injection modes

The `Request` carries an `injectionMode` string and four numeric
fields (`injectionTransitionAt`, `latePhaseAlpha`, `splitStart`,
`splitEnd`) that select how prompt B is mixed into prompt A inside
the diffusion pipeline. The non-linear modes are only implemented on
the SA Open *native* path (`_generate_native` in
`backend/pipe_inference.py`) and exploit two mechanisms in the inner
DiT (`stable_audio_tools.models.dit.DiffusionTransformer`):

1. A **sampler-step counter** ticked by patching
   `pipe.model.model.forward` (the `DiTWrapper` that wraps the inner
   DiT). The wrapper is invoked exactly once per sampler step; the
   patch increments a closure-captured `state["calls"]` counter and
   forwards the call.
2. **Per-block forward_pre_hooks** on each
   `block.cross_attn` (16 blocks for SA Open). The hook receives the
   runtime `context` kwarg (the `to_cond_embed`-projected conditioning
   tensor, doubled for CFG) and returns a replacement tensor — either
   a single per-block context that replaces the cond half (CFG=1) or
   `cat([cond_half, uncond_half], dim=0)` when the runtime context is
   batch-doubled.

Modes:

- **`"linear"`** — Classical crossfade.
  `manipulated = (0.5 − 0.5·α)·A + (0.5 + 0.5·α)·B`, applied at
  `cond_a["prompt"]` once before sampling. No hooks, no patches.
  Reproduces v1.5.x byte-for-byte.

- **`"delta"`** — `manipulated = A + α·(B − null)`. Preserves A more
  strongly. No hooks.

- **`"late_step"` (UI: *Fine*)** — Patches `DiTWrapper.forward` to
  read the step counter; for steps `>= transition_step`, replaces
  `kwargs["cross_attn_cond"]` with a precomputed
  `late_blend = (0.5 − 0.5·αlate)·A + (0.5 + 0.5·αlate)·B` and
  `kwargs["cross_attn_mask"]` with the union mask. Early steps see
  pure A; late steps see the blend uniformly across all blocks.
  `transition_step = max(1, round(steps · injection_transition_at))`.

- **`"layer_split"` (UI: *Layer*)** — Pre-projects A, B, and a zero
  null embedding through `dit.to_cond_embed` once, then for every
  block computes a per-block context
  `(1 − w_i)·proj_a + w_i·proj_b` where the weight is a sigmoid
  top-hat over `[split_start, split_end]` with `smoothness = 2`
  (so each edge ramps over ~2 layers). The per-block tensors are
  bound into individual forward_pre_hooks. Hooks fire on every
  sampler step; the wrapper-level cross-attention conditioning is
  ignored.

- **`"kombi1"` / `"kombi2"` / `"kombi3"` (UI: *Kombi 1/2/3*)** — Step
  phase × layer band combined into a single preset. Each Kombi
  hardcodes its own layer range (Kombi 1 = [0, 4), Kombi 2 = [4, 12),
  Kombi 3 = [6, 10)) and uses a **hard layer mask** (`w = 1` inside,
  `0` outside) — sharper than `layer_split`'s sigmoid because the
  band is fixed by preset, not user-dragged. The hook reads the
  shared step counter and switches between two contexts:
  - early-phase (`state["calls"] < transition_step`): the hook
    returns `proj_a` for every block — pure A everywhere.
  - late-phase: the hook returns the per-block hard-mask blend of
    `proj_a` and the projected `late_blend`. Blocks inside the band
    see 100 % `late_blend`; blocks outside see 100 % A.

  Both the late-step counter patch *and* the per-block hooks are
  installed simultaneously; the `restore_forward` cleanup removes
  hooks first then deletes the wrapper monkey-patch.

The C++ side stores the active mode and slider values in
`PromptPanel` as research-mode local state (not in APVTS):
`injectionMode_`, `lateMixFine_/Kombi1_/Kombi2_/Kombi3_` for
per-mode slider memory, and `splitLayerStart_`/`splitLayerEnd_` for
Layer mode. The Kombi modes share the Fine slider's coupled mapping
`transition_at = 0.5 − 0.45·t`, `late_α = t`, with `t` taken from
the active mode's stored slider value. When the active mode is a
Kombi, `buildInferenceRequest` overwrites `splitStart`/`splitEnd`
with the per-mode hardcoded band so drift / preset / slider state
can never desync the geometry — the backend re-asserts the same
band as a defense-in-depth measure.

Embedding manipulations (`magnitude`, `noise_sigma`, `semantic_axes`,
`dimension_offsets`) are wired into every mode: the backend defines
per-request `apply_pre_offsets` (magnitude + noise + axes) and
`apply_dim_offsets` helpers and feeds them whichever tensor(s) flow
into the cross-attention layer for the active mode — the global
blend in Linear/Delta, the early A path *and* the late blend in
Fine, A and B (independently, same shifts) before projection in
Layer, and A and the late blend before projection in Kombi 1/2/3.
The noise tensor is drawn once from the seed-derived PCG64 RNG and
reused across all paths so the Linear invariant "noise added once
to the blend" carries through to the non-linear modes verbatim.

---

## 8. APVTS parameters and non-APVTS state

### What lives in APVTS

Every real-time audio parameter (oscillator scan, filter cutoff, envelope
stages, LFO rate/depth, modulation targets, delay/reverb mix, sequencer
bpm, step values, arpeggiator mode, master volume, etc.) is declared in
`T5ynthProcessor::createParameterLayout()` at `src/PluginProcessor.cpp:19`.
The layout has grown to ~445 lines and is the single authoritative list;
when in doubt, grep this function for a parameter ID.

Adding a new parameter requires three edits:

1. **`createParameterLayout()`** — register the ID and declare the range.
2. **DSP class that consumes it** — usually add a field to
   `BlockParams.h` and populate it in `processBlock` (and then read in
   the voice / DSP class).
3. **Panel that exposes it** — add a slider/combo and attach it via
   `SliderAttachment` / `ComboBoxAttachment` / `ButtonAttachment`.

### What does NOT live in APVTS

Some state is GUI-visible but does not belong on the audio thread and
cannot easily be encoded as a `juce::RangedAudioParameter`. It lives as
plain members on `T5ynthProcessor` and is serialized only via the .t5p
preset format:

- Prompt text A/B — `setLastPrompts`, `getLastPromptA/B`
  (`src/PluginProcessor.h:68`).
- Last seed — `setLastSeed / getLastSeed` (`PluginProcessor.h:72`).
- Axes panel slot state — `lastAxes[3]` with `{dropdownId, value}`
  (`PluginProcessor.h:76`).
- Prompt embeddings — `lastEmbeddingA`, `lastEmbeddingB` (two 768-float
  vectors, used by DimensionExplorer).
- Generated audio — `generatedAudioFull` (post-HF-boost, for playback +
  display) and `generatedAudioRaw` (unmodified VAE output, for re-apply
  on HF toggle) plus `generatedSampleRate`.
- `lastDevice` / `lastModel` — for preset tagging.
- Csound-authored orchestra text, prompt, reading and authored body —
  `getCsoundOrchestraText()` (`PluginProcessor.h:147`),
  `setCsoundPrompt`/`getCsoundPrompt` (`:299-300`),
  `setCsoundReading`/`getCsoundReading` (`:287-288`),
  `setCsoundParamsText`/`getCsoundParamsText` (`:309-310`) — the LCO's
  counterpart to the prompt/embedding state above (§5).

Note that DAW session state (`getStateInformation` /
`setStateInformation`) only captures APVTS, so any of the above that is
needed in a DAW round-trip must be in a preset file or pushed into APVTS.

---

## 9. Binary resources

`CMakeLists.txt:110` calls `juce_add_binary_data(T5ynthData ...)` to bake
runtime assets into the plugin library. Current contents:

- `resources/ir/emt_140_plate_bright.wav`
- `resources/ir/emt_140_plate_medium.wav`
- `resources/ir/emt_140_plate_dark.wav`
- `resources/presets/DEMO T5-Oscillator-Drift.t5p`
- `resources/akroasys_Guide.html`

These are accessed via the generated `BinaryData::` namespace symbols
(`BinaryData::emt_140_plate_medium_wav`, `::emt_140_plate_medium_wavSize`,
etc.). The convolution reverb loads its default IR this way in
`prepareToPlay` (`src/PluginProcessor.cpp:482`), and the Manual overlay
extracts `akroasys_Guide_html` to a temp file before pointing
`WebBrowserComponent` at it.

Adding a new binary resource is a CMake-level change: append the source
file path to the `juce_add_binary_data` block and rebuild — the
`BinaryData` sources are regenerated automatically.

---

## 10. Vendored JUCE

`JUCE/` is a full vendored copy of the JUCE framework
(**version 8.0.6** at time of writing, per `JUCE/CHANGE_LIST.md`).
`CMakeLists.txt` pulls it in with a plain `add_subdirectory(JUCE)`
(line 31); there is no JUCE find_package or Projucer layer.

Guidelines:

- **Do not modify JUCE core.** Treat the `JUCE/` tree as a read-only
  third-party checkout. Local fixes to framework bugs should be made
  only if unavoidable, and must be clearly flagged in the commit log
  because they will complicate future JUCE bumps.
- **`JUCE_WEB_BROWSER=1`** is set in `CMakeLists.txt:142`. This is a
  hard requirement for the Manual overlay (`WebBrowserComponent`).
  Disabling it will fail to link.
- **Linux WebBrowser deps.** `CMakeLists.txt:132` explicitly links
  `juce::pkgconfig_JUCE_BROWSER_LINUX_DEPS` because JUCE 8's
  `juce_gui_extra` module does not auto-declare its webkit2gtk/gtk3
  requirements. Without this, Linux builds fail with
  `gtk/gtk.h: No such file or directory`.

---

## 11. Build outputs

The project convention is to build out-of-source in `build_clean/` using
the `Release` config:

```sh
cmake -S . -B build_clean -DCMAKE_BUILD_TYPE=Release
cmake --build build_clean --config Release -j
```

Plugin artefacts are written under
`build_clean/T5ynth_artefacts/Release/`:

- `Standalone/T5ynth.app` — standalone macOS app (the one JUCE builds
  for "Standalone" target; also `.exe` on Windows, binary on Linux)
- `VST3/T5ynth.vst3`
- `AU/T5ynth.component`
- `../JuceLibraryCode/` — intermediate JUCE module compile units
- `../libT5ynth_SharedCode.a` — the shared static archive linked by each
  plugin wrapper

Do not commit `build_clean/`. Do not use alternate build directories —
sibling scripts (distribution, signing, log paths) assume `build_clean/`.

### Python bundle

The PyInstaller output at `backend/dist/pipe_inference/` is produced
separately by running `pyinstaller backend/pipe_inference.spec` from the
`backend/` directory. In CI (`.github/workflows/build.yml`) this happens
before the C++ build so that the bundle can be copied into the plugin
resources. For local dev, running from a source checkout lets
`PipeInference` fall back to `python pipe_inference.py`, so the
PyInstaller step is optional during iteration.

---

## 12. What is auto-generated

Do not edit these by hand — they are regenerated on every CMake
configure/build:

- **`juceaide`** — JUCE's build helper, compiled once into
  `build_clean/JUCE/extras/Build/juceaide/`.
- **`JuceHeader.h`** — master include umbrella, generated by
  `juce_generate_juce_header(T5ynth)` (`CMakeLists.txt:51`). This is
  the file to `#include <JuceHeader.h>` from; do not include individual
  JUCE module headers directly in T5ynth sources.
- **`BinaryData.{h,cpp}`** — generated by `juce_add_binary_data` from
  the resources listed in §9. Output lands in
  `build_clean/juce_binarydata_T5ynthData/`.
- **`libT5ynthData.a`** — static archive wrapping the binary data, linked
  into the plugin targets via `target_link_libraries(T5ynth PRIVATE T5ynthData)`.

---

## 13. Cross-references

Developer docs being written alongside this file. Some may not yet exist
at the moment you read this — paths are listed so that links can be
followed once they land:

- **`docs/LCO_CONCEPT.md`** — the LCO's goal, architecture, what an
  instrument is, and the platform invariants every instrument must obey
  (§1–§4 of that document). Authoritative for those; its account of the
  implementation predates the model-authored write path.
  **`docs/plans/HANDOVER_LCO.md`** carries the current implementation
  state instead (the library, the author model, the repair loop, measured
  facts, open items). Both referenced from §5.
- **`docs/IPC_PROTOCOL.md`** — exact wire format for the JUCE ↔ Python
  pipe (flag bytes, header layout, error encoding, handshake). Referenced
  from §5 and §7.
- **`docs/ADDING_A_MODEL.md`** — step-by-step HOWTO for plugging a new
  text-to-audio backend into `backend/services/` and advertising it in
  the inference handshake. Referenced from §7.
- **`docs/ADDING_A_MODULATION_TARGET.md`** — HOWTO for adding a new
  `*_target` choice entry, wiring the `processBlock` dispatch, and
  declaring the associated atomic in `ModulatedValues`. Referenced from §6.
- **`docs/PRESET_FORMAT.md`** — complete `.t5p` binary format spec,
  including the JSON payload schema, the current version byte, and the
  legacy JSON/XML fallback detection path. Referenced from §1 and §8.
- **`resources/akroasys_Guide.html`, section 16** — end-user and DSP
  signal flow reference. This is the "how does the audio actually get
  from MIDI to speakers" document and is intentionally not duplicated
  here. Accessible at runtime via the plugin's *Manual* button.
- **`README.md`** — user-facing project description and install
  instructions. Not duplicated here.
