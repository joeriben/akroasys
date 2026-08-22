#pragma once
#include "SynthVoice.h"
#include "BlockParams.h"
#include <array>
#include <cmath>

// Forward-declared only — VoiceManager only ever holds/passes a raw pointer
// (CsoundEngine::voiceBuffer / ::kMaxVoices), so the full CsoundEngine.h
// (and its conditional real-vs-stub Impl) stays confined to VoiceManager.cpp,
// mirroring how this header stays JUCE/Csound-agnostic elsewhere.
class CsoundEngine;

/**
 * Polyphonic voice manager — 8 voices with tiered voice stealing
 * (releasing-oldest first, then lowest-amplitude held voice).
 *
 * Signal chain: MIDI → Voice allocation → per-voice (Osc→VCA→Filter) → sum
 * Dynamic equal-power scaling: each voice at 1/sqrt(N) where N = active voices.
 * Gain transitions are ramped over ~5ms to avoid clicks.
 */
class VoiceManager
{
public:
    static constexpr int MAX_VOICES = 128;

    VoiceManager() = default;

    void prepare(double sampleRate, int samplesPerBlock);
    void reset();
    void setBlockParams(const BlockParams& bp);

    // ── MIDI handling ──
    // mpeChannel: 0 = internal note (sequencer/arp, never MPE-tracked);
    //             1-16 = external controller channel (tagged for per-note MPE).
    void noteOn(int note, float velocity, bool isBind, float glideMs,
                bool lfo1TrigMode, bool lfo2TrigMode, bool lfo3TrigMode,
                int sourceId = -1, float pan = 0.0f, int mpeChannel = 0);
    // forceRelease: bypass the sustain/sostenuto hold. For a note the synth is
    // TAKING AWAY rather than a key being lifted — the arpeggiator switching on
    // over a held chord, where "the pedal is down, keep ringing" would leave the
    // raw chord drone under the arpeggio. Ordinary key-lifts leave it false.
    void noteOff(int note, int sourceId = -1, bool forceRelease = false);
    void allNotesOff();
    void setSustainPedal(bool down);
    void setSostenutoPedal(bool down);
    void setSoftPedal(bool down);
    void setPitchBendSemitones(float semitones);
    void setModWheel(float value);
    void setBreathController(float value);
    void setExpression(float value);
    void setChannelVolume(float value);
    void setChannelPressure(float pressure);
    void setPolyPressure(int note, float pressure, int sourceId = -1);
    void resetPerformanceControllers();

    // MPE: route pitch-wheel on a per-note channel to the voice(s) triggered on it.
    void setPerVoicePitchBend(int midiChannel, float semitones);
    // MPE Loudness (Z): channel pressure on a member channel drives only the
    // voice(s) tagged with that channel, not the whole zone.
    void setChannelPressureForChannel(int midiChannel, float pressure);
    // MPE Timbre (Y, CC74): route to the voice(s) tagged with that channel.
    void setTimbre(int midiChannel, float value);

    // ── Drone (step-hold) handling ──
    // A drone is a user-held note (e.g. mouse-hold on a sequencer step) that
    // reserves a voice for as long as the user holds. In mono the drone takes
    // over voice 0 and suppresses seq noteOns while held. In poly the drone's
    // voice is excluded from voice stealing and same-note matching by the seq
    // path, so seq triggers happen in parallel on other voices.
    void setDroneNote(int note, float velocity, bool lfo1TrigMode, bool lfo2TrigMode, bool lfo3TrigMode);
    void clearDroneNote();
    bool hasDrone() const { return droneVoiceIndex >= 0; }
    int  getDroneNote() const { return droneNote; }

    // ── Per-block rendering ──
    struct VoiceOutput {
        float lastAmpVal = 0.0f;
        float lastModVal[kNumModEnvs] = {};   // ENV 2..5, in panel order
        float lastModulatedCutoff = 20000.0f;
        float lastModulatedResonance = 0.0f;
        float lastModulatedScan = 0.0f;
        float lastModulatedNoiseLevel = 0.0f;
        int   lastTriggeredNote = -1; // for pitch modulation
        bool  hasActiveVoices = false;
    };

    /** Render all active voices into buffer (summed with 1/sqrt(N) scaling).
     *  Global LFOs are ticked externally; their per-sample values are passed in.
     *  startSample: offset into the output buffer (for sample-accurate rendering).
     *  csoundVoiceBufs: per-voice block-aligned Csound render buffers (Phase-2 spec
     *  S7), entry vi = the buffer SynthVoice vi reads (nullptr entry = silence for
     *  that voice); VoiceManager adds startSample itself, exactly as before. The
     *  array pointer itself is nullptr in every non-Csound mode / not-yet-ready
     *  state — the whole bridge is then inert. The processor builds this array
     *  from the active engine's own voiceBuffer()s during normal play, or from its
     *  csoundMixBufs_ while crossfading to a new orchestra (S5). */
    VoiceOutput renderBlock(juce::AudioBuffer<float>& buffer, const BlockParams& bp,
                            const float* lfo1Buf, const float* lfo2Buf, const float* lfo3Buf,
                            int startSample, int numSamples, const float* const* csoundVoiceBufs);

    /** Back-compat single-engine convenience (Phase-1 call sites, e.g.
     *  tools/audition_csound_engine.cpp): builds the per-voice pointer array from
     *  `cs` (nullptr = no csound) and forwards to the array-based overload above. */
    VoiceOutput renderBlock(juce::AudioBuffer<float>& buffer, const BlockParams& bp,
                            const float* lfo1Buf, const float* lfo2Buf, const float* lfo3Buf,
                            int startSample, int numSamples, const CsoundEngine* cs = nullptr);

    // Csound voice bridge (Phase-1 spec §3, D2/D8): publishes every active
    // voice's current state (gate/freq/vel/pres/timb/trig-epoch) to the
    // engine's cached channel pointers. Called by the processor right before
    // each csoundEngine.renderUpTo() sub-call, so the orchestra always renders
    // from control values that reflect every MIDI event dispatched so far this
    // block (D2's on-demand pump timing). Only voices 0..kMaxVoices-1 have a
    // Csound instrument/channel set at all (Phase-1 fixed 16-voice orchestra,
    // D1); voices beyond that are simply not written here (they keep rendering
    // silently via SynthVoice's null-csoundBuf_ safety net until they end).
    void writeCsoundControls(CsoundEngine& cs, float performancePitchRatio, int samplesSinceLastWrite);
    // Phase-2 (spec S6): computes each voice's CURRENT control values ONCE
    // (advancing the per-voice glide smoother exactly once, regardless of how
    // many engines are being fed), then applies them to every engine in
    // `engines` — numEngines is 1 during normal play, 2 while crossfading to a
    // new orchestra (S5). Calling the single-engine overload above twice per
    // write point would advance the glide smoother twice as fast — the
    // "double-advance" trap S6 warns about — so the fade path MUST go through
    // this overload instead.
    //
    // `modParams` carries the PITCH MODULATION BUS (LFO/env/drift/aftertouch
    // routed to Pitch) into the published freq, together with the three RAW
    // global LFO samples at this write point. It is optional so that offline
    // audition tools can drive the bridge with no modulation at all -- passing
    // nullptr publishes bends only, which is exactly what this bridge did for
    // every caller before, and is why vibrato never reached the orchestra.
    void writeCsoundControls(CsoundEngine* const* engines, int numEngines,
                             float performancePitchRatio, int samplesSinceLastWrite,
                             const BlockParams* modParams = nullptr,
                             float lfo1Raw = 0.0f, float lfo2Raw = 0.0f, float lfo3Raw = 0.0f);
    // Phase-2 (spec S3/S4): read-only snapshot of every voice's current trigger
    // epoch + effective Csound frequency, for CsoundEngine::primeForTakeover.
    // Does NOT advance the glide smoother (readCsoundFreq(0) is a pure peek —
    // see its own header comment). Callers off the audio thread MUST hold
    // getCallbackLock() first, exactly like every other message-thread
    // voice-state reader in this class (distributeSamplerBuffer et al.).
    void snapshotCsoundState(float epochsOut[], float freqsOut[], float performancePitchRatio);
    // The GLOBAL pitch-bend wheel as a frequency multiplier — the same value
    // applyPerformanceControllers() writes into BlockParams::performancePitchRatio
    // for the internal engines. Exposed because processBlock's own `bp` NEVER
    // carries it: performancePitchRatio is assigned only on the COPY that
    // applyPerformanceControllers returns, so `bp.performancePitchRatio` sits at
    // its 1.0f default forever. The Csound bridge read that default from Phase 1
    // onward, which meant the pitch WHEEL never reached the orchestra at all
    // (per-voice MPE bend did — it comes from the voice, not from bp). Callers
    // on the audio thread pass this instead of bp's dead field; the accessor
    // exists so they need not copy the whole BlockParams per MIDI sub-segment.
    float globalPitchBendRatio() const { return std::exp2(pitchBendSemitones / 12.0f); }

    // ── Engine data distribution ──
    void setEngineMode(SynthVoice::EngineMode mode);
    // allowMorph=true lets a HELD sampler voice crossfade-follow the new snapshot
    // (morphToBufferFrom, morphMs = Drift Crossfade) so a sustained note plays the
    // freshly generated sample during A/B-drift regenerate. The sampler morph runs
    // on the AUDIO THREAD (the reader) — so, opposite to distributeFreezeBuffer,
    // allowMorph MUST be true ONLY at the audio-thread call site; off-thread callers
    // pass false and leave held voices to crossfade on the next audio block.
    void distributeSamplerBuffer(const SamplePlayer& master, float morphMs, bool allowMorph);
    // Release the per-voice sampler reclaim slots populated by morphToBufferFrom.
    // Off-thread only; sequence before the master republishes its snapshot.
    void drainRetiredSamplerSnapshots();
    // A HELD wavetable-mode voice crossfade-adopts the new bank (morphToFramesFrom),
    // an inactive voice shares it immediately; an empty master is a safe no-op.
    void distributeWavetableFrames(const WavetableOscillator& masterOsc);
    // allowMorph=true lets a HELD granular voice crossfade-adopt the new buffer
    // (morphToBufferFrom, morphMs = Drift Crossfade) instead of clinging to the
    // old one. MUST be false at audio-thread call sites — morphToBufferFrom may
    // release a retired snapshot off-thread and must never run on the audio
    // thread. false reproduces the legacy "keep old buffer on held voices".
    void distributeFreezeBuffer(const FreezeTextureEngine& masterFreeze, float morphMs, bool allowMorph);

    // ── Query ──
    int getActiveVoiceCount() const;
    bool hasActiveVoices() const;
    /** Keys actually DOWN. Unlike hasActiveVoices(), which stays true for the
     *  whole release tail, this goes to zero the moment the player lets go. */
    int getHeldVoiceCount() const;
    /** The hardest-pressed sounding voice, 0 when nothing sounds. What a target
     *  reads when it acts on the whole instrument instead of on one voice: MPE
     *  gives every note its own pressure, and the instrument can only be in one
     *  place, so the note leaning hardest is the one that moves it. */
    float maxSoundingPressure() const;

    /** Set voice limit at runtime (1=mono, 4/6/8/12/16). */
    void setVoiceLimit(int limit) { voiceLimit = juce::jlimit(1, MAX_VOICES, limit); }
    int getVoiceLimit() const { return voiceLimit; }

    /** Set tuning table pointer (128 floats, MIDI note → Hz). Must be called before noteOn/renderBlock. */
    void setTuningTable(const float* table) { tuningHz_ = table; }
    const float* getTuningTable() const { return tuningHz_; }
    bool isMono() const { return voiceLimit == 1; }

    SynthVoice& getVoice(int index) { return voices[static_cast<size_t>(index)]; }
    const SynthVoice& getVoice(int index) const { return voices[static_cast<size_t>(index)]; }

private:
    std::array<SynthVoice, MAX_VOICES> voices;

    // Monotonic counter for voice-stealing age
    uint64_t noteOnCounter = 0;

    // Gain ramping for voice count changes
    float currentGain = 1.0f;
    float targetGain = 1.0f;
    float gainRampIncr = 0.0f;
    int   gainRampSamplesLeft = 0;

    double sr = 44100.0;
    int maxBlockSize = 512;
    int voiceLimit = 8; // runtime polyphony (1=mono)
    const float* tuningHz_ = nullptr;
    const SamplePlayer* currentSamplerMaster_ = nullptr;
    const WavetableOscillator* currentWavetableMaster_ = nullptr;
    const FreezeTextureEngine* currentFreezeMaster_ = nullptr;
    BlockParams currentBlockParams_;
    bool hasCurrentBlockParams_ = false;

    // ── Drone (step-hold) reservation ──
    int droneVoiceIndex = -1;    // -1 = no drone active
    int droneNote       = -1;    // last pitch the drone was set to

    // Pre-allocated per-voice scratch buffers
    std::array<std::vector<float>, MAX_VOICES> voiceScratch;
    std::array<std::vector<float>, MAX_VOICES> voiceScratchRight;
    // ONE buffer for the whole pool: the DCA curve a voice analyses for the
    // sampler's pre-stretch normalization. That analysis is on the audio thread,
    // so it may not allocate — and it may not be per-voice either, because it is
    // clamped to 3 s and the pool is MAX_VOICES deep (70 MB at 48 kHz for
    // something only one voice touches at a time). Every configureForBlock call
    // site is under the processor's callback lock, so there is exactly one
    // writer. prepare() sizes it and lends it to each voice.
    std::vector<float> dcaAnalysisScratch;
    std::array<float, MAX_VOICES> voicePan {};
    std::array<int, MAX_VOICES> voiceSourceId {};
    std::array<int8_t, MAX_VOICES> voiceMidiChannel_ {};  // 0=unassigned, 1-16=MIDI channel
    std::array<float, MAX_VOICES> voiceMpePressure_ {};   // MPE per-note Z (member-channel pressure)
    std::array<bool, MAX_VOICES> sustainedVoice {};
    std::array<bool, MAX_VOICES> sostenutoVoice {};
    std::array<bool, MAX_VOICES> sostenutoReleasedVoice {};
    std::array<float, 128> polyPressureByNote {};
    float channelPressure = 0.0f;
    float modWheelPressure = 0.0f;
    float breathPressure = 0.0f;
    float expressionGain = 1.0f;
    float channelVolumeGain = 1.0f;
    float pitchBendSemitones = 0.0f;
    bool sustainPedalDown = false;
    bool sostenutoPedalDown = false;
    bool softPedalDown = false;

    // ── Voice allocation ──
    int findFreeVoice() const;
    int stealVoice() const; // tiered: releasing-oldest first, then lowest-amplitude

    void updateGainTarget();
    void releaseSustainedVoices();
    void releaseSostenutoVoices();
    void refreshPerformancePressure();
    float pressureForNote(int note) const;
    // Effective Z for a voice = max(its note's aggregate pressure, its own MPE
    // member-channel pressure). Keeps per-note Z independent of zone-wide pressure.
    float pressureForVoice(int voiceIdx) const;
    float performanceOutputGain() const;
    BlockParams applyPerformanceControllers(const BlockParams& bp) const;
    static constexpr float GAIN_RAMP_MS = 5.0f;
};
