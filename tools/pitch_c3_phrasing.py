#!/usr/bin/env python3
"""Which word should a C3 switch write? A follow-up to the matrix, decided after it.

    .venv/bin/python tools/pitch_c3_phrasing.py

The matrix fixed `, c3` because the bank uses it. A switch can write anything, so the
question is whether another spelling holds the octave better. Same prompts and the first
eight seeds of the matrix; a cell differs from the matrix's `, c3` cell by the spelling
alone. `, c` drops the digit, which says what the digit contributes at all. Audio goes
to STUDY_DIR/phrasing.
"""

from __future__ import annotations

import json
import time

from pitch_c3_generate import ENGINES, PITCHED, SEED_BASE, slug, write_clip
from pitch_c3_ipc import STUDY_DIR, Backend, base_request

VARIANTS = {"upper": ", C3", "note": ", note c3", "letter": ", c", "hz": ", 130.8 Hz"}
ENGINE_NAMES = ["sao-small", "sa3-small-music", "sa3-medium"]
SEEDS = 8


def main():
    root = STUDY_DIR / "phrasing"
    root.mkdir(parents=True, exist_ok=True)
    index = open(root / "index.jsonl", "a", encoding="utf-8")
    be = Backend(STUDY_DIR / "backend_stderr.log")
    device = be.ready["default"]
    for name in ENGINE_NAMES:
        model, track = ENGINES[name]
        t0, done = time.time(), 0
        for prompt in PITCHED:
            for cond, suffix in VARIANTS.items():
                for s in range(SEEDS):
                    path = root / name / slug(prompt) / f"{cond}__s{SEED_BASE + s}.flac"
                    if path.exists():
                        continue
                    audio, sr, seed, ms = be.request(
                        base_request(model, device, prompt + suffix, seed=SEED_BASE + s, track_type=track))
                    peak = write_clip(path, audio, sr)
                    index.write(json.dumps({
                        "engine": name, "model": model, "track_type": track, "group": "pitched",
                        "prompt": prompt, "condition": cond, "text": prompt + suffix, "seed": seed,
                        "path": str(path.relative_to(root)), "peak": peak, "ms": ms,
                        "samples": audio.shape[1]}, ensure_ascii=False) + "\n")
                    index.flush()
                    done += 1
        print(f"{name}: {done} new in {time.time() - t0:.0f} s", flush=True)
    be.close()


if __name__ == "__main__":
    main()
