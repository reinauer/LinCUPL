#!/usr/bin/env python3
"""
csim -f (simulate the fuse map of a JEDEC file) against csim on the compiled
design.

Every design (tests/csim_jed/*.pld, doc/help/examples/*.pld and, where the
extracted data is there, share/lincupl/Examples/Atmel/*.PLD for the GAL16V8,
GAL20V8 and GAL22V10) that has a .si file is compiled with "cupl -jaxm4",
which gives both <design>.abs and the .jed file, and simulated the usual way
("csim -lw").  The fuse map alone is then simulated with "csim -f" in
directories that have no .abs file:

  names    with the .pld file next to the .jed file (csim takes the pin names
           from it, also those its preprocessor directives make) and the
           unchanged .si file.  The listing (.so) and the waveform file
           (.wo) must be the same as those of the compiled design, and so
           must the JEDEC file after "csim -j".
  trace    as "names" with $TRACE 3 in front of the vectors: the listing
           with every equation as it is evaluated must be the same too
           (but for reset and preset terms written as always false).
  pins     without the .pld file, the names of the .si file replaced by the
           default names P<n>.  A name that is declared active low (!name)
           becomes !P<n>, which means the same; the vector lines of the
           listing must be those of the compiled design.
  levels   as "pins", but every column is P<n> (the level of the pin) and
           the test values of the columns that were active low are inverted
           in the .si file.  The vector lines must be those of the compiled
           design with these columns inverted, and "csim -j" must write the
           same test vectors into the JEDEC file (they are pin levels).
  device   as "pins" with the device of the design named on the command
           line (csim -f <device> <design>).
  alias    as "pins" with the .pld file next to the .jed file: P<n> stands
           for the pin whatever name the file gives it (in the ORDER
           statement; designs whose .si file names pins elsewhere are left
           out).

The device is named only in "device": a file cupl wrote names its device,
which matters for a PLCC package (other pin numbers).

Further cases check that fuse maps as other programs write them are read
(no STX/ETX, fields starting with '*', lower case, CR LF, missing QF, notes
and vectors, a 22V10 map without its signature fuses), that damaged ones
are refused with a message (cut short, wrong fuse checksum, fuse without a
state, a number of fuses no supported device has), which device a file is
taken for that names none (by its fuses and, with QP28, its pins), and that
product terms of a macrocell which the fuse map makes an input are
reported.

    csim_jed.py --bin-dir build/bin [--share DIR] [-k SUBSTR] [--keep DIR]
    csim_jed.py --bin-dir build/bin --corpus DIR   # load and run every .jed under DIR

Exit status: 0 all passed, 1 failures, 77 no device library found (skipped).
"""
import argparse
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
OWN = os.path.join(ROOT, 'tests', 'csim_jed')
HELP = os.path.join(ROOT, 'doc', 'help', 'examples')
SUPPORTED = re.compile(r'^(g16v8|g20v8|g22v10|p22v10)')
# Devices of the same families whose fuse maps have another fuse (power-down
# option) and are not simulated from a JEDEC file
UNSUPPORTED = re.compile(r'^g(16v8|20v8|22v10)cp')
SWAP = {'0': '1', '1': '0', 'L': 'H', 'H': 'L', 'l': 'h', 'h': 'l'}


class Unsupported(Exception):
    """The .si file cannot be rewritten for this mode."""


class Tools:
    def __init__(self, bin_dir, share):
        self.bin_dir = bin_dir
        self.env = dict(os.environ, LINCUPL_SHARE=share)
        self.env.pop('LIBCUPL', None)

    def run(self, tool, args, cwd):
        r = subprocess.run([os.path.join(self.bin_dir, tool)] + args, cwd=cwd, env=self.env,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
        return r.returncode, r.stdout.decode('latin1').replace('\r', '')


def read_text(path):
    return open(path, 'rb').read().decode('latin1').replace('\r', '')


def find_ci(directory, name):
    for f in os.listdir(directory):
        if f.lower() == name.lower():
            return os.path.join(directory, f)
    return None


# ---------------------------------------------------------------------------
# The pin declarations of a design
# ---------------------------------------------------------------------------
def pld_source(path, depth=0, includes=True, defines=None):
    """The text of a .pld file without comments, $include files put in,
    names defined with $define replaced and the lines that $ifdef leaves
    out dropped.  (Roughly what cupl's preprocessor does; the pins of a
    compiled design are taken from its .doc file, see doc_pins.)"""
    text = re.sub(r'/\*.*?\*/', ' ', read_text(path), flags=re.S)
    defines = {} if defines is None else defines
    out, levels = [], []            # levels: [the text around is read, the condition holds]
    for line in text.split('\n'):
        reading = all(outer and taken for outer, taken in levels)
        m = re.match(r'\s*\$(\w+)\s*(\S*)\s*(.*?)\s*$', line)
        if not m:
            if reading:
                for name in sorted(defines, key=len, reverse=True):
                    line = re.sub(re.escape(name) + r'(?![A-Za-z])', lambda _, v=defines[name]: v, line)
                out.append(line)
            continue
        word, name, rest = m.group(1).lower(), m.group(2), m.group(3)
        if word in ('ifdef', 'ifndef'):
            levels.append([reading, (name in defines) == (word == 'ifdef')])
        elif word == 'else' and levels:
            levels[-1][1] = not levels[-1][1]
        elif word == 'endif' and levels:
            levels.pop()
        elif not reading:
            pass
        elif word == 'define':
            defines.setdefault(name, rest)
        elif word == 'undef':
            defines.pop(name, None)
        elif word == 'include' and depth < 4:
            inc = find_ci(os.path.dirname(path), name.strip('";'))
            if inc and includes:
                out.append(pld_source(inc, depth + 1, True, defines))
    return '\n'.join(out)


def expand_numbers(text):
    out = []
    for item in text.split(','):
        m = re.match(r'\s*(\d+)\s*(?:\.\.\s*(\d+))?\s*$', item)
        if not m:
            raise ValueError(item)
        a = int(m.group(1))
        b = int(m.group(2)) if m.group(2) else a
        out += list(range(a, b + 1)) if b >= a else list(range(a, b - 1, -1))
    return out


def expand_names(text):
    """[(name, low)] of a name list (the text between the brackets)."""
    out = []
    for item in text.split(','):
        m = re.match(r'\s*(!?)\s*([A-Za-z_0-9]+?)(\d+)\s*\.\.\s*(?:\2)?(\d+)\s*$', item)
        if m:
            a, b = int(m.group(3)), int(m.group(4))
            for v in (range(a, b + 1) if b >= a else range(a, b - 1, -1)):
                out.append((m.group(2) + str(v), m.group(1) == '!'))
            continue
        m = re.match(r'\s*(!?)\s*([A-Za-z_0-9]+)\s*$', item)
        if not m:
            raise ValueError(item)
        out.append((m.group(2), m.group(1) == '!'))
    return out


def pld_pins(path, includes=True):
    """({name: (pin, low)}, {field: [member names]}, device) of a design."""
    text = pld_source(path, 0, includes)
    pins, fields = {}, {}
    m = re.search(r'^\s*device\s+(\S+?)\s*;', text, re.I | re.M)
    device = m.group(1).lower() if m else ''
    for stmt in text.split(';'):
        stmt = stmt.strip()
        m = re.search(r'(?:^|\n)\s*pin\s*(\[[^\]]*\]|\d+)\s*=\s*(!?)\s*(\[[^\]]*\]|[A-Za-z_0-9]+)\s*$', stmt, re.I)
        if m:
            numbers = expand_numbers(m.group(1).strip('[]'))
            names = expand_names(m.group(3).strip('[]'))
            if len(numbers) == len(names):
                for n, (name, low) in zip(numbers, names):
                    pins.setdefault(name, (n, low != (m.group(2) == '!')))
            continue
        m = re.search(r'(?:^|\n)\s*(?:field|fld)\s+([A-Za-z_0-9]+)\s*=\s*\[([^\]]*)\]\s*$', stmt, re.I)
        if m:
            try:
                fields[m.group(1)] = [name for name, low in expand_names(m.group(2))]
            except ValueError:
                pass
    return pins, fields, device


def doc_pins(doc):
    """{name: (pin, low)} from the symbol table of the .doc file cupl
    writes: the pins as the compiler has them, whatever made their names."""
    pins = {}
    for m in re.finditer(r'^ (!| )  (\S+) +(\d+) +V ', doc, re.M):
        if int(m.group(3)) > 0:
            pins[m.group(2)] = (int(m.group(3)), m.group(1) == '!')
    return pins


# ---------------------------------------------------------------------------
# Rewriting a .si file for the default pin names
# ---------------------------------------------------------------------------
TOKEN = re.compile(r'/\*.*?\*/|"[^"\n]*"|\'[^\'\n]*\'|[A-Za-z_][A-Za-z_0-9]*', re.S)


def split_si(text):
    """(header, ORDER statement, rest) of a .si file."""
    m = re.search(r'^[ \t]*order\s*:', text, re.I | re.M)
    if not m:
        raise Unsupported('no ORDER statement')
    end = m.end()
    while True:     # the ';' that ends the statement, not one in a comment or text
        t = re.compile(r'/\*.*?\*/|"[^"\n]*"|;', re.S).search(text, end)
        if not t:
            raise Unsupported('ORDER statement without end')
        end = t.end()
        if t.group(0) == ';':
            break
    return text[:m.start()], text[m.start():end], text[end:]


def order_items(order, pins, fields):
    """The items of an ORDER statement: ('pin', name, negated), ('field',
    name) or ('other', text) for spacing; ranges of names are expanded."""
    body = re.sub(r'/\*.*?\*/', ' ', order[order.index(':') + 1:order.rindex(';')], flags=re.S)
    items = []
    for item in body.split(','):
        item = item.strip()
        if not item:
            continue
        if item.startswith('%'):
            items.append(('other', item))
            continue
        if item.startswith('"'):
            raise Unsupported('text in the ORDER statement')
        m = re.match(r'(!?)\s*([A-Za-z_0-9.]+)$', item)
        if not m:
            raise Unsupported('ORDER item %r' % item)
        try:
            names = [n for n, _ in expand_names(m.group(2))]
        except ValueError:
            raise Unsupported('ORDER item %r' % item)
        for name in names:
            if name in fields and len(names) == 1:
                if m.group(1):
                    raise Unsupported('negated field in the ORDER statement')
                items.append(('field', name))
            elif name in pins:
                items.append(('pin', name, m.group(1) == '!'))
            else:
                raise Unsupported('ORDER name %r is no pin' % name)
    return items


def order_columns(order, pins, fields):
    """The columns of an ORDER statement: [(name, negated)] with fields
    expanded to their members, and the names of the fields used."""
    columns, used = [], []
    for item in order_items(order, pins, fields):
        if item[0] == 'field':
            used.append(item[1])
            columns += [(name, False) for name in fields[item[1]]]
        elif item[0] == 'pin':
            columns.append((item[1], item[2]))
    return columns, used


def si_for_pins(text, pins, fields, levels, define_fields=True):
    """The .si file with P<n> for every pin name.  Returns the new text and,
    with `levels`, the set of ORDER columns whose test values were inverted
    (else names that are active low are written as !P<n>).  The fields the
    ORDER statement uses are defined over the new names, unless they are
    known from elsewhere."""
    header, order, rest = split_si(text)
    columns, used_fields = order_columns(order, pins, fields)
    low_names = {name for name, (pin, low) in pins.items() if low}
    inverted = set()

    def rename(m):
        word = m.group(0)
        if word[0] in '/"\'' or word not in pins:
            return word
        return 'P%d' % pins[word][0]

    # the ORDER statement: name -> P<n>; "!" is added or dropped for a name
    # that is active low, unless its test values are inverted instead
    new_items = []
    for item in order_items(order, pins, fields):
        if item[0] == 'pin':
            negated = item[2] != (pins[item[1]][1] and not levels)
            new_items.append(('!' if negated else '') + 'P%d' % pins[item[1]][0])
        else:
            new_items.append(item[1])
    order = 'ORDER: ' + ', '.join(new_items) + ';'
    if levels:
        for i, (name, negated) in enumerate(columns):
            if pins[name][1] and not negated:
                inverted.add(i)
    # the fields of the design, over the new names
    defs = []
    for name in used_fields:
        if any(m not in pins for m in fields[name]):
            raise Unsupported('field %s has members that are no pins' % name)
        lows = [pins[m][1] for m in fields[name]]
        if any(lows) and not all(lows):
            raise Unsupported('field %s mixes polarities' % name)
        members = ','.join('P%d' % pins[m][0] for m in fields[name])
        defs.append('FIELD %s = %s[%s];\n' % (name, '!' if all(lows) and not levels else '', members))
    # the statements after it: names in "$" lines; test values in vector lines
    out = []
    in_vectors = False
    for line in rest.split('\n'):
        code = re.sub(r'/\*.*?\*/', '', line, flags=re.S).strip()
        if re.match(r'vectors\s*:', code, re.I):
            in_vectors = True
        elif code.startswith('$') or not in_vectors:
            if not re.match(r'\$\s*msg\b', code, re.I):
                words = set(TOKEN.findall(re.sub(r'/\*.*?\*/|"[^"\n]*"', ' ', line)))
                if words & low_names:
                    raise Unsupported('a statement uses a name that is active low')
                if words & set(pins):
                    if not define_fields:
                        raise Unsupported('P<n> for a pin the .pld file names stands only in the ORDER statement')
                    line = TOKEN.sub(rename, line)
        elif code and inverted:
            line = invert_vector(line, columns, fields, used_fields, inverted)
        out.append(line)
    return header + (''.join(defs) if define_fields else '') + order + '\n'.join(out), inverted


def invert_vector(line, columns, fields, used_fields, inverted):
    """A vector line with the values of the inverted columns swapped.  The
    values are single characters, or a quoted number for a whole field."""
    m = re.match(r'([^/]*?)(\s*(/\*.*)?)$', line, re.S)
    values, tail = m.group(1), m.group(2)
    if not re.match(r'^[\s01XLHZCKPN*xlhzckpn"\'A-Fa-f2-9]*$', values):
        raise Unsupported('vector line %r' % line)
    out, col = [], 0
    pos = 0
    while pos < len(values):
        c = values[pos]
        if c.isspace():
            out.append(c)
            pos += 1
        elif c in '"\'':
            end = values.index(c, pos + 1)
            out.append(values[pos:end + 1])
            pos = end + 1
            # a quoted number covers the columns of one field
            width = None
            for name in used_fields:
                first = [n for n, _ in columns].index(fields[name][0])
                if first == col:
                    width = len(fields[name])
            if width is None or any(i in inverted for i in range(col, col + width)):
                raise Unsupported('quoted value in an inverted column')
            col += width
        else:
            out.append(SWAP.get(c, c) if col in inverted else c)
            if col in inverted and c in 'CKPckp':
                raise Unsupported('clock on an active low column')
            col += 1
            pos += 1
    return ''.join(out) + tail


VECTOR_LINE = re.compile(r'^\d{4}: ')


def vector_lines(listing):
    return [line for line in listing.split('\n') if VECTOR_LINE.match(line)]


def invert_listing_lines(lines, inverted, ncolumns):
    """The vector lines of a listing with the inverted columns swapped."""
    out = []
    for line in lines:
        chars = list(line)
        col = 0
        for i in range(6, len(chars)):
            if chars[i] != ' ':
                if col in inverted:
                    chars[i] = SWAP.get(chars[i], chars[i])
                col += 1
        if col != ncolumns:
            raise Unsupported('listing line %r has %d columns, not %d' % (line, col, ncolumns))
        out.append(''.join(chars))
    return out


def mask_listing(text):
    return re.sub(r'^CREATED .*$', 'CREATED', text, flags=re.M)


def trace_lines(listing):
    """The lines of a $TRACE listing, without the reset and preset terms
    that a design writes as always false (q.ar = 'b'0): in the fuse map
    they are terms that are not there, and csim -f does not list them."""
    lines = mask_listing(listing).split('\n')
    out = []
    i = 0
    while i < len(lines):
        if re.match(r'----> (\S+\.(ar|sp) )+$', lines[i]) and i + 1 < len(lines) and \
                re.match(r'\(0\)[ -]*$', lines[i + 1]) and not (i + 2 < len(lines) and lines[i + 2].startswith('(')):
            i += 2
            continue
        out.append(lines[i])
        i += 1
    return out


def jedec_vectors(text):
    """The fuse data and the test vectors of a JEDEC file, without the
    transmission checksum."""
    return re.sub(r'\x03[0-9A-Fa-f]{4}', '\x03', text)


# ---------------------------------------------------------------------------
# Random test vectors for the designs that name their inputs and outputs
# ---------------------------------------------------------------------------
def random_vectors(spec, count, seed):
    """Vector lines for "$RANDOM <clock|-> <inputs> : <outputs>": random
    values for the inputs, '*' for the outputs."""
    rnd = random.Random(seed)
    clock, ninputs, noutputs = spec
    lines = []
    for _ in range(count):
        values = ''.join(rnd.choice('01') for _ in range(ninputs))
        lines.append((clock + ' ' if clock else '') + values + ' ' + '*' * noutputs)
    return lines


def expand_random(si_text, seed):
    """A line "$RANDOM <n> <clock value or -> <inputs> <outputs>;" stands
    for n random vectors (the test designs use it; csim never sees it)."""
    def repl(m):
        clock = '' if m.group(2) == '-' else m.group(2)
        return '\n'.join(random_vectors((clock, int(m.group(3)), int(m.group(4))), int(m.group(1)), seed))
    return re.sub(r'^\$RANDOM\s+(\d+)\s+(\S+)\s+(\d+)\s+(\d+)\s*;[ \t]*$', repl, si_text, flags=re.M)


# ---------------------------------------------------------------------------
# The cases
# ---------------------------------------------------------------------------
class Suite:
    def __init__(self, tools, work, keep):
        self.tools = tools
        self.work = work
        self.keep = keep
        self.passed = 0
        self.failed = []
        self.skipped = []
        self.counts = {}
        self.status = {}        # exit status of csim on the compiled design

    def ok(self, name):
        self.passed += 1
        kind = name.split(':')[0]
        self.counts[kind] = self.counts.get(kind, 0) + 1
        print('  [PASS] %s' % name)

    def fail(self, name, why):
        self.failed.append(name)
        print('  [FAIL] %s: %s' % (name, why))

    def skip(self, name, why):
        self.skipped.append((name, why))
        print('  [SKIP] %s: %s' % (name, why))

    def dir(self, *parts):
        d = os.path.join(self.work, *parts)
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
        return d

    def compare(self, name, got, want, what):
        if got == want:
            return True
        for i, (g, w) in enumerate(zip(got, want)):
            if g != w:
                self.fail(name, '%s differ at entry %d:\n      fuse map: %r\n      compiled: %r' % (what, i + 1, g, w))
                return False
        self.fail(name, '%s: %d entries from the fuse map, %d from the compiled design' % (what, len(got), len(want)))
        return False

    # -- one design ---------------------------------------------------------
    def design(self, pld, si):
        base = os.path.splitext(os.path.basename(pld))[0].lower()
        label = os.path.relpath(pld, ROOT)
        pins, fields, device = pld_pins(pld)
        print('%s (%s)' % (label, device))
        # compile; simulate the compiled design
        ref = self.dir(base, 'ref')
        for f in os.listdir(os.path.dirname(pld)):
            src = os.path.join(os.path.dirname(pld), f)
            if os.path.isfile(src) and not f.lower().endswith(('.pld', '.si')):
                shutil.copy(src, ref)       # include files
        shutil.copy(pld, os.path.join(ref, base + '.pld'))
        si_text = expand_random(read_text(si), base)
        open(os.path.join(ref, base + '.si'), 'w').write(si_text)
        rc, out = self.tools.run('cupl', ['-jaxm4', base + '.pld'], ref)
        jeds = [f for f in os.listdir(ref) if f.lower().endswith('.jed')]
        if rc != 0 or len(jeds) != 1 or not find_ci(ref, base + '.abs'):
            self.fail(label, 'cupl -jaxm4 failed:\n' + out[-600:])
            return
        jed = os.path.join(ref, jeds[0])
        fuses_only = read_text(jed)
        doc = find_ci(ref, base + '.doc')
        if doc:
            pins.update(doc_pins(read_text(doc)))
        rc, out = self.tools.run('csim', ['-lw', base], ref)
        ref_so = read_text(find_ci(ref, base + '.so'))
        ref_wo = read_text(find_ci(ref, base + '.wo'))
        ref_lines = vector_lines(ref_so)
        if not ref_lines:
            self.fail(label, 'csim -lw gave no vector lines:\n' + out[-600:])
            return
        ref_rc = self.status[base] = rc
        # csim -j on the compiled design: the test vectors in the JEDEC file
        self.tools.run('csim', ['-j', base], ref)
        ref_jed = jedec_vectors(read_text(jed))
        if 'V0001' not in ref_jed:
            ref_jed = None

        def jed_dir(mode, si_new, with_pld):
            d = self.dir(base, mode)
            open(os.path.join(d, base + '.jed'), 'w').write(fuses_only)
            open(os.path.join(d, base + '.si'), 'w').write(si_new)
            if with_pld:
                shutil.copy(pld, os.path.join(d, base + '.pld'))
                for f in os.listdir(os.path.dirname(pld)):
                    src = os.path.join(os.path.dirname(pld), f)
                    if os.path.isfile(src) and not f.lower().endswith(('.pld', '.si')):
                        shutil.copy(src, d)
            return d

        def run_f(d, flags, dev=None):
            rc, out = self.tools.run('csim', [flags] + ([dev] if dev else []) + [base], d)
            so = find_ci(d, base + '.so')
            return rc, out, read_text(so) if so else ''

        # names: the .pld file gives the names
        name = 'names: %s' % label
        if True:
            d = jed_dir('names', si_text, True)
            rc, out, so = run_f(d, '-lwf')
            wo = find_ci(d, base + '.wo')
            if rc != ref_rc:
                self.fail(name, 'exit status %d, compiled design %d:\n%s' % (rc, ref_rc, out[-800:]))
            elif self.compare(name, mask_listing(so).split('\n'), mask_listing(ref_so).split('\n'), 'listings') and \
                    self.compare(name, read_text(wo).split('\n') if wo else [], ref_wo.split('\n'), 'waveform files'):
                if ref_jed is not None:
                    self.tools.run('csim', ['-jf', base], d)
                    if not self.compare(name, jedec_vectors(read_text(os.path.join(d, base + '.jed'))).split('\n'),
                                        ref_jed.split('\n'), 'JEDEC files after csim -j'):
                        return
                    # the file now has test vectors: it is read as before
                    rc, out, so = run_f(d, '-lf')
                    if not self.compare(name, vector_lines(so), ref_lines, 'vector lines from the file with vectors'):
                        return
                self.ok(name)

        # trace: every equation as it is evaluated
        name = 'trace: %s' % label
        m = re.search(r'^[ \t]*vectors[ \t]*:[ \t]*\n', si_text, re.I | re.M)
        if m:
            si_trace = si_text[:m.end()] + '$TRACE 3;\n' + si_text[m.end():]
            d = self.dir(base, 'trace_ref')
            for f in os.listdir(ref):
                if not f.lower().endswith(('.so', '.wo', '.si')):
                    shutil.copy(os.path.join(ref, f), d)
            open(os.path.join(d, base + '.si'), 'w').write(si_trace)
            rc_ref, out = self.tools.run('csim', ['-l', base], d)
            want = trace_lines(read_text(find_ci(d, base + '.so')))
            rc, out, so = run_f(jed_dir('trace', si_trace, True), '-lf')
            if rc != rc_ref:
                self.fail(name, 'exit status %d, compiled design %d:\n%s' % (rc, rc_ref, out[-800:]))
            elif self.compare(name, trace_lines(so), want, 'listings'):
                self.ok(name)

        # pins / device / levels: default names; alias: default names for
        # pins that have a name from the .pld file
        for mode in ('pins', 'device', 'levels', 'alias'):
            name = '%s: %s' % (mode, label)
            try:
                si_new, inverted = si_for_pins(si_text, pins, fields, mode == 'levels', mode != 'alias')
                ncolumns = len(order_columns(split_si(si_text)[1], pins, fields)[0])
                want = invert_listing_lines(ref_lines, inverted, ncolumns)
            except Unsupported as e:
                self.skip(name, str(e))
                continue
            if mode == 'levels' and not inverted:
                continue        # nothing is active low: the same as "pins"
            if mode == 'alias' and any(re.match(r'P\d+$', n) for n in pins):
                continue        # the design has names of this form itself
            d = jed_dir(mode, si_new, mode == 'alias')
            rc, out, so = run_f(d, '-lf', device if mode == 'device' else None)
            if rc != ref_rc:
                self.fail(name, 'exit status %d, compiled design %d:\n%s' % (rc, ref_rc, out[-800:]))
                continue
            if not self.compare(name, vector_lines(so), want, 'vector lines'):
                continue
            if mode == 'levels' and ref_jed is not None:
                self.tools.run('csim', ['-jf', base], d)
                got = [l for l in read_text(os.path.join(d, base + '.jed')).split('\n') if l.startswith('V')]
                if not self.compare(name, got, [l for l in ref_jed.split('\n') if l.startswith('V')],
                                    'test vectors written by csim -j'):
                    continue
            self.ok(name)


# ---------------------------------------------------------------------------
# JEDEC files as other programs write them, and damaged ones
# ---------------------------------------------------------------------------
def parse_jed(text):
    """(number of fuses, list of fuse states) of a JEDEC file cupl wrote."""
    qf = int(re.search(r'\*QF(\d+)', text).group(1))
    default = int(re.search(r'\*F([01])', text).group(1))
    fuses = [default] * qf
    for m in re.finditer(r'\*L(\d+)\s+([01\s]+)', text):
        for i, c in enumerate(re.sub(r'\s', '', m.group(2))):
            fuses[int(m.group(1)) + i] = int(c)
    return qf, fuses


def checksum(fuses):
    total = 0
    for i in range(0, len(fuses), 8):
        total += sum(b << j for j, b in enumerate(fuses[i:i + 8]))
    return total & 0xffff


def rows(fuses, width, digits=5, skip_blank=False):
    out = []
    for i in range(0, len(fuses), width):
        row = fuses[i:i + width]
        if skip_blank and not any(row):
            continue
        out.append('L%0*d %s' % (digits, i, ''.join(map(str, row))))
    return out


def variants(text):
    """The fuse map of a JEDEC file in the forms of other programs:
    [(name, file contents as bytes, device or None)]."""
    qf, fuses = parse_jed(text)
    c = checksum(fuses)
    width = 44 if qf > 5000 else (40 if qf == 2706 else 32)
    out = []
    # a device programmer: every fuse listed, no F field, upper case, CR LF
    body = '\x02\r\nfuse map of a device programmer*\r\nQF%d*\r\n' % qf
    body += ''.join(l + '*\r\n' for l in rows(fuses, width)) + 'C%04X*\r\n\x030000\r\n' % c
    out.append(('programmer dump (no F field, CR LF)', body.encode(), None))
    # GALer: no STX / ETX, fields start with '*', lower case checksum
    body = 'Used Program:   GALer V2.0\nDevice:         GAL\n\n*F0\n*G0\n*QF%d\n' % qf
    body += ''.join('*' + l + '\n' for l in rows(fuses, width, 4, True)) + '*C%04x\n*\n' % c
    out.append(('no STX and ETX, fields start with *', body.encode(), None))
    # another programmer: '*' in the heading, QF0000, no ETX, a 3-digit checksum would lose its zero
    body = '\x02MODEL: programmer\nTYPE:GAL *20PL\nDM: X\nQF0000*\n'
    body += ''.join(l + '*\n' for l in rows(fuses, width, 6)) + 'C%X*\n' % c
    out.append(('QF0000, no ETX, * in the heading', body.encode(), None))
    # lower case fields, notes, security fuse, test vectors, no checksum
    body = '\x02\nheading\n*qp24*qf%d*g1*f0*\nn a note*\n' % qf
    body += ''.join(l.lower() + '\n*' for l in rows(fuses, width, 5, True))
    body += '\nN another note *\nP 1 2 3*\nV0001 000*\n\x031234\n'
    out.append(('lower case, notes, G P V fields, no C field', body.encode(), None))
    # text in front of the STX, Ctrl-Z behind the checksum, fuses split over lines
    body = 'PALASM\n TITLE x * y\n\x02\nPAL\n_X*\nQP20*\nQF%d*\nG0*F0*\n' % qf
    for l in rows(fuses, width, 4, True):
        addr, bits = l.split(' ')
        body += addr + ' ' + bits[:width // 2] + '\n ' + bits[width // 2:] + '*\n'
    body += 'C%04X*\n\x03ABCD\n\x1a' % c
    out.append(('text before STX, fuses over two lines, Ctrl-Z', body.encode(), None))
    if qf == 5892:
        # a 22V10 without the signature: 5828 fuses
        short = fuses[:5828]
        body = '\x02\nGAL22V10 (no UES)*\nQF5828*\nF0*\n' + ''.join(l + '*\n' for l in rows(short, 44, 5, True))
        body += 'C%04X*\n\x030000\n' % checksum(short)
        out.append(('22V10 map of 5828 fuses', body.encode(), None))
        out.append(('22V10 map of 5828 fuses as p22v10', body.encode(), 'p22v10'))
    return out


def damaged(text):
    """Damaged copies of a JEDEC file: [(name, contents, text the message
    must have)]."""
    qf, fuses = parse_jed(text)
    c = checksum(fuses)
    out = []
    cut = text[:text.index('*L') + len(text[text.index('*L'):]) // 2]
    out.append(('cut short in the fuse list', cut, 'cut short'))
    out.append(('cut short behind the fuses', text[:text.rindex('*C')], 'cut short'))
    out.append(('wrong fuse checksum', text.replace('*C%04X' % c, '*C%04X' % ((c + 1) & 0xffff)), 'fuse checksum'))
    one = re.sub(r'(\*L\d+ )([01])', lambda m: m.group(1) + ('1' if m.group(2) == '0' else '0'), text, count=1)
    out.append(('one fuse changed', one, 'fuse checksum'))
    out.append(('fuse without a state', re.sub(r'\*F[01]\s*', '', text), 'has no state'))
    out.append(('fuse number beyond QF', text.replace('*C', '*L%05d 1\n*C' % qf), 'QF says'))
    out.append(('letter in the fuse list', re.sub(r'(\*L\d+ [01])[01]', r'\1x', text, count=1), 'invalid character'))
    out.append(('empty file', '', 'cut short'))
    out.append(('not a JEDEC file', 'Name x;\nDevice g16v8;\n', 'cut short'))
    return out


def format_cases(suite, base_dir, base):
    """Run the variants and damaged copies of the fuse map of design `base`
    (already compiled and simulated in base_dir/pins)."""
    src = os.path.join(base_dir, 'pins')
    text = read_text(os.path.join(src, base + '.jed'))
    si = read_text(os.path.join(src, base + '.si'))
    want = vector_lines(read_text(os.path.join(src, base + '.so')))
    for name, data, device in variants(text):
        name = 'format: %s, %s' % (base, name)
        d = suite.dir(base, 'format')
        open(os.path.join(d, base + '.jed'), 'wb').write(data)
        open(os.path.join(d, base + '.si'), 'w').write(si)
        rc, out = suite.tools.run('csim', ['-lf'] + ([device] if device else []) + [base], d)
        so = find_ci(d, base + '.so')
        if rc != suite.status[base]:
            suite.fail(name, 'exit status %d, compiled design %d:\n%s' % (rc, suite.status[base], out[-600:]))
        elif suite.compare(name, vector_lines(read_text(so)), want, 'vector lines'):
            suite.ok(name)
    for name, data, message in damaged(text):
        name = 'refused: %s, %s' % (base, name)
        d = suite.dir(base, 'damaged')
        open(os.path.join(d, base + '.jed'), 'w').write(data)
        open(os.path.join(d, base + '.si'), 'w').write(si)
        rc, out = suite.tools.run('csim', ['-lf', base], d)
        if rc == 0 or 'invalid JEDEC file' not in out or message not in out or re.search(r'^\d{4}: ', out, re.M):
            suite.fail(name, 'expected "invalid JEDEC file: ... %s", exit status %d:\n%s' % (message, rc, out[-500:]))
        else:
            suite.ok(name)


def write_jed(fuses, default=0):
    """A JEDEC file of the fuse map, as cupl writes it."""
    width = 44 if len(fuses) > 5000 else (40 if len(fuses) == 2706 else 32)
    body = '\x02\nfuse map\n*QP24 \n*QF%d \n*G0 \n*F%d \n' % (len(fuses), default)
    for i in range(0, len(fuses), width):
        row = fuses[i:i + width]
        if any(f != default for f in row):
            body += '*L%05d %s\n' % (i, ''.join(map(str, row)))
    return body + '*C%04X\n*\x030000' % checksum(fuses)


def mode_cases(suite, work):
    """The device and its mode named on the command line."""
    def run(base, device, same, messages=()):
        name = 'device: %s as %s' % (base, device)
        src = os.path.join(work, base, 'pins')
        d = suite.dir(base, 'mode')
        for ext in ('.jed', '.si'):
            shutil.copy(os.path.join(src, base + ext), d)
        rc, out = suite.tools.run('csim', ['-lf', device, base], d)
        lines = vector_lines(read_text(find_ci(d, base + '.so') or os.devnull))
        want = vector_lines(read_text(os.path.join(src, base + '.so')))
        if not all(m in out for m in messages):
            suite.fail(name, 'expected %r:\n%s' % (messages, out[:600]))
        elif same and (rc != suite.status[base] or lines != want):
            suite.fail(name, 'not the result of the fuse map without a device (exit status %d)' % rc)
        elif not same and (lines == want or len(lines) != len(want)):
            suite.fail(name, 'expected %d other vector lines, got %d' % (len(want), len(lines)))
        else:
            suite.ok(name)
    run('t16r', 'g16v8ms', True, ['simulated as device g16v8ms'])
    run('t16r', 'g16v8', True, ['simulated as device g16v8ms'])
    run('t16r', 'g16v8a', True, ['simulated as device g16v8ms'])
    run('t16c', 'g16v8ma', True, ['simulated as device g16v8ma'])
    run('t16s', 'g16v8s', True, ['simulated as device g16v8s'])
    run('t16s', 'g16v8a', True, ['simulated as device g16v8as'])
    run('t16s', 'g16v8as', True, ['simulated as device g16v8as'])
    run('t20r', 'g20v8ms', True, ['simulated as device g20v8ms'])
    run('t20c', 'g20v8a', True, ['simulated as device g20v8ma'])
    run('t20s', 'g20v8', True, ['simulated as device g20v8s'])
    run('t22a', 'g22v10', True, ['simulated as device g22v10'])
    # a mode the fuses do not select: a note, and another result
    run('t16c', 'g16v8ms', False, ['[0038sa] Please note: mode fuses of the JEDEC file select another mode than device:  g16v8ms'])
    run('t16s', 'g16v8ma', False, ['[0038sa] Please note: mode fuses'])


def package_cases(suite, work):
    """The device of a fuse map when csim is not told: the one the file
    names (cupl writes it into the design specification), else by the
    number of fuses and, for the PLCC package, of pins (QP28)."""
    def run(what, base, change, device, same):
        name = 'package: %s, %s' % (base, what)
        src = os.path.join(work, base, 'pins')
        d = suite.dir(base, 'package')
        jed = change(read_text(os.path.join(src, base + '.jed')))
        open(os.path.join(d, base + '.jed'), 'w').write(jed)
        shutil.copy(os.path.join(src, base + '.si'), d)
        rc, out = suite.tools.run('csim', ['-lf', base], d)
        lines = vector_lines(read_text(find_ci(d, base + '.so') or os.devnull))
        want = vector_lines(read_text(os.path.join(src, base + '.so')))
        m = re.search(r'simulated as device (\S+)', out)
        if not m or not re.match(device + '$', m.group(1)):
            suite.fail(name, 'expected device %s:\n%s' % (device, out[:600]))
        elif same and (rc != suite.status[base] or lines != want):
            suite.fail(name, 'not the result of the file as cupl wrote it (exit status %d):\n%s' % (rc, out[-400:]))
        elif not same and lines == want:
            suite.fail(name, 'the pin numbers of the PLCC package were understood')
        else:
            suite.ok(name)

    def spec(text):
        """The file with this design specification."""
        return lambda jed: re.sub(r'^\x02.*?(?=\*QP)', lambda _: '\x02\n' + text, jed, flags=re.S)

    def no_qp(change):
        return lambda jed: change(jed).replace('*QP28 \n', '')

    def qp24(change):
        return lambda jed: change(jed).replace('*QP28 ', '*QP24 ')

    if os.path.isdir(os.path.join(work, 't22l', 'pins')):
        run('no device named, QP28', 't22l', spec('a fuse map\n'), 'g22v10lcc', True)
        run('a device the library does not have, QP28', 't22l', spec('Device   nosuchdev\n'), 'g22v10lcc', True)
        run('g22v10lcc named, QP24', 't22l', qp24(lambda jed: jed), 'g22v10lcc', True)
        run('DEVICE: G22V10LCC named, no QP', 't22l', no_qp(spec('DEVICE: G22V10LCC\n')), 'g22v10lcc', True)
        run('no device named, no QP', 't22l', no_qp(spec('a fuse map\n')), 'g22v10', False)
        run('no device named, QP24', 't22l', qp24(spec('a fuse map\n')), 'g22v10', False)
    if os.path.isdir(os.path.join(work, 't20l', 'pins')):
        run('no device named, QP28', 't20l', spec('a fuse map\n'), r'g20v8\w*lcc', True)
        run('no device named, no QP', 't20l', no_qp(spec('a fuse map\n')), r'g20v8(ms|ma|s|as)', False)
    if os.path.isdir(os.path.join(work, 't16r', 'pins')):
        # a mode entry the fuses do not select is not taken from the file
        run('g16v8s named, registered mode', 't16r', spec('Device   g16v8s\n'), 'g16v8ms', True)


DEAD_HEADER = '''Name     dead16;
PartNo   00;
Date     2026-01-01;
Revision 01;
Designer LinCUPL;
Company  LinCUPL;
Assembly None;
Location None;
Device   g16v8s;
'''

# y is read back.  In the simple mode cupl makes its pin an input (AC1 = 1)
# and still gives it the product term.
DEAD_PLD = DEAD_HEADER + '''
Pin 2  = a;
Pin 3  = b;
Pin 4  = c;
Pin 19 = y;
Pin 18 = z;
Pin 17 = w;

y = a & b;
z = y & c;
w = a # c;
'''

DEAD_SI_COMPILED = DEAD_HEADER + '''
ORDER: a, b, c, %2, y, z, w;

VECTORS:
1 1 1  H H H
1 0 1  L L H
0 0 0  L L L
'''

DEAD_SI_MAP = DEAD_HEADER + '''
ORDER: %s, %%2, %s;

VECTORS:
1 1 1 0  L H
1 1 1 1  H H
0 0 1 1  H H
0 0 0 1  L L
'''


def dead_terms_case(suite):
    """Product terms of a macrocell that the fuse map makes an input: the
    compiled design simulates as it is written, the fuse map as the device
    behaves (the pin is an input), and csim says so."""
    name = 'unused: product terms of an input macrocell'
    note = '[0039sa] Please note: product terms without effect, the fuse map makes an input of pin:  19'
    ref = suite.dir('dead16', 'ref')
    open(os.path.join(ref, 'dead16.pld'), 'w').write(DEAD_PLD)
    open(os.path.join(ref, 'dead16.si'), 'w').write(DEAD_SI_COMPILED)
    rc, out = suite.tools.run('cupl', ['-jaxm4', 'dead16.pld'], ref)
    jed = find_ci(ref, 'dead16.jed')
    if rc != 0 or not jed:
        suite.fail(name, 'cupl -jaxm4 failed:\n' + out[-600:])
        return
    rc, out = suite.tools.run('csim', ['-l', 'dead16'], ref)
    if rc != 0:
        suite.fail(name, 'the compiled design does not simulate as written:\n' + out[-600:])
        return
    for with_pld, order, end in ((True, ('a, b, c, y', 'z, w'), ' (y)\n'), (False, ('P2, P3, P4, P19', 'P18, P17'), '\n')):
        d = suite.dir('dead16', 'names' if with_pld else 'pins')
        shutil.copy(jed, os.path.join(d, 'dead16.jed'))
        if with_pld:
            shutil.copy(os.path.join(ref, 'dead16.pld'), d)
        open(os.path.join(d, 'dead16.si'), 'w').write(DEAD_SI_MAP % order)
        rc, out = suite.tools.run('csim', ['-lf', 'dead16'], d)
        listing = read_text(find_ci(d, 'dead16.so') or os.devnull)
        inputs = [(pin, pin_name) for pin, pin_name, low in summary_pins(out, 'inputs:')]
        if rc != 0 or note + end not in out or note + end not in listing or \
                (19, 'y' if with_pld else None) not in inputs or len(vector_lines(listing)) != 4:
            suite.fail(name, 'exit status %d, inputs %s:\n%s' % (rc, inputs, out[-800:]))
            return
    suite.ok(name)


def ptd_case(suite, work, base, ptd_fuse, width):
    """Product term disable fuses of the GAL16V8 / GAL20V8: a row whose
    fuse is intact (0) counts as little as a row whose inputs are all
    connected, and both change the result."""
    name = 'ptd: %s' % base
    src = os.path.join(work, base, 'pins')
    qf, fuses = parse_jed(read_text(os.path.join(src, base + '.jed')))
    si = read_text(os.path.join(src, base + '.si'))
    want = vector_lines(read_text(os.path.join(src, base + '.so')))

    def run(mode, changed):
        d = suite.dir(base, mode)
        open(os.path.join(d, base + '.jed'), 'w').write(write_jed(changed))
        open(os.path.join(d, base + '.si'), 'w').write(si)
        rc, out = suite.tools.run('csim', ['-lf', base], d)
        so = find_ci(d, base + '.so')
        return vector_lines(read_text(so)) if so else ['no listing: ' + out[-300:]]
    tried = 0
    for row in range(len(fuses[:ptd_fuse - 80]) // width):
        first = row * width
        if first + width > 64 * width or not any(fuses[first:first + width]) or all(fuses[first:first + width]):
            continue
        blank = fuses[:first] + [0] * width + fuses[first + width:]
        off = list(fuses)
        off[ptd_fuse + row] = 0
        a, b = run('ptd_blank', blank), run('ptd_off', off)
        if a != b:
            suite.fail(name, 'row %d: disabled by its PTD fuse and with all inputs connected give other results' % row)
            return
        tried += 1
        if a != want:
            suite.ok(name + ' (row %d)' % row)
            return
    suite.fail(name, 'no product term of %d changed the result' % tried)


def path_case(suite, work, base, mode='pins'):
    """The design named with a directory; in mode "names" with its .pld
    file and what that includes, which are looked for there too."""
    name = 'path: %s%s' % (base, '' if mode == 'pins' else ' with its .pld file')
    src = os.path.join(work, base, mode)
    d = suite.dir(base, 'path', 'sub dir')
    for f in os.listdir(src):
        if not f.lower().endswith(('.so', '.wo')):
            shutil.copy(os.path.join(src, f), d)
    rc, out = suite.tools.run('csim', ['-lf', os.path.join('sub dir', base)], os.path.dirname(d))
    so = find_ci(d, base + '.so')
    if so and vector_lines(read_text(so)) == vector_lines(read_text(os.path.join(src, base + '.so'))):
        suite.ok(name)
    else:
        suite.fail(name, 'exit status %d:\n%s' % (rc, out[-400:]))


def refusal_cases(suite, base_dir, base):
    """Fuse counts no supported device has, devices that are not supported,
    a missing file."""
    src = os.path.join(base_dir, 'pins')
    si = read_text(os.path.join(src, base + '.si'))
    good = read_text(os.path.join(src, base + '.jed'))

    def run(name, jed, args, messages):
        d = suite.dir(base, 'refusal')
        if jed is not None:
            open(os.path.join(d, base + '.jed'), 'w').write(jed)
        open(os.path.join(d, base + '.si'), 'w').write(si)
        rc, out = suite.tools.run('csim', args + [base], d)
        if rc == 0 or not all(m in out for m in messages) or re.search(r'^\d{4}: ', out, re.M):
            suite.fail(name, 'expected %r, exit status %d:\n%s' % (messages, rc, out[-500:]))
        else:
            suite.ok(name)

    for qf in (2048, 2195, 2560, 5893, 12832):
        fuses = [0] * qf
        jed = '\x02\nx*\nQF%d*\nF0*\nL0 1*\nC0001*\n\x030000\n' % qf
        run('refused: map of %d fuses' % qf, jed, ['-lf'], ['no device for JEDEC file', '%d fuses' % qf])
    run('refused: no .jed file', None, ['-lf'], ['could not open', base + '.jed'])
    qf = parse_jed(good)[0]
    other = 'g22v10' if qf != 5892 else 'g16v8'
    run('refused: device with another number of fuses', good, ['-lf', other], ['invalid JEDEC file', 'fuses, device'])
    run('refused: device that is not supported', good, ['-lf', 'v5000'],
        ['device cannot be simulated from a JEDEC file', 'v5000'])
    run('refused: device not in the library', good, ['-lf', 'nosuchdev'], ['device not in library', 'nosuchdev'])


# ---------------------------------------------------------------------------
# A collection of JEDEC files without test vectors
# ---------------------------------------------------------------------------
def summary_pins(out, title):
    """[(pin, name or None, low)] of a line of the summary csim -f prints."""
    t = re.search(r'^  %s\s+(.*)$' % title, out, re.M)
    pins = []
    for item in (t.group(1).split() if t else []):
        m = re.match(r'(\d+)(?:=(!?)(\S+))?$', item)
        pins.append((int(m.group(1)), m.group(3), m.group(2) == '!'))
    return pins


def corpus_vectors(seed, device, ins, outs, named_only, count):
    """An ORDER statement and vectors for a fuse map: random values for the
    inputs, a clock on pin 1 if there are registers, '*' for the outputs.
    The output enable pin of the registered modes is mostly active."""
    registered = bool(outs[1])
    enable = {20: 11, 24: 13}.get(max([p for p, _, _ in ins + outs[0] + outs[1]] + [0]) // 4 * 4 + 4) \
        if device.endswith('ms') else None
    rnd = random.Random(seed)
    columns = [(p, n, low) for p, n, low in ins + outs[0] + outs[1] if n or not named_only]
    order = ', '.join(n or 'P%d' % p for p, n, low in columns)
    nin = len([c for c in columns if c in ins])
    lines = []
    for i in range(count):
        values = []
        for p, n, low in columns[:nin]:
            if registered and p == 1:
                values.append('C')
            elif p == enable:
                values.append(str(int((rnd.random() < 0.9) == low)))     # the pin is low
            elif i < 2:
                values.append(str(i))
            else:
                values.append(rnd.choice('01'))
        lines.append(''.join(values) + ' ' + '*' * (len(columns) - nin))
    return 'ORDER: %s;\nVECTORS:\n%s\n' % (order, '\n'.join(lines)), len(lines)


def messages(out):
    return sorted(set(re.findall(r'^\[(\d{4})sa\]', out, re.M)))


def corpus(tools, root, work):
    """Load every .jed file under root and simulate a few vectors; then,
    for every line "<source.pld> <device> <dump.jed>" of a verify.lst file
    under root that names a supported device, compile the source and
    compare its simulation with that of the dump."""
    files = []
    lists = []
    for dp, _, fn in os.walk(root):
        files += [os.path.join(dp, f) for f in fn if f.lower().endswith('.jed')]
        lists += [os.path.join(dp, f) for f in fn if f == 'verify.lst']
    files.sort()
    counts = {'files': 0, 'loaded': 0, 'all vectors simulated': 0, 'stopped: too many errors': 0,
              'internal error': 0, 'refused': 0}
    reasons, devices, notes = {}, {}, {}
    for n, path in enumerate(files):
        d = os.path.join(work, 'corpus', '%03d' % n)
        os.makedirs(d)
        shutil.copy(path, os.path.join(d, 'x.jed'))
        open(os.path.join(d, 'x.si'), 'w').write('ORDER: P1;\nVECTORS:\nX\n')
        rc, out = tools.run('csim', ['-f', 'x'], d)
        m = re.search(r'^fuse map .*: (\d+) fuses, simulated as device (\S+)$', out, re.M)
        rel = os.path.relpath(path, root)
        counts['files'] += 1
        if not m:
            counts['refused'] += 1
            why = re.search(r'^\[\d+sa\] (.*)$', out, re.M)
            why = why.group(1) if why else out[-200:]
            why = re.sub(r'\S*x\.jed:?\s*', '', why)
            why = re.sub(r'has \d+ fuses', 'has N fuses', why)
            reasons[why] = reasons.get(why, 0) + 1
            continue
        counts['loaded'] += 1
        devices[m.group(2)] = devices.get(m.group(2), 0) + 1
        ins = summary_pins(out, 'inputs:')
        outs = (summary_pins(out, 'combinatorial outputs:'), summary_pins(out, 'registered outputs:'))
        si, nvec = corpus_vectors(rel, m.group(2), ins, outs, False, 16)
        open(os.path.join(d, 'x.si'), 'w').write(si)
        rc, out = tools.run('csim', ['-lf', 'x'], d)
        so = find_ci(d, 'x.so')
        nlines = len(vector_lines(read_text(so))) if so else 0
        codes = messages(out)
        if rc < 0 or rc > 1 or any(int(c) >= 1000 for c in codes) or 'program error' in out:
            counts['internal error'] += 1
            print('  [FAIL] %s: status %d, %d of %d vectors, messages %s' % (rel, rc, nlines, nvec, codes))
        elif nlines != nvec:
            counts['stopped: too many errors'] += 1
            print('  %s: %d of %d vectors, messages %s' % (rel, nlines, nvec, codes))
        else:
            counts['all vectors simulated'] += 1
        for code in codes:
            notes[code] = notes.get(code, 0) + 1
    print('JEDEC files under %s:' % root)
    for k, v in counts.items():
        print('  %-26s %d' % (k, v))
    for k, v in sorted(devices.items()):
        print('  simulated as %-13s %d' % (k, v))
    for k, v in sorted(notes.items()):
        print('  files with message [%ssa]  %d' % (k, v))
    for k, v in sorted(reasons.items(), key=lambda kv: -kv[1]):
        print('  refused (%d): %s' % (v, k))
    ok = counts['internal error'] == 0

    # sources that are said to reproduce a dump
    result = {}
    jedcmp = os.path.join(root, 'tools', 'jedcmp.py')
    n = 0
    for lst in sorted(lists):
        for line in read_text(lst).split('\n'):
            parts = line.split()
            if len(parts) < 3 or parts[0].startswith('#'):
                continue
            src, device, dump = (os.path.join(os.path.dirname(lst), parts[0]), parts[1].lower(),
                                 os.path.join(os.path.dirname(lst), parts[2]))
            if not SUPPORTED.match(device) or UNSUPPORTED.match(device):
                continue
            n += 1
            label = '%s (%s) and %s' % (os.path.relpath(src, root), device, parts[2])
            ref = os.path.join(work, 'sources', '%03d' % n, 'ref')
            dj = os.path.join(work, 'sources', '%03d' % n, 'jed')
            os.makedirs(ref)
            os.makedirs(dj)
            # the device is given on the command line, as tools/verify.sh does
            text = '\n'.join(l for l in read_text(src).split('\n') if not re.match(r'\s*device\s+[^;]*;', l, re.I))
            for d in (ref, dj):
                open(os.path.join(d, 'x.pld'), 'w').write(text)
            rc, out = tools.run('cupl', ['-jaxm4', device, 'x.pld'], ref)
            jeds = [f for f in os.listdir(ref) if f.lower().endswith('.jed')]
            if rc != 0 or len(jeds) != 1:
                result.setdefault('source does not compile', []).append(label)
                continue
            same = None
            if os.path.isfile(jedcmp):
                same = subprocess.run([sys.executable, '-I', jedcmp, os.path.join(ref, jeds[0]), dump],
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0
            kind = {True: 'same fuses', False: 'other fuses', None: 'fuses not compared'}[same]
            shutil.copy(dump, os.path.join(dj, 'x.jed'))
            open(os.path.join(dj, 'x.si'), 'w').write('ORDER: P1;\nVECTORS:\nX\n')
            rc, out = tools.run('csim', ['-f', device, 'x'], dj)
            if 'simulated as device' not in out:
                result.setdefault('dump refused', []).append(label + ': ' + ' '.join(re.findall(r'^\[.*$', out, re.M)))
                continue
            ins = summary_pins(out, 'inputs:')
            outs = (summary_pins(out, 'combinatorial outputs:'), summary_pins(out, 'registered outputs:'))
            mode = re.search(r'simulated as device (\S+)', out).group(1)
            si, nvec = corpus_vectors(label, mode, ins, outs, True, 400)
            lines = []
            for d, args in ((ref, ['-l', 'x']), (dj, ['-lf', device, 'x'])):
                open(os.path.join(d, 'x.si'), 'w').write(si)
                rc, out = tools.run('csim', args, d)
                so = find_ci(d, 'x.so')
                lines.append((vector_lines(read_text(so)) if so else [], messages(out), out))
            if lines[0][0] and lines[0][0] == lines[1][0]:
                result.setdefault('same result, %s' % kind, []).append(label)
            else:
                first = next((i for i, (a, b) in enumerate(zip(lines[0][0], lines[1][0])) if a != b),
                             min(len(lines[0][0]), len(lines[1][0])))
                diff = sum(1 for a, b in zip(lines[0][0], lines[1][0]) if a != b)
                result.setdefault('other result, %s' % kind, []).append(
                    '%s: %d of %d vectors differ, first %d; messages compiled %s, dump %s\n        compiled %s\n        dump     %s' % (
                        label, diff, len(lines[0][0]), first + 1, lines[0][1], lines[1][1],
                        lines[0][0][first] if first < len(lines[0][0]) else lines[0][2].strip().split('\n')[-3:],
                        lines[1][0][first] if first < len(lines[1][0]) else lines[1][2].strip().split('\n')[-3:]))
    if n:
        print('sources and the dumps they reproduce (verify.lst), %d pairs with a supported device:' % n)
        for k in sorted(result):
            print('  %-34s %d' % (k, len(result[k])))
        for k in sorted(result):
            if not k.startswith('same result'):
                for label in result[k]:
                    print('    %s: %s' % (k, label))
    return ok


def find_designs(share, keys):
    found = []
    for directory in (OWN, HELP, os.path.join(share, 'Examples', 'Atmel')):
        if not os.path.isdir(directory):
            continue
        for f in sorted(os.listdir(directory)):
            if not f.lower().endswith('.pld'):
                continue
            pld = os.path.join(directory, f)
            si = find_ci(directory, os.path.splitext(f)[0] + '.si')
            try:
                device = pld_pins(pld)[2]
            except (ValueError, OSError):
                continue
            if si and SUPPORTED.match(device) and not UNSUPPORTED.match(device):
                found.append((pld, si))
    if keys:
        found = [d for d in found if any(k in d[0] for k in keys)]
    return found


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--bin-dir', default=os.path.join(ROOT, 'build', 'bin'), help='directory holding cupl and csim')
    ap.add_argument('--share', help='directory of the device libraries (default: next to the programs, else LINCUPL_SHARE)')
    ap.add_argument('-k', action='append', default=[], metavar='SUBSTR', help='only designs whose path contains SUBSTR')
    ap.add_argument('--keep', metavar='DIR', help='leave the work files in DIR')
    ap.add_argument('--corpus', metavar='DIR', help='instead: load and simulate every .jed file under DIR')
    a = ap.parse_args()
    bin_dir = os.path.abspath(a.bin_dir)
    share = None
    for cand in (a.share, os.path.join(bin_dir, '..', 'share', 'lincupl'), os.environ.get('LINCUPL_SHARE'),
                 os.path.join(ROOT, 'share', 'lincupl')):
        if cand and any(os.path.isfile(os.path.join(cand, n)) for n in ('lincupl.dl', 'Atmel.dl', 'cupl.dl')):
            share = os.path.abspath(cand)
            break
    if share is None:
        print('no device library found (give --share or set LINCUPL_SHARE): skipped')
        return 77
    tools = Tools(bin_dir, share)
    work = os.path.abspath(a.keep) if a.keep else tempfile.mkdtemp(prefix='csim_jed_')
    os.makedirs(work, exist_ok=True)
    try:
        if a.corpus:
            return 0 if corpus(tools, os.path.abspath(a.corpus), work) else 1
        suite = Suite(tools, work, a.keep)
        designs = find_designs(share, a.k)
        for pld, si in designs:
            suite.design(pld, si)
        if not a.k:
            print('JEDEC file formats')
            for base in ('t16r', 't20c', 't22a'):
                if os.path.isdir(os.path.join(work, base, 'pins')):
                    format_cases(suite, os.path.join(work, base), base)
            refusal_cases(suite, os.path.join(work, 't16r'), 't16r')
            print('Devices and modes, product term disable fuses')
            mode_cases(suite, work)
            ptd_case(suite, work, 't16c', 2128, 32)
            ptd_case(suite, work, 't16r', 2128, 32)
            ptd_case(suite, work, 't20c', 2640, 40)
            path_case(suite, work, 't22a')
            path_case(suite, work, 'pdefs', 'names')
            print('Devices that are not named, product terms without effect')
            package_cases(suite, work)
            dead_terms_case(suite)
        print('\n%d designs; %d passed (%s), %d failed, %d skipped' % (
            len(designs), suite.passed, ', '.join('%s %d' % kv for kv in sorted(suite.counts.items())),
            len(suite.failed), len(suite.skipped)))
        for name in suite.failed:
            print('  failed: %s' % name)
        return 1 if suite.failed or not designs else 0
    finally:
        if not a.keep:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == '__main__':
    sys.exit(main())
