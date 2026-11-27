# SPDX-License-Identifier: BSD-2-Clause
"""Compare generated ATF750C device images with legacy library entries.

Report raw identity separately from canonical record equality. The latter
excludes storage metadata and applies the existing ATF750C reader repairs.
Compatibility metadata is reported separately. Every normalized byte is
counted by rule; all remaining record bytes, including reserved bytes, match.
"""
from collections import Counter
import hashlib
import struct
from .binary import (HEADER_SIZE, DIRECTORY_COUNT, ENTRY_SIZE, DATA_START,
                     SECTION_COUNT, SECTION_HEADER_SIZE, SECTION_NAMES, WIDTHS,
                     D, TFB, ARRAY, REGISTER, NORMAL_OUTPUT, POSITIVE, CONTROL)
from .model import require
from .audit import COMPATIBILITY_FIELDS


def u16(data, offset):
    return struct.unpack_from('<H', data, offset)[0]


def u32(data, offset):
    return struct.unpack_from('<I', data, offset)[0]


def images(data, first_match=False):
    require(len(data) >= DATA_START and data[:4] == struct.pack('<HH', 0xd431, 0x28),
            'invalid or truncated DL header/directory')
    result = {}
    for index in range(DIRECTORY_COUNT):
        entry = data[HEADER_SIZE + index * ENTRY_SIZE:HEADER_SIZE + (index + 1) * ENTRY_SIZE]
        if not entry[17] & 1:
            continue
        require(b'\0' in entry[:17], 'unterminated device mnemonic')
        name = entry[:17].split(b'\0')[0].decode('ascii')
        require(first_match or name not in result, f'duplicate device mnemonic {name}')
        size, offset = u32(entry, 18), u32(entry, 22)
        require(size >= SECTION_COUNT * SECTION_HEADER_SIZE and offset >= DATA_START and
                offset + size <= len(data), f'{name}: invalid image bounds')
        if name not in result:
            result[name] = data[offset:offset + size]
    return result


def records(image):
    require(len(image) >= SECTION_COUNT * SECTION_HEADER_SIZE, 'truncated device image')
    result, ranges = [], []
    for section in range(SECTION_COUNT):
        if section < 3:
            result.append([])
            continue
        header = section * SECTION_HEADER_SIZE
        count, width, offset = u16(image, header + 4), u16(image, header + 6), u32(image, header + 8)
        require(not count or (width == WIDTHS[section] and width > 0),
                f'{SECTION_NAMES[section]}: unsupported record size')
        end = offset + count * width
        require(not count or (offset >= SECTION_COUNT * SECTION_HEADER_SIZE and end <= len(image)),
                f'{SECTION_NAMES[section]}: records outside image')
        if count:
            require(all(end <= start or offset >= stop for start, stop in ranges), 'overlapping sections')
            ranges.append((offset, end))
        result.append([bytearray(image[offset + i * width:offset + (i + 1) * width]) for i in range(count)])
    require(len(result[3]) == 1, 'device must contain one attribute record')
    return result


def normalize_compatibility(sections, changes):
    for section, fields in COMPATIBILITY_FIELDS.items():
        for record in sections[section]:
            for offset, name in fields:
                changes[f'compatibility metadata: {name}'] += record[offset] != 0
                record[offset] = 0


def compatibility_differences(left, right):
    """Report metadata after the same row removals as hardware comparison."""
    differences = []
    for section, fields in COMPATIBILITY_FIELDS.items():
        for offset, name in fields:
            a = [r[offset] for r in left[section]]
            b = [r[offset] for r in right[section]]
            if a != b:
                differences.append(dict(section=SECTION_NAMES[section], field=name,
                    generated_records=len(a), reference_records=len(b),
                    different_records=sum(x != y for x, y in zip(a, b)) + abs(len(a) - len(b)),
                    examples=[dict(record=i, generated=a[i] if i < len(a) else None,
                                   reference=b[i] if i < len(b) else None)
                              for i in range(max(len(a), len(b)))
                              if i >= len(a) or i >= len(b) or a[i] != b[i]][:8]))
    return differences


def canonical(image, repairs=True, family='atf750c', compatibility=True):
    require(family in ('atf750c', 'gal22v10', 'atf2500'), 'unsupported hardware comparison family')
    result = records(image)
    changes = Counter()
    def replace(record, offset, value, rule):
        value = bytes(value)
        changes[rule] += sum(a != b for a, b in zip(record[offset:offset + len(value)], value))
        record[offset:offset + len(value)] = value
    def zero(record, offsets, rule):
        for offset in offsets:
            replace(record, offset, b'\0', rule)
    attr = result[3][0]
    npins = attr[8]
    if repairs and family == 'atf2500':
        # Match correct_2500_buried_functions in the native reader. The
        # hardware feedback bypass does not expose an independent legacy
        # combinational equation target for these buried registers.
        removed = [r for r in result[7] if npins < u16(r, 2) <= npins + 24 and r[4] == 0]
        changes['unsupported buried combinational assignment records'] += len(removed)
        result[7] = [r for r in result[7] if not (npins < u16(r, 2) <= npins + 24 and r[4] == 0)]
    if repairs and family == 'atf750c':
        for p in result[6]:
            if p[4] == TFB:
                replace(p, 11, b'\0', 'TFB must not configure register type')
        for alias in result[7]:
            if u16(alias, 2) <= npins or alias[4] != D:
                continue
            for parent in result[7]:
                if (u16(parent, 0) != u16(alias, 0) or u16(parent, 2) > npins or
                        parent[4] != D or parent[5] != NORMAL_OUTPUT):
                    continue
                for source in result[6]:
                    if u16(source, 2) != u16(parent, 2) or source[4] != 0 or source[6] != D:
                        continue
                    flip = int((alias[6] != 0) != (parent[6] != 0))
                    for p in result[6]:
                        if u16(p, 2) == u16(alias, 2) and p[4] == 0 and p[5] == REGISTER and p[6] == 0:
                            replace(p, 7, bytes([source[7 + flip], source[8 - flip]]), 'register alias columns')
        for n in result[7]:
            if u16(n, 2) > npins and n[4] == D and n[5] == CONTROL:
                replace(n, 5, bytes([POSITIVE]), 'buried D fixed positive polarity')
    # These two fields have PROM consumers, but no PROM meaning in the
    # supported PLD architectures. Do not classify them as unmapped.
    zero(attr, [9, 10], f'inactive {family.upper()} PROM operands')
    if compatibility:
        normalize_compatibility(result, changes)
    if family == 'atf750c':
        for p in result[6]:
            if p[11] == 0:
                zero(p, [10, 12, 13, 14, 15], 'inactive pin configuration operands')
        for n in result[7]:
            if n[4] == ARRAY:
                # Array declarations are resolved by array number, not pin.
                # The shipped map inconsistently annotates only pin 22's OE.
                zero(n, [2, 3], 'array declaration pin annotation')
    for section, fields in [(3, [(67, 13), (80, 32), (112, 20), (132, 20)]),
                            (4, [(0, 20)]), (8, [(0, 9)]), (11, [(1, 23)])]:
        for r in result[section]:
            for start, length in fields:
                text = r[start:start + length]
                require(0 in text, f'{SECTION_NAMES[section]}: unterminated text field')
                zero(r, range(start + text.index(0) + 1, start + length), 'bytes after text terminator')
    empty = [r for r in result[11] if not any(r)]
    if empty:
        changes['empty fitter argument records'] += len(empty)
        result[11] = [r for r in result[11] if any(r)]
    return result, {k: v for k, v in changes.items() if v}


def compare_image(generated, reference, family='atf750c'):
    # A compiler must emit corrected records itself. Repairing its output
    # here could hide a regression in the writer.
    left, left_rules = canonical(generated, repairs=False, family=family, compatibility=False)
    right, right_rules = canonical(reference, family=family, compatibility=False)
    metadata = compatibility_differences(left, right)
    left_rules, right_rules = Counter(left_rules), Counter(right_rules)
    normalize_compatibility(left, left_rules)
    normalize_compatibility(right, right_rules)
    differences = []
    section_results = []
    for section, (a, b) in enumerate(zip(left, right)):
        different_bytes = sum(x != y for r, s in zip(a, b) for x, y in zip(r, s))
        equal = a == b
        section_results.append(dict(section=SECTION_NAMES[section], generated_records=len(a),
                                    reference_records=len(b), equal=equal))
        if not equal:
            differences.append(dict(section=SECTION_NAMES[section], different_bytes=different_bytes,
                                    record_count_delta=len(a) - len(b),
                                    examples=[dict(record=i, byte=j, generated=x, reference=y)
                                              for i, (r, s) in enumerate(zip(a, b))
                                              for j, (x, y) in enumerate(zip(r, s)) if x != y][:12]))
    return dict(raw_equal=generated == reference, generated_bytes=len(generated), reference_bytes=len(reference),
                generated_sha256=hashlib.sha256(generated).hexdigest(),
                reference_sha256=hashlib.sha256(reference).hexdigest(),
                canonical_equal=left == right, sections=section_results, differences=differences,
                compatibility_equal=not metadata, compatibility_differences=metadata,
                generated_normalizations={k: v for k, v in left_rules.items() if v},
                reference_normalizations={k: v for k, v in right_rules.items() if v},
                storage_note='Section offsets, capacities, process pointers and image padding are excluded; '
                             'record order and every remaining record byte are compared.')


def compare_libraries(generated, reference):
    # Native lookup selects the first active entry. Later duplicates in a
    # source catalog must not prevent comparison of an unrelated device.
    actual, original = images(generated), images(reference, first_match=True)
    require(set(actual) <= set(original), 'reference library is missing generated device entries')
    dip = {'v750c', 'v750cext', 'v750cppk', 'v750cextppk'}
    families = {name: 'atf750c' for name in dip | {name + 'lcc' for name in dip}}
    families.update({name: 'gal22v10' for name in
                     ('g22v10', 'g22v10lcc', 'g22v10i', 'p22v10', 'p22v10lcc')})
    families.update({name: 'atf2500' for name in
                     ('v2500c', 'v2500cppk', 'v2500clcc', 'v2500cppklcc')})
    require(set(actual) <= set(families), 'unsupported device in hardware comparison')
    return {name: compare_image(data, original[name], family=families[name])
            for name, data in actual.items()}
