#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Check CBGA132 ball assignments against equivalent TQFP100 designs."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from defects import fuses
from fit150x_edif import one_cell

# ATF1508BE data sheet 3663A, tables 13-1/13-2 (pages 23-25).
# MC17 is G2: figure 1-2 (page 4) identifies C2 as the separate OE1
# input, and G2 is the only remaining I/O ball after the other 79.
MACROCELLS = '''
G1 - F1 - F2 F3 - E1 E2 - E3 - D1 D2 - C1
G2 - G3 - H1 H2 - H3 J1 - J2 - K1 K3 - M9
B1 - B2 - A1 B4 - A4 C5 - B5 - A5 C6 - N10
P2 - M3 - N3 P3 - M4 M5 - N5 - P5 M6 - N6
G13 - G12 - F14 F13 - F12 E13 - E12 - D14 D13 - D12
H12 - H13 - J13 J12 - K14 K13 - L14 - L13 L12 - M10
C12 - B12 - A12 C11 - B11 A11 - C10 - A10 C9 - B9
N14 - N13 - P14 P12 - M11 N11 - P11 - P10 P9 - M8
'''.split()
TQFP = '''
2 0 1 0 100 99 0 98 97 0 96 0 94 93 0 92
14 0 13 0 12 10 0 9 8 0 7 0 6 5 0 4
25 0 24 0 23 22 0 21 20 0 19 0 17 16 0 15
37 0 36 0 35 33 0 32 31 0 30 0 29 28 0 27
40 0 41 0 42 44 0 45 46 0 47 0 48 49 0 50
52 0 53 0 54 55 0 56 57 0 58 0 60 61 0 62
63 0 64 0 65 67 0 68 69 0 70 0 71 72 0 73
75 0 76 0 77 78 0 79 80 0 81 0 83 84 0 85
'''.split()
BALLS = [f'{row}{column}' for row in 'ABCDEFGHJKLMNP'
         for column in range(1, 15)
         if row in 'ABCMNP' or column in (1, 2, 3, 12, 13, 14)]
DEDICATED = {'C3': 90, 'A3': 89, 'C2': 88, 'L2': 87}
PIN_MAP = {ball: int(pin) for ball, pin in zip(MACROCELLS, TQFP) if ball != '-'}
PIN_MAP.update(DEDICATED)


def main():
    bindir = Path(sys.argv[1]).resolve()
    suffix = '.exe' if os.name == 'nt' else ''
    checks = 0
    with tempfile.TemporaryDirectory(prefix='CBGA132 ') as temporary:
        root = Path(temporary)
        empty = root / 'empty'
        empty.mkdir()
        env = dict(os.environ, FITTERDIR=str(empty), LINCUPL_FIT150X_MODE='extended')

        def run(tool, args, work, success=True):
            nonlocal checks
            result = subprocess.run([str(bindir / (tool + suffix)), *map(str, args)],
                                    cwd=work, env=env, capture_output=True,
                                    text=True, timeout=30)
            checks += 1
            text = result.stdout + result.stderr
            assert (result.returncode == 0) == success, (tool, args, result.returncode, text)
            return text

        def fit(pins, package='CBGA132', source=None, options=(), success=True,
                tech='ATF1508BE', properties=False, tool='fit1508'):
            work = root / str(checks)
            work.mkdir()
            fmt = 'EDIF' if source is not None else 'EQN'
            path = 'test.edf' if source is not None else 'test.eqn'
            if source is None:
                source = 'INORDER=a b; OUTORDER=q; q=a*b;\n'
            if properties:
                for name, pin in pins.items():
                    direction = 'OUTPUT' if name == 'q' else 'INPUT'
                    source = source.replace(f'(port {name} (direction {direction}))',
                        f'(port {name} (direction {direction}) (property LOC (string "{pin}")))')
            (work / path).write_text(source)
            (work / 'board.pin').write_text(''.join(f'{name} : {pin}\n' for name, pin in pins.items()))
            args = ['-i', path, '-ifmt', fmt, '-device', package, '-tech', tech,
                    '-JTAG', 'OFF', '-preassign', 'KEEP', '-output_fast', 'OFF']
            if not properties:
                args += ['-pinfile', 'board.pin']
            run(tool, [*args, *options], work, success)
            assert (work / 'test.jed').exists() == success
            return work

        def simulate(work, names, vectors='00L\n01L\n10L\n11H\n', literal=False):
            if not literal:
                names = ['BALL_' + name if name.upper() in BALLS and name[0] in 'Pp'
                         else name for name in names]
            (work / 'test.si').write_text('ORDER:' + ','.join(names) + ';\nVECTORS:\n' + vectors)
            run('csim', ['-fl', 'test'], work)

        # The same constrained design must reach identical silicon fuses
        # through both package descriptions, for every bonded macrocell.
        first = None
        for mc, ball in enumerate(MACROCELLS, 1):
            if ball == '-':
                continue
            assigned = {'a': 'L2', 'b': 'C2', 'q': ball}
            work = fit(assigned)
            equivalent = fit({name: PIN_MAP[pin] for name, pin in assigned.items()}, 'TQFP100')
            assert fuses(work / 'test.jed') == fuses(equivalent / 'test.jed'), (mc, ball)
            report = (work / 'test.fit').read_text()
            assert f'Ball {ball} = q; /* MC {mc} */' in report
            simulate(work, ['l2', 'c2', ball.lower()])
            assert re.search(rf'^q\s*:\s*{ball}$', (work / 'test.pin').read_text(), re.M)
            if first is None:
                first = work

        # Every physical ball occurs once, with JEDEC ordinals and correct
        # bonded/unbonded macrocell reporting. Power and NC balls have no pad.
        report = (first / 'test.fit').read_text()
        listing = re.findall(r'^\s*(\d+)\s+([A-P]\d+)\s+([^\s]+)', report, re.M)
        assert [(int(n), ball) for n, ball, _ in listing] == list(enumerate(BALLS, 1))
        assert len(PIN_MAP) == 84 and len(BALLS) == 132
        cells = re.findall(r'^MC([1-9]\d*)\s+(\S+)', report, re.M)
        assert cells == [(str(mc), '0' if ball == '-' else ball)
                         for mc, ball in enumerate(MACROCELLS, 1)]
        for ball in BALLS:
            if ball not in PIN_MAP:
                fit({'a': 'L2', 'b': 'C2', 'q': ball}, success=False)
        for ball in ('D4', 'I1', 'O1', 'A0', 'A15', 'P15', 'G01'):
            fit({'a': 'L2', 'b': 'C2', 'q': ball}, success=False)

        # All dedicated inputs, ball labels in port properties, and the
        # P-row ambiguity with numeric P-prefixed pin names.
        for ball in DEDICATED:
            fit({'a': 'G1', 'b': 'P14', 'q': ball}, success=False)
            other = 'L2' if ball != 'L2' else 'C2'
            work = fit({'a': ball, 'b': other, 'q': 'P12'},
                       source=one_cell('AND2', ('A', 'B')), properties=True)
            simulate(work, [ball, other, 'p12'])
        work = fit({'a': 'l2', 'b': 'c2', 'q': 'p12'}, package='P1508B132')
        simulate(work, ['P86', 'P30', 'P130'])
        # The generated ball constraint file can be read back unchanged.
        before = fuses(work / 'test.jed')
        run('fit1508', ['-i', 'test.eqn', '-ifmt', 'EQN', '-device', 'CBGA132',
            '-tech', 'ATF1508BE', '-JTAG', 'OFF', '-preassign', 'KEEP',
            '-output_fast', 'OFF', '-pinfile', 'test.pin'], work)
        assert fuses(work / 'test.jed') == before

        # Numeric P<n> vector positions retain their meaning, while the
        # BALL_ prefix disambiguates physical locations in package row P.
        work = fit({'a': 'A10', 'en': 'P10', 'q': 'G1'},
                   source=one_cell('TRI', ('A', 'EN')))
        for order in (['P10', 'BALL_P10', 'G1'], ['p10', 'ball_p10', 'ball_g1']):
            simulate(work, order, '00Z\n10Z\n01L\n11H\n', literal=True)

        # Clock and clear selectors must name the same silicon controls
        # in both packages, including the shared GCLK3 I/O ball.
        for clock in ('L2', 'C3', 'M8'):
            assigned = {'clk': clock, 'd': 'G1', 'ar': 'A3', 'q': 'P14'}
            source = one_cell('DFFAR', ('CLK', 'D', 'AR'))
            work = fit(assigned, source=source)
            equivalent = fit({name: PIN_MAP[pin] for name, pin in assigned.items()},
                             'TQFP100', source=source)
            assert fuses(work / 'test.jed') == fuses(equivalent / 'test.jed'), clock
            simulate(work, list(assigned.values()), '001L\nC10H\nC00L\n011L\n')

        # Exercise every pad selectable by the six global OE multiplexers.
        oe_balls = ('C2 C3 H3 M4 F12 B11 P10 D1 L13 A5 J2 E3 A10 D14 '
                    'K1 P5 B9 D12 N6 M9 M8 C1 M10 N5 N10 E1 P12 B4 J12 C12').split()
        for enable in oe_balls:
            assigned = {'a': 'L2', 'en': enable, 'q': 'G1'}
            source = one_cell('TRI', ('A', 'EN'))
            work = fit(assigned, source=source)
            equivalent = fit({name: PIN_MAP[pin] for name, pin in assigned.items()},
                             'TQFP100', source=source)
            assert fuses(work / 'test.jed') == fuses(equivalent / 'test.jed'), enable
            try:
                simulate(work, list(assigned.values()), '00Z\n10Z\n01L\n11H\n')
            except AssertionError as error:
                raise AssertionError(f'global OE ball {enable}: {error}') from error

        # JTAG and power-down own their package-specific balls.
        for ball in ('M9', 'N10', 'M10', 'B9'):
            fit({'a': 'L2', 'b': 'C2', 'q': ball}, options=('-JTAG', 'ON'), success=False)
        for ball, option in [('F1', '-pd1'), ('G12', '-pd2')]:
            fit({'a': 'L2', 'b': 'C2', 'q': ball}, options=(option, 'ON'), success=False)

        # Reject unsupported silicon/package combinations instead of
        # silently placing the design into another package.
        fit({'a': 87, 'b': 88, 'q': 2}, tech='ATF1508AS', success=False)
        fit({'a': 37, 'b': 38, 'q': 6}, tool='fit1504', tech='ATF1504BE', success=False)
        bits = fuses(first / 'test.jed')[:74136]
        (root / 'as.jed').write_text('\x02Device f1508cbga132\n*QF74136*QP132*L0 ' +
                                   ''.join(map(str, bits)) + '*\x030000')
        text = run('jedcompare', ['as.jed', 'as.jed'], root, False)
        assert 'BE' in text and 'CBGA132' in text, text

    print(f'ATF1508BE CBGA132: {checks} checks passed')


if __name__ == '__main__':
    main()
