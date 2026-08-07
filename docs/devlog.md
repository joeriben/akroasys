# T5ynth Development Log

## 2026-08-07 — the typing keyboard stopped asking the system which keys are down

BJ, on the typing keyboard: *"Taste a funktioniert komplett im Mac, hängt
nirgendwo, a a a a kann ganz normal verwendet werden. Im Synth: Neustart. Ich
spiele Taste s (ein D). Taste a (ein C) klingt sofort mit und hängt. → Panic.
Alles aus. Ab dann Taste a: kein Sound."* And, holding s and pressing the octave
keys: *"Taste a (C) klingt mit obwohl nicht gedrückt — und hängt wieder."*

One measurement explains every one of those sentences. `CGEventSourceKeyState`,
the API `t5::physicalKeyDown` used to read, reports **keycode 0x00 — physical A,
the typing keyboard's C — as held, permanently, with nothing pressed**. A sweep of
0x00…0x7F finds exactly that one key; `kCGEventSourceStateHIDSystemState` and
`kCGEventSourceStateCombinedSessionState` both say it, and Carbon `GetKeys()`
agrees, so all three read one shared, global map that no process here can clear.
`tools/probe_physkey.cpp` is that measurement.

From there the symptoms follow mechanically. `scanComputerKeyboard` re-reads
*every* mapped key on *every* keypress, so pressing s started C as well; nothing
ever released it, because release needs the read to go false. Panic silenced the
voice but left `computerKeyboardNotesDown[0]` true, so the next a press compared
equal to the stored state and did nothing at all — the dead key. And
`shiftComputerKeyboardOctave` releases held notes before applying the new offset,
after which the same scan immediately re-started the phantom C: y or x alone, a
hanging C, no note key touched.

The instrument reads physical key POSITIONS rather than characters so that the
same keys play the same notes on a German and a US layout, and that stays. Two
things changed underneath it.

**A note may only BEGIN on an actual keystroke.** The scan used to ask one
question — *is this key down?* — and answer it for all twenty keys on every key
press, which is what turned one wrong bit into a phantom note on every other
key's press. It now asks two, and they come from different places. What starts a
note is a STRIKE: a key-down event that has arrived and that no pass has acted on
yet, drained once per pass. What ends one is the held-state. A key that wrongly
reads "down" can then only delay a release; it can never manufacture an attack.
This is the part that makes the class of bug impossible rather than the instance
of it fixed, and it has a second, unrelated benefit: a tap short enough to be
over before the message thread gets to it now sounds, where asking "is it down
now?" already answered no and dropped the note.

**Held-state is the AND of two sources**, because each covers exactly the other's
failure — and both failures are the same thing, a note that will not stop. The
app-local map fed by our own `NSEvent` key events (`PhysicalKeyStateMac.mm`, a
local monitor, no Input-Monitoring permission) cannot report a release it never
received: AppKit delivers no key-up while Command is held, and its own header
states a local monitor is not called for events consumed by nested event-tracking
loops — control tracking, menu tracking, window dragging. The system key state
cannot miss a release, and it is the one that latched. Either source saying "up"
ends the note. Where a keycode is latched the AND degenerates to the map alone,
which is why it is a floor and not the guarantee, and why starting a note does
not depend on it at all.

The map additionally forgets everything while Command is held (both edges,
derived per event from the event's own flags, so a `flagsChanged` swallowed by a
tracking loop cannot strand it either way), on `NSApplicationDidResignActive`,
and on `NSMenuDidEndTracking`. Window dragging is deliberately left uncovered:
`NSWindowDidMoveNotification` fires for every window in the process moving for
any reason, and JUCE builds each dropdown by adding a window to the desktop and
then positioning it — so that observer would kill a held chord every time a combo
box is opened. Measured, not assumed.

Because the scan may no longer start anything by itself,
`shiftComputerKeyboardOctave` re-starts the still-held notes at the new offset
explicitly. Holding a key and pressing the octave keys still moves the note; it
just no longer happens as a side effect of re-reading key state.

Two more things worth keeping in mind for anything that touches this file. The
map is process-global, so it must never be cleared from per-editor state — a
second plugin editor sitting with its typing keyboard off would otherwise wipe,
twenty times a second, the key state the instance you are playing reads from. And
the local monitor runs inside `sendEvent:` *before* the responder chain, which is
what lets `MainPanel::keyPressed` read an already up-to-date map for the very key
press being delivered; that ordering is AppKit's documented contract and was
verified against a running app rather than assumed.

Two defects in this area are older than this change and survive it, both in the
same shape: a key released and struck again inside one 50 ms poll tick is
swallowed, because `computerKeyboardNotesDown` is only reconciled by the poll, so
a fast repeated note can sound as one held note (the same applies to two quick
taps of an octave key). The strike bit is exactly the signal that would tell a
re-strike from OS auto-repeat, so this is now cheap to fix — but it changes when
notes retrigger, which is a separate decision from stopping the phantom.

Not fixed, and now stated where it matters rather than papered over: on Windows
JUCE's `isKeyCurrentlyDown` goes to `GetAsyncKeyState`, the same kind of global
state, so a key held in another application is visible to the fallback path in
`PhysicalKeyState.cpp` too. That path is already layout-dependent and wants its
own scancode rewrite; both belong in the same change.

## 2026-08-05 — Shift is fine adjustment, on every control that is dragged

BJ, after playing the new envelope curves: *"die Maus-Skalierung ist etwas zu grob
… man wischt praktisch von einer Ecke des Synths in die andere — Vorteil natürlich
gute Feineinstellung. aber da hatte ich auch für A/B immer schon gedacht: ein
globaler switch dass mit gehaltenem Shift alle Regler nur noch 10% fahren?"*

They do now. `FineDrag` (`GuiHelpers.h`) accumulates the pointer's movement
scaled by `kFineDragScale = 0.1` while Shift is down — incrementally, not as a
re-mapped distance from the grab point, so pressing or releasing Shift in the
middle of a gesture changes the rate from there on and never jumps the value.
`MidiLearnSlider` feeds every drag through it, which covers every `SliderRow` in
the instrument and the A↔B blend that derives from it; `masterVolKnob`, the
sound-character axis sliders and the replay speed slider stopped being bare
`juce::Slider`s to join them; `AdsrGraph` uses the same accumulator on all six of
its handles.

Two places keep Shift for what it already meant. The aftertouch and velocity bars
are absolute — the bar position *is* the value — so a fine mode there would have
to make them relative. And on a two-value slider JUCE itself spends Shift on
dragging both thumbs while holding the gap, which is what the A↔B fader becomes
in Layer Split; `MidiLearnSlider` therefore scales nothing on `TwoValue*` /
`ThreeValue*` styles. Taking a gesture that already exists would have been the
worse trade.

One behaviour changed without being asked for: right-clicking the master fader
used to jump it to the click point, because a bare `juce::Slider` treats the
right button as a drag. `MidiLearnSlider` swallows it, which is why it exists.

Because a precise gesture now exists, the ordinary one got shorter: a segment's
whole bend range is dragged over `kBendTravel = 0.75` of the plot height instead
of one and a half of them — half the travel it had this morning. Plain drag moves
about 0.03 of bend per pixel, Shift about 0.003, i.e. finer than the parameter's
own 0.01 step.

## 2026-08-05 — The envelope curve became a travel, and the release started meaning its own number

BJ: *"könnten die splines der envs auch kontinuierlich sein? oft treffen sie nicht
so gut."* Five fixed shapes per stage, and the one you want is rarely among them.

**The axis.** Each of the 15 per-stage curve parameters (5 envelopes × A/D/R) is a
bend instead of a 5-entry choice, with the old shapes as five named points on it:
`-1` Log, `-0.5` SLog, `0` Lin, `+0.5` SExp, `+1` Exp. (How far past the named
ends a stage travels is the next section but one.) Those five points are not
approximations of the old shapes,
they ARE them — `tools/audition_env_bend.cpp` asserts bit-identity against a
frozen transcription of the pre-change code, and the two named anchors on each
side are returned by the original expressions rather than by the blend, because
`1 - inv*inv` contracts into an fma and the blend's two-statement form does not
(one ulp, inaudible, but the claim is either true or it is a hedge). Between the
anchors, attack and decay interpolate `t → t² → t³ → t⁴ → t⁵` — multiplies only,
no `pow` on the audio thread.

**What it cost, and it was the release.** Exp and SExp there were never
progress-based: they were an RC discharge with τ = release/5 resp. release/3
against a -80 dB cutoff, so a release ran 1.84× resp. **3.07×** the time on the
fader and stood at -43 dB resp. -26 dB when that time was up. The fader did not
mean what it said, and the ADSR graph had always drawn the *normalised* curve,
i.e. the one that ends where the handle is. The release now follows what was
drawn: the same discharge (through k=3 at SExp and k=5 at Exp, k=0 at Lin, so the
two halves meet without a step), offset and rescaled to reach
zero AT the set time. Mid-release the level differs by 0.7 dB (Exp) / 1.75 dB
(SExp) from before; what disappears is the tail that used to run after the note's
own release time. Loop mode with a concave release is now a cycle of A+D+Hold+R
rather than one stretched by that tail. This is the one deliberate change to an
existing sound, and it is why the release could join the same axis at all.

**One unit past the named end, on one side only.** BJ, same day: the maximum was
not steep enough *"zumindest für die langsam ansteigende/schnell fallende"* — the
direction each stage sags towards on screen — while the other one *("ein ewiges
quasi-sustain und superschneller Absturz")* was already strong enough. So the
range is asymmetric and differs by stage: an attack travels to −2 (quintic: 3.1 %
of the way up at half the attack time, against 12.5 % at Log), a decay and a
release to +2 (quintic resp. RC k = 8.33, where `k(b) = 5·b^log2(5/3)` — a power
law through the same two anchors as before, chosen over a quadratic because it
keeps climbing instead of turning back). That last one is what BJ was after: the
fall has to be over at the release time either way, so a steeper k does not
shorten the release, it lengthens the *tail* inside it — the share of the release
below −20 dB goes from 55 % at Exp to 72 % at the new end.

**What that costs, once.** A stored *normalised* value no longer means what it
did — the poles moved when the range widened. Nothing of ours reads one (the .t5p
JSON stores the shape by key, the session XML and the .t5p snapshot trees store
the denormalised value and travel through Calibration epoch 9, and both map onto
the anchors exactly), but a DAW automation lane does, and a lane written against
the old 5-way choice now lands one shape off. Lanes live in the host, not in our
state, so there is nothing to migrate. A preset still writes the nearest named
key beside the exact bend, so a build older than today reads a preset from a newer
one and lands on the closest curve it owns instead of `choiceFromKey`'s index-0
fallback (Log).

**On screen.** The segment body is dragged now, not clicked: the curve bends
towards the cursor (up on the attack, down on the decay and release) and the
read-out names the pole and the distance (`Exp 0.62`). And the three timed stages
stopped sharing one width — attack and decay span 5 s, release 10 s, so equal
thirds drew the same millisecond at two different x. A stage's width now goes as
`end^skew` of its own range (release 2^0.3 = 1.23× the others, not 2×), read off
the parameters so a range change cannot leave the drawing behind.

## 2026-08-04 — CLAP plug-in format, macOS only

A fourth artefact, `akroasys.clap`, built from the same shared code as the
Standalone, VST3 and AU by [clap-juce-extensions](https://github.com/free-audio/clap-juce-extensions)
(MIT, pinned to commit `54b3c326`). One processor, one APVTS, one preset format —
there is no second implementation to keep in step.

**Why macOS only.** On macOS a `.clap` is a real bundle, so everything the VST3
and AU already do applies to it unchanged: the Csound payload goes into
`Contents/libs`, the binary's `@loader_path/../libs/CsoundLib64` resolves,
`tools/bundle_csound_macos.sh` and `tools/verify_csound_bundle.py` work without a
line of change, and the `.pkg` gains a component. On Windows and Linux a `.clap`
is a bare DLL / `.so` with nothing around it: the Csound payload beside it would
land in `Common Files\CLAP\` or `~/.clap`, a directory shared with every other
vendor. `src/dsp/CsoundEngine.cpp` deliberately anchors both the library load and
the opcode directory on *our own module's* directory (the Windows loader keys
modules by base name, so anchoring on the loaded library would hand us another
Csound plugin's installation) — so those two platforms need a per-product data
directory and a loader fallback first. That is its own piece of work, not a
side effect of this one.

**What it cost, beyond the two lines of CMake:** `T5YNTH_CLAP_FORMAT`, a variable
that is `CLAP` when the target exists and empty otherwise, appended to the three
per-format loops that already existed (ad-hoc signing, Csound bundling, the
charset gate). CLAP cannot go in `T5YNTH_PLUGIN_FORMATS` — `juce_add_plugin`
would reject the name, because JUCE has no CLAP client and has never promised
one.

**`CLAP_USE_JUCE_PARAMETER_RANGES ALL`**, not the default. Without it the wrapper
hands the host normalised 0..1 values while JUCE's own formats expose the
parameter's real range, so one automation lane would read `0.5` in a CLAP host and
`10010 Hz` in the VST3. Verified through the CLAP ABI: `Filter Cutoff` reports
`20.000 .. 20000.000`.

**One real find.** `supportsMPE()` was never overridden, so it returned JUCE's
default `false`. The synth has always parsed a full MPE zone out of raw MIDI
(ch1/ch16 master, member channels for per-note bend, pressure and CC74, RPN 0 for
the bend range) — it simply never told anyone. Two wrappers ask: the CLAP one
only offers `CLAP_NOTE_DIALECT_MIDI_MPE` when it is true, and the AU one answers
`kAudioUnitProperty_SupportsMPE` with it. So an MPE controller in a CLAP host, and
in Logic, was reaching a synth that would have understood it. The VST3 and
Standalone paths never read the flag, so nothing there changes.

**And behind it, a second one that was already live.** Channel 16 was treated as a
zone master for pressure, CC74 timbre and the RPN 0 bend range, while pitch bend
already treated it as a member. Per MMA MPE — read off `juce::MPEZoneLayout` in
this tree rather than from memory — channel 16 masters the UPPER zone and nothing
else; in the fifteen-channel lower zone that Logic, Bitwig and Live default to it
is an ordinary member, so one note in fifteen was swelling every voice and losing
its slide. The layout is now learned from the MPE Configuration Message (RPN 6)
instead of assumed, with JUCE's own shrink rule for two zones that would exceed
fifteen channels.

Two adversarial passes were needed and both were worth it. The first revision
tried to keep the Launch Control XL safe by folding `dawModeActive_` into the
predicate — which quietly turned channel-16 pitch bend into a global bend for
anyone who merely had an XL selected as MIDI *output*, at the one site that never
had the defect. The second tried to fix channel 1 for an upper zone of fifteen
members, which split channel 1's meaning between the expressions for a layout
nothing else in the file handles. Both came back out. What remains is one
predicate, three call sites, and a stated cost: a plain non-MPE controller
transmitting on channel 16 now sends per-note pressure rather than zone-wide.

---

## 2026-08-04 — Where the synth sits between the two modular traditions (analysis, no code change)

BJ raised the question: the T5 Oscillator looks West-Coast-ish (timbre made at
the source, not carved out of a sawtooth), but the machine around it is
East Coast — filter, classical modulators, effects. Is that inconsistent?
Nothing was rebuilt; this entry records the reading and the one manual change
that came out of it.

**The chain as built**, re-read rather than remembered. Per voice: engine
(Wavetable / Sampler / Granular / Csound) → noise mix → drive (pre-filter
`tanh`, 2/4/8× oversampled, or the Ladder's own stages) → filter (SVF /
Ladder / CutoffWarp) → VCA. Post-sum: `AmpEffects` (distortion → chorus →
phaser → tremolo) → delay → parallel reverb send → limiter → master.
Source → VCF → VCA with ADSR and LFOs beside it. Grepped for the West Coast
idioms: no wavefolder, no low-pass gate, no vactrol, no function generator,
and the terms appear nowhere in the tree.

**The T5 Oscillator is not actually a West Coast source.** It has one of the
three load-bearing properties — complexity at the source rather than by
subtraction. The other two are absent: the timbre is not playable at audio
rate (a generation is a fixed buffer; A/B, Magnitude, Emb. Noise and the axes
move at regeneration rate with the equal-power crossfade), and there is no
non-linear timbre-generating element under CV. What it is, architecturally, is
a PCM/digital source — the PPG Wave / Fairlight / Prophet VS / wavetable
lineage, which has always been paired with exactly this subtractive machine.
The topology is the norm for this kind of source, not a mismatch.

**Where the West Coast idiom does sit in this synth**, unlabelled: in the
modulation destination lists, not in the module chain. `DriftTarget`'s first
seven entries are generation-side (A/B, Emb. Noise, Magnitude, Axis 1–3,
Resynth), and Scan is a destination for envelope, LFO, aftertouch and drift —
modulation into the making of the timbre rather than into the subtraction
behind it. Add the looping envelopes and the ENV↔LFO cross-wiring
(`EnvTarget::LFO*Rate/Depth`, `AftertouchTarget::Env*Sustain`) and the
modulator section is closer to a patch than to a Minimoog.

**The LRO side is where the real tension is**, and it is documented rather
than accidental: `docs/LCO_CONCEPT.md` §4 states the East Coast contract
outright — the oscillator is a spectrum source, the synth owns envelope,
glide, filter and expression. But that same section has moved the boundary
four times, always toward the body (three withdrawn clauses 2026-07-27/28,
the `wave` convention retired 2026-07-26), and movement-by-default plus
`; DECAY: SELF` describe a source that is already complete. That is the West
Coast stance returning through the back door — inside the LRO's own rule set,
not between the two oscillators.

**What the filter actually does to this material** (the part that reached the
manual). A lowpass over a sawtooth performs a *count*: partials sit on an
exact grid with nothing between them, and the source's own spectral envelope
(1/n, i.e. 6 dB per octave of harmonic number — not the filter's slope) falls
evenly, so cutoff decides how many survive and the filter's slope becomes the
resulting spectral envelope. Over the T5 Oscillator's material it performs a *tilt*
instead. Sampler and Granular pass the generated buffer's own spectrum, which
has energy between the partials (noise floor, transients, room) and already
carries a spectral envelope of its own — so the filter multiplies onto an
existing envelope instead of authoring one, the same cutoff value reads
differently from generation to generation, and resonance colours a band
rather than lighting single partials. Wavetable is the honest exception:
`extractFramesFromBuffer` (`WavetableOscillator.cpp:475`) resamples one
detected period per frame, snaps to a zero crossing and ramps out the
boundary difference, so the frame is periodic and therefore strictly
harmonic — but the amplitudes come from an arbitrary period of a recording
(peaks and gaps, not a monotone rolloff) and shift as Scan moves, so a given
cutoff is still not a fixed amount of brightness. And subtraction never adds
structure: a grainy or inharmonic generation stays that way.

Practical consequence, and the only change made: Bandpass, the 6/12 dB slopes
and Filter Mix shape this material more predictably than the 24 dB resonant
sweep, which is at its strongest on a spectrum the filter itself builds. A
note to that effect was added to §1.3 of `resources/akroasys_Guide.html`, with
a pointer from the Filter section of §3.

## 2026-06-12 — Maintainer machine: bank saves overwrite directly (no "(mine)" fork)

"(mine)" semantics clarified by the maintainer: on the mother machine those
files were always *pending updates* to the same-named bank presets — the
Phase-4 fork-on-save rule (and the tag-edit fork) just kept duplicating
them into stale parallel versions, the source of the "half edited, half
reset" preset confusion. New `PresetFormat::userPresetsDirIsGitCheckout()`
(.git as direct child of the user presets dir) now disables both forks on
the maintainer checkout only: Save prefills the original name + bank and
overwrites via the normal Replace flow; tag edits patch in place; git
carries the change upstream. End-user installations are unchanged. The
"(mine)" backlog was applied and published the same day (Echoes of a
Laughing Kalimba Gran, frenzy dream, Evil Beauty → preset repo `cdaf5bc`);
`docs/PRESET_LIBRARY_MAINTENANCE.md` re-states the semantics.

## 2026-06-12 — Preset publishing workflow documented; path docs corrected

The "mother machine" arrangement was undocumented: on the maintainer Mac,
`~/Library/T5ynth/presets/` is simultaneously the plugin's live preset
directory and a git checkout of joeriben/T5ynth-Presets (sparse-checkout
materializes only `*.t5p` + `scripts` + `.github`, keeping manifest.json /
README / LICENSE virtual). Saving into "UCDCAE AI Lab/" overwrites the
tracked file; publishing = explicit-path `git add` + commit + push, CI
regenerates the manifest. Root-level and "(mine)"-suffixed presets are
personal and stay untracked. Written up in
`docs/PRESET_LIBRARY_MAINTENANCE.md` (indexed in CLAUDE.md).

Path corrections in the same pass: `getUserPresetsDirectory()` derives
from JUCE `userApplicationDataDirectory`, which is `~/Library` on macOS —
not `~/Library/Application Support`. Fixed in `PresetUpdater.h`'s
docstring and the 2026-04-11 entry below; `MACOS_INSTALLATION.md` also
still listed the system-wide presets dir that installers stopped creating
on 2026-05-22.

## 2026-05-22 — Bank Collapse: Drop Factory + Read-Only

Three banks ("Factory", "My Presets", "UCDCAE AI Lab") with near-identical
contents collapsed to a single user-writable library. The driving principle:
*user sind mündig* — no read-only gating, no save-redirects, no orphan-prune
that silently deletes user files. Three commits, one concern each.

### Code (`ce23261b`)

- `PresetFormat::getFactoryPresetsDirectory()` and `isInReadOnlyBank()` are
  deleted. `getReadOnlyBankName()` renamed to `getBundledBankName()` — same
  return value (`"UCDCAE AI Lab"`), purely descriptive now. `getAllPresetFiles()`
  scans the user directory recursively only.
- `PresetManagerPanel::Entry` drops `isFactory` and `isReadOnly`. All
  Rename / Duplicate / Delete / tag-edit code paths are unconditional. The
  `ChipKind::ActiveLocked` chip variant is removed; only `ActiveRemovable`
  remains. The Save flow no longer redirects writes to the user bank when
  the source preset lives under the bundled bank.
- `MainPanel::renameCurrentPreset` / `deleteCurrentPreset` no longer guard on
  "is this a factory preset" — the previous "Cannot rename a factory preset"
  status message is gone.
- `PresetUpdater` no longer prunes orphans. The old behavior deleted any
  local `.t5p` under the UCDCAE bank that was missing from upstream
  `manifest.json`. With the bank now writable that loop would silently
  destroy user-saved files; it's replaced with an explanatory comment. The
  `Stats::removed` field is dropped. The trade-off is explicit: users who
  delete a bundled preset locally will get it back on the next *Update
  Library*; nothing the user added ever gets removed.
- Conflict-row "Replace" wiring now also fires for UCDCAE entries because
  `existingPathKeys` covers the bundled bank via recursive `findChildFiles`.
  This is intended new behavior: overwrite a bundled preset just like any
  other.

### Installers (`d4f9a5dd`)

The system-wide preset tree (`/Library/Application Support/T5ynth/presets/`,
`C:\ProgramData\T5ynth\presets\`, `/usr/share/T5ynth/presets/`) is no longer
shipped by any installer. All three platforms rely on the existing
`juce_add_binary_data` glob + `MainPanel::ensureBundledPresetsExist` seeding
into the user directory on first launch — that path has been live since the
"Factory / User Preset Split" entry below, so removing the system-wide tree
is a clean simplification. macOS `distribution.xml` choice renamed
"Factory Presets & Support Data" → "Support Data". Windows `.iss` drops the
`[Dirs]` section and the `PresetsDir` define. Linux `.deb` / `.rpm` drop the
`/usr/share/T5ynth/presets` install lines. The `--presets` argument is gone
from `build_pkg.sh` and from `.github/workflows/build.yml`.

### Docs

This entry, plus `docs/PRESET_FORMAT.md` §9.3 (now "Bundled presets"),
`docs/LINUX_PACKAGING.md` (install layout no longer lists
`/usr/share/T5ynth/presets`), `ARCHITECTURE.md` (directory comment),
`scripts/preset-repo-template/README.md` (no more "read-only inside the
plugin" caveat — also explicit that the updater never deletes user files).
The CMake variable `T5YNTH_FACTORY_PRESETS` is intentionally left as-is to
keep this commit focused on docs; the glob still does exactly what it did.

The historical entries below (especially "Factory / User Preset Split",
2026-04-11) describe the *prior* design; they remain accurate as a record
of how we got here.

## 2026-05-01 — BPM-sync + Free/Trig + Delay crossfade (v1.7.0-beta.1)

Shipped four-day continuation of session 17: the BPM-sync UI from the
previous commit (`cd9b1d52`) is now wired through the DSP, the dead
LFO Trigger mode finally retriggers, the delay's dry/wet curve is a
true crossfade, and two preset-load stickiness bugs that surfaced
during testing are fixed.

### BPM-sync wiring (Steps 3-6 of the session-17 plan)

`T5ynthProcessor::resolveSyncBpm()` is the single source of truth for
synced rates, with priority **live host > running in-app sequencer >
frozen `hostBpmLastSeen` > `seqBpm` fallback**. The host transport is
snapshotted at the top of `processBlock` into two relaxed-ordering
atomics (`hostBpmLastSeen`, `hostPlayingNow`); both are written and
read exclusively from the audio thread, so no synchronisation cost.

Effective values are derived by free helpers in `BlockParams.h`:
`ClockSync::computeRate(bpm, divIdx)` for LFO/Drift Hz and
`ClockSync::computeDelayMs(bpm, divIdx)` for delay milliseconds.
Division → factor mapping lives in `ClockDivision::kFactor[]` — the
canonical table; never duplicated.

When a `*_clock_mode` is `Sync`, the synced value is written back into
`BlockParams` (LFO) or fed directly to the DSP setter (Drift, Delay),
so downstream env→LFO-rate modulation scales relative to the synced
rate naturally instead of getting clobbered.

The slow end of the division table (`16/1`, `8/1`, `4/1`, `2/1`)
extends the slider to drift cycles spanning many bars — suited to
slow harmonic motion and long auto-regen swings. All references go
through the named enum (`ClockDivision::D1_4`) or `kCount`, so GUI
and computeRate auto-track the +4 index shift.

### LFO Free/Trigger regression

The per-voice `perVoiceLfo1/2/3` instances on `SynthVoice` were prepared
and reset on note-on by `VoiceManager`, but their output was never read.
All voices read the global free-running LFO buffer regardless of mode,
so Trigger had no audible effect.

Fix: `SynthVoice::renderBlock` takes a small per-voice LFO buffer
(allocated once in `prepare`); when `bp.lfoNTrigMode` is true it syncs
rate/waveform from the global LFO, fills the per-voice buffer, and
rebinds the function-local `lfoNBuf` pointer so all downstream readers
(filter, scan, pitch) see the retriggered signal. Free mode is
unchanged. Global FX targets (Dly Time/FB/Mix, Rev Mix) still use the
shared global LFO since the FX bus is a single signal across voices.

### Delay dry/wet crossfade

`DelayLine::processBlock` previously used `dryGain = 1 - mix * 0.3f` —
a hybrid that attenuated the dry path by 30 % at full wet, mislabelled
in comments as "parallel send-bus". At high mix the delay felt like it
was eating the signal; in external loopbacks each pass through the
dry-attenuation compounded into a downward spiral. Now a true
crossfade — `out = dry × (1 − mix) + wet × mix`, identical to the
reverb's curve — so at `mix = 1` only the delay tail (with its own
feedback) remains. Internal feedback path is unchanged; this only
touches the post-mix output line.

### Preset-load stickiness fixes

Two bugs surfaced during user testing:

1. **Clock state stuck across loads.** APVTS `replaceState` leaves
   missing-from-tree params untouched. Old sessions / presets without
   the new clock fields would inherit whatever clock state was last
   touched in the host process — Drift 1's clock would mysteriously
   stay on Sync after loading a vanilla preset. Fix in
   `setStateInformation`: parse the loaded XML into a `ValueTree`,
   walk children to find which clock PIDs are present, append PARAM
   nodes with default values for the missing ones, then `replaceState`
   atomically. No `setValueNotifyingHost` glitch between a pre-reset
   and the actual restore. `importJsonPreset` gets the same
   hasProperty fallback for each LFO / drift / delay clock pair, and
   `exportJsonPreset` writes the new fields so v1.7+ presets round-trip
   cleanly.

2. **Injection mode stuck across `.t5p` loads.** Pre-injection presets
   (saved before v1.6) didn't carry `injectionMode`; the loader left
   the panel on whatever mode (e.g. Kombi 3) was active before, and
   the resulting audio no longer matched the preset's source recording.
   Old presets now default to `linear / 0.75 / 4 / 16` directly in
   `PresetFormat::LoadResult`, so loading reproduces the original A↔B
   crossfade behaviour the preset was generated with. New presets
   continue to overwrite these with their saved values.

### Files

- `src/dsp/BlockParams.h` — `ClockSync::computeRate`/`computeDelayMs`
  helpers, prepended slow divisions, added `lfoNTrigMode` to
  `BlockParams`.
- `src/PluginProcessor.{h,cpp}` — `hostBpmLastSeen`/`hostPlayingNow`
  atomics, `seqRunningNow()`, `resolveSyncBpm()`, host-transport
  snapshot in `processBlock`, LFO/Drift/Delay sync overrides,
  ValueTree-injection in `setStateInformation`, JSON
  export/import with hasProperty fallback for the clock fields.
- `src/dsp/SynthVoice.{h,cpp}` — three per-voice LFO scratch buffers
  filled when `lfoNTrigMode` is on, function-parameter pointer rebind.
- `src/dsp/DelayLine.{h,cpp}` — true crossfade, comment cleanup.
- `src/presets/PresetFormat.h` — `LoadResult` defaults to linear /
  0.75 / 4 / 16 instead of empty/NaN.
- `Resources/T5ynth_Guide.html`, `ARCHITECTURE.md`, `CHANGELOG.md` —
  user-facing docs for the BPM-sync feature.

### Verification

Sticky-fix path was caught by a verification agent (Bug #2 in the
review): the original `setValueNotifyingHost`-based pre-reset would
have caused a brief audio-thread render of the wrong clock state
between the reset and the `replaceState`. Switched to direct
ValueTree manipulation before the swap — atomic from the audio
thread's perspective.

CI: `v1.7.0-beta.1` tagged after testing; the tag-triggered run
publishes the macOS `.pkg` and Windows installer assets to a GitHub
prerelease.

---

## 2026-04-30 — Prompt-injection modes (v1.6.0-beta.1)

Shipped six prompt-injection modes — *Linear*, *Fine*, *Layer*,
*Kombi 1*, *Kombi 2*, *Kombi 3* — accessible via a six-button row
above the Alpha slider. Linear is the historical crossfade (bit-
identical to v1.5.x); the five new modes operate on diffusion sampler
steps and on individual DiT block cross-attention layers.

### Mechanics

The non-linear modes exploit two manipulation points in the loaded
`stable-audio-open-small` pipeline, both implemented as Python-level
monkey-patches that are applied per-request and torn down at the end
of generation:

1. **A `DiTWrapper.forward` patch** that increments a closure-captured
   `state["calls"]` counter once per sampler step. Late-step modes
   compare against `transition_step = max(1, round(steps · injection_transition_at))`
   to decide whether the early or late conditioning is active.
2. **`forward_pre_hook`s on every `block.cross_attn`** that override
   the runtime cross-attention `context` kwarg with a precomputed,
   per-block tensor. The replacement is pre-projected through
   `dit.to_cond_embed` once at request start, so the hook bypasses
   projection and just swaps the post-projection context.

Mode dispatch:

- **Linear / Delta** — no hooks, no patches. Manipulate the wrapper-
  level conditioning once.
- **Fine (`late_step`)** — wrapper patch; on late steps replaces
  `kwargs["cross_attn_cond"]` with a Fine-controlled blend of A and
  B. No per-block hooks.
- **Layer (`layer_split`)** — per-block hooks; sigmoid top-hat over
  `[split_start, split_end]` with smoothness=2 (4-layer ramps at
  edges). No wrapper patch.
- **Kombi 1/2/3** — wrapper patch *and* per-block hooks together.
  The hook reads the step counter and switches between an "early"
  per-block tensor (uniform `proj_a`) and a "late" per-block tensor
  (hard-mask blend of `proj_a` and the projected `late_blend`).

### Empirical findings

- **Hard mask vs sigmoid top-hat.** `layer_split`'s sigmoid edges
  cause a 4-block-wide band to peak at w≈0.534 in the centre and
  drop to w≈0.441 at the edges. Even at slider=1 (`late_blend = pure
  B`), per-block B contribution is bounded around 50 %. For Kombi
  modes, where the band is fixed by the preset and the slider only
  controls intensity, replaced with a hard mask (w=1 inside, 0
  outside) so slider=1 is genuinely 100 % `late_blend` in the
  band's blocks.
- **High DiT blocks (12..15) have low conditioning leverage.** The
  original plan put Kombi 2 at blocks 12..16 to mirror Kombi 1's
  surface band (0..4) at the top of the stack. In listening, that
  band was nearly inaudible — the top 4 SA Open Small blocks are
  dominated by refinement work, not prompt-following. Moved Kombi 2
  to **[4, 12] (broad mid)**, where the diffusion does most of its
  conditioning work; the change turned Kombi 2 from "sounds like A"
  into the most distinct of the three Kombis.
- **Kombi 1 (surface, blocks 0..3) is structurally subtle by
  design.** Four narrow surface blocks, all twelve other blocks
  seeing pure A through every step, produces an A-dominant result.
  This is the "B as surface skin" intent. If too subtle in practice,
  candidate widenings are [0, 6] or [2, 6].
- **Kombi 3 = narrow center [6, 10]** was added as an empirical
  probe: same band width as Kombi 1, different vertical position,
  inside the high-leverage mid region. Hypothesis is that K3 may
  turn out the most aggressive of the three despite the narrowest
  band.

### UX changes

- **Per-mode slider memory.** Fine and the three Kombi modes each
  remember their own intensity-slider value, so A/B-ing modes by
  clicking buttons does not destroy the last-used position of any
  individual mode. Linear (APVTS) and Layer (own state fields)
  already had independent state.
- **Mode buttons trigger regeneration** to match the existing
  slider/drift auto-regen UX.
- **Slider scale**: the Fine/Kombi intensity slider now displays
  0–1 (was 0.5–1.0). The internal mapping onto the audible region
  of `injection_transition_at` / `late_phase_alpha` is unchanged —
  it just no longer shifts the displayed numbers. Old presets
  reload their stored value into the corresponding mode slot, but
  the rendered output may differ since the slider value is no
  longer remapped before being sent to the backend.

### Files

- Backend: `backend/pipe_inference.py` (mode validation, kombi
  payload, kombi install/teardown block).
- IPC: `src/inference/PipeInference.h` (`Request` fields), serialised
  via existing path in `src/inference/PipeInference.cpp:774-778`.
- UI: `src/gui/PromptPanel.h/.cpp` — six radio buttons,
  `applyModeToSlider`, mode-aware ghost / drift mapping,
  per-mode lateMix slots via `lateMixForMode` helper.
- Plan & background: `docs/INJECTION_RECTANGLE_PLAN.md`,
  `ARCHITECTURE.md` §6.5, in-app manual §1 + §17.

## 2026-04-25 — Aftertouch Deferred

Aftertouch is explicitly **not a near-term priority**. The feature remains a
possible later addition, but only after higher-value work is done.

### Why it is deferred

- There is currently **no AT path** in the synth. `PluginProcessor` only handles
  `NoteOn` / `NoteOff` in the sample-accurate MIDI split loop.
- The current render path splits the audio block at MIDI event boundaries. A
  dense AT stream would therefore increase sub-block fragmentation and
  `renderBlock()` churn.
- `VoiceManager` and `SynthVoice` currently have no dedicated per-voice pressure
  state, so **Poly AT** would not be a small UI addition; it would require new
  voice-state plumbing and careful real-time handling.

### If revisited later

Keep v1 deliberately small:
- **Channel AT only**
- one target plus amount
- targets limited to obvious real-time destinations such as `DCA`, `Filter`, or
  `Pitch`

Do not start with Poly AT, multi-target routing, or a mini mod-matrix.

## 2026-04-24 — Sampler Normalize Rework (signal-aware, linear)

Replaced the sampler's `RMS -> soft-knee tanh` normalize path with a **signal-aware, fully linear normalization stage** in `SamplePlayer`. The old approach could sound fine on some material but broke down on low-RMS choir-style samples because it tried to force a fixed RMS target and then hid the overshoot inside a nonlinearity. The new path explicitly avoids that failure mode.

### New normalize model

`SamplePlayer::normalizeBuffer()` now runs a lightweight analysis over the actual audible play region and chooses one of four modes:
- **Bypass** — near-silence / floor-guard
- **PeakCap** — already-hot material; only trim to ceiling if needed
- **Transient** — short, sparse, or high-crest material; normalize against `p99.9`
- **Sustained** — dense/tonal material; normalize against active RMS

The gain is then applied as **one linear stereo-linked multiplier**, region-limited to the measured playback span. No soft-knee, no waveshaping, no hard clip, and no scaling of unrelated buffer sections.

### Heuristics

Mode selection uses:
- duration
- 50 ms active-block ratio
- crest factor
- peak-to-`p99.9` gap
- peak headroom / near-silence guards

Targets:
- ceiling: `-1 dBFS`
- sustained target: `-18 dBFS` active RMS
- transient target: `-10 dBFS` at `p99.9`

This is intentionally conservative: difficult samples now tend to get either a sane linear lift or a cap/no-op, rather than being "normalized" into audible deformation.

### Validation

Added `tools/batch_normalize_samples.py` to mirror the C++ heuristic offline, batch-render normalized WAVs, and write CSV/Markdown reports for listening checks.

Tested against a deliberately broad corpus:
- choir pads / soft strings / fortissimo strings
- speech / interviews / agitated crowd speech
- clicks / static / crashes / piano / samba / ambience
- near-silence
- synthetic fixtures for loop-pad, bright rhythm, sub-heavy bass, and wide stereo texture

Observed behavior matched intent:
- quiet sustained material gets lifted cleanly
- sparse/transient material avoids LUFS-style over-push
- already-hot/noisy files get capped or left alone
- silence stays silence

### Build / workflow

- `cmake --build build_clean --config Release --target T5ynth_Standalone -j4`
- offline listening batches written to `/tmp/t5ynth_normalize_batch*`

## 2026-04-24 — Polyphonic Generative Sequencer (feature/polyphonic-gen-seq)

Turned the previously mono `T5ynthGenerativeSequencer` into a four-strand polyrhythmic engine. Five atomic commits on a feature branch off main; every phase builds clean as an isolated step.

### Aesthetic framing
User explicitly rejected functional harmony defaults ("keine 1625-Klischees"). No chord progressions, no I-IV-V Markov — the polyphony is coupled through a **shared Pitch-Field** (pc-set + center + optional row) that evolves under one of four modes:
- **Static** — fixed pc-set
- **Drift** (default) — one pc swaps per tick
- **Transform** — twelve-tone row ops (Tn / In / R / RI)
- **Pivot** — pc-set transposition by pivotInterval (m3 default)

### Architecture (three layers)

1. **Strand struct** holds per-pattern state (Euclidean params, playback clocks, drift counters, Turing degree-walk, fix flags). `strands[MAX_STRANDS=4]` at class level.
2. **PitchField struct** holds shared pc-set + row + evolution-mode state. Advances on strand 0's cycle boundary.
3. **pickNote()** projects a strand's raw Turing-walked scale degree into MIDI via role + metric weighting:
   - **Density role** → `chromaticFieldWalk` (±1 semitone from last note, ignores metric weighting)
   - **Others** → strong beats snap to centerPc with probability = `chordToneDominance`; otherwise `voiceLedFieldMember` picks the field's nearest pc to the raw. Weak beats that fall inside the field keep their raw pc (preserves Turing dynamics).

### processBlock scheduler
Replaced the single-strand `while (samplePos < numSamples)` loop with a multi-strand event scheduler: each iteration finds the earliest upcoming step-boundary-or-gate-off across all enabled strands and processes one event. Each strand runs its own clock at `stepDur / divisionMultiplier`, producing real polymeter (e.g. Anchor ½× alongside Density 2×). Gate-off and note-emission track per-strand `lastPlayedNote` separately; VoiceManager handles concurrent voices naturally.

### APVTS surface
~50 new params across `gen_field_*` (mode, rate, centerPc, pivot-interval), strand-0's new role/octave/div-mult/dominance (`gen_role` etc.), and strands 2–4's full 13-param set (`gen2_*`, `gen3_*`, `gen4_*`). Existing `gen_steps`/`gen_pulses`/… IDs retained — strand 0 still uses them, so old presets load with strand 0's drift/writeback behavior bit-identical for the first ~8 cycles; then the shared pc-set begins to drift (intended).

### UI
Minimal compact addition to `SequencerPanel` visible in GEN mode:
- Row 4: Field-Mode dropdown + Field-Rate slider
- Row 5: three `[ON][Role▾]` clusters for strands 2/3/4

All other per-strand parameters (steps, pulses, rotation, mutation, octave, div-mult, dominance, fix-flags) and the remaining field params (centerPc, pivot-interval) stay at the APVTS level so the panel stays tight. Full per-strand sliders + 4-lane viz were scoped out as a v2 follow-up.

### Commits on `feature/polyphonic-gen-seq`
1. `refactor(sequencer): extract per-strand state into Strand struct` — mechanical
2. `feat(sequencer): pitch field + static/drift/transform/pivot modes`
3. `feat(sequencer): metric weighting + strand roles + sheets-of-sound`
4. `feat(processor): wire polyphonic generative sequencer params`
5. `ui(sequencer): polyphonic strand controls + field block`

### Audio-thread safety
All new code uses stack-local `std::uniform_*_distribution`s, fixed-size `std::array`, atomic stores only with `memory_order_relaxed`. No allocations, no locks, no I/O in `processBlock`. Grep-verified against `new`/`malloc`/`std::cout`/`printf`/`std::mutex`/`std::lock` in the diff.

### Open follow-ups (plan-documented)
- Full 12-slot row editor for Transform mode (v1 only has the auto-seeded ascending row)
- Chromatic-density saturation parameter per Density strand (chromatic bridging density)
- Live MIDI-input → Field-adaptation (interactive listening mode)
- Per-strand pitch-field override (one strand contrasts against the shared field)
- **Inline slider values**: evaluate an `SliderRow` variant (or new component) that renders the numeric value *inside* the bar instead of as a tiny label at the right edge. Tested briefly here with `juce::Slider::LinearBar + TextBoxLeft` on the Field Rate slider — ergonomically much better than the current right-aligned readout, especially at narrow widths, but inconsistent with the rest of the panel. Applies to every slider in the app, not just this one row. Worth a dedicated pass after the v1 feature set stabilises.

## 2026-04-23 — Session 16: Ladder Drive × Resonance ROAR

Resolves the "drive kills resonance" open item from the earlier entry today. The previous tree had a Version C hot-ceiling hack (`hot = kHotCeil · tanh(raw/kHotCeil)`) that was strictly worse than Version B — it killed both the drive harmonics *and* the resonance peak. Reverted it; the fix lives at the per-stage saturation instead.

**Algorithm A — Huovilainen / Surge thermal-voltage normalised stages.** Replace plain `tanh(y)` at each ladder stage with `satStage(x) = 2·Vt · tanh(x / (2·Vt))`. Slope at zero stays 1 (self-oscillation threshold unchanged at `k = 4`), but the per-stage ceiling widens to ±2·Vt. Because `y4` is bounded by that ceiling, so is the feedback tap `k · y4`. With plain tanh (≡ Vt = 0.5) the feedback amplitude was capped at ±4.2 — not enough to swing a first stage pinned at saturation by a 36 dB hot signal through its linear region, so the resonance ring collapsed. Raising to `kVt = 1.22` (canonical Surge value for VintageLadders) gives ±10.2 of feedback swing — the exact headroom that lets the loop oscillate the drive-pinned first stage across zero each cycle. That's the mechanism behind the Minimoog-style ROAR at high drive + high resonance.

Applied to all five per-sample `tanh` calls in `MoogLadderFilter::processSample` (the pre-ladder `tanh(fbIn)` and the four per-stage `tanh(y_i)`). Mirrored into `CutoffWarpFilter`'s Tanh style (`sat(·, 0) → satStage(·)`); other styles (SoftClip / OJD / Sin / Digital / Asym) kept their original curves on this first pass — revisit per-style if the acceptance matrix falls flat for one of them.

**Why not Algorithm B (drive-inside-the-loop) or cytomic/RK4.** Algorithm A is the canonical Huovilainen form used in production plugins (Surge XT, et al.), ~6 lines of change, no coefficient re-derivation, no stability analysis required. B would change the drive topology away from a real Moog (drive is externally applied there, not intra-loop); C and D (Cytomic SVF-physical, RK4+TV) are 4–10× the implementation cost and not obviously needed if A works.

**Level impact.** At low drive the signal doesn't hit `satStage`'s ceiling, so Vt = 1.22 output is indistinguishable from Vt = 0.5 — no regression on the `d0f78364` level parity with the SVF. At 24–36 dB the tap *is* louder by up to ~2.4×, which is the desired "drive makes it louder + crunchier" behaviour, not a bug. The `kTapComp = 1.20` is unchanged; re-tune only if the listen test reveals an audible mid-drive jump.

**Bundled along with the fix** (each its own commit, independently revertable):
- `1178002b` — startup visuals of the filter card's radio rows (TYPE / SLOPE / ALG / OS) now sync directly from APVTS instead of relying on the ComboBoxAttachment's initial `onChange` firing, which some JUCE versions skip with `dontSendNotification`. Fixes the "no active button until you click" first-paint glitch.
- `a021f083` — per-style resonance scaling for `CutoffWarp` (0.65 Sin, 1.35 SoftClip, 1.10 OJD, 1.00 Tanh/Digital/Asym). Each saturation curve has a different DC slope, so a single nominal `k` made Sin ring at r ≈ 0.25 while SoftClip stayed silent at r = 1. Tuned by ear.
- `acba990c` — full problem-statement / acceptance-test handover doc (`docs/handover_session16_filter_drive.md`) so the reasoning is recoverable.
- `5d833f55` — Algorithm A itself.

`kVt` is duplicated verbatim in `MoogLadderFilter.h` and `CutoffWarpFilter.h` (both `1.22f`) with cross-reference notes in both. Deliberately kept each filter self-contained rather than introducing a shared constants header for a single float — if a later change needs more shared state, that's the point to extract.

**Open.** Listen test in progress against the acceptance matrix in handover §8. Expected tuning range for `kVt` if adjustment is needed: 1.0 – 1.5. The preset save/load audit that was listed as open in the earlier entry today was already completed in session 15 (handover §9).

## 2026-04-23 — Nonlinear Filter Algorithms (Huovilainen + Cutoff Warp)

Added two nonlinear filter algorithms alongside the existing linear TPT SVF, selectable via a new Algorithm switchbox in the filter header:

- **Huovilainen-style Moog ladder** (`src/dsp/MoogLadderFilter.h`). 4-pole with `tanh` at each stage input and a half-sample-delay-compensated feedback path (`xPrev` + 50/50 average). The compensation is what makes the classical form actually resonate — without it the loop sees an extra sample of delay and clips instead of ringing. Slope switch taps y1..y4 (6/12/18/24 dB) for the standard Moog multi-mode.
- **Surge-XT-inspired Cutoff Warp** (`src/dsp/CutoffWarpFilter.h`). ZDF 4-pole ladder with a style-switchable per-stage saturation: Tanh / SoftClip / OJD (x/√(1+x²), centered at zero — the initial atan-with-bias version had DC drift that glitched the ZDF loop under modulation) / Sin-fold / Digital clip / Asym (bias-compensated asymmetric tanh).

Credit: Huovilainen 2004 DAFx paper for the Moog algorithm; Surge XT project (GPLv3) for the Cutoff Warp concept. Implementations written from scratch; no reference code copied. Entries added to `THIRD_PARTY_LICENSES.txt`, `resources/T5ynth_Guide.html`, and `ARCHITECTURE.md`.

**Drive topology difference.** The SVF uses the existing pre-filter `tanh` (+ optional oversampling) as its saturation character — it's LTI on its own, the drive stage *is* the character. Ladder and Warp have their own nonlinear stages, so the pre-filter `tanh` would flat-clip and leave nothing for the filter to shape. Instead the drive amount is forwarded via `setInputDrive()` as input gain only; Phase B is a no-op for them and the filter's own `tanh`/`sat(·)` stages do the work. No output compensation (`1/inDrive`) — drive makes these filters louder + crunchier like an analog Moog, not level-matched.

**Level parity.** The half-sample input averaging is a `cos(ω/2)` FIR lowpass (−3 dB at sr/4) — required for correct resonance but it pulls Ladder/Warp ~1.5 dB under the SVF on broadband material. Compensated with a fixed 1.20× tap gain.

**Feedback-tap evolution.** First ship had `k·tanh(y4)` (Ladder) and `k·sat(y4, style)` (Warp). Both saturations cap the feedback at ±1, so the loop gain ceiling of 4.2 never translated to an actual resonance peak — the reso slider "controlled something" but didn't ring. Fixed by making the feedback tap linear on `y4` and moving the per-stage saturation to each stage's input (not the feedback tap). Stability still comes from the per-stage sat bounding the internal state.

**Unresolved** (see `docs/handover_session15.md`): Ladder/Warp resonance is still too drive-sensitive — at ~50 % drive on Ladder (and >21 dB on Warp Sin) the resonance collapses entirely because the operating point sits deep in the saturation zone where the local derivative is near zero, killing effective loop gain. Also: the filter-type / algorithm radio buttons occasionally don't reflect their APVTS value on first paint (explicit `onChange()` poke after attachment did not fully fix it); and the preset save/load coverage of all filter-related params still needs an audit.

UI: Algorithm switchbox + Warp Style combo live in the filter header row next to TYPE/SLOPE (Drive slider shortened accordingly). Drive-OS default is 4×; old .t5p files without the new fields load on Algorithm=SVF, Warp Style=Tanh, Drive OS=Off so existing sessions stay bit-identical to pre-Ladder/Warp builds.

## 2026-04-18 — Sampler Wave Cursor Reverted

Tested a live playback cursor in the sampler waveform view and reverted it. On loop-heavy material the 30 Hz smoothed marker felt visually late relative to the audio and was more distracting than helpful. If this is revisited, it should probably use a different visualization approach rather than a lagged dot/cursor over the waveform.

## 2026-04-11 — Installers + Path Architecture

### macOS .pkg Installer
Built with `pkgbuild`/`productbuild`. Four component packages: Standalone (required), VST3, AU, Support Data. The installer creates `/Library/Application Support/T5ynth/presets/` (factory presets, read-only) and `/Library/Application Support/T5ynth/models/` (scan-only). Postinstall removes quarantine flags (`xattr -cr`).

### Windows Inno Setup Installer
`installer/windows/t5ynth.iss`. Standalone + VST3 components, Start Menu entry, uninstaller. Factory presets in `C:\ProgramData\T5ynth\presets\`, models dir in `C:\ProgramData\T5ynth\models\` with `users-modify` ACL.

### Factory / User Preset Split
- Factory presets: `/Library/Application Support/T5ynth/presets/` (macOS), `C:\ProgramData\T5ynth\presets\` (Win) — installed by the installer, read-only for users.
- User presets: `~/Library/T5ynth/presets/` (macOS), `%APPDATA%\T5ynth\presets\` (Win) — writable, created on demand. *(Corrected 2026-06-12: this entry originally claimed `~/Library/Application Support/…` — JUCE's `userApplicationDataDirectory` actually resolves to `~/Library` on macOS.)*
- `PresetFormat::getFactoryPresetsDirectory()` / `getUserPresetsDirectory()` / `getAllPresetFiles()` added.

### Design Decision: Models Are Per-User
Model download target is always the per-user directory (`~/Library/Application Support/T5ynth/models/` on macOS, `%APPDATA%\T5ynth\models\` on Windows). Rationale: model licenses (Stability AI Community License, CC-BY-NC-SA 4.0) are accepted individually per user. Different users on the same machine may have different license status (e.g. commercial vs. non-commercial accounts). The system-wide path (`/Library/Application Support/` / `C:\ProgramData\`) is scanned as a read-only candidate — an admin can pre-deploy models there, but the app never writes to it.

### CI Integration
Both installers are built in CI (`build.yml`), uploaded as artifacts on every push, and included in GitHub Releases on tags. macOS: `build_pkg.sh` in the `macos` job. Windows: `choco install innosetup` + `iscc` in the `windows` job.

## 2026-03-31 — Session 8: Signal Chain Fixes + RT Safety

### Critical Bug Fixes

- **Filter never called (CRITICAL):** `SynthVoice::processFilter()` existed with full modulation logic but was never invoked anywhere. Per-voice buffers don't exist (voices are summed sample-by-sample), so a per-voice filter was architecturally impossible without major refactor. **Fix:** Added post-sum `T5ynthFilter postFilter` to `PluginProcessor`, wired between voice rendering and effects chain. Full modulation ported from the dead per-voice code: keyboard tracking (uses newest voice's note), mod envelope → cutoff (subtractive sweep with `1 ± amount × 8` peak/floor), LFO → cutoff (bipolar multiplicative). `hasActiveVoices` guard prevents stale mod values from shifting cutoff when silent.

- **Pitch accumulation (CRITICAL):** `renderSample()` did `osc.setFrequency(osc.getFrequency() * pitchFactor)` — multiplied the *current* frequency by the pitch factor each sample, causing exponential drift to 20kHz in ~100 samples. **Fix:** Added `float baseFrequency` to `SynthVoice`, cached at `noteOn()` and `glideToNote()`. Pitch modulation now uses `osc.setFrequency(baseFrequency * (1 + pitchMod))` — always absolute, never accumulating. Frequency resets cleanly when modulation stops.

### Real-Time Safety

- **Debug FILE* logging removed:** `static FILE* dbgFile` with `fopen`/`fprintf`/`fflush` on audio thread — RT violation + file handle leak. Deleted.
- **LFO buffer heap allocation removed:** `std::vector<float>` was allocated per `processBlock` call. Moved to pre-allocated member vectors resized in `prepareToPlay()`.

### Cleanup

- Renamed `Looper` → `Sampler` throughout (EngineMode enum, GUI buttons, accessor methods, preset import/export). Aligns with actual functionality.
- Added porting comparison CSVs (`docs/portierung_*.csv`) for all composables.

### Correct Signal Flow (after fix)
```
Voices (Osc/Sampler → VCA → sum with 1/sqrt(N))
  → Post-Sum Filter (kbd tracking + env→cutoff + LFO→cutoff)
  → Effects (Delay ‖ Reverb parallel send-bus)
  → Master Volume → Limiter
```

### Known Remaining RT Violations (deferred)
- `T5ynthFilter::processBlock()` allocates `dryBuffer` when mix ∈ (0,1)
- `reverbSrc` buffer allocated when both delay+reverb are enabled simultaneously
- Per-voice `processFilter()` and per-voice `T5ynthFilter filter` are now dead code

---

## 2026-03-29 — Session 6: DSP Bugfixes + LibTorch Migration Start

### DSP Bugfixes (3 remaining from Session 5 audit)
- **modPitch dead code:** Pitch modulation was accumulated in processBlock but never applied. Moved to per-sample in `SynthVoice::renderSample()` where env/LFO values are available. Applies `freq *= (1 + pitchMod)` to wavetable oscillator.
- **barStartFlag unconsumed:** StepSequencer sets flag at bar boundaries but processBlock never read it. Now consumed via `exchange(false)`.
- **DCF idle cutoff drop:** Verified already fixed — `!isIdle()` guard correctly prevents filter modulation when envelope is idle.

### Architecture Decision: LibTorch Migration
- HTTP-based Python backend (Flask + diffusers) is architecturally unacceptable for a standalone audio plugin
- Decision: migrate to LibTorch C++ inference — no Python, no HTTP, no server process
- VRAM properly freed on plugin destructor

### TorchScript Export Progress
- Created `tools/export_to_torchscript.py` — exports Stable Audio components individually
- **T5 Encoder:** Successfully exported (0.00 max diff, 438.9 MB)
- **Projection Model:** Successfully exported (0.00 max diff, 1.6 MB)
- **DiT (Diffusion Transformer):** BLOCKED — diffusers attention processor uses `repeat_interleave` with `output_size` that causes CPU/CUDA device mismatch during tracing
- **VAE Decoder:** Not yet reached (blocked by DiT)

### DiT Export — Next Steps
The fix requires replacing `StableAudioAttnProcessor2_0` with a custom processor that avoids the `output_size` argument in `repeat_interleave`, or patching diffusers locally. Input shapes confirmed: `hidden_states=[1,64,1024]`, `global_hidden_states=[1,1536]`, `encoder_attention_mask` must be bool not long.

---

## 2026-03-28 — Session 4: Reference Audit + Critical Repairs

### Full Gap Analysis Against Reference (useAudioLooper.ts, useModulation.ts, useEffects.ts, useFilter.ts, useDriftLfo.ts, crossmodal_lab.vue)
- Session 3 left 75% of features unimplemented or broken
- Delay/Reverb/Limiter parameters were defined in APVTS + GUI but **never passed to DSP classes**
- Reverb IR was never loaded — ConvolutionReverb was permanently dead
- Filter modulation double-ticked envelopes/LFOs (ran at 2× speed)
- AudioLooper could only do endless forward loop — missing one-shot, ping-pong, crossfade, normalize, loop brackets, retrigger
- ADSREnvelope used exponential decay (wrong) instead of reference's linear decay, and simple exponential release instead of RC-discharge

### Parameter Wiring (Phase 1)
- `delay.setTime/setFeedback/setMix()` now called in processBlock
- Reverb IR loaded from BinaryData in `prepareToPlay()`, mix wired, IR switching on `reverb_ir` parameter change
- `limiter.setThreshold/setRelease()` wired
- Looper: `loop_mode`, `crossfade_ms`, `normalize` wired

### Correctness Fixes (Phase 2)
- **Double-tick bug**: Filter modulation now uses captured last values from per-sample loop instead of re-calling `processSample()`
- **DCF sweep factor**: 4 → 8 to match reference (`base × (1 + amount × 8)`)
- **Alpha conversion**: UI range (-2..+2) → backend (0..1) via `alpha/2 + 0.5`
- **Fake S&H removed**: LFO waveform list reduced from 5 to 4 (S&H was not in reference, returned silence)
- **Drift LFO 3**: rate/depth/target now wired, `drift3_target` and `drift3_wave` parameters added

### AudioLooper Rewrite (from useAudioLooper.ts)
- LoopMode enum: OneShot, Loop, PingPong
- Loop start/end brackets (fractional 0–1 of buffer)
- Equal-power crossfade baked into buffer (`sin/cos` curves, max half loop length)
- Cross-correlation loop-point optimization (512-sample window, 2000-sample search)
- Palindrome buffer for ping-pong (endpoints not doubled)
- Peak normalization to 0.95
- Retrigger (hard restart from cold-start offset)
- processBlock respects play region, stops on one-shot end

### ADSREnvelope Rewrite (from useModulation.ts)
- Linear attack with soft retrigger (ramps from current level, not zero)
- Linear decay (was exponential RC — wrong curve shape)
- RC-discharge release: `e^(-t/τ)`, `τ = releaseMs/5`, hard-zero at end
- 3ms minimum ramp for all stages
- Loop re-triggers attack from sustain

### Remaining Work (identified in gap analysis, not yet done)
- Filter slope 12/24dB (parameter exists, not implemented)
- Parallel effects chain (currently serial: signal → delay → reverb)
- 13 modulation targets (currently only 4 per source)
- Delay damping filter in feedback loop
- Drift LFO waveform selection (currently sine-only)
- Delay BPM sync
- Sequencer: gate length, glide, note divisions, presets
- Arpeggiator: musical rate divisions instead of float
- UI: on-screen keyboard, preset panel, dimension explorer canvas, loop region display, recording

---

## 2026-03-28 — Session 3: Audio Pipeline Debugging + Sequencer Fix

### Critical Bug Fixed: No Sound After Generation
- **Root cause**: Looper audio was multiplied by amp envelope, which defaults to 0 without MIDI note-on. Generated audio x 0 = silence.
- **Fix**: Auto-trigger envelopes when `loadGeneratedAudio()` is called.
- **Secondary issue**: SVT filter's `setType()` was called every processBlock, found to not reset state (JUCE source confirms it only sets a variable). Actual filter silence was caused by DryWetMixer behavior — bypassed for fallback sine.
- **Base64 decode**: Original code used `MemoryBlock::fromBase64Encoding()` which is JUCE-proprietary format, NOT RFC 4648. Python's `base64.b64encode()` produces standard base64. Switched to `juce::Base64::convertFromBase64()` which is RFC 4648 compliant.
- **HTTP timeout**: Backend lazy-loads Stable Audio model on first request (10-15s). Client timeout was 5s. Increased to 120s.

### Sequencer MIDI Pipeline
- Implemented `StepSequencer::processBlock()` — sample-accurate 16th-note stepping with MIDI note generation.
- Implemented `Arpeggiator::processBlock()` — 5 modes (Up/Down/UpDown/Random/Order), multi-octave cycling.
- **Critical bug**: `start()` was called every `processBlock`, resetting `samplesUntilNextStep` to 0 each block — sequencer ran at block rate (~86 Hz) instead of BPM. Fixed: only reset on actual start transition.
- Seq and Arp now run **in series** (Seq generates notes, Arp arpeggates them + external MIDI), not mutually exclusive.

### Waveform Display
- `WaveformDisplay::setWaveform()` was never called. Wired via 30Hz timer in SynthPanel polling a snapshot buffer in PluginProcessor. Peak-preserving downsample to 1024 display points.

### GUI Fixes
- Fixed aspect ratio (3:2) — window scales proportionally.
- All ENV/LFO/Filter/Drift sections always visible (dimmed at 30% alpha when inactive) — no collapsing, predictable layout height.
- Font sizes derived from panel's own available height / total content units — everything fits at any size.
- Minimum window: 1050x700.
- Settings page as overlay (auto-scans model at known paths, shows backend status).
- Settings button injected into JUCE standalone header next to Options.
- Master volume as rotary knob.
- MIDI monitor in sequencer panel.
- Seed field as numeric text input with Random toggle.
- Fallback sine oscillator for MIDI testing without generated audio.

### Backend Connection
- `BackendManager` status ("Running") and `BackendConnection::isConnected()` were independent — synced by calling `checkHealth()` when manager reports Running.

---

## 2026-03-26/27 — Sessions 1-2: Foundation + GUI Design Pass

### Project Scaffold
- JUCE standalone + VST3 plugin with full DSP chain (wavetable oscillator, audio looper, 3 ADSR envelopes, 2 LFOs, drift LFOs, SVT filter, delay, convolution reverb, limiter).
- Python backend (Flask) wrapping Stable Audio Open 1.0 via diffusers `StableAudioPipeline`.
- `BackendManager` launches Python server as child process, polls `/health`.
- `BackendConnection` HTTP client on dedicated thread, delivers results via `MessageManager::callAsync`.

### GUI
- 2-column layout: Col1 (25%) Generation (prompts, embedding controls, axes) | Col2 (75%) Engine/Filter/Modulation/Drift.
- Footer: Sequencer + FX (Delay/Reverb) + Master Volume.
- Section colors: Filter=cyan, Modulation=orange, Drift=purple.
- Linear sliders with value display and units (no rotary knobs for parameters).
- Color-coded semantic axes (pink/blue/green) + PCA axes (6 colors).

### DSP
- Full modulation routing in processBlock: envelopes/LFOs → DCA/Filter/Scan targets.
- Filter dry/wet mix via DryWetMixer.
- Envelope looping (re-enters Attack from Sustain).
- Drift LFO re-generation (randomize rate/depth on phase wrap).

### Model Loading
- Local model path support: `~/t5ynth/models/stable-audio-open-1.0/` (symlink to HuggingFace cache or ComfyUI directory).
- Fallback: HuggingFace auto-download.
- Requires HuggingFace pipeline format (`model_index.json`) — single `.safetensors` not usable because embedding manipulation needs separate access to text encoder + projection model.

---

## Architecture Notes

### Signal Flow
```
MIDI In → Sequencer → Arpeggiator → Note Processing
                                         ↓
              Wavetable Oscillator / Audio Sampler
                    ↓ (per-voice VCA, sum with 1/sqrt(N))
              Post-Sum Filter (SVT, modulated cutoff)
                                         ↓
                    Delay ‖ Reverb (parallel send-bus) → Master Vol → Limiter → Out
```

### Audio Generation Flow
```
User clicks Generate → PromptPanel builds GenerationRequest
  → BackendConnection POST /api/cross_aesthetic/synth (JSON)
  → Python backend: T5 encode → embedding manipulation → StableAudioPipeline
  → Response: base64-encoded WAV
  → JUCE: Base64 decode → WAV parse → AudioBuffer
  → loadGeneratedAudio(): Looper + Wavetable extract + Waveform snapshot
  → Auto-trigger envelopes → Sound plays immediately
```
