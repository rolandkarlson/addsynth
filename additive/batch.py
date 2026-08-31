"""Batch import: a directory of audio files -> many .addm models.

For each audio file, sample random slices, analyze each, score it on
"how tonal is this" (pyin voiced fraction x pitch stability), and keep
the best `per_file` slices as models. Long polyphonic tracks work: the
scoring simply hunts for moments where something pitched dominates.
"""

from __future__ import annotations

import os
import re
import traceback

import numpy as np
import librosa
import soundfile as sf

from .analyze import analyze_signal
from .model import AdditiveModel
from .synth import render_note

AUDIO_EXTS = (".wav", ".aif", ".aiff", ".flac", ".mp3", ".m4a", ".ogg")

# slices below these gates only get used if nothing better is found
MIN_VOICED = 0.6
MIN_STABILITY = 0.5
MIN_RMS = 1e-3


def _slug(s: str) -> str:
    s = re.sub(r"[^A-Za-z0-9]+", "_", s).strip("_").lower()
    return s[:48] or "track"


def _normalize(model: AdditiveModel, peak: float = 0.25) -> None:
    """Scale amplitudes so models from differently-mastered sources
    sit at a comparable level in the plugin."""
    top = max(float(model.env.max()), 1e-9)
    g = peak / top
    model.env *= g
    model.noise_env *= g


def import_file(
    path: str,
    out_dir: str,
    per_file: int = 5,
    min_len: float = 1.5,
    max_len: float = 4.0,
    rng: np.random.Generator | None = None,
    preview: bool = True,
    n_partials: int | None = None,
    log=print,
) -> list[str]:
    rng = rng or np.random.default_rng()
    stem = _slug(os.path.splitext(os.path.basename(path))[0])
    duration = librosa.get_duration(path=path)

    candidates = []  # (score, meets_gates, model)
    max_attempts = per_file * 6
    passing = 0
    max_rms_seen = 0.0

    for attempt in range(max_attempts):
        if passing >= per_file:
            break
        if duration <= max_len:
            start, length = 0.0, duration
        else:
            length = float(rng.uniform(min_len, max_len))
            start = float(rng.uniform(0.0, duration - length))

        try:
            y, sr = librosa.load(path, sr=None, mono=True,
                                 offset=start, duration=length)
        except Exception as e:
            log(f"    [{stem}] load failed at {start:.1f}s: {e}")
            continue
        rms = float(np.sqrt(np.mean(y ** 2))) if len(y) else 0.0
        max_rms_seen = max(max_rms_seen, rms)
        if rms < MIN_RMS:
            continue

        try:
            m = analyze_signal(y, sr, n_partials=n_partials, fmin=40.0,
                               name=f"{stem}", f0_override=None)
        except ValueError:
            continue  # nothing pitched in this slice
        except Exception:
            log(f"    [{stem}] analyze error at {start:.1f}s:\n"
                + traceback.format_exc(limit=2))
            continue

        vf = m.meta.get("voiced_fraction", 0.0)
        st = m.meta.get("pitch_stability", 0.0)
        ok = vf >= MIN_VOICED and st >= MIN_STABILITY
        passing += int(ok)
        m.meta.update(source=path, offset_sec=round(start, 2),
                      slice_sec=round(length, 2))
        candidates.append((vf * st, ok, m))

        if duration <= max_len:
            break  # short file: one slice is all there is

    if not candidates:
        if max_rms_seen < MIN_RMS:
            log(f"  {stem}: the audio is SILENT (peak rms "
                f"{max_rms_seen:.6f}) — check the export/bounce")
        else:
            log(f"  {stem}: no usable pitched slices found, skipped")
        return []

    # prefer gate-passing slices, then higher score
    candidates.sort(key=lambda c: (c[1], c[0]), reverse=True)
    chosen = candidates[:per_file]

    os.makedirs(out_dir, exist_ok=True)
    written = []
    for i, (score, ok, m) in enumerate(chosen):
        m.name = f"{stem}_{i:02d}"
        _normalize(m)
        out = os.path.join(out_dir, f"{m.name}.addm")
        m.save(out)
        written.append(out)
        if preview:
            pdir = os.path.join(out_dir, "previews")
            os.makedirs(pdir, exist_ok=True)
            wav = render_note(m, sr=44100, seed=0)
            sf.write(os.path.join(pdir, f"{m.name}.wav"), wav, 44100)
        note = librosa.hz_to_note(m.f0_ref)
        log(f"  {m.name}: f0={m.f0_ref:.0f} Hz ({note}) "
            f"@{m.meta['offset_sec']}s len={m.meta['slice_sec']}s "
            f"voiced={m.meta['voiced_fraction']:.2f} "
            f"stable={m.meta['pitch_stability']:.2f}"
            + ("" if ok else "  (below gates, best available)"))
    return written


def import_dir(
    src_dir: str,
    out_dir: str,
    per_file: int = 5,
    min_len: float = 1.5,
    max_len: float = 4.0,
    seed: int | None = None,
    preview: bool = True,
    log=print,
) -> list[str]:
    rng = np.random.default_rng(seed)
    files = sorted(
        os.path.join(src_dir, f) for f in os.listdir(src_dir)
        if f.lower().endswith(AUDIO_EXTS) and not f.startswith(".")
    )
    log(f"importing {len(files)} audio files from {src_dir} -> {out_dir}")
    all_written = []
    for n, path in enumerate(files, 1):
        log(f"[{n}/{len(files)}] {os.path.basename(path)}")
        try:
            all_written += import_file(
                path, out_dir, per_file=per_file, min_len=min_len,
                max_len=max_len, rng=rng, preview=preview, log=log)
        except Exception:
            log(f"  FAILED:\n{traceback.format_exc(limit=3)}")
    log(f"done: {len(all_written)} models written to {out_dir}")
    return all_written
