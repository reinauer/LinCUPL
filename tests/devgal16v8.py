#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Validate GAL16V8 modes against catalog records and independent pin traces."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import devcompile
from devlang import catalog, model, syntax
from csim_jed import vector_lines
from defects import fuses


def compare_records(actual, reference, name):
    a, b = map(catalog.records, (actual, reference))
    # Each difference has a reviewed rule, applied only to its named field.
    b[3][0][0:2] = (1).to_bytes(2, 'little')  # hardware compiler revision
    b[3][0][9:11] = b'\0\0'  # inactive PROM operands
    if name == 'g16v8':
        assert int.from_bytes(b[3][0][62:66], 'little') == 2048
        b[3][0][62:66] = (2056).to_bytes(4, 'little')
    for offset in (4, 5, 7, 20):
        a[3][0][offset] = b[3][0][offset]  # retained during default assembly
    for left, right in zip(a[7], b[7]):
        left[26] = right[26]  # legacy width, also retained during assembly
        if right[29] == 0:
            right[18:22] = bytes(4)  # inactive configuration fuse address
    for row in b[6]:
        if name == 'g16v8s' and int.from_bytes(row[2:4], 'little') in (12, 13, 14, 17, 18, 19):
            assert row[5] == 0 and row[10:12] == bytes([1, 1])
            row[5] = 7  # pad feedback, not an input-only configuration write
            row[10:16] = bytes(6)
        if row[11] == 0:
            row[10] = 0
            row[12:16] = bytes(4)
    assert b[11] == [bytes(24)]
    b[11] = []
    assert a == b, (name, [i for i, (left, right) in enumerate(zip(a, b)) if left != right])


def main():
    bindir = Path(sys.argv[1]).resolve()
    env = dict(os.environ, LINCUPL_SHARE=str(bindir.parent / 'share/lincupl'))
    env.pop('LIBCUPL', None)
    env.pop('FITTERDIR', None)
    source = (ROOT / 'devices/families/gal16v8.dev').read_text()
    compiled = devcompile.compile_text(source, devices=['g16v8', 'g16v8ms', 'g16v8ma', 'g16v8s'])
    generated = {e['name']: e['image'] for e in catalog.entries(compiled)}
    assert set(generated) == {'g16v8', 'g16v8ms', 'g16v8ma', 'g16v8s'}
    reference_data = devcompile.compile_directory(ROOT / 'devices', generated, hardware=False)
    originals = {e['name']: e['image'] for e in catalog.entries(reference_data)}
    for name, image in generated.items():
        compare_records(image, originals[name], name)
    for device in model.inherited_devices(syntax.parse(source)):
        hardware = model.compile_model(device)
        assert set(hardware['fuse_owners']) == set(range(hardware['fuses']))
    rejected = 0
    for before, after in [
            ('columns = 32', 'columns = 31'), ('first_row = 8', 'first_row = 0'),
            ('first_row = 8', 'first_row = 9'), ('polarity_fuse = 2049', 'polarity_fuse = 2048'),
            ('cell_mode_fuse = 2121', 'cell_mode_fuse = 2119'),
            ('registered = 2\n', 'registered = 0\n'), ('simple_input = 1', 'simple_input = 0'),
            ('first_fuse = 2056', 'first_fuse = 2048'), ('first_fuse = 2128', 'first_fuse = 2127'),
            ('enabled = 1', 'enabled = 0'), ('clock = device.global_clock', 'clock = pin(2)'),
            ('registered = inverted(state.q)', 'registered = state.q'),
            ('6, 14, 7, 13', '6, 15, 7, 13'), ('6, 15, 7, 14', '6, 15, 7, 15'),
            ('complex = g16v8ma', 'complex = g16v8ms')]:
        assert before in source, before
        try:
            devcompile.compile_text(source.replace(before, after, 1))
        except syntax.Error:
            rejected += 1
        else:
            raise AssertionError(('invalid description accepted', after))
    cases = 0
    with tempfile.TemporaryDirectory(prefix='devgal16v8-') as temp:
        root = Path(temp)
        hardware, reference = root / 'hardware.dl', root / 'catalog.dl'
        hardware.write_bytes(compiled)
        reference.write_bytes(reference_data)

        def run(tool, args, work, expected=0):
            exe = bindir / (tool + ('.exe' if os.name == 'nt' else ''))
            p = subprocess.run([str(exe), *map(str, args)], cwd=work, env=env,
                               capture_output=True, text=True, timeout=20)
            assert p.returncode == expected, (work.name, tool, args, p.stdout, p.stderr)
            return p.stdout + p.stderr

        def check(device, label, body, order, vectors, mode, index, polarity, legacy_feedback=False):
            nonlocal cases
            maps, listings = [], []
            for kind, library in [('catalog', reference), ('hardware', hardware)]:
                work = root / f'{device}-{label}-{kind}'
                work.mkdir()
                (work / 'test.pld').write_text(f'Name test; Partno V8TEST12; Device {device};\n' + body)
                (work / 'test.si').write_text(f'ORDER: {order};\nVECTORS:\n' + vectors)
                run('cupl', ['-jaxfm1', '-u', library, 'test.pld'], work)
                bits = fuses(work / 'test.jed')
                assert len(bits) == 2194
                assert bits[2192:2194] == {'registered': [0, 1], 'complex': [1, 1], 'simple': [1, 0]}[mode]
                assert bits[2048 + index] == polarity
                signature = [int(bit) for byte in b'V8TEST12' for bit in f'{byte:08b}']
                assert bits[2056:2120] == signature, (device, 'signature')
                assert bits[2128:2192] == [1] * 64
                maps.append(bits)
                for flags in ('-l', '-fl'):
                    if legacy_feedback and kind == 'catalog' and flags == '-fl':
                        output = run('csim', [flags, '-u', library, 'test'], work, 1)
                        assert 'fuse map makes an input of pin' in output
                        continue
                    run('csim', [flags, '-u', library, 'test'], work)
                    listings.append(vector_lines((work / 'test.so').read_text()))
            if legacy_feedback:
                assert maps[0][2120 + index] == 1 and maps[1][2120 + index] == 0
                assert [i for i, (a, b) in enumerate(zip(*maps)) if a != b] == [2120 + index]
            else:
                assert maps[0] == maps[1], (device, label, 'JEDEC difference')
            assert all(rows == listings[0] for rows in listings), (device, label, 'simulation difference')
            cases += 1
            return bits, work

        for device in generated:
            loaded = root / (device + '.image')
            run('devlib_read_test', [hardware, device, loaded], root)
            assert loaded.read_bytes() == generated[device], 'generated image required reader repairs'
            modes = (['registered', 'complex', 'simple'] if device == 'g16v8' else
                     [{'g16v8ms': 'registered', 'g16v8ma': 'complex', 'g16v8s': 'simple'}[device]])
            for mode in modes:
                for index, pin in enumerate(range(19, 11, -1)):
                    for inverted in (False, True):
                        q = '!q' if inverted else 'q'
                        body = f'pin 2=a; pin 3=b; pin {pin}={q}; '
                        if mode == 'registered':
                            body += 'pin 1=clk; pin 11=disable; q.d=a;'
                            order = f'clk,a,disable,{q}'
                            vectors = ('C10' + 'LH'[1 ^ inverted] + '\n010' + 'LH'[1 ^ inverted] +
                                       '\nC00' + 'LH'[inverted] + '\nC11Z\n001Z\n000' +
                                       'LH'[1 ^ inverted] + '\n')
                        else:
                            body += 'q=a&b;'
                            if mode == 'complex':
                                body += ' pin 4=enable; q.oe=enable;'
                            order = 'a,b,' + ('enable,' if mode == 'complex' else '') + q
                            vectors = ''.join(f'{a}{b}' + ('1' if mode == 'complex' else '') +
                                              'LH'[(a & b) ^ inverted] + '\n'
                                              for a in (0, 1) for b in (0, 1))
                            if mode == 'complex':
                                vectors += '110Z\n000Z\n'
                        bits, work = check(device, f'{mode}-{pin}-{inverted}', body, order, vectors,
                                           mode, index, int(not inverted))
                        assert bits[2120 + index] == int(mode == 'complex')
                        # A deliberately wrong expectation must be observed.
                        if index == 0 and not inverted:
                            wrong = vectors.splitlines()
                            wrong[0] = wrong[0][:-1] + ('L' if wrong[0][-1] == 'H' else 'H')
                            (work / 'test.si').write_text(f'ORDER:{order};\nVECTORS:\n' + '\n'.join(wrong))
                            for flags in ('-l', '-fl'):
                                assert 'user expected' in run('csim', [flags, '-u', hardware, 'test'], work, 1)

        # Registered feedback must survive a disabled output buffer.
        for inverted in (False, True):
            q = '!q' if inverted else 'q'
            body = (f'pin 1=clk; pin 2=a; pin 3=b; pin 11=disable; pin 19={q}; pin 18=y; '
                    'q.d=a; y=q&b;')
            vectors = ('C110' + 'LH'[1 ^ inverted] + 'H\n0111ZH\n0001ZL\nC010' +
                       'LH'[inverted] + 'L\n')
            check('g16v8ms', f'feedback-{inverted}', body, f'clk,a,b,disable,{q},y', vectors,
                  'registered', 0, int(not inverted))

        # Feed output pads back only where each mode has a physical path.
        for device, mode, available in [('g16v8ma', 'complex', range(13, 19)),
                                        ('g16v8s', 'simple', (12, 13, 14, 17, 18, 19))]:
            for pin in range(12, 20):
                other = 18 if pin == 19 else 19
                body = f'pin 2=a; pin 3=b; pin {pin}=q; pin {other}=y; q=a; y=q&b;'
                if pin in available:
                    check(device, f'pad-feedback-{pin}', body, 'a,b,q,y',
                          '00LL\n01LL\n10HL\n11HH\n', mode, 19 - pin, 1, mode == 'simple')
                else:
                    work = root / f'{device}-unavailable-feedback-{pin}'
                    work.mkdir()
                    (work / 'test.pld').write_text(f'Name test; Device {device}; ' + body)
                    run('cupl', ['-jm1', '-u', hardware, 'test.pld'], work, 1)

        # Simple mode can use a feedback-capable output as an input.
        bits, _ = check('g16v8s', 'input', 'pin 17=a; pin 2=b; pin 19=q; q=a&b;',
                        'a,b,q', '00L\n01L\n10L\n11H\n', 'simple', 0, 1)
        assert bits[2122] == 1 and bits[2120] == 0
    print(f'PASS: {cases} GAL16V8 compiler/ABS/JEDEC cases; {rejected} invalid descriptions rejected')


if __name__ == '__main__':
    main()
