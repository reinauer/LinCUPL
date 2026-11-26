#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Compare checked-in descriptions with reference libraries and native readers."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import devcompile
from devlang import catalog
from defects import fuses


def check_connection_fuses(source, rebuilt, bindir, root):
    """Check source-dependent selection through the native JEDEC writer."""
    available = {e['name'] for e in catalog.entries(source.read_bytes())}
    available &= {e['name'] for e in catalog.entries(rebuilt.read_bytes())}
    results = []
    exe = bindir / ('cupl.exe' if os.name == 'nt' else 'cupl')
    env = dict(os.environ, LINCUPL_SHARE=str(source.parent))
    env.pop('LIBCUPL', None)
    env.pop('FITTERDIR', None)
    for device, expression, fuse, value in [
            ('v750c', 'q.ckmux=clk;', 14395, 1),
            ('v750c', 'q.ck=a;', 14395, 0),
            ('v750b', 'q.ckmux=clk;', 14395, 1),
            ('p22v10s', 'q.ckmux=!clk;', 6358, 1),
            ('p22v10s', 'q.ckmux=clk;', 6358, 0)]:
        if device not in available:
            continue
        maps = []
        for label, library in [('source', source), ('rebuilt', rebuilt)]:
            directory = root / f'{device}-{len(results)}-{label}'
            directory.mkdir()
            (directory / 'test.pld').write_text(
                f'Name test; Device {device}; pin 1=clk; pin 2=a; pin 23=q; '
                f'q.d=a; {expression}')
            p = subprocess.run([str(exe), '-jam1', '-u', str(library), 'test.pld'],
                               cwd=directory, env=env, capture_output=True, text=True, timeout=10)
            assert p.returncode == 0, (device, label, p.stdout, p.stderr)
            bits = fuses(directory / 'test.jed')
            assert bits[fuse] == value, (device, label, expression, fuse, bits[fuse], value)
            maps.append(bits)
        assert maps[0] == maps[1], (device, expression, 'connection selection changed fuse map')
        results.append(dict(device=device, expression=expression, fuse=fuse, value=value))
    assert results, 'no connection-selection regression was exercised'
    return results


def check_fitter_output(source, rebuilt, bindir, root):
    """Exercise format options through the real PLA writer, not the schema.

    Use one compilable example for each available format/option sequence.
    The -b option emits fitter input without invoking an external fitter.
    """
    tested, results = set(), []
    env = dict(os.environ, LINCUPL_SHARE=str(source.parent))
    env.pop('LIBCUPL', None)
    env.pop('FITTERDIR', None)
    for entry in catalog.entries(source.read_bytes()):
        sections = catalog.records(entry['image'])
        attr = sections[3][0]
        options = bytes(attr[112:132]).split(b'\0')[0]
        key = (attr[18], options)
        if not attr[18] or key in tested:
            continue
        pins = [(int.from_bytes(p[2:4], 'little'), p[5]) for p in sections[6]
                if p[4] == 0 and 0 < int.from_bytes(p[2:4], 'little') <= attr[8]
                and p[5] in (0, 7, 11, 12, 13, 14, 16, 21, 22, 23)]
        outputs = [n for n, kind in pins if kind in (7, 11, 16, 23)]
        if not outputs:
            continue
        output_pin = outputs[0]
        inputs = [n for n, _ in pins if n != output_pin]
        if not inputs:
            continue
        name = entry['name']
        statuses, files = [], []
        for label, library in [('source', source), ('rebuilt', rebuilt)]:
            directory = root / f'{entry["index"]}-{label}'
            directory.mkdir()
            (directory / 'test.pld').write_text(
                f'Name test; Device {name}; pin {inputs[0]}=a; pin {output_pin}=y; y=a;')
            exe = bindir / ('cupl.exe' if os.name == 'nt' else 'cupl')
            p = subprocess.run([str(exe), '-bam1', '-u', str(library), 'test.pld'],
                               cwd=directory, env=env, capture_output=True, text=True, timeout=10)
            statuses.append(p.returncode)
            files.append({f.suffix: re.sub(r'^#\s+Created.*$', '', f.read_text(), flags=re.M)
                          for f in directory.iterdir() if f.suffix in ('.pla', '.tt2')})
        assert statuses[0] >= 0 and statuses[0] == statuses[1], (name, statuses)
        assert files[0] == files[1], f'{name}: compiled catalog changed fitter input'
        if statuses[0] == 0 and '.pla' in files[0]:
            if b'T' in options:
                assert 'TOOL CUPL' in files[0]['.pla'], name
            if b'2' in options:
                assert '.tt2' in files[0], name
            tested.add(key)
            results.append(dict(device=name, input_format=key[0], options=options.decode('ascii'),
                                files=sorted(files[0])))
    assert results, 'no fitter-format regression was exercised'
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('bin_dir', type=Path)
    parser.add_argument('--share', type=Path)
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    bindir = args.bin_dir.resolve()
    share = (args.share or bindir.parent / 'share/lincupl').resolve()
    paths = [share / name for name in ('Atmel.dl', 'cupl.dl')]
    if not all(p.is_file() for p in paths):
        print('SKIP: Atmel.dl and cupl.dl references are required')
        return 77
    reader = bindir / ('devlib_read_test.exe' if os.name == 'nt' else 'devlib_read_test')
    reports = []
    with tempfile.TemporaryDirectory(prefix='devcatalog-libraries-') as temp:
        root = Path(temp)
        for name in ('Atmel', 'cupl'):
            (root / f'{name}.dl').write_bytes(devcompile.compile_directory(ROOT / 'devices' / name))
        (root / 'lincupl.dl').write_bytes(devcompile.compile_directory(ROOT / 'devices'))
        # Build an independent combined reference with the native merger.
        cbld = bindir / ('cbld.exe' if os.name == 'nt' else 'cbld')
        reference = root / 'reference/lincupl.dl'
        p = subprocess.run(['cmake', f'-DCBLD={cbld}', f'-DATMEL={paths[0]}',
                            f'-DCUPL={paths[1]}', f'-DOUT={reference}',
                            '-P', str(ROOT / 'cmake/make_lincupl_dl.cmake')],
                           capture_output=True, text=True, timeout=30)
        assert p.returncode == 0, (p.stdout, p.stderr)
        paths.append(reference)
        for source in paths:
            rebuilt = root / source.name
            original_entries = catalog.entries(source.read_bytes())
            compiled_entries = catalog.entries(rebuilt.read_bytes())
            assert len(original_entries) == len(compiled_entries), source
            comparisons = catalog.compare_libraries(rebuilt.read_bytes(), source.read_bytes())
            assert all(r['canonical_equal'] for r in comparisons.values()), source
            report = dict(library=source.name, entries=len(compiled_entries),
                          records=sum(r['records'] for r in comparisons.values()))
            changes = 0
            for original, compiled in zip(original_entries, compiled_entries):
                assert original['name'] == compiled['name'] and original['flags'] == compiled['flags']
                # All connection bytes must survive, even if the schema
                # accidentally omits a field and comparison masks it out.
                assert catalog.records(original['image'])[10] == catalog.records(compiled['image'])[10]
                normalized = []
                for label, lib, entry in [('original', source, original), ('compiled', rebuilt, compiled)]:
                    path = root / f'{label}.image'
                    p = subprocess.run([str(reader), str(lib), '--index', str(entry['index']), str(path)],
                                       capture_output=True, text=True, timeout=10)
                    assert p.returncode == 0, (source, entry['name'], p.stdout, p.stderr)
                    image = path.read_bytes()
                    normalized.append(image)
                    if label == 'original' and image != entry['image']:
                        changes += 1
                result = catalog.compare_image(normalized[1], normalized[0])
                assert result['canonical_equal'], (source, original['name'], result)
            report = {k: v for k, v in report.items() if k != 'devices'}
            report.update(native_reader_matches=len(original_entries), entries_repaired_by_reader=changes)
            probes = root / f'{source.stem}-connection-probes'
            probes.mkdir()
            report['connection_fuses'] = check_connection_fuses(source, rebuilt, bindir, probes)
            if source.name == 'lincupl.dl':
                probes = root / 'fitter-probes'
                probes.mkdir()
                report['fitter_outputs'] = check_fitter_output(source, rebuilt, bindir, probes)
            reports.append(report)
            print(f'{source.name}: {report["entries"]} descriptions, {report["records"]} records; '
                  f'all raw-field and native-reader comparisons pass', flush=True)
    if args.report:
        args.report.write_text(json.dumps(reports, indent=2) + '\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
