#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Compile readable .dev descriptions into a standalone .dl library."""
import argparse
from contextlib import ExitStack
import json
from pathlib import Path
import sys

from devlang.syntax import Error, parse
from devlang.model import models
from devlang.binary import library
from devlang.compare import compare_libraries
from devlang import catalog


def compile_text(text, filename='<input>', devices=None):
    tree = parse(text, filename)
    if any('representation' in d.fields for d in tree.children):
        entries = catalog.compile_tree(tree)
        if devices:
            missing = set(devices) - {e['name'] for e in entries}
            if missing:
                raise Error(f'unknown device selection: {", ".join(sorted(missing))}')
            entries = [e for e in entries if e['name'] in devices]
        return catalog.pack_library(entries)
    descriptions = models(tree)
    if devices:
        available = {name for m in descriptions for name in m['variants']}
        missing = set(devices) - available
        if missing:
            raise Error(f'unknown device selection: {", ".join(sorted(missing))}')
        for m in descriptions:
            m['variants'] = {name: options for name, options in m['variants'].items() if name in devices}
    return library(descriptions)


def directory_entries(source, selected, depth=0):
    """Compile ordered catalogs, preserving first-match precedence in sets."""
    if depth > 16:
        raise Error('catalog nesting exceeds 16')
    manifest = json.loads((source / 'manifest.json').read_text(encoding='utf-8'))
    if not isinstance(manifest, dict):
        raise Error('unsupported catalog manifest')
    if manifest.get('format') == 'lincupl-device-catalog-set-1':
        children = manifest.get('catalogs')
        if not isinstance(children, list) or not children:
            raise Error('catalog set requires a nonempty catalogs list')
        entries, seen = [], set()
        for child in children:
            if not isinstance(child, str):
                raise Error('catalog name must be a directory name')
            path = source / child
            if path.resolve().parent != source.resolve() or not path.is_dir():
                raise Error('catalog must be a child directory')
            for entry in directory_entries(path, selected, depth + 1):
                if entry['name'] not in seen:
                    entries.append(entry)
                    seen.add(entry['name'])
        return entries
    if manifest.get('format') != 'lincupl-device-catalog-1':
        raise Error('unsupported catalog manifest')
    if not isinstance(manifest.get('entries'), list) or not manifest['entries']:
        raise Error('catalog manifest requires a nonempty entries list')
    entries = []
    for item in manifest['entries']:
        if (not isinstance(item, dict) or not isinstance(item.get('name'), str) or
                not isinstance(item.get('file'), str) or 'flags' not in item):
            raise Error('catalog entry requires name, file and flags')
        if selected and item['name'] not in selected:
            continue
        path = source / item['file']
        if path.resolve().parent != source.resolve() or path.suffix != '.dev':
            raise Error('catalog file must be a .dev file inside its directory')
        compiled = catalog.compile_text(path.read_text(encoding='utf-8'), str(path))
        if len(compiled) != 1 or compiled[0]['name'] != item['name']:
            raise Error(f'{path}: mnemonic does not match catalog')
        flags = item['flags']
        if type(flags) is not int or not 0 < flags <= 255 or not flags & 1:
            raise Error(f'{path}: invalid directory flags')
        compiled[0]['flags'] = flags
        entries.extend(compiled)
    return entries


def compile_directory(source, devices=None):
    """The manifest preserves native lookup order and duplicate mnemonics."""
    selected = set(devices or [])
    entries = directory_entries(source, selected)
    missing = selected - {e['name'] for e in entries}
    if missing:
        raise Error(f'unknown device selection: {", ".join(sorted(missing))}')
    return catalog.pack_library(entries)


def write_outputs(outputs):
    """Create all destinations exclusively, removing new files on failure."""
    created = []
    try:
        with ExitStack() as stack:
            streams = []
            for path, data in outputs:
                stream = stack.enter_context(path.open('xb'))
                created.append(path)
                streams.append((stream, data))
            for stream, data in streams:
                stream.write(data)
    except BaseException:
        for path in created:
            path.unlink()
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path, help='.dev file or device catalog directory')
    parser.add_argument('-o', '--output', required=True, type=Path)
    parser.add_argument('--device', action='append', help='emit only this mnemonic (repeatable; default: all)')
    parser.add_argument('--compare', type=Path, help='compare generated devices against this DL library')
    parser.add_argument('--report', type=Path, help='write the binary comparison as JSON (requires --compare)')
    args = parser.parse_args()
    if args.report and not args.compare:
        parser.error('--report requires --compare')
    try:
        imported = args.source.is_dir()
        if imported:
            data = compile_directory(args.source, args.device)
        else:
            source = args.source.read_text(encoding='utf-8')
            imported = any('representation' in d.fields for d in parse(source).children)
            data = compile_text(source, str(args.source), args.device)
        compare = catalog.compare_libraries if imported else compare_libraries
        report = compare(data, args.compare.read_bytes()) if args.compare else None
        # Validate before opening the destination, and never truncate a
        # source library or previous output by accident.
        outputs = [(args.output, data)]
        if args.report:
            outputs.append((args.report, (json.dumps(report, indent=2) + '\n').encode('utf-8')))
        write_outputs(outputs)
        print(f'{args.output}: {len(data)} bytes')
        if report is not None:
            for name, result in report.items():
                print(f'{name}: raw bytes {"identical" if result["raw_equal"] else "differ"}; '
                      f'canonical records {"match" if result["canonical_equal"] else "DIFFER"}')
            if not all(r['canonical_equal'] for r in report.values()):
                return 1
    except (Error, OSError, UnicodeError, KeyError, TypeError, json.JSONDecodeError) as exc:
        print(f'devcompile: {exc}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
