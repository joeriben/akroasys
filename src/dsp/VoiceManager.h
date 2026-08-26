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
    /** @param mpeChannel  ORIGIN, filed exactly as noteOn files it: 1-16 = a note
                            an external key struck on that member channel, and 0
                            (the default) = an INTERNAL note, the sequencers' and
                            the arpeggiator's. It is not a wildcard and 0 does not
                            mean "any" -- a note-off ends a voice of its own origin
                            and no other. Two keys of the same pitch on two member
                            channels are two notes and one key-up must not end
                            both; and a sequencer step reaching a pitch a hand is
                            holding must not end the hand's note, which is what
                            "any" used to do. Every caller that means an external
                            key therefore has to name its channel. */
    void noteOff(int note, int sourceId = -1, bool forceRelease = false, int mpeChannel = 0);
    /** End every sounding note.

        @param cutSound  true = CC 120 All Sound Off, the GUI panic button, and
               the replay transport: the notes are TAKEN AWAY, so this also
               resets the performance controllers and zeroes the stored
               expression of the tails (see resetPerformanceControllers).
               false = CC 123 All Notes Off, which the MIDI spec defines as
               every key coming up -- so it goes through the same steps a
               single key-up does, damper and sostenuto included ("notes may
               continue to sound if the damper is down"), it freezes the
               expression instead of zeroing it, and it resets no controller:
               that is CC 121's message, not this one. */
    void allNotesOff(bool cutSound = true);
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
    /** Reset the performance controllers to rest.

        @param endingEveryNote  true only from allNotesOff(cutSound = true) --
               CC 120, the panic button, the replay transport. That is TAKING
               the notes away, so it zeroes the stored pressure of every
               sounding voice, tails and pedal-held notes included -- cases 50
               and 62 hold that. CC 123 does not come here at all any more. CC 121 is not that: it resets controller
               VALUES while every note goes on sounding, so it may only reach
               the voices a live control is still allowed to move. Zeroing a
               tail there cut a decaying note off in one block instead of
               letting it fade -- measured, level 0.2851 to 0.0000 in 5.3 ms,
               and a pedal-held note from 0.629. A key still DOWN is zeroed
               either way, which is what the message asks for. */
    void resetPerformanceControllers(bool endingEveryNote = false);

    /** The physical-key ledger: told, not derived. The poly-key-pressure latch
        belongs to a FINGER, and voice state is only a proxy for that -- a proxy
        that is wrong in both directions. With the arpeggiator on, a held key
        sounds nothing between its steps, so the voices report "released" while
        the hand is still leaning in; under the damper a voice holds a pitch no
        key is on any more. Neither can the arpeggiator's own held-key list
        stand in: it is a SET, one entry per note number, so two fingers on one
        pitch -- or a controller that re-sends a note-on it never ended -- look
        exactly like one.

        What tells those two apart is the CHANNEL, which is the whole point of
        MPE: two fingers on one pitch arrive on two member channels, a
        retransmit arrives twice on one. So each note number holds a SET of the
        channels currently pressing it -- bit 0 for the computer keyboard and
        anything channel-less, bits 1..16 for the MIDI channels. The processor
        calls these at every point a physical key really moves: the two loops
        that feed the arpeggiator from raw MIDI (the only place external key
        events are seen in both arp states) and the computer keyboard's own
        entry points.

        Down: the FIRST finger on a pitch starts its reading over, because
        aftertouch begins at nothing and rises and a fresh press must not
        inherit the last one's. A second finger changes nothing, and neither
        does a note-on a controller re-sends without ever having ended the last.
        Up: the LAST finger off a pitch ends the reading -- unless a voice still
        holds it, which is the damper. */
    void noteKeyDown(int note, int midiChannel) noexcept;
    void noteKeyUp(int note, int midiChannel) noexcept;
    /** noteKeyDown from a pass that reads a whole buffer before any of it is
        walked -- which is where the arpeggiator's branch has to keep the ledger,
        because with the arp on the note events never reach the walk at all.

        The key event itself lands at once, mask and reset together: the arp can
        emit a step for this very key in this same block, and a note-on is seeded
        from the reading, so a reset that waits arrives after the note it was for.
        What cannot be answered here is the GATE -- whether an aftertouch message
        elsewhere in the same buffer belongs to this press or to the one before
        it. The pass does carry sample positions, but it runs to completion
        before the walk starts, so the comparison the gate needs is not available
        at the moment the mark is set. So a FIRST finger marks the note and every
        reading for it is refused for the rest of the block. That costs one
        buffer of a fresh press's aftertouch; letting a stale one through costs
        the whole note. A key that was already down is not marked: nothing there
        is ambiguous. */
    void noteKeyDownBuffered(int note, int midiChannel) noexcept;
    /** Clears the buffered-press marks. Once per block, before any key event. */
    void beginBlockKeyEvents() noexcept;
    /** Every key up at once: panic, editor focus loss, replay takeover. */
    void allKeysReleased() noexcept;

    // MPE: route pitch-wheel on a per-note channel to the voice(s) triggered on it.
    /** MPE X on one member channel. Two numbers for one gesture: `semitones` is
     *  the bend (range already applied) and moves the pitch; `normalised` is how
     *  far the wheel travelled, ±1 at full deflection, and is what the expression
     *  matrix can route to a target. See SynthVoice::setPerVoicePitchBend. */
    void setPerVoicePitchBend(int midiChannel, float semitones, float normalised);
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
    /** CC 7 * CC 11 * the soft pedal -- what every output sample is
        multiplied by. Public for the same reason globalPitchBendRatio is:
        the corpus has to be able to read it. */
    float performanceOutputGain() const;

    // ── Engine data distribution ──
    void setEngineMode(SynthVoice::EngineMode mode);
    // allowMorph=true lets a HELD sampler voice crossfade-follow the new snapshot
    // (morphToBufferFrom, morphMs = Drift Crossfade) so a sustained note plays the
    // freshly generated sample during A/B-drift regenerate. The sampler morph runs
    // on the AUDIO THREAD (the reader) — so, opposite to distributeFreezeBuffer,
    // allowMorph MUST be true ONLY at the audio-thread call site; off-thread callers
    // pass false and leave held voices to crossfade on the next audio block.
    void distributeSamplerBuffer(const SamplePlayer& master, float morphMs, bool allowMorph,
                                 bool onAudioThread);
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
    void distributeFreezeBuffer(const FreezeTextureEngine& masterFreeze, float morphMs, bool allowMorph,
                                bool onAudioThread);

    // ── Per-note engine data (cache position per voice) ──
    // MPE means per note, and a cache position is a whole sample. So a voice can
    // be pointed at a master OF ITS OWN instead of the instrument-wide one the
    // three distribute functions above are handed: two fingers at two lateral
    // positions play two different samples, which one bar and one master cannot
    // express. nullptr - the default, and what every voice is born with - means
    // "follow the instrument-wide master", i.e. exactly the behaviour before
    // this existed.
    //
    // The pointer is to a master owned by the PROCESSOR for the lifetime of the
    // plug-in (T5ynthProcessor::cachePosEngines_), never to a temporary: the
    // audio thread dereferences it on every block, and the three distribute
    // functions morph from it under the same snapshot discipline they use for
    // the instrument-wide master. A master still being prepared is simply not
    // handed over - the voice keeps what it has until it is ready.
    void setVoiceEngineMasters(int voice,
                               const SamplePlayer* sampler,
                               const WavetableOscillator* osc,
                               const FreezeTextureEngine* freeze);
    /** Back to the instrument-wide master. Called when a voice is taken for a
     *  new note, and whenever the per-note path stops being in charge. */
    void clearVoiceEngineMasters(int voice);
    /** Give EVERY voice back to the instrument-wide masters. The next distribute
     *  pass then crossfades each held note onto whatever they now hold, over the
     *  Regen XFade - it does not swap. Called wherever the instrument-wide sound
     *  is replaced (a regenerate, a preset, a cache the positions no longer
     *  describe), because a held note plays the CURRENT sample and a position
     *  claim by the hand does not outrank that. */
    void clearAllVoiceEngineMasters();
    /** True while any voice follows a master of its own. */
    bool hasVoiceEngineMasters() const
    {
        // The sampler slot is the claim marker: setVoiceEngineMasters' only two
        // callers set all three pointers or clear all three, never a mixture.
        for (const auto& m : voiceSamplerMaster_)
            if (m.load(std::memory_order_relaxed) != nullptr)
                return true;
        return false;
    }
    /** Held GRANULAR voices onto the per-note masters the audio thread pointed
     *  them at. Off the audio thread only, and for the same reason
     *  distributeFreezeBuffer's allowMorph is: FreezeTextureEngine::
     *  morphToBufferFrom frees the snapshots the previous morph retired, which
     *  must never happen on the audio thread. Wavetable and Sampler need no
     *  counterpart - both of those morph on the audio thread's own distribute
     *  pass, which is where their contract puts them.
     *  Idempotent: the engine's own generation guard makes a repeat a no-op. */
    void morphHeldFreezeVoicesToOwnMasters(float morphMs);
    /** The member channel whose expression this voice answers, 0 for none. The
     *  channel is the FINGER: an MPE controller rotates its members, and the
     *  mono legato path hands one voice from one finger to the next without a
     *  fresh strike, so this is what tells them apart when triggerEpoch cannot. */
    int voiceExprChannel(int voice) const
    {
        return (voice >= 0 && voice < MAX_VOICES)
             ? static_cast<int>(voiceExprChannel_[static_cast<size_t>(voice)]) : 0;
    }
    /** Which sampler master this voice follows, nullptr for the instrument-wide
     *  one. The observable that says whether an expression target acts per note:
     *  two held keys leaning differently must not come back with one pointer. */
    const SamplePlayer* voiceSamplerMaster(int voice) const
    {
        return (voice >= 0 && voice < MAX_VOICES)
             ? voiceSamplerMaster_[static_cast<size_t>(voice)].load(std::memory_order_relaxed)
             : nullptr;
    }

    // ── Query ──
    int getActiveVoiceCount() const;
    bool hasActiveVoices() const;
    /** Keys actually DOWN. Unlike hasActiveVoices(), which stays true for the
     *  whole release tail, this goes to zero the moment the player lets go. */
    int getHeldVoiceCount() const;
    /** Voices held by an external KEY that is still down - the damper pedal,
     *  sostenuto, the drone and everything the sequencers and the arpeggiator
     *  play excluded. getHeldVoiceCount() counts those too, which is right for
     *  voice allocation and wrong for anything asking whether a hand is on the
     *  keyboard. Computer-keyboard notes count: they carry no MIDI channel and
     *  are recognised by their source id instead. */
    int getKeyHeldVoiceCount() const;
    /** The key that is still DOWN and leaning hardest on one expression axis
     *  (ExprSource), 0 when none is. What a target reads when it acts on the
     *  whole instrument instead of on one voice: MPE gives every note its own
     *  expression, and the instrument can only be in one place, so the note
     *  leaning hardest is the one that moves it. Over the same voices as
     *  getKeyHeldVoiceCount(). Sign is kept, because X leans both ways. */
    float maxHeldExpression(int src) const;
    /** Is THIS voice held by a key that is still down? The per-voice form of
     *  getKeyHeldVoiceCount(), over exactly the same voices. */
    bool isVoiceKeyHeld(int voiceIdx) const { return isKeyHeldVoice(voiceIdx); }
    /** ONE voice's own reading on one expression axis (ExprSource) - what
     *  maxHeldExpression folds away. MPE gives every note its own expression,
     *  and a target that acts PER NOTE has to read the note rather than the
     *  instrument. Sign is kept, for the same reason: X leans both ways.
     *  Out-of-range or silent voices read rest. */
    float voiceExpression(int voice, int src) const;
    /** Voice source id the computer keyboard plays under. Above every sequencer
     *  strand on purpose, so such a voice can be told apart from an internal one. */
    static constexpr int kComputerKeyboardSourceId = 15;
    /** The pressure reaching one note number, whether or not it currently has a
     *  voice - channel pressure, mod wheel, breath, or its own poly pressure.
     *  For sources that hold keys without holding voices, the arpeggiator above
     *  all: between its steps nothing is sounding, and a hand pressing into the
     *  chord it is playing has to be readable all the same. */
    float pressureForHeldNote(int note) const { return pressureForNote(note); }

    /** May a LIVE performance control -- the wheel, the breath, zone-wide or
        poly pressure -- still move this voice's stored pressure?

        Two kinds of voice, and the answer is different for each:
          - a voice a HAND started follows only while that hand's key is down.
            Once the key comes up the pressure is frozen at what the key left,
            through release, damper and sostenuto alike.
          - a voice with no hand behind it -- the sequencers', the arpeggiator's,
            the drone's -- follows for its whole sounding life. Those notes never
            had a finger to lose, and the wheel is the only thing that drives
            them at all.

        Written as ONE question because the previous form asked three proxies
        (isReleasing, sustainedVoice, sostenutoReleasedVoice) that stand for "a
        key came up" only when a key existed. sustainedVoice is set for every
        caller with sourceId < 0 -- which is the step sequencer and the
        arpeggiator as much as it is external MIDI -- so under the damper an
        arpeggio froze at whatever the wheel last held, and with aftertouch ->
        DCA the whole pedalled stack sat at full level with the wheel down.

        It reads voiceStartedByHand_ rather than deriving the answer from
        voiceMidiChannel_ and voiceSourceId, and the first version of it did
        derive: a panic then handed every dying note straight back to the wheel.
        allNotesOff wipes voiceMidiChannel_ mid-tail on purpose -- so a panic's
        own dying notes stop obeying a finger that is still resting on a key --
        and that wipe also erased the only evidence a hand had ever been there,
        turning each panicked tail into "a voice with no hand behind it", which
        follows live controls by definition. Touch the wheel after a panic and,
        with aftertouch -> DCA, the stack just killed came back at full level
        for the length of its release, up to ten seconds; a DAW sends that panic
        on transport stop, with the hands still down. Whether a HAND started a
        voice is a fact about its ORIGIN, so it gets a field that says exactly
        that and that no controller reset clears. */
    bool followsLivePressure(int i) const
    {
        if (isKeyHeldVoice(i))
            return true;
        // A voice with no hand follows for its whole sounding life -- but a note
        // that has been ENDED is not sounding, it is dying, and the boundary is
        // the same one a key-up draws for a hand. Without this term a panic left
        // every sequencer, arpeggiator and drone tail following the wheel: CC123
        // (what a DAW sends on transport stop) cut the line, and the next wheel,
        // breath or channel-pressure move brought all of it back from silence to
        // full level for the length of the release -- measured, four voices at
        // once, still ringing 2.1 s later. The hand half of that was fixed and
        // this half was not, which made the rule true of hands only.
        //
        // Not isReleasing() alone: a note the damper is holding has had its
        // gate-off too but is still SOUNDING at full, and that one must keep
        // following. isReleasing() is false there, so it does.
        return ! voiceStartedByHand_[static_cast<size_t>(i)]
            && ! voices[static_cast<size_t>(i)].isReleasing();
    }

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

    // Per-note override of the three above. nullptr = follow the instrument-wide
    // master. See setVoiceEngineMasters.
    // ATOMIC, because these cross threads in BOTH directions: written on the
    // audio thread (updateVoiceCachePositions, noteOn, renderBlock) and read -
    // and dereferenced - off it, by morphHeldFreezeVoicesToOwnMasters on the
    // message thread and by the position builder's own distribute sweep. On
    // CLAP getCallbackLock() does not exclude the audio thread, so the lock
    // those readers take is not what holds; these are. Relaxed is enough: the
    // audio DATA behind each pointer is published by the engine's own
    // acquire/release snapshot pair, and the slots themselves outlive both.
    std::array<std::atomic<const SamplePlayer*>, MAX_VOICES>        voiceSamplerMaster_ {};
    std::array<std::atomic<const WavetableOscillator*>, MAX_VOICES> voiceOscMaster_ {};
    std::array<std::atomic<const FreezeTextureEngine*>, MAX_VOICES> voiceFreezeMaster_ {};
    // No side table saying WHICH voices carry one - neither a count nor a
    // bitmask. Both were tried and both are the same mistake: a second variable
    // that has to be kept in step with the pointers by a separate atomic op,
    // which two threads acting on the same voice can leave disagreeing in
    // either direction (claims standing that read as none, or none standing
    // that read as claims). hasVoiceEngineMasters() reads the pointers
    // themselves, so there is nothing to disagree with. 128 relaxed loads,
    // three times a block - tens of nanoseconds against a 10 ms block.
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
    // Did a HAND start this voice -- an external MIDI key, or the computer
    // keyboard? Set once where the voice is allocated, cleared when the slot is
    // freed, and never cleared out from under a voice that is still SOUNDING --
    // not by allNotesOff, not by resetPerformanceControllers, both of which
    // wipe voiceMidiChannel_ mid-tail on purpose. A panic ends the notes; it
    // does not retroactively unmake the hand that played them. (The idle-slot
    // loop in resetPerformanceControllers does clear it, alongside the two
    // channel tags, for slots that hold nothing.) followsLivePressure is the
    // reader.
    std::array<bool, MAX_VOICES> voiceStartedByHand_ {};
    std::array<int8_t, MAX_VOICES> voiceMidiChannel_ {};  // 0=unassigned, 1-16=MIDI channel

    // Which voice a member channel's EXPRESSION reaches. A second field and not
    // a reuse of the one above, because that one carries a second meaning:
    // ORIGIN, so a step-seq bind cannot continue a held external note (noteOn's
    // originMatches, parity capability 24). Clearing it on hand-off would allow
    // exactly that.
    //
    // The defect this closes: voiceMidiChannel_ falls only when a voice goes
    // silent (renderBlock), so a RELEASING or sustained voice keeps its tag. An
    // MPE controller rotates its member channels, and with a normal release time
    // a channel comes round again while the previous voice on it is still
    // audible -- the new key's pressure then also drove the old, dying note. With
    // AT->DCA a released note swelled back up; with AT->Cutoff it brightened
    // again. Poly-AT never showed it because it matches by NOTE NUMBER, which is
    // the whole reason PolyAT mode behaved and MPE mode did not.
    //
    // 0 = this voice answers to no channel. A voice that loses the channel keeps
    // the pressure, bend and timbre it last had, frozen -- which is what a
    // released note should do.
    std::array<int8_t, MAX_VOICES> voiceExprChannel_ {};

    /** Give voice `voiceIndex` the expression channel `channel`, taking it off
        every other voice that still holds it. Called wherever a voice is tagged
        with a MIDI channel; `channel` 0 simply clears this voice. */
    void claimExprChannel(int voiceIndex, int8_t channel) noexcept;

    /** Drop `note`'s poly-key-pressure latch unless something still holds that
        pitch. Called wherever a hold ends -- the note-off message, each of the
        three paths that release a voice directly (both pedals and the drone),
        and the allocation paths that re-purpose a held voice instead of
        releasing it -- because the latch outlives any of them that forgets it.

        It asks TWO ledgers, because neither alone is the answer. A voice can
        hold a pitch with no key down (the damper), and a key can hold a pitch
        with no voice at all (the arpeggiator between its steps). Clearing on
        either half alone is a bug in one of those two directions.

        Not public: a key-up goes through noteKeyUp, which owns the count this
        consults. `ignoreVoice` is for the allocation sites, where the voice being asked
        about is the one being taken away from that pitch: it still reports the
        old note at the moment the question has to be asked, because the answer
        decides what the NEW note is seeded with. */
    void clearPolyPressureIfReleased(int note, int ignoreVoice = -1) noexcept;
    std::array<float, MAX_VOICES> voiceMpePressure_ {};   // MPE per-note Z (member-channel pressure)
    // Last CC 74 seen per MIDI channel (1..16; index 0 unused). Not a voice
    // property: a note's Y rest is the value in force when it STARTED, and the
    // controller sends that before the note-on, when no voice holds the channel.
    std::array<float, 17> channelTimbre_ {};
    float channelTimbreFor(int midiChannel) const
    {
        return (midiChannel >= 1 && midiChannel <= 16)
                 ? channelTimbre_[static_cast<size_t>(midiChannel)]
                 : SynthVoice::kTimbreRest;
    }
    std::array<bool, MAX_VOICES> sustainedVoice {};
    std::array<bool, MAX_VOICES> sostenutoVoice {};
    std::array<bool, MAX_VOICES> sostenutoReleasedVoice {};
    std::array<float, 128> polyPressureByNote {};
    // Which channels are pressing each note number -- see noteKeyDown. Nothing
    // here can derive it: with the arpeggiator on, an external note on/off
    // never reaches these voices at all, and the arp's own step note-offs carry
    // sourceId -1, indistinguishable from a key-up by every field noteOff
    // receives. Bit 0 = channel-less (the computer keyboard), bits 1..16 = the
    // MIDI channels. keysDown_ counts the non-empty entries, kept only so a
    // panic can skip the sweep when nothing was down.
    std::array<uint32_t, 128> keyDownChannels_ {};
    int keysDown_ = 0;
    // Note numbers that were pressed in a pass that could not say WHEN -- see
    // noteKeyDownBuffered. Every reading for them is refused for the rest of
    // the block. freshPresses_ is the count, so the usual empty block clears
    // nothing.
    std::array<bool, 128> bufferedPress_ {};
    int freshPresses_ = 0;
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
    bool  isKeyHeldVoice(int voiceIdx) const;
    // Effective Z for a voice = max(its note's aggregate pressure, its own MPE
    // member-channel pressure). Keeps per-note Z independent of zone-wide pressure.
    float pressureForVoice(int voiceIdx) const;
    BlockParams applyPerformanceControllers(const BlockParams& bp) const;
    static constexpr float GAIN_RAMP_MS = 5.0f;
};
