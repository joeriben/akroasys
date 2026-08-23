#pragma once
#include <JuceHeader.h>
#include "../dsp/VoiceEvent.h"
#include <vector>
#include <array>

/**
 * Arpeggiator — a held-note arpeggiator.
 *
 * The source is the set of keys currently DOWN (computer keyboard or external
 * MIDI): they are cycled, across the selected octave range, in the selected
 * pattern order. Hold a chord and it arpeggiates the chord; hold one key and it
 * arpeggiates that key across the octaves. When the last key lifts, the arp
 * falls silent — an arpeggiator with nothing held plays nothing.
 *
 * The running sequencer may ALSO feed the arp, as a stand-in for "nothing is
 * held": its lead note becomes a single-note source (setSeqLead). Held keys
 * always win over it, and clearSeqLead() — which the processor calls whenever
 * the transport is not running — removes it again. That, not the arp's own
 * state, is what makes stopping the sequencer stop the arp.
 *
 * Key tracking is unconditional: noteOn/noteOff are called whether or not the
 * arp is switched on, so switching it on mid-hold arpeggiates what is already
 * down instead of waiting for the next key press.
 *
 * Musical rate divisions: 1/4, 1/8, 1/16, 1/32, 1/4T, 1/8T, 1/16T.
 */
class T5ynthArpeggiator
{
public:
    enum class Mode { Up, Down, UpDown, Random };

    /** Rate division indices (matching APVTS choice order) */
    enum RateDiv { Rate_1_4 = 0, Rate_1_8, Rate_1_16, Rate_1_32, Rate_1_4T, Rate_1_8T, Rate_1_16T };
    static constexpr float RATE_FACTORS[] = { 1.0f, 0.5f, 0.25f, 0.125f, 2.0f/3.0f, 1.0f/3.0f, 1.0f/6.0f };
    static constexpr int NUM_RATES = 7;

    T5ynthArpeggiator() = default;

    void prepare(double sampleRate, int samplesPerBlock);
    /** Append this block's arpeggiated note events (typed, not MIDI) to `out`. */
    void processBlock(juce::AudioBuffer<float>& buffer, std::vector<VoiceEvent>& out);
    void reset();

    void setMode(Mode m);
    void setRate(int rateIndex) { rate = juce::jlimit(0, NUM_RATES - 1, rateIndex); }
    void setOctaveRange(int octaves);
    void setBpm(double b) { bpm = b; }
    void setGate(float g) { gate = juce::jlimit(0.1f, 1.0f, g); }
    void setShuffle(float amount) { shuffle = juce::jlimit(0.0f, 0.75f, amount); }

    // ── Held keys: the arpeggiator's source ────────────────────────────────
    /** One key that is physically down. `sourceId`/`mpeChannel` are carried
        verbatim so the processor can hand the key back to the voices — with the
        id its eventual note-off will use — when the arp is switched off while it
        is still held. */
    struct HeldKey
    {
        int note = 60;
        float velocity = 0.8f;
        int sourceId = -1;      // -1 = external MIDI, else the caller's voice source id
        int mpeChannel = 0;     // external MIDI channel (0 for internal sources)
    };

    /** A key went down. Identity is the pitch AND the channel it arrived on --
        the same distinction the pressure ledger draws, and for the same reason:
        two fingers on one key are two keys, while the same key re-sent on one
        channel is one. Re-pressing a key that is already down only refreshes its
        velocity — the pattern (and the running clock) stay put. */
    void noteOn(int midiNote, float velocity, int sourceId, int mpeChannel = 0);
    /** A key came up, named by pitch and channel. The arp stops when the last
        one lifts. Keyed by pitch alone, the first of two fingers sharing a key
        took the entry with it, and the arp then handed nothing back for a key
        that was still down: silence under a pressed key until it was released
        and pressed again. */
    void noteOff(int midiNote, int mpeChannel);
    /** Every key up — panic, editor focus loss, replay takeover. */
    void allKeysUp();
    bool hasHeldKeys() const { return ! heldNotes.empty(); }
    /** Held keys in ascending pitch order. The processor needs them at the arp's
        on/off edges: switching on, to release the voices they were sounding;
        switching off, to hand them back to the voices. */
    const std::vector<HeldKey>& getHeldKeys() const { return heldNotes; }

    // ── Sequencer lead: source of last resort, only while the transport runs ─
    /** The lead note the running sequencer just emitted. Used only while no key
        is held. */
    void setSeqLead(int midiNote, float velocity);
    /** Drop the sequencer lead (transport stopped, or the arp switched off). */
    void clearSeqLead();

    /** Stop emitting and drop the timing state, but KEEP the held keys — they
        are physical key state the arpeggiator does not own. Used when the arp is
        switched off; switching it back on picks the held keys straight up. */
    void suspend();

    /** Append a note-off for any currently-sounding arp note to `out` at
        `sampleOffset`, then clear lastPlayedNote. Does not alter source or
        pattern state — safe to call unconditionally at any transition. */
    void allNotesOff(std::vector<VoiceEvent>& out, int sampleOffset = 0);

    bool isActive() const { return active; }
    int getLastPlayedNote() const { return lastPlayedNote; }

private:
    struct PatternStep { int note = 60; float velocity = 0.8f; };

    // Upper bounds so every rebuild stays inside reserved capacity: the audio
    // thread rebuilds the pattern and must never allocate.
    // One per MIDI note was the old identity; the channel is part of it now, so
    // this is a cap on fingers rather than on pitches. Far above ten either way.
    static constexpr int MAX_HELD = 128;
    static constexpr int MAX_PATTERN = MAX_HELD * 4 * 2; // × octave range, × UpDown

    Mode mode = Mode::Up;
    int rate = 2; // default 1/16
    int octaveRange = 1;
    bool active = false;

    // Held keys, kept sorted ascending — pattern order is derived from them.
    std::vector<HeldKey> heldNotes;
    int seqLeadNote = -1;
    float seqLeadVelocity = 0.8f;

    std::vector<PatternStep> pattern;
    bool patternDirty = true;

    int currentIndex = 0;
    double sampleRateVal = 44100.0;
    double bpm = 120.0;
    float gate = 1.0f;
    float shuffle = 0.0f;
    double samplesUntilNext = 0.0;
    double samplesUntilGateOff = -1.0;
    int lastPlayedNote = -1;

    juce::Random rng;

    void rebuildPattern();
    void fisherYatesShuffle();
    double shuffledStepDurationSamples(double baseStepSamples, int stepIdx, int cycleLength) const;
};
