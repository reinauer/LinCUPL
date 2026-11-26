# SPDX-License-Identifier: BSD-2-Clause
"""ATF2500C dual-register hardware model and legacy library lowering."""
import struct

from .hardware import Array, Feedback, FuseMap, Mux, Package, Register, Selector
from .syntax import expanded, symbol
from .model import atom, call, expect, integer, require, shape


def compile_model(device):
    templates, instances = device.all('macrocell_template'), device.all('macrocell')
    d = expanded(type(device)(device.kind, device.name, device.parent, device.fields,
                             [b for b in device.children if b.kind not in ('macrocell_template', 'macrocell')]))
    shape(d, ('architecture', 'mnemonic', 'package', 'pins', 'fuses', 'default_fuse',
              'inputs', 'outputs', 'global_clock'),
          ('power', 'array', 'pin_columns', 'user_row', 'setting', 'compatibility_fuses', 'variant', 'pin_map'))
    expect(d, dict(architecture='atf2500', fuses=71816, default_fuse=0, global_clock='pin(1)'))
    package_name = atom(d.fields['package'])
    if d.all('pin_map'):
        mapping = d.one('pin_map')
        shape(mapping, ('physical', 'ground', 'supply'))
        require(package_name == 'PLCC44' and d.fields['pins'] == 44 and
                len(mapping.fields['physical']) == 40, 'unsupported ATF2500C package')
        package = Package(package_name, 44, tuple(mapping.fields['physical']),
                          ground=tuple(mapping.fields['ground']), supply=tuple(mapping.fields['supply']), drawing=4)
        require(package.ground == (4, 26, 33) and package.supply == (11,), 'unsupported extra package power pins')
        suffix = 'lcc'
    else:
        require(package_name == 'DIP40' and d.fields['pins'] == 40, 'unsupported ATF2500C package')
        package = Package(package_name, 40, tuple(range(1, 41)))
        suffix = ''
    mnemonic = atom(d.fields['mnemonic'])
    require(mnemonic == 'v2500c' + suffix, 'ATF2500C mnemonic must match package')
    inputs = [1, 2, 3, *range(17, 24), *range(37, 41)]
    outputs = [*range(4, 10), *range(11, 17), *range(24, 30), *range(31, 37)]
    require(d.fields['inputs'] == inputs and d.fields['outputs'] == outputs, 'unsupported ATF2500C pin roles')
    power = d.one('power')
    shape(power, ('ground', 'supply'))
    expect(power, dict(ground=30, supply=10))
    a = d.one('array', 'logic')
    shape(a, ('rows', 'columns', 'first_fuse', 'address_order'), ('crosspoint',))
    expect(a, dict(rows=416, columns=172, first_fuse=0, address_order='row_major'))
    crosspoint = a.one('crosspoint')
    shape(crosspoint, ('connected', 'disconnected'))
    expect(crosspoint, dict(connected=0, disconnected=1))
    logic = Array('logic', a.fields['rows'], a.fields['columns'], a.fields['first_fuse'])
    mapping = d.one('pin_columns')
    shape(mapping, children=('pin',))
    columns = {}
    for p in mapping.children:
        shape(p, ('high', 'low'))
        require(p.name not in columns and p.name in inputs + outputs, 'duplicate or invalid input column pin')
        columns[p.name] = tuple(integer(p.fields[k], 0, 171, 'input column') for k in ('high', 'low'))
    require(set(columns) == set(inputs + outputs), 'missing pin input columns')
    user = d.one('user_row')
    shape(user, ('first_fuse', 'bits', 'default_bit'))
    expect(user, dict(first_fuse=71746, bits=64, default_bit=1))
    keeper = d.one('setting', 'pin_keeper')
    require(len(d.all('setting')) == 1, 'unsupported device setting')
    shape(keeper, ('fuse', 'disabled', 'enabled', 'default'))
    expect(keeper, dict(fuse=71745, disabled=0, enabled=1, default='disabled'))
    compatibility = d.one('compatibility_fuses')
    shape(compatibility, ('positions', 'value', 'purpose'))
    expect(compatibility, dict(value=0, purpose='undocumented'))
    require(compatibility.fields['positions'] == [71648, *range(71810, 71816)],
            'unsupported ATF2500C compatibility positions')
    ownership = FuseMap(d.fields['fuses'])
    ownership.claim_range(logic.first_fuse, logic.fuse_count, 'logic array')
    ownership.claim_range(user.fields['first_fuse'], user.fields['bits'], 'user row')
    ownership.claim([keeper.fields['fuse']], 'pin keeper')
    ownership.claim(compatibility.fields['positions'], 'compatibility')
    require(len(templates) == 1 and len(instances) == 24, 'ATF2500C needs one template and 24 cells')
    require(len({c.name for c in instances}) == len(instances), 'duplicate macrocell name')
    template = templates[0]
    shape(template, ('parameters',), ('product_terms', 'setting', 'register', 'feedback', 'routing'))
    parameters = [atom(p) for p in expanded(type(template)(template.kind, fields=template.fields)).fields['parameters']]
    require(len(set(parameters)) == len(parameters), 'duplicate template parameter')
    require(set(parameters) == {'pin', 'first_row', 'output_fuses', 'register_fuses', 'preset_row',
                                'primary_node', 'secondary_node', 'io_column'}, 'unsupported cell parameters')
    cells, used_rows, used_columns = [], set(), [x for pair in columns.values() for x in pair]
    for instance in instances:
        require(instance.parent == template.name and not instance.children and set(instance.fields) == set(parameters),
                f'{instance.name}: invalid template parameters')
        env = expanded(instance).fields
        require(all(type(v) is int for v in env.values()), 'template parameters must be integers')
        c = expanded(type(template)(template.kind, children=template.children), env)
        shape(c, children=('product_terms', 'setting', 'register', 'feedback', 'routing'))
        require(len(c.all('setting')) == 3 and len(c.all('register')) == 2, 'invalid cell selectors or registers')
        pin = integer(env['pin'], 1, 40, 'output pin')
        require(pin in outputs, 'macrocell output is not an I/O pin')
        terms = c.one('product_terms')
        names = ('first_sum', 'middle_sum', 'last_sum', 'reset_primary', 'clock_primary',
                 'clock_secondary', 'reset_secondary', 'output_enable', 'preset')
        shape(terms, names)
        products = {}
        for name, (offset, count) in zip(names[:-1], [(0,4), (4,4), (8,4), (12,1),
                                                    (13,1), (14,1), (15,1), (16,1)]):
            args = call(terms.fields[name], 'product' if count == 1 else 'rows', 2 if count == 1 else 3)
            expected = (symbol('logic'), env['first_row'] + offset) + (() if count == 1 else (count,))
            require(args == expected, 'unsupported product-term layout')
            products[name] = logic.product(args[1], count)
        args = call(terms.fields['preset'], 'product', 2)
        require(args == (symbol('logic'), env['preset_row']), 'unsupported shared preset term')
        products['preset'] = logic.product(args[1])
        for row in range(env['first_row'], env['first_row'] + 17):
            require(row not in used_rows and row != env['preset_row'], 'overlapping cell product terms')
            used_rows.add(row)
        settings = {}
        for name, offset, choices in [
                ('term_routing', 0, dict(combined=0, independent=1, extended=2, feedback_bypass=3)),
                ('output_source', 2, dict(registered=0, combinational=1)),
                ('output_polarity', 3, dict(inverted=0, normal=1))]:
            b = c.one('setting', name)
            shape(b, ('positions', *choices))
            expect(b, choices)
            positions = tuple(b.fields['positions'])
            require(positions == tuple(range(env['output_fuses'] + offset,
                                             env['output_fuses'] + offset + (2 if name == 'term_routing' else 1))),
                    'unsupported output selector coordinates')
            settings[name] = Selector(positions, choices)
            ownership.claim(positions, f'{instance.name}.{name}')
        require(len(c.all('routing')) == 2, 'missing output-routing descriptions')
        comb = c.one('routing', 'combinational')
        shape(comb, ('combined', 'independent', 'extended', 'feedback_bypass'))
        expect(comb, dict(combined='sum(first_sum, middle_sum)', independent='first_sum',
                             extended='sum(first_sum, middle_sum, last_sum)',
                             feedback_bypass='sum(first_sum, selected(last_sum, secondary.type.t))'))
        registered = c.one('routing', 'registered')
        shape(registered, ('combined', 'independent', 'extended', 'feedback_bypass'))
        expect(registered, dict(combined='sum(first_sum, middle_sum)', independent='unsupported',
                               extended='sum(first_sum, middle_sum, last_sum)',
                               feedback_bypass='sum(first_sum, middle_sum)'))
        sums = {name: products[name] for name in ('first_sum', 'middle_sum', 'last_sum')}
        combined = ('sum', sums['first_sum'], sums['middle_sum'])
        extended = (*combined, sums['last_sum'])
        routing = dict(
            combinational=Mux(settings['term_routing'], dict(combined=combined,
                independent=sums['first_sum'], extended=extended,
                feedback_bypass=('sum', sums['first_sum'],
                                 ('selected', sums['last_sum'], 'secondary.type.t')))),
            registered=Mux(settings['term_routing'], dict(combined=combined,
                independent=None, extended=extended, feedback_bypass=combined)))
        registers, feedback = {}, {}
        for role, type_offset, clock_offset in [('primary', 0, 2), ('secondary', 1, 3)]:
            reg = c.one('register', role)
            shape(reg, ('data', 'asynchronous_reset', 'synchronous_preset', 'power_up'), ('clock', 'type'))
            expect(reg, dict(data='select(output_source.registered, routing.registered, middle_sum)'
                             if role == 'primary' else 'last_sum',
                             asynchronous_reset='reset_' + role, synchronous_preset='preset', power_up='low'))
            clk = reg.one('clock')
            shape(clk, ('select_fuse', 'product_term', 'gated_global', 'edge', 'source', 'global_pin'))
            expect(clk, dict(select_fuse=env['register_fuses'] + clock_offset, product_term=0,
                             gated_global=1, edge='rising', source='clock_' + role, global_pin='device.global_clock'))
            typ = reg.one('type')
            shape(typ, ('select_fuse', 'd', 't'))
            expect(typ, dict(select_fuse=env['register_fuses'] + type_offset, d=0, t=1))
            clock_selector = Selector((clk.fields['select_fuse'],), dict(product_term=0, gated_global=1))
            clock = Mux(clock_selector, dict(product_term=products['clock_' + role],
                        gated_global=('and', products['clock_' + role], ('pin', 1))))
            kind = Selector((typ.fields['select_fuse'],), dict(d=0, t=1))
            data = Mux(settings['output_source'], dict(registered=routing['registered'],
                       combinational=products['middle_sum'])) if role == 'primary' else products['last_sum']
            registers[role] = Register(role, data,
                                       clock, kind, products['reset_' + role], products['preset'], 'low')
            ownership.claim(clock_selector.positions, f'{instance.name}.{role}.clock')
            ownership.claim(kind.positions, f'{instance.name}.{role}.type')
        fb = c.one('feedback')
        shape(fb, children=('primary', 'secondary', 'io'))
        for role in ('primary', 'secondary'):
            b = fb.one(role)
            shape(b, ('source', 'high_column', 'low_column', 'alias'))
            expect(b, dict(source='primary.q' if role == 'primary' else
                           'select(term_routing.feedback_bypass, last_sum, secondary.q)'))
            high, low = (integer(b.fields[k], 0, 171, 'feedback column') for k in ('high_column','low_column'))
            alias = call(b.fields['alias'], 'node', 1)[0]
            require(alias == env[role + '_node'], 'register alias differs from cell node')
            source = 'primary.q' if role == 'primary' else Mux(settings['term_routing'],
                dict(combined='secondary.q', independent='secondary.q', extended='secondary.q',
                     feedback_bypass=products['last_sum']))
            feedback[role] = Feedback(source, high, low, alias)
            used_columns.extend([high, low])
        io = fb.one('io')
        shape(io, ('source', 'columns'))
        expect(io, dict(source=f'pin_level({pin})', columns=f'device.pin_columns[{pin}]'))
        cells.append(dict(name=instance.name, pin=pin, first_row=env['first_row'],
                          preset_row=env['preset_row'], config=env['output_fuses'],
                          products=products, selectors=settings, registers=registers, feedback=feedback,
                          routing=routing, output=Mux(settings['output_source'],
                          dict(registered='primary.q', combinational=routing['combinational']))))
    require(sorted(c['pin'] for c in cells) == outputs, 'missing or repeated output cell')
    for role, first in [('secondary',41), ('primary',65)]:
        require(sorted(c['feedback'][role].alias for c in cells) == list(range(first,first+24)),
                'missing or repeated register alias')
    preset_rows = {c['preset_row'] for c in cells}
    require(len(preset_rows) == 8 and not used_rows.intersection(preset_rows) and
            used_rows | preset_rows == set(range(logic.rows)), 'product rows are not completely assigned')
    require(sorted(used_columns) == list(range(logic.columns)), 'input/feedback columns overlap or are missing')
    ownership.complete()
    cells.sort(key=lambda c:c['first_row'])
    variants = {mnemonic: (0,)}
    for v in d.all('variant'):
        shape(v, ('pin_keeper',))
        expect(v, dict(pin_keeper='enabled'))
        require(v.name == 'v2500cppk' + suffix and v.name not in variants, 'invalid ATF2500C variant')
        variants[v.name] = (1,)
    return dict(architecture='atf2500', name=device.name, mnemonic=mnemonic, fuses=d.fields['fuses'],
                array=logic, package=package, columns=columns, cells=cells, variants=variants,
                user_row=user.fields, keeper=keeper.fields, compatibility=compatibility.fields,
                fuse_owners=ownership.owners)


def sections(model, options):
    from . import binary as b
    result = [[] for _ in range(13)]
    cells, logic = [dict(c) for c in model['cells']], model['array']
    count = len(cells)
    simple_pins = [1, 2, 3, 10, *range(17, 24), 30, *range(37, 41)]
    array_slots = 1 + 10 * count + len({c['preset_row'] for c in cells})
    input_slots = 1 + len(simple_pins) + 3 * count
    NONE, D, OE, SP, AR, ARRAY, CK, T, CE, DFB, IO = 0, 1, 2, 3, 7, 14, 15, 27, 31, 34, 37
    # The old ABI retains an inactive high word in unused fuse coordinates.
    inactive = 65536
    attrs = bytearray(152)
    for offset, fmt, value in [(0,'H',4),(2,'B',30),(3,'B',10),(8,'B',40),(10,'B',count),
            (12,'H',2*count),(15,'B',count),(21,'I',model['fuses']),(25,'H',2*count),(27,'B',count),
            (29,'H',logic.rows),(37,'H',350),(39,'H',array_slots),(41,'H',input_slots),
            (43,'B',1),(44,'H',3*count),(49,'B',1)]:
        struct.pack_into('<'+fmt,attrs,offset,value)
    result[3]=[bytes(attrs)]
    result[4]=[b.text_field(name,20)+bytes([code]) for name,code in
               [('d',D),('t',T),('ar',AR),('ck',CK),('oe',OE),('sp',SP),('io',IO),('ce',CE),('dfb',DFB)]]
    def pin(**values):
        result[6].append(b.record(16,b.PIN_FIELDS,dict(fuse=inactive,**values)))
    def node(**values):
        values.setdefault('config',inactive)
        result[7].append(b.record(39,b.NODE_FIELDS,values))
    def array(number, products, p=0):
        node(array=number,pin=p,extension=ARRAY,first=products.first_fuse,width=logic.columns,terms=products.count)
    def control(number,p,extension,**values):
        node(array=number,pin=p,extension=extension,kind=4,**values)
    for line,p in enumerate(simple_pins,1):
        values=dict(line=line,pin=p,kind=3 if p==10 else 2 if p==30 else 12 if p==1 else 22)
        if p not in (10,30): values.update(zip(('high','low'),model['columns'][p]))
        pin(**values)
    for i,c in enumerate(sorted(cells,key=lambda c:-c['pin'])):
        p,line=c['pin'],len(simple_pins)+1+3*i
        high,low=model['columns'][p]
        primary,secondary=c['feedback']['primary'],c['feedback']['secondary']
        result[6].append(b.record(16,b.PIN_FIELDS,dict(line=line,pin=p,kind=7,high=high,low=low,
                                                    value=1,bits=1,fuse=c['config']+2)))
        for extension in (D,T):
            pin(line=line,pin=p,kind=23,variant=extension,high=primary.high_column,low=primary.low_column)
        pin(line=line,pin=p,extension=IO,kind=16,high=high,low=low)
        for step,fb in [(1,primary),(2,secondary)]:
            pin(line=line+step,pin=fb.alias,kind=23,high=fb.high_column,low=fb.low_column)
    number=1
    for c in cells:
        c['array_id']=number
        a,p,config,products=number,c['pin'],c['config'],c['products']
        primary,secondary=c['feedback']['primary'].alias,c['feedback']['secondary'].alias
        array(a,logic.product(c['first_row'],12))
        for ext in (NONE,D,T):
            for polarity,normal in [(6,0),(7,1)]:
                value=dict(array=a,pin=p,extension=ext,kind=polarity,invert=int(ext!=NONE and not normal),
                           select=int(ext==NONE)+2*normal,first=config+2,width=2,has_default=1,sharing=1,active=1)
                if ext==T:value.update(value=1,config=c['registers']['primary'].kind.positions[0],bits=1)
                node(**value)
        for slot,role,term,owner in [(1,AR,'reset_primary',p),(2,CK,'clock_primary',p),(3,OE,'output_enable',p)]:
            array(a+slot,products[term],owner)
            if role==CK:
                fuse=c['registers']['primary'].clock.selector.positions[0]
                control(a+slot,owner,CK,first=fuse,width=1)
                control(a+slot,owner,CE,first=fuse,width=1,select=1)
            else:control(a+slot,owner,role,default=2 if role==OE else 3,
                         first=products[term].first_fuse & 0xffff0000)
        array(a+4,products['last_sum'])
        # The legacy mapper cannot assign the physical bypass as an
        # independent combinational pinnode equation. Keep register data
        # assignments separate from the hardware feedback-routing mux.
        for ext in (D,T):
            node(array=a+4,pin=secondary,extension=ext,kind=2,select=0,
                 value=int(ext!=D),first=config+1,width=1,
                 config=c['registers']['secondary'].kind.positions[0],bits=1,has_default=1,active=1)
        for slot,role,term in [(5,AR,'reset_secondary'),(6,CK,'clock_secondary')]:
            array(a+slot,products[term],secondary)
            if role==CK:
                fuse=c['registers']['secondary'].clock.selector.positions[0]
                control(a+slot,secondary,CK,first=fuse,width=1)
                control(a+slot,secondary,CE,first=fuse,width=1,select=1)
            else:control(a+slot,secondary,AR,default=3,first=products[term].first_fuse & 0xffff0000)
        array(a+7,products['middle_sum'])
        for ext in (D,T):
            node(array=a+7,pin=primary,extension=ext,kind=2,select=1,value=int(ext==T),first=config,width=1,
                 config=c['registers']['primary'].kind.positions[0],bits=1,has_default=1,active=1)
        for slot,role,term in [(8,AR,'reset_primary'),(9,CK,'clock_primary')]:
            array(a+slot,products[term],primary)
            if role==CK:
                fuse=c['registers']['primary'].clock.selector.positions[0]
                control(a+slot,primary,CK,first=fuse,width=1)
                control(a+slot,primary,CE,first=fuse,width=1,select=1)
            else:control(a+slot,primary,AR,default=3,first=products[term].first_fuse & 0xffff0000)
        number+=10
        if c['first_row']+17==c['preset_row']:
            group=[x for x in cells if x['preset_row']==c['preset_row']]
            array(number,products['preset'])
            for role in ('pin','secondary','primary'):
                for x in group:
                    x['preset_array']=number
                    owner=x['pin'] if role=='pin' else x['feedback'][role].alias
                    control(number,owner,SP,default=3,first=products['preset'].first_fuse & 0xffff0000)
            number+=1
    require(number==array_slots and all('preset_array' in c for c in cells), 'invalid shared-preset grouping')
    def link(line,p,ext,group,member=0,source=0):
        result[10].append(b.record(13,b.LINK_FIELDS,dict(line=line,pin=p,extension=ext,group=group,
                                                      member=member,source_pin=source,compiler_link=1)))
    group=0
    for function in (CK,AR,OE,SP):
        for role in (('pin',) if function==OE else ('pin','secondary','primary')):
            for c in cells:
                group+=1
                owner=c['pin'] if role=='pin' else c['feedback'][role].alias
                base=c['array_id']+{'pin':0,'secondary':4,'primary':7}[role]
                control_id=c['preset_array'] if function==SP else base+({CK:2,AR:1,OE:3}[function])
                link(control_id,owner,function,group,1)
                if function==CK:link(1,1,CK,group,1,1)
                for ext in (D,T):link(base,owner,ext,group)
                if function==CK:link(control_id,owner,CE,group)
                if function==OE:link(base,owner,NONE,group)
    # Named defaults retain the native compiler's initial mode selection.
    initial=[(c['config']+1,c['config']+2,0) for c in cells]
    initial += [(c['registers']['secondary'].kind.positions[0],)*2+(0,) for c in cells]
    initial += [(model['keeper']['fuse'],model['keeper']['fuse'],options[0]),
                (model['user_row']['first_fuse'],model['user_row']['first_fuse']+model['user_row']['bits']-1,1),
                (71810,71815,0)]
    result[9]=[struct.pack('<IIB',*r) for r in initial]
    # A geometry-derived plot uses the existing VM without a copied
    # instruction stream. It displays every logic and configuration fuse.
    plot=[]
    def emit(op,group=0,a=0,bv=0,c=0,d=0,text=''):
        plot.append(b.text_field(text,9)+struct.pack('<BBIIII',op,group,a,bv,c,d))
    emit(9)
    for group,first,rows,width in [(1,0,logic.rows,logic.columns),(2,logic.fuse_count,1,model['fuses']-logic.fuse_count)]:
        emit(3,group,1,rows,0,first)
        emit(7,group,0)
        emit(5,group,0,0,1,width,'x-')
        emit(10,group,1)
        emit(8,group,0,0,0,width)
    emit(2)
    result[8]=plot
    result=b.package_sections(model,result)
    if model['package'].ground or model['package'].supply:
        attrs=bytearray(result[3][0])
        attrs[3]=min((attrs[3],*model['package'].supply))
        result[3]=[bytes(attrs)]
        extra=[(p,2) for p in model['package'].ground]+[(p,3) for p in model['package'].supply]
        for line,(p,kind) in enumerate(sorted(extra),input_slots):
            pin(line=line,pin=p,kind=kind)
    return result
