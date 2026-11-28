# SPDX-License-Identifier: BSD-2-Clause
"""Lower device blocks into validated hardware objects for DL backends."""
import re
from copy import deepcopy
from .syntax import Error, Expr, expanded, symbol
from .hardware import Array, Feedback, FuseMap, Mux, Package, Register, Selector


def require(condition, message):
    if not condition:
        raise Error(message)


def shape(block, fields=(), children=()):
    require(set(block.fields) == set(fields),
            f'{block.kind} {block.name or ""}: fields must be {", ".join(fields)}; '
            f'missing {sorted(set(fields) - set(block.fields))}, '
            f'unknown {sorted(set(block.fields) - set(fields))}')
    require(all(c.kind in children for c in block.children),
            f'{block.kind}: unsupported child block')
    repeated = {'device', 'family', 'pin', 'setting', 'routing', 'register', 'variant'}
    named = repeated | {'array'}
    for kind in children:
        require(kind in repeated or len(block.all(kind)) <= 1,
                f'{block.kind}: duplicate {kind} block')
    require(all(c.name is None for c in block.children if c.kind not in named),
            f'{block.kind}: unexpected name on a singleton block')
    require(block.parent is None, f'{block.kind}: inheritance is only allowed on macrocells')


def expect(block, values):
    for key, value in values.items():
        if isinstance(value, str):
            value = symbol(value)
        require(block.fields.get(key) == value,
                f'{block.kind} {block.name or ""}: unsupported {key}: '
                f'{block.fields.get(key)} (expected {value})')


def integer(value, low, high, name):
    require(type(value) is int and low <= value <= high,
            f'{name}: expected an integer in {low}..{high}')
    return value


def call(value, function, count):
    require(isinstance(value, Expr) and value.op == 'call' and
            value.args[0] == symbol(function) and len(value.args) == count + 1,
            f'expected {function} with {count} arguments')
    return value.args[1:]


def atom(value):
    require(isinstance(value, Expr) and value.op == 'name', 'expected a name')
    return value.args[0]


def compile_atf750c(device):
    # Template bodies are expanded before evaluation: parameter arithmetic
    # must never be evaluated in the device's unrelated scope.
    template_blocks = device.all('macrocell_template')
    instance_blocks = device.all('macrocell')
    d = expanded(type(device)(device.kind, device.name, device.parent,
                             device.fields, [c for c in device.children
                                             if c.kind not in ('macrocell', 'macrocell_template')]))
    shape(d, ('architecture', 'mnemonic', 'package', 'pins', 'fuses', 'default_fuse',
              'inputs', 'outputs', 'global_clock', 'synchronous_preset'),
          ('power', 'array', 'pin_columns', 'signature', 'setting',
           'compatibility_fuses', 'variant', 'pin_map'))
    expect(d, dict(architecture='atf750c', default_fuse=0, global_clock='pin(1)'))
    package_name = atom(d.fields['package'])
    if d.all('pin_map'):
        mapping = d.one('pin_map')
        shape(mapping, ('physical', 'unconnected'))
        require(isinstance(mapping.fields['physical'], list) and
                isinstance(mapping.fields['unconnected'], list), 'pin map values must be lists')
        require(package_name == 'PLCC28' and d.fields['pins'] == 28 and
                len(mapping.fields['physical']) == 24, 'unsupported ATF750C package')
        package = Package(package_name, d.fields['pins'], tuple(mapping.fields['physical']),
                          tuple(mapping.fields['unconnected']), drawing=4)
        suffix = 'lcc'
    else:
        expect(d, dict(package='DIP24', pins=24))
        package = Package('DIP24', 24, tuple(range(1, 25)))
        suffix = ''
    require(len(d.all('array')) == 1, 'ATF750C requires one logic array')
    require(len(d.all('setting')) == 2, 'ATF750C requires power_down and pin_keeper settings')
    mnemonic = atom(d.fields['mnemonic'])
    require(mnemonic == 'v750c' + suffix, 'ATF750C mnemonic must match its package')
    nfuses = integer(d.fields['fuses'], 1, 65535, 'fuse count')
    power = d.one('power')
    shape(power, ('ground', 'supply'))
    expect(power, dict(ground=12, supply=24))
    require(d.fields['inputs'] == list(range(1, 12)) + [13] and
            d.fields['outputs'] == list(range(14, 24)), 'unsupported DIP24 pin assignment')
    array = d.one('array', 'logic')
    shape(array, ('rows', 'columns', 'first_fuse', 'address_order'), ('crosspoint',))
    expect(array, dict(rows=171, columns=84, first_fuse=0, address_order='row_major'))
    crosspoint = array.one('crosspoint')
    shape(crosspoint, ('connected', 'disconnected'))
    expect(crosspoint, dict(connected=0, disconnected=1))
    logic = Array(array.name, array.fields['rows'], array.fields['columns'],
                  array.fields['first_fuse'], **crosspoint.fields)
    columns = d.one('pin_columns')
    shape(columns, children=('pin',))
    require(sorted(c.name for c in columns.children) ==
            sorted(d.fields['inputs'] + d.fields['outputs']), 'pin columns must cover all logic pins')
    pin_columns = {}
    for p in columns.children:
        shape(p, ('high', 'low'))
        pin_columns[p.name] = tuple(integer(p.fields[k], 0, 83, f'pin {p.name} {k}')
                                    for k in ('high', 'low'))
    signature = d.one('signature')
    shape(signature, ('first_fuse', 'bits', 'byte_order', 'bit_order', 'default_bit'))
    expect(signature, dict(bits=64, byte_order='text_order',
                           bit_order='most_significant_first', default_bit=0))
    power_down = d.one('setting', 'power_down')
    shape(power_down, ('fuse', 'disabled', 'pin_controlled', 'default', 'control_pin',
                       'active', 'pin_controlled_replaces', 'functional_test_level'))
    expect(power_down, dict(disabled=0, pin_controlled=1, default='disabled', control_pin=4,
                            active='high', pin_controlled_replaces='logic_input', functional_test_level='low'))
    keeper = d.one('setting', 'pin_keeper')
    shape(keeper, ('fuse', 'disabled', 'enabled', 'default'))
    expect(keeper, dict(disabled=0, enabled=1, default='disabled'))
    compatibility = d.one('compatibility_fuses')
    shape(compatibility, ('positions', 'value', 'purpose'))
    expect(compatibility, dict(value=0, purpose='undocumented'))
    require(isinstance(compatibility.fields['positions'], list), 'compatibility positions must be a list')

    fuse_map = FuseMap(nfuses)
    def own(first, count, label):
        fuse_map.claim_range(first, count, label)

    own(logic.first_fuse, logic.fuse_count, 'AND array')
    own(signature.fields['first_fuse'], signature.fields['bits'], 'signature')
    own(power_down.fields['fuse'], 1, 'power_down')
    own(keeper.fields['fuse'], 1, 'pin_keeper')
    for fuse in compatibility.fields['positions']:
        own(fuse, 1, 'compatibility')
    presets = call(d.fields['synchronous_preset'], 'product', 2)
    require(presets == (symbol('logic'), 170), 'common preset must occupy logic row 170')
    require(len(template_blocks) == 1 and len(instance_blocks) == 10,
            'ATF750C requires one dual-register template and ten macrocells')
    template = template_blocks[0]
    shape(template, ('parameters',), ('product_terms', 'setting', 'routing', 'register', 'output', 'feedback'))
    parameters = [atom(x) for x in expanded(type(template)(template.kind, fields=template.fields)).fields['parameters']]
    require(len(set(parameters)) == len(parameters), 'duplicate template parameter')
    cells = []
    used_rows = {170}
    used_columns = [v for pair in pin_columns.values() for v in pair]
    for instance in instance_blocks:
        require(instance.parent == template.name, f'{instance.name}: unknown template {instance.parent}')
        require(not instance.children and set(instance.fields) == set(parameters),
                f'{instance.name}: template parameters differ')
        env = expanded(instance).fields
        require(all(type(v) is int for v in env.values()), 'template parameters must be integers')
        cell = expanded(type(template)(template.kind, children=template.children), env)
        require(len(cell.all('setting')) == 3 and len(cell.all('routing')) == 2 and
                len(cell.all('register')) == 2, 'unsupported dual-register cell structure')
        product = cell.one('product_terms')
        shape(product, ('first_half', 'second_half', 'reset_primary', 'clock_primary',
                        'clock_secondary', 'reset_secondary', 'output_enable'))
        first = call(product.fields['first_half'], 'rows', 3)
        second = call(product.fields['second_half'], 'rows', 3)
        require(first[0] == second[0] == symbol('logic'), 'cell products must use logic array')
        start, half = first[1:]
        integer(start, 0, 169, 'first row')
        integer(half, 1, 8, 'half sum terms')
        require(second == (symbol('logic'), start + half, half), 'cell halves must be adjacent and equal')
        products = dict(first_half=logic.product(start, half),
                        second_half=logic.product(start + half, half))
        for role, offset in [('reset_primary', 0), ('clock_primary', 1),
                             ('clock_secondary', 2), ('reset_secondary', 3), ('output_enable', 4)]:
            require(call(product.fields[role], 'product', 2) ==
                    (symbol('logic'), start + 2 * half + offset), 'unsupported control-row ordering')
            products[role] = logic.product(start + 2 * half + offset)
        for row in range(start, start + 2 * half + 5):
            require(row < 170 and row not in used_rows, f'overlapping or out-of-range product row {row}')
            used_rows.add(row)
        settings, selectors = {}, {}
        for name, choices in [('output_source', dict(registered=0, combinational=1)),
                              ('term_routing', dict(combined=0, separate=1)),
                              ('output_polarity', dict(inverted=0, normal=1))]:
            b = cell.one('setting', name)
            shape(b, ('fuse', *choices))
            expect(b, choices)
            settings[name] = b.fields['fuse']
            selectors[name] = Selector((b.fields['fuse'],),
                                        {name: b.fields[name] for name in choices})
            own(b.fields['fuse'], 1, f'{instance.name}.{name}')
        config = settings['output_source']
        require(settings['term_routing'] == config + 1 and settings['output_polarity'] == config + 2,
                'output selector, routing and polarity fuses must be adjacent')
        for mode, primary in [('combined', 'sum(first_half, second_half)'), ('separate', 'sum(first_half)')]:
            routing = cell.one('routing', mode)
            shape(routing, ('primary', 'secondary'))
            expect(routing, dict(primary=primary, secondary='sum(second_half)'))
        register_fuses, registers = [], {}
        for name in ('primary', 'secondary'):
            reg = cell.one('register', name)
            shape(reg, ('data', 'asynchronous_reset', 'synchronous_preset', 'power_up'), ('clock', 'type'))
            expect(reg, dict(data=f'term_routing.{name}', asynchronous_reset=f'reset_{name}',
                             synchronous_preset='device.synchronous_preset', power_up='low'))
            clk = reg.one('clock')
            shape(clk, ('edge', 'select_fuse', 'product_term', 'global_pin',
                        'product_term_source', 'global_pin_source'))
            expect(clk, dict(edge='rising', product_term=0, global_pin=1,
                             product_term_source=f'clock_{name}', global_pin_source='device.global_clock'))
            typ = reg.one('type')
            shape(typ, ('select_fuse', 'd', 't'))
            expect(typ, dict(d=0, t=1))
            for b in (clk, typ):
                own(b.fields['select_fuse'], 1, f'{instance.name}.{name}.{b.kind}')
                register_fuses.append(b.fields['select_fuse'])
            clock_selector = Selector((clk.fields['select_fuse'],),
                                      {key: clk.fields[key] for key in ('product_term', 'global_pin')})
            registers[name] = Register(name, reg.fields['data'],
                Mux(clock_selector, dict(product_term=products[f'clock_{name}'],
                                        global_pin=d.fields['global_clock'])),
                Selector((typ.fields['select_fuse'],), {key: typ.fields[key] for key in ('d', 't')}),
                products[f'reset_{name}'], logic.product(presets[1]), atom(reg.fields['power_up']))
        clock0, type0, clock1, type1 = register_fuses
        require((clock1, type0, type1) == (clock0 + 1, clock0 + 2, clock0 + 3),
                'clock/type selectors must occupy four adjacent fuses')
        output = cell.one('output')
        shape(output, ('pin', 'source', 'registered', 'combinational', 'polarity', 'enable'))
        pin = integer(output.fields['pin'], 14, 23, 'output pin')
        expect(output, dict(source='output_source', registered='primary.q',
                            combinational='term_routing.primary', polarity='output_polarity', enable='output_enable'))
        feedback = cell.one('feedback')
        shape(feedback, children=('primary', 'secondary', 'io'))
        aliases, columns_pair, feedbacks = [], [], {}
        for name in ('primary', 'secondary'):
            fb = feedback.one(name)
            shape(fb, ('source', 'high_column', 'low_column', 'alias'))
            expect(fb, dict(source=f'{name}.q'))
            pair = tuple(integer(fb.fields[k], 0, 83, name + ' feedback column')
                         for k in ('high_column', 'low_column'))
            used_columns.extend(pair)
            columns_pair.append(pair)
            aliases.append(integer(call(fb.fields['alias'], 'node', 1)[0], 25, 44, name + ' alias'))
            feedbacks[name] = Feedback(f'{name}.q', *pair, aliases[-1])
        io = feedback.one('io')
        shape(io, ('source', 'columns'))
        expect(io, dict(source=f'pin_level({pin})', columns=f'device.pin_columns[{pin}]'))
        cells.append(dict(pin=pin, start=start, terms=2 * half, config=config, clock=clock0,
                          primary=aliases[0], secondary=aliases[1],
                          q0=columns_pair[0], q1=columns_pair[1], products=products,
                          selectors=selectors, registers=registers, feedbacks=feedbacks))
    require(sorted(c['pin'] for c in cells) == list(range(14, 24)), 'duplicate or missing output cell')
    require(sorted(c[k] for c in cells for k in ('primary', 'secondary')) == list(range(25, 45)),
            'duplicate or missing register alias')
    require(used_rows == set(range(171)), 'product rows are not completely assigned')
    require(sorted(used_columns) == list(range(84)), 'input columns overlap or are unassigned')
    fuse_map.complete()
    cells.sort(key=lambda c: -c['pin'])
    # The compact legacy plot program counts through this regular layout.
    for i, c in enumerate(cells):
        require(c['primary'] == c['pin'] + 21 and c['secondary'] == c['pin'] + 11 and
                c['config'] == cells[0]['config'] + 3 * i and
                c['clock'] == cells[0]['clock'] + 4 * i,
                'ATF750C backend requires regular aliases and selector strides')
    require([c['start'] for c in cells] == sorted(c['start'] for c in cells),
            'product rows must follow descending output pin order')
    variants = {mnemonic: (0, 0)}
    for v in d.all('variant'):
        require(v.parent is None and not v.children and v.fields and
                set(v.fields) <= {'power_down', 'pin_keeper'}, f'{v.name}: invalid variant')
        values = []
        for key, enabled in [('power_down', 'pin_controlled'), ('pin_keeper', 'enabled')]:
            value = v.fields.get(key, symbol('disabled'))
            require(value in (symbol('disabled'), symbol(enabled)), f'{v.name}: invalid {key} value')
            values.append(int(value == symbol(enabled)))
        require(re.fullmatch(r'[A-Za-z_][A-Za-z_0-9]{0,15}', str(v.name)) and v.name not in variants,
                'invalid or duplicate variant mnemonic')
        expected = 'v750c' + ('ext' if values[0] else '') + ('ppk' if values[1] else '') + suffix
        require(v.name == expected, 'variant mnemonic must match the ATF750C runtime selection')
        variants[v.name] = tuple(values)
    return dict(name=device.name, architecture='atf750c', mnemonic=mnemonic, fuses=nfuses, cells=cells,
                columns=pin_columns, signature=signature.fields, power_down=power_down.fields,
                keeper=keeper.fields, compatibility=compatibility.fields, variants=variants,
                array=logic, package=package,
                fuse_owners=fuse_map.owners)


def compile_model(device):
    architecture = atom(device.fields.get('architecture'))
    if architecture == 'atf750c':
        return compile_atf750c(device)
    if architecture == 'gal22v10':
        from .gal22v10 import compile_model as lower
        return lower(device)
    if architecture in ('gal16v8', 'gal20v8'):
        from .galv8 import compile_model as lower
        return lower(device)
    if architecture == 'atf2500':
        from .atf2500 import compile_model as lower
        return lower(device)
    raise Error(f'unsupported architecture {architecture}')


def inherited_devices(tree):
    """Resolve device/family inheritance before evaluating template bodies.

    Named blocks override matching parent blocks. An explicit variant list
    replaces the parent's list so package mnemonics remain deliberate.
    """
    definitions = {}
    for block in tree.children:
        require(block.name not in definitions, f'duplicate device/family name {block.name}')
        definitions[block.name] = block
    resolved = {}
    def resolve(name, path=()):
        require(name in definitions, f'unknown parent device/family {name}')
        require(name not in path and len(path) < 32, f'inheritance cycle or excessive depth at {name}')
        if name in resolved:
            return resolved[name]
        block = deepcopy(definitions[name])
        if block.parent is not None:
            parent = deepcopy(resolve(block.parent, (*path, name)))
            parent.fields.update(block.fields)
            replacements = {(child.kind, child.name): child for child in block.children}
            if block.all('variant'):
                parent.children = [c for c in parent.children if c.kind != 'variant']
            merged = [replacements.pop((c.kind, c.name), c) for c in parent.children]
            merged.extend(replacements.values())
            block.fields, block.children = parent.fields, merged
        block.parent = None
        resolved[name] = block
        return block
    # Validate the declaration graph even for abstract families that no
    # current device instantiates. Their hardware fields may be partial,
    # but a missing parent or inheritance cycle is still a source error.
    for name in definitions:
        resolve(name)
    return [resolved[block.name] for block in tree.children if block.kind == 'device']


def models(tree):
    shape(tree, ('format',), ('device', 'family'))
    require(tree.fields['format'] == 1, 'only format = 1 is supported')
    require(tree.children, 'description contains no devices')
    devices = inherited_devices(tree)
    require(devices, 'description contains no concrete devices')
    result = [compile_model(d) for d in devices]
    names = [n for m in result for n in m['variants']]
    require(len(set(names)) == len(names), 'duplicate library mnemonic')
    return result
