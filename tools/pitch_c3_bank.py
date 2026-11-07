#!/usr/bin/env python3
"""Pull the stored audio out of every preset, for the bank side of the c3 study.

    ~/.cache/t5ynth-pitch-venv/bin/python tools/pitch_c3_bank.py

A preset stores the buffer the player actually hears, and, when it has one, its
inference cache: successive generations of the same patch. Layout: docs/PRESET_FORMAT.md
section 2 and 4.1: from v4 the primary and the cache entries are [uint32 byteLen][FLAC],
in v3 they are raw interleaved float32 of a length the JSON gives. Read only; the presets
are not touched. Output: STUDY_DIR/bank, outside the repository. Identical audio stored
under several presets is kept once; caches.jsonl keeps each preset's own list of entries.
"""

from __future__ import annotations

import hashlib
import io
import json
import re
import struct
from pathlib import Path

import numpy as np
import soundfile as sf

from pitch_c3_ipc import STUDY_DIR

PRESETS = Path.home() / "Library/T5ynth/presets"
PITCH_WORD = re.compile(r"(?:^|[\s,.])([a-g][#b]?[0-9])\s*(?:[,.]|$)", re.IGNORECASE)


def pitch_words(text: str | None) -> list[str]:
    return [m.lower() for m in PITCH_WORD.findall(text or "")]


def read_preset(path: Path):
    data = path.read_bytes()
    if data[:4] != b"T5YN":
        return None
    version, json_len = struct.unpack("<II", data[4:12])
    meta = json.loads(data[12:12 + json_len].decode("utf-8"))
    return version, meta, data[12 + json_len:]


def blobs(tail: bytes):
    pos = 0
    while pos + 4 <= len(tail):
        (n,) = struct.unpack("<I", tail[pos:pos + 4])
        if tail[pos + 4:pos + 8] != b"fLaC" or pos + 4 + n > len(tail):
            return
        yield tail[pos + 4:pos + 4 + n]
        pos += 4 + n


def payloads(version: int, meta: dict, tail: bytes) -> tuple[list[bytes], bool]:
    """The preset's audio payloads as FLAC files in memory, primary first, and whether it
    has a primary. A v3 payload is encoded here, 24 bit like the v4 writer."""
    has_audio = (meta.get("audio_meta") or {}).get("numSamples", 0) > 0
    cache = (meta.get("inferenceCache") or {}).get("entries") or []
    if version >= 4:
        return list(blobs(tail))[:(1 if has_audio else 0) + len(cache)], has_audio
    specs = ([meta["audio_meta"]] if has_audio else []) + cache
    out, pos = [], 0
    for spec in specs:
        channels, frames = int(spec["channels"]), int(spec["numSamples"])
        n = channels * frames * 4
        pcm = np.frombuffer(tail[pos:pos + n], dtype="<f4").reshape(frames, channels)
        pos += n
        buf = io.BytesIO()
        sf.write(buf, np.clip(pcm, -1.0, 1.0), int(spec["sampleRate"]), subtype="PCM_24", format="FLAC")
        out.append(buf.getvalue())
    return out, has_audio


def main():
    root = STUDY_DIR / "bank"
    (root / "audio").mkdir(parents=True, exist_ok=True)
    seen: dict[str, dict] = {}
    caches: list[dict] = []
    n_presets = 0
    for path in sorted(PRESETS.rglob("*.t5p")):
        got = read_preset(path)
        if got is None:
            continue
        version, meta, tail = got
        n_presets += 1
        synth, engine = meta.get("synth", {}), meta.get("engine", {})
        rel = str(path.relative_to(PRESETS))
        all_blobs, has_audio = payloads(version, meta, tail)
        n_primary = 1 if has_audio else 0
        own_cache = []
        for i, blob in enumerate(all_blobs):
            digest = hashlib.sha1(blob).hexdigest()[:16]
            role = "primary" if (has_audio and i == 0) else "cache"
            entry = seen.get(digest)
            if entry is None:
                name = f"audio/{digest}.flac"
                (root / name).write_bytes(blob)
                entry = seen[digest] = {
                    "path": name, "presets": [], "role": role,
                    "prompt_a": synth.get("promptA"), "prompt_b": synth.get("promptB"),
                    "pitch_words": pitch_words(synth.get("promptA")) + pitch_words(synth.get("promptB")),
                    "model": synth.get("model") or "", "engine_mode": engine.get("mode"),
                    "published": rel.startswith("UCDCAE AI Lab/"),
                    "cache_of": rel if role == "cache" else None,
                    "cache_index": (i - n_primary) if role == "cache" else None,
                }
            entry["presets"].append(rel)
            entry["published"] = entry["published"] or rel.startswith("UCDCAE AI Lab/")
            if i >= n_primary:
                own_cache.append(entry["path"])
        if own_cache:
            caches.append({"preset": rel, "published": rel.startswith("UCDCAE AI Lab/"), "paths": own_cache})
    with (root / "index.jsonl").open("w", encoding="utf-8") as out:
        for e in seen.values():
            out.write(json.dumps(e, ensure_ascii=False) + "\n")
    with (root / "caches.jsonl").open("w", encoding="utf-8") as out:
        for c in caches:
            out.write(json.dumps(c, ensure_ascii=False) + "\n")
    roles = [e["role"] for e in seen.values()]
    print(f"{n_presets} presets, {len(seen)} distinct buffers: "
          f"{roles.count('primary')} primary, {roles.count('cache')} cache")


if __name__ == "__main__":
    main()
