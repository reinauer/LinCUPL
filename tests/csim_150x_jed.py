# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, LinCUPL team
"""Exercise ATF150x fuse decoding with Boolean and sequential vectors."""
import itertools
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from fit150x_edif import document, one_cell, tt2
from defects import fuses


def main():
    bindir = Path(sys.argv[1]).resolve()
    suffix = '.exe' if os.name == 'nt' else ''
    checks = 0
    with tempfile.TemporaryDirectory(prefix='ATF150x JEDEC ') as temp:
        root = Path(temp)
        (root / 'empty').mkdir()
        mode = sys.argv[2] if len(sys.argv) > 2 else 'extended'
        library = bindir.parent / 'share/lincupl/lincupl.dl'
        env = dict(os.environ, FITTERDIR=str(root / 'empty'), LINCUPL_FIT150X_MODE=mode,
                   LIBCUPL=str(library))

        def run(tool, args, work, expected=0):
            nonlocal checks
            checks += 1
            p = subprocess.run([str(bindir / (tool + suffix)), *args], cwd=work,
                               env=env, capture_output=True, text=True, timeout=20)
            text = p.stdout + p.stderr
            assert p.returncode == expected, (work, args, p.returncode, text)
            return text

        def fit(part, source, package=None, fmt='EDIF', options=(), pins=None):
            work = root / str(checks)
            work.mkdir()
            (work / 'test.in').write_text(source)
            if package is None: package = 'PLCC84' if part == 1508 else 'PLCC44'
            if pins:
                (work / 'board.pin').write_text(''.join(f'{n}:{p}\n' for n, p in pins.items()))
            run(f'fit{part}', ['-i', 'test.in', '-ifmt', fmt, '-device', package,
                             '-tech', f'ATF{part}AS', '-JTAG', 'OFF',
                             *(['-pinfile', 'board.pin'] if pins else []), *options], work)
            text = (work / 'test.tt3').read_text()
            line = next(s for s in text.splitlines() if s.startswith('#$ PINS'))
            mapping = {name.lower(): int(pin) for name, pin in
                       re.findall(r'([^\s:+-]+)[+-]?:(\d+)', line)}
            assert not (work / 'test.abs').exists()
            return work, mapping

        def simulate(work, mapping, names, vectors):
            order = ','.join(f'P{mapping[n]}' for n in names)
            (work / 'test.si').write_text(f'ORDER:{order};\nVECTORS:\n' + vectors)
            return run('csim', ['-fl', 'test'], work)

        def compiled(part, source, order, vectors):
            work = root / str(checks)
            work.mkdir()
            package = 'plcc84' if part == 1508 else 'plcc44'
            (work / 'test.pld').write_text(f'Name test; Device f{part}{package};\n{source}')
            run('cupl', ['-jxm1', '-u', str(library), 'test.pld'], work)
            assert not (work / 'test.abs').exists()
            (work / 'test.si').write_text(f'ORDER:{order};\nVECTORS:\n{vectors}')
            run('csim', ['-fl', 'test'], work)
            return work

        def write_map(work, bits, device='', pins=44):
            title = f'Device {device}\n' if device else 'unnamed map\n'
            (work / 'test.jed').write_text(
                f'\x02{title}*QF{len(bits)}*QP{pins}*L0 ' +
                ''.join(map(str, bits)) + '*\x030000')

        maps = {}
        for part in (1502, 1504, 1508):
            for model, output, function in [
                ('AND2', 'Q', lambda a, b: a & b), ('OR2', 'Q', lambda a, b: a | b),
                ('NAND2', 'QN', lambda a, b: not (a & b)), ('NOR2', 'QN', lambda a, b: not (a | b)),
                ('XOR2', 'Q', lambda a, b: a ^ b), ('XNOR2', 'QN', lambda a, b: not (a ^ b)),
            ]:
                work, mapping = fit(part, one_cell(model, ('A', 'B'), (output,)),
                                    pins={'a': 2, 'b': 1, output.lower(): 4})
                vectors = ''.join(f'{a}{b}' + ('H' if function(a, b) else 'L') + '\n'
                                  for a, b in itertools.product((0, 1), repeat=2))
                simulate(work, mapping, ['a', 'b', output.lower()], vectors)
                run('jedcompare', ['test.jed', 'test.jed'], work)
                maps[part, model] = work / 'test.jed'
            text = run('jedcompare', [str(maps[part, 'AND2']), str(maps[part, 'OR2'])], root, 1)
            assert 'Not equivalent:' in text and 'Inputs:' in text, text

            # A parity truth table requires more than one macrocell when
            # XOR extraction is disabled, exercising the cascade chain.
            names = list('abcde')
            source = tt2(names, ['q'], lambda *bits: sum(bits) % 2, {})
            work, mapping = fit(part, source, fmt='TT', options=['-xor_synthesis', 'OFF'])
            vectors = ''.join(''.join(map(str, bits)) + ('H' if sum(bits) % 2 else 'L') + '\n'
                              for bits in itertools.product((0, 1), repeat=5))
            simulate(work, mapping, names + ['q'], vectors)
            # Borrowed cells must not drive additional package pins.
            text = run('jedcompare', ['test.jed', 'test.jed'], work)
            assert ', 1 outputs' in text, text

            for model, inputs, vectors in [
                ('DFF', ('CLK', 'D'), '00L\nC1H\nC0L\n01L\nC1H\n'),
                ('DFFAR', ('CLK', 'D', 'AR'), '001L\nC10H\nC00L\n011L\n'),
                ('DFFAS', ('CLK', 'D', 'AS'), '001H\nC00L\nC10H\n001H\n'),
                ('DFFARS', ('CLK', 'D', 'AR', 'AS'), '0010L\nC100H\nC000L\n0001H\nC000L\n'),
                ('DFFEARS', ('CLK', 'D', 'AR', 'AS', 'CE'),
                 '00100L\nC1000L\nC1001H\nC0000H\nC0001L\n'),
                ('LATCH', ('EN', 'D', 'AR', 'AS'), '0010L\n1100H\n0100H\n1000L\n0001H\n'),
            ]:
                work, mapping = fit(part, one_cell(model, inputs))
                simulate(work, mapping, [n.lower() for n in inputs] + ['q'], vectors)
                if model == 'DFF':
                    simulate(work, mapping, ['clk', 'd', 'q'],
                             'P01\n00H\nC0L\nP00\n01L\nC1H\nP0N\n00H\nP0X\n00X\nC0L\n')
                    package = 'plcc84' if part == 1508 else 'plcc44'
                    (work / 'test.pld').write_text(
                        f'Device f{part}{package}; pin {mapping["clk"]}=clk; '
                        f'pin {mapping["d"]}=d; pin {mapping["q"]}=!q;')
                    (work / 'test.si').write_text(
                        'ORDER:clk,d,q;\nVECTORS:\nP01\n00L\nC0H\nP00\n01H\nC1L\n')
                    run('csim', ['-fl', 'test'], work)
                if model == 'DFFAR':
                    simulate(work, mapping, ['clk', 'd', 'ar', 'q'],
                             'P011\n000H\n001L\n')
                text = run('jedcompare', ['test.jed', 'test.jed'], work, 2)
                assert 'use --sequential' in text, text

            work, mapping = fit(part, one_cell('TRI', ('A', 'EN')))
            simulate(work, mapping, ['a', 'en', 'q'], '00Z\n10Z\n01L\n11H\n')
            run('jedcompare', ['test.jed', 'test.jed'], work)

            # All three global clock inputs, both polarities, and global
            # clear. The fixed package pins prevent product-term clocks
            # from accidentally providing coverage for a global mux.
            clocks = (83, 2, 81) if part == 1508 else (43, 2, 41)
            for clock, falling in itertools.product(clocks, (False, True)):
                pulse = 'K' if falling else 'C'
                initial = '1' if falling else '0'
                compiled(part,
                         f'pin {clock}=clk; pin 5=a; pin 1=rst; pin 4=q;\n'
                         f'q.d=a; q.ck={"!" if falling else ""}clk; q.ar=rst;',
                         'clk,a,rst,q', f'{initial}01L\n{pulse}10H\n{pulse}00L\n')
            compiled(part,
                     f'pin {clocks[0]}=clk; pin 5=a; pin 1=rst; pin 4=q;\n'
                     'q.t=a; q.ck=clk; q.ar=rst;', 'clk,a,rst,q',
                     '001L\nC10H\nC10L\nC00L\nC10H\n')
            compiled(part,
                     'pin 2=clk; pin 6=en; pin 5=a; pin 1=rst; pin 4=q;\n'
                     'q.d=a; q.ck=clk & en; q.ar=rst;', 'clk,en,a,rst,q',
                     '0001L\nC110H\nC100L\nC010L\nC110H\n')
            compiled(part,
                     f'pin {clocks[0]}=clk; pin 1=rst; pin 4=q; pin 5=r;\n'
                     'q.t=!rst; q.ck=clk; q.ar=rst; r.t=!rst; r.ck=q; r.ar=rst;',
                     'clk,rst,q,r', '01LL\nC0HH\nC0LH\nC0HL\nC0LL\n')
            # Keep a buried register separate from the combinational
            # outputs, then preload it through its actual bonded pad.
            mid = 601 if part == 1502 else 616
            work = compiled(part,
                            f'pin {clocks[0]}=clk; pin 1=rst; pin 5=a; pin 6=b; '
                            f'pin 4=q; pin 8=r; pinnode {mid}=mid;\n'
                            'mid.d=a; mid.ck=clk; mid.ar=rst; q=b; r=mid # b;',
                            'clk,a,b,rst,q,r', '0001LL\nC100LH\n0110HH\nC010HH\n')
            text = run('jedcompare', ['test.jed', 'test.jed'], work, 2)
            assert 'use --sequential' in text, text
            mid_pin = int(re.search(r'^MC\d+\s+(\d+)\s+.*\bmid\b',
                                   (work / 'test.fit').read_text(), re.M).group(1))
            if mid_pin:
                mapping = {'clk': clocks[0], 'a': 5, 'b': 6, 'rst': 1, 'q': 4, 'r': 8}
                names = list(mapping)
                if mid_pin not in mapping.values():
                    mapping['mid'] = mid_pin
                    names.append('mid')
                preload = {p: '0' for p in mapping.values()}
                preload.update({clocks[0]: 'P', 4: 'N', 8: 'N', mid_pin: '1'})
                first = ''.join(preload[mapping[n]] for n in names)
                extra = '0' if 'mid' in mapping else ''
                simulate(work, mapping, names, first + '\n0000LH' + extra + '\nC000LL' + extra + '\n')
            for node, equation, function in [
                (601, 'mid=a $ b; q=mid & c;', lambda a, b, c: (a ^ b) & c),
                (301, 'mid=!(a & b); q=mid & c;', lambda a, b, c: not (a & b) and c),
            ]:
                vectors = ''.join(''.join(map(str, bits)) + ('H' if function(*bits) else 'L') + '\n'
                                  for bits in itertools.product((0, 1), repeat=3))
                work = compiled(part, f'pin 5=a; pin 6=b; pin 8=c; pin 4=q; pinnode {node}=mid;\n' +
                                equation, 'a,b,c,q', vectors)
                run('jedcompare', ['test.jed', 'test.jed'], work)

            work, mapping = fit(part, one_cell('BUF', ('A',)), options=['-open_collector', 'ON'])
            simulate(work, mapping, ['a', 'q'], '0L\n1Z\n')
            run('jedcompare', ['test.jed', 'test.jed'], work)

            # An I/O pad feeds back the pin level, independently of the
            # macrocell's data. A disabled driver retains a weak keeper
            # level after an external drive is removed.
            bidir = document([('a', 'INPUT'), ('en', 'INPUT'), ('pad', 'INOUT'), ('q', 'OUTPUT')],
                             {'BIBUF': [('A', 'INPUT'), ('EN', 'INPUT'), ('PAD', 'INOUT'), ('Q', 'OUTPUT')]},
                             [('gate', 'BIBUF')],
                             [[(p.lower(), None), (p, 'gate')] for p in ('A', 'EN', 'PAD', 'Q')])
            work, mapping = fit(part, bidir, pins={'a': 2, 'en': 1, 'pad': 4, 'q': 5},
                                options=['-pin_keep', 'ON'])
            simulate(work, mapping, ['a', 'en', 'pad', 'q'],
                     '000L\n001H\n00ZH\n01LL\n11HH\n10ZH\n')
            text = run('jedcompare', ['test.jed', 'test.jed'], work, 2)
            assert 'use --sequential to compare output feedback' in text, text

            # Change a connected AND literal without changing its pins.
            # Equivalence must depend on decoded fuses, not design names
            # or fitted equations left next to the map.
            original = fuses(maps[part, 'AND2'])
            work = root / str(checks)
            work.mkdir()
            pins = 84 if part == 1508 else 44
            device = f'f{part}plcc{pins}'
            pt_end, uim_end, width, config = {
                1502: (15360, 16720, 5, 16750),
                1504: (30720, 34080, 9, 34134),
                1508: (61440, 73920, 27, 74082),
            }[part]
            row = next(i for i in range(0, pt_end, 96) if 0 < sum(original[i:i + 96]) < 96)
            literal = next(i for i in range(row + 16, row + 96, 2)
                           if original[i] != original[i + 1])
            bits = original.copy()
            bits[literal], bits[literal + 1] = bits[literal + 1], bits[literal]
            write_map(work, bits, device, pins)
            text = run('jedcompare', [str(maps[part, 'AND2']), 'test.jed'], work, 1)
            assert 'Not equivalent:' in text, text

            # A two-source UIM connection is not a valid selector.
            uim_start = pt_end + (960 if part == 1502 else 1920 if part == 1504 else 3840)
            switch = next(i for i in range(uim_start, uim_end, width) if 0 in original[i:i + width])
            bits = original.copy()
            bits[switch:switch + width] = [0] * width
            write_map(work, bits, device, pins)
            text = run('jedcompare', ['test.jed', 'test.jed'], work, 2)
            assert 'invalid or unbonded source' in text, text

            bits = original.copy()
            bits[config + 2] = 0  # PD1 selects the power-down function.
            write_map(work, bits, device, pins)
            text = run('jedcompare', ['test.jed', 'test.jed'], work, 2)
            assert 'use --sequential' in text, text
            pd1, pd2 = (12, 45) if part == 1508 else (11, 25)
            simulate(work, {'a': 2, 'b': 1, 'q': 4, 'pd': pd1},
                     ['a', 'b', 'pd', 'q'], '000L\n110H\n001H\n101H\n100L\n')

            # Either power-down input freezes the output and its enable.
            held, mapping = fit(part, one_cell('TRI', ('A', 'EN')),
                                pins={'a': 2, 'en': 1, 'q': 4})
            bits = fuses(held / 'test.jed')
            bits[config + 2] = bits[config + 3] = 0
            write_map(held, bits, device, pins)
            mapping.update(pd1=pd1, pd2=pd2)
            simulate(held, mapping, ['a', 'en', 'pd1', 'pd2', 'q'],
                     '1100H\n0010H\n0001H\n0000Z\n1110Z\n1101Z\n1100H\n')

            # Clock, data and reset are blocked while asleep. Releasing
            # power-down restores asynchronous reset before normal clocks.
            held, mapping = fit(part, one_cell('DFFAR', ('CLK', 'D', 'AR')),
                                pins={'clk': 2, 'd': 5, 'ar': 1, 'q': 4})
            bits = fuses(held / 'test.jed')
            bits[config + 2] = 0
            write_map(held, bits, device, pins)
            mapping['pd'] = pd1
            simulate(held, mapping, ['clk', 'd', 'ar', 'pd', 'q'],
                     '0010L\nC100H\nC001H\n0011H\n0010L\nC100H\n')
            write_map(work, original, device, pins + 1)
            text = run('jedcompare', ['test.jed', 'test.jed'], work, 2)
            assert 'QP pin count' in text, text

            if part != 1508:
                write_map(work, original, pins=pins)
                text = run('jedcompare', ['test.jed', 'test.jed'], work, 2)
                assert 'package is ambiguous' in text, text
                run('jedcompare', ['-d', device, 'test.jed', 'test.jed'], work)
            write_map(work, original, device, pins)
            text = (work / 'test.jed').read_text().replace('Device ', f'ATF{part}BE\nDevice ')
            (work / 'test.jed').write_text(text)
            text = run('jedcompare', ['test.jed', 'test.jed'], work, 2)
            assert 'BE-series description disagrees with QF fuse count' in text, text
            write_map(work, original, device, pins)
            run('jedcompare', ['-u', str(root / 'missing.dl'), 'test.jed', 'test.jed'], work)
            (work / 'test.si').write_text('ORDER:P2,P1,P4;\nVECTORS:\n00L\n01L\n10L\n11H\n')
            (work / 'test.pld').write_text('Device g16v8; pin 2=wrong;')
            text = run('csim', ['-fl', 'test'], work)
            assert 'pin names' in text and 'not used' in text, text
            (work / 'test.si').write_text('ORDER:P2,P1,P4;\nVECTORS:\nP00\n')
            text = run('csim', ['-fl', 'test'], work, 1)
            assert 'without active storage' in text, text

        # Package pin numbering must follow the selected package, including
        # packages with unbonded internal macrocells.
        for part, packages in [
            (1502, ['PLCC44', 'TQFP44']),
            (1504, ['PLCC44', 'TQFP44', 'PLCC68', 'PLCC84', 'PQFP100', 'TQFP100']),
            (1508, ['PLCC68', 'PLCC84', 'PQFP100', 'TQFP100', 'PQFP160']),
        ]:
            for package in packages:
                work, mapping = fit(part, one_cell('AND2', ('A', 'B')), package)
                simulate(work, mapping, ['a', 'b', 'q'], '00L\n01L\n10L\n11H\n')
                run('jedcompare', ['test.jed', 'test.jed'], work)
        print(f'ATF150x JEDEC simulation ({mode}): {checks} checks passed')


if __name__ == '__main__':
    main()
