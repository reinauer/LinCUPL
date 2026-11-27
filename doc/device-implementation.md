# Device implementation and deliberate differences

## Catalogs and default library

| Sources | Entries | Behavior |
| --- | ---: | --- |
| `devices/Atmel/*.dev` | 85 | Preserve the records and order of `Atmel.dl` |
| `devices/cupl/*.dev` | 624 | Preserve `cupl.dl`, including both `p16l8` entries |
| `devices/manifest.json` | 660 selected | Atmel precedence, then 17 explicit hardware replacements |
| `devices/families/*.dev` | 17 interfaces | Default hardware models with the deliberate differences below |

The normal build compiles and installs `lincupl.dl` from the two complete
catalogs and the three hardware-family files. The manifest replaces 17
entries in place; the other 643 images, lookup order and directory flags
are unchanged. It needs no binary device library or target executable, including
when cross-compiling. Source/manifests and compiler changes trigger rebuilding.
Optional supporting data cannot overwrite the generated default library.

The catalogs describe the existing compiler interface. They do not add a
fitter or simulator architecture where the tools lack one. External-fitter
entries describe invocation and input formats, not their internal fuse maps.
The 17 modeled interfaces cover eight ATF750C package/options, five 22V10
interfaces and four ATF2500C package/keeper combinations. Earlier ATF2500
revisions and special 22V10 modes remain available through the catalog files.
`devcompile.py devices --catalog-only` builds the combined archival catalog
without hardware replacements. The individual source catalogs are retained
in full and still compile independently.

## Storage and comparison rules

Generated headers contain name `LinCUPL`, revision `v1.0` and description
`LinCUPL device descriptions`. Directory names, active flags and lookup
order are retained. Image lengths, record counts, widths and offsets are
calculated; section capacities equal counts and stored process pointers
are zero. Unreferenced image gaps are not emitted. Text fields retain all
bytes before their first NUL; bytes after it are zero.

Current readers locate records by count, width and offset, and allocate
images by image length. Descriptor capacity is not a silicon resource or
an allocation size in these readers: changing 5,000 to 94 saves no memory
by itself. Attribute resource limits are separate and remain intact.

| Section | Bytes | Disposition and established meaning |
| --- | --- | --- |
| Attributes (3) | 4 | Retained as `compatibility.attribute_4`; nonzero only in `f506`, `f506lcc` and `mapl144` |
| Attributes (3) | 5, 7, 20 | Retained as `compatibility.attribute_5`, `attribute_7` and `attribute_20`; no circuit meaning established |
| Attributes (3) | 6, 31–36, 50, 57–60 | Reserved zero regions; zero in all 709 entries, emitted as zero and compared |
| Pin functions (6) | 9 | Reserved zero byte; zero in every source record, emitted as zero and compared |
| Node functions (7) | 26 | Retained as `legacy_width`; the historical field label is `width`, distinct from the next byte's `jwidth` |
| Connections (10) | 4 | Reserved zero byte; zero in every source record, emitted as zero and compared |

No current native computation reads the five compatibility fields. The
node diagnostic labels establish that `legacy_width` and `row_width` are
separate quantities, not that one can be derived from the other. For example,
`p10l8` array records have legacy width 32 and fuse row width 20. The exact
values remain in every catalog description; compilation no longer discards
them. Assigning circuit names to the four attributes would imply knowledge
that the reader audit and controlled tests do not establish.

`audit.py` requires each record byte to belong to a named field or an
explicit reserved zero region, rejecting gaps and overlaps. Reserved regions
are not comparison exclusions. A nonzero value on either side differs from
the catalog's zero, so extending the supported format requires reviewing
that field. Zero-only observations are not proof that the bytes are unused
in every library tool.

Connection byte 12 is a configuration value and is retained and compared.
Attribute bytes 51–56 describe generic `SEQUENCE` expansion and are also
retained and compared, including alternate function codes when the count
selects only the primary function.

Controlled compilations preserved JEDEC, ABS and normalized documentation
when the compatibility fields, section capacities or stored pointers were
changed in selected devices. Generic state-machine count changes supplied
positive failure controls. These results support the tested reader behavior;
they do not justify deleting compatibility data or assigning it new meaning.

Separate reference simulations cover `p10l8`, `g20v8`, `v750c` and `f506`.
For each, independently setting each of the four attributes and every legacy
width to a nonzero sentinel, or clearing all five fields, preserved the
waveform and listing apart from its creation time. Wrong expected outputs
failed in all four cases. Changing the power-up attribute changed both
registered-device waveforms, confirming that the selected library affects
simulation. These are bounded behavioral checks, not a claim that every
possible historical consumer ignores the compatibility fields.
The rebuilt default library also reproduces all four reference simulations,
including the regenerated header, capacities, pointers and image layout.

Catalog comparison checks every record byte, including reserved bytes,
record order, inactive operands and empty records. Only bytes after a text
terminator are normalized. This is record equality, not whole-file identity:
headers, section capacities, stored pointers and image layout follow the
storage rules above. The native reader applies its existing repairs to both
reference and catalog-compiled images, and tests compare those results too.

The field-preservation audit is complete: every byte has an explicit
retention or normalization rule, and unexplained nonzero fields are retained.
The four attribute meanings and the numerical interpretation of legacy
width remain unspecified. They are compatibility data, not prerequisites
for describing the hardware resources used by the compiler and simulator.

## Deliberate hardware-model differences

The default library now uses these models. Assembly copies the four
compatibility attributes from each selected catalog entry and retains node
`legacy_width` by array, pin and function, rather than by record position.
Array declarations are matched by array and function alone. Missing matches
or conflicting catalog values fail the build. The unsupported ATF2500C
records intentionally omitted below remain in the archival catalog sources.
This metadata transfer changes no modeled hardware field and does not infer
legacy width from the fuse-row width.

The family files generate table IDs, connection groups and plotting programs
from hardware resources. Their generated images must need no native reader
repairs. Unlike catalog comparison, hardware comparison applies the documented
reader repairs to references, clears inactive PROM operands, and omits empty
fitter-argument records. The five compatibility fields are reported separately
as `compatibility_equal` and `compatibility_differences`, with per-field counts
and examples. They are normalized only for the hardware record comparison;
`canonical_equal` alone does not claim preservation of that metadata.
Reserved bytes remain part of both comparisons. All remaining differences
are reported.

| Family / interface | Deliberate difference |
| --- | --- |
| ATF750C, all | Encode the existing `.tfb`, register-alias column and buried-D polarity repairs directly. Clear inactive pin configuration operands and the inconsistent pin-22 OE array annotation. |
| `v750c`, `v750cextppk` | Canonical records match the reference. |
| `v750cext` | Use consistent D/T feedback polarity instead of extra inversion flags on these functions and their aliases. |
| `v750cppk` | Use the base device's pin-16 D/T feedback columns and alias columns. |
| ATF750C PLCC variants | Apply DIP silicon behavior through the package map: remove extra feedback inversion flags, correct two reversed pairs in the base entry, and use the modeled kind for 30 clock-selector array records. |
| `g22v10` | Canonical records match the reference. |
| Other modeled 22V10 interfaces | Use a common reset connection group and consistent revision markers. |
| `g22v10lcc`, `g22v10i` | Generate plot labels at the actual product-row boundaries for the first five outputs. |
| ATF2500C, all | Omit 24 unsupported buried combinational-assignment records, matching the native reader correction, including keeper variants. Retain the physical feedback bypass in the mux model. |
| ATF2500C, all | Order shared-preset records by macrocell. The reviewed records match as a multiset. |
| ATF2500C PLCC | Order four additional power-pin records by physical pin and use the four-sided package diagram style. |
| ATF2500C, all | Generate a compact fuse plot covering every address once; its grouping differs from the catalog plot. |

ATF750C power-down and keeper options do not alter register feedback paths.
The variant tests check that only their option bits differ for equivalent
designs. The ATF2500C user row retains its initialized-one behavior; it is
not exposed as the compiler's part-number signature. Undocumented compatibility
fuses keep explicit defaults without an inferred circuit function.

## Validation

`devcatalog_sources` compiles all 709 descriptions and the combined 660-entry
catalog without reference binaries. Independent mutation checks cover all 19
formerly excluded byte offsets in both comparison directions, including
compatibility differences and reserved-byte failures. `devcatalog_libraries`
compares all entries against optional reference libraries through every
record byte and the
native C reader, then checks clock-selection fuses and fitter input output.
The combined archival catalog is also compared with a library built by the
native merger. `devdefault` independently checks all 17 replacements against
their hardware models, all compatibility values against their catalogs,
and all 643 fallback images byte for byte. It checks that a family-source
edit changes the default image, that malformed replacements fail, and that
the built library requires no native reader repairs for modeled entries.
For every modeled interface it compares default compilation against an
explicit family library and checks both ABS and JEDEC simulation vectors.
`devsequence` checks generic state-machine declarations against explicit
register-type declarations, registered `OUT` clauses, and expected simulation
vectors. It exercises both source catalogs and all modeled interfaces without
requiring reference binaries.

Family tests check complete fuse ownership, selector encodings, package
maps, reader stability, emitted JEDEC maps and expected ABS/JEDEC simulation
vectors. They cover 160 ATF750C package pairs, 250 22V10 designs and 576
ATF2500C designs, plus independent handwritten maps. These are software
checks, not physical programming measurements.

```sh
cmake --build build
ctest --test-dir build --output-on-failure
```

Reference comparisons skip when reference data is absent. Configure with
`-DLINCUPL_REQUIRE_TEST_DATA=ON` to require that additional data.

## Retained field consumers

Offsets are decimal and relative to each record. The schemas live in
`tools/devlang/catalog.py`; reserved regions and compatibility fields are
listed in `tools/devlang/audit.py`.
These names describe the interface consumed by the native compiler and
simulator, not necessarily a physical silicon property.

### Attributes, section 3, 152 bytes

| Bytes | Retained field | Consumer and established interpretation |
| --- | --- | --- |
| 0–1 | `compiler.revision` | `expand_load_device()` rejects zero support; `banner_lines()` prints the revision |
| 2 | `power.ground` | `src/cupl/netlist.c` uses the ground pin in generated device netlists |
| 3 | `power.supply` | `src/cupl/netlist.c` uses the supply pin in generated device netlists |
| 8 | `package.pins` | Symbol bounds in expansion, fuse mapper, package diagrams and simulator |
| 9 | `prom.address_bits` | `src/cupl/prom.c:build_prom_map()` uses PROM address width |
| 10 | `prom.data_width` | `build_prom_map()` computes data width modulo 17 |
| 11 | `package.drawing_style` | `src/cupl/doc.c:write_chip_diagram()` selects DIP versus LCC drawing |
| 12–13 | `package.internal_nodes` | Expansion and netlists size the additional node namespace |
| 14 | `compiler.extra_nodes_a` | `expand_load_device()` adds this count to the internal-node limit |
| 15 | `compiler.extra_nodes_b` | Same allocation path |
| 16 | `compiler.extra_nodes_c` | Same allocation path |
| 17 | `compiler.family` | Compiler/simulator family dispatch; numeric modes without established names remain numeric |
| 18 | `fitter.input_format` | `src/cupl/fusemap.c` selects native mapping or fitter input; simulator also checks supported classes |
| 19 | `fuses.default` | `src/cupl/jedec.c` initializes/compacts fuse data against this default |
| 21–24 | `fuses.count` | Fuse-buffer size and output boundaries; `src/sim/jedload.c` checks the JEDEC size |
| 25–26 | `compiler.register_resources` | MAX expander allocation in `src/cupl/fusemap.c`; simulator tests register availability |
| 27 | `compiler.clocked_resources` | `src/sim/core.c:init_levels()` selects register initialization behavior |
| 28 | `compiler.default_extension` | `src/cupl/expand_device.c:mark_usage()` activates the default function |
| 29–30 | `compiler.product_rows` | PROM word count and product-term resources; interpretation is family dependent |
| 37–38 | `compiler.initial_term_capacity` | `src/cupl/expand_frame.c` sizes initial term frames; not a uniform physical limit |
| 39–40 | `compiler.array_slots` | Expansion bitmaps, fuse mapper OR-array table and simulator term table |
| 41–42 | `compiler.input_line_slots` | `expand_alloc_usage_maps()` allocates line ownership |
| 43 | `compiler.download` | `src/cupl/fusemap.c` selects JEDEC/hex/HL output |
| 44–45 | `compiler.extra_symbols` | `add_linked_functions()` sizes generated default/control symbols |
| 46 | `simulation.output_enable_mode` | `src/sim/eval.c` combines output-enable conditions |
| 47 | `simulation.output_polarity_mode` | `src/sim/eval.c` and `src/sim/core.c` select output/register polarity behavior |
| 48 | `simulation.internal_polarity_mode` | `src/sim/eval.c` and compiler polarity handling select internal-feedback conventions |
| 49 | `simulation.power_up` | `src/sim/core.c:init_levels()` selects unknown/low/high initial register state |
| 51 | `sequence.next_function` | Declared as `ARCH_SEQ_NEXT_1` in `src/cupl/parse_int.h`; primary next-state function descriptor, retained even though the parser currently uses byte 54 for both next-state and registered-output expansion |
| 52 | `sequence.next_alternate` | Declared as `ARCH_SEQ_NEXT_2`; alternate next-state function descriptor, with the same consumer limitation as byte 51 |
| 53 | `sequence.next_count` | `src/cupl/parse_sequence.c:act_sequence_begin()` sets the number of next-state equations; zero rejects generic `SEQUENCE` |
| 54 | `sequence.input_function` | `act_sequence_begin()` resolves the primary register-input extension used by `make_next_statement()` and `make_output_statement()` |
| 55 | `sequence.input_alternate` | Same path resolves the second register-input extension, used when the count selects two equations, such as `.s`/`.r` or `.j`/`.k` |
| 56 | `sequence.input_count` | `act_sequence_begin()` sets the number of registered-output equations; zero rejects generic `SEQUENCE` |
| 61 | `signature.bits` | JEDEC writer/loader signature length |
| 62–65 | `signature.first_fuse` | JEDEC writer/loader signature position |
| 66 | `simulation.preload_polarity` | `src/sim/core.c` determines preload polarity |
| 67–79 | `fitter.name` | `src/cupl/fusemap.c` constructs the external fitter invocation |
| 80–111 | `fitter.arguments` | Same invocation path; a separate string from PLA options |
| 112–131 | `fitter.pla_options` | `src/cupl/pla.c:select_keywords()` and TT2 output consume option characters |
| 132–151 | `fitter.part` | External fitter input/invocation uses the part name |

Some simulator paths read a 16-bit quantity starting at attribute byte 8;
the catalog retains both bytes 8 and 9. The profile does not reinterpret or
repair that historical overlap.

The next-state and input triples (51–53 and 54–56) agree in all 709 source
entries. They remain separate fields rather than treating that observation
as a format requirement. For example, D-register entries normally specify
`d, none, 1`, SR entries specify `s, r, 2`, and JK entries specify `j, k, 2`.
ATF2500C retains `d, ce, 1`, including its unused alternate descriptor.
Explicit `SEQUENCED`, `SEQUENCERS`, `SEQUENCEJK` and `SEQUENCET` select their
own input functions; testing only those statements cannot validate these
device defaults.

### Pin functions, section 6, 16 bytes

The layouts are named in `src/cupl/expand_int.h` and
`src/cupl/fusemap.h`. Expansion binds the pin/function rows; the fuse mapper
and JEDEC simulator resolve their input columns and configuration fuses.

| Bytes | Field | Consumer and established interpretation |
| --- | --- | --- |
| 0–1 | `line` | Expansion associates a row with its input line; connection searches match it |
| 2–3 | `pin` | Expansion and simulation bind a physical pin or internal node |
| 4 | `function` | CUPL extension for this feedback/input function |
| 5 | `compiler_type` | Compiler/simulator pin kind, including supply, clock, input, output and feedback |
| 6 | `variant` | `find_pin_function()` / `find_pin_variant()` choose mode-specific input rows |
| 7 | `true_column` | Fuse mapper and `src/sim/jedload.c` locate the true product-term input |
| 8 | `complement_column` | Same paths locate the complementary input |
| 10 | `config_value` | `src/cupl/jedec.c` emits pin configuration bits |
| 11 | `config_bits` | Width/enabling of that configuration write |
| 12–15 | `config_first_fuse` | First fuse of that configuration write |

### Node functions, section 7, 39 bytes

| Bytes | Field | Consumer and established interpretation |
| --- | --- | --- |
| 0–1 | `array` | Expansion/fuse mapper associate a function with a term array |
| 2–3 | `pin` | Physical pin or internal node of the function; array declarations may carry annotations |
| 4 | `function` | CUPL function or array declaration, used by expansion, mapper and simulator |
| 5 | `polarity` | Function polarity/control kind; `polarity_mode()` and macrocell selection |
| 6 | `invert` | `src/cupl/jedec.c` adjusts function/feedback polarity |
| 7–8 | `select_value` | `check_macrocell()` emits ordinary or split selector patterns |
| 9 | `config_value` | `check_macrocell()` emits additional configuration bits |
| 10–13 | `first_row_fuse` | AND-row start or selector-fuse start, depending on function |
| 14–17 | `first_or_fuse` | Programmable OR-array position; expansion also reads this as `NODEFN_GROUP` |
| 18–21 | `config_first_fuse` | Additional configuration-fuse start |
| 22–25 | `pair_first_fuse` | Source-dependent mux/control-selection fuse start |
| 26 | `legacy_width` | Retained diagnostic width metadata; not used for fuse stride and not derived from `row_width` |
| 27 | `row_width` | AND row width or selector bit count in `src/cupl/jedec.c` |
| 28 | `or_width` | Programmable OR row width in mapper and JEDEC writer |
| 29 | `config_bits` | Additional configuration bit count |
| 30 | `pair_bits` | Source-dependent selection bit count, passed to `set_pair_bits()` |
| 31–32 | `max_terms` | Expansion/fitter term allocation; may be adjusted during placement |
| 33 | `split_terms` | Split-array/XOR term boundary in expansion and mapper |
| 34 | `default_rule` | `add_linked_function()` selects default, bank or expression requirements |
| 35 | `has_default` | Expansion/default generation and mapper function selection |
| 36 | `sharing_mode` | Expansion and mapper sharing/placement rules; retained as an ABI mode |
| 37 | `active` | Active-function selection in expansion and mapping |
| 38 | `alternate_step` | Alternate-node allocation/partner lookup in expansion and mapper |

`NODE_B26` in the native C declarations means hexadecimal offset `0x26`,
which is decimal **38**. It does not identify decimal byte 26 (`legacy_width`).

### Connections, section 10, 13 bytes

| Bytes | Field | Consumer and established interpretation |
| --- | --- | --- |
| 0–1 | `line` | Expansion and `find_extension_pair()` match the owning line/array |
| 2–3 | `pin` | Owner/source pin or node in connection lookup |
| 5–6 | `function` | Function used by compiler connection lookup; simulator checks its low byte |
| 7–8 | `group` | Compiler/simulator join related functions through a common group |
| 9 | `member` | Compiler distinguishes own versus other group members |
| 10 | `source_pin` | Retained source flag; simulator combines bytes 9–10 as its 16-bit member selector |
| 11 | `compiler_link` | Expansion's `link_iter_*()` selects the link class for default/control generation |
| 12 | `config_value` | `src/cupl/jedec.c:set_pair_bits()` writes this value to the node's pair-selection fuses |

Byte 12 is not padding. The source pin and member select a connection;
its configuration value is a separate input to fuse generation. Tests
check the full connection table without exclusions and exercise global
clock and inverted-clock selections through the native JEDEC writer.

### Remaining tables

| Section / bytes | Field | Consumer and established interpretation |
| --- | --- | --- |
| 4 / 0–19 | `extensions.name` | `extension_code()` / `extension_name()` translate CUPL extension spelling |
| 4 / 20 | `extensions.function` | Numeric function paired with that spelling |
| 5 / 0–19 | `fitter_extensions.extension` | `src/cupl/pla.c` looks up the CUPL extension |
| 5 / 20–39 | `fitter_extensions.alias` | The spelling written for the external fitter |
| 8 / 0–8 | `plot.text` | `src/cupl/doc.c` plotting interpreter text |
| 8 / 9 | `plot.operation` | Interpreter opcode |
| 8 / 10 | `plot.group` | Interpreter loop/group selection |
| 8 / 11–14 | `plot.a` | First opcode-specific operand |
| 8 / 15–18 | `plot.b` | Second opcode-specific operand |
| 8 / 19–22 | `plot.c` | Third opcode-specific operand |
| 8 / 23–26 | `plot.d` | Fourth opcode-specific operand |
| 9 / 0–3 | `initialization.first_fuse` | JEDEC initialization range start |
| 9 / 4–7 | `initialization.last_fuse` | Inclusive initialization range end |
| 9 / 8 | `initialization.value` | Initial value, applied in record order |
| 11 / 0 | `fitter_arguments.kind` | `src/cupl/fusemap.c` selects filename, part, literal or directory substitution |
| 11 / 1–23 | `fitter_arguments.text` | Ordered argument content; empty/terminating rows remain present |
| 12 / 0–19 | `alternatives.device` | `pick_alternative_device()` selects another library mnemonic |
| 12 / 20 | `alternatives.condition` | D/OE/combinational/OEMUX/unconditional selection condition |
| 12 / 21 | `alternatives.priority` | Tie/selection priority in directory alternatives |
