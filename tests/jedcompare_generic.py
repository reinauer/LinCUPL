#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, LinCUPL team
"""Check decoded sequential counterexamples against independent state machines."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def main():
    bindir = Path(sys.argv[1]).resolve()
    suffix = '.exe' if os.name == 'nt' else ''
    library = bindir.parent / 'share/lincupl/lincupl.dl'
    if not library.is_file():
        print('SKIP: device library is required')
        return 77
    env = dict(os.environ, LINCUPL_SHARE=str(library.parent))
    env.pop('LIBCUPL', None)
    checks = 0
    with tempfile.TemporaryDirectory(prefix='generic sequential ') as temp:
        root = Path(temp)

        def run(tool, args, cwd=root, code=0):
            nonlocal checks
            p = subprocess.run([str(bindir / (tool + suffix)), *map(str, args)],
                               cwd=cwd, env=env, capture_output=True,
                               text=True, timeout=60)
            text = p.stdout + p.stderr
            assert p.returncode == code, (cwd.name, tool, args, p.returncode, text)
            checks += 1
            return text

        def compile_map(device, pins, label, equation):
            clock, data, output, enable = pins
            work = root / f'{device}-{label}'
            work.mkdir()
            (work / 'test.pld').write_text(
                f'Name test; Device {device}; pin {clock}=clk; '
                f'pin {data}=a; pin {output}=q;\n{equation}\n' +
                ('q.ck=clk;\n' if device.startswith('v') else ''))
            run('cupl', ['-jaxm1', 'test.pld'], work)
            # Comparator inputs come entirely from the JEDEC file.
            (work / 'test.pld').unlink()
            (work / 'test.abs').unlink()
            return work / 'test.jed'

        def preload(path, value):
            listing = run('jedcompare', ['--list-state', path])
            names = re.findall(r'^(R\d+)=[01X]$', listing, re.M)
            assert names and len(names) == len(set(names)), listing
            return ','.join(f'{name}={value}' for name in names)

        def raw_zero(device):
            # The V8 registered output path inverts stored Q. The positive
            # output equations below therefore store one for a low pin.
            return int(device.startswith(('g16v8', 'g20v8')))

        def compare(left, right, left_preload, right_preload, code=0):
            return run('jedcompare', ['--sequential', '--preload-left', left_preload,
                       '--preload-right', right_preload, left, right], code=code)

        def trace(text):
            rows = re.findall(r'^Step (\d+):(.*)$', text, re.M)
            assert rows and [int(n) for n, _ in rows] == list(range(len(rows))), text
            return [{int(p): int(v) for p, v in re.findall(r'P(\d+)=([01])', row)}
                    for _, row in rows]

        def oracle(sequence, pins, mode, initial):
            clock, data, output, enable = pins
            q, previous = initial, None
            values = []
            for row in sequence:
                ck, a = row[clock], row.get(data, 0)
                if previous == 0 and ck == 1:
                    q = a if mode == 'd' else (1 - a if mode == 'not' else q ^ a)
                previous = ck
                values.append('Z' if enable and row[enable] else str(q))
            return values

        families = [
            ('g16v8ms', (1, 2, 19, 11)),
            ('g20v8ms', (1, 2, 22, 13)),
            ('g20v8mslcc', (2, 3, 26, 16)),
            ('g22v10', (1, 2, 23, 0)),
            ('g22v10lcc', (2, 3, 27, 0)),
        ]
        for family in ('v750', 'v750b', 'v750c', 'v750cext', 'v750cppk', 'v750cextppk'):
            families += [(family, (1, 2, 23, 0)), (family + 'lcc', (2, 3, 27, 0))]
        for family in ('v2500', 'v2500b', 'v2500c', 'v2500cppk'):
            families += [(family, (1, 2, 4, 0)), (family + 'lcc', (1, 2, 5, 0))]

        for device, pins in families:
            left = compile_map(device, pins, 'd', 'q.d=a;')
            right = compile_map(device, pins, 'not', 'q.d=!a;')
            initial = raw_zero(device)
            a, b = preload(left, initial), preload(right, initial)
            assert 'Equivalent:' in compare(left, left, a, a)
            text = compare(left, right, a, b, code=1)
            sequence = trace(text)
            lhs, rhs = oracle(sequence, pins, 'd', 0), oracle(sequence, pins, 'not', 0)
            assert lhs[:-1] == rhs[:-1] and lhs[-1] != rhs[-1], (device, text, lhs, rhs)
            assert f'P{pins[2]} is {lhs[-1]} versus {rhs[-1]}' in text, text
            assert len(sequence) == 2, (device, text)

            # A complemented initial storage value must be observable before
            # the first edge. This also checks that settling does not clock D.
            text = compare(left, left, a, preload(left, 1 - initial), code=1)
            sequence = trace(text)
            assert len(sequence) == 1, text
            lhs, rhs = oracle(sequence, pins, 'd', 0), oracle(sequence, pins, 'd', 1)
            assert f'P{pins[2]} is {lhs[-1]} versus {rhs[-1]}' in text, text

        # D and T histories agree on the first edge from zero, then diverge
        # on a later edge. Check a stateful counterexample for each family.
        for device, pins in [families[i] for i in (0, 1, 3, 9, 21)]:
            left = root / f'{device}-d/test.jed'
            right = compile_map(device, pins, 'toggle', 'q.d=q $ a;')
            initial = raw_zero(device)
            text = compare(left, right, preload(left, initial), preload(right, initial), code=1)
            sequence = trace(text)
            lhs, rhs = oracle(sequence, pins, 'd', 0), oracle(sequence, pins, 'toggle', 0)
            assert lhs[:-1] == rhs[:-1] and lhs[-1] != rhs[-1], (device, text, lhs, rhs)
            assert f'P{pins[2]} is {lhs[-1]} versus {rhs[-1]}' in text, text
            assert len(sequence) == 4, text

        for device, pins, reset, other in [
            ('g22v10', (1, 2, 23, 0), 3, 22),
            ('v750c', (1, 2, 23, 0), 3, 22),
            ('v750clcc', (2, 3, 27, 0), 4, 26),
            ('v2500c', (1, 2, 4, 0), 3, 5),
            ('v2500clcc', (1, 2, 5, 0), 3, 6),
        ]:
            for control, initial in [('ar', 1), ('sp', 0)]:
                maps = [compile_map(device, pins, control + str(inv),
                    f'pin {reset}=r; q.d=a; q.{control}={"!" if inv else ""}r;')
                    for inv in (0, 1)]
                text = compare(*maps, preload(maps[0], initial),
                               preload(maps[1], initial), code=1)
                sequence = trace(text)
                values = []
                for inv in (0, 1):
                    q, previous, seen = initial, None, []
                    for row in sequence:
                        ck, asserted = row[pins[0]], row[reset] ^ inv
                        if control == 'ar' and asserted:
                            q = 0
                        elif previous == 0 and ck == 1:
                            q = 1 if control == 'sp' and asserted else row.get(pins[1], 0)
                        previous = ck
                        seen.append(str(q))
                    values.append(seen)
                assert values[0][:-1] == values[1][:-1], (device, text, values)
                assert values[0][-1] != values[1][-1], (device, text, values)
                assert f'P{pins[2]} is {values[0][-1]} versus {values[1][-1]}' in text

            if device.startswith('g'):
                continue
            # Masking physical feedback with OE gives the same result as
            # masking raw Q: disabled, floating feedback cannot leak through.
            maps = [compile_map(device, pins, 'feedback-' + str(physical),
                    f'pin {reset}=en; pin {other}=y; q.d=a; q.oe=en; '
                    f'y=q{".io" if physical else ""} & en;') for physical in (0, 1)]
            compare(*maps, preload(maps[0], 's'), preload(maps[1], 's'))
            unmasked = compile_map(device, pins, 'feedback-floating',
                f'pin {reset}=en; pin {other}=y; q.d=a; q.oe=en; y=q.io;')
            text = compare(unmasked, unmasked, preload(unmasked, 0),
                           preload(unmasked, 0), code=2)
            assert 'indeterminate' in text, text

        # The same logical state can occupy different buried registers.
        # Seed names correlate their values without relying on cell numbers.
        for device, pins, buried in [
            ('v750c', (1, 2, 23, 0), 34),
            ('v750clcc', (2, 3, 27, 0), 38),
            ('v2500c', (1, 2, 4, 0), 41),
            ('v2500clcc', (1, 2, 5, 0), 45),
        ]:
            maps = [compile_map(device, pins, f'buried-{p}',
                    f'pinnode {p}=b; b.d=a; b.ck=clk; q=b;')
                    for p in (buried, buried + 1)]
            left, right = f'R{buried}=s', f'R{buried + 1}=s'
            compare(*maps, left, right)
            for other_seed in ('!s', 't'):
                text = compare(*maps, left, f'R{buried + 1}={other_seed}', code=1)
                assert len(trace(text)) == 1 and 'Preload seeds:' in text, text

        # Compatible physical interfaces can also cross architectures.
        left = root / 'g22v10-d/test.jed'
        right = root / 'v750c-d/test.jed'
        compare(left, right, 'R23=s', 'R23=s')

    print(f'PASS: {checks} generic sequential oracle checks')


if __name__ == '__main__':
    main()
