#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, LinCUPL team
"""Independent, hand-built BE maps for storage, controls and pin termination."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    bindir = Path(sys.argv[1]).resolve()
    suffix = '.exe' if os.name == 'nt' else ''
    checks = 0
    with tempfile.TemporaryDirectory(prefix='BE fuse vectors ') as temp:
        root = Path(temp)

        def run(tool, args, code=0):
            nonlocal checks
            p = subprocess.run([str(bindir / (tool + suffix)), *args], cwd=root,
                               capture_output=True, text=True, timeout=20)
            text = p.stdout + p.stderr
            assert p.returncode == code, (args, p.returncode, text)
            checks += 1
            return text

        for part, count, pt_end, blocks, start, config, pin_count, clock, output, pd in [
            (1502, 16814, 15360, 2, 15765, 16750, 44, 37, 42, 5),
            (1504, 34198, 30720, 4, 32160, 34134, 44, 37, 42, 5),
            (1508, 74146, 61440, 8, 64725, 74082, 100, 87, 2, 1),
        ]:
            order = f'P{clock},P{output}'
            pd_order = f'P{clock},P{pd},P{output}'
            bits = [1] * count
            bits[:pt_end] = [0] * pt_end
            for block in range(blocks):
                bits[pt_end + block * 480:pt_end + block * 480 + 432] = [0] * 432
            # The output macrocell: registered output, constant-one D, GCLK1,
            # no asynchronous controls, always enabled, high power-up.
            for offset in (5, 6, 7, 9, 11, 13, 14, 15, 17, 23):
                bits[start + offset] = 1
            bits[config] = 0  # armed
            bits[config + 8] = 0  # noninverting GCLK1 from the package clock pin
            bits[config + 33] = 0  # user I/O instead of JTAG

            def write():
                (root / 'test.jed').write_text(
                    f'\x02ATF{part}BE\nDevice f{part}tqfp{pin_count}\n*QF{count}*QP{pin_count}*L0 ' +
                    ''.join(map(str, bits)) + '*\x030000')

            def simulate(order, vectors):
                write()
                (root / 'test.si').write_text(f'ORDER:{order};\nVECTORS:\n{vectors}')
                run('csim', ['-fl', 'test'])

            simulate(order, '0H\nC H\nP 0\n0L\nC H\n')
            run('jedcompare', ['--sequential', 'test.jed', 'test.jed'])
            # Turning an enabled register output into open drain yields Z
            # for one, but can still sink a preloaded zero.
            bits[start + 3] = 1
            simulate(order, '0Z\nP 0\n0L\nC Z\n')
            bits[start + 3] = 0
            # BE PD1 is in the expanded global area, not the AS location.
            bits[config + 34] = 0
            simulate(pd_order, '00H\nP00\n01L\nC1L\n00L\nC0H\n')
            # Input keepers remain active while the logic is asleep.
            # A clock driven high in power-down is held after release.
            bits[start + 9] = 0
            bits[config + 23] = bits[config + 24] = 1
            simulate(pd_order, 'P00\n00L\n11L\nZ1L\nZ0H\n')
            bits[start + 9] = 1
            bits[config + 34] = 1
            if part != 1502:
                # Native T storage is a separate mode from XOR feedback.
                bits[start + 8], bits[start + 9] = 1, 0
                simulate(order, '0H\nC L\nC H\nC L\nP 1\n0H\nC L\n')
                run('jedcompare', ['--sequential', 'test.jed', 'test.jed'])
                bits[start + 9] = 1
                write()
                assert 'invalid storage' in run('jedcompare', ['--sequential', 'test.jed', 'test.jed'], 2)
                bits[start + 8], bits[start + 9] = 0, 1
            # Disconnected dedicated clock: pull-down holds it low, while
            # a pull-up opens a latch and admits the constant-one D value.
            bits[start + 9] = 0
            bits[config + 23], bits[config + 24] = 1, 0
            simulate(order, 'P 0\nZ L\n')
            bits[config + 23], bits[config + 24] = 0, 1
            simulate(order, 'P 0\nZ H\n')
    print(f'PASS: {checks} independent BE map checks')


if __name__ == '__main__':
    main()
