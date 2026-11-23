"""Exercise the extracted Windows ZIP without build tools on PATH."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import zipfile

archive = Path(sys.argv[1]).resolve()
share = Path(sys.argv[2]).resolve()
with tempfile.TemporaryDirectory(prefix="LinCUPL package ") as tmp:
    root = Path(tmp)
    with zipfile.ZipFile(archive) as z:
        z.extractall(root)
    package, = root.iterdir()
    bindir = package / "bin"
    env = dict(os.environ)
    for key in ("QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH", "QTDIR",
                "QT_ROOT", "LIBCUPL", "LINCUPL_SHARE", "FITTERDIR"):
        env.pop(key, None)
    env["PATH"] = str(Path(env["SYSTEMROOT"]) / "System32")
    env["QT_QPA_PLATFORM"] = "windows"

    def run(tool, *args, code=0):
        p = subprocess.run([str(bindir / (tool + ".exe")), *args],
                           cwd=root, env=env, capture_output=True, timeout=90)
        assert p.returncode == code, (tool, p.returncode, p.stdout, p.stderr)
        return p

    for gui in ("lincupl", "linsim"):
        run(gui, "--quit-after", "100")
    run("jedinspect", "--help")
    run("jedcompare", "--help")

    # Device data is downloaded for tests, not distributed in the archive.
    shutil.copytree(share, package / "share/lincupl", dirs_exist_ok=True)
    (root / "test.pld").write_text(
        "Name test;\nDevice g16v8;\nPIN 2 = A;\nPIN 19 = Y;\nY = !A;\n")
    (root / "test.si").write_text("ORDER: A, Y;\nVECTORS:\n0 H\n1 L\n")
    run("cupl", "-jaxm1", str(root / "test.pld"))
    run("csim", "-l", "test")
    run("csim", "-fl", "test")
    report = run("jedinspect", "--json", "test.jed")
    assert b'"fuses"' in report.stdout
    # Exercise the compiler's launch of a sibling ATF150x fitter too.
    for part, pins in (("1502", 44), ("1504", 44), ("1508", 84)):
        (root / "test.pld").write_text(
            f"Name test;\nDevice f{part}plcc{pins};\n"
            "PIN 2 = A;\nPIN 4 = Y;\nY = A;\n")
        (root / "test.jed").unlink()
        result = run("cupl", "-jx", "test.pld")
        assert b"Design fits successfully" in result.stdout
        assert (root / "test.jed").stat().st_size > 0
print("Packaged CLI tools, fitters and Qt applications passed")
