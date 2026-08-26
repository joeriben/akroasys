#pragma once
#include <JuceHeader.h>
#include "../dsp/BlockParams.h"   // PID::, EnvTarget::, LfoTarget::
#include "../dsp/DriftLFO.h"      // DriftLFO::TgtFilter

// ─────────────────────────────────────────────────────────────────────────────
// Calibration migration.
//
// When a DSP transfer function's full-scale is recalibrated (e.g. cutoff
// modulation goes from ±10 to ±4 octaves), a value stored under the OLD full-scale
// would sound different under the new one. To keep existing presets / DAW sessions
// sonically IDENTICAL, every stored value authored before a calibration is rescaled
// on load by the inverse of the full-scale change. The epoch counter records "which
// calibrations a file predates"; bump kEpoch and add a Rescale for each change.
//
// Two kinds of rescale:
//   • Rescale     — unconditional: a parameter always means the recalibrated thing
//                   (e.g. the AT→Cutoff amount is always a cutoff depth).
//   • CondRescale — target-conditional: a parameter only means the recalibrated
//                   thing when a sibling TARGET param selects it (e.g. an env
//                   "amount" is only a cutoff depth when that env targets Filter;
//                   on other targets it means loudness/pitch/scan and must NOT be
//                   rescaled). The condition is read from the same file.
//
// Two application surfaces share this one table:
//   • live APVTS params  — DAW state restore + main .t5p params (PluginProcessor)
//   • stored ValueTrees  — per-slot snapshot param trees (.t5p only, MainPanel)
//
// Stamped into every save as `calibEpoch`; ABSENT on load = epoch 0 (the
// pre-calibration regime). New saves always stamp kEpoch, so a load→re-save
// cycle never double-migrates. Rescaled values are CLAMPED to the param's range
// when applied (so an old patch sweeping past the new ceiling caps at the ceiling).
// ─────────────────────────────────────────────────────────────────────────────
namespace Calibration
{
// Current calibration epoch. Bump by 1 for each new recalibration that needs a
// migration entry below.
//   Epoch 1: AT→Cutoff full-scale 10→4 octaves (Phase 1).
//   Epoch 2: env/LFO/Drift→Cutoff folded into the unified ±4-oct cutoff bus
//            (was ±10 for env/LFO/Drift); target-conditional on Filter.
//   Epoch 3: CoordinationMode choice table shrank 4→3 (2026-07-16). The
//            never-implemented no-op entries "Algebraic"(2)/"Counterpoint"(3)
//            were removed and raw index 2 became the audible Dialog mode. A
//            2/3 stored by an older file meant "behaves as Independent" —
//            keep that meaning instead of silently activating Dialog
//            (IndexRemap, not a rescale). Only the XML surfaces carry raw
//            indices; the .t5p JSON stores key strings and maps removed keys
//            via choiceFromKey's unknown-key fallback.
//   Epoch 4: OscMix mod-target removed (2026-07-17, the dual A+B oscillator
//            split is dead). OscMix was the LAST slot in both EnvTarget (16)
//            and LfoTarget (15), so no other target's index shifts; a stored
//            index of 16 (env target) / 15 (lfo target) — the only values that
//            meant OscMix — remaps to None. Same two-surface handling as epoch
//            3: the XML/DAW raw indices migrate via the IndexRemaps below; the
//            .t5p JSON's now-unknown "osc_mix" key falls back to None via
//            choiceFromKey.
//   Epoch 5: FX Mix wet/dry law + wet-path normalisation (2026-07-22). Every
//            wet path was a different level at Mix=1.0 (Tape +4.7 dB, BBD
//            -2.5 dB, Freeverb +5.5 dB, plate pushed to +6.0 dB by the retired
//            kPlateWetGain=2.0 comp) and the law was `wet=m, dry=1-m` — so the
//            usable range sat in the bottom third of the travel and mid-travel
//            cost 6 dB of direct signal. Now: every path trimmed to unity plus
//            a constant-power law (wet=m^1.5, dry=sqrt(1-m³)). A stored value
//            keeps its AUDIBLE wet/dry balance by moving to the knob position
//            that reproduces it — a closed-form remap, not a factor, and
//            conditional on the effect type because each type's old path gain
//            (and, for Algo, its old squared curve) differed. See FxMixLaw.
//   Epoch 6: AT→DCA law (2026-07-25). The old law was gain ×(1 + pressure·amt),
//            so a POSITIVE amount left the resting gain at unity and pressure
//            pushed ABOVE it — at most ×2 (+6 dB). That is a trim, not an amp
//            aftertouch, and the boost landed in the always-on master limiter.
//            The new law gives the positive direction the whole amp range by
//            making the amount the resting ATTENUATION that pressure reopens:
//            ×(1 − amt·(1 − pressure)) — rest = 1−amt, full pressure = 1.0. The
//            negative direction is unchanged (×(1 + amt·pressure)).
//            No value under the new law reproduces the old positive branch: its
//            resting gain was unity, which the new law reaches only at amt = 0.
//            A stored positive amount is therefore RESET to 0, not rescaled —
//            that keeps an old file's resting level exactly as it was and drops
//            only the ≤6 dB the old branch could add at full pressure. Stored
//            negative values keep their meaning and are left untouched.
//   Epoch 7: Ladder + Warp resonance law (2026-07-28). Both ladder filters put
//            the knob straight on the feedback gain (k = 4.2·r), but a 4-pole
//            ZDF ladder's peak goes as 1/(1 − k/k_pole) — so the first half of
//            the travel delivered under +1 dB and the last eighth climbed 40.
//            The knob now travels evenly in dB of peak (LadderResoLaw), which
//            is the law the SVF has always had (Q = 0.5·36^r). Conditional on
//            the algorithm, and for the Warp on the saturation style too, since
//            its pole is per style: an SVF preset's resonance is untouched, a
//            Ladder or Warp preset's stored position moves to the one producing
//            the SAME feedback gain — a closed-form inversion, not a factor. A
//            file with no algorithm property predates Ladder/Warp and was an
//            SVF; one with no style property loaded as Tanh then and now.
//            NOT covered, and not coverable by a scalar: MODULATION of the
//            resonance (aftertouch/env/LFO/drift → Resonance). Those amounts
//            are additive offsets in knob units, so the swing a stored depth
//            produces depends on where the base sits — an old Ladder preset
//            keeps its resting resonance exactly, but a modulated sweep over it
//            covers a different span. Same boundary every law change in this
//            file has: epoch 5 migrated the FX mix knob, not modulation of it.
//   Epoch 8: cutoff-bus depth law (2026-08-01). Epoch 2's unified bus was LINEAR
//            onto ±4 octaves, and four octaves is not a filter envelope: from a
//            low base (156 Hz → 2.5 kHz) nothing could open the filter, and the
//            epoch-2 migration itself clamped every older preset that swept
//            wider ("accepted trade-off for whole-slider controllability",
//            17ba4844). The ceiling was never the problem it solved — the ±10
//            law it replaced put the musical band in the bottom tenth of a
//            LINEAR control. So the full-scale goes back to ±10 octaves (the
//            whole 20 Hz–20 kHz span) and the travel gets the curve the old law
//            lacked: ModCalib::cutoffDepthCurve, position^2.3 of full scale.
//            The retired ±4 now sits at 0.67 of the travel and mid-travel is
//            within 0.03 oct of what it was, so the usable half of every cutoff
//            depth control keeps its feel.
//            Every source on the bus is covered — env amounts, LFO depths,
//            drift depths (each target-conditional on Filter) and the AT cutoff
//            amount (unconditional, and bipolar, so the remap is sign-
//            preserving). A stored position moves to the one producing the SAME
//            octave swing (ModCalib::migrateCutoffDepth): a closed-form
//            inversion, not a factor, like epochs 5 and 7.
//            TWO deliberate non-identities, both restorations rather than side
//            effects: (a) a pre-epoch-2 file chains ×2.5 then this remap and
//            gets back the SWEEP it was authored with, including the part epoch
//            2 clamped away — the old ±10 and the new ±10 are the same ten
//            octaves, so full depth means full depth again (the knob POSITION
//            moves, of course: a^(1/2.3) equals a only at 0 and 1);
//            (b) MPE timbre had no stored depth (the CC 74 travel WAS the
//            amount), so it could not be migrated and kept its ±4 octaves by
//            construction. That private path is gone: timbre is now the Y
//            source of the expression matrix and carries an ordinary per-target
//            amount, which this epoch's remap covers like any other.
//            SAME BOUNDARY as epochs 5 and 7: modulation OF a depth (an env or
//            LFO driving an LFO's Amt) is not migrated. The depth it lands on
//            is curved live, so the destination is right; the swing an old
//            stored modulation depth produces around it is not preserved.
//   Epoch 9: env curve choice → continuous bend (2026-08-05). The 15 per-stage
//            curve parameters (5 envelopes × A/D/R) were a 5-entry choice, 0=Log
//            … 4=Exp; they are now a float bend whose five named anchors ARE
//            those shapes (bend = (index−2)/2), on a range that reaches one unit
//            past the anchor each stage sags towards: attack [-2,+1], decay and
//            release [-1,+2]. A stored INDEX has to travel, and unmigrated it
//            lands somewhere legal rather than somewhere obviously broken —
//            Lin (2) would read as Exp 2.00 on a release, Log (0) as Lin
//            everywhere: the worst kind of near-miss. Affine, exact on all five:
//            ChoiceToValue. Only the surfaces that store the DENORMALISED value
//            need it — the DAW-session XML and the .t5p snapshot trees below.
//            NOT the .t5p JSON (it stores the shape by key and maps straight
//            onto the anchor, so migrateScalar deliberately carries no entry: an
//            epoch-8 file would otherwise be migrated twice).
//            NOT host automation lanes or MIDI-learn mappings either — but for
//            the opposite reason, and this one is a real cost, not a nicety.
//            They ride the NORMALISED value, which lives in the host, not in our
//            state, so there is nothing here to migrate; and because the range
//            widened by a different amount on each side, index/4 no longer
//            equals the anchor's normalised position. A lane written against the
//            old 5-way choice therefore lands one shape off. Documented in
//            docs/devlog.md 2026-08-05 rather than papered over.
// Epoch 10: the per-note MPE bend became a WIRING. X reaching the pitch used
//            to be a fact of the instrument, applied whatever the Pitch row
//            said; now the row's expression source decides, and it ships on X
//            (BJ, 26.08.2026: "Pitch<-X als Verschaltung statt fest
//            verdrahtet, mit Default an"). A file written before this stored
//            "off" on that row to mean "nothing MODULATES the pitch" - the
//            audible bend was on regardless - so an unmigrated "off" would
//            silently take the bend away from every MPE preset ever saved.
//            A PAIR of writes, not an IndexRemap: the amount bar was
//            independently draggable in that window, so "off" with a raised
//            amount was a row doing nothing at all, and carrying that amount
//            across would have it drive the pitch bus on top of the bend the
//            row now switches on - a double bend performed by the migration.
//            The inert amount is zeroed with it. A stored Y or Z is left
//            alone: deliberate, and it keeps working - at the documented cost
//            that the audible bend goes with it.
//            THREE surfaces need it and only two get it from this table. The
//            DAW-session XML and the .t5p snapshot trees go through
//            migrateValueTree. The .t5p JSON does NOT - it resolves the row
//            from the file's key and has no epoch pass - so importJsonPreset
//            carries the same rule inline. That surface is the one that bites:
//            sources shipped while the epoch was 9, so every .t5p written since
//            2026-08-23, INCLUDING the standalone's own session buffer, holds
//            "pitch": "off" stamped epoch 9. Unmigrated, the instrument comes
//            up with lateral finger travel doing nothing.
//            Files that predate expression sources ENTIRELY never reach this
//            table - they carry no source block and are filled in by the legacy
//            fallback. There the Pitch row cannot keep both halves of what such
//            a file did, so the AMOUNT decides: at rest, X, because the only
//            thing there was to lose is the per-note bend; raised, kLegacy,
//            because the player deliberately dialled a pressure-to-pitch depth.
//            NOT .t5evt replay either: migrateLoggedValue applies rescales
//            and choiceToValues but never indexRemaps, so a tape that logged a
//            touch of this switch replays the raw index. That hole is older
//            than this epoch - epochs 3 and 4 have it too - but epoch 10 is the
//            first index remap whose unmigrated value is AUDIBLE, so it is
//            named here rather than left to be discovered.
//            NOT host automation lanes or MIDI-learn mappings. They ride the
//            normalised value, which lives in the host, and the ExprSource
//            table did not change size or order - so a lane parked on "off"
//            still resolves to "off". The cost is that "off" there now silences
//            the audible bend as well as leaving the row unwired, which is a
//            heavier consequence than the same lane had before. Unmigratable by
//            construction, like epoch 9's.
inline constexpr int kEpoch = 10;

struct Rescale
{
    const char* id;         // parameter ID whose stored value is rescaled
    float       factor;     // multiply the stored value by this
    int         sinceEpoch; // applied when the file's epoch < this
};

// Target-conditional rescale: only applied when the sibling target param selects
// the matching destination index.
struct CondRescale
{
    const char* id;         // value parameter ID to rescale
    float       factor;     // multiply the stored value by this
    int         sinceEpoch; // applied when the file's epoch < this
    const char* condId;     // sibling TARGET parameter ID to inspect
    int         condValue;  // rescale only when condId's selected index == this
};

// One-sided law change: a parameter whose transfer function changed on ONE side
// only, so severely that no value under the new law reproduces the old side's
// behaviour (a factor cannot express it). Stored values on the affected side are
// reset to a neutral constant; the other side keeps its stored value untouched.
struct SidedReset
{
    const char* id;         // parameter whose one-sided law changed
    float       above;      // stored values strictly greater than this...
    float       toValue;    // ...become this
    int         sinceEpoch; // applied when the file's epoch < this
};

// Choice-index remap: when a choice table changes MEANING (entries removed /
// repurposed), a raw stored index must be remapped, not rescaled. Any stored
// index >= minStored maps onto toIndex.
struct IndexRemap
{
    const char* id;         // choice parameter ID whose stored index is remapped
    int         minStored;  // stored indices >= this...
    int         toIndex;    // ...become this
    int         sinceEpoch; // applied when the file's epoch < this
};

// A parameter that WAS a choice and is now a continuous value over the same span.
// The stored index travels affinely: value = index·scale + offset.
struct ChoiceToValue
{
    const char* id;         // parameter ID that changed from choice to float
    float       scale;      // stored index × this...
    float       offset;     // ...plus this
    int         sinceEpoch; // applied when the file's epoch < this
};

// One entry per unconditionally-recalibrated parameter, in epoch order.
//   Epoch 1: AT→Cutoff full-scale 10→4 octaves. A stored amount a produced 10·a
//            octaves at full pressure; ×(10/4) keeps the same octave swing under
//            the new constant. AT→Cutoff is now part of the unified ±4 cutoff bus,
//            which keeps the same ±4 full-scale, so this entry is still correct.
inline const std::array<Rescale, 1>& rescales()
{
    static const std::array<Rescale, 1> table = { {
        { PID::aftertouchAmtCutoff, 10.0f / 4.0f, 1 },
    } };
    return table;
}

// One entry per target-conditional recalibration.
//   Epoch 2: env/LFO/Drift → Cutoff unified into the ±4-oct bus.
//     • env amount / LFO depth were ×10 octaves when targeting Filter → ×(10/4)=2.5.
//     • Drift filter offset was depth·0.3·10 = ±3 oct; now depth·1.0·4 = ±4 oct,
//       so depth ×(3/4)=0.75 preserves the ±3-oct swing (never clamps).
inline const std::array<CondRescale, 9>& condRescales()
{
    static const std::array<CondRescale, 9> table = { {
        { PID::ampAmount,   2.5f,  2, PID::ampTarget,    EnvTarget::Filter   },
        { PID::mod1Amount,  2.5f,  2, PID::mod1Target,   EnvTarget::Filter   },
        { PID::mod2Amount,  2.5f,  2, PID::mod2Target,   EnvTarget::Filter   },
        { PID::lfo1Depth,   2.5f,  2, PID::lfo1Target,   LfoTarget::Filter   },
        { PID::lfo2Depth,   2.5f,  2, PID::lfo2Target,   LfoTarget::Filter   },
        { PID::lfo3Depth,   2.5f,  2, PID::lfo3Target,   LfoTarget::Filter   },
        { PID::drift1Depth, 0.75f, 2, PID::drift1Target, DriftLFO::TgtFilter },
        { PID::drift2Depth, 0.75f, 2, PID::drift2Target, DriftLFO::TgtFilter },
        { PID::drift3Depth, 0.75f, 2, PID::drift3Target, DriftLFO::TgtFilter },
    } };
    return table;
}

// One entry per one-sided law change.
//   Epoch 6: AT→DCA positive branch. The old positive law boosted above unity by
//            at most +6 dB from a resting gain of 1.0; the new one attenuates the
//            rest by the amount and lets pressure reopen it. Only amt = 0 keeps
//            the old resting gain, so positives reset to 0. Negatives share the
//            unchanged ×(1 + amt·pressure) branch and must NOT be touched.
inline const std::array<SidedReset, 1>& sidedResets()
{
    static const std::array<SidedReset, 1> table = { {
        { PID::aftertouchAmtDca, 0.0f, 0.0f, 6 },
    } };
    return table;
}

// One entry per choice-table meaning change.
//   Epoch 3: CoordinationMode 4→3. Stored 2 ("Algebraic") / 3 ("Counterpoint")
//            were silent Independent fall-throughs; map them to Independent so
//            an old DAW session / snapshot does not silently activate Dialog
//            (which now owns index 2). Host automation LANES riding those old
//            indices cannot be detected or migrated — only stored state can.
//
//            KNOWN BOUNDARY (verified 2026-07-16, dev-machine-only): the table
//            meaning changed at 8843e3cc while kEpoch was still 2; the bump to
//            3 landed one commit later (ece51fe1, ~38 min after). State saved
//            with Dialog by a dev build inside that window is stamped epoch 2
//            and is byte-indistinguishable from a genuine old no-op index 2 —
//            this remap reverts it to Independent. Unresolvable retroactively;
//            the trade-off deliberately protects every pre-8843e3cc file (the
//            only kind that exists outside build_clean — no release carries
//            the window). LESSON: bump kEpoch IN the commit that changes a
//            table's meaning, never in a follow-up.
inline const std::array<IndexRemap, 7>& indexRemaps()
{
    static const std::array<IndexRemap, 7> table = { {
        { PID::genCoordinationMode, 2, CoordinationMode::Independent, 3 },
        // Epoch 4: a stored OscMix target → None. OscMix was the removed last
        // slot: raw index 16 for env targets, 15 for lfo targets (the enum
        // constants are gone, so these are the literal old stored values).
        // minStored catches exactly that now-out-of-range index.
        { PID::ampTarget,  16, EnvTarget::None, 4 },
        { PID::mod1Target, 16, EnvTarget::None, 4 },
        { PID::mod2Target, 16, EnvTarget::None, 4 },
        { PID::lfo1Target, 15, LfoTarget::None, 4 },
        { PID::lfo2Target, 15, LfoTarget::None, 4 },
        { PID::lfo3Target, 15, LfoTarget::None, 4 },
    } };
    return table;
}

// One entry per parameter that stopped being a choice.
//   Epoch 9: the 15 envelope curve params. Built FROM PID::allEnvs rather than
//            written out, so a sixth envelope cannot arrive with an unmigrated
//            curve. bend = index·0.5 − 1.0 → Log(0)=-1 … Lin(2)=0 … Exp(4)=+1.
inline const std::array<ChoiceToValue, 3 * PID::kNumEnvs>& choiceToValues()
{
    static const auto table = []
    {
        std::array<ChoiceToValue, 3 * PID::kNumEnvs> t{};
        for (int i = 0; i < PID::kNumEnvs; ++i)
        {
            const auto& e = PID::allEnvs[i];
            t[static_cast<std::size_t>(3 * i + 0)] = { e.attackCurve,  0.5f, -1.0f, 9 };
            t[static_cast<std::size_t>(3 * i + 1)] = { e.decayCurve,   0.5f, -1.0f, 9 };
            t[static_cast<std::size_t>(3 * i + 2)] = { e.releaseCurve, 0.5f, -1.0f, 9 };
        }
        return t;
    }();
    return table;
}

// A mix control whose LAW changed, not just its full-scale. The remap is a
// closed-form inversion (FxMixLaw::migrate*Mix), so there is no factor here —
// only which sibling TYPE parameter selects the old wet path that applied.
struct MixLawRemap
{
    const char* id;         // mix parameter whose dry/wet law changed
    const char* condId;     // sibling type param selecting the old wet path
    int         sinceEpoch; // applied when the file's epoch < this
};

inline const std::array<MixLawRemap, 2>& mixLawRemaps()
{
    static const std::array<MixLawRemap, 2> table = { {
        { PID::delayMix,  PID::delayType,  5 },
        { PID::reverbMix, PID::reverbType, 5 },
    } };
    return table;
}

// Dispatch a stored mix value onto the new law. `typeIndex` is the sibling type
// param's stored index, read from the SAME file.
inline float remapMixValue(const char* id, float stored, int typeIndex)
{
    if (juce::String(id) == PID::delayMix)
        return FxMixLaw::migrateDelayMix(stored, typeIndex);
    if (juce::String(id) == PID::reverbMix)
        return FxMixLaw::migrateReverbMix(stored, typeIndex);
    return stored;
}

// .t5p JSON surface for the two mix controls: call with the value the file
// carried and the effect type resolved from the same record. Presence-aware by
// construction, like migrateScalar.
inline float migrateMixScalar(const char* id, float value, int fromEpoch, int typeIndex)
{
    if (fromEpoch >= kEpoch)
        return value;
    for (const auto& m : mixLawRemaps())
        if (fromEpoch < m.sinceEpoch && juce::String(m.id) == id)
            return remapMixValue(id, value, typeIndex);
    return value;
}

// Epoch 7: the filter resonance knob's LAW changed for the Ladder algorithm
// only. Same shape as MixLawRemap — a closed-form inversion
// (LadderResoLaw::migrateResonance) conditional on the sibling algorithm param,
// so there is no factor here.
struct ResoLawRemap
{
    const char* id;         // resonance parameter whose law changed
    const char* condId;     // sibling algorithm param selecting the affected filter
    const char* styleId;    // sibling warp-style param (the Warp's pole is per style)
    int         sinceEpoch; // applied when the file's epoch < this
};

inline const std::array<ResoLawRemap, 1>& resoLawRemaps()
{
    static const std::array<ResoLawRemap, 1> table = { {
        { PID::filterResonance, PID::filterAlgorithm, PID::filterWarpStyle, 7 },
    } };
    return table;
}

// The SVF's r → Q map did not change, so its stored value is already correct.
// The two ladders invert against their own pole — one number for the Ladder,
// one per saturation style for the Warp.
inline float remapResoValue(float stored, int algIndex, int warpStyle)
{
    if (algIndex == FilterAlgorithm::Ladder)
        return LadderResoLaw::migrateResonance(stored, LadderResoLaw::kLadderPole);
    if (algIndex == FilterAlgorithm::Warp)
        return LadderResoLaw::migrateResonance(stored, LadderResoLaw::warpPole(warpStyle));
    return stored;
}

// .t5p JSON surface for the resonance knob: call with the value the file carried
// and the algorithm + warp style resolved from the same record. A file with no
// algorithm property predates Ladder/Warp and is loaded as SVF — pass SVF, not
// "unknown"; likewise a Warp file with no style property is loaded as Tanh.
inline float migrateResoScalar(float value, int fromEpoch, int algIndex, int warpStyle)
{
    if (fromEpoch >= kEpoch)
        return value;
    for (const auto& r : resoLawRemaps())
        if (fromEpoch < r.sinceEpoch)
            return remapResoValue(value, algIndex, warpStyle);
    return value;
}

// Epoch 8: a cutoff-bus depth control whose LAW changed — linear onto ±4 octaves
// → ModCalib's curve onto ±10. Closed-form (ModCalib::migrateCutoffDepth), so
// there is no factor here, only which sibling TARGET param has to select Filter
// for the stored value to be a cutoff depth at all. `condId == nullptr` means
// unconditional: the AT cutoff amount is a dedicated cutoff depth with no target
// selector of its own.
struct CutoffLawRemap
{
    const char* id;         // depth/amount parameter whose law changed
    const char* condId;     // sibling target param, or nullptr if unconditional
    int         condValue;  // remap only when condId's selected index == this
    int         sinceEpoch; // applied when the file's epoch < this
};

inline const std::array<CutoffLawRemap, 12>& cutoffLawRemaps()
{
    static const std::array<CutoffLawRemap, 12> table = { {
        { PID::ampAmount,          PID::ampTarget,    EnvTarget::Filter,   8 },
        { PID::mod1Amount,         PID::mod1Target,   EnvTarget::Filter,   8 },
        { PID::mod2Amount,         PID::mod2Target,   EnvTarget::Filter,   8 },
        { PID::mod3Amount,         PID::mod3Target,   EnvTarget::Filter,   8 },
        { PID::mod4Amount,         PID::mod4Target,   EnvTarget::Filter,   8 },
        { PID::lfo1Depth,          PID::lfo1Target,   LfoTarget::Filter,   8 },
        { PID::lfo2Depth,          PID::lfo2Target,   LfoTarget::Filter,   8 },
        { PID::lfo3Depth,          PID::lfo3Target,   LfoTarget::Filter,   8 },
        { PID::drift1Depth,        PID::drift1Target, DriftLFO::TgtFilter, 8 },
        { PID::drift2Depth,        PID::drift2Target, DriftLFO::TgtFilter, 8 },
        { PID::drift3Depth,        PID::drift3Target, DriftLFO::TgtFilter, 8 },
        { PID::aftertouchAmtCutoff, nullptr,          0,                   8 },
    } };
    return table;
}

// Rescale a single stored scalar value for `id`. For load paths that apply
// values one at a time from non-PID-keyed storage (the .t5p JSON), wrap each
// stored value with this. Presence-aware BY CONSTRUCTION — it is only ever
// called for a value the file actually carried, so it can never rescale a stale
// live value left over from a previous patch. Range clamping happens when the
// returned value is applied to the param (setParam → convertTo0to1).
// Entries CHAIN in epoch order — a parameter can be recalibrated more than once
// in its life, and a file old enough predates all of them. The AT cutoff amount
// is the first such value: epoch 1 rescaled its full-scale, epoch 8 changed its
// law, and a pre-epoch-1 file has to travel both in that order. (This is why no
// loop below returns early on a match.)
inline float migrateScalar(const char* id, float value, int fromEpoch)
{
    if (fromEpoch >= kEpoch)
        return value;
    float v = value;
    for (const auto& r : rescales())
        if (fromEpoch < r.sinceEpoch && juce::String(r.id) == id)
            v *= r.factor;
    for (const auto& s : sidedResets())
        if (fromEpoch < s.sinceEpoch && juce::String(s.id) == id && v > s.above)
            v = s.toValue;
    for (const auto& c : cutoffLawRemaps())
        if (c.condId == nullptr && fromEpoch < c.sinceEpoch && juce::String(c.id) == id)
            v = ModCalib::migrateCutoffDepth(v);
    return v;
}

// A raw parameter value stored BY ID, for the surface that keeps no keys and no
// tree: the .t5evt event log, which records the DENORMALISED value straight off
// the APVTS listener and plays it back into the parameter it came from. Every
// unconditional entry migrateScalar applies, PLUS the choice→value remaps that
// migrateScalar must not carry (the .t5p JSON stores those shapes by key and
// would migrate twice). Without this, a tape recorded before epoch 9 replays a
// curve INDEX into the bend axis: a logged 2 (Lin) would arrive as +1 (Exp).
//
// DELIBERATE BOUNDARY, and it is older than this function: the target-CONDITIONAL
// entries are not applied. They need the sibling target's value at that point on
// the tape, which is its own event, not context available here — so an old tape
// still replays a filter-targeted env amount at its pre-epoch-2 meaning. That
// slips a depth; the curve remap above would have swapped a shape family.
inline float migrateLoggedValue(const juce::String& id, float value, int fromEpoch)
{
    if (fromEpoch >= kEpoch)
        return value;
    float v = migrateScalar(id.toRawUTF8(), value, fromEpoch);
    for (const auto& c : choiceToValues())
        if (fromEpoch < c.sinceEpoch && id == c.id)
            v = v * c.scale + c.offset;
    return v;
}

// Target-conditional variant: rescale only when the file's sibling target value
// (`targetValue`, already resolved by the caller from the same .t5p record) equals
// the entry's condValue. Presence-aware like migrateScalar.
inline float migrateScalarCond(const char* id, float value, int fromEpoch, int targetValue)
{
    if (fromEpoch >= kEpoch)
        return value;
    float v = value;
    // Chained, in epoch order — see migrateScalar. A filter-targeted env amount
    // written before epoch 2 takes that epoch's ×2.5 AND epoch 8's law remap.
    for (const auto& c : condRescales())
        if (fromEpoch < c.sinceEpoch && targetValue == c.condValue && juce::String(c.id) == id)
            v *= c.factor;
    for (const auto& c : cutoffLawRemaps())
        if (c.condId != nullptr && fromEpoch < c.sinceEpoch
            && targetValue == c.condValue && juce::String(c.id) == id)
            v = ModCalib::migrateCutoffDepth(v);
    return v;
}

// Rescale the matching PARAM nodes inside a stored APVTS ValueTree — the DAW
// state tree (before replaceState) and per-slot snapshot trees. Presence-aware:
// only nodes actually in the tree are touched. Values are clamped to the param's
// range when the tree is applied to the APVTS.
inline void migrateValueTree(juce::ValueTree& tree, int fromEpoch)
{
    if (! tree.isValid() || fromEpoch >= kEpoch)
        return;

    // Unconditional rescales.
    for (const auto& r : rescales())
    {
        if (fromEpoch >= r.sinceEpoch)
            continue;
        for (int i = 0; i < tree.getNumChildren(); ++i)
        {
            auto child = tree.getChild(i);
            if (child.getProperty("id").toString() == r.id && child.hasProperty("value"))
                child.setProperty("value",
                                  static_cast<float>(child.getProperty("value")) * r.factor,
                                  nullptr);
        }
    }

    // One-sided law resets: only the affected side is neutralised, the other
    // side's stored values keep their (unchanged) meaning.
    for (const auto& s : sidedResets())
    {
        if (fromEpoch >= s.sinceEpoch)
            continue;
        for (int i = 0; i < tree.getNumChildren(); ++i)
        {
            auto child = tree.getChild(i);
            if (child.getProperty("id").toString() == s.id && child.hasProperty("value")
                && static_cast<float>(child.getProperty("value")) > s.above)
                child.setProperty("value", s.toValue, nullptr);
        }
    }

    // Epoch 10: the Pitch row's "off". A PAIR of writes on two parameters, and
    // that is why it is here rather than an entry in one of the tables - none of
    // them can touch a sibling, and CondRescale, which comes closest, is applied
    // AFTER the index remaps below and would read a value this has already
    // changed.
    //
    // "off" used to mean "nothing MODULATES the pitch" while the per-note bend
    // played regardless, so it becomes X - the bend, as before. But the AMOUNT
    // bar was independently draggable in that window, and raised beside an "off"
    // source it did nothing at all. Carried across unchanged it would start
    // driving the pitch bus at kPitchModSemitones ON TOP of the bend the row now
    // switches on - a double bend performed by the migration rather than by the
    // player. So the inert amount goes with it. A row that stored a real Y or Z
    // routing is left alone: that one is deliberate, and it keeps working.
    if (fromEpoch < 10)
    {
        juce::ValueTree srcNode, amtNode;
        for (int i = 0; i < tree.getNumChildren(); ++i)
        {
            auto child = tree.getChild(i);
            const auto id = child.getProperty("id").toString();
            if (id == PID::exprSrcPitch)          srcNode = child;
            else if (id == PID::aftertouchAmtPitch) amtNode = child;
        }
        if (srcNode.isValid() && srcNode.hasProperty("value")
            && juce::roundToInt(static_cast<double>(srcNode.getProperty("value")))
                   >= ExprSource::None)
        {
            srcNode.setProperty("value", static_cast<float>(ExprSource::X), nullptr);
            if (amtNode.isValid() && amtNode.hasProperty("value"))
                amtNode.setProperty("value", 0.0f, nullptr);
        }
    }

    // Choice-index remaps: a stored index whose table entry was removed or
    // repurposed keeps its OLD meaning.
    for (const auto& m : indexRemaps())
    {
        if (fromEpoch >= m.sinceEpoch)
            continue;
        for (int i = 0; i < tree.getNumChildren(); ++i)
        {
            auto child = tree.getChild(i);
            if (child.getProperty("id").toString() == m.id && child.hasProperty("value")
                && juce::roundToInt(static_cast<double>(child.getProperty("value"))) >= m.minStored)
                child.setProperty("value", static_cast<float>(m.toIndex), nullptr);
        }
    }

    // Choice → continuous: a stored INDEX becomes the value it now names. No
    // other epoch touches these ids, so this runs order-independently.
    for (const auto& c : choiceToValues())
    {
        if (fromEpoch >= c.sinceEpoch)
            continue;
        for (int i = 0; i < tree.getNumChildren(); ++i)
        {
            auto child = tree.getChild(i);
            if (child.getProperty("id").toString() == c.id && child.hasProperty("value"))
                child.setProperty("value",
                                  static_cast<float>(child.getProperty("value")) * c.scale + c.offset,
                                  nullptr);
        }
    }

    // Target-conditional rescales: apply only when the sibling target param in the
    // same tree selects the matching destination (e.g. an env amount is only a
    // cutoff depth when that env targets Filter).
    for (const auto& c : condRescales())
    {
        if (fromEpoch >= c.sinceEpoch)
            continue;

        int targetIndex = -1;
        for (int i = 0; i < tree.getNumChildren(); ++i)
        {
            auto child = tree.getChild(i);
            if (child.getProperty("id").toString() == c.condId)
            {
                targetIndex = juce::roundToInt(static_cast<double>(child.getProperty("value")));
                break;
            }
        }
        if (targetIndex != c.condValue)
            continue;

        for (int i = 0; i < tree.getNumChildren(); ++i)
        {
            auto child = tree.getChild(i);
            if (child.getProperty("id").toString() == c.id && child.hasProperty("value"))
                child.setProperty("value",
                                  static_cast<float>(child.getProperty("value")) * c.factor,
                                  nullptr);
        }
    }

    // Cutoff-bus law remaps: the stored depth position moves to the one that
    // produces the same octave swing under the curve. Runs AFTER the rescales
    // above so a pre-epoch-2 file chains through both, in epoch order, exactly
    // as migrateScalar/migrateScalarCond do. A conditional entry needs its
    // sibling target to select Filter; a tree that carries the depth but not its
    // target is left alone rather than migrated against a guessed routing — an
    // absent target is unknown, not Filter.
    for (const auto& c : cutoffLawRemaps())
    {
        if (fromEpoch >= c.sinceEpoch)
            continue;

        if (c.condId != nullptr)
        {
            int targetIndex = -1;
            for (int i = 0; i < tree.getNumChildren(); ++i)
            {
                auto child = tree.getChild(i);
                if (child.getProperty("id").toString() == c.condId && child.hasProperty("value"))
                {
                    targetIndex = juce::roundToInt(static_cast<double>(child.getProperty("value")));
                    break;
                }
            }
            if (targetIndex != c.condValue)
                continue;
        }

        for (int i = 0; i < tree.getNumChildren(); ++i)
        {
            auto child = tree.getChild(i);
            if (child.getProperty("id").toString() == c.id && child.hasProperty("value"))
                child.setProperty("value",
                                  ModCalib::migrateCutoffDepth(
                                      static_cast<float>(child.getProperty("value"))),
                                  nullptr);
        }
    }

    // Mix-law remaps: the stored knob position moves to the one that reproduces
    // the same audible wet/dry balance under the new law. Which old wet path
    // applied is read from the sibling type param in the SAME tree; a tree that
    // carries the mix but not its type keeps the mix untouched rather than
    // guessing a type (an absent type is not "Off" — it is unknown).
    for (const auto& m : mixLawRemaps())
    {
        if (fromEpoch >= m.sinceEpoch)
            continue;

        int typeIndex = -1;
        for (int i = 0; i < tree.getNumChildren(); ++i)
        {
            auto child = tree.getChild(i);
            if (child.getProperty("id").toString() == m.condId && child.hasProperty("value"))
            {
                typeIndex = juce::roundToInt(static_cast<double>(child.getProperty("value")));
                break;
            }
        }
        if (typeIndex < 0)
            continue;

        for (int i = 0; i < tree.getNumChildren(); ++i)
        {
            auto child = tree.getChild(i);
            if (child.getProperty("id").toString() == m.id && child.hasProperty("value"))
                child.setProperty("value",
                                  remapMixValue(m.id,
                                                static_cast<float>(child.getProperty("value")),
                                                typeIndex),
                                  nullptr);
        }
    }

    // Resonance-law remap: only the two ladder algorithms changed law, so the
    // sibling algorithm param in the SAME tree decides — and for the Warp, the
    // sibling style too, because its pole is per style. A tree carrying the
    // resonance but no algorithm param predates Ladder/Warp — it was an SVF, and
    // the SVF's law is unchanged, so leaving it alone is correct (unlike the mix
    // case above, where an absent type really is unknown).
    for (const auto& rr : resoLawRemaps())
    {
        if (fromEpoch >= rr.sinceEpoch)
            continue;

        int algIndex  = FilterAlgorithm::SVF;
        int warpStyle = FilterWarpStyle::Tanh;
        for (int i = 0; i < tree.getNumChildren(); ++i)
        {
            auto child = tree.getChild(i);
            if (! child.hasProperty("value"))
                continue;
            const auto id = child.getProperty("id").toString();
            if (id == rr.condId)
                algIndex = juce::roundToInt(static_cast<double>(child.getProperty("value")));
            else if (id == rr.styleId)
                warpStyle = juce::roundToInt(static_cast<double>(child.getProperty("value")));
        }

        for (int i = 0; i < tree.getNumChildren(); ++i)
        {
            auto child = tree.getChild(i);
            if (child.getProperty("id").toString() == rr.id && child.hasProperty("value"))
                child.setProperty("value",
                                  remapResoValue(static_cast<float>(child.getProperty("value")),
                                                 algIndex, warpStyle),
                                  nullptr);
        }
    }
}
} // namespace Calibration
