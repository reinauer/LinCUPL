#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Automatic device selection must reserve registers for T equations."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from defects import fuses


def main():
    bindir = Path(sys.argv[1]).resolve()
    library = bindir.parent / 'share/lincupl/lincupl.dl'
    env = dict(os.environ, LINCUPL_SHARE=str(library.parent))
    env.pop('LIBCUPL', None)
    env.pop('FITTERDIR', None)
    exe = bindir / ('cupl.exe' if os.name == 'nt' else 'cupl')
    checks = 0
    with tempfile.TemporaryDirectory(prefix='automatic-toggle-') as tmp:
        root = Path(tmp)
        for lcc in (False, True):
            pins = [27, 26, 24, 23, 19, 18, 17, 16] if lcc else [23, 22, 20, 19, 16, 15, 14, 13]
            clk, a, oe = (2, 3, 12) if lcc else (1, 2, 10)
            for mixed in ('t', 'd', 'oe'):
                for inverted in (False, True):
                    maps = []
                    for automatic in (False, True):
                        device = 'g16v8h' + ('' if automatic else 'ms') + ('lcc' if lcc else '')
                        directory = root / f'{device}-{mixed}-{inverted}'
                        directory.mkdir()
                        source = (f'Name test; Device {device}; pin {clk}=clk; '
                                  f'pin {a}=a; pin {oe}=disable;\n')
                        for i, pin in enumerate(pins):
                            ext = mixed if i == 7 else 't'
                            source += f'pin {pin}={"!" if inverted else ""}q{i};\n'
                            source += (f'q{i}=a; q{i}.oe=a;\n' if ext == 'oe' else f'q{i}.{ext}=a;\n')
                        (directory / 'test.pld').write_text(source)
                        p = subprocess.run([str(exe), '-jm1', '-u', str(library), 'test.pld'],
                                           cwd=directory, env=env, capture_output=True, text=True, timeout=20)
                        assert p.returncode == 0, (device, mixed, p.stdout, p.stderr)
                        bits = fuses(directory / 'test.jed')
                        assert bits[2192:2194] == [0, 1], 'T equations require registered mode'
                        assert bits[2218:2225] == [0] * 7
                        if mixed != 'oe':
                            assert bits[2225] == int(mixed == 'd')
                        maps.append(bits)
                        checks += 1
                    assert maps[0] == maps[1], (device, mixed, 'automatic and fixed modes differ')
    print(f'PASS: {checks} automatic/fixed T-register compilations')


if __name__ == '__main__':
    main()
