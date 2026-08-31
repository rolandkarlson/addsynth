"""Render AdditiveModel instances to audio.

Sine bank + shaped noise. Everything is vectorized numpy; offline
rendering, meant for fast iteration (the real-time version is the JUCE
plugin's job).
"""

from __future__ import annotations

import numpy as np
import librosa

from .model import AdditiveModel


def _control_to_audio(values: np.ndarray, control_rate: float,
                      n_samples: int, sr: int,
                      stretch: float = 1.0) -> np.ndarray:
    """Linear-interpolate control frames (F,) or (F, C) to n_samples.

    stretch > 1 slows the envelope down (longer note), < 1 speeds it up.
    """
    n_frames = values.shape[0]
    t = np.arange(n_samples) / sr                     # seconds, synth time
    pos = np.clip(t * control_rate / stretch, 0, n_frames - 1)
    i0 = np.floor(pos).astype(int)
    i1 = np.minimum(i0 + 1, n_frames - 1)
    frac = (pos - i0)
    if values.ndim == 1:
        return values[i0] * (1 - frac) + values[i1] * frac
    frac = frac[:, None]
    return values[i0] * (1 - frac) + values[i1] * frac


def render_note(
    model: AdditiveModel,
    f0: float | None = None,
    midi: float | None = None,
    duration: float | None = None,
    sr: int = 44100,
    gain: float = 1.0,
    noise_gain: float = 1.0,
    harmonic_gain: float = 1.0,
    use_pitch_track: bool = True,
    inharmonicity: float | None = None,
    seed: int | None = None,
) -> np.ndarray:
    """Render the model at an arbitrary pitch.

    f0/midi: target fundamental (defaults to the model's own f0_ref).
    duration: seconds; the envelope matrix is time-stretched to fit
              (default: the analyzed duration).
    """
    if midi is not None:
        f0 = 440.0 * 2 ** ((midi - 69) / 12)
    if f0 is None:
        f0 = model.f0_ref
    if duration is None:
        duration = model.duration
    stretch = duration / model.duration
    B = model.inharmonicity if inharmonicity is None else inharmonicity

    n = int(round(duration * sr))
    out = np.zeros(n)

    # ---- harmonic part ---------------------------------------------------
    if harmonic_gain > 0:
        amp = _control_to_audio(model.env.astype(np.float64),
                                model.control_rate, n, sr, stretch)   # (N, P)
        if use_pitch_track:
            ratio = _control_to_audio(model.f0_track.astype(np.float64),
                                      model.control_rate, n, sr, stretch)
        else:
            ratio = np.ones(n)

        k = np.arange(1, model.n_partials + 1, dtype=np.float64)
        kfac = k * np.sqrt(1.0 + B * k * k) * model.detune        # (P,)
        freqs = (f0 * ratio)[:, None] * kfac[None, :]             # (N, P)
        amp = np.where(freqs < sr / 2 * 0.98, amp, 0.0)           # anti-alias
        phase = 2 * np.pi * np.cumsum(freqs, axis=0) / sr
        out += harmonic_gain * np.sum(amp * np.sin(phase), axis=1)

    # ---- noise part --------------------------------------------------------
    if noise_gain > 0 and model.noise_env.size and model.noise_env.max() > 0:
        rng = np.random.default_rng(seed)
        noise = rng.standard_normal(n)
        n_fft, hop = 2048, 512
        Z = librosa.stft(noise, n_fft=n_fft, hop_length=hop)
        n_bins, n_zframes = Z.shape
        bin_freqs = np.arange(n_bins) * sr / n_fft

        # target band RMS magnitude per synth frame
        zt = np.arange(n_zframes) * hop / sr
        pos = np.clip(zt * model.control_rate / stretch, 0, model.n_frames - 1)
        i0 = np.floor(pos).astype(int)
        i1 = np.minimum(i0 + 1, model.n_frames - 1)
        fr = (pos - i0)[:, None]
        target = model.noise_env[i0] * (1 - fr) + model.noise_env[i1] * fr  # (Fz, B)

        # interpolate band targets across bins (log-frequency axis)
        band_f = model.noise_band_freqs.astype(np.float64)
        tgt_bins = np.empty((n_bins, n_zframes))
        logf = np.log(np.maximum(bin_freqs, 1.0))
        logb = np.log(band_f)
        for fi in range(n_zframes):
            tgt_bins[:, fi] = np.interp(logf, logb, target[fi])

        # normalize: white noise through STFT has roughly uniform expected
        # magnitude per bin; scale to match target RMS per bin
        cur = np.abs(Z) + 1e-12
        cur_rms = np.sqrt(np.mean(cur ** 2))
        win_gain = np.sum(np.hanning(n_fft)) / 2.0
        Z_shaped = Z / cur_rms * tgt_bins * win_gain
        shaped = librosa.istft(Z_shaped, n_fft=n_fft, hop_length=hop, length=n)
        out += noise_gain * shaped

    peak = np.max(np.abs(out))
    if peak > 1e-9:
        out = out / max(peak, 1.0)  # only attenuate if clipping
    return (gain * out).astype(np.float32)


def render_sequence(
    model: AdditiveModel,
    notes: list[tuple[float, float, float]],  # (start_sec, midi, duration_sec)
    sr: int = 44100,
    **kw,
) -> np.ndarray:
    """Render a list of (start, midi, duration) notes into one buffer."""
    end = max(s + d for s, _, d in notes) + 1.0
    out = np.zeros(int(end * sr), dtype=np.float64)
    for start, midi, dur in notes:
        y = render_note(model, midi=midi, duration=dur, sr=sr, **kw)
        a = int(start * sr)
        out[a:a + len(y)] += y
    peak = np.max(np.abs(out))
    if peak > 1.0:
        out /= peak
    return out.astype(np.float32)
