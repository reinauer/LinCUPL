#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Check the GAL16V8H simple-mode input shared with the enable pin."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from csim_jed import vector_lines
from defects import fuses


def main():
    bindir = Path(sys.argv[1]).resolve()
    library = bindir.parent / 'share/lincupl/lincupl.dl'
    env = dict(os.environ, LINCUPL_SHARE=str(library.parent))
    env.pop('LIBCUPL', None)
    env.pop('FITTERDIR', None)
    with tempfile.TemporaryDirectory(prefix='gal16v8h-pin-') as tmp:
        root = Path(tmp)

        def run(tool, args, directory, code=0):
            exe = bindir / (tool + ('.exe' if os.name == 'nt' else ''))
            result = subprocess.run([str(exe), *map(str, args)], cwd=directory,
                                    env=env, capture_output=True, text=True, timeout=20)
            diagnostic = result.stdout + result.stderr
            assert result.returncode == code, (directory.name, args, diagnostic)
            return diagnostic

        for inverted in (False, True):
            reference = None
            for device, pin, output in [('g16v8hs', 10, 23),
                                        ('g16v8hslcc', 12, 27),
                                        ('g16v8hlcc', 12, 27)]:
                directory = root / f'{device}-{inverted}'
                directory.mkdir()
                name = '!q' if inverted else 'q'
                (directory / 'test.pld').write_text(
                    f'Name test; Device {device}; pin {pin}=a; '
                    f'pin {output}={name}; q=a;\n')
                run('cupl', ['-jaxm1', '-u', library, 'test.pld'], directory)
                bits = fuses(directory / 'test.jed')
                assert len(bits) == 2226
                assert bits[2209] == 1, 'ordinary input must bypass the input latch'
                if reference is None:
                    reference = bits
                else:
                    assert bits == reference, (device, 'package fuse maps differ')
                si = directory / 'test.si'
                si.write_text(f'ORDER:a,{name};\nVECTORS:\n' +
                              ('0H\n1L\n' if inverted else '0L\n1H\n'))
                run('csim', ['-l', '-u', library, 'test'], directory)
                assert len(vector_lines((directory / 'test.so').read_text())) == 2
                si.write_text(f'ORDER:a,{name};\nVECTORS:\n' +
                              ('0L\n' if inverted else '0H\n'))
                diagnostic = run('csim', ['-l', '-u', library, 'test'], directory, 1)
                assert 'for q' in diagnostic, diagnostic

        # The correction must not turn the actual ground pin into an input.
        directory = root / 'ground'
        directory.mkdir()
        (directory / 'test.pld').write_text(
            'Name test; Device g16v8hslcc; pin 20=a; pin 27=q; q=a;\n')
        diagnostic = run('cupl', ['-jaxm1', '-u', library, 'test.pld'], directory, 1)
        assert '[0014cb] invalid input:  a' in diagnostic, diagnostic
    print('PASS: GAL16V8H DIP/PLCC input maps, simulation and ground rejection')


if __name__ == '__main__':
    main()
