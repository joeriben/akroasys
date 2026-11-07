#!/usr/bin/env python3
"""Turn the c3 study's per-clip results into the report's tables (Markdown on stdout).

    ~/.cache/t5ynth-pitch-venv/bin/python tools/pitch_c3_tables.py [matrix|agreement|seed|sweep|regen|bank|phrasing|residual|all]

A clip counts as pitched in the main tables only when pYIN and CREPE both call it
stable and agree within 50 cents ("consensus"); one estimator alone never decides.
"""

from __future__ import annotations

import json
import sys
from collections import Counter, defaultdict
from math import sqrt
from pathlib import Path

import numpy as np

import pitch_c3_f0 as P
from pitch_c3_ipc import STUDY_DIR

PC = {"c": 0, "g": 7}
TARGET = {"none": None, "c3": 0, "c2": 0, "c4": 0, "g3": 7}
ENGINE_ORDER = ["sao-small", "sao-1.0", "sa3-small-music", "sa3-medium", "sa3-medium-sfx", "audioldm2"]


def load(subdir: str) -> list[dict]:
    root = STUDY_DIR / subdir
    rows = {}
    for line in (root / "index.jsonl").open(encoding="utf-8"):
        r = json.loads(line)
        rows[r["path"]] = r
    for est in ("pyin", "crepe", "yin"):
        f = root / f"f0_{est}.jsonl"
        if f.exists():
            for line in f.open(encoding="utf-8"):
                s = json.loads(line)
                if s["path"] in rows:
                    rows[s["path"]][est] = s
    done = [r for r in rows.values() if "pyin" in r and "crepe" in r]
    if len(done) < len(rows):
        print(f"[{subdir}: {len(rows) - len(done)} of {len(rows)} clips have no pYIN and CREPE result yet "
              "and are left out]", file=sys.stderr)
    return done


def consensus(r: dict, voiced=P.STABLE_VOICED, conc=P.STABLE_CONCENTRATION):
    """MIDI pitch when pYIN and CREPE both call the clip stable and agree, else None."""
    a, b = r["pyin"], r["crepe"]
    if not (P.is_stable(a, voiced, conc) and P.is_stable(b, voiced, conc)):
        return None
    if abs(a["dominant_midi"] - b["dominant_midi"]) > 0.5:
        return None
    return a["dominant_midi"]   # pYIN: CREPE decodes to 20-cent bins (calibration: ~6 c)


def wilson(k: int, n: int) -> tuple[float, float]:
    if n == 0:
        return 0.0, 0.0
    z, p = 1.96, k / n
    d = 1 + z * z / n
    c = p + z * z / (2 * n)
    h = z * sqrt(p * (1 - p) / n + z * z / (4 * n * n))
    return max(0.0, (c - h) / d), min(1.0, (c + h) / d)


def on_class(midi: float, pc: int) -> bool:
    return abs(P.cents_to_pitch_class(midi, pc)) <= 50.0 + 100.0 * P.GRID_SLACK


def pct(k, n):
    return f"{100.0 * k / n:.0f} %" if n else "–"


def octave_of(midi: float) -> int:
    return P.nearest_note(midi) // 12 - 1


def table_matrix(rows):
    rows = [r for r in rows if r["group"] == "pitched"]
    print("### Share on the named pitch class\n")
    print("Consensus clips only. Chance is 9 %: 11 of the 120 ten-cent bins pYIN reports fall "
          "within ±50 c of one pitch class. Range = lowest and highest prompt.\n")
    print("| Engine | Suffix | Clips | No consensus f0 | Consensus | On C | 95 % CI | Range over prompts | On G |")
    print("|---|---|---:|---:|---:|---:|---:|---:|---:|")
    for e in ENGINE_ORDER:
        for c in ("none", "c3", "c2", "c4", "g3"):
            rs = [r for r in rows if r["engine"] == e and r["condition"] == c]
            if not rs:
                continue
            mids = [(r, consensus(r)) for r in rs]
            ok = [(r, m) for r, m in mids if m is not None]
            k = sum(on_class(m, 0) for _, m in ok)
            g = sum(on_class(m, 7) for _, m in ok)
            lo, hi = wilson(k, len(ok))
            per = defaultdict(list)
            for r, m in ok:
                per[r["prompt"]].append(on_class(m, 0))
            shares = [100 * np.mean(v) for v in per.values()]
            rng = f"{min(shares):.0f}–{max(shares):.0f} %" if shares else "–"
            print(f"| {e} | {c} | {len(rs)} | {pct(len(rs) - len(ok), len(rs))} | {len(ok)} | "
                  f"{pct(k, len(ok))} | {100 * lo:.0f}–{100 * hi:.0f} % | {rng} | {pct(g, len(ok))} |")
    print("\n### Octave of the clips that landed on C (scientific pitch: C4 = MIDI 60 = 261.6 Hz)\n")
    octs = [1, 2, 3, 4, 5, 6]
    print("| Engine | Suffix | On C | " + " | ".join(f"C{o}" for o in octs) + " |")
    print("|---|---|---:|" + "---:|" * len(octs))
    for e in ENGINE_ORDER:
        for c in ("none", "c2", "c3", "c4"):
            rs = [r for r in rows if r["engine"] == e and r["condition"] == c]
            if not rs:
                continue
            ms = [m for m in (consensus(r) for r in rs) if m is not None and on_class(m, 0)]
            cnt = Counter(octave_of(m) for m in ms)
            print(f"| {e} | {c} | {len(ms)} | " + " | ".join(str(cnt.get(o, 0)) for o in octs) + " |")
    print("\n### Does the number move the octave? Same prompt, same seed, c2 → c4\n")
    print("The pairs hold everything but the digit. A model that reads the digit as an octave gives +24.\n")
    print("| Engine | Pairs, both consensus | Median shift, semitones | Exactly +24 (±0.5) | Unchanged (±0.5) | Other |")
    print("|---|---:|---:|---:|---:|---:|")
    for e in ENGINE_ORDER:
        by = {}
        for r in rows:
            if r["engine"] == e and r["condition"] in ("c2", "c4"):
                by[(r["prompt"], r["seed"], r["condition"])] = consensus(r)
        d = []
        for (p, s, c), m in by.items():
            if c == "c2" and m is not None and by.get((p, s, "c4")) is not None:
                d.append(by[(p, s, "c4")] - m)
        if not d:
            continue
        d = np.array(d)
        up = int(np.sum(np.abs(d - 24) <= 0.5))
        same = int(np.sum(np.abs(d) <= 0.5))
        print(f"| {e} | {len(d)} | {np.median(d):+.1f} | {pct(up, len(d))} | {pct(same, len(d))} | "
              f"{pct(len(d) - up - same, len(d))} |")
    print("\n### Median pitch by suffix, all consensus clips\n")
    print("| Engine | none | c2 | c3 | c4 | g3 |\n|---|---:|---:|---:|---:|---:|")
    for e in ENGINE_ORDER:
        cells = []
        for c in ("none", "c2", "c3", "c4", "g3"):
            ms = [m for m in (consensus(r) for r in rows if r["engine"] == e and r["condition"] == c) if m is not None]
            cells.append(f"{np.median(ms):.1f} ({P.note_name(float(np.median(ms)))})" if ms else "–")
        if any(x != "–" for x in cells):
            print(f"| {e} | " + " | ".join(cells) + " |")
    print("\n### Per prompt, share on C, none → c3\n")
    engines = [e for e in ENGINE_ORDER if any(r["engine"] == e for r in rows)]
    print("| Prompt | " + " | ".join(engines) + " |\n|---|" + "---:|" * len(engines))
    for p in sorted({r["prompt"] for r in rows}):
        cells = []
        for e in engines:
            v = []
            for c in ("none", "c3"):
                ms = [m for m in (consensus(r) for r in rows if r["engine"] == e and r["prompt"] == p and r["condition"] == c) if m is not None]
                v.append(f"{sum(on_class(m, 0) for m in ms)}/{len(ms)}")
            cells.append(" → ".join(v))
        print(f"| {p} | " + " | ".join(cells) + " |")


def table_agreement(rows):
    print("### Agreement between estimators on generated audio\n")
    print("Clips both estimators of a pair call stable. YIN is the detector the wavetable engine ships.\n")
    print("| Engine | Pair | Clips | Both stable | Within 50 cents | Octave apart | Other |")
    print("|---|---|---:|---:|---:|---:|---:|")
    for e in ENGINE_ORDER + ["all"]:
        rs = [r for r in rows if e == "all" or r["engine"] == e]
        if not rs:
            continue
        for a, b in (("pyin", "crepe"), ("pyin", "yin"), ("crepe", "yin")):
            both = [r for r in rs if b in r and a in r and P.is_stable(r[a]) and P.is_stable(r[b])]
            same = octv = other = 0
            for r in both:
                d = (r[a]["dominant_midi"] - r[b]["dominant_midi"]) * 100
                if abs(d) <= 50:
                    same += 1
                elif any(abs(d - 1200 * m) <= 50 for m in (-3, -2, -1, 1, 2, 3)):
                    octv += 1
                else:
                    other += 1
            print(f"| {e} | {a}–{b} | {len(rs)} | {len(both)} | {pct(same, len(both))} | {pct(octv, len(both))} | {pct(other, len(both))} |")


def table_sweep(rows):
    print("### What a voicing cut-off does: pitched prompts against prompts without a pitch\n")
    print("Share of clips called pitched. A bare control prompt has no pitch by name, so a "
          "bare control clip called pitched is a false alarm. A control prompt with `, c3` is "
          "a different matter: see the next table.\n")
    pit_s = [r for r in rows if r["group"] == "pitched" and r["condition"] != "none"]
    pit_b = [r for r in rows if r["group"] == "pitched" and r["condition"] == "none"]
    con_b = [r for r in rows if r["group"] == "control" and r["condition"] == "none"]
    con_s = [r for r in rows if r["group"] == "control" and r["condition"] == "c3"]
    con = con_b + con_s
    print(f"Clips: {len(pit_s)} pitched with a suffix, {len(pit_b)} pitched bare, "
          f"{len(con_b)} control bare, {len(con_s)} control with `, c3`.\n")
    print("| Voiced ≥ | Concentration ≥ | Pitched, suffix | Pitched, bare | Control, bare | Control, `, c3` | Shipped YIN alone: pitched, suffix | Shipped YIN alone: control, bare |")
    print("|---:|---:|---:|---:|---:|---:|---:|---:|")
    for v in (0.3, 0.5, 0.7, 0.9):
        for c in (0.3, 0.5, 0.7, 0.9):
            share = lambda rs: pct(sum(consensus(r, v, c) is not None for r in rs), len(rs))
            alone = lambda rs: pct(sum(P.is_stable(r["yin"], v, c) for r in rs if "yin" in r), len(rs))
            print(f"| {v} | {c} | {share(pit_s)} | {share(pit_b)} | {share(con_b)} | {share(con_s)} | "
                  f"{alone(pit_s)} | {alone(con_b)} |")
    print("\n### Prompts without a pitch: does `, c3` give them one?\n")
    print("Consensus clips of all engines at the provisional cut-offs; on C in brackets.\n")
    print("| Prompt | Clips per suffix | bare | `, c3` |\n|---|---:|---:|---:|")
    for p in sorted({r["prompt"] for r in con}):
        cells = []
        for c in ("none", "c3"):
            ms = [consensus(r) for r in con if r["prompt"] == p and r["condition"] == c]
            ok = [m for m in ms if m is not None]
            cells.append(f"{len(ok)} ({sum(on_class(m, 0) for m in ok)})")
        n = sum(1 for r in con if r["prompt"] == p and r["condition"] == "none")
        print(f"| {p} | {n} | {cells[0]} | {cells[1]} |")


def table_shipped(rows):
    print("### Could the detector the product already ships decide alone?\n")
    print("Shipped YIN against the two published estimators, all clips of the matrix. Reference: a "
          "clip counts as pitched when pYIN and CREPE agree on it. Two gates. Shipped rule: what "
          "`prepareFramesFromBuffer` does, a pitch from two confident windows on. Cut-offs: the same "
          "pitch, kept only with at least half of ALL windows confident and half of those within "
          "50 cents of it. Note that pYIN grew out of YIN, so their agreement is no independent proof; "
          "CREPE is the independent one.\n")
    print("| Engine | Gate | Clips | Agreed pitched | Gives a pitch on those | Same note | Octave off | Other | Gives a pitch where they do not agree |")
    print("|---|---|---:|---:|---:|---:|---:|---:|---:|")
    gates = (("shipped rule", lambda s: s["dominant_midi"] is not None), ("cut-offs", P.is_stable))
    for e in ENGINE_ORDER + ["all"]:
        rs = [r for r in rows if "yin" in r and (e == "all" or r["engine"] == e)]
        if not rs:
            continue
        ref = [(r, consensus(r)) for r in rs]
        pos = [(r, m) for r, m in ref if m is not None]
        neg = [r for r, m in ref if m is None]
        for name, gate in gates:
            got = [(r, m) for r, m in pos if gate(r["yin"])]
            same = octv = other = 0
            for r, m in got:
                d = (r["yin"]["dominant_midi"] - m) * 100
                if abs(d) <= 50:
                    same += 1
                elif any(abs(d - 1200 * k) <= 50 for k in (-3, -2, -1, 1, 2, 3)):
                    octv += 1
                else:
                    other += 1
            fa = sum(gate(r["yin"]) for r in neg)
            print(f"| {e} | {name} | {len(rs)} | {len(pos)} | {pct(len(got), len(pos))} | {pct(same, len(got))} | "
                  f"{pct(octv, len(got))} | {pct(other, len(got))} | {pct(fa, len(neg))} |")
    bare = [r for r in rows if r["group"] == "control" and r["condition"] == "none" and "yin" in r]
    print(f"\nOn the {len(bare)} bare control clips, prompts without a pitch: shipped rule gives a pitch on "
          f"{sum(r['yin']['dominant_midi'] is not None for r in bare)}, the cut-offs on "
          f"{sum(P.is_stable(r['yin']) for r in bare)}, the agreement of pYIN and CREPE on "
          f"{sum(consensus(r) is not None for r in bare)}.")


def table_seed(rows):
    rows = [r for r in rows if r["group"] == "pitched"]
    print("### From one seed to the next: octave under `, c3`\n")
    print("Per prompt, over ALL clips of the prompt, so a clip off C or without an agreed pitch counts "
          "as a miss. Same octave twice: two different seeds drawn from the prompt's clips, the chance "
          "that both are on C in one octave, averaged over prompts. Of those known: the same, among "
          "pairs where both clips are on C. Tries: generations until one lands on C in the prompt's most "
          "frequent octave, median and worst prompt; strict counts a clip without an agreed pitch as a "
          "miss, lenient lets it pass because nothing can reject it. Never: no clip of that prompt "
          "ever qualified. Time: median generation of one clip on this Mac.\n")
    print("| Engine | Prompts | Octaves per prompt, median | Same octave twice | Of those known | Tries strict, median | Strict, worst prompt | Tries lenient, median | Lenient, worst prompt | Seconds per clip |")
    print("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for e in ENGINE_ORDER:
        same, known, strict, lenient, spread = [], [], [], [], []
        for p in sorted({r["prompt"] for r in rows if r["engine"] == e}):
            cell = [consensus(r) for r in rows if r["engine"] == e and r["prompt"] == p and r["condition"] == "c3"]
            n = len(cell)
            if n < 2:
                continue
            on = [m for m in cell if m is not None and on_class(m, 0)]
            cnt = Counter(octave_of(m) for m in on)
            k = np.array(list(cnt.values()) or [0], dtype=float)
            unknown = sum(m is None for m in cell)
            pairs = float(np.sum(k * (k - 1)))
            same.append(pairs / (n * (n - 1)))
            if len(on) >= 2:
                known.append(pairs / (len(on) * (len(on) - 1)))
            strict.append(n / k.max() if k.max() else np.inf)
            lenient.append(n / (k.max() + unknown) if k.max() + unknown else np.inf)
            if on:
                spread.append(len(cnt))
        ms_clip = [r["ms"] for r in rows if r["engine"] == e and r.get("ms")]
        if not same:
            continue
        of_known = f"{100 * np.mean(known):.0f} %" if known else "–"
        tries = lambda x: "never" if np.isinf(x) else f"{x:.1f}"
        print(f"| {e} | {len(same)} | {np.median(spread):.1f} | {100 * np.mean(same):.0f} % | {of_known} | "
              f"{tries(np.median(strict))} | {tries(np.max(strict))} | {tries(np.median(lenient))} | "
              f"{tries(np.max(lenient))} | {np.median(ms_clip) / 1000:.2f} |")


def table_phrasing(rows, base):
    print("### Other spellings of the switch word\n")
    print("Same prompts, seeds 1000 to 1007. Consensus clips; octaves of the clips on C.\n")
    octs = [1, 2, 3, 4, 5, 6]
    print("| Engine | Spelling | Clips | Consensus | On C | " + " | ".join(f"C{o}" for o in octs) + " |")
    print("|---|---|---:|---:|---:|" + "---:|" * len(octs))
    spelled = {"c3": "`, c3`", "upper": "`, C3`", "note": "`, note c3`", "letter": "`, c`", "hz": "`, 130.8 Hz`"}
    for e in ENGINE_ORDER:
        for c, label in spelled.items():
            src = base if c == "c3" else rows
            rs = [r for r in src if r["engine"] == e and r["condition"] == c and r["group"] == "pitched"
                  and 1000 <= r["seed"] < 1008]
            if not rs or not any(r["engine"] == e for r in rows):
                continue
            ms = [m for m in (consensus(r) for r in rs) if m is not None]
            on = [m for m in ms if on_class(m, 0)]
            cnt = Counter(octave_of(m) for m in on)
            print(f"| {e} | {label} | {len(rs)} | {len(ms)} | {pct(len(on), len(ms))} | "
                  + " | ".join(str(cnt.get(o, 0)) for o in octs) + " |")


def table_register(rows):
    rows = [r for r in rows if r["group"] == "pitched" and r["condition"] == "c3"]
    print("### The octave under `, c3`, prompt by prompt\n")
    engines = [e for e in ENGINE_ORDER if any(r["engine"] == e for r in rows)]
    seeds = ", ".join(f"{e} {len({r['seed'] for r in rows if r['engine'] == e})}" for e in engines)
    print(f"Consensus clips on C, as counts per octave: C2/C3/C4/C5. Seeds per cell: {seeds}. "
          "Other octaves are left out; a dash is no clip on C.\n")
    print("| Prompt | " + " | ".join(engines) + " |\n|---|" + "---:|" * len(engines))
    for p in sorted({r["prompt"] for r in rows}):
        cells = []
        for e in engines:
            ms = [m for m in (consensus(r) for r in rows if r["engine"] == e and r["prompt"] == p) if m is not None and on_class(m, 0)]
            cnt = Counter(octave_of(m) for m in ms)
            cells.append("/".join(str(cnt.get(o, 0)) for o in (2, 3, 4, 5)) if ms else "–")
        print(f"| {p} | " + " | ".join(cells) + " |")


def table_detune(rows):
    rows = [r for r in rows if r["group"] == "pitched"]
    print("### How far from an exact C the clips on C sit\n")
    print("Cents from the nearest C, signed for the median. pYIN value, so ±5 c is the grid of "
          "the estimator, not the model.\n")
    print("| Engine | Suffix | On C | Median | Median of the absolute | 95th percentile of the absolute |\n|---|---|---:|---:|---:|---:|")
    for e in ENGINE_ORDER:
        for c in ("c2", "c3", "c4"):
            ms = [m for m in (consensus(r) for r in rows if r["engine"] == e and r["condition"] == c) if m is not None and on_class(m, 0)]
            if not ms:
                continue
            d = np.array([P.cents_to_pitch_class(m, 0) for m in ms])
            print(f"| {e} | {c} | {len(d)} | {int(round(np.median(d))):+d} c | {np.median(np.abs(d)):.0f} c | {np.percentile(np.abs(d), 95):.0f} c |")
    fine = []
    for r in rows:
        m = consensus(r)
        if r["condition"] in ("c2", "c3", "c4") and m is not None and on_class(m, 0) and "yin" in r and P.is_stable(r["yin"]):
            y = r["yin"]["dominant_midi"]
            if abs(y - m) <= 0.5:
                fine.append(abs(P.cents_to_pitch_class(y, 0)))
    print(f"\nShipped YIN, which reports continuous values, on {len(fine)} of those clips: median "
          f"{np.median(fine):.1f} c from exact C, 95th percentile {np.percentile(fine, 95):.1f} c.")


def table_residual():
    print("### After the offline correction\n")
    print("Each buffer with an agreed pitch moved tape-style onto the C nearest its series' median. "
          "Residual = measured again, minus the target. A residual near a whole octave means the "
          "estimators changed their mind about the moved buffer.\n")
    corrected = load("corrected")
    print("| Regime | Buffers moved | Moved by more than 50 c | Largest move | Length factor, range | Agreed again | Residual within 25 c | Median residual |")
    print("|---|---:|---:|---:|---:|---:|---:|---:|")
    for reg in ("fixed_seed_alpha_drift", "fixed_seed_wide_drift", "random_seed", "random_seed_alpha_drift", "resynth_25", "resynth_75"):
        rs = [r for r in corrected if r["regime"] == reg]
        if not rs:
            continue
        cents = np.array([1200.0 * np.log2(r["ratio"]) for r in rs])
        again = [(consensus(r), r["target"]) for r in rs]
        again = [(m, t) for m, t in again if m is not None]
        res = np.array([(m - t) * 100.0 for m, t in again])
        ok = int(np.sum(np.abs(res) <= 25.0))
        lengths = [r["seconds_out"] / r["seconds_in"] for r in rs]
        print(f"| {reg} | {len(rs)} | {int(np.sum(np.abs(cents) > 50))} | {np.max(np.abs(cents)):.0f} c | "
              f"{min(lengths):.2f}–{max(lengths):.2f} | {len(again)} of {len(rs)} | {pct(ok, len(res))} | "
              f"{np.median(np.abs(res)):.0f} c |")


def regen_stats(ms):
    """ms: consensus pitch per regenerated buffer, None where there is none."""
    ok = [m for m in ms if m is not None]
    out = {"n": len(ms), "stable": len(ok)}
    if len(ok) >= 2:
        arr = np.array(ok)
        steps = [abs(b - a) * 100 for a, b in zip(ms, ms[1:]) if a is not None and b is not None]
        out.update({"range": (arr.max() - arr.min()) * 100, "sd": float(np.std(arr, ddof=1)) * 100,
                    "classes": len({P.nearest_note(m) % 12 for m in ok}),
                    "octaves": len({octave_of(m) for m in ok}),
                    "median_step": float(np.median(steps)) if steps else None,
                    "big_steps": sum(s >= 50 for s in steps), "steps": len(steps)})
    return out


def table_regen(rows):
    print("### Pitch of each regenerated buffer\n")
    print("Range = highest minus lowest, in cents. Step = between consecutive buffers. "
          "A note held across a regenerate moves by exactly that step.\n")
    print("| Preset | Regime | Consensus of 10 | Range | SD | Note names | Octaves | Median step | Steps ≥ 50 c |")
    print("|---|---|---:|---:|---:|---:|---:|---:|---:|")
    regimes = ["fixed_seed_alpha_drift", "fixed_seed_wide_drift", "random_seed", "random_seed_alpha_drift", "resynth_25", "resynth_75"]
    for preset in dict.fromkeys(r["preset"] for r in rows):
        for reg in regimes:
            rs = sorted([r for r in rows if r["preset"] == preset and r["regime"] == reg], key=lambda r: r["i"])
            if not rs:
                continue
            s = regen_stats([consensus(r) for r in rs])
            if "range" not in s:
                print(f"| {preset} | {reg} | {s['stable']} | – | – | – | – | – | – |")
                continue
            print(f"| {preset} | {reg} | {s['stable']} | {s['range']:.0f} | {s['sd']:.0f} | {s['classes']} | "
                  f"{s['octaves']} | {s['median_step'] if s['median_step'] is None else round(s['median_step'])} | "
                  f"{s['big_steps']}/{s['steps']} |")
    print("\n### The series, note by note\n")
    print("| Preset | Regime | Buffers (– = no consensus f0) |\n|---|---|---|")
    for preset in dict.fromkeys(r["preset"] for r in rows):
        for reg in regimes:
            rs = sorted([r for r in rows if r["preset"] == preset and r["regime"] == reg], key=lambda r: r["i"])
            if rs:
                names = [("–" if m is None else P.note_name(m) + (f"{c:+d}" if (c := int(round((m - round(m)) * 100))) else ""))
                         for m in (consensus(r) for r in rs)]
                print(f"| {preset} | {reg} | {' '.join(names)} |")


def table_bank(rows):
    prim = [r for r in rows if r["role"] == "primary"]
    print("### Stored buffers in the preset library\n")
    print("| Pitch word in the prompts | Buffers | Consensus f0 | On C | On C of consensus |\n|---|---:|---:|---:|---:|")
    groups = {"c3": lambda r: "c3" in r["pitch_words"], "none": lambda r: not r["pitch_words"]}
    for name, f in groups.items():
        rs = [r for r in prim if f(r)]
        ms = [m for m in (consensus(r) for r in rs) if m is not None]
        k = sum(on_class(m, 0) for m in ms)
        print(f"| {name} | {len(rs)} | {len(ms)} | {k} | {pct(k, len(ms))} |")
    print("\n### Pitch classes of stored buffers with c3 in a prompt, consensus only\n")
    names = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
    cnt = Counter(P.nearest_note(m) % 12 for m in (consensus(r) for r in prim if "c3" in r["pitch_words"]) if m is not None)
    print("| " + " | ".join(names) + " |\n|" + "---:|" * 12)
    print("| " + " | ".join(str(cnt.get(i, 0)) for i in range(12)) + " |")
    print("\n### Inference caches: successive generations a player kept, published presets\n")
    print("| Preset | Entries | Consensus | Range | Note names | Octaves |\n|---|---:|---:|---:|---:|---:|")
    by_path = {r["path"]: r for r in rows}
    for line in (STUDY_DIR / "bank/caches.jsonl").open(encoding="utf-8"):
        c = json.loads(line)
        if not c["published"]:
            continue
        s = regen_stats([consensus(by_path[p]) if p in by_path else None for p in c["paths"]])
        short = c["preset"].split("/", 1)[-1].removesuffix(".t5p")
        if "range" in s:
            print(f"| {short} | {s['n']} | {s['stable']} | {s['range']:.0f} | {s['classes']} | {s['octaves']} |")
        else:
            print(f"| {short} | {s['n']} | {s['stable']} | – | – | – |")


def table_bank_sweep(rows):
    prim = [r for r in rows if r["role"] == "primary"]
    pitched = [r for r in prim if r["pitch_words"]]
    bare = [r for r in prim if not r["pitch_words"]]
    print("### The cut-offs on the bank's own stored buffers\n")
    print(f"{len(pitched)} primary buffers whose prompts carry a pitch word, {len(bare)} whose prompts carry none. "
          "Share called pitched by consensus. A buffer with a word may still be a chord or a "
          "texture; a buffer without one may still hold a tone. The table shows what each "
          "cut-off would touch.\n")
    print("| Voiced ≥ | Concentration ≥ | With a pitch word | Without one | Shipped YIN alone: with | Shipped YIN alone: without |\n|---:|---:|---:|---:|---:|---:|")
    for v in (0.3, 0.5, 0.7, 0.9):
        for c in (0.3, 0.5, 0.7, 0.9):
            a = sum(consensus(r, v, c) is not None for r in pitched)
            b = sum(consensus(r, v, c) is not None for r in bare)
            ya = sum(P.is_stable(r["yin"], v, c) for r in pitched)
            yb = sum(P.is_stable(r["yin"], v, c) for r in bare)
            print(f"| {v} | {c} | {a} of {len(pitched)} | {b} of {len(bare)} | {ya} of {len(pitched)} | {yb} of {len(bare)} |")


def main():
    what = sys.argv[1] if len(sys.argv) > 1 else "all"
    if what in ("matrix", "all"):
        table_matrix(load("matrix"))
    if what in ("agreement", "all"):
        table_agreement(load("matrix"))
        table_shipped(load("matrix"))
    if what in ("seed", "all"):
        table_seed(load("matrix"))
        table_register(load("matrix"))
        table_detune(load("matrix"))
    if what in ("sweep", "all"):
        table_sweep(load("matrix"))
    if what == "residual":
        table_residual()
    if what == "phrasing":
        table_phrasing(load("phrasing"), load("matrix"))
    if what in ("regen", "all"):
        table_regen(load("regen"))
    if what in ("bank", "all"):
        table_bank(load("bank"))
        table_bank_sweep(load("bank"))


if __name__ == "__main__":
    main()
