#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Validate 22V10 hardware lowering against fuse coordinates and behavior."""
import argparse
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
from devlang.compare import images, records, compare_image
from devlang.syntax import Error
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
    source = (ROOT / 'devices/families/gal22v10.dev').read_text()
    compiled = devcompile.compile_text(source)
    generated = images(compiled)
    reference_data = devcompile.compile_directory(ROOT / 'devices', list(generated), hardware=False)
    originals = images(reference_data)
    assert set(generated) == {'g22v10', 'g22v10lcc', 'g22v10i', 'p22v10', 'p22v10lcc'}
    assert generated['g22v10lcc'] == generated['g22v10i']
    expected_differences = {'g22v10': set(), 'g22v10lcc': {'attributes', 'connections', 'fuse_plot'},
                            'g22v10i': {'attributes', 'connections', 'fuse_plot'},
                            'p22v10': {'attributes', 'connections'},
                            'p22v10lcc': {'attributes', 'connections', 'fuse_plot'}}
    comparisons = {}
    for name, image in generated.items():
        report = compare_image(image, originals[name], family='gal22v10')
        assert {d['section'] for d in report['differences']} == expected_differences[name], report
        comparisons[name] = report
        s = records(image)
        assert len(s[7]) == 92 and len(s[10]) == 81
        physical_rows = [r for r in s[7] if r[4] == 14]
        actual_rows = set()
        for r in physical_rows:
            start, count = int.from_bytes(r[10:14], 'little'), int.from_bytes(r[31:33], 'little')
            assert r[27] == 44 and start % 44 == 0
            new_rows = set(range(start // 44, start // 44 + count))
            assert not actual_rows & new_rows
            actual_rows.update(new_rows)
        assert actual_rows == set(range(132))
    rejected = 0
    for before, after in [
            ('columns = 44', 'columns = 43'), ('terms = 8 mode_fuse', 'terms = 10 mode_fuse'),
            ('enable_row = 10', 'enable_row = 9'), ('mode_fuse = 5810', 'mode_fuse = 5808'),
            ('registered_normal = 1', 'registered_normal = 4'),
            ('feedback_column = 6', 'feedback_column = 2'),
            ('registered = inverted(state.q)', 'registered = state.q'),
            ('first_fuse = 5828', 'first_fuse = 5827'),
            ('clock = device.global_clock', 'clock = pin(2)'),
            ('unconnected = [1, 8, 15, 22]', 'unconnected = [1, 8, 15, 21]')]:
        assert before in source
        try:
            devcompile.compile_text(source.replace(before, after, 1))
        except Error:
            rejected += 1
        else:
            raise AssertionError(f'invalid hardware description accepted: {after}')
    pin_map = dict(zip(range(1, 25), [*range(2, 8), *range(9, 15),
                                     *range(16, 22), *range(23, 29)]))
    runs, cases, plots = 0, [], []
    with tempfile.TemporaryDirectory(prefix='devgal22v10-') as temp:
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
            assert p.returncode == 0, (tool, argv, cwd.name, p.stdout, p.stderr)
        for device in generated:
            number = (lambda n: pin_map[n]) if device.endswith('lcc') or device == 'g22v10i' else (lambda n: n)
            loaded = root / f'{device}.image'
            run('devlib_read_test', [library, device, loaded], root)
            assert loaded.read_bytes() == generated[device], 'generated device required reader repair'
            for index, pin in enumerate(range(23, 13, -1)):
                for mode in ('combinational', 'inverted', 'registered', 'controls', 'feedback'):
                    body = (f'pin {number(1)}=clk; pin {number(2)}=a; pin {number(3)}=b; '
                            f'pin {number(4)}=r; pin {number(5)}=s; pin {number(6)}=oe; '
                            f'pin {number(pin)}=q;\n')
                    if mode == 'combinational':
                        body += 'q=a&b;\n'
                        vectors = '000001 L\n001001 L\n010001 L\n011001 H\n'
                    elif mode == 'inverted':
                        body += 'q=!(a&b);\n'
                        vectors = '000001 H\n001001 H\n010001 H\n011001 L\n'
                    elif mode == 'registered':
                        body += 'q.d=a;\n'
                        vectors = '000001 L\nC10001 H\n000001 H\nC00001 L\n'
                    elif mode == 'controls':
                        body += 'q.d=a; q.ar=r; q.sp=s; q.oe=oe;\n'
                        vectors = 'C10001 H\n000101 L\nC00011 H\nC00000 Z\n010001 L\n'
                    else:
                        other = 22 if pin == 23 else 23
                        body += f'pin {number(other)}=y; q.d=a; y=q&b;\n'
                        vectors = '000001 LL\nC11001 HH\n000001 HL\nC01001 LL\n'
                    order = 'clk,a,b,r,s,oe,q' + (',y' if mode == 'feedback' else '')
                    maps, simulations = [], []
                    for label, lib in [('original', reference), ('generated', library)]:
                        work = root / f'{device}-{pin}-{mode}-{label}'
                        work.mkdir()
                        (work / 'test.pld').write_text(f'Name test; Device {device};\n' + body)
                        (work / 'test.si').write_text(f'ORDER: {order};\nVECTORS:\n' + vectors)
                        run('cupl', ['-jaxm1', '-u', lib, 'test.pld'], work)
                        bits = fuses(work / 'test.jed')
                        assert len(bits) == (5828 if device.startswith('p') else 5892)
                        expected = {'combinational': [1, 1], 'inverted': [0, 1],
                                    'registered': [1, 0], 'controls': [1, 0], 'feedback': [1, 0]}[mode]
                        assert bits[5808 + 2 * index:5810 + 2 * index] == expected, (device, pin, mode)
                        maps.append(bits)
                        for flags in ('-l', '-fl'):
                            run('csim', [flags, '-u', lib, 'test'], work)
                            simulations.append(vector_lines((work / 'test.so').read_text()))
                    assert maps[0] == maps[1], (device, pin, mode, 'fuse difference')
                    assert all(x == simulations[0] for x in simulations), (device, pin, mode)
                    cases.append(dict(device=device, pin=pin, mode=mode))
            # One asynchronous-reset equation must reach both physical
            # registers even when only one output spells out the control.
            body = (f'pin {number(1)}=clk; pin {number(2)}=a; pin {number(3)}=b; '
                    f'pin {number(4)}=r; pin {number(23)}=q0; pin {number(22)}=q1; '
                    'q0.d=a; q1.d=b; q0.ar=r;')
            for label, lib in [('original', reference), ('generated', library)]:
                work = root / f'{device}-shared-reset-{label}'
                work.mkdir()
                (work / 'test.pld').write_text(f'Name test; Device {device};\n' + body)
                (work / 'test.si').write_text('ORDER: clk,a,b,r,q0,q1;\nVECTORS:\nC110 HH\n0001 LL\n')
                run('cupl', ['-jaxm1', '-u', lib, 'test.pld'], work)
                for flags in ('-l', '-fl'):
                    run('csim', [flags, '-u', lib, 'test'], work)

            documents = []
            for label, lib in [('original', reference), ('generated', library)]:
                work = root / f'{device}-plot-{label}'
                work.mkdir()
                (work / 'test.pld').write_text(f'Name test; Device {device}; '
                    f'pin {number(2)}=a; pin {number(23)}=q; q=a;')
                run('cupl', ['-jafxm1', '-u', lib, 'test.pld'], work)
                text = (work / 'test.doc').read_text()
                documents.append(text[text.index('Fuse Plot'):])
            headers = re.findall(r'Pin #(\d+)\s+(\d+) Mode[^\n]*\n\s+(\d+)', documents[1])
            starts = [1, 10, 21, 34, 49, 66, 83, 98, 111, 122]
            assert headers == [(str(number(23 - i)), f'{5808 + 2 * i:05d}', f'{row * 44:05d}')
                               for i, row in enumerate(starts)], (device, headers)
            same_plot = documents[0] == documents[1]
            assert same_plot == (device not in ('g22v10lcc', 'g22v10i'))
            plots.append(dict(device=device, matches_original=same_plot, row_labels_verified=True))
    report = dict(cases=len(cases), tool_runs=runs, rejected_descriptions=rejected,
                  binary_comparison=comparisons, shared_reset_cases=len(generated), plots=plots)
    if args.report:
        args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(f'PASS: {len(cases)} 22V10 compiler/ABS/JEDEC cases; {runs} native runs; {rejected} rejected descriptions')
    return 0


if __name__ == '__main__':
    sys.exit(main())
