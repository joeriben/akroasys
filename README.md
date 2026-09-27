# akróasys

**ἀ·κρό·α·σις** [aˈkro.a.sis]: *listening, paying attention, hearing*

akróasys is a sonic device that uses generative AI very differently: as the
**oscillator** of a playable
instrument, not as a generator that returns finished audio. The model sits where
a sine or a saw would sit in the signal path, and what it resonates with is what
you say: **resonance with meaning**.

There are two such oscillators. The **T5 Oscillator** reaches into a diffusion
model's latent space of sound *before* that space has become audio: two short
impulses mark it out, and you move through what can become audible between them
— textures, transients, patterns, sonic and musical fragments, field recordings,
everyday noises, orchestral gestures, alien voices, human emotional expressions,
or impossible hybrids. The **Language-Resonant Oscillator** works the other way round: you describe an
instrument, a language model writes the Csound orchestra for it, and that code —
compiled and run live — is what every voice sounds. Your words become the
oscillator itself.

Most AI audio tools keep the model behind a render button and hand back finished
audio. In akróasys, generation is not a separate AI step after the synth: the
synth reaches into the model itself, and what comes out is one stage in a signal
path that continues through filters, envelopes, LFOs, sequencing, delay, reverb
and limiting.

Links:

- User guide: bundled and rendered inside the app; source HTML lives at [`resources/akroasys_Guide.html`](resources/akroasys_Guide.html)
- Preset collection: [`joeriben/T5ynth-Presets`](https://github.com/joeriben/T5ynth-Presets)
- Technical description of the Language-Resonant Oscillator: [`docs/LRO_TECHNICAL_DISCLOSURE.md`](docs/LRO_TECHNICAL_DISCLOSURE.md)
- Developer documentation index: [`docs/README.md`](docs/README.md) · release history: [`CHANGELOG.md`](CHANGELOG.md)
- Latest release: [`v3.1.0-beta.0`](https://github.com/joeriben/akroasys/releases/tag/v3.1.0-beta.0) — the clarinet is a real bore, envelope curves became a continuous travel, and Shift is fine adjustment on every control. [`v3.0.0-beta.1`](https://github.com/joeriben/akroasys/releases/tag/v3.0.0-beta.1) was the first under the new name and the first with the Language-Resonant Oscillator; [`v2.5.3-beta.1`](https://github.com/joeriben/akroasys/releases/tag/v2.5.3-beta.1) is the last one published as **T5ynth**.

Current tagged GitHub Releases publish:

- **macOS** — single `.pkg` containing Standalone, VST3 plugin, Audio Unit (AU) plugin and CLAP plugin.
- **Windows** — `.exe` setup containing Standalone and VST3 plugin.
- **Linux** — *best-effort.* Build from source via [`docs/DEV_BUILD.md`](docs/DEV_BUILD.md) or [`docs/LINUX_INSTALLATION.md`](docs/LINUX_INSTALLATION.md). The CI builds Linux Standalone and VST3 archives plus an Ubuntu `.deb` on every push; they are downloadable as workflow artifacts under the *Actions* tab but are intentionally not attached to release pages and are not officially supported.

---

## What akróasys Does

In a conventional synth, the oscillator produces audio immediately: sine, saw,
square, noise, sample, wavetable. In akróasys a model stands in that place. Two
of them do, in two different ways, and the toggle at the top of the Generation
column decides which one is sounding. Everything downstream — filters,
envelopes, LFOs, sequencers, delay, reverb, limiter, presets — is the same synth
either way.

### The T5 Oscillator

A diffusion model's **latent space** is where sound sits before it is audio. Two
short text impulses, **Impulse A** and **Impulse B**, are read by the model's
text encoder and become two points in that space. The musical act is to move
between and past them and decide which point gets rendered.

A and B are impulses, not a conversation with a chatbot and not a song request.
They mark one space; they do not produce two samples and they are not two
oscillators.

The controls that move the point are **A/B** for the pull between the two,
**Magnitude** for how far the point is pushed from the model's neutral centre,
**Noise** for perturbation, **sound-character axes** for broad directions,
**Dimension Explorer** for individual dimensions, **Injection Modes** for where
B enters A during generation, and **Drift** for movement over time. They work on
the conditioning, not on audio: no signal is being mixed.

Most of the travel they allow leads out of the region any text can address. The
A/B slider runs to ±2, so most of its range lies past both impulses; an empty
field reflects the other one through the model's neutral point, which is an
anti-prompt no wording produces; Magnitude reaches 5.0. The model was not
trained on what it finds there, and that is what this oscillator is for.

**Generate** renders the current point into a short audio fragment, which is
then played back as a **Sampler**, **Wavetable** or **Granular** voice.

So the flow is:

1. Two short impulses mark the latent space — for example "steady clean saw
   wave, C3" and "120 bpm syncopated transient pattern".
2. A/B, Magnitude, Noise, Semantic Axes, Dimension Explorer, Injection Modes and
   Drift move the point that will be rendered.
3. Generate, then play it as Sampler, Wavetable or Granular through filters,
   envelopes, LFOs, sequencers, delay, reverb, limiter, presets, and **Drift
   Modulators & Auto-Regenerate**.

akróasys can use **Stable Audio 3 Small Music**, **Stable Audio 3 Small SFX**,
**Stable Audio 3 Medium**, **Stable Audio Open 1.0**, **Stable Audio Open Small**,
or **AudioLDM2**. Each
engine has its own learned response profile, so the same A/B pair can open a
different space depending on the selected model. The Stable Audio engines are
strongest with English sound-oriented phrases: sound effects, field recordings,
drum or instrument loops, ambient sounds, foley, and production elements. The
**Stable Audio 3** engines are the default since v2.1.0: a diffusion transformer
with a t5gemma text encoder — Small Music for instrumental music, Small SFX for
sound effects. A larger **Stable Audio 3 Medium** checkpoint (one model for both
modalities, deeper transformer, ~16 GB to run) can be installed as a per-machine
alternative to the Small tiers. AudioLDM2 is broader in its training goal and can cover sound effects,
speech-like material, and music, but it is non-commercial only and less tied to
akróasys's newer injection-mode research.

The meaning poles do not have to be narrow "sound ideas". They can evoke sound
sources, materials in motion, acoustic scenes, bodies, gestures, moods,
fictional agents, or impossible combinations. A visual phrase such as "a rose
in a vase" is not a sound by itself; "water in a glass vase, quiet room, petals
brushed by fingers" gives the model acoustic handles. But the abstract phrase
can still be used as a strange marker in the model's space. akróasys is where you
find out what that marker can become.

### The Language-Resonant Oscillator

The T5 Oscillator works inside the diffusion paradigm: sound arrives by
denoising. The **LRO** is the alternative to that paradigm — sound arrives as
*written software*. You describe an instrument — "a bowed cello", "bright
shimmer degrading to a dark rumble" — and a language model, local by default or
an external provider with your API key, writes a Csound orchestra for exactly
that description. The code is compiled and run live, and it is what every voice
sounds: the source code is the sound.

And it is real, portable Csound — the same language any Csound system compiles,
not an internal format of this app. The orchestra is stored with the preset, and
the panel shows it. Every authoring is a fresh piece of writing, so the
oscillator you keep can be one of a kind: a real digital instrument that exists
nowhere else.

The panel shows the whole path rather than a progress bar. What the model was
given, which entries it asked to have opened from the code library, its
reasoning as it streams, the orchestra it wrote, a repair round if the first
attempt did not compile or ran silent, and finally whether the code compiled and
played. The toggle at the top of the Generation column switches between the two
oscillators; **Generate** and the four Snapshot slots stay where they are and
drive whichever one is active.

Csound needs no installing: it travels inside akróasys on macOS, Windows and
Linux alike. The one thing the LRO needs that the T5 Oscillator does not is a
language model — either the local Gemma 4 12B QAT (about 7 GB, ungated,
Apache-2.0), which installs from the Settings page, or an external provider
(OpenRouter, Mistral, IONOS, Mammouth, Anthropic, OpenAI, or a local Ollama)
with an API key, where only the text step leaves the machine. Install neither
and the rest of the instrument is unaffected.

About the name: in the ancient Greek language, ἀκροάομαι is a middle verb with
no ordinary active form, so hearing is not something a subject performs on an
object. It happens in the between, and whoever hears is part of it. *akróasis*
is the noun of that act — and **akróasys** spells the synth into it.

It replaces **T5ynth**. That name pointed at T5, the text encoder the Stable
Audio engines use to turn a phrase into control data for the audio model. Releases up to
**T5ynth 2.5.3** keep the old name and do not have the new oscillator; the
first release called akróasys is **3.0.0**.

From there, akróasys behaves much more like an instrument than like an audio
generator website:

1. The impulses mark a latent space inside the audio model.
2. If you want to, you move through that space with A/B, Magnitude,
   model-space noise, sound-character axes, the Dimension Explorer, Injection
   Modes, and Drift.
3. The diffusion backend renders the current internal state into short stereo
   audio.
4. The synth engine plays that fragment as a sampler source or converts it
   into a scannable wavetable.
5. The rest is synthesis: filter, envelopes, LFOs, drift, sequencers, delay,
   reverb, limiter, presets.

The generated audio is not the final output. It is a playable fragment inside
a larger instrument.

This is also where akróasys parts company with tools that let you pick a style
and shape it from there. Such a tool gives you parameters around a single
chosen result — how strictly the style is followed, how much it varies, which
elements play — but the result itself stays whatever the model would produce.
akróasys's controls act on the **embeddings** instead: A/B moves through the
relation between the two poles, an empty field reflects an impulse back as an
echo chamber, the sound-character axes shift along directions in that space,
and the Dimension Explorer re-weights its individual coordinates. You work on
the model's representation directly — combining, reflecting, re-weighting —
which is how you reach points a single prompt would never land on.

## Why This Exists

Generative audio systems are often designed as black boxes: enter a request,
receive a result, consume the output. That positions the musician outside the
model, after the important decisions have already happened.

akróasys deliberately inverts that relationship:

- **Standard AI audio workflow:** human writes a request -> model hides the
  internal search -> audio result appears. AI company makes money; musicians
  lose commissions, jobs, and control.
- **akróasys workflow:** human marks and moves through the model's latent space -> the model renders a playable fragment -> human plays, shapes,
  rejects, edits, saves, and composes. akróasys will by no means address these
  challenges; it only tries to show that it ain't necessarily so.

This is why akróasys matters even, and maybe especially, for musicians who
are skeptical of generative AI music. It does not automate musical judgment. It
makes the latent space before the result available for musical judgment.

## Research Context

akróasys is a personal side project by Prof. Dr. Benjamin Jörissen, UNESCO Chair
in Digital Culture and Arts in Education (UCDCAE),
Friedrich-Alexander-Universität Erlangen-Nürnberg, and part of the
[UCDCAE AI Lab Software Collection](https://github.com/joeriben/ucdcae-ai-lab).

It is inspired by the results of two of his research projects:

- [AI for Arts Education (AI4ArtsEd)](https://kubi-meta.de/ai4artsed),
  conducted together with the University of Cologne and the German Research
  Institute for Artificial Intelligence (DFKI) Kaiserslautern.
- [ComeArts Across](https://comearts.uni-due.de/comenets/artsacross/), a
  research project on digital cultural teacher education.

Both are funded by the Federal Ministry for Education, Family, Senior Citizens,
Women and Youth (BMBFSFJ).

akróasys is dedicated to my dear colleague at the DFKI, musician and AI
researcher Dr. Stephan Baumann, without whom AI4ArtsEd would not have come
into being.

## What Is New in 3.1.0

- **Every part of a written instrument gets its own controls and its own
  fader.** Ask for "a vibraphone > a saw wave" and both ends now arrive as full
  instruments with all their knobs, each on its own level — where the second end
  used to shrink to a bare oscillator with one control that did nothing.
- **An LRO instrument keeps playing.** Its voices were scored for 100 hours;
  past that the sound froze into a buzz or into silence until you regenerated.
  They now run for about 63 years, and presets saved before this fix are
  corrected as they load.
- **The panel tells you when an orchestra has stopped itself.** A written
  instrument can end its own performance from inside. That used to show as
  nothing but silence; the Reading field now names it, and regenerating brings
  the engine back.
- **A clarinet that is a bore, not an impression of one.** The library entry the
  LRO reaches for when you ask for a clarinet is now Csound's `wgclar` — a
  waveguide with a reed, a bore and a bell — where it used to be FM standing in
  for one. Its controls are named after what they do to the instrument rather
  than after the numbers behind them.
- **Envelope curves became a travel.** Each stage's shape used to be a choice of
  five fixed curves. It is now one continuous control from strongly logarithmic
  through linear to strongly exponential, and the ADSR display draws what you
  set. Presets saved with the old five stops load onto the matching positions.
- **Shift is fine adjustment, everywhere.** Hold Shift while dragging any knob,
  slider or bracket and it moves at a fraction of the speed. It was previously
  on some controls and not others.
- **The LRO plays at the same level as the other engines.** It was 12.4 dB
  quieter than what it is A/B'd against, so every comparison was decided by
  loudness before it was decided by sound.
- **Louder, and one level at every voice count**: the voice-count switch no
  longer changes how loud a note is.
- **MPE**: the zone layout is now JUCE's own, and a controller's NRPN messages
  can no longer be mistaken for a zone declaration or a bend range.
- **The in-app manual** describes the instrument as it is, without arguing for it.

## What Is New in 3.0.0

- **An alternative to the denoising paradigm: the Language-Resonant Oscillator
  (LRO).** Describe an
  instrument in words and a language model writes a Csound orchestra from
  that description. The compiled code — not a sample, not a preset — is what
  every voice sounds. The panel discloses the whole path while it happens: what
  the model was given, which library entries it asked to have opened, its
  reasoning as it streams, the code it wrote, any repair round, and whether the
  orchestra compiled and ran.
- **The instrument is called akróasys.** See *About the name* above.
  T5ynth 2.5.3 remains the last version under the old name and without the LRO,
  and it stays installable beside this one.
- **The LRO needs a language model, and nothing else.** Csound ships inside the
  app on all three platforms. The authoring model is either local (Gemma 4 12B
  QAT, about 7 GB, ungated, Apache-2.0, installed from the Settings page like
  any other model) or an external provider with an API key. The T5 Oscillator
  and everything downstream of it are unaffected if you install neither.

See [`CHANGELOG.md`](CHANGELOG.md) for the full release history.

## Core Concepts

### The Latent Space of Sound

Traditional oscillators generate sine, saw, square, noise, or sample playback.
The T5 Oscillator starts inside the model, in the space where meaning shapes
what sound can become — including the parts of it no wording reaches.

Behind the scenes, that space is numerical. You can ignore that while playing,
just as you can use FM without solving the equations.

- **Impulse A/B** marks the latent space and the **A/B** slider moves through it
  and past both ends — its range is ±2. The midpoint is a new internal state,
  not an audio crossfade.
- **Magnitude** changes how strongly that state steers the diffusion model.
- **Model-space noise** perturbs that state.
- **Sound-character axes** add musically legible directions such as noisy/tonal or
  sustained/rhythmic. Sometimes they work better, sometimes not, depending on
  the impulses.
- **Dimension Explorer** opens all 768 internal control dimensions directly.
  You will mostly learn that AI-generated meaning making is a black box humans
  do not really understand, but sometimes the results are unexpectedly
  interesting.
- **Injection Modes** change where B enters A inside the
  diffusion process: Linear, Step-in, Layer, Combo 1, Combo 2, Combo 3.
- **Drift** keeps the latent space moving over time and can trigger new
  generations in the background.
- **Re-Prompt** closes a second loop through language rather than parameters:
  after each render a machine-listening model (CLAP) hears the output and a local
  LLM rewrites the prompt under a chosen *stance*, so the words drift, not just
  the controls.

### Drift Modulators & Auto-Regenerate

Drift Modulators & Auto-Regenerate turn the latent space into something
that can evolve. Three slow Drift LFOs can target generation-level parameters
such as A/B, Semantic Axes, Noise, and Magnitude. When Auto-Regenerate is active,
akróasys generates new audio in the background as the latent space moves,
then crossfades the new fragment into playback. Depending on the host machine,
regeneration can range from roughly 0.1 seconds on an RTX 6000-class GPU to
several seconds on a Mac M-series processor without a dedicated AI-capable
graphics card.

With v1.7, those drift cycles can be clock-synced to musical divisions, so
long sound-space motion can sit inside a DAW, sequencer, or standalone tempo
workflow.

### Sampler, Wavetable and Granular Modes

akróasys can play a generated fragment three ways:

- **Sampler Mode** plays the generated fragment directly with loop modes,
  crossfade, normalization, and pitch following through time-stretching.
- **Wavetable Mode** extracts pitch-synchronous single-cycle frames and turns
  the generation into a playable, scannable wavetable oscillator.
- **Granular Mode** reads the fragment as a cloud of overlapping grains, with
  grain size, density, spray and scan position under modulation.

## Feature Overview

- **Oscillators:** T5 Oscillator (a diffusion model's latent space) and
  Language-Resonant Oscillator (a language model writes the Csound orchestra),
  switched from the Generation column's header.
- **Generation:** Impulse A/B, the A/B slider, Magnitude, Noise, Duration,
  Seed, Start Position, HF Boost.
- **Source controls:** Sound-character axes, 768-dimension explorer, Linear/
  Step-in/Layer/Combo injection modes.
- **Playback engine:** Sampler, Wavetable and Granular modes, loop
  optimization, wavetable scan, noise source.
- **Synthesis:** voice manager, seven settings from Mono to 64 voices
  (capped at 16 in LRO mode), assignable envelopes, multimode filter algorithms,
  keyboard tracking, drive, modulation ghost indicators.
- **Modulation:** 5 ADSR envelopes (ENV 1 starts on the DCA), 3 LFOs, 3 Drift
  LFOs, free or clock-synced rates, per-LFO note-reset lock.
- **Sequencing:** Step sequencer, arpeggiator, polyphonic generative sequencer
  with up to five strands and a shared pitch field.
- **Effects:** Tempo-syncable delay, convolution and algorithmic reverb,
  limiter.
- **Live performance:** Four **Snapshot** slots bound to keys
  <kbd>1</kbd> / <kbd>2</kbd> / <kbd>3</kbd> / <kbd>4</kbd> switch the entire
  oscillator state (audio, prompts, axes, dimensions, engine parameters)
  in one keypress — useful for live keyboard play and for swapping one-shot
  voices during step-sequencer playback. An optional **Inference Cache**
  (2–64 slots) turns Generate into a zero-latency round-robin audition of
  earlier results; cache contents travel with the preset.
- **Presets:** `.t5p` files store parameters, A/B texts, generated audio, and
  internal source data, so loading a preset does not require regeneration.

## Architecture Summary

akróasys has two main parts:

- A JUCE/C++ synthesizer, UI, preset, modulation, sequencing, and DSP engine.
- A Python inference subprocess that runs the diffusion model and communicates
  with JUCE through a binary stdin/stdout pipe protocol.

The Python backend is used because the BrownianTree SDE sampler and model
runtime are not available as equivalent C++ components. The subprocess stays
alive between generations, so repeated generations avoid backend startup
overhead.

For code-level details, see [`ARCHITECTURE.md`](ARCHITECTURE.md),
[`docs/IPC_PROTOCOL.md`](docs/IPC_PROTOCOL.md), and the signal-flow section in
the user guide.

---

## Building

If you just want to install akróasys, use the tagged GitHub Release assets:
current public releases provide ready-made macOS and Windows installers.

The old minimal snippet in this README was not enough to produce a working
Linux build. The authoritative build guides now live here:

- Linux / Fedora 42 source build on a developer/build host: [docs/LINUX_INSTALLATION.md](docs/LINUX_INSTALLATION.md)
- Linux package-layer docs, currently Fedora RPM and Ubuntu/Debian `.deb` from a prebuilt isolated backend bundle: [docs/LINUX_PACKAGING.md](docs/LINUX_PACKAGING.md)
- Cross-platform developer build guide: [docs/DEV_BUILD.md](docs/DEV_BUILD.md)

Linux now has one common build contract and multiple distribution layers:

- the Ubuntu CI `linux` job produces Linux base archives (`akroasys` plus sibling `backend/`)
- Fedora RPM wraps that same app/backend layout for installation on Fedora
- Ubuntu/Debian `.deb` wraps that same app/backend layout for installation on Ubuntu-family systems

### Quick Source Build

This is the developer/build-host path. It creates a repo-local `.venv`,
installs Python dependencies there, and freezes the backend bundle locally.
It is not the target-machine installer path.

```bash
# Clone
git clone https://github.com/joeriben/akroasys.git
cd t5ynth

# Python backend
python3.11 -m venv .venv --clear
source .venv/bin/activate
python -m pip install --upgrade pip setuptools wheel
python -m pip install pyinstaller
python -m pip install torch==2.7.1 torchaudio==2.7.1 torchvision==0.22.1 --index-url https://download.pytorch.org/whl/cu128  # Linux/Windows NVIDIA
# python -m pip install torch==2.7.1 torchaudio==2.7.1 torchvision==0.22.1  # macOS or CPU-only fallback
python -m pip install -r backend/requirements.txt

# Bundle backend
( cd backend && pyinstaller pipe_inference.spec --noconfirm )

# Configure + build
cmake -S . -B build_clean -DCMAKE_BUILD_TYPE=Release
cmake --build build_clean --config Release

# Linux standalone layout
mkdir -p dist/akroasys/backend
cp build_clean/T5ynth_artefacts/Release/Standalone/akroasys dist/akroasys/
cp -R backend/dist/pipe_inference/* dist/akroasys/backend/
./dist/akroasys/akroasys
```

For Linux package-layer installation, do not rebuild Python/Torch on the target
machine. Build the isolated backend bundle once on a build host, stage it into
a named release bundle, then wrap that selected bundle into the RPM or `.deb`
described in [docs/LINUX_PACKAGING.md](docs/LINUX_PACKAGING.md).

### Model Download

akróasys requires at least one diffusion model. Audio model weights are not bundled — they must be downloaded separately. The Stable Audio Open engines (1.0 and Small) also need the auxiliary T5-Base text encoder (ungated, Apache-2.0), which installs automatically with the engine; Stable Audio 3 uses its own t5gemma encoder, fetched at install.

Use the **Settings** panel on first launch:

1. **Stable Audio 3 Small Music / Small SFX** *(new in v2.1.0, recommended default)* — Stability AI's current small-format engines: a diffusion transformer with a t5gemma text encoder, for instrumental music (Music) and sound effects (SFX). Select the model, accept the license dialog, and click *Download*. Two licenses apply and both must be accepted in the dialog: the [Stability AI Community License](https://stability.ai/community-license-agreement) (audio model) and the [Google Gemma Terms of Use](https://ai.google.dev/gemma/terms) with the Gemma Prohibited Use Policy (t5gemma encoder). The weights then download without a HuggingFace account or token, and copies of both licenses are written into the model folder. If you already have the files from Stability, use *Auto-Scan* instead.
2. **AudioLDM2** — an academic latent-diffusion text-to-audio model published by CVSSP / University of Surrey and collaborators ([Liu et al., 2023](https://arxiv.org/abs/2308.05734)), released as an open research artefact for studying generalised audio, music, and speech generation from text. Ungated on HuggingFace and installable directly in-app. Licensed under [CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/) — **non-commercial only, no revenue threshold, no exceptions**. Included as an alternative sound source for non-commercial musical exploration.
3. **Stable Audio Open 1.0** — the original full-size Stable Audio Open checkpoint, licensed under the [Stability AI Community License](https://stability.ai/community-license-agreement). Select it, accept the license dialog, and click *Download*; the ~4.85 GB checkpoint then downloads without a HuggingFace account or token, and a license copy is written into the model folder. If you already have the files from Stability by hand, use *Auto-Scan* instead.
4. **Stable Audio Open Small** — the compact, fastest Stable Audio Open checkpoint, licensed under the [Stability AI Community License](https://stability.ai/community-license-agreement). Select it, accept the license dialog, and click *Download*; the ~1.68 GB checkpoint then downloads without a HuggingFace account or token (from an ungated mirror that redistributes the official weights unmodified), and a license copy is written into the model folder. If you already have the files from Stability by hand, use *Auto-Scan* instead.

Manual install locations if you prefer to place files yourself:

| Platform | Models Directory |
| --- | --- |
| macOS | `~/Library/T5ynth/models/<model-id>/` |
| Linux | `~/.local/share/T5ynth/models/<model-id>/` |
| Windows 11 | `%APPDATA%\T5ynth\models\<model-id>\` |

After a manual install, click *Auto-Scan* in Settings to register the model.

---

## License

akróasys is licensed under the **GNU General Public License v3.0** — see [LICENSE.txt](LICENSE.txt).

This means you are free to use, modify, and redistribute akróasys, provided that derivative works are also released under GPLv3 with source code available.

### Third-Party Components

- **Stable Audio 3 Small Music / Small SFX** — audio model under the [Stability AI Community License](https://stability.ai/community-license-agreement); t5gemma text encoder under the [Google Gemma Terms of Use](https://ai.google.dev/gemma/terms) and the Gemma Prohibited Use Policy. Weights are not included in this repository; they download without a HuggingFace account or token, both licenses are accepted in-app, and copies are written into the model folder. Powered by Stability AI.
- **Stable Audio Open 1.0 / Stable Audio Open Small** — [Stability AI Community License](https://stability.ai/community-license-agreement). The models are not included in this repository. Users download them separately and must accept their license. Powered by Stability AI.
- **AudioLDM2** — [CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/). Non-commercial use only. Not included; users download separately.
- **JUCE Framework** — AGPLv3 (vendored in `JUCE/`). Provides the application framework and DSP building blocks used by the SVF, delay, convolution reverb, algorithmic reverb, limiter, and oversampling paths.
- **EMT 140 plate reverb impulse responses** — Greg Hopkins, Creative Commons Attribution (CC BY). Used for the Dark, Medium, and Bright convolution reverb modes.
- **Signalsmith Stretch** — Geraint Luff / Signalsmith Audio Ltd., MIT. Used for pitch-preserving sample transposition.
- **nlohmann/json** — Niels Lohmann, MIT. Used for configuration and preset parsing.
- **SentencePiece** — Apache 2.0. Used for the native C++ T5 tokenizer.
- **Python inference stack** — `diffusers`, `transformers`, PyTorch, `torchsde`, `soundfile`, and SciPy provide the model pipeline, tensor runtime, sampler support, audio I/O, and signal-processing utilities used by the backend.
- **Huovilainen ladder-filter reference** — Antti Huovilainen's DAFx-04 paper is credited for the non-linear digital ladder topology implemented in akróasys.
- **Cutoff Warp filter inspiration** — Surge XT is credited for the musical idea of a style-switchable cutoff-warp character control. akróasys's implementation is written from scratch; no Surge XT source code is copied.

akróasys would be much poorer without these projects, papers, impulse responses,
and DSP references. See [THIRD_PARTY_LICENSES.txt](THIRD_PARTY_LICENSES.txt)
for full license details, URLs, and attribution notes.

### Citation

If you use akróasys in academic work, cite it from
[`CITATION.cff`](CITATION.cff), which GitHub renders as a citation block in the
sidebar and which tools read directly:

```text
Jörissen, B. (2026). akróasys (Version 3.1.0) [Computer software].
UNESCO Chair in Digital Culture and Arts in Education (UCDCAE),
Friedrich-Alexander-Universität Erlangen-Nürnberg.
https://github.com/joeriben/akroasys
```

For the oscillator itself rather than the software, cite
[`docs/LRO_TECHNICAL_DISCLOSURE.md`](docs/LRO_TECHNICAL_DISCLOSURE.md) — the
standalone technical description of the Language-Resonant Oscillator, written
for a reader outside the project.

### Documentation Note

Parts of the early project text and user manual were drafted with
AI-assisted co-coding tools and edited by the human author.
