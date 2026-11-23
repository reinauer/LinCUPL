"""Run the fitter launcher from a directory and filename containing spaces."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix="lincupl tools ") as tmp:
    source = Path(sys.argv[1]).resolve()
    program = Path(tmp) / ("fitter test" + source.suffix)
    shutil.copy2(source, program)
    env = dict(os.environ, PATH=tmp + os.pathsep + os.environ.get("PATH", ""))
    env.pop("FITTERDIR", None)
    subprocess.run([str(program)], env=env, check=True, timeout=20)
    env["FITTERDIR"] = "inherited support directory"
    subprocess.run([str(program)], env=env, check=True, timeout=20)
