"""Fit without support files and compare with an optional gate library."""
import itertools
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from defects import fuses

bindir = Path(sys.argv[1]).resolve()
suffix = '.exe' if os.name == 'nt' else ''
compiler = bindir / ('cupl' + suffix)
library = bindir.parent / 'share/lincupl/lincupl.dl'
reference = Path(__file__).resolve().parent.parent / 'share/lincupl/Fitters'
env = dict(os.environ, LIBCUPL=str(library))
for key in ('LINCUPL_SHARE', 'LINCUPL_FITTERS', 'FITTERDIR'):
    env.pop(key, None)

with tempfile.TemporaryDirectory(prefix='fit150x builtin ') as tmp:
    root = Path(tmp)
    empty = root / 'empty'
    empty.mkdir()
    work = root / 'work'
    work.mkdir()
    checked = compared = 0

    def compile_design(source, support, mode, success=True):
        (work / 'test.pld').write_text(source)
        jed = work / 'test.jed'
        if jed.exists():
            jed.unlink()
        child_env = dict(env, FITTERDIR=str(support), LINCUPL_FIT150X_MODE=mode)
        result = subprocess.run([str(compiler), '-jx', 'test.pld'], cwd=work,
                                env=child_env, capture_output=True, timeout=30)
        output = (result.stdout + result.stderr).decode(errors='replace')
        if success:
            assert result.returncode == 0, (source, mode, output)
            assert 'Design fits successfully' in output, output
            return fuses(jed)
        assert result.returncode != 0, output

    def check(source, mode):
        global checked, compared
        actual = compile_design(source, empty, mode)
        checked += 1
        if (reference / 'atmel.std').is_file():
            expected = compile_design(source, reference, mode)
            assert actual == expected, (source, mode, 'fuse maps differ')
            compared += 1

    for mode in ('legacy', 'extended'):
        for part, pins in (('1502', 44), ('1504', 44), ('1508', 84)):
            header = (f'Name test;\nDevice f{part}plcc{pins};\n'
                      'PIN 2=A;\nPIN 4=Y;\nPIN 1=CLK;\n')
            check(header + 'Y=A;\n', mode)
            check(header + 'Y=!A;\nY.oe=CLK;\n', mode)
            for register, inverted, falling, reset, enable in itertools.product(
                    ('d', 't', 'lq'), (False, True), (False, True),
                    (False, True), (False, True)):
                source = header
                source += f'Y.{register}={"!" if inverted else ""}A;\n'
                source += f'Y.ck={"!" if falling else ""}CLK;\n'
                if reset:
                    source += 'Y.ar=!A;\n'
                if enable:
                    source += 'Y.oe=A;\n'
                check(source, mode)

    # A supplied but invalid library must still fail, not use the fallback.
    (empty / 'atmel.std').write_text('not_a_gate_library\n')
    compile_design(header + 'Y=A;\n', empty, 'legacy', success=False)

print(f'PASS: {checked} fits without support files; '
      f'{compared} matching reference fuse maps; invalid library rejected')
