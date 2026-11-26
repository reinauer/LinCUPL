# SPDX-License-Identifier: BSD-2-Clause
"""Reviewed exclusions from the imported library's record comparison.

These ranges have no identified consumer in the current native readers.
They are not asserted to be padding or meaningless in other tools. Source
libraries are retained separately from compiled device descriptions.
See doc/device-implementation.md for consumers and remaining questions.
"""
from .syntax import Error


# (first byte, width), relative to a record. Every section is explicit,
# including sections for which no whole-field exclusion is permitted.
UNMAPPED_RANGES = {
    3: ((4, 4), (20, 1), (31, 6), (50, 11)),
    4: (),
    5: (),
    6: ((9, 1),),
    7: ((26, 1),),
    8: (),
    9: (),
    10: ((4, 1),),
    11: (),
    12: (),
}


def excluded_offsets(section):
    return tuple(offset for start, width in UNMAPPED_RANGES[section]
                 for offset in range(start, start + width))


def validate_coverage(fields, widths):
    """Reject holes, overlaps and out-of-bounds schema/exclusion ranges.

    Losing a schema field must fail here, rather than silently classifying
    its bytes as another exclusion. Adding an interpreted field requires
    removing its reviewed exclusion in the same change.
    """
    expected = set(range(3, len(widths)))
    if set(fields) != expected or set(UNMAPPED_RANGES) != expected:
        raise Error('record schema and reviewed exclusions must cover every section')
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
        for start, width in UNMAPPED_RANGES[section]:
            cover(start, width, 'reviewed unmapped range')
        missing = [offset for offset, name in enumerate(owner) if name is None]
        if missing:
            raise Error(f'section {section}: bytes without a field or reviewed exclusion: {missing}')
