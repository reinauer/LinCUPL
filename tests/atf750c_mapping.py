#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, LinCUPL team
"""Audit ATF750C coordinates, feedback, options and library normalization.

The coordinates below are independent of the library reader and simulator.
This is not a reader or writer for the proposed device description language.
"""
import itertools
import argparse
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile

from csim_jed import vector_lines
from defects import fuses

ROOT = Path(__file__).resolve().parent.parent
# Pin, first data row, number of data terms. Five control rows follow.
CELLS = [(23, 0, 8), (22, 13, 10), (21, 28, 12), (20, 45, 14),
         (19, 64, 16), (18, 85, 16), (17, 106, 14), (16, 125, 12),
         (15, 142, 10), (14, 157, 8)]
INPUTS = {1: 83, 2: 75, 3: 67, 4: 59, 5: 51, 6: 43, 7: 35,
          8: 27, 9: 19, 10: 11, 11: 3, 13: 1,
          **{pin: 9 + 8 * (pin - 14) for pin, _, _ in CELLS}}
FUSE_COUNT = 14504
ROW_WIDTH = 84
HEADER_SIZE, ENTRY_SIZE, ENTRY_COUNT = 199, 26, 1000
SECTION_SIZE = 20


def u16(data, offset):
    return struct.unpack_from('<H', data, offset)[0]


def u32(data, offset):
    return struct.unpack_from('<I', data, offset)[0]


def devices(data):
    for i in range(ENTRY_COUNT):
        start = HEADER_SIZE + i * ENTRY_SIZE
        entry = data[start:start + ENTRY_SIZE]
        if entry[17] & 1:
            name = entry[:17].split(b'\0')[0].decode('ascii')
            yield name, u32(entry, 22), u32(entry, 18)


def records(image, section):
    h = section * SECTION_SIZE
    count, size, offset = u16(image, h + 4), u16(image, h + 6), u32(image, h + 8)
    assert offset + count * size <= len(image)
    for i in range(count):
        yield offset + i * size, image[offset + i * size:offset + (i + 1) * size]


def audit_library(data):
    found = set()
    for name, offset, size in devices(data):
        if name not in ('v750c', 'v750cext', 'v750cppk', 'v750cextppk'):
            continue
        found.add(name)
        image = data[offset:offset + size]
        attr = next(records(image, 3))[1]
        assert (attr[2], attr[3], attr[8], u32(attr, 21)) == (12, 24, 24, FUSE_COUNT)
        assert (u16(attr, 12), u16(attr, 29), attr[61], u32(attr, 62)) == (20, 171, 64, 14435)
        pins = [p for _, p in records(image, 6)]
        nodes = [n for _, n in records(image, 7)]
        arrays = {u16(n, 0): n for n in nodes if n[4] == 14}
        used_rows, used_columns = set(), set()
        for pin, high in INPUTS.items():
            if pin == 4 and 'ext' in name:
                continue  # Pin-controlled power-down replaces the logic input.
            assert any(u16(p, 2) == pin and p[4] in (0, 37) and
                       (p[7], p[8]) == (high, high - 1) for p in pins), (name, pin)
            used_columns.update((high, high - 1))
        for i, (pin, row, terms) in enumerate(CELLS):
            config, registers = 14364 + 3 * i, 14395 + 4 * i
            base_array = 1 + 7 * i
            for number, first, count in [(base_array, row, terms),
                                         (base_array + 4, row + terms // 2, terms // 2),
                                         (base_array + 1, row + terms, 1),
                                         (base_array + 2, row + terms + 1, 1),
                                         (base_array + 5, row + terms + 2, 1),
                                         (base_array + 6, row + terms + 3, 1),
                                         (base_array + 3, row + terms + 4, 1)]:
                n = arrays[number]
                assert (u32(n, 10), n[27], u16(n, 31)) == (first * ROW_WIDTH, ROW_WIDTH, count)
            allocation = set(range(row, row + terms + 5))
            assert not used_rows & allocation
            used_rows.update(allocation)
            for alias, high in [(pin + 21, 78 - 8 * i), (pin + 11, 76 - 8 * i)]:
                assert any(u16(p, 2) == alias and p[4] == 0 and
                           (p[7], p[8]) == (high, high + 1) for p in pins)
                used_columns.update((high, high + 1))
            for ext in (0, 1, 27):
                for kind, select in [(6, int(ext == 0)), (7, 4 + int(ext == 0))]:
                    n = next(n for n in nodes if u16(n, 2) == pin and n[4:6] == bytes([ext, kind]))
                    assert (u32(n, 10), n[27], u16(n, 7)) == (config, 3, select)
            for number, clock_fuse, type_fuse in [(pin, registers, registers + 2),
                                                  (pin + 21, registers, registers + 2),
                                                  (pin + 11, registers + 1, registers + 3)]:
                clock = next(n for n in nodes if u16(n, 2) == number and n[4] == 42)
                toggle = next(n for n in nodes if u16(n, 2) == number and n[4] == 27)
                assert (u32(clock, 22), clock[30]) == (clock_fuse, 1)
                assert type_fuse in (u32(toggle, 10), u32(toggle, 18))
        assert used_rows == set(range(170))
        assert used_columns == set(range(84)) - ({58, 59} if 'ext' in name else set())
        assert (u32(arrays[71], 10), u16(arrays[71], 31)) == (170 * ROW_WIDTH, 1)
        initial = [attr[19]] * FUSE_COUNT
        for _, r in records(image, 9):
            first, last = u32(r, 0), u32(r, 4)
            assert first <= last < FUSE_COUNT and r[8] in (0, 1)
            initial[first:last + 1] = [r[8]] * (last - first + 1)
        assert initial[14499:14501] == [int('ext' in name), int('ppk' in name)]
        assert all(initial[f] == 0 for f in [14394, *range(14435, 14499), *range(14501, 14504)])
    assert len(found) == 4, found


def without_unused_metadata(data):
    """Mutate a copy, retaining records and offsets; this is not a DL writer."""
    result = bytearray(data)
    for name, offset, size in devices(data):
        if not name.startswith('v750c'):
            continue
        image = bytearray(data[offset:offset + size])
        for section in range(13):
            h = section * SECTION_SIZE
            image[h + 2:h + 4] = image[h + 4:h + 6]  # capacity = count
            image[h + 12:h + 20] = bytes(8)  # historical process pointers
        attr_offset = next(records(image, 3))[0]
        for i in [*range(4, 8), 9, 10, 20, *range(31, 37), *range(50, 61)]:
            image[attr_offset + i] = 0
        # These columns are not consumed by the current compiler/simulator.
        for section, unused in [(6, [9]), (7, [26]), (10, [4])]:
            for start, _ in records(image, section):
                for i in unused:
                    image[start + i] = 0
        # Ignore bytes following NUL in each separately defined text field.
        for section, fields in [(3, [(67, 13), (80, 32), (112, 20), (132, 20)]),
                                (4, [(0, 20)]), (8, [(0, 9)]), (11, [(1, 23)])]:
            for start, record in records(image, section):
                for field, length in fields:
                    text = record[field:field + length]
                    if 0 in text:
                        tail = field + text.index(0)
                        image[start + tail:start + field + length] = bytes(field + length - tail)
        result[offset:offset + size] = image
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('bin_dir', type=Path)
    parser.add_argument('--library', type=Path, help='audit a generated standalone device library')
    parser.add_argument('--skip-metadata', action='store_true', help='run geometry and feedback checks only')
    args = parser.parse_args()
    bindir = args.bin_dir.resolve()
    candidates = [bindir.parent / 'share/lincupl/lincupl.dl',
                  Path(os.environ.get('LINCUPL_SHARE', '/nonexistent')) / 'lincupl.dl']
    library = args.library.resolve() if args.library else next((p for p in candidates if p.is_file()), None)
    if library is None:
        print('SKIP: device library missing')
        return 77
    data = library.read_bytes()
    audit_library(data)
    checks = 0
    with tempfile.TemporaryDirectory(prefix='atf750c-mapping-') as tmp:
        root = Path(tmp)
        canonical = root / 'lincupl.dl'
        canonical.write_bytes(without_unused_metadata(data))

        def run(tool, args, directory, expected=0):
            nonlocal checks
            env = dict(os.environ, LINCUPL_SHARE=str(library.parent))
            env.pop('LIBCUPL', None)
            p = subprocess.run([str(bindir / tool), *map(str, args)], cwd=directory,
                               env=env, capture_output=True, text=True, timeout=20)
            assert p.returncode == expected, (directory.name, args, p.returncode, p.stdout, p.stderr)
            checks += 1
            return p.stdout + p.stderr

        def row(bits, number, column=None):
            first = number * ROW_WIDTH
            bits[first:first + ROW_WIDTH] = [1] * ROW_WIDTH
            if column is not None:
                bits[first + column] = 0

        for extended, keeper in itertools.product(range(2), repeat=2):
            device = 'v750c' + ('ext' if extended else '') + ('ppk' if keeper else '')
            directory = root / device
            directory.mkdir()
            (directory / 'test.pld').write_text(
                f'Name test; Device {device}; Partno ABCDEFGH;\n'
                'pin 2=a; pin 23=y; y=a;\n')
            run('cupl', ['-jaxm1', '-u', library, 'test.pld'], directory)
            bits = fuses(directory / 'test.jed')
            signature = [int(bit) for ch in b'ABCDEFGH' for bit in f'{ch:08b}']
            assert bits[14435:14499] == signature
            assert bits[14499:14501] == [extended, keeper]
            assert all(bits[f] == 0 for f in [14394, 14501, 14502, 14503])

        # Exercise both true/complement columns of all twenty registers.
        # Observe the physical pin separately, including an external driver
        # while OE is off; neither register feedback path may follow it.
        for i, (pin, first, terms) in enumerate(CELLS):
            observers = [c for c in CELLS if c[0] != pin][:5]
            for t0, t1, inverted in itertools.product(range(2), repeat=3):
                directory = root / f'feedback-{pin}-{t0}-{t1}-{inverted}'
                directory.mkdir()
                bits = [0] * FUSE_COUNT
                config, registers = 14364 + 3 * i, 14395 + 4 * i
                row(bits, first, INPUTS[2])
                row(bits, first + terms // 2, INPUTS[3])
                row(bits, first + terms, INPUTS[5])
                row(bits, first + terms + 3, INPUTS[6])
                row(bits, first + terms + 4, INPUTS[4])
                row(bits, 170, INPUTS[7])
                bits[config:config + 3] = [0, 1, 1 - inverted]
                bits[registers:registers + 4] = [1, 1, t0, t1]
                columns = [78 - 8 * i, 79 - 8 * i, 76 - 8 * i, 77 - 8 * i, INPUTS[pin]]
                for (observer, start, count), column in zip(observers, columns):
                    row(bits, start, column)
                    row(bits, start + count + 4)
                    cfg = 14364 + 3 * (23 - observer)
                    bits[cfg:cfg + 3] = [1, 0, 1]
                (directory / 'test.jed').write_text(
                    '\x02Device v750c\n*QF14504*QP24*L0 ' + ''.join(map(str, bits)) + '*\x030000')
                # clk, data0, data1, OE, AR0, AR1, SP, external pin level.
                events = [('0', 0, 0, 1, 0, 0, 0, 0), ('C', 1, 0, 1, 0, 0, 0, 0),
                          ('C', 1, 1, 1, 0, 0, 0, 0), ('C', 0, 1, 1, 0, 0, 0, 0),
                          ('0', 0, 0, 0, 0, 0, 0, 0), ('0', 0, 0, 0, 0, 0, 0, 1),
                          ('C', 1, 1, 0, 0, 0, 0, 0), ('0', 0, 0, 1, 1, 0, 0, 0),
                          ('0', 0, 0, 1, 0, 1, 0, 0), ('0', 0, 0, 1, 0, 0, 1, 0),
                          ('C', 0, 0, 1, 0, 0, 1, 0), ('C', 0, 0, 1, 0, 0, 0, 0)]
                q0 = q1 = 0
                vectors = []
                for clk, a, b, enable, ar0, ar1, preset, external in events:
                    if clk == 'C':
                        q0 = 1 if preset else (q0 ^ a if t0 else a)
                        q1 = 1 if preset else (q1 ^ b if t1 else b)
                    if ar0:
                        q0 = 0
                    if ar1:
                        q1 = 0
                    physical = q0 ^ inverted if enable else external
                    pin_level = 'LH'[physical] if enable else str(external)
                    expected = ''.join('LH'[v] for v in [q0, 1 - q0, q1, 1 - q1, physical])
                    expected += 'LH'[q0] + 'LH'[q1]
                    vectors.append(clk + ''.join(map(str, [a, b, enable, ar0, ar1, preset])) + pin_level + expected)
                order = [1, 2, 3, 4, 5, 6, 7, pin, *[p for p, _, _ in observers], pin + 21, pin + 11]
                (directory / 'test.si').write_text('ORDER: ' + ','.join(f'P{p}' for p in order) +
                                                  ';\nVECTORS:\n' + '\n'.join(vectors) + '\n')
                run('csim', ['-fl', '-u', library, 'test'], directory)
                # Names and inverted names must resolve to the same state;
                # neither alias creates another clocked register.
                (directory / 'test.pld').write_text(
                    f'Device v750c; pinnode {pin + 21}=!primary; '
                    f'pinnode {pin + 11}=!secondary;\n')
                si = (directory / 'test.si').read_text()
                si = si.replace(f'P{pin + 21}', '!primary').replace(f'P{pin + 11}', '!secondary')
                (directory / 'test.si').write_text(si)
                run('csim', ['-fl', '-u', library, 'test'], directory)

        # Normalize fields with no identified native consumer. Every
        # ATF750C defect case must still produce
        # the same fuses, diagnostics and simulated vector values.
        cases = 0
        available = {name for name, _, _ in devices(data)}
        for path in ([] if args.skip_metadata else sorted((ROOT / 'tests/defects').glob('*.json'))):
            case = json.loads(path.read_text())
            for number, variant in enumerate(case['variants']):
                device = variant.get('device', case.get('device', ''))
                if not device.startswith('v750c') or device not in available:
                    continue
                results = []
                for label, lib in [('original', library), ('canonical', canonical)]:
                    directory = root / f'{path.stem}-{number}-{label}'
                    directory.mkdir()
                    (directory / 'test.pld').write_text(f'Name test; Device {device};\n' + variant['source'])
                    log = run('cupl', [variant.get('flags', '-jaxm1'), '-u', lib, 'test.pld'],
                              directory, 1 if 'diagnostic' in variant else 0)
                    result = []
                    if 'diagnostic' in variant:
                        assert re.search(variant['diagnostic'], log), log
                    else:
                        result.append(fuses(directory / 'test.jed'))
                        if 'vectors' in variant:
                            (directory / 'test.si').write_text(variant['vectors'])
                            for options in ['-l', '-fl']:
                                run('csim', [options, '-u', lib, 'test'], directory)
                                lines = vector_lines((directory / 'test.so').read_text())
                                assert lines
                                result.append(lines)
                    results.append(result)
                assert results[0] == results[1], (path.stem, number)
                cases += 1
    print(f'PASS: 4 option maps, 84 columns, 171 rows, 80 feedback maps; '
          f'{cases} metadata comparisons; {checks} compiler/simulator runs')
    return 0


if __name__ == '__main__':
    sys.exit(main())
