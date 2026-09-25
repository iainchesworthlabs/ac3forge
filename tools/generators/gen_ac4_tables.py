"""Generate the AC-4 Huffman codebooks, scale factor band tables, noise and QMF tables.

ETSI TS 103 190-1 V1.4.1 prints its Huffman codebooks only by name. Annex
A.0 says the lengths and codewords are in the accompanying
ts_10319001v010401p0.zip, whose ts_103190_tables.c holds a <name>_LEN and a
<name>_CW array for every codebook; that file is the normative table. Annex
A prints, per codebook, codebook_length and the cb_off, cb_mod, cb_mod2 and
cb_mod3 values the decoding process uses, and Tables A.14 (CB_DIM) and A.15
(UNSIGNED_CB). The attachment's Annex B section is empty, so Annex B's
tables are read from the text. ETSI TS 103 190-2 V1.3.1 does the same for
the A-JCC codebooks of its Annex A.1.2 (Tables A.13 to A.24) and the A-JOC
codebooks of its Annex A.1.1 (Tables A.1 to A.12), whose arrays are in
ts_10319002v010301p0.zip's ts_103190_tables_part2.c; they are generated
after Part 1's, as a sixth and a seventh clause.

Reads, from --spec-dir (default spec/ in the repo root):
  ts_10319001_attach/ts_103190_tables.c  every <name>_LEN and <name>_CW array,
                                         RANDOM_NOISE_TABLE (Annex C.11),
                                         ASPX_NOISE (Annex D.2) and QWIN
                                         (Annex D.3).
  ts_10319001v010401p.txt                Annex A's codebook tables and Tables
                                         A.14 and A.15; Annex B's Table B.1,
                                         the 44.1/48 kHz columns of Tables B.4
                                         to B.7, and Tables B.8 to B.19; and
                                         Table 106, for n_side_bits.
  ts_10319002_attach/ts_103190_tables_part2.c
                                         the AJCC_HCB_* and AJOC_HCB_* _LEN and
                                         _CW arrays, and the ISF rendering
                                         matrices SR<config>_to_<layout>
                                         (Annex A.2.1).
  ts_10319002v010301p.txt                Part 2 Annex A.1.1's and A.1.2's
                                         codebook tables.

Writes src/ac4core/src/tables/huffman_tables.hpp and .cpp (every Annex A
codebook, its entries sorted by length and then codeword, as
huffman_codebook.hpp's Codebook wants them for reading), huffman_codes.hpp and
.cpp (the same codebooks in index order, the codeword and its length for each
index, for writing), sfb_tables.hpp and .cpp (Annex
B at 44.1 and 48 kHz), noise_tables.hpp and .cpp (Annex C.11, which the
spectral noise fill of clause 5.1.4 reads through Pseudocode 57) and
qmf_tables.hpp and .cpp (Annex D.3, the QMF banks' window of clauses 5.7.3 and
5.7.4, and Annex D.2, A-SPX's noise generator table of clause 5.7.6.4.3) and
isf_tables.hpp and .cpp (Part 2 Annex A.2.1, the intermediate spatial format's
rendering matrices of Part 2 clause 5.10.3).
src/ac4core is what the AC-4 decoder and encoder share.

Checks, all of them before anything is written, every one failing the run:
  Huffman  Annex A names the same codebooks as the attachment, with the
           attachment's array names; each _LEN and _CW array holds the
           codebook_length Annex A prints; every length is 1 to 32 bits and
           every codeword fits in its length; and no codeword is a prefix of
           (or equal to) another. The Kraft sum of every codebook is printed;
           a codebook whose sum is below 1 is an incomplete code, which is
           listed again at the end but does not fail the run. Part 2's A.1.2
           takes the same checks against the AJCC arrays of its attachment,
           its tables numbered A.13 to A.24 without a gap, each printing
           codebook_length and cb_off and nothing else; and A.1.1 against the
           AJOC arrays, its tables numbered A.1 to A.12, the same way.
  Annex A  its tables run A.1, A.2, ... without a gap; A.14 and A.15 cover
           spectrum codebooks 1 to 11; a dimension-4 spectrum codebook prints
           cb_mod, cb_mod2, cb_mod3 and cb_off with cb_mod^4 ==
           codebook_length, cb_mod2 == cb_mod^2 and cb_mod3 == cb_mod^3, and a
           dimension-2 one prints cb_mod and cb_off only, with cb_mod^2 ==
           codebook_length; and cb_off agrees with A.15 - 0 for an unsigned
           codebook, (cb_mod - 1) / 2 for a signed one, whose values
           Pseudocode 19 then centres on zero.
  Noise    RANDOM_NOISE_TABLE holds 256 float literals, and the negation of
           every entry is also an entry, so the table's mean is exactly zero;
           its mean square is printed.
  QMF      QWIN holds 640 numbers, QWIN[0] is 0, and |QWIN[n]| equals
           |QWIN[640 - n]| for every other n (the signs are the table's own);
           ASPX_NOISE holds 512 pairs, whose mean energy clause 5.7.6.4.3 says
           is 1, and is printed.
  Annex B  Table B.1 lists the fifteen 44.1/48 kHz transform lengths, as
           Table 106 does; every row of Tables B.1 and B.4 to B.19 has
           exactly one reading (see read_row) with its sfb or max_sfb_master
           index in sequence; every 44.1/48 kHz offset column starts at 0,
           strictly increases through num_sfb + 1 entries and ends at its
           transform length; and each of Tables B.8 to B.19 has one row per
           max_sfb_master value n_side_bits can express, columns only for
           shorter lengths that Table B.1 lists, and values that never
           decrease down a column nor exceed that column's num_sfb.

In Annex B's text a number prints with a space as its thousands separator
("1 600"); columns are separated by runs of spaces, except that an index
column can sit one space from the value beside it ("37 576"); and a column
holds "-" below the last band of its transform length. read_row()
enumerates every way to read a row and requires exactly one of them to fit
the table's shape, so a row with two fitting readings stops the run.

Run from the repo root:
    python tools/generators/gen_ac4_tables.py [--spec-dir DIR]
"""

import argparse
import itertools
import re
from dataclasses import dataclass, field
from fractions import Fraction
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
OUT_DIR = REPO / "src" / "ac4core" / "src" / "tables"
SPEC_TXT = "ts_10319001v010401p.txt"
TABLES_C = Path("ts_10319001_attach") / "ts_103190_tables.c"
SPEC2_TXT = "ts_10319002v010301p.txt"
TABLES2_C = Path("ts_10319002_attach") / "ts_103190_tables_part2.c"

MAX_BITS = 32  # huffman_codebook.hpp's kMaxHuffBits

# Annex A's clauses, with the heading each has in the text and the label
# huffman_tables.hpp's section comments give it.
CLAUSES = {
    1: ("ASF Huffman codebook tables", "ASF"),
    2: ("A-SPX Huffman codebook tables", "A-SPX"),
    3: ("A-CPL Huffman codebook tables", "A-CPL"),
    4: ("Dialogue enhancement Huffman codebook tables", "dialogue enhancement"),
    5: ("Dynamic range control Huffman codebook table", "DRC"),
}

# Part 2's Annex A.1.2, the A-JCC codebooks, which the generated files list
# after Part 1's five clauses as a sixth. Its tables are numbered A.13 to
# A.24, after A.1.1's twelve A-JOC codebooks.
AJCC_CLAUSE = 6
AJCC_LABEL = "Part 2 A.1.2: A-JCC"
AJCC_TABLES = range(13, 25)
AJCC_NAME = re.compile(r"AJCC_HCB_(?:DRY|WET)_(?:COARSE|FINE)_(?:F0|DF|DT)")

# Part 2's Annex A.1.1, the A-JOC codebooks, listed after A-JCC's as a seventh
# clause, so that what the generated files held before keeps its place.
AJOC_CLAUSE = 7
AJOC_LABEL = "Part 2 A.1.1: A-JOC"
AJOC_TABLES = range(1, 13)
AJOC_NAME = re.compile(r"AJOC_HCB_(?:DRY|WET)_(?:COARSE|FINE)_(?:F0|DF|DT)")

# Every clause the generated files have, with its label.
OUTPUT_CLAUSES = {**{n: label for n, (_, label) in CLAUSES.items()}, AJCC_CLAUSE: AJCC_LABEL,
                  AJOC_CLAUSE: AJOC_LABEL}
PART2_CLAUSES = (AJCC_CLAUSE, AJOC_CLAUSE)

# Tables A.14 and A.15 number the spectrum codebooks 1 to 11, and
# huffman_tables.hpp's arrays by that number are 12 long, index 0 unused.
SPECTRUM_CODEBOOKS = 11

# The transform lengths of a 44.1 or 48 kHz stream: the frame lengths 2 048,
# 1 920 and 1 536 and their halves down to a sixteenth, in Table B.1's order.
LENGTHS_48 = [2048, 1920, 1536, 1024, 960, 768, 512, 480, 384, 256, 240, 192, 128, 120, 96]
# Tables B.2 and B.3: every LENGTHS_48 entry doubled and quadrupled, in the
# same order - the HSF extension's own transform lengths at 96 and 192 kHz.
# Numerically some of these coincide with a LENGTHS_48 or LENGTHS_96 value
# (4 x 256 == 2 x 512 == 1 024, say); that is not a collision, since each
# rate's num_sfb/offsets are looked up in that rate's own table.
LENGTHS_96 = [length * 2 for length in LENGTHS_48]
LENGTHS_192 = [length * 4 for length in LENGTHS_48]

TABLE_TITLE = re.compile(r"^\s*Table ([AB])\.(\d+):\s*(.*?)\s*$")
CLAUSE_HEADING = re.compile(r"^\s*A\.(\d)\s+(\S.*?)\s*$")
CODEBOOK_KEYS = ("Codebook name", "Codebook length table", "Codebook codeword table",
                 "codebook_length", "cb_mod3", "cb_mod2", "cb_mod", "cb_off")
CODEBOOK_FIELD = re.compile(r"^\s*(" + "|".join(CODEBOOK_KEYS) + r")\s+(\S+)\s*$")
SPECTRUM_ROW = re.compile(r"^\s*(Codebook number|CB_DIM|UNSIGNED_CB)((?:\s+\S+)+)\s*$")
C_ARRAY = re.compile(
    r"\bconst\s+(?:unsigned\s+)?\w+\s+(\w+)_(LEN|CW)\s*\[\s*(\d+)\s*\]\s*=\s*\{([^{}]*)\}\s*;")
C_VALUE = re.compile(r"0[xX][0-9a-fA-F]+|\d+")
NOISE_TABLE = re.compile(
    r"\bconst\s+float32\s+RANDOM_NOISE_TABLE\s*\[\s*(\d+)\s*\]\s*=\s*\{([^{}]*)\}\s*;")
FLOAT_LITERAL = re.compile(r"-?\d+\.\d+f")
NOISE_ENTRIES = 256
QWIN_TABLE = re.compile(r"\bconst\s+float\s+QWIN\s*\[\s*(\d+)\s*\]\s*=\s*\{([^{}]*)\}\s*;")
QWIN_ENTRIES = 640
ASPX_NOISE_TABLE = re.compile(
    r"\bconst\s+float\s+ASPX_NOISE\s*\[\s*(\d+)\s*\]\s*\[\s*2\s*\]\s*=\s*\{(.*?)\}\s*;",
    re.S)
ASPX_NOISE_ENTRIES = 512
# A number as Annex D prints it: "0", "-0.70912", "1.990318758627504e-004".
D_NUMBER = re.compile(r"-?\d+(?:\.\d+)?(?:e[-+]\d+)?")
# Part 2 Annex A.2.1: float SR<config>_to_<layout>[NUM_ISF_CHAN_SR<config>]
# [NUM_SPKR_CHAN_<layout>] = {{...}, ...}; and the sizes' #defines.
ISF_MATRIX = re.compile(
    r"\bfloat\s+SR(\d+)_to_(\d+)\s*\[\s*(\w+)\s*\]\s*\[\s*(\w+)\s*\]\s*=\s*\{(.*?)\}\s*;", re.S)
ISF_ROW = re.compile(r"\{([^{}]*)\}")
ISF_FLOAT = re.compile(r"-?\d+\.\d+e[-+]\d+f")
C_DEFINE = re.compile(r"#define\s+(\w+)\s+(\d+)")
# Table 61's stacked ring formats in isf_config order, and the output layouts
# Tables A.25 and A.26 name, in the order kIsfMatrices holds them.
ISF_CONFIGS = ["3100", "5300", "7300", "9500", "7530", "15951"]
ISF_LAYOUTS = ["2", "5", "7", "9", "502", "504", "702", "704", "902", "904"]
ASPX_NOISE_PAIR = re.compile(r"\{\s*(" + D_NUMBER.pattern + r")\s*,\s*(" + D_NUMBER.pattern
                             + r")\s*\}")
# A printed number: its first group of digits, then any groups of three after
# a single space.
NUMBER = r"\d{1,3}(?: \d{3})*"


def check(condition, message):
    """Fail the run with SystemExit, which `python -O` keeps (it strips asserts)."""
    if not condition:
        raise SystemExit(f"gen_ac4_tables.py: {message}")


def number_value(text):
    return int(text.replace(" ", ""))


# ---------------------------------------------------------------------------
# The text: annexes and tables
# ---------------------------------------------------------------------------

def annex(lines, letter, next_letter):
    """(line number, text) for every line of Annex `letter`, headings excluded."""
    def heading(which):
        found = [i for i, text in enumerate(lines) if text.strip() == f"Annex {which} (normative):"]
        check(len(found) == 1, f"'Annex {which} (normative):' found {len(found)} times, not once")
        return found[0]

    start, end = heading(letter), heading(next_letter)
    check(start < end, f"Annex {letter} does not come before Annex {next_letter}")
    return [(i + 1, lines[i]) for i in range(start + 1, end)]


@dataclass
class Section:
    number: int
    line: int
    title: str
    body: list  # (line number, text) after the title


def table_sections(numbered, letter):
    """{n: Section} for every 'Table <letter>.n:' title, numbered 1, 2, ... in order.

    A title runs on over the lines after it until a blank line; the body is
    everything from there to the next title.
    """
    starts = [i for i, (_, text) in enumerate(numbered)
              if (m := TABLE_TITLE.match(text)) and m.group(1) == letter]
    sections = {}
    for position, start in enumerate(starts):
        end = starts[position + 1] if position + 1 < len(starts) else len(numbered)
        match = TABLE_TITLE.match(numbered[start][1])
        title = [match.group(3)]
        cursor = start + 1
        while cursor < end and numbered[cursor][1].strip():
            title.append(numbered[cursor][1].strip())
            cursor += 1
        number = int(match.group(2))
        check(number == position + 1,
              f"line {numbered[start][0]}: Table {letter}.{number} where Table "
              f"{letter}.{position + 1} was due")
        sections[number] = Section(number, numbered[start][0], " ".join(title),
                                   numbered[cursor:end])
    return sections


# ---------------------------------------------------------------------------
# Annex A and the attachment
# ---------------------------------------------------------------------------

@dataclass
class Codebook:
    table: int  # Annex A's table number
    clause: int
    name: str
    printed: dict  # codebook_length, cb_mod, cb_mod2, cb_mod3, cb_off: whichever the table prints
    lengths: list = field(default_factory=list)
    codewords: list = field(default_factory=list)
    kraft: Fraction = Fraction(0)
    part: int = 1  # the part whose Annex A prints it

    @property
    def cxx(self):
        return "k" + "".join(part.capitalize() for part in self.name.split("_"))

    @property
    def label(self):
        """The table's name as the comments give it: Part 1's plainly, Part 2's with its part."""
        return f"Table A.{self.table}" if self.part == 1 else f"Part 2 Table A.{self.table}"

    def value(self, key):
        return self.printed.get(key, 0)


def parse_annex_a(numbered):
    """The codebooks in table order, and Tables A.14 and A.15 as lists by codebook number."""
    headings = {}
    clause = 0
    tables = []  # [number, title, clause, line, fields, rows]
    for line, text in numbered:
        if match := CLAUSE_HEADING.match(text):
            clause = int(match.group(1))
            headings[clause] = match.group(2)
        elif match := TABLE_TITLE.match(text):
            check(match.group(1) == "A", f"line {line}: a Table {match.group(1)} inside Annex A")
            tables.append([int(match.group(2)), match.group(3), clause, line, {}, []])
        elif match := CODEBOOK_FIELD.match(text):
            check(tables, f"line {line}: {match.group(1)} before Annex A's first table")
            key, fields = match.group(1), tables[-1][4]
            check(key not in fields, f"line {line}: Table A.{tables[-1][0]} prints {key} twice")
            fields[key] = match.group(2)
        elif match := SPECTRUM_ROW.match(text):
            check(tables, f"line {line}: {match.group(1)} before Annex A's first table")
            tables[-1][5].append((match.group(1), match.group(2).split()))
        else:
            # Everything left is prose, page furniture or blank - but a line
            # that starts like a field and did not parse as one is a misread.
            check(not text.strip().startswith((*CODEBOOK_KEYS, "CB_DIM", "UNSIGNED_CB")),
                  f"line {line}: unreadable Annex A field: {text.strip()!r}")

    check(headings == {0: "Introduction", **{n: h for n, (h, _) in CLAUSES.items()}},
          f"Annex A's clause headings are {headings}")
    numbers = [table[0] for table in tables]
    check(numbers == list(range(1, len(numbers) + 1)),
          f"Annex A's tables are numbered {numbers}, not A.1 to A.{len(numbers)}")

    codebooks, spectrum = [], {}
    for number, title, table_clause, line, fields, rows in tables:
        if title in ("CB_DIM", "UNSIGNED_CB"):
            check(not fields, f"Table A.{number} ({title}) prints codebook fields")
            spectrum[title] = (number, dict(rows), line)
            continue
        check(not rows, f"Table A.{number} prints a CB_DIM or UNSIGNED_CB row")
        for key in CODEBOOK_KEYS[:4]:
            check(key in fields, f"Table A.{number} (line {line}) prints no {key}")
        name = fields["Codebook name"]
        check(re.fullmatch(r"[A-Z][A-Z0-9_]*", name), f"Table A.{number}: codebook name {name!r}")
        for key, suffix in (("Codebook length table", "_LEN"), ("Codebook codeword table", "_CW")):
            check(fields[key] == name + suffix,
                  f"Table A.{number}: {key} {fields[key]}, not {name}{suffix}")
        printed = {}
        for key in CODEBOOK_KEYS[3:]:
            if key in fields:
                check(re.fullmatch(r"\d+", fields[key]),
                      f"Table A.{number}: {key} is {fields[key]!r}, not a number")
                printed[key] = int(fields[key])
        check(table_clause in CLAUSES, f"Table A.{number} is outside clauses A.1 to A.5")
        codebooks.append(Codebook(number, table_clause, name, printed))

    check(set(spectrum) == {"CB_DIM", "UNSIGNED_CB"}, "Annex A has no Table CB_DIM or UNSIGNED_CB")
    labels = [str(n) for n in range(1, SPECTRUM_CODEBOOKS + 1)]
    by_number = {}
    for title, parse in (("CB_DIM", int), ("UNSIGNED_CB", {"true": True, "false": False}.get)):
        number, rows, line = spectrum[title]
        check(set(rows) == {"Codebook number", title},
              f"Table A.{number} (line {line}) has rows {sorted(rows)}")
        check(rows["Codebook number"] == labels,
              f"Table A.{number} numbers codebooks {rows['Codebook number']}, not 1 to "
              f"{SPECTRUM_CODEBOOKS}")
        check(len(rows[title]) == SPECTRUM_CODEBOOKS and
              all(re.fullmatch(r"\d+|true|false", v) for v in rows[title]),
              f"Table A.{number} gives {rows[title]}")
        by_number[title] = [None, *(parse(v) for v in rows[title])]
    return codebooks, by_number["CB_DIM"], by_number["UNSIGNED_CB"]


def part2_section(lines, heading, next_heading):
    """(line number, text) for a clause of Part 2's Annex A, from its heading to the next's."""
    starts = [i for i, text in enumerate(lines) if re.fullmatch(heading, text)]
    ends = [i for i, text in enumerate(lines) if re.fullmatch(next_heading, text)]
    check(len(starts) == 1 and len(ends) == 1 and starts[0] < ends[0],
          f"Part 2's heading {heading!r} found {len(starts)} times and the next "
          f"{len(ends)} times")
    return [(i + 1, lines[i]) for i in range(starts[0] + 1, ends[0])]


def ajcc_section(lines):
    """Part 2's Annex A.1.2, from its heading to A.2's."""
    return part2_section(lines, r"\s*A\.1\.2\s+A-JCC Huffman codebook tables\s*",
                         r"\s*A\.2\s+Coefficient tables\s*")


def ajoc_section(lines):
    """Part 2's Annex A.1.1, from its heading to A.1.2's."""
    return part2_section(lines, r"\s*A\.1\.1\s+A-JOC Huffman codebook tables\s*",
                         r"\s*A\.1\.2\s+A-JCC Huffman codebook tables\s*")


def parse_part2_annex(numbered, clause, tables_range, name_re, tool, heading):
    """A clause of Part 2 Annex A's codebooks in table order, read as parse_annex_a()
    reads Part 1's: `tool` is the name the tables' titles give ("A-JCC"), `heading`
    the clause's number ("A.1.2")."""
    tables = []  # [number, title, line, fields]
    for line, text in numbered:
        if match := TABLE_TITLE.match(text):
            check(match.group(1) == "A",
                  f"Part 2 line {line}: a Table {match.group(1)} inside {heading}")
            tables.append([int(match.group(2)), match.group(3), line, {}])
        elif match := CODEBOOK_FIELD.match(text):
            check(tables, f"Part 2 line {line}: {match.group(1)} before {heading}'s first table")
            key, fields = match.group(1), tables[-1][3]
            check(key not in fields, f"Part 2 Table A.{tables[-1][0]} prints {key} twice")
            fields[key] = match.group(2)
        else:
            check(not text.strip().startswith(CODEBOOK_KEYS),
                  f"Part 2 line {line}: unreadable {heading} field: {text.strip()!r}")
    numbers = [table[0] for table in tables]
    check(numbers == list(tables_range),
          f"Part 2's {heading} numbers its tables {numbers}, not A.{tables_range[0]} to "
          f"A.{tables_range[-1]}")
    codebooks = []
    for number, title, line, fields in tables:
        expected = {"Codebook name", "Codebook length table", "Codebook codeword table",
                    "codebook_length", "cb_off"}
        check(set(fields) == expected,
              f"Part 2 Table A.{number} (line {line}) prints {sorted(fields)}, not "
              f"{sorted(expected)}")
        name = fields["Codebook name"]
        check(name_re.fullmatch(name), f"Part 2 Table A.{number}: codebook name {name!r}")
        check(title == f"{tool} Huffman codebook {name}",
              f"Part 2 Table A.{number} is titled {title!r} over codebook {name}")
        for key, suffix in (("Codebook length table", "_LEN"), ("Codebook codeword table", "_CW")):
            check(fields[key] == name + suffix,
                  f"Part 2 Table A.{number}: {key} {fields[key]}, not {name}{suffix}")
        printed = {}
        for key in ("codebook_length", "cb_off"):
            check(re.fullmatch(r"\d+", fields[key]),
                  f"Part 2 Table A.{number}: {key} is {fields[key]!r}, not a number")
            printed[key] = int(fields[key])
        codebooks.append(Codebook(number, clause, name, printed, part=2))
    return codebooks


def parse_ajcc_annex(numbered):
    """Part 2 Annex A.1.2's codebooks in table order."""
    return parse_part2_annex(numbered, AJCC_CLAUSE, AJCC_TABLES, AJCC_NAME, "A-JCC", "A.1.2")


def parse_ajoc_annex(numbered):
    """Part 2 Annex A.1.1's codebooks in table order."""
    return parse_part2_annex(numbered, AJOC_CLAUSE, AJOC_TABLES, AJOC_NAME, "A-JOC", "A.1.1")


def check_ajcc_offsets(codebooks):
    """Each A-JCC codebook's cb_off against its length: 0 for an F0 codebook, whose
    values huff_decode() returns as they are, and the middle index for a DF or DT
    one, whose values huff_decode_diff() centres on zero (Part 2 6.2.6.4)."""
    for cb in codebooks:
        length, offset = cb.printed["codebook_length"], cb.printed["cb_off"]
        if cb.name.endswith("_F0"):
            check(offset == 0, f"{cb.name}: cb_off {offset}, where an F0 codebook has 0")
        else:
            check(length % 2 == 1 and offset == (length - 1) // 2,
                  f"{cb.name}: cb_off {offset} is not the middle of {length} entries")


def check_ajoc_offsets(codebooks):
    """Each A-JOC codebook's cb_off against its length. An F0 and a DF codebook print
    0 and as many entries as the quantiser has steps (51 or 101 dry, 21 or 41 wet),
    since Pseudocode 16 adds a DF value to the band below modulo that count; a DT
    codebook prints twice the steps less one, centred, its values taken from -
    (steps - 1) to steps - 1 about the same band of the set before."""
    steps = {("DRY", "COARSE"): 51, ("DRY", "FINE"): 101, ("WET", "COARSE"): 21,
             ("WET", "FINE"): 41}
    for cb in codebooks:
        _, _, data_type, quant, kind = cb.name.split("_")
        count = steps[(data_type, quant)]
        length, offset = cb.printed["codebook_length"], cb.printed["cb_off"]
        if kind in ("F0", "DF"):
            check(length == count and offset == 0,
                  f"{cb.name}: {length} entries at cb_off {offset}, not {count} at 0")
        else:
            check(length == 2 * count - 1 and offset == count - 1,
                  f"{cb.name}: {length} entries at cb_off {offset}, not {2 * count - 1} at "
                  f"{count - 1}")


def parse_attachment(path):
    """{(codebook name, 'LEN' or 'CW'): [values]} for every Huffman array in the attachment."""
    source = path.read_text(encoding="utf-8")
    source = re.sub(r"/\*.*?\*/", " ", source, flags=re.S)
    source = re.sub(r"//[^\n]*", " ", source)
    arrays = {}
    for match in C_ARRAY.finditer(source):
        name, kind, size, body = match.groups()
        leftover = re.sub(r"[\s,]", "", C_VALUE.sub("", body))
        check(not leftover, f"{name}_{kind}: unexpected {leftover[:20]!r} among its values")
        values = [int(v, 16) if v[:2].lower() == "0x" else int(v) for v in C_VALUE.findall(body)]
        check(len(values) == int(size), f"{name}_{kind}[{size}] holds {len(values)} values")
        check((name, kind) not in arrays, f"{name}_{kind} is defined twice")
        arrays[name, kind] = values
    declared = re.findall(r"\b\w+_(?:LEN|CW)\s*\[", source)
    check(len(declared) == len(arrays),
          f"{len(declared)} _LEN/_CW arrays declared but {len(arrays)} parsed")
    return arrays


def parse_noise_table(path):
    """RANDOM_NOISE_TABLE's entries, as the float literals the attachment prints."""
    source = path.read_text(encoding="utf-8")
    check("Annex C.11 RANDOM_NOISE_TABLE" in source,
          "the attachment has no 'Annex C.11 RANDOM_NOISE_TABLE' heading")
    match = NOISE_TABLE.search(source)
    check(match is not None, "the attachment has no RANDOM_NOISE_TABLE array")
    size, body = match.groups()
    literals = FLOAT_LITERAL.findall(body)
    leftover = re.sub(r"[\s,]", "", FLOAT_LITERAL.sub("", body))
    check(not leftover, f"RANDOM_NOISE_TABLE: unexpected {leftover[:20]!r} among its values")
    check(int(size) == NOISE_ENTRIES and len(literals) == NOISE_ENTRIES,
          f"RANDOM_NOISE_TABLE[{size}] holds {len(literals)} values, not {NOISE_ENTRIES}")
    values = [Fraction(text[:-1]) for text in literals]
    present = set(values)
    unpaired = [text for text, value in zip(literals, values, strict=True) if -value not in present]
    check(not unpaired, f"RANDOM_NOISE_TABLE: no negation of {unpaired[:4]}")
    return literals


def float_literal(text):
    """A number Annex D prints, as a C++ float literal that keeps every digit printed."""
    return f"{text}f" if "." in text or "e" in text else f"{text}.0f"


def parse_qwin(path):
    """QWIN's 640 entries (Annex D.3), as the numbers the attachment prints."""
    source = path.read_text(encoding="utf-8")
    check("Annex D.3 QWIN" in source, "the attachment has no 'Annex D.3 QWIN' heading")
    match = QWIN_TABLE.search(source)
    check(match is not None, "the attachment has no QWIN array")
    size, body = match.groups()
    numbers = D_NUMBER.findall(body)
    leftover = re.sub(r"[\s,]", "", D_NUMBER.sub("", body))
    check(not leftover, f"QWIN: unexpected {leftover[:20]!r} among its values")
    check(int(size) == QWIN_ENTRIES and len(numbers) == QWIN_ENTRIES,
          f"QWIN[{size}] holds {len(numbers)} values, not {QWIN_ENTRIES}")
    values = [Fraction(text) for text in numbers]
    check(values[0] == 0, f"QWIN[0] is {numbers[0]}, not 0")
    asymmetric = [n for n in range(1, QWIN_ENTRIES)
                  if abs(values[n]) != abs(values[QWIN_ENTRIES - n])]
    check(not asymmetric, f"QWIN: |QWIN[n]| != |QWIN[640 - n]| at n = {asymmetric[:4]}")
    return numbers


def parse_aspx_noise(path):
    """ASPX_NOISE's 512 (real, imaginary) pairs (Annex D.2), as the attachment prints them."""
    source = path.read_text(encoding="utf-8")
    check("Annex D.2 ASPX_NOISE" in source, "the attachment has no 'Annex D.2 ASPX_NOISE' heading")
    match = ASPX_NOISE_TABLE.search(source)
    check(match is not None, "the attachment has no ASPX_NOISE array")
    size, body = match.groups()
    pairs = ASPX_NOISE_PAIR.findall(body)
    leftover = re.sub(r"[\s,]", "", ASPX_NOISE_PAIR.sub("", body))
    check(not leftover, f"ASPX_NOISE: unexpected {leftover[:20]!r} among its values")
    check(int(size) == ASPX_NOISE_ENTRIES and len(pairs) == ASPX_NOISE_ENTRIES,
          f"ASPX_NOISE[{size}][2] holds {len(pairs)} pairs, not {ASPX_NOISE_ENTRIES}")
    return pairs


def parse_isf(path):
    """{(config, layout): [[float literal, ...] per ISF channel]} for every matrix of Part 2
    Annex A.2.1, their sizes checked against the attachment's #defines."""
    source = path.read_text(encoding="utf-8")
    defines = {name: int(value) for name, value in C_DEFINE.findall(source)}
    matrices = {}
    for config, layout, rows_name, cols_name, body in ISF_MATRIX.findall(source):
        rows = [ISF_FLOAT.findall(row) for row in ISF_ROW.findall(body)]
        leftover = re.sub(r"[\s,{}]", "", ISF_FLOAT.sub("", body))
        check(not leftover,
              f"SR{config}_to_{layout}: unexpected {leftover[:20]!r} among its values")
        check(rows_name in defines and cols_name in defines,
              f"SR{config}_to_{layout}: {rows_name} or {cols_name} is not defined")
        check(len(rows) == defines[rows_name] and all(len(r) == defines[cols_name] for r in rows),
              f"SR{config}_to_{layout} is not {defines[rows_name]} rows of {defines[cols_name]}")
        check((config, layout) not in matrices, f"SR{config}_to_{layout} is defined twice")
        matrices[config, layout] = rows
    wanted = {(config, layout) for config in ISF_CONFIGS for layout in ISF_LAYOUTS}
    check(set(matrices) == wanted, f"ISF matrices missing {sorted(wanted - set(matrices))[:4]}, "
                                   f"extra {sorted(set(matrices) - wanted)[:4]}")
    return matrices


def attach_codes(codebooks, arrays):
    names = {cb.name for cb in codebooks}
    check(len(names) == len(codebooks), "Annex A names a codebook twice")
    in_attachment = {name for name, _ in arrays}
    check(names == in_attachment,
          f"codebooks only in Annex A: {sorted(names - in_attachment)}; only in the attachment: "
          f"{sorted(in_attachment - names)}")
    for cb in codebooks:
        check((cb.name, "LEN") in arrays and (cb.name, "CW") in arrays,
              f"{cb.name}: the attachment lacks its _LEN or its _CW array")
        cb.lengths, cb.codewords = arrays[cb.name, "LEN"], arrays[cb.name, "CW"]
        cb.kraft = check_code(cb)


def check_code(cb):
    """Fail on anything that stops the arrays being a prefix code; return the Kraft sum."""
    length = cb.printed["codebook_length"]
    check(len(cb.lengths) == length,
          f"{cb.name}_LEN has {len(cb.lengths)} entries; {cb.label} prints "
          f"codebook_length {length}")
    check(len(cb.codewords) == length,
          f"{cb.name}_CW has {len(cb.codewords)} entries; {cb.label} prints "
          f"codebook_length {length}")
    check(length < 1 << 16, f"{cb.name}: {length} entries overflow HuffEntry::index")
    pairs = list(zip(cb.lengths, cb.codewords, strict=True))
    for index, (bits, code) in enumerate(pairs):
        check(1 <= bits <= MAX_BITS, f"{cb.name}[{index}]: length {bits} is not 1 to {MAX_BITS}")
        check(code < 1 << bits,
              f"{cb.name}[{index}]: codeword {code:#x} needs more than {bits} bits")
    # Left-aligned, a codeword's extensions sort straight after it, so a
    # prefix shows up as a clash between neighbours.
    aligned = sorted((code << (MAX_BITS - bits), bits, index)
                     for index, (bits, code) in enumerate(pairs))
    for (first, first_bits, first_index), (second, _, second_index) in itertools.pairwise(aligned):
        check(second >= first + (1 << (MAX_BITS - first_bits)),
              f"{cb.name}: codeword {first_index} is a prefix of, or equal to, codeword "
              f"{second_index}")
    return sum((Fraction(1, 1 << bits) for bits in cb.lengths), Fraction(0))


def check_spectrum_values(codebooks, cb_dim, unsigned_cb):
    by_name = {cb.name: cb for cb in codebooks}
    for number in range(1, SPECTRUM_CODEBOOKS + 1):
        name = f"ASF_HCB_{number}"
        check(name in by_name, f"Annex A has no codebook {name}")
        cb, dim = by_name[name], cb_dim[number]
        printed, length = cb.printed, cb.printed["codebook_length"]
        if dim == 4:
            check(set(printed) == {"codebook_length", "cb_mod", "cb_mod2", "cb_mod3", "cb_off"},
                  f"{name}: CB_DIM 4, but Table A.{cb.table} prints {sorted(printed)}")
            mod = printed["cb_mod"]
            check(mod ** 4 == length, f"{name}: cb_mod {mod}, but codebook_length {length}")
            check(printed["cb_mod2"] == mod ** 2, f"{name}: cb_mod2 {printed['cb_mod2']}")
            check(printed["cb_mod3"] == mod ** 3, f"{name}: cb_mod3 {printed['cb_mod3']}")
        else:
            check(dim == 2, f"Table A.14 gives codebook {number} dimension {dim}")
            check(set(printed) == {"codebook_length", "cb_mod", "cb_off"},
                  f"{name}: CB_DIM 2, but Table A.{cb.table} prints {sorted(printed)}")
            mod = printed["cb_mod"]
            check(mod ** 2 == length, f"{name}: cb_mod {mod}, but codebook_length {length}")
        expected_off = 0 if unsigned_cb[number] else (mod - 1) // 2
        check(unsigned_cb[number] or mod % 2 == 1, f"{name}: signed, but cb_mod {mod} is even")
        check(printed["cb_off"] == expected_off,
              f"{name}: cb_off {printed['cb_off']}, but UNSIGNED_CB {unsigned_cb[number]} "
              f"with cb_mod {mod} wants {expected_off}")
    for cb in codebooks:
        for key in ("cb_off", "cb_mod", "cb_mod2", "cb_mod3"):
            check(cb.value(key) < 1 << 15, f"{cb.name}: {key} {cb.value(key)} overflows int16")


# ---------------------------------------------------------------------------
# Annex B
# ---------------------------------------------------------------------------

HEADER_MARKS = ("sfb_offset", "max_sfb_master[", "num_sfb")


def body_rows(section):
    """The column heading lines and the data rows of a table's body.

    A heading line starts the rows; a line with letters on it (a page
    header, "ETSI") stops them until the next heading, which each page
    repeats; lines of '@' labels under a heading are neither.
    """
    headings, rows = [], []
    in_rows = False
    for line, text in section.body:
        stripped = text.strip()
        if not stripped:
            continue
        if any(mark in stripped for mark in HEADER_MARKS):
            headings.append(stripped)
            in_rows = True
        elif re.fullmatch(r"[\d -]+", stripped):
            check(in_rows, f"line {line}: Table B.{section.number} has a row before its headings")
            rows.append((line, text))
        elif re.search(r"[A-Za-z]", stripped):
            in_rows = False
    check(headings and rows, f"Table B.{section.number}: no headings or no rows")
    return headings, rows


def readings(text):
    """Every way to read a row's tokens as fields.

    A field is "-" or a number. A token of exactly three digits one space
    after a number may be either the next thousands group of that number or
    a field of its own, and both readings are kept; everything else is
    decided by the text (a token with a leading zero cannot start a number).
    """
    tokens = [(m.start(), m.end(), m.group()) for m in re.finditer(r"\S+", text)]
    found = []

    def extend(position, fields):
        if position == len(tokens):
            found.append(fields)
            return
        start, _, token = tokens[position]
        if (fields and start == tokens[position - 1][1] + 1 and re.fullmatch(r"\d{3}", token)
                and re.fullmatch(NUMBER, fields[-1])):
            extend(position + 1, [*fields[:-1], f"{fields[-1]} {token}"])
        if token == "-" or re.fullmatch(r"0|[1-9]\d{0,2}", token):
            extend(position + 1, [*fields, token])

    extend(0, [])
    return found


def read_row(line, text, width, index=None, fits=None):
    """The one reading of a row with `width` fields that fits.

    `index`, when given, is what the first field must be; `fits` any further
    condition. No reading, or more than one, fails the run.
    """
    fitting = [fields for fields in readings(text)
               if len(fields) == width and (index is None or fields[0] == str(index))
               and (fits is None or fits(fields))]
    check(len(fitting) == 1,
          f"line {line}: {len(fitting)} readings fit, not exactly 1: {text.strip()!r}")
    return fitting[0]


# ERRATA.md's "Misprints with no effect": Table B.2 (96 kHz) prints a
# transform length of 920 where every other table - Table 83's frame_len_base
# doubling for 96 kHz, and the 96 kHz column of Tables B.4 to B.7, whose own
# data this length's num_sfb has to agree with - has 960. Corrected here, at
# the one place that reads Table B.2's transform-length column, rather than
# carried as a second, wrong spelling of the same length into LENGTHS_96 and
# everything keyed by it.
MISPRINTS = {("B", 2, 920): 960}


def parse_num_sfb(section, expected_lengths):
    """{transform length: num_sfb} for one of Tables B.1 to B.3."""
    number = section.number
    _, rows = body_rows(section)
    num_sfb = {}
    for line, text in rows:
        fields = read_row(line, text, 2, fits=lambda f: "-" not in f)
        length = number_value(fields[0])
        length = MISPRINTS.get(("B", number, length), length)
        check(length not in num_sfb, f"line {line}: Table B.{number} lists {length} twice")
        num_sfb[length] = number_value(fields[1])
    check(list(num_sfb) == expected_lengths,
          f"Table B.{number} lists transform lengths {list(num_sfb)}, not {expected_lengths}")
    return num_sfb


def parse_b1(section):
    return parse_num_sfb(section, LENGTHS_48)


def parse_n_side_bits(lines):
    """Table 106's n_side_bits, by transform length."""
    title = [i for i, text in enumerate(lines) if re.match(r"^\s*Table 106:", text)]
    check(len(title) == 1, f"'Table 106:' found {len(title)} times, not once")
    rows, heading = {}, False
    for i in range(title[0] + 1, len(lines)):
        text = lines[i].strip()
        if text.startswith("Table 107:"):
            break
        if re.fullmatch(r"Transform length\s+n_msfb_bits\s+n_side_bits\s+n_msfbl_bits", text):
            heading = True
        elif match := re.fullmatch(r"(\d{1,3}(?: \d{3})?)\s{2,}(\d+)\s{2,}(\d+)\s{2,}(\d+|N/A)",
                                   text):
            check(heading, f"line {i + 1}: a Table 106 row before its heading")
            length = number_value(match.group(1))
            check(length not in rows, f"line {i + 1}: Table 106 lists {length} twice")
            rows[length] = int(match.group(3))
    check(list(rows) == LENGTHS_48, f"Table 106 lists transform lengths {list(rows)}")
    return rows


def parse_offsets(section, num_sfb, num_sfb_96, num_sfb_192):
    """{rate: {transform length: offsets}} for rates 48, 96 and 192, from the
    44.1/48 kHz columns of one of Tables B.4 to B.7.

    The title names the 44.1/48 kHz lengths in column order. Tables B.4 to
    B.6 print each row twice over, the second half (after a "-" column)
    carrying on from the sfb where the first half's last row stops; Table
    B.7 prints one half. Each column continues below its 48 kHz length's
    last band, as far as it goes: to the 96 kHz length sharing it (double),
    then to the 192 kHz length sharing it (quadruple) where the title names
    one - the same underlying sequence at three prefix lengths, per the
    tables' own headings ("2 048@44,1 / 2 048@48 / 4 096@96 / 8 192@192").
    """
    number = section.number
    match = re.search(r"44,1 kHz or 48 kHz and transform length (.*?);", section.title)
    check(match, f"Table B.{number}'s title names no 44,1 kHz or 48 kHz transform lengths")
    lengths = [number_value(v) for v in re.findall(NUMBER, match.group(1))]
    headings, rows = body_rows(section)
    halves = headings[0].count("sfb_offset")
    check(halves in (1, 2) and all(h.count("sfb_offset") == halves for h in headings),
          f"Table B.{number}: headings {headings}")
    width = len(lengths)

    columns = [[] for _ in lengths]
    second = [[] for _ in lengths]
    second_start, second_ended = None, False
    for row, (line, text) in enumerate(rows):
        if halves == 1:
            fields = read_row(line, text, 1 + width, index=row)
            for column, value in enumerate(fields[1:]):
                columns[column].append(value)
            continue
        fields = read_row(line, text, 3 + 2 * width, index=row,
                          fits=lambda f: f[1 + width] == "-"
                          and re.fullmatch(r"\d+|-", f[2 + width]) is not None)
        for column, value in enumerate(fields[1:1 + width]):
            columns[column].append(value)
        sfb, values = fields[2 + width], fields[3 + width:]
        if sfb == "-":
            check(all(v == "-" for v in values), f"line {line}: values beside an sfb of '-'")
            second_ended = True
            continue
        check(not second_ended, f"line {line}: the second half resumes after a row of '-'")
        if second_start is None:
            second_start = int(sfb)
        check(int(sfb) == second_start + row,
              f"line {line}: second-half sfb {sfb} where {second_start + row} was due")
        for column, value in enumerate(values):
            second[column].append(value)
    if halves == 2:
        check(second_start == len(rows),
              f"Table B.{number}: the second half starts at sfb {second_start}, but the first "
              f"half has {len(rows)} rows")
        columns = [first + more for first, more in zip(columns, second, strict=True)]

    columns = [[number_value(v) if v != "-" else "-" for v in column] for column in columns]

    def take(column, target_length, num_sfb_here, rate):
        count = num_sfb_here[target_length] + 1
        values = column[:count]
        check(len(values) == count and "-" not in values,
              f"Table B.{number}, {target_length}@{rate}: {len(values)} offsets before a '-' or "
              f"the end, num_sfb {num_sfb_here[target_length]} needs {count}")
        check(values[0] == 0, f"Table B.{number}, {target_length}@{rate}: sfb 0 is at {values[0]}")
        for sfb, (a, b) in enumerate(itertools.pairwise(values)):
            check(a < b, f"Table B.{number}, {target_length}@{rate}: sfb {sfb + 1} at {b} is "
                  f"not after {a}")
        check(values[-1] == target_length,
              f"Table B.{number}, {target_length}@{rate}: sfb {count - 1} ends at {values[-1]}, "
              f"not {target_length}")
        return values

    offsets_48, offsets_96, offsets_192 = {}, {}, {}
    for column, length in enumerate(lengths):
        check(length in num_sfb, f"Table B.{number}: {length} is not in Table B.1")
        offsets_48[length] = take(columns[column], length, num_sfb, "48")
        length_96 = length * 2
        if length_96 in num_sfb_96:
            offsets_96[length_96] = take(columns[column], length_96, num_sfb_96, "96")
        length_192 = length * 4
        if length_192 in num_sfb_192:
            offsets_192[length_192] = take(columns[column], length_192, num_sfb_192, "192")
    return {48: offsets_48, 96: offsets_96, 192: offsets_192}


@dataclass
class HsfTables:
    """num_sfb/offsets/offset_tables for 96 and 192 kHz (Tables B.2 to B.7's
    96/192 kHz columns) - the HSF extension's own transform lengths, num_sfb_
    48's counterparts at LENGTHS_96 and LENGTHS_192 rather than LENGTHS_48."""
    num_sfb_96: dict
    offsets_96: dict
    offset_tables_96: dict
    num_sfb_192: dict
    offsets_192: dict
    offset_tables_192: dict


@dataclass
class Mapping:
    table: int
    master: int
    sides: list  # the n_sfb_side columns' transform lengths, left to right
    rows: list  # rows[max_sfb_master][column]


def parse_mapping(section, num_sfb, n_side_bits):
    number = section.number
    match = re.search(r"Mapping from max_sfb_master from transform length (" + NUMBER + r")\b",
                      section.title)
    check(match, f"Table B.{number}'s title names no master transform length")
    master = number_value(match.group(1))
    headings, rows = body_rows(section)
    check(len(headings) == 1, f"Table B.{number} has {len(headings)} heading lines")
    heading = re.fullmatch(r"max_sfb_master\[(" + NUMBER + r")\]((?:\s+n_sfb_side\[" + NUMBER
                           + r"\])+)", headings[0])
    check(heading, f"Table B.{number}: heading {headings[0]!r}")
    check(number_value(heading.group(1)) == master,
          f"Table B.{number}: the title's length {master}, the heading's {heading.group(1)}")
    sides = [number_value(v) for v in re.findall(r"n_sfb_side\[(" + NUMBER + r")\]",
                                                 heading.group(2))]
    for side in sides:
        check(side in num_sfb and side < master,
              f"Table B.{number}: a column for {side}, which is not a shorter Table B.1 length")
    check(len(set(sides)) == len(sides), f"Table B.{number}: columns {sides} repeat")

    values = []
    for row, (line, text) in enumerate(rows):
        fields = read_row(line, text, 1 + len(sides), index=row,
                          fits=lambda f: all(re.fullmatch(r"\d+", v) for v in f))
        values.append([int(v) for v in fields[1:]])
    check(len(values) == 1 << n_side_bits[master],
          f"Table B.{number}: {len(values)} rows, but n_side_bits {n_side_bits[master]} for "
          f"{master} can express {1 << n_side_bits[master]} max_sfb_master values")
    for column, side in enumerate(sides):
        down = [row[column] for row in values]
        for row, (a, b) in enumerate(itertools.pairwise(down)):
            check(a <= b, f"Table B.{number}, n_sfb_side[{side}]: row {row + 1} decreases")
        check(max(down) <= num_sfb[side],
              f"Table B.{number}, n_sfb_side[{side}]: {max(down)} exceeds num_sfb "
              f"{num_sfb[side]}")
    return Mapping(number, master, sides, values)


def parse_annex_b(numbered, n_side_bits):
    sections = table_sections(numbered, "B")
    check(sorted(sections) == list(range(1, 20)),
          f"Annex B has Tables B.{sorted(sections)}, not B.1 to B.19")
    num_sfb = parse_b1(sections[1])
    num_sfb_96 = parse_num_sfb(sections[2], LENGTHS_96)
    num_sfb_192 = parse_num_sfb(sections[3], LENGTHS_192)
    offsets, offsets_96, offsets_192 = {}, {}, {}
    offset_tables, offset_tables_96, offset_tables_192 = {}, {}, {}
    for number in range(4, 8):
        by_rate = parse_offsets(sections[number], num_sfb, num_sfb_96, num_sfb_192)
        for length, values in by_rate[48].items():
            check(length not in offsets, f"Table B.{number}: {length} appears in two tables")
            offsets[length], offset_tables[length] = values, number
        for length, values in by_rate[96].items():
            check(length not in offsets_96, f"Table B.{number}: {length}@96 appears in two tables")
            offsets_96[length], offset_tables_96[length] = values, number
        for length, values in by_rate[192].items():
            check(length not in offsets_192,
                  f"Table B.{number}: {length}@192 appears in two tables")
            offsets_192[length], offset_tables_192[length] = values, number
    check(sorted(offsets) == sorted(LENGTHS_48),
          f"Tables B.4 to B.7 give offsets for {sorted(offsets)}")
    check(sorted(offsets_96) == sorted(LENGTHS_96),
          f"Tables B.4 to B.7 give 96 kHz offsets for {sorted(offsets_96)}, "
          f"not {sorted(LENGTHS_96)}")
    check(sorted(offsets_192) == sorted(LENGTHS_192),
          f"Tables B.4 to B.7 give 192 kHz offsets for {sorted(offsets_192)}, "
          f"not {sorted(LENGTHS_192)}")
    mappings = [parse_mapping(sections[n], num_sfb, n_side_bits) for n in range(8, 20)]
    masters = [m.master for m in mappings]
    check(len(set(masters)) == len(masters), f"Tables B.8 to B.19 repeat a master: {masters}")
    hsf = HsfTables(num_sfb_96, offsets_96, offset_tables_96,
                    num_sfb_192, offsets_192, offset_tables_192)
    return num_sfb, offsets, offset_tables, mappings, hsf


# ---------------------------------------------------------------------------
# C++ output
# ---------------------------------------------------------------------------

def wrap(items, indent, limit=99):
    """`items` as lines of at most `limit` columns, a comma after every item."""
    lines, current = [], indent
    for item in items:
        piece = f"{item},"
        if current.strip() and len(current) + 1 + len(piece) > limit:
            lines.append(current)
            current = indent
        current = f"{current} {piece}" if current.strip() else f"{current}{piece}"
    if current.strip():
        lines.append(current)
    return lines


def spaced(value):
    """The spec's own way of printing a number, for comments: 2 048."""
    return f"{value:,}".replace(",", " ")


HEADER_BANNER_HUFFMAN = [
    "// Every Huffman codebook of ETSI TS 103 190-1 V1.4.1 Annex A, and the A-JCC and",
    "// A-JOC codebooks of ETSI TS 103 190-2 V1.3.1 Annex A.1. GENERATED by",
    "// tools/generators/gen_ac4_tables.py from the attachments ts_103190_tables.c and",
    "// ts_103190_tables_part2.c and the two annexes' text; do not edit by hand.",
]


def clause_heading(clause):
    """A clause's section comment: Part 1's by its number, Part 2's by its label."""
    label = OUTPUT_CLAUSES[clause]
    return f"// {label}." if clause in PART2_CLAUSES else f"// A.{clause}: {label}."


def emit_huffman_header(codebooks):
    spectrum = [cb for cb in codebooks if re.fullmatch(r"ASF_HCB_\d+", cb.name)]
    comments = {}
    for cb in codebooks:
        if cb.clause == 1 and cb not in spectrum:
            comments[cb.name] = f"// Table A.{cb.table}"
    comments[spectrum[0].name] = (f"// Tables A.{spectrum[0].table} to A.{spectrum[-1].table}: "
                                  f"the spectrum codebooks 1 to {len(spectrum)}")
    column = max(len(f"extern const Codebook {cb.cxx};") for cb in codebooks
                 if cb.name in comments) + 2

    out = ["#pragma once", "", "#include <array>", "", '#include "huffman_codebook.hpp"', "",
           *HEADER_BANNER_HUFFMAN, "", "namespace ac4::detail::tables {", ""]
    for clause in OUTPUT_CLAUSES:
        out.append(clause_heading(clause))
        for cb in codebooks:
            if cb.clause != clause:
                continue
            declaration = f"extern const Codebook {cb.cxx};"
            if cb.name in comments:
                declaration = f"{declaration:<{column}}{comments[cb.name]}"
            out.append(declaration)
        if clause == 1:
            out += [
                "",
                "// The spectrum codebooks by number (index 0 is unused), and Tables A.14 and",
                "// A.15 by the same number.",
                f"extern const std::array<const Codebook*, {SPECTRUM_CODEBOOKS + 1}> "
                "kAsfSpectrumCodebooks;",
                f"extern const std::array<int, {SPECTRUM_CODEBOOKS + 1}> kCbDim;",
                f"extern const std::array<bool, {SPECTRUM_CODEBOOKS + 1}> kUnsignedCb;",
            ]
        out.append("")
    out.append("}  // namespace ac4::detail::tables")
    return out


def emit_codes_header(codebooks):
    out = ["#pragma once", "", "#include <array>", "#include <span>", "",
           '#include "huffman_codebook.hpp"', "",
           "// Every Huffman codebook of ETSI TS 103 190-1 V1.4.1 Annex A, and the A-JCC",
           "// and A-JOC codebooks of ETSI TS 103 190-2 V1.3.1 Annex A.1, in index order,",
           "// for writing: the codeword and its length for each index huff_decode()",
           "// returns.",
           "// GENERATED by tools/generators/gen_ac4_tables.py with huffman_tables.hpp,",
           "// from the attachments ts_103190_tables.c and ts_103190_tables_part2.c; do",
           "// not edit by hand. The decoder reads through huffman_tables.hpp and does not",
           "// link these.",
           "", "namespace ac4::detail::tables {", ""]
    for clause in OUTPUT_CLAUSES:
        out.append(clause_heading(clause))
        for cb in codebooks:
            if cb.clause == clause:
                out.append(f"extern const std::array<HuffCode, {len(cb.lengths)}> {cb.cxx}Codes;")
        if clause == 1:
            out += [
                "",
                "// The spectrum codebooks by number, as kAsfSpectrumCodebooks has them;",
                "// index 0 is empty.",
                f"extern const std::array<std::span<const HuffCode>, {SPECTRUM_CODEBOOKS + 1}> "
                "kAsfSpectrumCodes;",
            ]
        out.append("")
    out.append("}  // namespace ac4::detail::tables")
    return out


def emit_codes_source(codebooks):
    out = ['#include "huffman_codes.hpp"', "", "namespace ac4::detail::tables {", ""]
    for cb in codebooks:
        digits = (max(cb.lengths) + 3) // 4
        out.append(f"// {cb.label}, {cb.name}.")
        out.append(f"constinit const std::array<HuffCode, {len(cb.lengths)}> {cb.cxx}Codes = {{{{")
        out += wrap([f"{{0x{code:0{digits}x}, {bits}}}"
                     for bits, code in zip(cb.lengths, cb.codewords, strict=True)], "    ")
        out.append("}};")
        out.append("")
    by_name = {cb.name: cb for cb in codebooks}
    spectrum = ["{}", *(f"{by_name[f'ASF_HCB_{n}'].cxx}Codes"
                        for n in range(1, SPECTRUM_CODEBOOKS + 1))]
    out += [
        f"constinit const std::array<std::span<const HuffCode>, {SPECTRUM_CODEBOOKS + 1}> "
        "kAsfSpectrumCodes = {{",
        *wrap(spectrum, "    "),
        "}};",
        "",
        "}  // namespace ac4::detail::tables",
    ]
    return out


def emit_huffman_source(codebooks, cb_dim, unsigned_cb):
    out = ['#include "huffman_tables.hpp"', "", "#include <array>", "",
           *HEADER_BANNER_HUFFMAN,
           "//",
           "// Each codebook's entries are sorted by length and then by codeword, with",
           "// the index huff_decode() returns - the entry's position in the attachment's",
           "// _LEN and _CW arrays - beside each one. length_start[L] is the first entry",
           "// of length L. Where Annex A prints no cb_off, cb_mod, cb_mod2 or cb_mod3 for",
           "// a codebook, the value is 0.",
           "", "namespace ac4::detail::tables {", "", "namespace {", ""]
    for cb in codebooks:
        entries = sorted((bits, code, index)
                         for index, (bits, code) in enumerate(zip(cb.lengths, cb.codewords,
                                                                  strict=True)))
        low, high = entries[0][0], entries[-1][0]
        kraft = "1" if cb.kraft == 1 else f"{cb.kraft.numerator}/{cb.kraft.denominator}"
        out.append(f"// {cb.label}, {cb.name}: {len(entries)} codewords of {low} to "
                   f"{high} bits, Kraft sum {kraft}.")
        out.append(f"constexpr std::array<HuffEntry, {len(entries)}> {cb.cxx}Entries = {{{{")
        digits = (high + 3) // 4
        for length, group in itertools.groupby(entries, key=lambda entry: entry[0]):
            out.append(f"    // {length} bit{'s' if length != 1 else ''}")
            out += wrap([f"{{0x{code:0{digits}x}, {index}, {bits}}}"
                         for bits, code, index in group], "    ")
        out.append("}};")
        out.append("")
    out += ["}  // namespace", ""]

    for cb in codebooks:
        starts = [sum(1 for bits in cb.lengths if bits < length) for length in range(MAX_BITS + 2)]
        out += [
            f"constinit const Codebook {cb.cxx}{{",
            f'    .name = "{cb.name}",',
            f"    .sorted = {cb.cxx}Entries,",
            "    .length_start = {{",
            *wrap(starts, "        "),
            "    }},",
            f"    .codebook_length = {cb.printed['codebook_length']},",
            f"    .max_bits = {max(cb.lengths)},",
            f"    .cb_off = {cb.value('cb_off')},",
            f"    .cb_mod = {cb.value('cb_mod')},",
            f"    .cb_mod2 = {cb.value('cb_mod2')},",
            f"    .cb_mod3 = {cb.value('cb_mod3')},",
            "};",
            "",
        ]

    by_name = {cb.name: cb for cb in codebooks}
    spectrum = ["nullptr", *(f"&{by_name[f'ASF_HCB_{n}'].cxx}"
                             for n in range(1, SPECTRUM_CODEBOOKS + 1))]
    size = SPECTRUM_CODEBOOKS + 1
    out += [
        f"constinit const std::array<const Codebook*, {size}> kAsfSpectrumCodebooks = {{{{",
        *wrap(spectrum, "    "),
        "}};",
        "",
        "// Table A.14, by codebook number.",
        f"constinit const std::array<int, {size}> kCbDim = {{{{",
        *wrap([0, *cb_dim[1:]], "    "),
        "}};",
        "",
        "// Table A.15, by codebook number.",
        f"constinit const std::array<bool, {size}> kUnsignedCb = {{{{",
        *wrap(["false", *("true" if u else "false" for u in unsigned_cb[1:])], "    "),
        "}};",
        "",
        "}  // namespace ac4::detail::tables",
    ]
    return out


SFB_HEADER = [
    "#pragma once",
    "",
    "#include <cstdint>",
    "#include <span>",
    "",
    "// ETSI TS 103 190-1 V1.4.1 Annex B, the ASF scale factor band tables, at the",
    "// 44.1 kHz, 48 kHz, 96 kHz and 192 kHz sampling frequencies (the last two for",
    "// the HSF extension, ac4_hsf_ext_substream() - see Sec.4.2.4.3). GENERATED by",
    "// tools/generators/gen_ac4_tables.py from Annex B's text; do not edit by hand.",
    "",
    "namespace ac4::detail::tables {",
    "",
    "// Table B.1: num_sfb_48(transform_length). 0 for a length the table does not",
    "// list.",
    "[[nodiscard]] int num_sfb_48(int transform_length) noexcept;",
    "",
    "// Tables B.4 to B.7: the scale factor band offsets for a transform length,",
    "// num_sfb_48(transform_length) + 1 entries, the last equal to the transform",
    "// length. Empty for a length the tables do not list.",
    "[[nodiscard]] std::span<const std::uint16_t> sfb_offsets_48(int transform_length) noexcept;",
    "",
    "// Table B.2 and the 96 kHz columns of Tables B.4 to B.7: num_sfb_96() and",
    "// sfb_offsets_96(), the same shape as num_sfb_48()/sfb_offsets_48() but for",
    "// the HSF extension's own transform length (twice the owning channel's, Table",
    "// 17's max_sfb_ext_hsf loop). A length these do not list (0/empty) includes",
    "// every 44.1 kHz-only length: Table B.2 has no 44.1 kHz row.",
    "[[nodiscard]] int num_sfb_96(int transform_length) noexcept;",
    "[[nodiscard]] std::span<const std::uint16_t> sfb_offsets_96(int transform_length) noexcept;",
    "",
    "// Table B.3 and the 192 kHz columns of Tables B.4 to B.7: num_sfb_192() and",
    "// sfb_offsets_192(), four times the owning channel's transform length.",
    "[[nodiscard]] int num_sfb_192(int transform_length) noexcept;",
    "[[nodiscard]] std::span<const std::uint16_t> sfb_offsets_192(int transform_length) noexcept;",
    "",
    "// Tables B.8 to B.19, as clause 4.3.5.13 applies them: the max_sfb for a block",
    "// of transform length `target_length` in the two sf_data() elements that",
    "// follow a max_sfb_master element, where `master_length` is the largest",
    "// transform length signalled in the channel data before that element - the",
    "// length whose n_side_bits (Table 106) is max_sfb_master's width, per the",
    "// notes in clauses 4.2.6.6 and 4.2.6.14.",
    "//   - target_length == master_length: max_sfb_master itself, which \"maps",
    "//     directly\", for any max_sfb_master from 0 to num_sfb_48(master_length).",
    "//   - target_length shorter: the n_sfb_side value in row max_sfb_master of",
    "//     the table for master_length, in the column for target_length.",
    "// -1 for anything else: a length Table B.1 does not list, a target_length",
    "// longer than master_length or without a column in its table (another frame",
    "// length's family, or a master_length of 128, 120 or 96, which have no",
    "// table), or a max_sfb_master past the table's last row - which no value read",
    "// in n_side_bits bits reaches, every table having 2^n_side_bits rows.",
    "[[nodiscard]] int max_sfb_from_master(int master_length, int max_sfb_master,",
    "                                      int target_length) noexcept;",
    "",
    "}  // namespace ac4::detail::tables",
]


def emit_rate_offset_arrays(rate_label, lengths, num_sfb, offsets, offset_tables, prefix):
    """The kSfbOffset<prefix><length> arrays for one rate, then a
    kTransformLengths<PREFIX> array of {length, num_sfb, offsets} - PREFIX
    empty for 48 kHz (unprefixed, as before HSF), "96"/"192" otherwise."""
    out = []
    for length in lengths:
        values = offsets[length]
        out.append(f"// Table B.{offset_tables[length]}, the {spaced(length)}@{rate_label} "
                   f"column: sfb_offset for sfb 0 to {len(values) - 1}.")
        out.append(f"constexpr std::array<std::uint16_t, {len(values)}> "
                   f"kSfbOffset{prefix}{length} = {{{{")
        for first in range(0, len(values), 10):
            row = ", ".join(str(v) for v in values[first:first + 10]) + ","
            last = min(first + 10, len(values)) - 1
            span = f"sfb {first} to {last}" if last > first else f"sfb {first}"
            out.append(f"    {row:<60}// {span}")
        out.append("}};")
        out.append("")
    out += [
        f"constexpr std::array<TransformLength, {len(lengths)}> kTransformLengths{prefix} = {{{{",
        *(f"    {{{length}, {num_sfb[length]}, kSfbOffset{prefix}{length}}},"
          for length in lengths),
        "}};",
        "",
    ]
    return out


def emit_sfb_source(num_sfb, offsets, mappings, offset_tables, hsf):
    out = ['#include "sfb_tables.hpp"', "", "#include <array>", "#include <cstddef>",
           "#include <cstdint>", "#include <span>", "",
           "// GENERATED by tools/generators/gen_ac4_tables.py from ETSI TS 103 190-1 V1.4.1",
           "// Annex B's text; do not edit by hand.",
           "", "namespace ac4::detail::tables {", "", "namespace {", "",
           "struct TransformLength {",
           "    int transform_length;",
           "    int num_sfb;                             // Table B.1, B.2 or B.3",
           "    std::span<const std::uint16_t> offsets;  // num_sfb + 1 entries",
           "};",
           ""]
    out += emit_rate_offset_arrays("48", LENGTHS_48, num_sfb, offsets, offset_tables, "")
    out += emit_rate_offset_arrays("96", LENGTHS_96, hsf.num_sfb_96, hsf.offsets_96,
                                   hsf.offset_tables_96, "96")
    out += emit_rate_offset_arrays("192", LENGTHS_192, hsf.num_sfb_192, hsf.offsets_192,
                                   hsf.offset_tables_192, "192")

    most = max(len(m.sides) for m in mappings)
    for m in mappings:
        columns = ", ".join(f"[{spaced(side)}]" for side in m.sides)
        out.append(f"// Table B.{m.table}: max_sfb_master[{spaced(m.master)}], by row, to "
                   f"n_sfb_side{columns}.")
        out.append(f"constexpr std::array<std::uint8_t, {len(m.rows) * len(m.sides)}> "
                   f"kSfbSide{m.master} = {{{{")
        for row, values in enumerate(m.rows):
            text = ", ".join(f"{v:2d}" for v in values) + ","
            out.append(f"    {text:<20}// {row}")
        out.append("}};")
        out.append("")

    out += [
        "struct MasterTable {",
        "    int master_length;",
        f"    std::array<int, {most}> side_lengths;  // the n_sfb_side columns in order; "
        "0 past them",
        "    std::size_t columns;",
        "    std::span<const std::uint8_t> values;  // [max_sfb_master * columns + column]",
        "};",
        "",
        f"constexpr std::array<MasterTable, {len(mappings)}> kMasterTables = {{{{",
    ]
    for m in mappings:
        sides = ", ".join(str(s) for s in [*m.sides, *[0] * (most - len(m.sides))])
        out.append(f"    {{{m.master}, {{{{{sides}}}}}, {len(m.sides)}, kSfbSide{m.master}}},")
    out += [
        "}};",
        "",
        "const TransformLength* find_length(std::span<const TransformLength> table,",
        "                                   int transform_length) noexcept {",
        "    for (const TransformLength& entry : table) {",
        "        if (entry.transform_length == transform_length) {",
        "            return &entry;",
        "        }",
        "    }",
        "    return nullptr;",
        "}",
        "",
        "}  // namespace",
        "",
        "int num_sfb_48(int transform_length) noexcept {",
        "    const TransformLength* entry = find_length(kTransformLengths, transform_length);",
        "    return entry != nullptr ? entry->num_sfb : 0;",
        "}",
        "",
        "std::span<const std::uint16_t> sfb_offsets_48(int transform_length) noexcept {",
        "    const TransformLength* entry = find_length(kTransformLengths, transform_length);",
        "    return entry != nullptr ? entry->offsets : std::span<const std::uint16_t>{};",
        "}",
        "",
        "int num_sfb_96(int transform_length) noexcept {",
        "    const TransformLength* entry = find_length(kTransformLengths96, transform_length);",
        "    return entry != nullptr ? entry->num_sfb : 0;",
        "}",
        "",
        "std::span<const std::uint16_t> sfb_offsets_96(int transform_length) noexcept {",
        "    const TransformLength* entry = find_length(kTransformLengths96, transform_length);",
        "    return entry != nullptr ? entry->offsets : std::span<const std::uint16_t>{};",
        "}",
        "",
        "int num_sfb_192(int transform_length) noexcept {",
        "    const TransformLength* entry = find_length(kTransformLengths192, transform_length);",
        "    return entry != nullptr ? entry->num_sfb : 0;",
        "}",
        "",
        "std::span<const std::uint16_t> sfb_offsets_192(int transform_length) noexcept {",
        "    const TransformLength* entry = find_length(kTransformLengths192, transform_length);",
        "    return entry != nullptr ? entry->offsets : std::span<const std::uint16_t>{};",
        "}",
        "",
        "int max_sfb_from_master(int master_length, int max_sfb_master, "
        "int target_length) noexcept {",
        "    const int num_sfb = num_sfb_48(master_length);",
        "    if (num_sfb == 0 || max_sfb_master < 0) {",
        "        return -1;",
        "    }",
        "    if (target_length == master_length) {",
        "        return max_sfb_master <= num_sfb ? max_sfb_master : -1;",
        "    }",
        "    const auto row = static_cast<std::size_t>(max_sfb_master);",
        "    for (const MasterTable& table : kMasterTables) {",
        "        if (table.master_length != master_length) {",
        "            continue;",
        "        }",
        "        if (row >= table.values.size() / table.columns) {",
        "            return -1;",
        "        }",
        "        for (std::size_t column = 0; column < table.columns; ++column) {",
        "            if (table.side_lengths[column] == target_length) {",
        "                return table.values[row * table.columns + column];",
        "            }",
        "        }",
        "        return -1;",
        "    }",
        "    return -1;",
        "}",
        "",
        "}  // namespace ac4::detail::tables",
    ]
    return out


# ---------------------------------------------------------------------------

NOISE_HEADER = [
    "#pragma once",
    "",
    "#include <array>",
    "",
    "// ETSI TS 103 190-1 V1.4.1 Annex C.11, RANDOM_NOISE_TABLE. GENERATED by",
    "// tools/generators/gen_ac4_tables.py from the attachment ts_103190_tables.c;",
    "// do not edit by hand.",
    "",
    "namespace ac4::detail::tables {",
    "",
    "// What GetRandomNoiseValue() (Pseudocode 57) looks up, in the float32 the",
    "// attachment declares: the speech spectral frontend's noise, and the audio",
    "// spectral frontend's noise fill (clause 5.1.4.2).",
    f"extern const std::array<float, {NOISE_ENTRIES}> kRandomNoiseTable;",
    "",
    "}  // namespace ac4::detail::tables",
]


def emit_noise_source(literals):
    return ['#include "noise_tables.hpp"', "",
            "namespace ac4::detail::tables {", "",
            f"const std::array<float, {NOISE_ENTRIES}> kRandomNoiseTable = {{",
            *wrap(literals, "   "),
            "};", "",
            "}  // namespace ac4::detail::tables"]


QMF_HEADER = [
    "#pragma once",
    "",
    "#include <array>",
    "",
    "// ETSI TS 103 190-1 V1.4.1 Annex D.3, QWIN, and Annex D.2, ASPX_NOISE. GENERATED",
    "// by tools/generators/gen_ac4_tables.py from the attachment ts_103190_tables.c;",
    "// do not edit by hand.",
    "",
    "namespace ac4::detail::tables {",
    "",
    "// The window of the QMF analysis and synthesis banks (clauses 5.7.3 and",
    "// 5.7.4), in the float the attachment declares. It carries its own signs.",
    f"extern const std::array<float, {QWIN_ENTRIES}> kQwin;",
    "",
    "// NoiseTable of A-SPX's noise generator (clause 5.7.6.4.3): complex numbers",
    "// of random phase and mean energy 1, {real, imaginary}, in the float the",
    "// attachment declares.",
    f"extern const std::array<std::array<float, 2>, {ASPX_NOISE_ENTRIES}> kAspxNoise;",
    "",
    "}  // namespace ac4::detail::tables",
]


def emit_qmf_source(qwin, aspx_noise):
    noise = [f"{{{float_literal(re)}, {float_literal(im)}}}" for re, im in aspx_noise]
    return ['#include "qmf_tables.hpp"', "",
            "namespace ac4::detail::tables {", "",
            f"const std::array<float, {QWIN_ENTRIES}> kQwin = {{",
            *wrap([float_literal(text) for text in qwin], "   "),
            "};", "",
            f"const std::array<std::array<float, 2>, {ASPX_NOISE_ENTRIES}> kAspxNoise = {{{{",
            *wrap(noise, "   "),
            "}};", "",
            "}  // namespace ac4::detail::tables"]


ISF_HEADER = [
    "#pragma once",
    "",
    "#include <array>",
    "#include <span>",
    "",
    "// ETSI TS 103 190-2 V1.3.1 Annex A.2.1, the intermediate spatial format's",
    "// rendering matrices. GENERATED by tools/generators/gen_ac4_tables.py from the",
    "// attachment ts_103190_tables_part2.c; do not edit by hand.",
    "",
    "namespace ac4::detail::tables {",
    "",
    "// Part 2 Table 61's stacked ring formats by isf_config, SR3.1.0.0 to",
    "// SR15.9.5.1, and each one's ISF channels.",
    f"inline constexpr std::array<int, {len(ISF_CONFIGS)}> kIsfChannels = "
    "{4, 8, 10, 14, 15, 30};",
    "",
    "// The output layouts of Tables A.25 and A.26 in the order kIsfMatrices holds",
    "// them - 2.x, 5.x, 7.x, 9.x, 5.x.2, 5.x.4, 7.x.2, 7.x.4, 9.x.2 and 9.x.4 -",
    "// and each one's loudspeakers.",
    f"inline constexpr std::array<int, {len(ISF_LAYOUTS)}> kIsfOutputs = "
    "{2, 5, 7, 9, 7, 9, 9, 11, 11, 13};",
    "",
    "// SR<config>_to_<layout>, [isf_config][layout]: kIsfChannels rows, one per ISF",
    "// channel in the order t = [M1..., U1..., L1..., Z] takes them (clause",
    "// 5.10.3.4), of kIsfOutputs coefficients each, in the float the attachment",
    "// declares.",
    "extern const std::array<std::array<std::span<const float>, "
    f"{len(ISF_LAYOUTS)}>, {len(ISF_CONFIGS)}>",
    "    kIsfMatrices;",
    "",
    "}  // namespace ac4::detail::tables",
]


def emit_isf_source(matrices):
    out = ['#include "isf_tables.hpp"', "", "namespace ac4::detail::tables {", "namespace {", ""]
    for config in ISF_CONFIGS:
        for layout in ISF_LAYOUTS:
            rows = matrices[config, layout]
            values = [text for row in rows for text in row]
            out.append(f"constexpr std::array<float, {len(values)}> kSR{config}To{layout} = {{")
            out.extend(wrap(values, "   "))
            out.append("};")
            out.append("")
    out.append("}  // namespace")
    out.append("")
    out.append("constinit const std::array<std::array<std::span<const float>, "
               f"{len(ISF_LAYOUTS)}>, {len(ISF_CONFIGS)}>")
    out.append("    kIsfMatrices = {{")
    for config in ISF_CONFIGS:
        names = [f"kSR{config}To{layout}" for layout in ISF_LAYOUTS]
        out.append("        {{")
        out.extend(wrap(names, "           "))
        out.append("        }},")
    out.append("    }};")
    out.append("")
    out.append("}  // namespace ac4::detail::tables")
    return out


def report_codebooks(codebooks):
    print(f"{'codebook':<28} {'table':>6} {'entries':>7} {'bits':>6}  Kraft sum")
    for cb in codebooks:
        kraft = "1" if cb.kraft == 1 else f"{cb.kraft} = {float(cb.kraft):.9f}"
        bits = f"{min(cb.lengths)}-{max(cb.lengths)}"
        table = f"{'' if cb.part == 1 else 'P2 '}A.{cb.table}"
        print(f"{cb.name:<28} {table:>9} {len(cb.lengths):>7} {bits:>6}  {kraft}")
    incomplete = [cb for cb in codebooks if cb.kraft != 1]
    if incomplete:
        print(f"\n{len(incomplete)} codebook(s) are not complete codes (Kraft sum below 1):")
        for cb in incomplete:
            print(f"  {cb.name} ({cb.label}): {cb.kraft} = {float(cb.kraft):.9f}")
    else:
        print("\nevery codebook is a complete code")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--spec-dir", type=Path, default=REPO / "spec",
                        help="directory holding ts_10319001v010401p.txt, "
                             "ts_10319002v010301p.txt and their attachments' directories "
                             "(default: REPO/spec)")
    args = parser.parse_args()
    spec_txt, tables_c = args.spec_dir / SPEC_TXT, args.spec_dir / TABLES_C
    spec2_txt, tables2_c = args.spec_dir / SPEC2_TXT, args.spec_dir / TABLES2_C
    for path in (spec_txt, tables_c, spec2_txt, tables2_c):
        check(path.is_file(), f"{path} does not exist")

    lines = spec_txt.read_text(encoding="utf-8").splitlines()
    codebooks, cb_dim, unsigned_cb = parse_annex_a(annex(lines, "A", "B"))
    attach_codes(codebooks, parse_attachment(tables_c))
    check_spectrum_values(codebooks, cb_dim, unsigned_cb)
    # Part 2's A-JCC and A-JOC codebooks, from its attachment's AJCC and AJOC
    # arrays.
    lines2 = spec2_txt.read_text(encoding="utf-8").splitlines()
    ajcc = parse_ajcc_annex(ajcc_section(lines2))
    check_ajcc_offsets(ajcc)
    ajoc = parse_ajoc_annex(ajoc_section(lines2))
    check_ajoc_offsets(ajoc)
    attachment2 = parse_attachment(tables2_c)
    attach_codes(ajcc, {key: values for key, values in attachment2.items()
                        if key[0].startswith("AJCC_")})
    attach_codes(ajoc, {key: values for key, values in attachment2.items()
                        if key[0].startswith("AJOC_")})
    part2 = ajcc + ajoc
    check(not {cb.cxx for cb in part2} & {cb.cxx for cb in codebooks},
          "a Part 2 codebook has a Part 1 codebook's name")
    codebooks = codebooks + part2
    n_side_bits = parse_n_side_bits(lines)
    num_sfb, offsets, offset_tables, mappings, hsf = parse_annex_b(annex(lines, "B", "C"),
                                                                   n_side_bits)

    noise = parse_noise_table(tables_c)
    qwin = parse_qwin(tables_c)
    aspx_noise = parse_aspx_noise(tables_c)
    isf = parse_isf(tables2_c)

    report_codebooks(codebooks)
    mean_square = sum(Fraction(text[:-1]) ** 2 for text in noise) / len(noise)
    print(f"\nRANDOM_NOISE_TABLE: {len(noise)} entries, mean 0, mean square "
          f"{float(mean_square):.6f}")
    energy = sum(Fraction(re) ** 2 + Fraction(im) ** 2 for re, im in aspx_noise) / len(aspx_noise)
    print(f"ASPX_NOISE: {len(aspx_noise)} entries, mean energy {float(energy):.9f}")
    print(f"QWIN: {len(qwin)} entries, |QWIN[n]| == |QWIN[640 - n]|, "
          f"{sum(1 for text in qwin if text.startswith('-'))} negative")
    print(f"ISF: {len(isf)} rendering matrices, {len(ISF_CONFIGS)} formats to "
          f"{len(ISF_LAYOUTS)} layouts")
    print(f"\nAnnex B: num_sfb and offsets for {len(offsets)} transform lengths at 48 kHz, "
          f"{len(hsf.offsets_96)} at 96 kHz, {len(hsf.offsets_192)} at 192 kHz, "
          f"{len(mappings)} max_sfb_master tables")

    outputs = {
        "huffman_tables.hpp": emit_huffman_header(codebooks),
        "huffman_tables.cpp": emit_huffman_source(codebooks, cb_dim, unsigned_cb),
        "huffman_codes.hpp": emit_codes_header(codebooks),
        "huffman_codes.cpp": emit_codes_source(codebooks),
        "sfb_tables.hpp": SFB_HEADER,
        "sfb_tables.cpp": emit_sfb_source(num_sfb, offsets, mappings, offset_tables, hsf),
        "noise_tables.hpp": NOISE_HEADER,
        "noise_tables.cpp": emit_noise_source(noise),
        "qmf_tables.hpp": QMF_HEADER,
        "qmf_tables.cpp": emit_qmf_source(qwin, aspx_noise),
        "isf_tables.hpp": ISF_HEADER,
        "isf_tables.cpp": emit_isf_source(isf),
    }
    for name, out in outputs.items():
        for number, text in enumerate(out, start=1):
            check(len(text) < 100, f"{name}:{number} is {len(text)} columns: {text!r}")
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    for name, out in outputs.items():
        path = OUT_DIR / name
        path.write_text("\n".join(out) + "\n", encoding="utf-8", newline="\n")
        shown = path.relative_to(REPO) if path.is_relative_to(REPO) else path
        print(f"wrote {shown} ({len(out)} lines)")


if __name__ == "__main__":
    main()
