"""Round-trip demo: analyze real sounds -> .addm models -> resynthesize.

Outputs land in out/:
  <name>_original.wav      the source (converted to wav for easy A/B)
  <name>_resynth.wav       model played back at its own pitch/duration
  <name>_transposed.wav    same model, -7 and +5 semitones
  <name>_melody.wav        a little tune played with the model
  models/<name>.addm       the model file (the thing JUCE will load)
"""

import os
import numpy as np
import librosa
import soundfile as sf

from additive import analyze, render_note, render_sequence

SR = 44100
os.makedirs("out", exist_ok=True)
os.makedirs("models", exist_ok=True)

SOURCES = [
    ("/System/Library/Sounds/Glass.aiff", "glass"),
    ("/System/Library/Sounds/Ping.aiff", "ping"),
    ("/System/Library/Sounds/Submarine.aiff", "submarine"),
]

# synthetic sanity check: saw wave with vibrato + noise burst attack
def make_test_wav(path):
    sr = SR
    t = np.arange(int(sr * 2.0)) / sr
    f0 = 220 * (1 + 0.01 * np.sin(2 * np.pi * 5 * t))       # 5 Hz vibrato
    phase = 2 * np.pi * np.cumsum(f0) / sr
    saw = sum(np.sin(k * phase) / k for k in range(1, 41))
    envl = np.minimum(t / 0.02, 1.0) * np.exp(-t * 1.5)
    y = saw * envl * 0.3
    y[: int(0.05 * sr)] += np.random.default_rng(0).standard_normal(int(0.05 * sr)) * 0.1 \
        * np.exp(-np.arange(int(0.05 * sr)) / (0.01 * sr))
    sf.write(path, y.astype(np.float32), sr)

make_test_wav("out/testsaw_source.wav")
SOURCES.insert(0, ("out/testsaw_source.wav", "testsaw"))

MELODY = [  # (start, midi, dur)
    (0.0, 57, 0.9), (0.5, 60, 0.9), (1.0, 64, 0.9), (1.5, 67, 1.4),
    (2.5, 65, 0.9), (3.0, 64, 0.9), (3.5, 60, 1.8),
]

for path, name in SOURCES:
    print(f"=== {name} ({path})")
    try:
        model = analyze(path, name=name)
    except ValueError as e:
        print("  skipped:", e)
        continue

    model.save(f"models/{name}.addm")
    note = librosa.hz_to_note(model.f0_ref)
    print(f"  f0_ref={model.f0_ref:.1f} Hz ({note}), "
          f"{model.n_frames} frames x {model.n_partials} partials, "
          f"{model.duration:.2f}s, control_rate={model.control_rate:.0f} Hz")

    y, sr = librosa.load(path, sr=SR, mono=True)
    sf.write(f"out/{name}_original.wav", y, SR)

    resynth = render_note(model, sr=SR, seed=1)
    sf.write(f"out/{name}_resynth.wav", resynth, SR)

    down = render_note(model, f0=model.f0_ref * 2 ** (-7 / 12), sr=SR, seed=1)
    up = render_note(model, f0=model.f0_ref * 2 ** (5 / 12), sr=SR, seed=1)
    gap = np.zeros(int(0.25 * SR), dtype=np.float32)
    sf.write(f"out/{name}_transposed.wav", np.concatenate([down, gap, up]), SR)

    mel = render_sequence(model, MELODY, sr=SR, seed=1)
    sf.write(f"out/{name}_melody.wav", mel, SR)

    # quick objective check: log-mel distance original vs resynth
    def logmel(x):
        m = librosa.feature.melspectrogram(y=x, sr=SR, n_mels=64)
        return librosa.power_to_db(m + 1e-9)
    n = min(len(y), len(resynth))
    d = np.mean(np.abs(logmel(y[:n]) - logmel(resynth[:n])))
    print(f"  log-mel L1 distance original vs resynth: {d:.2f} dB (lower=better)")

print("\nDone. Listen to files in out/, models in models/")
