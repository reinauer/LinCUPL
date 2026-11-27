#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Fit BE maps and check their coordinates and behavior independently."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from defects import fuses
from fit150x_edif import one_cell


def main():
    bindir = Path(sys.argv[1]).resolve()
    suffix = '.exe' if os.name == 'nt' else ''
    checks = 0
    with tempfile.TemporaryDirectory(prefix='BE fitter ') as temporary:
        root = Path(temporary)
        empty = root / 'empty'
        empty.mkdir()
        env = dict(os.environ, FITTERDIR=str(empty), LINCUPL_FIT150X_MODE='extended')

        def run(tool, arguments, work, success=True):
            nonlocal checks
            p = subprocess.run([str(bindir / (tool + suffix)), *map(str, arguments)],
                               cwd=work, env=env, capture_output=True, text=True, timeout=30)
            checks += 1
            text = p.stdout + p.stderr
            assert (p.returncode == 0) == success, (tool, arguments, p.returncode, text)
            return text

        def fit(part, package, pins, kind, tech, options=(), success=True):
            work = root / str(checks)
            work.mkdir()
            if kind == 'AND':
                source = one_cell('AND2', ('A', 'B'))
                names = ('a', 'b', 'q')
                lib = 'GATE 0 0 AND2 1 50 Q=A*B;\nPIN * UNKNOWN 1 1 1 0 1 0\n'
            else:
                source = one_cell('CUSTOM', ('CK', 'RST', 'DATA'), ('OUT',))
                names = ('ck', 'rst', 'data', 'out')
                gate, clock = {'D': ('DG', 'RECK'), 'T': ('TG', 'RECK'), 'L': ('LG', 'L1')}[kind[0]]
                outputs = '* OUT *' if kind.endswith('N') else 'OUT * *'
                lib = (f'PGATE 0 0 {gate} CUSTOM 1 CK {clock} RST * * * DATA {outputs}\n'
                       'RELATED_OUTPUT_PIN OUT 10\nPIN * UNKNOWN 1 1 1 0 1 0 0 0\n')
            (work / 'test.edf').write_text(source)
            (work / 'cells.lib').write_text(lib)
            (work / 'test.pin').write_text(''.join(f'{name} : {pin}\n' for name, pin in zip(names, pins)))
            output = run(f'fit{part}', ['-i', 'test.edf', '-ifmt', 'EDIF', '-lib', 'cells.lib',
                '-device', package, '-tech', f'ATF{part}{tech}', '-JTAG', 'OFF',
                '-pinfile', 'test.pin', '-preassign', 'KEEP', '-output_fast', 'OFF',
                '-UES', 'AZ', *options], work, success)
            if not success:
                assert not (work / 'test.jed').exists(), output
                return output
            assert 'Design fits successfully' in output, output
            assert f'ATF{part} Fitter' in output
            report = (work / 'test.fit').read_text()
            assert f'ATF{part} Fitter' in report.splitlines()[0]
            assert f'Tech_name = ATF{part}{tech}' in report
            return work, fuses(work / 'test.jed')

        for part, package, pins, mc, cfg, count in [
                (1502, 'TQFP44', (37, 38, 42), 15765, 16750, 16814),
                (1504, 'TQFP44', (37, 38, 6), 32565, 34134, 34198),
                (1504, 'TQFP100', (87, 88, 14), 32565, 34134, 34198),
                (1508, 'TQFP100', (87, 88, 2), 64725, 74082, 74146)]:
            for kind in ('AND', 'D', 'DN', 'T', 'TN', 'L'):
                assigned = pins if kind == 'AND' else (pins[0], pins[1], pins[0] + 3, pins[2])
                as_work, a = fit(part, package, assigned, kind, 'AS')
                be_work, b = fit(part, package, assigned, kind, 'BE')
                assert len(b) == count and len(a) == count - (10 if part == 1508 else 6)
                assert f'JEDEC file for: ATF{part}BE' in (be_work / 'test.jed').read_text()
                # Product terms and interconnect retain the AS coordinates.
                pt_end = {1502: 15360, 1504: 30720, 1508: 61440}[part]
                uim = {1502: 16320, 1504: 32640, 1508: 65280}[part]
                assert a[:pt_end] == b[:pt_end]
                assert a[uim:cfg] == b[uim:cfg]
                assert b[cfg] == 0 and b[cfg + 33] == 0
                assert b[cfg + 34:cfg + 40] == [1] * 6
                assert b[cfg + 40:cfg + 56] == [int(x) for x in '0100000101011010']
                assert b[cfg + 56:] == [0] * 8
                for block in range({1502: 2, 1504: 4, 1508: 8}[part]):
                    assert b[pt_end + block * 480 + 432:pt_end + (block + 1) * 480] == [1] * 48
                if kind != 'AND':
                    expected = [1, 0] if kind.startswith('T') and part != 1502 else [0, int(kind != 'L')]
                    assert b[mc + 8:mc + 10] == expected, (part, kind, b[mc:mc + 27])
                run('jedcompare', [*(['--sequential'] if kind != 'AND' else []),
                                   as_work / 'test.jed', be_work / 'test.jed'], root)
                vectors = {
                    'AND': '00L\n01L\n10L\n11H\n',
                    'D': '000L\nC01H\n001H\nC00L\n011L\n',
                    'DN': '010H\nC01L\n001L\nC00H\n011H\n',
                    'T': '000L\nC01H\nC00H\nC01L\n011L\nC01H\n',
                    'TN': '010H\nC01L\nC00L\nC01H\n011H\nC01L\n',
                    'L': '000L\n101H\n000H\n011L\n101H\n100L\n'}[kind]
                for work in (as_work, be_work):
                    (work / 'test.si').write_text('ORDER:' + ','.join(f'P{p}' for p in assigned) +
                                                  ';\nVECTORS:\n' + vectors)
                    run('csim', ['-fl', 'test'], work)

        # Dedicated inputs have reversed pull-up/down encoding relative
        # to macrocell pins, and their hysteresis bit is active-low.
        for part, pin, mc, cfg in [(1502, 42, 15765, 16750), (1504, 6, 32565, 34134)]:
            _, b = fit(part, 'TQFP44', (37, 38, pin), 'AND', 'BE',
                       ['-pin_keep', 'a', '-schmitt_trigger', 'b', '-pull_up', 'q'])
            assert b[cfg + 23:cfg + 25] == [1, 1]
            assert b[cfg + 21] == 0 and b[mc:mc + 2] == [1, 0], (part, b[cfg + 19:cfg + 23], b[mc:mc + 2])
            _, b = fit(part, 'TQFP44', (37, 38, pin), 'AND', 'BE', ['-pull_down', 'a'])
            assert b[cfg + 23:cfg + 25] == [1, 0]
            for option in [('-voltage_level_A', '3.3'), ('-unused_To_Ground', 'ON'),
                           ('-POR', 'one'), ('-mc_power', 'ON'), ('-pull_down', 'q')]:
                assert 'BE ' in fit(part, 'TQFP44', (37, 38, pin), 'AND', 'BE', option, False)
    print(f'PASS: {checks} BE fitter layout, simulation, equivalence and rejection checks')


if __name__ == '__main__':
    main()
