#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, LinCUPL team
"""ATF fuse-map simulation against vectors and independent routing tables.

Reuse the compiler regression vectors in both ABS and JEDEC modes. Run the
maps again without source or ABS files, using physical pin/node numbers.
Handwritten maps exercise routing combinations a compiler need not emit.
"""
import itertools
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from csim_jed import jedec_vectors, si_for_pins, vector_lines

ROOT = Path(__file__).resolve().parent.parent


def main():
    bindir = Path(sys.argv[1]).resolve()
    candidates = [bindir.parent / 'share/lincupl/lincupl.dl',
                  Path(os.environ.get('LINCUPL_SHARE', '/nonexistent')) / 'lincupl.dl']
    library = next((p for p in candidates if p.is_file()), None)
    if library is None:
        print('SKIP: device library missing')
        return 77
    env = dict(os.environ, LINCUPL_SHARE=str(library.parent))
    env.pop('LIBCUPL', None)
    checks = 0
    with tempfile.TemporaryDirectory(prefix='csim-atf-jed-') as tmp:
        root = Path(tmp)

        def run(tool, args, directory, code=0):
            nonlocal checks
            p = subprocess.run([str(bindir / tool), *map(str, args)], cwd=directory,
                               env=env, capture_output=True, text=True, timeout=20)
            assert p.returncode == code, (directory.name, args, p.returncode, p.stdout, p.stderr)
            checks += 1
            return p.stdout + p.stderr

        def simulate(directory, vectors, options='-fl', device=None):
            (directory / 'test.si').write_text(vectors)
            args = [options, '-u', str(library)]
            if device:
                args.append(device)
            run('csim', [*args, 'test'], directory)
            lines = vector_lines((directory / 'test.so').read_text())
            assert lines, f'{directory}: no vectors simulated'
            return lines

        def compiled(label, device, source, vectors, flags='-jaxm1'):
            directory = root / label
            directory.mkdir()
            (directory / 'test.pld').write_text(f'Name test; Device {device};\n{source}')
            run('cupl', [flags, '-u', library, 'test.pld'], directory)
            expected = simulate(directory, vectors, '-lj')
            exported = jedec_vectors((directory / 'test.jed').read_text())
            (directory / 'test.abs').unlink()
            assert simulate(directory, vectors, '-lfj') == expected, directory.name
            assert jedec_vectors((directory / 'test.jed').read_text()) == exported, directory.name
            pins = {name: (int(number), low == '!') for number, low, name in re.findall(
                r'\bpin(?:node)?\s+(\d+)\s*=\s*(!?)\s*(\w+)', source, re.I)}
            numbered, _ = si_for_pins(vectors, pins, {}, False)
            (directory / 'test.pld').unlink()
            assert simulate(directory, numbered, '-lfj') == expected, directory.name
            assert jedec_vectors((directory / 'test.jed').read_text()) == exported, directory.name
            assert simulate(directory, numbered, device=device) == expected, directory.name

        for path in sorted((ROOT / 'tests/defects').glob('*.json')):
            case = json.loads(path.read_text())
            for i, variant in enumerate(case['variants']):
                device = variant.get('device', case.get('device', ''))
                if device.startswith(('v750', 'v2500')) and 'vectors' in variant:
                    compiled(f'{path.stem}-{i}', device, variant['source'], variant['vectors'],
                             variant.get('flags', '-jaxm1'))

        for device in ['v750c', 'v750clcc', 'v2500c', 'v2500clcc']:
            small, lcc = device.startswith('v750'), device.endswith('lcc')
            clk = 2 if small and lcc else 1
            a = 3 if clk == 2 else 2
            en = 5 if clk == 2 else 3
            q, y, z = ((27, 26, 25) if lcc else (23, 22, 21)) if small else (
                (5, 6, 8) if lcc else (4, 5, 6))
            compiled(device + '-io', device,
                     f'pin {clk}=clk; pin {a}=a; pin {en}=en; pin {q}=q; '
                     f'pin {y}=y; pin {z}=z; q.d=a; q.ck=clk; q.oe=en; y=q; z=q.io;',
                     'ORDER: clk,a,en,q,y,z;\nVECTORS:\n'
                     '001 LLL\nC11 HHH\n010 0HL\nC00 1LH\n001 LLL\n')
            b = (38 if lcc else 34) if small else (45 if lcc else 41)
            rst, preset = (6, 7) if small else (19, 17)
            compiled(device + '-preset', device,
                     f'pin {clk}=clk; pin {a}=a; pin {rst}=rst; pin {preset}=pre; '
                     f'pin {q}=q; pin {y}=y; pinnode {b}=b; '
                     'q.d=a; b.d=!a; q.ck=clk; b.ck=clk; q.ar=rst; b.ar=rst; '
                     'q.sp=pre; y=b;',
                     'ORDER: clk,a,pre,rst,q,y;\nVECTORS:\n'
                     '0001 LL\nC100 HL\nC010 HH\nC000 LH\n0001 LL\n')

        def write_map(directory, bits, device, pins, header=True):
            # Deliberately omit checksums: these cases test the logic,
            # while the shared JEDEC reader has separate checksum tests.
            title = f'Device {device}\n' if header else 'unnamed map\n'
            text = ''.join(map(str, bits))
            (directory / 'test.jed').write_text(
                f'\x02{title}*QF{len(bits)}*QP{pins}*L0 {text}*\x030000')

        def row(bits, first, width, column=None):
            bits[first:first + width] = [1] * width
            if column is not None:
                bits[first + column] = 0

        # ATF2500C output-logic modes. Put a different input
        # in each group of four terms, so a mistaken overlap is visible.
        # P2, P3, P17 feed the first, middle and last groups respectively.
        for mode, t2, low in itertools.product(range(4), range(2), range(2)):
            directory = root / f'2500-rows-{mode}-{t2}-{low}'
            directory.mkdir()
            bits = [0] * 71816
            for first, col in [(68456, 46), (69144, 48), (69832, 134)]:
                row(bits, first, 172, col)
            row(bits, 71208, 172)  # enable P4
            bits[71644:71648] = [mode & 1, mode >> 1, 1, 1 - low]
            bits[71742] = t2
            write_map(directory, bits, 'v2500c', 40, header=False)
            lines = []
            for a, b, c in itertools.product(range(2), repeat=3):
                value = [a | b, a, a | b | c, a | (c if t2 else 0)][mode] ^ low
                bypass = c if mode == 3 else 0
                lines.append(f'{a}{b}{c} {"H" if value else "L"}{"H" if bypass else "L"}L')
            simulate(directory, 'ORDER: P2,P3,P17,P4,P41,P65;\nVECTORS:\n' + '\n'.join(lines) + '\n')

        # Two sums on ATF750C: S1 separates the output's first half from
        # FF2's half. Both halves use independent physical inputs.
        for split, low in itertools.product(range(2), repeat=2):
            directory = root / f'750-rows-{split}-{low}'
            directory.mkdir()
            bits = [0] * 14504
            row(bits, 0, 84, 75)  # P2
            row(bits, 336, 84, 67)  # P3
            row(bits, 1008, 84)
            bits[14364:14367] = [1, split, 1 - low]
            write_map(directory, bits, 'v750c', 24, header=False)
            lines = [f'{a}{b} {"H" if ((a if split else a | b) ^ low) else "L"}LL'
                     for a, b in itertools.product(range(2), repeat=2)]
            simulate(directory, 'ORDER: P2,P3,P23,P34,P44;\nVECTORS:\n' + '\n'.join(lines) + '\n')

        for extended, keeper in itertools.product(range(2), repeat=2):
            directory = root / f'750-options-{extended}-{keeper}'
            directory.mkdir()
            bits = [0] * 14504
            row(bits, 0, 84, 75)
            row(bits, 1008, 84)
            bits[14364:14367] = [1, 0, 1]
            bits[14499:14501] = [extended, keeper]
            device = 'v750c' + ('ext' if extended else '') + ('ppk' if keeper else '')
            write_map(directory, bits, device, 24, header=False)
            simulate(directory, 'ORDER: P2,P23;\nVECTORS:\n0L\n1H\n')
            out = run('csim', ['-fl', '-u', library, 'test'], directory)
            assert f'simulated as device {device}\n' in out

        # Selecting the global clock must ignore a false product clock.
        # The other FF keeps its independent product clock from P3.
        directory = root / '750-clocks'
        directory.mkdir()
        bits = [0] * 14504
        row(bits, 0, 84, 75)
        row(bits, 336, 84, 75)
        row(bits, 840, 84, 67)
        row(bits, 1008, 84)
        bits[14364:14367] = [0, 1, 1]
        bits[14395] = 1
        write_map(directory, bits, 'v750c', 24)
        simulate(directory, 'ORDER: P1,P2,P3,P23,P34;\nVECTORS:\n'
                 '000 LL\nC10 HL\n010 HL\n01C HH\nC00 LH\n00C LL\n')

        # FF1's numbered alias observes its state before an inverted pin
        # buffer. T mode must toggle only once per physical clock edge.
        bits[14366] = 0
        bits[14397] = 1
        write_map(directory, bits, 'v750c', 24)
        simulate(directory, 'ORDER: P1,P2,P23,P44;\nVECTORS:\n'
                 '00 HL\nC1 LH\nC1 HL\nC0 HL\nC1 LH\n')

        directory = root / '2500-clock-gate'
        directory.mkdir()
        bits = [0] * 71816
        row(bits, 68456, 172, 46)  # data from P2
        row(bits, 70692, 172, 48)  # clock product from P3
        row(bits, 71208, 172)
        bits[71647] = bits[71743] = 1
        write_map(directory, bits, 'v2500c', 40)
        # S6 is a physical AND gate: raising the product term while the
        # global clock is high creates a clock edge too.
        simulate(directory, 'ORDER: P1,P2,P3,P4;\nVECTORS:\n'
                 '000 L\n110 L\n111 H\n101 H\n100 H\n101 L\n')

        # Truth-table comparison must not silently accept buried state
        # merely because the observable pin is combinational.
        directory = root / '750-rows-0-0'
        out = run('jedcompare', ['-u', library, 'test.jed', 'test.jed'], directory, code=2)
        assert 'not supported' in out
    print(f'PASS: {checks} ATF compilation and simulation checks')
    return 0


if __name__ == '__main__':
    sys.exit(main())
