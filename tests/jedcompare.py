#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, LinCUPL team
"""Compile independent Boolean examples and check pin equivalence."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def main():
    bindir = Path(sys.argv[1]).resolve()
    library = None
    for directory in [bindir.parent / 'share/lincupl',
                      Path(os.environ.get('LINCUPL_SHARE', '/nonexistent'))]:
        for name in ['lincupl.dl', 'Atmel.dl', 'cupl.dl']:
            if (directory / name).is_file():
                library = directory / name
                break
        if library:
            break
    if not library:
        print('no device library: skipped')
        return 77
    checks = 0
    serial = 0
    with tempfile.TemporaryDirectory(prefix='jedcompare-') as tmp:
        root = Path(tmp)

        def run(tool, *args, code=0):
            nonlocal checks
            p = subprocess.run([str(bindir / tool), *map(str, args)],
                               capture_output=True, text=True, timeout=30)
            assert p.returncode == code, (tool, args, p.returncode, p.stdout, p.stderr)
            checks += 1
            return p.stdout + p.stderr

        def compile_map(equation, device='g22v10', mode=0, output=23, extra=''):
            nonlocal serial
            serial += 1
            pld = root / f'design{serial}.pld'
            pld.write_text(f'Name design{serial};\nDevice {device};\n'
                           'Pin 2 = a;\nPin 3 = b;\nPin 4 = en;\n'
                           f'Pin {output} = y;\n{equation}\n{extra}\n')
            run('cupl', f'-jm{mode}', '-u', library, pld)
            return pld.with_suffix('.jed')

        def compare(a, b, *options, code=0):
            out = run('jedcompare', '-u', library, *options, a, b, code=code)
            if code == 0:
                assert 'Equivalent:' in out
            elif code == 2:
                assert 'Equivalent:' not in out
            return out

        def inspect(path):
            import json
            return json.loads(run('jedinspect', '--json', '--fuses', f'0:{fuse_count(path)}', path))

        def fuse_count(path):
            return int(re.search(rb'QF(\d+)', path.read_bytes()).group(1))

        def write_fuses(path, bits, pins):
            # No C field: checksums are not the subject of these mutations.
            path.write_text(f'\x02different heading*QF{len(bits)}*QP{pins}*'
                            f'L0 {bits}*\x030000', encoding='ascii')
            return path

        for device, output in [('g16v8', 19), ('g16v8s', 19), ('g20v8', 22),
                               ('g20v8as', 22), ('g22v10', 23),
                               ('g20v8lcc', 26), ('g22v10lcc', 27)]:
            a = compile_map('y = a & b;', device, output=output)
            b = compile_map('y = !( !a # !b );', device, mode=4, output=output)
            compare(a, b)
            c = compile_map('y = a # b;', device, output=output)
            out = compare(a, c, code=1)
            assert 'Not equivalent:' in out and 'Inputs:' in out
            assignment = {int(p): int(v) for p, v in re.findall(r'P(\d+)=(\d)', out)}
            assert (assignment[2] & assignment[3]) != (assignment[2] | assignment[3])

        # Different product-term counts, literal supports and fuse order;
        # matching bytes or checksums cannot establish these equalities.
        base = compile_map('y = a;')
        redundant = compile_map('y = (a & b) # (a & !b);')
        assert base.read_bytes() != redundant.read_bytes()
        assert '4 combinations' in compare(base, redundant)
        compare(base, redundant, '--max-inputs', '1', code=2)
        compare(base, compile_map('y = !a;'), code=1)
        compare(compile_map("y = 'b'0;"), compile_map("y = 'b'1;"), code=1)
        compare(compile_map("y = 'b'0;"), compile_map("y = 'b'0;"), '--max-inputs', '0')

        # All 16 functions of two inputs, with an independent truth-table
        # oracle. Every distinct pair must produce a valid counterexample,
        # including functions whose input supports differ or are empty.
        functions = []
        for truth in range(16):
            terms = []
            for row in range(4):
                if truth & (1 << row):
                    terms.append(f"({'a' if row & 1 else '!a'} & {'b' if row & 2 else '!b'})")
            expr = ' # '.join(terms) if terms else "'b'0"
            path = compile_map(f'y = {expr};')
            compare(path, compile_map(f'y = {expr};', mode=4))
            functions.append(path)
        for left in range(16):
            for right in range(left + 1, 16):
                out = compare(functions[left], functions[right], code=1)
                assignment = {int(p): int(v) for p, v in re.findall(r'P(\d+)=(\d)', out)}
                row = assignment.get(2, 0) | (assignment.get(3, 0) << 1)
                actual = re.search(r'P23 is ([01]) versus ([01])', out)
                assert actual, out
                expected = ((left >> row) & 1, (right >> row) & 1)
                assert expected[0] != expected[1]
                assert tuple(map(int, actual.groups())) == expected, out

        # Signature bits do not affect logic. The inspector still reports
        # the physical change; a file without the signature also compares.
        report = inspect(base)
        bits = report['range']['bits']
        flipped = bits[:-1] + ('1' if bits[-1] == '0' else '0')
        signature = write_fuses(root / 'signature.jed', flipped, 24)
        run('jedinspect', '--diff', signature, base, code=1)
        compare(base, signature)
        compare(base, write_fuses(root / 'short.jed', bits[:5828], 24))

        enabled = compile_map('y = a;', extra='y.oe = en;')
        always = compile_map('y = a;')
        out = compare(enabled, always, code=1)
        assert 'Z versus' in out or 'versus Z' in out
        compare(enabled, compile_map('y = a;', extra='y.oe = !en;'), code=1)
        compare(enabled, compile_map('y = a;', mode=4, extra='y.oe = en;'))

        # The union of each map's support must be bounded too.
        out = compare(base, compile_map('y = b;'), '--max-inputs', '1', code=2)
        assert 'combined inputs' in out
        compare(base, compile_map('y = a;', device='g16v8', output=19), code=1)
        compare(base, compile_map('y = a;', output=22), code=1)

        register = compile_map('y.d = a;', extra='Pin 1 = clk;')
        assert 'registered' in compare(register, register, code=2)
        feedback = compile_map('y = a # y;')
        assert 'feedback' in compare(feedback, feedback, code=2)
        wrong_pins = write_fuses(root / 'wrong-pins.jed', bits, 28)
        compare(base, wrong_pins, '-d', 'g22v10', code=2)
        wrong_mode = compile_map('y = a;', device='g16v8s', output=19)
        compare(wrong_mode, wrong_mode, '-d', 'g16v8ma', code=2)
        unsupported = write_fuses(root / 'unsupported.jed', '0' * 100, 24)
        compare(unsupported, unsupported, code=2)
        broken = root / 'broken.jed'
        broken.write_text('incomplete')
        compare(base, broken, code=2)
        compare(root / 'missing.jed', base, code=2)

        # An arbitrary extension and misleading neighboring files must not
        # change the JEDEC input, device choice or physical polarity.
        copied = root / 'a map with spaces.bin'
        copied.write_bytes(base.read_bytes())
        copied.with_suffix('.pld').write_text('Device g16v8; Pin 2 = !a;')
        copied.with_suffix('.si').write_text('invalid vector file')
        snapshot = {p.name: p.read_bytes() for p in root.iterdir()}
        compare(base, copied)
        assert snapshot == {p.name: p.read_bytes() for p in root.iterdir()}
        compare(base, base, '-d', 'G22V10')
        for args in [[], ['--unknown'], ['--max-inputs', '21', base, base],
                     ['--max-inputs', '-1', base, base], ['--max-inputs', '1x', base, base]]:
            run('jedcompare', *args, code=2)
        run('jedcompare', '-u', root / 'missing.dl', base, base, code=2)
        assert 'Usage:' in run('jedcompare', '--help')
    print(f'{checks} compiler, inspector and equivalence checks passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
