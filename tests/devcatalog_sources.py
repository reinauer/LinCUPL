#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Compile every checked-in catalog entry without a reference library."""
from collections import Counter
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import devcompile
from devlang import catalog
from devlang.audit import RESERVED_ZERO_RANGES


def main():
    names = set()
    expected = {}
    for name, count in [('Atmel', 85), ('cupl', 624)]:
        source = ROOT / 'devices' / name
        manifest = json.loads((source / 'manifest.json').read_text(encoding='utf-8'))
        declared = manifest['entries']
        assert len(declared) == count
        assert {p.name for p in source.glob('*.dev')} == {e['file'] for e in declared}
        assert len({e['file'] for e in declared}) == count
        data = devcompile.compile_directory(source)
        assert data[4:21].rstrip(b'\0') == b'LinCUPL'
        assert data[21:38].rstrip(b'\0') == b'v1.0'
        assert data[38:199].rstrip(b'\0') == b'LinCUPL device descriptions'
        entries = catalog.entries(data)
        assert len(entries) == count
        assert [(e['name'], e['flags']) for e in entries] == [
            (e['name'], e['flags']) for e in declared]
        for entry in entries:
            _, omitted = catalog.canonical(entry['image'])
            assert not any(omitted.values()), entry['name']
            sections = catalog.records(entry['image'])
            for section, ranges in RESERVED_ZERO_RANGES.items():
                for row in sections[section]:
                    for start, width in ranges:
                        assert row[start:start + width] == bytes(width), (entry['name'], section, start)
            expected.setdefault(entry['name'], entry)
        occurrences = Counter(e['name'] for e in entries)
        assert {n: c for n, c in occurrences.items() if c > 1} == (
            {'p16l8': 2} if name == 'cupl' else {})
        names.update(occurrences)
        print(f'{name}: {count} checked-in descriptions compiled', flush=True)
    assert len(names) == 660
    merged = catalog.entries(devcompile.compile_directory(ROOT / 'devices', hardware=False))
    assert [e['name'] for e in merged] == list(expected)
    for entry in merged:
        first = expected[entry['name']]
        assert (entry['flags'], entry['image']) == (first['flags'], first['image'])
    selected = catalog.entries(devcompile.compile_directory(
        ROOT / 'devices', ['v750c', 'p16l8'], hardware=False))
    assert {e['name'] for e in selected} == {'v750c', 'p16l8'}
    assert all(e['image'] == expected[e['name']]['image'] for e in selected)
    # These values are not reconstructible from the fuse row width or
    # package pin count. Keep direct byte checks independent of the schema.
    attrs = catalog.records(expected['f506']['image'])[3][0]
    assert [attrs[b] for b in (4, 5, 7, 20)] == [17, 0, 0, 1]
    attrs = catalog.records(expected['v750c']['image'])[3][0]
    assert [attrs[b] for b in (4, 5, 7, 20)] == [0, 1, 1, 1]
    nodes = catalog.records(expected['p10l8']['image'])[7]
    assert {(n[26], n[27]) for n in nodes} == {(32, 20), (0, 0)}
    print('PASS: all 709 descriptions compile without reference libraries; 660 distinct mnemonics')


if __name__ == '__main__':
    main()
