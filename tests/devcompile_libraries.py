#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Compare generated ATF750C data with installed libraries and the C reader.

The base and EXT+PPK entries must match after documented normalization.
The EXT and PPK entries have separate, explicit legacy feedback discrepancies;
the generated model must agree with its base architecture for those variants.
"""
import argparse
from collections import Counter
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
spec = importlib.util.spec_from_file_location('device_compiler', ROOT / 'tools/devcompile.py')
compiler = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compiler)
from devlang.compare import canonical, images, compare_libraries  # noqa: E402
from defects import fuses  # noqa: E402
from csim_jed import vector_lines  # noqa: E402
from devcatalog_libraries import check_connection_fuses  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('bin_dir', type=Path)
    parser.add_argument('--reference', type=Path)
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    bindir = args.bin_dir.resolve()
    candidates = [bindir.parent / 'share/lincupl/Atmel.dl',
                  Path(os.environ.get('LINCUPL_SHARE', '/nonexistent')) / 'Atmel.dl']
    reference = args.reference.resolve() if args.reference else next((p for p in candidates if p.is_file()), None)
    if reference is None:
        print('SKIP: device library missing')
        return 77
    data = compiler.compile_text((ROOT / 'devices/families/atf750c.dev').read_text(),
                                 devices=['v750c', 'v750cext', 'v750cppk', 'v750cextppk'])
    original_data = reference.read_bytes()
    comparison = compare_libraries(data, original_data)
    expected_sections = {'v750c': set(), 'v750cextppk': set(),
                         'v750cext': {'pins', 'nodes'}, 'v750cppk': {'pins'}}
    for name, result in comparison.items():
        assert {d['section'] for d in result['differences']} == expected_sections[name], result
        assert not result['generated_normalizations'], result
        for d in result['differences']:
            assert d['record_count_delta'] == 0
            assert d['different_bytes'] == (6 if name == 'v750cppk' else 20), d
    runs = 0
    same, different, diagnostics, variant_consistency = Counter(), Counter(), 0, 0
    mismatch_cases = []
    with tempfile.TemporaryDirectory(prefix='devcompile-library-') as tmp:
        root = Path(tmp)
        generated = root / 'generated.dl'
        generated.write_bytes(data)
        env = dict(os.environ, LINCUPL_SHARE=str(reference.parent))
        env.pop('LIBCUPL', None)
        env.pop('FITTERDIR', None)
        probes = root / 'connection-probes'
        probes.mkdir()
        connection_fuses = check_connection_fuses(reference, generated, bindir, probes)

        def run(tool, argv, directory, expected=0):
            nonlocal runs
            executable = bindir / (tool + ('.exe' if os.name == 'nt' else ''))
            p = subprocess.run([str(executable), *map(str, argv)], cwd=directory,
                               env=env, capture_output=True, text=True, timeout=20)
            runs += 1
            assert p.returncode == expected, (tool, argv, directory.name, p.returncode, p.stdout, p.stderr)
            return p.stdout + p.stderr

        # The comparison's repairs must agree with the actual shared C
        # reader. Generated records must need no repair at all.
        originals, emitted = images(original_data), images(data)
        for name in emitted:
            for label, lib, binary in [('original', reference, originals[name]),
                                        ('generated', generated, emitted[name])]:
                output = root / f'{name}-{label}.image'
                run('devlib_read_test', [lib, name, output], root)
                normalized = output.read_bytes()
                assert canonical(normalized, repairs=False)[0] == canonical(binary)[0]
                if label == 'generated':
                    assert normalized == binary, f'{name}: generated image requires runtime repair'

        # The mapping audit uses independently written JEDEC rows and an
        # independent register-state evaluator, not compiler-emitted maps.
        p = subprocess.run([sys.executable, str(ROOT / 'tests/atf750c_mapping.py'), str(bindir),
                            '--library', str(generated), '--skip-metadata'],
                           env=env, capture_output=True, text=True, timeout=120)
        assert p.returncode == 0, p.stdout + p.stderr
        independent_result = p.stdout.strip()

        for path in sorted((ROOT / 'tests/defects').glob('*.json')):
            case = json.loads(path.read_text())
            for number, variant in enumerate(case['variants']):
                device = variant.get('device', case.get('device', ''))
                if device not in emitted:
                    continue
                results = []
                for label, lib in [('original', reference), ('generated', generated)]:
                    directory = root / f'{path.stem}-{number}-{label}'
                    directory.mkdir()
                    (directory / 'test.pld').write_text(f'Name test; Device {device};\n' + variant['source'])
                    log = run('cupl', [variant.get('flags', '-jaxm1'), '-u', lib, 'test.pld'], directory,
                              1 if 'diagnostic' in variant else 0)
                    result = []
                    if 'diagnostic' in variant:
                        assert re.search(variant['diagnostic'], log), log
                    else:
                        result.append(fuses(directory / 'test.jed'))
                        if 'vectors' in variant:
                            (directory / 'test.si').write_text(variant['vectors'])
                            for flags in ('-l', '-fl'):
                                run('csim', [flags, '-u', lib, 'test'], directory)
                                result.append(vector_lines((directory / 'test.so').read_text()))
                    results.append(result)
                if 'diagnostic' in variant:
                    diagnostics += 1
                if results[0] == results[1]:
                    same[device] += 1
                else:
                    different[device] += 1
                    assert device in ('v750cext', 'v750cppk'), (path.name, number, device)
                    assert results[0][1:] == results[1][1:], 'unexpected simulation-vector change'
                    mismatch_cases.append(dict(case=path.stem, variant=number, device=device,
                                               changed_fuses=sum(a != b for a, b in zip(results[0][0], results[1][0]))))
                # Option bits cannot change register-feedback logic. For
                # each accepted variant design, compile it as the base
                # architecture and compare all other fuse positions.
                if results[1] and device != 'v750c':
                    directory = root / f'{path.stem}-{number}-base'
                    directory.mkdir()
                    (directory / 'test.pld').write_text('Name test; Device v750c;\n' + variant['source'])
                    run('cupl', [variant.get('flags', '-jaxm1'), '-u', generated, 'test.pld'], directory)
                    bits = fuses(directory / 'test.jed')
                    assert all(a == b for i, (a, b) in enumerate(zip(bits, results[1][0]))
                               if i not in (14499, 14500)), (path.name, number, device)
                    variant_consistency += 1

        # Include the generated fuse-plot VM and the pin diagram in the
        # comparison, not only the default compilation flags.
        documents = []
        for label, lib in [('original', reference), ('generated', generated)]:
            directory = root / f'plot-{label}'
            directory.mkdir()
            (directory / 'test.pld').write_text('Name test; Device v750c; Partno ABCDEFGH;\n'
                                               'pin 2=a; pin 23=q; q.d=a;\n')
            run('cupl', ['-jafxm1', '-u', lib, 'test.pld'], directory)
            doc = (directory / 'test.doc').read_text()
            assert 'Fuse Plot' in doc
            documents.append(doc[doc.index('Fuse Plot'):])
        assert documents[0] == documents[1], 'generated fuse plot or pin diagram differs'

    assert not different['v750c'] and not different['v750cextppk']
    assert different['v750cext'] > 0 and different['v750cppk'] > 0
    report = dict(reference=str(reference), binary_comparison=comparison,
                  equal_design_cases=dict(same), differing_design_cases=dict(different),
                  matching_diagnostic_cases=diagnostics, variant_consistency_checks=variant_consistency,
                  mismatches=mismatch_cases, compiler_simulator_reader_runs=runs,
                  independent_mapping_audit=independent_result, fuse_plot_equal=True)
    report['connection_fuses'] = connection_fuses
    if args.report:
        args.report.write_text(json.dumps(report, indent=2) + '\n')
    print('PASS: base and EXT+PPK canonical records, C reader agreement, independent feedback audit, '
          'fuse plot and option invariance')
    print(f'Design comparisons: {sum(same.values())} equal; {sum(different.values())} legacy variant '
          f'differences ({dict(different)}); {runs} tool runs')
    return 0


if __name__ == '__main__':
    sys.exit(main())
