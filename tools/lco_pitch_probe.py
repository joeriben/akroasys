#!/usr/bin/env python3
"""Does this body play the note it is asked for? Three measurements per register and
engine rate, on any lexicon body, through the real orchestra head.

A physical model compiles cleanly and plays the WRONG note in three distinct ways,
none of which `lco_axis_probe --gate` looks for (it measures loudness, movement and
clipping, not pitch):

  cents    where the first partial sits against the note asked for. A resonator must
           land on it exactly when nothing nonlinear drives it; a driven model may sit
           off by its own nature (the bowed string catches the smeared Helmholtz corner
           early and plays +4 c at 440, +13 c at 1320). The former is corrected by
           derivation from the model's own mathematics, the latter is DECLARED in the
           entry, with the numbers. Either way it is measured, never assumed.
  capture  how far the partial at 2f or 3f stands ABOVE the one at f, in dB. A model
           that has locked onto a higher mode (the bowed string below Schelleng's
           minimum force; `wgflute` below 220 Hz, over-blowing) shows this large AND
           a `cents` value that is noise, because there is no partial near f left to
           measure. A BODY resonance can also lift the second or third partial above
           the fundamental while the string still plays f: the bowed string at 880 Hz
           reads +6 dB here because its bridge hill sits at 2.3 kHz, and its `cents`
           column still says +6 c. Read the two columns together; the flag only says
           that 2f or 3f is the strongest.
  sub      period doubling: energy at f/2 relative to f, in dB. A clean note sits well
           below -50 dB; the bowed string crosses -20 dB at bow place >= 0.22 or at
           heavy weight. The parameter RANGES of an entry are set where this begins,
           with margin.

Peaks are parabolic-interpolated maxima of a Hann-windowed FFT within +-6 % of each
target, in the window t0..t1 of the note (after the attack has settled). Both engine
rates are measured by default: 44100 and 176400 (the plugin's 4x rate), because an
sr-relative coefficient anywhere in the body makes two different instruments of it.

    .venv/bin/python tools/lco_pitch_probe.py --body bowed_string
    .venv/bin/python tools/lco_pitch_probe.py --body tools/lco_ab_bowed_waveguide/new_body.orc \
        --freqs 55 110 220 440 880 1320 2500 --rates 44100 176400
    .venv/bin/python tools/lco_pitch_probe.py --body bowed_string --axis place=0.25

Written for the bowed string (docs/LRO_INSTRUMENT_PROCEDURE.md §5), where these
numbers decided the `place` range, dropped the `press` axis and fixed the free
string's tuning. Measurement, not judgement: the entry's declarations are written
from the table; BJ's ear is not replaced by it.
"""
import argparse
import json
import sys
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools"))
sys.path.insert(0, str(REPO / "backend"))

import lco_measure as M          # noqa: E402
import lco_axis_probe as P       # noqa: E402


def load_body(spec):
    """A lexicon key (the SHIPPED body from lco_library.json) or a path to an .orc."""
    p = Path(spec)
    if p.exists():
        return p.read_text()
    lib = json.loads((REPO / "backend" / "lco_library.json").read_text())
    for e in lib["instruments"]:
        if e["key"] == spec:
            return e["code"]
    raise SystemExit(f"no body called {spec!r}: not a file, not a key in backend/lco_library.json")


def _peak(S, fr, fc, tol=0.06):
    """(frequency, dB) of the strongest bin within +-tol of fc, parabolic-interpolated."""
    lo, hi = np.searchsorted(fr, fc * (1 - tol)), np.searchsorted(fr, fc * (1 + tol))
    if hi - lo < 3:
        return float("nan"), float("-inf")
    k = lo + int(np.argmax(S[lo:hi]))
    k = min(max(k, 1), len(S) - 2)
    a, b, c = (np.log(S[k - 1] + 1e-30), np.log(S[k] + 1e-30), np.log(S[k + 1] + 1e-30))
    den = a - 2 * b + c
    d = 0.5 * (a - c) / den if den != 0 else 0.0
    return float(fr[k] + d * (fr[1] - fr[0])), float(20 * np.log10(S[k] + 1e-30))


def measure(y, fs, f, t0, t1):
    """(cents of the first partial, capture dB, sub-harmonic dB) over y[t0..t1]."""
    seg = y[int(t0 * fs):int(t1 * fs)]
    if len(seg) < 64:
        return float("nan"), float("nan"), float("nan")
    seg = seg * np.hanning(len(seg))
    n = 1 << (int(np.ceil(np.log2(len(seg)))) + 2)
    S = np.abs(np.fft.rfft(seg, n))
    fr = np.fft.rfftfreq(n, 1 / fs)
    f1, m1 = _peak(S, fr, f)
    _, m2 = _peak(S, fr, 2 * f)
    _, m3 = _peak(S, fr, 3 * f)
    _, mh = _peak(S, fr, 0.5 * f)
    cents = 1200 * np.log2(f1 / f) if f1 > 0 else float("nan")
    return float(cents), float(max(m2, m3) - m1), float(mh - m1)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--body", required=True, help="lexicon key or path to an .orc body")
    ap.add_argument("--axis", action="append", default=[], metavar="NAME=VALUE",
                    help="set an axis before measuring (repeatable), via lco_axis_probe.with_axis")
    ap.add_argument("--freqs", type=float, nargs="+",
                    default=[55, 110, 220, 440, 880, 1320, 1760, 2500])
    ap.add_argument("--rates", type=int, nargs="+", default=[44100, 176400])
    ap.add_argument("--dur", type=float, default=3.0)
    ap.add_argument("--preroll", type=float, default=0.5)
    ap.add_argument("--t0", type=float, default=0.8, help="analysis window start, s into the note")
    ap.add_argument("--t1", type=float, default=2.9, help="analysis window end, s into the note")
    ap.add_argument("--capture-db", type=float, default=6.0,
                    help="flag a register as captured when 2f/3f stand this far above f")
    ap.add_argument("--sub-db", type=float, default=-30.0,
                    help="flag period doubling when f/2 stands above this, relative to f")
    args = ap.parse_args()

    body = load_body(args.body)
    for spec in args.axis:
        name, value = spec.split("=", 1)
        body = P.with_axis(body, name.strip(), float(value))

    flags = 0
    for sr in args.rates:
        M.SR = sr
        print(f"\n=== sr {sr}   window {args.t0:.1f}..{args.t1:.1f} s of a {args.dur:.1f} s note, "
              f"preroll {args.preroll:.1f} s")
        print(f"{'Hz':>7} {'cents':>7} {'capture dB':>11} {'sub dB':>8}  flags")
        for f in args.freqs:
            y, err = M.render(body, dur=args.dur, freq=f, preroll=args.preroll)
            if err is not None:
                print(f"{f:7.0f}   render error: {err}")
                flags += 1
                continue
            y = np.asarray(y, dtype=float)
            if y.ndim > 1:
                y = y.mean(axis=1)
            c, cap, sub = measure(y, sr, f, args.t0, args.t1)
            tags = []
            if cap > args.capture_db:
                tags.append("2f/3f-ABOVE-f")
            if sub > args.sub_db:
                tags.append("PERIOD-DOUBLING")
            flags += bool(tags)
            print(f"{f:7.0f} {c:+7.1f} {cap:+11.1f} {sub:+8.1f}  {' '.join(tags)}")
    print(f"\n{flags} flagged register(s)." if flags else "\nno register flagged.")
    return 1 if flags else 0


if __name__ == "__main__":
    sys.exit(main())
