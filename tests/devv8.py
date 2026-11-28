#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Validate standard V8 variants, package routing and reviewed catalog changes."""
import copy
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import devcompile
from devlang import binary, catalog, model, syntax
from devlang.compare import canonical, u16, u32
from defects import fuses


NAMES = {
    'gal16v8': set('g16v8 g16v8ms g16v8ma g16v8s g16v8a g16v8as g16v8c '
                  'g16v8cms g16v8cma g16v8cas g16v8cp g16v8cpms g16v8cpma g16v8cpas'.split()),
    'gal20v8': {name + suffix for name in
               'g20v8 g20v8ms g20v8ma g20v8s g20v8a g20v8as '
               'g20v8cp g20v8cpms g20v8cpma g20v8cps'.split() for suffix in ('', 'lcc')},
}


def descriptions():
    return {h['mnemonic']: h for family in NAMES for h in model.models(syntax.parse(
        (ROOT / 'devices/families' / (family + '.dev')).read_text()))}


def compare_records(actual, original, h, all_generated, all_original):
    family, name = h['architecture'], h['mnemonic']
    a, _ = canonical(actual, repairs=False, family=family)
    b, _ = canonical(original, repairs=False, family=family)
    b[3][0][0:2] = bytes([1, 0])
    automatic = h['mode'] == 'automatic'
    if family == 'gal16v8' and automatic:
        assert u32(b[3][0], 62) == 2048
        struct.pack_into('<I', b[3][0], 62, 2056)
        if name != 'g16v8':
            assert (u16(b[3][0], 25), b[3][0][27], u16(b[3][0], 39)) == (0, 0, 9)
            struct.pack_into('<H', b[3][0], 25, 8)
            b[3][0][27] = 8
            struct.pack_into('<H', b[3][0], 39, 25)
    if name in ('g20v8alcc', 'g20v8cplcc', 'g20v8aslcc', 'g20v8cpslcc'):
        assert b[3][0][11] == 0
        b[3][0][11] = 4  # draw the declared PLCC package
    if family == 'gal20v8' and automatic:
        # Automatic catalog entries are selection skeletons. Verify each
        # placeholder against the fixed registered view before replacing it.
        target = h['alternatives']['registered']
        fixed, _ = canonical(all_generated[target], repairs=False, family=family)
        old_fixed, _ = canonical(all_original[target], repairs=False, family=family)
        for section in (6, 7, 8, 10):
            assert a[section] == fixed[section], (name, section)
        assert a[9] == fixed[9][:1] + fixed[9][2:]
        for section in (8, 9, 10):
            assert b[section] == []
        expected_pins = []
        pins = list(range(1, 12)) + list(range(13, 24)) + [12, 24]
        for line, pin in enumerate(pins, 1):
            pd = bool(h['power_down']) and pin == 4
            expected_pins.append(bytearray(binary.record(16, binary.PIN_FIELDS,
                dict(line=line, pin=h['package'].number(pin), extension=40 if pd else 0,
                     kind=25 if pd else 2 if pin == 12 else 3 if pin == 24 else 0))))
        for pin in h['package'].unconnected:
            expected_pins.append(bytearray(binary.record(16, binary.PIN_FIELDS, dict(line=25, pin=pin, kind=19))))
        assert b[6] == expected_pins, name
        for row in old_fixed[7]:
            row[7] = row[9] = row[29] = 0
            row[10:14] = bytes([0, 0, int(name in ('g20v8lcc', 'g20v8alcc')), 0])
            row[18:22] = bytes(4)
            if row[4] == 14:
                row[26] = 0  # compatibility was normalized on both sides
            else:
                row[27] = 0
        assert b[7] == old_fixed[7], name
        for section in (6, 7, 8, 9, 10):
            b[section] = copy.deepcopy(a[section])
    else:
        # Correct the DIP CP power-down reservation to physical pin 4.
        if name == 'g20v8cpms':
            third = next(r for r in b[6] if u16(r, 2) == 3)
            fourth = next(r for r in b[6] if u16(r, 2) == 4)
            assert third[4:9] == bytes([40, 25, 0, 0, 0])
            assert fourth[4:9] == bytes([0, 0, 0, 8, 9])
            third[4:9] = bytes([0, 0, 0, 4, 5])
            fourth[4:9] = bytes([40, 25, 0, 0, 0])
        if name == 'g20v8cps':
            last = next(r for r in b[6] if u16(r, 2) == 23)
            assert last[4:9] == bytes([40, 25, 0, 0, 0])
            last[4:9] = bytes([0, 0, 0, 6, 7])
            fourth = next(r for r in b[6] if u16(r, 2) == 4)
            assert fourth[4:9] == bytes([0, 0, 0, 8, 9])
            fourth[4:9] = bytes([40, 25, 0, 0, 0])
        if h['mode'] == 'simple':
            outputs = {h['package'].number(c['pin']): c['simple'].positions[0] for c in h['cells']}
            rows = []
            for row in b[6]:
                pin = u16(row, 2)
                if pin in outputs and row[5] == 0:
                    if row[11]:
                        assert row[10:12] == bytes([1, 1]) and u32(row, 12) == outputs[pin]
                        row[5], row[10:16] = 7, bytes(6)
                    else:
                        # A-mode entries have duplicate input/feedback rows.
                        feedback = bytearray(row)
                        feedback[5] = 7
                        assert feedback in b[6]
                        continue
                rows.append(row)
            b[6] = rows
        if name in ('g20v8aslcc', 'g20v8cpslcc'):
            for i, row in enumerate(r for r in b[6] if r[5] == 19):
                assert u16(row, 0) == 25
                struct.pack_into('<H', row, 0, 23 + i)
    assert a == b, (name, [i for i in range(13) if a[i] != b[i]])


def main():
    bindir = Path(sys.argv[1]).resolve()
    env = dict(os.environ, LINCUPL_SHARE=str(bindir.parent / 'share/lincupl'))
    env.pop('LIBCUPL', None)
    env.pop('FITTERDIR', None)
    hardware = descriptions()
    generated = {}
    for family, names in NAMES.items():
        data = devcompile.compile_text((ROOT / 'devices/families' / (family + '.dev')).read_text())
        entries = catalog.entries(data)
        assert {e['name'] for e in entries} == names
        generated.update({e['name']: e['image'] for e in entries})
    assert len(hardware) == len(generated) == 34
    original_data = devcompile.compile_directory(ROOT / 'devices', generated, hardware=False)
    originals = {e['name']: e['image'] for e in catalog.entries(original_data)}
    for name, image in generated.items():
        compare_records(image, originals[name], hardware[name], generated, originals)
        assert set(hardware[name]['fuse_owners']) == set(range(hardware[name]['fuses']))

    source = (ROOT / 'devices/families/gal20v8.dev').read_text()
    invalid = [('columns = 40', 'columns = 32'), ('[2..7, 9..14, 16..21, 23..28]', '[2..7, 9..14, 16..22, 24..28]'),
               ('unconnected = [1, 8, 15, 22]', 'unconnected = [1, 8, 15, 23]'),
               ('control = pin(4)', 'control = pin(3)'), ('fuse = 2706', 'fuse = 2705'),
               ('fuses = 2707', 'fuses = 2706'), ('combinational_feedback = simple', 'combinational_feedback = registered'),
               ('9, 15, 10, 14', '9, 18, 10, 14')]
    for before, after in invalid:
        assert before in source
        try:
            devcompile.compile_text(source.replace(before, after, 1))
        except syntax.Error:
            pass
        else:
            raise AssertionError(('accepted invalid V8 model', after))

    cases = 0
    with tempfile.TemporaryDirectory(prefix='devv8-') as temp:
        root = Path(temp)
        libraries = {'hardware': root / 'hardware.dl', 'catalog': root / 'catalog.dl'}
        libraries['hardware'].write_bytes(catalog.pack_library([dict(name=n, image=b) for n, b in generated.items()]))
        libraries['catalog'].write_bytes(original_data)

        def run(tool, args, work, expected=0):
            exe = bindir / (tool + ('.exe' if os.name == 'nt' else ''))
            p = subprocess.run([str(exe), *map(str, args)], cwd=work, env=env,
                               capture_output=True, text=True, timeout=20)
            assert p.returncode == expected, (work.name, tool, args, p.stdout, p.stderr)
            return p.stdout + p.stderr

        def check(h, label, body, order, vectors, mode, index=0, polarity=1, feedback=False, compare=True):
            nonlocal cases
            name, wide = h['mnemonic'], h['architecture'] == 'gal20v8'
            base = 2560 if wide else 2048
            maps = []
            for kind in (('catalog', 'hardware') if compare else ('hardware',)):
                library = libraries[kind]
                work = root / f'{name}-{label}-{kind}'
                work.mkdir()
                (work / 'test.pld').write_text(f'Name test; Partno V8TEST12; Device {name};\n' + body)
                (work / 'test.si').write_text(f'ORDER:{order};\nVECTORS:\n' + vectors)
                run('cupl', ['-jaxfm1', '-u', library, 'test.pld'], work)
                bits = fuses(work / 'test.jed')
                assert len(bits) == base + 146 + bool(h['power_down'])
                assert bits[base:base + 8][index] == polarity
                assert bits[base + 8:base + 72] == [int(b) for c in b'V8TEST12' for b in f'{c:08b}']
                assert bits[base + 80:base + 144] == [1] * 64
                assert bits[base + 144:base + 146] == {'registered': [0, 1], 'complex': [1, 1], 'simple': [1, 0]}[mode], (name, label, kind, mode, bits[base + 144:base + 146])
                if h['power_down']:
                    assert bits[-1] == 1
                maps.append(bits)
                run('csim', ['-l', '-u', library, 'test'], work)
                if not h['power_down']:
                    broken = feedback and kind == 'catalog' and name in ('g16v8s', 'g20v8s', 'g20v8slcc')
                    log = run('csim', ['-fl', '-u', library, 'test'], work, int(broken))
                    if broken:
                        assert 'fuse map makes an input of pin' in log
                else:
                    log = run('csim', ['-fl', '-u', library, 'test'], work, 1)
                    assert 'no device for JEDEC file' in log, log
                # Check the actual plot VM: every cell header and all 64 rows.
                doc = (work / 'test.doc').read_text()
                assert list(map(int, re.findall(r'Pin #\s*(\d+)', doc))) == [h['package'].number(c['pin']) for c in h['cells']]
                assert len(re.findall(r'^\s*\d{5}\s+[x-]{' + str(40 if wide else 32) + r'}\s*$', doc, re.M)) == 64
            if compare:
                delta = [i for i, (a, b) in enumerate(zip(*maps)) if a != b]
                expected = [base + 72 + index] if feedback and name in ('g16v8s', 'g20v8s', 'g20v8slcc') else []
                assert delta == expected, (name, label, delta)
            # A deliberately wrong vector must fail in each supported mode.
            wrong = vectors.splitlines()
            wrong[0] = wrong[0][:-1] + ('L' if wrong[0][-1] == 'H' else 'H')
            (work / 'test.si').write_text(f'ORDER:{order};\nVECTORS:\n' + '\n'.join(wrong))
            for flag in (('-l',) if h['power_down'] else ('-l', '-fl')):
                assert 'user expected' in run('csim', [flag, '-u', libraries['hardware'], 'test'], work, 1)
            cases += 1
            return bits

        for name, h in hardware.items():
            loaded = root / (name + '.image')
            run('devlib_read_test', [libraries['hardware'], name, loaded], root)
            assert loaded.read_bytes() == generated[name]
            p = h['package'].number
            # Avoid the reserved PD pad in common catalog comparisons.
            modes = ('registered', 'complex', 'simple') if h['mode'] == 'automatic' else (h['mode'],)
            for mode in modes:
                for i, cell in enumerate(h['cells']):
                    for inv in (False, True):
                        q = '!q' if inv else 'q'
                        body = f'pin {p(2)}=a; pin {p(5)}=b; pin {p(cell["pin"])}={q}; '
                        if mode == 'registered':
                            body += f'pin {p(1)}=clk; pin {p(h["enable_pin"])}=disable; q.d=a;'
                            order = 'clk,a,disable,' + q
                            vectors = 'C10' + 'LH'[1 ^ inv] + '\n010' + 'LH'[1 ^ inv] + '\nC00' + 'LH'[inv] + '\nC11Z\n000' + 'LH'[1 ^ inv] + '\n'
                        else:
                            body += 'q=a&b;'
                            if mode == 'complex':
                                body += f'pin {p(6)}=enable; q.oe=enable;'
                            order = 'a,b,' + ('enable,' if mode == 'complex' else '') + q
                            vectors = ''.join(f'{a}{b}' + ('1' if mode == 'complex' else '') + 'LH'[(a & b) ^ inv] + '\n' for a in (0, 1) for b in (0, 1))
                            if mode == 'complex':
                                vectors += '110Z\n000Z\n'
                        check(h, f'{mode}-{i}-{inv}', body, order, vectors, mode, i, int(not inv))
            if h['mode'] == 'automatic':
                body = f'pin {p(2)}=a; pin {p(h["cells"][0]["pin"])}=q; q=a;'
                check(h, 'automatic-combinational', body, 'a,q', '0L\n1H\n', 'simple')
                body = (f'pin {p(2)}=a; pin {p(h["cells"][1]["pin"])}=q; '
                        f'pin {p(h["cells"][0]["pin"])}=y; q=a; y=q;')
                check(h, 'automatic-feedback', body, 'a,q,y', '0LL\n1HH\n',
                      'complex' if h['prefer_complex_feedback'] else 'simple', index=1)
            if h['mode'] in ('simple', 'complex'):
                for i, cell in enumerate(h['cells']):
                    pin = cell['pin']
                    other = h['cells'][int(i == 0)]['pin']
                    body = f'pin {p(2)}=a; pin {p(5)}=b; pin {p(pin)}=q; pin {p(other)}=y; q=a; y=q&b;'
                    if pin in h['columns'][h['mode']]:
                        check(h, f'feedback-{i}', body, 'a,b,q,y', '00LL\n01LL\n10HL\n11HH\n',
                              h['mode'], i, feedback=h['mode'] == 'simple')
                        if i:
                            body = f'pin {p(pin)}=a; pin {p(h["cells"][0]["pin"])}=q; q=a;'
                            bits = check(h, f'pad-input-{i}', body, 'a,q', '0L\n1H\n', h['mode'])
                            if h['mode'] == 'simple':
                                assert bits[cell['simple'].positions[0]] == 1
                    else:
                        work = root / f'{name}-missing-feedback-{i}'
                        work.mkdir()
                        (work / 'test.pld').write_text(f'Name test; Device {name}; ' + body)
                        run('cupl', ['-jm1', '-u', libraries['hardware'], 'test.pld'], work, 1)
            if h['power_down']:
                # Pin 3 remains a logic input in every CP package; pin 4 is PD.
                mode = 'registered' if h['mode'] in ('registered', 'automatic') else h['mode']
                body = f'pin {p(3)}=a; pin {p(h["cells"][0]["pin"])}=q; '
                if mode == 'registered':
                    body += f'pin {p(1)}=clk; pin {p(h["enable_pin"])}=disable; q.d=a;'
                    order, vectors = 'clk,a,disable,q', 'C00L\nC10H\n000H\n'
                else:
                    body += 'q=a;'
                    order, vectors = 'a,q', '0L\n1H\n'
                check(h, 'pin3', body, order, vectors, mode, compare=False)
                if h['architecture'] == 'gal20v8':
                    check(h, 'pin23', body.replace(f'pin {p(3)}=a;', f'pin {p(23)}=a;'),
                          order, vectors, mode, compare=False)
                work = root / f'{name}-reserved-pd'
                work.mkdir()
                (work / 'test.pld').write_text(f'Name test; Device {name}; ' + body.replace(f'pin {p(3)}=a;', f'pin {p(4)}=a;'))
                run('cupl', ['-jm1', '-u', libraries['hardware'], 'test.pld'], work, 1)
    print(f'PASS: 34 V8 interfaces, {cases} native cases, {len(invalid)} invalid models; CP JEDEC decoding remains unsupported')


if __name__ == '__main__':
    main()
