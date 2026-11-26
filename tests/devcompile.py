#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Device syntax, semantic validation, emission and comparison regressions."""
import copy
import importlib.util
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
# The test and CLI share a filename; load the CLI by its exact path.
spec = importlib.util.spec_from_file_location('device_compiler', ROOT / 'tools/devcompile.py')
compiler = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compiler)
from devlang.syntax import Error, parse, evaluate, symbol  # noqa: E402
from devlang.model import models  # noqa: E402
from devlang.binary import image  # noqa: E402
from devlang.compare import images, records, canonical, compare_libraries  # noqa: E402


def main():
    source = (ROOT / 'devices/families/atf750c.dev').read_text()
    compiled = compiler.compile_text(source)
    devices = images(compiled)
    dip_names = {'v750c', 'v750cext', 'v750cppk', 'v750cextppk'}
    assert set(devices) == dip_names | {name + 'lcc' for name in dip_names}
    assert compiled == compiler.compile_text(source), 'output must be deterministic'
    assert set(images(compiler.compile_text(source, devices=['v750c']))) == {'v750c'}
    assert evaluate(symbol('2 + 3 * 4')) == 14
    assert evaluate(symbol('(2 + 3) * 4')) == 20
    assert evaluate(symbol('[1..3, 5]')) == [1, 2, 3, 5]
    # Abstract families share the same inheritance rules but emit no entry.
    abstract = source.replace('device ATF750C {', 'family core {', 1)
    abstract = abstract.replace('device ATF750C_PLCC28 : ATF750C',
                                'device ATF750C_PLCC28 : core')
    abstract += '\ndevice ATF750C_DIP24 : core {}\n'
    assert images(compiler.compile_text(abstract)) == devices

    mutations = [
        ('format = 1', 'format = 2'),
        ('format = 1', 'format = 1\nformat = 1'),
        ('pins = 24', 'pins = 24\nunknown = 1'),
        ('architecture = atf750c', 'architecture = gal22v10'),
        ('package = DIP24', 'package = PLCC28'),
        ('ground = 12', 'ground = 1'),
        ('pins = 24', 'pins = 25'),
        ('fuses = 14504', 'fuses = 14505'),
        ('rows = 171', 'rows = 172'),
        ('connected = 0', 'connected = 1'),
        ('byte_order = text_order', 'byte_order = reverse'),
        ('first_fuse = 14435', 'first_fuse = 14434'),
        ('fuse = 14499', 'fuse = 14500'),
        ('positions = [14394, 14501..14503]', 'positions = [14394, 14501..14502]'),
        ('positions = [14394, 14501..14503]', 'positions = [14394, 14394, 14501..14503]'),
        ('active = high', 'active = low'),
        ('default = disabled', 'default = missing'),
        ('high = 83', 'high = 84'),
        ('high = 83', 'high = 82'),
        ('terms / 2', 'terms / 0'),
        ('terms / 2', 'terms / 3'),
        ('first_row = 13', 'first_row = 0'),
        ('output_fuses = 14367', 'output_fuses = 14364'),
        ('register_fuses = 14399', 'register_fuses = 14395'),
        ('pin = 22', 'pin = 23'),
        ('alias = node(pin + 21)', 'alias = node(pin + 11)'),
        ('source = primary.q', 'source = pin_level(pin)'),
        ('enable = output_enable', 'enable = unknown'),
        ('edge = rising', 'edge = falling'),
        ('d = 0', 'd = 1'),
        ('data = term_routing.primary', 'data = term_routing.secondary'),
        ('primary = sum(first_half, second_half)', 'primary = sum(second_half)'),
        ('combinational = term_routing.primary', 'combinational = secondary.q'),
        ('power_up = low', 'power_up = high'),
        ('variant v750cext    { power_down = pin_controlled }',
         'variant v750cext    { power_dwon = pin_controlled }'),
        ('macrocell output23 : dual_register', 'macrocell output23 : missing'),
        ('terms = 8', 'terms = missing'),
        ('terms = 8', 'terms = 999999999999999999999'),
        ('format = 1', 'format = __import__(os)'),
        ('power {', 'power unexpected {'),
        ('format = 1', 'format = 1\n@'),
        ('physical = [2..7, 9..14, 16..21, 23..28]',
         'physical = [1..7, 9..14, 16..21, 23..28]'),
        ('physical = [2..7, 9..14, 16..21, 23..28]',
         'physical = [2..7, 9..14, 16..21, 23..27, 27]'),
        ('unconnected = [1, 8, 15, 22]', 'unconnected = [1, 8, 15]'),
        ('device ATF750C_PLCC28 : ATF750C', 'device ATF750C_PLCC28 : missing'),
        ('device ATF750C_PLCC28 : ATF750C', 'device ATF750C_PLCC28 : ATF750C_PLCC28'),
    ]
    rejected = 0
    for before, after in mutations:
        assert before in source, before
        try:
            compiler.compile_text(source.replace(before, after, 1))
        except Error:
            rejected += 1
        else:
            raise AssertionError(f'accepted invalid description: {before} -> {after}')
    for invalid in [source[:-2], source + 'trailing', 'format = 1', source.replace('}', '', 1),
                    source + '\nfamily Unused : Missing {}\n',
                    source + '\nfamily UnusedA : UnusedB {}\nfamily UnusedB : UnusedA {}\n']:
        try:
            compiler.compile_text(invalid)
        except Error:
            rejected += 1
        else:
            raise AssertionError('accepted malformed description or inheritance graph')

    gal_source = (ROOT / 'devices/families/gal22v10.dev').read_text()
    for extra in ('setting ignored { nonsense = 123 }',
                  'register ignored { nonsense = 123 }'):
        changed = gal_source.replace('setting mode {', extra + '\nsetting mode {', 1)
        try:
            compiler.compile_text(changed)
        except Error:
            rejected += 1
        else:
            raise AssertionError(f'accepted an unconsumed 22V10 template block: {extra}')

    # A meaningful source edit must affect the emitted library. Swap two
    # input pairs while retaining complete, non-overlapping geometry.
    changed = source.replace('pin 2  { high = 75 low = 74 }', 'pin 2  { high = 67 low = 66 }')
    changed = changed.replace('pin 3  { high = 67 low = 66 }', 'pin 3  { high = 75 low = 74 }')
    modified = compiler.compile_text(changed)
    assert modified != compiled
    for name, data in images(modified).items():
        s = records(data)
        assert s[6][1][7:9] == bytes([67, 66])
        differences = compare_libraries(modified, compiled)[name]['differences']
        assert {d['section'] for d in differences} == {'pins'}
    # A source catalog can repeat mnemonics. Match native first-entry
    # lookup even when a later duplicate contains different hardware.
    from devlang import catalog
    entries = catalog.entries(compiled)
    duplicate = dict(entries[0], image=images(modified)[entries[0]['name']])
    reference = catalog.pack_library([*entries, duplicate])
    assert all(r['canonical_equal'] for r in compare_libraries(compiled, reference).values())
    # Wrong fuse coordinates or selector encodings must fail comparison;
    # there is no broad mask over unknown records or configuration bytes.
    model = models(parse(source))[0]
    from devlang.binary import sections
    s = sections(model, (0, 0))
    for section, row, byte in [(6, 1, 7), (7, 0, 10), (7, 3, 7), (8, 5, 23),
                               (9, 1, 8), (10, 0, 5), (10, 0, 12)]:
        mutated = copy.deepcopy(s)
        r = bytearray(mutated[section][row]); r[byte] ^= 1; mutated[section][row] = bytes(r)
        assert canonical(image(mutated), repairs=False)[0] != canonical(image(s), repairs=False)[0]

    # Package instantiation changes physical pin identities and node aliases,
    # never the silicon's fuse addresses or its product rows.
    pin_map = dict(zip(range(1, 25), [*range(2, 8), *range(9, 15),
                                     *range(16, 22), *range(23, 29)]))
    def mapped(number):
        return pin_map.get(number, number + 4 if number > 24 else number)
    for name in dip_names:
        dip, plcc = records(devices[name]), records(devices[name + 'lcc'])
        assert (plcc[3][0][2], plcc[3][0][3], plcc[3][0][8]) == (14, 28, 28)
        for section in (6, 7, 10):
            assert len(plcc[section]) == len(dip[section]) + (4 if section == 6 else 0)
            for x, y in zip(dip[section], plcc[section]):
                expected = bytearray(x)
                struct.pack_into('<H', expected, 2, mapped(struct.unpack_from('<H', x, 2)[0]))
                assert y == expected
        assert [struct.unpack_from('<H', r, 2)[0] for r in plcc[6][-4:]] == [1, 8, 15, 22]
        assert all(r[5] == 19 for r in plcc[6][-4:])
        assert dip[9] == plcc[9]

    with tempfile.TemporaryDirectory(prefix='devcompile-syntax-') as tmp:
        tmp = Path(tmp)
        input_path, output_path = tmp / 'chip.dev', tmp / 'chip.dl'
        input_path.write_text(source)
        command = [sys.executable, str(ROOT / 'tools/devcompile.py'), str(input_path), '-o', str(output_path)]
        result = subprocess.run(command, capture_output=True, text=True, timeout=10)
        assert result.returncode == 0, result.stderr
        assert output_path.read_bytes() == compiled
        result = subprocess.run(command, capture_output=True, text=True, timeout=10)
        assert result.returncode == 1 and output_path.read_bytes() == compiled
        # Reserve both destinations before publishing either. A report
        # collision, bad parent or identical output path must leave no DL.
        fresh, report = tmp / 'fresh.dl', tmp / 'report.json'
        report.write_text('keep')
        compare = command[:-1] + [str(fresh), '--compare', str(output_path)]
        for destination in (report, tmp / 'missing/report.json', fresh):
            result = subprocess.run(compare + ['--report', str(destination)],
                                    capture_output=True, text=True, timeout=10)
            assert result.returncode == 1 and not fresh.exists(), result
            assert report.read_text() == 'keep'
        new_report = tmp / 'new-report.json'
        result = subprocess.run(compare + ['--report', str(new_report)],
                                capture_output=True, text=True, timeout=10)
        assert result.returncode == 0, result.stderr
        assert fresh.read_bytes() == compiled and new_report.is_file()
        input_path.write_text('format = 1\ndevice broken {')
        result = subprocess.run(command, capture_output=True, text=True, timeout=10)
        assert result.returncode == 1 and ':2:' in result.stderr
        assert output_path.read_bytes() == compiled
        result = subprocess.run(command[:-1] + [str(input_path)], capture_output=True, text=True, timeout=10)
        assert result.returncode == 1 and input_path.read_text() == 'format = 1\ndevice broken {'
    print(f'PASS: deterministic compilation, {rejected} rejected descriptions, source-edit propagation, '
          'binary mismatch detection and CLI output protection')


if __name__ == '__main__':
    main()
