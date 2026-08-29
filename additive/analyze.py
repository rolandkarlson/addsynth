"""Analyze an audio file into an AdditiveModel.

Pipeline:
  1. load mono audio
  2. pitch-track with pYIN -> f0 per frame, f0_ref = median voiced f0
  3. STFT; for each frame, read the magnitude at each harmonic k*f0
     (linear interpolation between bins) -> envelope matrix
  4. residual: zero the spectrum near harmonics, average what remains
     into log-spaced bands -> noise band envelopes
"""

from __future__ import annotations

import numpy as np
import librosa

from .model import AdditiveModel

DEFAULT_N_PARTIALS = 64
DEFAULT_N_NOISE_BANDS = 24


def analyze(
    path: str,
    n_partials: int = DEFAULT_N_PARTIALS,
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
    n_partials: int = DEFAULT_N_PARTIALS,
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
        # quality stats for slice selection (batch import)
        voiced_fraction = float(np.mean(voiced))
        semitones = 12 * np.log2(f0[voiced] / f0_ref)
        pitch_stability = float(np.mean(np.abs(semitones) <= 1.0))
        # fill unvoiced gaps by interpolating between voiced frames
        idx = np.arange(len(f0))
        f0 = np.interp(idx, idx[voiced], f0[voiced])

    # ---- STFT ------------------------------------------------------------
    S = np.abs(librosa.stft(y, n_fft=n_fft, hop_length=hop, window="hann"))
    n_bins, n_frames = S.shape
    f0 = np.resize(f0, n_frames)
    bin_hz = sr / n_fft

    # Hann window: sum(w) = N/2, and STFT magnitude of a sinusoid with
    # amplitude A peaks at A * sum(w) / 2. Undo that to get amplitudes.
    win_gain = np.sum(np.hanning(n_fft)) / 2.0

    # ---- harmonic envelopes ------------------------------------------------
    k = np.arange(1, n_partials + 1)[:, None]            # (P, 1)
    harm_hz = k * f0[None, :]                            # (P, F)
    harm_bin = harm_hz / bin_hz                          # fractional bin
    below_nyq = harm_bin < (n_bins - 2)

    lo = np.clip(np.floor(harm_bin).astype(int), 0, n_bins - 2)
    frac = np.clip(harm_bin - lo, 0.0, 1.0)
    cols = np.arange(n_frames)[None, :]
    mag = S[lo, cols] * (1 - frac) + S[lo + 1, cols] * frac
    env = np.where(below_nyq, mag / win_gain, 0.0).T     # (F, P)

    # ---- noise residual ----------------------------------------------------
    S_noise = S.copy()
    guard = 2  # bins zeroed on each side of a harmonic
    max_harm = int(np.max(harm_bin[below_nyq])) if np.any(below_nyq) else 0
    freqs_bin = np.arange(n_bins)
    for fi in range(n_frames):
        hb = harm_bin[:, fi][below_nyq[:, fi]]
        if len(hb) == 0:
            continue
        centers = np.round(hb).astype(int)
        for g in range(-guard, guard + 1):
            b = np.clip(centers + g, 0, n_bins - 1)
            S_noise[b, fi] = 0.0

    band_edges = np.geomspace(40.0, sr / 2.0, n_noise_bands + 1)
    band_centers = np.sqrt(band_edges[:-1] * band_edges[1:])
    noise_env = np.zeros((n_frames, n_noise_bands))
    bin_freqs = freqs_bin * bin_hz
    for b in range(n_noise_bands):
        sel = (bin_freqs >= band_edges[b]) & (bin_freqs < band_edges[b + 1])
        if not np.any(sel):
            continue
        # RMS magnitude of surviving bins in the band, window-compensated
        noise_env[:, b] = np.sqrt(np.mean(S_noise[sel, :] ** 2, axis=0)) / win_gain

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
        f0_ref=f0_ref,
        control_rate=control_rate,
        name=name,
        source_sr=sr,
        meta={
            "voiced_fraction": voiced_fraction,
            "pitch_stability": pitch_stability,
        },
    )
