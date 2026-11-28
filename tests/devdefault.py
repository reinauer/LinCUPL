#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Check default hardware selection, archival metadata and native behavior."""
import copy
import json
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
from devlang.syntax import Error
from defects import fuses
from devv8 import NAMES, descriptions


FAMILIES = {
    **NAMES,
    'atf750c': {'v750c', 'v750cext', 'v750cppk', 'v750cextppk',
                'v750clcc', 'v750cextlcc', 'v750cppklcc', 'v750cextppklcc'},
    'gal22v10': {'g22v10', 'g22v10lcc', 'g22v10i', 'p22v10', 'p22v10lcc'},
    'atf2500': {'v2500c', 'v2500cppk', 'v2500clcc', 'v2500cppklcc'},
}


def identity(row):
    array, pin, function = struct.unpack_from('<HHB', row)
    return array, 0 if function == 14 else pin, function


def check_metadata(actual, model, original):
    a, b, old = map(catalog.records, (actual, model, original))
    # Compare every hardware byte; only the five compatibility fields may
    # differ from the standalone model, and each must equal its catalog value.
    for offset in (4, 5, 7, 20):
        assert a[3][0][offset] == old[3][0][offset]
        a[3][0][offset] = b[3][0][offset]
    widths = {}
    for row in old[7]:
        key = identity(row)
        assert key not in widths or widths[key] == row[26]
        widths[key] = row[26]
    for row, modeled in zip(a[7], b[7]):
        assert row[26] == widths[identity(row)]
        row[26] = modeled[26]
    assert a == b, 'default assembly changed modeled hardware'


def main():
    bindir = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else None
    old_data = devcompile.compile_directory(ROOT / 'devices', hardware=False)
    data = devcompile.compile_directory(ROOT / 'devices')
    old, actual = catalog.entries(old_data), catalog.entries(data)
    expected = {entry['name']: entry for entry in old}
    assert len(actual) == len(old) == 660
    assert [(e['name'], e['flags']) for e in actual] == [(e['name'], e['flags']) for e in old]
    v8_models = descriptions()
    models = {}
    for family, names in FAMILIES.items():
        path = ROOT / 'devices/families' / (family + '.dev')
        emitted = catalog.entries(devcompile.compile_text(path.read_text(), str(path)))
        assert {e['name'] for e in emitted} == names
        models.update({e['name']: e['image'] for e in emitted})
    assert len(models) == 51
    for entry in actual:
        name = entry['name']
        if name in models:
            check_metadata(entry['image'], models[name], expected[name]['image'])
        else:
            assert entry['image'] == expected[name]['image'], name
    chosen = ['v750c', 'v2500clcc', 'g22v10i', 'g16v8ma', 'p16l8']
    selected = catalog.entries(devcompile.compile_directory(ROOT / 'devices', chosen))
    assert [(e['name'], e['image']) for e in selected] == [
        (e['name'], e['image']) for e in actual if e['name'] in chosen]

    # Nonzero sentinels and reordered records must be copied by identity,
    # never inferred from a hardware width or attached by row position.
    source = catalog.records(expected['v750c']['image'])
    for i, offset in enumerate((4, 5, 7, 20)):
        source[3][0][offset] = 201 + i
    values = {key: (i * 7 + 13) % 256 for i, key in enumerate(sorted({identity(r) for r in source[7]}))}
    for row in source[7]:
        row[26] = values[identity(row)]
    source[7].reverse()
    sentinels = binary.image(source)
    patched = catalog.preserve_compatibility(models['v750c'], sentinels)
    check_metadata(patched, models['v750c'], sentinels)

    def rejected(action):
        try:
            action()
        except (Error, OSError):
            return
        raise AssertionError('accepted invalid hardware replacement')

    conflict = copy.deepcopy(source)
    duplicate = bytearray(conflict[7][0])
    duplicate[26] ^= 1
    conflict[7].append(duplicate)
    rejected(lambda: catalog.preserve_compatibility(models['v750c'], binary.image(conflict)))
    missing = catalog.records(models['v750c'])
    struct.pack_into('<H', missing[7][0], 0, 65535)
    rejected(lambda: catalog.preserve_compatibility(binary.image(missing), sentinels))

    with tempfile.TemporaryDirectory(prefix='devdefault-') as temp:
        root = Path(temp)
        archive = root / 'archive'
        archive.mkdir()
        for name in ('v750c', 'p16l8'):
            origin = ROOT / 'devices' / ('Atmel' if name == 'v750c' else 'cupl') / (name + '.dev')
            (archive / origin.name).write_bytes(origin.read_bytes())
        (archive / 'manifest.json').write_text(json.dumps(dict(format='lincupl-device-catalog-1', entries=[
            dict(name='v750c', file='v750c.dev', flags=3),
            dict(name='v750c', file='v750c.dev', flags=1),
            dict(name='p16l8', file='p16l8.dev', flags=1)])))
        source_path = root / 'family.dev'
        text = (ROOT / 'devices/families/atf750c.dev').read_text()
        source_path.write_text(text)
        manifest = dict(format='lincupl-device-catalog-set-2', catalogs=['archive'],
                        hardware=[dict(file='family.dev', devices=['v750c'])])
        manifest_path = root / 'manifest.json'
        manifest_path.write_text(json.dumps(manifest))
        before = catalog.entries(devcompile.compile_directory(root))
        assert [e['name'] for e in before] == ['v750c', 'p16l8']
        assert before[0]['flags'] == 3
        # A valid source edit must reach the default library, while the
        # archival output must stay unchanged.
        modified = text.replace('pin 2  { high = 75 low = 74 }', 'pin 2  { high = 67 low = 66 }')
        modified = modified.replace('pin 3  { high = 67 low = 66 }', 'pin 3  { high = 75 low = 74 }')
        assert modified != text
        source_path.write_text(modified)
        after = catalog.entries(devcompile.compile_directory(root))
        assert before[0]['image'] != after[0]['image']
        assert before[1] == after[1]
        archival = devcompile.compile_directory(root, hardware=False)
        assert catalog.entries(archival)[0]['image'] == expected['v750c']['image']
        # Existing version-1 manifests may carry informational metadata.
        manifest_path.write_text(json.dumps(dict(format='lincupl-device-catalog-set-1',
            catalogs=['archive'], description='archival catalog')))
        assert devcompile.compile_directory(root) == archival
        source_path.write_text(text)
        malformed = [dict(manifest, format='lincupl-device-catalog-set-1'),
                     {k: v for k, v in manifest.items() if k != 'hardware'}]
        for definitions in ([], None, {}, [None], [dict(file='family.dev', devices=[])],
                            [dict(file='family.dev', devices=[1])],
                            [dict(file='family.dev', devices=['v750c', 'v750c'])],
                            manifest['hardware'] * 2,
                            [dict(file='family.dev', devices=['absent'])],
                            [dict(file='family.dev', devices=['p16l8'])],
                            [dict(file='../outside.dev', devices=['v750c'])],
                            [dict(file=str(source_path), devices=['v750c'])],
                            [dict(file='archive/v750c.dev', devices=['v750c'])]):
            malformed.append(dict(manifest, hardware=definitions))
        for bad in malformed:
            manifest_path.write_text(json.dumps(bad))
            rejected(lambda: devcompile.compile_directory(root))
        manifest_path.write_text(json.dumps(manifest))
        source_path.unlink()
        output = root / 'catalog.dl'
        result = subprocess.run([sys.executable, str(ROOT / 'tools/devcompile.py'), str(root),
                                 '--catalog-only', '-o', str(output)], capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
        assert output.read_bytes() == archival
        result = subprocess.run([sys.executable, str(ROOT / 'tools/devcompile.py'), str(root),
                                 '-o', str(root / 'missing.dl')], capture_output=True, text=True)
        assert result.returncode == 1 and 'Traceback' not in result.stderr
        assert not (root / 'missing.dl').exists()

        if bindir is not None:
            share = root / 'share'
            share.mkdir()
            installed = bindir.parent / 'share/lincupl/lincupl.dl'
            assert installed.read_bytes() == data, 'build did not use the default manifest'
            (share / 'lincupl.dl').write_bytes(data)
            explicit = root / 'models.dl'
            explicit.write_bytes(catalog.pack_library([dict(name=n, image=b) for n, b in models.items()]))
            env = dict(os.environ, LINCUPL_SHARE=str(share))
            env.pop('LIBCUPL', None)
            env.pop('FITTERDIR', None)

            def run(tool, args, cwd):
                exe = bindir / (tool + ('.exe' if os.name == 'nt' else ''))
                p = subprocess.run([str(exe), *map(str, args)], cwd=cwd, env=env,
                                   capture_output=True, text=True, timeout=20)
                assert p.returncode == 0, (tool, args, p.stdout, p.stderr)

            for entry in actual:
                device = entry['name']
                if device not in models:
                    continue
                loaded = root / (device + '.image')
                run('devlib_read_test', [installed, device, loaded], root)
                assert loaded.read_bytes() == entry['image'], device
                pins = catalog.records(entry['image'])[3][0][8]
                is_2500 = device.startswith('v2500')
                clock, data_pin, output_pin = ((1, 2, 5 if pins == 44 else 4) if is_2500 else
                                               (2, 3, 27) if pins == 28 else (1, 2, 23))
                body = (f'Name test; Device {device}; pin {clock}=clk; pin {data_pin}=a; '
                        f'pin {output_pin}=q; q.d=a;\n' + ('q.ck=clk;\n' if device.startswith('v') else ''))
                vectors = 'ORDER:clk,a,q;\nVECTORS:\nC0L\nC1H\n00H\nC0L\n'
                if device in v8_models:
                    h = v8_models[device]
                    p = h['package'].number
                    body = f'Name test; Device {device}; pin {p(2)}=a; pin {p(h["cells"][0]["pin"])}=q; '
                    if h['mode'] in ('automatic', 'registered'):
                        body += f'pin {p(1)}=clk; pin {p(h["enable_pin"])}=disable; q.d=a;'
                        vectors = 'ORDER:clk,a,disable,q;\nVECTORS:\nC00L\nC10H\n000H\nC00L\n'
                    else:
                        body += 'q=a;'
                        vectors = 'ORDER:a,q;\nVECTORS:\n0L\n1H\n0L\n'
                maps = []
                for label, options in [('default', []), ('explicit', ['-u', explicit])]:
                    work = root / f'{device}-{label}'
                    work.mkdir()
                    (work / 'test.pld').write_text(body)
                    run('cupl', ['-jaxm1', *options, 'test.pld'], work)
                    maps.append(fuses(work / 'test.jed'))
                    (work / 'test.si').write_text(vectors)
                    flags_to_test = ('-l',) if device in v8_models and h['power_down'] else ('-l', '-fl')
                    for flags in flags_to_test:
                        run('csim', [flags, *options, 'test'], work)
                assert maps[0] == maps[1], device
                if device.startswith('v750c'):
                    # Register aliases read raw Q before output polarity.
                    # Option bits and package changes cannot invert that path.
                    for kind in ('d', 't'):
                        work = root / f'{device}-negative-alias-{kind}'
                        work.mkdir()
                        (work / 'test.pld').write_text(
                            f'Name test; Device {device}; pin {clock}=clk; '
                            f'pin {data_pin}=a; pin {data_pin + 1}=b; '
                            f'pin {output_pin}=!r; pin {output_pin - 1}=y; '
                            f'pinnode {output_pin + 21}=q; r.{kind}=b; r.ck=clk; y=q&a;')
                        run('cupl', ['-jaxm1', 'test.pld'], work)
                        bits = fuses(work / 'test.jed')
                        assert bits[1170:1172] == [0, 1], device
                        state, lines = 0, []
                        for ck, a, b in [('0', 1, 0), ('C', 1, 0), ('C', 1, 1),
                                         ('0', 1, 0), ('C', 0, 1), ('C', 1, 1)]:
                            if ck == 'C':
                                state = b if kind == 'd' else state ^ b
                            levels = (1 - state, state & a, state)
                            lines.append(ck + str(a) + str(b) + ''.join('LH'[v] for v in levels))
                        (work / 'test.si').write_text('ORDER:clk,a,b,!r,y,q;\nVECTORS:\n' +
                                                      '\n'.join(lines) + '\n')
                        run('csim', ['-fl', 'test'], work)
    print('PASS: 51 default hardware interfaces, 609 unchanged fallbacks, metadata and selection checks')


if __name__ == '__main__':
    main()
