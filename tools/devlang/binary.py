# SPDX-License-Identifier: BSD-2-Clause
"""Generate DL records from a validated hardware model.

Layouts and function codes follow src/shared/devlib.h, src/cupl/fusemap.h,
src/cupl/expand_int.h and src/cupl/doc.c. No original library is read.
"""
import struct
from .model import require

HEADER_SIZE, DIRECTORY_COUNT, ENTRY_SIZE = 199, 1000, 26
DATA_START = HEADER_SIZE + DIRECTORY_COUNT * ENTRY_SIZE
SECTION_COUNT, SECTION_HEADER_SIZE = 13, 20
WIDTHS = (0, 0, 0, 152, 21, 40, 16, 39, 27, 9, 13, 24, 22)
SECTION_NAMES = ('reserved0', 'reserved1', 'reserved2', 'attributes', 'extensions',
                 'fitter_extensions', 'pins', 'nodes', 'fuse_plot',
                 'initialization', 'connections', 'fitter_arguments', 'alternatives')

# Binary ABI codes; these do not appear in the device description.
NONE, D, OE, SP, AR, ARRAY, CK, T, DFB, TFB, IO, CKMUX = 0, 1, 2, 3, 7, 14, 15, 27, 34, 36, 37, 42
OE_TERM = 17
GROUND, SUPPLY, FEEDBACK, CLOCK, COMBINATIONAL, INPUT, REGISTER = 2, 3, 7, 12, 16, 22, 23
POSITIVE, CONTROL, INVERTED_OUTPUT, NORMAL_OUTPUT = 2, 4, 6, 7
EXTENSIONS = [('d', D), ('t', T), ('ar', AR), ('ck', CK), ('ckmux', CKMUX),
              ('oe', OE), ('sp', SP), ('dfb', DFB), ('tfb', TFB), ('io', IO)]

PIN_FIELDS = dict(line=(0, 'H'), pin=(2, 'H'), extension=(4, 'B'), kind=(5, 'B'),
                  variant=(6, 'B'), high=(7, 'B'), low=(8, 'B'), value=(10, 'B'),
                  bits=(11, 'B'), fuse=(12, 'I'))
NODE_FIELDS = dict(array=(0, 'H'), pin=(2, 'H'), extension=(4, 'B'), kind=(5, 'B'),
                   invert=(6, 'B'), select=(7, 'H'), value=(9, 'B'), first=(10, 'I'),
                   or_first=(14, 'I'), config=(18, 'I'), pair=(22, 'I'),
                   width=(27, 'B'), or_width=(28, 'B'), bits=(29, 'B'),
                   pair_bits=(30, 'B'), terms=(31, 'H'), split=(33, 'B'),
                   default=(34, 'B'), has_default=(35, 'B'), sharing=(36, 'B'), active=(37, 'B'), step=(38, 'B'))
LINK_FIELDS = dict(line=(0, 'H'), pin=(2, 'H'), extension=(5, 'H'), group=(7, 'H'),
                   member=(9, 'B'), source_pin=(10, 'B'), compiler_link=(11, 'B'),
                   config_value=(12, 'B'))


def record(size, fields, values):
    result = bytearray(size)
    for name, value in values.items():
        offset, code = fields[name]
        struct.pack_into('<' + code, result, offset, value)
    return bytes(result)


def text_field(text, width):
    raw = text.encode('ascii')
    require(len(raw) < width, f'text does not fit {width}-byte field: {text}')
    return raw.ljust(width, b'\0')


def plot_program(model):
    # Register allocation for the legacy fuse-plot VM, not hardware IDs.
    address, remaining, pin, clock_fuse, output_fuse, new_cell, _, q0, q1 = range(9)
    END, SETUP, TEXT, FUSES, ADDRESS, ADD_ADDRESS, BEGIN, NEWLINE = 2, 3, 4, 5, 7, 8, 9, 10
    ADD, SUBTRACT, SET, PRINT_DECREMENT, IF_EQUAL = 12, 13, 14, 17, 20
    result = []
    def emit(op, group=0, a=0, b=0, c=0, d=0, text=' '):
        result.append(text_field(text, 9) + struct.pack('<BBIIII', op, group, a, b, c, d))
    cells, width, package = model['cells'], model['array'].columns, model['package']
    first = cells[0]
    preset = cells[-1]['start'] + cells[-1]['terms'] + 5
    emit(BEGIN, text='')
    emit(SETUP, 1, q0, 1, q1, model['array'].product(preset).first_fuse)
    emit(TEXT, 1, text='SP')
    emit(NEWLINE, 1, 1)
    emit(ADDRESS, 1, q1)
    emit(FUSES, 1, q1, 0, 1, width, 'x-')
    emit(NEWLINE, 1, 2)
    emit(ADD_ADDRESS, 1, 0, 0, 0, width)
    for register, value in [(output_fuse, first['config']), (clock_fuse, first['clock']),
                             (pin, package.number(first['pin'])),
                             (q1, package.number(first['secondary'])),
                             (q0, package.number(first['primary']))]:
        emit(SET, 2, register, value)
    emit(SETUP, 2, remaining, preset, address, 0)
    output_pins = [package.number(c['pin']) for c in cells]
    descending = output_pins == sorted(output_pins, reverse=True)
    for cell, physical in zip(cells, output_pins):
        emit(IF_EQUAL, 2, remaining, preset - cell['start'], 0, 1 if descending else 2)
        emit(ADD, 2, new_cell, 1)
        if not descending:
            emit(SET, 2, pin, physical)
    # Retain compact count-down programs for ordered packages; arbitrary
    # package maps assign the actual pin at each cell boundary above.
    if descending:
        outputs = set(output_pins)
        for absent in sorted(set(range(min(outputs), max(outputs) + 1)) - outputs, reverse=True):
            emit(IF_EQUAL, 2, pin, absent, 0, 1)
            emit(SUBTRACT, 2, pin, 1)
    emit(IF_EQUAL, 2, new_cell, 1, 0, 11)
    emit(PRINT_DECREMENT, 2, pin, 1, text='Pin #')
    emit(PRINT_DECREMENT, 2, q1, 1, text='Node ')
    emit(PRINT_DECREMENT, 2, q0, 1, text=',')
    emit(ADDRESS, 2, output_fuse)
    emit(TEXT, 2, text='Mode ')
    emit(FUSES, 2, output_fuse, 0, 1, 3, 'x-')
    emit(ADDRESS, 2, clock_fuse)
    emit(FUSES, 2, clock_fuse, 0, 1, 4, 'x-')
    emit(ADD, 2, output_fuse, 3, clock_fuse, 4)
    emit(NEWLINE, 2, 1)
    emit(SUBTRACT, 2, new_cell, 1)
    emit(ADDRESS, 2, address)
    emit(FUSES, 2, address, 0, 1, width, 'x-')
    emit(NEWLINE, 2, 1)
    emit(ADD_ADDRESS, 2, 0, 0, 0, width)
    emit(END, text='')
    return result


def sections(model, options):
    if model['architecture'] in ('gal16v8', 'gal20v8'):
        from .galv8 import sections as lower
        return lower(model, options)
    if model['architecture'] == 'gal22v10':
        from .gal22v10 import sections as lower
        return lower(model, options)
    if model['architecture'] == 'atf2500':
        from .atf2500 import sections as lower
        return lower(model, options)
    sections = [[] for _ in range(SECTION_COUNT)]
    cells, columns, logic = model['cells'], model['columns'], model['array']
    count = len(cells)
    common_sp = 7 * count + 1
    attr = bytearray(152)
    for offset, code, value in [(0, 'H', 2), (2, 'B', 12), (3, 'B', 24), (8, 'B', 24),
                                (11, 'B', 0), (12, 'H', 2 * count),
                                (21, 'I', model['fuses']), (25, 'H', 2 * count), (27, 'B', 2 * count),
                                (29, 'H', logic.rows), (37, 'H', 250), (39, 'H', common_sp + 3 * count + 1),
                                (41, 'H', 16 + 3 * count), (43, 'B', 1), (44, 'H', 3 * count),
                                (49, 'B', 1), (61, 'B', model['signature']['bits']),
                                (62, 'I', model['signature']['first_fuse'])]:
        struct.pack_into('<' + code, attr, offset, value)
    attr[51:57] = bytes([D, 0, 1, D, 0, 1])
    sections[3] = [bytes(attr)]
    sections[4] = [text_field(name, 20) + bytes([code]) for name, code in EXTENSIONS]

    def pin(**values):
        sections[6].append(record(16, PIN_FIELDS, values))
    def node(**values):
        sections[7].append(record(39, NODE_FIELDS, values))
    def array(number, row, terms):
        products = logic.product(row, terms)
        node(array=number, extension=ARRAY, first=products.first_fuse,
             width=logic.columns, terms=products.count)
    def control(number, p, extension, **values):
        node(array=number, pin=p, extension=extension, kind=CONTROL, **values)

    for p in range(1, 14):
        if p == model['power_down']['control_pin'] and options[0]:
            continue
        if p == 12:
            pin(line=p, pin=p, kind=GROUND)
        else:
            high, low = columns[p]
            pin(line=p, pin=p, kind=CLOCK if p == 1 else INPUT, high=high, low=low)
    pin(line=14, pin=24, kind=SUPPLY)
    for index, c in enumerate(cells):
        p, q0, q1 = c['pin'], c['primary'], c['secondary']
        line, a = 16 + 3 * index, 1 + 7 * index
        high, low = columns[p]
        pin(line=line, pin=p, kind=COMBINATIONAL, high=high, low=low,
            value=c['selectors']['output_source'].encode('combinational'), bits=1, fuse=c['config'])
        for ext in (D, T):
            pin(line=line, pin=p, kind=REGISTER, variant=ext, high=c['q0'][0], low=c['q0'][1])
        pin(line=line, pin=p, extension=IO, kind=COMBINATIONAL, high=high, low=low)
        for ext in (DFB, TFB):
            pin(line=line, pin=p, extension=ext, kind=FEEDBACK, high=c['q0'][0], low=c['q0'][1])
        pin(line=line + 1, pin=q0, kind=REGISTER, high=c['q0'][0], low=c['q0'][1])
        pin(line=line + 2, pin=q1, kind=REGISTER, high=c['q1'][0], low=c['q1'][1])

        array(a, c['start'], c['terms'])
        for extension in (NONE, D, T):
            for kind, positive in [(INVERTED_OUTPUT, 0), (NORMAL_OUTPUT, 1)]:
                values = dict(array=a, pin=p, extension=extension, kind=kind,
                              select=int(extension == NONE) + 4 * positive,
                              first=c['config'], width=3, has_default=1, sharing=1, active=1)
                if extension == T:
                    values.update(value=c['registers']['primary'].kind.encode('t'),
                                  config=c['registers']['primary'].kind.positions[0], bits=1)
                node(**values)
        for extension in (D, T):
            node(array=a, pin=q0, extension=extension, kind=POSITIVE if extension == D else CONTROL,
                 select=int(extension == T), first=c['clock'] + 2, width=1,
                 has_default=1, sharing=1, active=1)
        for slot, role, row, pins in [(a + 1, AR, c['products']['reset_primary'].first_row, [p, q0]),
                                      (a + 2, CK, c['products']['clock_primary'].first_row, [p, q0]),
                                      (a + 3, OE, c['products']['output_enable'].first_row, [p])]:
            array(slot, row, 1)
            for number in pins:
                control(slot, number, role, default=2 if role == OE else 0)
        array(a + 4, c['start'] + c['terms'] // 2, c['terms'] // 2)
        for extension in (D, T):
            node(array=a + 4, pin=q1, extension=extension,
                 kind=POSITIVE if extension == D else CONTROL, select=1,
                 first=c['config'] + 1, width=1, value=int(extension == T),
                 config=c['clock'] + 3, bits=1, has_default=1, sharing=1, active=1)
        for slot, role, row in [(a + 5, CK, c['products']['clock_secondary'].first_row),
                                (a + 6, AR, c['products']['reset_secondary'].first_row)]:
            array(slot, row, 1)
            control(slot, q1, role)
    # Historical record order is retained to make comparisons inspectable.
    registers = [(c, 'pin') for c in cells]
    registers += sorted(((c, role) for c in cells for role in ('secondary', 'primary')),
                        key=lambda item: item[0][item[1]])
    array(common_sp, cells[0]['registers']['primary'].synchronous_preset.first_row, 1)
    for c, role in registers:
        control(common_sp, c[role], SP, default=3)
    for index, (c, role) in enumerate(registers, common_sp + 1):
        reg = c['registers']['secondary' if role == 'secondary' else 'primary']
        control(index, c[role], CKMUX, pair=reg.clock.selector.positions[0], pair_bits=1)
        node(array=index, extension=ARRAY, width=1, terms=1, sharing=3)
    sections[8] = plot_program(model)
    signature = model['signature']
    initial = [(signature['first_fuse'], signature['first_fuse'] + signature['bits'] - 1, 0),
               (model['power_down']['fuse'], model['power_down']['fuse'], options[0]),
               (model['keeper']['fuse'], model['keeper']['fuse'], options[1])]
    # Default-zero compatibility positions need no initializer. Keep runs
    # after the signature explicit, as in legacy native-mapper libraries.
    positions = sorted(p for p in model['compatibility']['positions']
                       if p > signature['first_fuse'] + signature['bits'] - 1)
    for p in positions:
        if initial[-1][2] == 0 and initial[-1][1] + 1 == p and initial[-1][0] in positions:
            initial[-1] = (initial[-1][0], p, 0)
        else:
            initial.append((p, p, 0))
    sections[9] = [struct.pack('<IIB', *r) for r in initial]

    def link(line, p, extension, group, member=0, compiler_link=0, source_pin=0, config_value=0):
        sections[10].append(record(13, LINK_FIELDS,
                                  dict(line=line, pin=p, extension=extension, group=group,
                                       member=member, compiler_link=compiler_link,
                                       source_pin=source_pin, config_value=config_value)))
    # Each role has its own connection group. Q0 aliases use the primary
    # array/control records but never allocate another physical register.
    for group_base, role, function, slot, linked in [
            (0, 'pin', CK, 2, 1), (count, 'secondary', CK, 5, 1),
            (2 * count, 'primary', CK, 2, 1), (3 * count, 'pin', OE, 3, 1),
            (4 * count, 'pin', AR, 1, 0), (5 * count, 'secondary', AR, 6, 0),
            (6 * count, 'primary', AR, 1, 0),
            (8 * count, 'pin', SP, None, 1), (9 * count, 'secondary', SP, None, 1),
            (10 * count, 'primary', SP, None, 1)]:
        for i, c in enumerate(cells):
            a, group = 1 + 7 * i, group_base + i + 1
            link(common_sp if function == SP else a + slot, c[role], function, group, 1, linked)
            for extension in ((NONE, D, T) if function == OE else (D, T)):
                link(a + (4 if role == 'secondary' else 0), c[role], extension, group, 0, linked)
    group = 11 * count + 1
    # The validated clock model selects the global pin with fuse value 1.
    # Connection membership alone does not program that selector.
    link(1, 1, CK, group, 1, 1, 1,
         config_value=cells[0]['registers']['primary'].clock.selector.encode('global_pin'))
    for role in ('pin', 'secondary', 'primary'):
        for i, c in enumerate(cells):
            for extension in (D, T):
                link(1 + 7 * i + (4 if role == 'secondary' else 0), c[role], extension, group, 0, 1)
    return package_sections(model, sections)


def package_sections(model, sections):
    """Assign physical pins after lowering the shared silicon topology."""
    package = model['package']
    if package.name == 'DIP24':
        return sections
    attrs = bytearray(sections[3][0])
    attrs[2], attrs[3], attrs[8], attrs[11] = (
        package.number(attrs[2]), package.number(attrs[3]), package.pins, package.drawing)
    input_slots = struct.unpack_from('<H', attrs, 41)[0]
    struct.pack_into('<H', attrs, 41, input_slots + len(package.unconnected))
    sections[3] = [bytes(attrs)]
    for section in (6, 7, 10):
        mapped = []
        for raw in sections[section]:
            r = bytearray(raw)
            struct.pack_into('<H', r, 2, package.number(struct.unpack_from('<H', r, 2)[0]))
            mapped.append(bytes(r))
        sections[section] = mapped
    for line, pin in enumerate(package.unconnected, input_slots):
        sections[6].append(record(16, PIN_FIELDS, dict(line=line, pin=pin, kind=19)))
    return sections


def image(sections):
    result = bytearray(SECTION_COUNT * SECTION_HEADER_SIZE)
    # The first three descriptors refer to the outer library header,
    # directory, and this descriptor table. They are not record payloads.
    for index, segment, count, width, offset in [
            (0, 0, 1, HEADER_SIZE, 0),
            (1, 0, DIRECTORY_COUNT, ENTRY_SIZE, HEADER_SIZE),
            (2, 1, SECTION_COUNT, SECTION_HEADER_SIZE, 0)]:
        struct.pack_into('<BBHHHIII', result, index * SECTION_HEADER_SIZE,
                         index, segment, count, count, width, offset, 0, 0)
    for index, records in enumerate(sections):
        if index < 3:
            continue
        width = WIDTHS[index]
        require(len(records) <= 65535 and all(len(r) == width for r in records), 'invalid section records')
        struct.pack_into('<BBHHHIII', result, index * SECTION_HEADER_SIZE,
                         index, 1, len(records), len(records), width, len(result), 0, 0)
        result.extend(b''.join(records))
    return bytes(result)


def library(models):
    entries = [(name, image(sections(model, options)))
               for model in models for name, options in model['variants'].items()]
    require(len(entries) <= DIRECTORY_COUNT, 'too many device entries')
    result = bytearray(DATA_START)
    struct.pack_into('<HH', result, 0, 0xd431, 0x28)
    result[4:21] = text_field('LinCUPL', 17)
    result[21:38] = text_field('v1.0', 17)
    result[38:199] = text_field('LinCUPL device descriptions', 161)
    for index, (name, data) in enumerate(entries):
        start = HEADER_SIZE + index * ENTRY_SIZE
        result[start:start + 17] = text_field(name, 17)
        result[start + 17] = 1
        struct.pack_into('<II', result, start + 18, len(data), len(result))
        result.extend(data)
    return bytes(result)
