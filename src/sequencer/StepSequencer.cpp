#include "StepSequencer.h"

// ─── Preset patterns (32-step) ─────────────────────────────────────────────
// First halves derive from the useStepSequencer.ts 16-step ports; each preset
// then develops over the full 32 (some first halves redesigned — see the
// per-preset notes for the technique and rationale).

using BindMode = T5ynthStepSequencer::BindMode;

static constexpr T5ynthStepSequencer::Step mkStep(int semi, float vel, float gate,
                                                  bool active = true,
                                                  BindMode mode = BindMode::Off)
{
    return { 60 + semi, vel, gate, active, mode };
}
static constexpr T5ynthStepSequencer::Step REST = { 60, 0.0f, 0.0f, false, BindMode::Off };

// clang-format off

// 1. Octave Bounce — root bounces against the octave (odd steps), scale tones
//    climb between (even steps). 32: the 2nd half lifts the whole figure an
//    octave — upper scale tones bounce against the double-octave (24); step
//    30→31 (11→12, leading-tone → octave) leads back to the root for the loop.
static constexpr T5ynthStepSequencer::Step P_OCTAVE_BOUNCE[] = {
    mkStep(0,0.90f,0.4f),  mkStep(12,0.70f,0.25f), mkStep(4,0.75f,0.4f),  mkStep(12,0.65f,0.25f),
    mkStep(7,0.85f,0.4f),  mkStep(12,0.70f,0.25f), mkStep(9,0.75f,0.4f),  mkStep(12,0.65f,0.25f),
    mkStep(0,0.90f,0.4f),  mkStep(12,0.70f,0.25f), mkStep(4,0.75f,0.4f),  mkStep(12,0.65f,0.25f),
    mkStep(7,0.85f,0.4f),  mkStep(12,0.70f,0.25f), mkStep(10,0.80f,0.5f), mkStep(12,0.65f,0.25f),
    mkStep(12,0.80f,0.4f), mkStep(24,0.72f,0.25f), mkStep(16,0.78f,0.4f), mkStep(24,0.68f,0.25f),
    mkStep(19,0.85f,0.4f), mkStep(24,0.72f,0.25f), mkStep(21,0.80f,0.4f), mkStep(24,0.68f,0.25f),
    mkStep(12,0.85f,0.4f), mkStep(24,0.72f,0.25f), mkStep(16,0.78f,0.4f), mkStep(24,0.68f,0.25f),
    mkStep(19,0.85f,0.4f), mkStep(24,0.72f,0.25f), mkStep(11,0.82f,0.5f), mkStep(12,0.68f,0.25f),
};

// 2. Wide Leap — every interval ≥ P4, alternating up/down. 32: the 2nd half
//    widens the leaps and lifts the tessitura, then funnels back; step 31 (+7)
//    lands a fifth above so the loop re-enters on a downward leap to the root.
static constexpr T5ynthStepSequencer::Step P_WIDE_LEAP[] = {
    mkStep(0,0.85f,0.5f),   mkStep(7,0.75f,0.5f),   mkStep(-12,0.90f,0.5f), mkStep(9,0.70f,0.5f),
    mkStep(-9,0.80f,0.5f),  mkStep(11,0.75f,0.5f),  mkStep(-7,0.85f,0.5f),  mkStep(12,0.70f,0.5f),
    mkStep(0,0.90f,0.5f),   mkStep(8,0.75f,0.5f),   mkStep(-10,0.80f,0.5f), mkStep(10,0.70f,0.5f),
    mkStep(-8,0.85f,0.5f),  mkStep(7,0.75f,0.5f),   mkStep(-14,0.90f,0.5f), mkStep(12,0.70f,0.5f),
    mkStep(-2,0.80f,0.5f),  mkStep(14,0.72f,0.5f),  mkStep(-12,0.88f,0.5f), mkStep(9,0.70f,0.5f),
    mkStep(-7,0.82f,0.5f),  mkStep(16,0.74f,0.5f),  mkStep(-10,0.85f,0.5f), mkStep(12,0.70f,0.5f),
    mkStep(-5,0.85f,0.5f),  mkStep(13,0.74f,0.5f),  mkStep(-9,0.82f,0.5f),  mkStep(11,0.70f,0.5f),
    mkStep(-7,0.85f,0.5f),  mkStep(7,0.74f,0.5f),   mkStep(-12,0.90f,0.5f), mkStep(7,0.72f,0.5f),
};

// 3. Off-Beat Minor — natural minor on the off-beats only (rests on the beat).
//    32: the 1st half ascends the scale, the 2nd mirrors it back down (G–F–E♭–
//    D–C–B♭–A♭–G); step 31 (G, a fifth below) resolves up to the root on the loop.
static constexpr T5ynthStepSequencer::Step P_OFFBEAT_MINOR[] = {
    REST, mkStep(0,0.80f,0.4f),  REST, mkStep(3,0.85f,0.4f),
    REST, mkStep(5,0.80f,0.4f),  REST, mkStep(7,0.90f,0.4f),
    REST, mkStep(8,0.75f,0.4f),  REST, mkStep(10,0.85f,0.4f),
    REST, mkStep(12,0.90f,0.4f), REST, mkStep(8,0.80f,0.4f),
    REST, mkStep(7,0.85f,0.4f),  REST, mkStep(5,0.80f,0.4f),
    REST, mkStep(3,0.85f,0.4f),  REST, mkStep(2,0.78f,0.4f),
    REST, mkStep(0,0.88f,0.4f),  REST, mkStep(-2,0.75f,0.4f),
    REST, mkStep(-4,0.82f,0.4f), REST, mkStep(-5,0.80f,0.4f),
};

// 4. Glide Groove — stepwise motion with glide between connected notes. 32: the
//    2nd half is a descending "answer" (starts high at +10, glides down); the
//    leaps punctuating it break the glide for breath; step 31 glides to the root.
static constexpr T5ynthStepSequencer::Step P_GLIDE_GROOVE[] = {
    mkStep(0,0.85f,0.8f), mkStep(2,0.70f,0.8f,true,BindMode::Glide),  mkStep(3,0.70f,0.8f,true,BindMode::Glide), mkStep(7,0.90f,0.6f),
    mkStep(5,0.70f,0.8f,true,BindMode::Glide), mkStep(3,0.80f,0.6f),  mkStep(0,0.70f,0.8f,true,BindMode::Glide), mkStep(2,0.70f,0.8f,true,BindMode::Glide),
    mkStep(3,0.85f,0.8f), mkStep(5,0.70f,0.8f,true,BindMode::Glide),  mkStep(7,0.70f,0.8f,true,BindMode::Glide), mkStep(8,0.90f,0.6f),
    mkStep(10,0.75f,0.8f,true,BindMode::Glide), mkStep(8,0.70f,0.8f,true,BindMode::Glide), mkStep(7,0.80f,0.6f), mkStep(5,0.70f,0.8f,true,BindMode::Glide),
    mkStep(10,0.88f,0.6f), mkStep(8,0.70f,0.8f,true,BindMode::Glide),  mkStep(7,0.70f,0.8f,true,BindMode::Glide), mkStep(3,0.85f,0.6f),
    mkStep(5,0.70f,0.8f,true,BindMode::Glide), mkStep(7,0.70f,0.8f,true,BindMode::Glide), mkStep(8,0.88f,0.6f), mkStep(5,0.70f,0.8f,true,BindMode::Glide),
    mkStep(3,0.82f,0.8f), mkStep(2,0.70f,0.8f,true,BindMode::Glide),  mkStep(0,0.70f,0.8f,true,BindMode::Glide), mkStep(-2,0.80f,0.6f),
    mkStep(0,0.70f,0.8f,true,BindMode::Glide), mkStep(3,0.70f,0.8f,true,BindMode::Glide), mkStep(5,0.82f,0.6f), mkStep(2,0.70f,0.8f,true,BindMode::Glide),
};

// 5. Sparse Stab — staccato stabs, short gates. 32: the 1st half hits on the beat
//    (4 stabs); the 2nd displaces them off the grid (every 3rd step: 18,21,24,27,
//    30 → a 3-against-16 cross-rhythm) and climbs into the upper register.
static constexpr T5ynthStepSequencer::Step P_SPARSE_STAB[] = {
    mkStep(0,1.00f,0.1f), REST, REST, REST,
    mkStep(7,0.85f,0.1f), REST, REST, REST,
    mkStep(3,0.90f,0.1f), REST, REST, REST,
    mkStep(10,0.80f,0.1f),REST, REST, REST,
    REST, REST, mkStep(12,0.95f,0.1f), REST,
    REST, mkStep(5,0.85f,0.1f), REST, REST,
    mkStep(8,0.90f,0.1f), REST, REST, mkStep(14,0.80f,0.1f),
    REST, REST, mkStep(10,0.88f,0.1f), REST,
};

// 6. Rising Arc — pitch follows a sine arch, velocity tracks it. Redesigned for
//    32 as a COMPLETE arch — round(24·sin(π·i/31)): rise to the peak at the
//    midpoint, fall back; velocity crescendos to the apex then decrescendos.
//    (The old 16-step version was only the rising quarter.)
static constexpr T5ynthStepSequencer::Step P_RISING_ARC[] = {
    mkStep(0,0.50f,0.9f),  mkStep(2,0.53f,0.9f),  mkStep(5,0.57f,0.9f),  mkStep(7,0.60f,0.9f),
    mkStep(9,0.63f,0.9f),  mkStep(12,0.67f,0.9f), mkStep(14,0.70f,0.9f), mkStep(16,0.73f,0.9f),
    mkStep(17,0.77f,0.9f), mkStep(19,0.80f,0.9f), mkStep(20,0.83f,0.9f), mkStep(22,0.87f,0.9f),
    mkStep(23,0.90f,0.9f), mkStep(23,0.93f,0.9f), mkStep(24,0.97f,0.9f), mkStep(24,1.00f,0.9f),
    mkStep(24,1.00f,0.9f), mkStep(24,0.97f,0.9f), mkStep(23,0.93f,0.9f), mkStep(23,0.90f,0.9f),
    mkStep(22,0.87f,0.9f), mkStep(20,0.83f,0.9f), mkStep(19,0.80f,0.9f), mkStep(17,0.77f,0.9f),
    mkStep(16,0.73f,0.9f), mkStep(14,0.70f,0.9f), mkStep(12,0.67f,0.9f), mkStep(9,0.63f,0.9f),
    mkStep(7,0.60f,0.9f),  mkStep(5,0.57f,0.9f),  mkStep(2,0.53f,0.9f),  mkStep(0,0.50f,0.9f),
};

// 7. Scatter — modular stride (i·7 mod 13 → extended major degree), every 4th
//    step a rest. 32: the SAME rule continued for i=16…31 — deterministic, it
//    reaches the higher degrees (+21,+23) before the modulo folds back.
//    (First 16 unchanged from the useStepSequencer.ts port.)
static constexpr T5ynthStepSequencer::Step P_SCATTER[] = {
    mkStep(0,0.85f,0.4f),  mkStep(12,0.75f,0.4f), mkStep(2,0.80f,0.4f),  REST,
    mkStep(4,0.85f,0.4f),  mkStep(16,0.70f,0.4f), mkStep(5,0.80f,0.4f),  REST,
    mkStep(7,0.85f,0.4f),  mkStep(19,0.75f,0.4f), mkStep(9,0.80f,0.4f),  REST,
    mkStep(11,0.90f,0.4f), mkStep(0,0.70f,0.4f),  mkStep(12,0.80f,0.4f), REST,
    mkStep(14,0.82f,0.4f), mkStep(4,0.78f,0.4f),  mkStep(16,0.80f,0.4f), REST,
    mkStep(17,0.85f,0.4f), mkStep(7,0.78f,0.4f),  mkStep(19,0.75f,0.4f), REST,
    mkStep(21,0.88f,0.4f), mkStep(11,0.78f,0.4f), mkStep(0,0.80f,0.4f),  REST,
    mkStep(2,0.85f,0.4f),  mkStep(14,0.75f,0.4f), mkStep(4,0.80f,0.4f),  REST,
};

// 8. Chromatic — strict half-steps. Redesigned for 32 as a full chromatic arch:
//    rise 0→+16 (steps 0–16) then fall back; step 31 (+1) resolves chromatically
//    to the root on the loop. (The old 16-step version barely began descending.)
static constexpr T5ynthStepSequencer::Step P_CHROMATIC[] = {
    mkStep(0,0.78f,0.5f),  mkStep(1,0.74f,0.5f),  mkStep(2,0.78f,0.5f),  mkStep(3,0.74f,0.5f),
    mkStep(4,0.80f,0.5f),  mkStep(5,0.74f,0.5f),  mkStep(6,0.80f,0.5f),  mkStep(7,0.76f,0.5f),
    mkStep(8,0.82f,0.5f),  mkStep(9,0.76f,0.5f),  mkStep(10,0.82f,0.5f), mkStep(11,0.78f,0.5f),
    mkStep(12,0.86f,0.5f), mkStep(13,0.80f,0.5f), mkStep(14,0.88f,0.5f), mkStep(15,0.82f,0.5f),
    mkStep(16,0.95f,0.5f), mkStep(15,0.82f,0.5f), mkStep(14,0.86f,0.5f), mkStep(13,0.80f,0.5f),
    mkStep(12,0.84f,0.5f), mkStep(11,0.78f,0.5f), mkStep(10,0.80f,0.5f), mkStep(9,0.76f,0.5f),
    mkStep(8,0.78f,0.5f),  mkStep(7,0.74f,0.5f),  mkStep(6,0.76f,0.5f),  mkStep(5,0.72f,0.5f),
    mkStep(4,0.74f,0.5f),  mkStep(3,0.72f,0.5f),  mkStep(2,0.72f,0.5f),  mkStep(1,0.70f,0.5f),
};

// 9. Bass Walk — walking bass, low register. 32: the 16-step walk played twice
//    (2nd half is an exact copy of the 1st) — a straight repeat suits a bass
//    line better than a mirror or development.
static constexpr T5ynthStepSequencer::Step P_BASS_WALK[] = {
    mkStep(-12,0.90f,0.5f), mkStep(-8,0.75f,0.5f),  mkStep(-5,0.80f,0.5f),  mkStep(-3,0.70f,0.5f),
    mkStep(-2,0.75f,0.5f),  mkStep(-5,0.80f,0.5f),  mkStep(-8,0.75f,0.5f),  mkStep(-10,0.70f,0.5f),
    mkStep(-12,0.90f,0.5f), mkStep(-10,0.75f,0.5f), mkStep(-8,0.80f,0.5f),  mkStep(-7,0.75f,0.5f),
    mkStep(-5,0.85f,0.5f),  mkStep(-3,0.75f,0.5f),  mkStep(-2,0.80f,0.5f),  mkStep(-1,0.70f,0.5f),
    mkStep(-12,0.90f,0.5f), mkStep(-8,0.75f,0.5f),  mkStep(-5,0.80f,0.5f),  mkStep(-3,0.70f,0.5f),
    mkStep(-2,0.75f,0.5f),  mkStep(-5,0.80f,0.5f),  mkStep(-8,0.75f,0.5f),  mkStep(-10,0.70f,0.5f),
    mkStep(-12,0.90f,0.5f), mkStep(-10,0.75f,0.5f), mkStep(-8,0.80f,0.5f),  mkStep(-7,0.75f,0.5f),
    mkStep(-5,0.85f,0.5f),  mkStep(-3,0.75f,0.5f),  mkStep(-2,0.80f,0.5f),  mkStep(-1,0.70f,0.5f),
};

// 10. Gated Pulse — single repeated pitch, rhythmic velocity gating (Euclidean
//     feel). 32: the 2nd half rotates the pulse pattern and pops the octave (+12)
//     on the strongest hit (step 24) for a lift; otherwise the root, staccato.
static constexpr T5ynthStepSequencer::Step P_GATED_PULSE[] = {
    mkStep(0,1.00f,0.15f), mkStep(0,0.50f,0.15f), REST,                  mkStep(0,0.80f,0.15f),
    mkStep(0,0.40f,0.15f), REST,                  mkStep(0,1.00f,0.15f), REST,
    mkStep(0,0.90f,0.15f), mkStep(0,0.45f,0.15f), REST,                  mkStep(0,0.70f,0.15f),
    REST,                  mkStep(0,0.85f,0.15f), mkStep(0,0.50f,0.15f), REST,
    mkStep(0,1.00f,0.15f), REST,                  mkStep(0,0.60f,0.15f), mkStep(0,0.90f,0.15f),
    REST,                  mkStep(0,0.50f,0.15f), mkStep(0,1.00f,0.15f), REST,
    mkStep(12,0.95f,0.15f),mkStep(0,0.50f,0.15f), REST,                  mkStep(0,0.80f,0.15f),
    mkStep(0,0.45f,0.15f), REST,                  mkStep(0,0.90f,0.15f), mkStep(0,0.60f,0.15f),
};

// clang-format on

const T5ynthStepSequencer::PresetData T5ynthStepSequencer::presetTable[NUM_PRESETS] = {
    { "Octave Bounce",  P_OCTAVE_BOUNCE,  32 },
    { "Wide Leap",      P_WIDE_LEAP,      32 },
    { "Off-Beat Minor", P_OFFBEAT_MINOR,  32 },
    { "Glide Groove",   P_GLIDE_GROOVE,   32 },
    { "Sparse Stab",    P_SPARSE_STAB,    32 },
    { "Rising Arc",     P_RISING_ARC,     32 },
    { "Scatter",        P_SCATTER,        32 },
    { "Chromatic",      P_CHROMATIC,      32 },
    { "Bass Walk",      P_BASS_WALK,      32 },
    { "Gated Pulse",    P_GATED_PULSE,    32 },
};

// ─── Implementation ────────────────────────────────────────────────────────

double T5ynthStepSequencer::stepDurationSamples() const
{
    // samplesPerStep = sampleRate * (60 / bpm) * divisionFactor
    return sampleRate * 60.0 / bpm * static_cast<double>(DIVISION_FACTORS[division]);
}

double T5ynthStepSequencer::shuffledStepDurationSamples(int stepIdx) const
{
    const double base = stepDurationSamples();
    if (shuffle <= 0.0f || numSteps <= 1)
        return base;

    // Complete even/odd pairs keep their combined duration. If the pattern
    // length is odd, the final unpaired interval stays straight.
    if ((numSteps & 1) != 0 && stepIdx == numSteps - 1)
        return base;

    const double amount = static_cast<double>(shuffle);
    return base * ((stepIdx & 1) == 0 ? (1.0 + amount) : (1.0 - amount));
}

void T5ynthStepSequencer::emitOneShotTriggers(int stepIdx, const Step& step, int sampleOffset)
{
    if (!oneShotTriggerCallback)
        return;

    for (int slot = 0; slot < ONE_SHOT_SLOTS; ++slot)
    {
        const auto mode = step.oneShotModes[static_cast<size_t>(slot)];
        if (mode == OneShotMode::Mute)
            continue;

        OneShotTrigger trigger;
        trigger.stepIndex = stepIdx;
        trigger.slotIndex = slot;
        trigger.gain = mode == OneShotMode::Accent ? 1.5f : 1.0f;
        trigger.sampleOffset = sampleOffset;
        oneShotTriggerCallback(trigger);
    }
}

void T5ynthStepSequencer::prepare(double sr, int /*samplesPerBlock*/)
{
    sampleRate = sr;
    samplesUntilNextStep = 0.0;
    samplesUntilGateOff = -1.0;
}

void T5ynthStepSequencer::processBlock(juce::AudioBuffer<float>& buffer,
                                       std::vector<VoiceEvent>& out)
{
    if (!running)
    {
        if (lastPlayedNote >= 0)
        {
            out.push_back({ 0, VoiceEvent::Type::NoteOff, lastPlayedNote, 0.0f,
                            VoiceEvent::Articulation::Normal, -1, 0.0f });
            lastPlayedNote = -1;
        }
        currentStep = 0;
        currentStepForGui.store(-1, std::memory_order_relaxed);
        return;
    }

    const int numSamples = buffer.getNumSamples();
    int samplePos = 0;

    while (samplePos < numSamples)
    {
        // How many samples remain in this block?
        int remaining = numSamples - samplePos;

        // Find the next event: step boundary or gate-off
        double nextEvent = samplesUntilNextStep;
        bool gateOffFirst = false;
        if (samplesUntilGateOff >= 0.0 && samplesUntilGateOff <= samplesUntilNextStep)
        {
            nextEvent = samplesUntilGateOff;
            gateOffFirst = true;
        }

        // If the next event is beyond this block, just advance to block end
        if (nextEvent > static_cast<double>(remaining))
        {
            samplesUntilNextStep -= remaining;
            if (samplesUntilGateOff >= 0.0)
                samplesUntilGateOff -= remaining;
            samplePos = numSamples;
            break;
        }

        // Advance to the event
        int advance = std::max(0, static_cast<int>(nextEvent));
        samplePos += advance;
        samplesUntilNextStep -= advance;
        if (samplesUntilGateOff >= 0.0)
            samplesUntilGateOff -= advance;

        int eventPos = juce::jmin(samplePos, numSamples - 1);

        if (gateOffFirst)
        {
            // Gate-off event
            if (lastPlayedNote >= 0)
            {
                out.push_back({ eventPos, VoiceEvent::Type::NoteOff, lastPlayedNote, 0.0f,
                                VoiceEvent::Articulation::Normal, -1, 0.0f });
                lastPlayedNote = -1;
            }
            samplesUntilGateOff = -1.0;
            continue;
        }

        // Step boundary event
        int stepIdx = scheduledStep % numSteps;
        const double stepDur = shuffledStepDurationSamples(stepIdx);
        if (stepIdx == 0 && scheduledStep > 0)
            barStartFlag.store(true, std::memory_order_relaxed);

        auto& step = steps[static_cast<size_t>(stepIdx)];

        // Bind/Glide is owned by the SOURCE step and ties it into the FOLLOWING
        // note (TB-303 "slide" convention): a slide flag on step N means N's
        // voice carries into N+1 with no retrigger. So whether THIS note-on
        // continues the live voice is driven by the PREVIOUS step's flag — if the
        // previous step slid, keep its voice alive and continue it here instead
        // of retriggering. (scheduledStep increments monotonically and is never
        // reset on loop, so prevIdx also resolves a slide on the last step into
        // the first step of the next loop; scheduledStep == 0 has no predecessor.)
        const int prevIdx = (scheduledStep - 1 + numSteps) % numSteps;
        const bool prevSlid = scheduledStep > 0
                           && steps[static_cast<size_t>(prevIdx)].enabled
                           && steps[static_cast<size_t>(prevIdx)].bindMode != BindMode::Off;

        // Note-off for the previous note — but SKIP it if the previous step slid
        // (its voice must stay alive so this note can continue/ramp it).
        if (lastPlayedNote >= 0 && !prevSlid)
        {
            out.push_back({ eventPos, VoiceEvent::Type::NoteOff, lastPlayedNote, 0.0f,
                            VoiceEvent::Articulation::Normal, -1, 0.0f });
            lastPlayedNote = -1;
        }

        // Note-on for current step if enabled
        if (step.enabled)
        {
            int midiNote = juce::jlimit(0, 127, step.note + octaveShiftSemitones);
            float vel = juce::jlimit(0.0f, 1.0f, step.velocity);
            // Articulation carries the INCOMING transition for voice dispatch: if
            // the previous step slid, continue its voice — Glide (ramped) or Bind
            // (instant); otherwise a fresh Normal note.
            const BindMode incoming = prevSlid ? steps[static_cast<size_t>(prevIdx)].bindMode
                                               : BindMode::Off;
            const VoiceEvent::Articulation artic =
                  incoming == BindMode::Glide ? VoiceEvent::Articulation::Glide
                : incoming == BindMode::Bind  ? VoiceEvent::Articulation::Bind
                                              : VoiceEvent::Articulation::Normal;
            out.push_back({ eventPos, VoiceEvent::Type::NoteOn, midiNote,
                            vel, artic, -1, 0.0f });
            lastPlayedNote = midiNote;

            // If THIS step slides into an enabled next step, hold the note for
            // the full step (no early gate-off) so the voice stays alive to be
            // continued/ramped by that next note. A slide into a rest has no
            // target, so it gates off normally.
            const int nextIdx = (scheduledStep + 1) % numSteps;
            const bool slidesToNext = step.bindMode != BindMode::Off
                                   && steps[static_cast<size_t>(nextIdx)].enabled;
            samplesUntilGateOff = slidesToNext ? -1.0 : step.gate * stepDur;
        }
        else
        {
            samplesUntilGateOff = -1.0;
        }

        emitOneShotTriggers(stepIdx, step, eventPos);

        currentStep = stepIdx;
        currentStepForGui.store(currentStep, std::memory_order_relaxed);
        scheduledStep++;
        samplesUntilNextStep += stepDur;
    }
}

void T5ynthStepSequencer::stop()
{
    running = false;
    // Don't clear lastPlayedNote here — processBlock() needs it to emit noteOff.
    // processBlock() will clear it after sending the noteOff event.
    samplesUntilGateOff = -1.0;
    currentStep = 0;
    scheduledStep = 0;
    currentStepForGui.store(-1, std::memory_order_relaxed);
}

void T5ynthStepSequencer::reset()
{
    stop();
}

void T5ynthStepSequencer::allNotesOff(std::vector<VoiceEvent>& out, int sampleOffset)
{
    if (lastPlayedNote >= 0)
    {
        out.push_back({ sampleOffset, VoiceEvent::Type::NoteOff, lastPlayedNote, 0.0f,
                        VoiceEvent::Articulation::Normal, -1, 0.0f });
        lastPlayedNote = -1;
    }
    samplesUntilGateOff = -1.0;
}

void T5ynthStepSequencer::setNumSteps(int s)
{
    numSteps = juce::jlimit(1, MAX_STEPS, s);
}

void T5ynthStepSequencer::setStepNote(int step, int midiNote)
{
    if (step >= 0 && step < MAX_STEPS)
        steps[static_cast<size_t>(step)].note = midiNote;
}

void T5ynthStepSequencer::setStepVelocity(int step, float velocity)
{
    if (step >= 0 && step < MAX_STEPS)
        steps[static_cast<size_t>(step)].velocity = juce::jlimit(0.0f, 1.0f, velocity);
}

void T5ynthStepSequencer::setStepEnabled(int step, bool enabled)
{
    if (step >= 0 && step < MAX_STEPS)
        steps[static_cast<size_t>(step)].enabled = enabled;
}

void T5ynthStepSequencer::setStepGate(int step, float gate)
{
    if (step >= 0 && step < MAX_STEPS)
        steps[static_cast<size_t>(step)].gate = juce::jlimit(0.1f, 1.0f, gate);
}

void T5ynthStepSequencer::setStepBindMode(int step, BindMode mode)
{
    if (step >= 0 && step < MAX_STEPS)
        steps[static_cast<size_t>(step)].bindMode = mode;
}

void T5ynthStepSequencer::cycleStepBindMode(int step)
{
    if (step >= 0 && step < MAX_STEPS)
    {
        auto& m = steps[static_cast<size_t>(step)].bindMode;
        // Off → Bind → Glide → Off
        m = m == BindMode::Off  ? BindMode::Bind
          : m == BindMode::Bind ? BindMode::Glide
                                : BindMode::Off;
    }
}

void T5ynthStepSequencer::setStepOneShotMode(int step, int slot, OneShotMode mode)
{
    if (step >= 0 && step < MAX_STEPS && slot >= 0 && slot < ONE_SHOT_SLOTS)
        steps[static_cast<size_t>(step)].oneShotModes[static_cast<size_t>(slot)] = mode;
}

void T5ynthStepSequencer::cycleStepOneShotMode(int step, int slot)
{
    if (step < 0 || step >= MAX_STEPS || slot < 0 || slot >= ONE_SHOT_SLOTS)
        return;

    auto& mode = steps[static_cast<size_t>(step)].oneShotModes[static_cast<size_t>(slot)];
    switch (mode)
    {
        case OneShotMode::Normal: mode = OneShotMode::Accent; break;
        case OneShotMode::Accent: mode = OneShotMode::Mute;   break;
        case OneShotMode::Mute:   mode = OneShotMode::Normal; break;
    }
}

T5ynthStepSequencer::OneShotMode T5ynthStepSequencer::getStepOneShotMode(int step, int slot) const
{
    if (step >= 0 && step < MAX_STEPS && slot >= 0 && slot < ONE_SHOT_SLOTS)
        return steps[static_cast<size_t>(step)].oneShotModes[static_cast<size_t>(slot)];

    return OneShotMode::Normal;
}

void T5ynthStepSequencer::setOneShotTriggerCallback(OneShotTriggerCallback callback)
{
    oneShotTriggerCallback = std::move(callback);
}

void T5ynthStepSequencer::setAllGates(float gate)
{
    float g = juce::jlimit(0.1f, 1.0f, gate);
    for (auto& s : steps)
        s.gate = g;
}

void T5ynthStepSequencer::loadPreset(int index)
{
    if (index < 0 || index >= NUM_PRESETS) return;

    const auto& preset = presetTable[index];
    numSteps = preset.count;

    for (int i = 0; i < MAX_STEPS; ++i)
    {
        const auto oneShotModes = steps[static_cast<size_t>(i)].oneShotModes;
        if (i < preset.count)
            steps[static_cast<size_t>(i)] = preset.steps[i];
        else
            steps[static_cast<size_t>(i)] = { 60, 0.8f, 0.8f, false, BindMode::Off };
        steps[static_cast<size_t>(i)].oneShotModes = oneShotModes;
    }
    // The caller publishes the replacement (markPatternReplaced) once it has
    // also written the step count parameter the GUI reads.
}

void T5ynthStepSequencer::resetGrid()
{
    for (auto& s : steps)
        s = { 60, 0.8f, 0.8f, true, BindMode::Off };
}
