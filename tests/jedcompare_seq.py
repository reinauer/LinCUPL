#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, LinCUPL team
"""Fit stateful designs and verify proofs, limits and replayable differences."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from fit150x_edif import document, one_cell
from defects import fuses


def main():
    bindir = Path(sys.argv[1]).resolve()
    suffix = '.exe' if os.name == 'nt' else ''
    library = bindir.parent / 'share/lincupl/lincupl.dl'
    if not library.is_file():
        print('SKIP: device library is required')
        return 77
    checks = 0
    with tempfile.TemporaryDirectory(prefix='jedcompare sequential ') as temp:
        root = Path(temp)
        empty = root / 'empty'
        empty.mkdir()
        env = dict(os.environ, LIBCUPL=str(library), LINCUPL_SHARE=str(library.parent),
                   LINCUPL_FIT150X_MODE='extended', FITTERDIR=str(empty))
        env.pop('LINCUPL_FITTERS', None)
        serial = 0

        def run(tool, args, cwd=root, code=0):
            nonlocal checks
            result = subprocess.run([str(bindir / (tool + suffix)), *map(str, args)], cwd=cwd,
                                    env=env, capture_output=True, text=True, timeout=60)
            text = result.stdout + result.stderr
            assert result.returncode == code, (tool, args, result.returncode, text)
            checks += 1
            return text

        def directory(label):
            nonlocal serial
            serial += 1
            work = root / f'{serial}-{label}'
            work.mkdir()
            return work

        def compile_map(body, part=1502, negative=False):
            work = directory('compiled')
            pins = 84 if part == 1508 else 44
            (work / 'test.pld').write_text(f'Name test; Device f{part}plcc{pins};\n'
                f'pin 2=clk; pin 5=a; pin 6=b; pin 1=en; pin 4={"!" if negative else ""}q;\n' + body)
            run('cupl', ['-jxm1', '-u', library, 'test.pld'], work)
            return work / 'test.jed'

        def fit(source, pins):
            work = directory('fitted')
            (work / 'test.edf').write_text(source)
            (work / 'board.pin').write_text(''.join(f'{n}:{p}\n' for n, p in pins.items()))
            run('fit1502', ['-i', 'test.edf', '-ifmt', 'EDIF', '-device', 'PLCC44',
                           '-tech', 'ATF1502AS', '-JTAG', 'OFF', '-pinfile', 'board.pin'], work)
            return work / 'test.jed'

        def compare(left, right, *options, code=0):
            text = run('jedcompare', ['--sequential', '-u', library, *options, left, right], code=code)
            if code == 0:
                assert 'Equivalent:' in text and 'reachable state pairs' in text
            elif code == 1:
                assert 'Not equivalent:' in text or 'Different pin interfaces:' in text
            else:
                assert 'inconclusive:' in text and 'Equivalent:' not in text
            return text

        def trace(text):
            rows = re.findall(r'^Step (\d+):(.*)$', text, re.M)
            assert rows and [int(n) for n, _ in rows] == list(range(len(rows))), text
            result = [{int(p): int(v) for p, v in re.findall(r'P(\d+)=([01])', row)} for _, row in rows]
            assert all(set(row) == set(result[0]) for row in result), text
            return result

        def replay(path, sequence, expected):
            work = directory('replay')
            (work / 'test.jed').write_bytes(path.read_bytes())
            pins = sorted(sequence[0])
            order = ','.join(f'P{p}' for p in [*pins, 4])
            levels = {'0': 'L', '1': 'H', 'Z': 'Z'}
            rows = [''.join(str(row[p]) for p in pins) + levels[str(value)]
                    for row, value in zip(sequence, expected)]
            assert len(expected) == len(sequence)
            (work / 'test.si').write_text(f'ORDER:{order};\nVECTORS:\n' + '\n'.join(rows) + '\n')
            run('csim', ['-fl', 'test'], work)

        def delay(sequence, length=1, initial=0, reset=False):
            state, previous = [initial] * length, None
            result = []
            for row in sequence:
                clock = row.get(2, 0)
                if reset and row.get(1, 0):
                    state = [0] * length
                elif previous == 0 and clock == 1:
                    state = [row.get(5, 0), *state[:-1]]
                previous = clock
                result.append(state[-1])
            return result

        def verify_difference(left, right, text, left_oracle, right_oracle, length):
            sequence = trace(text)
            assert len(sequence) == length, text
            a, b = left_oracle(sequence), right_oracle(sequence)
            assert a[:-1] == b[:-1] and a[-1] != b[-1], (sequence, a, b)
            match = re.search(r'Not equivalent: P4 is ([01Z]) versus ([01Z])', text)
            assert match and match.groups() == (str(a[-1]), str(b[-1])), text
            replay(left, sequence, a)
            replay(right, sequence, b)

        # Explicit placement moves a buried register behind a combinational
        # output. The independent b input prevents register/output merging.
        for part in (1502, 1504, 1508):
            direct = compile_map('q.d=a; q.ck=clk;', part)
            left = compile_map('pinnode 605=mid; mid.d=a; mid.ck=clk; q=mid # b;', part)
            relocated = compile_map('pinnode 609=mid; mid.d=a; mid.ck=clk; q=mid # b;', part)
            assert fuses(left) != fuses(relocated)
            compare(left, relocated)
            assert 'use --sequential' in run(
                'jedcompare', [direct, direct], code=2)

        direct = compile_map('q.d=a; q.ck=clk;')
        negative_initial = compile_map('q.d=!a; q.ck=clk;', negative=True)
        # Both produce a after each edge, but the physical power-up levels
        # differ. The shortest counterexample must include startup itself.
        text = compare(direct, negative_initial, code=1)
        verify_difference(direct, negative_initial, text, delay,
                          lambda s: delay(s, initial=1), 1)

        second = compile_map('pinnode 605=m; m.d=a; m.ck=clk; q.d=m; q.ck=clk;')
        third = compile_map('pinnode 605=m; pinnode 609=n; m.d=a; n.d=m; q.d=n; '
                            'm.ck=clk; n.ck=clk; q.ck=clk;')
        text = compare(second, third, code=1)
        verify_difference(second, third, text, lambda s: delay(s, 2), lambda s: delay(s, 3), 4)

        reset = compile_map('q.d=a; q.ck=clk; q.ar=en;')
        text = compare(reset, direct, code=1)
        verify_difference(reset, direct, text, lambda s: delay(s, reset=True), delay, 2)
        compare(reset, compile_map('pinnode 605=m; m.d=a; m.ck=clk; m.ar=en; q=m;'))

        sleeping = root / 'power-down.jed'
        bits = fuses(direct)
        bits[16750 + 2] = 0
        sleeping.write_text('\x02Device f1502plcc44\n*QF16808*QP44*L0 ' +
                            ''.join(map(str, bits)) + '*\x030000')
        compare(sleeping, sleeping)

        def sleeping_values(sequence):
            value, previous, result = 0, None, []
            for row in sequence:
                clock = row.get(2, 0)
                if not row.get(11, 0):
                    if previous == 0 and clock == 1:
                        value = row.get(5, 0)
                    previous = clock
                result.append(value)
            return result

        text = compare(sleeping, direct, code=1)
        sequence = trace(text)
        verify_difference(sleeping, direct, text, sleeping_values, delay, len(sequence))

        enabled = compile_map('q.d=a; q.ck=clk; q.oe=en;')
        compare(enabled, compile_map('pinnode 605=m; m.d=a; m.ck=clk; q=m; q.oe=en;'))
        text = compare(enabled, direct, code=1)
        verify_difference(enabled, direct, text,
            lambda s: [v if row.get(1, 0) else 'Z' for row, v in zip(s, delay(s))], delay, 1)

        zero = compile_map("q='b'0;")
        fixed_register = compile_map("q.d='b'0; q.ck=clk;")
        assert 'use --sequential' in run(
            'jedcompare', [fixed_register, fixed_register], code=2)
        compare(zero, fixed_register)
        compare(zero, zero, '--max-inputs', '0')
        compare(zero, zero, '--max-inputs', '20', '--max-states', '1000000',
                '--max-transitions', '1000000000')
        compare(compile_map('q=a;'), compile_map('q=!(!a);'))

        def counter(first, toggle):
            source = ''.join(f'pinnode {first+i}=r{i};\n' for i in range(7))
            for i in range(7):
                carry = ' & '.join(f'r{j}' for j in range(i)) or "'b'1"
                expression = carry if toggle else f'r{i} $ ({carry})'
                source += f'r{i}.{"t" if toggle else "d"}={expression}; r{i}.ck=clk;\n'
            return compile_map(source + 'q=r6 # b;')

        # A 7-bit counter forces hundreds of reachable pairs and hash lookups. T and D implementations use different placements.
        counted, counted_d = counter(605, True), counter(617, False)
        text = compare(counted, counted_d)
        match = re.search(r'Equivalent: (\d+) reachable state pairs, (\d+) transitions', text)
        states, transitions = map(int, match.groups())
        assert states > 128, text
        compare(counted, counted_d, '--max-states', str(states), '--max-transitions', str(transitions))
        assert 'state limit' in compare(counted, counted_d, '--max-states', str(states - 1), code=2)
        assert 'transition limit' in compare(counted, counted_d, '--max-transitions', str(transitions - 1), code=2)
        assert 'state limit' in compare(direct, direct, '--max-states', '1', code=2)
        assert 'transition limit' in compare(direct, direct, '--max-transitions', '1', code=2)

        def count_values(sequence):
            value, previous, result = 0, None, []
            for row in sequence:
                clock = row.get(2, 0)
                if previous == 0 and clock == 1:
                    value = (value + 1) % 128
                previous = clock
                result.append(((value >> 6) & 1) | row.get(6, 0))
            return result

        passthrough = compile_map('q=b;')
        text = compare(counted, passthrough, code=1)
        verify_difference(counted, passthrough, text, count_values,
                          lambda s: [row.get(6, 0) for row in s], 128)

        other_input = compile_map('q.d=b; q.ck=clk;')
        assert 'combined inputs' in compare(direct, other_input, '--max-inputs', '2', code=2)
        compare(direct, other_input, code=1)
        compare(direct, compile_map('q.d=a; q.ck=clk;', 1508), code=1)

        # Simultaneous asynchronous set/reset has no binary deterministic
        # result. Even comparison with itself must remain inconclusive.
        ambiguous = fit(one_cell('DFFARS', ('CLK', 'D', 'AR', 'AS')),
                        {'clk': 2, 'd': 5, 'ar': 1, 'as': 6, 'q': 4})
        assert 'indeterminate' in compare(ambiguous, ambiguous, code=2)
        bidirectional = document(
            [('a', 'INPUT'), ('en', 'INPUT'), ('pad', 'INOUT'), ('q', 'OUTPUT')],
            {'BIBUF': [('A', 'INPUT'), ('EN', 'INPUT'), ('PAD', 'INOUT'), ('Q', 'OUTPUT')]},
            [('gate', 'BIBUF')], [[(p.lower(), None), (p, 'gate')] for p in ('A', 'EN', 'PAD', 'Q')])
        feedback = fit(bidirectional, {'a': 2, 'en': 1, 'pad': 4, 'q': 5})
        assert 'indeterminate' in compare(feedback, feedback, code=2)
        broken = root / 'broken.jed'
        broken.write_text('not a JEDEC map')
        compare(direct, broken, code=2)
        compare(root / 'absent.jed', direct, code=2)
        unsupported = root / 'unsupported.jed'
        unsupported.write_text('\x02QF100*QP24*F0*\x030000')
        compare(unsupported, unsupported, code=2)

        def storage(path):
            text = run('jedcompare', ['-u', library, '--list-state', path])
            return dict(re.findall(r'^(MC[0-9]+|R[0-9]+)=([01X])$', text, re.M))

        # State names include buried registers, independent of bonded pins.
        buried = compile_map('pinnode 605=m; m.d=a; m.ck=clk; q=m # b;')
        moved = compile_map('pinnode 609=m; m.d=a; m.ck=clk; q=m # b;')
        inverted = compile_map('pinnode 609=m; m.d=!a; m.ck=clk; q=!m # b;')
        left_name, = storage(buried)
        right_name, = storage(moved)
        inverse_name, = storage(inverted)
        assert left_name != right_name, (left_name, right_name)
        paired = ['--preload-left', left_name + '=s', '--preload-right', right_name + '=s']
        assert '2 initial assignments' in compare(buried, moved, *paired)
        assert '2 initial assignments' in compare(
            buried, inverted, '--preload-left', left_name + '=s',
            '--preload-right', inverse_name + '=!s')
        text = compare(buried, moved, '--preload-left', left_name + '=s',
                       '--preload-right', right_name + '=!s', code=1)
        assert 'Preload seeds: s=0' in text and 'Initial left: ' + left_name + '=0' in text
        assert 'Initial right: ' + right_name + '=1' in text
        assert len(trace(text)) == 1
        # A concrete preload reproduces the same shortest counterexample.
        concrete = compare(buried, moved, '--preload-left', left_name + '=0',
                           '--preload-right', right_name + '=1', code=1)
        assert trace(text) == trace(concrete)
        assert 'preload assignments' in compare(buried, moved, *paired,
                                                '--max-states', '1', code=2)
        assert 'preload assignments' in compare(buried, moved, *paired,
                                                '--max-transitions', '1', code=2)
        for invalid in ('', left_name + '=X?', left_name + '=2',
                        left_name + '=0,', left_name + '=0,' + left_name + '=1',
                        left_name + '=!0', left_name + '==s'):
            assert 'preload' in compare(buried, moved, '--preload-left', invalid, code=2)
        # Unknown storage returns its own useful diagnostic.
        assert '--list-state' in compare(buried, moved, '--preload-left', 'MC999=0', code=2)
        assert 'Usage:' in run('jedcompare', ['--preload-left', left_name + '=0', buried, moved], code=2)

        # Physical output feedback is deterministic while its driver is on.
        physical = compile_map('pin 7=z; q.d=a; q.ck=clk; z=q.io;')
        compare(physical, physical)
        kept_source = compile_map('pin 7=z; q.d=a; q.ck=clk; q.oe=!q; z=q.io;')
        kept = root / 'kept-feedback.jed'
        bits = fuses(kept_source)
        bits[16750 + 1] = 0  # global pin keeper enable
        kept.write_text('\x02Device f1502plcc44\n*QF16808*QP44*L0 ' +
                        ''.join(map(str, bits)) + '*\x030000')
        compare(kept, kept)
        assert 'indeterminate' in compare(kept_source, kept_source, code=2)
        wrong_physical = compile_map('pin 7=z; q.d=a; q.ck=clk; z=!q.io;')
        compare(physical, wrong_physical, code=1)

        # A binary-looking unrelated output must not hide an oscillator.
        loop = directory('unstable')
        (loop / 'test.pld').write_text('Name test; Device g22v10; '
            "pin 23=q; pin 22=y; q=!q; y='b'0;")
        run('cupl', ['-jxm1', '-u', library, 'test.pld'], loop)
        assert 'unstable' in compare(loop / 'test.jed', loop / 'test.jed', code=2)

        copied = root / 'a stateful map.bin'
        copied.write_bytes(direct.read_bytes())
        copied.with_suffix('.pld').write_text('invalid unrelated source')
        copied.with_suffix('.si').write_text('invalid unrelated vectors')
        snapshot = {p: p.read_bytes() for p in root.iterdir() if p.is_file()}
        compare(direct, copied)
        assert snapshot == {p: p.read_bytes() for p in root.iterdir() if p.is_file()}
        for option, values in [('--max-states', ['0', '-1', '+1', '1x', '1000001']),
                               ('--max-transitions', ['0', '-1', '+1', '1x', '1000000001']),
                               ('--max-inputs', ['21', '-1'])]:
            for value in [*values, '18446744073709551616', '99999999999999999999999999999999999999']:
                text = run('jedcompare', ['--sequential', option, value, direct, direct], code=2)
                assert 'Usage:' in text and 'Equivalent:' not in text
        for option in ('--max-states', '--max-transitions'):
            assert 'Usage:' in run('jedcompare', [option, '10', direct, direct], code=2)
        assert '--sequential' in run('jedcompare', ['--help'])
    print(f'PASS: {checks} sequential fitting, equivalence and counterexample replay checks')
    return 0


if __name__ == '__main__':
    sys.exit(main())
