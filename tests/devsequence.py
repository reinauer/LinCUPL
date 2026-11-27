#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Exercise device-selected state machines and registered sequence outputs."""
import argparse
import itertools
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import devcompile
from devlang import binary, catalog
from devlang.audit import validate_coverage
from devlang.syntax import Error
from defects import fuses


MACHINE = '''{keyword} state {{
    present 0
        if a next 1 out y;
        default next 0;
    present 1
        if a next 0;
        default next 1 out y;
}}
'''
VECTORS = '''ORDER: clk,a,q,y;
VECTORS:
C0 LL
C1 HH
00 HH
C0 HH
C1 LL
01 LL
C1 HH
C1 LL
'''


def source(device, pins, keyword, clock=False, buried=False):
    clk, a, q, y = pins
    return (f'Name test; Device {device}; pin {clk}=clk; pin {a}=a; '
            f'{"pinnode" if buried else "pin"} {q}=q; pin {y}=y; field state=[q];\n'
            + MACHINE.format(keyword=keyword)
            + ('q.ck=clk; y.ck=clk;\n' if clock else ''))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('bin_dir', type=Path)
    bindir = parser.parse_args().bin_dir.resolve()
    env = dict(os.environ, LINCUPL_SHARE=str(bindir.parent / 'share/lincupl'))
    env.pop('LIBCUPL', None)
    env.pop('FITTERDIR', None)
    runs = cases = 0
    with tempfile.TemporaryDirectory(prefix='devsequence-') as temp:
        root = Path(temp)

        def run(tool, args, cwd, expected=0):
            nonlocal runs
            exe = bindir / (tool + ('.exe' if os.name == 'nt' else ''))
            result = subprocess.run([str(exe), *map(str, args)], cwd=cwd, env=env,
                                    capture_output=True, text=True, timeout=20)
            runs += 1
            assert result.returncode == expected, (cwd.name, tool, result.stdout, result.stderr)
            return result.stdout + result.stderr

        def check(library, label, device, kind, pins, clock=False, buried=False):
            nonlocal cases
            maps = []
            for keyword in ('sequence', 'sequence' + kind):
                work = root / f'{label}-{device}-{keyword}'
                work.mkdir()
                (work / 'test.pld').write_text(source(device, pins, keyword, clock, buried))
                run('cupl', ['-jaxm1', '-u', library, 'test.pld'], work)
                maps.append(fuses(work / 'test.jed'))
                # The state and registered OUT both implement q XOR a. A
                # low clock must retain both values. These families have
                # independent JEDEC simulation as well as ABS simulation.
                if kind == 'd':
                    (work / 'test.si').write_text(VECTORS)
                    for flags in ('-l', '-fl'):
                        run('csim', [flags, '-u', library, 'test'], work)
            assert maps[0] == maps[1], (label, device, 'default register type changed the fuse map')
            cases += 1

        selections = {
            'Atmel': [('g22v10', 'd', (1, 2, 23, 22), False, False),
                      ('v750c', 'd', (1, 2, 23, 22), True, False),
                      ('v2500c', 'd', (1, 2, 4, 5), True, False)],
            'cupl': [('g22v10', 'd', (1, 2, 23, 22), False, False),
                     ('f105', 'rs', (1, 2, 29, 18), False, True),
                     ('f155', 'jk', (1, 2, 17, 16), False, False),
                     ('f506', 'rs', (1, 2, 25, 8), False, True)],
        }
        for label, selected in selections.items():
            library = root / (label + '.dl')
            library.write_bytes(devcompile.compile_directory(
                ROOT / 'devices' / label, [case[0] for case in selected]))
            for case in selected:
                check(library, label, *case)

        modeled = 0
        for family in ('atf750c', 'gal22v10', 'atf2500'):
            library = root / (family + '.dl')
            library.write_bytes(devcompile.compile_text(
                (ROOT / 'devices/families' / (family + '.dev')).read_text()))
            for entry in catalog.entries(library.read_bytes()):
                attrs = catalog.records(entry['image'])[3][0]
                if family == 'atf2500':
                    pins = (1, 2, 5, 6) if attrs[8] == 44 else (1, 2, 4, 5)
                    assert attrs[51:57] == bytes([1, 31, 1, 1, 31, 1])
                else:
                    pins = (2, 3, 27, 26) if attrs[8] == 28 else (1, 2, 23, 22)
                    assert attrs[51:57] == bytes([1, 0, 1, 1, 0, 1])
                check(library, family, entry['name'], 'd', pins, family != 'gal22v10')
                modeled += 1
        assert modeled == 17

        # Even currently inactive next-function descriptors are retained and
        # compared. They must not silently return to the exclusion list.
        text = (ROOT / 'devices/Atmel/g22v10.dev').read_text()
        item, = catalog.compile_text(text)
        image = item['image']
        offset = struct.unpack_from('<I', image, 3 * 20 + 8)[0]
        for field in catalog.ATTRIBUTES['sequence']:
            missing = dict(catalog.FIELDS)
            missing[3] = [f for f in missing[3] if f != field]
            try:
                validate_coverage(missing, binary.WIDTHS)
            except Error:
                pass
            else:
                raise AssertionError(f'{field.name} was silently excluded')
            changed = bytearray(image)
            changed[offset + field.offset] ^= 1
            assert not catalog.compare_image(changed, image)['canonical_equal']
        for field in ('next_count', 'input_count'):
            changed, = catalog.compile_text(text.replace(f'{field} = 1', f'{field} = 0'))
            library = root / (field + '.dl')
            library.write_bytes(catalog.pack_library([changed]))
            work = root / field
            work.mkdir()
            (work / 'test.pld').write_text(source('g22v10', (1, 2, 23, 22), 'sequence'))
            log = run('cupl', ['-jaxm1', '-u', library, 'test.pld'], work, expected=1)
            assert 'does not support synchronous state machines' in log

        # This interface uses two equations for enabled-D registers. Check
        # the emitted fitter input directly, without an external fitter.
        library = root / 'enabled.dl'
        library.write_bytes(devcompile.compile_directory(ROOT / 'devices/cupl', ['mapl144']))
        work = root / 'enabled'
        work.mkdir()
        (work / 'test.pld').write_text(source('mapl144', (1, 2, 13, 14), 'sequence'))
        run('cupl', ['-baxm1', '-u', library, 'test.pld'], work)
        lines = (work / 'test.pla').read_text().splitlines()
        inputs = next(line.split()[1:] for line in lines if line.startswith('.ilb '))
        outputs = next(line.split()[1:] for line in lines if line.startswith('.ob '))
        assert inputs == ['a', 'q'] and outputs[:4] == ['q.D', 'q.CE', 'y.D', 'y.CE']
        products = [line.split() for line in lines if line and line[0] in '01-']
        for a, q in itertools.product((0, 1), repeat=2):
            values = f'{a}{q}'
            enabled = [out for term, out in products
                       if all(bit == '-' or bit == value for bit, value in zip(term, values))]
            actual = [int(any(out[i] == '1' for out in enabled)) for i in range(len(outputs))]
            assert actual == [a & (1 - q), a, a ^ q, a ^ q] + [1] * (len(outputs) - 4)

    print(f'PASS: {cases} default-register comparisons, all 17 modeled interfaces, '
          f'enabled-D fitter equations, and {runs} native checks')
    return 0


if __name__ == '__main__':
    sys.exit(main())
