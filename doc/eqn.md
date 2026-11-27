# Boolean equation input

The ATF1502, ATF1504 and ATF1508 fitters accept combinational equations
with `-ifmt EQN`:

```sh
fit1504 -i design.eqn -ifmt EQN -device PLCC44 -tech ATF1504AS
```

```text
NAME = example;
INORDER = a b select;
OUTORDER = q;

both = a * b;
q = both + select;
```

Each declaration and equation ends with a semicolon. `NAME` is optional
metadata. `INORDER` declares physical inputs; `OUTORDER` declares physical
outputs. Other assigned signals become internal combinational nodes.
Definitions may appear after their uses. Names are case-insensitive, as in
the rest of the fitter. Letters, digits, underscores, dots, dollar signs
and brackets are accepted in names; `0` and `1` are Boolean constants.
`#` starts a comment ending at the next newline.

| Operation | Syntax | Precedence |
|---|---|---|
| Complement | `!a`, `~a`, `a'` | Highest |
| AND | `a * b`, `a & b`, `a b` | Above XOR |
| XOR | `a ^ b` | Above OR |
| OR | `a + b`, `a \| b` | Lowest |
| Grouping | `(a + b) * c` | Explicit |

Without `INORDER`, unassigned signals used by expressions become inputs.
Without `OUTORDER`, assigned signals that no equation reads become outputs.
Explicit declarations are preferable: they catch misspelled input names
and make the intended interface clear.

An optional `-pinfile board.pin` fixes package pin assignments:

```text
a : 2
b : 4
select : 5
q : 6
```

Quoted names, `#` and `//` comments, and a `CHIP` header are accepted in
pin files. `-pinfile ON` reads `<output-base>.pin`. Assignments must name
declared or inferred ports; conflicting assignments are errors.

The reader rejects multiple drivers, undriven signals, combinational
cycles, invalid characters and incomplete expressions with line-numbered
diagnostics. Input is limited to 4 MiB, 4,096 signals, 65,536 expression
nodes, 128 levels of nesting or logic depth, and 255 characters per name.

Equations enter the same synthesis, placement and routing pipeline as
truth tables and EDIF. The outputs include JEDEC, fitted `.tt3` equations,
and placement reports. Register, latch and tristate descriptions use
CUPL or [EDIF](edif.md); the EQN reader describes Boolean functions.
