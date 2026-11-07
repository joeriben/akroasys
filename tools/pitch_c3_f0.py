#!/usr/bin/env python3
"""f0 of generated audio, by published estimators only. The c3 pitch study.

Runs in its own venv (~/.cache/t5ynth-pitch-venv: librosa ISC, torchcrepe MIT), so
nothing is added to the project environment or to what ships.

    pYIN   Mauch & Dixon 2014, "pYIN: A fundamental frequency estimator using
           probabilistic threshold distributions", ICASSP. librosa.pyin.
    CREPE  Kim, Salamon, Li, Bello 2018, "CREPE: A convolutional representation for
           pitch estimation", ICASSP. torchcrepe, model "full", Viterbi decoding,
           periodicity as confidence. The 0.21 voicing threshold is torchcrepe's own
           documented example value.

What is OURS and has no source, and is therefore declared and never decisive alone:
the summary of a clip's frame track into one number. It is plain descriptive
statistics and makes no perceptual claim:

    active frame   frame level within ACTIVE_DB of the clip's loudest frame
    voiced share   voiced frames / active frames
    dominant pitch median of the frames inside the 100-cent window that holds the
                   most voiced frames
    concentration  frames inside that window / voiced frames

A stationary tone has concentration 1. A phrase of several notes has less. A clip is
reported as "no stable f0" under the two provisional cut-offs below; the report shows
what the result does across other values, because a cut-off chosen here decides nothing.
"""

from __future__ import annotations

import warnings

import numpy as np

SR_PYIN = 22050
SR_CREPE = 16000
FMIN = 32.70        # C1
FMAX_PYIN = 2093.0  # C7
FMAX_CREPE = 1975.5  # top of CREPE's range
PYIN_FRAME = 2048   # 93 ms: three periods of C1
PYIN_HOP = 256
CREPE_HOP = 320     # 20 ms at 16 kHz
CREPE_VOICED = 0.21
ACTIVE_DB = 40.0
WINDOW_CENTS = 100.0
MIN_VOICED_FRAMES = 10

# Provisional, see the module docstring.
STABLE_VOICED = 0.5
STABLE_CONCENTRATION = 0.5


def hz_to_midi(hz):
    return 69.0 + 12.0 * np.log2(np.asarray(hz, dtype=np.float64) / 440.0)


def midi_to_hz(m):
    return 440.0 * 2.0 ** ((np.asarray(m, dtype=np.float64) - 69.0) / 12.0)


# pYIN reports on a 10-cent grid anchored at fmin, and FMIN is 0.17 cents below C1, so the
# grid point meant as "50 cents below C" is -50.17 and the one meant as +50 is +49.83.
# Without this slack the +-50 window would be lopsided on pYIN's values, -45 to +55.
GRID_SLACK = 0.002   # semitones


def nearest_note(midi: float) -> int:
    """The nearest semitone; a half-way pitch counts upward, on pYIN's grid as well."""
    return int(np.floor(midi + 0.5 + GRID_SLACK))


def note_name(midi: float) -> str:
    """Scientific pitch notation: MIDI 60 = C4. A note's name is its nearest semitone."""
    names = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
    n = nearest_note(midi)
    return f"{names[n % 12]}{n // 12 - 1}"


def cents_to_pitch_class(midi: float, pc: int) -> float:
    """Signed distance in cents to the nearest instance of pitch class pc, in [-600, 600)."""
    return ((midi - pc + 6.0) % 12.0 - 6.0) * 100.0


def to_mono(audio: np.ndarray) -> np.ndarray:
    """audio is [samples] or [samples, channels]."""
    return audio if audio.ndim == 1 else audio.mean(axis=1)


def summarise(midi_frames, voiced, active) -> dict:
    """midi_frames, voiced, active: equal-length frame arrays. See module docstring."""
    midi_frames = np.asarray(midi_frames, dtype=np.float64)
    use = np.asarray(voiced, dtype=bool) & np.asarray(active, dtype=bool) & np.isfinite(midi_frames)
    n_active = int(np.count_nonzero(active))
    n_voiced = int(np.count_nonzero(use))
    out = {"n_active": n_active, "n_voiced": n_voiced,
           "voiced_share": n_voiced / n_active if n_active else 0.0,
           "dominant_midi": None, "dominant_hz": None, "concentration": 0.0,
           "spread_cents": None}
    if n_voiced < MIN_VOICED_FRAMES:
        return out
    pitches = np.sort(midi_frames[use])
    width = WINDOW_CENTS / 100.0
    # For each frame as a window's lower edge, how many frames fall inside.
    upper = np.searchsorted(pitches, pitches + width, side="right")
    counts = upper - np.arange(n_voiced)
    best = int(np.argmax(counts))
    inside = pitches[best:upper[best]]
    dominant = float(np.median(inside))
    out["dominant_midi"] = dominant
    out["dominant_hz"] = float(midi_to_hz(dominant))
    out["concentration"] = float(counts[best]) / n_voiced
    q1, q3 = np.percentile(pitches, [25, 75])
    out["spread_cents"] = float(q3 - q1) * 100.0
    return out


def is_stable(s: dict, voiced=STABLE_VOICED, conc=STABLE_CONCENTRATION) -> bool:
    return (s["dominant_midi"] is not None and s["voiced_share"] >= voiced
            and s["concentration"] >= conc)


def _frame_db(y: np.ndarray, frame: int, hop: int, n_frames: int) -> np.ndarray:
    """Level per analysis frame, centred like librosa/torchcrepe frames."""
    pad = np.pad(y, frame // 2)
    idx = np.arange(n_frames) * hop
    csum = np.concatenate([[0.0], np.cumsum(pad.astype(np.float64) ** 2)])
    hi = np.minimum(idx + frame, len(pad))
    lo = np.minimum(idx, len(pad))
    power = (csum[hi] - csum[lo]) / frame
    return 10.0 * np.log10(np.maximum(power, 1e-20))


def active_mask(y, frame, hop, n_frames) -> np.ndarray:
    db = _frame_db(y, frame, hop, n_frames)
    return db >= db.max() - ACTIVE_DB


def pyin_track(y: np.ndarray, sr: int) -> dict:
    import librosa
    if sr != SR_PYIN:
        y = librosa.resample(y, orig_sr=sr, target_sr=SR_PYIN, res_type="soxr_hq")
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        f0, flag, prob = librosa.pyin(y, fmin=FMIN, fmax=FMAX_PYIN, sr=SR_PYIN,
                                      frame_length=PYIN_FRAME, hop_length=PYIN_HOP,
                                      fill_na=np.nan)
    midi = np.full(f0.shape, np.nan)
    ok = np.isfinite(f0) & (f0 > 0)
    midi[ok] = hz_to_midi(f0[ok])
    active = active_mask(y, PYIN_FRAME, PYIN_HOP, len(f0))
    return {"midi": midi, "voiced": flag, "conf": prob, "active": active}


_crepe_ready = {}


def crepe_track(y: np.ndarray, sr: int, device: str = "cpu", model: str = "full") -> dict:
    import librosa
    import torch
    import torchcrepe
    if sr != SR_CREPE:
        y = librosa.resample(y, orig_sr=sr, target_sr=SR_CREPE, res_type="soxr_hq")
    audio = torch.from_numpy(np.ascontiguousarray(y, dtype=np.float32))[None]
    with torch.no_grad():
        f0, per = torchcrepe.predict(audio, SR_CREPE, CREPE_HOP, FMIN, FMAX_CREPE, model,
                                     decoder=torchcrepe.decode.viterbi,
                                     return_periodicity=True, batch_size=1024,
                                     device=device, pad=True)
    f0 = f0[0].cpu().numpy().astype(np.float64)
    per = per[0].cpu().numpy().astype(np.float64)
    midi = hz_to_midi(np.maximum(f0, 1e-6))
    active = active_mask(y, 1024, CREPE_HOP, len(f0))
    return {"midi": midi, "voiced": per >= CREPE_VOICED, "conf": per, "active": active}


# ── The estimator the product already ships ─────────────────────────────────

SHIPPED_WINDOW = 4096
SHIPPED_HOP = 2048
SHIPPED_THRESHOLD = 0.1
SHIPPED_CONFIDENCE = 0.9


def yin_window(x: np.ndarray, sr: float) -> tuple[float, float]:
    """WavetableOscillator::analyzePitchWindow, step for step: YIN (de Cheveigne &
    Kawahara 2002) difference function, cumulative mean normalisation, absolute
    threshold 0.1, parabolic refinement. Returns (hz, confidence), (0, 0) for none."""
    half = len(x) // 2
    x = x.astype(np.float64)
    first = x[:half]
    n = 1 << int(np.ceil(np.log2(len(x) + half)))
    cross = np.fft.irfft(np.fft.rfft(x, n) * np.conj(np.fft.rfft(first, n)), n)[:half]
    energy_first = float(np.dot(first, first))
    csum = np.concatenate([[0.0], np.cumsum(x * x)])
    energy_shift = csum[np.arange(half) + half] - csum[np.arange(half)]
    diff = energy_first + energy_shift - 2.0 * cross
    diff[0] = 0.0
    running = np.cumsum(diff[1:])
    cmnd = np.ones(half)
    with np.errstate(divide="ignore", invalid="ignore"):
        cmnd[1:] = np.where(running > 0, diff[1:] * np.arange(1, half) / running, 1.0)
    below = np.nonzero(cmnd[2:] < SHIPPED_THRESHOLD)[0]
    if len(below) == 0:
        return 0.0, 0.0
    tau = int(below[0]) + 2
    while tau + 1 < half and cmnd[tau + 1] < cmnd[tau]:
        tau += 1
    s0, s1, s2 = cmnd[max(0, tau - 1)], cmnd[tau], cmnd[min(half - 1, tau + 1)]
    refined = tau + 0.5 * (s0 - s2) / (s0 - 2.0 * s1 + s2 + 1e-10)
    return sr / refined, float(min(1.0, max(0.0, 1.0 - s1)))


def shipped_yin(y: np.ndarray, sr: int) -> dict:
    """The clip-level pitch of WavetableOscillator::prepareFramesFromBuffer: windows of
    4096 at hop 2048, keep those with confidence >= 0.9 and 20 < hz < 5000, and take the
    upper median of what is left. Fewer than two candidates is no pitch. That code reads
    channel 0; here the mono mix is measured, like the other estimators."""
    cands = []
    for start in range(0, len(y) - SHIPPED_WINDOW + 1, SHIPPED_HOP):
        hz, conf = yin_window(y[start:start + SHIPPED_WINDOW], sr)
        if conf >= SHIPPED_CONFIDENCE and 20.0 < hz < 5000.0:
            cands.append(hz)
    n_windows = max(1, (len(y) - SHIPPED_WINDOW) // SHIPPED_HOP + 1)
    out = {"n_windows": n_windows, "n_candidates": len(cands),
           "voiced_share": len(cands) / n_windows, "dominant_midi": None, "dominant_hz": None,
           "concentration": 0.0}
    if len(cands) < 2:
        return out
    cands = sorted(cands)
    hz = cands[len(cands) // 2]
    midi = hz_to_midi(np.array(cands))
    out["dominant_hz"] = float(hz)
    out["dominant_midi"] = float(hz_to_midi(hz))
    out["concentration"] = float(np.mean(np.abs(midi - out["dominant_midi"]) <= 0.5))
    return out


def summarise_track(t: dict) -> dict:
    s = summarise(t["midi"], t["voiced"], t["active"])
    use = t["active"] & np.isfinite(t["conf"])
    s["mean_conf"] = float(np.mean(t["conf"][use])) if np.any(use) else 0.0
    return s
