"""Exercise EDIF connectivity and primitive semantics through fitted fuse maps."""
import itertools
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from defects import fuses


def document(ports, models, instances, nets, contents_extra='', libraries=''):
    def interface(items):
        return '\n'.join(f'(port {name} (direction {direction}))'
                         for name, direction in items)

    external = '\n'.join(
        f'(cell {name} (cellType GENERIC) (view logic (viewType NETLIST) '
        f'(interface {interface(pins)})))' for name, pins in models.items())
    cells = '\n'.join(f'(instance {name} (viewRef logic (cellRef {model} '
                      '(libraryRef primitives))))' for name, model in instances)
    connections = []
    for number, refs in enumerate(nets):
        joined = ' '.join(f'(portRef {port}' +
                          (f' (instanceRef {instance})' if instance else '') + ')'
                          for port, instance in refs)
        connections.append(f'(net n{number} (joined {joined}))')
    return (f'(edif test (edifVersion 2 0 0) (edifLevel 0) '
            f'(keywordMap (keywordLevel 0)) '
            f'(external primitives (edifLevel 0) (technology (numberDefinition)) {external}) '
            f'{libraries} '
            f'(library work (edifLevel 0) (technology (numberDefinition)) '
            f'(cell top (cellType GENERIC) (view logic (viewType NETLIST) '
            f'(interface {interface(ports)}) '
            f'(contents {cells} {" ".join(connections)} {contents_extra})))) '
            f'(design test (cellRef top (libraryRef work))))\n')


def one_cell(model, inputs, outputs=('Q',)):
    ports = [(name.lower(), 'INPUT') for name in inputs]
    ports += [(name.lower(), 'OUTPUT') for name in outputs]
    pins = [(name, 'INPUT') for name in inputs]
    pins += [(name, 'OUTPUT') for name in outputs]
    nets = [[(name.lower(), None), (name, 'gate')] for name in (*inputs, *outputs)]
    reference = model.split()[1] if model.startswith('(rename ') else model
    return document(ports, {model: pins}, [('gate', reference)], nets)


def functions(path):
    """Canonical post-fit truth functions, including register control terms."""
    inputs, outputs = [], []
    cupl = False
    result = {}
    for line in path.read_text().splitlines():
        line = line.strip()
        if line.startswith('#$ TOOL CUPL'): cupl = True
        elif line.startswith('.ilb '): inputs = line.split()[1:]
        elif line.startswith('.ob '):
            # CUPL calls a high-level latch enable LE; ABEL calls it LH.
            outputs = [name[:-3] + '.LH' if cupl and name.endswith('.LE') else name
                       for name in line.split()[1:]]
            result = {name: {'0': set(), '1': set()} for name in outputs}
        elif line and line[0] in '01-':
            left, right = line.split() if inputs else ('', line)
            term = tuple(sorted((name, value) for name, value in zip(inputs, left) if value != '-'))
            for name, value in zip(outputs, right):
                if value in '01': result[name][value].add(term)
    truth = {}
    for name, sets in result.items():
        terms = sets['1'] or sets['0']
        names = tuple(sorted({n for term in terms for n, value in term}))
        values = []
        for bits in itertools.product('01', repeat=len(names)):
            state = dict(zip(names, bits))
            value = any(all(state[n] == bit for n, bit in term) for term in terms)
            values.append(not value if not sets['1'] and sets['0'] else value)
        truth[name] = (names, tuple(values))
    return truth


def tt2(inputs, outputs, evaluate, pins):
    rows = []
    cubes = [set() for _ in outputs]
    for values in itertools.product((0, 1), repeat=len(inputs)):
        result = evaluate(*values)
        if not isinstance(result, tuple):
            result = (result,)
        for index, value in enumerate(result):
            if value:
                cubes[index].add(''.join(map(str, values)))
    for index, terms in enumerate(cubes):
        primes = set()
        while terms:
            combined, used = set(), set()
            for a, b in itertools.combinations(sorted(terms), 2):
                differences = [i for i, (x, y) in enumerate(zip(a, b)) if x != y]
                if len(differences) != 1:
                    continue
                i = differences[0]
                if a[i] != '-' and b[i] != '-':
                    combined.add(a[:i] + '-' + a[i+1:])
                    used.update((a, b))
            primes.update(terms - used)
            terms = combined
        for term in sorted(primes):
            rows.append(term + ' ' + '~' * index + '1' + '~' * (len(outputs) - index - 1))
    return (f'#$ TOOL CUPL\n#$ PINS {len(pins)} ' +
            ' '.join(f'{name}+:{pin}' for name, pin in pins.items()) +
            f'\n.i {len(inputs)}\n.o {len(outputs)}\n.type f\n.ilb ' +
            ' '.join(inputs) + '\n.ob ' + ' '.join(outputs) +
            f'\n.p {len(rows)}\n' + '\n'.join(rows) + '\n.e\n')


def main():
    bindir = Path(sys.argv[1]).resolve()
    suffix = '.exe' if os.name == 'nt' else ''
    checks = 0
    with tempfile.TemporaryDirectory(prefix='edif reader ') as temp:
        root = Path(temp)
        empty = root / 'empty'
        empty.mkdir()
        env = dict(os.environ, FITTERDIR=str(empty))
        env['LINCUPL_FIT150X_MODE'] = sys.argv[2] if len(sys.argv) > 2 else 'legacy'

        def fit(source, fmt, part, pins=None, options=(), success=True, message=None,
                basename='design', pin_options=None):
            nonlocal checks
            work = root / str(checks)
            checks += 1
            work.mkdir()
            path = work / ('design.edf' if fmt == 'EDIF' else 'design.tt2')
            path.write_text(source)
            if pins:
                (work / (basename + '.pin')).write_text(
                    ''.join(f'{n} : {p}\n' for n, p in pins.items()))
            if pin_options is None:
                pin_options = ['-pinfile', basename + '.pin'] if pins else []
            package = 84 if part == 1508 else 44
            result = subprocess.run(
                [str(bindir / (f'fit{part}' + suffix)), '-i', path.name,
                 '-ifmt', fmt, '-dev', f'P{part}C{package}', '-JTAG', 'OFF',
                 *(['-o', basename] if basename != 'design' else []), *pin_options, *options],
                cwd=work, env=env, capture_output=True, text=True, timeout=20)
            output = result.stdout + result.stderr
            if success:
                assert result.returncode == 0, (source, options, output)
                assert 'Design fits successfully' in output, output
                return fuses(work / (basename + '.jed')), functions(work / (basename + '.tt3'))
            assert result.returncode > 0, (result.returncode, source, output)
            assert not (work / (basename + '.jed')).exists(), output
            if message:
                assert message in output, (message, output)

        def compare(model, ins, function, out='Q'):
            names = [p.lower() for p in ins]
            pins = dict(zip(names, (2, 1, 5, 6, 8, 9, 11, 12)))
            pins[out.lower()] = 4
            source = one_cell(model, ins, (out,))
            reference = tt2(names, [out.lower()], function, pins)
            for part in (1502, 1504, 1508):
                actual = fit(source, 'EDIF', part, pins)
                expected = fit(reference, 'TT', part)
                # Product-term order may change the fuse addresses without
                # changing any fitted function.
                assert actual[1] == expected[1], (model, part, actual[1], expected[1])

        compare('AND2', ('A', 'B'), lambda a, b: a & b)
        compare('OR2', ('A', 'B'), lambda a, b: a | b)
        compare('NAND2', ('A', 'B'), lambda a, b: not (a & b), 'QN')
        compare('NOR2', ('A', 'B'), lambda a, b: not (a | b), 'QN')
        compare('XOR2', ('A', 'B'), lambda a, b: a ^ b)
        compare('XNOR2', ('A', 'B'), lambda a, b: not (a ^ b), 'QN')
        compare('AND3I2', ('A', 'BN', 'CN'), lambda a, bn, cn: a and not bn and not cn)
        compare('OR3I1', ('A', 'B', 'CN'), lambda a, b, cn: a or b or not cn)
        compare('MUX2', ('S0', 'M0', 'M1'), lambda s, a, b: b if s else a)
        compare('(rename gate "$_MUX_")', ('S', 'A', 'B'), lambda s, a, b: b if s else a, 'Y')
        compare('(rename gate "$_ANDNOT_")', ('A', 'B'), lambda a, b: a and not b, 'Y')
        compare('(rename gate "$_ORNOT_")', ('A', 'B'), lambda a, b: a or not b, 'Y')
        compare('(rename gate "$_NMUX_")', ('S', 'A', 'B'), lambda s, a, b: not (b if s else a), 'Y')
        compare('(rename gate "$_AOI3_")', ('A', 'B', 'C'), lambda a, b, c: not ((a and b) or c), 'Y')
        compare('(rename gate "$_OAI4_")', ('A', 'B', 'C', 'D'), lambda a, b, c, d: not ((a or b) and (c or d)), 'Y')
        compare('BUF', ('A',), lambda a: a)
        compare('INV', ('A',), lambda a: not a, 'QN')
        for part in (1502, 1504, 1508):
            for model, value in (('GND', False), ('VCC', True)):
                assert fit(one_cell(model, ()), 'EDIF', part)[1] == {'q': ((), (value,))}
            source = one_cell('DFF', ('CLK', 'D'), ('QN',))
            pins = {'clk': 43, 'd': 2, 'qn': 4}
            expected = fit(tt2(['clk', 'd'], ['qn.REG', 'qn.C'],
                               lambda c, d: (not d, c), pins), 'TT', part)
            actual = fit(source, 'EDIF', part, pins)
            assert actual[1] == expected[1], (part, actual[1], expected[1])
            both = fit(one_cell('DFF', ('CLK', 'D'), ('Q', 'QN')), 'EDIF', part)[1]
            assert both == {'q.D': (('d',), (False, True)),
                            'q.C': (('clk',), (False, True)),
                            'qn': (('q.Q',), (True, False))}, both
            # Complementing state also exchanges async reset and preset.
            for model, control, extension in (('DFFAR', 'AR', 'AP'), ('DFFAS', 'AS', 'AR')):
                pins = {'clk': 43, 'd': 2, control.lower(): 1, 'qn': 4}
                source = one_cell(model, ('CLK', 'D', control), ('QN',))
                reference = tt2(['clk', 'd', control.lower()], ['qn.REG', 'qn.C', 'qn.' + extension],
                                lambda c, d, r: (not d, c, r), pins)
                actual = fit(source, 'EDIF', part, pins)
                expected = fit(reference, 'TT', part)
                assert actual == expected, (model, part, actual[1], expected[1])

        # Every edge/reset/enable polarity must survive primitive import.
        for clock, reset, value, enable in itertools.product('PN', 'PN', '01', 'PN'):
            model = f'(rename ff "$_DFFE_{clock}{reset}{value}{enable}_")'
            ins = ('C', 'D', 'R', 'E')
            pins = {'c': 1, 'd': 2, 'r': 5, 'e': 6, 'q': 4}
            source = one_cell(model, ins)
            reference = tt2(['c', 'd', 'r', 'e'],
                            ['q.REG', 'q.C', 'q.AP' if value == '1' else 'q.AR', 'q.CE'],
                            lambda c, d, r, e: (d, c if clock == 'P' else not c,
                                               r if reset == 'P' else not r,
                                               e if enable == 'P' else not e), pins)
            for part in (1502, 1504, 1508):
                actual = fit(source, 'EDIF', part, pins)
                expected = fit(reference, 'TT', part)
                assert actual[1] == expected[1], (model, part, actual[1], expected[1])

        # Resolve a child view in another library and flatten its port
        # boundary. References use EDIF identifiers, never rename strings.
        hierarchy = '''(library children (edifLevel 0)
          (cell (rename child "display child") (cellType GENERIC)
            (view nested (viewType NETLIST)
              (interface (port a (direction INPUT)) (port b (direction INPUT))
                         (port q (direction OUTPUT)))
              (contents
                (instance inside (viewRef logic (cellRef AND2 (libraryRef primitives))))
                (net a (joined (portRef a) (portRef A (instanceRef inside))))
                (net b (joined (portRef b) (portRef B (instanceRef inside))))
                (net q (joined (portRef q) (portRef Q (instanceRef inside))))))))'''
        flat = one_cell('AND2', ('A', 'B'))
        nested = flat.replace('(library work', hierarchy + '(library work')
        nested = nested.replace('(instance gate (viewRef logic (cellRef AND2 (libraryRef primitives))))',
                                '(instance gate (viewRef nested (cellRef child (libraryRef children))))')
        pins = {'a': 2, 'b': 1, 'q': 4}
        assert fit(flat, 'EDIF', 1502, pins) == fit(nested, 'EDIF', 1502, pins)
        assert fit(flat, 'EDIF', 1502, pins)[0] == fit(flat.swapcase(), 'EDIF', 1502,
                                                     {n.upper(): p for n, p in pins.items()})[0]

        bus = document([('(array data 2)', 'INPUT'), ('q', 'OUTPUT')],
                       {'AND2': [('A', 'INPUT'), ('B', 'INPUT'), ('Q', 'OUTPUT')]},
                       [('gate', 'AND2')],
                       [[('(member data 0)', None), ('A', 'gate')],
                        [('(member data 1)', None), ('B', 'gate')],
                        [('q', None), ('Q', 'gate')]])
        fit(bus, 'EDIF', 1502, {'data[0]': 2, 'data[1]': 1, 'q': 4})
        fit(bus.replace('(member data 1)', '(member data 2)'), 'EDIF', 1502,
            success=False, message='integer must be')
        array_ports = [('(array a 2)', 'INPUT'), ('(array b 2)', 'INPUT'), ('(array q 2)', 'OUTPUT')]
        array_models = {'AND2': [('A', 'INPUT'), ('B', 'INPUT'), ('Q', 'OUTPUT')]}
        array_nets = [[(f'(member {p.lower()} {bit})', None), (p, f'(member gates {bit})')]
                      for bit in range(2) for p in ('A', 'B', 'Q')]
        array = document(array_ports, array_models, [('(array gates 2)', 'AND2')], array_nets)
        expanded = document(array_ports, array_models, [('gate0', 'AND2'), ('gate1', 'AND2')],
                            [[(f'(member {p.lower()} {bit})', None), (p, f'gate{bit}')]
                             for bit in range(2) for p in ('A', 'B', 'Q')])
        pins = {'a[0]': 2, 'a[1]': 1, 'b[0]': 6, 'b[1]': 8, 'q[0]': 4, 'q[1]': 5}
        assert fit(array, 'EDIF', 1502, pins) == fit(expanded, 'EDIF', 1502, pins)

        # Whole-array connections use the same member order at each boundary.
        array_child = '''(library buses (edifLevel 0)
          (cell bus (cellType GENERIC) (view logic (viewType NETLIST)
            (interface (port (array a 2) (direction INPUT))
                       (port (array q 2) (direction OUTPUT)))
            (contents (net (array link 2) (joined (portRef a) (portRef q)))))))'''
        bus = document([('(array a 2)', 'INPUT'), ('(array q 2)', 'OUTPUT')], {}, [], [],
                       contents_extra='''(instance child (viewRef logic (cellRef bus (libraryRef buses))))
                       (net (array ai 2) (joined (portRef a) (portRef a (instanceRef child))))
                       (net (array qo 2) (joined (portRef q) (portRef q (instanceRef child))))''',
                       libraries=array_child)
        pins = {'a[0]': 2, 'a[1]': 1, 'q[0]': 4, 'q[1]': 5}
        expected = {f'q[{bit}]': ((f'a[{bit}]',), (False, True)) for bit in range(2)}
        assert fit(bus, 'EDIF', 1502, pins)[1] == expected
        fit(bus.replace('(array ai 2)', '(array ai 3)'), 'EDIF', 1502,
            success=False, message='array widths differ')

        # Source constraints and explicit pin files must agree.
        source = one_cell('AND2', ('A', 'B'))
        located = source.replace('(port a (direction INPUT))',
                                 '(port a (direction INPUT) (property LOC (string "P2")))')
        fit(located, 'EDIF', 1502, {'a': 1}, success=False, message='conflicting assignments')
        named = source.replace('(port a (direction INPUT))',
                               '(port (rename a "input%32%pad") (direction INPUT))')
        fit(named, 'EDIF', 1502, {'"input pad"': 2, 'b': 1, 'q': 4})
        fit(named.replace('input%32%pad', 'input:pad'), 'EDIF', 1502,
            {'"input:pad"': 2, 'b': 1, 'q': 4})
        pins = {'a': 2, 'b': 1, 'q': 4}
        expected = fit(source, 'EDIF', 1502, pins)
        for pin_options in (['-pinfile'], ['-pinfile', 'ON'], ['-pinfile', 'fitted design.pin']):
            assert fit(source, 'EDIF', 1502, pins, basename='fitted design',
                       pin_options=pin_options) == expected
        fit(source, 'EDIF', 1502, options=('-pinfile', 'missing.pin'),
            success=False, message='cannot open pin file')
        # Pin files are opt-in, including when an old output file is present.
        fit(source, 'EDIF', 1502, {'unknown': 2}, pin_options=[])

        custom = root / 'custom.lib'
        custom.write_text('GATE 0 0 CUSTOM 1 50 RESULT = !LEFT + RIGHT;\n'
                          'PIN * UNKNOWN 1 1 1 0 1 0\n')
        custom_source = one_cell('CUSTOM', ('LEFT', 'RIGHT'), ('RESULT',))
        custom_pins = {'left': 2, 'right': 1, 'result': 4}
        actual = fit(custom_source, 'EDIF', 1502, custom_pins, ('-lib', str(custom)))
        expected = fit(tt2(['left', 'right'], ['result'], lambda a, b: not a or b,
                           custom_pins), 'TT', 1502)
        assert actual[1] == expected[1], (actual[1], expected[1])

        for kind, clock, extension in (('DG', 'RECK', 'REG'), ('TG', 'RECK', 'T'), ('LG', 'L1', 'L')):
            custom.write_text(f'PGATE 0 0 {kind} CUSTOM 1 CK {clock} RST * * * DATA OUT * *\n'
                              'RELATED_OUTPUT_PIN OUT 10\nPIN * UNKNOWN 1 1 1 0 1 0 0 0\n')
            source = one_cell('CUSTOM', ('CK', 'RST', 'DATA'), ('OUT',))
            pins = {'ck': 1, 'rst': 5, 'data': 2, 'out': 4}
            reference = tt2(['ck', 'rst', 'data'], [f'out.{extension}',
                            'out.LH' if kind == 'LG' else 'out.C', 'out.AR'],
                            lambda c, r, d: (d, c, r), pins)
            for part in (1502, 1504, 1508):
                actual = fit(source, 'EDIF', part, pins, ('-lib', str(custom)))
                expected = fit(reference, 'TT', part)
                assert actual[1] == expected[1], (kind, part, actual[1], expected[1])

        # The pad feedback of a bidirectional buffer is independent of D.
        bidir = document([('a', 'INPUT'), ('en', 'INPUT'), ('pad', 'INOUT'), ('q', 'OUTPUT')],
                         {'BIBUF': [('A', 'INPUT'), ('EN', 'INPUT'), ('PAD', 'INOUT'), ('Q', 'OUTPUT')]},
                         [('gate', 'BIBUF')],
                         [[(p.lower(), None), (p, 'gate')] for p in ('A', 'EN', 'PAD', 'Q')])
        for part in (1502, 1504, 1508):
            actual = fit(bidir, 'EDIF', part, {'a': 2, 'en': 1, 'pad': 4, 'q': 5})
            expected = {'pad': (('a',), (False, True)),
                        'pad.OE': (('en',), (False, True)),
                        'q': (('pad.PIN',), (False, True))}
            assert actual[1] == expected, actual[1]

        source = one_cell('AND2', ('A', 'B'))
        fit('', 'EDIF', 1502, success=False, message='unexpected end of input')
        fit(source.replace('(direction INPUT)', '(direction INPUT) (property INIT (string "0"))', 1),
            'EDIF', 1502, success=False, message='initial state is not supported')
        fit(source.replace('(viewRef logic (cellRef AND2 (libraryRef primitives)))',
                           '(viewRef logic (cellRef top (libraryRef work)))'),
            'EDIF', 1502, success=False, message='recursive')
        fit(named.replace('%32%', '%bad%'), 'EDIF', 1502,
            success=False, message='invalid string escape')
        fit(source[:-3], 'EDIF', 1502, success=False, message='unterminated')
        fit(source + '(extra)', 'EDIF', 1502, success=False, message='one edif document')
        fit(source.replace('AND2', '"unterminated', 1), 'EDIF', 1502,
            success=False, message='unterminated string')
        fit(source.replace('(portRef a)', '(portRef a) (portRef b)'), 'EDIF', 1502,
            success=False, message='multiple drivers')
        fit(source.replace('(net n1', '(net n0'), 'EDIF', 1502,
            success=False, message='duplicate net')
        fit(source.replace('(edifLevel 0)', '(edifLevel 1)', 1), 'EDIF', 1502,
            success=False, message='integer must be')
        fit(source.replace('(portRef A ', '(portRef MISSING '), 'EDIF', 1502,
            success=False, message='unknown port')
        fit(source.replace('(instanceRef gate)', '(instanceRef missing)', 1), 'EDIF', 1502,
            success=False, message='unknown instance')
        fit(source.replace('(cellRef AND2 ', '(cellRef missing '), 'EDIF', 1502,
            success=False, message='unresolved')
        fit(source.replace('AND2', 'UNKNOWN'), 'EDIF', 1502,
            success=False, message='unknown primitive')
        fit(source.replace('(portRef A (instanceRef gate))', ''), 'EDIF', 1502,
            success=False, message='unconnected input')
        fit(source.replace('(portRef Q (instanceRef gate))',
                           '(portRef Q (instanceRef gate)) (portRef a)'), 'EDIF', 1502,
            success=False, message='multiple drivers')
        fit(source, 'EDIF', 1502, {'a': 2, 'b': 2}, success=False, message='assigned to both')
        fit(source, 'EDIF', 1502, {'missing': 2}, success=False, message='unknown port')
        cycle = document([('q', 'OUTPUT')], {'INV': [('A', 'INPUT'), ('QN', 'OUTPUT')]},
                         [('gate', 'INV')], [[('A', 'gate'), ('QN', 'gate'), ('q', None)]])
        fit(cycle, 'EDIF', 1502, success=False, message='combinational cycle')
        fit('(' * 140 + 'x' + ')' * 140, 'EDIF', 1502,
            success=False, message='nested too deeply')
    print(f'PASS: {checks} EDIF/TT2 fitting and rejection checks')


if __name__ == '__main__':
    main()
