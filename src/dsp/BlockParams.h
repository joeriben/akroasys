#pragma once

#include <algorithm>
#include <array>
#include <cmath>

// ── Choice-parameter single-source-of-truth tables ──
//
// Every AudioParameterChoice in T5ynth has its canonical entry list here.
// The `kEntries` arrays are consumed by:
//   - juce::AudioParameterChoice StringArrays in PluginProcessor.cpp
//   - juce::ComboBox::addItemList calls in gui/*.cpp
//   - preset save/load helpers in PluginProcessor.cpp (via the .key column)
//
// Each entry carries a `.key` column (stable snake_case identifier used for
// JSON serialization — never changes once shipped) and a `.label` column
// (user-facing display string — may be renamed without breaking presets).
// IMPORTANT: `.label` MUST be ASCII (bytes <= 127). Consumers build it via
// juce::String(const char*) / StringArray::add(const char*), which treat the
// pointer as ASCII-only — any UTF-8 glyph (arrows, quarter-note, multiply sign)
// turns into mojibake in the ComboBox AND the DAW automation list. Spell it out
// in ASCII (e.g. "A/B" not "A<arrow>B"), as the x/× and arrow labels learned.
// Both columns live in the same array at the same index, so adding a new
// choice means editing exactly one place. A `static_assert` below every
// table pins the enum's last value to `kCount - 1` to prevent drift.
//
// Note: the `.key` column is used by preset JSON save/load (Session 2
// onwards). In Session 1 the JSON helpers still use their legacy name
// tables — only the `.label` column is consumed. Keys are committed now
// so that Session 2's helper rewrite is a pure swap.

/** Common struct layout for every choice-parameter entry table. */
struct ChoiceEntry {
    const char* key;
    const char* label;
};

/** How many MOD envelopes there are — ENV 2..5 on the panel. ENV 1 is the amp
    envelope, which owns the VCA and keeps its own named parameters throughout.
    Everything else about a mod envelope is indexed by this, so a sixth one is
    this constant plus its parameter IDs. */
static constexpr int kNumModEnvs = 4;

// ── APVTS parameter-ID constants ──
// Every parameter has its canonical ID here; use PID::xxx everywhere
// instead of string literals to get compile-time typo detection.
namespace PID {
    static constexpr const char* oscScan          = "osc_scan";
    static constexpr const char* oscOctave        = "osc_octave";
    static constexpr const char* engineMode       = "engine_mode";
    static constexpr const char* voiceCount       = "voice_count";
    static constexpr const char* tuning           = "tuning";
    static constexpr const char* masterVol        = "master_vol";
    static constexpr const char* ampAttack        = "amp_attack";
    static constexpr const char* ampDecay         = "amp_decay";
    static constexpr const char* ampSustain       = "amp_sustain";
    static constexpr const char* ampRelease       = "amp_release";
    static constexpr const char* ampAmount        = "amp_amount";
    static constexpr const char* velAmt           = "vel_amt";   // global velocity→peak amount
    static constexpr const char* ampLoop          = "amp_loop";
    static constexpr const char* ampTarget        = "amp_target";
    static constexpr const char* ampAttackCurve   = "amp_attack_curve";
    static constexpr const char* ampDecayCurve    = "amp_decay_curve";
    static constexpr const char* ampReleaseCurve  = "amp_release_curve";
    static constexpr const char* ampAttackVelSens = "amp_attack_vel_sens";
    static constexpr const char* ampDecayVelSens  = "amp_decay_vel_sens";
    static constexpr const char* ampReleaseVelSens= "amp_release_vel_sens";
    static constexpr const char* mod1Attack       = "mod1_attack";
    static constexpr const char* mod1Decay        = "mod1_decay";
    static constexpr const char* mod1Sustain      = "mod1_sustain";
    static constexpr const char* mod1Release      = "mod1_release";
    static constexpr const char* mod1Amount       = "mod1_amount";
    static constexpr const char* mod1Loop         = "mod1_loop";
    static constexpr const char* mod1Target       = "mod1_target";
    static constexpr const char* mod1AttackCurve  = "mod1_attack_curve";
    static constexpr const char* mod1DecayCurve   = "mod1_decay_curve";
    static constexpr const char* mod1ReleaseCurve = "mod1_release_curve";
    static constexpr const char* mod1AttackVelSens = "mod1_attack_vel_sens";
    static constexpr const char* mod1DecayVelSens  = "mod1_decay_vel_sens";
    static constexpr const char* mod1ReleaseVelSens= "mod1_release_vel_sens";
    static constexpr const char* mod2Attack       = "mod2_attack";
    static constexpr const char* mod2Decay        = "mod2_decay";
    static constexpr const char* mod2Sustain      = "mod2_sustain";
    static constexpr const char* mod2Release      = "mod2_release";
    static constexpr const char* mod2Amount       = "mod2_amount";
    static constexpr const char* mod2Loop         = "mod2_loop";
    static constexpr const char* mod2Target       = "mod2_target";
    static constexpr const char* mod2AttackCurve  = "mod2_attack_curve";
    static constexpr const char* mod2DecayCurve   = "mod2_decay_curve";
    static constexpr const char* mod2ReleaseCurve = "mod2_release_curve";
    static constexpr const char* mod2AttackVelSens = "mod2_attack_vel_sens";
    static constexpr const char* mod2DecayVelSens  = "mod2_decay_vel_sens";
    static constexpr const char* mod2ReleaseVelSens= "mod2_release_vel_sens";
    static constexpr const char* mod3Attack       = "mod3_attack";
    static constexpr const char* mod3Decay        = "mod3_decay";
    static constexpr const char* mod3Sustain      = "mod3_sustain";
    static constexpr const char* mod3Release      = "mod3_release";
    static constexpr const char* mod3Amount       = "mod3_amount";
    static constexpr const char* mod3Loop         = "mod3_loop";
    static constexpr const char* mod3Target       = "mod3_target";
    static constexpr const char* mod3AttackCurve  = "mod3_attack_curve";
    static constexpr const char* mod3DecayCurve   = "mod3_decay_curve";
    static constexpr const char* mod3ReleaseCurve = "mod3_release_curve";
    static constexpr const char* mod3AttackVelSens = "mod3_attack_vel_sens";
    static constexpr const char* mod3DecayVelSens  = "mod3_decay_vel_sens";
    static constexpr const char* mod3ReleaseVelSens= "mod3_release_vel_sens";
    static constexpr const char* mod4Attack       = "mod4_attack";
    static constexpr const char* mod4Decay        = "mod4_decay";
    static constexpr const char* mod4Sustain      = "mod4_sustain";
    static constexpr const char* mod4Release      = "mod4_release";
    static constexpr const char* mod4Amount       = "mod4_amount";
    static constexpr const char* mod4Loop         = "mod4_loop";
    static constexpr const char* mod4Target       = "mod4_target";
    static constexpr const char* mod4AttackCurve  = "mod4_attack_curve";
    static constexpr const char* mod4DecayCurve   = "mod4_decay_curve";
    static constexpr const char* mod4ReleaseCurve = "mod4_release_curve";
    static constexpr const char* mod4AttackVelSens = "mod4_attack_vel_sens";
    static constexpr const char* mod4DecayVelSens  = "mod4_decay_vel_sens";
    static constexpr const char* mod4ReleaseVelSens= "mod4_release_vel_sens";

    // The four mod envelopes as one indexable table — ENV 2..5 on the panel.
    // The individual constants above stay: APVTS creation, the preset writer and
    // the LED map name them one at a time, and only the block-rate read and the
    // per-voice DSP want the index. Both halves refer to the same string, so a
    // typo cannot make them disagree.
    struct ModEnvIds {
        const char* attack;        const char* decay;
        const char* sustain;       const char* release;
        const char* amount;        const char* loop;
        const char* target;        const char* attackCurve;
        const char* decayCurve;    const char* releaseCurve;
        const char* attackVelSens; const char* decayVelSens;
        const char* releaseVelSens;

        // Every id of this envelope in one place, so anything that has to walk
        // a whole envelope (preset defaulting, copy/paste) cannot fall behind
        // the struct when a field is added.
        constexpr std::array<const char*, 13> all() const
        {
            return { attack, decay, sustain, release, amount, loop, target,
                     attackCurve, decayCurve, releaseCurve,
                     attackVelSens, decayVelSens, releaseVelSens };
        }
    };
    static_assert(sizeof(ModEnvIds) == 13 * sizeof(const char*),
                  "ModEnvIds gained a field - add it to all().");
    static constexpr ModEnvIds modEnv[] = {
        { mod1Attack, mod1Decay, mod1Sustain, mod1Release, mod1Amount, mod1Loop,
          mod1Target, mod1AttackCurve, mod1DecayCurve, mod1ReleaseCurve,
          mod1AttackVelSens, mod1DecayVelSens, mod1ReleaseVelSens },
        { mod2Attack, mod2Decay, mod2Sustain, mod2Release, mod2Amount, mod2Loop,
          mod2Target, mod2AttackCurve, mod2DecayCurve, mod2ReleaseCurve,
          mod2AttackVelSens, mod2DecayVelSens, mod2ReleaseVelSens },
        { mod3Attack, mod3Decay, mod3Sustain, mod3Release, mod3Amount, mod3Loop,
          mod3Target, mod3AttackCurve, mod3DecayCurve, mod3ReleaseCurve,
          mod3AttackVelSens, mod3DecayVelSens, mod3ReleaseVelSens },
        { mod4Attack, mod4Decay, mod4Sustain, mod4Release, mod4Amount, mod4Loop,
          mod4Target, mod4AttackCurve, mod4DecayCurve, mod4ReleaseCurve,
          mod4AttackVelSens, mod4DecayVelSens, mod4ReleaseVelSens }
    };
    static_assert(sizeof(modEnv) / sizeof(modEnv[0]) == kNumModEnvs,
                  "PID::modEnv table and kNumModEnvs are out of sync.");
    // The amp envelope wearing the same shape, and the five envelopes in tab
    // order, so anything that walks "an envelope" or "every envelope" has one
    // table to walk: the preset loader's kEnvPIDs IS this table, and the GUI
    // hands each envelope section its own row of it.
    static constexpr ModEnvIds ampEnv = {
        ampAttack, ampDecay, ampSustain, ampRelease, ampAmount, ampLoop,
        ampTarget, ampAttackCurve, ampDecayCurve, ampReleaseCurve,
        ampAttackVelSens, ampDecayVelSens, ampReleaseVelSens };
    static constexpr ModEnvIds allEnvs[] = {
        ampEnv, modEnv[0], modEnv[1], modEnv[2], modEnv[3] };
    static constexpr int kNumEnvs = 1 + kNumModEnvs;
    static_assert(sizeof(allEnvs) / sizeof(allEnvs[0]) == kNumEnvs,
                  "PID::allEnvs must carry the amp envelope plus every mod envelope.");
    static constexpr const char* lfo1Rate         = "lfo1_rate";
    static constexpr const char* lfo1Depth        = "lfo1_depth";
    static constexpr const char* lfo1Wave         = "lfo1_wave";
    static constexpr const char* lfo1Target       = "lfo1_target";
    static constexpr const char* lfo1Mode         = "lfo1_mode";
    static constexpr const char* lfo2Rate         = "lfo2_rate";
    static constexpr const char* lfo2Depth        = "lfo2_depth";
    static constexpr const char* lfo2Wave         = "lfo2_wave";
    static constexpr const char* lfo2Target       = "lfo2_target";
    static constexpr const char* lfo2Mode         = "lfo2_mode";
    static constexpr const char* lfo3Rate         = "lfo3_rate";
    static constexpr const char* lfo3Depth        = "lfo3_depth";
    static constexpr const char* lfo3Wave         = "lfo3_wave";
    static constexpr const char* lfo3Target       = "lfo3_target";
    static constexpr const char* lfo3Mode         = "lfo3_mode";
    // Per-target aftertouch amounts (bipolar, -1..+1). One float per
    // AftertouchTarget member 1..12; order mirrors the enum. Each target's own
    // amount is its signed depth: pressure x amount drives the target (sign sets
    // direction). (Superseded the old single-select aftertouch_target Choice +
    // global aftertouch_amount, retired with preset/DAW migration.)
    static constexpr const char* aftertouchAmtLfo1Depth   = "aftertouch_amt_lfo1_depth";
    static constexpr const char* aftertouchAmtLfo2Depth   = "aftertouch_amt_lfo2_depth";
    static constexpr const char* aftertouchAmtLfo3Depth   = "aftertouch_amt_lfo3_depth";
    static constexpr const char* aftertouchAmtEnv1Sustain = "aftertouch_amt_env1_sustain";
    static constexpr const char* aftertouchAmtEnv2Sustain = "aftertouch_amt_env2_sustain";
    static constexpr const char* aftertouchAmtEnv3Sustain = "aftertouch_amt_env3_sustain";
    static constexpr const char* aftertouchAmtEnv4Sustain = "aftertouch_amt_env4_sustain";
    static constexpr const char* aftertouchAmtEnv5Sustain = "aftertouch_amt_env5_sustain";
    static constexpr const char* aftertouchAmtCache       = "aftertouch_amt_cache";
    static constexpr const char* aftertouchAmtSnap        = "aftertouch_amt_snap";
    static constexpr const char* aftertouchAmtCutoff      = "aftertouch_amt_cutoff";
    static constexpr const char* aftertouchAmtResonance   = "aftertouch_amt_resonance";
    static constexpr const char* aftertouchAmtScan        = "aftertouch_amt_scan";
    static constexpr const char* aftertouchAmtDca         = "aftertouch_amt_dca";
    static constexpr const char* aftertouchAmtPitch       = "aftertouch_amt_pitch";
    static constexpr const char* aftertouchAmtNoiseLevel  = "aftertouch_amt_noise_level";
    // Per-target expression SOURCE (ExprSource below). One choice per target,
    // beside that target's amount: the amount says how deep, this says which of
    // the player's expression axes drives it -- or Ø, none of them, which is
    // where thirteen of the sixteen rows start (ExprSource::defaultFor).
    //
    // A preset or DAW session saved BEFORE these keys existed does not take that
    // default: its amounts were all written to mean pressure, so both loaders
    // ask whether the file knows the idea at all and fall back to
    // ExprSource::kLegacy when it does not (importJsonPreset,
    // setStateInformation, and MainPanel's snapshot slots).
    static constexpr const char* exprSrcLfo1Depth   = "expr_src_lfo1_depth";
    static constexpr const char* exprSrcLfo2Depth   = "expr_src_lfo2_depth";
    static constexpr const char* exprSrcLfo3Depth   = "expr_src_lfo3_depth";
    static constexpr const char* exprSrcEnv1Sustain = "expr_src_env1_sustain";
    static constexpr const char* exprSrcEnv2Sustain = "expr_src_env2_sustain";
    static constexpr const char* exprSrcEnv3Sustain = "expr_src_env3_sustain";
    static constexpr const char* exprSrcEnv4Sustain = "expr_src_env4_sustain";
    static constexpr const char* exprSrcEnv5Sustain = "expr_src_env5_sustain";
    static constexpr const char* exprSrcCache       = "expr_src_cache";
    static constexpr const char* exprSrcSnap        = "expr_src_snap";
    static constexpr const char* exprSrcCutoff      = "expr_src_cutoff";
    static constexpr const char* exprSrcResonance   = "expr_src_resonance";
    static constexpr const char* exprSrcScan        = "expr_src_scan";
    static constexpr const char* exprSrcDca         = "expr_src_dca";
    static constexpr const char* exprSrcPitch       = "expr_src_pitch";
    static constexpr const char* exprSrcNoiseLevel  = "expr_src_noise_level";
    static constexpr const char* driftEnabled     = "drift_enabled";
    static constexpr const char* driftRegen       = "drift_regen";
    static constexpr const char* driftCrossfade   = "drift_crossfade";
    static constexpr const char* cacheAsync       = "cache_async";
    static constexpr const char* drift1Rate       = "drift1_rate";
    static constexpr const char* drift1Depth      = "drift1_depth";
    static constexpr const char* drift1Target     = "drift1_target";
    static constexpr const char* drift1Wave       = "drift1_wave";
    static constexpr const char* drift2Rate       = "drift2_rate";
    static constexpr const char* drift2Depth      = "drift2_depth";
    static constexpr const char* drift2Target     = "drift2_target";
    static constexpr const char* drift2Wave       = "drift2_wave";
    static constexpr const char* drift3Rate       = "drift3_rate";
    static constexpr const char* drift3Depth      = "drift3_depth";
    static constexpr const char* drift3Target     = "drift3_target";
    static constexpr const char* drift3Wave       = "drift3_wave";
    static constexpr const char* filterEnabled    = "filter_enabled";
    static constexpr const char* filterType       = "filter_type";
    static constexpr const char* filterSlope      = "filter_slope";
    static constexpr const char* filterCutoff     = "filter_cutoff";
    static constexpr const char* filterResonance  = "filter_resonance";
    static constexpr const char* filterMix        = "filter_mix";
    static constexpr const char* filterKbdTrack   = "filter_kbd_track";
    static constexpr const char* filterDrive      = "filter_drive";
    static constexpr const char* filterDriveOs    = "filter_drive_os";
    static constexpr const char* filterAlgorithm  = "filter_algorithm";
    static constexpr const char* filterWarpStyle  = "filter_warp_style";
    static constexpr const char* delayType        = "delay_type";
    static constexpr const char* delayTime        = "delay_time";
    static constexpr const char* delayFeedback    = "delay_feedback";
    static constexpr const char* delayMix         = "delay_mix";
    static constexpr const char* delayDamp        = "delay_damp";
    // The amplifier chain (src/dsp/AmpEffects.h), added 2026-07-31. Every one of
    // these defaults to OFF -- 0 dB of drive, 0 depth, 0 mix -- because adding a
    // parameter to a shipping synth may not change a single existing preset.
    // Per-effect bypass for the amplifier chain. Delay and Reverb have OFF as a
    // value of their own type parameter; these four have no type, so their OFF
    // is a switch of its own. Default ON, which changes nothing about how a
    // patch sounds — an effect at mix/depth 0 was already silent — so a preset
    // written before these existed loads exactly as it did.
    static constexpr const char* fxDistOn         = "fx_dist_on";
    static constexpr const char* fxChorusOn       = "fx_chorus_on";
    static constexpr const char* fxPhaserOn       = "fx_phaser_on";
    static constexpr const char* fxTremOn         = "fx_trem_on";
    static constexpr const char* fxDistDrive      = "fx_dist_drive";
    static constexpr const char* fxDistMix        = "fx_dist_mix";
    static constexpr const char* fxTremRate       = "fx_trem_rate";
    static constexpr const char* fxTremDepth      = "fx_trem_depth";
    static constexpr const char* fxTremStereo     = "fx_trem_stereo";
    // Sine / Tri / Soft / Square, T5ynthTremolo::Wave order. Default Sine, which
    // is the shape the tremolo had before it could be chosen, so every patch
    // written before this sounds unchanged.
    static constexpr const char* fxTremWave       = "fx_trem_wave";
    static constexpr const char* fxChorusRate     = "fx_chorus_rate";
    static constexpr const char* fxChorusDepth    = "fx_chorus_depth";
    static constexpr const char* fxChorusMix      = "fx_chorus_mix";
    static constexpr const char* fxPhaserRate     = "fx_phaser_rate";
    static constexpr const char* fxPhaserDepth    = "fx_phaser_depth";
    static constexpr const char* fxPhaserFeedback = "fx_phaser_feedback";
    static constexpr const char* fxPhaserMix      = "fx_phaser_mix";
    static constexpr const char* reverbType       = "reverb_type";
    static constexpr const char* reverbMix        = "reverb_mix";
    static constexpr const char* algoRoom         = "algo_room";
    static constexpr const char* algoDamping      = "algo_damping";
    static constexpr const char* algoWidth        = "algo_width";
    static constexpr const char* limiterThresh    = "limiter_thresh";
    static constexpr const char* limiterRelease   = "limiter_release";
    static constexpr const char* genAlpha         = "gen_alpha";
    static constexpr const char* genMagnitude     = "gen_magnitude";
    static constexpr const char* genNoise         = "gen_noise";
    static constexpr const char* genAxesAmount    = "gen_axes_amount";
    static constexpr const char* genDuration      = "gen_duration";
    static constexpr const char* genStart         = "gen_start";
    static constexpr const char* genCfg           = "gen_cfg";
    static constexpr const char* genSeed          = "gen_seed";
    static constexpr const char* genHfBoost       = "gen_hf_boost";
    static constexpr const char* resynthAmount    = "resynth_amount";   // Resynth (init_audio / i2i): 0=off .. 1=full
    static constexpr const char* resynthSource   = "resynth_source";   // ResynthSource: 0=Internal (self-feedback), 1=External (live capture)
    // ── Semantic self-listening loop (CLAP ear → LLM interpreter → next prompt) ──
    // Both are read message-thread-only at generation time (PromptPanel), NOT in
    // processBlock — they have no audio-thread consumer.
    static constexpr const char* repromptStance       = "reprompt_stance";      // interpreter stance (Off disables the loop)
    static constexpr const char* repromptCoupling     = "reprompt_coupling";    // how far the machine displaces the human input
    // The DCO/Advanced panel's OWN stance parameter — deliberately separate from
    // repromptStance above (paradigm isolation, docs/DCO_REPROMPT_CONCEPT.md): the
    // neural loop's stance must never leak into the DCO panel and vice versa. The
    // two loops share only the interpreter (Qwen) and the stance system-prompt
    // TEXT (RepromptStances::stanceSystemPrompt), never state or a parameter.
    static constexpr const char* dcoRepromptStance    = "dco_reprompt_stance";  // DCO panel's own re-prompt stance
    static constexpr const char* infSteps         = "inf_steps";
    static constexpr const char* loopMode         = "loop_mode";
    static constexpr const char* crossfadeMs      = "crossfade_ms";
    static constexpr const char* normalize        = "normalize";
    static constexpr const char* loopOptimize     = "loop_optimize";
    // The LRO author's authority over the synth's own knobs. OFF = the
    // parameters are the player's alone; ON = an authored instrument may
    // also set the filter, envelopes, LFOs, drift and aftertouch it needs.
    static constexpr const char* lcoSetsParams    = "lco_sets_params";
    // The knobs a written LRO instrument gives the player — twelve fixed slots,
    // three parts of the instrument by four places each, every one 0..1. Fixed
    // and always present, because they are automation targets and preset
    // entries: a parameter that appears and disappears with the orchestra could
    // be neither. What varies is only what they are CALLED and whether the body
    // reads them, and that follows the library parameters the body kept
    // (backend/lco_write.py: read_controls/wire_controls). A body that keeps
    // none simply leaves all twelve unread at 0.5.
    static constexpr const char* lroP1a           = "lro_p1a";
    static constexpr const char* lroP1b           = "lro_p1b";
    static constexpr const char* lroP1c           = "lro_p1c";
    static constexpr const char* lroP1d           = "lro_p1d";
    static constexpr const char* lroP2a           = "lro_p2a";
    static constexpr const char* lroP2b           = "lro_p2b";
    static constexpr const char* lroP2c           = "lro_p2c";
    static constexpr const char* lroP2d           = "lro_p2d";
    static constexpr const char* lroP3a           = "lro_p3a";
    static constexpr const char* lroP3b           = "lro_p3b";
    static constexpr const char* lroP3c           = "lro_p3c";
    static constexpr const char* lroP3d           = "lro_p3d";
    // One level per PART — the column's own attenuator. The orchestra scaffold
    // has always named these ("osc1vol".."osc3vol"), the system prompt has
    // always told the author that layer N must be scaled by `kvolN`, and every
    // authored body does it; nothing ever wrote the channels, so all three sat
    // at the head's constant 1.0. Default 1.0 keeps that exactly.
    static constexpr const char* lroLvl1          = "lro_lvl1";
    static constexpr const char* lroLvl2          = "lro_lvl2";
    static constexpr const char* lroLvl3          = "lro_lvl3";
    static constexpr const char* noiseLevel       = "noise_level";
    static constexpr const char* noiseType        = "noise_type";
    static constexpr const char* wtFrames         = "wt_frames";
    static constexpr const char* wtSmooth         = "wt_smooth";
    static constexpr const char* wtAutoScan       = "wt_auto_scan";
    static constexpr const char* freezeTexture    = "freeze_texture";
    static constexpr const char* freezeStereo     = "freeze_stereo";
    static constexpr const char* seqMode          = "seq_mode";
    static constexpr const char* seqRunning       = "seq_running";
    static constexpr const char* seqBpm           = "seq_bpm";
    static constexpr const char* seqSteps         = "seq_steps";
    static constexpr const char* seqDivision      = "seq_division";
    static constexpr const char* seqGlideTime     = "seq_glide_time";
    static constexpr const char* seqGate          = "seq_gate";
    static constexpr const char* seqShuffle       = "seq_shuffle";
    static constexpr const char* seqOctave        = "seq_octave";
    static constexpr const char* seqPreset        = "seq_preset";
    static constexpr const char* arpMode          = "arp_mode";
    static constexpr const char* arpRate          = "arp_rate";
    static constexpr const char* arpOctaves       = "arp_octaves";
    static constexpr const char* genSeqRunning    = "gen_seq_running";
    static constexpr const char* genSteps         = "gen_steps";
    static constexpr const char* genPulses        = "gen_pulses";
    static constexpr const char* genRotation      = "gen_rotation";
    static constexpr const char* genMutation      = "gen_mutation";
    static constexpr const char* genRange         = "gen_range";
    static constexpr const char* genFixSteps      = "gen_fix_steps";
    static constexpr const char* genFixPulses     = "gen_fix_pulses";
    static constexpr const char* genFixRotation   = "gen_fix_rotation";
    static constexpr const char* genFixMutation   = "gen_fix_mutation";
    static constexpr const char* scaleRoot        = "scale_root";
    static constexpr const char* scaleType        = "scale_type";

    // ── Polyphonic generative sequencer — shared pitch field ──
    static constexpr const char* genFieldMode     = "gen_field_mode";
    static constexpr const char* genFieldRate     = "gen_field_rate";
    static constexpr const char* genFieldCenterPc = "gen_field_center_pc";
    static constexpr const char* genFieldPivot    = "gen_field_pivot";

    // ── Inter-strand coordination ──
    static constexpr const char* genCoordinationMode = "gen_coordination_mode";
    static constexpr const char* genCoordinationCap  = "gen_coordination_cap";

    // Strand 0 (shares existing gen_* Euclidean/fix params — this block adds role/texture)
    static constexpr const char* genRole          = "gen_role";
    static constexpr const char* genOctave        = "gen_octave";
    static constexpr const char* genDivMult       = "gen_div_mult";
    static constexpr const char* genDominance     = "gen_dominance";

    // Strand 2
    static constexpr const char* gen2Enable       = "gen2_enable";
    static constexpr const char* gen2Role         = "gen2_role";
    static constexpr const char* gen2Octave       = "gen2_octave";
    static constexpr const char* gen2DivMult      = "gen2_div_mult";
    static constexpr const char* gen2Dominance    = "gen2_dominance";
    static constexpr const char* gen2Steps        = "gen2_steps";
    static constexpr const char* gen2Pulses       = "gen2_pulses";
    static constexpr const char* gen2Rotation     = "gen2_rotation";
    static constexpr const char* gen2Mutation     = "gen2_mutation";
    static constexpr const char* gen2FixSteps     = "gen2_fix_steps";
    static constexpr const char* gen2FixPulses    = "gen2_fix_pulses";
    static constexpr const char* gen2FixRotation  = "gen2_fix_rotation";
    static constexpr const char* gen2FixMutation  = "gen2_fix_mutation";

    // Strand 3
    static constexpr const char* gen3Enable       = "gen3_enable";
    static constexpr const char* gen3Role         = "gen3_role";
    static constexpr const char* gen3Octave       = "gen3_octave";
    static constexpr const char* gen3DivMult      = "gen3_div_mult";
    static constexpr const char* gen3Dominance    = "gen3_dominance";
    static constexpr const char* gen3Steps        = "gen3_steps";
    static constexpr const char* gen3Pulses       = "gen3_pulses";
    static constexpr const char* gen3Rotation     = "gen3_rotation";
    static constexpr const char* gen3Mutation     = "gen3_mutation";
    static constexpr const char* gen3FixSteps     = "gen3_fix_steps";
    static constexpr const char* gen3FixPulses    = "gen3_fix_pulses";
    static constexpr const char* gen3FixRotation  = "gen3_fix_rotation";
    static constexpr const char* gen3FixMutation  = "gen3_fix_mutation";

    // Strand 4
    static constexpr const char* gen4Enable       = "gen4_enable";
    static constexpr const char* gen4Role         = "gen4_role";
    static constexpr const char* gen4Octave       = "gen4_octave";
    static constexpr const char* gen4DivMult      = "gen4_div_mult";
    static constexpr const char* gen4Dominance    = "gen4_dominance";
    static constexpr const char* gen4Steps        = "gen4_steps";
    static constexpr const char* gen4Pulses       = "gen4_pulses";
    static constexpr const char* gen4Rotation     = "gen4_rotation";
    static constexpr const char* gen4Mutation     = "gen4_mutation";
    static constexpr const char* gen4FixSteps     = "gen4_fix_steps";
    static constexpr const char* gen4FixPulses    = "gen4_fix_pulses";
    static constexpr const char* gen4FixRotation  = "gen4_fix_rotation";
    static constexpr const char* gen4FixMutation  = "gen4_fix_mutation";

    // Strand 5
    static constexpr const char* gen5Enable       = "gen5_enable";
    static constexpr const char* gen5Role         = "gen5_role";
    static constexpr const char* gen5Octave       = "gen5_octave";
    static constexpr const char* gen5DivMult      = "gen5_div_mult";
    static constexpr const char* gen5Dominance    = "gen5_dominance";
    static constexpr const char* gen5Steps        = "gen5_steps";
    static constexpr const char* gen5Pulses       = "gen5_pulses";
    static constexpr const char* gen5Rotation     = "gen5_rotation";
    static constexpr const char* gen5Mutation     = "gen5_mutation";
    static constexpr const char* gen5FixSteps     = "gen5_fix_steps";
    static constexpr const char* gen5FixPulses    = "gen5_fix_pulses";
    static constexpr const char* gen5FixRotation  = "gen5_fix_rotation";
    static constexpr const char* gen5FixMutation  = "gen5_fix_mutation";

    // ── BPM-sync per LFO/Drift/Delay ──
    static constexpr const char* lfo1ClockMode      = "lfo1_clock_mode";
    static constexpr const char* lfo1ClockDivision  = "lfo1_clock_division";
    static constexpr const char* lfo2ClockMode      = "lfo2_clock_mode";
    static constexpr const char* lfo2ClockDivision  = "lfo2_clock_division";
    static constexpr const char* lfo3ClockMode      = "lfo3_clock_mode";
    static constexpr const char* lfo3ClockDivision  = "lfo3_clock_division";
    static constexpr const char* drift1ClockMode    = "drift1_clock_mode";
    static constexpr const char* drift1ClockDivision= "drift1_clock_division";
    static constexpr const char* drift2ClockMode    = "drift2_clock_mode";
    static constexpr const char* drift2ClockDivision= "drift2_clock_division";
    static constexpr const char* drift3ClockMode    = "drift3_clock_mode";
    static constexpr const char* drift3ClockDivision= "drift3_clock_division";
    static constexpr const char* delayClockMode     = "delay_clock_mode";
    static constexpr const char* delayClockDivision = "delay_clock_division";
}

// ── Modulation envelope targets ──
// ── Modulation full-scale calibration (destination-owned) ──────────────────────
// Each modulation DESTINATION owns the single full-scale that converts a summed,
// NORMALIZED modulation contribution into the real parameter change. Every source
// (env, LFO, Drift, aftertouch, MPE timbre) feeds a normalized amount into the
// SAME constant — so the feel is calibrated here, once per destination, never
// per-source. ±1 of summed contribution maps to the full-scale below.
namespace ModCalib {
    static constexpr float kCutoffModOctaves  = 10.0f; // cutoff: full depth → ±10 octaves (the whole 20 Hz–20 kHz span)
    static constexpr float kPitchModSemitones = 12.0f; // pitch:  ±1 summed → ±12 semitones (±1 octave)

    // ── The cutoff bus's DEPTH law ──────────────────────────────────────────
    // The full-scale above is reached along a curve, not linearly: a depth knob
    // at position a spends |a|^kCutoffModCurve of it. The exponent is fixed by
    // one requirement — the ±4 octaves that WERE the whole range must still sit
    // where the hand expects them, at about two thirds of the travel:
    //   a = 0.25 → 0.4 oct | a = 0.5 → 2.0 oct | a = 0.67 → 4.0 oct | a = 1 → 10 oct
    // (0.5 lands within 0.03 oct of the retired linear ±4 law, so the whole
    // lower half of every cutoff depth control keeps the feel it had.)
    //
    // Why a curve rather than the ±10 full-scale the law had before epoch 2: at
    // ±10 LINEAR the musical band sat in the bottom tenth of the travel, which
    // is what the ±4 ceiling was introduced to fix — at the price of the range
    // itself. No filter envelope could open a filter from a low base, and any
    // preset that used to sweep wider was clamped on load. The curve buys the
    // controllability without paying in reach.
    static constexpr float kCutoffModCurve = 2.3f;

    // A source hands the bus its shape ALREADY scaled by its own depth knob
    // (env level = contour·amount, LFO = wave·depth), so the factor that turns
    // that linear product into the curved one is |a|^(curve−1): the product is
    // shape·sign(a)·|a|^curve. Only the DEPTH travels the curve — the contour
    // the source sends stays linear in octaves, so a drawn envelope and an LFO
    // waveform still reach the filter with the shape they have.
    inline float cutoffDepthCurve (float a) noexcept
    {
        const float m = std::abs(a);
        return m > 0.0f ? std::pow(m, kCutoffModCurve - 1.0f) : 0.0f;
    }

    // ── Migration (calibration epoch 8) ──
    // The retired law was linear onto ±4 octaves, so a stored position a meant
    // 4·a octaves; the position producing the SAME octaves under the curve is
    // (4·a / kCutoffModOctaves)^(1/curve). Sign-preserving — the aftertouch
    // cutoff amount is bipolar. A stored 1.0 lands at 0.672, so nothing clamps;
    // and a pre-epoch-2 value that chains through the ×2.5 of that epoch lands
    // at exactly 1.0 for the old full-scale, because the old ±10 and the new
    // ±10 are the same ten octaves. Presets clamped by epoch 2 get their sweep
    // back — which is the point of this epoch, not a side effect of it.
    inline float migrateCutoffDepth (float a) noexcept
    {
        const float m = std::abs(a);
        if (m <= 0.0f)
            return a;
        const float moved = std::pow(m * 4.0f / kCutoffModOctaves, 1.0f / kCutoffModCurve);
        return a < 0.0f ? -moved : moved;
    }
}

// ── Engine level calibration ────────────────────────────────────────────────
//
// The four engines were never matched to one another. Measured on the real
// processor with `tools/measure_engine_levels.cpp` — filter OFF, one amp
// envelope holding at sustain, no mod envelope, no LFO, no drift, no effects,
// velocity taken out — a single note leaves the voice chain at:
//
//     Wavetable  0.931   |   Sampler  0.358   |   LRO  0.278   |   Granular  0.193
//
// which is a spread of 13.7 dB between the loudest and the quietest engine.
// Switching engines changed the loudness of the instrument by more than a
// fader move, and nothing in the signal path corrected for it.
//
// WHY THE LRO IS THE ANCHOR, at ×1.0 and therefore bit-identical. Its level is
// not a property of a playback path — it is set body by body in
// `backend/lco_library.json`, on RMS, tuned by ear against neighbouring
// entries. That tuning is the one level convention in the instrument that
// somebody listened to, so it is the one the others move to. The other three
// play back a peak-normalised buffer and have no such convention.
//
// A caveat the numbers do not carry on their own: the LRO row was measured on
// the BUILT-IN fallback orchestra, not on an LLM-authored body. It is the
// reference point, not a guarantee about every body the author writes.
//
// These are RELATIVE, and they are applied AT THE VCA (SynthVoice.cpp), i.e.
// at the end of the voice chain, so that they move the level and touch neither
// the noise balance nor the drive into the saturating stages. The absolute
// level is `outputGainForThreshold` in PluginProcessor.cpp; the two are set
// together, and the engine that grows fastest with polyphony is what fixes it.
namespace EngineCalib {
    static constexpr float kSampler   = 0.7765f;  // 0.278 / 0.358
    static constexpr float kWavetable = 0.2986f;  // 0.278 / 0.931
    static constexpr float kFreeze    = 1.4404f;  // 0.278 / 0.193  (Granular)
    // THE SAMPLER'S OWN TRIM, deliberately the identical number and not a second
    // one tuned to match it. CsoundEngine::prepare() now normalises each compiled
    // orchestra to the same BUFFER level SamplePlayer normalises a sample to
    // (CsoundEngine::kLevelPeakCeilingDb == SamplePlayer's kNormalizeCeilingDb),
    // so by the time the two engines reach this line they are handing the VCA the
    // same kind of source and must take the same trim. Sharing the constant is
    // what makes them track each other when either side is retuned; two separately
    // fitted numbers would drift apart silently.
    //
    // It was 1.0f before, commented "the anchor", and that was the defect. Nothing
    // had ever measured an AUTHORED body — the number came from the built-in
    // placeholder orchestra, which is not an instrument anyone plays, and the
    // sampler it was matched against was a synthetic saw (tools/
    // measure_engine_levels.cpp) quiet enough to take SamplePlayer's SUSTAINED
    // normalise branch where every real T5-oscillator render, peaking at ~1.0,
    // takes the PEAK-CAP one. Both halves of that comparison were unrepresentative
    // in the same direction, so a table that reads as level-matched shipped the LRO
    // 12.4 dB rms / 14.3 dB peak under the engine a player A/Bs it against
    // (BJ, measured off his own recording 2026-08-05).
    //
    // Guarded by tools/audition_lro_level_match.cpp, which asserts the DELIVERED
    // level — this trim included — against the sampler's, because a check written
    // against the LRO's own target constant would certify any value of it.
    static constexpr float kCsound    = kSampler;
}

namespace EnvTarget {
    // Keep the shared target order aligned with LfoTarget: Filter, Scan,
    // Pitch, Delay, Reverb, Noise. Env-only targets stay in their own blocks.
    enum : int {
        None = 0,
        DCA = 1,
        Filter = 2,
        Scan = 3,
        Pitch = 4,
        DelayTime = 5,
        DelayFB = 6,
        DelayMix = 7,
        ReverbMix = 8,
        NoiseLevel = 9,
        LFO1Rate = 10,
        LFO1Depth = 11,
        LFO2Rate = 12,
        LFO2Depth = 13,
        LFO3Rate = 14,
        LFO3Depth = 15
    };
    static constexpr ChoiceEntry kEntries[] = {
        { "none",       "---"        },
        { "dca",        "DCA"        },
        { "filter",     "Filter"     },
        { "scan",       "Scan"       },
        { "pitch",      "Pitch"      },
        { "delay_time", "Dly Time"   },
        { "delay_fb",   "Dly FB"     },
        { "delay_mix",  "Dly Mix"    },
        { "reverb_mix", "Rev Mix"    },
        { "noise_level","Noise Lvl"  },
        { "lfo1_rate",  "LFO1 Rate"  },
        { "lfo1_depth", "LFO1 Amt" },
        { "lfo2_rate",  "LFO2 Rate"  },
        { "lfo2_depth", "LFO2 Amt" },
        { "lfo3_rate",  "LFO3 Rate"  },
        { "lfo3_depth", "LFO3 Amt" }
    };
    static constexpr int kCount = sizeof(kEntries) / sizeof(kEntries[0]);
    static_assert(LFO3Depth + 1 == kCount,
                  "EnvTarget enum and kEntries are out of sync.");

    /** Does this target live OUTSIDE the voice — i.e. does an envelope pointed
        at it keep mattering after the voice itself has gone silent?

        The delay, the reverb and the LFO rates/depths are processor-wide: the
        block-rate ghost reads them off the NEWEST ACTIVE voice
        (VoiceManager.cpp, `out.lastAmpVal`), so the voice has to stay alive for
        the envelope to keep driving them. DCA, Filter, Scan, Pitch and Noise
        are the voice's own — they die with it and cannot outlast it.

        SynthVoice's voice-free test asks this: a voice whose LEVEL is finished
        may still be needed as a modulation source, and only when neither holds
        may the slot be released. */
    inline constexpr bool isOutsideTheVoice (int target)
    {
        return target == DelayTime || target == DelayFB || target == DelayMix
            || target == ReverbMix
            || target == LFO1Rate || target == LFO1Depth
            || target == LFO2Rate || target == LFO2Depth
            || target == LFO3Rate || target == LFO3Depth;
    }
    static_assert(! isOutsideTheVoice(None) && ! isOutsideTheVoice(DCA)
                  && ! isOutsideTheVoice(Filter) && ! isOutsideTheVoice(Scan)
                  && ! isOutsideTheVoice(Pitch) && ! isOutsideTheVoice(NoiseLevel)
                  && isOutsideTheVoice(DelayTime) && isOutsideTheVoice(LFO3Depth),
                  "isOutsideTheVoice and the EnvTarget enum have drifted apart.");
}

// ── LFO targets ──
namespace LfoTarget {
    enum : int {
        None = 0,
        Filter = 1,
        Scan = 2,
        Pitch = 3,
        DelayTime = 4,
        DelayFB = 5,
        DelayMix = 6,
        ReverbMix = 7,
        NoiseLevel = 8,
        Env1Amt = 9,
        Env2Amt = 10,
        Env3Amt = 11,
        Drift1Depth = 12,
        Drift2Depth = 13,
        Drift3Depth = 14,
        // ENV 4/5 are APPENDED, not inserted next to ENV1-3, because a DAW
        // session stores this choice as its INDEX (APVTS state), not as the
        // string id a .t5p carries — inserting would silently turn every saved
        // "Drift1 Amt" into something else. The dropdown reads out of order; a
        // session saved before they existed still restores what it meant.
        Env4Amt = 15,
        Env5Amt = 16
    };
    static constexpr ChoiceEntry kEntries[] = {
        { "none",       "---"       },
        { "filter",     "Filter"    },
        { "scan",       "Scan"      },
        { "pitch",      "Pitch"     },
        { "delay_time", "Dly Time"  },
        { "delay_fb",   "Dly FB"    },
        { "delay_mix",  "Dly Mix"   },
        { "reverb_mix", "Rev Mix"   },
        { "noise_level","Noise Lvl" },
        { "env1_amt",   "ENV1 Amt"  },
        { "env2_amt",   "ENV2 Amt"  },
        { "env3_amt",   "ENV3 Amt"  },
        { "drift1_depth","Drift1 Amt" },
        { "drift2_depth","Drift2 Amt" },
        { "drift3_depth","Drift3 Amt" },
        { "env4_amt",   "ENV4 Amt"  },
        { "env5_amt",   "ENV5 Amt"  }
    };
    static constexpr int kCount = sizeof(kEntries) / sizeof(kEntries[0]);
    static_assert(Env5Amt + 1 == kCount,
                  "LfoTarget enum and kEntries are out of sync.");

    /** The target that drives mod envelope `i`'s Amt — i.e. ENV (i+2), because
        ENV1 is the amp envelope. NOT `Env2Amt + i`: ENV4/5 had to be appended
        after the drift depths to keep DAW sessions readable, so the run is
        broken and this table is the only safe way across it. */
    inline constexpr int modEnvAmt (int i)
    {
        constexpr int t[] = { Env2Amt, Env3Amt, Env4Amt, Env5Amt };
        return t[i];
    }
    static_assert(kNumModEnvs == 4,
                  "LfoTarget::modEnvAmt lists 4 entries -- add one per new mod envelope.");
}

// ── MIDI aftertouch performance targets ──
// All aftertouch targets are applied per-voice (poly): each voice modulates
// from its own resolved pressure (max of channel pressure, poly key pressure,
// mod wheel, breath — see VoiceManager::pressureForNote), so per-note poly
// aftertouch and channel-wide pressure both drive the target. Scan is the
// granular/wavetable read position — expressive under key pressure.
namespace AftertouchTarget {
    enum : int {
        None = 0,
        LFO1Depth = 1,
        LFO2Depth = 2,
        LFO3Depth = 3,
        Env1Sustain = 4,
        Env2Sustain = 5,
        Env3Sustain = 6,
        Cutoff = 7,
        Resonance = 8,
        Scan = 9,
        DCA = 10,
        Pitch = 11,
        NoiseLevel = 12,
        // Appended for the same reason as LfoTarget::Env4Amt above: a DAW
        // session stores the choice INDEX, so inserting these beside ENV1-3
        // would re-point every saved Cutoff/Resonance/Scan/DCA/Pitch setting.
        Env4Sustain = 13,
        Env5Sustain = 14,
        // The two that do not modulate a voice: they MOVE THE INSTRUMENT, to
        // another cached sample or another snapshot. Everything above is a
        // continuous depth SynthVoice reads per block; these two resolve to a
        // position and hand it to the message thread, the same way the hardware
        // controller's snapshot buttons already do. From the player's side that
        // difference does not exist - it is a bar in this module like the others,
        // and pressure moves it. Appended, never inserted: a DAW session stores
        // the choice INDEX.
        Cache = 15,
        Snap = 16
    };
    static constexpr ChoiceEntry kEntries[] = {
        { "none",         "---"          },
        { "lfo1_depth",   "LFO1 Amt"   },
        { "lfo2_depth",   "LFO2 Amt"   },
        { "lfo3_depth",   "LFO3 Amt"   },
        { "env1_sustain", "ENV1 Sustain" },
        { "env2_sustain", "ENV2 Sustain" },
        { "env3_sustain", "ENV3 Sustain" },
        { "cutoff",       "Cutoff"       },
        { "resonance",    "Resonance"    },
        { "scan",         "Scan"         },
        { "dca",          "DCA"          },
        { "pitch",        "Pitch"        },
        { "noise_level",  "Noise"        },
        { "env4_sustain", "ENV4 Sustain" },
        { "env5_sustain", "ENV5 Sustain" },
        { "cache",        "Cache"        },
        { "snap",         "Snap"         }
    };
    static constexpr int kCount = sizeof(kEntries) / sizeof(kEntries[0]);
    static_assert(Snap + 1 == kCount,
                  "AftertouchTarget enum and kEntries are out of sync.");

    /** True for the targets that move the instrument instead of modulating a
     *  voice. SynthVoice must never read these: there is no per-voice depth
     *  behind them, and aftertouchDrive would hand it one. */
    constexpr bool movesTheInstrument(int target)
    {
        return target == Cache || target == Snap;
    }

    /** The target that holds mod envelope `i`'s sustain — ENV (i+2), because ENV1
        is the amp envelope. A table for the same reason as LfoTarget::modEnvAmt:
        ENV4/5 sit after the non-envelope targets, so the run is broken. */
    inline constexpr int modEnvSustain (int i)
    {
        constexpr int t[] = { Env2Sustain, Env3Sustain, Env4Sustain, Env5Sustain };
        return t[i];
    }
    static_assert(kNumModEnvs == 4,
                  "modEnvSustain lists 4 entries -- add one per new mod envelope.");
}

// ── Expression sources: the player's four continuous axes ──
//
// Every target in AftertouchTarget above owns an AMOUNT (how deep) and a SOURCE
// (which axis drives it). Before this existed the source was always pressure,
// hard-coded; MPE gives a key three axes and a note-on gives a fourth, and there
// is no reason the filter has to be the pressure's and only the pressure's.
//
// The letters are the player's, not MIDI's: X is the sideways/bend axis, Y the
// second continuous axis a key reports (CC74), Z the pressure. V is velocity,
// which is not continuous at all -- it is fixed at note-on and holds for the
// note's life. That difference is real and deliberate: V offsets a target for
// the whole note, the other three move it while the finger moves.
//
// ORDER IS FROZEN. A DAW session stores a Choice as its INDEX, so inserting a
// source here would re-point every saved routing. Append only.
//
// There are two different "defaults" below and they must not be confused.
// kLegacy is Z, and it is only ever what a FILE that predates sources means:
// "amount alone" meant pressure, so such a preset restores to exactly what it
// did. defaultFor is what a FRESH patch starts on, and for most rows that is
// None -- nothing drives it until the player says what should.
/** Below this an aftertouch AMOUNT is off. Half of the control's 0.01 step, so
 *  it can only ever swallow a value the player cannot set -- and it has one job:
 *  a bar at rest does not hold 0.0f. juce::Slider snaps through
 *  NormalisableRange<float>(-1, 1, 0.01), which computes zero as
 *  -1.0f + 0.01f * 100.0f = -2.235e-08. Measured. An exact `!= 0.0f` therefore
 *  calls every untouched row active, and `< 0.0` calls it negative. */
static constexpr float kAftertouchAmtEpsilon = 0.005f;

namespace ExprSource {
    enum : int {
        Velocity = 0,   // V -- note-on velocity, constant for the note
        X        = 1,   // per-note pitch bend, normalised by the bend range in force
        Y        = 2,   // CC74 as TRAVEL from the value in force at note-on
        Z        = 3,   // pressure (channel/poly/mod wheel/breath), the old behaviour
        // Nothing drives this target. Appended, not inserted at 0, because a DAW
        // session stores a Choice as its INDEX and index 0 is Velocity in every
        // session already saved. It is the resting state of the column: most
        // targets start here, and the switch draws it as the only grey one, so
        // what is WIRED is what shows.
        None     = 4
    };
    static constexpr ChoiceEntry kEntries[] = {
        { "velocity", "V"   },
        { "x",        "X"   },
        { "y",        "Y"   },
        { "z",        "Z"   },
        // ASCII label on purpose: this string goes into the DAW's automation
        // lane through AudioParameterChoice, and the empty-set glyph the switch
        // paints is drawn from a UTF-8 literal there (see ExprSourceSwitch).
        { "off",      "off" }
    };
    static constexpr int kCount = sizeof(kEntries) / sizeof(kEntries[0]);
    static_assert(None + 1 == kCount, "ExprSource enum and kEntries are out of sync.");

    /** The source a file that PREDATES sources means. Not the fresh-patch
     *  default -- that is defaultFor below, and for most rows it is None. This
     *  one exists for exactly one job: a .t5p written when an amount could only
     *  mean pressure restores as pressure. */
    static constexpr int kLegacy = Z;

    /** Which axis a target starts on. Three rows are wired and the rest are
     *  None, because a fresh patch should say what it is rather than arm
     *  fourteen depths at once -- and the column then reads at a glance: three
     *  lit boxes, thirteen grey.
     *
     *  The three are not a preference dressed as a default. Z into the DCA is
     *  MPE's own core gesture, press harder and it is louder. Y is the
     *  brightness axis -- MPE names CC74 "Timbre", and until sources existed
     *  this synth wired CC74 straight into the cutoff, so Cutoff keeps that
     *  wiring, now as a routing the player can move. Scan is there for the same
     *  reason and not by analogy: on the wavetable and granular engines the read
     *  position IS where the timbre comes from, so it is the second thing a
     *  finger sliding up the key should reach.
     *
     *  Pitch is deliberately NOT on X: X is already applied to it as the
     *  per-note bend, and routing it here again would bend it twice. */
    inline constexpr int defaultFor (int target)
    {
        switch (target)
        {
            case AftertouchTarget::DCA:    return Z;
            case AftertouchTarget::Cutoff: return Y;
            case AftertouchTarget::Scan:   return Y;
            default:                       return None;
        }
    }

    /** The per-target table above, materialised. Written out rather than
     *  value-initialised because no default is 0 -- a zeroed table would
     *  silently route every target to VELOCITY. */
    inline constexpr std::array<int, AftertouchTarget::kCount> defaults()
    {
        std::array<int, AftertouchTarget::kCount> a {};
        for (int t = 0; t < AftertouchTarget::kCount; ++t)
            a[(std::size_t) t] = defaultFor(t);
        return a;
    }
}

// ── Drift LFO targets ──
namespace DriftTarget {
    // Generation-side ("T5Osc") targets — alpha, noise, magnitude, the three
    // semantic axes, and resynth — are grouped first; the synth DSP targets
    // (scan, filter, … envelopes) follow. Resynth sits with the generation
    // group (right after Axis3) because it drives a re-inference, not in-process
    // DSP. kEntries order must mirror this; .t5p presets key off the string id,
    // so the order is free to change without breaking presets.
    enum : int {
        None = 0,
        Alpha = 1,
        Noise = 2,
        Magnitude = 3,
        Axis1 = 4,
        Axis2 = 5,
        Axis3 = 6,
        Resynth = 7,
        WtScan = 8,
        Filter = 9,
        Pitch = 10,
        DelayTime = 11,
        DelayFB = 12,
        DelayMix = 13,
        ReverbMix = 14,
        Env1Amt = 15,
        Env2Amt = 16,
        Env3Amt = 17,
        Env4Amt = 18,
        Env5Amt = 19
    };
    static constexpr ChoiceEntry kEntries[] = {
        { "none",       "---"        },
        { "alpha",      "A/B"        },
        { "noise",      "Emb. Noise" },
        { "magnitude",  "Magnitude"  },
        { "axis_1",     "Axis 1"     },
        { "axis_2",     "Axis 2"     },
        { "axis_3",     "Axis 3"     },
        { "resynth",    "Resynth"    },
        { "wt_scan",    "Scan"       },
        { "filter",     "Filter"     },
        { "pitch",      "Pitch"      },
        { "delay_time", "Dly Time"   },
        { "delay_fb",   "Dly FB"     },
        { "delay_mix",  "Dly Mix"    },
        { "reverb_mix", "Rev Mix"    },
        { "env1_amt",   "ENV1 Amt"   },
        { "env2_amt",   "ENV2 Amt"   },
        { "env3_amt",   "ENV3 Amt"   },
        { "env4_amt",   "ENV4 Amt"   },
        { "env5_amt",   "ENV5 Amt"   }
    };
    static constexpr int kCount = sizeof(kEntries) / sizeof(kEntries[0]);
    static_assert(Env5Amt + 1 == kCount,
                  "DriftTarget enum and kEntries are out of sync.");
}

// ── Engine mode ──
namespace EngineMode {
    // Lco: a DCO/LCO bake (prompt -> lexicon recipe -> baked wavetable +
    // optional real-time additive bank on the second oscillator). Distinct
    // from Wavetable so presets round-trip their prompt/readings/frames
    // instead of silently degrading to a generic neural wavetable on save
    // (docs/PRESET_FORMAT.md format v5). Maps to the SAME SynthVoice::EngineMode
    // Wavetable DSP path — SynthVoice's own enum stays 3-valued; only this
    // BlockParams-level identity gains a 4th value.
    //
    // Csound: UNLIKE Lco above, this is a REAL DSP path with its OWN
    // SynthVoice::EngineMode value (SynthVoice.h) — a processor-owned Csound
    // instance renders the voice's audio directly; it does not collapse onto
    // Wavetable. The enum value exists on every build unconditionally (Phase-1
    // spec D5): a .t5p saved with engineMode=csound must not be silently
    // remapped to another engine on a build without the Csound framework —
    // on those builds/machines, selecting Csound renders silence instead.
    enum : int { Sampler = 0, Wavetable = 1, Freeze = 2, Lco = 3, Csound = 4 };
    static constexpr ChoiceEntry kEntries[] = {
        { "sampler",   "Sampler"   },
        { "wavetable", "Wavetable" },
        { "freeze",    "Granular"  },
        { "lco",       "LRO"       },
        { "csound",    "Csound"    }
    };
    static constexpr int kCount = sizeof(kEntries) / sizeof(kEntries[0]);
    static_assert(Csound + 1 == kCount, "EngineMode out of sync.");
}

// ── Granular texture macro ──
// These are curated internal combinations of grain length, density, motion,
// and blur. Granular intentionally exposes musical texture classes instead of
// a full chaos-prone granular parameter bank.
namespace FreezeTexture {
    enum : int { Hold = 0, Silk = 1, Air = 2, Cloud = 3 };
    static constexpr ChoiceEntry kEntries[] = {
        { "hold",  "Hold"  },
        { "silk",  "Silk"  },
        { "air",   "Air"   },
        { "cloud", "Cloud" }
    };
    static constexpr int kCount = sizeof(kEntries) / sizeof(kEntries[0]);
    static_assert(Cloud + 1 == kCount, "FreezeTexture out of sync.");
}

// ── Sample loop mode ──
namespace LoopMode {
    enum : int { OneShot = 0, Loop = 1, PingPong = 2 };
    static constexpr ChoiceEntry kEntries[] = {
        { "oneshot",  "One-shot"  },
        { "loop",     "Loop"      },
        { "pingpong", "Ping-Pong" }
    };
    static constexpr int kCount = sizeof(kEntries) / sizeof(kEntries[0]);
    static_assert(PingPong + 1 == kCount, "LoopMode out of sync.");
}

// ── Loop optimization level ──
namespace LoopOptimize {
    enum : int { Off = 0, Low = 1, High = 2 };
    static constexpr ChoiceEntry kEntries[] = {
        { "off",  "Off"  },
        { "low",  "Low"  },
        { "high", "High" }
    };
    static constexpr int kCount = sizeof(kEntries) / sizeof(kEntries[0]);
    static_assert(High + 1 == kCount, "LoopOptimize out of sync.");
}

// ── Filter type ──
namespace FilterType {
    enum : int { Off = 0, Lowpass = 1, Highpass = 2, Bandpass = 3 };
    static constexpr ChoiceEntry kEntries[] = {
        { "off",      "Off"      },
        { "lowpass",  "Lowpass"  },
        { "highpass", "Highpass" },
        { "bandpass", "Bandpass" }
    };
    static constexpr int kCount = sizeof(kEntries) / sizeof(kEntries[0]);
    static_assert(Bandpass + 1 == kCount, "FilterType out of sync.");
}

// ── Filter slope ──
namespace FilterSlope {
    enum : int { Slope6 = 0, Slope12 = 1, Slope18 = 2, Slope24 = 3 };
    static constexpr ChoiceEntry kEntries[] = {
        { "6db",  "6dB"  },
        { "12db", "12dB" },
        { "18db", "18dB" },
        { "24db", "24dB" }
    };
    static constexpr int kCount = sizeof(kEntries) / sizeof(kEntries[0]);
    static_assert(Slope24 + 1 == kCount, "FilterSlope out of sync.");
}

// ── Filter drive oversampling factor ──
namespace FilterDriveOs {
    enum : int { Off = 0, X2 = 1, X4 = 2, X8 = 3 };
    static constexpr ChoiceEntry kEntries[] = {
        { "off", "Off" },
        { "2x",  "2x"  },
        { "4x",  "4x"  },
        { "8x",  "8x"  }
    };
    static constexpr int kCount = sizeof(kEntries) / sizeof(kEntries[0]);
    static_assert(X8 + 1 == kCount, "FilterDriveOs out of sync.");
}

// ── Filter algorithm ──
// SVF: existing linear TPT SVF + one-pole cascade (low CPU, clean default).
// Ladder: Huovilainen 4-pole transistor ladder with tanh saturation in each stage
//         (warm analog growl, self-oscillating at high resonance).
// Warp: Surge-XT-style ZDF ladder with per-pole nonlinearity, style-switchable
//       (wide continuous character space, designed for embedding modulation).
namespace FilterAlgorithm {
    enum : int { SVF = 0, Ladder = 1, Warp = 2 };
    static constexpr ChoiceEntry kEntries[] = {
        { "svf",    "SVF"    },
        { "ladder", "Ladder" },
        { "warp",   "Warp"   }
    };
    static constexpr int kCount = sizeof(kEntries) / sizeof(kEntries[0]);
    static_assert(Warp + 1 == kCount, "FilterAlgorithm out of sync.");
}

// ── Cutoff-Warp saturation style ──
// Applied to the feedback path of the ZDF ladder in CutoffWarpFilter. Only
// consumed when filterAlgorithm == Warp. Keys are stable forever — new styles
// must be appended, never reordered.
//
// ONE KEY HAS CHANGED, and it is the only one that ever will: index 2 was
// named after a product, which is forbidden anywhere in this repo. The curve
// it names is x / sqrt(1 + x*x), the algebraic sigmoid, so the key now says
// that. Presets written before the change store the old key; the reader in
// PluginProcessor.cpp maps it, because choiceFromKey's fallback is index 0 and
// an unknown key would silently load a DIFFERENT saturation.
namespace FilterWarpStyle {
    enum : int { Tanh = 0, SoftClip = 1, Algebraic = 2, Sin = 3, Digital = 4, Asym = 5 };
    static constexpr ChoiceEntry kEntries[] = {
        { "tanh",      "Tanh"      },
        { "softclip",  "SoftClip"  },
        { "algebraic", "Algebraic" },
        { "sin",      "Sin"      },
        { "digital",  "Digital"  },
        { "asym",     "Asym"     }
    };
    static constexpr int kCount = sizeof(kEntries) / sizeof(kEntries[0]);
    static_assert(Asym + 1 == kCount, "FilterWarpStyle out of sync.");
}

// ── Delay type ──
// Index 1 keeps the key "stereo" for preset back-compat: presets saved before
// the multi-mode rework stored delay_type = "stereo" and must still resolve to
// the original clean dual-mono behaviour (now labelled "Digital"). Choice params
// are serialised by key (choiceToKey/choiceFromKey), so ordering is free — only
// the key→DSP mapping must stay stable. DSP voicing lives in dsp/DelayLine.cpp.
namespace DelayType {
    enum : int { Off = 0, Digital = 1, PingPong = 2, Tape = 3, Bbd = 4,
                 TapeWarm = 5, TapeWild = 6, BbdClean = 7, BbdDegraded = 8,
                 TapeOld = 9 };
    // Every member of a family carries its character in its name — the plain
    // "Tape" and "BBD" said nothing about which of their four/three characters
    // they were, which is confusing in a menu listing all of them side by side.
    static constexpr ChoiceEntry kEntries[] = {
        { "off",         "Off"         },
        { "stereo",      "Digital"     },   // clean dual-mono; key kept for preset back-compat
        { "pingpong",    "Ping-Pong"   },   // true ping-pong (mono-sum in, cross feedback)
        { "tape3",       "Tape Clean"  },   // 3-head tape echo (RE-201 1:2:3 spacing). Key
                                            // "tape3" kept for back-compat; the retired 2-head
                                            // "tape2" folds in on load (see setStateInformation).
        { "bbd",         "BBD Warm"    },   // bucket-brigade (analog): dark steep recon, grit
        { "tape_warm",   "Tape Warm"   },   // tape + deeper wow + subtle 9.7 Hz zitter
        { "tape_wild",   "Tape Wild"   },   // tape + heavy wow + 6.0 + 9.7 Hz flutter
        { "bbd_clean",   "BBD Clean"   },   // BBD + bright recon + low grit (DM-2 style)
        { "bbd_degraded","BBD Degraded"},   // BBD + dark recon + heavy grit + warble
        { "tape_old",    "Tape Old"    },   // the pre-6988cdf1 ADDITIVE wobble (physically
                                            // wrong: same excursion on all heads) — restored
                                            // on request as a distinct flavour, NOT a default.
    };
    static constexpr int kCount = sizeof(kEntries) / sizeof(kEntries[0]);
    static_assert(TapeOld + 1 == kCount, "DelayType out of sync.");

    // Map flat choice index to DSP voicing mode (Off/Digital/PP/Tape/BBD)
    // and to character preset (0=default, 1=variant1, 2=variant2, 3=Tape-old additive).
    inline int baseMode(int dt) {
        if (dt == TapeWarm || dt == TapeWild || dt == TapeOld) return Tape;
        if (dt == BbdClean || dt == BbdDegraded)               return Bbd;
        return dt;
    }
    inline int character(int dt) {
        if (dt == TapeWarm || dt == BbdClean)    return 1;
        if (dt == TapeWild || dt == BbdDegraded) return 2;
        if (dt == TapeOld)                       return 3;
        return 0;
    }
}

// ── Reverb type ──
namespace ReverbType {
    // Dark/Medium/Bright are three IRs of the SAME EMT-140 plate — one reverb in
    // three tone colours, not three reverbs, so they are labelled as such. The keys
    // are untouched, so every stored preset keeps loading. The two
    // algorithmic entries are named for their algorithm: JUCE's reverb is Freeverb
    // ("based on the technique and tunings used in FreeVerb" — juce_Reverb.h), which
    // implements only the Schroeder-Moorer LATE field. Freeverb+ is the same late
    // field with Moorer's missing front end: a sparse early-reflection bank whose
    // tap times scale with Room, so Room finally means geometry and not just decay.
    // APPENDED, never inserted — every stored index below it keeps its meaning, so
    // this needs no calibration-epoch remap. The "algo" KEY is unchanged so existing
    // presets keep loading; only its display label became honest.
    enum : int { Off = 0, Dark = 1, Medium = 2, Bright = 3, Algo = 4, AlgoPlus = 5 };
    static constexpr ChoiceEntry kEntries[] = {
        { "off",       "Off"       },
        { "dark",      "Plate Dk"  },
        { "medium",    "Plate Md"  },
        { "bright",    "Plate Br"  },
        { "algo",      "Freeverb"  },
        { "algo_plus", "Freeverb+" }
    };
    static constexpr int kCount = sizeof(kEntries) / sizeof(kEntries[0]);
    static_assert(AlgoPlus + 1 == kCount, "ReverbType out of sync.");

    /** Both algorithmic types; the plate IRs are convolution. */
    inline bool isAlgorithmic(int t) { return t == Algo || t == AlgoPlus; }
    /** The three tone colours of the one EMT-140 plate. */
    inline bool isPlate(int t) { return t == Dark || t == Medium || t == Bright; }
}

namespace TremWave {
    // The tremolo's LFO shape. Order is mildest to hardest and MUST match
    // T5ynthTremolo::Wave, which is the enum the DSP switches on. Sine is index
    // 0 because it is the shape the tremolo had before it could be chosen, so a
    // patch written without this parameter loads sounding exactly as it did.
    // "Soft" is the rounded square real optical and bias tremolos actually make;
    // "Square" is the fast-edged one, still continuous, because a gain that
    // steps by the full depth twice a cycle is a click (src/dsp/AmpEffects.h).
    enum : int { Sine = 0, Tri = 1, Soft = 2, Square = 3 };
    static constexpr ChoiceEntry kEntries[] = {
        { "sine",   "Sine"   },
        { "tri",    "Tri"    },
        { "soft",   "Soft"   },
        { "square", "Square" }
    };
    static constexpr int kCount = sizeof(kEntries) / sizeof(kEntries[0]);
    static_assert(Square + 1 == kCount, "TremWave out of sync.");
}

// ── FX Mix: wet-path normalisation + the dry/wet law ─────────────────────────
//
// TWO separate things decide what the Delay/Reverb "Mix" knob does, and BOTH
// were wrong (measured, tools/measure_fx_mix.cpp):
//
//  1. The WET PATH GAIN. Each effect returned a different level at Mix=1.0 —
//     Digital/Ping-Pong unity, Tape +4.7 dB (three heads summed), BBD -2.5 dB,
//     Freeverb +5.5 dB, and the EMT-140 plate was pushed to +6.0 dB by an
//     explicit `kPlateWetGain = 2.0` comp whose only job was to MATCH the
//     already-hot Freeverb. So the same knob position meant a different amount
//     of effect per effect and per delay mode, and mid-travel on the plate put
//     the wet 6 dB ABOVE the dry. Every path is now trimmed to unity: Mix=1.0
//     returns the wet at the level the dry had.
//
//  2. The LAW. `wet = m, dry = 1-m` on top of those hot paths crammed the whole
//     musical range into the bottom third of the travel (plate: wet already
//     equalled dry at m=0.33) while pulling the direct signal down 6 dB at
//     mid-travel — the attack transient dropped out of the mix. The Freeverb's
//     `wet = m*m` was a second band-aid on the same hot path, which then made
//     ITS bottom end useless (-33.6 dB at m=0.10).
//
// The law is constant power BY CONSTRUCTION — dry² + wet² = 1 at every m — with
// the wet on a 1.5 power so the useful range spreads over the WHOLE slider:
//
//     m     0.10    0.20    0.30    0.40    0.50    0.60    0.75    0.90   1.00
//     W/D  -30.0   -20.9   -15.6   -11.7    -8.5    -5.6    -1.4    +4.3   pure
//     dry   -0.0    -0.0    -0.1    -0.3    -0.6    -1.1    -2.4    -5.7   -inf
//
// ~5 dB per 10 % of travel, the direct sound still essentially intact at
// mid-travel (-0.6 dB) where the effect is already clearly present, and m=1.0
// still reaches 100 % wet. Do NOT "fix" a hot effect by curving this law — trim
// its wet path to unity instead, which is what the trims below are for.
namespace FxMixLaw {
    inline float wetGain(float m)
    {
        const float c = m < 0.0f ? 0.0f : (m > 1.0f ? 1.0f : m);
        return c * std::sqrt(c);                                   // m^1.5
    }
    inline float dryGain(float m)
    {
        const float c = m < 0.0f ? 0.0f : (m > 1.0f ? 1.0f : m);
        return std::sqrt(std::max(0.0f, 1.0f - c * c * c));         // sqrt(1 - m^3)
    }

    // Wet-path trims. Measured pure-wet (feedback 0, internal mix 1.0) against a
    // steady 220 Hz saw, RMS over t=3..5 s. The tape/BBD characters spread ±0.6 dB
    // around their family mean; one trim per family, the residual is inaudible.
    //
    // ONE path is not flat across its own controls: the Freeverb's wet gain tracks
    // Room, because roomSize feeds the comb feedback (+1.31 dB at Room 0.0, +5.46
    // at 0.7, +7.30 at 1.0 — juce_Reverb.h sets no delay lengths from it). The trim
    // below is measured at the DEFAULT Room 0.7, so Algo still drifts -4.2 dB at
    // Room 0.0 and +1.8 dB at Room 1.0. Making the trim Room-dependent would stop
    // Room being a level control as well as a decay control — a change to what Room
    // MEANS, so it is not made here silently.
    static constexpr float kDelayTrimClean  = 1.000f;  // Digital / Ping-Pong: +0.00 dB
    static constexpr float kDelayTrimTape   = 0.579f;  // Tape family:  +4.74 dB (3 heads)
    static constexpr float kDelayTrimBbd    = 1.338f;  // BBD family:   -2.53 dB (recon + HP loss)
    // The REVERB trims come from a different meter than the delay ones, because a
    // steady tone cannot measure a reverb: it samples the wet path only where the
    // stimulus has harmonics, and it says nothing about the tail, which is the part
    // a reverb is judged by. Worse, the first version of that measurement read the
    // plates while juce::dsp::Convolution was still loading their IR on its
    // background thread — Convolution passes audio through UNCHANGED until the swap
    // lands, so it measured the dry signal and reported the perfectly plausible
    // "the EMT-140 IRs are already unity". They are not: energy-normalised IRs
    // (Convolution::Normalise::yes) spread the same energy over a long dense decay,
    // which is quiet at every instant. Measured properly (tools/measure_reverb_tail.cpp:
    // K-weighted loudness of a 300 ms note AND its tail, channel powers summed,
    // never mono-folded) the plates sat 14.0 / 16.7 / 18.5 dB below Freeverb —
    // audible as "Bright is barely there, Dark is fine", which is what it sounded like.
    //
    // Freeverb at Room 0.7 is the anchor: its level was judged right, so every
    // other type is trimmed onto it.
    static constexpr float kReverbTrimAlgo  = 0.533f;  // ANCHOR. Freeverb wetLevel=1.0 @ Room 0.7
    static constexpr float kReverbTrimAlgoPlus = 0.703f;  // -2.40 dB vs Freeverb: the combs are fed
                                                          // from the normalised reflection bank
    static constexpr float kReverbTrimPlateDark   = 2.670f;  // -13.99 dB vs Freeverb
    static constexpr float kReverbTrimPlateMedium = 3.646f;  // -16.70 dB
    static constexpr float kReverbTrimPlateBright = 4.483f;  // -18.50 dB

    inline float delayTrim(int baseMode)
    {
        if (baseMode == DelayType::Tape) return kDelayTrimTape;
        if (baseMode == DelayType::Bbd)  return kDelayTrimBbd;
        return kDelayTrimClean;
    }
    inline float reverbTrim(int reverbType)
    {
        switch (reverbType)
        {
            case ReverbType::Algo:     return kReverbTrimAlgo;
            case ReverbType::AlgoPlus: return kReverbTrimAlgoPlus;
            case ReverbType::Dark:     return kReverbTrimPlateDark;
            case ReverbType::Medium:   return kReverbTrimPlateMedium;
            case ReverbType::Bright:   return kReverbTrimPlateBright;
            default:                   return kReverbTrimPlateMedium;   // Off: see the note below
        }
    }

    // ── Migration (calibration epoch 5) ──
    // The PRE-epoch-5 wet/dry ratio at knob m was
    //     R = wet_old(m) · pathGain_old / (1 - m),   wet_old(m) = m or m²
    // and the new law's ratio is R = m^1.5 / sqrt(1 - m³), which inverts exactly:
    //     m_new = ( R² / (1 + R²) )^(1/3)
    // so a stored value migrates to the position that keeps the SAME audible
    // wet/dry balance. pathGain_old is the reciprocal of the trim now applied.
    inline float migrateMix(float mOld, int oldWetPow, float oldPathGain)
    {
        if (mOld <= 0.0f)    return 0.0f;
        if (mOld >= 0.999f)  return 1.0f;              // was pure wet, stays pure wet
        const float wetOld = (oldWetPow == 2) ? mOld * mOld : mOld;
        const float r      = (wetOld * oldPathGain) / (1.0f - mOld);
        const float r2     = r * r;
        return std::cbrt(r2 / (1.0f + r2));
    }
    inline float migrateDelayMix(float mOld, int delayType)
    {
        return migrateMix(mOld, 1, 1.0f / delayTrim(DelayType::baseMode(delayType)));
    }
    inline float migrateReverbMix(float mOld, int reverbType)
    {
        // The old path gain is the raw wet path (= 1 / its trim) times whatever
        // extra gain the old code applied on top: nothing for the algorithmic
        // types, the retired kPlateWetGain = 2.0 comp for the plates. Algo alone
        // squared the knob.
        const float raw = 1.0f / reverbTrim(reverbType);
        return ReverbType::isAlgorithmic(reverbType)
            ? migrateMix(mOld, 2, raw)
            : migrateMix(mOld, 1, 2.0f * raw);
    }
    // An effect stored as Off still migrates (against the plate / clean-delay path):
    // the parked value is a knob position in the OLD law's units, and leaving it
    // there guarantees it is misread the moment the effect is switched on. That is
    // NOT the same case as a stored tree with no type param at all — there the
    // file tells us nothing, so migrateValueTree leaves the mix alone.
}

// ── Ladder resonance law ──────────────────────────────────────────────────────
//
// Both ladder filters mapped the resonance knob straight onto the feedback gain:
// k = 4.2·r. That is linear in the wrong quantity. A 4-pole ZDF ladder's peak at
// cutoff is set by how close the loop gain comes to the pole (Zavalishin §5.2:
// each stage contributes 1/4 of the loop at ω_c, so |H| ≈ 1/(1 − k/k_pole)),
// which in dB is
//
//     P(k) = −20·log10(1 − k/k_pole).
//
// P is flat in k until k is nearly 4 and then a cliff. Measured on
// LadderFilter itself (LP, 24 dB/oct, 1 kHz, small-signal impulse), the old
// mapping delivered a peak of
//
//     r    0.000  0.125  0.250  0.375  0.500  0.625  0.750  0.875  0.950
//     dB    +1.6   −1.7   −2.1   −1.3   +0.2   +2.6   +6.4  +14.5  +41.8
//
// — the first half of the travel does nothing at all and everything audible
// happens in the last eighth. That is the "weak, not expressive" the filter was
// reported as. Inverting P for a knob that travels evenly in DECIBELS instead:
//
//     k(r) = k_pole·(1 − 10^(−P_max·r/20)),   P_max = 42 dB.
//
// The top of the travel is the self-oscillation region, which this law cannot
// express — k_pole IS the pole, and 1/(1 − k/k_pole) is infinite there. So above
// the knee k ramps on to k_pole·1.05, the small overshoot that guarantees an
// audible ring once the per-stage saturation has shaved a little loop gain. The
// knee sits exactly where the OLD mapping crossed the pole (r = k_pole/4.2), so
// the self-oscillating stretch of the knob is unchanged for every filter.
//
// k_pole per filter:
//   • LadderFilter — exactly 4, derived, not fitted (Zavalishin §5.2:
//     1/|G(jω_c)| = 1/(1/4)). Measured onset lands between r = 0.94 and 0.96,
//     i.e. k between 3.95 and 4.03, which is the derivation confirmed.
//   • CutoffWarpFilter — one per saturation style, because each style's curve
//     has its own slope near zero and kStyleScale only approximately compensates
//     it. Bisected on the sustained-ring criterion (excite, wait 1 s, require
//     RMS > 1e-3 over the next second), in NOMINAL k, i.e. what setResonance
//     writes before kStyleScale — so the law composes with that scale instead of
//     fighting it. Measured at 110 Hz / 1 kHz / 4 kHz; the spread across cutoff
//     is under 2 %, so one number per style stands. The values below are the
//     1 kHz column. Tanh measures 3.9934 against the theoretical 4 — the
//     method validating itself on the one style whose pole is known.
//
// P_max inverts the LINEAR model, while the ladder's per-stage saturation shaves
// the top of the range — so the delivered peak is not 42 dB but, on the Ladder,
// the measured
//
//     r    0.000  0.125  0.250  0.375  0.500  0.625  0.750  0.875  0.950
//     dB    +1.6   −0.5   +4.1   +9.4  +14.7  +20.2  +25.5  +30.7  +33.5
//
// which is what matters: ~5.3 dB per eighth of travel from 0.25 upward, and the
// compression is uniform rather than a cliff. Self-oscillation onset is
// unchanged (between r = 0.94 and 0.96, at 110 Hz as at 1 kHz). Each Warp style
// compresses differently above the knee — that difference IS the style, and is
// left alone; what is evened out is only where the travel starts to bite.
//
// This is the law the SVF has always had: T5ynthFilter maps r onto Q = 0.5·36^r,
// i.e. −6 + 31·r dB of peak — even in dB across the whole travel. The two
// ladders were the outliers, not the knob.
namespace LadderResoLaw {
    static constexpr float kPeakDbMax = 42.0f;   // dB of peak at the top of the even stretch
    static constexpr float kOldSlope  = 4.2f;    // the retired mapping, k = 4.2·r
    static constexpr float kOvershoot = 1.05f;   // k at r = 1, relative to the pole
    // 10^(−42/20), the knee's fraction of the pole. Written out rather than
    // recomputed: feedback() runs per voice per block on the audio thread.
    static constexpr float kKneeFrac  = 1.0f - 0.00794328f;

    static constexpr float kLadderPole  = 4.0f;    // derived (Zavalishin §5.2)

    // CutoffWarpFilter, indexed by FilterWarpStyle. Bisected, 1 kHz — see above.
    static constexpr float kWarpPole[6] = {
        3.9934f,   // Tanh
        3.8683f,   // SoftClip
        3.9176f,   // Algebraic
        3.8536f,   // Sin
        3.9120f,   // Digital
        3.9086f,   // Asym
    };
    inline float warpPole(int style)
    {
        return kWarpPole[(style < 0 || style > 5) ? 0 : style];
    }

    // Where the OLD mapping crossed this pole — the knee, and the start of the
    // self-oscillating stretch.
    inline float selfOscR(float kPole) { return kPole / kOldSlope; }

    // Feedback gain for a knob position. Even in dB up to the knee, then into
    // self-oscillation.
    inline float feedback(float r, float kPole)
    {
        const float c = r < 0.0f ? 0.0f : (r > 1.0f ? 1.0f : r);
        const float rKnee = selfOscR(kPole);
        if (c <= rKnee)
            return kPole * (1.0f - std::pow(10.0f, -kPeakDbMax * (c / rKnee) / 20.0f));
        const float kKnee = kPole * kKneeFrac;
        const float t     = (c - rKnee) / (1.0f - rKnee);
        return kKnee + t * (kPole * kOvershoot - kKnee);
    }

    // ── Migration (calibration epoch 7) ──
    // Inverse of the above composed with the old k = 4.2·r, so a stored knob
    // position moves to the one that produces the SAME feedback gain — and
    // therefore the same sound — under the new law. The SVF's r → Q map did not
    // change, so an SVF preset never comes through here.
    inline float migrateResonance(float rOld, float kPole)
    {
        if (rOld <= 0.0f)   return 0.0f;
        if (rOld >= 0.999f) return 1.0f;
        const float kOld  = kOldSlope * rOld;
        const float rKnee = selfOscR(kPole);
        const float kKnee = kPole * kKneeFrac;
        if (kOld >= kKnee)   // the old top stretch lands in the new self-osc ramp
            return rKnee + (1.0f - rKnee) * ((kOld - kKnee) / (kPole * kOvershoot - kKnee));
        const float p = -20.0f * std::log10(1.0f - kOld / kPole);  // dB the old k produced
        return rKnee * (p / kPeakDbMax);
    }
}

// ── Noise oscillator type (namespace name avoids clash with the global
//    `enum class NoiseType` in dsp/NoiseGenerator.h). ──
namespace NoiseKind {
    enum : int { White = 0, Pink = 1, Brown = 2 };
    static constexpr ChoiceEntry kEntries[] = {
        { "white", "White" },
        { "pink",  "Pink"  },
        { "brown", "Brown" }
    };
    static constexpr int kCount = sizeof(kEntries) / sizeof(kEntries[0]);
    static_assert(Brown + 1 == kCount, "NoiseKind out of sync.");
}

// ── LFO waveform (6 entries; SawDown = inverse/falling saw, appended last so
//    existing choice indices stay stable for DAW-session recall) ──
namespace LfoWave {
    enum : int { Sine = 0, Tri = 1, Saw = 2, Square = 3, SampleHold = 4, SawDown = 5 };
    static constexpr ChoiceEntry kEntries[] = {
        { "sine",            "Sin"  },
        { "triangle",        "Tri"  },
        { "sawtooth",        "Saw"  },
        { "square",          "Sq"   },
        { "sample_and_hold", "S&H"  },
        { "saw_down",        "SawD" }
    };
    static constexpr int kCount = sizeof(kEntries) / sizeof(kEntries[0]);
    static_assert(SawDown + 1 == kCount, "LfoWave out of sync.");
}

// ── LFO trigger mode ──
namespace LfoMode {
    enum : int { Free = 0, Trigger = 1 };
    static constexpr ChoiceEntry kEntries[] = {
        { "free",    "Free" },
        { "trigger", "Trig" }
    };
    static constexpr int kCount = sizeof(kEntries) / sizeof(kEntries[0]);
    static_assert(Trigger + 1 == kCount, "LfoMode out of sync.");
}

// ── BPM-sync clock state ─ Off / Sync; clock source auto-resolved
//    (host transport when playing, in-app sequencer when running, last-
//    seen host BPM as freeze, seqBpm as standalone fallback).
namespace ClockMode {
    enum : int { Off = 0, Sync = 1 };
    static constexpr ChoiceEntry kEntries[] = {
        { "off",  "Off"  },
        { "sync", "Sync" }
    };
    static constexpr int kCount = sizeof(kEntries) / sizeof(kEntries[0]);
    static_assert(Sync + 1 == kCount, "ClockMode out of sync.");
}

// ── BPM-sync musical divisions ─ monotonic in rate (slow → fast) so the
//    UI renders as a coherent stepped slider. kFactor[i] = events per
//    whole note. Triplet = 3-in-2 (×1.5 vs straight); quintuplet = 5-in-4
//    (×1.25 vs straight). Slow end (16/1 … 2/1) gives drift/LFO cycles
//    spanning multiple bars — suited to slow harmonic motion and long
//    auto-regen swings.
namespace ClockDivision {
    enum : int {
        D16_1  = 0,
        D8_1   = 1,
        D4_1   = 2,
        D2_1   = 3,
        D1_1   = 4,
        D1_2   = 5,
        D1_2T  = 6,
        D1_4   = 7,
        D1_4Q  = 8,
        D1_4T  = 9,
        D1_8   = 10,
        D1_8Q  = 11,
        D1_8T  = 12,
        D1_16  = 13,
        D1_16Q = 14,
        D1_16T = 15,
        D1_32  = 16
    };
    static constexpr ChoiceEntry kEntries[] = {
        { "16_1",  "16/1"  },
        { "8_1",   "8/1"   },
        { "4_1",   "4/1"   },
        { "2_1",   "2/1"   },
        { "1_1",   "1/1"   },
        { "1_2",   "1/2"   },
        { "1_2t",  "1/2T"  },
        { "1_4",   "1/4"   },
        { "1_4q",  "1/4Q"  },
        { "1_4t",  "1/4T"  },
        { "1_8",   "1/8"   },
        { "1_8q",  "1/8Q"  },
        { "1_8t",  "1/8T"  },
        { "1_16",  "1/16"  },
        { "1_16q", "1/16Q" },
        { "1_16t", "1/16T" },
        { "1_32",  "1/32"  }
    };
    static constexpr float kFactor[] = {
        1.0f / 16.0f, 1.0f / 8.0f, 1.0f / 4.0f, 1.0f / 2.0f,
        1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f,
        8.0f, 10.0f, 12.0f, 16.0f, 20.0f, 24.0f, 32.0f
    };
    static constexpr int kCount = sizeof(kEntries) / sizeof(kEntries[0]);
    static_assert(D1_32 + 1 == kCount,
                  "ClockDivision enum and kEntries out of sync.");
    static_assert(sizeof(kFactor) / sizeof(kFactor[0]) == kCount,
                  "ClockDivision kFactor and kEntries out of sync.");
}

// ── Drift-only BPM-sync divisions ─ a slower, coarser subset than
//    ClockDivision. Drift is slow harmonic/timbral motion, so its sync range
//    runs from 64/1 (64 whole-note cycles) at the slow end down to 1/4 at the
//    fast end — no sub-1/4 or tuplet steps. Monotonic slow → fast like
//    ClockDivision so the stepped slider stays coherent. The `.key` strings
//    deliberately MATCH ClockDivision's for the shared divisions (16/1 … 1/4),
//    so a preset saved before Drift had its own list reloads by key with no
//    migration; a now-removed division (e.g. "1_8" or a tuplet) falls back to
//    1/4, the nearest surviving step (the param DEFAULT, separately, is 2/1).
//    kFactor[i] = events per whole note.
namespace DriftDivision {
    enum : int {
        D64_1 = 0,
        D32_1 = 1,
        D16_1 = 2,
        D8_1  = 3,
        D4_1  = 4,
        D2_1  = 5,
        D1_1  = 6,
        D1_2  = 7,
        D1_4  = 8
    };
    static constexpr ChoiceEntry kEntries[] = {
        { "64_1", "64/1" },
        { "32_1", "32/1" },
        { "16_1", "16/1" },
        { "8_1",  "8/1"  },
        { "4_1",  "4/1"  },
        { "2_1",  "2/1"  },
        { "1_1",  "1/1"  },
        { "1_2",  "1/2"  },
        { "1_4",  "1/4"  }
    };
    static constexpr float kFactor[] = {
        1.0f / 64.0f, 1.0f / 32.0f, 1.0f / 16.0f, 1.0f / 8.0f,
        1.0f / 4.0f, 1.0f / 2.0f, 1.0f, 2.0f, 4.0f
    };
    static constexpr int kCount = sizeof(kEntries) / sizeof(kEntries[0]);
    static_assert(D1_4 + 1 == kCount,
                  "DriftDivision enum and kEntries out of sync.");
    static_assert(sizeof(kFactor) / sizeof(kFactor[0]) == kCount,
                  "DriftDivision kFactor and kEntries out of sync.");
}

// ── Sync-rate / sync-delay helpers ──
// `bpm` is the resolved sync BPM (host transport, in-app sequencer, frozen
// host, or seqBpm fallback — caller is responsible for resolution). A zero
// or negative bpm degrades to 120 to keep the LFO/delay alive instead of
// freezing or dividing by zero.
namespace ClockSync {
    // Core: events-per-whole-note `factor` → cycles/sec at `bpm` (4 beats per
    // whole note). Drift calls this directly with DriftDivision::kFactor; the
    // ClockDivision path goes through computeRate() just below.
    inline float computeRateFromFactor(float bpm, float factor)
    {
        if (! (bpm > 0.0f)) bpm = 120.0f;
        return factor * (bpm / 60.0f) / 4.0f;
    }
    inline float computeRate(float bpm, int divisionIdx)
    {
        const int idx = std::clamp(divisionIdx, 0, ClockDivision::kCount - 1);
        return computeRateFromFactor(bpm, ClockDivision::kFactor[idx]);
    }
    inline float computeDelayMs(float bpm, int divisionIdx)
    {
        if (! (bpm > 0.0f)) bpm = 120.0f;
        const int idx = std::clamp(divisionIdx, 0, ClockDivision::kCount - 1);
        return (60000.0f / bpm) * (4.0f / ClockDivision::kFactor[idx]);
    }
}

// ── Drift LFO waveform (label "Sq" differs from LfoWave "Square"!).
//    SawDown = inverse/falling saw, appended last to keep choice indices
//    stable for DAW-session recall.
//    Index 4 is bit-for-bit the same DSP as LfoWave::SampleHold — one xorshift
//    value latched per period, held flat in between (stepped, never ramped) —
//    so it carries the same "S&H" label. The .t5p KEY stays "random": keys are
//    written into saved presets and choiceFromKey falls back to index 0 (Sine)
//    on an unknown one, so renaming it would silently turn every saved Drift
//    S&H patch into a sine. ──
namespace DriftWave {
    enum : int { Sine = 0, Tri = 1, Saw = 2, Square = 3, SampleHold = 4, SawDown = 5 };
    static constexpr ChoiceEntry kEntries[] = {
        { "sine",     "Sine" },
        { "triangle", "Tri"  },
        { "sawtooth", "Saw"  },
        { "square",   "Sq"   },
        { "random",   "S&H"  },
        { "saw_down", "SawD" }
    };
    static constexpr int kCount = sizeof(kEntries) / sizeof(kEntries[0]);
    static_assert(SawDown + 1 == kCount, "DriftWave out of sync.");
}

// ── Envelope curve shape. Since 2026-08-05 the PARAMETER is a continuous bend
//    (dsp/ADSREnvelope.h); this table is no longer a choice list but the five
//    NAMED anchors on that axis, and it survives because saved presets store the
//    shape by key. bend = (index − 2)/2, so log→-1 … exp→+1; how far a stage
//    travels PAST those two is kBendSag below. ──
namespace EnvCurve {
    enum : int { Log = 0, SLog = 1, Lin = 2, SExp = 3, Exp = 4 };
    static constexpr ChoiceEntry kEntries[] = {
        { "log",     "Log"  },
        { "softlog", "SLog" },
        { "lin",     "Lin"  },
        { "softexp", "SExp" },
        { "exp",     "Exp"  }
    };
    static constexpr int kCount = sizeof(kEntries) / sizeof(kEntries[0]);
    static_assert(Exp + 1 == kCount, "EnvCurve out of sync.");

    // The bend axis. The five NAMED shapes end at -1 and +1, and each stage
    // travels ONE UNIT FURTHER in the direction it sags on screen: the attack
    // towards Log (a rise that stays down longer and then comes up), the decay
    // and release towards Exp (a faster initial fall — and, at the same release
    // time, a longer quiet tail). BJ 2026-08-05: the old maximum was not steep
    // enough on exactly that side, while the opposite one ("ein ewiges
    // quasi-sustain und superschneller Absturz") was already strong enough, so
    // it stops at the pole. |bend| ≤ kBendSag on both laws; see ADSREnvelope.h.
    static constexpr float kBendPole = 1.0f;   // where the five named shapes sit
    static constexpr float kBendSag  = 2.0f;   // how far the sagging side goes
    static constexpr float kBendStep = 0.01f;

    /** Anchor index → bend. Mirrors ::bendFromCurveIndex in dsp/ADSREnvelope.h,
     *  repeated here so the preset/parameter layer needs no DSP include. */
    static constexpr float bendFromIndex(int i) { return (static_cast<float>(i) - 2.0f) * 0.5f; }

    /** Nearest named anchor for a bend — for the key a .t5p still writes so an
     *  older build reading a newer preset lands on the closest curve it has. */
    inline int nearestIndex(float bend)
    {
        const int i = static_cast<int>(std::lround(bend * 2.0f)) + 2;
        return i < 0 ? 0 : (i > Exp ? Exp : i);
    }
}

// ── Envelope velocity→time mode ──
namespace EnvVelTimeMode {
    enum : int { Off = 0, Positive = 1, Negative = 2 };
    static constexpr ChoiceEntry kEntries[] = {
        { "off", "Off"  },
        { "pos", "Vel+" },
        { "neg", "Vel-" }
    };
    static constexpr int kCount = sizeof(kEntries) / sizeof(kEntries[0]);
    static_assert(Negative + 1 == kCount, "EnvVelTimeMode out of sync.");
}

// ── Drift regenerate mode (ASCII labels: manual / a.s.a.p. / N bars) ──
namespace DriftRegen {
    enum : int { Manual = 0, Auto = 1, Bar1 = 2, Bar2 = 3, Bar4 = 4, Bar8 = 5, Bar16 = 6 };
    // "repeat every": manual / a.s.a.p. / N bars (1 bar = 4 beats; the actual
    // cooldown lives in PromptPanel::pollDriftRegen). ASCII labels only (no mojibake
    // risk). Keys "manual"/"auto" are kept stable for preset compatibility; the OLD
    // beat-based keys (max_1beat/…) are absent now, so a preset that saved one
    // falls back to Manual on load (choiceFromKey → 0) — a safe default.
    static constexpr ChoiceEntry kEntries[] = {
        { "manual", "manual"   },
        { "auto",   "a.s.a.p." },
        { "bar_1",  "1 bar"    },
        { "bar_2",  "2 bars"   },
        { "bar_4",  "4 bars"   },
        { "bar_8",  "8 bars"   },
        { "bar_16", "16 bars"  }
    };
    static constexpr int kCount = sizeof(kEntries) / sizeof(kEntries[0]);
    static_assert(Bar16 + 1 == kCount, "DriftRegen out of sync.");
}

// ── Semantic-loop interpreter stance ──
// The curated SIX deconstructive stances of the CLAP→LLM self-listening loop
// (tools/clap_llm_loop.py MODES), in movement-type order. The `.key` column IS
// the stance id consumed by inference/RepromptStances (stanceSystemPrompt /
// buildStanceUserTurn) — it must match the keys in clap_llm_loop's MODES dict.
// The `.label` is a Phase-C placeholder (the custom slider will paint formal
// symbols); for now it doubles as the host generic-editor display string.
// Off (index 0) disables the loop entirely (the cheap early-out in
// PromptPanel::runSemanticLoopStep).
namespace RepromptStance {
    enum : int {
        Off = 0, Transcribe = 1, Entkitscher = 2, Verniedlicher = 3,
        Variation = 4, Abduktion = 5, Opposite = 6
    };
    static constexpr ChoiceEntry kEntries[] = {
        { "off",           "Off"           },
        { "transcribe",    "Transcribe"    },
        { "entkitscher",   "Sober"         },
        { "verniedlicher", "Sweeten"       },
        { "variation",     "Variation"     },
        { "abduction",     "Abduction"     },
        { "opposite",      "Opposite"      }
    };
    static constexpr int kCount = sizeof(kEntries) / sizeof(kEntries[0]);
    static_assert(Opposite + 1 == kCount, "RepromptStance out of sync.");
}

// ── Semantic-loop coupling topology ──
// One axis: how far the machine displaces the human input (clap_llm_loop's
// --couple). alpha = A fixed anchor, only B rewritten; ab_add = both poles, each
// = its own original + ", " + its latest interpretation (the _concat2 pattern);
// ab_replace = both poles fully replaced by the same stance with per-pole inputs.
// The `.key` matches the tool's coupling names (alpha / concat / voll) so a
// future preset round-trip lines up with the verified algorithm.
namespace RepromptCoupling {
    enum : int { Alpha = 0, AbAdd = 1, AbReplace = 2 };
    static constexpr ChoiceEntry kEntries[] = {
        { "alpha",  "A->B"     },   // = clap_llm_loop "alpha"
        { "concat", "A+B add"  },   // = clap_llm_loop "concat"
        { "voll",   "A+B repl" }    // = clap_llm_loop "voll"
    };
    static constexpr int kCount = sizeof(kEntries) / sizeof(kEntries[0]);
    static_assert(AbReplace + 1 == kCount, "RepromptCoupling out of sync.");
}

// ── Resynth seed source ──────────────────────────────────────────────────────
// Controls which audio seeds the Resynth (init_audio / SDEdit) path in
// buildInferenceRequest. Per-preset (saved/loaded as a key string). Default 0
// (Internal) restores the original self-feedback behaviour so old presets load
// correctly; External requires a live input bus (ext-audio capture feature).
namespace ResynthSource {
    enum : int { Internal = 0, External = 1 };
    static constexpr ChoiceEntry kEntries[] = {
        { "int", "Int" },   // self-feedback: seed = last generation
        { "ext", "Ext" }    // external: seed = live audio input capture
    };
    static constexpr int kCount = sizeof(kEntries) / sizeof(kEntries[0]);
    static_assert(External + 1 == kCount, "ResynthSource out of sync.");
}

// ── Voice count ──
namespace VoiceCount {
    enum : int { Mono = 0, V4 = 1, V6 = 2, V8 = 3, V12 = 4, V16 = 5, V64 = 6, V128 = 7 };
    static constexpr ChoiceEntry kEntries[] = {
        { "mono", "Mono" },
        { "4",    "4"    },
        { "6",    "6"    },
        { "8",    "8"    },
        { "12",   "12"   },
        { "16",   "16"   },
        { "64",   "64"   },
        { "128",  "128"  }
    };
    static constexpr int kCount = sizeof(kEntries) / sizeof(kEntries[0]);
    static_assert(V128 + 1 == kCount, "VoiceCount out of sync.");
}

// ── Wavetable frame count ──
namespace WtFrames {
    enum : int { F32 = 0, F64 = 1, F128 = 2, F256 = 3 };
    static constexpr ChoiceEntry kEntries[] = {
        { "32",  "32"  },
        { "64",  "64"  },
        { "128", "128" },
        { "256", "256" }
    };
    static constexpr int kCount = sizeof(kEntries) / sizeof(kEntries[0]);
    static_assert(F256 + 1 == kCount, "WtFrames out of sync.");
}

// ── Oscillator octave shift (pitch compensation for inferred fundamental) ──
namespace OscOctave {
    enum : int { Neg2 = 0, Neg1 = 1, Zero = 2, Pos1 = 3, Pos2 = 4 };
    static constexpr ChoiceEntry kEntries[] = {
        { "-2", "-2" },
        { "-1", "-1" },
        { "0",  "0"  },
        { "+1", "+1" },
        { "+2", "+2" }
    };
    static constexpr int kCount = sizeof(kEntries) / sizeof(kEntries[0]);
    static_assert(Pos2 + 1 == kCount, "OscOctave out of sync.");
}

// ── Sequencer octave shift (melodic range, semantically different from
//    OscOctave — kept as a separate namespace on purpose). ──
namespace SeqOctave {
    enum : int { Neg2 = 0, Neg1 = 1, Zero = 2, Pos1 = 3, Pos2 = 4 };
    static constexpr ChoiceEntry kEntries[] = {
        { "-2", "-2" },
        { "-1", "-1" },
        { "0",  "0"  },
        { "+1", "+1" },
        { "+2", "+2" }
    };
    static constexpr int kCount = sizeof(kEntries) / sizeof(kEntries[0]);
    static_assert(Pos2 + 1 == kCount, "SeqOctave out of sync.");
}

// ── Sequencer mode ──
namespace SeqMode {
    enum : int { Seq = 0, ArpUp = 1, ArpDown = 2, ArpUpDown = 3, ArpRandom = 4 };
    static constexpr ChoiceEntry kEntries[] = {
        { "seq",        "Seq"     },
        { "arp_up",     "Arp Up"  },
        { "arp_down",   "Arp Dn"  },
        { "arp_updown", "Arp UD"  },
        { "arp_random", "Arp Rnd" }
    };
    static constexpr int kCount = sizeof(kEntries) / sizeof(kEntries[0]);
    static_assert(ArpRandom + 1 == kCount, "SeqMode out of sync.");
}

// ── Sequencer note division ──
namespace SeqDivision {
    enum : int { D1_1 = 0, D1_2 = 1, D1_4 = 2, D1_8 = 3, D1_16 = 4 };
    static constexpr ChoiceEntry kEntries[] = {
        { "1_1",  "1/1"  },
        { "1_2",  "1/2"  },
        { "1_4",  "1/4"  },
        { "1_8",  "1/8"  },
        { "1_16", "1/16" }
    };
    static constexpr int kCount = sizeof(kEntries) / sizeof(kEntries[0]);
    static_assert(D1_16 + 1 == kCount, "SeqDivision out of sync.");
}

// ── Arpeggiator rate (straight + triplet divisions) ──
namespace ArpRate {
    enum : int {
        R1_4 = 0, R1_8 = 1, R1_16 = 2, R1_32 = 3,
        R1_4T = 4, R1_8T = 5, R1_16T = 6
    };
    static constexpr ChoiceEntry kEntries[] = {
        { "1_4",   "1/4"   },
        { "1_8",   "1/8"   },
        { "1_16",  "1/16"  },
        { "1_32",  "1/32"  },
        { "1_4t",  "1/4T"  },
        { "1_8t",  "1/8T"  },
        { "1_16t", "1/16T" }
    };
    static constexpr int kCount = sizeof(kEntries) / sizeof(kEntries[0]);
    static_assert(R1_16T + 1 == kCount, "ArpRate out of sync.");
}

// ── Arpeggiator mode ──
namespace ArpMode {
    enum : int { Off = 0, Up = 1, Down = 2, UpDown = 3, Random = 4 };
    static constexpr ChoiceEntry kEntries[] = {
        { "off",    "Off"    },
        { "up",     "Up"     },
        { "down",   "Down"   },
        { "updown", "UpDown" },
        { "random", "Random" }
    };
    static constexpr int kCount = sizeof(kEntries) / sizeof(kEntries[0]);
    static_assert(Random + 1 == kCount, "ArpMode out of sync.");
}

// ── Musical scale root ──
namespace ScaleRoot {
    enum : int {
        C = 0, Cs = 1, D = 2, Ds = 3, E = 4, F = 5,
        Fs = 6, G = 7, Gs = 8, A = 9, As = 10, B = 11
    };
    static constexpr ChoiceEntry kEntries[] = {
        { "c",  "C"  },
        { "cs", "C#" },
        { "d",  "D"  },
        { "ds", "D#" },
        { "e",  "E"  },
        { "f",  "F"  },
        { "fs", "F#" },
        { "g",  "G"  },
        { "gs", "G#" },
        { "a",  "A"  },
        { "as", "A#" },
        { "b",  "B"  }
    };
    static constexpr int kCount = sizeof(kEntries) / sizeof(kEntries[0]);
    static_assert(B + 1 == kCount, "ScaleRoot out of sync.");
}

// ── Musical scale type ──
namespace ScaleType {
    enum : int {
        Off = 0, Maj, Min, Pent, Dor, Harm, WhlT,           // 0-6: unchanged
        Mixo, Phry, Lyd, Locr, MinP, Blu, MelM,             // 7-13: western ext.
        Hira, InSn, Iwat, Kumo, Ryuk,                        // 14-18: east asian
        Hjz, DblH, Todi, Purv, Pers,                         // 19-23: middle east / south asia
        HunM, NeaM,                                           // 24-25: european ext.
        Pelg, Slnd                                             // 26-27: southeast asian
    };
    static constexpr ChoiceEntry kEntries[] = {
        { "off",   "Chromatic" },   // key "off" kept stable; "off" == no scale quantize
        { "maj",   "Major"     },
        { "min",   "Minor"     },
        { "pent",  "Penta"     },
        { "dor",   "Dorian"    },
        { "harm",  "Harm.Min"  },
        { "whole", "WhlTone"   },
        { "mixo",  "Mixolyd"   },
        { "phry",  "Phrygian"  },
        { "lyd",   "Lydian"    },
        { "locr",  "Locrian"   },
        { "minp",  "MinPent"   },
        { "blu",   "Blues"     },
        { "melm",  "Mel.Min"   },
        { "hira",  "Hirajoshi" },
        { "insn",  "In-sen"    },
        { "iwat",  "Iwato"     },
        { "kumo",  "Kumoi"     },
        { "ryuk",  "Ryukyu"    },
        { "hjz",   "Hijaz"     },
        { "dblh",  "DblHarm"   },
        { "todi",  "R.Todi"    },
        { "purv",  "R.Purvi"   },
        { "pers",  "Persian"   },
        { "hunm",  "Hung.Min"  },
        { "neam",  "Neap.Min"  },
        { "pelg",  "Pelog"     },
        { "slnd",  "Slendro"   },
    };
    static constexpr int kCount = sizeof(kEntries) / sizeof(kEntries[0]);
    static_assert(Slnd + 1 == kCount, "ScaleType out of sync.");
}

// ── Tuning system ──
namespace TuningType {
    enum : int { Equal = 0, Maqm, Shru, Pelg, Slnd };
    static constexpr ChoiceEntry kEntries[] = {
        { "eq",   "12-TET"  },
        { "maqm", "Maqam"   },
        { "shru", "Shruti"  },
        { "pelg", "Pelog"   },
        { "slnd", "Slendro" },
    };
    static constexpr int kCount = sizeof(kEntries) / sizeof(kEntries[0]);
    static_assert(Slnd + 1 == kCount, "TuningType out of sync.");
}

// ── Generative sequencer octave range ──
namespace GenRange {
    enum : int { R1 = 0, R2 = 1, R3 = 2, R4 = 3 };
    static constexpr ChoiceEntry kEntries[] = {
        { "1", "1" },
        { "2", "2" },
        { "3", "3" },
        { "4", "4" }
    };
    static constexpr int kCount = sizeof(kEntries) / sizeof(kEntries[0]);
    static_assert(R4 + 1 == kCount, "GenRange out of sync.");
}

// ── Pitch-field evolution mode (polyphonic generative sequencer) ──
namespace FieldMode {
    enum : int { Static = 0, Drift = 1, Transform = 2, Pivot = 3 };
    static constexpr ChoiceEntry kEntries[] = {
        { "static",    "Static"    },
        { "drift",     "Drift"     },
        { "transform", "Transform" },
        { "pivot",     "Pivot"     }
    };
    static constexpr int kCount = sizeof(kEntries) / sizeof(kEntries[0]);
    static_assert(Pivot + 1 == kCount, "FieldMode out of sync.");
}

// ── Pitch-field pivot interval (semitones) — pc-set transposition amount ──
namespace FieldPivot {
    enum : int { m3 = 0, M3 = 1, TT = 2, P5 = 3 };
    static constexpr ChoiceEntry kEntries[] = {
        { "m3", "m3"  },
        { "M3", "M3"  },
        { "tt", "Tri" },
        { "p5", "P5"  }
    };
    static constexpr int kCount = sizeof(kEntries) / sizeof(kEntries[0]);
    static_assert(P5 + 1 == kCount, "FieldPivot out of sync.");
    static constexpr int kSemitones[kCount] = { 3, 4, 6, 7 };
}

// ── Strand role (textural function — deliberately post-tonal labels) ──
namespace StrandRole {
    enum : int { Anchor = 0, Line = 1, Density = 2, Gesture = 3 };
    static constexpr ChoiceEntry kEntries[] = {
        { "anchor",  "Anchor"  },
        { "line",    "Line"    },
        { "density", "Density" },
        { "gesture", "Gesture" }
    };
    static constexpr int kCount = sizeof(kEntries) / sizeof(kEntries[0]);
    static_assert(Gesture + 1 == kCount, "StrandRole out of sync.");
}

// ── Inter-strand coordination strategy ──
//
// Voice-1 principles operate within a strand; this enum chooses how the
// strands relate to each other. DensityBudget (default) caps simultaneous
// onsets by role priority; Dialog adds deterministic per-strand stances
// toward strand 0 (follow / counter / independent — see the Stance enum in
// GenerativeSequencer.h; reference: Lewis 2000). The former reserved IDs
// "AlgebraicCoupling"/"ContrapuntalChecks" (silent Independent fall-throughs,
// never implemented) were removed 2026-07-16 — Western counterpoint
// machinery is deliberately out of scope for this ensemble.
namespace CoordinationMode {
    enum : int { Independent = 0, DensityBudget = 1, Dialog = 2 };
    static constexpr ChoiceEntry kEntries[] = {
        { "independent",        "Independent"   },
        { "density_budget",     "Density Budget" },
        { "dialog",             "Dialog"         }
    };
    static constexpr int kCount = sizeof(kEntries) / sizeof(kEntries[0]);
    static_assert(Dialog + 1 == kCount, "CoordinationMode out of sync.");
}

// ── Strand division multiplier (speed factor relative to global division) ──
namespace StrandDivMult {
    enum : int {
        Sixteenth = 0, Eighth = 1, Fifth = 2, Quarter = 3, Third = 4, Half = 5, X = 6,
        X2 = 7, X3 = 8, X4 = 9, X5 = 10, X8 = 11, X16 = 12
    };
    // Labels use plain ASCII "x" — the previous UTF-8 multiplication sign
    // (U+00D7) failed to render correctly through JUCE's ComboBox font on
    // Linux ("1Ã—" instead of "1×").
    static constexpr ChoiceEntry kEntries[] = {
        { "sixteenth", "1/16x" },
        { "eighth",    "1/8x"  },
        { "fifth",     "1/5x"  },
        { "quarter",   "1/4x"  },
        { "third",     "1/3x"  },
        { "half",      "1/2x"  },
        { "x1",        "1x"    },
        { "x2",        "2x"    },
        { "x3",        "3x"    },
        { "x4",        "4x"    },
        { "x5",        "5x"    },
        { "x8",        "8x"    },
        { "x16",       "16x"   }
    };
    static constexpr int kCount = sizeof(kEntries) / sizeof(kEntries[0]);
    static_assert(X16 + 1 == kCount, "StrandDivMult out of sync.");
    static constexpr float kFactor[kCount] = {
        1.0f / 16.0f, 1.0f / 8.0f, 1.0f / 5.0f, 1.0f / 4.0f,
        1.0f / 3.0f, 0.5f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 8.0f, 16.0f
    };
}

// ── Sequencer pattern preset ──
namespace SeqPreset {
    enum : int {
        OctaveBounce = 0,
        WideLeap = 1,
        OffBeatMinor = 2,
        GlideGroove = 3,
        SparseStab = 4,
        RisingArc = 5,
        Scatter = 6,
        Chromatic = 7,
        BassWalk = 8,
        GatedPulse = 9
    };
    static constexpr ChoiceEntry kEntries[] = {
        { "octave_bounce",  "Octave Bounce"  },
        { "wide_leap",      "Wide Leap"      },
        { "off_beat_minor", "Off-Beat Minor" },
        { "glide_groove",   "Glide Groove"   },
        { "sparse_stab",    "Sparse Stab"    },
        { "rising_arc",     "Rising Arc"     },
        { "scatter",        "Scatter"        },
        { "chromatic",      "Chromatic"      },
        { "bass_walk",      "Bass Walk"      },
        { "gated_pulse",    "Gated Pulse"    }
    };
    static constexpr int kCount = sizeof(kEntries) / sizeof(kEntries[0]);
    static_assert(GatedPulse + 1 == kCount, "SeqPreset out of sync.");
}

/**
 * One MOD envelope's block-rate parameters — ENV 2..5 on the panel.
 *
 * The amp envelope (ENV 1) is deliberately NOT one of these: it owns the VCA,
 * its Amt defaults to full rather than zero, and `computeDcaGain` treats it
 * separately. Giving it the same struct would invite a loop that silently
 * folded the VCA in with the modulators.
 */
struct ModEnvParams
{
    float attack = 0.0f, decay = 0.0f, sustain = 1.0f, release = 0.0f;
    float amount = 0.0f;
    float attackVelSens = 0.0f, decayVelSens = 0.0f, releaseVelSens = 0.0f;
    int   target = 0;                                   // EnvTarget::None
    // Stage bends on the Log … Lin … Exp axis (EnvCurve above), NOT the old 0..4
    // choice indices. Attack in [-2,+1], decay and release in [-1,+2].
    float attackBend = 0.0f, decayBend = 0.0f, releaseBend = 1.0f;
    bool  loop = false;
};

/**
 * Snapshot of all block-rate parameters, read once per processBlock from APVTS.
 * Passed to SynthVoice(s) to avoid per-voice atomic reads.
 */
struct BlockParams
{
    // Amp envelope
    float ampAttack = 0.0f, ampDecay = 0.0f, ampSustain = 1.0f, ampRelease = 0.0f;
    float ampAmount = 1.0f;
    // Global velocity → envelope-peak amount [0..1], default 1.0 (full): scales
    // EVERY envelope's note-on peak by (1−velAmt)+velAmt·velocity, so velocity
    // drives the env's depth on any target (DCA loudness, filter, pitch, scan…).
    // 0 = velocity-independent (peak 1.0). Orthogonal to per-env Amt (static depth).
    float velAmt = 1.0f;
    // Per-stage velocity sensitivity, signed [-1..+1]: velocity→stage TIME only
    // (A/D/R). 0 = no velocity effect. Velocity→peak is the global velAmt above;
    // the held level is expressed via Aftertouch, not velocity.
    float ampAttackVelSens = 0.0f, ampDecayVelSens = 0.0f, ampReleaseVelSens = 0.0f;
    int   ampTarget = EnvTarget::DCA;
    // Stage bends on the Log … Lin … Exp axis (EnvCurve above), NOT the old 0..4
    // choice indices. Attack in [-2,+1], decay and release in [-1,+2].
    float ampAttackBend = 0.0f, ampDecayBend = 0.0f, ampReleaseBend = 1.0f;
    bool  ampLoop = false;

    // Mod envelopes — ENV 2..5 on the panel, `modEnv[0]` is ENV 2. They were
    // written out one at a time (mod1*, mod2*) until 2026-07-29; the fourth and
    // fifth are what made that untenable.
    ModEnvParams modEnv[kNumModEnvs];

    // LFOs (global rates/depths for cross-mod, targets for routing).
    // `lfoNTrigMode` flips per-voice rendering to its own retriggered LFO
    // (resets on note-on); when false, voices read the shared global LFO
    // buffer and free-run.
    float lfo1Rate = 1.0f, lfo1Depth = 1.0f;
    int   lfo1Wave = 0, lfo1Target = 0; // LfoTarget::None
    bool  lfo1TrigMode = false;
    float lfo2Rate = 1.0f, lfo2Depth = 1.0f;
    int   lfo2Wave = 0, lfo2Target = 0; // LfoTarget::None
    bool  lfo2TrigMode = false;
    float lfo3Rate = 1.0f, lfo3Depth = 1.0f;
    int   lfo3Wave = 0, lfo3Target = 0; // LfoTarget::None
    bool  lfo3TrigMode = false;

    // MIDI aftertouch (channel pressure and poly pressure are resolved per
    // voice before rendering). Per-target bipolar amount: index by
    // AftertouchTarget (1..12); pressure x amount drives the target, sign sets
    // direction.
    float aftertouchTargetAmt[AftertouchTarget::kCount] = {}; // [t] = signed depth for target t
    // Which axis drives each target, or none (ExprSource). Amount
    // says how deep, this says from where. Default Z for every target: that is
    // what an amount on its own meant before sources existed.
    std::array<int, AftertouchTarget::kCount> aftertouchTargetSrc = ExprSource::defaults();

    // Filter
    bool  filterEnabled = false;
    float baseCutoff = 20000.0f;
    float baseReso = 0.0f;
    int   filterType = 0;  // FilterType index
    int   filterSlope = 0; // FilterSlope index
    float filterMix = 1.0f;
    // Pre-filter drive: user-facing controls
    float filterDriveDb = 0.0f;        // 0…36 dB, 0 = bypass
    int   filterDriveOs = FilterDriveOs::X2;  // Oversampling around tanh
    // Filter algorithm selection (SVF / Ladder / Warp) and Warp-specific style.
    int   filterAlgorithm = FilterAlgorithm::SVF;
    int   filterWarpStyle = FilterWarpStyle::Tanh;
    // Nonlinear-filter (Ladder/Warp) oversampling factor: 1=Off, 2, 4. GLOBAL
    // machine setting, not a preset param — filled in processBlock from the
    // processor's atomic. Ladder/Warp run their per-sample loop at sr×this to
    // suppress in-loop saturation aliasing; SVF (linear) ignores it. Struct
    // default 1 = safe Off; the product default (2×) is enforced by the processor.
    int   filterOsFactor = 1;
    // Pre-computed derived value (filled in processBlock, not by user):
    float filterDriveGain = 1.0f;      // 10^(driveDb/20)
    float kbdTrack = 0.0f;

    // Scan
    float baseScan = 0.0f;
    float driftScanOffset = 0.0f;

    // Drift offsets for filter and pitch (applied per-voice in SynthVoice)
    float driftFilterOffset = 0.0f;  // normalized cutoff contribution (octave-fraction), summed into the cutoff bus
    float driftPitchOffset = 0.0f;   // normalized pitch contribution (semitone-fraction), summed into the pitch bus
    float driftPitchReach  = 0.0f;   // the largest |driftPitchOffset| the current arming/depths allow, phase-independent
    float performancePitchRatio = 1.0f; // MIDI pitch bend, applied as a frequency multiplier

    // Noise oscillator
    float noiseLevel = 0.0f;      // 0-1 mix level
    int   noiseType = 0;          // NoiseType index

    // Drift Crossfade ("Regen XFade") APVTS value, mirrored each block. Sizes
    // the held-note wavetable/freeze/sampler content morph in SynthVoice/
    // VoiceManager — a bake landing on a HELD note crossfades onto the new
    // bank over this window instead of hard-cutting (CLAUDE.md held-note
    // invariant).
    float driftCrossfade = 200.0f;

    // Octave shift (-2..+2)
    int octaveShift = 0;

    // Engine
    int engineMode = EngineMode::Sampler;
    bool engineIsWavetable = false;
    bool engineIsFreeze = false;
    int freezeTexture = FreezeTexture::Silk;
    float freezeStereo = 0.25f;
    bool wtSmooth = true; // Catmull-Rom frame interpolation
    bool wtAutoScan = true;

};
