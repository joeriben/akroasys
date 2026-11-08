#!/usr/bin/env python3
"""A/B: a waveguide bowed string with a loss filter in Hz and a body, against the
library's shipped "bowed" string.

BJ, 2026-11-07, on the analysis in docs/plans/LRO_BOWED_STRING_ANALYSIS.md: the
shipped `string` at bow=1.0 is `dust` into `streson` — random impulses, no bow —
and the one real bow model in this Csound (`wgbow`, STK 1995) runs 1.6–1.9
octaves too bright at the plugin's 4x rate because its loss filter is specified
relative to the sample rate. 2026-11-08: "bau das so" — build the recommended
probe: the bow at orchestra level, with the loss filter in Hz.

METHOD AND SOURCE, written before the first orchestra line (CLAUDE.md,
Instrument Authoring rule 1):

  METHOD  digital waveguide bowed string in VELOCITY waves. The bow splits the
          string into two sections; each is one delay line carrying the round
          trip bow -> termination -> bow. The nut reflects with -1, the bridge
          with -1 through a first-order lowpass that consolidates all losses
          ("a single spring in parallel with a frequency-independent resistance",
          Smith, PASP, Digital Waveguide Bowed-String). The bow is a memoryless
          nonlinear two-port: v_delta = v_bow - (v_in_left + v_in_right), and
          each outgoing wave is the OPPOSITE incoming wave plus rho(v_delta) *
          v_delta (Smith, PASP, The Bow-String Scattering Junction). rho is the
          reflection coefficient of the bow-string friction: flat (stuck) around
          zero, falling off (slipping) outside -- Cook's hyperbolic form
          rho = min(1, (|v_delta * slope| + 0.75)^-4), slope = bow force.
          Bow position is the split of the period between the two lines, beta
          and 1 - beta, interpolated, so it is continuous and follows kfreq.
  SOURCE  Julius O. Smith III, "Efficient Simulation of the Reed-Bore and
          Bow-String Mechanisms", Proc. ICMC 1986; Smith, Physical Audio Signal
          Processing (W3K 2010), sections "Bowed Strings", "Digital Waveguide
          Bowed-String", "The Bow-String Scattering Junction"; McIntyre,
          Schumacher & Woodhouse, "On the oscillations of musical instruments",
          JASA 74(5), 1983 (the friction-curve model). The body: Jim Woodhouse,
          Euphonics 5.3, the violin's signature modes A0 272 Hz, CBR 407, B1- 462,
          B1+ 551 and the bridge hill ~2.3 kHz, as a `mode` bank (numbers only;
          Q values are this entry's, by ear -- the source does not give them).
  WHAT IS DIFFERENT FROM `wgbow`  the loss filter is a `tone` with its cutoff in
          Hz, so the string loses the same top per round trip at 44100 and at
          176400; the body is the measured violin modes instead of one biquad
          at 500 Hz; bow velocity, force and position are k-rate inputs with no
          built-in envelope; the whole thing is readable Csound the author can
          see and vary. What is the SAME: the friction curve. Whether that
          curve is already enough once it is not running 1.6-1.9 octaves
          bright is exactly what this probe asks.
  NOT CLAIMED  no bow-hair compliance, no finite bow width, no stiffness
          dispersion, no torsion, no thermal or elasto-plastic friction
          (hysteresis neglected -- Smith says so of this table). Those are
          route C in the analysis.

The UDO `t5bow` lives in the orchestra head (`lco_write._HEAD`), beside the
ftgens: a substrate idiom like `wgbow`, available to every body, so the library
entry stays short and the author does not have to reproduce forty lines of
delay-line code to use it.

Three sides: the incumbent, the new body at its defaults (a violin-sized box), and
the new body with `box` at a cello's 0.37, all at A2 / A3 / A4.

RULE 4 -- comparison first, code second: `--side nearest_existing` is rendered
before the new body exists. The incumbent is `string`'s shipped `bow=1.0`
anchor, verbatim from the built library. Rendered at 176400 (the plugin's
default 4x), 4 s notes after a 0.5 s preroll, levelled to the incumbent per
pitch and then one gain over all files. The SA3 references BJ accepted
("sehr gut, es flattert nur etwas") are copied in beside them.

    .venv/bin/python tools/lco_ab_bowed_waveguide.py --side nearest_existing
    .venv/bin/python tools/lco_ab_bowed_waveguide.py --side new
    .venv/bin/python tools/lco_ab_bowed_waveguide.py --side new_cello
"""

from __future__ import annotations

import argparse
import json
import shutil
import sys
from pathlib import Path

import numpy as np
import soundfile as sf

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools"))
sys.path.insert(0, str(REPO / "backend"))
import lco_measure as M  # noqa: E402

OUT = REPO / "tools" / "lco_ab_bowed_waveguide"
REFS = REPO / "tools" / "lco_reference_out" / "string"
NOTES = [("A2", 110.0), ("A3", 220.0), ("A4", 440.0)]
DUR = 4.0
PREROLL = 0.5
RATE = 176400
SIDES = ("nearest_existing", "new", "new_cello")


def incumbent() -> str:
    lib = json.loads((REPO / "backend" / "lco_library.json").read_text())
    e = [x for x in lib["instruments"] if x["key"] == "string"][0]
    for label, code in e["anchor_code"].items():
        if label.split()[0] == "bow=1.0":
            return code
    raise SystemExit("the library no longer carries `string`'s bow=1.0 anchor")


def challenger(side: str = "new") -> str:
    """The new entry's body, verbatim from the built library once it is there;
    until then the file kept beside the renders. `new_cello` is the same body with
    `box` at 0.37, where the four signature modes sit at a cello's frequencies --
    the setting a cello prompt would get, so that the A2/A3 renders can be heard
    against the SA3 cello and not only against the incumbent."""
    lib = json.loads((REPO / "backend" / "lco_library.json").read_text())
    body = next((x["code"] for x in lib["instruments"] if x["key"] == "bowed_string"),
                None) or (OUT / "new_body.orc").read_text()
    if side == "new_cello":
        import lco_axis_probe as P
        body = P.with_axis(body, "box", 0.37)
    return body


def render(side: str) -> None:
    M.SR = RATE
    body = incumbent() if side == "nearest_existing" else challenger(side)
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / f"{side}_body.orc").write_text(body)
    raw = {}
    for name, f in NOTES:
        y, err = M.render(body, dur=DUR, freq=f, preroll=PREROLL)
        if err:
            raise SystemExit(f"{name}: {err}")
        raw[name] = y
        print(f"{side} {name} {f:6.1f} Hz  rms={M.rms_db(y):7.2f} dB  "
              f"peak={float(np.max(np.abs(y))):.3f}  "
              f"f0={M.f0(y):8.2f} ({M.cents(M.f0(y), f):+6.1f} c)  "
              f"comb={M.comb_contrast(y, f):6.2f}  cent={M.centroid(y):8.1f}  "
              f"travel={M.loudness_travel(y):5.2f} dB")
    np.savez(OUT / f"{side}.npz", **raw)


def write_wavs() -> None:
    sides = {s: np.load(OUT / f"{s}.npz") for s in SIDES
             if (OUT / f"{s}.npz").exists()}
    out, matched = {}, {}
    for name, f in NOTES:
        a = sides["nearest_existing"][name]
        rms_a = float(np.sqrt(np.mean(a * a)))
        for s in sides:
            b = sides[s][name]
            g = rms_a / float(np.sqrt(np.mean(b * b)))
            out[(s, name, f)] = b * g
            matched.setdefault(s, {})[name] = round(20 * float(np.log10(g)), 2)
    for s, per in matched.items():
        if s != "nearest_existing":
            print(f"{s}: matched to the incumbent by "
                  + ", ".join(f"{n} {d:+.2f} dB" for n, d in per.items()))
    top = max(float(np.max(np.abs(y))) for y in out.values())
    g = min(1.0, 0.98 / top)
    print(f"one gain over all {len(out)} files: {20 * np.log10(g):+.2f} dB "
          f"(loudest peak was {top:.3f})")
    for (side, name, f), y in out.items():
        sf.write(OUT / f"{side}__{name}_{int(f)}hz.wav", y * g, RATE,
                 subtype="PCM_24")
    (OUT / "levels.json").write_text(json.dumps(
        {"rate": RATE, "preroll_s": PREROLL, "seconds": DUR,
         "match_db": matched,
         "common_gain_db": round(20 * float(np.log10(g)), 2)}, indent=1))
    for ref in sorted(REFS.glob("*_sa3_seed*.wav")):
        shutil.copy2(ref, OUT / f"reference__{ref.name}")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--side", required=True, choices=SIDES)
    side = ap.parse_args().side
    render(side)
    if all((OUT / f"{s}.npz").exists() for s in SIDES):
        write_wavs()


if __name__ == "__main__":
    main()
