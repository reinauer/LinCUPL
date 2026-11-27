"""Check Boolean equation input against independently generated truth tables."""
import itertools
import os
from pathlib import Path
import random
import subprocess
import sys
import tempfile

from fit150x_edif import functions, tt2


def main():
    bindir = Path(sys.argv[1]).resolve()
    suffix = '.exe' if os.name == 'nt' else ''
    mode = sys.argv[2] if len(sys.argv) > 2 else 'legacy'
    checks = 0
    with tempfile.TemporaryDirectory(prefix='equation input ') as temp:
        root = Path(temp)
        (root / 'empty').mkdir()
        env = dict(os.environ, FITTERDIR=str(root / 'empty'),
                   LINCUPL_FIT150X_MODE=mode)

        def run(source, fmt='EQN', part=1502, diagnostic=None, pins=None):
            nonlocal checks
            checks += 1
            work = root / str(checks)
            work.mkdir()
            path = work / ('test.eqn' if fmt == 'EQN' else 'test.tt2')
            path.write_text(source)
            if pins is not None: (work / 'board.pin').write_text(pins)
            package = 84 if part == 1508 else 44
            result = subprocess.run(
                [str(bindir / (f'fit{part}' + suffix)), '-i', path.name,
                 '-ifmt', fmt, '-dev', f'P{part}C{package}', '-JTAG', 'OFF',
                 *(['-pinfile', 'board.pin'] if pins is not None else [])],
                cwd=work, env=env, capture_output=True, text=True, timeout=20)
            output = result.stdout + result.stderr
            if diagnostic:
                assert result.returncode != 0, output
                assert 'EQN line ' in output and diagnostic in output, output
                assert not (work / 'test.jed').exists(), output
                return
            assert result.returncode == 0 and 'Design fits successfully' in output, output
            assert (work / 'test.jed').exists(), output
            equations = functions(work / 'test.tt3')
            values = []
            for bits in itertools.product((0, 1), repeat=3):
                state = dict(zip('abc', bits))
                pending = dict(equations)
                while pending:
                    ready = [name for name, (inputs, _) in pending.items()
                             if all(n in state for n in inputs)]
                    assert ready, ('cycle in fitted equations', equations)
                    for name in ready:
                        inputs, truth = pending.pop(name)
                        index = 0
                        for n in inputs: index = (index << 1) | int(state[n])
                        state[name] = truth[index]
                values.append(state['q'])
            return tuple(values)

        cases = [
            ('a * b', lambda a, b, c: a and b),
            ('a + b * c', lambda a, b, c: a or (b and c)),
            ('a ^ b * c', lambda a, b, c: a ^ (b and c)),
            ('a + b ^ c', lambda a, b, c: a or (b ^ c)),
            ('!(a + b) * c', lambda a, b, c: not (a or b) and c),
            ("(a + b)' c", lambda a, b, c: not (a or b) and c),
            ('~a & b | c', lambda a, b, c: (not a and b) or c),
            ('a (b + c)', lambda a, b, c: a and (b or c)),
            ('a ^ b ^ c', lambda a, b, c: a ^ b ^ c),
            ('!!a', lambda a, b, c: a),
            ('0', lambda a, b, c: 0),
            ('1', lambda a, b, c: 1),
        ]
        for part in (1502, 1504, 1508):
            for expr, evaluate in cases:
                actual = run('NAME = test;\nINORDER = a b c;\nOUTORDER = q;\n'
                             f'q = {expr};\n', part=part)
                expected = run(tt2(['a', 'b', 'c'], ['q'], evaluate, {}), 'TT', part)
                assert actual == expected, (part, expr, actual, expected)

        expected = run(tt2(['a', 'b', 'c'], ['q'], lambda a, b, c: (a and b) or c, {}), 'TT')
        for source in [
            'q = t + c; t = a * b;',
            'INORDER=a b c; q=t+c; t=a*b;',
            'OUTORDER=q; q=t+c; t=a*b;',
            'q=t+c; INORDER=a b c; OUTORDER=q; t=a*b;',
            '# comments\r\nname=test;\r\ninorder=a b c; outorder=q;\r\n'
            'q=t+c; # forward reference\r\nt=a*b; # EOF comment',
        ]:
            assert run(source) == expected, source

        malformed = [
            ('', 'no outputs'),
            ('INORDER=a; OUTORDER=q;', "undefined signal 'q'"),
            ('INORDER=a; OUTORDER=q; q=b;', "undefined signal 'b'"),
            ('INORDER=a; OUTORDER=q; a=1; q=a;', 'also has an equation'),
            ('q=a; Q=b;', 'multiple equations'),
            ('INORDER=a a; q=a;', 'port declaration'),
            ('INORDER=a; OUTORDER=a;', 'port declaration'),
            ('INORDER=a; INORDER=b; q=a;', 'duplicate INORDER'),
            ('OUTORDER=q; OUTORDER=r; q=a; r=a;', 'duplicate OUTORDER'),
            ('NAME=a; NAME=b; q=1;', 'duplicate NAME'),
            ('q = a', "expected ';'"),
            ('q := a;', 'invalid character'),
            ('q = a / b;', 'invalid character'),
            ('q = a\0b;', 'invalid character'),
            ('q = a % b;', 'invalid character'),
            ('q = a + ;', 'expected an expression'),
            ('q = (a;', "expected ')'"),
            ('q = a);', "expected ';'"),
            ('q = q;', 'combinational cycle'),
            ('OUTORDER=q; q=t; t=q;', 'combinational cycle'),
            ('OUTORDER=q; q=a; unused=unused;', 'combinational cycle'),
            ('q=' + '(' * 140 + 'a' + ')' * 140 + ';', 'nesting limit'),
            ('q=' + '!' * 140 + 'a;', 'nesting limit'),
            ('q=' + '+'.join(['a'] * 140) + ';', 'complexity limit'),
            ('q=' + 'a' * 300 + ';', 'name exceeds'),
        ]
        for source, diagnostic in malformed:
            run(source, diagnostic=diagnostic)
        chain = 't0=a;' + ''.join(f't{i}=t{i-1};' for i in range(1, 150)) + 'q=t149;'
        run(chain, diagnostic='logic depth limit')
        source = 'INORDER=a b; OUTORDER=q; q=a*b;'
        for part in (1502, 1504, 1508):
            assert run(source, part=part, pins='a : 2\nb : 1\nq : 4\n') == run(source, part=part)
        for pins, diagnostic in [
            ('bad:4\n', 'unknown port'), ('a:2\nb:2\n', 'assigned to both'),
            ('a:2\na:4\n', 'conflicting assignments'), ('a:0\n', 'invalid pin'),
            ('a:99999999999999999999999\n', 'invalid pin'),
            ('a:-1\n', 'invalid pin'), ('a = 2\n', "expected 'signal : pin'"),
            ('"a:2\n', 'unterminated pin name'),
        ]:
            run(source, pins=pins, diagnostic=diagnostic)

        rng = random.Random(1504)

        def generate(depth):
            if not depth or rng.randrange(4) == 0:
                n = rng.randrange(3)
                return 'abc'[n], lambda values: values[n]
            a, av = generate(depth - 1)
            if rng.randrange(4) == 0:
                return f'!({a})', lambda values: not av(values)
            b, bv = generate(depth - 1)
            op = rng.choice('*+^')
            return f'({a}{op}{b})', lambda values: (
                av(values) and bv(values) if op == '*' else
                av(values) or bv(values) if op == '+' else
                av(values) ^ bv(values))

        for _ in range(30):
            expr, evaluate = generate(4)
            source = f'INORDER=a b c; OUTORDER=q; q={expr};'
            assert run(source) == run(tt2(['a', 'b', 'c'], ['q'],
                                         lambda *args: evaluate(args), {}), 'TT'), expr
    print(f'Boolean equation input ({mode}): {checks} checks passed')


if __name__ == '__main__':
    main()
