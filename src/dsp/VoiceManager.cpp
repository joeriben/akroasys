#include "VoiceManager.h"
#include "CsoundEngine.h"

namespace
{
constexpr bool kSamplerDebugLogging = false;

void samplerVoiceDebugLog(const juce::String& message)
{
    if constexpr (kSamplerDebugLogging)
    {
        juce::Logger::writeToLog("[SamplerDebug] " + message);
        juce::FileOutputStream out(juce::File("/tmp/t5ynth_sampler_debug.log"));
        if (out.openedOk())
        {
            out << "[SamplerDebug] " << message << juce::newLine;
            out.flush();
        }
    }
}

const char* engineModeName(SynthVoice::EngineMode mode)
{
    switch (mode)
    {
        case SynthVoice::EngineMode::Sampler:   return "Sampler";
        case SynthVoice::EngineMode::Wavetable: return "Wavetable";
        case SynthVoice::EngineMode::Freeze:    return "Granular";
        case SynthVoice::EngineMode::Csound:    return "Csound";
    }
    return "?";
}

void equalPowerPan(float pan, float& left, float& right)
{
    pan = juce::jlimit(-1.0f, 1.0f, pan);
    const float angle = (pan + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
    // √2 normalization: CENTER gain = cos(π/4)·√2 = 1.0 (unity), not the bare
    // equal-power 0.707. This is the ONLY pan-law point in the mixer and it is applied
    // ONLY to generative-strand voices (sourceId 0-3); step/manual voices use the
    // full-level stereo passthrough (gain 1.0). With the bare 0.707 a centered gen
    // strand sat ~3 dB BELOW a step note, so switching STEP↔GEN jumped in loudness —
    // the "GenSeq→StepSeq verändert Lautstärke" bug. Unity-center makes a centered gen
    // strand match the passthrough level exactly (mono voice → sampleMono·1.0 = s, same
    // as step's sampleLeft/sampleRight). Still constant power (left²+right² = 2 for all
    // pan), so the stereo spread is unchanged and perceived loudness stays flat as a
    // strand pans; only the absolute reference rises to meet the centered passthrough.
    constexpr float kUnityCenter = juce::MathConstants<float>::sqrt2;  // 1.41421356…
    left  = kUnityCenter * std::cos(angle);
    right = kUnityCenter * std::sin(angle);
}
}

void VoiceManager::prepare(double sampleRate, int samplesPerBlock)
{
    sr = sampleRate;
    maxBlockSize = samplesPerBlock;
    // Sized before the voices are handed pointers into it — the 3 s ceiling
    // SynthVoice::updateSamplerPreStretchNorm clamps its analysis window to.
    dcaAnalysisScratch.assign(static_cast<size_t>(sampleRate * 3.0), 0.0f);
    for (auto& v : voices)
    {
        v.prepare(sampleRate, samplesPerBlock);
        v.setDcaAnalysisScratch(dcaAnalysisScratch.data(),
                                static_cast<int>(dcaAnalysisScratch.size()));
    }
    for (auto& scratch : voiceScratch)
        scratch.resize(static_cast<size_t>(samplesPerBlock));
    for (auto& scratch : voiceScratchRight)
        scratch.resize(static_cast<size_t>(samplesPerBlock));
    voicePan.fill(0.0f);
    voiceSourceId.fill(-1);
    voiceMidiChannel_.fill(0);
    voiceMpePressure_.fill(0.0f);
    sustainedVoice.fill(false);
    sostenutoVoice.fill(false);
    sostenutoReleasedVoice.fill(false);
    polyPressureByNote.fill(0.0f);
    channelPressure = 0.0f;
    modWheelPressure = 0.0f;
    breathPressure = 0.0f;
    expressionGain = 1.0f;
    channelVolumeGain = 1.0f;
    pitchBendSemitones = 0.0f;
    for (auto& v : voices) v.setPerVoicePitchBend(0.0f);
    sustainPedalDown = false;
    sostenutoPedalDown = false;
    softPedalDown = false;
    currentGain = 1.0f;
    targetGain = 1.0f;
    gainRampSamplesLeft = 0;
}

void VoiceManager::reset()
{
    for (auto& v : voices)
        v.reset();
    noteOnCounter = 0;
    currentGain = 1.0f;
    targetGain = 1.0f;
    gainRampSamplesLeft = 0;
    currentSamplerMaster_ = nullptr;
    currentWavetableMaster_ = nullptr;
    currentFreezeMaster_ = nullptr;
    hasCurrentBlockParams_ = false;
    droneVoiceIndex = -1;
    droneNote = -1;
    voicePan.fill(0.0f);
    voiceSourceId.fill(-1);
    voiceMidiChannel_.fill(0);
    voiceMpePressure_.fill(0.0f);
    channelTimbre_.fill(SynthVoice::kTimbreRest);
    sustainedVoice.fill(false);
    sostenutoVoice.fill(false);
    sostenutoReleasedVoice.fill(false);
    polyPressureByNote.fill(0.0f);
    channelPressure = 0.0f;
    modWheelPressure = 0.0f;
    breathPressure = 0.0f;
    expressionGain = 1.0f;
    channelVolumeGain = 1.0f;
    pitchBendSemitones = 0.0f;
    for (auto& v : voices) v.setPerVoicePitchBend(0.0f);
    sustainPedalDown = false;
    sostenutoPedalDown = false;
    softPedalDown = false;
}

void VoiceManager::setBlockParams(const BlockParams& bp)
{
    currentBlockParams_ = bp;
    hasCurrentBlockParams_ = true;
}

// ═══════════════════════════════════════════════════════════════════
// MIDI → Voice allocation
// ═══════════════════════════════════════════════════════════════════

void VoiceManager::noteOn(int note, float velocity, bool isBind, float glideMs,
                           bool lfo1TrigMode, bool lfo2TrigMode, bool lfo3TrigMode,
                           int sourceId, float pan, int mpeChannel)
{
    sourceId = sourceId >= 0 ? juce::jlimit(0, 15, sourceId) : -1;
    pan = juce::jlimit(-1.0f, 1.0f, pan);

    // Origin is explicit: mpeChannel 1-16 = external controller note (tracked so
    // per-note pitch bend / pressure / timbre route to it); 0 = internal note
    // (sequencer or arp) which is never part of any MPE zone.
    const int8_t effectiveMidiChannel =
        (mpeChannel >= 1 && mpeChannel <= 16) ? static_cast<int8_t>(mpeChannel) : 0;

    // ── Mono mode: always voice 0, legato (no retrigger if held) ──
    if (voiceLimit == 1)
    {
        // Drone owns voice 0 in mono: seq noteOns are fully suppressed while held.
        if (droneVoiceIndex == 0)
            return;
        auto& v = voices[0];
        v.setTuningTable(tuningHz_);
        if (hasCurrentBlockParams_)
            v.configureForBlock(applyPerformanceControllers(currentBlockParams_));
        bool legato = v.isActive() && !v.isReleasing();
        if (legato || (isBind && v.isActive()))
        {
            voiceSourceId[0] = sourceId;
            voicePan[0] = pan;
            voiceMidiChannel_[0] = effectiveMidiChannel;
            voiceMpePressure_[0] = 0.0f;
            // NO beginTimbre here. This branch is a legato slide or a bind:
            // the same note continuing to a new pitch, so its Y origin is the
            // one it was struck with. Re-capturing it mid-slide would move the
            // ground under a finger that has not left the key -- and on the bind
            // path it would run BEFORE the conditional noteOn below, which
            // resets the rest again.
            sustainedVoice[0] = false;
            sostenutoVoice[0] = false;
            sostenutoReleasedVoice[0] = false;
            v.setPerVoicePitchBend(0.0f);
            v.setAftertouch(pressureForNote(note));
            // Glide pitch without retriggering envelopes
            // (If voice is releasing, re-hold it so it stays alive during glide)
            if (v.isReleasing())
            {
                v.noteOn(v.getCurrentNote(), velocity, false);
                // Don't retrigger sampler — keep audio continuous
            }
            v.glideToNote(note, glideMs > 0.0f ? glideMs : 30.0f);
            return;
        }
        if (v.isActive())
            v.beginRestartFade();
        sustainedVoice[0] = false;
        sostenutoVoice[0] = false;
        sostenutoReleasedVoice[0] = false;
        v.setAftertouch(pressureForNote(note));
        if (v.getEngineMode() == SynthVoice::EngineMode::Sampler && currentSamplerMaster_ != nullptr)
        {
            v.getSampler().shareBufferFrom(*currentSamplerMaster_);
            samplerVoiceDebugLog("noteOn mono share voice=0 note=" + juce::String(note)
                                 + " engine=" + juce::String(engineModeName(v.getEngineMode())));
        }
        if (v.getEngineMode() == SynthVoice::EngineMode::Wavetable && currentWavetableMaster_ != nullptr)
            v.getOsc().shareFramesFrom(*currentWavetableMaster_);
        if (v.getEngineMode() == SynthVoice::EngineMode::Freeze && currentFreezeMaster_ != nullptr)
            v.getFreezeEngine().shareBufferFrom(*currentFreezeMaster_);
        v.noteOn(note, velocity, false);
        voiceSourceId[0] = sourceId;
        voicePan[0] = pan;
        voiceMidiChannel_[0] = effectiveMidiChannel;
        voiceMpePressure_[0] = 0.0f;
        v.beginTimbre(channelTimbreFor(effectiveMidiChannel));
        v.setPerVoicePitchBend(0.0f);
        samplerVoiceDebugLog("noteOn mono trigger voice=0 note=" + juce::String(note)
                             + " velocity=" + juce::String(velocity, 3)
                             + " engine=" + juce::String(engineModeName(v.getEngineMode())));
        v.noteOnTimestamp = ++noteOnCounter;
        v.triggerEpoch = v.noteOnTimestamp;  // genuine fresh strike (D8) — mono, non-legato
        if (lfo1TrigMode) v.getPerVoiceLfo1().reset();
        if (lfo2TrigMode) v.getPerVoiceLfo2().reset();
        if (lfo3TrigMode) v.getPerVoiceLfo3().reset();
        if (v.getEngineMode() == SynthVoice::EngineMode::Sampler && v.getSampler().hasAudio())
        {
            v.getSampler().retrigger();
            samplerVoiceDebugLog("noteOn mono retrigger voice=0 note=" + juce::String(note));
        }
        if (v.getEngineMode() == SynthVoice::EngineMode::Wavetable)
        {
            v.getSampler().stop();
            v.getOsc().retriggerAutoScan();
        }
        updateGainTarget();
        return;
    }

    // ── Poly: glide handling ──
    if (isBind)
    {
        // Continue the most recently triggered active voice OF THE SAME SOURCE
        // (exclude drone). A bind/slide must never hijack a voice owned by another
        // origin (e.g. a held keyboard note while the step-seq slides): gliding it
        // drifts that voice's currentNote so neither origin's note-off can match it
        // again, stranding the voice held until a panic/restart. Same-source match
        // mirrors noteOff() (sourceId<0 ↔ voiceSourceId<0). The owning source's
        // note-off can then always release the continued voice.
        int newest = -1;
        uint64_t maxTs = 0;
        for (int i = 0; i < voiceLimit; ++i)
        {
            if (i == droneVoiceIndex) continue;
            auto& vi = voices[static_cast<size_t>(i)];
            const bool sourceMatches = sourceId >= 0
                ? voiceSourceId[static_cast<size_t>(i)] == sourceId
                : voiceSourceId[static_cast<size_t>(i)] < 0;
            // The sourceId<0 bucket conflates the step-seq with external MIDI (both
            // pass -1), so also require the same ORIGIN: a step-seq slide (internal,
            // channel 0) must not continue a held external-MIDI note (channel 1-16),
            // which would strand that key's voice. Binds are always internal, so this
            // never blocks a real slide. (channel 0 == internal sequencer/arp.)
            const bool originMatches =
                voiceMidiChannel_[static_cast<size_t>(i)] == effectiveMidiChannel;
            if (vi.isActive() && sourceMatches && originMatches
                && vi.noteOnTimestamp >= maxTs)
            {
                maxTs = vi.noteOnTimestamp;
                newest = i;
            }
        }
        if (newest >= 0)
        {
            auto& v = voices[static_cast<size_t>(newest)];
            v.setTuningTable(tuningHz_);
            v.setPerVoicePitchBend(0.0f);
            voiceSourceId[static_cast<size_t>(newest)] = sourceId;
            voiceMidiChannel_[static_cast<size_t>(newest)] = effectiveMidiChannel;
            voiceMpePressure_[static_cast<size_t>(newest)] = 0.0f;
            // No beginTimbre: same reason as the mono legato branch above --
            // this is a continued voice gliding, not a fresh strike.
            v.glideToNote(note, glideMs);
            // Continued voice is now the newest: keeps it from becoming the steal
            // victim mid-slide, and makes the next same-source bind find it.
            // Deliberately NOT touching v.triggerEpoch here — a poly BIND is a
            // glide continuation, not a fresh strike (D8), so the orchestra's
            // changed2(trig) must not see a change.
            v.noteOnTimestamp = ++noteOnCounter;
            return;
        }
        // No same-source active voice to continue — fall through to a fresh noteOn.
    }

    // Rotate voices: take a FREE voice first, steal the oldest only when every
    // voice is busy. A repeated note (same pitch) MUST land on a fresh voice so
    // the previous note keeps its own voice and rings out its release. The old
    // first priority — re-trigger the SAME-pitch voice — grabbed that voice back
    // and beginRestartFade + retriggered it, so the release was cut dead and the
    // sample onset replayed at full level: the audible "new attack" with no
    // release, on every repeat (sequencer AND manual, same allocation path).
    int idx = findFreeVoice();
    if (idx < 0) idx = stealVoice();

    auto& v = voices[static_cast<size_t>(idx)];
    v.setTuningTable(tuningHz_);
    if (hasCurrentBlockParams_)
        v.configureForBlock(applyPerformanceControllers(currentBlockParams_));

    if (v.isActive())
        v.beginRestartFade();
    sustainedVoice[static_cast<size_t>(idx)] = false;
    sostenutoVoice[static_cast<size_t>(idx)] = false;
    sostenutoReleasedVoice[static_cast<size_t>(idx)] = false;
    v.setAftertouch(pressureForNote(note));

    if (v.getEngineMode() == SynthVoice::EngineMode::Sampler && currentSamplerMaster_ != nullptr)
    {
        v.getSampler().shareBufferFrom(*currentSamplerMaster_);
        samplerVoiceDebugLog("noteOn poly share voice=" + juce::String(idx)
                             + " note=" + juce::String(note)
                             + " engine=" + juce::String(engineModeName(v.getEngineMode())));
    }
    if (v.getEngineMode() == SynthVoice::EngineMode::Wavetable && currentWavetableMaster_ != nullptr)
        v.getOsc().shareFramesFrom(*currentWavetableMaster_);
    if (v.getEngineMode() == SynthVoice::EngineMode::Freeze && currentFreezeMaster_ != nullptr)
        v.getFreezeEngine().shareBufferFrom(*currentFreezeMaster_);
    v.noteOn(note, velocity, false);
    voiceSourceId[static_cast<size_t>(idx)] = sourceId;
    voicePan[static_cast<size_t>(idx)] = pan;
    voiceMidiChannel_[static_cast<size_t>(idx)] = effectiveMidiChannel;
    voiceMpePressure_[static_cast<size_t>(idx)] = 0.0f;
    v.beginTimbre(channelTimbreFor(effectiveMidiChannel));
    v.setPerVoicePitchBend(0.0f);
    samplerVoiceDebugLog("noteOn poly trigger voice=" + juce::String(idx)
                         + " note=" + juce::String(note)
                         + " velocity=" + juce::String(velocity, 3)
                         + " engine=" + juce::String(engineModeName(v.getEngineMode())));
    v.noteOnTimestamp = ++noteOnCounter;
    v.triggerEpoch = v.noteOnTimestamp;  // genuine fresh strike (D8) — poly, new/stolen voice

    // LFO trigger mode: reset per-voice LFO phase
    if (lfo1TrigMode)
        v.getPerVoiceLfo1().reset();
    if (lfo2TrigMode)
        v.getPerVoiceLfo2().reset();
    if (lfo3TrigMode)
        v.getPerVoiceLfo3().reset();

    // Retrigger sampler if in sampler mode
    if (v.getEngineMode() == SynthVoice::EngineMode::Sampler && v.getSampler().hasAudio())
    {
        v.getSampler().retrigger();
        samplerVoiceDebugLog("noteOn poly retrigger voice=" + juce::String(idx)
                             + " note=" + juce::String(note));
    }
    if (v.getEngineMode() == SynthVoice::EngineMode::Wavetable)
    {
        v.getSampler().stop();
        v.getOsc().retriggerAutoScan();
    }

    updateGainTarget();
}

void VoiceManager::noteOff(int note, int sourceId, bool forceRelease)
{
    sourceId = sourceId >= 0 ? juce::jlimit(0, 15, sourceId) : -1;
    for (int i = 0; i < MAX_VOICES; ++i)
    {
        if (i == droneVoiceIndex) continue; // drone holds independent of MIDI noteOff
        auto& v = voices[static_cast<size_t>(i)];
        const bool sourceMatches = sourceId < 0
                                || voiceSourceId[static_cast<size_t>(i)] == sourceId;
        if (v.isActive() && !v.isReleasing() && v.getCurrentNote() == note && sourceMatches)
        {
            if (hasCurrentBlockParams_)
                v.configureForBlock(applyPerformanceControllers(currentBlockParams_));
            const bool heldBySostenuto = ! forceRelease
                                      && sourceId < 0
                                      && sostenutoPedalDown
                                      && sostenutoVoice[static_cast<size_t>(i)];
            if (heldBySostenuto)
                sostenutoReleasedVoice[static_cast<size_t>(i)] = true;

            if (! forceRelease && sourceId < 0 && sustainPedalDown)
            {
                sustainedVoice[static_cast<size_t>(i)] = true;
                continue;
            }
            if (heldBySostenuto)
                continue;
            v.noteOff();
            sustainedVoice[static_cast<size_t>(i)] = false;
            sostenutoVoice[static_cast<size_t>(i)] = false;
            sostenutoReleasedVoice[static_cast<size_t>(i)] = false;
        }
    }
    // Update gain: held voice count decreased (releasing voices don't count).
    updateGainTarget();
}

void VoiceManager::allNotesOff()
{
    for (auto& v : voices)
    {
        if (v.isActive())
            v.noteOff();
    }
    // Panic also ends a drone hold (DAW reset / host-driven silence).
    droneVoiceIndex = -1;
    droneNote = -1;
    sustainedVoice.fill(false);
    sostenutoVoice.fill(false);
    sostenutoReleasedVoice.fill(false);
    resetPerformanceControllers();
}

void VoiceManager::setSustainPedal(bool down)
{
    if (sustainPedalDown == down)
        return;

    sustainPedalDown = down;
    if (!sustainPedalDown)
        releaseSustainedVoices();
}

void VoiceManager::setSostenutoPedal(bool down)
{
    if (sostenutoPedalDown == down)
        return;

    sostenutoPedalDown = down;
    if (sostenutoPedalDown)
    {
        for (int i = 0; i < MAX_VOICES; ++i)
        {
            auto& v = voices[static_cast<size_t>(i)];
            const bool manualVoice = voiceSourceId[static_cast<size_t>(i)] < 0;
            sostenutoVoice[static_cast<size_t>(i)] = manualVoice && v.isActive() && !v.isReleasing();
            sostenutoReleasedVoice[static_cast<size_t>(i)] = false;
        }
    }
    else
    {
        releaseSostenutoVoices();
    }
}

void VoiceManager::setSoftPedal(bool down)
{
    softPedalDown = down;
}

void VoiceManager::setPitchBendSemitones(float semitones)
{
    pitchBendSemitones = juce::jlimit(-24.0f, 24.0f, semitones);
}

void VoiceManager::setModWheel(float value)
{
    modWheelPressure = juce::jlimit(0.0f, 1.0f, value);
    refreshPerformancePressure();
}

void VoiceManager::setBreathController(float value)
{
    breathPressure = juce::jlimit(0.0f, 1.0f, value);
    refreshPerformancePressure();
}

void VoiceManager::setExpression(float value)
{
    expressionGain = juce::jlimit(0.0f, 1.0f, value);
}

void VoiceManager::setChannelVolume(float value)
{
    channelVolumeGain = juce::jlimit(0.0f, 1.0f, value);
}

void VoiceManager::setChannelPressure(float pressure)
{
    channelPressure = juce::jlimit(0.0f, 1.0f, pressure);
    refreshPerformancePressure();
}

void VoiceManager::setPolyPressure(int note, float pressure, int sourceId)
{
    note = juce::jlimit(0, 127, note);
    sourceId = sourceId >= 0 ? juce::jlimit(0, 15, sourceId) : -1;
    polyPressureByNote[static_cast<size_t>(note)] = juce::jlimit(0.0f, 1.0f, pressure);

    for (int i = 0; i < MAX_VOICES; ++i)
    {
        auto& v = voices[static_cast<size_t>(i)];
        const bool sourceMatches = sourceId < 0
                                || voiceSourceId[static_cast<size_t>(i)] == sourceId;
        if (v.isActive() && v.getCurrentNote() == note && sourceMatches)
            v.setAftertouch(pressureForVoice(i));
    }
}

void VoiceManager::resetPerformanceControllers()
{
    if (sustainPedalDown)
        releaseSustainedVoices();
    if (sostenutoPedalDown)
        releaseSostenutoVoices();
    sustainPedalDown = false;
    sostenutoPedalDown = false;
    softPedalDown = false;
    sustainedVoice.fill(false);
    sostenutoVoice.fill(false);
    sostenutoReleasedVoice.fill(false);
    channelPressure = 0.0f;
    modWheelPressure = 0.0f;
    breathPressure = 0.0f;
    expressionGain = 1.0f;
    channelVolumeGain = 1.0f;
    pitchBendSemitones = 0.0f;
    polyPressureByNote.fill(0.0f);
    for (auto& v : voices)
    {
        if (v.isActive())
            v.setAftertouch(0.0f);
        v.setPerVoicePitchBend(0.0f);
    }
    voiceMidiChannel_.fill(0);
    voiceMpePressure_.fill(0.0f);
    channelTimbre_.fill(SynthVoice::kTimbreRest);
}

void VoiceManager::setPerVoicePitchBend(int midiChannel, float semitones, float normalised)
{
    if (midiChannel < 1 || midiChannel > 16)
        return;
    const auto ch = static_cast<int8_t>(midiChannel);
    for (int i = 0; i < MAX_VOICES; ++i)
        if (voiceMidiChannel_[static_cast<size_t>(i)] == ch)
            voices[static_cast<size_t>(i)].setPerVoicePitchBend(semitones, normalised);
}

void VoiceManager::setChannelPressureForChannel(int midiChannel, float pressure)
{
    if (midiChannel < 1 || midiChannel > 16)
        return;
    pressure = juce::jlimit(0.0f, 1.0f, pressure);
    const auto ch = static_cast<int8_t>(midiChannel);
    for (int i = 0; i < MAX_VOICES; ++i)
        if (voiceMidiChannel_[static_cast<size_t>(i)] == ch
            && voices[static_cast<size_t>(i)].isActive())
        {
            // Per-note Z stored separately, then the voice's effective pressure is
            // max(this, zone-wide pressure) — so member and master pressure compose
            // instead of clobbering each other.
            voiceMpePressure_[static_cast<size_t>(i)] = pressure;
            voices[static_cast<size_t>(i)].setAftertouch(pressureForVoice(i));
        }
}

void VoiceManager::setTimbre(int midiChannel, float value)
{
    if (midiChannel < 1 || midiChannel > 16)
        return;
    value = juce::jlimit(0.0f, 1.0f, value);
    // Remembered per channel, because a note's Y REST is the value in force when
    // it began (SynthVoice::beginTimbre) and MPE controllers send it just BEFORE
    // the note-on rather than after. Kept even while no voice holds the channel:
    // that is exactly the moment it is being set for.
    channelTimbre_[static_cast<size_t>(midiChannel)] = value;
    const auto ch = static_cast<int8_t>(midiChannel);
    for (int i = 0; i < MAX_VOICES; ++i)
        if (voiceMidiChannel_[static_cast<size_t>(i)] == ch)
            voices[static_cast<size_t>(i)].setTimbre(value);
}

// ═══════════════════════════════════════════════════════════════════
// Per-block rendering
// ═══════════════════════════════════════════════════════════════════

VoiceManager::VoiceOutput VoiceManager::renderBlock(
    juce::AudioBuffer<float>& buffer, const BlockParams& bp,
    const float* lfo1Buf, const float* lfo2Buf, const float* lfo3Buf,
    int startSample, int numSamples, const CsoundEngine* cs)
{
    // Back-compat single-engine convenience (Phase-1 call sites): builds the
    // per-voice pointer array cs->voiceBuffer(vi) once and forwards. Mirrors
    // exactly what this function used to compute inline, per voice, below.
    if (cs == nullptr)
        return renderBlock(buffer, bp, lfo1Buf, lfo2Buf, lfo3Buf, startSample, numSamples,
                            static_cast<const float* const*>(nullptr));

    const float* bufs[CsoundEngine::kMaxVoices];
    for (int vi = 0; vi < CsoundEngine::kMaxVoices; ++vi)
        bufs[vi] = cs->voiceBuffer(vi);
    return renderBlock(buffer, bp, lfo1Buf, lfo2Buf, lfo3Buf, startSample, numSamples, bufs);
}

VoiceManager::VoiceOutput VoiceManager::renderBlock(
    juce::AudioBuffer<float>& buffer, const BlockParams& bp,
    const float* lfo1Buf, const float* lfo2Buf, const float* lfo3Buf,
    int startSample, int numSamples, const float* const* csoundVoiceBufs)
{
    VoiceOutput out;

    // Early exit when no voices are active — buffer already cleared by caller
    if (!hasActiveVoices())
    {
        out.hasActiveVoices = false;
        return out;
    }

    const int numChannels = buffer.getNumChannels();

    // Pass tuning table and configure all active voices once per block
    const BlockParams performanceParams = applyPerformanceControllers(bp);
    for (auto& v : voices)
    {
        v.setTuningTable(tuningHz_);
        if (v.isActive())
            v.configureForBlock(performanceParams);
    }

    // Track which voice was triggered most recently (for pitch mod / DCF)
    int newestIdx = -1;
    uint64_t maxTs = 0;
    for (int i = 0; i < MAX_VOICES; ++i)
    {
        if (voices[static_cast<size_t>(i)].isActive()
            && voices[static_cast<size_t>(i)].noteOnTimestamp >= maxTs)
        {
            maxTs = voices[static_cast<size_t>(i)].noteOnTimestamp;
            newestIdx = i;
        }
    }

    bool anyBecameInactive = false;

    // ── Voice-first rendering: each voice renders its full block ──
    int activeCount = 0;
    int activeIndices[MAX_VOICES];

    for (int vi = 0; vi < MAX_VOICES; ++vi)
    {
        auto& v = voices[static_cast<size_t>(vi)];
        if (!v.isActive()) continue;

        // Use sub-block renderBlock for active voices
        float* scratch = voiceScratch[static_cast<size_t>(vi)].data();
        float* scratchRight = voiceScratchRight[static_cast<size_t>(vi)].data();
        // Csound voice bridge (Phase-1 spec §3): only voice indices below the
        // fixed 16-instrument orchestra (D1) have a channel/buffer at all — the
        // vi bound here is MANDATORY, not defensive style. An engine switch
        // into Csound while >16 voices are already active must not read past
        // CsoundEngine's fixed-size per-voice array (confirmed crash vector,
        // adversarial review); voices >= kMaxVoices simply keep rendering
        // silently (null csoundBuf, SynthVoice's own safety net) until they
        // end — no forced note-off on an engine switch, ever, in this codebase.
        // csoundVoiceBufs[vi] is block-aligned (starts at sample 0 of THIS
        // host block, Phase-2 spec S7), unlike lfo1Buf/lfo2Buf/lfo3Buf above
        // which the caller already offset by startSample — so the
        // +startSample offset is applied here, once, matching what those
        // buffers already reflect.
        const float* csoundVoiceBuf = nullptr;
        if (csoundVoiceBufs != nullptr && vi < CsoundEngine::kMaxVoices)
        {
            if (const float* base = csoundVoiceBufs[vi])
                csoundVoiceBuf = base + startSample;
        }
        v.renderBlock(scratch, scratchRight, performanceParams, lfo1Buf, lfo2Buf, lfo3Buf, numSamples,
                      csoundVoiceBuf);

        if (!v.isActive())
        {
            voiceSourceId[static_cast<size_t>(vi)] = -1;
            voicePan[static_cast<size_t>(vi)] = 0.0f;
            voiceMidiChannel_[static_cast<size_t>(vi)] = 0;
            voiceMpePressure_[static_cast<size_t>(vi)] = 0.0f;
            v.setPerVoicePitchBend(0.0f);
            sustainedVoice[static_cast<size_t>(vi)] = false;
            sostenutoVoice[static_cast<size_t>(vi)] = false;
            sostenutoReleasedVoice[static_cast<size_t>(vi)] = false;
            anyBecameInactive = true;
        }

        activeIndices[activeCount++] = vi;
    }

    // ── Sum voice buffers + apply gain ramp ──
    for (int i = 0; i < numSamples; ++i)
    {
        if (gainRampSamplesLeft > 0)
        {
            currentGain += gainRampIncr;
            if (--gainRampSamplesLeft == 0)
                currentGain = targetGain;
        }

        float monoSum = 0.0f;
        float leftSum = 0.0f;
        float rightSum = 0.0f;
        for (int a = 0; a < activeCount; ++a)
        {
            const int vi = activeIndices[a];
            const float sampleLeft = voiceScratch[static_cast<size_t>(vi)][static_cast<size_t>(i)];
            const float sampleRight = voiceScratchRight[static_cast<size_t>(vi)][static_cast<size_t>(i)];
            const float sampleMono = 0.5f * (sampleLeft + sampleRight);
            monoSum += sampleMono;

            if (voiceSourceId[static_cast<size_t>(vi)] < 0
                || voiceSourceId[static_cast<size_t>(vi)] > 3)
            {
                leftSum += sampleLeft;
                rightSum += sampleRight;
            }
            else
            {
                float leftGain = 1.0f;
                float rightGain = 1.0f;
                equalPowerPan(voicePan[static_cast<size_t>(vi)], leftGain, rightGain);
                leftSum  += sampleMono * leftGain;
                rightSum += sampleMono * rightGain;
            }
        }

        const float outputGain = currentGain * performanceOutputGain();
        monoSum *= outputGain;
        leftSum *= outputGain;
        rightSum *= outputGain;

        if (numChannels <= 1)
        {
            buffer.setSample(0, startSample + i, monoSum);
        }
        else
        {
            buffer.setSample(0, startSample + i, leftSum);
            buffer.setSample(1, startSample + i, rightSum);
            for (int ch = 2; ch < numChannels; ++ch)
                buffer.setSample(ch, startSample + i, 0.5f * (leftSum + rightSum));
        }
    }

    // Update gain target if voices became inactive during this block
    if (anyBecameInactive)
        updateGainTarget();

    // Capture mod values from newest voice (end-of-block snapshot)
    if (newestIdx >= 0)
    {
        auto& nv = voices[static_cast<size_t>(newestIdx)];
        out.lastAmpVal = nv.getAmpEnvLevel();
        const float* mv = nv.getLastModVals();
        for (int m = 0; m < kNumModEnvs; ++m) out.lastModVal[m] = mv[m];
        out.lastModulatedCutoff = nv.getLastModulatedCutoff();
        out.lastModulatedResonance = nv.getLastModulatedResonance();
        out.lastModulatedScan = nv.getLastModulatedScan();
        out.lastModulatedNoiseLevel = nv.getLastModulatedNoiseLevel();
        out.lastTriggeredNote = nv.getCurrentNote();
        out.hasActiveVoices = true;
    }
    else
    {
        out.hasActiveVoices = hasActiveVoices();
    }

    return out;
}

void VoiceManager::writeCsoundControls(CsoundEngine* const* engines, int numEngines,
                                       float performancePitchRatio, int samplesSinceLastWrite,
                                       const BlockParams* modParams,
                                       float lfo1Raw, float lfo2Raw, float lfo3Raw)
{
    // Phase-1 spec §3/D2 (extended Phase-2 spec S6): publishes CURRENT voice
    // state to the orchestra's cached channels, right before the processor
    // pumps csoundPerformKsmps forward — so gate/freq/vel/pres/timb/trig
    // always reflect every event dispatched so far this block. Only voices
    // 0..kMaxVoices-1 have a Csound instrument at all (D1's fixed 16-voice
    // orchestra); voices beyond that are silently skipped here (they still
    // render — silently — via SynthVoice's own null-csoundBuf_ safety net
    // until they end).
    //
    // S6: readCsoundFreq() ADVANCES the per-voice glide smoother by
    // samplesSinceLastWrite — it must be called exactly ONCE per write point,
    // never once per engine, or a two-engine crossfade would advance the
    // glide twice as fast as a single-engine write (subtle, audible). Compute
    // each voice's controls ONCE here, then fan the SAME values out to every
    // engine in `engines` (numEngines is 1 during normal play, 2 while
    // crossfading to a new orchestra).
    for (int vi = 0; vi < CsoundEngine::kMaxVoices; ++vi)
    {
        auto& v = voices[static_cast<size_t>(vi)];

        CsoundEngine::VoiceControls c;
        // D3: gate = voice ACTIVE (including release), never bare note-held —
        // closing the gate on note-off would abort the release tail; the
        // orchestra's own portk declick (~8ms) is inaudible under the VCA,
        // which is already at/heading to 0 by then.
        c.gate     = v.isActive() ? 1.0f : 0.0f;
        // Composition mirrors effectivePitchRatio (SynthVoice.cpp): smoothed
        // base Hz × global performance pitch ratio × per-voice MPE bend ×
        // the PITCH MODULATION BUS.
        //
        // That last factor was missing, and its absence is what made vibrato
        // not work on the Csound engine at all: an LFO routed to Pitch reached
        // wavetable/sampler/freeze (which resolve the bus per sample inside
        // renderBlock) and reached the orchestra never, because the only pitch
        // factors published here were the two BENDS. The wire existed, carried
        // a value, and nobody summed the bus into it -- so vibrato was not weak
        // on the LCO, it was absent. Both paths now call the same
        // SynthVoice::pitchBusSemitones, which is the point of it being one
        // function rather than a fourth hand-kept copy.
        //
        // KNOWN LIMITS of publishing the bus here rather than per sample. Both
        // were found by adversarial review, and neither is "coarseness":
        //
        //  (a) The control rate is the HOST BLOCK, so a fast LFO ALIASES to a
        //      WRONG RATE, not merely a stepped one. Writes happen once per MIDI
        //      sub-segment -- once per block with no event inside. At 2048
        //      samples / 48 kHz that is 23.4 Hz, so anything above ~11.7 Hz
        //      folds: a 20 Hz LFO (the rate range reaches 30) reads as 3.4 Hz on
        //      the orchestra while the internal engines play it at 20. The
        //      sampling instants are also non-uniform, because MIDI events split
        //      the block -- in dense passages the vibrato is jittered by note
        //      timing. Correct for the slow vibrato this fixes; wrong above
        //      ~11.7 Hz at 2048, ~16.9 Hz at 1024. `kfreq` carries no portk in
        //      the orchestra (only `kgate` does), so nothing smooths it after.
        //
        //  (b) Trig-mode LFOs are invisible here. SynthVoice::renderBlock swaps
        //      in the per-voice, note-reset perVoiceLfoBuf*_ when p.lfoNTrigMode
        //      -- but it FILLS those buffers itself, and it runs AFTER this
        //      write. So at this point the per-voice LFO has not been advanced
        //      for this block and cannot be read; the orchestra gets the global
        //      free-running LFO instead. Same rate, wrong phase: with LFO1 in
        //      Trig mode routed to Pitch, the other three engines give note-
        //      synced vibrato and the LCO gives free-running. Advancing the
        //      per-voice LFO from here would be the S6 double-advance trap in a
        //      second place; the real fix is ordering, not a peek.
        c.freqHz   = v.readCsoundFreq(samplesSinceLastWrite)
                   * performancePitchRatio
                   * std::exp2(v.getPerVoicePitchBend() / 12.0f)
                   * (modParams != nullptr
                        ? v.pitchBusRatioFromRawLfo(*modParams, lfo1Raw, lfo2Raw, lfo3Raw)
                        : 1.0f);
        c.velocity = v.getCurrentVelocity();
        c.pressure = pressureForVoice(vi);
        // The `timb` channel a body reads is 0..1 and means "how far the player
        // has pushed Y", so it takes the UPWARD half of the signed travel. On a
        // controller whose Y rests at the bottom -- the Osmose, and every
        // Initial-position device -- that is the whole travel and nothing
        // changes. On an Initial-64 controller the downward half has nowhere to
        // go in a 0..1 channel; better a body that rests at 0 everywhere than
        // one that rests at half scale on some instruments.
        c.timbre   = juce::jmax(0.0f, v.getTimbre());
        // D8: wrap to stay float-exact (well inside float's 24-bit exact-
        // integer range) — the orchestra only needs changed2() to detect a
        // step, not the absolute value.
        c.trigEpoch = static_cast<float>(v.triggerEpoch % 1000000ULL);

        for (int e = 0; e < numEngines; ++e)
            if (engines[e] != nullptr)
                engines[e]->setVoiceControls(vi, c);
    }
}

void VoiceManager::writeCsoundControls(CsoundEngine& cs, float performancePitchRatio,
                                       int samplesSinceLastWrite)
{
    // Back-compat single-engine convenience (Phase-1 call sites, e.g.
    // tools/audition_csound_engine.cpp) — forwards to the array form above
    // with numEngines=1, so the single-advance guarantee (S6) applies here too.
    CsoundEngine* engines[1] = { &cs };
    writeCsoundControls(engines, 1, performancePitchRatio, samplesSinceLastWrite);
}

void VoiceManager::snapshotCsoundState(float epochsOut[], float freqsOut[],
                                       float performancePitchRatio)
{
    // Phase-2 (spec S3/S4): read-only snapshot for CsoundEngine::primeForTakeover,
    // captured by the requester (PluginProcessor::requestCsoundOrchestra, message
    // thread, under getCallbackLock — never the audio thread). readCsoundFreq(0)
    // is a pure peek: samplesToAdvance<=0 skips the smoother's .skip() call (see
    // its own header comment), so this never advances glide state and is safe to
    // call off the audio thread.
    //
    // Mirrors writeCsoundControls' formula EXCEPT its pitch-modulation-bus
    // factor, and cannot do otherwise: this runs on the message thread, where no
    // LFO sample for a given block position exists. The divergence is bounded
    // and deliberate -- primeForTakeover settles the fresh orchestra for 0.25 s
    // with the gate CLOSED, so the cost is that its filter/reverb state settles
    // at the unmodulated pitch instead of the vibrato'd one. Do not "fix" this
    // by reaching for a live LFO value here; the honest options are to accept it
    // or to snapshot the bus factor alongside the freq at request time.
    for (int vi = 0; vi < CsoundEngine::kMaxVoices; ++vi)
    {
        auto& v = voices[static_cast<size_t>(vi)];
        epochsOut[vi] = static_cast<float>(v.triggerEpoch % 1000000ULL);
        freqsOut[vi]  = v.readCsoundFreq(0)
                      * performancePitchRatio
                      * std::exp2(v.getPerVoicePitchBend() / 12.0f);
    }
}

// ═══════════════════════════════════════════════════════════════════
// Engine data distribution
// ═══════════════════════════════════════════════════════════════════

void VoiceManager::setEngineMode(SynthVoice::EngineMode mode)
{
    for (auto& v : voices)
        v.setEngineMode(mode);
}

void VoiceManager::drainRetiredSamplerSnapshots()
{
    for (auto& v : voices)
        v.getSampler().drainRetiredSnapshot();
}

void VoiceManager::distributeSamplerBuffer(const SamplePlayer& master, float morphMs, bool allowMorph)
{
    currentSamplerMaster_ = &master;
    for (auto& v : voices)
    {
        if (v.isActive() && v.getEngineMode() == SynthVoice::EngineMode::Sampler)
        {
            // Held sampler note: equal-power crossfade-follow the freshly published
            // snapshot (over morphMs = Drift Crossfade) so a held tone plays the
            // CURRENT sample during A/B-drift regenerate. Crossfade ONLY on the
            // audio-thread pass (allowMorph) — morphToBufferFrom's own contract
            // confines it to the audio thread (its doc comment in SamplePlayer.h).
            // It parks the displaced snapshot into retiredSnapshot_ with a real
            // atomic_store_explicit (release), which is what keeps it from racing
            // drainRetiredSamplerSnapshots()'s off-thread atomic_exchange on EVERY
            // format — including CLAP, where processBlock holds no lock at all
            // (see retiredSnapshot_'s declaration in SamplePlayer.h). The retired
            // snapshot is freed later by drainRetiredSamplerSnapshots(), off-thread.
            // Never shareBufferFrom a held voice — that hard-swaps mid-note and clicks.
            if (allowMorph)
                v.getSampler().morphToBufferFrom(master, morphMs);
            continue;
        }

        v.getSampler().shareBufferFrom(master);
    }
}

void VoiceManager::distributeWavetableFrames(const WavetableOscillator& masterOsc)
{
    currentWavetableMaster_ = &masterOsc;
    // A HELD wavetable-mode voice crossfades onto the new bank (morphToFramesFrom,
    // equal-power over the Regen XFade window — the held-note-live-follow
    // invariant); an inactive voice adopts it immediately (shareFramesFrom). Both
    // early-out on hasFrames(), so an empty master leaves every voice untouched.
    for (auto& v : voices)
    {
        if (v.isActive() && v.getEngineMode() == SynthVoice::EngineMode::Wavetable)
            v.getOsc().morphToFramesFrom(masterOsc);
        else
            v.getOsc().shareFramesFrom(masterOsc);
    }
}

void VoiceManager::distributeFreezeBuffer(const FreezeTextureEngine& masterFreeze, float morphMs, bool allowMorph)
{
    currentFreezeMaster_ = &masterFreeze;
    for (auto& v : voices)
    {
        const bool heldGranular = v.isActive()
            && v.getEngineMode() == SynthVoice::EngineMode::Freeze
            && v.getFreezeEngine().hasAudio();

        if (heldGranular)
        {
            // Held granular voice. allowMorph (off-audio-thread sites only) →
            // crossfade-adopt the new buffer live; otherwise keep the old buffer
            // until note-off (legacy behaviour, and the only RT-safe option on the
            // audio thread). Either way, never shareBufferFrom — that hard-swaps
            // mid-grain and clicks.
            if (allowMorph)
                v.getFreezeEngine().morphToBufferFrom(masterFreeze, morphMs);
            continue;
        }

        v.getFreezeEngine().shareBufferFrom(masterFreeze);
    }
}

// ═══════════════════════════════════════════════════════════════════
// Voice allocation helpers
// ═══════════════════════════════════════════════════════════════════

int VoiceManager::findFreeVoice() const
{
    for (int i = 0; i < voiceLimit; ++i)
    {
        if (i == droneVoiceIndex) continue;
        if (!voices[static_cast<size_t>(i)].isActive())
            return i;
    }
    return -1;
}

int VoiceManager::stealVoice() const
{
    // Tiered voice-stealing policy:
    //   Tier 0: releasing voices, oldest first (longest in release = most expendable)
    //   Tier 1: active held voices, lowest amplitude first, oldest as tiebreaker
    // The drone voice (sequencer-held step) is never stolen.
    //
    // getAmpEnvLevel() returns the last rendered amp-envelope level (0..1),
    // already computed in renderBlock — no extra CPU cost for the comparison.

    int bestIdx = -1;
    uint64_t bestTs = 0;

    // ── Tier 0: prefer releasing voices, oldest first ──
    for (int i = 0; i < voiceLimit; ++i)
    {
        if (i == droneVoiceIndex) continue;
        if (!voices[static_cast<size_t>(i)].isReleasing()) continue;

        if (bestIdx < 0 || voices[static_cast<size_t>(i)].noteOnTimestamp < bestTs)
        {
            bestTs = voices[static_cast<size_t>(i)].noteOnTimestamp;
            bestIdx = i;
        }
    }
    if (bestIdx >= 0) return bestIdx;

    // ── Tier 1: any active voice — lowest amplitude wins, oldest breaks ties ──
    float bestAmp = 2.0f;  // higher than any possible ampEnvLevel (0..1)
    for (int i = 0; i < voiceLimit; ++i)
    {
        if (i == droneVoiceIndex) continue;
        if (!voices[static_cast<size_t>(i)].isActive()) continue;

        float amp = voices[static_cast<size_t>(i)].getAmpEnvLevel();
        bool better = (amp < bestAmp)
                   || (amp == bestAmp && voices[static_cast<size_t>(i)].noteOnTimestamp < bestTs);
        if (better)
        {
            bestAmp = amp;
            bestTs  = voices[static_cast<size_t>(i)].noteOnTimestamp;
            bestIdx = i;
        }
    }

    // Fallback (should never be reached — stealVoice() is only called when
    // findFreeVoice() == -1, i.e. at least one non-drone voice is active).
    return bestIdx >= 0 ? bestIdx : 0;
}

int VoiceManager::getActiveVoiceCount() const
{
    int count = 0;
    for (const auto& v : voices)
    {
        if (v.isActive()) count++;
    }
    return count;
}

bool VoiceManager::hasActiveVoices() const
{
    for (const auto& v : voices)
    {
        if (v.isActive()) return true;
    }
    return false;
}

void VoiceManager::updateGainTarget()
{
    // Very gentle per-voice compensation (1/N^0.1):
    //   n=2  → -0.30 dB     n=8  → -0.90 dB
    //   n=4  → -0.60 dB     n=16 → -1.20 dB
    // Sub-perceptual per-voice so engaging a drone or adding a chord note
    // doesn't audibly duck the mix, while still leaving ~1.2 dB of headroom
    // at full 16-voice polyphony before the end limiter starts working.
    int n = getHeldVoiceCount();
    float newTarget = (n > 0) ? 1.0f / std::pow(static_cast<float>(n), 0.1f) : 1.0f;

    if (newTarget != targetGain)
    {
        targetGain = newTarget;
        int rampSamples = std::max(1, static_cast<int>(GAIN_RAMP_MS * 0.001f * static_cast<float>(sr)));
        gainRampIncr = (targetGain - currentGain) / static_cast<float>(rampSamples);
        gainRampSamplesLeft = rampSamples;
    }
}

int VoiceManager::getHeldVoiceCount() const
{
    int count = 0;
    for (const auto& v : voices)
    {
        if (v.isActive() && !v.isReleasing()) count++;
    }
    return count;
}

void VoiceManager::releaseSustainedVoices()
{
    for (int i = 0; i < MAX_VOICES; ++i)
    {
        auto& v = voices[static_cast<size_t>(i)];
        if (sustainedVoice[static_cast<size_t>(i)] && v.isActive() && !v.isReleasing())
        {
            if (hasCurrentBlockParams_)
                v.configureForBlock(applyPerformanceControllers(currentBlockParams_));
            if (sostenutoPedalDown && sostenutoVoice[static_cast<size_t>(i)])
                sostenutoReleasedVoice[static_cast<size_t>(i)] = true;
            else
                v.noteOff();
        }
        sustainedVoice[static_cast<size_t>(i)] = false;
    }
    updateGainTarget();
}

void VoiceManager::releaseSostenutoVoices()
{
    for (int i = 0; i < MAX_VOICES; ++i)
    {
        auto& v = voices[static_cast<size_t>(i)];
        if (sostenutoVoice[static_cast<size_t>(i)]
            && sostenutoReleasedVoice[static_cast<size_t>(i)]
            && v.isActive()
            && !v.isReleasing())
        {
            if (hasCurrentBlockParams_)
                v.configureForBlock(applyPerformanceControllers(currentBlockParams_));
            v.noteOff();
        }
        sostenutoVoice[static_cast<size_t>(i)] = false;
        sostenutoReleasedVoice[static_cast<size_t>(i)] = false;
    }
    updateGainTarget();
}

void VoiceManager::refreshPerformancePressure()
{
    for (int i = 0; i < MAX_VOICES; ++i)
        if (voices[static_cast<size_t>(i)].isActive())
            voices[static_cast<size_t>(i)].setAftertouch(pressureForVoice(i));
}

float VoiceManager::pressureForNote(int note) const
{
    note = juce::jlimit(0, 127, note);
    return juce::jmax(juce::jmax(channelPressure, modWheelPressure),
                      juce::jmax(breathPressure, polyPressureByNote[static_cast<size_t>(note)]));
}

float VoiceManager::pressureForVoice(int voiceIdx) const
{
    const auto& v = voices[static_cast<size_t>(voiceIdx)];
    return juce::jmax(pressureForNote(v.getCurrentNote()),
                      voiceMpePressure_[static_cast<size_t>(voiceIdx)]);
}

bool VoiceManager::isKeyHeldVoice(int i) const
{
    // A key that is DOWN, and nothing else.
    //   - releasing: its pressure is frozen at what it had when it was let go,
    //     and stays readable through the whole amp tail;
    //   - the damper and sostenuto: the voice sings on with the key up, and its
    //     stored pressure with it;
    //   - the drone: the step sequencer's, held with nobody at the keyboard;
    //   - anything the sequencers or the arpeggiator play: those are internal
    //     notes (MIDI channel 0), not hands.
    // Every one of them reads as a held key to a naive test, and each would let
    // a control that is meant to follow a finger follow something else instead.
    if (i == droneVoiceIndex) return false;
    const auto& v = voices[static_cast<size_t>(i)];
    return v.isActive()
        && ! v.isReleasing()
        && ! sustainedVoice[static_cast<size_t>(i)]
        && ! sostenutoReleasedVoice[static_cast<size_t>(i)]
        // An external key carries its MIDI channel; the computer keyboard plays
        // on none and is told apart by its source id. Both are hands. The
        // arpeggiator's held-key list covers them as well, but it is keyed by
        // note NUMBER - hold C4 on the computer keyboard, then play and release
        // C4 on the MIDI keyboard, and that list drops a key still held.
        && (voiceMidiChannel_[static_cast<size_t>(i)] > 0
            || voiceSourceId[static_cast<size_t>(i)] == kComputerKeyboardSourceId);
}

int VoiceManager::getKeyHeldVoiceCount() const
{
    int count = 0;
    for (int i = 0; i < MAX_VOICES; ++i)
        if (isKeyHeldVoice(i)) ++count;
    return count;
}

float VoiceManager::maxHeldExpression(int src) const
{
    // Furthest from rest, sign kept. For pressure and velocity that is the
    // largest value, since neither goes below zero; X and Y are both bipolar,
    // and there "leaning hardest" has to be able to lean the other way.
    // A target nobody wired reads rest, and says so before walking the pool.
    if (src == ExprSource::None)
        return 0.0f;

    float furthest = 0.0f;
    for (int i = 0; i < MAX_VOICES; ++i)
    {
        if (! isKeyHeldVoice(i))
            continue;
        const auto& v = voices[static_cast<size_t>(i)];
        float value = 0.0f;
        switch (src)
        {
            case ExprSource::Velocity: value = v.getCurrentVelocity();       break;
            case ExprSource::X:        value = v.getPerVoicePitchBendNorm(); break;
            case ExprSource::Y:        value = v.getTimbre();                break;
            default:                   value = pressureForVoice(i);          break;
        }
        if (std::abs(value) > std::abs(furthest))
            furthest = value;
    }
    return furthest;
}

float VoiceManager::performanceOutputGain() const
{
    const float softGain = softPedalDown ? 0.65f : 1.0f;
    return juce::jlimit(0.0f, 1.0f, channelVolumeGain * expressionGain * softGain);
}

BlockParams VoiceManager::applyPerformanceControllers(const BlockParams& bp) const
{
    auto out = bp;
    out.performancePitchRatio = std::pow(2.0f, pitchBendSemitones / 12.0f);
    if (softPedalDown)
        out.baseCutoff *= 0.72f;
    return out;
}

// ═══════════════════════════════════════════════════════════════════
// Drone (step-hold) handling
// ═══════════════════════════════════════════════════════════════════

void VoiceManager::setDroneNote(int note, float velocity, bool lfo1TrigMode, bool lfo2TrigMode, bool lfo3TrigMode)
{
    note = juce::jlimit(0, 127, note);
    velocity = juce::jlimit(0.0f, 1.0f, velocity);

    // Same pitch as current drone → no-op (mouse stayed at same drag-Y).
    if (droneVoiceIndex >= 0 && droneNote == note)
        return;

    if (droneVoiceIndex < 0)
    {
        // ── First drone trigger: pick a voice and do a full noteOn on it. ──
        int idx;
        if (isMono())
        {
            idx = 0;
        }
        else
        {
            idx = findFreeVoice();
            if (idx < 0) idx = stealVoice();
        }

        auto& v = voices[static_cast<size_t>(idx)];
        v.setTuningTable(tuningHz_);
        if (hasCurrentBlockParams_)
            v.configureForBlock(applyPerformanceControllers(currentBlockParams_));
        if (v.isActive())
            v.beginRestartFade();
        sustainedVoice[static_cast<size_t>(idx)] = false;
        sostenutoVoice[static_cast<size_t>(idx)] = false;
        sostenutoReleasedVoice[static_cast<size_t>(idx)] = false;
        v.setAftertouch(pressureForNote(note));
        if (v.getEngineMode() == SynthVoice::EngineMode::Sampler && currentSamplerMaster_ != nullptr)
            v.getSampler().shareBufferFrom(*currentSamplerMaster_);
        if (v.getEngineMode() == SynthVoice::EngineMode::Wavetable && currentWavetableMaster_ != nullptr)
            v.getOsc().shareFramesFrom(*currentWavetableMaster_);
        if (v.getEngineMode() == SynthVoice::EngineMode::Freeze && currentFreezeMaster_ != nullptr)
            v.getFreezeEngine().shareBufferFrom(*currentFreezeMaster_);

        v.noteOn(note, velocity, false);
        voiceSourceId[static_cast<size_t>(idx)] = -1;
        voicePan[static_cast<size_t>(idx)] = 0.0f;
        voiceMidiChannel_[static_cast<size_t>(idx)] = 0;  // drone is not an MPE note
        voiceMpePressure_[static_cast<size_t>(idx)] = 0.0f;
        v.setPerVoicePitchBend(0.0f);
        v.noteOnTimestamp = ++noteOnCounter;
        v.triggerEpoch = v.noteOnTimestamp;  // genuine fresh strike (D8) — first drone trigger
        if (lfo1TrigMode) v.getPerVoiceLfo1().reset();
        if (lfo2TrigMode) v.getPerVoiceLfo2().reset();
        if (lfo3TrigMode) v.getPerVoiceLfo3().reset();
        if (v.getEngineMode() == SynthVoice::EngineMode::Sampler && v.getSampler().hasAudio())
            v.getSampler().retrigger();
        if (v.getEngineMode() == SynthVoice::EngineMode::Wavetable)
        {
            v.getSampler().stop();
            v.getOsc().retriggerAutoScan();
        }
        droneVoiceIndex = idx;
    }
    else
    {
        // ── Drone pitch change while held: glide on same voice, no env retrigger. ──
        auto& v = voices[static_cast<size_t>(droneVoiceIndex)];
        v.setTuningTable(tuningHz_);
        if (hasCurrentBlockParams_)
            v.configureForBlock(applyPerformanceControllers(currentBlockParams_));
        v.setAftertouch(pressureForNote(note));
        v.glideToNote(note, 15.0f);  // short glide keeps mouse-drag scrubs click-free
    }

    droneNote = note;
    updateGainTarget();
}

void VoiceManager::clearDroneNote()
{
    if (droneVoiceIndex < 0) return;
    auto& v = voices[static_cast<size_t>(droneVoiceIndex)];
    if (v.isActive())
        v.noteOff();
    droneVoiceIndex = -1;
    droneNote = -1;
    updateGainTarget();
}
