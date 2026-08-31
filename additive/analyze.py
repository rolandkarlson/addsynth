"""Analyze an audio file into an AdditiveModel.

Pipeline:
  1. load mono audio
  2. pitch-track with pYIN -> f0 per frame, f0_ref = median voiced f0
  3. long-window STFT (4096): harmonic amplitudes at k*f0 per frame,
     plus per-partial DETUNE (actual spectral peak vs the ideal grid)
  4. short-window STFT (1024): same harmonic sampling with ~4x better
     time resolution; the first ~150 ms of the envelopes crossfade from
     the short-window analysis so attacks keep their snap
  5. residual (spectrum minus harmonics) -> 48 log-spaced noise bands
  6. partial count adapts to pitch: enough harmonics to reach ~16 kHz
     (24..128), so bass slices keep their top octaves
"""

from __future__ import annotations

import numpy as np
import librosa

from .model import AdditiveModel

DEFAULT_N_NOISE_BANDS = 48
MAX_PARTIALS = 128
MIN_PARTIALS = 24
TARGET_TOP_HZ = 16000.0

# transient crossfade: short-window analysis rules until ATTACK_END,
# fades out by ATTACK_FADE_END
ATTACK_END = 0.08
ATTACK_FADE_END = 0.18


def _stft_mag(y: np.ndarray, n_fft: int, hop: int):
    S = np.abs(librosa.stft(y, n_fft=n_fft, hop_length=hop, window="hann"))
    win_gain = np.sum(np.hanning(n_fft)) / 2.0
    return S, win_gain


def _refine_f0(S: np.ndarray, bin_hz: float, f0: np.ndarray) -> np.ndarray:
    """Correct pyin's small bias per frame using the actual spectral peaks
    of harmonics 1..3 (parabolic interpolation). A fractional-percent f0
    error otherwise derails the sampling grid at high harmonics."""
    n_bins, n_frames = S.shape
    out = f0.copy()
    for fi in range(n_frames):
        ests, wts = [], []
        for k in (1, 2, 3):
            hb = k * f0[fi] / bin_hz
            if hb >= n_bins - 2:
                break
            w = max(2, int(0.4 * f0[fi] / bin_hz))
            lo, hi = int(max(1, hb - w)), int(min(n_bins - 2, hb + w))
            if hi <= lo + 1:
                continue
            pk = int(np.argmax(S[lo:hi + 1, fi])) + lo
            if pk <= lo or pk >= hi:
                continue
            a, b, c = S[pk - 1, fi], S[pk, fi], S[pk + 1, fi]
            den = a - 2 * b + c
            d = 0.5 * (a - c) / den if abs(den) > 1e-12 else 0.0
            ests.append((pk + d) * bin_hz / k)
            wts.append(b)
        if ests:
            out[fi] = float(np.average(ests, weights=wts))
    # never trust the refinement further than a semitone from pyin
    return np.clip(out, f0 * 2 ** (-1 / 12), f0 * 2 ** (1 / 12))


def _harmonic_env(S: np.ndarray, win_gain: float, bin_hz: float,
                  f0: np.ndarray, n_partials: int,
                  detune: np.ndarray | None = None):
    """Sample |STFT| at every harmonic k*f0 (times per-partial detune)
    -> (n_frames, n_partials). Where the sampling point sits on a local
    spectral peak, use the parabolic vertex magnitude (no scalloping
    loss); otherwise fall back to linear interpolation."""
    n_bins, n_frames = S.shape
    k = np.arange(1, n_partials + 1, dtype=np.float64)[:, None]  # (P, 1)
    if detune is not None:
        k = k * detune[:, None]
    harm_bin = (k * f0[None, :n_frames]) / bin_hz        # fractional bin
    below_nyq = harm_bin < (n_bins - 2)

    lo = np.clip(np.floor(harm_bin).astype(int), 0, n_bins - 2)
    frac = np.clip(harm_bin - lo, 0.0, 1.0)
    cols = np.arange(n_frames)[None, :]
    mag = S[lo, cols] * (1 - frac) + S[lo + 1, cols] * frac

    c = np.clip(np.round(harm_bin).astype(int), 1, n_bins - 2)
    sa, sb, sc = S[c - 1, cols], S[c, cols], S[c + 1, cols]
    den = sa - 2 * sb + sc
    delta = np.where(np.abs(den) > 1e-12, 0.5 * (sa - sc) / den, 0.0)
    vertex = sb - 0.25 * (sa - sc) * delta
    on_peak = (sb >= sa) & (sb >= sc) & (sb > 0) & (np.abs(delta) <= 0.6)
    mag = np.where(on_peak, np.maximum(vertex, 0.0), mag)

    env = np.where(below_nyq, mag / win_gain, 0.0).T     # (F, P)
    return env, harm_bin, below_nyq


def _estimate_detune(S: np.ndarray, env: np.ndarray, harm_bin: np.ndarray,
                     below_nyq: np.ndarray, f0: np.ndarray,
                     bin_hz: float) -> np.ndarray:
    """Per-partial frequency ratio vs the ideal harmonic grid.

    For each partial, at its strongest frames, find the actual spectral
    peak near k*f0 (parabolic interpolation) and take the median ratio.
    Clamped to +-6% — larger deviations mean the 'harmonic' was really
    some other source, and 1.0 is safer.
    """
    n_bins = S.shape[0]
    n_partials = env.shape[1]
    det = np.ones(n_partials, dtype=np.float32)
    env_top = float(env.max())
    if env_top <= 0:
        return det

    for k in range(n_partials):
        amps = env[:, k]
        if amps.max() < env_top * 2e-3:
            continue
        strongest = np.argsort(amps)[-8:]
        ratios = []
        for fi in strongest:
            if not below_nyq[k, fi]:
                continue
            hb = harm_bin[k, fi]
            w = max(2, int(0.45 * f0[fi] / bin_hz))
            lo = int(max(1, hb - w))
            hi = int(min(n_bins - 2, hb + w))
            if hi <= lo + 1:
                continue
            pk = int(np.argmax(S[lo:hi + 1, fi])) + lo
            if pk <= lo or pk >= hi:
                continue  # peak ran into the search edge: unreliable
            a, b, c = S[pk - 1, fi], S[pk, fi], S[pk + 1, fi]
            denom = a - 2 * b + c
            d = 0.5 * (a - c) / denom if abs(denom) > 1e-12 else 0.0
            ratios.append((pk + d) / hb)
        if ratios:
            det[k] = float(np.clip(np.median(ratios), 0.94, 1.06))
    return det


def _noise_bands(S: np.ndarray, win_gain: float, bin_hz: float,
                 harm_bin: np.ndarray, below_nyq: np.ndarray,
                 band_edges: np.ndarray) -> np.ndarray:
    """Zero the spectrum near harmonics, average the rest into bands."""
    n_bins, n_frames = S.shape
    S_noise = S.copy()
    guard = 2  # +-2 bins around each harmonic
    for fi in range(n_frames):
        hb = harm_bin[:, fi][below_nyq[:, fi]]
        if len(hb) == 0:
            continue
        centers = np.round(hb).astype(int)
        for g in range(-guard, guard + 1):
            b = np.clip(centers + g, 0, n_bins - 1)
            S_noise[b, fi] = 0.0

    n_bands = len(band_edges) - 1
    bin_freqs = np.arange(n_bins) * bin_hz
    out = np.zeros((n_frames, n_bands))
    for b in range(n_bands):
        sel = (bin_freqs >= band_edges[b]) & (bin_freqs < band_edges[b + 1])
        if not np.any(sel):
            continue
        out[:, b] = np.sqrt(np.mean(S_noise[sel, :] ** 2, axis=0)) / win_gain
    return out


def analyze(
    path: str,
    n_partials: int | None = None,
    n_noise_bands: int = DEFAULT_N_NOISE_BANDS,
    fmin: float = 50.0,
    fmax: float = 2000.0,
    n_fft: int = 4096,
    hop: int = 256,
    name: str | None = None,
    f0_override: float | None = None,
) -> AdditiveModel:
    y, sr = librosa.load(path, sr=None, mono=True)
    model = analyze_signal(
        y, sr, n_partials=n_partials, n_noise_bands=n_noise_bands,
        fmin=fmin, fmax=fmax, n_fft=n_fft, hop=hop,
        name=name or path.rsplit("/", 1)[-1].rsplit(".", 1)[0],
        f0_override=f0_override,
    )
    model.meta["source"] = path
    return model


def analyze_signal(
    y: np.ndarray,
    sr: int,
    n_partials: int | None = None,   # None = adapt to pitch (24..128)
    n_noise_bands: int = DEFAULT_N_NOISE_BANDS,
    fmin: float = 50.0,
    fmax: float = 2000.0,
    n_fft: int = 4096,
    hop: int = 256,
    name: str = "untitled",
    f0_override: float | None = None,
) -> AdditiveModel:
    y = np.asarray(y, dtype=np.float64)
    if len(y) < n_fft:
        y = np.pad(y, (0, n_fft - len(y)))

    control_rate = sr / hop

    # ---- pitch track ----------------------------------------------------
    if f0_override is not None:
        f0_ref = float(f0_override)
        n_frames_est = 1 + len(y) // hop
        f0 = np.full(n_frames_est, f0_ref)
        voiced_fraction = 1.0
        pitch_stability = 1.0
    else:
        f0, voiced, _ = librosa.pyin(
            y, fmin=fmin, fmax=fmax, sr=sr,
            frame_length=n_fft, hop_length=hop,
        )
        if not np.any(voiced):
            raise ValueError(
                f"{name}: no pitched content found (fmin={fmin}, fmax={fmax}). "
                "Try f0_override or different fmin/fmax."
            )
        f0_ref = float(np.median(f0[voiced]))
        voiced_fraction = float(np.mean(voiced))
        semitones = 12 * np.log2(f0[voiced] / f0_ref)
        pitch_stability = float(np.mean(np.abs(semitones) <= 1.0))
        idx = np.arange(len(f0))
        f0 = np.interp(idx, idx[voiced], f0[voiced])

    # adaptive partial count: reach ~16 kHz whatever the pitch
    n_partials_auto = n_partials is None
    if n_partials_auto:
        n_partials = int(np.clip(round(TARGET_TOP_HZ / f0_ref),
                                 MIN_PARTIALS, MAX_PARTIALS))

    # ---- long-window analysis (frequency detail, detune) -----------------
    S_long, wg_long = _stft_mag(y, n_fft, hop)
    n_frames = S_long.shape[1]
    f0 = np.resize(f0, n_frames)
    if f0_override is None:
        f0 = _refine_f0(S_long, sr / n_fft, f0)
        f0_ref = float(np.median(f0))
        if n_partials_auto:
            n_partials = int(np.clip(round(TARGET_TOP_HZ / f0_ref),
                                     MIN_PARTIALS, MAX_PARTIALS))
    # pass 1: rough grid to find per-partial detune, pass 2: sample the
    # envelopes at the detuned positions (this is what makes high
    # harmonics come out at the right level)
    env_rough, hb_long, nyq_long = _harmonic_env(
        S_long, wg_long, sr / n_fft, f0, n_partials)
    detune = _estimate_detune(S_long, env_rough, hb_long, nyq_long,
                              f0, sr / n_fft)
    env_long, hb_long, nyq_long = _harmonic_env(
        S_long, wg_long, sr / n_fft, f0, n_partials, detune)

    # ---- short-window analysis (time detail for the attack) --------------
    # Only useful when the short window can resolve the harmonic spacing
    # (~172 Hz at 44.1k); below that it smears neighbours together and
    # RUINS the attack spectrum, so bass relies on the long window plus
    # the energy correction below.
    n_fft_short = 1024
    t = np.arange(n_frames) * hop / sr
    w_short = np.clip(1.0 - (t - ATTACK_END) / (ATTACK_FADE_END - ATTACK_END),
                      0.0, 1.0)
    S_short, wg_short = _stft_mag(y, n_fft_short, hop)
    ns = min(n_frames, S_short.shape[1])
    env = env_long.copy()
    if f0_ref >= 1.05 * sr / n_fft_short * 4:   # mainlobe fits between harmonics
        env_short, hb_short, nyq_short = _harmonic_env(
            S_short[:, :ns], wg_short, sr / n_fft_short, f0[:ns],
            n_partials, detune)
        env[:ns] = (w_short[:ns, None] * env_short
                    + (1.0 - w_short[:ns, None]) * env_long[:ns])
    else:
        _, hb_short, nyq_short = _harmonic_env(
            S_short[:, :ns], wg_short, sr / n_fft_short, f0[:ns], n_partials)

    # ---- noise residual ---------------------------------------------------
    band_edges = np.geomspace(40.0, sr / 2.0, n_noise_bands + 1)
    band_centers = np.sqrt(band_edges[:-1] * band_edges[1:])
    noise_long = _noise_bands(S_long, wg_long, sr / n_fft,
                              hb_long, nyq_long, band_edges)
    noise_short = _noise_bands(S_short[:, :ns], wg_short, sr / n_fft_short,
                               hb_short, nyq_short, band_edges)
    noise_env = noise_long.copy()
    noise_env[:ns] = (w_short[:ns, None] * noise_short
                      + (1.0 - w_short[:ns, None]) * noise_long[:ns])

    # ---- attack energy correction ------------------------------------------
    # The analysis windows smear fast onsets; force the model's short-time
    # energy to match the source's during the attack. The proxy scale
    # cancels out by normalizing against the sustain, so only the SHAPE
    # of the true envelope is imposed.
    rt = np.array([np.sqrt(np.mean(y[max(0, i * hop - hop // 2)
                                     : i * hop + hop // 2] ** 2) + 1e-18)
                   for i in range(n_frames)])
    proxy = np.sqrt(0.5 * np.sum(env ** 2, axis=1)
                    + np.sum(noise_env ** 2, axis=1) + 1e-18)
    ratio = rt / proxy
    sustain = ratio[(t >= 0.3) & (rt > rt.max() * 0.05)]
    if len(sustain) >= 4:
        ratio = ratio / np.median(sustain)
        corr = np.clip(ratio, 0.3, 3.0)
        fade = np.clip(1.0 - (t - 0.25) / 0.1, 0.0, 1.0)  # only the attack
        corr = 1.0 + (corr - 1.0) * fade
        env *= corr[:, None]
        noise_env *= corr[:, None]

    # ---- trim leading/trailing silence in the control frames ---------------
    total = env.sum(axis=1) + noise_env.sum(axis=1)
    active = np.where(total > total.max() * 1e-4)[0]
    if len(active) > 0:
        a, b = active[0], active[-1] + 1
        env, noise_env, f0 = env[a:b], noise_env[a:b], f0[a:b]

    return AdditiveModel(
        env=env.astype(np.float32),
        f0_track=(f0 / f0_ref).astype(np.float32),
        noise_env=noise_env.astype(np.float32),
        noise_band_freqs=band_centers.astype(np.float32),
        detune=detune,
        f0_ref=f0_ref,
        control_rate=control_rate,
        name=name,
        source_sr=sr,
        meta={
            "voiced_fraction": voiced_fraction,
            "pitch_stability": pitch_stability,
        },
    )
