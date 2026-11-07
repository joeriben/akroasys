#!/usr/bin/env python3
"""Run one estimator over a folder of clips; append one summary line per clip.

    ~/.cache/t5ynth-pitch-venv/bin/python tools/pitch_c3_analyse.py <subdir> --estimator pyin --procs 6
    ~/.cache/t5ynth-pitch-venv/bin/python tools/pitch_c3_analyse.py <subdir> --estimator crepe --device mps
    ~/.cache/t5ynth-pitch-venv/bin/python tools/pitch_c3_analyse.py <subdir> --estimator yin

<subdir> is under STUDY_DIR and holds index.jsonl (one object with "path" per clip) and
the clips. Results go to <subdir>/f0_<estimator>.jsonl and, per clip, the frame track to
<subdir>/tracks/<estimator>/ so a cut-off can be swept later without running the
estimator again. Resumable.
"""

from __future__ import annotations

import argparse
import json
from multiprocessing import Pool
from pathlib import Path

import numpy as np
import soundfile as sf

import pitch_c3_f0 as P
from pitch_c3_ipc import STUDY_DIR


def analyse(args):
    root, rel, estimator, device = args
    y, sr = sf.read(Path(root) / rel, dtype="float32", always_2d=True)
    y = P.to_mono(y)
    if estimator == "yin":
        s = P.shipped_yin(y, sr)
        s["path"] = rel
        s["seconds"] = len(y) / sr
        return s
    t = P.pyin_track(y, sr) if estimator == "pyin" else P.crepe_track(y, sr, device=device)
    s = P.summarise_track(t)
    s["path"] = rel
    s["seconds"] = len(y) / sr
    out = Path(root) / "tracks" / estimator / (rel + ".npz")
    out.parent.mkdir(parents=True, exist_ok=True)
    np.savez_compressed(out, midi=t["midi"].astype(np.float32), conf=t["conf"].astype(np.float32),
                        voiced=t["voiced"], active=t["active"])
    return s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("subdir")
    ap.add_argument("--estimator", choices=["pyin", "crepe", "yin"], required=True)
    ap.add_argument("--procs", type=int, default=1)
    ap.add_argument("--device", default="cpu")
    a = ap.parse_args()
    root = STUDY_DIR / a.subdir
    results = root / f"f0_{a.estimator}.jsonl"
    done = set()
    if results.exists():
        done = {json.loads(l)["path"] for l in results.open(encoding="utf-8")}
    paths = []
    for line in (root / "index.jsonl").open(encoding="utf-8"):
        rel = json.loads(line)["path"]
        if rel not in done and rel not in paths:
            paths.append(rel)
    jobs = [(str(root), rel, a.estimator, a.device) for rel in paths]
    print(f"{a.estimator}: {len(jobs)} clips to do, {len(done)} done", flush=True)
    with results.open("a", encoding="utf-8") as out:
        if a.procs > 1:
            with Pool(a.procs) as pool:
                for n, s in enumerate(pool.imap_unordered(analyse, jobs, chunksize=4), 1):
                    out.write(json.dumps(s) + "\n")
                    out.flush()
                    if n % 200 == 0:
                        print(f"{a.estimator}: {n}", flush=True)
        else:
            for n, j in enumerate(jobs, 1):
                out.write(json.dumps(analyse(j)) + "\n")
                out.flush()
                if n % 200 == 0:
                    print(f"{a.estimator}: {n}", flush=True)
    print(f"{a.estimator}: finished", flush=True)


if __name__ == "__main__":
    main()
