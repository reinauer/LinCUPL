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
  write that name in the JEDEC heading. BE-series maps are unsupported.
- Matching physical pin interfaces. A different pin count or input/output
  role is reported as an interface difference; pins are not remapped.
- Binary external inputs and logical output levels, including high
  impedance. Timing, hazards, programming/security settings and analog
  behavior are outside the comparison.
- Registers and physical output feedback are rejected, including acyclic output
  feedback. Sequential equivalence needs a separate proof method. csim
  can simulate ATF750 and ATF2500 maps, but their buried registers remain
  outside this proof scope even when the pin outputs are combinational.
  ATF150x registers are detected through the active routing graph,
  including buried state feeding a combinational pin. Power-down pin
  operation and invalid active routing configurations are rejected.

Exit status is 0 only for proven equivalence within this scope, 1 for a
pin-interface difference or a counterexample, and 2 for an error,
unsupported design, indeterminate output or exceeded input bound. The
proof is relative to csim's existing decoder and models; it is not an
independent validation of those models or a physical device test.
