# SPDX-License-Identifier: BSD-2-Clause
"""Parser and template expansion for version 1 device descriptions.

Expressions are interpreted here; no Python evaluation or imports are allowed.
Unresolved names remain symbolic connections for the architecture validator.
"""
from dataclasses import dataclass, field
import json
import re


class Error(ValueError):
    pass


@dataclass(frozen=True)
class Expr:
    op: str
    args: tuple


@dataclass
class Block:
    kind: str
    name: object = None
    parent: str = None
    fields: dict = field(default_factory=dict)
    children: list = field(default_factory=list)

    def one(self, kind, name=None):
        matches = [b for b in self.children if (b.kind, b.name) == (kind, name)]
        if len(matches) != 1:
            raise Error(f'{self.kind}: expected one {kind} {name or ""} block')
        return matches[0]

    def all(self, kind):
        return [b for b in self.children if b.kind == kind]


TOKEN = re.compile(r'\s+|\#[^\n]*|"(?:[^"\\\n]|\\[^\n])*"|[A-Za-z_][A-Za-z_0-9]*|[0-9]+|\.\.|[{}\[\](),.:=+*/-]')
PRECEDENCE = {'+': 1, '-': 1, '*': 2, '/': 2}


class Parser:
    def __init__(self, text, filename='<input>'):
        if len(text) > 1024 * 1024:
            raise Error('description exceeds 1 MiB')
        self.text, self.filename = text, filename
        self.tokens = []
        offset = 0
        for m in TOKEN.finditer(text):
            if m.start() != offset:
                self.fail('invalid character', offset)
            offset = m.end()
            if not m[0].isspace() and not m[0].startswith('#'):
                self.tokens.append((m[0], m.start()))
        if offset != len(text):
            self.fail('invalid character', offset)
        self.tokens.append(('', len(text)))
        self.i = 0

    def fail(self, message, offset=None):
        if offset is None:
            offset = self.tokens[self.i][1]
        line = self.text.count('\n', 0, offset) + 1
        column = offset - self.text.rfind('\n', 0, offset)
        raise Error(f'{self.filename}:{line}:{column}: {message}')

    def peek(self):
        return self.tokens[self.i][0]

    def take(self, expected=None):
        value = self.peek()
        if not value or (expected is not None and value != expected):
            self.fail(f'expected {expected or "token"}, found {value or "end of file"}')
        self.i += 1
        return value

    def identifier(self):
        if not re.fullmatch(r'[A-Za-z_][A-Za-z_0-9]*', self.peek()):
            self.fail('expected identifier')
        return self.take()

    def expression(self, minimum=0, depth=0):
        if depth > 64:
            self.fail('expression nesting exceeds 64')
        token = self.take()
        if token.startswith('"'):
            try:
                value = json.loads(token)
            except ValueError:
                self.fail('invalid quoted string')
        elif token.isdigit():
            if len(token) > 10:
                self.fail('integer exceeds 32-bit range')
            value = int(token)
        elif token == '-':
            value = Expr('negate', (self.expression(3, depth + 1),))
        elif token == '(':
            value = self.expression(depth=depth + 1)
            self.take(')')
        elif token == '[':
            values = []
            while self.peek() != ']':
                item = self.expression(depth=depth + 1)
                if self.peek() == '..':
                    self.take()
                    item = Expr('range', (item, self.expression(depth=depth + 1)))
                values.append(item)
                if self.peek() != ',':
                    break
                self.take()
            self.take(']')
            value = Expr('list', tuple(values))
        elif re.fullmatch(r'[A-Za-z_][A-Za-z_0-9]*', token):
            value = Expr('name', (token,))
        else:
            self.fail(f'expected expression, found {token}')
        while True:
            token = self.peek()
            if token == '.':
                self.take()
                value = Expr('member', (value, self.identifier()))
            elif token == '[':
                self.take()
                value = Expr('index', (value, self.expression(depth=depth + 1)))
                self.take(']')
            elif token == '(':
                self.take()
                args = []
                while self.peek() != ')':
                    args.append(self.expression(depth=depth + 1))
                    if self.peek() != ',':
                        break
                    self.take()
                self.take(')')
                value = Expr('call', (value, *args))
            elif token in PRECEDENCE and PRECEDENCE[token] >= minimum:
                self.take()
                right = self.expression(PRECEDENCE[token] + 1, depth + 1)
                value = Expr(token, (value, right))
            else:
                return value

    def body(self, block, closing='', depth=0):
        if depth > 32:
            self.fail('block nesting exceeds 32')
        seen = set()
        while self.peek() != closing:
            kind = self.identifier()
            if self.peek() == '=':
                if kind in block.fields:
                    self.fail(f'duplicate field {kind}')
                self.take()
                block.fields[kind] = self.expression()
            else:
                name = None
                if self.peek() not in ('{', ':'):
                    name = int(self.take()) if self.peek().isdigit() else self.identifier()
                parent = None
                if self.peek() == ':':
                    self.take()
                    parent = self.identifier()
                if (kind, name) in seen:
                    self.fail(f'duplicate block {kind} {name}')
                seen.add((kind, name))
                self.take('{')
                child = Block(kind, name, parent)
                self.body(child, '}', depth + 1)
                self.take('}')
                block.children.append(child)
        return block


def evaluate(value, environment=None):
    environment = environment or {}
    if not isinstance(value, Expr):
        return value
    op, args = value.op, value.args
    if op == 'name':
        return environment.get(args[0], value)
    if op == 'member':
        return Expr(op, (evaluate(args[0], environment), args[1]))
    args = tuple(evaluate(a, environment) for a in args)
    if op in (*PRECEDENCE, 'negate', 'range'):
        if not all(type(a) is int for a in args):
            raise Error(f'{op}: arithmetic requires resolved integers')
        if op == 'range':
            first, last = args
            if first > last or last - first > 65535:
                raise Error('invalid or oversized range')
            return list(range(first, last + 1))
        if op == '/':
            if args[1] == 0 or args[0] % args[1]:
                raise Error('division must have a nonzero divisor and an integer result')
            result = args[0] // args[1]
        elif op == '+':
            result = args[0] + args[1]
        elif op == '-':
            result = args[0] - args[1]
        elif op == '*':
            result = args[0] * args[1]
        else:
            result = -args[0]
        if abs(result) > 0xffffffff:
            raise Error('integer exceeds 32-bit range')
        return result
    if op == 'list':
        return [v for a in args for v in (a if isinstance(a, list) else [a])]
    return Expr(op, args)


def expanded(block, environment=None):
    return Block(block.kind, block.name, block.parent,
                 {k: evaluate(v, environment) for k, v in block.fields.items()},
                 [expanded(c, environment) for c in block.children])


def symbol(text):
    parser = Parser(text)
    value = evaluate(parser.expression())
    if parser.peek():
        parser.fail('trailing expression text')
    return value


def parse(text, filename='<input>'):
    try:
        return Parser(text, filename).body(Block('file'))
    except RecursionError as exc:
        raise Error(f'{filename}: nesting limit exceeded') from exc
