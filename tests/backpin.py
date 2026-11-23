"""Backups preserve source bytes and cannot name the source or a hard link."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

tool = str(Path(sys.argv[1]).resolve())
original = b"Name test;\r\nDevice g16v8;\r\nPIN 2 = A;\r\nPIN 19 = Y;\r\nY = A;\r\n"
with tempfile.TemporaryDirectory(prefix="backpin ") as tmp:
    root = Path(tmp)
    source = root / "test.pld"
    backup = root / "test.bak"
    source.write_bytes(original)
    backup.write_bytes(b"previous backup")

    def run(destination, code):
        p = subprocess.run([tool, "-f", "-b", str(destination), "g16v8", "test.pld"],
                           cwd=root, capture_output=True, timeout=10)
        assert p.returncode == code, (p.returncode, p.stdout, p.stderr)

    run(backup, 0)
    assert backup.read_bytes() == original
    assert source.read_bytes() != original
    source.write_bytes(original)
    run(source, 1)
    assert source.read_bytes() == original
    alias = root / "alias.pld"
    os.link(source, alias)
    run(alias, 1)
    assert source.read_bytes() == original and alias.read_bytes() == original
