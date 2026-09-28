"""Decompile PS4 module functions with headless Ghidra.

  python tools/re/decompile.py <module.sprx> <out.c> <func> [<func> ...]

<func> is a readable name known from reference/shadps4 (e.g. sceVrTrackerGetResult),
an 11-char NID, or name=0xADDR for an internal function. All exports are named in
Ghidra so that decompiled calls between them are readable; only requested
functions are printed. The Ghidra project is cached under tools/re/ghidra/projects.
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(__file__))
from prx import Prx, known_names, nid  # noqa: E402

GHIDRA = os.environ.get("GHIDRA_HOME", r"C:\Users\habbo\tools\ghidra_12.1.4_PUBLIC")
HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    module, out = sys.argv[1], os.path.abspath(sys.argv[2])
    wanted = sys.argv[3:]
    prx = Prx(module)
    names = known_names()
    by_nid = {}
    for n, va, _ in prx.exports():
        short = n.split("#")[0]
        by_nid[short] = (names.get(short, "nid_" + short.replace("+", "_").replace("-", "_")), va)

    args = []
    # every export, named, but not printed
    for short, (name, va) in by_nid.items():
        args.append(f"{name}:{va:x}:skip")
    # imported functions: name their PLT stubs
    for n, va in prx.plt_stubs().items():
        short = n.split("#")[0]
        args.append(f"{names.get(short, 'imp_' + short.replace('+', '_').replace('-', '_'))}:{va:x}:skip")
    for w in wanted:
        if "=" in w:
            name, addr = w.split("=")
            args.append(f"{name}:{int(addr, 16):x}")
            continue
        key = w if w in by_nid else nid(w)
        if key not in by_nid:
            sys.exit(f"{w}: not exported")
        name, va = by_nid[key]
        args.append(f"{name}:{va:x}")

    proj_dir = os.path.join(HERE, "ghidra", "projects")
    os.makedirs(proj_dir, exist_ok=True)
    proj = os.path.splitext(os.path.basename(module))[0]
    exists = os.path.exists(os.path.join(proj_dir, proj + ".gpr"))
    cmd = [os.path.join(GHIDRA, "support", "analyzeHeadless.bat"), proj_dir, proj]
    if exists:
        cmd += ["-process", os.path.basename(module), "-noanalysis"]
    else:
        cmd += ["-import", os.path.abspath(module), "-loader", "ElfLoader", "-loader-imagebase", "0"]
        if os.path.getsize(module) > 8 * 1024 * 1024:
            cmd += ["-noanalysis"]  # big executables: decompile on demand only
    args_file = out + ".args"
    with open(args_file, "w") as f:
        f.write("\n".join(args))
    cmd += ["-scriptPath", os.path.join(HERE, "ghidra"), "-postScript", "DecompileAt.java", out, "@" + args_file]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if os.environ.get("RE_VERBOSE"):
        print(r.stdout[-6000:], r.stderr[-3000:])
    if r.returncode != 0 or not os.path.exists(out) or os.path.getsize(out) == 0:
        print(r.stdout[-4000:], r.stderr[-4000:])
        sys.exit("ghidra failed")
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
