#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Check package-independent fuse maps and simulated register behavior."""
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
from defects import fuses
from csim_jed import vector_lines


def check_plot_mappings(root, run):
    """Package output order may differ from physical macrocell order."""
    original = '[2..7, 9..14, 16..21, 23..28]'
    swapped = '[2..7, 9..14, 16..21, 23..25, 27, 26, 28]'
    physical = [*range(2, 8), *range(9, 15), *range(16, 22), *range(23, 29)]
    cases = 0
    for family, device, width, rows in [
            ('atf750c', 'v750clcc', 84, [0, 13, 28, 45, 64, 85, 106, 125, 142, 157]),
            ('gal22v10', 'g22v10lcc', 44, [1, 10, 21, 34, 49, 66, 83, 98, 111, 122])]:
        source = (ROOT / f'devices/families/{family}.dev').read_text()
        maps = []
        for label, mapping in [('ordered', physical),
                               ('swapped', physical[:21] + [physical[22], physical[21], physical[23]])]:
            work = root / f'{family}-plot-{label}'
            work.mkdir()
            text = source if label == 'ordered' else source.replace(original, swapped)
            library = work / 'hardware.dl'
            library.write_bytes(devcompile.compile_text(text, devices=[device]))
            (work / 'test.pld').write_text(f'Name test; Device {device}; '
                f'pin {mapping[1]}=a; pin {mapping[22]}=q; q=a;')
            run('cupl', ['-jafxm1', '-u', library, 'test.pld'], work)
            document = (work / 'test.doc').read_text()
            headers = re.findall(r'Pin #(\d+)[^\n]*\n\s+(\d+)', document)
            assert headers == [(str(mapping[22 - i]), f'{row * width:05d}')
                               for i, row in enumerate(rows)], (family, label, headers)
            maps.append(fuses(work / 'test.jed'))
            cases += 1
        assert maps[0] == maps[1], (family, 'pin remapping changed programming fuses')
    return cases


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
    data = devcompile.compile_text((ROOT / 'devices/families/atf750c.dev').read_text())
    pin_map = dict(zip(range(1, 25), [*range(2, 8), *range(9, 15),
                                     *range(16, 22), *range(23, 29)]))
    variants = ('v750c', 'v750cext', 'v750cppk', 'v750cextppk')
    results, runs = [], 0
    with tempfile.TemporaryDirectory(prefix='devpackages-') as temp:
        root = Path(temp)
        library = root / 'hardware.dl'
        library.write_bytes(data)

        def run(tool, argv, cwd):
            nonlocal runs
            exe = bindir / (tool + ('.exe' if os.name == 'nt' else ''))
            p = subprocess.run([str(exe), *map(str, argv)], cwd=cwd, env=env,
                               capture_output=True, text=True, timeout=20)
            runs += 1
            assert p.returncode == 0, (tool, argv, cwd.name, p.stdout, p.stderr)

        plot_cases = check_plot_mappings(root, run)
        for variant in variants:
            for index, pin in enumerate(range(23, 13, -1)):
                for mode in ('combinational', 'd', 't', 'dual_feedback'):
                    maps, simulations = [], []
                    for package in ('DIP24', 'PLCC28'):
                        def number(n):
                            return n if package == 'DIP24' else pin_map.get(n, n + 4)
                        device = variant + ('lcc' if package == 'PLCC28' else '')
                        work = root / f'{device}-{pin}-{mode}'
                        work.mkdir()
                        source = f'Name test; Device {device};\n'
                        source += f'pin {number(1)}=clk; pin {number(2)}=a; pin {number(3)}=b;\n'
                        if mode == 'dual_feedback':
                            output = 22 if pin == 23 else 23
                            source += (f'pin {number(output)}=q; pinnode {number(pin + 21)}=p0; '
                                       f'pinnode {number(pin + 11)}=p1;\n'
                                       'p0.d=a; p1.d=b; p0.ckmux=clk; p1.ckmux=clk; q=p0&p1;\n')
                            vectors = '000 L\nC10 L\nC01 L\nC11 H\n000 H\nC00 L\n'
                        else:
                            source += f'pin {number(pin)}=q;\n'
                            if mode == 'combinational':
                                source += 'q=a#!b;\n'
                                vectors = '000 H\n001 L\n010 H\n011 H\n'
                            else:
                                source += f'q.{mode}=a; q.ckmux=clk;\n'
                                vectors = ('000 L\nC10 H\n000 H\nC00 L\nC10 H\n' if mode == 'd'
                                           else '000 L\nC10 H\nC00 H\nC10 L\nC10 H\n')
                        (work / 'test.pld').write_text(source)
                        (work / 'test.si').write_text('ORDER: clk,a,b,q;\nVECTORS:\n' + vectors)
                        run('cupl', ['-jaxm1', '-u', library, 'test.pld'], work)
                        bits = fuses(work / 'test.jed')
                        if mode in ('d', 't', 'dual_feedback'):
                            assert bits[14395 + 4 * index] == 1
                            assert bits[14397 + 4 * index] == int(mode == 't')
                        if mode == 'dual_feedback':
                            assert bits[14396 + 4 * index] == 1
                        assert bits[14499:14501] == [int('ext' in variant), int('ppk' in variant)]
                        maps.append(bits)
                        for flags in ('-l', '-fl'):
                            run('csim', [flags, '-u', library, 'test'], work)
                            simulations.append(vector_lines((work / 'test.so').read_text()))
                    assert maps[0] == maps[1], (variant, pin, mode, 'package changed fuse map')
                    assert all(s == simulations[0] for s in simulations), (variant, pin, mode)
                    results.append(dict(variant=variant, pin=pin, mode=mode))
    report = dict(package_pairs=len(results), plot_mappings=plot_cases, tool_runs=runs, cases=results)
    if args.report:
        args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(f'PASS: {len(results)} DIP24/PLCC28 fuse-map pairs and ABS/JEDEC simulations; {runs} tool runs')
    return 0


if __name__ == '__main__':
    sys.exit(main())
