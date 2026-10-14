"""Compile the PyTorch kernel with Brrr and check it against torch.

    python examples/build.py [--brrr PATH] [--t 4,64,512] [--reps N] [--work DIR]

The whole path, one command:

    kernel.br (from kernel.py, torch.export)
      -> brrr run --passes=lower-aten        loop nests over buffers, every access proven in bounds
      -> brrr to-llvm                        LLVM IR text
      -> clang -O3 -march=native             a native binary (with harness.c)
      -> run against torch's own output      max abs error, and time per call vs torch

The kernel is specialized for nothing yet: these are the naive loops, the
baseline the agent's niching starts from.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)


def find_brrr(given):
    if given:  # absolute: Windows does not resolve a relative program path with '/'
        return os.path.abspath(given) if os.path.exists(given) else given
    for cand in [os.path.join(HERE, "..", "src", "build", "brrr.exe"),
                 os.path.join(HERE, "..", "src", "build", "brrr")]:
        if os.path.exists(cand):
            return os.path.abspath(cand)
    return shutil.which("brrr") or "brrr"


def sh(cmd, out=None):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("failed: %s\n%s%s" % (" ".join(cmd), r.stdout, r.stderr))
    if out:
        with open(out, "w", newline="\n") as f:
            f.write(r.stdout)
    return r.stdout


def time_torch(model, x, reps, threads):
    import torch

    old = torch.get_num_threads()
    torch.set_num_threads(threads)
    with torch.no_grad():
        model(x)
        start = time.perf_counter()
        for _ in range(reps):
            model(x)
        ms = (time.perf_counter() - start) * 1e3 / reps
    torch.set_num_threads(old)
    return ms


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--brrr", help="the brrr binary (default: src/build/brrr)")
    ap.add_argument("--t", default="4,64,512", help="token counts are 8 * t; comma-separated")
    ap.add_argument("--reps", type=int, default=20)
    ap.add_argument("--work", help="keep the intermediate files here")
    ap.add_argument("--no-torch-timing", action="store_true")
    args = ap.parse_args()

    import torch
    import kernel

    brrr = find_brrr(args.brrr)
    work = args.work or tempfile.mkdtemp(prefix="brrr-build-")
    os.makedirs(work, exist_ok=True)
    store = os.path.join(work, "store")

    sh([brrr, "run", os.path.join(HERE, "kernel.br"), "--passes=lower-aten",
        "--schemas=" + os.path.join(HERE, "aten.bs"), "--store=" + store])
    lowered = os.path.join(work, "kernel_lowered.br")
    sh([brrr, "show", "r1", "--store=" + store], out=lowered)
    ll = os.path.join(work, "kernel.ll")
    sh([brrr, "to-llvm", lowered], out=ll)
    exe = os.path.join(work, "kernel.exe")
    sh(["clang", "-O3", "-march=native", "-w", ll, os.path.join(HERE, "harness.c"), "-o", exe])
    print("compiled: %s" % exe)

    print("%8s  %12s  %12s  %12s  %12s" % ("tokens", "max abs err", "brrr ms", "torch ms", "torch 1t ms"))
    ok = True
    for t in [int(v) for v in args.t.split(",")]:
        data = os.path.join(work, "t%d" % t)
        model, x = kernel.dump_data(data, t)
        r = subprocess.run([exe, data, str(t), str(args.reps)], capture_output=True, text=True)
        fields = dict(kv.split("=") for kv in r.stdout.split())
        ok = ok and r.returncode == 0
        if args.no_torch_timing:
            tt = t1 = float("nan")
        else:
            tt = time_torch(model, x, args.reps, torch.get_num_threads())
            t1 = time_torch(model, x, args.reps, 1)
        print("%8s  %12s  %12s  %12.4f  %12.4f" % (fields["tokens"], fields["max_abs_err"],
                                                   fields["ms_per_call"], tt, t1))
    print("match: torch and brrr agree within 1e-4" if ok else "MISMATCH: see max abs err")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
