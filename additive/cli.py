"""Command line interface.

  python -m additive.cli analyze input.wav [-o models/name.addm] [--f0 220]
  python -m additive.cli play models/name.addm [-o out.wav] [--midi 60] [--dur 2]
  python -m additive.cli morph a.addm b.addm 0.5 [-o out.addm]
  python -m additive.cli mutate in.addm [-n 8] [--amount 0.4] [-o outdir]
"""

from __future__ import annotations

import argparse
import os

import numpy as np
import soundfile as sf

from . import analyze as _analyze, render_note, AdditiveModel
from . import mutate as _mut


def main() -> None:
    ap = argparse.ArgumentParser(prog="additive")
    sub = ap.add_subparsers(dest="cmd", required=True)

    a = sub.add_parser("analyze", help="audio file -> .addm model")
    a.add_argument("input")
    a.add_argument("-o", "--output")
    a.add_argument("--partials", type=int, default=None, help="default: adaptive")
    a.add_argument("--f0", type=float, help="skip pitch detection, use this Hz")
    a.add_argument("--fmin", type=float, default=50.0)
    a.add_argument("--fmax", type=float, default=2000.0)

    p = sub.add_parser("play", help="render a model to wav")
    p.add_argument("model")
    p.add_argument("-o", "--output")
    p.add_argument("--midi", type=float)
    p.add_argument("--f0", type=float)
    p.add_argument("--dur", type=float)
    p.add_argument("--sr", type=int, default=44100)

    m = sub.add_parser("morph", help="interpolate two models")
    m.add_argument("a")
    m.add_argument("b")
    m.add_argument("t", type=float)
    m.add_argument("-o", "--output")

    imf = sub.add_parser("import-file", help="one audio file -> up to N models")
    imf.add_argument("input")
    imf.add_argument("-o", "--outdir", default="models")
    imf.add_argument("-n", "--per-file", type=int, default=5)
    imf.add_argument("--seed", type=int)

    im = sub.add_parser("import", help="directory of audio -> many models")
    im.add_argument("dir")
    im.add_argument("-o", "--outdir", default="models/imported")
    im.add_argument("-n", "--per-file", type=int, default=5)
    im.add_argument("--min-len", type=float, default=1.5)
    im.add_argument("--max-len", type=float, default=4.0)
    im.add_argument("--seed", type=int)
    im.add_argument("--no-preview", action="store_true")

    ev = sub.add_parser("evolve", help="evolve a model toward a reference sound")
    ev.add_argument("reference", help="reference audio file")
    ev.add_argument("base", help="starting .addm model")
    ev.add_argument("-o", "--output")
    ev.add_argument("--gens", type=int, default=20)
    ev.add_argument("--pop", type=int, default=16)
    ev.add_argument("--f0", type=float, help="reference f0 if pitch detection fails")
    ev.add_argument("--seed", type=int)

    br = sub.add_parser("breed", help="interactive breeding (you pick favourites)")
    br.add_argument("action", choices=["start", "next", "pick"])
    br.add_argument("args", nargs="*", help="start: base.addm | next: A B | pick: N")
    br.add_argument("-o", "--session", default="breeding/session")
    br.add_argument("-n", type=int, default=8)
    br.add_argument("--amount", type=float)
    br.add_argument("--dest", default="models")
    br.add_argument("--seed", type=int)

    mu = sub.add_parser("mutate", help="random variations of a model")
    mu.add_argument("model")
    mu.add_argument("-n", type=int, default=8)
    mu.add_argument("--amount", type=float, default=0.35)
    mu.add_argument("-o", "--outdir", default="mutations")
    mu.add_argument("--seed", type=int)

    args = ap.parse_args()

    if args.cmd == "analyze":
        model = _analyze(args.input, n_partials=args.partials,
                         f0_override=args.f0, fmin=args.fmin, fmax=args.fmax)
        out = args.output or os.path.splitext(args.input)[0] + ".addm"
        model.save(out)
        print(f"{out}: f0={model.f0_ref:.1f} Hz, {model.n_frames} frames, "
              f"{model.duration:.2f}s")

    elif args.cmd == "play":
        model = AdditiveModel.load(args.model)
        y = render_note(model, f0=args.f0, midi=args.midi,
                        duration=args.dur, sr=args.sr)
        out = args.output or os.path.splitext(args.model)[0] + "_play.wav"
        sf.write(out, y, args.sr)
        print(out)

    elif args.cmd == "import-file":
        from .batch import import_file
        written = import_file(args.input, args.outdir, per_file=args.per_file,
                              rng=np.random.default_rng(args.seed))
        print(f"{len(written)} model(s) from {os.path.basename(args.input)}")

    elif args.cmd == "import":
        from .batch import import_dir
        import_dir(args.dir, args.outdir, per_file=args.per_file,
                   min_len=args.min_len, max_len=args.max_len,
                   seed=args.seed, preview=not args.no_preview)

    elif args.cmd == "morph":
        ma, mb = AdditiveModel.load(args.a), AdditiveModel.load(args.b)
        mm = _mut.morph(ma, mb, args.t)
        out = args.output or f"{mm.name}.addm"
        mm.save(out)
        print(out)

    elif args.cmd == "evolve":
        from .evolve import evolve
        base = AdditiveModel.load(args.base)
        res = evolve(args.reference, base, generations=args.gens,
                     population=args.pop, f0_override=args.f0, seed=args.seed)
        out = args.output or os.path.splitext(args.base)[0] + "_evolved.addm"
        res.model.save(out)
        wav = os.path.splitext(out)[0] + ".wav"
        sf.write(wav, render_note(res.model, sr=44100), 44100)
        print(f"best fitness {res.fitness:.3f} dB -> {out} (+ preview {wav})")

    elif args.cmd == "breed":
        from . import breed as B
        if args.action == "start":
            if len(args.args) != 1:
                raise SystemExit("usage: breed start base.addm -o sessiondir")
            B.start(args.args[0], args.session, n=args.n,
                    amount=args.amount or 0.4, seed=args.seed)
        elif args.action == "next":
            if len(args.args) != 2:
                raise SystemExit("usage: breed next A B -o sessiondir")
            B.next_gen(args.session, int(args.args[0]), int(args.args[1]),
                       n=args.n, amount=args.amount or 0.25, seed=args.seed)
        else:
            if len(args.args) != 1:
                raise SystemExit("usage: breed pick N -o sessiondir")
            B.pick(args.session, int(args.args[0]), dest=args.dest)

    elif args.cmd == "mutate":
        model = AdditiveModel.load(args.model)
        rng = np.random.default_rng(args.seed)
        os.makedirs(args.outdir, exist_ok=True)
        for i in range(args.n):
            v = _mut.mutate(model, amount=args.amount, rng=rng)
            path = os.path.join(args.outdir, f"{model.name}_mut{i:02d}.addm")
            v.save(path)
            wav = path.replace(".addm", ".wav")
            sf.write(wav, render_note(v, sr=44100), 44100)
            print(path)


if __name__ == "__main__":
    main()
