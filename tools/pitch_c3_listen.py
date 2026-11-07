#!/usr/bin/env python3
"""Offline prototype of the pitch correction, and the listening page for BJ's ear.

    ~/.cache/t5ynth-pitch-venv/bin/python tools/pitch_c3_listen.py correct
    ~/.cache/t5ynth-pitch-venv/bin/python tools/pitch_c3_listen.py page

`correct` resamples each regenerated buffer whose pitch both estimators agree on, so that
all buffers of a series land on one pitch: the C nearest the series' median, octave
included. Tape-style: pitch and length move together, which is what an offline resample
or a per-buffer read rate would both do. A buffer without an agreed pitch stays as it is.
The corrected buffers go into STUDY_DIR/corrected and can be measured again with
pitch_c3_analyse.py.

`page` writes ~/Desktop/akroasys_pitch_c3 with WAVs and index.html. The "held note" files
are a simulation: consecutive buffers joined by an equal-power crossfade of the preset's
Regen XFade, in three versions: as generated, moved onto one pitch, and without the buffers
that missed it, which stands for generating again until the pitch fits. They are not a
recording from the synth. Nothing here touches src/ or
backend/, and the sound path has no correction.
"""

from __future__ import annotations

import html
import json
import sys
from pathlib import Path

import librosa
import numpy as np
import soundfile as sf

import pitch_c3_f0 as P
from pitch_c3_generate import load_preset
from pitch_c3_ipc import STUDY_DIR
from pitch_c3_tables import consensus, load

OUT = Path.home() / "Desktop/akroasys_pitch_c3"
REGIMES = ("random_seed_alpha_drift",)
TARGET_PC = 0   # C
HOLD_SECONDS = 2.5
SKIP_SECONDS = 0.25


def series_target(pitches: list[float]) -> float:
    """The C nearest the series' median: the pitch the correction moves every buffer to,
    octave included. A product would take a fixed target; a series needs one that is
    near where the model already was, or every buffer moves an octave for nothing."""
    median = float(np.median(pitches))
    return median - P.cents_to_pitch_class(median, TARGET_PC) / 100.0


def correct():
    rows = load("regen")
    root = STUDY_DIR / "corrected"
    root.mkdir(parents=True, exist_ok=True)
    src = STUDY_DIR / "regen"
    series: dict[tuple, list] = {}
    for r in rows:
        if r["regime"] != "saved":
            series.setdefault((r["preset"], r["regime"]), []).append(r)
    with (root / "index.jsonl").open("w", encoding="utf-8") as index:
        for (preset, regime), rs in series.items():
            known = [(r, consensus(r)) for r in rs]
            pitches = [m for _, m in known if m is not None]
            if not pitches:
                continue
            target = series_target(pitches)
            for r, m in known:
                if m is None:
                    continue
                ratio = 2.0 ** ((target - m) / 12.0)
                y, sr = sf.read(src / r["path"], dtype="float32", always_2d=True)
                z = librosa.resample(y.T, orig_sr=sr, target_sr=sr / ratio, res_type="soxr_hq").T
                out = root / r["path"]
                out.parent.mkdir(parents=True, exist_ok=True)
                sf.write(out, np.clip(z, -1, 1), sr, subtype="PCM_24", format="FLAC")
                index.write(json.dumps({"path": r["path"], "preset": preset, "regime": regime,
                                        "i": r["i"], "measured": m, "target": target,
                                        "ratio": ratio, "seconds_in": len(y) / sr,
                                        "seconds_out": len(z) / sr}) + "\n")


def level(y):
    peak = float(np.abs(y).max())
    return y * (0.7 / peak) if peak > 1e-6 else y


def held_note(buffers, sr, xfade_s):
    """Each buffer from SKIP_SECONDS for HOLD_SECONDS, joined by an equal-power crossfade."""
    n = int(HOLD_SECONDS * sr)
    x = max(1, int(xfade_s * sr))
    out = None
    for y in buffers:
        start = min(int(SKIP_SECONDS * sr), max(0, len(y) - n))
        seg = level(y[start:start + n + x])
        if out is None:
            out = seg
            continue
        m = min(x, len(seg), len(out))
        t = np.linspace(0, np.pi / 2, m)[:, None]   # the whole sweep, whatever length is left
        head = out[:-m] if m < len(out) else out[:0]
        mix = out[-m:] * np.cos(t) + seg[:m] * np.sin(t)
        out = np.concatenate([head, mix, seg[m:]])
    return out


WORD_CLIPS = [("calm waves", "sa3-medium"), ("a samba group", "sa3-medium"), ("helicopter rotor", "sao-small")]
WORD_SEEDS = (1000, 1001)


def word_section():
    """Prompts without a pitch, bare and with `, c3`: what the switch would do to them."""
    rows = load("matrix")
    folder = OUT / "word_on_unpitched"
    folder.mkdir(parents=True, exist_ok=True)
    body = ""
    for prompt, engine in WORD_CLIPS:
        for seed in WORD_SEEDS:
            cells = ""
            for cond in ("none", "c3"):
                r = next(x for x in rows if x["prompt"] == prompt and x["engine"] == engine
                         and x["condition"] == cond and x["seed"] == seed)
                y, sr = sf.read(STUDY_DIR / "matrix" / r["path"], dtype="float32", always_2d=True)
                name = f"{prompt.replace(' ', '_')}_{engine}_{seed}_{cond}.wav"
                sf.write(folder / name, y, sr, subtype="PCM_24")
                m = consensus(r)
                note = "no agreed pitch" if m is None else P.note_name(m)
                cells += (f"<td>{html.escape(note)}<audio controls preload=none "
                          f"src='word_on_unpitched/{name}'></audio></td>")
            body += f"<tr><td>{html.escape(prompt)}</td><td>{engine}</td><td>{seed}</td>{cells}</tr>"
    return ("<section><h2>What the word does to a prompt without a pitch</h2>"
            "<p class=sub>Same seed, once bare, once with <code>, c3</code>.</p><div class=scroll><table>"
            "<thead><tr><th>Prompt</th><th>Engine</th><th>Seed</th><th>Bare</th><th>With , c3</th></tr></thead>"
            f"<tbody>{body}</tbody></table></div></section>")


def page():
    rows = {r["path"]: r for r in load("regen")}
    corrected = {}
    for line in (STUDY_DIR / "corrected/index.jsonl").open(encoding="utf-8"):
        c = json.loads(line)
        corrected[c["path"]] = c
    OUT.mkdir(parents=True, exist_ok=True)
    sections = []
    presets = list(dict.fromkeys(r["preset"] for r in rows.values()))
    for preset in presets:
        meta, _ = load_preset(preset)
        xfade = float(meta.get("driftCrossfade", 200.0)) / 1000.0   # 0 is a hard switch, as stored
        for regime in REGIMES:
            rs = sorted([r for r in rows.values() if r["preset"] == preset and r["regime"] == regime],
                        key=lambda r: r["i"])
            if not rs:
                continue
            folder = OUT / preset.replace(" ", "_") / regime
            (folder / "as_generated").mkdir(parents=True, exist_ok=True)
            (folder / "corrected").mkdir(parents=True, exist_ok=True)
            plain, fixed, kept, cells = [], [], [], []
            sr = 44100
            pitches = [m for m in (consensus(r) for r in rs) if m is not None]
            target = series_target(pitches) if pitches else None
            for r in rs:
                y, sr = sf.read(STUDY_DIR / "regen" / r["path"], dtype="float32", always_2d=True)
                name = f"{r['i']:02d}.wav"
                sf.write(folder / "as_generated" / name, y, sr, subtype="PCM_24")
                plain.append(y)
                c = corrected.get(r["path"])
                z = y
                if c is not None:
                    z, _ = sf.read(STUDY_DIR / "corrected" / r["path"], dtype="float32", always_2d=True)
                sf.write(folder / "corrected" / name, z, sr, subtype="PCM_24")
                fixed.append(z)
                m = consensus(r)
                if m is None or abs(m - target) <= 0.5:
                    kept.append(y)   # an unknown pitch cannot be rejected; a re-roll lets it pass
                note = "no agreed pitch" if m is None else f"{P.note_name(m)} {(m - round(m)) * 100:+.0f} c"
                move = "unchanged" if c is None else f"moved {(c['target'] - c['measured']) * 100:+.0f} c"
                cells.append((r["i"], note, move, name))
            sf.write(folder / "held_as_generated.wav", held_note(plain, sr, xfade), sr, subtype="PCM_24")
            sf.write(folder / "held_corrected.wav", held_note(fixed, sr, xfade), sr, subtype="PCM_24")
            sf.write(folder / "held_rerolled.wav", held_note(kept, sr, xfade), sr, subtype="PCM_24")
            base = f"{preset.replace(' ', '_')}/{regime}"
            label = "A new seed each regenerate, A/B drift running" + (
                f". Target {P.note_name(target)}" if target is not None else "")
            body = "".join(
                f"<tr><td>{i + 1}</td><td>{html.escape(n)}</td><td>{html.escape(mv)}</td>"
                f"<td><audio controls preload=none src='{base}/as_generated/{f}'></audio></td>"
                f"<td><audio controls preload=none src='{base}/corrected/{f}'></audio></td></tr>"
                for i, n, mv, f in cells)
            sections.append(
                f"<section><h2>{html.escape(preset)}</h2><p class=sub>{html.escape(label)}</p>"
                f"<div class=held><div><h3>Held note, as generated</h3>"
                f"<audio controls preload=none src='{base}/held_as_generated.wav'></audio></div>"
                f"<div><h3>Moved onto one pitch</h3>"
                f"<audio controls preload=none src='{base}/held_corrected.wav'></audio></div>"
                f"<div><h3>Off-pitch buffers rolled again ({len(kept)} of {len(rs)} kept)</h3>"
                f"<audio controls preload=none src='{base}/held_rerolled.wav'></audio></div></div>"
                f"<details><summary>The ten buffers</summary><div class=scroll><table><thead><tr><th>#</th><th>Measured</th>"
                f"<th>Correction</th><th>As generated</th><th>Corrected</th></tr></thead>"
                f"<tbody>{body}</tbody></table></div></details></section>")
    sections.append(word_section())
    (OUT / "index.html").write_text(PAGE.replace("%SECTIONS%", "".join(sections)), encoding="utf-8")
    print(OUT / "index.html")


PAGE = """<!doctype html><html lang=en><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><title>Pitch on Regenerate</title>
<style>
:root{--bg:#f7f6f3;--fg:#1f1e1c;--muted:#6b6862;--line:#dcd9d2;--card:#fff}
@media (prefers-color-scheme:dark){:root{--bg:#191917;--fg:#ecebe6;--muted:#a09d95;--line:#3a3935;--card:#222220}}
body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.5 -apple-system,system-ui,sans-serif}
main{max-width:960px;margin:0 auto;padding:32px 16px 64px}
h1{font-size:26px;margin:0 0 8px}h2{font-size:19px;margin:0}h3{font-size:14px;margin:0 0 6px;font-weight:600}
.lead,.sub{color:var(--muted);margin:4px 0 16px}
section{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:18px;margin:18px 0}
.held{display:grid;grid-template-columns:repeat(auto-fit,minmax(280px,1fr));gap:16px;margin-bottom:12px}
audio{width:100%;height:36px}table{width:100%;border-collapse:collapse;font-size:14px;margin-top:10px}
th,td{padding:6px 8px;border-top:1px solid var(--line);text-align:left;vertical-align:middle}
td audio{min-width:190px}summary{cursor:pointer;color:var(--muted)}
.scroll{overflow-x:auto}
</style><main><h1>Pitch on Regenerate</h1>
<p class=lead>Three versions of one series of regenerated buffers, a new seed each time and A/B drift running.
As generated is what the model made. Moved onto one pitch resamples each buffer tape-style, octave included, so a
buffer moved up an octave also runs twice as fast. Rolled again keeps the buffers as they are and leaves out those
that missed the pitch, which is what generating again until the pitch fits would give. The held-note files join the
buffers with the preset's Regen XFade. They are an offline simulation, not a recording from the synth. A buffer
with no agreed pitch stays in all three.</p>
%SECTIONS%</main></html>"""


if __name__ == "__main__":
    {"correct": correct, "page": page}[sys.argv[1]]()
