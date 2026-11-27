#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Validate ATF2500C hardware lowering, native compilation and simulation."""
import argparse
from collections import Counter
import itertools
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import devcompile
from devlang.compare import canonical, compare_image, images, records
from devlang.model import models
from devlang.syntax import Error, parse
from defects import fuses
from csim_jed import vector_lines


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('bin_dir', type=Path)
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    bindir = args.bin_dir.resolve()
    share = bindir.parent / 'share/lincupl'
    env = dict(os.environ, LINCUPL_SHARE=str(share))
    env.pop('LIBCUPL', None)
    env.pop('FITTERDIR', None)
    source = (ROOT / 'devices/families/atf2500.dev').read_text()
    compiled = devcompile.compile_text(source)
    assert compiled == devcompile.compile_text(source)
    descriptions = models(parse(source))
    generated = images(compiled)
    reference_data = devcompile.compile_directory(ROOT / 'devices', list(generated), hardware=False)
    originals = images(reference_data)
    assert set(generated) == {'v2500c', 'v2500cppk', 'v2500clcc', 'v2500cppklcc'}
    for m in descriptions:
        assert set(m['fuse_owners']) == set(range(71816))
        assert len(m['cells']) == 24
        assert len({c['preset_row'] for c in m['cells']}) == 8
        assert all(c['registers']['primary'].data.sources['combinational'].count == 4 for c in m['cells'])
        assert all(c['routing']['registered'].sources['independent'] is None for c in m['cells'])
        assert all(c['registers']['secondary'].data.count == 4 for c in m['cells'])
    comparisons = {}
    for name, image in generated.items():
        report = compare_image(image, originals[name], family='atf2500')
        expected = {'nodes', 'fuse_plot'}
        if name.endswith('lcc'):
            expected |= {'attributes', 'pins'}
        assert {d['section'] for d in report['differences']} == expected, report
        comparisons[name] = report
        a, _ = canonical(image, repairs=False, family='atf2500')
        b, _ = canonical(originals[name], family='atf2500')
        # Every reviewed node byte agrees; only shared-preset record order
        # differs. The compiler looks these up by array, pin and extension.
        assert Counter(map(bytes, a[7])) == Counter(map(bytes, b[7]))
        assert all(x == y for x, y in zip(a[7], b[7]) if x[4] != 3)
        assert a[9] == b[9] and a[10] == b[10]
        if name.endswith('lcc'):
            attr = bytearray(a[3][0])
            assert attr[11] == 4 and b[3][0][11] == 0
            attr[11] = 0
            assert attr == b[3][0]
            assert a[6][:160] == b[6][:160]
            # The four additional supply/ground pins use a deterministic
            # physical-pin order instead of historical input-table order.
            assert Counter(bytes(r[2:]) for r in a[6][160:]) == Counter(bytes(r[2:]) for r in b[6][160:])
        s = records(image)
        covered = set()
        for r in s[7]:
            if r[4] != 14:
                continue
            first, count = int.from_bytes(r[10:14], 'little') // 172, int.from_bytes(r[31:33], 'little')
            assert r[27] == 172
            covered.update(range(first, first + count))
        assert covered == set(range(416))
    rejected = 0
    for before, after in [
            ('columns = 172', 'columns = 171'),
            ('output_fuses = 71556', 'output_fuses = 71552'),
            ('register_fuses = 71653', 'register_fuses = 71649'),
            ('first_row = 17', 'first_row = 0'),
            ('primary_node = 81', 'primary_node = 82'),
            ('high = 46 low = 47', 'high = 44 low = 45'),
            ('extended = 2', 'extended = 3'),
            ('product_term = 0', 'product_term = 1'),
            ('data = select(output_source.registered, routing.registered, middle_sum)', 'data = first_sum'),
            ('independent = unsupported', 'independent = first_sum'),
            ('power_up = low', 'power_up = high'),
            ('first_fuse = 71746', 'first_fuse = 71745'),
            ('user_row {', 'setting ignored {} user_row {'),
            ('user_row {', 'array ignored {} user_row {'),
            ('routing combinational {', 'routing ignored {} routing combinational {'),
            ('register primary {', 'register ignored {} register primary {'),
            ('setting term_routing {', 'setting ignored {} setting term_routing {'),
            ('ground = [4, 26, 33]', 'ground = [4, 26, 32]')]:
        assert before in source, before
        try:
            devcompile.compile_text(source.replace(before, after, 1))
        except Error:
            rejected += 1
        else:
            raise AssertionError(f'invalid hardware description accepted: {after}')
    runs, cases, plots = 0, [], []
    with tempfile.TemporaryDirectory(prefix='devatf2500-') as temp:
        root = Path(temp)
        reference = root / 'catalog.dl'
        reference.write_bytes(reference_data)
        library = root / 'hardware.dl'
        library.write_bytes(compiled)

        def run(tool, argv, cwd):
            nonlocal runs
            exe = bindir / (tool + ('.exe' if os.name == 'nt' else ''))
            p = subprocess.run([str(exe), *map(str, argv)], cwd=cwd, env=env,
                               capture_output=True, text=True, timeout=20)
            runs += 1
            assert p.returncode == 0, (tool, argv, cwd.name, p.returncode, p.stdout, p.stderr)

        for m in descriptions:
            number = m['package'].number
            for device, options in m['variants'].items():
                loaded = root / f'{device}.image'
                run('devlib_read_test', [library, device, loaded], root)
                assert loaded.read_bytes() == generated[device], 'generated device required reader repair'
                for cell in m['cells']:
                    pin = cell['pin']
                    for mode in ('combinational', 'registered', 'toggle', 'controls', 'primary', 'secondary'):
                        body = (f'pin {number(1)}=clk; pin {number(2)}=a; '
                                f'pin {number(17)}=r; pin {number(18)}=s; pin {number(19)}=oe; '
                                f'pin {number(pin)}=q; ')
                        if mode == 'combinational':
                            body += 'q=a;'
                            vectors = '00001 L\n01001 H\n00001 L\n'
                        elif mode == 'registered':
                            body += 'q.d=a; q.ck=clk;'
                            vectors = '00001 L\nC1001 H\n00001 H\nC0001 L\n'
                        elif mode == 'toggle':
                            body += 'q.t=a; q.ck=clk;'
                            vectors = '00001 L\nC1001 H\nC1001 L\nC0001 L\nC1001 H\n'
                        elif mode == 'controls':
                            body += 'q.d=a; q.ck=clk; q.ar=r; q.sp=s; q.oe=oe;'
                            vectors = 'C1001 H\n00001 H\n00101 L\n00011 L\nC0011 H\n00000 Z\n00001 H\n'
                        else:
                            alias = number(cell['feedback'][mode].alias)
                            body += f'pinnode {alias}=b; b.d=a; b.ck=clk; q=b;'
                            vectors = '00001 L\nC1001 H\n00001 H\nC0001 L\n'
                        maps, simulations = [], []
                        for label, lib in [('original', reference), ('generated', library)]:
                            work = root / f'{device}-{pin}-{mode}-{label}'
                            work.mkdir()
                            (work / 'test.pld').write_text(f'Name test; Device {device};\n' + body)
                            (work / 'test.si').write_text('ORDER: clk,a,r,s,oe,q;\nVECTORS:\n' + vectors)
                            run('cupl', ['-jaxm1', '-u', lib, 'test.pld'], work)
                            bits = fuses(work / 'test.jed')
                            assert len(bits) == 71816 and bits[71745] == options[0]
                            assert bits[71746:71810] == [1] * 64
                            maps.append(bits)
                            for flags in ('-l', '-fl'):
                                run('csim', [flags, '-u', lib, 'test'], work)
                                simulations.append(vector_lines((work / 'test.so').read_text()))
                        assert maps[0] == maps[1], (device, pin, mode, 'fuse difference')
                        assert all(x == simulations[0] for x in simulations), (device, pin, mode)
                        cases.append(dict(device=device, pin=pin, mode=mode))
                work = root / f'{device}-plot'
                work.mkdir()
                (work / 'test.pld').write_text(f'Name test; Device {device}; '
                    f'pin {number(2)}=a; pin {number(4)}=q; q=a;')
                run('cupl', ['-jafxm1', '-u', library, 'test.pld'], work)
                document = (work / 'test.doc').read_text()
                rows = re.findall(r'^(\d{5}) ([x-]+)\s*$', document, re.M)
                assert [(int(first), len(bits)) for first, bits in rows] == [
                    *((row * 172, 172) for row in range(416)), (71552, 264)]
                plots.append(dict(device=device, displayed_fuses=sum(len(bits) for _, bits in rows)))

        # Independent hand-programmed maps separate all three four-term
        # sums and cover encodings that the compiler need not choose.
        routing_cases = 0
        for m in descriptions:
            number, device = m['package'].number, m['mnemonic']
            for mode, toggle, low in itertools.product(range(4), range(2), range(2)):
                work = root / f'{device}-routing-{mode}-{toggle}-{low}'
                work.mkdir()
                bits = [0] * 71816
                for first, column in [(68456, 46), (69144, 48), (69832, 134), (71208, None)]:
                    bits[first:first + 172] = [1] * 172
                    if column is not None:
                        bits[first + column] = 0
                bits[71644:71648] = [mode & 1, mode >> 1, 1, 1 - low]
                bits[71742] = toggle
                (work / 'test.jed').write_text(f'\x02QF71816*QP{m["package"].pins}*L0 '
                                             + ''.join(map(str, bits)) + '*\x030000')
                order = ','.join(f'P{number(p)}' for p in (2, 3, 17, 4, 41, 65))
                lines = []
                for a, b, c in itertools.product(range(2), repeat=3):
                    q = [a | b, a, a | b | c, a | (c if toggle else 0)][mode] ^ low
                    bypass = c if mode == 3 else 0
                    lines.append(f'{a}{b}{c} {"H" if q else "L"}{"H" if bypass else "L"}L')
                (work / 'test.si').write_text(f'ORDER: {order};\nVECTORS:\n' + '\n'.join(lines) + '\n')
                run('csim', ['-fl', '-u', library, device, 'test'], work)
                routing_cases += 1
            for mode, toggle in itertools.product((0, 2, 3), range(2)):
                work = root / f'{device}-registered-routing-{mode}-{toggle}'
                work.mkdir()
                bits = [0] * 71816
                for first, column in [(68456, 46), (69144, 48), (69832, 134),
                                      (70692, 132), (71208, None)]:
                    bits[first:first + 172] = [1] * 172
                    if column is not None:
                        bits[first + column] = 0
                bits[71644:71648] = [mode & 1, mode >> 1, 0, 1]
                bits[71741], bits[71743] = toggle, 1
                (work / 'test.jed').write_text(f'\x02QF71816*QP{m["package"].pins}*L0 '
                                             + ''.join(map(str, bits)) + '*\x030000')
                order = ','.join(f'P{number(p)}' for p in (1, 2, 3, 17, 18, 4, 65))
                state, lines = 0, []
                for a, b, c in itertools.product(range(2), repeat=3):
                    value = a | b | (c if mode == 2 else 0)
                    state = state ^ value if toggle else value
                    lines.append(f'C{a}{b}{c}1 {"HH" if state else "LL"}')
                (work / 'test.si').write_text(f'ORDER: {order};\nVECTORS:\n' + '\n'.join(lines) + '\n')
                run('csim', ['-fl', '-u', library, device, 'test'], work)
                routing_cases += 1
            for role, data_row, clock_row, target, clock_fuse in [
                    ('primary', 68456, 70692, 4, 71743),
                    ('secondary', 69832, 70864, 41, 71744)]:
                work = root / f'{device}-{role}-clock-gate'
                work.mkdir()
                bits = [0] * 71816
                for first, column in [(data_row, 46), (clock_row, 48), (71208, None)]:
                    bits[first:first + 172] = [1] * 172
                    if column is not None:
                        bits[first + column] = 0
                bits[71647] = bits[clock_fuse] = 1
                (work / 'test.jed').write_text(f'\x02QF71816*QP{m["package"].pins}*L0 '
                                             + ''.join(map(str, bits)) + '*\x030000')
                order = ','.join(f'P{number(p)}' for p in (1, 2, 3, target))
                # Raising the product clock while PIN1 is already high
                # must create an edge at the physical AND gate's output.
                (work / 'test.si').write_text(f'ORDER: {order};\nVECTORS:\n'
                    '000 L\n110 L\n111 H\n101 H\n100 H\n101 L\n')
                run('csim', ['-fl', '-u', library, device, 'test'], work)
                routing_cases += 1
                # A constant-true product is still gated by PIN1; it must
                # not collapse the entire selected clock to a constant.
                bits[clock_row:clock_row + 172] = [1] * 172
                (work / 'test.jed').write_text(f'\x02QF71816*QP{m["package"].pins}*L0 '
                                             + ''.join(map(str, bits)) + '*\x030000')
                order = ','.join(f'P{number(p)}' for p in (1, 2, target))
                (work / 'test.si').write_text(f'ORDER: {order};\nVECTORS:\n'
                    '00 L\n11 H\n00 H\n10 L\nC1 H\nC0 L\n')
                run('csim', ['-fl', '-u', library, device, 'test'], work)
                routing_cases += 1
    report = dict(cases=len(cases), tool_runs=runs, rejected_descriptions=rejected,
                  independent_routing_maps=routing_cases, binary_comparison=comparisons, plots=plots)
    if args.report:
        args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(f'PASS: {len(cases)} ATF2500C compiler/ABS/JEDEC cases; {runs} native runs; '
          f'{routing_cases} independent maps; {rejected} rejected descriptions')
    return 0


if __name__ == '__main__':
    sys.exit(main())
