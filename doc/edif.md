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

For ATF1508BE, `-device CBGA132 -tech ATF1508BE` selects the 132-ball
package. Pin files and port location properties accept case-insensitive
ball names such as `L2`, `G1` and `P12`. The fitter's `.pin`, `.io` and
placement reports use the physical ball names. `P1508B132` is an alias
for this package. Set `LINCUPL_FIT150X_MODE=extended` to enable the BE fitter.

JEDEC vectors still use numeric positions. They enumerate occupied balls
left to right in rows A, B, C, D, E, F, G, H, J, K, L, M, N, P.
Rows A-C and M-P contain columns 1-14; rows D-L contain columns 1-3 and
12-14. For example, position 1 is A1, 61 is G1, 86 is L2 and 132 is P14.
The placement report includes the complete position-to-ball table.

The package connections follow [ATF1508BE data sheet 3663A](https://ww1.microchip.com/downloads/en/DeviceDoc/doc3663.pdf), tables 13-1
and 13-2 (pages 23-25), with one deliberate correction: MC17 uses G2.
Table 13-2 prints C2 for MC17, but table 13-1 and figure 1-2 (page 4)
identify C2 as the separate OE1 input. G2 is the only I/O ball left after
mapping the other 79 bonded macrocells. This correction follows the
package diagram and complete I/O mapping; it has not been verified on
physical hardware.

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
The `csim_150x_jed_legacy` and `csim_150x_jed_extended` suites additionally
decode the emitted fuse maps and check physical pin vectors. They cover
Boolean logic, cascades, foldback, internal and pad feedback, global and
product clocks, registers, latches, enables, shared register/output
macrocells and every supported package. Mutated maps check counterexamples
and rejection of invalid routing. These are logical checks, not hardware
timing measurements.

Use `csim -fl design` with `design.jed` and `design.si` to simulate the
fitted result without an ABS file. LinSim exposes the same mode through
**Simulator > Simulate JEDEC Fuse Map**. `jedcompare` checks combinational
maps using that decoder; it rejects active register state and physical
output feedback. See [JEDEC tools](jed-tools.md) for the proof scope.

## BE electrical options

In extended mode, `-voltage_level_A` and `-voltage_level_B` describe the
external I/O bank supplies. ATF1502BE accepts 1.8, 2.5 and 3.3 V;
ATF1504BE and ATF1508BE also accept 1.5 V. These options validate electrical
choices; they do not program supply-voltage selectors or change Boolean
logic. Each bank contains half the macrocells, with bank A first.

On ATF1504BE and ATF1508BE, `-SSTL_input name1,name2` enables SSTL on the
named I/O inputs, including the input buffer of bidirectional pads. Their
bank must use 2.5 or 3.3 V. The fitter reserves that bank's VREF pin and
keeps its termination disabled. VREF cannot also serve
as user I/O or power down. Dedicated inputs cannot use SSTL, and SSTL
cannot share a pad with Schmitt-trigger mode. `-SSTL_input ON` selects all
I/O inputs while leaving dedicated inputs in CMOS mode; unconstrained
inputs are placed in a suitable bank. An explicit list containing an
output without input feedback, unknown name or dedicated input is an error.

`-pull_down name1,name2` selects pull-down termination on named dedicated
inputs or I/O pads. `-unused_To_Ground ON` selects it for unused pads.
`-pull_up_unused ON` and `-unused_To_PinKeeper ON` provide the other unused
pad policies; these policies are mutually exclusive. Active VREF pads
remain floating under every policy, including global `-pin_keep ON`.

`-POR one` and `-power_reset ON` are accepted compatibility options on BE
parts and report that they do not change configuration fuses. They do not
select an initial register value. Express a required initial state through
reset logic; simulator preload vectors provide a separate way to test
arbitrary register states. JEDEC simulation models binary I/O levels,
not SSTL analog thresholds, supply ramp behavior or termination strength.
