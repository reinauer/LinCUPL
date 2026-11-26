# SPDX-License-Identifier: BSD-2-Clause
"""Shared hardware objects and validation, independent of DL record layouts."""
from dataclasses import dataclass

from .syntax import Error


def require(condition, message):
    if not condition:
        raise Error(message)


def unsigned(value, limit, label):
    require(type(value) is int and 0 <= value < limit,
            f'{label}: expected an integer in 0..{limit - 1}')
    return value


@dataclass(frozen=True)
class Array:
    name: str
    rows: int
    columns: int
    first_fuse: int = 0
    connected: int = 0
    disconnected: int = 1

    def __post_init__(self):
        require(type(self.rows) is int and self.rows > 0, 'array rows must be positive')
        require(type(self.columns) is int and self.columns > 0, 'array columns must be positive')
        require(type(self.first_fuse) is int and self.first_fuse >= 0, 'invalid array start')
        require({self.connected, self.disconnected} == {0, 1}, 'invalid crosspoint encodings')

    @property
    def fuse_count(self):
        return self.rows * self.columns

    def product(self, first_row, count=1):
        return Products(self, first_row, count)


@dataclass(frozen=True)
class Products:
    array: Array
    first_row: int
    count: int = 1

    def __post_init__(self):
        unsigned(self.first_row, self.array.rows, 'product row')
        require(type(self.count) is int and 0 < self.count <= self.array.rows - self.first_row,
                'product range exceeds its array')

    @property
    def first_fuse(self):
        return self.array.first_fuse + self.first_row * self.array.columns


@dataclass(frozen=True)
class Selector:
    """Named choices encoded across an ordered collection of fuse bits."""
    positions: tuple
    choices: dict

    def __post_init__(self):
        require(self.positions and len(set(self.positions)) == len(self.positions),
                'selector positions must be nonempty and distinct')
        require(all(type(p) is int and p >= 0 for p in self.positions), 'invalid selector position')
        require(self.choices and all(isinstance(n, str) and n for n in self.choices),
                'selector choices must have names')
        for name, value in self.choices.items():
            unsigned(value, 1 << len(self.positions), f'selector {name}')
        require(len(set(self.choices.values())) == len(self.choices),
                'selector choices have duplicate encodings')

    def encode(self, name):
        require(name in self.choices, f'unknown selector choice {name}')
        return self.choices[name]


@dataclass(frozen=True)
class Mux:
    selector: Selector
    sources: dict

    def __post_init__(self):
        require(set(self.sources) == set(self.selector.choices),
                'mux sources and selector choices differ')


@dataclass(frozen=True)
class FixedClock:
    source: object
    edge: str = 'rising'

    def __post_init__(self):
        require(self.edge in ('rising', 'falling'), 'invalid clock edge')


@dataclass(frozen=True)
class Register:
    name: str
    data: object
    clock: object
    kind: object
    asynchronous_reset: Products
    synchronous_preset: Products
    power_up: str


@dataclass(frozen=True)
class Feedback:
    source: object
    high_column: int
    low_column: int
    alias: int


class FuseMap:
    def __init__(self, count):
        require(type(count) is int and count > 0, 'fuse count must be positive')
        self.count = count
        self.owners = {}

    def claim(self, positions, label):
        for fuse in positions:
            unsigned(fuse, self.count, label)
            require(fuse not in self.owners,
                    f'fuse {fuse}: {label} overlaps {self.owners.get(fuse)}')
            self.owners[fuse] = label

    def claim_range(self, first, count, label):
        unsigned(first, self.count, label)
        require(type(count) is int and 0 < count <= self.count - first, f'{label}: invalid length')
        self.claim(range(first, first + count), label)

    def complete(self):
        require(len(self.owners) == self.count, 'fuse map contains unassigned positions')


@dataclass(frozen=True)
class Package:
    name: str
    pins: int
    physical: tuple
    unconnected: tuple = ()
    drawing: int = 0
    ground: tuple = ()
    supply: tuple = ()

    def __post_init__(self):
        unsigned(self.pins, 256, 'package pin count')
        require(self.pins > 0 and self.physical, 'package requires pins and a pin map')
        assigned = (*self.physical, *self.unconnected, *self.ground, *self.supply)
        require(all(type(p) is int and 1 <= p <= self.pins for p in assigned),
                'package map contains an invalid pin')
        require(sorted(assigned) == list(range(1, self.pins + 1)),
                'package pin map must cover every pin exactly once')

    def number(self, number):
        """Map physical pins; keep buried-node ordinals after the package."""
        if number == 0:
            return 0
        require(type(number) is int and number > 0, 'invalid pin or node number')
        if number <= len(self.physical):
            return self.physical[number - 1]
        return number + self.pins - len(self.physical)
