#!/usr/bin/env python3
"""Run a defect's independently written compiler/simulator regressions.

Each JSON file supplies source variants and either a diagnostic, a reference
equation, fuse assertions, or simulation vectors containing expected results.
Commands run in fresh directories with deadlines; no external compiler is
needed.  Missing optional device data is reported as a CTest skip (77).
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def fuses(path):
    text = path.read_text(encoding="latin1")
    fields = [field.strip() for field in text.split("*")]
    counts = [field[2:] for field in fields if re.fullmatch(r"QF\d+", field)]
    assert len(counts) == 1, "missing or repeated QF"
    count = int(counts[0])
    default = next((int(field[1]) for field in fields if field in ("F0", "F1")), 0)
    bits = [default] * count
    for field in fields:
        match = re.fullmatch(r"L(\d+)\s+([01\s]+)", field)
        if not match:
            continue
        start = int(match.group(1))
        values = [int(c) for c in match.group(2) if c in "01"]
        if start + len(values) > count:
            raise AssertionError("JEDEC data exceeds QF")
        bits[start:start + len(values)] = values
    return bits


def run(tool, args, cwd, env, timeout):
    result = subprocess.run([str(tool), *args], cwd=cwd, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            timeout=timeout)
    log = result.stdout.decode("latin1").replace("\r", "")
    (cwd / (tool.name + ".log")).write_text(log)
    if result.returncode < 0:
        raise AssertionError(f"{tool.name} crashed: {result.returncode}\n{log}")
    return result.returncode, log


def check(bin_dir, library, case, variant, directory):
    device = variant.get("device", case["device"])
    env = dict(os.environ, LINCUPL_SHARE=str(library.parent))
    env.pop("LIBCUPL", None)
    timeout = variant.get("timeout", 10)
    flags = variant.get("flags", "-jaxm1")
    source = variant["source"]

    def compile_source(body, dest):
        dest.mkdir()
        text = f"Name test;\nDevice {device};\n" + body
        (dest / "test.pld").write_bytes(text.encode("ascii"))
        return run(bin_dir / "cupl", [flags, "-u", str(library), "test.pld"],
                   dest, env, timeout)

    actual = directory / "actual"
    rc, log = compile_source(source, actual)
    if "diagnostic" in variant:
        assert rc == 1, f"expected diagnostic failure, got {rc}\n{log}"
        assert re.search(variant["diagnostic"], log), log
        assert not (actual / "test.jed").exists(), "rejected design produced a JEDEC file"
        return
    assert rc == 0, log
    if "reference" in variant:
        reference = directory / "reference"
        rc, log = compile_source(variant["reference"], reference)
        assert rc == 0, "reference failed:\n" + log
        a, b = fuses(actual / "test.jed"), fuses(reference / "test.jed")
        assert a == b, f"fuse maps differ at {[i for i, (x, y) in enumerate(zip(a, b)) if x != y][:20]}"
    if "fuses" in variant:
        actual_bits = fuses(actual / "test.jed")
        for address, value in variant["fuses"].items():
            assert actual_bits[int(address)] == value, f"wrong value at fuse {address}"
    if "vectors" in variant:
        (actual / "test.si").write_text(variant["vectors"])
        for mode in variant.get("simulate", ["abs"]):
            rc, log = run(bin_dir / "csim", ["-lf" if mode == "jed" else "-l", "test"],
                          actual, env, timeout)
            assert rc == 0, f"{mode} simulation failed:\n{log}"
            listing = (actual / "test.so").read_text(encoding="latin1")
            assert re.search(r"^\d{4}:", listing, re.M), "no vectors were simulated"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("case", type=Path)
    parser.add_argument("--bin-dir", required=True, type=Path)
    parser.add_argument("--library", type=Path)
    parser.add_argument("--keep", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    bin_dir = args.bin_dir.resolve()
    candidates = [args.library, bin_dir.parent / "share/lincupl/lincupl.dl",
                  root / "build/share/lincupl/lincupl.dl",
                  root / "share/lincupl/lincupl.dl"]
    if os.environ.get("LINCUPL_SHARE"):
        candidates.insert(1, Path(os.environ["LINCUPL_SHARE"]) / "lincupl.dl")
    library = next((p.resolve() for p in candidates if p and p.is_file()), None)
    if library is None:
        print("SKIP: device library missing; pass --library or set LINCUPL_SHARE")
        return 77
    case = json.loads(args.case.read_text())
    failed = 0
    with tempfile.TemporaryDirectory(prefix="lincupl-defect-") as temporary:
        work = args.keep.resolve() if args.keep else Path(temporary)
        work.mkdir(parents=True, exist_ok=True)
        for number, variant in enumerate(case["variants"]):
            label = variant["label"]
            directory = work / str(number)
            directory.mkdir()
            try:
                check(bin_dir, library, case, variant, directory)
                print(f"PASS: {args.case.stem}: {label}")
            except (AssertionError, OSError, ValueError, subprocess.TimeoutExpired) as error:
                failed += 1
                print(f"FAIL: {args.case.stem}: {label}: {error}")
    return int(failed != 0)


if __name__ == "__main__":
    sys.exit(main())
