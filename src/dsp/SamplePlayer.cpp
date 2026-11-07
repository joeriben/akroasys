#include "SamplePlayer.h"
#include "LoopFinder.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

#define SAMPLER_DEBUG_LOG 0

namespace
{
constexpr float kNormalizeCeilingDb = -1.0f;
constexpr float kSustainedTargetDb = -18.0f;
constexpr float kTransientPercentileTargetDb = -10.0f;
constexpr float kNearSilentPeakDb = -36.0f;
constexpr float kNearSilentActiveDb = -50.0f;
constexpr float kHotHeadroomDb = 2.0f;
constexpr float kShortTransientSeconds = 0.75f;
constexpr float kActiveBlockThresholdDb = -40.0f;
constexpr float kTransientActiveRatio = 0.35f;
constexpr float kTransientCrestDb = 18.0f;
constexpr float kTransientPeakGapDb = 8.0f;
// Inside this much of the sample's own pitch, the direct read IS the better
// answer as well as the cheaper one: it hands the recorded samples through
// untouched, where the stretcher would take them apart and rebuild them.
constexpr float kNearUnitySemitones = 0.1f;

#if SAMPLER_DEBUG_LOG
juce::String samplerPtrTag(const void* ptr)
{
    return "0x" + juce::String::toHexString(static_cast<juce::int64>(reinterpret_cast<std::uintptr_t>(ptr)));
}

void samplerDebugLog(const juce::String& message)
{
    juce::Logger::writeToLog("[SamplerDebug] " + message);
    juce::FileOutputStream out(juce::File("/tmp/t5ynth_sampler_debug.log"));
    if (out.openedOk())
    {
        out << "[SamplerDebug] " << message << juce::newLine;
        out.flush();
    }
}
#else
// When debug logging is disabled, replace the call with a no-op so the
// expensive string-concat arguments (juce::String + samplerPtrTag +
// debugStateString) are never evaluated. The previous `if constexpr` lived
// inside the function body — too late: the caller had already built the
// final juce::String via heap allocations on the audio thread.
#define samplerDebugLog(...) ((void)0)
#endif

const char* loopModeName(SamplePlayer::LoopMode mode)
{
    switch (mode)
    {
        case SamplePlayer::LoopMode::OneShot:  return "OneShot";
        case SamplePlayer::LoopMode::Loop:     return "Loop";
        case SamplePlayer::LoopMode::PingPong: return "PingPong";
    }
    return "?";
}

float dbToGain(float db)
{
    return juce::Decibels::decibelsToGain(db);
}

float gainToDb(float gain)
{
    return juce::Decibels::gainToDecibels(std::max(gain, 1.0e-9f));
}
}

void SamplePlayer::prepare(double sampleRate, int samplesPerBlock)
{
    playbackSampleRate = sampleRate;
    maxBlockSize = samplesPerBlock;
    rawReadBuf.resize(static_cast<size_t>(samplesPerBlock));
    prepareStretcher();
}

void SamplePlayer::reset()
{
    originalBuffer.setSize(0, 0);
    playBuffer.setSize(0, 0);
    // playbackSnapshot_/retiredSnapshot_ are republished/parked through the
    // atomic free-function API (see their declarations in SamplePlayer.h and
    // applyPreparedPlaybackState/morphToBufferFrom/drainRetiredSnapshot) and
    // read the same way by audio-thread voices — that pairing is what makes
    // them safe on every format, CLAP included, while the plugin is running.
    // This reset(), by contrast, clears them with a PLAIN .reset(): safe not
    // because of the atomics or getCallbackLock(), but because the only path
    // in is T5ynthProcessor::releaseResources (master directly, voices through
    // VoiceManager::reset() -> SynthVoice::reset()), which JUCE's contract
    // guarantees runs with the audio callback stopped entirely, on every
    // format — there is no concurrent audio-thread access to exclude here, so
    // no atomic and no lock is needed. reset() also rewrites the equally
    // non-atomic originalBuffer/playBuffer/audioLoaded that the worker
    // mutates under getCallbackLock() off-thread; that part of the argument
    // (worker excluded) DOES depend on releaseResources holding the lock,
    // which it does on every format.
    playbackSnapshot_.reset();
    retiredSnapshot_.reset();
    morphFromSnapshot_.reset();
    audioLoaded = false;
    playing = false;
    readPosition = 0.0;
    sourceGain_ = 1.0f;
    morphActive_ = false;
    morphAlpha_ = 1.0f;
    morphIncrement_ = 0.0f;
    morphFromGain_ = 1.0f;
    morphRecoverTarget_ = 1.0f;
    morphTail_.active = false;
    sharedMode = false;
    stretcher.reset();
    pitchModReachSemis_ = 0.0f;
    pitchPathLatched_ = false;
    pitchPathUsesStretch_ = false;
}

void SamplePlayer::loadBuffer(const juce::AudioBuffer<float>& buffer, double bufferSampleRate)
{
    if (sharedMode)
    {
        samplerDebugLog("loadBuffer ignored sharedMode player=" + samplerPtrTag(this));
        return; // shared-mode players don't own audio
    }

    samplerDebugLog("loadBuffer player=" + samplerPtrTag(this)
                    + " samples=" + juce::String(buffer.getNumSamples())
                    + " sr=" + juce::String(bufferSampleRate, 2));
    const auto config = capturePrepareConfig();
    applyPreparedBufferLoad(prepareBufferLoad(buffer, bufferSampleRate, config), config);
    playing = true;
}

SamplePlayer::PrepareConfig SamplePlayer::capturePrepareConfig() const
{
    PrepareConfig config;
    config.loopMode = loopMode;
    config.crossfadeMs = crossfadeMsVal;
    config.normalizeOn = normalizeOn;
    config.loopOptimizeLevel = loopOptimizeLevel;
    config.startPosFrac = startPosFrac;
    config.loopStartFrac = loopStartFrac;
    config.loopEndFrac = loopEndFrac;
    return config;
}

SamplePlayer::PreparedBufferLoad SamplePlayer::prepareBufferLoad(const juce::AudioBuffer<float>& buffer,
                                                                double bufferSampleRate,
                                                                const PrepareConfig& config) const
{
    PreparedBufferLoad prepared;
    prepared.originalBuffer.makeCopyOf(buffer);
    trimLeadingSilence(prepared.originalBuffer);
    prepared.playbackState = preparePlaybackState(prepared.originalBuffer, bufferSampleRate, config);
    return prepared;
}

void SamplePlayer::applyPreparedBufferLoad(PreparedBufferLoad prepared, const PrepareConfig& config)
{
    originalBuffer = std::move(prepared.originalBuffer);
    loopMode = config.loopMode;
    crossfadeMsVal = config.crossfadeMs;
    normalizeOn = config.normalizeOn;
    loopOptimizeLevel = config.loopOptimizeLevel;
    startPosFrac = config.startPosFrac;
    loopStartFrac = config.loopStartFrac;
    loopEndFrac = config.loopEndFrac;
    applyPreparedPlaybackState(std::move(prepared.playbackState));
    sharedMode = false;
    needsReprepareFlag = false;
}

void SamplePlayer::shareBufferFrom(const SamplePlayer& master)
{
    bool wasShared = sharedMode;
    sharedMode = true;
    // The master republishes its snapshot with atomic_store_explicit (release;
    // see applyPreparedPlaybackState) precisely because getCallbackLock() does
    // not cover every caller of this function on every format — this can run
    // on the audio thread inside processBlock (unlocked on CLAP; locked for
    // its whole duration by the host wrapper on Standalone/VST3/AU) or
    // off-thread under an explicit ScopedLock(getCallbackLock()) — see
    // VoiceManager::distributeSamplerBuffer's callers. The atomic acquire load
    // below is the guard that holds on every format; the lock, where it is
    // also in effect, is a second one.
    playbackSnapshot_ = std::atomic_load_explicit(&master.playbackSnapshot_,
                                                  std::memory_order_acquire);
    if (playbackSnapshot_ == nullptr)
        playBuffer.setSize(0, 0);
    // The geometry comes from the snapshot just acquired, so it is the one
    // prepared for exactly these buffers (see PlaybackSnapshot); without a
    // snapshot there is no audio and the master's plain fields will do.
    if (playbackSnapshot_ != nullptr) adoptGeometry(*playbackSnapshot_);
    else
    {
        bufferOriginalSR = master.bufferOriginalSR;
        playStart = master.playStart;
        playEnd = master.playEnd;
        coldStart = master.coldStart;
        ppFadeStart_ = master.ppFadeStart_;
        ppFadeEnd_   = master.ppFadeEnd_;
        ppCorrStart_ = master.ppCorrStart_;
        ppCorrEnd_   = master.ppCorrEnd_;
        loopFadeSide_ = master.loopFadeSide_;
        loopMode = master.loopMode;
        audioLoaded = master.audioLoaded;
    }
    startPosFrac = master.startPosFrac;
    loopStartFrac = master.loopStartFrac;
    loopEndFrac = master.loopEndFrac;
    wtExtractStartFrac_ = master.wtExtractStartFrac_;
    wtExtractEndFrac_ = master.wtExtractEndFrac_;
    startPosOffset_ = master.startPosOffset_;
    sourceGain_ = master.sourceGain_;
    // A plain (non-crossfade) buffer sync — cancel any in-flight crossfade so the
    // freshly shared buffer plays directly. morphFromSnapshot_ is retained (freed
    // off-thread at the next morph/reset), never released on the audio thread.
    morphActive_ = false;
    // Only reset read position on first share, not on subsequent syncs
    if (!wasShared)
        readPosition = static_cast<double>(coldStart);
    needsReprepareFlag = false;

    samplerDebugLog("shareBufferFrom dst=" + samplerPtrTag(this)
                    + " src=" + samplerPtrTag(&master)
                    + " wasShared=" + juce::String(wasShared ? 1 : 0)
                    + " readPos=" + juce::String(readPosition, 2)
                    + " state={" + debugStateString() + "}");
}

void SamplePlayer::morphToBufferFrom(const SamplePlayer& master, float morphMs)
{
    // Live-follow crossfade for a playing shared voice. Runs ONLY on the AUDIO
    // THREAD, inside processBlock — VoiceManager.h documents allowMorph=true as
    // valid only at the audio-thread call site. On Standalone/VST3/AU that
    // means processBlock's host-wrapper lock is also in effect for this whole
    // function; on CLAP it means nothing lock-wise, because the
    // clap-juce-extensions wrapper calls processBlock with no lock held at all
    // (verified against clap-juce-wrapper.cpp: zero getCallbackLock
    // references). So the cross-instance read of master.playbackSnapshot_
    // below is an atomic_load (acquire), paired with
    // applyPreparedPlaybackState's atomic_store (release) — that pairing, not
    // the lock, is what is safe on every format. retiredSnapshot_ below is
    // likewise atomic, paired with drainRetiredSnapshot's atomic_exchange. The
    // voice-side pointers (playbackSnapshot_, morphFromSnapshot_) ARE plain —
    // they are never written from any other thread, audio or otherwise, so
    // touching them here needs no atomic and no lock of their own. Snapshot
    // discipline mirrors FreezeTextureEngine.
    auto masterSnap = std::atomic_load_explicit(&master.playbackSnapshot_,
                                                std::memory_order_acquire);
    if (masterSnap == nullptr || masterSnap == playbackSnapshot_)
        return;                              // no master audio, or already current —
                                             // pointer identity is the generation guard
                                             // (never restarts an in-flight crossfade)

    if (std::atomic_load_explicit(&retiredSnapshot_, std::memory_order_acquire) != nullptr)
        return;                              // reclaim slot still full — defer one
                                             // block (current crossfade stays valid)

    // A CROSSFADE MAY ALREADY BE RUNNING, and what happens then decides whether
    // this is a crossfade or a hard swap.
    //
    // Restarting the ramp unconditionally — which is what this did — makes the
    // buffer we are PLAYING the new fade-from and stands it up at full gain. Mid
    // crossfade that buffer is the fade-TO, i.e. the QUIET side: 10 ms into a
    // 200 ms fade it carries sin(0.05*pi/2) = 0.08 of the sound. So the restart
    // dropped the 0.997 side and promoted the 0.08 side — a hard swap, the one
    // thing this function exists to avoid. Measured, and it is not hypothetical:
    // a SNAP recall used to re-assert the slot's loop points AFTER
    // loadGeneratedAudio had published, which marked the master for a re-prepare
    // and landed a second publication of the SAME audio ~10 ms later. Nothing
    // about the sound had changed and the held note jumped anyway
    // (tools/audition_sampler_follow.cpp's republish cases: a sample-to-sample
    // step 13x the material's own largest, +3.2 dB of level in 5 ms).
    //
    // Two slots is all there is — a fade-from and a fade-to — so a third buffer
    // cannot arrive without one of the two currently sounding sides going away.
    // The rule is to drop whichever side is QUIETER, and it is the rule the
    // other two engines already run (WavetableOscillator::beginMorphToMipData,
    // FreezeTextureEngine::morphToBufferFrom — "promote the dominant side"):
    //
    //   alpha >= 0.5  the in-flight target has taken over. It becomes the
    //                 fade-from; the side dropped is the old fade-from, at
    //                 cos(alpha*pi/2) <= 0.707.
    //   alpha <  0.5  the fade-from is still dominant and stays. The side
    //                 dropped is the target, at sin(alpha*pi/2) <= 0.707.
    //
    // The ramp is reset in BOTH cases, so the newcomer enters at gain 0 and
    // contributes nothing to the step. What the step IS: the dropped side
    // vanishing at its current gain, PLUS the kept side stepping from its
    // current gain up to 1.0 — worst case sin(a*pi/2)*|dropped| +
    // (1-cos(a*pi/2))*|kept| below halfway, mirrored above it. Both terms are
    // small early and jointly largest right at the 0.5 boundary; the second one
    // is why this is not a "bounded by the dropped side alone" rule.
    //
    // Worst per-sample step, measured on the guard's material (220 Hz at 0.50
    // fading to 660 Hz at 0.85, third buffer 330 Hz at 0.70, 200 ms ramp, worst
    // of 8 sub-block offsets), against the two alternatives:
    //
    //   alpha   rotate-always (what this replaces)   THIS   re-point, keep ramp
    //   0.05                              1.176     0.075                 0.107
    //   0.20                              0.884     0.247                 0.317
    //   0.40                              0.659     0.530                 0.858
    //   0.495                             0.562     0.721                 1.088
    //   0.55 and up                                identical by construction
    //
    // Two things that table settles. Re-pointing the target WITHOUT resetting
    // the ramp — the obvious "gentler" move — substitutes one buffer for another
    // at the target's live gain, costs |new - old| there, and is worse than
    // either of the others past about a third of the ramp. And this rule is NOT
    // uniformly better than the unconditional rotate it replaces: in a narrow
    // band from about alpha 0.42 to 0.5 it costs up to 0.16 more, because the
    // kept side's step to unity is largest exactly where the rule still keeps it.
    // The band is 8% of the ramp; the case that made this audible sits at alpha
    // 0.05, where the step drops 15-fold and lands below the material's own
    // largest sample-to-sample motion (0.073). The threshold is where it is
    // because it is exact for two equally loud sides (the step comparison
    // reduces to cos > sin); moving it per-amplitude would beat the house rule
    // in that band and would be a rule the other two engines do not run.
    //
    // Either way playbackSnapshot_ ends up as the master's newest, which is a
    // lifetime invariant and not just tidiness: shareBufferFrom() overwrites
    // playbackSnapshot_ with a plain assignment on the audio thread when the
    // voice goes inactive, so a voice left holding a snapshot nobody else holds
    // would free its buffers there. Deferring adoption while the fade runs opens
    // exactly that window, for a whole Regen XFade.
    //
    // The cost of the shared rule, stated once so no caller is surprised: while
    // publications keep arriving faster than morphMs, every one of them restarts
    // the ramp, so the kept fade-from never ages out and the held note does not
    // walk forward through the generations — it reaches the newest buffer one
    // full morphMs after the stream stops, not during it. Wavetable and Freeze
    // behave the same way; the audition guard's "publication stream" case pins
    // the landing.
    //
    // The displaced snapshot — exactly one per call, either branch — is parked
    // for the off-thread drain rather than released here: its last reference
    // must not drop on the audio thread. The slot was just observed empty and
    // only the audio thread parks, so the std::vector free always lands in
    // drainRetiredSnapshot() off the audio thread. The store into
    // retiredSnapshot_ below is atomic_store_explicit (release), paired with
    // drainRetiredSnapshot's atomic_exchange_explicit — that pairing, not a
    // lock, is what keeps the park here and the drain there from ever
    // overlapping on every format, including CLAP (no processBlock lock
    // there). playbackSnapshot_ just above stays a plain write — this voice's
    // own copy of it is never touched by any other thread, only master.
    // playbackSnapshot_ (read above) is cross-thread and atomic.
    std::shared_ptr<const PlaybackSnapshot> outgoing;

    // Two sides can sound; the louder one stays and becomes (or remains) the
    // fade-from, the quieter one goes (a publication is a crossfade from what
    // sounds now, never a third voice). The rule compares the levels the sides
    // HAVE at this moment: a side whose head has ended (a OneShot that crossed
    // its marker) is silent, whatever the ramp says, so a dead fade-from is
    // never kept and brought back, and a dead target never displaces the
    // fade-from that is the only sound left. The kept side then carries on at
    // exactly its present level (morphFromGain_), the new sample fades in from
    // zero; only the dropped side steps out, at the lower of the two levels.
    const float rampNow     = juce::jlimit(0.0f, 1.0f, morphAlpha_);
    const bool  tailSounds  = morphActive_ && morphFromSnapshot_ != nullptr && morphTail_.active;
    const float fromGainNow = tailSounds ? fadeFromGain(morphFromGain_, morphRecoverTarget_, rampNow, morphRecoverAlpha_) : 0.0f;
    const float toGainNow   = headEnded_ ? 0.0f
                            : morphActive_ ? std::sin(rampNow * juce::MathConstants<float>::halfPi) : 1.0f;
    const bool keepFadeFrom = tailSounds && fromGainNow > toGainNow;
    if (keepFadeFrom)
    {
        outgoing = playbackSnapshot_;            // the quiet side is the target: it goes,
        if (! headEnded_)                        // the dominant fade-from keeps its place and level
            morphFromGain_ = fromGainNow;        // (under a dead head its running ramp goes on untouched, below)
    }
    else
    {
        outgoing           = morphFromSnapshot_; // prior fade-from (may be null)
        morphFromSnapshot_ = playbackSnapshot_;  // current playback → fade-from
        morphTail_.head    = currentHead();      // ... and keeps playing as it was
        morphTail_.geo     = currentGeometry();
        morphTail_.active  = ! headEnded_;       // (unless it had already ended)
        morphFromGain_     = toGainNow;          // ... at the level it has now
    }
    playbackSnapshot_ = masterSnap;              // adopt the new buffer
    std::atomic_store_explicit(&retiredSnapshot_, outgoing, std::memory_order_release);

    // Equal-power ramp over morphMs (the global Drift Crossfade). morphMs<=0 →
    // 1 sample = effectively instant. Only crossfade when there is a fade-from
    // buffer and the voice is sounding; otherwise this is a plain adopt.
    // A head that has ENDED stays ended: an ended OneShot is silence until the
    // next note, whichever buffer is published under it (before the outgoing
    // playhead existed the voice had already stopped here). The ramp restarts
    // only so that a newcomer can enter at zero; under a dead head there is
    // no newcomer, so a kept fade-from keeps its running ramp and finishes
    // its fade to silence on schedule, whatever stream of publications lands
    // meanwhile (restarting it stretched old material for as long as the
    // stream lasted: measured −2 dB after 2 s at 4 ms intervals).
    const int morphSamples = juce::jmax(1, juce::roundToInt(
        static_cast<double>(morphMs) * 0.001 * playbackSampleRate));
    if (! (keepFadeFrom && headEnded_))
    {
        // What the kept side recovers to: the voice's level at this moment.
        morphRecoverTarget_ = juce::jmin(1.0f, std::sqrt(fromGainNow * fromGainNow + toGainNow * toGainNow));
        morphAlpha_     = 0.0f;
        morphIncrement_ = 1.0f / static_cast<float>(morphSamples);
        morphRecoverAlpha_ = juce::jlimit(morphIncrement_, 1.0f,
                                          juce::jmin(static_cast<float>(0.010 * playbackSampleRate) / static_cast<float>(morphSamples),
                                                     0.25f));
    }
    morphActive_    = playing && morphFromSnapshot_ != nullptr;
    morphTail_.active = morphTail_.active && morphActive_;

    // Follow the new sample's region / loop / SR state — from the snapshot
    // just acquired, which carries the geometry prepared for its buffers.
    adoptGeometry(*masterSnap);
    startPosFrac        = master.startPosFrac;
    loopStartFrac       = master.loopStartFrac;
    loopEndFrac         = master.loopEndFrac;
    wtExtractStartFrac_ = master.wtExtractStartFrac_;
    wtExtractEndFrac_   = master.wtExtractEndFrac_;
    startPosOffset_     = master.startPosOffset_;
    sharedMode          = true;
    needsReprepareFlag  = false;
    // readPosition / playDirection_ / inFirstPass_ deliberately preserved
    // (playback continuity). sourceGain_ preserved (per-voice noteOn
    // normalization; configureForBlock re-asserts it each block).

    samplerDebugLog("morphToBufferFrom dst=" + samplerPtrTag(this)
                    + " src=" + samplerPtrTag(&master)
                    + " morphMs=" + juce::String(morphMs, 1)
                    + " active=" + juce::String(morphActive_ ? 1 : 0)
                    + " readPos=" + juce::String(readPosition, 2)
                    + " state={" + debugStateString() + "}");
}

void SamplePlayer::drainRetiredSnapshot()
{
    // Every caller (VoiceManager::drainRetiredSamplerSnapshots, called from
    // PluginProcessor.cpp's serviceSamplerReprepare/loadGeneratedAudio/
    // reloadProcessedAudio) already holds getCallbackLock() for its own,
    // broader reasons — that serializes this against any OTHER off-thread
    // caller on Standalone/VST3/AU. It does NOT protect against the audio
    // thread on CLAP: the clap-juce-extensions wrapper calls processBlock with
    // no lock held at all (verified against clap-juce-wrapper.cpp — zero
    // getCallbackLock references, processBlock called bare). What actually
    // keeps this exchange from racing morphToBufferFrom's atomic park on the
    // audio thread, on every format including CLAP, is that both sides use
    // the real atomic free-function API on retiredSnapshot_ (see its
    // declaration in SamplePlayer.h). Take the parked snapshot and let this
    // local drop the last reference here — the buffer free lands off the
    // audio thread.
    auto dead = std::atomic_exchange_explicit(&retiredSnapshot_,
                                              std::shared_ptr<const PlaybackSnapshot>{},
                                              std::memory_order_acq_rel);
    (void) dead;
}

void SamplePlayer::setMidiNote(int note)
{
    transposeRatio = std::pow(2.0, (note - 60) / 12.0);
    glideSamplesLeft = 0; // cancel any active glide
}

void SamplePlayer::setTransposeRatio(double ratio)
{
    transposeRatio = ratio;
    glideSamplesLeft = 0;
}

void SamplePlayer::glideToSemitones(int semitones, float durationMs)
{
    glideToRatio(std::pow(2.0, semitones / 12.0), durationMs);
}

void SamplePlayer::glideToRatio(double targetRatio, float durationMs)
{
    double durationSamples = (durationMs / 1000.0) * playbackSampleRate;
    int samples = std::max(1, static_cast<int>(durationSamples));

    glideTargetRatio = targetRatio;
    glideRatioIncr = (targetRatio - transposeRatio) / static_cast<double>(samples);
    glideSamplesLeft = samples;
}

void SamplePlayer::retrigger()
{
    // Fresh note: the render path is chosen again on this note's first block.
    // Above the empty-buffer early return — this is note state, not buffer
    // geometry, and must never leak from one note into the next.
    pitchPathLatched_ = false;
    pitchPathUsesStretch_ = false;

    const auto& buf = currentPlaybackBuffer();
    const int bufLen = buf.getNumSamples();
    if (bufLen == 0) return;

    // Effective P1 = base + modulation offset (Scan→P1 in Sampler mode)
    float effectiveP1 = juce::jlimit(0.0f, 1.0f, startPosFrac + startPosOffset_);

    // Convert P1 fraction to sample position
    int startSample = static_cast<int>(std::floor(effectiveP1 * bufLen));

    // Direction: a start at or beyond the loop END plays backwards into the
    // loop. Compared in samples against the seam the optimizer actually
    // chose, not against the raw marker fraction (they differ by up to the
    // search radius), so a note never starts forward beyond its loop end.
    bool reversed = (startSample >= playEnd);
    playDirection_ = reversed ? -1 : 1;
    inFirstPass_ = true;
    turnFade_ = 0;      // no reflection yet: no turnaround fade in flight

    readPosition = static_cast<double>(juce::jlimit(0, bufLen - 1, startSample));
    playing = true;
    headEnded_ = false;
    stretchDrainLeft_ = 0;
    // Fresh hard restart — stop any in-flight buffer-adoption crossfade so the
    // fading-out old buffer can't bleed into the new note's first samples.
    // morphFromSnapshot_ is retained (audio thread); released off-thread on the
    // next morphToBufferFrom park / reset.
    morphActive_ = false;
    morphTail_.active = false;
    morphAlpha_ = 1.0f;
    morphFromGain_ = 1.0f;
    morphRecoverTarget_ = 1.0f;

    if (stretcherPrepared)
    {
        stretcher.reset();
        stretcherNeedsPriming = true;
    }

    samplerDebugLog("retrigger player=" + samplerPtrTag(this)
                    + " startSample=" + juce::String(startSample)
                    + " effectiveP1=" + juce::String(effectiveP1, 4)
                    + " state={" + debugStateString() + "}");
}

void SamplePlayer::setLoopStart(float frac)
{
    float clamped = juce::jlimit(0.0f, loopEndFrac - 0.01f, frac);
    if (clamped != loopStartFrac)
    {
        loopStartFrac = clamped;
        needsReprepareFlag = true;
    }
}

void SamplePlayer::setLoopEnd(float frac)
{
    float clamped = juce::jlimit(loopStartFrac + 0.01f, 1.0f, frac);
    if (clamped != loopEndFrac)
    {
        loopEndFrac = clamped;
        needsReprepareFlag = true;
    }
}

void SamplePlayer::setLoopRegion(float startFrac, float endFrac)
{
    // Same clamps as the two single setters, applied to the pair in one step so
    // the region never passes through a wider intermediate one (see the header).
    const float s = juce::jlimit(0.0f, 0.99f, startFrac);
    const float e = juce::jlimit(s + 0.01f, 1.0f, endFrac);

    if (s != loopStartFrac || e != loopEndFrac)
    {
        loopStartFrac = s;
        loopEndFrac = e;
        needsReprepareFlag = true;
    }
}

void SamplePlayer::setStartPos(float frac)
{
    startPosFrac = juce::jlimit(0.0f, 1.0f, frac);
}

void SamplePlayer::setLoopMode(LoopMode mode)
{
    if (mode != loopMode) { loopMode = mode; needsReprepareFlag = true; }
}

void SamplePlayer::setCrossfadeMs(float ms)
{
    float clamped = juce::jlimit(0.0f, 500.0f, ms);
    if (clamped != crossfadeMsVal) { crossfadeMsVal = clamped; needsReprepareFlag = true; }
}

void SamplePlayer::setNormalize(bool on)
{
    if (on != normalizeOn) { normalizeOn = on; needsReprepareFlag = true; }
}

void SamplePlayer::setLoopOptimizeLevel(int level)
{
    int clamped = juce::jlimit(0, 2, level);
    if (clamped != loopOptimizeLevel) { loopOptimizeLevel = clamped; needsReprepareFlag = true; }
}

// ═══════════════════════════════════════════════════════════════════
// Buffer preparation (mirrors useSamplePlayer.ts prepareBuffer)
// ═══════════════════════════════════════════════════════════════════

void SamplePlayer::preparePlaybackBuffer()
{
    if (originalBuffer.getNumSamples() == 0)
        return;

    applyPreparedPlaybackState(
        preparePlaybackState(originalBuffer, bufferOriginalSR, capturePrepareConfig()));
    needsReprepareFlag = false;

    samplerDebugLog("preparePlaybackBuffer player=" + samplerPtrTag(this)
                    + " bufLen=" + juce::String(originalBuffer.getNumSamples())
                    + " norm=" + juce::String(normalizeOn ? 1 : 0)
                    + " state={" + debugStateString() + "}");
}

SamplePlayer::PreparedPlaybackState SamplePlayer::preparePlaybackState(
    const juce::AudioBuffer<float>& sourceBuffer, double sourceSampleRate, const PrepareConfig& config) const
{
    PreparedPlaybackState prepared;
    prepared.bufferOriginalSR = sourceSampleRate;
    prepared.audioLoaded = sourceBuffer.getNumSamples() > 0 && sourceBuffer.getNumChannels() > 0;
    prepared.loopMode = config.loopMode;

    if (!prepared.audioLoaded)
        return prepared;

    const int bufLen = sourceBuffer.getNumSamples();
    const int numCh  = sourceBuffer.getNumChannels();

    // Rounded, not floored/ceiled: the fractions come from sample-exact seams
    // (analyzeGeneratedRegions) and must come back as the same samples.
    int ls = juce::jlimit(0, bufLen, juce::roundToInt(config.loopStartFrac * static_cast<double>(bufLen)));
    int le = juce::jlimit(0, bufLen, juce::roundToInt(config.loopEndFrac   * static_cast<double>(bufLen)));

    if (le - ls < 4)
        le = std::min(bufLen, ls + 4);

    prepared.playBuffer.makeCopyOf(sourceBuffer);

    const float* data = numCh > 0 ? sourceBuffer.getReadPointer(0) : nullptr;
    const int fadeReq = std::max(0, static_cast<int>(config.crossfadeMs / 1000.0f
                                                     * static_cast<float>(sourceSampleRate)));
    // Marker search radius per optimizer level: Off = the markers as set,
    // Low = +-10 ms, High = +-50 ms (sampler-editor practice: the user's point
    // is intent, the clean splice is nearby).
    const int radius = config.loopOptimizeLevel <= 0 ? 0
                     : static_cast<int>((config.loopOptimizeLevel == 1 ? 0.010 : 0.050) * sourceSampleRate);

    if (config.loopMode == LoopMode::Loop)
    {
        LoopFinder::Seam seam;
        if (data != nullptr)
        {
            seam = radius > 0
                ? LoopFinder::findSeam(data, bufLen, fadeReq, ls - radius, ls + radius, le - radius, le + radius,
                                       4, bufLen, le - ls)
                : LoopFinder::scoreSeam(data, bufLen, ls, le, fadeReq);
        }
        if (seam.valid && seam.end - seam.start >= 4)
        {
            ls = seam.start;
            le = seam.end;
        }
        // The fade mixes the material BEFORE the loop start into the tail
        // (fadeSide -1): the wrap then lands on what is already the start, and
        // the first pass simply plays through it. A loop starting too close to
        // the buffer's beginning has no such material; then the material AFTER
        // the end is mixed into the head instead (fadeSide +1), and the first
        // pass reads a pristine copy so the attack does not pass through that
        // mixed head before the first wrap (what the old head crossfade did).
        // A start position beyond the loop end plays BACKWARDS into the loop;
        // that pass would run from the original material straight into the
        // baked tail at the loop end. It reads the pristine copy instead and
        // meets the faded tail only at its first wrap at the loop start, where
        // the tail's end is already the material before the start.
        const int fade = seam.valid ? seam.fade : 0;
        const int side = seam.valid ? seam.fadeSide : -1;
        applyLoopCrossfade(prepared.playBuffer, ls, le, fade, side, seam.valid ? seam.correlation : 0.0f);
        prepared.loopFadeSide = side;
        // Whether a first pass reads this copy is decided per note from its
        // direction (firstPassReadsPristine), because the start position can
        // move across the loop end after prepare (P1 drag, Scan -> P1).
        if (fade > 0)
            prepared.firstPassBuffer.makeCopyOf(sourceBuffer);

        prepared.playStart = ls;
        prepared.playEnd   = le;
        prepared.coldStart = ls;
    }
    else if (config.loopMode == LoopMode::PingPong)
    {
        // Each boundary is a turnaround: the forward continuation beyond it is
        // blended into the reversed playback over the crossfade time, so the point
        // must leave that much material beyond itself. The search prefers points
        // of local time-symmetry (LoopFinder::findTurnaround).
        int endPos = std::min(le, bufLen - 1);
        LoopFinder::Turnaround tE, tS;
        if (data != nullptr)
        {
            // A turnaround is clean only at a point of local time-symmetry, and
            // those are discrete: a marker set by hand is never one to the
            // sample. So even with the optimizer Off the point is searched
            // within +-2 ms of the marker; Low and High widen that. Neither
            // end may cross the other's half of the loop.
            const int ppRadius = std::max(radius, static_cast<int>(0.002 * sourceSampleRate));
            const int halfLen  = std::max(4, (le - ls) / 2);
            const int fadePP   = std::min(fadeReq, halfLen);   // what the runtime will play
            tE = LoopFinder::findTurnaround(data, bufLen, fadePP,
                                            std::max(ls + halfLen, endPos - ppRadius), endPos + ppRadius, endPos);
            if (tE.valid) endPos = tE.position;
            tS = LoopFinder::findTurnaround(data, bufLen, fadePP,
                                            ls - ppRadius, std::min(ls + ppRadius, endPos - halfLen), ls);
            if (tS.valid && tS.position <= endPos - 4) ls = tS.position;
        }
        le = juce::jlimit(std::min(ls + 4, bufLen), bufLen, endPos);
        const int halfLoop = (le - ls) / 2;
        prepared.ppFadeEnd   = juce::jlimit(0, std::max(0, std::min(bufLen - 1 - le, halfLoop)), fadeReq);
        prepared.ppFadeStart = juce::jlimit(0, std::max(0, std::min(ls, halfLoop)), fadeReq);
        prepared.ppCorrEnd   = tE.valid ? tE.correlation : 0.0f;
        prepared.ppCorrStart = tS.valid ? tS.correlation : 0.0f;

        prepared.playStart = ls;
        prepared.playEnd   = le;
        prepared.coldStart = ls;
    }
    else // OneShot
    {
        prepared.playStart = ls;
        prepared.playEnd   = le;
        prepared.coldStart = ls;
    }

    if (config.normalizeOn)
    {
        // The measured region is the hull of the start position and the loop:
        // a start before the loop plays forward into it, a start beyond the
        // loop end plays backwards into it, and neither pass may cross a level
        // step on the way.
        int p1 = static_cast<int>(std::floor(config.startPosFrac * bufLen));
        int normStart = std::min(p1, prepared.playStart);
        const int rs = juce::jlimit(0, prepared.playBuffer.getNumSamples(), normStart);
        const int re = juce::jlimit(rs, prepared.playBuffer.getNumSamples(), std::max(prepared.playEnd, p1 + 1));
        if (re > rs && prepared.playBuffer.getNumChannels() > 0)
        {
            const NormalizeAnalysis analysis = analyzeNormalizeRegion(prepared.playBuffer, rs, re, sourceSampleRate);
            const NormalizeMode mode = chooseNormalizeMode(analysis);
            const float gain = chooseNormalizeGain(analysis, mode);
            if (std::isfinite(gain) && std::abs(gain - 1.0f) >= 1.0e-4f)
            {
                // The gain is MEASURED on the play region but APPLIED to the
                // material the turnaround fades read beyond it as well
                // (readLoopedSample blends x[E + tau] in): a boundary the gain
                // stopped at would be a level step inside every turnaround.
                // ... plus the two samples a cubic tap reaches beyond either edge.
                const int rsApply = juce::jmax(0, rs - prepared.ppFadeStart - 2);
                const int reApply = juce::jmin(prepared.playBuffer.getNumSamples(), re + prepared.ppFadeEnd + 2);
                applyGainToRegion(prepared.playBuffer, rsApply, reApply, gain);
                if (prepared.firstPassBuffer.getNumSamples() == prepared.playBuffer.getNumSamples())
                    applyGainToRegion(prepared.firstPassBuffer, rsApply,
                                      juce::jmin(prepared.firstPassBuffer.getNumSamples(), reApply + 4), gain);

            }

            samplerDebugLog("normalizeBuffer mode=" + juce::String(normalizeModeName(mode))
                            + " gainDb=" + juce::String(gainToDb(gain), 2)
                            + " peakDb=" + juce::String(gainToDb(analysis.peak), 2)
                            + " p999Db=" + juce::String(gainToDb(analysis.percentilePeak), 2)
                            + " rmsDb=" + juce::String(gainToDb(analysis.rms), 2)
                            + " activeDb=" + juce::String(gainToDb(analysis.activeRms), 2)
                            + " crestDb=" + juce::String(analysis.crestDb, 2)
                            + " peakGapDb=" + juce::String(analysis.peakToPercentileDb, 2)
                            + " activeRatio=" + juce::String(analysis.activeRatio, 3)
                            + " dur=" + juce::String(analysis.durationSeconds, 3));
        }
    }


    return prepared;
}

void SamplePlayer::applyPreparedPlaybackState(PreparedPlaybackState preparedState)
{
    auto snapshot = std::make_shared<PlaybackSnapshot>();
    snapshot->playBuffer = std::move(preparedState.playBuffer);
    snapshot->firstPassBuffer = std::move(preparedState.firstPassBuffer);
    snapshot->bufferOriginalSR = preparedState.bufferOriginalSR;
    snapshot->playStart        = preparedState.playStart;
    snapshot->playEnd          = preparedState.playEnd;
    snapshot->coldStart        = preparedState.coldStart;
    snapshot->ppFadeStart      = preparedState.ppFadeStart;
    snapshot->ppFadeEnd        = preparedState.ppFadeEnd;
    snapshot->ppCorrStart      = preparedState.ppCorrStart;
    snapshot->ppCorrEnd        = preparedState.ppCorrEnd;
    snapshot->loopFadeSide     = preparedState.loopFadeSide;
    snapshot->loopMode         = preparedState.loopMode;
    snapshot->audioLoaded      = preparedState.audioLoaded;
    // Publish atomically (release): held voices read this with acquire from the
    // audio thread (morphToBufferFrom / shareBufferFrom). This republish itself
    // runs off-thread, under an explicit ScopedLock(getCallbackLock()) at every
    // live call site (applyPreparedBufferLoad, from serviceSamplerReprepare /
    // loadGeneratedAudio / reloadProcessedAudio in PluginProcessor.cpp). That
    // lock is NOT sufficient by itself: processBlock holds it for its whole
    // duration on Standalone/VST3/AU, but the CLAP build calls processBlock
    // with NO lock at all (the clap-juce-extensions wrapper takes none —
    // verified against clap-juce-wrapper.cpp: zero getCallbackLock references,
    // processBlock called bare at both its call sites). The atomic store here,
    // paired with the atomic_load_explicit reads in shareBufferFrom /
    // morphToBufferFrom, is what actually excludes the audio thread on every
    // format, CLAP included — no torn pointer, no UAF of the retired master
    // snapshot. The lock remains real and necessary for the other three
    // formats and for this class's non-atomic state; do NOT drop the atomics
    // trusting the lock alone, and do NOT drop the lock trusting the atomics
    // alone. (preparePlaybackBuffer() also reaches this function but currently
    // has no caller anywhere in src/; if it ever gains one, that caller MUST
    // hold getCallbackLock() too — the atomics alone do not cover this
    // function's OTHER, plain field writes.)
    // The master's plain copies of the geometry are for the off-thread
    // readers (processor, GUI). A voice takes the geometry from the snapshot
    // it acquires (shareBufferFrom / morphToBufferFrom), so an audio thread
    // that runs without the lock (CLAP) can neither see the previous
    // buffer's loop points with this buffer nor these loop points torn while
    // the next re-prepare writes them.
    bufferOriginalSR = preparedState.bufferOriginalSR;
    playStart = preparedState.playStart;
    playEnd = preparedState.playEnd;
    coldStart = preparedState.coldStart;
    ppFadeStart_ = preparedState.ppFadeStart;
    ppFadeEnd_   = preparedState.ppFadeEnd;
    ppCorrStart_ = preparedState.ppCorrStart;
    ppCorrEnd_   = preparedState.ppCorrEnd;
    loopFadeSide_ = preparedState.loopFadeSide;
    audioLoaded = preparedState.audioLoaded;

    std::atomic_store_explicit(&playbackSnapshot_,
                               std::shared_ptr<const PlaybackSnapshot>(std::move(snapshot)),
                               std::memory_order_release);

    playBuffer.setSize(0, 0);
    readPosition = static_cast<double>(coldStart);
}

// ═══════════════════════════════════════════════════════════════════
// processBlock
// ═══════════════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════════════
// Catmull-Rom cubic interpolation (4-point, no trig — ~20× faster than Lanczos)
// ═══════════════════════════════════════════════════════════════════

const juce::AudioBuffer<float>& SamplePlayer::currentPlaybackBuffer() const
{
    if (playbackSnapshot_ != nullptr)
        return playbackSnapshot_->playBuffer;
    return playBuffer;
}

const juce::AudioBuffer<float>& SamplePlayer::currentFirstPassBuffer() const
{
    if (playbackSnapshot_ != nullptr
        && playbackSnapshot_->firstPassBuffer.getNumSamples() > 0
        && playbackSnapshot_->firstPassBuffer.getNumChannels() > 0)
    {
        return playbackSnapshot_->firstPassBuffer;
    }

    return currentPlaybackBuffer();
}

double SamplePlayer::currentBufferOriginalSR() const
{
    if (playbackSnapshot_ != nullptr)
        return playbackSnapshot_->bufferOriginalSR;
    return bufferOriginalSR;
}

float SamplePlayer::cubicSample(double pos) const
{
    return cubicSampleFrom(currentPlaybackBuffer(), pos);
}

float SamplePlayer::playbackSample(double pos, bool useFirstPassBuffer) const
{
    // A head that has ended is silent in the estimate as in the live read.
    const float newS = headEnded_ ? 0.0f : cubicSampleFrom(
        useFirstPassBuffer ? currentFirstPassBuffer() : currentPlaybackBuffer(), pos);

    // Held-note Drift-Crossfade: equal-power blend the outgoing buffer in while
    // the crossfade ramps. All morph state is audio-thread-owned (morphToBufferFrom
    // runs on the audio thread), so these are plain reads. Both buffers are read at
    // the same read position; cubicSampleFrom bounds-clamps each independently, so a
    // length/region mismatch is at worst a brief, fading-out artefact, never an OOB.
    if (! morphActive_ || morphFromSnapshot_ == nullptr)
        return newS;

    const float g       = juce::jlimit(0.0f, 1.0f, morphAlpha_);
    const float newGain = std::sin(g * juce::MathConstants<float>::halfPi);
    const float oldGain = fadeFromGain(morphFromGain_, morphRecoverTarget_, g, morphRecoverAlpha_);
    if (! morphTail_.active)                 // the outgoing side has ended
        return newS * newGain;

    // A level ESTIMATE (estimatePlaybackRms simulates a pass from P1): the old
    // side follows the simulated pass's own pristine choice, as the new one does.
    const auto& oldBuf =
        (useFirstPassBuffer && morphFromSnapshot_->firstPassBuffer.getNumSamples() > 0)
            ? morphFromSnapshot_->firstPassBuffer
            : morphFromSnapshot_->playBuffer;
    const float oldS = cubicSampleFrom(oldBuf, pos);
    return newS * newGain + oldS * oldGain;
}

void SamplePlayer::adoptGeometry(const PlaybackSnapshot& snap)
{
    bufferOriginalSR = snap.bufferOriginalSR;
    playStart        = snap.playStart;
    playEnd          = snap.playEnd;
    coldStart        = snap.coldStart;
    ppFadeStart_     = snap.ppFadeStart;
    ppFadeEnd_       = snap.ppFadeEnd;
    ppCorrStart_     = snap.ppCorrStart;
    ppCorrEnd_       = snap.ppCorrEnd;
    loopFadeSide_    = snap.loopFadeSide;
    loopMode         = snap.loopMode;
    audioLoaded      = snap.audioLoaded;
}

SamplePlayer::Playhead SamplePlayer::currentHead() const
{
    Playhead h;
    h.pos = readPosition; h.dir = playDirection_; h.firstPass = inFirstPass_;
    h.turnPos = turnPos_; h.turnFade = turnFade_; h.turnCorr = turnCorr_;
    return h;
}

SamplePlayer::LoopGeometry SamplePlayer::currentGeometry() const
{
    LoopGeometry g;
    g.playStart = playStart; g.playEnd = playEnd;
    g.ppFadeStart = ppFadeStart_; g.ppFadeEnd = ppFadeEnd_;
    g.ppCorrStart = ppCorrStart_; g.ppCorrEnd = ppCorrEnd_;
    g.fadeSide = loopFadeSide_; g.mode = loopMode;
    return g;
}

float SamplePlayer::boundaryAwareSample(const juce::AudioBuffer<float>& buf, bool pristine,
                                        const Playhead& h, const LoopGeometry& g) const
{
    const int bufLen = buf.getNumSamples();
    if (bufLen <= 0 || buf.getNumChannels() <= 0)
        return 0.0f;
    const double pos = h.pos;
    if (pristine || g.mode == LoopMode::OneShot || g.playEnd - g.playStart < 2)
        return cubicSampleFrom(buf, pos);

    const int i1 = static_cast<int>(std::floor(pos));
    // Fast path: all four taps inside the loop region.
    if (i1 - 1 >= g.playStart && i1 + 2 < g.playEnd)
        return cubicSampleFrom(buf, pos);

    // A tap beyond a boundary reads where the playhead goes next: in Loop mode
    // the other end, in PingPong mode the mirror image about the boundary
    // (the position reflects as 2E - pos, so tap j maps to 2E - j). On the
    // first pass only the boundary the playhead is moving toward wraps: the
    // material before the loop (P1 < start) and beyond it (a reversed start)
    // is played as it is.
    const bool wrapFwd  = h.dir > 0 || ! h.firstPass;
    const bool wrapBack = h.dir < 0 || ! h.firstPass;
    const int  len      = g.playEnd - g.playStart;
    const bool pingPong = g.mode == LoopMode::PingPong;
    // A ping-pong boundary WITH a turnaround fade is approached reading the
    // real continuation beyond it (the fade starts from exactly that signal,
    // so the slope is continuous through the reflection); only a bare
    // reversal (fade 0) mirrors its taps.
    auto tap = [&](int j) -> int
    {
        if (j >= g.playEnd && wrapFwd)
            j = pingPong ? (g.ppFadeEnd > 0 ? j : 2 * g.playEnd - j) : j - len;
        else if (j < g.playStart && wrapBack)
            j = pingPong ? (g.ppFadeStart > 0 ? j : 2 * g.playStart - j) : j + len;
        return juce::jlimit(0, bufLen - 1, j);
    };
    const float* data = buf.getReadPointer(0);
    const float t  = static_cast<float>(pos - i1);
    const float p0 = data[tap(i1 - 1)], p1 = data[tap(i1)], p2 = data[tap(i1 + 1)], p3 = data[tap(i1 + 2)];
    const float a = -0.5f * p0 + 1.5f * p1 - 1.5f * p2 + 0.5f * p3;
    const float b =         p0 - 2.5f * p1 + 2.0f * p2 - 0.5f * p3;
    const float c = -0.5f * p0              + 0.5f * p2;
    return ((a * t + b) * t + c) * t + p1;
}

float SamplePlayer::loopedSampleFrom(const juce::AudioBuffer<float>& buf, bool pristine,
                                     const Playhead& h, const LoopGeometry& g) const
{
    const float s = boundaryAwareSample(buf, pristine, h, g);
    if (h.turnFade <= 0)      // no reflection in flight (Loop wraps clear it)
        return s;

    // After the reflection at `anchor` the playhead reads x[anchor - tau] for
    // growing tau; the signal that would have continued is x[anchor + tau].
    // Blending the two over the fade is the standard turnaround crossfade; the
    // gains follow the mirror correlation measured at prepare time (coherent
    // -> linear, incoherent -> equal power), the same law as the forward
    // loop's fade. The anchor is the reflection this playhead actually made.
    const double anchor = h.turnPos;
    const int    fade   = h.turnFade;
    const double tau = h.dir < 0 ? anchor - h.pos : h.pos - anchor;
    if (tau >= 0.0 && tau < static_cast<double>(fade))
    {
        const double fwdPos = h.dir < 0 ? anchor + tau : anchor - tau;
        const float forward = cubicSampleFrom(buf, fwdPos);     // the real continuation
        float gIn, gOut;
        LoopFinder::crossfadeGains(static_cast<float>(tau / fade), h.turnCorr, gIn, gOut);
        return s * gIn + forward * gOut;
    }
    return s;
}

float SamplePlayer::readLoopedSample(double pos, bool useFirstPassBuffer) const
{
    // "Pristine" only when a first-pass copy actually exists (fadeSide +1); on
    // the classical fade side currentFirstPassBuffer() falls back to the faded
    // playBuffer, whose tail must be read with wrapped taps or the first wrap
    // interpolates toward the un-faded original beyond the loop end.
    Playhead head = currentHead();
    head.pos = pos;
    const LoopGeometry geo = currentGeometry();
    const juce::AudioBuffer<float>& fpBuf = currentFirstPassBuffer();
    const juce::AudioBuffer<float>& pbBuf = currentPlaybackBuffer();
    const bool newPristine = useFirstPassBuffer && &fpBuf != &pbBuf;
    const float newS = headEnded_ ? 0.0f : loopedSampleFrom(newPristine ? fpBuf : pbBuf, newPristine, head, geo);

    // Held-note Drift-Crossfade: the outgoing buffer keeps playing exactly as
    // it did — its own playhead, loop points, fades, mode and last reflection
    // (morphTail_, advanced in advancePosition) — under the cos ramp, so a
    // regenerate changes nothing about what the outgoing side plays; only the
    // equal-power ramp moves the voice over. All morph state is
    // audio-thread-owned (morphToBufferFrom runs on the audio thread), so
    // these are plain reads.
    if (! morphActive_ || morphFromSnapshot_ == nullptr)
        return newS;

    const float g       = juce::jlimit(0.0f, 1.0f, morphAlpha_);
    const float newGain = std::sin(g * juce::MathConstants<float>::halfPi);
    const float oldGain = fadeFromGain(morphFromGain_, morphRecoverTarget_, g, morphRecoverAlpha_);
    if (! morphTail_.active)                 // the outgoing OneShot has ended
        return newS * newGain;

    const PlaybackSnapshot& snap = *morphFromSnapshot_;
    const bool oldPristine = firstPassReadsPristine(morphTail_.head, morphTail_.geo)
                             && snap.firstPassBuffer.getNumSamples() > 0;
    const float oldS = loopedSampleFrom(oldPristine ? snap.firstPassBuffer : snap.playBuffer, oldPristine,
                                        morphTail_.head, morphTail_.geo);
    return newS * newGain + oldS * oldGain;
}

// Gain of the fade-from side at ramp position g in [0,1]. A side that became
// the fade-from at full level follows the equal-power cos law. A side kept or
// promoted inside a running crossfade starts at the level it had (startLevel,
// so no step at the publication) and recovers over the first 10 ms of the new
// ramp (a raised cosine, so no kink either) to the level the voice had at the
// publication (target: both sides' power), then follows the cos law:
// level(g) = (startLevel + (target - startLevel) * r(g)) * cos(g*pi/2).
// Without the recovery each publication inside a crossfade would lower the
// kept side for good, and a stream of publications faster than the crossfade
// (a dragged loop point republishes every few ms) drained the held note
// toward silence (measured -17 dB at 17 ms intervals). A recovery spread over
// the whole ramp still lost up to 3.5 dB at intervals of 25-50 % of the
// crossfade; over 10 ms the summed power stays within a few percent of
// nominal at every interval, and the dropped side leaves at the lower of the
// two levels. The target is the voice's level, not full level: a side that
// is quiet because the note itself is fading (the other side has ended) must
// stay on its fade, not come back up by 16 dB.
float SamplePlayer::fadeFromGain(float startLevel, float target, float ramp, float recoverAlpha) noexcept
{
    const float g = juce::jlimit(0.0f, 1.0f, ramp);
    const float u = juce::jlimit(0.0f, 1.0f, g / juce::jmax(1.0e-6f, recoverAlpha));
    const float r = 0.5f - 0.5f * std::cos(u * juce::MathConstants<float>::pi);
    const float c = std::cos(g * juce::MathConstants<float>::halfPi);
    return (startLevel + (target - startLevel) * r) * c;
}

float SamplePlayer::cubicSampleFrom(const juce::AudioBuffer<float>& buf, double pos) const
{
    if (buf.getNumSamples() <= 0 || buf.getNumChannels() <= 0)
        return 0.0f;

    const float* data = buf.getReadPointer(0);
    const int bufLen = buf.getNumSamples();

    int i1 = static_cast<int>(std::floor(pos));
    float t = static_cast<float>(pos - i1);

    // Clamp the base index into the buffer FIRST, then derive all four cubic taps
    // from the clamped value so every data[] read stays in bounds even when pos
    // runs past the buffer. A held voice live-following onto a freshly-adopted
    // SHORTER buffer keeps its old readPosition (morphToBufferFrom preserves it),
    // so pos can exceed bufLen for the sample or two before advancePosition wraps
    // it. i0 was previously computed from the PRE-clamp i1 and read out of bounds
    // (crash: i0 = floor(pos)-1 far past a shorter buffer). t is the fractional
    // part (pos - floor pos) and is unaffected by the clamp.
    if (i1 < 0) i1 = 0;
    else if (i1 >= bufLen) i1 = bufLen - 1;
    int i0 = (i1 > 0) ? i1 - 1 : 0;
    int i2 = (i1 + 1 < bufLen) ? i1 + 1 : bufLen - 1;
    int i3 = (i1 + 2 < bufLen) ? i1 + 2 : bufLen - 1;

    float p0 = data[i0], p1 = data[i1], p2 = data[i2], p3 = data[i3];

    // Catmull-Rom spline: 12 mul + 8 add, zero trig
    float a = -0.5f * p0 + 1.5f * p1 - 1.5f * p2 + 0.5f * p3;
    float b =         p0 - 2.5f * p1 + 2.0f * p2 - 0.5f * p3;
    float c = -0.5f * p0              + 0.5f * p2;
    // d = p1
    return ((a * t + b) * t + c) * t + p1;
}

// ═══════════════════════════════════════════════════════════════════
// advancePosition — 3-point loop logic (shared by processSample/readRawSamples)
// ═══════════════════════════════════════════════════════════════════

bool SamplePlayer::advanceHead(Playhead& h, const LoopGeometry& g, double speedMagnitude, int bufLen)
{
    const double prevPosition = h.pos;
    h.pos += speedMagnitude * h.dir;

    const double pEnd   = static_cast<double>(g.playEnd);
    const double pStart = static_cast<double>(g.playStart);

    if (g.mode == LoopMode::OneShot)
    {
        // A OneShot note ends where it CROSSES its region boundary, or at the
        // end of the material. "Already outside" is not a crossing: a held
        // note whose mode changed under it (morphToBufferFrom copies the mode)
        // with the old loop's seam lying beyond the raw marker runs on instead
        // of being cut on the spot at full level.
        const bool crossedEnd   = prevPosition <  pEnd   && h.pos >= pEnd;
        const bool crossedStart = prevPosition >= pStart && h.pos <  pStart;
        if (crossedEnd || crossedStart || h.pos >= static_cast<double>(bufLen) || h.pos < 0.0)
        {
            h.firstPass = false;
            return false;
        }
        return true;
    }

    const bool pingPong = g.mode == LoopMode::PingPong;
    if (h.firstPass)
    {
        // During first pass, only check the boundary the playhead is moving toward
        if (h.dir > 0 && h.pos >= pEnd)
        {
            h.firstPass = false;
            const double overshoot = h.pos - pEnd;
            if (pingPong)
            {
                h.pos = pEnd - overshoot;
                h.dir = -1;
                h.turnPos = pEnd; h.turnFade = g.ppFadeEnd; h.turnCorr = g.ppCorrEnd;
            }
            else // Loop
            {
                h.pos = pStart + overshoot;
                h.turnFade = 0;
            }
        }
        else if (h.dir < 0 && h.pos < pStart)
        {
            h.firstPass = false;
            const double overshoot = pStart - h.pos;
            if (pingPong)
            {
                h.pos = pStart + overshoot;
                h.dir = 1;
                h.turnPos = pStart; h.turnFade = g.ppFadeStart; h.turnCorr = g.ppCorrStart;
            }
            else // Loop
            {
                h.pos = pEnd - overshoot;
                h.turnFade = 0;
            }
        }
    }
    else
    {
        // Standard looping between P2-P3
        if (h.pos >= pEnd)
        {
            const double overshoot = h.pos - pEnd;
            if (pingPong)
            {
                h.pos = pEnd - overshoot;
                h.dir = -1;
                h.turnPos = pEnd; h.turnFade = g.ppFadeEnd; h.turnCorr = g.ppCorrEnd;
            }
            else
            {
                h.pos = pStart + overshoot;
                h.turnFade = 0;
            }
        }
        else if (h.pos < pStart)
        {
            const double overshoot = pStart - h.pos;
            if (pingPong)
            {
                h.pos = pStart + overshoot;
                h.dir = 1;
                h.turnPos = pStart; h.turnFade = g.ppFadeStart; h.turnCorr = g.ppCorrStart;
            }
            else
            {
                h.pos = pEnd - overshoot;
                h.turnFade = 0;
            }
        }
    }
    return true;
}

bool SamplePlayer::advancePosition(double speedMagnitude)
{
    if (! headEnded_)
    {
        Playhead head = currentHead();
        if (! advanceHead(head, currentGeometry(), speedMagnitude, currentPlaybackBuffer().getNumSamples()))
            headEnded_ = true;
        readPosition   = head.pos;
        playDirection_ = head.dir;
        inFirstPass_   = head.firstPass;
        turnPos_ = head.turnPos; turnFade_ = head.turnFade; turnCorr_ = head.turnCorr;
    }

    // The outgoing side of a held-note morph walks on through ITS buffer at
    // its own speed (the two buffers may differ in original sample rate).
    if (morphActive_ && morphTail_.active && morphFromSnapshot_ != nullptr)
    {
        const double curSR  = currentBufferOriginalSR();
        const double tailSR = morphFromSnapshot_->bufferOriginalSR;
        const double tailSpeed = (curSR > 0.0 && tailSR > 0.0) ? speedMagnitude * (tailSR / curSR) : speedMagnitude;
        morphTail_.active = advanceHead(morphTail_.head, morphTail_.geo, tailSpeed,
                                        morphFromSnapshot_->playBuffer.getNumSamples());
    }

    // The voice ends when its own head has ended AND nothing is left fading
    // out: a tail still sounding under the cos ramp plays on (the incoming
    // side contributes silence, see readLoopedSample) until it ends or the
    // ramp completes.
    if (headEnded_ && ! (morphActive_ && morphTail_.active))
    {
        playing = false;
        return false;
    }

    // Advance the held-note Drift-Crossfade ramp one source-sample step. Called
    // once per emitted sample in the bypass path and once per consumed input
    // sample (1:1 with output per block) in the stretch path, so the crossfade
    // lasts morphMs of output either way. At alpha=1 the old buffer is fully gone.
    if (morphActive_)
    {
        morphAlpha_ += morphIncrement_;
        if (morphAlpha_ >= 1.0f)
        {
            morphAlpha_ = 1.0f;
            morphActive_ = false;
            // morphFromSnapshot_ retained (audio thread); released off-thread on the
            // next morphToBufferFrom park / reset — never freed here.
        }
    }

    return true;
}

// ═══════════════════════════════════════════════════════════════════
// processSample (Bypass mode / legacy — speed-based transposition)
// ═══════════════════════════════════════════════════════════════════

float SamplePlayer::processSample()
{
    const int regionLen = playEnd - playStart;
    if (regionLen <= 0 || !playing) return 0.0f;

    // Apply pitch glide (per-sample linear ramp)
    if (glideSamplesLeft > 0)
    {
        transposeRatio += glideRatioIncr;
        glideSamplesLeft--;
        if (glideSamplesLeft == 0)
            transposeRatio = glideTargetRatio;
    }

    // pitchModFactor belongs in the read speed, not only in the stretcher's
    // transposition: reading faster or slower IS how this path bends a note, so
    // leaving it out silently deletes everything routed to the pitch bus —
    // envelopes, LFOs, Drift, pitch bend, MPE per-note bend. It was invisible
    // while this path was only ever entered inside a 0.1-semitone dead zone
    // (0.6 cents of error at worst); once the render path is latched at the note's
    // first block it becomes unbounded, and a note held on the direct read simply
    // would not bend. Block-rate steps in the factor step the read speed, never
    // the position, so there is no seam — the same block-rate quantisation the
    // stretch path has.
    const double srRatio = currentBufferOriginalSR() / playbackSampleRate;
    double speedRatio = srRatio * transposeRatio * static_cast<double>(pitchModFactor);

    // Read with cubic interpolation. The first pass uses the linear source path
    // so loop crossfade/optimization is only heard after the first wrap.
    const bool useFirstPassBuffer = firstPassReadsPristine();
    float result = readLoopedSample(readPosition, useFirstPassBuffer) * sourceGain_;

    // Advance with 3-point logic (direction, first-pass, boundaries)
    advancePosition(std::abs(speedRatio));

    return result;
}

// ═══════════════════════════════════════════════════════════════════
// readRawSamples — read at 1:1 speed (SR-corrected, no transposition)
// ═══════════════════════════════════════════════════════════════════

void SamplePlayer::readRawSamples(float* output, int numSamples)
{
    const int regionLen = playEnd - playStart;
    if (regionLen <= 0 || !playing)
    {
        std::memset(output, 0, sizeof(float) * static_cast<size_t>(numSamples));
        return;
    }

    const double srRatio = currentBufferOriginalSR() / playbackSampleRate;

    for (int i = 0; i < numSamples; ++i)
    {
        const bool useFirstPassBuffer = firstPassReadsPristine();
        output[i] = readLoopedSample(readPosition, useFirstPassBuffer) * sourceGain_;

        if (!advancePosition(std::abs(srRatio)))
        {
            // The voice ended — zero-fill the remainder, and let the stretcher
            // play out what it still holds (renderPitchedBlock): its output
            // runs on for up to two windows after the input ends (measured on
            // both presets; the cheaper one needs a window plus two intervals,
            // the default one close to two windows).
            std::memset(output + i + 1, 0,
                        sizeof(float) * static_cast<size_t>(numSamples - i - 1));
            if (stretcherPrepared)
                stretchDrainLeft_ = 2 * stretcher.blockSamples();
            return;
        }
    }
}

// ═══════════════════════════════════════════════════════════════════
// renderPitchedBlock — pitch-preserving transposition via Signalsmith Stretch
// ═══════════════════════════════════════════════════════════════════

void SamplePlayer::renderPitchedBlock(float* output, int numSamples)
{
    if (!audioLoaded || (!playing && stretchDrainLeft_ <= 0))
    {
        std::memset(output, 0, sizeof(float) * static_cast<size_t>(numSamples));
        return;
    }
    if (!playing)
    {
        // Draining: the voice has ended on the stretch path; feed silence and
        // emit what the stretcher still holds, so the end is the material's
        // own last window and not a cut one window early.
        std::fill(rawReadBuf.begin(), rawReadBuf.begin() + numSamples, 0.0f);
        float* inPtr = rawReadBuf.data();
        stretcher.process(&inPtr, numSamples, &output, numSamples);
        stretchDrainLeft_ -= numSamples;
        return;
    }

    // Determine mode before glide advancement (use current ratio)
    double effectiveRatio = transposeRatio * static_cast<double>(pitchModFactor);
    float semitones = static_cast<float>(12.0 * std::log2(std::max(effectiveRatio, 1e-6)));

    // Which of the two render paths this note uses is decided ONCE, here, on its
    // first block, and then kept until the note retriggers.
    //
    // Deciding it per block — asking "is the CURRENT pitch inside the dead
    // zone?" — switches engines under a sounding note, and every such switch
    // costs an analysis window: the stretcher's input→output positional latency
    // is a FULL window, so entering mid-note restarted the heard stream ~50 ms
    // back into already-played material with a splice click, and leaving it
    // skipped ~100 ms forward, because readPosition leads the heard stream by
    // that window while stretching. A vibrato on a root-pitch note was enough
    // to trigger it.
    //
    // The reach the voice pushes (setPitchModulationReach) is how far pitch
    // bend and the pitch bus can carry this note at full scale, so a note that
    // can never leave the dead zone keeps the exact direct read for its whole
    // life, and one that can leave it is on the stretcher from its first
    // sample. Notes that DO stay on the direct read still follow their pitch
    // modulation — read faster or slower, so the colour rides along with the
    // pitch the way it does on a real instrument, which is what BJ chose over
    // formant-preserving vibrato (2026-07-26).
    //
    // Consequence, deliberate: a poly BIND glide (glideToNote → glideToRatio)
    // arrives mid-note and can take a latched direct-read note far outside the
    // dead zone. It stays on the direct read and the glide is heard tape-style.
    // Re-deciding there would reintroduce exactly the mid-note switch this
    // removes. The path it stays on is the shipped PitchShiftQuality::Bypass
    // path, not a new one.
    if (!pitchPathLatched_)
    {
        const float baseSemis = static_cast<float>(
            12.0 * std::log2(std::max(transposeRatio, 1e-6)));
        pitchPathUsesStretch_ =
            (std::abs(baseSemis) + pitchModReachSemis_) >= kNearUnitySemitones;
        pitchPathLatched_ = true;
    }

    // Bypass: processSample() handles glide per-sample — no block-level advancement
    if (pitchQuality == PitchShiftQuality::Bypass || !pitchPathUsesStretch_)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            output[i] = processSample();
            if (!playing)
            {
                std::memset(output + i + 1, 0,
                            sizeof(float) * static_cast<size_t>(numSamples - i - 1));
                break;
            }
        }
        return;
    }

    // Stretch path: advance glide at block level (processSample is not called)
    if (glideSamplesLeft > 0)
    {
        int steps = std::min(glideSamplesLeft, numSamples);
        transposeRatio += glideRatioIncr * steps;
        glideSamplesLeft -= steps;
        if (glideSamplesLeft <= 0)
        {
            transposeRatio = glideTargetRatio;
            glideSamplesLeft = 0;
        }
        // Recalculate after glide advancement
        effectiveRatio = transposeRatio * static_cast<double>(pitchModFactor);
        semitones = static_cast<float>(12.0 * std::log2(std::max(effectiveRatio, 1e-6)));
    }

    if (!stretcherPrepared)
        prepareStretcher();

    // Set transposition BEFORE priming — primeStretcher() runs process()
    // internally, which must use the new note's pitch, not the previous one's.
    stretcher.setTransposeSemitones(semitones);

    if (stretcherNeedsPriming)
    {
        primeStretcher();
        stretcherNeedsPriming = false;
    }

    // Read raw samples at original speed (SR-corrected only)
    readRawSamples(rawReadBuf.data(), numSamples);

    // Pitch-shift through Signalsmith Stretch (mono: 1 channel). When a held-note
    // crossfade is in flight, readRawSamples already blended the fade-from buffer
    // into rawReadBuf, so the stretcher pitch-shifts the crossfaded source.
    float* inPtr = rawReadBuf.data();
    stretcher.process(&inPtr, numSamples, &output, numSamples);
}

int SamplePlayer::estimateReferenceLengthSamples() const
{
    const auto& buf = currentPlaybackBuffer();
    const int bufLen = buf.getNumSamples();
    if (bufLen <= 0 || !audioLoaded)
        return 0;

    const double srRatio = currentBufferOriginalSR() / playbackSampleRate;
    if (srRatio <= 0.0)
        return 0;

    const float effectiveP1 = juce::jlimit(0.0f, 1.0f, startPosFrac + startPosOffset_);
    int startSample = static_cast<int>(std::floor(effectiveP1 * static_cast<float>(bufLen)));
    startSample = juce::jlimit(0, bufLen - 1, startSample);

    const bool reversed = (startSample >= playEnd);   // same rule as retrigger
    const int loopLen = juce::jmax(1, playEnd - playStart);
    int sourceSamples = 0;

    if (!reversed)
        sourceSamples = juce::jmax(0, playEnd - startSample);
    else
        sourceSamples = juce::jmax(0, startSample - playStart);

    if (loopMode == LoopMode::Loop)
        sourceSamples += loopLen;
    else if (loopMode == LoopMode::PingPong)
        sourceSamples += loopLen * 2;

    return juce::jmax(1, static_cast<int>(std::ceil(static_cast<double>(sourceSamples) / srRatio)));
}

juce::String SamplePlayer::debugStateString() const
{
    const auto& buf = currentPlaybackBuffer();

    return "shared=" + juce::String(sharedMode ? 1 : 0)
        + " playing=" + juce::String(playing ? 1 : 0)
        + " loaded=" + juce::String(audioLoaded ? 1 : 0)
        + " read=" + juce::String(readPosition, 2)
        + " playStart=" + juce::String(playStart)
        + " coldStart=" + juce::String(coldStart)
        + " playEnd=" + juce::String(playEnd)
        + " len=" + juce::String(buf.getNumSamples())
        + " p1=" + juce::String(startPosFrac, 4)
        + " p2=" + juce::String(loopStartFrac, 4)
        + " p3=" + juce::String(loopEndFrac, 4)
        + " p1Off=" + juce::String(startPosOffset_, 4)
        + " dir=" + juce::String(playDirection_)
        + " firstPass=" + juce::String(inFirstPass_ ? 1 : 0)
        + " mode=" + juce::String(loopModeName(loopMode))
        + " sourceGain=" + juce::String(sourceGain_, 4)
        + " needsReprepare=" + juce::String(needsReprepareFlag ? 1 : 0);
}

float SamplePlayer::estimatePlaybackRms(const float* gains, int numSamples, float* outPeak) const
{
    const auto& buf = currentPlaybackBuffer();
    const int bufLen = buf.getNumSamples();
    if (gains == nullptr || numSamples <= 0 || bufLen <= 0 || !audioLoaded)
    {
        if (outPeak != nullptr) *outPeak = 0.0f;
        return 0.0f;
    }

    const double srRatio = currentBufferOriginalSR() / playbackSampleRate;
    const float effectiveP1 = juce::jlimit(0.0f, 1.0f, startPosFrac + startPosOffset_);

    bool inFirstPass = true;
    double analysisReadPosition = static_cast<double>(
        std::floor(effectiveP1 * static_cast<float>(bufLen)));
    int playDirection = (analysisReadPosition >= static_cast<double>(playEnd)) ? -1 : 1;

    double sumSq = 0.0;
    int renderedSamples = 0;
    float peak = 0.0f;

    for (int i = 0; i < numSamples; ++i)
    {
        const bool useFirstPassBuffer = inFirstPass && loopMode == LoopMode::Loop
                                        && ((playDirection > 0) == (loopFadeSide_ > 0));
        float sample = playbackSample(analysisReadPosition, useFirstPassBuffer);
        float weighted = sample * gains[i];
        sumSq += static_cast<double>(weighted) * static_cast<double>(weighted);
        peak = std::max(peak, std::abs(weighted));
        ++renderedSamples;

        analysisReadPosition += srRatio * static_cast<double>(playDirection);

        const double pEnd = static_cast<double>(playEnd);
        const double pStart = static_cast<double>(playStart);

        if (inFirstPass)
        {
            if (playDirection > 0 && analysisReadPosition >= pEnd)
            {
                inFirstPass = false;
                if (loopMode == LoopMode::OneShot)
                    break;

                const double overshoot = analysisReadPosition - pEnd;
                if (loopMode == LoopMode::PingPong)
                {
                    analysisReadPosition = pEnd - overshoot;
                    playDirection = -1;
                }
                else
                {
                    analysisReadPosition = pStart + overshoot;
                }
            }
            else if (playDirection < 0 && analysisReadPosition < pStart)
            {
                inFirstPass = false;
                if (loopMode == LoopMode::OneShot)
                    break;

                const double overshoot = pStart - analysisReadPosition;
                if (loopMode == LoopMode::PingPong)
                {
                    analysisReadPosition = pStart + overshoot;
                    playDirection = 1;
                }
                else
                {
                    analysisReadPosition = pEnd - overshoot;
                }
            }
        }
        else
        {
            if (analysisReadPosition >= pEnd)
            {
                const double overshoot = analysisReadPosition - pEnd;
                if (loopMode == LoopMode::PingPong)
                {
                    analysisReadPosition = pEnd - overshoot;
                    playDirection = -1;
                }
                else
                {
                    analysisReadPosition = pStart + overshoot;
                }
            }
            else if (analysisReadPosition < pStart)
            {
                const double overshoot = pStart - analysisReadPosition;
                if (loopMode == LoopMode::PingPong)
                {
                    analysisReadPosition = pStart + overshoot;
                    playDirection = 1;
                }
                else
                {
                    analysisReadPosition = pEnd - overshoot;
                }
            }
        }
    }

    if (outPeak != nullptr)
        *outPeak = peak;

    if (renderedSamples <= 0)
        return 0.0f;

    return static_cast<float>(std::sqrt(sumSq / static_cast<double>(renderedSamples)));
}

// ═══════════════════════════════════════════════════════════════════
// Pitch shifter configuration
// ═══════════════════════════════════════════════════════════════════

void SamplePlayer::prepareStretcher()
{
    float sr = static_cast<float>(playbackSampleRate);
    switch (pitchQuality)
    {
        case PitchShiftQuality::Bypass:
            break;
        case PitchShiftQuality::Efficient:
            stretcher.presetCheaper(1, sr);
            break;
        case PitchShiftQuality::Default:
            stretcher.presetCheaper(1, sr); // cheap enough for polyphonic real-time
            break;
        case PitchShiftQuality::HighQuality:
            stretcher.presetDefault(1, sr);
            break;
        default:
            stretcher.presetCheaper(1, sr);
            break;
    }
    stretcherPrepared = (pitchQuality != PitchShiftQuality::Bypass);

    // Size the prime-time scratch buffers off the audio thread (prepareStretcher
    // runs from prepare()/setPitchShiftQuality, never from renderPitchedBlock).
    // primeStretcher() now uses these in place of per-call std::vector allocs.
    if (stretcherPrepared)
    {
        const auto seekCap   = static_cast<size_t>(
            std::max(0, stretcher.blockSamples() + stretcher.intervalSamples()));
        const auto primeCap  = static_cast<size_t>(std::max(0, stretcher.inputLatency()));
        if (primeSeekBuf.size()    < seekCap)  primeSeekBuf.resize(seekCap);
        if (primeInputBuf.size()   < primeCap) primeInputBuf.resize(primeCap);
        if (primeDiscardBuf.size() < primeCap) primeDiscardBuf.resize(primeCap);
    }
}

void SamplePlayer::primeStretcher()
{
    const double srRatio = currentBufferOriginalSR() / playbackSampleRate;

    // ── Step 1: seek() — fill STFT analysis context with pre-roll audio ──
    // Reads audio BEFORE readPosition so the first STFT frames
    // have real left context instead of post-reset silence.
    //
    // When the start point sits at (or near) the very beginning of the buffer
    // there IS no audio before it. This used to clamp seekLen down to whatever
    // fit — to zero at P1 = 0 — so the STFT started from empty history and the
    // synthesis-window overlap faded the first ~windowSize of output in from
    // silence: measured 46 ms to 10 % of level on every transposed note (the
    // root note stays on the direct read and never enters the stretcher), and a
    // fresh generation lands in exactly this state because loadGeneratedAudio
    // trims leading silence and auto-positions P1 at the first active window.
    // Standard edge handling instead: REFLECT the buffer around the start
    // point (read -pos for pos < 0), so the analysis windows are always FULL
    // of real local material — time-reversed, which only matters to the prime
    // as spectral content — and the full pre-roll is always paid.
    // cubicSampleFrom bounds-clamps, so a buffer shorter than the mirrored
    // reach degrades to an edge-hold, never an OOB. Measured (96 kHz,
    // tools/measure_note_latency): transposed note at P1 = 0 was 46 ms to
    // 10 % of level before, ~1 ms after — same as with real pre-roll.
    const int seekLen = stretcher.blockSamples() + stretcher.intervalSamples();
    if (seekLen > 0)
    {
        // Capacity is sized in prepareStretcher() from the same stretcher
        // instance; mismatch means prepare() never ran. Assert in debug, bail
        // in release rather than alloc on the audio thread.
        jassert (static_cast<size_t>(seekLen) <= primeSeekBuf.size());
        if (static_cast<size_t>(seekLen) <= primeSeekBuf.size())
        {
            const bool useFP = firstPassReadsPristine();
            double pos = readPosition - seekLen * srRatio;
            for (int i = 0; i < seekLen; ++i)
            {
                // sourceGain_ matches what processSample()/readRawSamples()
                // apply to the real audio that follows this pre-roll — without
                // it the analysis history sits at a different level than the
                // input, and with Normalize active the first ~windowSize/2 of
                // output rides that mismatch (adversarial-review finding).
                primeSeekBuf[static_cast<size_t>(i)] =
                    playbackSample(pos < 0.0 ? -pos : pos, useFP) * sourceGain_;
                pos += srRatio;
            }
            float* seekPtr = primeSeekBuf.data();
            stretcher.seek(&seekPtr, seekLen, 1.0);
        }
    }

    // ── Step 2: process()+discard — pay STFT output latency ──
    // After seek, the first inputLatency() output samples correspond to
    // pre-roll audio, not the retrigger point. Discard them so the next real output
    // starts at approximately readPosition. readPosition advances intentionally.
    int primeSamples = stretcher.inputLatency();
    if (primeSamples <= 0) return;
    jassert (static_cast<size_t>(primeSamples) <= primeInputBuf.size()
             && static_cast<size_t>(primeSamples) <= primeDiscardBuf.size());
    if (static_cast<size_t>(primeSamples) > primeInputBuf.size()
        || static_cast<size_t>(primeSamples) > primeDiscardBuf.size())
        return; // safety: bail rather than allocate on the audio thread

    readRawSamples(primeInputBuf.data(), primeSamples);

    float* inPtr = primeInputBuf.data();
    float* outPtr = primeDiscardBuf.data();
    stretcher.process(&inPtr, primeSamples, &outPtr, primeSamples);
}

void SamplePlayer::setPitchShiftQuality(PitchShiftQuality quality)
{
    if (quality != pitchQuality)
    {
        pitchQuality = quality;
        prepareStretcher();
    }
}

void SamplePlayer::processBlock(juce::AudioBuffer<float>& output)
{
    if (!audioLoaded || !playing || currentPlaybackBuffer().getNumSamples() == 0)
        return;

    const int numOutChannels = output.getNumChannels();
    const int numOutSamples  = output.getNumSamples();

    for (int i = 0; i < numOutSamples; ++i)
    {
        float sample = processSample();
        if (!playing) break; // one-shot ended

        for (int ch = 0; ch < numOutChannels; ++ch)
            output.addSample(ch, i, sample);
    }
}

// ═══════════════════════════════════════════════════════════════════
// Cross-correlation loop optimizer (from useSamplePlayer.ts)
// ═══════════════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════════════
// Loop crossfade, baked into the tail
// ═══════════════════════════════════════════════════════════════════
//
// The classical crossfade loop: the last `fade` samples of the loop become a
// fade from themselves into the `fade` samples that PRECEDE the loop start.
// Arriving at loopEnd the signal is then x[loopStart-1] as the original had it,
// and the wrap to x[loopStart] is the original's own continuity. The two mixed
// regions are one loop length apart, so for a periodic sound an integer-period
// loop makes them coherent — which is what the seam search measured as
// `correlation`, and what chooses the gain law here: coherent regions add
// linearly (an equal-power fade would swell +3 dB mid-fade), incoherent ones
// add in power (a linear fade would dip). The previous scheme faded the tail
// INTO the head (regions fade-length apart, coherent only when the fade itself
// was a period multiple) and shortened the loop by the fade; this does neither.

void SamplePlayer::applyLoopCrossfade(juce::AudioBuffer<float>& buf, int loopStart, int loopEnd,
                                      int fadeSamples, int fadeSide, float correlation)
{
    const int half = (loopEnd - loopStart) / 2;
    const int fade = fadeSide < 0 ? std::min(fadeSamples, std::min(loopStart, half))
                                  : std::min(fadeSamples, std::min(buf.getNumSamples() - loopEnd, half));
    if (fade < 2) return;

    for (int ch = 0; ch < buf.getNumChannels(); ++ch)
    {
        float* d = buf.getWritePointer(ch);
        for (int i = 0; i < fade; ++i)
        {
            const float t = static_cast<float>(i) / static_cast<float>(fade);
            float gIn, gOut;
            LoopFinder::crossfadeGains(t, correlation, gIn, gOut);
            if (fadeSide < 0)
            {
                // Tail fades out, the material before the start fades in: the
                // last tail sample is (nearly) x[start-1], the wrap lands on x[start].
                const int tailIdx = loopEnd - fade + i;
                const int preIdx  = loopStart - fade + i;
                d[tailIdx] = d[tailIdx] * gOut + d[preIdx] * gIn;
            }
            else
            {
                // Head fades in, the material after the end fades out: the head
                // begins (nearly) at x[end], where the unfaded tail leaves off.
                const int headIdx = loopStart + i;
                const int postIdx = loopEnd + i;
                d[headIdx] = d[headIdx] * gIn + d[postIdx] * gOut;
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════
// Signal-aware linear normalization:
//   - near-silence: bypass
//   - already-hot material: ceiling cap only
//   - sparse/transient material: percentile-based upward normalize
//   - sustained material: active-RMS normalize with peak ceiling
// The gain is applied only to the measured playback region.
// ═══════════════════════════════════════════════════════════════════

const char* SamplePlayer::normalizeModeName(NormalizeMode mode)
{
    switch (mode)
    {
        case NormalizeMode::Bypass:    return "Bypass";
        case NormalizeMode::PeakCap:   return "PeakCap";
        case NormalizeMode::Transient: return "Transient";
        case NormalizeMode::Sustained: return "Sustained";
    }
    return "?";
}

SamplePlayer::NormalizeAnalysis SamplePlayer::analyzeNormalizeRegion(const juce::AudioBuffer<float>& buf,
                                                                    int regionStart,
                                                                    int regionEnd,
                                                                    double bufferSampleRate) const
{
    NormalizeAnalysis analysis;

    const int rs = juce::jlimit(0, buf.getNumSamples(), regionStart);
    const int re = juce::jlimit(rs, buf.getNumSamples(), regionEnd);
    const int numChannels = buf.getNumChannels();
    if (re <= rs || numChannels <= 0 || bufferSampleRate <= 0.0)
        return analysis;

    const int regionFrames = re - rs;
    analysis.durationSeconds = static_cast<float>(regionFrames / bufferSampleRate);

    std::vector<float> framePeaks;
    framePeaks.reserve(static_cast<size_t>(regionFrames));

    const int blockFrames = juce::jmax(1, static_cast<int>(std::round(bufferSampleRate * 0.050)));
    const float activeThreshold = dbToGain(kActiveBlockThresholdDb);

    double totalSq = 0.0;
    int totalSamples = 0;
    double activeSq = 0.0;
    int activeSamples = 0;
    int activeBlocks = 0;
    int totalBlocks = 0;
    double blockSq = 0.0;
    int framesInBlock = 0;

    for (int i = rs; i < re; ++i)
    {
        float framePeak = 0.0f;
        double frameSq = 0.0;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float sample = buf.getReadPointer(ch)[i];
            const float absSample = std::abs(sample);
            framePeak = std::max(framePeak, absSample);
            frameSq += static_cast<double>(sample) * static_cast<double>(sample);
        }

        framePeaks.push_back(framePeak);
        analysis.peak = std::max(analysis.peak, framePeak);
        totalSq += frameSq;
        totalSamples += numChannels;
        blockSq += frameSq;
        ++framesInBlock;

        if (framesInBlock == blockFrames || i == re - 1)
        {
            ++totalBlocks;
            const float blockRms = static_cast<float>(std::sqrt(
                blockSq / static_cast<double>(framesInBlock * numChannels)));
            if (blockRms > activeThreshold)
            {
                activeSq += blockSq;
                activeSamples += framesInBlock * numChannels;
                ++activeBlocks;
            }
            blockSq = 0.0;
            framesInBlock = 0;
        }
    }

    if (totalSamples <= 0 || framePeaks.empty())
        return analysis;

    analysis.rms = static_cast<float>(std::sqrt(totalSq / static_cast<double>(totalSamples)));
    analysis.activeRms = activeSamples > 0
        ? static_cast<float>(std::sqrt(activeSq / static_cast<double>(activeSamples)))
        : analysis.rms;
    analysis.activeRatio = totalBlocks > 0
        ? static_cast<float>(activeBlocks) / static_cast<float>(totalBlocks)
        : 0.0f;
    analysis.crestDb = gainToDb(analysis.peak / std::max(analysis.rms, 1.0e-9f));

    auto percentilePeaks = framePeaks;
    const size_t n = percentilePeaks.size();
    const size_t percentileIndex = juce::jlimit<size_t>(
        0u,
        n - 1,
        static_cast<size_t>(std::floor(0.999 * static_cast<double>(n - 1))));
    std::nth_element(percentilePeaks.begin(),
                     percentilePeaks.begin() + static_cast<std::ptrdiff_t>(percentileIndex),
                     percentilePeaks.end());
    analysis.percentilePeak = percentilePeaks[percentileIndex];
    analysis.peakToPercentileDb = gainToDb(
        analysis.peak / std::max(analysis.percentilePeak, 1.0e-9f));

    return analysis;
}

SamplePlayer::NormalizeMode SamplePlayer::chooseNormalizeMode(const NormalizeAnalysis& analysis) const
{
    if (analysis.peak <= 0.0f)
        return NormalizeMode::Bypass;

    const float peakDb = gainToDb(analysis.peak);
    const float activeDb = gainToDb(std::max(analysis.activeRms, analysis.rms));
    const float headroomDb = -peakDb;

    if (peakDb <= kNearSilentPeakDb && activeDb <= kNearSilentActiveDb)
        return NormalizeMode::Bypass;

    if (headroomDb < kHotHeadroomDb)
        return NormalizeMode::PeakCap;

    if (analysis.durationSeconds < kShortTransientSeconds
        || analysis.activeRatio < kTransientActiveRatio
        || analysis.crestDb > kTransientCrestDb
        || analysis.peakToPercentileDb > kTransientPeakGapDb)
    {
        return NormalizeMode::Transient;
    }

    return NormalizeMode::Sustained;
}

float SamplePlayer::chooseNormalizeGain(const NormalizeAnalysis& analysis, NormalizeMode mode) const
{
    if (analysis.peak <= 0.0f)
        return 1.0f;

    const float ceilingGain = dbToGain(kNormalizeCeilingDb) / analysis.peak;

    switch (mode)
    {
        case NormalizeMode::Bypass:
            return 1.0f;

        case NormalizeMode::PeakCap:
            return std::min(1.0f, ceilingGain);

        case NormalizeMode::Transient:
        {
            if (analysis.percentilePeak <= 0.0f)
                return std::min(1.0f, ceilingGain);

            const float transientGain = dbToGain(kTransientPercentileTargetDb) / analysis.percentilePeak;
            return std::min(ceilingGain, std::max(1.0f, transientGain));
        }

        case NormalizeMode::Sustained:
        {
            const float reference = std::max(analysis.activeRms, analysis.rms);
            if (reference <= 0.0f)
                return std::min(1.0f, ceilingGain);

            const float sustainedGain = dbToGain(kSustainedTargetDb) / reference;
            return std::min(ceilingGain, sustainedGain);
        }
    }

    return 1.0f;
}

void SamplePlayer::normalizeBuffer(juce::AudioBuffer<float>& buf,
                                   int regionStart,
                                   int regionEnd,
                                   double bufferSampleRate) const
{
    const int rs = juce::jlimit(0, buf.getNumSamples(), regionStart);
    const int re = juce::jlimit(rs, buf.getNumSamples(), regionEnd);
    const int numChannels = buf.getNumChannels();
    if (re <= rs || numChannels <= 0)
        return;

    const NormalizeAnalysis analysis = analyzeNormalizeRegion(buf, rs, re, bufferSampleRate);
    const NormalizeMode mode = chooseNormalizeMode(analysis);
    const float gain = chooseNormalizeGain(analysis, mode);
    if (!std::isfinite(gain) || std::abs(gain - 1.0f) < 1.0e-4f)
        return;

    applyGainToRegion(buf, rs, re, gain);

    samplerDebugLog("normalizeBuffer mode=" + juce::String(normalizeModeName(mode))
                    + " gainDb=" + juce::String(gainToDb(gain), 2)
                    + " peakDb=" + juce::String(gainToDb(analysis.peak), 2)
                    + " p999Db=" + juce::String(gainToDb(analysis.percentilePeak), 2)
                    + " rmsDb=" + juce::String(gainToDb(analysis.rms), 2)
                    + " activeDb=" + juce::String(gainToDb(analysis.activeRms), 2)
                    + " crestDb=" + juce::String(analysis.crestDb, 2)
                    + " peakGapDb=" + juce::String(analysis.peakToPercentileDb, 2)
                    + " activeRatio=" + juce::String(analysis.activeRatio, 3)
                    + " dur=" + juce::String(analysis.durationSeconds, 3));
}

void SamplePlayer::applyGainToRegion(juce::AudioBuffer<float>& buf,
                                     int regionStart,
                                     int regionEnd,
                                     float gain) const
{
    if (!std::isfinite(gain) || std::abs(gain - 1.0f) < 1.0e-4f)
        return;

    const int rs = juce::jlimit(0, buf.getNumSamples(), regionStart);
    const int re = juce::jlimit(rs, buf.getNumSamples(), regionEnd);
    const int numChannels = buf.getNumChannels();
    if (re <= rs || numChannels <= 0)
        return;

    for (int ch = 0; ch < numChannels; ++ch)
    {
        float* data = buf.getWritePointer(ch);
        for (int i = rs; i < re; ++i)
            data[i] *= gain;
    }
}

// ═══════════════════════════════════════════════════════════════════
// Trim leading silence — removes near-zero head with no aesthetic value
// ═══════════════════════════════════════════════════════════════════

void SamplePlayer::trimLeadingSilence(juce::AudioBuffer<float>& buffer) const
{
    const int numSamples = buffer.getNumSamples();
    const int numCh      = buffer.getNumChannels();
    if (numSamples == 0 || numCh == 0) return;

    // Sustained windowed-RMS detection. A single-sample threshold ("first
    // non-zero") trips on isolated noise spikes and low-level DC drift —
    // common in VAE/decoder output — so the trim point would land in the
    // inaudible lead-in. We require ≥3 consecutive short RMS windows above
    // a conservative -50 dB absolute floor before we call it "audio".
    constexpr float threshold        = 0.00316f; // -50 dB absolute
    constexpr int   windowSamples    = 64;       // ~1.5 ms @ 44.1 kHz
    constexpr int   minSustainedWindows = 3;

    const int numWindows = numSamples / windowSamples;
    if (numWindows < minSustainedWindows) return;

    int firstActive = 0;
    int run = 0;
    bool found = false;
    for (int w = 0; w < numWindows; ++w)
    {
        const int base = w * windowSamples;
        double sumSq = 0.0;
        for (int ch = 0; ch < numCh; ++ch)
        {
            const float* p = buffer.getReadPointer(ch) + base;
            for (int i = 0; i < windowSamples; ++i)
            {
                double s = static_cast<double>(p[i]);
                sumSq += s * s;
            }
        }
        const float rms = std::sqrt(static_cast<float>(sumSq
                                    / static_cast<double>(windowSamples * numCh)));
        if (rms > threshold)
        {
            ++run;
            if (run >= minSustainedWindows)
            {
                firstActive = (w - minSustainedWindows + 1) * windowSamples;
                found = true;
                break;
            }
        }
        else
        {
            run = 0;
        }
    }

    if (!found || firstActive == 0) return; // nothing to trim

    // Truncate: copy from firstActive onward into a new buffer
    const int newLen = numSamples - firstActive;
    juce::AudioBuffer<float> trimmed(numCh, newLen);
    for (int ch = 0; ch < numCh; ++ch)
        trimmed.copyFrom(ch, 0, buffer, ch, firstActive, newLen);

    buffer = std::move(trimmed);
}

void SamplePlayer::trimTrailingSilence(juce::AudioBuffer<float>& buffer) const
{
    const int numSamples = buffer.getNumSamples();
    const int numCh      = buffer.getNumChannels();
    if (numSamples == 0 || numCh == 0) return;

    // Symmetric counterpart to trimLeadingSilence(): drop a sustained near-
    // silent TAIL so engines that traverse the whole buffer end at real content.
    // The granular/freeze engine maps scan 0..1 across the entire sample and has
    // no internal playhead, so a dead post-content field (the diffusion models
    // generate the requested duration even when the sound itself is short, e.g.
    // ~1 s of bell + ~10 s of near-silence at an 11 s request) parks the read
    // point in pure silence — and the waveform/playhead show a flat tail. Same
    // windowed-RMS / -50 dB floor / 3-window sustain test as the leading trim,
    // scanned for the LAST qualifying run. A ~46 ms margin past that run is kept
    // to preserve natural decay/release and keep the cut below -50 dB (no click).
    // Idempotent, and a no-op when content already runs to the end.
    constexpr float threshold           = 0.00316f; // -50 dB absolute (matches leading trim)
    constexpr int   windowSamples       = 64;       // ~1.5 ms @ 44.1 kHz
    constexpr int   minSustainedWindows = 3;
    constexpr int   marginWindows       = 32;        // ~46 ms tail kept past last active run

    const int numWindows = numSamples / windowSamples;
    if (numWindows < minSustainedWindows) return;

    int lastRunEnd = -1;   // exclusive window index of the end of the last sustained run
    int run = 0;
    for (int w = 0; w < numWindows; ++w)
    {
        const int base = w * windowSamples;
        double sumSq = 0.0;
        for (int ch = 0; ch < numCh; ++ch)
        {
            const float* p = buffer.getReadPointer(ch) + base;
            for (int i = 0; i < windowSamples; ++i)
            {
                double s = static_cast<double>(p[i]);
                sumSq += s * s;
            }
        }
        const float rms = std::sqrt(static_cast<float>(sumSq
                                    / static_cast<double>(windowSamples * numCh)));
        if (rms > threshold)
        {
            ++run;
            if (run >= minSustainedWindows)
                lastRunEnd = w + 1;
        }
        else
        {
            run = 0;
        }
    }

    if (lastRunEnd < 0) return; // no sustained content — leave a genuinely quiet buffer alone

    int newLen = (lastRunEnd + marginWindows) * windowSamples;
    newLen = juce::jmin(numSamples, newLen);
    if (newLen >= numSamples) return; // content runs to (near) the end — nothing to trim

    // Truncate in place; not on the audio thread (load path), so realloc is fine.
    buffer.setSize(numCh, newLen, /*keepExistingContent*/ true,
                   /*clearExtraSpace*/ false, /*avoidReallocating*/ true);
}
