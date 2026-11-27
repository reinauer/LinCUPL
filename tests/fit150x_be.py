#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Fit BE maps and check their coordinates and behavior independently."""
import os
import re
from pathlib import Path
import subprocess
import sys
import tempfile

from defects import fuses
from fit150x_edif import document, one_cell


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
            elif kind == 'B':
                source = document([('a', 'INPUT'), ('en', 'INPUT'), ('pad', 'INOUT'), ('q', 'OUTPUT')],
                    {'BIBUF': [('A', 'INPUT'), ('EN', 'INPUT'), ('PAD', 'INOUT'), ('Q', 'OUTPUT')]},
                    [('gate', 'BIBUF')],
                    [[(p.lower(), None), (p, 'gate')] for p in ('A', 'EN', 'PAD', 'Q')])
                names, lib = ('a', 'en', 'pad', 'q'), ''
            else:
                source = one_cell('CUSTOM', ('CK', 'RST', 'DATA'), ('OUT',))
                names = ('ck', 'rst', 'data', 'out')
                gate, clock = {'D': ('DG', 'RECK'), 'T': ('TG', 'RECK'), 'L': ('LG', 'L1')}[kind[0]]
                outputs = '* OUT *' if kind.endswith('N') else 'OUT * *'
                lib = (f'PGATE 0 0 {gate} CUSTOM 1 CK {clock} RST * * * DATA {outputs}\n'
                       'RELATED_OUTPUT_PIN OUT 10\nPIN * UNKNOWN 1 1 1 0 1 0 0 0\n')
            (work / 'test.edf').write_text(source)
            (work / 'cells.lib').write_text(lib)
            (work / 'test.pin').write_text(''.join(f'{name} : {pin}\n' for name, pin in zip(names, pins) if pin is not None))
            output = run(f'fit{part}', ['-i', 'test.edf', '-ifmt', 'EDIF',
                *([] if kind == 'B' else ['-lib', 'cells.lib']), '-device', package, '-tech', f'ATF{part}{tech}', '-JTAG', 'OFF',
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
            for option in [('-mc_power', 'ON')]:
                assert 'BE ' in fit(part, 'TQFP44', (37, 38, pin), 'AND', 'BE', option, False)

        # Bank voltages describe physical supplies, not programmable selectors.
        # POR and power_reset are accepted BE compatibility options; neither
        # selects a register initial value in the fuse map.
        for part, package, pins, cfg in [
                (1502, 'TQFP44', (37, 38, 42), 16750),
                (1504, 'TQFP44', (37, 38, 6), 34134),
                (1508, 'TQFP100', (87, 88, 2), 74082)]:
            _, base = fit(part, package, pins, 'AND', 'BE')
            for bank in 'AB':
                for voltage in ('1.8', '2.5', '3.3') + (() if part == 1502 else ('1.5',)):
                    _, bits = fit(part, package, pins, 'AND', 'BE',
                                  [f'-voltage_level_{bank}', voltage])
                    assert bits == base, (part, bank, voltage)
            for value in ('garbage', '5.0') + (('1.5',) if part == 1502 else ()):
                assert 'Invalid voltage_level_A' in fit(part, package, pins, 'AND', 'BE',
                    ['-voltage_level_A', value], False)
            for kind in ('D', 'DN', 'T', 'L'):
                assigned = (pins[0], pins[1], pins[0] + 3, pins[2])
                _, normal = fit(part, package, assigned, kind, 'BE')
                for options in (['-POR', 'one'], ['-power_reset', 'ON'],
                                ['-POR', 'one', '-power_reset', 'ON']):
                    _, bits = fit(part, package, assigned, kind, 'BE', options)
                    assert bits == normal, (part, kind, options)
            _, grounded = fit(part, package, pins, 'AND', 'BE', ['-unused_To_Ground', 'ON'])
            # The unused dedicated clock and clear pads use pull-down code 1.
            assert grounded[cfg + 25:cfg + 27] == [1, 0]
            assert grounded[cfg + 29:cfg + 31] == [1, 0]

        # Explicit I/O pull-down uses the same termination code as ground
        # on unused pads, and changes no routing or logic controls.
        for part, package, pins, input_cfg, output_cfg in [
                (1502, 'TQFP44', (1, 15, 42), 15684, 15765),
                (1504, 'TQFP44', (8, 23, 6), 30774, 32565),
                (1508, 'TQFP100', (13, 58, 2), 64854, 64725)]:
            _, normal = fit(part, package, pins, 'AND', 'BE')
            for name, cfg in [('a', input_cfg), ('q', output_cfg)]:
                work, bits = fit(part, package, pins, 'AND', 'BE', ['-pull_down', name])
                assert [i for i, (a, b) in enumerate(zip(normal, bits)) if a != b] == [cfg + 1], (part, name, cfg, [i for i, (a, b) in enumerate(zip(normal, bits)) if a != b])
                (work / 'test.si').write_text('ORDER:' + ','.join(f'P{p}' for p in pins) +
                                              ';\nVECTORS:\n00L\n01L\n10L\n11H\n')
                run('csim', ['-fl', 'test'], work)

        # SSTL inputs occupy an I/O cell and reserve its bank's reference pad.
        # Both 2.5-V and 3.3-V standards use the same I/O-standard fuse.
        for part, package, pins, inputs, refs in [
                (1504, 'TQFP44', (8, 23, 6), (30774, 31416), (32511, 31551)),
                (1504, 'TQFP100', (17, 52, 14), (30774, 31416), (32511, 31551)),
                (1508, 'TQFP100', (13, 58, 2), (64854, 63150), (64908, 63204)),
                (1508, 'CBGA132', (63, 90, 61), (64854, 63150), (64908, 63204))]:
            for bank, name in enumerate(('a', 'b')):
                for voltage in ('2.5', '3.3'):
                    work, bits = fit(part, package, pins, 'AND', 'BE',
                        [f'-voltage_level_{"AB"[bank]}', voltage, '-SSTL_input', name])
                    assert bits[inputs[bank] + 10] == 1, (part, bank, inputs[bank])
                    assert bits[inputs[1 - bank] + 10] == 0
                    assert bits[refs[bank]:refs[bank] + 2] == [0, 0]
                    assert bits[refs[bank] + 5:refs[bank] + 8] == [0, 0, 0]
                    (work / 'test.si').write_text('ORDER:' + ','.join(f'P{p}' for p in pins) +
                                                  ';\nVECTORS:\n00L\n01L\n10L\n11H\n')
                    run('csim', ['-fl', 'test'], work)
                    assert '# VREF' in (work / 'test.pin').read_text()
                    run(f'fit{part}', ['-i', 'test.edf', '-ifmt', 'EDIF', '-lib', 'cells.lib',
                        '-device', package, '-tech', f'ATF{part}BE', '-JTAG', 'OFF',
                        '-pinfile', 'test.pin', '-preassign', 'KEEP', '-output_fast', 'OFF',
                        '-UES', 'AZ', f'-voltage_level_{"AB"[bank]}', voltage,
                        '-SSTL_input', name], work)
                    assert fuses(work / 'test.jed') == bits
            for options, diagnostic in [
                    (['-SSTL_input', 'a'], '2.5 or 3.3 V'),
                    (['-voltage_level_A', '3.3', '-SSTL_input', 'b'], '2.5 or 3.3 V'),
                    (['-voltage_level_A', '3.3', '-SSTL_input', 'q'], 'not an input'),
                    (['-voltage_level_A', '3.3', '-SSTL_input', 'missing'], 'not an input'),
                    (['-voltage_level_A', '3.3', '-SSTL_input', 'a', '-schmitt_trigger', 'a'], 'Schmitt')]:
                assert diagnostic in fit(part, package, pins, 'AND', 'BE', options, False)
        # ON selects input pads only, including when output and input occupy
        # different banks. Unused-ground must leave the reference pad floating.
        _, bits = fit(1504, 'TQFP44', (8, 37, 18), 'AND', 'BE',
                      ['-voltage_level_A', '3.3', '-SSTL_input', 'ON',
                       '-unused_To_Ground', 'ON'])
        assert bits[30784] == 1 and bits[31210] == 0
        assert bits[32511:32513] == [0, 0]  # active VREFA
        assert bits[31551:31553] == [0, 1]  # unused VREFB remains a normal pad
        _, bits = fit(1504, 'TQFP44', (8, 37, 18), 'AND', 'BE',
                      ['-voltage_level_A', '3.3', '-SSTL_input', 'a', '-pin_keep', 'ON'])
        assert bits[32511:32513] == [0, 0]
        # Unconstrained SSTL inputs choose a bank with a suitable supply.
        _, bits = fit(1504, 'TQFP44', (None, None, 6), 'AND', 'BE',
                      ['-voltage_level_B', '2.5', '-SSTL_input', 'a,b'])
        assert bits[31210] == 1 and bits[31264] == 1
        assert bits[31556:31559] == [0, 0, 0]
        for pins, options in [
                ((8, 23, 5), ['-voltage_level_A', '3.3', '-SSTL_input', 'a']),
                ((8, 23, 6), ['-voltage_level_A', '3.3', '-SSTL_input', 'a', '-pd1', 'ON']),
                ((37, 38, 6), ['-voltage_level_A', '3.3', '-SSTL_input', 'a'])]:
            assert 'available VREF' in fit(1504, 'TQFP44', pins, 'AND', 'BE', options, False)
        # Enabling PD1 does not reserve the independent PD2 pad for logic.
        _, bits = fit(1504, 'TQFP44', (19, 37, 6), 'AND', 'BE',
                      ['-voltage_level_B', '3.3', '-SSTL_input', 'a', '-pd1', 'ON'])
        assert bits[31264] == 1
        for pins, bank, name, address in [
                ((37, 38, 8, 6), 'A', 'data', 30784),
                ((35, 38, 37, 6), 'B', 'ck', 32095)]:
            work, bits = fit(1504, 'TQFP44', pins, 'D', 'BE',
                             [f'-voltage_level_{bank}', '3.3', '-SSTL_input', name])
            assert bits[address] == 1
            (work / 'test.si').write_text('ORDER:' + ','.join(f'P{p}' for p in pins) +
                                         ';\nVECTORS:\n000L\nC01H\n001H\nC00L\n011L\n')
            run('csim', ['-fl', 'test'], work)
        # Bidirectional pads retain their output driver and physical input
        # feedback when the input buffer uses SSTL, including automatic pins.
        for part, package, pins, address in [
                (1504, 'TQFP44', (37, 38, 8, 6), 30784),
                (1508, 'TQFP100', (87, 88, 13, 2), 64864),
                (1508, 'CBGA132', (86, 30, 63, 61), 64864),
                (1504, 'TQFP44', (37, 38, None, 6), 32494)]:
            for selected in ('pad', 'ON'):
                work, bits = fit(part, package, pins, 'B', 'BE',
                    ['-voltage_level_A', '3.3', '-SSTL_input', selected])
                assert bits[address] == 1
                assigned = tuple(3 if p is None else p for p in pins)
                (work / 'test.si').write_text('ORDER:' + ','.join(f'P{p}' for p in assigned) +
                    ';\nVECTORS:\n000L\n001H\n01LL\n11HH\n')
                run('csim', ['-fl', 'test'], work)
                assert len(re.findall(r'^pad\s*:', (work / 'test.pin').read_text(), re.M)) == 1
                run(f'fit{part}', ['-i', 'test.edf', '-ifmt', 'EDIF',
                    '-device', package, '-tech', f'ATF{part}BE', '-JTAG', 'OFF',
                    '-pinfile', 'test.pin', '-preassign', 'KEEP', '-output_fast', 'OFF',
                    '-UES', 'AZ', '-voltage_level_A', '3.3', '-SSTL_input', selected], work)
                assert fuses(work / 'test.jed') == bits
        assert 'require ATF1504BE' in fit(1502, 'TQFP44', (37, 38, 42), 'AND', 'BE',
                                        ['-SSTL_input', 'a'], False)
    print(f'PASS: {checks} BE fitter layout, simulation, equivalence and rejection checks')


if __name__ == '__main__':
    main()
