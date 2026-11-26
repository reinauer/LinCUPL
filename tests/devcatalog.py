#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Exercise catalog compilation, source edits, lookup and malformed input."""
import json
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
from devlang.syntax import Error, parse


def rejected(call):
    try:
        call()
    except (Error, ValueError, UnicodeError):
        return
    raise AssertionError('invalid input was accepted')


def main():
    # A missing mapped field must not become an implicit exclusion. The
    # field removed here previously changed real clock-selection fuses.
    validate_coverage(catalog.FIELDS, binary.WIDTHS)
    for section, field in [(10, 'config_value'), (3, 'pla_options')]:
        missing = dict(catalog.FIELDS)
        missing[section] = [f for f in missing[section] if f.name != field]
        rejected(lambda: validate_coverage(missing, binary.WIDTHS))
    for section, field in [(7, catalog.F('overlap_unknown', 26)),
                           (10, catalog.F('overlap_value', 12)),
                           (10, catalog.F('outside_record', 13))]:
        overlapping = dict(catalog.FIELDS)
        overlapping[section] = [*overlapping[section], field]
        rejected(lambda: validate_coverage(overlapping, binary.WIDTHS))
    missing = {s: fields for s, fields in catalog.FIELDS.items() if s != 9}
    rejected(lambda: validate_coverage(missing, binary.WIDTHS))

    text = (ROOT / 'devices/Atmel/v750c.dev').read_text(encoding='utf-8')
    item, = catalog.compile_text(text)
    assert devcompile.compile_text(text) == catalog.pack_library([item])
    edited = text.replace('ground = 12', 'ground = 11')
    modified, = catalog.compile_text(edited)
    assert not catalog.compare_image(modified['image'], item['image'])['canonical_equal']
    assert catalog.records(modified['image'])[3][0][2] == 11
    assert catalog.records(item['image'])[3][0][2] == 12

    # Preserve inactive operands and configuration fields from the source.
    # Edits are made in the description, with byte positions checked separately.
    quirks_text = text.replace('address_bits = 0', 'address_bits = 7')
    quirks_text = quirks_text.replace('data_width = 10', 'data_width = 19')
    quirks_text = quirks_text.replace('row(1, 1, none, clock_input, default, 83, 82, 0, 0, 0)',
                                    'row(1, 1, none, clock_input, default, 83, 82, 3, 0, 12345)')
    quirks_text = quirks_text.replace('row(1, 0, array,', 'row(1, 42, array,', 1)
    quirks_text = quirks_text.replace('row(3, 23, ck, 1, 1, 0, yes, 0)',
                                    'row(3, 23, ck, 1, 1, 0, yes, 165)', 1)
    imported, = catalog.compile_text(quirks_text)
    quirks = imported['image']
    sections = catalog.records(quirks)
    assert sections[3][0][9:11] == bytes([7, 19])
    assert sections[6][0][10:16] == struct.pack('<BBI', 3, 0, 12345)
    assert sections[7][0][2:4] == struct.pack('<H', 42)
    assert sections[10][0][12] == 165
    assert sections[11] == [bytes(24)]
    assert catalog.compare_image(quirks, item['image'])['differences']

    # A connection value must be compared independently of the schema.
    changed_pair = bytearray(quirks)
    pair_offset = struct.unpack_from('<I', quirks, 10 * 20 + 8)[0]
    changed_pair[pair_offset + 12] ^= 1
    assert not catalog.compare_image(changed_pair, quirks)['canonical_equal']

    # Quoted fitter text survives escapes, comments and one-byte characters.
    arguments = 'tool "quoted" \\path # literal \u00e9'
    strings_text = quirks_text.replace('arguments = ""', 'arguments = ' + json.dumps(arguments))
    strings_text = strings_text.replace('pla_options = []',
        'pla_options = [tool_header, jedec_keyword, copy_tt2, omit_pin_zero, short_labels]')
    compiled, = catalog.compile_text(strings_text)
    strings = compiled['image']
    section = catalog.records(strings)[3][0]
    assert section[80:112] == arguments.encode('latin1').ljust(32, b'\0')
    assert section[112:132] == b'TJ2ps'.ljust(20, b'\0')
    missing_options = bytearray(strings)
    attr_offset = struct.unpack_from('<I', strings, 3 * 20 + 8)[0]
    missing_options[attr_offset + 112:attr_offset + 132] = bytes(20)
    assert not catalog.compare_image(missing_options, strings)['canonical_equal']
    assert devcompile.compile_text(strings_text) == catalog.pack_library([compiled])

    for before, after in [
        ('format = 1', 'format = 2'), ('representation = library', 'representation = wrong'),
        ('device v750c', 'device v750c : base'), ('ground = 12', 'ground = -1'),
        ('ground = 12', 'ground = 256'), ('ground = 12', 'ground = "12"'),
        ('ground = 12', 'ground = unresolved'), ('ground = 12', 'ground = yes'),
        ('ground = 12', 'missing = 12'), ('ground = 12', ''),
        ('revision = 2', 'revision = 0'), ('power_up = low', 'power_up = nonsense'),
        ('name = ""', 'name = "abcdefghijklmn"'), ('name = ""', 'name = "\\u0000"'),
        ('name = ""', 'name = "\\u1000"'), ('name = ""', 'name = "\\q"'),
        ('columns = [name, function]', 'columns = [name, name]'),
        ('columns = [name, function]', 'columns = [name, invalid]'),
        ('row("d", d)', 'row("d")'), ('row("d", d)', 'row("d", d, 1)'),
        ('row("d", d)', 'row("d", invalid)'), ('row("d", d)', 'wrong("d", d)'),
        ('row("d", d)', '["d", d]'), ('rows = [', 'rows = wrong('),
        ('extensions {', 'extensions named {'), ('extensions {', 'invalid {'),
    ]:
        assert before in text, before
        rejected(lambda: catalog.compile_text(text.replace(before, after, 1)))
    rejected(lambda: parse('format = 1\n# comment\nx = "unterminated'))
    rejected(lambda: devcompile.compile_text(text, devices=['absent']))

    # Directory order, duplicate mnemonics and source flags must survive.
    duplicates = catalog.pack_library([dict(item, flags=3), dict(item, image=modified['image'])])
    assert len(catalog.entries(duplicates)) == 2
    with tempfile.TemporaryDirectory(prefix='devcatalog-') as temp:
        root = Path(temp)
        destination = root / 'catalog'
        destination.mkdir()
        (destination / 'v750c.dev').write_text(text, encoding='utf-8')
        (destination / 'v750c__2.dev').write_text(edited, encoding='utf-8')
        manifest_path = destination / 'manifest.json'
        manifest = dict(format='lincupl-device-catalog-1', entries=[
            dict(name='v750c', file='v750c.dev', flags=3),
            dict(name='v750c', file='v750c__2.dev', flags=1)])
        manifest_path.write_text(json.dumps(manifest))
        # This directory contains only descriptions and a manifest.
        assert devcompile.compile_directory(destination) == duplicates
        root_manifest = root / 'manifest.json'
        root_manifest.write_text(json.dumps(dict(format='lincupl-device-catalog-set-1', catalogs=['catalog'])))
        first, = catalog.entries(devcompile.compile_directory(root))
        assert first['flags'] == 3 and first['image'] == item['image']
        for children in ([], None, {}, [1], ['..'], ['.'], ['absent']):
            root_manifest.write_text(json.dumps(dict(format='lincupl-device-catalog-set-1', catalogs=children)))
            rejected(lambda: devcompile.compile_directory(root))
        results = catalog.compare_libraries(duplicates, duplicates)
        assert set(results) == {'v750c', 'v750c#2'}
        assert all(r['canonical_equal'] for r in results.values())
        rejected(lambda: catalog.compare_libraries(duplicates, catalog.pack_library([item])))
        for malformed in ([], None, {'format': 'wrong'},
                          dict(manifest, entries=None), dict(manifest, entries=[]),
                          dict(manifest, entries={}), dict(manifest, entries=[[]]),
                          dict(manifest, entries=[{'name': 'v750c', 'file': 'v750c.dev'}]),
                          dict(manifest, entries=[dict(manifest['entries'][0], name=1)]),
                          dict(manifest, entries=[dict(manifest['entries'][0], file=None)])):
            manifest_path.write_text(json.dumps(malformed))
            rejected(lambda: devcompile.compile_directory(destination))
            p = subprocess.run([sys.executable, str(ROOT / 'tools/devcompile.py'),
                                str(destination), '-o', str(root / 'invalid.dl')],
                               capture_output=True, text=True, timeout=10)
            assert p.returncode == 1 and 'Traceback' not in p.stderr, p.stderr
            assert not (root / 'invalid.dl').exists()
        manifest['entries'][0]['file'] = '../escape.dev'
        manifest_path.write_text(json.dumps(manifest))
        rejected(lambda: devcompile.compile_directory(destination))

        broken = bytearray(duplicates)
        struct.pack_into('<I', broken, binary.HEADER_SIZE + 22, len(broken))
        rejected(lambda: catalog.entries(broken))

    for invalid in (b'', b'\0' * binary.DATA_START, duplicates[:binary.DATA_START - 1]):
        rejected(lambda: catalog.entries(invalid))
    bad = bytearray(item['image'])
    struct.pack_into('<H', bad, 6 * 20 + 6, 15)
    rejected(lambda: catalog.canonical(bad))
    struct.pack_into('<H', bad, 6 * 20 + 6, 16)
    struct.pack_into('<I', bad, 6 * 20 + 8, 260)
    rejected(lambda: catalog.canonical(bad))
    print('PASS: reviewed field coverage, independent compilation, '
          'catalog edits, strings, duplicate ordering, bounds and output protection')


if __name__ == '__main__':
    main()
