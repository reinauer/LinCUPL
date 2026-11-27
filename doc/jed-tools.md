# JEDEC inspection and equivalence

`jedinspect` and `jedcompare` use LinCUPL's shared JEDEC reader.
`jedcompare` also uses csim's device
decoder, binder and evaluator. Both are installed with the command-line
tools. Neither modifies its input files or creates simulation output files.

## Inspecting a fuse map

```sh
jedinspect design.jed
jedinspect --fuses 0:64 design.jed
jedinspect --diff revised.jed design.jed
jedinspect --json --diff revised.jed --limit 16 design.jed
```

The summary includes the fuse count, counts of zero and one bits, QP pin
count (zero if absent), calculated fuse checksum, presence of a C field,
STX/ETX framing, and design text. `--fuses START:COUNT` prints a range of
zero-based fuse addresses. A zero bit is an intact link; a one is a blown
link. No device library is needed, so inspection works for any architecture
within the shared reader's limit of 4,000,000 fuses.

The reader verifies a C field when present, checks fuse bounds and missing
states, and refuses a file with neither ETX nor a fuse checksum. It accepts
the same formatting variations as csim. It does not check the transmission
checksum or interpret security settings, notes or test vectors. Inspection
does not establish that a map implements a valid device configuration.

`--diff` compares fuse states and lengths, including signature bits. It
ignores design text, QP and record formatting. Missing fuses in a shorter
file are differences, shown as `-` in text or `null` in JSON. At most 64
differences are listed by default; `--limit N` changes that bound, and zero
prints only the count. The total count always covers the complete maps.

`--json` emits one object, with optional `range` and `comparison` members.
`comparison.count` is the total difference count;
`comparison.truncated` says whether the listing was limited. Non-ASCII
bytes in strings are escaped as `\u00XX`, preserving byte values even for
design text that is not UTF-8. Text output escapes non-ASCII and control
bytes in the design heading, except tabs and newlines.

Exit status is 0 for successful inspection or equal fuse maps, 1 for
different fuse maps, and 2 for invalid arguments, malformed input or an
I/O error. Equality here describes the fuse data only.

## Checking pin behavior

```sh
jedcompare original.jed revised.jed
jedcompare -u share/lincupl/lincupl.dl -d g22v10 original.jed revised.jed
jedcompare --max-inputs 20 original.jed revised.jed
```

The comparator obtains an exhaustive truth table from csim for each map,
then compares physical pin levels for every assignment to the union of
their used inputs. An output's high-impedance state (`Z`) is distinct from
both zero and one, so output-enable differences are checked too. Different
product-term arrangements, unused fuses and signature bits can still give
equivalent pin behavior.

Every simulation row starts afresh. A successful comparison requires a
complete table with determinate outputs; identical unknown values never
count as proof. The default bound is 16 combined used inputs (65,536
assignments), adjustable from 0 through 20. Exceeding the bound produces an
inconclusive result, with no sampling or partial-success fallback.

For example, comparing AND with OR reports a reproducible counterexample:

```text
Not equivalent: P23 is 0 versus 1
Inputs: P2=1 P3=0
```

The device library is located as for cupl/csim, or selected with `-u`.
Device detection uses the JEDEC heading, fuse count and QP; `-d` overrides
it for both files. An explicit mode that disagrees with the mode fuses,
or a QP value that disagrees with the selected package, is rejected.
Adjacent `.pld`, `.abs` and `.si` files are never consulted: pin numbers
and electrical polarity come from the fuse map and library alone.

Current proof scope:

- Combinational GAL16V8, GAL20V8 and GAL22V10 maps, including decoded PLCC
  packages and a GAL22V10 map without its signature fuses.
- Combinational ATF1502AS/1504AS/1508AS maps in the fitter's supported packages,
  including internal feedback, foldback, product-term cascades and output
  enables. These use the built-in wiring tables. A package name such as
  `f1504plcc44` is required when QP alone is ambiguous; the native fitters
  write that name in the JEDEC heading. ATF1502BE, ATF1504BE and ATF1508BE
  maps use distinct built-in configuration layouts.
- Matching physical pin interfaces. A different pin count or input/output
  role is reported as an interface difference; pins are not remapped.
- Binary external inputs and logical output levels, including high
  impedance. Timing, hazards, programming/security settings and analog
  behavior are outside the comparison.
- The default mode rejects registers, power-down state and physical output
  feedback. Use sequential mode for these designs and for ATF750/ATF2500
  maps with buried registers. Invalid active routing configurations are
  rejected.

### Sequential equivalence

```sh
jedcompare --sequential left.jed right.jed
jedcompare --sequential --max-states 100000 --max-transitions 10000000 left.jed right.jed
```

Sequential mode supports GAL16V8, GAL20V8, GAL22V10, ATF750, ATF2500 and
ATF150x maps. It explores reachable pairs of states from their configured
power-up values. It tries every binary assignment to the union
of used inputs at each state, including asynchronous controls and clocks.
Registers need not occupy matching macrocells or have matching counts.
Physical output feedback is included, along with output enables. Output
pins are not externally driven during comparison; an undriven feedback
pin needs a determinate pull or keeper value when its output is disabled.
ATF150x power-down retains
registers, clock history, output values and output enables; logical pin
keeper state is retained too.

The first input assignment settles the power-up inputs. Subsequent
assignments use csim's binary-vector transition rules: register updates
are simultaneous, and derived clocks are processed until the circuit settles.
Inputs are simultaneous logical levels, without a timing or hazard model.
By default the comparison starts at power-up. To select other initial
register values, first list the storage decoded from each map:

```sh
jedcompare --list-state left.jed
jedcompare --list-state right.jed
jedcompare --sequential --preload-left MC1=s,MC2=1 --preload-right MC5=s left.jed right.jed
```

`MC<n>` identifies ATF150x storage; `R<n>` identifies storage in the other
architectures. These names refer to raw register Q, before output polarity
and output enable, including buried registers. The listing gives the
configured power-up value of each register. Register names are local to
each map and need not match between maps.

A preload value is `0`, `1`, a symbolic bit such as `s`, or its complement
such as `!s`. A shared name denotes the same bit on both sides. Every binary
assignment to the named bits is checked, so `MC1=s` and `MC5=!s` describe a
pair of registers with opposite encodings of the same initial state.
Different symbolic names vary independently. Registers omitted from the
preload list retain their configured power-up values. Preloading changes
storage before the first input-settling step; it does not clock the design
or apply reset/preset controls by itself.
Preload lists have no spaces and support up to 20 distinct symbolic bits.
Use the concrete initial assignments printed with a counterexample to
repeat that comparison. The sequential simulation API can also replay its
exact steps, including buried storage; csim's `.si` preload vectors address
only registers exposed by its pin/node interface.

Equivalence is reported only when every reachable pair and outgoing input
assignment has been checked. A difference includes a shortest input trace
starting at the selected initial state, with physical pin numbers and
levels. A preload counterexample identifies its initial symbolic assignment.
Indeterminate outputs or nonsettling controls return inconclusive.
Identical unknown values are never accepted as proof.

The defaults are 10,000 state pairs and 1,000,000 transitions. The maximum
limits are 1,000,000 states and 1,000,000,000 transitions. State exploration
buffers have a 512 MiB budget; decoded models and runtime overhead are
additional. All preload assignments share the same exploration budget.
The combined-input limit applies in both modes. Reaching any
limit returns inconclusive, even if every transition checked so far agrees.

Exit status is 0 only for proven equivalence within this scope, 1 for a
pin-interface difference or a counterexample, and 2 for an error,
unsupported design, indeterminate output or exceeded exploration bound. The
proof is relative to csim's existing decoder and models; it is not an
independent validation of those models or a physical device test.
