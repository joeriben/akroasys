#!/usr/bin/env python3
"""Measure an LCO body — with a meter calibrated on signals whose answer is known.

Every number this file reports about a curated instrument was, at some point in
this project, first reported WRONG by a plausible-looking measurement:

  - an FFT-peak `f0` on a noise-excited comb measures the loudest PARTIAL, not
    the fundamental, and reports a confident, wrong pitch;
  - an autocorrelation `f0` octave-errs on a sparse harmonic series (`pluck` came
    back at -1200 and -2789 cents; there is no subharmonic there — f0/2 sits at
    -115 dB);
  - a render that CLIPPED yields plausible-but-wrong spectral numbers (-6032
    cents on a 440 Hz tone) with nothing in the output to say so.

So the measurement instrument is itself an instrument, and this one is calibrated
before it is trusted: `--selftest` renders signals whose f0, colour and comb
contrast are known in advance and asserts the meter returns them. Run it whenever
this file, `backend/lco_write.py`'s host scaffold, or the Csound version changes.
A number from an uncalibrated meter is not evidence.

The scaffold is DERIVED from `lco_write._HEAD`/`_TAIL` by substitution, and every
substitution is asserted to match exactly once — so a rename in the real host
fails this harness loudly instead of quietly measuring a different instrument.
What is measured is `asig`, the body's own output, before the host's `kgate *
kpresGain * HEADROOM` and its final `clip`.

    .venv/bin/python tools/lco_measure.py --selftest
    .venv/bin/python tools/lco_measure.py --key string [--freq 220] [--dur 4]
    .venv/bin/python tools/lco_measure.py --key string --anchor bow=bowed
    .venv/bin/python tools/lco_measure.py --key fm_bell --dump      # print the CSD
"""
import argparse
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
import soundfile as sf

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "backend"))

import lco_write as W  # noqa: E402  (needs the path above)

SR = 44100


# ─────────────────────────────────────────────────────────────────────────────
# What may be quoted from this file, and what may not
# ─────────────────────────────────────────────────────────────────────────────
# This is printed by every tool that reports from here, because it was NOT and the
# numbers travelled without it. The concrete damage: a count of "18 axes are really
# volume controls" was reported as a finding when 17 of the 18 read "no anchor_code
# — cannot be measured". The silence of the meter was counted as a result. Exactly
# one of the eighteen had been measured.
#
# BJ, 2026-07-27, on the whole class of tool in this directory: these hearing models
# are self-built, outside any research contribution to the subject; on the basis of
# this work an IRCAM or any sound-research chair would have none of it. A measuring
# device is itself an instrument and falls under the rule that already governs every
# sounding body here — a named method and a source, written down before the first
# line. That rule was never applied to the measuring side.

LIMITS = """\
limits of this measurement — what it may be quoted for
  reads a signal, and can be quoted as such: f0, cents, partial amplitudes,
    odd/even ratio, peak, RMS, crest.
  invented here, no external referent: every threshold in this file and in
    lco_axis_probe, and the derived meters centroid_motion_hz, motion_coherence,
    comb_db, event_rate_hz, loudness_travel_db, beat_depth. The beat meters and
    comb_db can be satisfied by amplitude alone, so they do not measure the thing
    their names say.
  not loudness: every dB here is peak or RMS amplitude. Perceived loudness has
    standards -- ITU-R BS.1770 / EBU R128, ISO 532-1/-2 -- and none is used here.
    Two entries compared in dB have not been compared for how loud they sound.
  rate: SR is 44100. The plugin compiles Csound at 176400 by default, so a reading
    taken without changing SR was taken at a rate the instrument never runs at.
  the self-tests check that the code computes what it claims to compute. They do
    not check that the computed thing means anything."""


# ─────────────────────────────────────────────────────────────────────────────
# The scaffold: the real host, with the plugin's per-voice channels stood in for
# ─────────────────────────────────────────────────────────────────────────────

def _sub(text, old, new, what):
    n = text.count(old)
    if n != 1:
        raise SystemExit(
            f"lco_measure is out of step with backend/lco_write.py: expected "
            f"exactly one {what} line ({old.strip()!r}), found {n}. The harness "
            f"would silently measure a different scaffold — fix the substitution.")
    return text.replace(old, new)


MIN_PREROLL = 2.0 * W.KSMPS / SR      # two k-periods: one to be low in, one to rise


def preroll_edge(preroll):
    """The sample at which the note's trigger edge lands for this preroll.

    Not `int(preroll * SR)`. Csound's `timeinsts()` returns n * ksmps / sr on k-cycle
    n, so the comparison first holds on cycle ceil(preroll * kr), and the audio of
    that cycle starts at (ceil(preroll * kr) - 1) * ksmps.
    """
    import math
    cycles = math.ceil(preroll * SR / W.KSMPS)
    return max(0, (cycles - 1) * W.KSMPS)


def scaffold(body, dur=4.0, freq=220.0, glide=None, preroll=0.0):
    """The authored body inside the REAL host, driven offline.

    glide=(f0, f1) sweeps the note's pitch exponentially over its whole length,
    which is how a body's behaviour across the keyboard gets measured in one
    render instead of twenty.

    preroll runs the instrument for that many seconds BEFORE the note, and the
    audio is trimmed back to the note. It matters more than it looks, because at
    preroll=0 this harness does not reproduce the host:

      the host scores `i 1 0 2000000000 <voice>` — sixteen instances, always on since
      the plugin loaded, notes signalled through `ktrig`/`kgate`
      this harness scores `i 1 0 <dur> 1` — the instance BEGINS at the note

    So at preroll=0 every piece of state that is not note-relative starts fresh at
    note-on, which in the plugin it never does. Two classes of body are measured
    wrongly, and both were shipped that way:

    - `balance`. Its RMS denominator starts at zero, so a body whose source builds
      from silence gets an enormous gain for the first milliseconds. `string` reads
      a true peak of 3.18 at 4.4 ms here and 0.19 with `balance` replaced by a fixed
      gain; in the plugin the estimator has been settled for as long as the plugin
      has been open. Two entries were reported as clipping on that artefact.
    - a free-running LFO. `tanpura` claims to be the one entry in the library whose
      spectrum climbs after the attack, via `khal poscil 0.5, 0.27`. At preroll=0 the
      3.70 s cycle starts at phase 0 exactly at note-on and the climb always appears.
      In the plugin the phase at note-on is the plugin's uptime mod 3.70 s: at a
      quarter turn the same note DARKENS. A body meant to behave a certain way over
      its note must key that behaviour to `knote`, which the host does reset.

    preroll stays 0.0 by default so that the recorded calibration keeps meaning what
    it says. `lco_axis_probe.gate` sweeps it on any body that contains a free-running
    k-rate oscillator, which is where the difference decides anything.

    A SECOND difference from the plugin is open and is not addressed here: the RATE.
    Since `5e65c0d4` the engine compiles Csound at `sampleRate x factor` (1/2/4, a
    Settings control since `667d8bea`) and decimates each voice back with a 63-tap
    halfband FIR, so `%SR%` in the plugin resolves to the oversampled rate. This
    harness substitutes `sr = 44100` unconditionally. Two consequences, both real:

    - a body that derives its own bandwidth from `sr` — `tanpura`'s `kmax = int(sr *
      0.45 / kf0)`, `fm_ep`'s tine-ratio cap — gets a different Nyquist here than in
      the plugin, so its partial count is measured at the most conservative setting
      the player can choose;
    - ALIASING is invisible to every meter in this file, because folding happens at
      the rate the generators are computed at and that rate is not this one.

    Nothing here is calibrated for that, and it must not be faked by resampling in
    Python: the plugin's decimator is a specific filter, and a second, different one
    would produce numbers that look like the plugin's and are not. The measurement
    that exists for it is `tools/audition_lro_oversampling.cpp`, on the real engine.
    """
    # The twelve knob channels FIRST, through lco_write's own substitution: the
    # head carries `%LRO1A%` placeholders and is not parseable Csound until they
    # are gone. Every knob the body declares starts where the body puts it — the
    # same `read_controls` the plugin uses decides that, so a measurement here
    # and a note in the plugin play the same instrument.
    head = W.bake_knob_defaults(W._HEAD, W.read_controls(body))
    head = _sub(head, "<CsOptions>\n-n -d\n</CsOptions>",
                "<CsOptions>\n-d\n</CsOptions>", "CsOptions")
    head = _sub(head, "sr = %SR%", f"sr = {SR}", "sample rate")
    head = _sub(head, f"nchnls = {W.NCHNLS}", "nchnls = 1", "channel count")
    # With a preroll the note is an EDGE, exactly as in the host: gate and trig go
    # high at `preroll`, `changed2(ktrig)` fires there, and `knote` resets there. The
    # glide has to wait too, or it would sweep during the preroll — a flat first
    # segment does that, and `expseg` interpolates a constant segment as a constant.
    if 0 < preroll < MIN_PREROLL:
        # Below two k-periods `ktrig` is already 1 on the first cycle, `changed2` has
        # no edge to fire on, and `knote` silently becomes the instance's uptime —
        # which is the very state the preroll exists to avoid. Refuse rather than
        # return a buffer that looks trimmed and is not keyed to a note.
        raise ValueError(
            f"preroll {preroll} is shorter than two k-periods ({MIN_PREROLL:.6f} s at "
            f"ksmps {W.KSMPS}); there would be no trigger edge and `knote` would read "
            f"the instance's uptime. Use 0 for no preroll, or at least {MIN_PREROLL:.6f}.")
    if preroll > 0:
        gate_expr = f"  kgateraw = (timeinsts() >= {preroll} ? 1 : 0)\n"
        trig_expr = f"  ktrig    = (timeinsts() >= {preroll} ? 1 : 0)\n"
        kfreq = (f"expseg {glide[0]}, {preroll}, {glide[0]}, {dur}, {glide[1]}"
                 if glide else f"= {freq}")
    else:
        gate_expr = "  kgateraw = 1\n"
        trig_expr = "  ktrig    = 1\n"
        kfreq = (f"expon {glide[0]}, {dur}, {glide[1]}" if glide else f"= {freq}")
    for old, new, what in (
        ("  kgateraw chnget Sgate\n", gate_expr, "gate"),
        ("  kfreqraw chnget Sfreq\n", f"  kfreqraw {kfreq}\n", "frequency"),
        ("  kvel     chnget Svel\n", "  kvel     = 1\n", "velocity"),
        ("  kpres    chnget Spres\n", "  kpres    = 0\n", "pressure"),
        ("  ktimb    chnget Stimb\n", "  ktimb    = 0\n", "timbre"),
        ("  ktrig    chnget Strig\n", trig_expr, "trigger"),
    ):
        head = _sub(head, old, new, what)

    # asig, not aout: the body's own output is what the library curates. The host
    # then multiplies by HEADROOM and clips at 0.95, so a body has to peak above
    # ~1/HEADROOM before that safety net starts shaping it.
    tail = _sub(W._TAIL, "  outch    ivoice, aout\n", "  out      asig\n", "output")

    indented = "\n".join(("  " + l) if not l.startswith(" ") else l
                         for l in body.splitlines())
    total = preroll + dur
    return (head + indented + tail
            + f"i 1 0 {total} 1\ne {total}\n</CsScore>\n</CsoundSynthesizer>\n")


def render(body, dur=4.0, freq=220.0, glide=None, keep=None, preroll=0.0):
    """Render a body. Returns (audio, err); err is None only on a clean render.

    A silent or non-finite render is an ERROR, not a result: every spectral number
    below is meaningless on one, and the failure mode this guards is a confident
    wrong number, not a crash. (The render is float, so a loud body is preserved
    exactly rather than distorted — but one loud enough that the HOST's safety
    clip would act on it is reported too, since that clip is audible and the body
    is then not what was measured.)

    preroll settles the instrument before the note and is trimmed off the returned
    audio, so the caller always gets exactly the note. See `scaffold` for what it
    is for and why 0.0 is not what the plugin does.

    `keep` is a directory to leave the `.csd` and the `.wav` in, for looking at
    afterwards (`--wav`). Without it they are scratch and are removed on every
    path out of here, including the error returns. They used not to be: every
    single call leaked a directory holding a FLOAT wav — 10.6 MiB for a 60 s
    render — and 267 900 of them, 251 GiB, had collected in the OS temp directory
    and filled the machine's disk to 100 %, at which point nothing on it could
    write a file at all. A measurement tool that is run in six-register grids
    cannot leave its own output behind.
    """
    d = Path(keep or tempfile.mkdtemp())
    try:
        return _render_into(d, body, dur, freq, glide, preroll)
    finally:
        if keep is None:
            shutil.rmtree(d, ignore_errors=True)


def _render_into(d, body, dur, freq, glide, preroll):
    csd = scaffold(body, dur, freq, glide, preroll)
    d.mkdir(parents=True, exist_ok=True)
    (d / "x.csd").write_text(csd)
    # THE OPCODE DIRECTORY THE APP SHIPS, not whatever the machine's own Csound
    # reaches for. Without it this renders against the build machine's install —
    # a different vocabulary from the one that plays, and on a machine with no
    # system Csound at all it does not render. `_csound_child_env` sets
    # OPCODE6DIR64 for the CHILD only, from the same vendored payload
    # CsoundEngine.cpp points the engine at. BJ 2026-08-05: "wir arbeiten nicht
    # mit der homebrew-installation". Only for a `csound` that runs as that
    # payload's architecture: an x86_64 one (Rosetta Homebrew on Apple Silicon)
    # cannot load its modules, gets none, and renders with its own install's.
    r = subprocess.run(["csound", "-W", "--format=float", "-o", str(d / "o.wav"),
                        "-m0", "-d", str(d / "x.csd")],
                       capture_output=True, text=True, timeout=120,
                       env=W._csound_child_env("csound"))
    err = re.sub(r"\x1b\[[0-9;]*m", "", r.stderr)
    if r.returncode != 0:
        bad = " | ".join(l for l in err.splitlines() if "error" in l.lower())
        return None, "csound: " + (bad[:200] or err.strip()[-200:])
    y, _ = sf.read(d / "o.wav")
    if y.ndim > 1:
        y = y[:, 0]
    if not np.isfinite(y).all():
        return None, "NaN/Inf"
    if preroll > 0:
        # Trim at the sample where the trigger EDGE actually landed, not at
        # int(preroll * SR). `timeinsts()` reads n * ksmps / sr on k-cycle n, so
        # `changed2(ktrig)` fires on cycle ceil(preroll * kr) and that cycle's audio
        # begins one k-period earlier. Trimming at the nominal time dropped 0.09 to
        # 0.95 ms of the note and left every buffer frame-misaligned, which made a
        # body keyed entirely to `knote` differ at full amplitude between prerolls —
        # so `render(preroll=...)` did not return the note it promised, and a
        # calibration case that compared raw samples could not fail.
        y = y[preroll_edge(preroll):]
        if len(y) < int(0.1 * SR):
            return None, "the preroll consumed the note"
    peak = float(np.abs(y).max())
    if peak < 1e-5:
        return None, "SILENT"
    # Which peak the host clip is judged on depends on whether a preroll was given —
    # see the block at ONSET_S. With one, the instrument was already running before the
    # note, so everything in this buffer is the note and the attack counts. Without
    # one, the buffer still holds the instrument's own start-up and the 50 ms exemption
    # stands.
    late = y[int(ONSET_S * SR):]
    peak_late = float(np.abs(late).max()) if len(late) else peak
    judged, where = ((peak, "over the note")
                     if preroll > 0 else
                     (peak_late, f"after the first {ONSET_S * 1000:.0f} ms"))
    if judged > HOST_TRANSPARENT:
        limited = " and HARD-LIMITED" if judged > HOST_CLIP else ""
        return y, (f"HOT (peak {judged:.2f} {where}; the host alters a body above "
                   f"{HOST_TRANSPARENT:.2f}{limited})")
    return y, None


# ─────────────────────────────────────────────────────────────────────────────
# The meters
# ─────────────────────────────────────────────────────────────────────────────

def _seg(y, t0, t1):
    return y[int(SR * t0):int(SR * min(t1, len(y) / SR))]


def rms_db(y, t0=0.5, t1=3.5):
    s = _seg(y, t0, t1)
    return float(20 * np.log10(max(np.sqrt((s ** 2).mean()), 1e-12)))


def peak_p999(y):
    """The 99.9th percentile, not the peak.

    A randomly-excited resonator (dust, rand) has a RANDOM peak: the same body
    measured 2.89 on one render and 0.79 on the next. Sizing a gain against the
    maximum of a random process fits the noise, not the instrument.

    THAT IS WHY IT MUST NOT BE THE HEADROOM CHECK, which is a different question with
    a different right answer. The host clips the true peak, so the only figure that
    says whether it clips is the true peak. On a SPARSE event texture the two diverge
    by a lot, because a percentile cannot see an event that occupies less than 0.1 % of
    the buffer: `ice` at four cracks a second read p99.9 1.98 and a true peak 4.03,
    with the host clipping above 2.97 — it passed the gate's headroom check and
    clipped. Measured across the library the ratio runs from 1.0x (`jaw_harp`, a
    continuous body) to 3.4x (`crackle`).

    So both are reported: this one for SIZING a body, `peak` for whether the host will
    clip it. See `lco_axis_probe.gate`, which counts how many renders of the sweep go
    over rather than trusting one draw of a random variable.
    """
    return float(np.percentile(np.abs(y), 99.9))


# What the host does to a body that goes over. `lco_write.wrap` scales `asig` by
# HEADROOM and the voice ends on `clip aout, 0, 0.95, 0.85`.
#
# That is Csound `clip` method 0, where the FOURTH argument is the fraction of
# `ilimit` at which limiting BEGINS — not the limit. `0.95 / HEADROOM = 2.97` read
# the third argument as the ceiling and was wrong by 18 %: the curve, measured by
# ramping 0..3 through that exact line, is transparent to 0.78, bends at 0.84 and
# goes STARK FLAT at 0.87875 — it never reaches 0.95 at any input.
#
#   input   0.78 -> 0.780000     (untouched)
#           0.84 -> 0.838393
#           0.90 -> 0.872578
#          >0.96 -> 0.878750     (and forever after)
#
# So in body units there are two ceilings, and neither is 2.97. The scaffold's own
# comment ("transparent below ~0.8, asymptotes ~0.88") said so all along.
HOST_TRANSPARENT = 0.85 * 0.95 / W.HEADROOM   # 2.523 — below this the host is a no-op
HOST_CLIP = 0.87875 / W.HEADROOM              # 2.746 — nothing above this gets out at all
# With pressure at maximum (`kpresGain = 1.0 + 0.15 * kpres`) both fall by 1.15.
HOST_PRES_GAIN_MAX = 1.15

# Judge headroom on the peak AFTER the onset, not on the whole buffer. Measured:
# `string`'s 3.18 at 69.3 Hz is ONE sample at t = 0.004 s against 0.86 for the rest of
# the note, and `ice` at crack 0.94 / glide 0.67 / 27.5 Hz peaks at 3.61 inside the
# first 100 ms against 1.18 after. Both were reported as clipping entries; neither is.
#
# `string`'s spike is `balance` STARTING UP, not the `streson` ring-up it was first
# blamed on — a high-Q filter rings up gradually and shows no spike at all. The AGC's
# RMS denominator begins at zero, so its first samples multiply by an enormous gain:
# the same body with a fixed gain in place of `balance` reads 0.62. Any body that
# normalises itself has this, and the plugin never hears it, because the plugin's
# instances have been running since load.
#
# The percentile was too lenient for a sustained over and the true peak is too strict,
# because it is dominated by the start-up of a body that is fine.
# 50 ms: long enough to clear a high-Q resonator's ring-up, short enough that a real
# sustained over cannot hide inside it.
ONSET_S = 0.050

# …and that exemption only applies when there is NO preroll. `preroll` was added after
# ONSET_S and it models the very thing ONSET_S argues from — "the plugin's instances
# have been running since load" — so with a preroll the start-up is already outside the
# returned buffer and whatever is left is the NOTE, attack included. Measured over
# prerolls 0.0 / 0.5 / 1.3, whole-note peak:
#   string @ 69.3 Hz   3.179 -> 0.777 / 0.772   the spike is start-up: gone with a preroll
#   glass  @ 220 Hz    1.266 -> 0.970 / 1.001   likewise
#   rhodes @ 220 Hz    0.596 -> 0.596 / 0.589   nothing there either way
#   tom    @ 220 Hz    6.059 -> 3.092 / 4.337   SURVIVES: the mallet, on every note
# So judging a prerolled render on `peak_late` hid a struck body clipping 3-4x over the
# host ceiling while reporting 0.70. The library was entirely standing tones until the
# first struck, decaying entry; `peak_late` cannot see an attack, and an attack keyed to
# `knote` recurs on every note-on because the host resets that clock.
#
# Note also that tom's figure MOVES with the preroll (3.09 vs 4.34): a burst exciter's
# `rand` realisation differs, so one prerolled render is not the worst case. A struck
# body has to be sized against several.


def _clip_transfer():
    """Measure the host's clip line by ramping through it. None if it cannot run.

    Not derived: the two ceilings above are only as good as this curve, and the way
    they were wrong the first time was by reading `clip`'s arguments instead of
    measuring what it does. `render` cannot be used — it outputs `asig`, upstream of
    the clip — so this builds the smallest orchestra that contains the real line.

    Its scratch directory is removed on every path out, for the reason given in
    `render`: this one leaked too, on every path that got as far as creating it.
    """
    line = [l for l in W._TAIL.splitlines() if " clip " in l]
    if len(line) != 1:
        return None
    args = line[0].split("clip", 1)[1].split(";")[0].strip()
    args = args.split(",", 1)[1].strip()           # drop the input variable
    csd = ("<CsoundSynthesizer>\n<CsOptions>\n-n -d\n</CsOptions>\n<CsInstruments>\n"
           f"sr = {SR}\nksmps = 1\nnchnls = 1\n0dbfs = 1\n"
           "instr 1\n  kin  line 0, p3, 3.0\n  ain  = a(kin)\n"
           f"  aout clip ain, {args}\n"
           "  kout downsamp aout\n"
           "  printks \"%.6f %.6f\\n\", 0.005, kin, kout\nendin\n"
           "</CsInstruments>\n<CsScore>\ni1 0 1\ne\n</CsScore>\n</CsoundSynthesizer>\n")
    d = Path(tempfile.mkdtemp())
    try:
        (d / "c.csd").write_text(csd)
        r = subprocess.run(["csound", "-n", "-d", str(d / "c.csd")],
                           capture_output=True, text=True, timeout=60,
                           env=W._csound_child_env("csound"))
    finally:
        shutil.rmtree(d, ignore_errors=True)
    rows = []
    for raw in (r.stdout + r.stderr).splitlines():
        parts = re.sub(r"\x1b\[[0-9;]*m", "", raw).split()
        if len(parts) == 2:
            try:
                rows.append((float(parts[0]), float(parts[1])))
            except ValueError:
                pass
    if len(rows) < 20:
        return None
    flat = [i for i, o in rows if abs(o - i) < 1e-4]
    return {"transparent_to": max(flat) if flat else 0.0,
            "ceiling": max(o for _i, o in rows)}


def sustain(y):
    """Last fifth's RMS over the first fifth's — does a held note stand?"""
    n = len(y)
    return float(np.sqrt((y[-n // 5:] ** 2).mean())
                 / max(np.sqrt((y[:n // 5] ** 2).mean()), 1e-12))


def loudness_travel(y, t0=1.0, win=0.05):
    """Loudness travelling INSIDE one held note, in dB, with the decay taken out.

    `rms_db` averages the whole note and `sustain` compares its ends, so between them
    they see a level that is wrong and a level that drifts — and neither sees a level
    that MOVES and comes back. `overtone_voice` at `press` 0 stepped -12.1 -> -15.4 ->
    recover -> -9.5 -> recover over a held note, 6.08 dB peak to peak, and the crossed
    gate passed it at a 0.33 dB spread because every corner's MEAN was steady. §4 of
    `LCO_CONCEPT.md` lets the colour travel over a note and not the loudness, so that
    was an invariant violation with no meter pointed at it.

    The envelope is read in dB and LINEARLY DETRENDED before the peak-to-peak is taken,
    which is what makes one number work for a standing tone and a struck one alike: an
    exponential decay is a straight line in dB, so detrending removes it exactly and
    leaves what the decay does not explain. A struck body therefore reads small here
    even though its `sustain` is 0.001.

    Calibrated on signals whose answer is known — a steady tone reads 0.02 dB, a clean
    exponential decay over 60 dB reads 0.16, and an asked 3.00 dB of 2 Hz tremolo reads
    3.00 — and on the library, where the honest floor is set by noise: a 50 ms window on
    white noise has about 0.13 dB of RMS scatter, so an unmodulated bed lands near 0.8
    and bodies whose SUBSTANCE is amplitude events (`rain`, `crackle`, `thunder`) land
    far above that and are not defects.

    The first second is skipped: an attack is not travel.

    THE SPAN ALONE DOES NOT SAY WHETHER THE BODY IS AT FAULT — read it with
    `loudness_is_the_body`. A narrow filter fed noise has a fluctuating envelope by
    construction, and the narrower it is the wilder that gets: with NOTHING whatever
    modulating it, `mode aex, 880, Q` measured 5.86 dB at Q 10, 14.92 at Q 200 and
    18.43 at Q 700, and 24.14 at Q 700 over twelve seconds instead of four. A
    `butterbp` of 1 Hz reads 20.11. A deliberate 3.00 dB tremolo on a wide band reads
    4.88 — LESS than any of them. So on every narrowband body in this library the span
    is dominated by Rayleigh statistics, it grows with the note's length, and a bare
    figure like "15 dB" is a statement about the filter's bandwidth and not about the
    design. Figures for `fm_bell`, `glass`, `struck_bar` and every mode bank were
    published from this before the split existed and meant nothing.

    THE DETREND IS A BLIND SPOT AND `loudness_drift_db` IS THE OTHER HALF. A straight
    line in dB is removed exactly, which is the point for a decay and a hole for
    everything else: a note that fades 6 dB or SWELLS 20 dB from beginning to end reads
    0.00 here, and §4 names exactly that ("a tone that fades to silence on its own is
    not"). Read the two together or the monotone class is invisible.

    Returns None, not 0.0, when it could not measure — a note too short for four windows
    or one with fewer than four windows above the floor. Those used to return 0.0, which
    is what a clean steady tone returns, so "declined to measure" and "measured no
    travel" were the same number: a 1.1 s note with 12 dB of tremolo in it read 0.00.
    """
    e = _loudness_track(y, t0, win)
    if e is None:
        return None
    t = np.arange(len(e))
    a, b = np.polyfit(t, e, 1)
    return float(np.ptp(e - (a * t + b)))


def loudness_drift_db(y, t0=1.0, win=0.05):
    """The monotone part — what `loudness_travel` detrends away — over the whole note, in dB.

    Signed: positive is a swell, negative a fade. This is the straight line's own total
    rise or fall across the window, so a body that gets steadily louder inside a held
    note reads its 20 dB here and 0.00 there. `sustain` sees the same class but as a
    ratio of the two ENDS, which a body that decays and then recovers can hide; this is
    the fit over every window.

    A decay is not a defect and reads large here on purpose — a struck note SHOULD fade.
    What the gate has to decide is whether a body is a struck one, and that is
    `sustain`'s job, not this one's.
    """
    e = _loudness_track(y, t0, win)
    if e is None or len(e) < 4:
        return None
    t = np.arange(len(e))
    a, _ = np.polyfit(t, e, 1)
    return float(a * (len(e) - 1))


def _loudness_track(y, t0=1.0, win=0.05):
    """The dB envelope `loudness_travel` and `loudness_coherence` both read, or None."""
    s = y[int(t0 * SR):]
    n = int(win * SR)
    if len(s) < 4 * n:
        return None
    e = np.array([np.sqrt((s[i:i + n] ** 2).mean()) for i in range(0, len(s) - n, n)])
    e = 20 * np.log10(np.maximum(e, 1e-12))
    # Windows more than 60 dB under the loudest are dropped, and that is not cosmetic:
    # a body that decays to true silence runs off to -240 dB, no straight line fits
    # that, and the residual is enormous — `rhodes` and `wurlitzer` read 212 and 218 dB
    # before this line, which is a meter reporting its own arithmetic.
    keep = e > e.max() - 60.0
    if keep.sum() < 4:
        return None
    return e[keep]


# `rand`'s seed is its SECOND argument — `ares rand xamp [, iseed] [, isel] [, ioffset]` —
# but `randi`'s and `randh`'s is their THIRD: `ares randi xamp, xcps [, iseed] [, isize]`.
# One pattern for all three rewrote the RATE of every `randi` in the library and called it
# a seed. Measured on `ice`, whose `kjit randi 0.30, 0.7, 0.5, 1` is its crack jitter: the
# old pattern produced `randi 0.30, 0.0100, 0.5, 1`, taking 0.7 Hz down to 0.01 Hz. So the
# "different noise realisation" was a differently-MOVING body, and `lco_phase_census` then
# read that difference as the exciter's dice and classified on it — `ice`'s published class
# turned on it, 1.5 % from flipping.
_RESEED_2ND = re.compile(r"(\brand\s+[^,;\n]+,\s*)(\d*\.?\d+)")
_RESEED_3RD = re.compile(r"(\brand[ih]\s+[^,;\n]+,\s*[^,;\n]+,\s*)(\d*\.?\d+)")
# Every opcode in Csound that draws a random number. A body containing none of these is
# fully deterministic, so whatever its level does, it does on purpose.
_STOCHASTIC = re.compile(
    r"\b(rand|randi|randh|random|randomi|randomh|rnd31|urandom|noise|pinkish|gauss|"
    r"gaussi|gausstrig|dust|dust2|fractalnoise|jitter|jitter2|jspline|rspline|urd|"
    r"unirand|linrand|trirand|exprand|bexprnd|cauchy|cauchyi|pcauchy|poisson|betarand|"
    r"weibull|vrandh|vrandi|duserrnd|cuserrnd|seed)\b")
# Above this the level is the body's doing. The null over 50 bare resonances maxed at
# +0.246, so this is that plus room, and nothing about it is a preference.
_BODY_MIN = 0.35


def reseed(body, k):
    """The same body with a different noise realisation, or None if there is none to change.

    Every stochastic source in the library is one of four opcodes and `rand` — 24 of the
    27 calls — carries its own explicit seed, so a different realisation is a rewrite of
    that argument and nothing else. `dust`, `dust2` and `pinkish` take the global seed,
    which this cannot reach from here, so a body whose ONLY noise is one of those gets
    None rather than a wrong answer.

    Two things this gets right that a single pattern over the whole text cannot: the seed
    is a different ARGUMENT for `rand` than for `randi`/`randh` (see above), and only the
    CODE part of a line is rewritten. `bubbles` carries the text "Written as `randi 0.14,
    0.19, 2`" in a comment, which the old pattern matched and edited — a comment that then
    disagreed with the code beside it.
    """
    n = [0]

    def sub(m):
        n[0] += 1
        return f"{m.group(1)}{(0.11 + 0.13 * n[0] + 0.37 * k) % 0.98 + 0.01:.4f}"

    out = []
    for line in body.split("\n"):
        code, sep, comment = line.partition(";")
        code = _RESEED_3RD.sub(sub, code)
        code = _RESEED_2ND.sub(sub, code)
        out.append(code + sep + comment)
    if n[0] == 0:
        return None
    return "\n".join(out)


def loudness_is_the_body(body, freq=220.0, dur=8.0, k=(0, 1, 2)):
    """Did the BODY move the level over the note, or is it the noise it is made of?

    `loudness_travel`'s span cannot answer this and neither can anything computed from
    one render. What answers it is that a deterministic modulation is INDEPENDENT of the
    noise realisation and a noise envelope is nothing but the realisation: so render the
    same body with different seeds and correlate the dB envelopes. Returns the median
    pairwise correlation, or None when the body has no reseedable source.

    The null is measured, not assumed: 50 bare resonances over Q 10..2000, two pitches
    and three durations read mean +0.016, sd 0.076, MAX +0.246. So `_BODY_MIN` = 0.35
    sits above the largest reading noise alone produced, the same way `moves` sets its
    own threshold. Above it, the body is doing it.

    ITS POWER IS NOT ABSOLUTE, and that is the honest limit: this measures how much of
    the span the modulation OWNS, so a modulation buried under the body's own envelope
    fluctuation cannot be seen. Measured over 60 tremolos of 0.5..6 dB at 0.3..5 Hz, the
    median reading is +0.892 on a 400 Hz band, +0.681 on 100 Hz and +0.193 on 10 Hz —
    on a band that narrow even 6 dB is invisible, because the envelope's own span there
    is about 13 dB. At 3 dB or more on a band of 100 Hz or wider the minimum is +0.836.

    So: ABOVE the threshold is proof, BELOW it is "not demonstrated", never "innocent".
    Two independent reasons for that asymmetry, both measured. A modulation whose RATE
    comes from the PITCH also decorrelates — the first version of this test compared
    pitches rather than seeds and read `overtone_voice`'s real 6.66 dB step pump as
    -0.03, because the harmonic it steps through is chosen from the pitch. And a body
    stochastic in a way `reseed` cannot reach returns None, which means "not measured".
    """
    if not _STOCHASTIC.search(body):
        # Nothing random in it at all, so whatever the level does, the body does. This is
        # not a measurement and does not need to be one; returning None here said "not
        # measured" about 13 entries whose answer is certain, `fm_bell` among them.
        return 1.0
    ts = []
    for i in k:
        b = reseed(body, i)
        if b is None:
            return None
        y, err = render(b, dur=dur, freq=freq)
        if y is None:
            continue
        e = _loudness_track(y)
        if e is None or len(e) < 16:
            continue
        t = np.arange(len(e))
        p, q = np.polyfit(t, e, 1)
        ts.append(e - (p * t + q))
    if len(ts) < 2:
        return None
    n = min(len(t) for t in ts)
    ts = [t[:n] for t in ts]
    cs = [float(np.corrcoef(a, b)[0, 1])
          for i, a in enumerate(ts) for b in ts[i + 1:]]
    return float(np.median(cs))


def f0(y, t0=0.5, t1=2.5):
    """Period-based, octave-safe f0 — or None when the signal has no pitch.

    Takes the SMALLEST lag whose autocorrelation is within 12% of the best in
    range and is a genuine local peak, so a 2x lag cannot win and the shoulder of
    a bigger peak cannot masquerade as one. Returns None rather than a fabricated
    number when nothing in range is periodic enough.
    """
    seg = _seg(y, t0, t1)
    if len(seg) < 1024:
        return None
    seg = seg - seg.mean()
    if np.sqrt((seg ** 2).mean()) < 1e-8:
        return None
    n = 1 << int(np.ceil(np.log2(len(seg) * 2)))
    S = np.fft.rfft(seg * np.hanning(len(seg)), n)
    ac = np.fft.irfft(np.abs(S) ** 2, n)[:len(seg)]
    if ac[0] <= 0:
        return None
    ac = ac / ac[0]
    lo, hi = max(2, int(SR / 14000)), min(len(ac) - 2, int(SR / 15))
    w = ac[lo:hi]
    if len(w) < 8:
        return None
    best = w.max()
    if best < 0.20:
        return None
    for c in np.where(w >= best * 0.88)[0]:
        i = c + lo
        if ac[i] >= ac[i - 1] and ac[i] >= ac[i + 1]:
            a, b, cc = ac[i - 1], ac[i], ac[i + 1]
            den = a - 2 * b + cc
            return float(SR / (i + (0.5 * (a - cc) / den if den else 0)))
    return None


def cents(measured, asked):
    if not measured or not asked:
        return None
    return float(1200 * np.log2(measured / asked))


def _mag(y, t0, t1):
    s = _seg(y, t0, t1)
    S = np.abs(np.fft.rfft(s * np.hanning(len(s))))
    return S, np.fft.rfftfreq(len(s), 1 / SR)


def centroid(y, t0=0.5, t1=3.5):
    """The amplitude-weighted spectral centroid — a DESCRIPTIVE colour coordinate.

    Every centroid figure quoted in the library's notes is this one. It is not the
    right weighting for an audibility CLAIM: see `centroid_audible`.
    """
    S, fq = _mag(y, t0, t1)
    return float((S * fq).sum() / max(S.sum(), 1e-12))


def centroid_audible(y, t0=0.5, t1=3.5):
    """The POWER-weighted centroid, for asking whether a listener would notice.

    An amplitude-weighted centroid on a linear frequency axis hands enormous
    leverage to quiet high-frequency energy: a partial 60 dB down at 18 kHz
    contributes 0.001 x 18000 against the fundamental's 1 x 220. So a standing
    220 Hz sine plus an inaudible 18 kHz whisper tremoloing at 0.5 Hz passed
    `moves()` with a span of 18.7 Hz at coherence 0.996 — a "movement by default"
    gate satisfied by a static tone.

    Power weighting is the standard definition anyway, and it removes the leverage
    arithmetically rather than by a threshold: that same signal now spans 0.04 Hz.
    The descriptive `centroid` is left alone because the whole library's notes quote
    it and because a colour coordinate and a perceptual claim are different
    questions. Measured over the 50 shipped bodies, power/amplitude runs 0.06 to
    1.07 with a median of 0.59, so the two are NOT interchangeable — do not compare
    a number from one against a number from the other.
    """
    S, fq = _mag(y, t0, t1)
    P = S ** 2
    return float((P * fq).sum() / max(P.sum(), 1e-12))


def partials(y, f, n=16, t0=0.5, t1=3.5):
    """Per-harmonic level in dB relative to the strongest of them."""
    S, fq = _mag(y, t0, t1)
    out = []
    for k in range(1, n + 1):
        m = (fq > f * k * 0.985) & (fq < f * k * 1.015)
        out.append(float(S[m].max()) if m.any() else 0.0)
    ref = max(out) or 1.0
    return [round(float(20 * np.log10(max(v, 1e-9) / ref)), 1) for v in out]


def comb_contrast(y, f, n=24, t0=0.5, t1=3.5):
    """How strongly the spectrum is combed at f, in dB.

    Harmonic bands against the half-way bands between them. The measure damping
    actually needs: counting "how many partials are visible" answers a different
    question and moves with the exciter's colour, not with the resonator's Q.
    Calibrated: white noise 0.1 dB, streson fb 0.995 / 0.90 / 0.575 -> 35.4 /
    22.3 / 10.7 dB.

    Two details are load-bearing, and getting either wrong reads as "the comb is
    shallow" on a resonator that is in fact ringing hard:
      - the bands are a fixed width in Hz (a fraction of the FUNDAMENTAL), not a
        percentage of each band's own centre, which would widen with k until a
        high harmonic's band is mostly the valley beside it;
      - the peak of each band, not its mean, since a comb tooth is narrow and
        averaging it against its own skirts dilutes exactly what is measured.
    Together they make the reading independent of the band width — 35.0 to 36.1 dB
    across a 7x sweep of it — which is the property that makes it a meter rather
    than a knob.
    """
    S, fq = _mag(y, t0, t1)
    half = f * 0.08
    peaks, valleys = [], []
    for k in range(1, n + 1):
        for centre, bucket in ((f * k, peaks), (f * (k + 0.5), valleys)):
            m = (fq > centre - half) & (fq < centre + half)
            if m.any():
                bucket.append(float(S[m].max()))
    if not peaks or not valleys:
        return 0.0
    return float(20 * np.log10(max(np.mean(peaks), 1e-12)
                               / max(np.mean(valleys), 1e-12)))


def logspec_rel(y, freq, lo_c=-600.0, hi_c=4200.0, cents_per_bin=20.0,
                t0=0.5, t1=3.5):
    """The magnitude spectrum on a log axis measured FROM the played note.

    The frame is what makes this a tracking meter. Expressed relative to its own
    `kfreq`, a sound that follows the keyboard has the SAME spectrum at every
    pitch — a harmonic comb, an inharmonic modal bank and a formant set alike,
    since scaling f0 is a pure translation on a log axis. One that ignores the
    keyboard has a spectrum that slides by exactly the interval played.

    The upper bound is a resolution limit. Above ~+4200 cents (five harmonics past
    the fourth octave) a 20-cent grid can no longer RESOLVE adjacent harmonics —
    the spacing 1200*log2(1+1/h) falls under two bins around h=43 — so a saw's comb
    turns into a smooth mush that correlates with itself at any shift, which is
    what read a saw's octave as 1360 of 3600 cents. The lower bound only has to
    reach under anything an oscillator puts energy into, and -1500 cents does:
    it was -600, which is above a sub-octave, and a body an octave down (`sub_sine`
    territory) then fell out of the band entirely and was reported "mixed".

    Each grid cell takes the LARGEST bin it spans, not a sample at its centre.
    This is not a refinement — a point sample makes the meter lie about most of
    the library. Vibrato smears a partial over several FFT bins, and the grid step
    at the top of the band is wider than that smear, so the point at the nominal
    frequency can land in the notch BETWEEN the two turning-point peaks: measured
    on the shipped `saw` at 880 Hz, the band around the fundamental peaks at 12349
    while the sample at exactly 880.0 Hz reads 11.5, a factor of 1073. That drove
    `r_note` for `saw` to 0.149 (0.957 with the max, 0.992 with the vibrato
    switched off), and `pulse` to within 0.032 of the meter declaring a plain pulse
    wave not to follow the keyboard. `comb_contrast` in this file already takes the
    band peak for exactly this reason.
    """
    S, fq = _mag(y, t0, t1)
    grid = np.arange(lo_c, hi_c, cents_per_bin)
    edges = freq * 2 ** ((np.append(grid, grid[-1] + cents_per_bin)
                          - cents_per_bin / 2) / 1200.0)
    idx = np.searchsorted(fq, edges)
    out = np.empty(len(grid))
    for i in range(len(grid)):
        a, b = idx[i], max(idx[i + 1], idx[i] + 1)
        seg = S[a:b]
        out[i] = seg.max() if seg.size else 0.0
    return grid, out


def _corr(a, b):
    """Pearson r, or None where there is nothing to correlate.

    None rather than a sentinel number: a -2.0 standing in for "cannot measure"
    compares as a correlation, and `tracks` would then read an unmeasurable fixed
    hypothesis as proof that the sound follows the keyboard."""
    n = min(len(a), len(b))
    if n < 8:
        return None
    a, b = a[:n] - a[:n].mean(), b[:n] - b[:n].mean()
    den = float(np.sqrt((a ** 2).sum() * (b ** 2).sum()))
    return float((a * b).sum() / den) if den > 0 else None


def tracks(y_low, y_high, f_low, f_high, cents_per_bin=20.0):
    """M2's real meter — did the spectrum come with the keyboard, or stay put?

    Two hypotheses, each scored once over the same band, no search:

      `r_note`   the two spectra agree when each is read FROM its own played note
                 — the sound follows the keyboard;
      `r_fixed`  they agree when both are read on one absolute frequency axis
                 — the sound has its own register and ignores the keyboard.

    `verdict` is whichever fits better by more than 0.1, else "mixed" — the honest
    answer for something like a fixed formant set over a tracking glottal source,
    where both are partly true.

    Two hypotheses rather than a lag search, because the lag search was tried and
    is not safe here: a short overlap at an absurd shift can score a perfect 1.0
    when one aligned spike dominates it. Measured — a `saw` whose fundamental
    landed on the 9th harmonic of the octave-up render scored r=1.0 at -3800
    cents, i.e. the meter called the plainest tracking oscillator in the library
    broken. Both correlations here run over the full band, so nothing can win by
    being narrow.

    And it is these two rather than the two obvious meters because both of those
    give confident wrong answers on exactly the entries that need judging:
      - **`f0`** locks onto the slow beat of a Q=900 inharmonic bank and reports
        37.8 Hz for a played 110 (`struck_bar`), and returns nothing at all for an
        unpitched membrane.
      - **the spectral centroid** SATURATES on any band-limited source: `vco2`
        band-limits to Nyquist, so a saw has 400 harmonics at 55 Hz and 12 at
        1760, and its centroid rises far less than proportionally — measured slope
        0.675 against a keyboard it follows to the cent.
    """
    lo_c, hi_c = -1500.0, 4200.0
    _, A = logspec_rel(y_low, f_low, lo_c, hi_c, cents_per_bin)
    _, B = logspec_rel(y_high, f_high, lo_c, hi_c, cents_per_bin)
    r_note = _corr(A, B)
    # The same two renders on ONE absolute axis, over the band the low note spans.
    lo_hz = f_low * 2 ** (lo_c / 1200.0)
    span_c = hi_c - lo_c
    _, Aa = logspec_rel(y_low, lo_hz, 0.0, span_c, cents_per_bin)
    _, Ba = logspec_rel(y_high, lo_hz, 0.0, span_c, cents_per_bin)
    r_fixed = _corr(Aa, Ba)
    if r_note is None or r_fixed is None:
        verdict = "not measurable"
    else:
        verdict = ("tracks" if r_note > r_fixed + 0.1 else
                   "fixed register" if r_fixed > r_note + 0.1 else "mixed")
    return {"asked_cents": round(float(1200 * np.log2(f_high / f_low)), 1),
            "r_note": None if r_note is None else round(r_note, 3),
            "r_fixed": None if r_fixed is None else round(r_fixed, 3),
            "verdict": verdict}


def travel(y, n=8):
    """Colour over the course of the note: the centroid of n successive windows.

    Movement by default is a platform fundamental, and a fundamental that is a
    requirement but not a measurement is a regression waiting to ship: a corpus of
    standing tones passes an implementation that cannot move at all. This is the
    objective signature — where the colour goes, and how far — so "it moves" stops
    being a matter of what somebody remembers hearing.

    The window length picks which RATE of motion is visible, and reading only one
    of them will call a moving instrument still: `pwm`'s duty sweep reads 48 Hz of
    travel in 0.5 s windows (n=8, a full LFO cycle per window, averaged flat) and
    696 Hz in 31 ms ones (n=128). A standing sine reads 0 at every window length,
    which is what makes the fast reading evidence rather than noise. `measure()`
    therefore reports both.

    Tracked on `centroid_audible`, not `centroid`: this is a claim about whether the
    sound moves, and amplitude weighting on a linear axis let an inaudible partial
    carry that claim. The span this returns is therefore NOT comparable with the
    descriptive centroid figures in the library's notes.
    """
    n = max(2, n)
    edges = np.linspace(0, len(y) / SR, n + 1)
    cs = [centroid_audible(y, a, b) for a, b in zip(edges[:-1], edges[1:])]
    return cs, (max(cs) - min(cs))


def coherence(y, n=128):
    """How much of that travel is MOVEMENT and how much is just noise variance.

    The span `travel` returns cannot tell the two apart, and on this library the
    difference decides a platform fundamental. A stochastic source's centroid
    wanders from window to window all by itself: measured, plain `rand` with
    nothing modulating it reads 830 Hz of travel at n=128, more than most of the
    genuinely moving entries, while a standing sine reads 0.4. So a movement test
    built on the span alone passes every noise bed in the library and would certify
    "movement by default" on a body that provably cannot move.

    What separates them is the SHAPE of the centroid track, not its range: a track
    driven by an LFO or an envelope has its energy in a FEW rates, a track that is
    only variance spreads it over all of them. So this is one minus the spectral
    flatness of the track — 1 for a track that moves at some definite rate, 0 for
    white wandering, and free of any assumption about WHICH rate.

    It was the lag-1 autocorrelation, which is the same idea done cheaply, and the
    cheapness cost real answers. Lag-1 on an n=128 track over 4 s samples the track
    at 32 Hz, and the autocorrelation of a component at rate f is cos(2*pi*f/32) —
    which is exactly ZERO at 8 Hz and NEGATIVE above it. A wineglass whose two
    quadrupole modes beat 8 Hz apart therefore read as having no movement at all,
    in 6 of its 27 parameter corners, while beating audibly. Flatness has no such
    null: a peak anywhere in the track spectrum counts.
    """
    cs, _ = travel(y, n)
    a = np.asarray(cs, dtype=float)
    span = float(a.max() - a.min()) if len(a) else 0.0
    a = a - a.mean()
    if len(a) < 8 or span < 1.0:
        # Under a hertz of travel there is no shape to characterise, and the
        # flatness of a float noise floor is a number about nothing. A standing
        # sine read 0.98 through this before the guard was absolute.
        return 0.0
    P = np.abs(np.fft.rfft(a * np.hanning(len(a)))) ** 2
    P = P[1:]                       # drop DC: the mean is already removed
    P = P[P > 0]
    if P.size < 4:
        return 0.0
    flat = float(np.exp(np.log(P).mean()) / P.mean())
    # A single periodogram of white noise does NOT read flatness 1: its bins are
    # exponentially distributed, whose geometric mean is exp(-gamma) = 0.5615 of
    # their arithmetic mean. Uncorrected, genuinely static noise measured 0.64 and
    # would have passed as movement. Dividing by that expectation puts white at 0
    # and a single-rate modulation at 1, which is what the number claims to mean.
    return float(max(0.0, min(1.0, 1.0 - flat / 0.5615)))


def crest_db(y, t0=0.5, t1=3.5):
    """Peak over RMS, in dB — how far the loudest single event stands above the bed.

    The whole spectral half of this meter is blind to EVENT DENSITY, and a class of
    axis in this library is nothing else. `crackle`'s `blaze` runs its pop rate from
    under six a second to 170: the centroid moves 54 Hz across all of it, the comb
    0.7 dB, and by every colour reading in `measure()` the axis is inert — while by
    ear it is the difference between embers and a bonfire. More of the same event is
    not a different spectrum, so no spectrum can see it.

    This can. It is a max statistic, which is the point: a sparse transient stays
    visible here even when its POWER is negligible, and that matches hearing, where
    a tick 18 dB under a hiss is plainly a tick. Measured across `crackle`'s axis it
    falls 19.0 dB to 10.8 monotonically. Being a max statistic is also its limit —
    it reads one sample out of 130 000, so it is noisy run to run and only a large
    move means anything.
    """
    s = _seg(y, t0, t1)
    s = s - s.mean()
    r = float(np.sqrt((s ** 2).mean()))
    if r <= 0.0:
        return None
    return float(20 * np.log10(float(np.abs(s).max()) / r))


def event_rate_hz(y, t0=0.5, t1=3.5, win=0.005):
    """Impulses per second, read back from how much the short-time envelope varies.

    An exact law, not a heuristic: the impulses in a window of a Poisson train at
    rate L are Poisson(L*win), so the envelope's coefficient of variation is
    1/sqrt(L*win) and L = 1/(win * cv^2). On a bare `dust2` that inversion recovers
    the rate to within a factor of 1.7 across a 280-fold sweep — 22/s reads 18, 366
    reads 449, 1400 reads 2427.

    Its limit has to be quoted with it, because it is severe and it is the reason
    `crest_db` exists beside it: ANY continuous bed swamps this. Inside `rain` the
    patter sits 13.8 dB under the wash, and a sweep from 366 to 7300 impacts a
    second moves the body's envelope roughness only 0.057 -> 0.055 — so this reads
    "already dense" at both ends and sees nothing, while the bare source it is made
    of moves 0.667 -> 0.108. A reading here that does not move is evidence about
    what is AUDIBLE in the mix, never evidence that the source did not change.

    Above a few thousand per second the figure stops being a rate and is only a way of
    saying "continuous" — white noise reads about 200 000/s, which is not a claim about
    white noise but the inversion running out of resolution once every window is full.
    """
    s = _seg(y, t0, t1)
    n = int(win * SR)
    if n < 1 or len(s) < 4 * n:
        return None
    e = np.sqrt((s[:len(s) // n * n].reshape(-1, n) ** 2).mean(1))
    m = float(e.mean())
    if m <= 0.0:
        return None
    cv = float(e.std() / m)
    if cv <= 0.0:
        return None
    return float(1.0 / (win * cv * cv))


def odd_even_db(y, freq, n=16):
    """Odd-harmonic energy over even-harmonic energy, in dB.

    Two things in this library need this and nothing else can see either.

    A **bore's shape**: a cylinder passes only the odd harmonics, a cone passes
    all of them. That is the objective difference between a clarinet and a
    saxophone, and it is enormous — measured on this build, `wgclar` reads
    +44.3 dB and the library's `clarinet` +115 dB, against `sax` +4.1 dB and
    `brass` +3.2 dB. No centroid or comb reading separates them at all.

    A **pulse wave's duty cycle**: the harmonic amplitudes go as |sin(pi*k*d)|/k,
    so at d = 0.5 every even harmonic vanishes and the ratio is huge, and it
    collapses as the duty narrows. Measured on `vco2`'s own `kpw`: -126.6 dB at
    50 % duty, -5.5 dB at 35 %, -1.1 dB at 10 %. This is what makes PWM
    objectively testable — the centroid barely moves under a duty sweep (805 Hz,
    non-monotonic), which is why a duty axis judged by colour alone reads as
    inert and gets thrown away.
    """
    lin = [10 ** (v / 20.0) for v in partials(y, freq, n=n)]
    odd = sum(lin[0::2])   # partials() starts at the fundamental = harmonic 1
    even = sum(lin[1::2])
    return float(20 * np.log10(max(odd, 1e-12) / max(even, 1e-12)))


def beat(y, lo_hz=0.5, hi_hz=25.0, t0=0.5, t1=3.5, at_hz=None, f0_hz=None):
    """The slow amplitude beat: how deep it is, and how fast.

    Two sources a few cents apart beat, and that beat is the whole substance of a
    musette accordion, a bagpipe drone and a detuned analogue pad. No spectral
    meter can see it — the spectrum is the same whether the partials beat or not,
    so `centroid`, `comb_contrast` and `travel` all read a musette axis as doing
    almost nothing (measured: `bagpipe`'s detune moved the centroid by 1 Hz over
    its whole range while the beat rate went from 1.0 to 4.0 Hz). Judging such an
    axis by colour alone concludes it is inert and invites deleting it.

    Read off the rectified signal at the FULL sample rate, as the largest spectral
    component in the beat band relative to the mean level, so `depth` comes out on
    the same scale as an asked-for AM depth. Calibrated against exactly that:
    asked 0.00 / 0.10 / 0.30 / 0.60 read 0.000 / 0.100 / 0.300 / 0.600.

    The full rate is not an optimisation, it is the correctness of the whole meter.
    Rectifying a tone at f0 puts a ripple at 2*f0 (at f0 itself, for an asymmetric
    waveform) into the envelope. This used to be block-averaged to 200.455 Hz, which
    ALIASES that ripple straight into the beat band, and the block average is a
    boxcar whose rejection at 110 Hz is only 0.574 — nowhere near enough to stop it.
    Measured on plain unmodulated sines, with nothing whatever modulating them:
    55 Hz read a beat of depth 0.372 at 90.39 Hz (the 110 Hz ripple folded about
    200.455) and 110 Hz read 0.052 at 19.68 Hz (220 folded to 19.55). The second one
    landed inside the NARROW 25 Hz band too, and at 0.052 it was within reach of the
    0.05 spread the audit's M6 treats as a real rate control, so no choice of band was
    ever going to fix it. (M6 thresholds the spread ACROSS an axis's anchors, not one
    absolute depth, so a constant fabrication like this one does not pass M6 on its
    own — it fabricates the baseline every anchor is then read against.)
    At the full rate nothing folds: the ripple stays at 2*f0 where it belongs.

    `f0_hz` closes the other half. A component at or above the fundamental is not a
    beat — it is a sideband, and the spectral meters already see it — so the band is
    capped just below the fundamental when the caller knows it. Unmodulated sines then
    read 0.000 at every register instead of 0.372, and an asked 0.30 at 90 Hz reads
    0.300 rather than the 0.200 the boxcar's roll-off left of it.

    `f0_hz` is the fundamental this signal ACTUALLY has, not the pitch that was asked
    for, and the difference is the whole point: an entry that plays an octave below the
    asked note has its own ripple at the asked pitch, so a cap derived from the ask
    excludes nothing. Measured at an asked 220 Hz — `bass_saw` sounds 109.9 and read a
    beat of 0.434 at 110.0 Hz, `sub_sine` 0.372 at 110.0, `organ` 0.145 at 110.67, and
    `bagpipe`, which sounds two octaves down, 0.213 at 109 Hz. None of those bodies modulates anything
    near those rates; `bass_saw`'s only LFO is at 0.043 Hz. Off the measured
    fundamental they read 0.019 / 0.020 / 0.084 / 0.212 — and bagpipe's is its real
    drone beat, at 2.33 Hz, which is what this meter was written for.

    The cap is 2 Hz below the fundamental and not half of it, because half throws away
    a whole octave of genuine beat: an asked 0.30 at 115 Hz on a 220 Hz carrier read
    0.003 at the half-cap and reads 0.300 now, `didgeridoo`'s 97 Hz vocal hum went from
    0.155 to 0.533 at a 110 Hz note, and `sax`'s 74 Hz growl from nothing to its own
    line. 2 Hz is six bins of the 3 s window against a Hann main lobe two bins wide,
    which is what it takes to keep the fundamental's own line out.

    `at_hz` asks about ONE rate instead of the strongest, because the strongest hides
    the others. A rubbed wineglass has two independent amplitude motions at once —
    the two quadrupole modes beating, and the finger passing four antinodes a lap —
    and searching for the maximum reported only the beat, at a depth that did not
    budge when the pulsation axis was swept from 0 to 1. The instrument then reads
    as inert on exactly the axis it is named for. Calibrated on a sum of two AMs
    where one is deliberately the louder: asked 0.30 at 3 Hz alongside 0.45 at
    9 Hz, `at_hz=3` reads 0.299 while the plain call reports the 9 Hz line.

    It has one limit, and it is not small: a STRONGER line within about a hertz
    leaks in and is reported instead. Measured, a 0.45-deep line at a distance of
    0.4 / 0.8 / 1.2 / 1.6 Hz makes `at_hz=4.6` read 0.400 / 0.350 / 0.021 / 0.000 —
    so the answer is trustworthy from roughly 1.2 Hz of separation onward and is
    simply the neighbour below that. This is the window's own resolution and no
    choice of band fixes it; 3 s at 200 Hz gives 0.33 Hz bins and a Hann main lobe
    two bins wide. The glass harp is exactly the case that found it: asking about
    its 4.6 Hz pulsation returned 0.23 at the pulsation depth of ZERO, because its
    3.4 Hz beat sits one bin below the band edge. If two rates are that close, they
    are one measurement, and the axis must be judged by colour instead.

    The resolution is set by the window in SECONDS and not by the envelope rate, so
    it is unchanged by reading at the full rate: 3 s gives 0.33 Hz bins either way.
    """
    s = y[int(t0 * SR):int(t1 * SR)]
    if len(s) < SR // 4:
        return 0.0, 0.0
    mean = float(np.abs(s).mean())
    env = np.abs(s) - mean
    F = np.abs(np.fft.rfft(env * np.hanning(len(env))))
    fq = np.fft.rfftfreq(len(env), 1 / SR)
    if at_hz is not None:
        # one rate, widened by the window's own resolution so a slightly-off rate
        # is still found: a Hann window spreads a line over ~2 bins each side.
        w = 2.5 * SR / len(env)
        band = (fq >= at_hz - w) & (fq <= at_hz + w)
        if not band.any() or mean <= 0:
            return 0.0, 0.0
        amp = 2 * float(F[band].max()) / (len(env) / 2)
        return amp / mean, float(at_hz)
    if f0_hz:
        hi_hz = min(hi_hz, max(f0_hz - 2.0, lo_hz + 0.1))
    band = (fq >= lo_hz) & (fq <= hi_hz)
    if not band.any() or mean <= 0:
        return 0.0, 0.0
    amp = 2 * float(F[band].max()) / (len(env) / 2)
    return amp / mean, float(fq[band][np.argmax(F[band])])


# What a STATIONARY noise bed reads on the two statistics that look like liveness.
# Measured 2026-07-25 over 40 renders: `rand` bare and through `reson` at ten
# centre/width pairs from 220/40 to 11000/6000, four seeds each, 4 s at a 220 Hz ask.
# Nothing in any of them moves — the source is stationary and no k-rate value changes.
#
#   colour travel   82 .. 1005 cents      worst at reson 300/200
#   crest           4.78 .. 14.55 dB      worst at reson 300/200
#
# Both numbers are large, and that is the point of recording them: a narrowband filter
# on white noise has a Rayleigh envelope and a centroid that wanders over most of its
# own passband, so a bed that does nothing at all reads as travelling more than an
# octave and peaking 14 dB over its own average. Any claim that a body's colour
# "travels" or that it "has events" has to clear these before it means anything. The
# library's own beds do not: `rain` reads 217 cents and 10.92 dB, `crackle` 273 and
# 14.03, `thunder` 1209 and 13.16, `cymbal` 797 and 13.37 — all inside the null on at
# least one, three of them on both.
#
# These constants exist to REFUSE a threshold, not to set one. A real sweep travels
# 959 cents, less than the bed that does nothing, so the populations overlap and no
# span bound separates them; the selftest asserts that overlap so it cannot be
# rediscovered as a threshold later. Coherence is the only statistic in this file that
# tells movement from variance, and for a stochastic texture it is the wrong question.
# See `docs/parked/lco_movement_gate_three_sounds.md`.
STATIONARY_SPAN_NULL_CENTS = 1005.0
STATIONARY_CREST_NULL_DB = 14.55


def travel_cents(y, n=128):
    """How far the colour travels, as an interval rather than in hertz.

    The span is judged in CENTS, not hertz. An absolute hertz bound is the same class
    of mistake as amplitude weighting was: 8 Hz of travel is nothing on `hiss`'s
    13.5 kHz centroid and a plainly audible wobble on `sub_sine`'s 205 Hz."""
    cs, span = travel(y, n)
    mean = float(np.mean(cs)) if len(cs) else 0.0
    return (1200 * np.log2(1 + span / mean)) if mean > 1e-9 else 0.0, span


def moves(y, n=128, span_cents=60.0, min_coherence=0.35):
    """Does the colour actually travel over the note? Span AND shape must agree.

    The threshold is calibrated on the shipped library rather than chosen, and the
    calibration also says what this test CANNOT do. Measured over all 50 entries at
    4 s and n=128:

      unpitched beds            0.00 - 0.18   (thunder, rain, crackle .00, hiss .06,
                                               noise .07, drum_head .13, cymbal .18)
      a deliberate modulation   0.51 - 1.00   (rhodes .51, wind .53, struck_bar .68,
                                               free_reed .83, harmonica .99, and 23
                                               entries at 1.00)

    and the null distribution over 60 renders of unmodulated noise, both bare and
    filtered, is mean 0.030, sd 0.045, MAX 0.188. Nothing whatever falls between 0.18
    and 0.51, so 0.35 sits in the middle of an empty band at nearly twice the largest
    false reading noise alone produced, which is why it is the threshold.
    But the band 0.20-0.86 is not empty in general: it holds three classes this
    meter cannot resolve at 4 s, and a reading in it means "not demonstrated in four
    seconds", never "static".

      * movement slower than the window. `wind` gusts at 0.071 Hz and reads 0.34;
        `surf` swells at 0.055 Hz and reads 0.57. Both move audibly. A quarter of a
        cycle cannot be told from a trend.
      * low-Q noise-driven mode banks, which ARE mostly filtered noise: `drum_head`
        (Q 7-20) reads 0.00, `cymbal` 0.24, against `glass` (Q 1400-2000) at 0.73.
      * bodies that decay to near silence inside the window, where the late
        centroid is a noise floor: `rhodes` 0.00, `wurlitzer` 0.37.
    """
    r1 = coherence(y, n)
    # Measured in cents the two populations separate more sharply than in hertz — the
    # eight bodies with nothing to move read 0 to 54 cents (sine 0, triangle 1,
    # square 3, bass_saw 7, the two fixed vowels 21, pulse 25, sub_sine 54) and
    # everything else reads 78 and up — and 60 cents fails exactly the same eight the
    # old 8 Hz bound did, so the change is verdict-neutral on the library and gives
    # the number a meaning. 60 cents of brightness is a bit over a quarter tone.
    span_c, span = travel_cents(y, n)
    return (bool(span_c > span_cents and r1 > min_coherence),
            round(span, 1), round(r1, 3))


def _r2(v):
    return None if v is None else round(v, 2)


def measure(y, asked_freq):
    f = f0(y)
    cs, span = travel(y, 8)
    _, fast = travel(y, 128)
    does_move, _, r1 = moves(y)
    # The beat is reported because a whole class of axis is a RATE and no spectral
    # meter can see one: a bagpipe's detune, a cricket's chirp rate and a frog's pulse
    # rate all move the centroid by single hertz while changing the sound completely.
    # An audit that reads only colour calls such an axis inert and invites deleting it.
    # The band reaches 120 Hz because these rates go far above a musette's few hertz,
    # and is capped just below the fundamental so the carrier's own rectification ripple
    # cannot be read back as a beat — see `beat`, which read 0.372 on a plain sine
    # before the cap and the full-rate envelope went in.
    #
    # The cap comes off `f`, the fundamental this signal ACTUALLY has, and not off the
    # pitch that was asked for. Passing the ask left the bug live for every entry that
    # sounds below the note: `bass_saw` read a beat of 0.434 at 110.0 Hz at an asked
    # 220, with nothing in it modulating faster than 0.043 Hz. When there is no
    # fundamental to find there is no ripple to exclude either, so an unpitched bed
    # is read uncapped.
    #
    # THE CAP HIDES REAL MODULATION ABOVE THE FUNDAMENTAL, and at the bass end of the
    # keyboard that is exactly where several axes live. Measured at a 55 Hz note:
    # `cricket`'s chirp runs at 31 Hz and reads 0.033 at 24.00 Hz capped against 0.401
    # at its own rate — twelve times understated, because `f0` returns 27.56 there, a
    # subharmonic of 55, and the cap follows it down. `frog`'s 57.7 Hz pulse reads
    # 0.064 against 0.187, `didgeridoo`'s 97 Hz hum 0.059 against 0.158. The cap's rule
    # is still the right one (a component at or above the fundamental IS a sideband,
    # and without it a plain saw read 0.434), and the cap cannot be raised on a hunch
    # about which f0 is the "real" one — `bass_saw` genuinely sounds an octave below
    # the note it is given, and its ripple is genuinely down there. So the cap is
    # REPORTED as `beat_cap_hz`, and an axis whose rate is known is read at that rate
    # with `beat(y, at_hz=rate)`, which ignores the cap by construction. A capped
    # reading near zero on an entry whose note quotes a rate above `beat_cap_hz` is a
    # floor and not a measurement.
    bd, br = beat(y, lo_hz=0.5, hi_hz=120.0, f0_hz=f)
    return {"f0": None if f is None else round(f, 2),
            "cents": None if f is None else round(cents(f, asked_freq), 1),
            "centroid": round(centroid(y), 1),
            "centroid_travel_hz": round(span, 1),
            "centroid_motion_hz": round(fast, 1),
            # The same travel as an interval, which is the form the thresholds use and
            # the only form comparable across a 13 kHz bed and a 205 Hz drone.
            "centroid_travel_cents": round(travel_cents(y, 128)[0], 0),
            # The span alone calls static noise the most mobile thing here.
            "motion_coherence": r1,
            "moves": does_move,
            # Loudness travelling INSIDE the note, which `rms_db` averages away and
            # `sustain` only sees if it drifts one way. `overtone_voice` stepped 6.08 dB
            # over a held note while every corner's mean was steady to 0.33 dB.
            # The span says how far the level moved and NOT whether the body moved it —
            # for that see `loudness_is_the_body`, which needs the body and not the
            # samples and so cannot be reported from here.
            "loudness_travel_db": _r2(loudness_travel(y)),
            # …and the monotone half of it, which the detrend above removes exactly and
            # §4 forbids just as squarely. None means the note was too short to read.
            "loudness_drift_db": _r2(loudness_drift_db(y)),
            "beat_depth": round(bd, 3),
            "beat_rate_hz": round(br, 2),
            # The cap that was applied, so a reading suppressed by it is visible as
            # such. Reporting the UNCAPPED depth beside it was tried and is worthless:
            # it measures the very ripple the cap removes, so a plain sine reads 0.667
            # and 30 of 57 entries "disagree with the cap" while nothing is wrong.
            "beat_cap_hz": None if f is None else round(max(f - 2.0, 0.6), 2),
            "centroid_over_note": [round(c) for c in cs],
            "rms_db": round(rms_db(y), 2),
            "peak_p999": round(peak_p999(y), 3),
            # The true peak over the whole buffer, including the onset. Report it, but
            # do NOT judge the host clip on it: see ONSET_S — `string` and `ice` were
            # both condemned on a filter start-up sample.
            "peak": round(float(np.abs(y).max()), 3),
            # The peak after the onset. THIS is the figure the host clip is judged on,
            # against HOST_TRANSPARENT. `peak` minus this is the size of the onset
            # transient, which the host's soft curve rounds rather than clips flat.
            "peak_late": round(float(np.abs(y[int(ONSET_S * SR):]).max())
                               if len(y) > int(ONSET_S * SR)
                               else float(np.abs(y).max()), 3),
            "sustain": round(sustain(y), 3),
            # Combed at the pitch the signal HAS, not the one it was asked for. Read
            # against the ask, a body that sounds an octave down has its "half-way
            # between the harmonics" points land on real harmonics — 330 and 550 are
            # the 6th and 10th of 55 — so the valleys are teeth and the contrast
            # collapses. `bagpipe` read 15.2 dB where it is combed 100.1, `fm_bell`
            # 1.0 against 61.5, `organ` 14.2 against 67.5, `metallic_fm` 4.3 against
            # 56.6: eight entries sound more than 50 cents off the ask and four of
            # them were wrong by 52 to 85 dB. The ask is the fallback for an unpitched
            # bed, where there is no grid of its own to use.
            "comb_db": round(comb_contrast(y, f or asked_freq), 1),
            # Event density, which nothing above can see. A whole class of axis here
            # changes how OFTEN something happens without changing what it is, and
            # every spectral reading calls that inert: `crackle`'s pop rate runs 30-fold
            # and moves the centroid 54 Hz. `crest_db` sees a sparse event even at
            # negligible power; `event_rate_hz` is the honest density of what is audible
            # and is swamped by any continuous bed. Both, because neither alone is enough.
            "crest_db": _r2(crest_db(y)),
            "event_rate_hz": _r2(event_rate_hz(y)),
            "partials": partials(y, asked_freq)}


# ─────────────────────────────────────────────────────────────────────────────
# Calibration — the meter measured against known answers
# ─────────────────────────────────────────────────────────────────────────────

_SINE = "asig    poscil 0.5, kfreq * koct1, giSine"
_SAW = "asig    vco2 0.5, kfreq * koct1, 0"
# gbuzz normalises to PEAK, not RMS; the closed-form hold is what keeps a
# harmonic stack at one loudness while its harmonic count moves.
_BUZZ = """knh     = 12
kmul    = 0.7
knrm    = ((1-kmul^knh)/(1-kmul)) / sqrt((1-kmul^(2*knh))/(1-kmul*kmul)*0.5)
asig    gbuzz 0.17 * knrm, kfreq * koct1, knh, 1, kmul, giCos"""
# No energy AT the fundamental: a pitch meter that follows the loudest partial
# answers 440 here, a period-based one answers 220.
_MISSING = """a2      poscil 0.3, kfreq * koct1 * 2, giSine
a3      poscil 0.3, kfreq * koct1 * 3, giSine
asig    = a2 + a3"""
# The 31-bit generator (isel=1), which is what every body in the library uses. The
# default 16-bit one repeats every 1.365 s, and that is a real 0.73 Hz periodicity:
# `coherence` reads it as 0.358 against 0.060 for this form, correctly, since the
# signal genuinely does repeat. Calling that "static noise" in a calibration made
# the meter look wrong when it was the test signal that was moving.
_NOISE = "asig    rand 0.4, 0.5, 1"


def _comb(fb):
    # A resonator's output grows roughly as 1/(1-fb), so a fixed exciter level
    # clips the high-feedback case — and a clipped render flattens the very comb
    # this is here to measure. Scaling the drive with the feedback keeps all three
    # calibration points inside the meter's range.
    return f"""aex     rand {0.35 * (1.0 - fb):.5f}
asig    streson aex, kfreq * koct1, {fb}"""


# How many calibration cases there are. Asserted at the end so a group that stops
# running — an early `return`, a render failure that `continue`s past its check, a
# refactor that drops a block — is a FAILURE and not a quietly shorter green run.
_CASES = 88


def selftest():
    """Known signals in, known answers out. Any failure invalidates every number
    this file has ever reported.

    The tally is a LIST and not a bool on purpose. It was a bool named `ok`, and one
    of the movement cases below unpacked its own result into the same name —
    `ok, sp, r1 = moves(yr)` — so the verdict reported whether the LAST rate case
    passed and erased 17 of the 40 others. `--selftest` then printed "meter
    calibrated" and exited 0 with a real failure on screen every single run, which is
    how the looping-noise case shipped. A list cannot be silently rebound by an
    unpacking assignment, and the count is asserted at the end.
    """
    fails, total = [], []

    def check(name, cond, detail):
        total.append(name)
        if not cond:
            fails.append(name)
        print(f"  {'PASS' if cond else 'FAIL'}  {name}: {detail}")

    print("pitch (asked 220 Hz, tolerance +-8 cents)")
    for name, body in (("sine", _SINE), ("saw", _SAW), ("gbuzz", _BUZZ),
                       ("missing fundamental", _MISSING),
                       ("comb-filtered noise", _comb(0.995))):
        y, err = render(body, freq=220.0)
        if err:
            check(name, False, err)
            continue
        c = cents(f0(y), 220.0)
        check(name, c is not None and abs(c) < 8,
              "none" if c is None else f"{c:+.1f} cents")

    print("no pitch where there is none")
    y, err = render(_NOISE)
    check("white noise", err is None and f0(y) is None,
          err or ("None" if f0(y) is None else f"{f0(y):.1f} Hz — fabricated"))

    print("render guards")
    y, err = render("asig    = 0")
    check("silence is an error", err == "SILENT", err)
    y, err = render("asig    poscil 4.0, kfreq * koct1, giSine")
    check("a body the host would clip is flagged", (err or "").startswith("HOT"), err)
    y, err = render("asig    frobnicate 1, 2")
    check("a broken body is an error", (err or "").startswith("csound:"),
          (err or "no error")[:80])

    # The two ceilings are the numbers every level decision in the library is made
    # against, and until this group existed nothing read them: the constant could be
    # 18 % wrong — it WAS — and the suite stayed 73/73. These cases measure the host's
    # own `clip` line rather than restating the arithmetic, so a change to the scaffold
    # that moves the ceiling fails here instead of silently reinterpreting the library.
    print("the host's clip ceiling, measured through the host's own line")
    _clip_line = [l for l in W._TAIL.splitlines() if " clip " in l]
    check("the scaffold still ends on a `clip` the ceiling can be derived from",
          len(_clip_line) == 1 and "0.95, 0.85" in _clip_line[0],
          _clip_line[0].strip() if _clip_line else "no clip line in _TAIL")
    # The transfer curve of the host's own clip line, ramped rather than assumed.
    # `render` outputs `asig` and never passes it through `clip`, so this cannot be
    # measured through the harness — it has to be the opcode itself, with the exact
    # arguments read out of `_TAIL` above.
    _curve = _clip_transfer()
    check("limiting begins where the FOURTH argument says, not the third",
          _curve is not None and abs(_curve["transparent_to"] - 0.8075) < 0.03,
          "could not measure the curve" if _curve is None else
          f"untouched to {_curve['transparent_to']:.4f}, and 0.85*0.95 = 0.8075")
    check("the curve asymptotes below `ilimit` and never reaches it",
          _curve is not None and abs(_curve["ceiling"] - 0.87875) < 0.002
          and _curve["ceiling"] < 0.95,
          "could not measure the curve" if _curve is None else
          f"ceiling {_curve['ceiling']:.5f} against an `ilimit` of 0.95")
    # The two constants against the MEASURED curve. Without these the cases above only
    # described the opcode and left the numbers the library is judged by unconstrained:
    # HOST_TRANSPARENT could be scaled by 0.7 — which would condemn `ice` and `bagpipe`
    # as clipping — and the suite stayed green. A wrong constant is the failure mode that
    # actually happened; a changed host line was never the risk.
    check("HOST_TRANSPARENT is the measured knee, divided by HEADROOM",
          _curve is not None
          and abs(HOST_TRANSPARENT - _curve["transparent_to"] / W.HEADROOM) < 0.12,
          "could not measure the curve" if _curve is None else
          f"{HOST_TRANSPARENT:.4f} against the measured "
          f"{_curve['transparent_to'] / W.HEADROOM:.4f}")
    check("HOST_CLIP is the measured asymptote, divided by HEADROOM",
          _curve is not None
          and abs(HOST_CLIP - _curve["ceiling"] / W.HEADROOM) < 0.01,
          "could not measure the curve" if _curve is None else
          f"{HOST_CLIP:.4f} against the measured {_curve['ceiling'] / W.HEADROOM:.4f}")
    check("the transparent ceiling is below the absolute one, and both below the old 2.97",
          HOST_TRANSPARENT < HOST_CLIP < 0.95 / W.HEADROOM,
          f"transparent {HOST_TRANSPARENT:.3f} < absolute {HOST_CLIP:.3f} < "
          f"the wrong 0.95/HEADROOM {0.95 / W.HEADROOM:.3f}")
    # The onset rule, on what actually produces an onset: `balance`, whose RMS
    # denominator starts at zero. This is the mechanism that had `string` and `ice`
    # reported as clipping entries. A high-Q filter, which is what I first blamed,
    # rings UP gradually and shows no spike at all — the case says so, so the wrong
    # explanation cannot come back.
    print("headroom is judged after the onset, because `balance` starting up is not a hot body")
    # The source has to START AT ZERO for the estimator to divide by nothing, which is
    # `string`'s case: its exciter is scaled by a `poscil` breath, and a poscil at
    # phase 0 is 0. An always-full source like a bare `rand` shows no spike at all.
    _bal = ("kbr      poscil 0.30, 0.23\n"
            "aex      rand 1.0, 0.5, 1\n"
            "astr     streson aex * a(kbr), kfreq * koct1, 0.995\n"
            "aref     poscil 0.35, 400\n"
            "asig     balance astr, aref\n")
    y, err = render(_bal, dur=4.0)
    y2, err2 = render(_bal, dur=4.0, preroll=1.0)
    if y is None or y2 is None:
        check("a body with `balance` measures differently from a fresh instance",
              False, err or err2)
    else:
        r, r2 = measure(y, 220.0), measure(y2, 220.0)
        # The load-bearing claim, and the whole reason `preroll` exists: the peak of a
        # body containing `balance` depends on whether the INSTANCE is fresh, which in
        # the plugin it never is. How large the difference is depends on the source's
        # own attack — `string` reads 3.18 against a sustained 0.81 — so this asserts
        # that the two disagree, not a magnitude that only one body happens to give.
        check("a body with `balance` measures differently from a fresh instance",
              abs(r["peak"] - r2["peak"]) > 0.05,
              f"true peak {r['peak']:.3f} from a fresh instance against "
              f"{r2['peak']:.3f} with a 1 s preroll — the plugin is always the second")
    # `peak_late` must actually be SMALLER on a body with a transient, and the window
    # must be the one ONSET_S says. Without this, ONSET_S = 0.0 (which restores the
    # behaviour that condemned `string`) and a `peak_late` that just returns `peak`
    # both left the suite green — the two cases that looked like coverage were a
    # tautology and a case that passes MORE strongly when the statistic is broken.
    _spike = ("kramp    = (knote < 0.004 ? 1 : 0.12)\n"
              "asig     poscil 2.2 * kramp, kfreq * koct1, giSine\n")
    ys, errs = render(_spike, dur=2.0)
    if ys is None:
        check("a body with a 4 ms transient reads a smaller peak after the onset",
              False, errs)
    else:
        rs = measure(ys, 220.0)
        check("a body with a 4 ms transient reads a smaller peak after the onset",
              rs["peak_late"] < rs["peak"] * 0.5,
              f"true peak {rs['peak']:.3f}, after {ONSET_S * 1000:.0f} ms "
              f"{rs['peak_late']:.3f} — the window has to be wide enough to clear the "
              f"transient and narrow enough to leave the note")
        check("the onset window is where ONSET_S says, not zero and not the whole note",
              0.010 <= ONSET_S <= 0.200,
              f"ONSET_S = {ONSET_S * 1000:.0f} ms: at 0 the transient is judged as the "
              f"body, and past ~200 ms a real sustained over could hide inside it")
    check("a steady body's two peaks agree, so the onset window costs nothing there",
          abs(measure(render(_SINE)[0], 220.0)["peak"]
              - measure(render(_SINE)[0], 220.0)["peak_late"]) < 0.01,
          "a plain sine reads the same before and after the onset window")

    # The preroll's own mechanics: the note must still be a note. If `changed2(ktrig)`
    # stopped firing at the edge, `knote` would never reset and every note-relative
    # body in the library would silently read the instrument's uptime instead.
    print("the preroll reproduces the host: an always-on instance, the note an edge")
    _knote = "asig     poscil 0.4 * (knote < 1 ? knote : 1), kfreq * koct1, giSine\n"
    ya, _ = render(_knote, dur=2.0)
    yb, _ = render(_knote, dur=2.0, preroll=1.5)
    if ya is None or yb is None:
        check("`knote` resets at the note, not at the instance", False, "render failed")
    else:
        # Compared as an ENVELOPE, not sample by sample: the carrier's own phase is
        # NOT note-relative — it has been running through the preroll — so identical
        # samples would be the wrong thing to ask for. What must match is the ramp.
        def env(y):
            k = int(0.05 * SR)
            return np.array([float(np.abs(y[i:i + k]).max())
                             for i in range(0, min(len(y), SR), k)])
        ea, eb = env(ya), env(yb)
        n = min(len(ea), len(eb))
        worst = float(np.abs(ea[:n] - eb[:n]).max())
        check("`knote` resets at the note, not at the instance", worst < 0.01,
              f"a body that ramps over its first second reads the same envelope with "
              f"and without a 1.5 s preroll (worst window differs by {worst:.4f}) — so "
              f"the trigger edge fired and knote restarted")

    # The trim itself, which is the thing that makes any of the above mean anything: a
    # body that is a function of `knote` ALONE must come back bit-identical whatever the
    # preroll. It did not, when the trim was `int(preroll * SR)` — the edge lands on a
    # k-cycle boundary and the nominal time does not, so every buffer was up to 0.95 ms
    # short and frame-misaligned. Compared from the second k-cycle, because `a()`
    # interpolates the first one from the value the preroll left behind.
    # `preroll=0.0` is in the list deliberately: it is the ONLY member with no trim at all
    # (the instance begins at the note, so `knote` is already note time), which makes it an
    # absolute reference. Without it every preroll could be trimmed by the same wrong
    # amount and this case would still be green — and a constant error of up to one
    # k-period is exactly the 0.09-0.95 ms class the trim exists to fix.
    _ramp = "asig     = a(0.4 * (knote < 1 ? knote : 2 - knote))\n"
    _base, _ = render(_ramp, dur=2.0, preroll=1.0)
    _worst, _detail = 0.0, "render failed"
    if _base is not None:
        for _pr in (0.0, 0.3333, 0.5, 0.925, 1.5, 2.0):
            _y, _ = render(_ramp, dur=2.0, preroll=_pr)
            if _y is None:
                _worst = 9.9
                break
            _n = min(len(_y), len(_base))
            _worst = max(_worst, float(np.abs(_y[W.KSMPS:_n] - _base[W.KSMPS:_n]).max()))
        _detail = (f"six prerolls from 0 to 2.0 s agree to {_worst:.9f} — including the "
                   f"un-trimmed 0, so the trim is the trigger edge in ABSOLUTE terms and "
                   f"not merely consistently wrong")
    check("the trim lands on the trigger edge, so the note is the same note",
          _base is not None and _worst < 1e-9, _detail)

    # Bit-identity across prerolls proves the trim is CONSISTENT. It cannot prove it is in
    # the right PLACE — an error of exactly one k-cycle at every preroll is invisible to
    # it, and one k-cycle is 1.45 ms, bigger than the misalignment the trim was written to
    # fix. So: a block-constant body whose value IS the cycle count. After the edge,
    # `knote` is n/kr on cycle n, so the first sample of the returned buffer must read
    # cycle 1 and the sample one k-period later must read cycle 2. Anything else and the
    # note's own first k-period is being thrown away or a preroll cycle kept.
    _cyc = ("kx       = knote * kr / 1000\n"
            "asig     upsamp kx\n")
    _pos, _bad = [], []
    for _pr in (MIN_PREROLL, 0.5, 1.0, 2.0):
        _y, _e = render(_cyc, dur=0.5, preroll=_pr)
        if _y is None or len(_y) <= W.KSMPS:
            _bad.append(f"{_pr:.3f}: {_e}")
            continue
        _pos.append((round(float(_y[0]) * 1000, 6), round(float(_y[W.KSMPS]) * 1000, 6)))
    check("the trim keeps the note's OWN first k-period, and not one of the preroll's",
          not _bad and len(_pos) == 4 and all(p == (1.0, 2.0) for p in _pos),
          "; ".join(_bad) if _bad else
          f"the returned buffer must read (cycle 1, cycle 2) = (1.0, 2.0) at sample 0 and "
          f"one k-period in; at four prerolls it reads {_pos[0]}. A trim off by one whole "
          f"k-period is the same at every preroll, so the bit-identity case above cannot "
          f"see it")

    # And the refusal, which is the only thing standing between a caller and a silent
    # measurement of the instance's uptime. Two k-periods is the minimum: the first has to
    # be LOW for `changed2(ktrig)` to have an edge to see at all, so one k-period produces
    # a scaffold where `ktrig` is high from cycle 1, `changed2` reads 0 for ever and
    # `knote` never restarts. Zeroing MIN_PREROLL, halving it, or deleting the raise all
    # left the suite green until this case existed.
    _refused = None
    try:
        scaffold(_cyc, dur=0.5, preroll=W.KSMPS / SR)
    except ValueError as _exc:
        _refused = str(_exc)
    check("a preroll too short to produce a trigger edge is REFUSED, not measured",
          _refused is not None and MIN_PREROLL >= 2.0 * W.KSMPS / SR,
          f"one k-period ({1000 * W.KSMPS / SR:.2f} ms) is refused and MIN_PREROLL is "
          f"{1000 * MIN_PREROLL:.3f} ms, at least two k-periods"
          if _refused else
          f"one k-period was accepted — `knote` would read the instance's uptime and "
          f"nobody would be told")

    # The tanpura class, and it has to be judged on the ENVELOPE. A raw-sample
    # comparison passes even when nothing in the fixture is free-running at all — the
    # carrier's own phase is not note-relative either — so this case asserted nothing
    # until it was written this way: the same body keyed to `knote` differs by 0.80 in
    # raw samples and by 0.0005 in envelope.
    _lfo = ("asig     poscil 0.4, kfreq * koct1, giSine, 0\n"
            "asig     = asig * (0.5 + poscil:k(0.5, 0.27))\n")
    _lfo_knote = ("asig     poscil 0.4, kfreq * koct1, giSine, 0\n"
                  "asig     = asig * (0.5 + 0.5 * cos(6.2832 * 0.27 * knote))\n")

    def env50(y):
        k = int(0.05 * SR)
        return np.array([float(np.abs(y[i:i + k]).max())
                         for i in range(0, len(y) - k, k)])

    def phase_spread(body):
        """How much a quarter turn of instance phase changes the note's envelope."""
        a, _ = render(body, dur=2.0)
        b, _ = render(body, dur=2.0, preroll=0.925)   # a quarter of the 3.70 s cycle
        if a is None or b is None:
            return None
        ea, eb = env50(a), env50(b)
        n = min(len(ea), len(eb))
        return float(np.abs(ea[:n] - eb[:n]).max())
    free, keyed = phase_spread(_lfo), phase_spread(_lfo_knote)
    check("a free-running LFO does NOT reset at the note — this is the tanpura class",
          free is not None and keyed is not None and free > 0.05 and keyed < 0.01,
          "one render failed" if free is None or keyed is None else
          f"a quarter turn of preroll moves the free-running body's envelope by "
          f"{free:.4f} and the same body keyed to `knote` by {keyed:.4f} — the first is "
          f"what the plugin's arbitrary uptime does, the second is what it must not do")

    print("colour orders as it must")
    ys, _ = render(_SINE)
    yw, _ = render(_SAW)
    check("sine is darker than saw", centroid(ys) < centroid(yw),
          f"{centroid(ys):.0f} < {centroid(yw):.0f} Hz")

    print("movement is visible, and stillness is not mistaken for it")
    ymov, emov = render("""asaw    vco2 0.5, kfreq * koct1, 0
kcut    expon 300, 4.0, 8000
asig    tone asaw, kcut""")
    if emov:
        check("a sweeping body renders", False, emov)
    else:
        _, span_mov = travel(ymov)
        _, span_still = travel(ys)
        # 267 Hz, not the 2274 the amplitude-weighted centroid read on the same
        # signal. A saw's POWER sits at its fundamental, so opening a one-pole
        # lowpass from 300 Hz to 8 kHz is a large audible change that moves the power
        # centroid only modestly — power weighting is conservative here, which is the
        # price of its not being fooled by inaudible energy. What is asserted is the
        # discrimination against a standing sine's 0.00 Hz, plus the strong case
        # below where the swept band IS the energy.
        check("a sweep travels", span_mov > 200, f"{span_mov:.0f} Hz")
        check("a standing sine does not", span_still < 20, f"{span_still:.0f} Hz")

    # The trap the span alone walks into: an unmodulated noise source. Its centroid
    # wanders more than most moving instruments do, so any test that reads only the
    # span certifies movement on a body that has none. Both readings are asserted
    # here, on the same signal, because it is their DISAGREEMENT that is the meter.
    print("movement is told apart from noise variance")
    yn2, en2 = render(_NOISE)
    ylfo, elfo = render("""anz     rand 0.4
kcut    poscil 1500, 0.7
asig    reson anz, 2200 + kcut, 900, 2""")
    if en2 or elfo:
        check("the movement calibration renders", False, en2 or elfo)
    else:
        m_n, sp_n, r_n = moves(yn2)
        m_l, sp_l, r_l = moves(ylfo)
        m_s, sp_s, r_s = moves(ys)
        m_m, sp_m, r_m = moves(ymov)
        check("static noise travels but does NOT move", not m_n,
              f"span {sp_n:.0f} Hz, coherence {r_n:+.2f}")
        check("the same noise under an LFO does move", m_l,
              f"span {sp_l:.0f} Hz, coherence {r_l:+.2f}")
        check("a swept saw moves", m_m, f"span {sp_m:.0f} Hz, coherence {r_m:+.2f}")
        check("a standing sine does not move", not m_s,
              f"span {sp_s:.0f} Hz, coherence {r_s:+.2f}")

    # …and the size of that trap, because a second route to a movement verdict was
    # opened for the event-texture class (BJ 2026-07-25: a findable RATE is the right
    # definition for a sustained tonal body and the wrong one for a stochastic texture,
    # "where being alive means precisely that there is no rate to find"). That route
    # cannot rest on the span or the crest without knowing what a bed that does NOTHING
    # reads on them. A narrowband filter on white noise is the worst case for both, and
    # it is not a contrived one: half this library's beds are exactly that shape.
    print("the stationary-noise null is what the texture route has to clear")
    worst_sp, worst_cr, where = 0.0, 0.0, ""
    for ctr, wid in ((300, 200), (220, 40), (3000, 100)):
        for seed in (0.11, 0.32):
            yq, eq = render(f"""anz     rand 1.0, {seed}, 1
afl     reson anz, {ctr}, {wid}, 2
asig    = afl * 0.30""")
            if eq:
                check(f"the null renders at {ctr}/{wid}", False, eq)
                continue
            spc, _ = travel_cents(yq)
            cr = crest_db(yq)
            if spc > worst_sp:
                worst_sp, where = spc, f"{ctr}/{wid}"
            worst_cr = max(worst_cr, cr or 0.0)
    check("a bed that does nothing still reads a huge colour travel",
          worst_sp > 600.0, f"{worst_sp:.0f} cents at reson {where}")
    check("...and a large crest with it", worst_cr > 10.0, f"{worst_cr:.2f} dB")
    check("both stay under the recorded nulls",
          worst_sp <= STATIONARY_SPAN_NULL_CENTS + 60
          and worst_cr <= STATIONARY_CREST_NULL_DB + 1.0,
          f"span {worst_sp:.0f} <= {STATIONARY_SPAN_NULL_CENTS:.0f} cents, "
          f"crest {worst_cr:.2f} <= {STATIONARY_CREST_NULL_DB:.2f} dB")
    # THE NEGATIVE RESULT, asserted so no later session re-invents the threshold I
    # tried here first: the span cannot carry the texture route either. A REAL sweep —
    # the positive control two blocks up, which `moves` passes at coherence 1.00 —
    # travels LESS in cents than a stationary narrowband bed does. The two populations
    # overlap, so no span bound separates them, in either direction.
    #
    # That is why the texture class in `lco_axis_probe` is a DECLARED, by-ear exemption
    # and not a second measurement. Coherence is the only statistic here that separates
    # movement from variance, dropping it leaves nothing to put in its place, and the
    # numbers printed beside a declared texture are context, never a pass-stamp. If a
    # future session wants a mechanical criterion for this class it has to come from a
    # statistic not in this file — the honest starting point is that the four beds
    # already shipped (`rain`, `crackle`, `thunder`, `cymbal`) sit inside the null.
    #
    # It is asserted against `worst_sp` — the bed measured THREE LINES UP, in this same
    # run — and not against the frozen 1005.0 literal. Against the literal the case can
    # pass while its own claim is false: at a bed of 620 cents and a sweep of 959 all
    # four predicates here still read True, and yet a bound at 800 cents would separate
    # the two populations cleanly. A case that can certify the opposite of its name is
    # worse than no case.
    if emov:
        check("no span bound separates a real sweep from a static bed", False, emov)
    else:
        sp_sweep, _ = travel_cents(ymov)
        check("no span bound separates a real sweep from a static bed",
              sp_sweep < worst_sp,
              f"a real sweep travels {sp_sweep:.0f} cents, and a bed that does nothing "
              f"{worst_sp:.0f} — measured in this run, not read off the recorded "
              f"{STATIONARY_SPAN_NULL_CENTS:.0f}")
    # Movement at every RATE, because the meter this replaced had a null at 8 Hz
    # (a quarter of the 32 Hz centroid-track rate) and read a beating wineglass as
    # standing still. Any single-lag statistic has that null; these three cases
    # bracket it, and a modulation rate is the last thing a candidate should have
    # to avoid.
    for rate in (0.7, 4.0, 8.0, 13.0):
        yr, er = render(f"""anz     rand 0.4
kcut    poscil 1500, {rate}
asig    reson anz, 2200 + kcut, 900, 2""")
        if er:
            check(f"movement at {rate} Hz", False, er)
            continue
        seen, sp, r1 = moves(yr)
        check(f"movement at {rate} Hz is seen", seen,
              f"span {sp:.0f} Hz, coherence {r1:+.2f}")

    # The exploit this weighting exists to close: energy nobody can hear must not be
    # able to carry a claim about whether the sound moves.
    yin, ein = render("""kt      poscil 0.5, 0.5
a1      poscil 0.4, kfreq * koct1, giSine
a2      poscil 0.0004 * (1 + kt), 18000, giSine
asig    = a1 + a2""")
    if ein:
        check("an inaudible partial cannot carry the movement verdict", False, ein)
    else:
        m_i, sp_i, r_i = moves(yin)
        check("an inaudible partial cannot carry the movement verdict", not m_i,
              f"a standing sine plus a -60 dB 18 kHz tremolo: span {sp_i:.1f} Hz, "
              f"coherence {r_i:+.2f} (amplitude-weighted it was 18.7 Hz at +1.00)")

    yrs, ers = render("""anz     rand 0.4, 0.5, 1
kcf     expon 400, 4.0, 6000
asig    reson anz, kcf, kcf * 0.3, 2""")
    if ers:
        check("a swept band on noise travels", False, ers)
    else:
        _, sp_rs = travel(yrs)
        check("a swept band on noise travels far", sp_rs > 3000, f"{sp_rs:.0f} Hz")

    print("tracking sees what follows the keyboard and what does not")
    _FIXED = "asig    poscil 0.4, 800, giSine"          # a fixed register: ignores kfreq
    _BANK = """aex     rand 0.06, 0.5, 1
a1      mode aex, kfreq * koct1 * 1.0, 40
a2      mode aex, kfreq * koct1 * 2.14, 36
a3      mode aex, kfreq * koct1 * 3.77, 32
asig    = (a1 + a2 + a3) * 0.5"""
    # A VIBRATOED saw, because every analogue-flavoured entry in the library has a
    # slow detune on it and the un-vibratoed cases above cannot see the trap it
    # sets: the smear moves the partial off the grid point. Shipped `saw` read
    # r_note 0.149 against a keyboard it follows to the cent.
    _VIB = """kvib    poscil 0.0025, 4.7
asig    vco2 0.5, kfreq * koct1 * (1 + kvib), 0"""
    # An octave below the played note — the whole reason the band reaches to -1500.
    _SUB = "asig    poscil 0.5, kfreq * koct1 * 0.5, giSine"
    for name, body, want_tracking in (("saw (band-limited)", _SAW, True),
                                      ("saw with vibrato", _VIB, True),
                                      ("a sub-octave sine", _SUB, True),
                                      ("sine", _SINE, True),
                                      ("inharmonic modal bank", _BANK, True),
                                      ("a fixed 800 Hz register", _FIXED, False)):
        ylo, elo = render(body, freq=110.0)
        yhi, ehi = render(body, freq=880.0)
        if elo or ehi:
            check(name, False, elo or ehi)
            continue
        s = tracks(ylo, yhi, 110.0, 880.0)
        check(name, (s["verdict"] == "tracks") == want_tracking,
              f"{s['verdict']} (r_note {s['r_note']} vs r_fixed {s['r_fixed']})")

    print("odd/even separates a cylinder from a cone, and reads a duty cycle")
    # |sin(pi*k*d)|/k: at d=0.5 every even harmonic is a zero of the sine.
    for duty, lo, hi in ((0.5, 40.0, 200.0), (0.35, 2.0, 12.0), (0.10, -3.0, 3.0)):
        yd, ed = render(f"asig    vco2 0.5, kfreq * koct1, 2, {duty}")
        if ed:
            check(f"duty {duty}", False, ed)
            continue
        v = odd_even_db(yd, 220.0)
        check(f"pulse at {duty:.0%} duty", lo <= v <= hi, f"{v:+.1f} dB odd/even")
    ysq, _e1 = render("asig    poscil 0.5, kfreq * koct1, giSine")
    check("a sine has no even harmonics to speak of", odd_even_db(ysq, 220.0) > 40,
          f"{odd_even_db(ysq, 220.0):+.1f} dB")

    print("the beat meter reads back the depth it was given")
    for asked in (0.0, 0.1, 0.3, 0.6):
        yb, eb = render(f"""kam     poscil {asked / 2:.4f}, 4.0
asig    poscil 0.4 * (1 + 2 * kam), kfreq * koct1, giSine""")
        if eb:
            check(f"AM depth {asked}", False, eb)
            continue
        d, r = beat(yb)
        check(f"AM depth {asked}", abs(d - asked) < 0.02 and (asked == 0 or abs(r - 4) < 0.4),
              f"read {d:.3f} at {r:.2f} Hz")
    ybt, ebt = render("""a1      poscil 0.3, kfreq * koct1, giSine
a2      poscil 0.3, kfreq * koct1 * 1.0136, giSine
asig    = a1 + a2""")
    if ebt:
        check("two detuned sines beat", False, ebt)
    else:
        d, r = beat(ybt)
        # 1.36 % of 220 Hz is 2.99 Hz; the beat is at the difference frequency.
        check("two detuned sines beat at their difference", abs(r - 2.99) < 0.3 and d > 0.5,
              f"depth {d:.2f} at {r:.2f} Hz (asked 2.99)")
    # Two amplitude motions at once, the WEAKER one being the question. Searching
    # for the maximum answers about the loud one and says nothing about the other,
    # which is how a two-motion instrument reads as inert on one of its own axes.
    ytm, etm = render("""ka      poscil 0.150, 3.0
kb      poscil 0.225, 9.0
asig    poscil 0.4 * (1 + 2 * ka + 2 * kb), kfreq * koct1, giSine""")
    if etm:
        check("two beats at once", False, etm)
    else:
        dmax, rmax = beat(ytm)
        d3, _ = beat(ytm, at_hz=3.0)
        d9, _ = beat(ytm, at_hz=9.0)
        check("the strongest of two beats is the one reported by default",
              abs(rmax - 9.0) < 0.4 and abs(dmax - 0.45) < 0.03,
              f"read {dmax:.3f} at {rmax:.2f} Hz (asked 0.45 at 9 Hz)")
        check("at_hz finds the quieter beat the maximum hides",
              abs(d3 - 0.30) < 0.03 and abs(d9 - 0.45) < 0.03,
              f"3 Hz reads {d3:.3f} (asked 0.30), 9 Hz reads {d9:.3f} (asked 0.45)")
    # …and the limit of that, asserted rather than left to be rediscovered: a
    # stronger line about a hertz away IS the answer at 4 s.
    near, far = [], []
    for d, into in ((0.8, near), (1.6, far)):
        yl, el = render(f"""ka      poscil 0.225, {4.6 - d:.3f}
asig    poscil 0.4 * (1 + 2 * ka), kfreq * koct1, giSine""")
        into.append(None if el else beat(yl, at_hz=4.6)[0])
    if near[0] is None or far[0] is None:
        check("at_hz leakage", False, "render failed")
    else:
        check("at_hz reports a stronger neighbour under ~1 Hz away, and not beyond",
              near[0] > 0.25 and far[0] < 0.05,
              f"0.8 Hz away reads {near[0]:.3f}, 1.6 Hz away reads {far[0]:.3f} "
              f"(nothing is modulating 4.6 Hz in either)")
    # The case the meter got WRONG, and the reason it is asserted here rather than
    # trusted: nothing is modulating these, so every register must read zero. On a
    # 200 Hz block-averaged envelope 55 Hz read 0.372 at 90.39 Hz and 110 Hz read
    # 0.052 at 19.68 Hz — the rectification ripple at 2*f0, folded about the envelope
    # rate. M6 calls 0.05 a real rate control, so this fabricated a rate axis for
    # every entry. Read through `measure`, which is the path the audit uses.
    flat = []
    for f in (55.0, 82.5, 110.0, 220.0, 440.0, 880.0, 1760.0):
        yf, ef = render("asig    poscil 0.3, kfreq", freq=f)
        flat.append((f, None if ef else measure(yf, f)["beat_depth"]))
    worst = max((v, f) for f, v in flat if v is not None)
    check("an unmodulated tone has no beat at any register", worst[0] < 0.02,
          "worst %.3f at %.1f Hz (%s)"
          % (worst[0], worst[1],
             " ".join("%.0f:%.3f" % (f, v) for f, v in flat if v is not None)))
    # The case the cap got wrong, and the reason both of these are asserted through
    # `measure`: an entry that SOUNDS an octave below the asked note has its ripple at
    # the asked pitch, so a cap taken off the ask excludes nothing. Every calibration
    # signal above has f0 == asked, which is exactly why they could not see it —
    # `bass_saw` read 0.434 at 110.0 Hz at an asked 220 with nothing in it faster than
    # 0.043 Hz. Two octaves down, because `bagpipe` is two octaves down.
    down = []
    for f in (110.0, 220.0, 880.0):
        yd, ed = render("asig    poscil 0.3, kfreq * koct1 / 4", freq=f)
        down.append((f, None if ed else measure(yd, f)["beat_depth"]))
    wd = max((v, f) for f, v in down if v is not None)
    check("a tone two octaves below the asked note still has no beat", wd[0] < 0.02,
          "worst %.3f at an asked %.0f Hz (%s)"
          % (wd[0], wd[1],
             " ".join("%.0f:%.3f" % (f, v) for f, v in down if v is not None)))
    # …and the other side of the same cap: the octave BELOW the fundamental is where
    # a vocal hum or a growl actually lives, and a cap at half the fundamental read
    # those as nothing (asked 0.30 at 115 Hz on a 220 Hz carrier came back 0.003).
    ysub, esub = render("""kam     poscil 0.15, 115.0
asig    poscil 0.4 * (1 + 2 * kam), kfreq * koct1, giSine""", freq=220.0)
    if esub:
        check("a beat just under the fundamental reads back its depth", False, esub)
    else:
        ds, rs = measure(ysub, 220.0)["beat_depth"], measure(ysub, 220.0)["beat_rate_hz"]
        check("a beat just under the fundamental reads back its depth",
              abs(ds - 0.30) < 0.03 and abs(rs - 115.0) < 0.5,
              f"read {ds:.3f} at {rs:.2f} Hz (asked 0.30 at 115 Hz, f0 220)")
    # …and a genuine fast beat is still read at its true depth up there, which the
    # boxcar's roll-off used to cost: an asked 0.30 at 90 Hz came back as 0.200.
    yfast, efast = render("""kam     poscil 0.15, 90.0
asig    poscil 0.4 * (1 + 2 * kam), kfreq * koct1, giSine""", freq=880.0)
    if efast:
        check("a 90 Hz beat reads back its depth", False, efast)
    else:
        dfa, rfa = beat(yfast, lo_hz=0.5, hi_hz=120.0, f0_hz=880.0)
        check("a 90 Hz beat reads back its depth, well above the old envelope rate",
              abs(dfa - 0.30) < 0.03 and abs(rfa - 90.0) < 0.5,
              f"read {dfa:.3f} at {rfa:.2f} Hz (asked 0.30 at 90 Hz)")

    print("loudness travelling inside one held note")
    # The invariant §4 states and nothing measured: the colour may travel over a note,
    # the loudness may not. `overtone_voice` stepped 6.66 dB inside a held note while the
    # gate's own numbers — a mean RMS per corner — read a 0.31 dB spread and passed it.
    # The decay case is asserted because it is what makes ONE number work for a standing
    # tone and a struck one: an exponential decay is a straight line in dB, so detrending
    # removes it exactly. Before the -60 dB floor went in, a body decaying to true silence
    # read 212 and 218 dB — the meter reporting its own arithmetic.
    for label, body, dur, want in (
        ("a steady tone reads no travel",
         "asig    poscil 0.4, kfreq * koct1", 6.0, (0.0, 0.10)),
        ("a 60 dB exponential decay reads no travel",
         "kenv    expon 1.0, 4.0, 0.001\nasig    poscil 0.4 * kenv, kfreq * koct1",
         6.0, (0.0, 0.30)),
        ("a decay into silence reads no travel, not its own arithmetic",
         "kenv    expseg 1.0, 3.0, 0.0001, 0.01, 0.00001, 9.0, 0.00001\n"
         "asig    poscil 0.4 * kenv, kfreq * koct1", 12.0, (0.0, 0.50)),
        ("3.00 dB of tremolo reads back its depth",
         "ktr     poscil 0.5, 2.0\n"
         "asig    poscil 0.4 * (10 ^ ((3.0 * ktr) / 20)), kfreq * koct1",
         6.0, (2.8, 3.4)),
        ("white noise reads its own 50 ms scatter and no more",
         "asig    rand 0.3, 0.5, 1", 6.0, (0.0, 0.80)),
    ):
        yt, et = render(body, dur=dur, freq=220.0)
        if et:
            check(label, False, et)
            continue
        v = loudness_travel(yt)
        check(label, v is not None and want[0] <= v <= want[1],
              f"{'None' if v is None else f'{v:.2f} dB'} "
              f"(expected {want[0]:.2f}..{want[1]:.2f})")

    # Four of the five cases above have a lower bound of 0.00, so a meter that returned a
    # constant 0.0 would pass four of them — and 0.0 is exactly what both early exits used
    # to return. These have teeth: the monotone class must be SEEN (by the drift, since the
    # detrend removes it by design), and an unmeasurable note must say None and not 0.
    for label, body, dur, want_travel, want_drift in (
        ("a -6 dB fade over the note is invisible to the travel and plain in the drift",
         "kenv    expon 1.0, 4.0, 0.501\nasig    poscil 0.4 * kenv, kfreq * koct1",
         5.0, (0.0, 0.10), (-7.0, -5.0)),
        ("a +20 dB swell inside a held note — §4's own named violation — is seen",
         "kenv    expon 0.1, 4.0, 1.0\nasig    poscil 0.4 * kenv, kfreq * koct1",
         5.0, (0.0, 0.10), (14.0, 21.0)),
        ("a steady tone drifts nowhere, so the drift does not cry wolf",
         "asig    poscil 0.4, kfreq * koct1", 6.0, (0.0, 0.10), (-0.10, 0.10)),
        ("a note too short to read says None and does not pass itself off as 0.00",
         "kt      poscil 0.7500, 4.0\nasig    poscil 0.4 * (1 + kt), kfreq * koct1",
         1.1, None, None),
    ):
        yt, et = render(body, dur=dur, freq=440.0)
        if et:
            check(label, False, et)
            continue
        tv, dr = loudness_travel(yt), loudness_drift_db(yt)
        if want_travel is None:
            check(label, tv is None and dr is None,
                  f"travel {tv}, drift {dr} (expected None, None)")
            continue
        check(label, tv is not None and dr is not None
              and want_travel[0] <= tv <= want_travel[1]
              and want_drift[0] <= dr <= want_drift[1],
              f"travel {tv if tv is None else f'{tv:.2f}'} dB, "
              f"drift {dr if dr is None else f'{dr:+.2f}'} dB (expected "
              f"{want_travel[0]:.2f}..{want_travel[1]:.2f} and "
              f"{want_drift[0]:+.1f}..{want_drift[1]:+.1f})")

    # …and the span alone is not a verdict. A narrow filter fed noise has a fluctuating
    # envelope with nothing modulating it at all, and the narrower it is the larger the
    # span: Q 10 reads 5.86 dB, Q 200 reads 14.92, Q 700 reads 18.43, and the same Q 700
    # over twelve seconds instead of four reads 24.14 — so the span also grows with how
    # long the note is held. A deliberate 3 dB tremolo reads 4.88 dB on a wide band, LESS
    # than any of them. Every figure published for a mode bank before `loudness_is_the_body`
    # existed was therefore a statement about a filter's bandwidth.
    #
    # The flatness of the loudness track does NOT rescue it and was tried first: measured
    # over 66 bare resonances the null runs to 0.836, because a very narrow filter's
    # envelope decorrelates over 1/bandwidth and at Q 2000 that is one slow swell in four
    # seconds — spectrally a single low rate, indistinguishable in shape from an LFO. The
    # signal distribution reaches down to 0.000 over the same range. The two overlap
    # completely and there is no threshold to pick.
    for label, body, want_span, want_body in (
        ("a bare Q 200 resonance on noise: a large span that is NOT the body's doing",
         "aex     rand 0.02, 0.5, 1\nasig    mode aex, 880, 200", (8.0, 30.0), (-0.35, 0.35)),
        ("a bare Q 2000 resonance reads the same way, and its span is even larger",
         "aex     rand 0.02, 0.5, 1\nasig    mode aex, 880, 2000", (8.0, 30.0), (-0.35, 0.35)),
        ("a 1 Hz butterbp on noise: it is the bandwidth, not the opcode",
         "aex     rand 0.02, 0.5, 1\nasig    butterbp aex, 880, 1", (8.0, 30.0), (-0.35, 0.35)),
        ("a real tremolo reads a SMALLER span and is unmistakably the body",
         "aex     rand 0.3, 0.5, 1\nkt      poscil 0.1730, 0.7\n"
         "asig    butterbp aex * (1 + kt), 880, 400", (2.0, 8.0), (_BODY_MIN, 1.0)),
        ("a tremolo on a narrower band is caught too, with less margin",
         "aex     rand 0.3, 0.5, 1\nkt      poscil 0.4150, 2.0\n"
         "asig    butterbp aex * (1 + kt), 880, 100", (2.0, 20.0), (_BODY_MIN, 1.0)),
        ("…but 1 dB hidden under a 10 Hz band's own 13 dB of envelope is NOT caught, "
         "and the test says so rather than guessing",
         "aex     rand 0.3, 0.5, 1\nkt      poscil 0.0593, 0.3\n"
         "asig    butterbp aex * (1 + kt), 880, 10", (5.0, 30.0), (-0.35, _BODY_MIN)),
        ("a body with nothing random in it needs no measuring: whatever the level does, "
         "it does",
         "asig    poscil 0.4, kfreq * koct1", (0.0, 0.10), (1.0, 1.0)),
        ("…but one whose only noise is `dust` cannot be reseeded from here, and says "
         "None rather than guessing",
         "aex     dust 0.02, 400\nasig    mode aex, 880, 200", (2.0, 30.0), (None, None)),
    ):
        yt, et = render(body, dur=8.0, freq=880.0)
        if et:
            check(label, False, et)
            continue
        sp = loudness_travel(yt) or 0.0
        bd = loudness_is_the_body(body, freq=880.0)
        ok = want_span[0] <= sp <= want_span[1] and (
            bd is None if want_body[0] is None else
            bd is not None and want_body[0] <= bd <= want_body[1])
        check(label, ok, f"{sp:.2f} dB, the body {'None' if bd is None else f'{bd:+.3f}'} "
              f"(expected {want_span[0]:.1f}..{want_span[1]:.1f} dB, "
              f"{'None' if want_body[0] is None else f'{want_body[0]:+.2f}..{want_body[1]:+.2f}'})")

    print("event density — the axis class no spectrum can see")
    # A Poisson train at rate L has 1/sqrt(L*win) of envelope variation, so the rate
    # inverts out of it. What has to be calibrated is not only that the inversion works
    # on a bare source but that BOTH readings behave on the two cases that matter: a
    # dense continuous bed, where there is no sparse event and neither number should
    # claim one, and a sparse train buried under such a bed, where `crest_db` must still
    # see it and `event_rate_hz` must not pretend to.
    _dust = "asig    dust2 0.8 * sqrt(22 / {r}.0), {r}"
    dens = {}
    bad = False
    for rate in (22, 366, 7300):
        yd, ed = render(_dust.format(r=rate), dur=4.0)
        if ed:
            check(f"the density calibration renders at {rate}/s", False, ed)
            bad = True
            break
        dens[rate] = (crest_db(yd), event_rate_hz(yd))
    if not bad:
        check("a sparse impulse train has a huge crest, a dense one nearly none",
              dens[22][0] > 30.0 > dens[7300][0] > 6.0
              and dens[22][0] > dens[366][0] > dens[7300][0],
              " -> ".join(f"{dens[r][0]:.1f} dB" for r in (22, 366, 7300)))
        check("the rate inverts out of the envelope to within a factor of 3",
              all(r / 3.0 < dens[r][1] < r * 3.0 for r in dens),
              ", ".join(f"{r}/s reads {dens[r][1]:.0f}" for r in (22, 366, 7300)))
        yw, ew = render(_NOISE, dur=4.0)
        if ew:
            check("the dense-bed density case renders", False, ew)
        else:
            check("continuous noise has no sparse event to find",
                  crest_db(yw) < 20.0 and event_rate_hz(yw) > 3000.0,
                  f"crest {crest_db(yw):.1f} dB, rate {event_rate_hz(yw):.0f}/s")
        # 22 pops a second, 18 dB under a continuous bed — `crackle`'s own situation.
        ym, em = render("anz0    rand 1.0, 0.5, 1\n"
                        "acrk0   dust2 0.8, 22\n"
                        "apop0   reson acrk0, 1600, 1200, 2\n"
                        "asig    = apop0 * 1.90 + anz0 * 0.30", dur=4.0)
        if em:
            check("the buried-train density case renders", False, em)
        else:
            check("a sparse train stays visible in the crest when buried under a bed, "
                  "and the rate does NOT see it — which is why both are reported",
                  crest_db(ym) > crest_db(yw) + 2.0 and event_rate_hz(ym) > 3000.0,
                  f"crest {crest_db(ym):.1f} dB against the bare bed's "
                  f"{crest_db(yw):.1f}, rate {event_rate_hz(ym):.0f}/s against "
                  f"{event_rate_hz(yw):.0f} — the rate is swamped, the crest is not")

    print("comb contrast tracks the resonator, not the exciter")
    (yn, en), (yh, eh), (yl, el) = (render(_NOISE), render(_comb(0.995)),
                                    render(_comb(0.575)))
    if en or eh or el:
        check("comb calibration renders cleanly", False, en or eh or el)
    else:
        cn, ch, cl = (comb_contrast(v, 220.0) for v in (yn, yh, yl))
        check("white noise is uncombed", abs(cn) < 2.0, f"{cn:+.1f} dB")
        check("high feedback combs hard", ch > 25, f"{ch:.1f} dB")
        check("low feedback combs less", 4 < cl < ch - 10, f"{cl:.1f} dB")

    # …and it has to be read on the grid the SIGNAL uses. A body sounding an octave
    # below the note it was given puts a real harmonic in every one of the valleys the
    # ask would look at — 330 and 550 are the 6th and 10th of 55 — so the comb reads as
    # shallow on a body that is combed hard. Eight library entries sound off the ask
    # and four read wrong by 52 to 85 dB.
    yo, eo = render("asig    poscil 0.5, kfreq * koct1 * 0.5", dur=4.0, freq=220.0)
    yd, ed = render("aex     rand 0.02, 0.5, 1\n"
                    "asig    streson aex, kfreq * koct1 * 0.5, 0.995", dur=4.0, freq=220.0)
    if eo or ed:
        check("the octave-down comb calibration renders cleanly", False, eo or ed)
    else:
        r = measure(yd, 220.0)
        wrong = comb_contrast(yd, 220.0)
        check("a body an octave below the note is combed on ITS pitch, not the ask",
              r["comb_db"] > wrong + 10.0,
              f"{r['comb_db']:.1f} dB on its own f0 {r['f0']}, {wrong:.1f} dB read "
              f"against the asked 220 — the ask must not be used when there is an f0")

    # A calibration that silently stops running cases is as bad as one that fails:
    # the count is part of the verdict, not decoration.
    # `!=`, not `<`. With `<` the constant could sit one below the real count and a
    # deleted case would then run 81 of 81 and print a green suite — which it did.
    # A case ADDED without updating the constant is equally a bookkeeping error, and
    # the whole point of the constant is that the count cannot drift unnoticed.
    if len(total) != _CASES:
        fails.append(f"{len(total)} calibration cases ran, and _CASES says {_CASES}")
        print(f"  FAIL  the calibration count drifted: {len(total)} ran, "
              f"_CASES = {_CASES}")
    print(f"\n{len(total) - len(fails)}/{len(total)} cases pass")
    if fails:
        print("METER IS WRONG — do not trust it. Failed: " + ", ".join(fails))
    else:
        print("meter calibrated")
    ok = not fails
    return 0 if ok else 1


# ─────────────────────────────────────────────────────────────────────────────

def _library_body(key, anchor):
    lib = json.loads((REPO / "backend" / "lco_library.json").read_text())
    for inst in lib["instruments"]:
        if inst["key"] != key:
            continue
        if anchor:
            # The keys carry their gloss ("bow=1.0  (bowed: drawn with a bow…)"),
            # so match on any substring of one: `--anchor bowed` is the useful form.
            variants = inst.get("anchor_code") or {}
            hits = [k for k in variants if anchor.lower() in k.lower()]
            if len(hits) != 1:
                raise SystemExit(f"{key}: {anchor!r} matches {len(hits)} anchors."
                                 + "".join("\n  " + k for k in sorted(variants)))
            return variants[hits[0]]
        return inst["code"]
    raise SystemExit(f"no instrument {key!r} in backend/lco_library.json")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--selftest", action="store_true",
                    help="calibrate the meter on signals with known answers")
    ap.add_argument("--key", help="library instrument key to render")
    ap.add_argument("--anchor", help="anchor variant, e.g. bow=bowed")
    ap.add_argument("--body", help="path to a body to render instead")
    ap.add_argument("--freq", type=float, default=220.0)
    ap.add_argument("--dur", type=float, default=4.0)
    ap.add_argument("--glide", help="f0:f1 — sweep the note's pitch")
    ap.add_argument("--dump", action="store_true", help="print the CSD and stop")
    ap.add_argument("--wav", help="keep the render here")
    args = ap.parse_args()

    if args.selftest:
        return selftest()
    if not (args.key or args.body):
        ap.error("--selftest, --key or --body")

    body = (Path(args.body).read_text() if args.body
            else _library_body(args.key, args.anchor))
    glide = tuple(float(x) for x in args.glide.split(":")) if args.glide else None

    if args.dump:
        print(scaffold(body, args.dur, args.freq, glide))
        return 0

    y, err = render(body, args.dur, args.freq, glide, keep=args.wav)
    if y is None:
        print(f"{args.key or args.body}: {err}", file=sys.stderr)
        return 1
    out = measure(y, args.freq)
    if err:
        out["warning"] = err
    # On stderr, so a caller piping the JSON still gets clean JSON and a person
    # reading the terminal cannot take the numbers without the limits.
    print(LIMITS, file=sys.stderr)
    print(json.dumps(out, indent=1))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
