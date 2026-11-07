#!/usr/bin/env python3
"""Minimal IPC client for the frozen backend, for the c3 pitch study.

Speaks docs/IPC_PROTOCOL.md to the same binary the built Standalone spawns
(PipeInference::findBundledBinary -> backend/dist/pipe_inference/pipe_inference).
No import of the backend, no second code path: requests are the JSON the plugin
serialises in PipeInference.cpp, frames are read as the plugin reads them.

Output of every tool in this study lives OUTSIDE the repository (see STUDY_DIR):
generated audio is not source.
"""

from __future__ import annotations

import json
import os
import struct
import subprocess
import sys
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
BINARY = REPO / "backend/dist/pipe_inference/pipe_inference"
STUDY_DIR = Path(os.environ.get("T5YNTH_PITCH_STUDY",
                                Path.home() / ".cache/t5ynth-pitch-study"))
# Generated audio and audio pulled from private presets must never become committable.
# An ignore pattern cannot know where this variable points; this check can.
if STUDY_DIR.resolve().is_relative_to(REPO):
    raise SystemExit(f"T5YNTH_PITCH_STUDY must lie outside the repository: {STUDY_DIR}")

# PromptPanel::defaultParamsFor — steps/cfg are forced per model at the
# generation choke point, a preset cannot change them.
def default_params_for(model: str) -> tuple[int, float]:
    m = model.lower()
    sa3 = "stable-audio-3" in m
    sao_small = (not sa3) and "small" in m
    ldm = "audioldm2" in m
    return (8 if (sa3 or sao_small) else (50 if ldm else 20),
            1.0 if (sa3 or sao_small) else (3.5 if ldm else 7.0))


K_MODALITY_EPOCH = 1  # PluginProcessor.h kModalityEpoch


class Backend:
    def __init__(self, stderr_path: Path | None = None):
        if not BINARY.is_file():
            raise SystemExit(f"backend binary missing: {BINARY}")
        err = open(stderr_path, "ab") if stderr_path else subprocess.DEVNULL
        self.proc = subprocess.Popen([str(BINARY)], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=err)
        self.ready = self._read_ready()

    def _read(self, n: int) -> bytes:
        buf = bytearray()
        while len(buf) < n:
            chunk = self.proc.stdout.read(n - len(buf))
            if not chunk:
                raise RuntimeError(f"backend closed the pipe after {len(buf)}/{n} bytes "
                                   f"(exit {self.proc.poll()})")
            buf.extend(chunk)
        return bytes(buf)

    def _read_error(self) -> str:
        (n,) = struct.unpack("<I", self._read(4))
        return self._read(n).decode("utf-8", "replace")

    def _read_ready(self) -> dict:
        b = self._read(1)
        if b == b"\x00":
            raise RuntimeError("backend error at start: " + self._read_error())
        if b != b"\x02":
            raise RuntimeError(f"unexpected ready byte {b!r}")
        (n,) = struct.unpack("<H", self._read(2))
        return json.loads(self._read(n).decode("utf-8"))

    def request(self, req: dict):
        """Returns (audio[channels, samples] float32, sr, seed, ms)."""
        line = json.dumps(req, ensure_ascii=False).replace("\n", " ").replace("\r", " ") + "\n"
        self.proc.stdin.write(line.encode("utf-8"))
        self.proc.stdin.flush()
        b = self._read(1)
        if b == b"\x00":
            raise RuntimeError("backend: " + self._read_error())
        if b != b"\x01":
            raise RuntimeError(f"unexpected response byte {b!r}")
        _flag, samples, channels, sr, seed, ms = struct.unpack("<iiiiif", self._read(24))
        pcm = np.frombuffer(self._read(channels * samples * 4), dtype="<f4")
        (dims,) = struct.unpack("<H", self._read(2))
        if dims:
            self._read(dims * 4 * 3)  # emb_a, emb_b, baseline
        return pcm.reshape(channels, samples).copy(), sr, seed, ms

    def close(self):
        try:
            self.proc.stdin.close()
            self.proc.wait(timeout=10)
        except Exception:
            self.proc.terminate()


def base_request(model: str, device: str, prompt_a: str, prompt_b: str = "",
                 alpha: float | None = None, seed: int = -1, duration: float = 3.0,
                 track_type: str | None = None) -> dict:
    """The fields PromptPanel::buildInferenceRequest + PipeInference::generate send
    for a plain generation. A lone prompt in linear mode is pinned to its pure end
    (the single-prompt guard): alpha -1 for A."""
    steps, cfg = default_params_for(model)
    if alpha is None:
        alpha = -1.0 if not prompt_b else 0.0
    req = {
        "prompt_a": prompt_a, "prompt_b": prompt_b, "alpha": alpha,
        "magnitude": 1.0, "noise_sigma": 0.0, "duration": duration, "start_pos": 0.0,
        "steps": steps, "cfg_scale": cfg, "seed": seed, "device": device, "model": model,
        "modality_epoch": K_MODALITY_EPOCH, "injection_mode": "linear",
        "injection_transition_at": 0.5, "late_phase_alpha": 0.0,
        "split_start": 4.0, "split_end": 16.0,
    }
    if "stable-audio-3" in model:
        req["track_type"] = track_type or "instrument"
    return req


if __name__ == "__main__":
    import time
    be = Backend(Path("/dev/stderr") if False else None)
    print(json.dumps({k: v for k, v in be.ready.items() if k != "model_metadata"}))
    for m in sys.argv[1:]:
        t = time.time()
        a, sr, seed, ms = be.request(base_request(m, be.ready["default"], "steady saw wave, c3", seed=1))
        t1 = time.time()
        a, sr, seed, ms = be.request(base_request(m, be.ready["default"], "steady saw wave, c3", seed=2))
        print(m, a.shape, sr, f"first={t1 - t:.1f}s second={time.time() - t1:.1f}s reported={ms:.0f}ms",
              f"peak={np.abs(a).max():.3f}", flush=True)
    be.close()
