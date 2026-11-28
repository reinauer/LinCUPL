#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Check the PALCE16V8HD mode table through the g16v8h interfaces."""
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
    outputs = [23, 22, 20, 19, 16, 15, 14, 13]
    packages = [outputs, [27, 26, 24, 23, 19, 18, 17, 16]]
    checks = 0
    with tempfile.TemporaryDirectory(prefix='v8h-mode-') as tmp:
        root = Path(tmp)
        for mode in ('simple', 'complex', 'd', 't'):
            for automatic in (False, True):
                # The automatic catalog view only declares D selection.
                if automatic and mode == 't':
                    continue
                for inverted in (False, True):
                    maps = []
                    for lcc in (False, True):
                        suffix = '' if automatic else {'simple': 's', 'complex': 'ma',
                                                       'd': 'ms', 't': 'ms'}[mode]
                        device = 'g16v8h' + suffix + ('lcc' if lcc else '')
                        directory = root / f'{device}-{mode}-{inverted}'
                        directory.mkdir()
                        a, en, clk, oe = (3, 4, 2, 12) if lcc else (2, 3, 1, 10)
                        source = f'Name test; Device {device}; pin {a}=a; pin {en}=en;\n'
                        if mode in ('d', 't'):
                            source += f'pin {clk}=clk; pin {oe}=oe;\n'
                        for i, pin in enumerate(packages[lcc]):
                            source += f'pin {pin}={"!" if inverted else ""}q{i};\n'
                            source += f'q{i}{"." + mode if mode in ("d", "t") else ""}=a;\n'
                            if mode == 'complex':
                                source += f'q{i}.oe=en;\n'
                        (directory / 'test.pld').write_text(source)
                        result = subprocess.run([str(exe), '-jm1', '-u', str(library), 'test.pld'],
                                                cwd=directory, env=env, capture_output=True,
                                                text=True, timeout=20)
                        assert result.returncode == 0, (device, mode, result.stdout, result.stderr)
                        bits = fuses(directory / 'test.jed')
                        # AMD macrocell table: SG0, SG1, SL0 and SL5.
                        assert len(bits) == 2226
                        expected = (1, 0) if mode == 'simple' else (1, 1) if mode == 'complex' else (0, 1)
                        assert tuple(bits[2192:2194]) == expected, (device, mode, bits[2192:2194])
                        assert bits[2120:2128] == [int(mode == 'complex')] * 8
                        assert bits[2048:2056] == [int(not inverted)] * 8
                        if mode in ('d', 't'):
                            assert bits[2218:2226] == [int(mode == 'd')] * 8
                        maps.append(bits)
                        checks += 1
                    assert maps[0] == maps[1], (mode, automatic, inverted, 'package maps differ')
    print(f'PASS: {checks} GAL16V8H mode maps across all eight macrocells')


if __name__ == '__main__':
    main()
