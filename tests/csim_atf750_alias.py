#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, LinCUPL team
"""Check ATF750C register aliases against independent D/T state traces."""
import itertools
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from csim_jed import vector_lines


def main():
    bindir = Path(sys.argv[1]).resolve()
    library = bindir.parent / 'share/lincupl/lincupl.dl'
    env = dict(os.environ, LINCUPL_SHARE=str(library.parent))
    env.pop('LIBCUPL', None)
    env.pop('FITTERDIR', None)
    checks = 0
    # Clock, data/toggle, output enable, AND input, asynchronous reset.
    events = [('0', 0, 1, 1, 0), ('C', 1, 1, 1, 0),
              ('0', 0, 0, 1, 0), ('C', 1, 0, 1, 0),
              ('C', 0, 1, 0, 0), ('0', 1, 1, 1, 1),
              ('0', 0, 0, 1, 0), ('C', 1, 1, 1, 0),
              ('C', 0, 1, 1, 0), ('C', 1, 1, 1, 0)]
    with tempfile.TemporaryDirectory(prefix='csim-atf750-alias-') as tmp:
        root = Path(tmp)

        def run(tool, args, directory, code=0):
            exe = bindir / (tool + ('.exe' if os.name == 'nt' else ''))
            result = subprocess.run([str(exe), *map(str, args)], cwd=directory,
                                    env=env, capture_output=True, text=True, timeout=20)
            assert result.returncode == code, (directory.name, args, result.stdout, result.stderr)
            return result.stdout + result.stderr

        for lcc, ext, keeper, kind, low, alias_low, pin, feedback in itertools.product(
                (False, True), (False, True), (False, True), ('d', 't'),
                (False, True), (False, True), (23, 16), (False, True)):
            device = 'v750c' + ('ext' if ext else '') + ('ppk' if keeper else '') + ('lcc' if lcc else '')
            if feedback and (ext or keeper):
                continue
            mapping = [2, 3, 4, 5, 6, 7, 9, 10, 11, 12, 13, 14,
                       16, 17, 18, 19, 20, 21, 23, 24, 25, 26, 27, 28]
            physical = lambda number: mapping[number - 1] if lcc else number
            alias_pin = pin + 21 + (4 if lcc else 0)
            alias = 'a_feedback' if kind == 'd' else 'z_feedback'
            output_name = ('!' if low else '') + 'q'
            alias_name = ('!' if alias_low else '') + alias
            directory = root / f'{device}-{kind}-{low}-{alias_low}-{pin}-{feedback}'
            directory.mkdir()
            term = f'(!{alias}&b)#({alias}&!b)' if feedback else 'b'
            source = (f'Name test; Device {device};\n'
                      f'pin {physical(1)}=clk; pin {physical(2)}=b; '
                      f'pin {physical(3)}=en; pin {physical(5)}=a; pin {physical(6)}=rst;\n'
                      f'pin {physical(pin)}={output_name}; pin {physical(pin - 1)}=y; '
                      f'pinnode {alias_pin}={alias_name};\n'
                      f'q.{kind}={term}; q.ck=clk; q.oe=en; q.ar=rst; y={alias}&a;\n')
            (directory / 'test.pld').write_text(source)
            run('cupl', ['-jaxm1', '-u', library, 'test.pld'], directory)
            state, vectors = 0, []
            for clock, data, enable, gate, reset in events:
                if reset:
                    state = 0
                elif clock == 'C':
                    term = (state ^ alias_low) ^ data if feedback else data
                    state = term if kind == 'd' else state ^ term
                output = 'LH'[state ^ low] if enable else 'Z'
                y = 'LH'[(state ^ alias_low) & gate]
                vectors.append(f'{clock}{data}{enable}{gate}{reset}{output}{y}{"LH"[state]}')
            (directory / 'test.si').write_text(
                f'ORDER:clk,b,en,a,rst,{output_name},y,{alias_name};\nVECTORS:\n' +
                '\n'.join(vectors) + '\n')
            for flags in ('-fl', '-l'):
                run('csim', [flags, '-u', library, 'test'], directory)
                assert len(vector_lines((directory / 'test.so').read_text())) == len(events)
            if device == 'v750c' and pin == 23 and not feedback:
                # An unbound ORDER symbol must not merely echo the expected
                # value. Flip only the alias expectation at power-up.
                si = directory / 'test.si'
                si.write_text(si.read_text().replace(vectors[0], vectors[0][:-1] + 'H', 1))
                diagnostic = run('csim', ['-l', '-u', library, 'test'], directory, code=1)
                assert 'for ' + alias in diagnostic, diagnostic
            checks += 1
    print(f'PASS: {checks} ATF750C alias designs in ABS and JEDEC simulation')


if __name__ == '__main__':
    main()
