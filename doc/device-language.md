# Device description language

`.dev` files compile into `.dl` libraries for `cupl`, `csim` and `cbld`.
The compiler is `tools/devcompile.py`; it requires Python 3 and its standard
library. Compilation reads source descriptions and catalog manifests only.
A reference `.dl` is an optional comparison input.

## Building libraries

The normal CMake build generates `share/lincupl/lincupl.dl` in the build
directory and installs it under `<prefix>/share/lincupl`. Its sources are
listed by `devices/manifest.json`. To compile manually:

```sh
python3 tools/devcompile.py devices -o /tmp/lincupl.dl
python3 tools/devcompile.py devices --catalog-only -o /tmp/catalog.dl
python3 tools/devcompile.py devices/Atmel -o /tmp/Atmel.dl
python3 tools/devcompile.py devices/cupl -o /tmp/cupl.dl
python3 tools/devcompile.py devices/Atmel/v750c.dev -o /tmp/v750c.dl
python3 tools/devcompile.py devices/families/atf750c.dev -o /tmp/atf750-family.dl
```

`--device mnemonic` selects entries and may be repeated. `--compare file.dl`
reports raw-byte identity separately from canonical record equality;
`--report file.json` saves the comparison. A canonical mismatch returns
status 1 and still writes the compiled library and report. Invalid input or a missing
reference entry fails before output is created. Existing output/report files
are never overwritten. The CMake build uses a temporary file and replaces
its generated library only after successful compilation.

For hardware descriptions, compatibility metadata is a separate console
and JSON result; those differences alone do not change the exit status.
Catalog comparison includes that metadata in canonical equality. Neither
comparison treats reserved record bytes as padding.
The default library includes deliberate hardware-model differences. Use
`--catalog-only` when comparing the combined catalog with a source library;
this option disables hardware replacements without changing catalog order.

## Common syntax

Every file starts with `format = 1`. Blocks contain `name = value` fields
and nested blocks. `#` introduces a line comment. Field and block names are
case-sensitive; duplicate or unknown fields are errors.

```text
format = 1

device Example {
    power {
        ground = 12
        supply = 24
    }
}
```

This fragment illustrates syntax; a complete device must provide the fields
required by its representation. Values are decimal integers, quoted strings,
symbolic names, expressions, or lists. Strings use JSON escaping. Arithmetic
supports parentheses, unary minus, `+`, `-`, `*` and `/`; division must produce
an integer. `[1..4, 8]` expands to `[1, 2, 3, 4, 8]`. Expressions are interpreted
by the parser and cannot execute Python code.

## Library representation

`device mnemonic { representation = library ... }` describes the compiler's
record interface. This representation covers every catalog entry, including
PROMs, programmable OR arrays and external-fitter interfaces.

Attribute blocks are `package`, `power`, `compatibility`, `fuses`, `signature`,
`compiler`, `simulation`, `sequence`, `prom` and `fitter`. Each declares all
its named fields.
The `sequence` block supplies generic state-machine register defaults;
explicit `SEQUENCED`, `SEQUENCERS`, `SEQUENCEJK` and `SEQUENCET` override them.
Tables use `columns`, `rows`, and optional `defaults`:

```text
extensions {
    columns = [name, function]
    rows = [row("d", d), row("oe", oe)]
}

initialization {
    defaults { value = 0 }
    columns = [first_fuse, last_fuse]
    rows = [row(100, 103)]
}
```

Every table field appears exactly once in `columns` or `defaults`.
`row(...)` values follow column order. A table may be absent to represent
zero records; `row()` still emits a record when all fields have defaults.
Record order and inactive operands are significant and retained.

The library profile also preserves compatibility metadata whose circuit
meaning is not established. The `compatibility` block contains
`attribute_4`, `attribute_5`, `attribute_7` and `attribute_20`, named by
attribute-record offset. The node table's `legacy_width` is a separate
field from `row_width`; their values need not agree. These fields are
required and range from 0 to 255. They belong to the library interface,
not the hardware model, and are compared exactly when checking catalogs.
Reserved zero regions are also compared; no record byte is silently ignored.

Supported tables are `extensions`, `fitter_extensions`, `pin_functions`,
`node_functions`, `connections`, `initialization`, `fitter_arguments`,
`alternatives` and `plot`. The implementation document lists their fields
and consumers. `tools/devlang/catalog.py` defines the exact field schemas,
numeric bounds and symbolic values. Unnamed compiler modes remain numbers.

`fitter.arguments` is quoted text. `fitter.pla_options` is an ordered list
of named options such as `tool_header`, `copy_tt2` and `short_labels`.
Compilation does not invoke a fitter. Plot rows contain named instructions
and numeric interpreter operands, not encoded byte strings.

## Hardware representation

Hardware devices declare `architecture`, `mnemonic`, package information,
arrays, configuration choices and macrocell templates. Supported backends
are `atf750c`, `gal22v10` and `atf2500`; complete examples are in
[`devices/families/`](../devices/families/).

An array defines its rows, columns, first fuse, address order, and connected
and disconnected encodings. `rows(logic, first, count)` identifies product
terms; `product(logic, row)` identifies one term. Addresses are calculated
from the array geometry.

Settings associate named choices with fuse positions and bit encodings.
Routing muxes associate choices with signals or products. Registers name
data, clock, type, reset, preset and power-up behavior. Feedback declarations
identify the source, true/complement columns and node alias. The backend
assigns the table IDs and connection groups required by `.dl` readers.

A `macrocell_template` declares integer parameters. A
`macrocell Name : Template` supplies those parameters; expressions in the
template are evaluated for each instance. Backends reject unsupported
connections, incomplete selectors, overlapping or unassigned fuses, rows
or columns, and invalid aliases rather than ignoring them.

## Families and package maps

`family Parent` is abstract. `device Child : Parent` inherits fields and
blocks from a family or another device. Child fields replace parent fields;
child blocks replace blocks with the same kind and name. An explicit child
variant list replaces the entire inherited variant list. Missing parents,
cycles and excessive inheritance depth are errors.

```text
device ATF750C_PLCC28 : ATF750C {
    mnemonic = v750clcc
    package = PLCC28
    pins = 28
    pin_map {
        physical = [2..7, 9..14, 16..21, 23..28]
        unconnected = [1, 8, 15, 22]
    }
}
```

`physical` lists destination pins in reference-pin order, starting at one.
Additional `ground`, `supply` and `unconnected` lists cover package positions
where supported by the backend. Every physical position must occur exactly
once. Buried-node ordinals follow the new physical-pin range. Package maps
change pin identities and diagram labels while retaining silicon fuse
coordinates. A `variant` block selects named configuration choices without
duplicating the hardware topology.

## Catalog manifests

A `lincupl-device-catalog-1` manifest lists `.dev` files, mnemonics and active
directory flags in lookup order. Files must be inside that catalog directory.
Duplicate mnemonics remain separate entries; `cupl/p16l8__2.dev` retains the
second `p16l8` occurrence. Provenance fields are informational, not compiler
inputs.

A `lincupl-device-catalog-set-1` manifest lists child catalog directories in
precedence order. The first occurrence of each mnemonic wins.

The root manifest uses `lincupl-device-catalog-set-2`. It first selects
`Atmel` before `cupl`, producing 660 entries from the 709 catalog descriptions,
then replaces explicitly listed mnemonics with hardware descriptions:

```json
{
  "format": "lincupl-device-catalog-set-2",
  "catalogs": ["Atmel", "cupl"],
  "hardware": [
    {"file": "families/atf750c.dev", "devices": ["v750c"]}
  ]
}
```

Each replacement must already exist in the selected catalog and must be
emitted by the named hardware file. Duplicate replacements and files outside
the catalog set are errors. Lookup positions and directory flags remain
unchanged. `--device` selects from the resulting library; `--catalog-only`
selects the catalog records before replacement. Catalogs and individual
family files remain independently compilable.

Default assembly retains the four compatibility attributes from the selected
catalog entry. It matches each generated node's `legacy_width` by array, pin
and function, ignoring pin annotations on array declarations. Missing or
ambiguous metadata matches are errors. All hardware fields come from the
family model, including its documented corrections.

See [implementation and deliberate differences](device-implementation.md)
for comparison rules and validation boundaries.
