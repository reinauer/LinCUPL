# SPDX-License-Identifier: BSD-2-Clause
"""Readable, independently compilable descriptions of DL library interfaces.

This profile retains the compiler's tables, including ordering and legacy
quirks. It does not infer a silicon architecture from a fitter interface.
Field layouts follow the native readers in src/cupl, src/sim and src/shared.
"""
from dataclasses import dataclass, field
import hashlib
import re
import struct

from . import binary
from .audit import validate_coverage
from .compare import records
from .syntax import Error, Expr, evaluate, parse, symbol


def require(condition, message):
    if not condition:
        raise Error(message)


# Only assign semantic names established by the native consumers. Other
# compiler modes remain explicitly numbered fields, not guessed hardware.
EXTENSIONS = dict(enumerate([
    'none', 'd', 'oe', 'sp', 'extension_4', 'ap', 'sr', 'ar',
    's', 'r', 'j', 'k', 'ffm', 'ca', 'array', 'ck', 'pr',
    'oe_term', 'ap_alternate', 'byp', 'extension_20', 'd_special',
    'd_enabled', 'dq', 'lq', 'extension_25', 'extension_26',
    't', 'l', 'le', 'inverted_latch', 'ce', 'obs', 'extension_33',
    'dfb', 'lfb', 'tfb', 'io', 'int', 'inverted_clock',
    'extension_40', 'extension_41', 'ckmux', 'oemux', 'lemux',
    'tec', 'cnt', 'extension_47', 'imux', 't1', 't2', 'iod',
    'iol', 'iot', 'ioar', 'ioap', 'iosr', 'iosp', 'iock',
    'ioce', 'iole', 'extension_61', 'armux', 'apmux',
    'srmux', 'spmux', 'cemux', 'iockmux']))
DOWNLOAD = {0: 'none', 1: 'jedec', 2: 'hl', 3: 'ascii_hex',
            4: 'jedec_and_hex', 5: 'jedec_and_hl'}
POWER_UP = {0: 'unknown', 1: 'low', 2: 'high'}
BOOL = {0: 'no', 1: 'yes'}
POLARITY = {0: 'unspecified', 1: 'negative', 2: 'positive',
            3: 'programmable', 4: 'control', 5: 'negative_output',
            6: 'programmable_negative', 7: 'programmable_positive'}
ARGUMENT = {0: 'none', 1: 'pla_file', 2: 'file_suffix', 3: 'jedec_file',
            4: 'part_name', 5: 'literal', 6: 'tool_directory'}
PLA_OPTIONS = {'p': 'omit_pin_zero', 'F': 'omit_type', '2': 'copy_tt2',
               's': 'short_labels', 'P': 'omit_plus', 'T': 'tool_header',
               'D': 'omit_device', 'o': 'output_keyword', 'A': 'alternate_keywords',
               'J': 'jedec_keyword', 'G': 'omit_group'}
FAMILIES = {0: 'pal', 3: 'ifl', 4: 'prom', 9: 'external_fitter', 255: 'special_pal'}
PIN_TYPES = {0: 'unspecified', 2: 'ground', 3: 'supply', 4: 'negative_supply',
             5: 'clock', 7: 'feedback', 9: 'output_enable', 11: 'register_feedback',
             12: 'clock_input', 16: 'combinational', 19: 'not_connected',
             22: 'input', 23: 'registered'}
VARIANTS = {0: 'default', 1: 'd', 8: 's', 9: 'r', 22: 'd_enabled',
            27: 't', 28: 'l', 255: 'io'}
CONDITION = {1: 'uses_d', 2: 'uses_oe', 3: 'uses_combinational',
             4: 'always', 5: 'uses_oemux'}
PLOT = {0: 'nop', 1: 'begin_hl', 2: 'end', 3: 'setup_loop',
        4: 'text', 5: 'fuses_forward', 6: 'fuses_reverse',
        7: 'address', 8: 'end_row', 9: 'begin_fuses', 10: 'newline',
        12: 'add', 13: 'subtract', 14: 'set', 15: 'print',
        16: 'print_increment', 17: 'print_decrement', 18: 'loop',
        19: 'fill', 20: 'if_equal', 21: 'prom', 22: 'print_two_digits'}


@dataclass(frozen=True)
class Field:
    name: str
    offset: int
    width: int = 1
    text: bool = False
    names: object = field(default=None, compare=False, hash=False)

    def encode(self, value):
        value = evaluate(value)
        if self.text:
            if self.names:
                require(isinstance(value, list), f'{self.name}: expected a list of options')
                inverse = {v: k for k, v in self.names.items()}
                names = [bare_name(v, self.name) for v in value]
                require(all(n in inverse for n in names), f'{self.name}: unknown option')
                value = ''.join(inverse[n] for n in names)
            require(isinstance(value, str) and '\0' not in value,
                    f'{self.name}: expected quoted text without NUL')
            try:
                data = value.encode('latin1')
            except UnicodeEncodeError as exc:
                raise Error(f'{self.name}: text must fit the DL character set') from exc
            require(len(data) <= self.width, f'{self.name}: text exceeds {self.width} bytes')
            return data.ljust(self.width, b'\0')
        if isinstance(value, Expr) and value.op == 'name' and self.names:
            inverse = {v: k for k, v in self.names.items()}
            require(value.args[0] in inverse, f'{self.name}: unknown value {value.args[0]}')
            value = inverse[value.args[0]]
        require(type(value) is int and 0 <= value < 1 << (8 * self.width),
                f'{self.name}: expected an unsigned {self.width * 8}-bit value')
        return value.to_bytes(self.width, 'little')



F = Field
ATTRIBUTES = {
    'package': [F('pins', 8), F('drawing_style', 11), F('internal_nodes', 12, 2)],
    'power': [F('ground', 2), F('supply', 3)],
    'compatibility': [F('attribute_4', 4), F('attribute_5', 5),
                      F('attribute_7', 7), F('attribute_20', 20)],
    'fuses': [F('count', 21, 4), F('default', 19)],
    'signature': [F('bits', 61), F('first_fuse', 62, 4)],
    'compiler': [F('revision', 0, 2), F('family', 17, names=FAMILIES),
                 F('extra_nodes_a', 14), F('extra_nodes_b', 15), F('extra_nodes_c', 16),
                 F('register_resources', 25, 2), F('clocked_resources', 27),
                 F('default_extension', 28, names=EXTENSIONS), F('product_rows', 29, 2),
                 F('initial_term_capacity', 37, 2), F('array_slots', 39, 2),
                 F('input_line_slots', 41, 2), F('extra_symbols', 44, 2),
                 F('download', 43, names=DOWNLOAD)],
    'simulation': [F('output_enable_mode', 46), F('output_polarity_mode', 47),
                   F('internal_polarity_mode', 48), F('power_up', 49, names=POWER_UP),
                   F('preload_polarity', 66)],
    'sequence': [F('next_function', 51, names=EXTENSIONS),
                 F('next_alternate', 52, names=EXTENSIONS), F('next_count', 53),
                 F('input_function', 54, names=EXTENSIONS),
                 F('input_alternate', 55, names=EXTENSIONS), F('input_count', 56)],
    'prom': [F('address_bits', 9), F('data_width', 10)],
    'fitter': [F('input_format', 18), F('name', 67, 13, True),
               F('arguments', 80, 32, True), F('pla_options', 112, 20, True, PLA_OPTIONS),
               F('part', 132, 20, True)],
}

TABLES = {
    'extensions': (4, [F('name', 0, 20, True), F('function', 20, names=EXTENSIONS)]),
    'fitter_extensions': (5, [F('extension', 0, 20, True), F('alias', 20, 20, True)]),
    'pin_functions': (6, [
        F('line', 0, 2), F('pin', 2, 2), F('function', 4, names=EXTENSIONS),
        F('compiler_type', 5, names=PIN_TYPES), F('variant', 6, names=VARIANTS), F('true_column', 7),
        F('complement_column', 8), F('config_value', 10), F('config_bits', 11),
        F('config_first_fuse', 12, 4)]),
    'node_functions': (7, [
        F('array', 0, 2), F('pin', 2, 2), F('function', 4, names=EXTENSIONS),
        F('polarity', 5, names=POLARITY), F('invert', 6, names=BOOL),
        F('select_value', 7, 2), F('config_value', 9),
        F('first_row_fuse', 10, 4), F('first_or_fuse', 14, 4),
        F('config_first_fuse', 18, 4), F('pair_first_fuse', 22, 4),
        F('legacy_width', 26), F('row_width', 27), F('or_width', 28), F('config_bits', 29),
        F('pair_bits', 30), F('max_terms', 31, 2), F('split_terms', 33),
        F('default_rule', 34), F('has_default', 35, names=BOOL),
        F('sharing_mode', 36), F('active', 37, names=BOOL), F('alternate_step', 38)]),
    'plot': (8, [F('operation', 9, names=PLOT), F('group', 10), F('text', 0, 9, True),
                F('a', 11, 4), F('b', 15, 4), F('c', 19, 4), F('d', 23, 4)]),
    'initialization': (9, [F('first_fuse', 0, 4), F('last_fuse', 4, 4), F('value', 8)]),
    'connections': (10, [F('line', 0, 2), F('pin', 2, 2),
                         F('function', 5, 2, names=EXTENSIONS), F('group', 7, 2),
                         F('member', 9), F('source_pin', 10), F('compiler_link', 11, names=BOOL),
                         F('config_value', 12)]),
    'fitter_arguments': (11, [F('kind', 0, names=ARGUMENT), F('text', 1, 23, True)]),
    'alternatives': (12, [F('device', 0, 20, True), F('condition', 20, names=CONDITION),
                         F('priority', 21)]),
}
FIELDS = {3: [f for fs in ATTRIBUTES.values() for f in fs],
          **{i: fs for i, fs in TABLES.values()}}


def entries(data):
    """Retain directory order and duplicate mnemonics, as native lookup does."""
    require(len(data) >= binary.DATA_START and data[:4] == struct.pack('<HH', 0xd431, 0x28),
            'invalid or truncated DL header/directory')
    result = []
    for i in range(binary.DIRECTORY_COUNT):
        p = binary.HEADER_SIZE + i * binary.ENTRY_SIZE
        name, flags, size, offset = struct.unpack_from('<17sBII', data, p)
        if not flags & 1:
            continue
        require(b'\0' in name, 'unterminated device mnemonic')
        name = name.split(b'\0', 1)[0].decode('ascii')
        require(re.fullmatch(r'[A-Za-z_][A-Za-z_0-9]*', name), 'invalid device mnemonic')
        require(offset >= binary.DATA_START and size >= 260 and offset + size <= len(data),
                f'{name}: image outside library')
        result.append(dict(name=name, index=i, flags=flags, image=data[offset:offset + size]))
    return result


def pack_library(items):
    require(0 < len(items) <= binary.DIRECTORY_COUNT, 'expected 1..1000 device entries')
    result = bytearray(binary.DATA_START)
    struct.pack_into('<HH', result, 0, 0xd431, 0x28)
    result[4:21] = binary.text_field('LinCUPL', 17)
    result[21:38] = binary.text_field('v1.0', 17)
    result[38:199] = binary.text_field('LinCUPL device descriptions', 161)
    for index, item in enumerate(items):
        name, data = item['name'], item['image']
        p = binary.HEADER_SIZE + index * binary.ENTRY_SIZE
        result[p:p + 17] = binary.text_field(name, 17)
        result[p + 17] = item.get('flags', 1)
        struct.pack_into('<II', result, p + 18, len(data), len(result))
        result.extend(data)
    return bytes(result)


def bare_name(value, context):
    require(isinstance(value, Expr) and value.op == 'name', f'{context}: expected a field name')
    return value.args[0]


def field_values(fields, values, context):
    expected = {f.name for f in fields}
    require(values.keys() == expected,
            f'{context}: missing {sorted(expected - values.keys())}; '
            f'unknown {sorted(values.keys() - expected)}')
    return {f: f.encode(values[f.name]) for f in fields}


def write_record(width, values):
    record = bytearray(width)
    for f, data in values.items():
        record[f.offset:f.offset + f.width] = data
    return bytes(record)


def compile_device(device):
    require(device.parent is None and isinstance(device.name, str) and len(device.name) <= 16,
            'library device requires a mnemonic of at most 16 characters')
    require(device.fields == {'representation': symbol('library')},
            f'{device.name}: expected representation = library')
    require(all(c.kind in ATTRIBUTES or c.kind in TABLES for c in device.children),
            f'{device.name}: unknown block')
    require(all(c.name is None and c.parent is None for c in device.children),
            f'{device.name}: library blocks cannot be named or inherit')
    sections = [[] for _ in range(binary.SECTION_COUNT)]
    attrs = {}
    for kind, fields in ATTRIBUTES.items():
        block = device.one(kind)
        require(not block.children, f'{kind}: unexpected nested block')
        attrs.update(field_values(fields, block.fields, kind))
    sections[3] = [write_record(152, attrs)]
    require(int.from_bytes(sections[3][0][:2], 'little') != 0,
            'compiler.revision: zero denotes an unsupported device')
    for kind, (section, fields) in TABLES.items():
        blocks = device.all(kind)
        if not blocks:
            continue
        block = blocks[0]
        require(block.fields.keys() == {'columns', 'rows'}, f'{kind}: expected columns and rows')
        require(len(block.children) <= 1 and all(c.kind == 'defaults' and c.name is None and
                c.parent is None and not c.children for c in block.children),
                f'{kind}: only one defaults block is allowed')
        defaults = block.children[0].fields if block.children else {}
        columns = evaluate(block.fields['columns'])
        rows = evaluate(block.fields['rows'])
        require(isinstance(columns, list) and isinstance(rows, list), f'{kind}: expected lists')
        columns = [bare_name(c, kind) for c in columns]
        require(len(set(columns)) == len(columns), f'{kind}: duplicate column')
        require(not (defaults.keys() & set(columns)), f'{kind}: default duplicates a column')
        require(len(rows) <= 65535, f'{kind}: too many records')
        # Validate the schema even for an empty table.
        require(defaults.keys() | set(columns) == {f.name for f in fields},
                f'{kind}: defaults and columns must specify every field exactly once')
        for f in fields:
            if f.name in defaults:
                f.encode(defaults[f.name])
        for row in rows:
            require(isinstance(row, Expr) and row.op == 'call' and row.args[0] == symbol('row'),
                    f'{kind}: expected row(...)')
            require(len(row.args) == len(columns) + 1, f'{kind}: row has wrong number of values')
            values = {**defaults, **dict(zip(columns, row.args[1:]))}
            sections[section].append(write_record(binary.WIDTHS[section], field_values(fields, values, kind)))
    return dict(name=device.name, image=binary.image(sections))


def compile_tree(tree):
    validate_coverage(FIELDS, binary.WIDTHS)
    require(tree.fields == {'format': 1} and tree.children and
            all(c.kind == 'device' for c in tree.children),
            'library description requires format = 1 and device blocks')
    return [compile_device(d) for d in tree.children]


def compile_text(text, filename='<input>'):
    return compile_tree(parse(text, filename))


def canonical(image):
    """Compare every record byte except bytes after text terminators.

    Unlike the ATF750C hardware comparison, this performs NO reader repairs,
    active-operand filtering, array annotation changes or empty-row removal.
    Compatibility metadata and reserved bytes are compared unchanged.
    """
    validate_coverage(FIELDS, binary.WIDTHS)
    sections = records(image)
    omitted = dict(bytes_after_text_terminator=0)
    for section, fields in FIELDS.items():
        for record in sections[section]:
            for f in fields:
                if not f.text:
                    continue
                raw = record[f.offset:f.offset + f.width]
                if 0 in raw:
                    for p in range(f.offset + raw.index(0) + 1, f.offset + f.width):
                        omitted['bytes_after_text_terminator'] += record[p] != 0
                        record[p] = 0
    return sections, omitted


def compare_image(generated, reference):
    left, left_omitted = canonical(generated)
    right, right_omitted = canonical(reference)
    differences = []
    for section in range(3, binary.SECTION_COUNT):
        if left[section] != right[section]:
            differences.append(dict(section=binary.SECTION_NAMES[section],
                                    generated_records=len(left[section]), reference_records=len(right[section]),
                                    examples=[dict(record=i, byte=j, generated=a, reference=b)
                                              for i, (x, y) in enumerate(zip(left[section], right[section]))
                                              for j, (a, b) in enumerate(zip(x, y)) if a != b][:8]))
    return dict(raw_equal=generated == reference, canonical_equal=left == right,
                generated_sha256=hashlib.sha256(generated).hexdigest(),
                reference_sha256=hashlib.sha256(reference).hexdigest(),
                generated_bytes=len(generated), reference_bytes=len(reference),
                records=sum(map(len, left)), differences=differences,
                generated_omissions=left_omitted, reference_omissions=right_omitted)


def compare_libraries(generated, reference):
    original = {}
    for e in entries(reference):
        original.setdefault(e['name'], []).append(e['image'])
    occurrences, result = {}, {}
    for e in entries(generated):
        name = e['name']
        n = occurrences.get(name, 0)
        occurrences[name] = n + 1
        require(name in original and n < len(original[name]), f'reference is missing {name} occurrence {n + 1}')
        key = name if n == 0 else f'{name}#{n + 1}'
        result[key] = compare_image(e['image'], original[name][n])
    return result
