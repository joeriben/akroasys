# MPE: moving the hand-written path onto JUCE's

This is the enumeration the project's Migration & Substrate Discipline requires
*before* the first line of the new implementation: every capability of the
existing hand-written MPE path, with the line it lives on, and for each one
either the proof that it survives on the library or the note that it needs
explicit preservation. Deleting the old code is the last step, not the first.

The old path was not a module with a boundary. It was a set of branches inside
`T5ynthProcessor::processBlock`'s MIDI walk plus the state they read, so the
list below is the boundary — there was nothing else to inherit.

**Outcome, stated up front.** `juce::MPEZoneLayout` took over the zone layout,
the MPE Configuration Message, the pitch-bend-range RPN, the per-channel RPN
detectors and the master/member classification — the part where the channel-16
bug lived. `juce::MPEInstrument`, the note tracker, did **not**, and the reason
is measured rather than argued: see §3. Per-note expression therefore stays
keyed by MIDI channel in `VoiceManager`, as before.

## 0. Where it lived

| Part | Location before |
| --- | --- |
| Zone-layout state and `isMpeMasterChannel` | `PluginProcessor.h:963-1013` |
| Pitch-bend ranges (master / per-note) | `PluginProcessor.h:949-961` |
| RPN selection state (one pair, global) | `PluginProcessor.h:952-953` |
| Poly aftertouch → per-note pressure | `PluginProcessor.cpp:4851` |
| Channel pressure → zone-wide or per-note | `PluginProcessor.cpp:4857` |
| Pitch wheel → global or per-note | `PluginProcessor.cpp:4869` |
| RPN 0 → bend ranges | `PluginProcessor.cpp:4936` |
| RPN 6 → zone layout (MCM) | `PluginProcessor.cpp:4968` |
| CC74 → per-note timbre | `PluginProcessor.cpp:5026` |
| RPN deselect on Reset All Controllers | `PluginProcessor.cpp:5194` |
| Channel tagging on note-on | `dsp/VoiceManager.cpp:147-149` |
| Channel-keyed routing of the three dimensions | `dsp/VoiceManager.cpp:517-556` |
| Tag cleared when a voice goes idle | `dsp/VoiceManager.cpp:657` |

## 1. Capabilities, one per row

**Survives** = the library does this, and does it the same way.
**Preserve** = the library would do something else, or does not reach this far;
the new code keeps the old behaviour deliberately, and the reason is given.

| # | Capability | Disposition |
| --- | --- | --- |
| 1 | Lower zone: master ch1, members 2..1+N; upper zone: master ch16, members 16-N..15 | Survives — `MPEZone::isUsingChannelAsMemberChannel`, the code the old comment already cited |
| 2 | Channel 16 is a master only where an upper zone was declared (the `03286a97` fix) | Survives, and structurally: one zone object, so the call sites cannot disagree |
| 3 | MCM (RPN 6) on ch1/ch16 sets a zone; 0 switches it off; ≥16 is discarded, not clamped | Survives — `MPEZoneLayout::processZoneLayoutRpnMessage`, `rpn.value < 16` |
| 4 | Declaring one zone shrinks the other where they would overlap | Survives — `MPEZoneLayout::setZone` |
| 5 | A layout survives `prepareToPlay`, preset load, panic and Reset All Controllers | Preserve — the layout is a member and nothing resets it; it stays out of the preset |
| 6 | RPN 0 on a member channel sets the per-note bend range | **Preserve** — the library *parses* it (`MidiRPNDetector`), the synth *stores* it. `MPEZoneLayout::processPitchbendRangeRpnMessage` would write it into the zone, where nothing here reads it, so the data byte that completes RPN 0 is not fed to the layout at all. That also keeps `updateMasterPitchbend`'s assertion (it range-checks the value already in the zone while assigning the new one unchecked, `juce_MPEZoneLayout.cpp:150-168`) off the audio thread. Not Release-observable — `jassert` compiles out — so no corpus case pins it; it is a debug-build hazard and a dead write, removed for both reasons |
| 7 | RPN 0 on a **master** channel sets the master range **and mirrors it to the per-note range** (a LinnStrument transmits Bend Range only there) | **Preserve** — JUCE would set the master range alone. Without the mirror, a LinnStrument's member notes bend at the zone default instead of the range the player set |
| 8 | There is a per-note range in force before any controller transmits one | **Preserve** — but the NUMBER changed on 2026-08-23, deliberately. It was ±24, on the belief recorded here that "a LinnStrument maxes at ±24, so ±48 over-bends it". Roger Linn Design's panel-settings page says the reverse: the panel values ("+/- 2, 3, 12 or 24 semitones") are ONE CHANNEL mode, while ChPerNote — its MPE mode — reads "Bend Range: 48 (This uses the hidden setting 'Any Bend Range')" and reaches 96. The default is now the spec's ±48, and the MPE settings tab owns it for devices that transmit no RPN 0 |
| 8a | There is ONE range pair for the whole instrument, and a zone declaration does not disturb it | **Preserve** — and this is where the library and the synth genuinely part company. `MPEZoneLayout` keeps a pair per zone and resets both to the spec's defaults on every zone declaration, because an MCM carries no range of its own. Reading the ranges back out of it would therefore reset a range the player had already dialled in, so the pair stays this synth's (`mpePerNoteBendRangeInForce_`, `mpeMasterBendRangeInForce_`) and only the layout and the parsing are the library's |
| 9 | Master bend range starts at ±2 | **Preserve** — `kMpeMasterBendRange`, which is JUCE's default too, but the constant is this synth's for the reason in 8a |
| 10 | Pitch wheel on ch1 is a **global** bend: it moves sequencer and arp voices as well, which have no MPE note at all | Unchanged |
| 11 | Pitch wheel on ch16 is a member bend even where an upper zone exists | Unchanged — deliberate, documented at the call site |
| 12 | Channel pressure on a master channel is zone-wide, on a member channel per-note | Unchanged routing; the master/member question is the zone's to answer now |
| 13 | A voice's pressure is `max(per-note, zone-wide)`, so master and member compose instead of clobbering | Unchanged — `VoiceManager::pressureForVoice`, below the library's level |
| 14 | Poly key pressure matches by note number across external voices | Unchanged |
| 13a | **New, 2026-08-23.** A voice whose KEY came up keeps the pressure that key left, against the GLOBAL sources too. `pressureForNote` maxes the poly latch together with the mod wheel, the breath controller and channel pressure, and each of those three called `refreshPerformancePressure`, which rewrote the stored pressure of every sounding voice — releasing and pedal-held ones with the rest. With aftertouch → DCA, moving the wheel while a chord decayed brought the whole decayed chord back at full level, and moving it after a hard press cut the ringing tail to silence in one block. Same boundary the latch (row 14a) and the expression hand-off (row 25) already draw. Deliberately NOT narrowed to `isKeyHeldVoice`: the sequencers', the arpeggiator's and the drone's notes never had a finger to lose, and the wheel is the only thing driving them, so they follow it for their whole sounding life. Corpus case 55 |
| 14a | **New, 2026-08-23.** That match is by note NUMBER, and `polyPressureByNote` is a latch that `pressureForNote` takes the max of — so a value left standing is a FLOOR under everything that note can later receive, MPE Z included. Nothing lowered it but a panic. Play in the controller's Poly-AT mode, switch it to MPE, and every note number that had been pressed hard stayed pressed for the rest of the session. The latch is now dropped once no voice still HOLDS that pitch (the drone counts, and a releasing voice keeps its own frozen value) — from the note-off message AND from each of the three paths that release a voice directly without one: the damper, sostenuto, and the drone. A latch the clear forgets is the same permanent floor, reached the long way round. **And the question is not the voices' to answer at all.** The latch belongs to a FINGER. Voice state is a proxy for that, and it is wrong in both directions: under the arpeggiator a held key sounds nothing between steps, so the voices report "released" while the hand is still leaning in — and the arp's own step note-off carries `sourceId` -1, exactly like a key-up, so no field `noteOff` receives can tell them apart. In the other direction a voice can hold a pitch no key is on any more (the damper), and allocation can take such a voice over for a new note, wiping the very pedal flags the release scans key on, so nothing ever scans that pitch again. Nor can the arpeggiator's held-key list stand in: it is a SET, one entry per note number, so two fingers on one pitch — or a controller re-sending a note-on it never ended — look exactly like one. So the ledger is TOLD, not derived. Each note number holds the set of channels currently pressing it (`keyDownChannels_`, bit 0 for the computer keyboard, bits 1..16 for the MIDI channels), written at the three points a physical key really moves: the two loops that feed the arpeggiator from raw MIDI — the only place external key events are seen in both arp states — and the computer keyboard's own entry points. The first finger on a pitch starts its reading over, because aftertouch begins at nothing and rises; the last one off ends it, unless a voice still holds the pitch, which is the damper. It is kept where the messages are in ORDER: with the arp off the note events reach the sample-accurate walk, so the walk keeps it and every message is judged at its own instant; with the arp on they are filtered out of the stream and never get there, so that branch keeps its own — and holds its key-DOWNs back until after the walk, because aftertouch is NOT filtered out and reaches the walk at its own offset, where a mask that already knew about a later press would let the previous press's dying value through. Key-ups apply at once: shutting the gate early only ever refuses, and a refused message costs one reading where a stale one costs the whole note. A block-top pass is wrong in both directions — a key already up accepts, a key not yet down accepts into a reading the press is then seeded from. **And the reading is only ever WRITTEN while a finger is recorded on that key** — which is what closes the class rather than patching it, and it is an order problem rather than a filter: the key events of a whole buffer are read at the top of the block, aftertouch is applied in the sample-accurate walk further down, so an aftertouch sitting in the same buffer as the note-off that ended the key arrives after the reading was closed. At 256 samples that is under six milliseconds, which a controller streaming pressure hits on most releases. The ledger is cleared with the latch and never without it (`prepare`, `reset`, panic): a mask left standing suppresses every later clear AND every later first-press reset, so one held key across a stream restart would deafen that pitch for good. The voice scan stays as the second half of that test, and now runs from the six allocation paths that re-purpose a voice instead of releasing it — the mono legato and mono fresh strikes, the poly steal, the poly bind/glide, and the drone both claiming a voice and gliding to a new pitch, before the new note is seeded and with the displaced voice excluded. Corpus [30] the key, [34] the pedal, [31] a held unison, [35] the takeover, [36] the arpeggiator's gap, [37] a fresh press over a pedalled note, [38] a key lifted under the arpeggiator, [39] a re-sent note-on, [40] two fingers on one pitch, [41] the machine's own keyboard, [42] a stream restart, [43] aftertouch in the release buffer, [44] a panic under a held chord, [45] a key-up on a channel nothing is down on, [46] reset-all-controllers, [47] aftertouch before the press, [48] a gliding step, [49] a sounding note keeps its channel, [50] a panic un-owns what it cut off, [51] the same under the arpeggiator, [52] a gliding step's arrival |
| 15 | CC74 on a member channel is per-note timbre | Unchanged |
| 16 | CC74 on a master channel is **not** timbre: it stays the control-surface default that drives Scan (`kExtMap`, `midi/LaunchControlXLLeds.h:235`) | Unchanged — and it is why `MPEInstrument` could not simply be handed the stream: it reads CC74 on the master channel as zone-wide timbre |
| 17 | CC70 is `seq_steps`; CC102/CC106 are not MPE LSBs, so timbre stays 7-bit | Unchanged — `MPEInstrument` maps all three |
| 18 | Sustain (CC64) and sostenuto (CC66) are the synth's, held in `VoiceManager` | Unchanged — `MPEInstrument` would hold notes too, and two owners of note lifetime is a stuck-note bug |
| 19 | In XL DAW mode, channel 16 is the encoder/fader channel and is not musical: no notes, no CC6, no CC100 | **Preserve** — the guard moved to the feed site, where it now also covers CC101 and CC98/CC99, and the CC121 deselect carries it too, so no path reaches channel 16's RPN state in DAW mode. CC98/CC99 matter here: they are the XL's Lfo3 Amt and Drift3 Rate relative encoders (`midi/LaunchControlXLLeds.h:198-201`), sent constantly, and they must neither be consumed nor set the NRPN bit. CC101 the XL does not send; that half is so the invariant holds without exception. Costs one thing, stated rather than hidden: an RPN selection latched on ch16 before DAW mode was entered now survives Reset All Controllers, and so does its NRPN bit, until DAW mode ends |
| 20 | After an MCM, the next CC6 must not be swallowed as another one (a bound fader would rewrite the zone from its position) | **Preserve** — JUCE's detector latches the selected RPN per channel exactly as the old global pair did. Done through the library: `deselectMpeRpn` hands it the CC100=127 a controller would send, which is the old "LSB only" deselect expressed as MIDI |
| 21 | The RPN selection was ONE global pair where the MIDI spec has sixteen (the old code's own comment named this as a defect) | **Improves** — `MidiRPNDetector` holds sixteen states |
| 21a | Selecting an **NRPN** (CC98/CC99) must suppress the next CC6, so the NRPN's own data byte is not read as a bend range | **Improves** — the old parser tracked CC100/CC101 only, so an NRPN data byte landed on the bend range (corpus [21] measures it: 40 semitones where 12 was set). One bit per channel (`mpeNrpnSelected_`), set on CC98/CC99, cleared on CC100/CC101 and on the CC121 deselect. CC98/CC99 are consumed by nobody and reach the bindings as before — in XL DAW mode they *are* bindings, the Lfo3 Amt and Drift3 Rate encoders (`midi/LaunchControlXLLeds.h:198-201`) |
| 21b | The NRPN bytes must **not** be handed to `juce::MidiRPNDetector` or to `MPEZoneLayout`, even though the detector understands them | **Preserve** — this is the one place the obvious use of the library is wrong, and it was measured, not guessed. `MidiRPNDetector::ChannelState` keeps ONE parameter register per channel that CC98/CC99 and CC100/CC101 both write, distinguished only by an `isNRPN` flag on the way out (`juce_MidiRPN.cpp:81-85`); and `MPEZoneLayout::processRpnMessage` never reads that flag (`juce_MPEZoneLayout.cpp:131-137`). Feed it the NRPN bytes and a well-formed NRPN 6 write installs an MPE zone — or, with value 0, switches a declared one **off** mid-performance (corpus [21] and [24]). What the bit does NOT change, because it is ordinary RPN behaviour the hand-written code had too: a latched parameter MSB still combines with a later CC100. So [23] asserts the precise claim — inserting an NRPN select byte into an RPN sequence changes nothing about what that sequence does — rather than an absolute outcome, which depends on what was latched before it |
| 21c | A bend range above the spec's 96 is honoured, and does not disturb the next one | **Preserve** — the floor of 1 is the old parser's (a transmitted 0 meant one semitone, not none); there is no ceiling here, and capability 6's decision not to feed the layout is what keeps it that way, since JUCE clamps this parameter to 0..96 wherever it owns it (`juce_MPEZoneLayout.cpp:74-76`). Corpus [22] reads it at a QUARTER wheel: `SynthVoice` clamps at ±48, so at full deflection 96, 100 and 127 are indistinguishable and only a quarter of the range clears the clamp |
| 22 | A CC6 that completes no RPN still reaches a user binding | **Preserve** — an else-if cannot both consume a message and decline it, so `handleMpeRpnByte` returns whether it took the byte and the branch is chosen from that |
| 23 | An **arpeggiated** note is an internal note: channel 0, never MPE-tracked, whatever channel the key arrived on (`dsp/VoiceEvent.h:32-38`) | Unchanged |
| 23a | Switching the arpeggiator **off** hands the still-held keys back **with their MPE channel intact** (`PluginProcessor.cpp:3851-3862`) | Unchanged — and the reason note IDs would have been expensive: while the arp is on it *consumes* the note-ons, so a note tracker would have had to be fed from a second place |
| 24 | `voiceMidiChannel_` also discriminates origin: a step-seq slide must not continue a held external note | Unchanged — this is not MPE routing and must not be replaced by a note ID |
| 24a | **New, 2026-08-23.** A key-up names its member channel. `noteOff` matched by pitch alone, so the same pitch held on two member channels — a second finger on a key another finger already holds, or a repeat rotated onto a fresh channel while the first is down — was ended by whichever key came up first, and the finger still down pointed at a voice already releasing. `mpeChannel` matches the ORIGIN tag, because the key being lifted is the key that struck the voice. It was a wildcard when 0 — see row 24b, which closed that. Corpus [32] |
| 24b | **New, 2026-08-23.** The same test in the other direction, and for the SOURCE half of the match. Every internal note event — the step sequencer's and the arpeggiator's — carries `sourceId` −1, and so does external MIDI; `noteOff` read that −1 as a WILDCARD, matching every voice of the pitch whatever struck it. Hold a note, start the sequencer, and the note was cut the first time the pattern reached that pitch (measured: 0.05 s at 240 BPM) with the finger still down. Under the damper it was not cut but MARKED sustained, which is worse: `isKeyHeldVoice` then reads false under a hand that never moved, so the Cache/Snap travellers stop seeing the hand, `claimExprChannel` is free to strip that voice's member channel, and lifting the pedal releases a key nobody lifted. The source test is now strict (the same form the bind branch has always used) and origin is required in both directions, so an internal note-off ends internal notes and nothing else. Every caller that means an external key names its channel. Corpus case 56 |
| 23b | **New, 2026-08-23.** The arpeggiator's held-key set is keyed by pitch AND channel, the same identity the pressure ledger uses. Keyed by pitch alone the second of two fingers on one key never got an entry, and the first key-up took the shared one away: switching the arp off then handed back nothing for a key that was still pressed (silence under it until it was released and pressed again), and switching the arp on released only one of the two voices, leaving the other droning under the arpeggio. The pattern still carries a shared pitch once — the arp plays pitches, not fingers. Corpus case 57 |
| 25 | A voice's MPE tag is cleared when it goes idle | **Split, 2026-08-23.** There are now two tags. `voiceMidiChannel_` is ORIGIN (row 24) and still falls only when the voice goes idle. `voiceExprChannel_` is EXPRESSION routing, and it falls on idle **or on hand-off**: when a new note is struck on a member channel, `claimExprChannel` strips that channel from every other voice. Without it a releasing or pedal-held voice kept the tag, and because an MPE controller reuses its member channels, the next key's pressure, bend and slide also drove the old, dying note — a released note swelling back up under AT→DCA. Poly-AT never showed it because it matches by note NUMBER, which is exactly why PolyAT mode behaved and MPE mode did not. The voice that loses the channel keeps its last expression, frozen. It is taken ONLY from voices no finger is on: two notes really can be down on one channel — a plain keyboard transmitting on channel 2, or an MPE zone with fewer members than fingers — and there the channel drives BOTH, which is MPE's own rule and also what bounds the freeze (on a still-held voice nothing would ever end it). Corpus [28] release, [29] pedal, [33] the boundary |
| 26 | Expression is applied at the event's sample position within the block, not at block start | Unchanged — the feed sits inside the existing sample-accurate walk |
| 27 | A note does not JUMP on Y because of a CC74 that preceded it, and starts unpressed | Preserved, by a different mechanism since 2026-08-23, and the row title is narrower than it was for that reason. It used to read "ignoring values received on that channel before the note", and the note did ignore them: it started at a fixed value. It now *adopts* the preceding value as its ORIGIN and starts at zero travel from there. `MPEInstrument` would have applied it as the note's initial VALUE, which is the jump this has always refused. Corpus [14] asserts the no-jump; what the old title claimed beyond that is gone, and [25] carries what replaced it |

## 2. What the library refuses that the old code allowed

`MPEInstrument::noteOn` returns early unless the channel belongs to a zone
(`juce_MPEInstrument.cpp:354`), and the same rule governs which channels
`MPEZoneLayout` treats as members. The default layout has no zones at all. The
old code needed no declaration: it routed per-note expression on any non-master
channel whether or not an MCM had ever arrived.

So the new path declares a **lower zone with 15 members** at construction —
master ch1, members ch2..16, per-note range 24, master range 2. That is the
layout every host defaults to, it makes channel 16 a member (capability 2), and
an ordinary MIDI keyboard on channel 1 is a note on the zone master.

The initial layout leaves no channel uncovered, and an MCM that shrinks the
lower zone does create uncovered ones — which costs nothing here, because
`isMpeMasterChannel` is the only reader and its answer for an uncovered channel
is "member", the same as before. The per-note bend range is one value for the
whole instrument (`mpePerNoteBendRangeInForce_`), exactly as the single
hand-written range was, so no channel can fall back to a different one either.

## 3. Why `juce::MPEInstrument` is not here

Measured, not argued — `tools/measure_mpe_instrument_rt.cpp`:

```
juce::Array<MPENote> -- 50 remove/add cycles at 16 held
  storage buffer moved                  : 0 times  (no reallocation)
  one note on/off, 50 times             : 2 moves
  a  4-note chord down and up, 50 times : 150 moves  <-- REALLOCATES WHILE PLAYING
  a 10-note chord down and up, 50 times : 250 moves  <-- REALLOCATES WHILE PLAYING
  a 15-note chord down and up, 50 times : 250 moves  <-- REALLOCATES WHILE PLAYING
```

`MPEInstrument` holds its live notes in a `juce::Array<MPENote>`, and
`Array::remove` calls `minimiseStorageAfterRemoval` (`juce_Array.h:1120-1130`)
on every single removal. So the storage shrinks as a chord is released and has
to grow again when the next one is played: five reallocations per chord cycle,
for as long as the player plays. A warm-up does not help, because the shrink
happens on release. `MPEInstrument` would have to be fed from `processBlock`,
and the project forbids allocating on the audio thread — so it stays out.

The counters used to reach that conclusion are worth naming, because the first
two could not see it. `operator new` misses it (`juce::Array` goes through
`HeapBlock`, i.e. `std::malloc`), and the malloc-zone block count misses it too
(a grow is free-then-alloc, which nets to zero live blocks). Watching the
storage pointer move is what shows it.

What ships instead measures clean, on the shipped path and not just the library
in isolation:

```
juce::MPEZoneLayout alone -- RPN traffic x300 : new=0 (this thread)

T5ynthProcessor::processBlock -- 200 blocks
  baseline, one held note, no MIDI       : new=0 (this thread)
  the same, plus 81 MPE messages a block : new=0 (this thread)
```

The allocation counter is filtered to the measuring thread on purpose: the
processor starts an event-log writer and JUCE a message thread, and both
allocate throughout, so a process-wide count is noise.

## 4. What was given up by not taking the note tracker

This section used to say that note IDs would buy one thing the channel key
cannot: correct behaviour when a controller reuses a member channel while the
previous note on it is still releasing — "today both notes follow that channel's
expression", called rare and cheap. It was neither. That is the Osmose's
erratic MPE aftertouch, and it is fixed by the hand-off in capability 25, with
no note tracker: `voiceExprChannel_` falls on hand-off as well as on idle. What
a note ID would still buy is the case where the same channel carries two
notes the receiver cannot tell apart at all — and there this synth deliberately
drives BOTH, which is MPE's own rule for it (capability 25, corpus [33]).

So the honest remainder is small and lies elsewhere: a note ID would let the
arpeggiator's consumed note-ons be tracked from one place instead of two
(capability 23a), and it would survive a controller that changes a note's
channel mid-note, which none of the reference instruments does.

## 4a. The two normalisations (2026-08-23)

Neither is parity — both are new, and both exist because a preset has to mean
the same gesture on a different controller. They are here because the corpus
now gates them.

**Y's rest is the value in force at Note On, per note.** The MPE spec (1.0,
§3.3.5) defines two schemes and says outright that neither fits every
instrument: *Initial-position*, where the value at Note On encodes where the
interaction started, and *Initial-64*, whose initial value "must be 40h (64
decimal), such that movement can follow in either a positive or negative
direction". A receiver that hard-codes either is wrong for the other half of
the instruments. Reading the value at Note On as the note's origin fits both
and needs no setting and no device table: an Osmose sends CC74 = 0 immediately
before each Note On — measured, 201 of 203 note-ons in a 120 s capture — so for
those 201 the travel is entirely upward and bit-identical to a plain absolute
reading, while an Initial-64 controller becomes bipolar around where the finger
landed. `VoiceManager::channelTimbre_` → `SynthVoice::beginTimbre`. Corpus [25].

The travel is the PLAIN difference, not scaled to the remaining span in each
direction. Scaling was the first version and it inverted the axis at the ends:
a rest captured at 127 left no upward span, so the reading pinned at 0 going up
and ran to −1 going down — the gesture sign-flipped, on the axis Cutoff and Scan
start on. Plain, the same note reads 0 at the top and travels down as the finger
lifts. Monotone with the hand at every rest, which is the property that matters;
the price is that a rest in the middle has half the travel each way, and that is
simply true of the finger. Corpus [25] third block.

Two things the rest is deliberately NOT re-captured for: a mono legato slide and
a glide continuation. Both are the same note reaching a new pitch, so moving
their origin would move the ground under a finger that never left the key.
Corpus [27].

Cost, stated: the Csound `timb` channel is 0..1 and takes the upward half
(`VoiceManager.cpp`, `c.timbre`). Unchanged for any note that began at the
bottom of the axis; a note that began higher loses its downward half there.

**X as a modulation source is measured in semitones, not wheel travel.** The
bend is unchanged — wheel travel times the range in force. But the wheel
fraction is not comparable between instruments: an Osmose's entire lateral
travel is ±171 of ±8192, 2.1 % of the wheel (`tools/midi_monitor.cpp`, 120 s of
ordinary playing), while a LinnStrument's slide crosses it many times over. So
a target routed to X reads `bend semitones / kMpeXFullScaleSemitones`, clamped
to ±1. One semitone, because it is the smallest unambiguously musical interval.
What that gives on the measured Osmose, stated rather than guessed: ±171 of the
wheel against the fallback range of 48 is ±1.00 semitones, so a full lean fills
the axis exactly. Whether the device intends that is not knowable from the
capture — it transmitted no RPN 0 in 120 s, so its own assumed receiver range is
unrecorded, and the MPE tab's diagnosis is what says so on screen rather than in
a comment. What is certain is the direction: 2.1 % of the wheel becomes 100 % of
the axis. The MPE settings tab owns the value in force; the constant is only its
default. Corpus [26].

## 4b. The MPE tab (2026-08-23)

Not parity either. It exists because of what §4a and capability 8 keep running
into: **the numbers in MPE are negotiated exactly once.** A controller announces
its zone with an MPE Configuration Message and its bend range with RPN 0, both
at power-on or when its MPE mode is selected. A plugin instantiated after that
moment never hears either, and MIDI has no read-back to ask with. So a receiver
falls back to a default and cannot know whether the default is what the device
meant — which is exactly how `kMpePerNoteBendRange` sat at 24 for a fortnight
with a comment asserting the opposite of the truth, and how the Osmose's 120 s
capture came to contain no RPN 0 at all.

Two halves, and the lower one is the reason for the upper one:

- **Three settings**, machine-wide and never in a preset: per-note bend range,
  master bend range, and `mpeXFullScaleSemitones`. Machine-wide is what answers
  "can one preset drive different MPE controllers" — the preset carries the
  DEPTHS, this carries what a full gesture is worth on this instrument, so a
  depth of 0.6 on X is the same lean on both and moving keyboards does not
  re-tune the patch.
- **A live read-out** of what the controller is sending: note channels, whether
  a range and a zone were transmitted, the widest lean seen and what fraction of
  the X axis it fills, CC74's observed span and which channels carry it, and
  which kinds of pressure arrive. `T5ynthProcessor::MpeObservation` — atomics
  written from the audio thread as MIDI arrives, polled at 4 Hz by the page and
  only while it is visible.

Whoever wrote LAST wins on the two ranges: the player's setting and the device's
RPN 0 are the same authority over the same number, and the read-out is what says
which one spoke. A transmitted range therefore moves the control with it, and a
value the drop-down does not list (a device may send any of 1..96) is added as
it is rather than rounded to a listed one.

The two in-force ranges became `std::atomic<int>` for this: they had one writer
on the audio thread and now have a second on the message thread.

**What this cost the gate, and how it was paid.** The moment those three numbers
became machine-wide user settings, the corpus stopped being hermetic: it built a
real `T5ynthProcessor` per case and asserted the literal 48, so the first player
who set their own instrument to 24 would have turned the capability guard red —
and correctly, which is worse than incorrectly. The split now is: the DEFAULTS
are pinned at compile time (`static_assert` against
`T5ynthProcessor::kMpePerNoteBendRange` and its two siblings, which is why those
constants are public), and every behavioural case asserts the RELATIONSHIP
against the value in force, read back off the processor. Case 26 declares its
own ranges by RPN rather than leaning on the fallback at all. Verified by
injecting 24 / 12 / 2.0 into the settings file and re-running: 90 checks, 0
failures, unchanged.

`tools/render_mpe_tab.cpp` renders the page offline at the settings overlay's
floor and ceiling (400×300 and 600×500) for four device situations, so its
layout can be judged without opening the synth. The floor is the one that
decides: the read-out is the element that has to give.

## 5. The gate

`tools/test_mpe_parity.cpp` is the frozen corpus: 210 assertions driven as raw
MIDI through the real `T5ynthProcessor::processBlock`, reading the result off
the voices. It was written against the hand-written code and was green on it
before the library was introduced — that is what makes it a record of the old
path's capability rather than a description of the new one, and it is why it
could not simply be regenerated afterwards.

It is mutation-checked in both directions, because a suite that cannot fail
certifies nothing: reinstating the pre-`03286a97` channel-16 bug fails cases 4
and 6; moving the per-note default off ±48 fails 2, 17, 19 and 26;
putting the three expression setters back on `voiceMidiChannel_` fails 28 and
29 with four assertions, and dropping the hand-off's held-key guard fails 33
with four more — the chord on one channel tears apart, only the last-struck
note still bending; leaving `polyPressureByNote` uncleared fails 30, and
clearing it only on the note-off message fails 34, and dropping the clear from
the allocation paths fails 35 twice. Each half of the key ledger is checked on
its own: dropping it from the clear fails 36 (an arp gap reads 0.000 under a
finger that never moved), dropping the key-UP half fails eleven assertions
across seven cases, dropping the first-finger test so every key-down resets
fails 39 and 40, folding the channels onto one bit fails 40 under the arp — and
only there, which is why that half of the case exists — and dropping the
computer keyboard's own keys fails 41. The ledger's own lifetime is checked the
same way: leaving it standing across `prepare`/`reset` fails 42, writing a
reading with no finger recorded on the key fails 43 and 44, and letting a
key-up on an unheld channel take the whole entry fails 45. Moving the ledger
back into the block-top pass fails 47 twice and, for the arp-on half, 51;
dropping the clear from the poly bind/glide branch fails 48 and dropping its
arrival seed fails 52; leaving the tags on a panic's dying voices fails 50 three
times — the tail swells, slides and changes timbre under a hand still on the
key. The arp-on pass keeps the ledger in two instants and both are checked:
deferring the key event itself rather than only the gate fails 53 twice (the
first-finger reset lands behind the arp step it was for), and applying the
key-down after the key-up of the same buffer fails 54 (a note shorter than one
buffer leaves a finger recorded on a key nobody is touching, and nothing can
clear it again). Letting `refreshPerformancePressure` write every active voice
fails 55 four times — the decayed chord returns at 1.000, the tail is cut to
0.000, and the pedalled note drops to 0.008. That case only discriminates
because it re-presses the key first: while the note's latch still stands it
holds the pedalled voice up on its own, and the assertion would pass whatever
the function does. Putting `sourceId < 0` back as a wildcard in `noteOff` fails
56 twice, once with the damper up and once with it down; keying the
arpeggiator's held set by pitch alone again fails 57 and the arp half of 40 —
and 57 needed its mid-case check that neither voice drones, because without it
the voice the on-edge failed to release satisfied the hand-back assertion by
itself. Dropping the channel
from the external note-off fails 31 and 32. Every one of these run, measured, and reverted, not
argued;
dropping the master-to-member mirror fails 9; dropping the RPN deselect fails 7.

Writing it corrected this enumeration twice. Row 23, where the arpeggiator
turned out to do the opposite of what was written down here first. And case 20,
which is a capability the corpus did not originally cover and the first cut of
the new code therefore lost: a controller that transmits its bend range BEFORE
declaring its zone had that range reset by the declaration. It was added after
the fact, so it was checked against the hand-written code as well before being
trusted — a case only the new implementation has ever passed is not evidence.

Cases 25 to 27 are the exception to all of the above and are labelled as such:
they assert the two normalisations in §4a, which the hand-written code never
had. They are not parity and are not evidence about it. They are here because a
capability that is a requirement but not a test is a regression waiting to ship.

Cases 21 to 24 came from the adversarial reviews and were run against the
hand-written code for the same reason. 22, 23 and 24 pass there, so they are
parity. 21's first half **fails** there — 40 semitones where 12 was set — so it
is not parity and is labelled in the corpus as the improvement it is.

Those four cases record three separate ways a test can look like evidence and
not be one. Each was caught by a review, not by the suite:

* **The convenient value.** Case 21's first version picked NRPN parameter
  **5** — the one low number `MPEZoneLayout` ignores. It passed while the same
  change was letting an NRPN **6** install a zone, which is strictly worse than
  the misfire it fixed, because a zone decides master-vs-member for pressure and
  CC74 as well. It picks 6 now, on the channel where accepting it does the most
  damage.
* **The convenient starting state.** Case 23's first version ran from a fresh
  channel, where the parameter register is still `0xff` — and *that*, not the
  code under test, is what made the sequences inert. It runs each sequence twice
  now, once from fresh and once behind the RPN a real device sends first, and
  compares the variant carrying the NRPN byte against the one without it. That
  is the actual claim; an absolute outcome would have been the wrong assertion,
  because a latched MSB legitimately changes what a later CC100 selects.
* **The saturated observable.** Case 22's first version asserted an out-of-spec
  bend range at a full wheel, where `SynthVoice`'s ±48 clamp makes 96, 100 and
  127 identical. It was then deleted as unfixable, which was also wrong: a
  quarter wheel clears the clamp and reads the range back directly.

A fourth case exists because every zone assertion in this suite starts from "no
zone", so the observable saturates at "no zone" and a defect that only
**destroys** zones passes all of them. Case 24 declares one first. Against the
revision that had the defect, cases 21, 23 and 24 all fail.

Not reachable from an offline harness, and stated in the tool rather than
skipped quietly: the Launch Control XL DAW-mode exemptions, because
`dawModeActive_` is only set when a real XL output device is opened.
