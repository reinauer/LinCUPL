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

        # Q0 feeds the array before output polarity and OE. Observe both
        # register columns and the pin column with separate output cells.
        cells = [(23, 0, 8), (22, 13, 10), (21, 28, 12), (20, 45, 14),
                 (19, 64, 16), (18, 85, 16), (17, 106, 14), (16, 125, 12),
                 (15, 142, 10), (14, 157, 8)]
        for i, (pin, first, terms) in enumerate(cells):
            observers = [c for c in cells if c[0] != pin][:3]
            for toggle, low in itertools.product(range(2), repeat=2):
                directory = root / f'750-feedback-{pin}-{toggle}-{low}'
                directory.mkdir()
                bits = [0] * 14504
                row(bits, first * 84, 84, 75)  # D/T from P2
                row(bits, (first + terms + 4) * 84, 84, 67)  # OE from P3
                cfg, reg = 14364 + 3 * i, 14395 + 4 * i
                bits[cfg:cfg + 3] = [0, 1, 1 - low]
                bits[reg] = 1  # global clock P1
                bits[reg + 2] = toggle
                columns = [78 - 8 * i, 79 - 8 * i, 81 - 8 * i]
                for (out, start, count), column in zip(observers, columns):
                    row(bits, start * 84, 84, column)
                    row(bits, (start + count + 4) * 84, 84)
                    cfg = 14364 + 3 * (23 - out)
                    bits[cfg:cfg + 3] = [1, 0, 1]
                write_map(directory, bits, 'v750c', 24)
                state, lines = 0, []
                for clock, data, enable, external in [
                        ('0', 0, 1, 0), ('C', 1, 1, 0), ('C', 1, 1, 0),
                        ('C', 0, 1, 0), ('0', 0, 0, 0), ('0', 0, 0, 1),
                        ('C', 1, 0, 0), ('0', 0, 1, 0)]:
                    if clock == 'C':
                        state = state ^ data if toggle else data
                    physical = state ^ low if enable else external
                    pin_level = 'LH'[physical] if enable else str(external)
                    observed = ''.join('LH'[v] for v in [state, 1 - state, physical])
                    lines.append(f'{clock}{data}{enable}{pin_level}{observed}')
                order = [1, 2, 3, pin, *[p for p, _, _ in observers]]
                simulate(directory, 'ORDER: ' + ','.join(f'P{p}' for p in order) +
                         ';\nVECTORS:\n' + '\n'.join(lines) + '\n')

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

        # An ORDER alias observes Q even while OE disables the pin. Cover
        # both architectures sharing this path, D/T mode, output polarity,
        # named aliases and explicit ORDER inversion. No register feedback
        # columns are used, so these cases only test the observation path.
        for small, toggle, low in itertools.product(range(2), repeat=3):
            directory = root / f'alias-{small}-{toggle}-{low}'
            directory.mkdir()
            device, count, pins = ('v750c', 14504, 24) if small else ('v2500c', 71816, 40)
            pin, alias = (23, 44) if small else (4, 65)
            bits = [0] * count
            if small:
                row(bits, 0, 84, 75)  # data from P2
                row(bits, 1008, 84, 67)  # OE from P3
                bits[14364:14367] = [0, 1, 1 - low]
                bits[14395] = 1  # global clock P1
                bits[14397] = toggle
            else:
                row(bits, 68456, 172, 46)  # data from P2
                row(bits, 70692, 172, 44)  # product clock from P1
                row(bits, 71208, 172, 48)  # OE from P3
                bits[71647] = 1 - low
                bits[71741] = toggle
            write_map(directory, bits, device, pins)
            state, lines = 0, []
            for clock, data, enable, external in [
                    ('0', 0, 1, 0), ('C', 1, 1, 0), ('0', 0, 0, 0),
                    ('0', 0, 0, 1), ('C', 1, 0, 0), ('C', 0, 0, 1),
                    ('C', 1, 0, 0), ('0', 0, 1, 0)]:
                for level in ('01' if clock == 'C' else '0'):
                    if level == '1':
                        state = state ^ data if toggle else data
                    physical = state ^ low if enable else external
                    pin_level = 'LH'[physical] if enable else str(external)
                    lines.append(f'{level}{data}{enable}{pin_level}{"LH"[state]}')
            vectors = '\nVECTORS:\n' + '\n'.join(lines) + '\n'
            simulate(directory, f'ORDER: P1,P2,P3,P{pin},P{alias};' + vectors)
            for inverted in range(2):
                sign = '!' if inverted else ''
                (directory / 'test.pld').write_text(
                    f'Device {device}; pinnode {alias}={sign}q;\n')
                simulate(directory, f'ORDER: P1,P2,P3,P{pin},{sign}q;' + vectors)

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
