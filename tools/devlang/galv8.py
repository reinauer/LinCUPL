# SPDX-License-Identifier: BSD-2-Clause
"""Lower shared GAL16V8/GAL20V8 macrocells and package maps to DL records."""
import re
import struct

from .hardware import Array, FixedClock, FuseMap, Package, Register, Selector
from .model import atom, call, expect, integer, require, shape
from .syntax import Block, expanded, symbol


def compile_model(device):
    templates, instances = device.all('macrocell_template'), device.all('macrocell')
    d = expanded(Block(device.kind, device.name, fields=device.fields,
                       children=[c for c in device.children
                                 if c.kind not in ('macrocell', 'macrocell_template')]))
    shape(d, ('architecture', 'mnemonic', 'package', 'pins', 'fuses', 'default_fuse',
              'inputs', 'outputs', 'global_clock', 'global_enable', 'mode'),
          ('power', 'array', 'pin_columns', 'setting', 'signature', 'product_disable',
           'mode_devices', 'pin_map', 'power_down'))
    architecture = atom(d.fields['architecture'])
    require(architecture in ('gal16v8', 'gal20v8'), 'unsupported V8 architecture')
    wide = architecture == 'gal20v8'
    npins, width, first_output = (24, 40, 15) if wide else (20, 32, 12)
    ground, enable_pin = npins // 2, npins // 2 + 1
    outputs = list(range(first_output, first_output + 8))
    inputs = list(range(2, ground)) + ([14, 23] if wide else [])
    base_fuses = 64 * width + 146
    expect(d, dict(default_fuse=0, global_clock='pin(1)',
                   global_enable=f'inverted(pin({enable_pin}))'))
    package_name = atom(d.fields['package'])
    if package_name == f'DIP{npins}':
        require(d.fields['pins'] == npins and not d.all('pin_map'), 'invalid DIP package')
        package = Package(package_name, npins, tuple(range(1, npins + 1)))
    else:
        require(wide and package_name == 'PLCC28' and d.fields['pins'] == 28, 'unsupported V8 package')
        mapping = d.one('pin_map')
        shape(mapping, ('physical', 'unconnected'))
        require(isinstance(mapping.fields['physical'], list) and len(mapping.fields['physical']) == npins
                and isinstance(mapping.fields['unconnected'], list), 'invalid reference package map')
        package = Package(package_name, 28, tuple(mapping.fields['physical']),
                          tuple(mapping.fields['unconnected']), drawing=4)
    mnemonic, mode = atom(d.fields['mnemonic']), atom(d.fields['mode'])
    require(re.fullmatch(r'[A-Za-z_][A-Za-z_0-9]{0,15}', mnemonic), 'invalid device mnemonic')
    require(mode in ('automatic', 'registered', 'complex', 'simple'), 'invalid V8 mode')
    power = d.one('power')
    shape(power, ('ground', 'supply'))
    expect(power, dict(ground=ground, supply=npins))
    require(d.fields['inputs'] == inputs and d.fields['outputs'] == outputs, 'invalid V8 pin allocation')
    a = d.one('array', 'logic')
    shape(a, ('rows', 'columns', 'first_fuse', 'address_order'), ('crosspoint',))
    expect(a, dict(rows=64, columns=width, first_fuse=0, address_order='row_major'))
    crosspoint = a.one('crosspoint')
    shape(crosspoint, ('connected', 'disconnected'))
    expect(crosspoint, dict(connected=0, disconnected=1))
    array = Array(a.name, a.fields['rows'], a.fields['columns'], a.fields['first_fuse'], **crosspoint.fields)
    owners = FuseMap(d.fields['fuses'])
    owners.claim_range(array.first_fuse, array.fuse_count, 'logic array')
    power_down = None
    if d.all('power_down'):
        pd = d.one('power_down')
        shape(pd, ('control', 'fuse', 'disabled', 'pin_controlled', 'selection'))
        pd_pin = 4
        expect(pd, dict(control=f'pin({pd_pin})', fuse=base_fuses,
                        disabled=0, pin_controlled=1, selection='pin_controlled'))
        power_down = dict(pin=pd_pin, fuse=pd.fields['fuse'], value=pd.fields['pin_controlled'])
        owners.claim([power_down['fuse']], 'power-down option')
    require(d.fields['fuses'] == base_fuses + bool(power_down), 'invalid V8 fuse count')
    setting = d.one('setting', 'mode')
    shape(setting, ('fuses', 'registered', 'complex', 'simple'))
    expect(setting, dict(fuses=[base_fuses - 2, base_fuses - 1], registered=2, complex=3, simple=1))
    selector = Selector(tuple(setting.fields['fuses']),
                        {k: setting.fields[k] for k in ('registered', 'complex', 'simple')})
    owners.claim(selector.positions, 'device mode')
    signature = d.one('signature')
    shape(signature, ('first_fuse', 'bits', 'byte_order', 'bit_order', 'default_bit'))
    expect(signature, dict(first_fuse=array.fuse_count + 8, bits=64, byte_order='text_order',
                           bit_order='most_significant_first', default_bit=0))
    owners.claim_range(signature.fields['first_fuse'], signature.fields['bits'], 'signature')
    disable = d.one('product_disable')
    shape(disable, ('first_fuse', 'bits', 'enabled', 'disabled'))
    expect(disable, dict(first_fuse=array.fuse_count + 80, bits=64, enabled=1, disabled=0))
    owners.claim_range(disable.fields['first_fuse'], disable.fields['bits'], 'product disables')
    columns = d.one('pin_columns')
    shape(columns, ('registered', 'complex', 'simple'))
    routes = {}
    for name, feedback in [('registered', outputs), ('complex', outputs[1:-1]),
                           ('simple', outputs[:3] + outputs[5:])]:
        pins = columns.fields[name]
        expected = set(inputs) | set(feedback)
        if name != 'registered':
            expected |= {1, enable_pin}
        require(isinstance(pins, list) and len(pins) == width // 2 and
                all(type(p) is int for p in pins) and set(pins) == expected,
                f'{name}: column sources must cover the available inputs and feedback')
        routes[name] = {pin: (2 * i, 2 * i + 1) for i, pin in enumerate(pins)}
    alternatives = {}
    prefer_complex_feedback = False
    if mode == 'automatic':
        targets = d.one('mode_devices')
        targets.fields.setdefault('combinational_feedback', symbol('complex'))
        shape(targets, ('registered', 'complex', 'simple', 'combinational_feedback'))
        require(targets.fields['combinational_feedback'] in (symbol('complex'), symbol('simple')),
                'combinational feedback preference must be complex or simple')
        prefer_complex_feedback = targets.fields['combinational_feedback'] == symbol('complex')
        alternatives = {k: atom(targets.fields[k]) for k in ('registered', 'complex', 'simple')}
        require(all(re.fullmatch(r'[A-Za-z_][A-Za-z_0-9]{0,15}', n)
                    for n in alternatives.values()), 'invalid mode device mnemonic')
        require(len(set(alternatives.values())) == 3 and mnemonic not in alternatives.values(),
                'mode devices must be distinct and cannot refer to the automatic entry')
    else:
        require(not d.all('mode_devices'), 'fixed modes cannot select alternative devices')
    require(len(templates) == 1 and len(instances) == 8, 'V8 requires a template and eight cells')
    template = templates[0]
    shape(template, ('parameters',), ('product_terms', 'setting', 'register', 'output', 'feedback'))
    require([s.name for s in template.all('setting')] == ['polarity', 'cell_mode'] and
            [r.name for r in template.all('register')] == ['state'], 'invalid V8 cell controls')
    parameters = [atom(v) for v in expanded(Block('parameters', fields=template.fields)).fields['parameters']]
    require(len(set(parameters)) == len(parameters), 'duplicate template parameter')
    cells, occupied = [], set()
    for instance in instances:
        require(instance.parent == template.name and not instance.children and
                set(instance.fields) == set(parameters), 'invalid macrocell template instance')
        env = expanded(instance).fields
        require(all(type(v) is int for v in env.values()), 'template parameters must be integers')
        cell = expanded(Block(template.kind, children=template.children), env)
        products = cell.one('product_terms')
        shape(products, ('registered', 'simple', 'combinational', 'output_enable'))
        terms = {}
        for name in products.fields:
            args = call(products.fields[name], 'product' if name == 'output_enable' else 'rows',
                        2 if name == 'output_enable' else 3)
            require(args[0] == symbol('logic'), 'cell products must use the logic array')
            terms[name] = array.product(*args[1:])
        first = terms['registered'].first_row
        require(first % 8 == 0 and terms['registered'].count == 8 and
                terms['simple'] == terms['registered'] and
                terms['combinational'] == array.product(first + 1, 7) and
                terms['output_enable'] == array.product(first), 'invalid V8 product allocation')
        rows = set(range(first, first + 8))
        require(not occupied & rows, 'overlapping macrocell product rows')
        occupied.update(rows)
        polarity = cell.one('setting', 'polarity')
        shape(polarity, ('fuses', 'inverted', 'normal'))
        expect(polarity, dict(inverted=0, normal=1))
        require(isinstance(polarity.fields['fuses'], list) and len(polarity.fields['fuses']) == 1,
                'polarity requires one fuse')
        polarity = Selector(tuple(polarity.fields['fuses']), dict(inverted=0, normal=1))
        owners.claim(polarity.positions, f'{instance.name}.polarity')
        ac1 = cell.one('setting', 'cell_mode')
        shape(ac1, ('fuses', 'registered', 'combinational', 'simple_output', 'simple_input'))
        expect(ac1, dict(registered=0, combinational=1, simple_output=0, simple_input=1))
        require(isinstance(ac1.fields['fuses'], list) and len(ac1.fields['fuses']) == 1,
                'cell mode requires one fuse')
        registered = Selector(tuple(ac1.fields['fuses']), dict(registered=0, combinational=1))
        simple = Selector(tuple(ac1.fields['fuses']), dict(simple_output=0, simple_input=1))
        owners.claim(registered.positions, f'{instance.name}.cell_mode')
        reg = cell.one('register', 'state')
        shape(reg, ('data', 'clock', 'edge', 'type', 'power_up'))
        expect(reg, dict(data='polarity(sum)', clock='device.global_clock', edge='rising', type='d', power_up='low'))
        register = Register('state', terms['registered'], FixedClock(d.fields['global_clock']), 'd', None, None, 'low')
        output = cell.one('output')
        shape(output, ('pin', 'registered', 'combinational', 'registered_enable',
                       'combinational_enable', 'simple_enable'))
        expect(output, dict(registered='inverted(state.q)', combinational='inverted(polarity(sum))',
                            registered_enable='device.global_enable', combinational_enable='output_enable',
                            simple_enable='always'))
        pin = integer(output.fields['pin'], outputs[0], outputs[-1], 'output pin')
        fb = cell.one('feedback')
        shape(fb, ('registered', 'combinational'))
        expect(fb, dict(registered='inverted(state.q)', combinational=f'pin_level({pin})'))
        cells.append(dict(pin=pin, products=terms, polarity=polarity, registered=registered,
                          simple=simple, register=register))
    require(occupied == set(range(array.rows)) and sorted(c['pin'] for c in cells) == outputs,
            'missing product rows or duplicate output pins')
    cells.sort(key=lambda c: -c['pin'])
    require(all(c['products']['registered'].first_row == 8 * i and
                c['polarity'].positions == (array.fuse_count + i,) and
                c['registered'].positions == (array.fuse_count + 72 + i,)
                for i, c in enumerate(cells)), 'cell resources must follow physical output order')
    owners.complete()
    return dict(name=device.name, architecture=architecture, mnemonic=mnemonic, fuses=d.fields['fuses'],
                variants={mnemonic: ()}, package=package, reference_pins=npins, enable_pin=enable_pin,
                power_down=power_down, prefer_complex_feedback=prefer_complex_feedback,
                array=array, cells=cells, columns=routes, mode=mode, mode_selector=selector,
                alternatives=alternatives, signature=signature.fields, disable=disable.fields,
                fuse_owners=owners.owners)


def plot_program(model):
    from .binary import text_field
    result = []
    def emit(op, group=0, a=0, b=0, c=0, d=0, text=' '):
        result.append(text_field(text, 9) + struct.pack('<BBIIII', op, group, a, b, c, d))
    emit(9, text='')
    emit(3, 0, 1, 1, 0, model['mode_selector'].positions[0])
    for i, label in enumerate(('Syn ', 'Ac0 ')):
        if i:
            emit(12, 0, 0, 1)
        emit(4, text=label)
        emit(7)
        emit(5, 0, 0, 0, 1, 1, 'x-')
    emit(10, 0, 2)
    emit(8)
    pins = [model['package'].number(c['pin']) for c in model['cells']]
    emit(14, 1, 2, pins[0])
    emit(14, 1, 4, model['cells'][0]['polarity'].positions[0])
    emit(14, 1, 5, model['cells'][0]['registered'].positions[0])
    emit(3, 1, 1, model['array'].rows, 0, model['array'].first_fuse)
    # Update discontinuous package numbering before the next cell header.
    for i, pin in enumerate(pins[1:], 1):
        if pin != pins[i - 1] - 1:
            emit(20, 1, 1, model['array'].rows - 8 * i + 1, 0, 1)
            emit(14, 1, 2, pin)
    emit(18, 1, 7, 0, 8, 9)
    emit(17, 1, 2, 1, text='Pin #')
    for reg, label in [(4, ' Pol'), (5, ' Ac1')]:
        emit(7, 1, reg)
        emit(4, 1, text=label)
        emit(5, 1, reg, 0, 1, 1, 'x-')
    emit(10, 1, 1, text='')
    emit(12, 1, 4, 1, 5, 1, '')
    emit(7, 1)
    emit(5, 1, 0, 0, 1, model['array'].columns, 'x-')
    emit(10, 1, 1)
    emit(8, 1, d=model['array'].columns)
    emit(2, text='')
    return result


def sections(model, options):
    from . import binary as dl
    require(not options, 'V8 options are selected by device inheritance')
    result = [[] for _ in range(dl.SECTION_COUNT)]
    automatic = model['mode'] == 'automatic'
    mode = 'registered' if automatic else model['mode']
    registered, simple = mode == 'registered', mode == 'simple'
    cells, array, columns = model['cells'], model['array'], model['columns'][mode]
    npins, enable_pin, power_down = model['reference_pins'], model['enable_pin'], model['power_down']
    ground = npins // 2
    per_cell = 3 if registered else 1 if simple else 2
    attr = bytearray(152)
    for offset, code, value in [(0, 'H', 1), (2, 'B', ground), (3, 'B', npins), (8, 'B', npins),
            (21, 'I', model['fuses']), (25, 'H', len(cells) if registered else 0),
            (27, 'B', len(cells) if registered else 0), (29, 'H', array.rows),
            # Initial compiler workspace, independent of silicon row limits.
            (37, 'H', 70), (39, 'H', per_cell * len(cells) + 1),
            (41, 'H', npins + 1 if registered else npins - 1), (43, 'B', 1),
            (44, 'H', 0 if simple else len(cells)), (47, 'B', 2 if registered and not automatic else 0),
            (49, 'B', int(registered)), (61, 'B', model['signature']['bits']),
            (62, 'I', model['signature']['first_fuse'])]:
        struct.pack_into('<' + code, attr, offset, value)
    if registered:
        attr[51:57] = bytes([dl.D, 0, 1, dl.D, 0, 1])
    result[3] = [bytes(attr)]
    extensions = [] if simple else [('oe', dl.OE)] + ([('d', dl.D)] if registered else [])
    result[4] = [dl.text_field(name, 20) + bytes([code]) for name, code in extensions]
    def pin(**values):
        result[6].append(dl.record(16, dl.PIN_FIELDS, values))
    def node(**values):
        result[7].append(dl.record(39, dl.NODE_FIELDS, values))
    def link(**values):
        result[10].append(dl.record(13, dl.LINK_FIELDS, values))
    cell_by_pin = {c['pin']: c for c in cells}
    ordered = list(columns) if simple else [1] + [p for p in columns if p != 1]
    if registered:
        ordered.append(enable_pin)
    for line, p in enumerate(ordered + [ground, npins], 1):
        kind = (dl.GROUND if p == ground else dl.SUPPLY if p == npins else
                5 if registered and p == 1 else 9 if registered and p == enable_pin else
                dl.FEEDBACK if p in cell_by_pin else 0)
        values = dict(line=line, pin=p, kind=kind)
        if p in columns:
            values.update(zip(('high', 'low'), columns[p]))
        if power_down and p == power_down['pin']:
            values.update(extension=40, kind=25, high=0, low=0)
        # Simple-mode pads default to inputs. An output equation sets AC1
        # to zero; using its feedback must not write the input default back.
        pin(**values)
    for i, cell in enumerate(cells):
        p, first = cell['pin'], per_cell * i + 1
        def rows(number, products, kind):
            node(array=number, pin=p, extension=dl.ARRAY, kind=kind,
                 first=products.first_fuse, width=array.columns, terms=products.count)
        def function(number, extension):
            config = cell['simple'] if simple else cell['registered']
            choice = 'simple_output' if simple else 'registered' if extension == dl.D else 'combinational'
            for kind, polarity in [(dl.NORMAL_OUTPUT, 'normal'), (dl.INVERTED_OUTPUT, 'inverted')]:
                node(array=number, pin=p, extension=extension, kind=kind,
                     select=cell['polarity'].encode(polarity), first=cell['polarity'].positions[0], width=1,
                     value=config.encode(choice), config=config.positions[0], bits=1,
                     has_default=int(extension != dl.D), sharing=int(registered), active=1)
        if simple:
            rows(first, cell['products']['simple'], 1)
            function(first, dl.NONE)
        else:
            rows(first, cell['products']['output_enable'], dl.CONTROL)
            node(array=first, pin=p, extension=dl.OE, kind=dl.CONTROL, default=2)
            rows(first + 1, cell['products']['combinational'], 1)
            function(first + 1, dl.NONE)
            if registered:
                rows(first + 2, cell['products']['registered'], 1)
                function(first + 2, dl.D)
    if registered:
        for line, p, function, group in [(1, 1, dl.CK, 0), (npins - 2, enable_pin, dl.OE_TERM, 1)]:
            link(line=line, pin=p, extension=function, group=group, member=1, source_pin=1)
            for i, c in enumerate(cells):
                link(line=3 * (i + 1), pin=c['pin'], extension=dl.D, group=group)
    if not simple:
        for i, c in enumerate(cells):
            for extension, member in [(dl.NONE, 0), (dl.OE, 1)]:
                link(line=per_cell * i + (1 if member else 2), pin=c['pin'], extension=extension,
                     group=i + (2 if registered else 1), member=member, compiler_link=1)
    def init(first, count, value):
        result[9].append(struct.pack('<IIB', first, first + count - 1, value))
    init(model['signature']['first_fuse'], model['signature']['bits'], model['signature']['default_bit'])
    if not automatic:
        init(cells[0]['registered'].positions[0], len(cells), 1)
    init(model['disable']['first_fuse'], model['disable']['bits'], model['disable']['enabled'])
    selected = model['mode_selector'].encode(mode)
    for i, fuse in enumerate(model['mode_selector'].positions):
        init(fuse, 1, (selected >> i) & 1)
    if power_down:
        init(power_down['fuse'], 1, power_down['value'])
    result[8] = plot_program(model)
    if automatic:
        choices = [('registered', 1), ('complex', 2), ('simple', 4)]
        if model['prefer_complex_feedback']:
            choices.insert(1, ('complex', 3))
        result[12] = [dl.text_field(model['alternatives'][name], 20) + bytes([condition, len(choices) - i])
                      for i, (name, condition) in enumerate(choices)]
    return dl.package_sections(model, result)
