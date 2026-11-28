"""Compare original RTL with fitted TT3 equations and decoded JEDEC maps.

Exercise synthesis, EDIF import, fitting and fuse-map simulation without
asserting timing equivalence. Yosys and Icarus are optional.
"""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


def run(args, directory, env=None, expected=0):
    result = subprocess.run(args, cwd=directory, env=env, capture_output=True,
                            text=True, errors='replace', timeout=60)
    output = result.stdout + result.stderr
    diagnostic = output if len(output) <= 6000 else output[:4000] + '\n...\n' + output[-2000:]
    assert result.returncode == expected, (directory, args, result.returncode, diagnostic)
    return output


def span(port):
    width, offset = len(port['bits']), port.get('offset', 0)
    if width == 1 and offset == 0:
        return ''
    left, right = offset + width - 1, offset
    if port.get('upto'):
        left, right = right, left
    return f'[{left}:{right}] '


def scalar_ports(ports):
    return {name + (f'[{i}]' if span(port) else ''): port['direction']
            for name, port in ports.items()
            for i in range(port.get('offset', 0), port.get('offset', 0) + len(port['bits']))}


def readback(path, ports):
    lines = path.read_text().splitlines()
    fields = {line.split()[0]: line.split()[1:] for line in lines if line.startswith('.')}
    inputs, outputs = fields['.ilb'], fields['.ob']
    rows = [line.split() for line in lines if line and line[0] in '-01']
    assert len(rows) == int(fields['.p'][0])
    assert all(len(a) == len(inputs) and len(b) == len(outputs) for a, b in rows)
    scalars = scalar_ports(ports)
    groups = {}
    for name in outputs:
        if '.' in name:
            base, extension = name.rsplit('.', 1)
            groups.setdefault(base, {})[extension] = name
    registers = {name for name, g in groups.items() if {'D', 'T', 'L'} & g.keys()}
    names = set(scalars) | {name.split('.')[0] for name in inputs + outputs}
    signals = {name: f'n{i}' for i, name in enumerate(sorted(names))}
    terms = {name: f'f{i}' for i, name in enumerate(outputs)}

    def signal(name):
        if name.endswith('.Q') or name.endswith('.FB'):
            return signals[name.rsplit('.', 1)[0]]
        assert '.' not in name, ('unsupported feedback extension', name)
        return signals[name]

    declarations = [port['direction'] + ' ' + span(port) + name for name, port in ports.items()]
    code = ['module fitted(' + ', '.join(declarations) + ');']
    for name, wire in signals.items():
        code.append(('reg ' if name in registers else 'wire ') + wire + ';')
    for name, direction in scalars.items():
        assert direction != 'inout'
        if direction == 'input':
            code.append(f'assign {signals[name]} = {name};')
        else:
            group = groups.get(name, {})
            value = signals[name]
            if name in registers and 'OE' in group:
                value = f"{terms[group['OE']]} ? {value} : 1'bz"
            code.append(f'assign {name} = {value};')
    for index, name in enumerate(outputs):
        sets = {'0': [], '1': []}
        for left, right in rows:
            if right[index] not in sets:
                continue
            literals = [('~' if value == '0' else '') + signal(n)
                        for n, value in zip(inputs, left) if value != '-']
            sets[right[index]].append('(' + ' & '.join(literals) + ')' if literals else "1'b1")
        # A fitter can emit only the OFF set of an output.
        if sets['1']:
            expr = ' | '.join(sets['1'])
        elif sets['0']:
            expr = '~(' + ' | '.join(sets['0']) + ')'
        else:
            expr = "1'b0"
        if fields.get('.phase', ['1' * len(outputs)])[0][index] == '0':
            expr = '~(' + expr + ')'
        code.append(f'wire {terms[name]} = {expr};')
    for name in outputs:
        if '.' not in name:
            group = groups.get(name, {})
            expr = terms[name]
            if 'OE' in group:
                expr = f"{terms[group['OE']]} ? {expr} : 1'bz"
            code.append(f'assign {signals[name]} = {expr};')
    for name in sorted(registers):
        group = groups[name]
        reset, preset = group.get('AR'), group.get('AP')
        controls = [term for term in (reset, preset) if term]
        if 'L' in group:
            enable = terms[group.get('LH', group.get('LE'))]
            if 'LE' in group:
                enable = '~' + enable
            code.append('always @*')
            for term, value in ((reset, 0), (preset, 1)):
                if term:
                    code.append(f"if ({terms[term]}) {signals[name]} = 1'b{value}; else")
            code.append(f"if ({enable}) {signals[name]} = {terms[group['L']]};")
        else:
            events = ['posedge ' + terms[group['C']]] + ['posedge ' + terms[t] for t in controls]
            code.append('always @(' + ' or '.join(events) + ')')
            for term, value in ((reset, 0), (preset, 1)):
                if term:
                    code.append(f"if ({terms[term]}) {signals[name]} <= 1'b{value}; else")
            if 'CE' in group:
                code.append(f"if ({terms[group['CE']]})")
            value = terms[group.get('D', group.get('T'))]
            if 'T' in group:
                value = signals[name] + ' ^ ' + value
            code.append(f'{signals[name]} <= {value};')
    return '\n'.join(code + ['endmodule']) + '\n'


def testbench(top, ports):
    code = ['module tb;', 'integer i;']
    inputs = [name for name, port in ports.items() if port['direction'] == 'input']
    outputs = [name for name in ports if name not in inputs]
    for name, port in ports.items():
        code.append(('reg ' if name in inputs else 'wire ') + span(port) +
                    (name if name in inputs else f'gold_{name}, actual_{name}') + ';')
    for label, module in (('gold', top), ('actual', 'fitted')):
        connections = [f'.{name}({name if name in inputs else label + "_" + name})' for name in ports]
        code.append(f'{module} {label}(' + ', '.join(connections) + ');')
    scalars = scalar_ports(ports)
    observed = [name if direction == 'input' else 'gold_' + name
                for name, direction in scalars.items()]
    trace = '$display("VECTOR %b", {' + ', '.join(observed) + '});'
    code += ['task check;', 'begin']
    for name in outputs:
        code.append(f'if (gold_{name} !== actual_{name}) $fatal(1, "{top}/{name} at step %0d", i);')
    code += [trace, 'end', 'endtask', 'initial begin'] + [name + '=0;' for name in inputs]
    # Seed physical inputs before the first edge. Registers may still be
    # unknown here; every subsequent checked sample must be determinate.
    code += ['#1;', trace]
    if top in ('and2', 'adder', 'tristate', 'offset'):
        width = sum(len(ports[name]['bits']) for name in inputs)
        code += [f'for (i=0; i<{1 << width}; i=i+1) begin',
                 '{' + ','.join(inputs) + '}=i;', '#5; check;', 'end']
    elif top == 'latch':
        code += ['gate=1; d=0; #5; check;', 'for (i=0;i<2000;i=i+1) begin',
                 'gate=$random; d=$random; #5; check;', 'end']
    else:
        reset = {'counter': 'rst', 'preset': 'rst_n', 'fsm': 'reset'}[top]
        idle, edge = (1, 0) if top == 'preset' else (0, 1)
        active = 0 if top == 'preset' else 1
        code += [f'clk={idle}; {reset}={active}; #5; clk={edge}; #5; check;',
                 'for (i=0;i<2000;i=i+1) begin', f'clk={idle}; #5;']
        for name in inputs:
            if name == 'clk': continue
            if name == reset:
                code.append(name + '=' + ('!' if active == 0 else '') +
                            '((i>64) && (($random & 31)==0));')
            elif top == 'counter' and name == 'en':
                code.append('en=(i<64) ? 1 : $random;')
            else:
                code.append(name + '=$random;')
        code += [f'#5; check; clk={edge}; #5; check;', 'end']
    return '\n'.join(code + ['$finish;', 'end', 'endmodule']) + '\n'


def simulate_jedec(bindir, work, ports, trace, env):
    pins = next(line for line in (work / 'design.tt3').read_text().splitlines()
                if line.startswith('#$ PINS'))
    mapping = {name: int(pin) for name, pin in re.findall(r'([^\s:+-]+)[+-]?:(\d+)', pins)}
    scalars = scalar_ports(ports)
    assert set(scalars) <= mapping.keys(), (scalars, mapping)
    # N observes an output without checking its level. Use it only for
    # RTL state that is unknown in the initial clock-seeding sample.
    levels = {'0': 'L', '1': 'H', 'z': 'Z', 'x': 'N'}
    vectors = []
    for index, row in enumerate(re.findall(r'^VECTOR ([01xz]+)$', trace, re.M)):
        assert len(row) == len(scalars), row
        vector = []
        for bit, direction in zip(row, scalars.values()):
            if direction == 'input':
                assert bit in '01', row
                vector.append(bit)
            else:
                assert bit != 'x' or index == 0, row
                vector.append(levels[bit])
        vectors.append(''.join(vector))
    assert len(vectors) > 1, trace
    order = ','.join(f'P{mapping[name]}' for name in scalars)
    header = f'ORDER:{order};\nVECTORS:\n'
    (work / 'design.si').write_text(header + '\n'.join(vectors) + '\n')
    assert not (work / 'design.abs').exists()
    suffix = '.exe' if os.name == 'nt' else ''
    args = [str(bindir / ('csim' + suffix)), '-fl', '-u',
            str(bindir.parent / 'share/lincupl/lincupl.dl'), 'design']
    run(args, work, env)
    # A wrong expectation must fail, including when the output is floating.
    index = next(i for i, direction in enumerate(scalars.values()) if direction == 'output')
    row = vectors[-1]
    wrong = 'L' if row[index] == 'H' else 'H'
    vectors[-1] = row[:index] + wrong + row[index + 1:]
    (work / 'design.si').write_text(header + '\n'.join(vectors) + '\n')
    assert 'user expected' in run(args, work, env, expected=1)


def main():
    tools = {name: shutil.which(name) for name in ('yosys', 'iverilog', 'vvp')}
    if not all(tools.values()):
        print('SKIP: Yosys and Icarus Verilog are required')
        return 77
    bindir = Path(sys.argv[1]).resolve()
    source = Path(__file__).resolve().parent / 'edif/designs.v'
    suffix = '.exe' if os.name == 'nt' else ''
    checks = 0
    with tempfile.TemporaryDirectory(prefix='lincupl-edif-yosys-') as tmp:
        root = Path(tmp)
        empty = root / 'empty'
        empty.mkdir()
        env = dict(os.environ, FITTERDIR=str(empty))
        env['LINCUPL_FIT150X_MODE'] = sys.argv[2] if len(sys.argv) > 2 else 'legacy'
        for top in ('and2', 'adder', 'counter', 'preset', 'latch', 'tristate', 'fsm', 'offset'):
            work = root / top
            work.mkdir()
            shutil.copyfile(source, work / 'designs.v')
            run([tools['yosys'], '-Q', '-T', '-p',
                 f'read_verilog designs.v; tribuf; synth -top {top} -flatten -noabc; '
                 'dffunmap; abc -g simple; clean; write_json design.json; '
                 'write_edif -pvector bra design.edf'], work)
            ports = json.loads((work / 'design.json').read_text())['modules'][top]['ports']
            (work / 'tb.v').write_text(testbench(top, ports))
            targets = [(1502, 'PLCC44', 'AS'), (1504, 'PLCC44', 'AS'), (1508, 'PLCC84', 'AS')]
            if env['LINCUPL_FIT150X_MODE'] == 'extended':
                targets += [(1502, 'TQFP44', 'BE'), (1504, 'TQFP44', 'BE'), (1508, 'TQFP100', 'BE')]
            for part, package, tech in targets:
                run([str(bindir / (f'fit{part}' + suffix)), '-i', 'design.edf', '-ifmt', 'EDIF',
                     '-device', package, '-tech', f'ATF{part}{tech}', '-JTAG', 'OFF'], work, env)
                (work / 'fitted.v').write_text(readback(work / 'design.tt3', ports))
                run([tools['iverilog'], '-g2012', '-s', 'tb', '-o', 'simulation',
                     'designs.v', 'fitted.v', 'tb.v'], work)
                trace = run([tools['vvp'], 'simulation'], work)
                simulate_jedec(bindir, work, ports, trace, env)
                checks += 1
    print(f'PASS: {checks} Yosys EDIF fits with RTL and JEDEC comparisons')
    return 0


if __name__ == '__main__':
    sys.exit(main())
