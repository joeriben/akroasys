#include "Arpeggiator.h"

void T5ynthArpeggiator::prepare(double sr, int /*samplesPerBlock*/)
{
    sampleRateVal = sr;
    samplesUntilNext = 0.0;

    // Reserve once, here, so every later rebuild — which happens on the audio
    // thread — stays inside capacity and never allocates.
    heldNotes.reserve(MAX_HELD);
    pattern.reserve(MAX_PATTERN);
}

void T5ynthArpeggiator::setMode(Mode m)
{
    if (m == mode)
        return;
    mode = m;
    patternDirty = true;
}

void T5ynthArpeggiator::setOctaveRange(int octaves)
{
    const int o = juce::jlimit(1, 4, octaves);
    if (o == octaveRange)
        return;
    octaveRange = o;
    patternDirty = true;
}

void T5ynthArpeggiator::noteOn(int midiNote, float velocity, int sourceId, int mpeChannel)
{
    const int n = juce::jlimit(0, 127, midiNote);
    const float v = juce::jlimit(0.0f, 1.0f, velocity);

    // Already down (key repeat, or a controller that re-sends note-on): refresh
    // the velocity but leave the pattern and the running clock alone. The
    // channel is part of the test -- a second finger landing on a key another
    // finger already holds arrives on its own member channel and is its own
    // key, and only its own key-up may take it away.
    for (auto& h : heldNotes)
        if (h.note == n && h.mpeChannel == mpeChannel)
        {
            h.velocity = v;
            return;
        }

    if (heldNotes.size() >= static_cast<size_t>(MAX_HELD))
        return;

    heldNotes.push_back({ n, v, sourceId, mpeChannel });
    std::sort(heldNotes.begin(), heldNotes.end(),
              [](const HeldKey& a, const HeldKey& b) { return a.note < b.note; });

    patternDirty = true;
}

void T5ynthArpeggiator::noteOff(int midiNote, int mpeChannel)
{
    const int n = juce::jlimit(0, 127, midiNote);
    for (size_t i = 0; i < heldNotes.size(); ++i)
    {
        if (heldNotes[i].note != n || heldNotes[i].mpeChannel != mpeChannel)
            continue;

        heldNotes.erase(heldNotes.begin() + static_cast<std::ptrdiff_t>(i));
        patternDirty = true;
        return;
    }
}

void T5ynthArpeggiator::allKeysUp()
{
    if (heldNotes.empty())
        return;
    heldNotes.clear();
    patternDirty = true;
}

void T5ynthArpeggiator::setSeqLead(int midiNote, float velocity)
{
    const int n = juce::jlimit(0, 127, midiNote);
    const float v = juce::jlimit(0.0f, 1.0f, velocity);
    if (n == seqLeadNote && juce::approximatelyEqual(v, seqLeadVelocity))
        return;
    seqLeadNote = n;
    seqLeadVelocity = v;
    patternDirty = true;
}

void T5ynthArpeggiator::clearSeqLead()
{
    if (seqLeadNote < 0)
        return;
    seqLeadNote = -1;
    patternDirty = true;
}

void T5ynthArpeggiator::suspend()
{
    active = false;
    currentIndex = 0;
    samplesUntilNext = 0.0;
    samplesUntilGateOff = -1.0;
    seqLeadNote = -1;
    pattern.clear();
    patternDirty = true;
}

void T5ynthArpeggiator::rebuildPattern()
{
    patternDirty = false;
    pattern.clear();

    // Held keys are the source. The sequencer lead only stands in for "nothing
    // is held" — the moment a key goes down it takes over, and when the
    // transport stops the processor clears the lead, leaving no source at all.
    const HeldKey lead { seqLeadNote, seqLeadVelocity, -1, 0 };
    const HeldKey* src = heldNotes.data();
    size_t srcCount = heldNotes.size();
    if (srcCount == 0 && seqLeadNote >= 0)
    {
        src = &lead;
        srcCount = 1;
    }

    // Two fingers on one key are two held keys and one PITCH: the arp plays the
    // pattern, not the fingers, so a shared pitch appears once. heldNotes is
    // sorted by note, so the duplicates are adjacent.
    for (int oct = 0; oct < octaveRange; ++oct)
        for (size_t i = 0; i < srcCount; ++i)
        {
            if (i > 0 && src[i].note == src[i - 1].note)
                continue;
            const int n = src[i].note + oct * 12;
            if (n > 127)
                continue;                       // octave stack runs off the top
            if (pattern.size() >= pattern.capacity())
                break;                          // never allocate on the audio thread
            pattern.push_back({ n, src[i].velocity });
        }

    // Pattern ordering. Built in place — no temporaries, so no allocation.
    switch (mode)
    {
        case Mode::Up:
            // Already ascending
            break;
        case Mode::Down:
            std::reverse(pattern.begin(), pattern.end());
            break;
        case Mode::UpDown:
        {
            // Append the descent, omitting both endpoints so the turn-arounds
            // are not double-struck.
            const int n = static_cast<int>(pattern.size());
            for (int i = n - 2; i >= 1; --i)
            {
                if (pattern.size() >= pattern.capacity())
                    break;
                pattern.push_back(pattern[static_cast<size_t>(i)]);
            }
            break;
        }
        case Mode::Random:
            fisherYatesShuffle();
            break;
    }

    // A source appearing where there was none starts a fresh cycle on the spot;
    // adding or dropping a key mid-hold does NOT restart the clock.
    const bool nowActive = ! pattern.empty();
    if (nowActive && ! active)
    {
        currentIndex = 0;
        samplesUntilNext = 0.0;
        samplesUntilGateOff = -1.0;
    }
    active = nowActive;
}

void T5ynthArpeggiator::fisherYatesShuffle()
{
    for (int i = static_cast<int>(pattern.size()) - 1; i > 0; --i)
    {
        int j = rng.nextInt(i + 1);
        std::swap(pattern[static_cast<size_t>(i)], pattern[static_cast<size_t>(j)]);
    }
}

double T5ynthArpeggiator::shuffledStepDurationSamples(double baseStepSamples,
                                                       int stepIdx,
                                                       int cycleLength) const
{
    if (shuffle <= 0.0f || cycleLength <= 1)
        return baseStepSamples;

    if ((cycleLength & 1) != 0 && stepIdx == cycleLength - 1)
        return baseStepSamples;

    const double amount = static_cast<double>(shuffle);
    return baseStepSamples * ((stepIdx & 1) == 0 ? (1.0 + amount) : (1.0 - amount));
}

void T5ynthArpeggiator::allNotesOff(std::vector<VoiceEvent>& out, int sampleOffset)
{
    if (lastPlayedNote >= 0)
    {
        out.push_back({ sampleOffset, VoiceEvent::Type::NoteOff, lastPlayedNote, 0.0f,
                        VoiceEvent::Articulation::Normal, -1, 0.0f });
        lastPlayedNote = -1;
    }
    samplesUntilGateOff = -1.0;
}

void T5ynthArpeggiator::processBlock(juce::AudioBuffer<float>& buffer,
                                     std::vector<VoiceEvent>& out)
{
    if (patternDirty)
        rebuildPattern();

    if (!active || pattern.empty())
    {
        if (lastPlayedNote >= 0)
        {
            out.push_back({ 0, VoiceEvent::Type::NoteOff, lastPlayedNote, 0.0f,
                            VoiceEvent::Articulation::Normal, -1, 0.0f });
            lastPlayedNote = -1;
        }
        samplesUntilGateOff = -1.0;
        return;
    }

    // Calculate step duration in samples
    double quarterNoteSamples = sampleRateVal * 60.0 / bpm;
    double samplesPerStep = quarterNoteSamples * static_cast<double>(RATE_FACTORS[rate]);

    const int numSamples = buffer.getNumSamples();
    int samplePos = 0;

    // Same event walk as StepSequencer::processBlock — including the "next event
    // is beyond this block" guard, which the arp used to lack. Without it a
    // gate-off scheduled past the block end still fired at the block end AND
    // advanced the clock by the full remaining gate time, consuming samples the
    // block never had: the arp ran several times too fast, by a factor that
    // depended on the host buffer size. Harmless only while gate was pinned at
    // 1.0 (gate-off disabled) — which is exactly how it shipped, unwired.
    while (samplePos < numSamples)
    {
        const int remaining = numSamples - samplePos;

        // Next event: step boundary or gate-off, whichever comes first.
        double nextEvent = samplesUntilNext;
        bool gateOffFirst = false;
        if (samplesUntilGateOff >= 0.0 && samplesUntilGateOff <= samplesUntilNext)
        {
            nextEvent = samplesUntilGateOff;
            gateOffFirst = true;
        }

        if (nextEvent > static_cast<double>(remaining))
        {
            samplesUntilNext -= remaining;
            if (samplesUntilGateOff >= 0.0)
                samplesUntilGateOff -= remaining;
            samplePos = numSamples;
            break;
        }

        const int advance = std::max(0, static_cast<int>(nextEvent));
        samplePos += advance;
        samplesUntilNext -= advance;
        if (samplesUntilGateOff >= 0.0)
            samplesUntilGateOff -= advance;

        const int eventPos = juce::jmin(samplePos, numSamples - 1);

        if (gateOffFirst)
        {
            if (lastPlayedNote >= 0)
            {
                out.push_back({ eventPos, VoiceEvent::Type::NoteOff, lastPlayedNote, 0.0f,
                                VoiceEvent::Articulation::Normal, -1, 0.0f });
                lastPlayedNote = -1;
            }
            samplesUntilGateOff = -1.0;
            continue;
        }

        // Step boundary. Note-off for the previous note if the gate didn't end it
        // (gate 100% — true legato, the note runs until the next one starts).
        if (lastPlayedNote >= 0)
        {
            out.push_back({ eventPos, VoiceEvent::Type::NoteOff, lastPlayedNote, 0.0f,
                            VoiceEvent::Articulation::Normal, -1, 0.0f });
            lastPlayedNote = -1;
        }

        const int cycleLength = static_cast<int>(pattern.size());
        const int idx = currentIndex % cycleLength;
        const double stepDur = shuffledStepDurationSamples(samplesPerStep, idx, cycleLength);
        const PatternStep& step = pattern[static_cast<size_t>(idx)];
        const int midiNote = juce::jlimit(0, 127, step.note);
        const float vel = juce::jlimit(0.0f, 1.0f, step.velocity);

        out.push_back({ eventPos, VoiceEvent::Type::NoteOn, midiNote, vel,
                        VoiceEvent::Articulation::Normal, -1, 0.0f });
        lastPlayedNote = midiNote;
        currentIndex++;

        // Schedule gate-off (skip for 100% gate — true legato, no retrigger)
        samplesUntilGateOff = (gate >= 1.0f) ? -1.0 : gate * stepDur;

        // Reshuffle on each cycle for random pattern freshness
        if (mode == Mode::Random && currentIndex % cycleLength == 0)
            fisherYatesShuffle();

        samplesUntilNext += stepDur;
    }
}

void T5ynthArpeggiator::reset()
{
    active = false;
    currentIndex = 0;
    samplesUntilNext = 0.0;
    samplesUntilGateOff = -1.0;
    lastPlayedNote = -1;
    heldNotes.clear();
    seqLeadNote = -1;
    pattern.clear();
    patternDirty = true;
}
