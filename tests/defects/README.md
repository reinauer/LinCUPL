# Compiler and simulator defect regressions

Run the registered cases with `ctest --test-dir build --output-on-failure`,
or one case directly:

```
python3 tests/defects.py tests/defects/baseline.json --bin-dir build/bin
```

The runner finds the locally extracted merged device library, or accepts
`--library /path/to/lincupl.dl`. Missing device data produces a visible skip,
not a passing test. `--keep /path/to/new-directory` retains sources, outputs
and command logs. Each compiler and simulator invocation has a deadline.

Cases named `bNNN.json` use stable defect identifiers. These are independently
written regressions; the suite requires no external compiler or fixture
corpus. Each case contains a device name and labelled source variants.
A variant can check:

* A required diagnostic and the absence of a JEDEC file on rejection.
* Fuse equality against an independently expressed reference equation.
* Specific fuse values whose addresses follow the device architecture.
* Expected `.si` vectors through compiled-design or JEDEC simulation.

Exact fuse equality is appropriate only where both expressions should
produce the same implementation. It is stronger than behavioral equality
and is not a replacement for a general equivalence checker. Tests should
include nearby valid designs as well as defect triggers. The baseline case
exercises successful compilation, diagnostics, fuse comparison and both
simulation paths before device-specific cases are added.
