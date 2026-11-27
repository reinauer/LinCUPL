# SPDX-License-Identifier: BSD-2-Clause
"""Lower fixed-OR 22V10 macrocells to the native compiler's DL interface."""
import re
import struct

from .hardware import Array, Feedback, FixedClock, FuseMap, Package, Register, Selector
from .model import atom, call, expect, integer, require, shape
from .syntax import Block, expanded, symbol


def compile_model(device):
    templates, instances = device.all('macrocell_template'), device.all('macrocell')
    d = expanded(Block(device.kind, device.name, fields=device.fields,
                       children=[c for c in device.children
                                 if c.kind not in ('macrocell', 'macrocell_template')]))
    shape(d, ('architecture', 'mnemonic', 'package', 'pins', 'fuses', 'default_fuse',
              'inputs', 'outputs', 'global_clock', 'asynchronous_reset', 'synchronous_preset'),
          ('power', 'array', 'pin_columns', 'signature', 'pin_map'))
    expect(d, dict(architecture='gal22v10', default_fuse=0, global_clock='pin(1)',
                   asynchronous_reset='product(logic, 0)', synchronous_preset='product(logic, 131)'))
    mnemonic = atom(d.fields['mnemonic'])
    require(re.fullmatch(r'[A-Za-z_][A-Za-z_0-9]{0,15}', mnemonic), 'invalid device mnemonic')
    power = d.one('power')
    shape(power, ('ground', 'supply'))
    expect(power, dict(ground=12, supply=24))
    package_name = atom(d.fields['package'])
    if package_name == 'DIP24':
        require(d.fields['pins'] == 24 and not d.all('pin_map'), 'invalid DIP24 package')
        package = Package('DIP24', 24, tuple(range(1, 25)))
    else:
        require(package_name == 'PLCC28' and d.fields['pins'] == 28, 'unsupported 22V10 package')
        mapping = d.one('pin_map')
        shape(mapping, ('physical', 'unconnected'))
        require(isinstance(mapping.fields['physical'], list) and
                isinstance(mapping.fields['unconnected'], list) and
                len(mapping.fields['physical']) == 24, 'invalid reference package map')
        package = Package('PLCC28', 28, tuple(mapping.fields['physical']),
                          tuple(mapping.fields['unconnected']), drawing=4)
    require(d.fields['inputs'] == list(range(1, 12)) + [13] and
            d.fields['outputs'] == list(range(14, 24)), 'invalid 22V10 logic pin allocation')
    a = d.one('array', 'logic')
    shape(a, ('rows', 'columns', 'first_fuse', 'address_order'), ('crosspoint',))
    expect(a, dict(rows=132, columns=44, first_fuse=0, address_order='row_major'))
    crosspoint = a.one('crosspoint')
    shape(crosspoint, ('connected', 'disconnected'))
    expect(crosspoint, dict(connected=0, disconnected=1))
    array = Array(a.name, a.fields['rows'], a.fields['columns'], a.fields['first_fuse'], **crosspoint.fields)
    nfuses = integer(d.fields['fuses'], 1, 65535, 'fuse count')
    owners = FuseMap(nfuses)
    owners.claim_range(array.first_fuse, array.fuse_count, 'logic array')
    signature = d.one('signature')
    shape(signature, ('first_fuse', 'bits', 'byte_order', 'bit_order', 'default_bit'))
    expect(signature, dict(byte_order='text_order', bit_order='most_significant_first', default_bit=0))
    require((signature.fields['first_fuse'], signature.fields['bits'], nfuses) in
            ((5828, 64, 5892), (0, 0, 5828)), 'unsupported 22V10 signature geometry')
    if signature.fields['bits']:
        owners.claim_range(signature.fields['first_fuse'], signature.fields['bits'], 'signature')
    columns = d.one('pin_columns')
    shape(columns, children=('pin',))
    require(sorted(c.name for c in columns.children) == sorted(d.fields['inputs']),
            'input column map must cover the logic inputs')
    pin_columns = {}
    for p in columns.children:
        shape(p, ('high', 'low'))
        pin_columns[p.name] = tuple(integer(p.fields[k], 0, 43, f'pin {p.name} column')
                                   for k in ('high', 'low'))
    require(len(templates) == 1 and len(instances) == 10, '22V10 requires a template and ten cells')
    template = templates[0]
    shape(template, ('parameters',), ('product_terms', 'setting', 'register', 'output', 'feedback'))
    require([c.name for c in template.all('setting')] == ['mode'],
            '22V10 requires exactly the mode setting')
    require([c.name for c in template.all('register')] == ['state'],
            '22V10 requires exactly the state register')
    parameters = [atom(v) for v in expanded(Block('parameters', fields=template.fields)).fields['parameters']]
    require(len(set(parameters)) == len(parameters), 'duplicate template parameter')
    cells, rows, used_columns = [], {0, 131}, [v for pair in pin_columns.values() for v in pair]
    for instance in instances:
        require(instance.parent == template.name and not instance.children and
                set(instance.fields) == set(parameters), 'invalid macrocell template instance')
        env = expanded(instance).fields
        require(all(type(v) is int for v in env.values()), 'template parameters must be integers')
        cell = expanded(Block(template.kind, children=template.children), env)
        products = cell.one('product_terms')
        shape(products, ('sum', 'output_enable'))
        total = call(products.fields['sum'], 'rows', 3)
        enable = call(products.fields['output_enable'], 'product', 2)
        require(total[0] == enable[0] == symbol('logic'), 'cell products must use the logic array')
        sum_terms, output_enable = array.product(*total[1:]), array.product(enable[1])
        require(sum_terms.first_row == output_enable.first_row + 1 and
                sum_terms.count in (8, 10, 12, 14, 16), 'invalid 22V10 product-term allocation')
        occupied = set(range(output_enable.first_row, sum_terms.first_row + sum_terms.count))
        require(not occupied & rows, 'overlapping product rows')
        rows.update(occupied)
        mode = cell.one('setting', 'mode')
        choices = dict(registered_inverted=0, registered_normal=1,
                       combinational_inverted=2, combinational_normal=3)
        shape(mode, ('fuses', *choices))
        expect(mode, choices)
        require(isinstance(mode.fields['fuses'], list) and len(mode.fields['fuses']) == 2 and
                mode.fields['fuses'][1] == mode.fields['fuses'][0] + 1, 'mode fuses must be adjacent')
        selector = Selector(tuple(mode.fields['fuses']), {key: mode.fields[key] for key in choices})
        owners.claim(selector.positions, f'{instance.name}.mode')
        reg = cell.one('register', 'state')
        shape(reg, ('data', 'clock', 'edge', 'type', 'asynchronous_reset', 'synchronous_preset', 'power_up'))
        expect(reg, dict(data='sum', clock='device.global_clock', edge='rising', type='d',
                         asynchronous_reset='device.asynchronous_reset',
                         synchronous_preset='device.synchronous_preset', power_up='low'))
        register = Register('state', sum_terms, FixedClock(d.fields['global_clock']), 'd',
                            array.product(0), array.product(131), 'low')
        output = cell.one('output')
        shape(output, ('pin', 'source', 'registered', 'combinational', 'enable'))
        expect(output, dict(source='mode', registered='state.q', combinational='sum', enable='output_enable'))
        pin = integer(output.fields['pin'], 14, 23, 'output pin')
        fb = cell.one('feedback')
        shape(fb, ('source', 'registered', 'combinational', 'high_column', 'low_column'))
        expect(fb, dict(source='mode', registered='inverted(state.q)', combinational=f'pin_level({pin})'))
        pair = tuple(integer(fb.fields[k], 0, 43, 'feedback column') for k in ('high_column', 'low_column'))
        used_columns.extend(pair)
        cells.append(dict(pin=pin, products=sum_terms, enable=output_enable, mode=selector,
                          register=register, feedback=Feedback('inverted(state.q)', *pair, pin)))
    require(rows == set(range(array.rows)), 'product rows are not completely assigned')
    require(sorted(used_columns) == list(range(array.columns)), 'input columns overlap or are unassigned')
    require(sorted(c['pin'] for c in cells) == list(range(14, 24)), 'duplicate or missing output cell')
    cells.sort(key=lambda c: -c['pin'])
    require([c['enable'].first_row for c in cells] == sorted(c['enable'].first_row for c in cells),
            'product rows must follow descending reference output order')
    require(all(c['mode'].positions == (5808 + 2 * i, 5809 + 2 * i) for i, c in enumerate(cells)),
            '22V10 mode selectors must follow physical cell order')
    owners.complete()
    return dict(name=device.name, architecture='gal22v10', mnemonic=mnemonic, fuses=nfuses,
                variants={mnemonic: ()}, package=package, array=array, cells=cells,
                columns=pin_columns, signature=signature.fields, fuse_owners=owners.owners)


def plot_program(model):
    from .binary import text_field
    result = []
    def emit(op, group=0, a=0, b=0, c=0, d=0, text=' '):
        result.append(text_field(text, 9) + struct.pack('<BBIIII', op, group, a, b, c, d))
    array, cells, package = model['array'], model['cells'], model['package']
    address, remaining, pin, mode, new_cell = 0, 1, 2, 4, 5
    emit(9, text='')
    emit(3, 1, 7, 1, 8, cells[0]['register'].synchronous_preset.first_fuse)
    for label, register in [('SP', 8), ('AR', 9)]:
        emit(4, 1, text=label)
        emit(10, 1, 1)
        emit(7, 1, register)
        emit(5, 1, register, 0, 1, array.columns, 'x-')
        emit(10, 1, 2)
    emit(8, 1, 0, 0, 0, array.columns)
    emit(14, 2, mode, cells[0]['mode'].positions[0])
    count = array.rows - 2
    emit(3, 2, remaining, count, pin, package.number(cells[0]['pin']))
    output_pins = [package.number(c['pin']) for c in cells]
    descending = output_pins == sorted(output_pins, reverse=True)
    for cell, physical in zip(cells, output_pins):
        emit(20, 2, remaining, count - cell['enable'].first_row + 1, 0, 1 if descending else 2)
        emit(12, 2, new_cell, 1)
        if not descending:
            emit(14, 2, pin, physical)
    if descending:
        outputs = set(output_pins)
        for absent in sorted(set(range(min(outputs), max(outputs) + 1)) - outputs, reverse=True):
            emit(20, 2, pin, absent, 0, 1)
            emit(13, 2, pin, 1)
    emit(20, 2, new_cell, 1, 0, 7)
    emit(17, 2, pin, 1, text='Pin #')
    emit(7, 2, mode)
    emit(4, 2, text='Mode ')
    emit(5, 2, mode, 0, 1, 2, 'x-')
    emit(12, 2, mode, 2)
    emit(10, 2, 1)
    emit(13, 2, new_cell, 1)
    emit(7, 2, address)
    emit(5, 2, address, 0, 1, array.columns, 'x-')
    emit(10, 2, 1)
    emit(8, 2, 0, 0, 0, array.columns)
    emit(2, text='')
    return result


def sections(model, options):
    from . import binary as dl
    require(not options, '22V10 has no option variants')
    result = [[] for _ in range(dl.SECTION_COUNT)]
    cells, array = model['cells'], model['array']
    attr = bytearray(152)
    signature = model['signature']
    # ABI resource pools are derived from the emitted cell/array graph.
    values = [(0, 'H', 1), (2, 'B', 12), (3, 'B', 24), (8, 'B', 24),
              (21, 'I', model['fuses']), (25, 'H', len(cells)), (27, 'B', len(cells)),
              (29, 'H', array.rows), (37, 'H', sum(c['products'].count for c in cells)),
              (39, 'H', 2 * len(cells) + 3), (41, 'H', 15 + len(cells)),
              (43, 'B', 1), (44, 'H', 3 * len(cells)), (49, 'B', 1),
              (61, 'B', signature['bits']), (62, 'I', signature['first_fuse'])]
    for offset, code, value in values:
        struct.pack_into('<' + code, attr, offset, value)
    attr[51:57] = bytes([dl.D, 0, 1, dl.D, 0, 1])
    result[3] = [bytes(attr)]
    result[4] = [dl.text_field(name, 20) + bytes([code])
                 for name, code in [('oe', dl.OE), ('d', dl.D), ('ar', dl.AR), ('sp', dl.SP)]]
    def pin(**values):
        result[6].append(dl.record(16, dl.PIN_FIELDS, values))
    def node(**values):
        result[7].append(dl.record(39, dl.NODE_FIELDS, values))
    def rows(number, products, p=0):
        node(array=number, pin=p, extension=dl.ARRAY, first=products.first_fuse,
             width=products.array.columns, terms=products.count)
    for p in range(1, 14):
        if p == 12:
            pin(line=p, pin=p, kind=dl.GROUND)
        else:
            high, low = model['columns'][p]
            pin(line=p, pin=p, kind=dl.CLOCK if p == 1 else 0, high=high, low=low)
    pin(line=14, pin=24, kind=dl.SUPPLY)
    for index, c in enumerate(cells):
        fb = c['feedback']
        pin(line=15 + index, pin=c['pin'], kind=7, high=fb.high_column, low=fb.low_column,
            value=1, bits=1, fuse=c['mode'].positions[1])
        pin(line=15 + index, pin=c['pin'], kind=11, variant=dl.D,
            high=fb.high_column, low=fb.low_column)
    for c in cells:
        node(array=1, pin=c['pin'], extension=dl.AR, kind=dl.CONTROL, default=3)
    rows(1, cells[0]['register'].asynchronous_reset)
    for index, c in enumerate(cells):
        oe, total = 2 + 2 * index, 3 + 2 * index
        rows(oe, c['enable'], c['pin'])
        node(array=oe, pin=c['pin'], extension=dl.OE, kind=dl.CONTROL, default=2)
        rows(total, c['products'], c['pin'])
        for extension, mode in [(dl.NONE, 'combinational'), (dl.D, 'registered')]:
            for kind, polarity in [(dl.INVERTED_OUTPUT, 'inverted'), (dl.NORMAL_OUTPUT, 'normal')]:
                node(array=total, pin=c['pin'], extension=extension, kind=kind,
                     invert=int(extension == dl.D and polarity == 'normal'),
                     select=c['mode'].encode(f'{mode}_{polarity}'),
                     first=c['mode'].positions[0], width=len(c['mode'].positions),
                     has_default=1, sharing=1, active=1)
    preset = 2 * len(cells) + 2
    for c in cells:
        node(array=preset, pin=c['pin'], extension=dl.SP, kind=dl.CONTROL, default=3)
    rows(preset, cells[0]['register'].synchronous_preset)
    result[8] = plot_program(model)
    def link(line, pin, function, group, member=0, compiler_link=0, source_pin=0):
        result[10].append(dl.record(13, dl.LINK_FIELDS,
            dict(line=line, pin=pin, extension=function, group=group, member=member,
                 compiler_link=compiler_link, source_pin=source_pin)))
    link(1, 1, dl.CK, 1, member=1, source_pin=1)
    for index, c in enumerate(cells):
        link(3 + 2 * index, c['pin'], dl.D, 1)
    for index, c in enumerate(cells):
        link(2 + 2 * index, c['pin'], dl.OE, index + 2, 1, 1)
        for extension in (dl.NONE, dl.D):
            link(3 + 2 * index, c['pin'], extension, index + 2, 0, 1)
    for function, number, group in [(dl.AR, 1, len(cells) + 2), (dl.SP, preset, len(cells) + 3)]:
        for c in cells:
            link(number, c['pin'], function, group, 1, 1)
        for index, c in enumerate(cells):
            link(3 + 2 * index, c['pin'], dl.D, group, 0, 1)
    return dl.package_sections(model, result)
