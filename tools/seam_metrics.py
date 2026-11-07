#!/usr/bin/env python3
"""Seam meters on a rendered loop (the WAV tools/audition_loop_seams.cpp writes).

    seam_metrics.py <rendered.wav> @<seam,seam,...> [plot.png]
    seam_metrics.py <rendered.wav> <regionLen_samples> <ratio> [plot.png]

Seam positions are the output indices the tool prints as seams_at= (preferred),
or k * regionLen / ratio. Two detectors, each reported against a NULL
distribution of the same measure at 200 random non-seam positions of the same
render (null_p95): a seam is a finding when it clearly exceeds that.

  lpc   short-term predictor residual (Vaseghi & Rayner 1990; Godsill & Rayner
        1998, ch. 5): an all-pole predictor fitted on the 60..10 ms before the
        seam, its residual energy in the worst 2 ms window at the seam over the
        louder neighbouring stretch. Sees steps and bursts.
  ltp   long-term (pitch) predictor residual (Atal & Schroeder 1979): x[n] -
        g*x[n-P], P from the autocorrelation before the seam. Sees what a
        time reversal or a phase jump does to a periodic sound, which the
        short predictor does not (the reversed waveform is locally smooth).
        nan where the material before the seam is not periodic.
"""
import sys, numpy as np, soundfile as sf

def lpc(x, order):
    # covariance method: least-squares predictor coefficients a, x[n] ~ sum a_k x[n-k]
    n = len(x)
    if n <= 2 * order: return np.zeros(order)
    X = np.stack([x[order - k - 1:n - k - 1] for k in range(order)], axis=1)
    t = x[order:]
    a, *_ = np.linalg.lstsq(X, t, rcond=None)
    return a

def residual(x, a):
    # e[n] = x[n] - sum_k a_k x[n-k]; filter taps [1, -a_1, ..., -a_p]
    h = np.concatenate([[1.0], -a])
    e = np.convolve(x, h)[:len(x)]
    e[:len(a)] = 0.0
    return e

def measure(y, c, sr, order=32):
    fitLo, fitHi = c - int(0.060*sr), c - int(0.010*sr)
    if fitLo < 0: return None
    a = lpc(y[fitLo:fitHi], order)
    seg = y[c - int(0.030*sr): c + int(0.030*sr)]
    if len(seg) < int(0.060*sr): return None
    res = residual(seg, a)**2
    mid = len(res)//2
    w = max(8, int(0.002*sr))
    # worst 2 ms window within +-5 ms of the seam
    near = max(res[k-w//2:k+w//2].mean() for k in range(mid-int(0.005*sr), mid+int(0.005*sr), w//4))
    # neighbours: 7.5-30 ms before (fit side) and after
    pre = res[mid-int(0.030*sr)+order: mid-int(0.0075*sr)].mean()
    post = res[mid+int(0.0075*sr): mid+int(0.030*sr)].mean()
    return 10*np.log10((near+1e-20)/(max(pre,post)+1e-20))

def ltp(y, c, sr):
    """Long-term (pitch) predictor residual across the seam: x[n] - g*x[n-P], P from
    the autocorrelation of the 60 ms before the seam (speech-coding pitch predictor,
    Atal & Schroeder 1979). A time reversal or a phase jump of a periodic sound
    breaks the periodic continuation for exactly one period at the seam; a
    crossfade spreads and lowers that. Returns (excess_dB, periodicity) or None
    when the material before the seam is not periodic (peak autocorr < 0.5)."""
    pre = y[c - int(0.060*sr): c]
    if len(pre) < int(0.060*sr): return None
    pre = pre - pre.mean()
    ac = np.correlate(pre, pre, 'full')[len(pre)-1:]
    if ac[0] <= 0: return None
    ac = ac / ac[0]
    lo, hi = int(sr/2000), int(sr/40)
    P = lo + int(np.argmax(ac[lo:hi])); rho = float(ac[P])
    if rho < 0.5: return None
    seg = y[c - int(0.030*sr): c + int(0.030*sr) + P]
    g = np.dot(seg[P:], seg[:-P]) / max(1e-12, np.dot(seg[:-P], seg[:-P]))
    g = float(np.clip(g, 0.0, 1.2))
    res = (seg[P:] - g*seg[:-P])**2          # res[k] belongs to sample seg[P+k]
    mid = int(0.030*sr)                       # index of the seam within res
    w = max(8, P)
    near = max(res[k:k+w].mean() for k in range(mid - P, mid + 1, max(1, P//8)))
    nb_pre = res[max(0, mid - int(0.030*sr)): mid - P].mean() if mid - P > 0 else 0.0
    nb_post = res[mid + 2*P: mid + 2*P + int(0.0225*sr)].mean() if mid + 2*P + 16 < len(res) else 0.0
    return 10*np.log10((near+1e-20)/(max(nb_pre, nb_post)+1e-20)), rho

def main():
    y, sr = sf.read(sys.argv[1], dtype='float32')
    if y.ndim > 1: y = y[:,0]
    if ',' in sys.argv[2] or sys.argv[2].startswith('@'):
        seams = [int(v) for v in sys.argv[2].lstrip('@').split(',') if v]
        L = np.median(np.diff(seams)) if len(seams) > 1 else len(y)
    else:
        L = float(sys.argv[2]) / float(sys.argv[3]); seams = []; s = L
        while s < len(y): seams.append(int(round(s))); s += L
    vals = []; lt = []
    for c in seams:
        if c + 0.05*sr >= len(y) or c < 0.07*sr: continue
        v = measure(y, c, sr)
        if v is not None: vals.append(v)
        t = ltp(y, c, sr)
        lt.append(t[0] if t else float('nan'))
    rng = np.random.default_rng(1)
    null = []; nullLt = []
    for _ in range(200):
        c = int(rng.integers(int(0.1*sr), len(y) - int(0.08*sr)))
        if min(abs(c - k) for k in seams) < 0.05*sr: continue
        v = measure(y, c, sr)
        if v is not None: null.append(v)
        t = ltp(y, c, sr)
        if t: nullLt.append(t[0])
    if not vals: print("no seams"); return
    print("per_seam_dB=" + ",".join(f"{v:.1f}" for v in vals))
    print("ltp_per_seam_dB=" + ",".join(f"{v:.1f}" for v in lt))
    ltv = [v for v in lt if v == v]
    ltp_s = f"ltp_max_dB={max(ltv):.1f} ltp_null_p95_dB={np.percentile(nullLt,95):.1f}" if ltv and nullLt else "ltp_max_dB=nan ltp_null_p95_dB=nan"
    print(f"lpc_max_dB={max(vals):.1f} lpc_mean_dB={np.mean(vals):.1f} null_p95_dB={np.percentile(null,95):.1f} null_max_dB={max(null):.1f} {ltp_s} seams={len(vals)}")
    if sys.argv[-1].endswith('.png'):
        import matplotlib; matplotlib.use('Agg'); import matplotlib.pyplot as plt
        w = int(0.040*sr); k = min(4, len(seams), len(vals))
        fig, ax = plt.subplots(k,1, figsize=(12,2.2*k))
        t = (np.arange(-w, w))/sr*1000
        for i in range(k):
            c = seams[i]
            ax[i].plot(t, y[c-w:c+w], lw=0.5); ax[i].axvline(0, color='r', lw=0.5)
            ax[i].set_title(f"{sys.argv[1].split('/')[-1]}  seam {i+1}  +-40 ms  lpc {vals[i]:.1f} dB", fontsize=8)
        fig.tight_layout(); fig.savefig(sys.argv[-1], dpi=100)

if __name__ == '__main__': main()
