#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Exercise V8 power-down independently of the equation simulator."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from devv8 import ROOT, descriptions
from defects import fuses
import devcompile


def jed(bits, pins):
    return '\x02\n*QF%d*QP%d*F0*L0 %s*\n\x03' % (len(bits), pins, ''.join(map(str, bits)))


def main():
    bindir = Path(sys.argv[1]).resolve()
    env = dict(os.environ, LINCUPL_SHARE=str(bindir.parent / 'share/lincupl'))
    env.pop('LIBCUPL', None)
    env.pop('FITTERDIR', None)
    models = {n: h for n, h in descriptions().items() if h['power_down']}
    assert len(models) == 12
    checks = 0
    with tempfile.TemporaryDirectory(prefix='csim-v8-power-') as temp:
        root = Path(temp)
        catalog = root / 'catalog.dl'
        catalog.write_bytes(devcompile.compile_directory(ROOT / 'devices', models, hardware=False))

        def run(tool, args, cwd, code=0):
            nonlocal checks
            exe = bindir / (tool + ('.exe' if os.name == 'nt' else ''))
            p = subprocess.run([str(exe), *map(str, args)], cwd=cwd, env=env,
                               capture_output=True, text=True, timeout=20)
            checks += 1
            assert p.returncode == code, (cwd.name, tool, args, p.stdout, p.stderr)
            return p.stdout + p.stderr

        for name, h in models.items():
            p = h['package'].number
            modes = ('registered', 'complex', 'simple') if h['mode'] == 'automatic' else (h['mode'],)
            for mode in modes:
                for inv in (0, 1):
                    work = root / f'{name}-{mode}-{inv}'
                    work.mkdir()
                    q = '!q' if inv else 'q'
                    output = p(h['cells'][0]['pin'])
                    # Exercise the restored catalog inputs as actual logic:
                    # DIP pin 3 in registered mode, pin 23 in simple mode.
                    source = 3 if mode == 'registered' else 23 if mode == 'simple' else 2
                    input_pin = p(source if h['architecture'] == 'gal20v8' else 2)
                    body = f'Name test; Device {name}; pin {input_pin}=a; pin {output}={q}; '
                    if mode == 'registered':
                        body += f'pin {p(1)}=clk; pin {p(h["enable_pin"])}=disable; q.d=a;'
                        # A sleeping clock pulse does not change Q or clock
                        # history. A high level at wake-up can make an edge.
                        samples = [('C', 0, 0, 0), ('C', 1, 0, 0), ('C', 0, 1, 1),
                                   ('1', 0, 1, 0), ('0', 0, 1, 1), ('0', 0, 0, 0),
                                   ('C', 0, 0, 0), ('1', 1, 1, 1), ('1', 1, 0, 0),
                                   ('0', 0, 0, 0), ('C', 0, 0, 1), ('C', 1, 1, 0),
                                   ('0', 1, 0, 0), ('1', 1, 0, 0), ('0', 0, 1, 0),
                                   ('1', 0, 0, 0), ('0', 0, 0, 0), ('1', 0, 0, 0)]
                        state, previous, held = 0, 0, 'X'
                        rows = []
                        for ck, a, pd, disable in samples:
                            if not pd:
                                if ck == 'C' or (ck == '1' and previous == 0):
                                    state = a
                                previous = int(ck == '1')
                                held = 'Z' if disable else 'LH'[state ^ inv]
                            rows.append(f'{ck}{a}{pd}{disable}{held}')
                        order = f'clk,a,P{p(4)},disable,{q}'
                        numbered = f'P{p(1)},P{input_pin},P{p(4)},P{p(h["enable_pin"])},P{output}'
                    else:
                        body += 'q=a;'
                        samples = [(0, 0, 1), (1, 0, 1), (0, 1, 1), (0, 1, 0),
                                   (1, 1, 0), (0, 0, 0), (1, 1, 1), (1, 0, 1), (0, 0, 1)]
                        if mode == 'complex':
                            body += f'pin {p(5)}=en; q.oe=en;'
                        rows, held = [], 'X'
                        for a, pd, en in samples:
                            if not pd:
                                held = 'LH'[a ^ inv] if en or mode == 'simple' else 'Z'
                            rows.append(f'{a}{pd}' + (str(en) if mode == 'complex' else '') + held)
                        order = f'a,P{p(4)},' + ('en,' if mode == 'complex' else '') + q
                        numbered = f'P{input_pin},P{p(4)},' + (f'P{p(5)},' if mode == 'complex' else '') + f'P{output}'
                    (work / 'test.pld').write_text(body)
                    run('cupl', ['-jaxm1', 'test.pld'], work)
                    bits = fuses(work / 'test.jed')
                    assert len(bits) == h['fuses'] and bits[-1] == 1

                    def vectors(order_text, lines):
                        (work / 'test.si').write_text(f'ORDER:{order_text};\nVECTORS:\n' + '\n'.join(lines) + '\n')

                    vectors(order, rows)
                    for options in ([], ['-u', catalog]):
                        run('csim', ['-fl', *options, 'test'], work)
                    wrong = rows.copy()
                    wrong[2] = wrong[2][:-1] + ('L' if wrong[2][-1] == 'H' else 'H')
                    vectors(order, wrong)
                    assert 'user expected' in run('csim', ['-fl', 'test'], work, 1)
                    # Exercise control polarity using physical pin aliases.
                    pd_column = 2 if mode == 'registered' else 1
                    inverted = [row[:pd_column] + str(1 - int(row[pd_column])) + row[pd_column + 1:] for row in rows]
                    vectors(order.replace(f'P{p(4)}', f'!P{p(4)}'), inverted)
                    run('csim', ['-fl', 'test'], work)

                    # QF/QP inference needs neither source names nor a device
                    # annotation. No ABS file can supply the held outputs.
                    (work / 'test.pld').unlink()
                    (work / 'test.abs').unlink()
                    (work / 'test.jed').write_text(jed(bits, h['package'].pins))
                    vectors(numbered, rows)
                    run('csim', ['-fl', 'test'], work)
                    # Power-down adds storage even to a simple Boolean map.
                    out = run('jedcompare', ['test.jed', 'test.jed'], work, 2)
                    assert 'stateful design' in out and '--sequential' in out
                    out = run('jedcompare', ['--sequential', 'test.jed', 'test.jed'], work, 2)
                    assert 'indeterminate' in out, out
                    enable_pin = p(h['enable_pin']) if mode == 'registered' else p(5) if mode == 'complex' else 0
                    run('v8_power_seq_test', ['test.jed', bindir.parent / 'share/lincupl/lincupl.dl',
                        input_pin, p(4), enable_pin, p(1) if mode == 'registered' else 0,
                        output, inv, int(mode == 'complex')], work)

                    # PD high before the first active vector has no sampled
                    # output to retain. Never invent a known output value.
                    first = rows[0]
                    vectors(numbered, [first[:pd_column] + '1' + first[pd_column + 1:-1] + 'X'])
                    run('csim', ['-fl', 'test'], work)
                    # X on PD makes retained state unknown; an awake vector
                    # recovers combinational logic or clocks a new D value.
                    unknown = rows[1][:pd_column] + 'X' + rows[1][pd_column + 1:-1] + 'X'
                    vectors(numbered, [rows[0], rows[1], unknown, rows[0]])
                    run('csim', ['-fl', 'test'], work)

                    if inv or mode != 'simple':
                        continue
                    # An active product cannot reference a reserved PD
                    # literal. Disabled rows may contain arbitrary fuses.
                    width = 40 if h['architecture'] == 'gal20v8' else 32
                    bad = bits.copy()
                    bad[8] = 0
                    (work / 'test.jed').write_text(jed(bad, h['package'].pins))
                    vectors(numbered, rows)
                    assert 'power-down pin is also used' in run('csim', ['-fl', 'test'], work, 1)
                    bad[width * 64 + 80] = 0
                    (work / 'test.jed').write_text(jed(bad, h['package'].pins))
                    # The row is ignored; use a wildcard for its changed output.
                    vectors(numbered, [rows[0][:-1] + '*'])
                    run('csim', ['-fl', 'test'], work)

        # A cleared extra option fuse behaves like a standard map: pin 4
        # remains a logic input and must not freeze the result when high.
        for name, npins, inp, out in [('g16v8s', 20, 4, 19), ('g20v8s', 24, 4, 22),
                                     ('g20v8slcc', 28, 5, 26)]:
            work = root / (name + '-disabled')
            work.mkdir()
            (work / 'test.pld').write_text(f'Name test; Device {name}; pin {inp}=a; pin {out}=q; q=a;')
            run('cupl', ['-jaxm1', 'test.pld'], work)
            bits = fuses(work / 'test.jed') + [0]
            (work / 'test.jed').write_text(jed(bits, npins))
            (work / 'test.pld').unlink()
            (work / 'test.abs').unlink()
            (work / 'test.si').write_text(f'ORDER:P{inp},P{out};\nVECTORS:\n0L\n1H\n0L\n')
            run('csim', ['-fl', 'test'], work)
            assert 'Equivalent:' in run('jedcompare', ['test.jed', 'test.jed'], work)
    print(f'PASS: {checks} V8 power-down checks across 12 CP interfaces and three package maps')


if __name__ == '__main__':
    main()
