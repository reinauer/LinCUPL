# SPDX-License-Identifier: BSD-2-Clause
"""Record coverage and compatibility metadata in device libraries.

Reserved regions are zero in the supported catalogs and are emitted as
zero, but remain part of record comparison. Compatibility fields preserve
values whose hardware meaning is not established. Only hardware-model
comparison separates those fields from the functional records.
"""
from .syntax import Error


# (first byte, width), relative to a record. Every section is explicit,
# including sections without reserved bytes. These are not exclusions.
RESERVED_ZERO_RANGES = {
    3: ((6, 1), (31, 6), (50, 1), (57, 4)),
    4: (),
    5: (),
    6: ((9, 1),),
    7: (),
    8: (),
    9: (),
    10: ((4, 1),),
    11: (),
    12: (),
}


# (byte, name), used only to report hardware-model compatibility metadata
# independently. Catalog comparison checks every one of these bytes.
COMPATIBILITY_FIELDS = {
    3: ((4, 'attribute_4'), (5, 'attribute_5'),
        (7, 'attribute_7'), (20, 'attribute_20')),
    7: ((26, 'legacy_width'),),
}


def validate_coverage(fields, widths):
    """Reject holes, overlaps and out-of-bounds field/reserved ranges.

    Losing a schema field must fail here, rather than silently classifying
    its bytes as reserved. Adding a field in a reserved region requires
    reviewing and narrowing that region in the same change.
    """
    expected = set(range(3, len(widths)))
    if set(fields) != expected or set(RESERVED_ZERO_RANGES) != expected:
        raise Error('record schema and reserved regions must cover every section')
    for section in expected:
        owner = [None] * widths[section]

        def cover(start, width, name):
            if type(start) is not int or type(width) is not int or start < 0 or width <= 0 or start + width > len(owner):
                raise Error(f'section {section}: {name} is outside the record')
            for offset in range(start, start + width):
                if owner[offset] is not None:
                    raise Error(f'section {section} byte {offset}: {name} overlaps {owner[offset]}')
                owner[offset] = name

        for field in fields[section]:
            cover(field.offset, field.width, field.name)
        for start, width in RESERVED_ZERO_RANGES[section]:
            cover(start, width, 'reserved zero region')
        missing = [offset for offset, name in enumerate(owner) if name is None]
        if missing:
            raise Error(f'section {section}: bytes without a field or reserved region: {missing}')
