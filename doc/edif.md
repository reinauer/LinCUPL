# EDIF input for ATF150x

`fit1502`, `fit1504` and `fit1508` accept EDIF 2 0 0, level 0 netlists:

```sh
fit1502 -i design.edf -ifmt EDIF -device PLCC44 -tech ATF1502AS \
    -pinfile board.pin -JTAG OFF
```

Use the fitter and package for the intended chip. The fitter writes the
JEDEC file, fitted equations (`.tt3`), placement report (`.fit`) and pin
listing (`.pin`). `-o name` selects the output basename. JTAG stays available
unless explicitly disabled; the example releases its pins for logic.

## Yosys

This flow uses Yosys's generic gates and needs no external primitive library:

```sh
yosys -p 'read_verilog design.v; tribuf;
    synth -top top -flatten -noabc; dffunmap; abc -g simple; clean;
    write_edif -pvector bra design.edf'
fit1502 -i design.edf -ifmt EDIF -device PLCC44 -tech ATF1502AS
```

`dffunmap` expresses clock enables and
synchronous resets as data logic, which gives the fitter more placement
options. Synthesize memories and arithmetic to gates before exporting.

`-pvector bra` preserves Verilog bus ranges in EDIF display names. For
example, member 0 of `(array (rename bus "bus[7:4]") 4)` is named `bus[7]`
in pin assignments. Ascending ranges work in the same way. Do not combine
this range convention with Yosys's `-lsbidx`, which reverses the member
numbering without changing the displayed range. For netlists without a
range, pins use EDIF member indices: `bus[0]`, `bus[1]`, etc. This also accepts
flows that use `write_edif -lsbidx` without `-pvector`.

## Pins and names

A pin file contains one assignment per line:

```text
clk : 43
data[0] : 4
data[1] : 5
result : 21
```

Blank lines, `#` or `//` comments, and the fitter's `CHIP ...` header are
accepted. Quote a signal name containing spaces. The header is optional.
An explicit `-pinfile board.pin` reads that path; bare `-pinfile` or
`-pinfile ON` reads `<output-basename>.pin`. No pin file is read implicitly.
Scalar port properties `LOC`, `PIN` and `PIN_NUMBER` also specify pins,
using integers or strings such as `"43"` and `"P43"`.

Assignments are checked for unknown names, duplicate pins and conflicting
locations. Package legality and JTAG reservations are checked by the fitter.
`-preassign KEEP`, `TRY` and `IGNORE` retain their usual meanings.

EDIF identifiers are case insensitive. A `rename` string supplies a display
name, not another identifier for reference lookup. Ordinary names and bus
subscripts remain readable in reports. Punctuation and spaces that the
fitter cannot use are encoded as `_hh_` byte values; pin files can use the
original name. Names that collide after conversion or exceed the fitter's
255-character limit are rejected.

## Cells and hierarchy

The reader selects the `design` cell, resolves library/cell/view references
and flattens the selected NETLIST hierarchy. It supports port arrays,
instance arrays, array members and array nets, including multidimensional
arrays. Whole-array connections must have matching widths. Recursive
hierarchy, unresolved references, multiple drivers and combinational loops
are errors. Register and latch feedback is preserved.

Built-in cells include:

* `GND`, `VCC`, `BUF`, `INBUF`, `IBUF`, `OUTBUF`, `INV` and `NOT`.
* AND, NAND, OR and NOR families, including inverted-input variants;
  `XOR`, `XOR2`, `XNOR2`, `MUX2`, `MUX4` and `MUX8`.
* `DFF`, `DFFE`, `DFFAR`, `DFFAS`, `DFFARS`, `DFFEARS` and `LATCH`,
  including complementary outputs and optional controls.
* `TRI` and `BIBUF` for tristate and bidirectional pads.
* Yosys `$_..._` Boolean gates, muxes, tristates, D flip-flops and latches.
  Clock, enable, reset and preset polarities are retained. The DFF/DFFE,
  DFFSR/DFFSRE, DLATCH/DLATCHSR and SDFF/SDFFE/SDFFCE variants are supported.

Cell interfaces are checked as well as names. For example, `AND3I2` has
inputs `A`, `BN`, `CN`, and computes `A & !BN & !CN`; an interface with
different pins needs an explicit library definition.

`-lib cells.lib` adds or overrides primitive definitions using the fitter's
existing GATE/PGATE library format, in text or binary form. Combinational
equations and register/pad controls are imported into the same internal
representation as the built-in cells. Unknown black boxes and cells without
a logic definition are rejected. An invalid supplied library is an error.

## Boundaries and validation

This is a logical netlist reader. Timing annotations and optimization hints
are not timing constraints. Behavioral views, unresolved parameters, LUTs
that have not been mapped to gates, and initial-state properties are not
supported. Initial state must be expressed through an explicit reset.
Unsupported connectivity constructs fail with a source-line diagnostic.

Parsing and hierarchy have a depth limit of 128. Reader storage is bounded
at 64 MiB, flattened connectivity at 65,536 wires, and package ports at 256.
The selected chip's actual resource limits are checked during fitting.

`fit150x_edif` tests pin assignments, hierarchy, arrays, cell libraries,
primitive polarities and invalid input. It compares fitted functions against
independently generated TT2 designs on all three device families.
`fit150x_yosys` additionally synthesizes Verilog, fits its EDIF and compares
an independent readback of the fitted TT3 equations with the original RTL.
It requires Yosys and Icarus Verilog and is skipped when they are unavailable.
Both suites run in the legacy and extended fitter modes. Linux regression
jobs install the synthesis and simulation tools so these checks run in CI.
These checks verify logical behavior; they do not decode the resulting
ATF150x JEDEC image or establish hardware timing.
