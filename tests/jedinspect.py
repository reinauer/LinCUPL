#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, LinCUPL team
"""Exercise the inspector without a device library or compiler."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    tool = str(Path(sys.argv[1]).resolve())
    checks = 0
    with tempfile.TemporaryDirectory(prefix='jedinspect-') as tmp:
        root = Path(tmp)

        def run(*args, code=0):
            nonlocal checks
            p = subprocess.run([tool, *map(str, args)], capture_output=True,
                               text=True, timeout=10)
            assert p.returncode == code, (args, p.returncode, p.stdout, p.stderr)
            checks += 1
            return p.stdout

        # 0x55 + 0x01 = 0x56, including the partial final fuse byte.
        a = root / 'map with spaces.jed'
        a.write_bytes(b'prefix\x02Quoted "design"\\\r\n\xff*QP20*QF9*F0*'
                      b'L0 101010101*C0056*\x030000')
        before = a.read_bytes()
        report = json.loads(run('--json', '--fuses', '1:8', a))
        assert report['fuses'] == 9 and report['pins'] == 20
        assert report['ones'] == 5 and report['zeros'] == 4
        assert report['checksum'] == '0056' and report['checksum_present']
        assert report['stx'] and report['etx']
        assert not report['transmission_checksum_checked']
        assert report['range'] == {'start': 1, 'count': 8, 'bits': '01010101'}
        assert report['design'] == 'Quoted "design"\\\n\xff'
        assert 'L000001 01010101' in run('--fuses', '1:8', a)

        b = root / 'equivalent.jed'
        b.write_text('different text*qf0*f1*l1 0*l3 0*l5 0*l7 01*c56*')
        assert json.loads(run('--json', '--diff', b, a))['comparison']['count'] == 0
        changed = root / 'changed.jed'
        changed.write_bytes(b'\x02*QF9*F0*\x030000')
        report = json.loads(run('--json', '--diff', changed, '--limit', '2', a, code=1))
        comparison = report['comparison']
        assert comparison['count'] == 5 and comparison['truncated']
        assert comparison['differences'] == [
            {'fuse': 0, 'left': 1, 'right': 0},
            {'fuse': 2, 'left': 1, 'right': 0}]
        report = json.loads(run('--json', '--diff', changed, '--limit', '0', a, code=1))
        assert report['comparison']['differences'] == []
        assert 'listing limited' in run('--diff', changed, '--limit', '0', a, code=1)
        short = root / 'short.jed'
        short.write_bytes(b'\x02*QF8*L0 10101010*\x03')
        report = json.loads(run('--json', '--diff', short, a, code=1))
        assert report['comparison']['differences'] == [{'fuse': 8, 'left': 1, 'right': None}]
        assert not json.loads(run('--json', short))['checksum_present']

        for text in [b'\x02*QF9*F0*C0001*\x03', b'\x02*QF9*F0*',
                     b'\x02*QF9*L0 0*\x03', b'\x02*QF1*L0 01*\x03',
                     b'\x02*QF4000001*F0*\x03', b'\x02*QF9*L0 0x*\x03']:
            invalid = root / 'invalid.jed'
            invalid.write_bytes(text)
            assert not run('--json', invalid, code=2)
        for bounds in ['9:1', '8:2', '0:0', '-1:2', '1:-2', '1:2x',
                       '9999999999999999999999:1', '1', ':1']:
            run('--fuses', bounds, a, code=2)
        run('--limit', '-1', a, code=2)
        run('--limit', '2x', a, code=2)
        run('--unknown', a, code=2)
        run(code=2)
        run(a, b, code=2)
        run(root / 'missing.jed', code=2)
        run('--diff', root / 'missing.jed', a, code=2)
        assert 'Usage:' in run('--help')
        assert json.loads(run('--json', '--', a))['fuses'] == 9
        assert a.read_bytes() == before
    print(f'{checks} inspector cases passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
