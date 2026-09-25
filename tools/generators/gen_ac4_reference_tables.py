"""Generate tools/references/ac4_tables.py from the ETSI TS 103 190-1 V1.4.1 texts.

The tables of the Python AC-4 syntax transcription (tools/references/ac4_syntax.py).
tools/generators/gen_ac4_tables.py generates the C++ decoder's tables from
the same sources; the two generators were written separately, so that a table
misread in one transcription shows as a trace difference against the other.

Sources:
  * Annex A Huffman LEN/CW arrays: the table attachment ts_103190_tables.c
    (identical to the ts_10319001v010401p0.zip that accompanies the spec).
  * Annex A codebook parameters (codebook_length, cb_off, cb_mod, cb_mod2,
    cb_mod3) and Tables A.14/A.15: transcribed by hand from the Annex A text
    below (CODEBOOK_PARAMS, CB_DIM, UNSIGNED_CB), not taken from the .c file.
  * Annex B Tables B.1-B.7 (44.1/48, 96 and 192 kHz - the last two for the
    HSF extension, ac4_hsf_ext_substream()) and B.8-B.19: parsed from the
    spec text file, then checked (every offset column rises from 0 to its
    transform length in num_sfb + 1 steps, multiples of 4).
  * The small clause 4/5 tables the syntax needs (Tables 83, 100, 103, 106,
    109, 110, 143, 163, 169, 171, 192, 194, 197 and the A-SPX template
    tables of clause 5.7.6.3.1.1): transcribed by hand below.
  * ETSI TS 103 190-2 V1.3.1's A-JCC codebooks (Annex A.1.2, Tables A.13 to
    A.24): the LEN/CW arrays from its attachment ts_103190_tables_part2.c,
    codebook_length and cb_off transcribed by hand from the text below
    (AJCC_CODEBOOK_PARAMS); and Part 2 Table 83, ajcc_num_bands_table.

Run from the repo root:
    python tools/generators/gen_ac4_reference_tables.py [--spec-dir DIR]

--spec-dir (default spec/ in the repo root) holds ts_10319001v010401p.txt,
ts_10319001_attach/ts_103190_tables.c and
ts_10319002_attach/ts_103190_tables_part2.c. Writes
tools/references/ac4_tables.py and prints the checks, including every
codebook's Kraft sum.
"""

import argparse
import pprint
import re
from fractions import Fraction
from itertools import pairwise
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
SPEC_TXT = Path('ts_10319001v010401p.txt')
TABLES_C = Path('ts_10319001_attach') / 'ts_103190_tables.c'
TABLES2_C = Path('ts_10319002_attach') / 'ts_103190_tables_part2.c'
OUT = REPO / 'tools' / 'references' / 'ac4_tables.py'

# ---------------------------------------------------------------------------
# Annex A codebook parameters, transcribed from TS 103 190-1 Annex A
# (tables A.1 to A.13 and A.16 to A.62). None = not listed for the codebook.
# name: (codebook_length, cb_off, cb_mod, cb_mod2, cb_mod3)
# ---------------------------------------------------------------------------
CODEBOOK_PARAMS = {
    # A.1 ASF
    'ASF_HCB_SCALEFAC': (121, None, None, None, None),          # Table A.1
    'ASF_HCB_1': (81, 1, 3, 9, 27),                              # Table A.2
    'ASF_HCB_2': (81, 1, 3, 9, 27),                              # Table A.3
    'ASF_HCB_3': (81, 0, 3, 9, 27),                              # Table A.4
    'ASF_HCB_4': (81, 0, 3, 9, 27),                              # Table A.5
    'ASF_HCB_5': (81, 4, 9, None, None),                         # Table A.6
    'ASF_HCB_6': (81, 4, 9, None, None),                         # Table A.7
    'ASF_HCB_7': (64, 0, 8, None, None),                         # Table A.8
    'ASF_HCB_8': (64, 0, 8, None, None),                         # Table A.9
    'ASF_HCB_9': (169, 0, 13, None, None),                       # Table A.10
    'ASF_HCB_10': (169, 0, 13, None, None),                      # Table A.11
    'ASF_HCB_11': (289, 0, 17, None, None),                      # Table A.12
    'ASF_HCB_SNF': (22, None, None, None, None),                 # Table A.13
    # A.2 A-SPX
    'ASPX_HCB_ENV_LEVEL_15_F0': (71, None, None, None, None),    # Table A.16
    'ASPX_HCB_ENV_LEVEL_15_DF': (141, 70, None, None, None),     # Table A.17
    'ASPX_HCB_ENV_LEVEL_15_DT': (141, 70, None, None, None),     # Table A.18
    'ASPX_HCB_ENV_BALANCE_15_F0': (25, None, None, None, None),  # Table A.19
    'ASPX_HCB_ENV_BALANCE_15_DF': (49, 24, None, None, None),    # Table A.20
    'ASPX_HCB_ENV_BALANCE_15_DT': (49, 24, None, None, None),    # Table A.21
    'ASPX_HCB_ENV_LEVEL_30_F0': (36, None, None, None, None),    # Table A.22
    'ASPX_HCB_ENV_LEVEL_30_DF': (71, 35, None, None, None),      # Table A.23
    'ASPX_HCB_ENV_LEVEL_30_DT': (71, 35, None, None, None),      # Table A.24
    'ASPX_HCB_ENV_BALANCE_30_F0': (13, None, None, None, None),  # Table A.25
    'ASPX_HCB_ENV_BALANCE_30_DF': (25, 12, None, None, None),    # Table A.26
    'ASPX_HCB_ENV_BALANCE_30_DT': (25, 12, None, None, None),    # Table A.27
    'ASPX_HCB_NOISE_LEVEL_F0': (30, None, None, None, None),     # Table A.28
    'ASPX_HCB_NOISE_LEVEL_DF': (59, 29, None, None, None),       # Table A.29
    'ASPX_HCB_NOISE_LEVEL_DT': (59, 29, None, None, None),       # Table A.30
    'ASPX_HCB_NOISE_BALANCE_F0': (13, None, None, None, None),   # Table A.31
    'ASPX_HCB_NOISE_BALANCE_DF': (25, 12, None, None, None),     # Table A.32
    'ASPX_HCB_NOISE_BALANCE_DT': (25, 12, None, None, None),     # Table A.33
    # A.3 A-CPL
    'ACPL_HCB_ALPHA_COARSE_F0': (17, 0, None, None, None),       # Table A.34
    'ACPL_HCB_ALPHA_FINE_F0': (33, 0, None, None, None),         # Table A.35
    'ACPL_HCB_ALPHA_COARSE_DF': (33, 16, None, None, None),      # Table A.36
    'ACPL_HCB_ALPHA_FINE_DF': (65, 32, None, None, None),        # Table A.37
    'ACPL_HCB_ALPHA_COARSE_DT': (33, 16, None, None, None),      # Table A.38
    'ACPL_HCB_ALPHA_FINE_DT': (65, 32, None, None, None),        # Table A.39
    'ACPL_HCB_BETA_COARSE_F0': (5, 0, None, None, None),         # Table A.40
    'ACPL_HCB_BETA_FINE_F0': (9, 0, None, None, None),           # Table A.41
    'ACPL_HCB_BETA_COARSE_DF': (9, 4, None, None, None),         # Table A.42
    'ACPL_HCB_BETA_FINE_DF': (17, 8, None, None, None),          # Table A.43
    'ACPL_HCB_BETA_COARSE_DT': (9, 4, None, None, None),         # Table A.44
    'ACPL_HCB_BETA_FINE_DT': (17, 8, None, None, None),          # Table A.45
    'ACPL_HCB_BETA3_COARSE_F0': (9, 0, None, None, None),        # Table A.46
    'ACPL_HCB_BETA3_FINE_F0': (17, 0, None, None, None),         # Table A.47
    'ACPL_HCB_BETA3_COARSE_DF': (17, 8, None, None, None),       # Table A.48
    'ACPL_HCB_BETA3_FINE_DF': (33, 16, None, None, None),        # Table A.49
    'ACPL_HCB_BETA3_COARSE_DT': (17, 8, None, None, None),       # Table A.50
    'ACPL_HCB_BETA3_FINE_DT': (33, 16, None, None, None),        # Table A.51
    'ACPL_HCB_GAMMA_COARSE_F0': (21, 10, None, None, None),      # Table A.52
    'ACPL_HCB_GAMMA_FINE_F0': (41, 20, None, None, None),        # Table A.53
    'ACPL_HCB_GAMMA_COARSE_DF': (41, 20, None, None, None),      # Table A.54
    'ACPL_HCB_GAMMA_FINE_DF': (81, 40, None, None, None),        # Table A.55
    'ACPL_HCB_GAMMA_COARSE_DT': (41, 20, None, None, None),      # Table A.56
    'ACPL_HCB_GAMMA_FINE_DT': (81, 40, None, None, None),        # Table A.57
    # A.4 dialogue enhancement
    'DE_HCB_ABS_0': (32, 0, None, None, None),                   # Table A.58
    'DE_HCB_DIFF_0': (63, 31, None, None, None),                 # Table A.59
    'DE_HCB_ABS_1': (61, 30, None, None, None),                  # Table A.60
    'DE_HCB_DIFF_1': (121, 60, None, None, None),                # Table A.61
    # A.5 DRC
    'DRC_HCB': (255, 127, None, None, None),                     # Table A.62
}

# ---------------------------------------------------------------------------
# ETSI TS 103 190-2 V1.3.1 Annex A.1.2, the A-JCC codebooks, transcribed from
# the text (Tables A.13 to A.24). name: (codebook_length, cb_off)
# ---------------------------------------------------------------------------
AJCC_CODEBOOK_PARAMS = {
    'AJCC_HCB_DRY_COARSE_F0': (12, 0),                            # Part 2 Table A.13
    'AJCC_HCB_DRY_FINE_F0': (23, 0),                              # Part 2 Table A.14
    'AJCC_HCB_DRY_COARSE_DF': (23, 11),                           # Part 2 Table A.15
    'AJCC_HCB_DRY_FINE_DF': (45, 22),                             # Part 2 Table A.16
    'AJCC_HCB_DRY_COARSE_DT': (23, 11),                           # Part 2 Table A.17
    'AJCC_HCB_DRY_FINE_DT': (45, 22),                             # Part 2 Table A.18
    'AJCC_HCB_WET_COARSE_F0': (21, 0),                            # Part 2 Table A.19
    'AJCC_HCB_WET_FINE_F0': (41, 0),                              # Part 2 Table A.20
    'AJCC_HCB_WET_COARSE_DF': (41, 20),                           # Part 2 Table A.21
    'AJCC_HCB_WET_FINE_DF': (81, 40),                             # Part 2 Table A.22
    'AJCC_HCB_WET_COARSE_DT': (41, 20),                           # Part 2 Table A.23
    'AJCC_HCB_WET_FINE_DT': (81, 40),                             # Part 2 Table A.24
}

# Part 2 Table 83: ajcc_num_param_bands_id -> ajcc_num_bands_table.
AJCC_NUM_BANDS = {0: 15, 1: 12, 2: 9, 3: 7}

# Table A.14 / Table A.15, codebook numbers 1..11.
CB_DIM = {1: 4, 2: 4, 3: 4, 4: 4, 5: 2, 6: 2, 7: 2, 8: 2, 9: 2, 10: 2, 11: 2}
UNSIGNED_CB = {1: False, 2: False, 3: True, 4: True, 5: False, 6: False,
               7: True, 8: True, 9: True, 10: True, 11: True}


def parse_attachment(path):
    text = path.read_text(encoding='latin-1')
    start = text.index('/* Annex A */')
    stop = text.index('/* Annex B */')
    text = text[start:stop]
    arrays = {}
    pattern = r'const\s+int(?:32)?\s+(\w+)_(LEN|CW)\[(\d+)\]\s*=\s*\{([^}]*)\};'
    for m in re.finditer(pattern, text):
        name, kind, n, body = m.group(1), m.group(2), int(m.group(3)), m.group(4)
        vals = [int(tok, 0) for tok in re.findall(r'0x[0-9a-fA-F]+|\d+', body)]
        if len(vals) != n:
            raise SystemExit(f'{name}_{kind}: declared {n}, found {len(vals)} values')
        arrays.setdefault(name, {})[kind] = vals
    return arrays


def parse_ajcc_attachment(path):
    """The AJCC_HCB_* arrays of Part 2's attachment: its 'Annex A.2' section (the
    text's A.1.2) holds them, after the A-JOC codebooks, which are not read."""
    text = path.read_text(encoding='latin-1')
    marker = '/* Annex A.2 A-JCC Huffman codebook tables */'
    if text.count(marker) != 1:
        raise SystemExit(f'{path}: {marker!r} found {text.count(marker)} times')
    text = text[text.index(marker):]
    arrays = {}
    pattern = r'const\s+int(?:32)?\s+(AJCC_\w+)_(LEN|CW)\[(\d+)\]\s*=\s*\{([^}]*)\};'
    for m in re.finditer(pattern, text):
        name, kind, n, body = m.group(1), m.group(2), int(m.group(3)), m.group(4)
        vals = [int(tok, 0) for tok in re.findall(r'0x[0-9a-fA-F]+|\d+', body)]
        if len(vals) != n:
            raise SystemExit(f'{name}_{kind}: declared {n}, found {len(vals)} values')
        if kind in arrays.get(name, {}):
            raise SystemExit(f'{name}_{kind} is defined twice')
        arrays.setdefault(name, {})[kind] = vals
    return arrays


def check_codebook(lens, cws):
    """Prefix-free check and Kraft sum (as an exact fraction)."""
    kraft = sum(Fraction(1, 1 << length) for length in lens)
    problems = []
    seen = {}
    for i, (length, cw) in enumerate(zip(lens, cws, strict=True)):
        if cw >= (1 << length):
            problems.append(f'index {i}: codeword {cw:#x} does not fit in {length} bits')
        if (length, cw) in seen:
            problems.append(f'index {i} duplicates index {seen[(length, cw)]}')
        seen[(length, cw)] = i
    # Each codeword covers an interval of maxlen-bit strings; a prefix-free code
    # has pairwise disjoint intervals, so sorted neighbours must not overlap.
    maxlen = max(lens)
    intervals = sorted((cw << (maxlen - length), (cw + 1) << (maxlen - length), i)
                       for i, (length, cw) in enumerate(zip(lens, cws, strict=True)))
    for (_, a1, ia), (b0, _, ib) in pairwise(intervals):
        if b0 < a1:
            problems.append(f'index {ia} is a prefix of (or equals) index {ib}')
    return kraft, problems


# ---------------------------------------------------------------------------
# Annex B parsing from the text file
# ---------------------------------------------------------------------------

def find_line(lines, needle, start=0):
    for i in range(start, len(lines)):
        if needle in lines[i]:
            return i
    raise SystemExit(f'not found: {needle}')


def merge_thousands(tokens):
    """The text writes 1 600 for 1600: a 1-digit token followed by a 3-digit
    token is one number (no row places a 1-digit value next to a 3-digit
    value in these tables)."""
    out = []
    i = 0
    while i < len(tokens):
        t = tokens[i]
        if (t.isdigit() and len(t) == 1 and i + 1 < len(tokens) and tokens[i + 1].isdigit()
                and len(tokens[i + 1]) == 3):
            out.append(t + tokens[i + 1])
            i += 2
        else:
            out.append(t)
            i += 1
    return out


def is_page_noise(line):
    s = line.strip()
    return (not s) or s == 'ETSI' or 'ETSI TS 103 190-1' in s or '@' in s


def parse_num_sfb_table(lines, title, next_title):
    i = find_line(lines, title)
    j = find_line(lines, next_title, i)
    table = {}
    for line in lines[i + 1:j]:
        toks = merge_thousands(line.split())
        if len(toks) == 2 and toks[0].isdigit() and toks[1].isdigit():
            table[int(toks[0])] = int(toks[1])
    return table


def parse_b1(lines):
    return parse_num_sfb_table(
        lines, 'Table B.1: Number of scale factor bands for 44,1 kHz or 48 kHz', 'Table B.2:')


# ERRATA.md's "Misprints with no effect": Table B.2 (96 kHz) prints a
# transform length of 920 where Table 83's frame_len_base doubling for 96 kHz
# and the 96 kHz columns of Tables B.4-B.7 (whose own data this length's
# num_sfb has to agree with) both have 960. Corrected here, independently of
# gen_ac4_tables.py's own MISPRINTS dict for the same reading.
B2_MISPRINTS = {920: 960}


def parse_b2(lines):
    table = parse_num_sfb_table(
        lines, 'Table B.2: Number of scale factor bands for 96 kHz', 'Table B.3:')
    return {B2_MISPRINTS.get(tl, tl): n for tl, n in table.items()}


def parse_b3(lines):
    return parse_num_sfb_table(
        lines, 'Table B.3: Number of scale factor bands for 192 kHz', 'Table B.4:')


def parse_split_offset_table(lines, title, next_title, columns):
    """Tables B.4-B.6: two halves side by side, separated by '-'.
    Each half: sfb, then len(columns) values ('-' = absent)."""
    i = find_line(lines, title)
    j = find_line(lines, next_title, i)
    cols = {c: {} for c in columns}
    for line in lines[i + 1:j]:
        if is_page_noise(line):
            continue
        toks = merge_thousands(line.split())
        if not toks or not toks[0].isdigit() or '-' not in toks:
            continue
        sep = toks.index('-')
        for half in (toks[:sep], toks[sep + 1:]):
            if not half or half[0] == '-':
                continue
            if len(half) != 1 + len(columns):
                raise SystemExit(f'{title}: cannot parse row {line!r} -> {half}')
            sfb = int(half[0])
            for c, v in zip(columns, half[1:], strict=True):
                if v != '-':
                    if sfb in cols[c]:
                        raise SystemExit(f'{title}: duplicate sfb {sfb} for {c}')
                    cols[c][sfb] = int(v)
    return {c: [d[k] for k in sorted(d)] for c, d in cols.items()}


def parse_single_offset_table(lines, title, next_title, columns):
    """Table B.7: one block, sfb then len(columns) values."""
    i = find_line(lines, title)
    j = find_line(lines, next_title, i)
    cols = {c: {} for c in columns}
    for line in lines[i + 1:j]:
        if is_page_noise(line):
            continue
        toks = merge_thousands(line.split())
        if not toks or not toks[0].isdigit() or len(toks) != 1 + len(columns):
            continue
        if not all(t.isdigit() or t == '-' for t in toks):
            continue
        sfb = int(toks[0])
        for c, v in zip(columns, toks[1:], strict=True):
            if v != '-':
                cols[c][sfb] = int(v)
    return {c: [d[k] for k in sorted(d)] for c, d in cols.items()}


def parse_master_table(lines, title, next_title, targets):
    i = find_line(lines, title)
    j = find_line(lines, next_title, i)
    rows = {}
    for line in lines[i + 1:j]:
        if is_page_noise(line):
            continue
        toks = line.split()
        if not toks or not all(t.isdigit() for t in toks) or len(toks) != 1 + len(targets):
            continue
        rows[int(toks[0])] = [int(t) for t in toks[1:]]
    n = max(rows) + 1
    if sorted(rows) != list(range(n)):
        raise SystemExit(f'{title}: rows not contiguous: {sorted(rows)}')
    return {t: [rows[m][k] for m in range(n)] for k, t in enumerate(targets)}


# ---------------------------------------------------------------------------
# Hand-transcribed clause 4/5 tables
# ---------------------------------------------------------------------------

# Table 83: frame_len_base for frame_rate_index 0..13 (48 kHz family). Table 84
# (44.1 kHz) only defines index 13, also 2 048.
FRAME_LEN_BASE = {0: 1920, 1: 1920, 2: 2048, 3: 1536, 4: 1536, 5: 960, 6: 960, 7: 1024,
                  8: 768, 9: 768, 10: 512, 11: 384, 12: 384, 13: 2048}
# Table 100: transform length for partial blocks, frame_len_base >= 1 536, 44.1/48 kHz,
# keyed by frame_length, indexed by transf_length[i].
TRANSF_LENGTH_LONG_BASE = {2048: (128, 256, 512, 1024), 1920: (120, 240, 480, 960),
                           1536: (96, 192, 384, 768)}
# Table 103: frame_len_base < 1 536, 44.1/48 kHz; None = the 'x' entries.
TRANSF_LENGTH_SHORT_BASE = {1024: (128, 256, 512, 1024), 960: (120, 240, 480, 960),
                            768: (96, 192, 384, 768), 512: (128, 256, 512, None),
                            384: (96, 192, 384, None)}
# Table 106: transform length -> (n_msfb_bits, n_side_bits, n_msfbl_bits or None)
N_MSFB_BITS_48 = {2048: (6, 5, 3), 1920: (6, 5, 3), 1536: (6, 5, 3), 1024: (6, 5, 2),
                  960: (6, 5, 2), 768: (6, 5, 2), 512: (6, 5, 2), 480: (6, 5, None),
                  384: (6, 4, 2), 256: (5, 4, None), 240: (5, 4, None), 192: (5, 3, None),
                  128: (4, 3, None), 120: (4, 3, None), 96: (4, 3, None)}
# Table 109: (transf_length[0], transf_length[1]) -> n_grp_bits, frame_len_base >= 1 536
N_GRP_BITS_LONG_BASE = {(0, 0): 15, (0, 1): 10, (0, 2): 8, (0, 3): 7,
                        (1, 0): 10, (1, 1): 7, (1, 2): 4, (1, 3): 3,
                        (2, 0): 8, (2, 1): 4, (2, 2): 3, (2, 3): 1,
                        (3, 0): 7, (3, 1): 3, (3, 2): 1, (3, 3): 1}
# Table 110: frame_len_base -> n_grp_bits by transf_length (None = not allowed)
N_GRP_BITS_SHORT_BASE = {1024: (7, 3, 1, 0), 960: (7, 3, 1, 0), 768: (7, 3, 1, 0),
                         512: (3, 1, 0, None), 384: (3, 1, 0, None)}
# Table 143
ACPL_NUM_PARAM_BANDS = {0: 15, 1: 12, 2: 9, 3: 7}
# Table 197: QMF subband (0..63) -> parameter band, per acpl_num_param_bands
_SB_GROUPS = [(0, 0), (1, 1), (2, 2), (3, 3), (4, 4), (5, 5), (6, 6), (7, 7), (8, 8),
              (9, 10), (11, 13), (14, 17), (18, 22), (23, 34), (35, 63)]
_PB_COLUMNS = {15: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14],
               12: [0, 1, 2, 3, 4, 4, 5, 5, 6, 6, 7, 8, 9, 10, 11],
               9: [0, 1, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8],
               7: [0, 1, 2, 2, 3, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6]}
# Table 163: drc_gains_config -> nr_drc_bands
NR_DRC_BANDS = {0: 1, 1: 1, 2: 2, 3: 4}
# Table 169: frame length -> nr_drc_subframes
NR_DRC_SUBFRAMES = {384: 1, 512: 2, 768: 3, 960: 3, 1024: 4, 1536: 6, 1920: 6, 2048: 8}
# Table 171: de_channel_config -> de_nr_channels
DE_NR_CHANNELS = (0, 1, 1, 2, 1, 2, 2, 3)
# Clause 5.7.6.3.1.1 template subband group tables
SBG_TEMPLATE_LOWRES = (10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 22, 24, 26, 28, 30, 32, 35,
                       38, 42, 46)
SBG_TEMPLATE_HIGHRES = (18, 19, 20, 21, 22, 23, 24, 26, 28, 30, 32, 34, 36, 38, 40, 42, 44, 47,
                        50, 53, 56, 59, 62)
# Table 192: frame_length -> num_ts_in_ats
NUM_TS_IN_ATS = {2048: 2, 1920: 2, 1536: 2, 1024: 1, 960: 1, 768: 1, 512: 1, 384: 1}
# Table 194: num_aspx_timeslots -> {aspx_num_[env|noise]: tab_border}
TAB_BORDER = {6: {1: (0, 6), 2: (0, 3, 6), 4: (0, 2, 3, 4, 6)},
              8: {1: (0, 8), 2: (0, 4, 8), 4: (0, 2, 4, 6, 8)},
              12: {1: (0, 12), 2: (0, 6, 12), 4: (0, 3, 6, 9, 12)},
              15: {1: (0, 15), 2: (0, 8, 15), 4: (0, 4, 8, 12, 15)},
              16: {1: (0, 16), 2: (0, 8, 16), 4: (0, 4, 8, 12, 16)}}


def sb_to_pb():
    table = {}
    for nb, col in _PB_COLUMNS.items():
        m = [None] * 64
        for (lo, hi), pb in zip(_SB_GROUPS, col, strict=True):
            for sb in range(lo, hi + 1):
                m[sb] = pb
        table[nb] = tuple(m)
    return table


def fmt_list(vals, per_line, indent):
    pad = ' ' * indent
    return '\n'.join(pad + ', '.join(str(v) for v in vals[k:k + per_line]) + ','
                     for k in range(0, len(vals), per_line))


def assign(name, value):
    text = pprint.pformat(value, width=96 - len(name), compact=True, sort_dicts=False)
    return f'{name} = ' + text.replace('\n', '\n' + ' ' * (len(name) + 3)) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--spec-dir', type=Path, default=REPO / 'spec',
                        help='directory holding the Part 1 text and its table attachment')
    args = parser.parse_args()
    spec = args.spec_dir / SPEC_TXT
    attach = args.spec_dir / TABLES_C
    report = []
    arrays = parse_attachment(attach)
    if set(arrays) != set(CODEBOOK_PARAMS):
        raise SystemExit(f'codebook set mismatch: attachment-only '
                         f'{set(arrays) - set(CODEBOOK_PARAMS)}, text-only '
                         f'{set(CODEBOOK_PARAMS) - set(arrays)}')
    kraft = {}
    for name, (n, _off, mod, mod2, mod3) in CODEBOOK_PARAMS.items():
        lens, cws = arrays[name]['LEN'], arrays[name]['CW']
        if len(lens) != n or len(cws) != n:
            raise SystemExit(f'{name}: Annex A codebook_length {n} but attachment has '
                             f'{len(lens)} LEN / {len(cws)} CW entries')
        k, problems = check_codebook(lens, cws)
        if problems:
            raise SystemExit(f'{name}: not prefix-free: {problems[:5]}')
        kraft[name] = k
        if mod3 is not None:
            if mod3 * 3 != n or mod2 * 3 != mod3 or mod * 3 != mod2:
                raise SystemExit(f'{name}: cb_mod chain inconsistent with length')
        elif mod is not None and mod * mod != n:
            raise SystemExit(f'{name}: cb_mod {mod}^2 != {n}')
        report.append(f'{name:28s} n={n:3d} maxlen={max(lens):2d} Kraft={k!s:>12s}'
                      f'{"" if k == 1 else "  (incomplete)"}')

    # Part 2's A-JCC codebooks: the text's codebook_length and cb_off against the
    # attachment's arrays, and cb_off 0 for F0 and the middle index for DF and DT.
    ajcc_arrays = parse_ajcc_attachment(args.spec_dir / TABLES2_C)
    if set(ajcc_arrays) != set(AJCC_CODEBOOK_PARAMS):
        raise SystemExit(f'A-JCC codebook set mismatch: attachment-only '
                         f'{set(ajcc_arrays) - set(AJCC_CODEBOOK_PARAMS)}, text-only '
                         f'{set(AJCC_CODEBOOK_PARAMS) - set(ajcc_arrays)}')
    for name, (n, off) in AJCC_CODEBOOK_PARAMS.items():
        lens, cws = ajcc_arrays[name].get('LEN'), ajcc_arrays[name].get('CW')
        if lens is None or cws is None or len(lens) != n or len(cws) != n:
            raise SystemExit(f'{name}: Part 2 Annex A.1.2 codebook_length {n}, attachment arrays '
                             f'{None if lens is None else len(lens)} LEN / '
                             f'{None if cws is None else len(cws)} CW')
        expected_off = 0 if name.endswith('_F0') else (n - 1) // 2
        if off != expected_off or (not name.endswith('_F0') and n % 2 == 0):
            raise SystemExit(f'{name}: cb_off {off} for {n} entries')
        k, problems = check_codebook(lens, cws)
        if problems:
            raise SystemExit(f'{name}: not prefix-free: {problems[:5]}')
        kraft[name] = k
        arrays[name] = ajcc_arrays[name]
        report.append(f'{name:28s} n={n:3d} maxlen={max(lens):2d} Kraft={k!s:>12s}'
                      f'{"" if k == 1 else "  (incomplete)"}')

    lines = spec.read_text(encoding='utf-8').splitlines()
    num_sfb = parse_b1(lines)
    lengths = [2048, 1920, 1536, 1024, 960, 768, 512, 480, 384, 256, 240, 192, 128, 120, 96]
    if sorted(num_sfb, reverse=True) != lengths:
        raise SystemExit(f'Table B.1 parse: {num_sfb}')
    # HSF (ac4_hsf_ext_substream(), Sec.4.2.4.3): every LENGTHS_48 transform
    # length doubled (96 kHz) and quadrupled (192 kHz) - Tables B.2/B.3's own
    # transform lengths, in the same order.
    num_sfb_96 = parse_b2(lines)
    lengths_96 = [tl * 2 for tl in lengths]
    if sorted(num_sfb_96, reverse=True) != lengths_96:
        raise SystemExit(f'Table B.2 parse: {num_sfb_96}')
    num_sfb_192 = parse_b3(lines)
    lengths_192 = [tl * 4 for tl in lengths]
    if sorted(num_sfb_192, reverse=True) != lengths_192:
        raise SystemExit(f'Table B.3 parse: {num_sfb_192}')

    offsets = {}
    offsets.update(parse_split_offset_table(
        lines, 'Table B.4: Scale factor band offsets', 'Table B.5: Scale factor band offsets',
        [2048, 1920, 1536]))
    offsets.update(parse_split_offset_table(
        lines, 'Table B.5: Scale factor band offsets', 'Table B.6: Scale factor band offsets',
        [1024, 960, 768]))
    offsets.update(parse_split_offset_table(
        lines, 'Table B.6: Scale factor band offsets', 'Table B.7: Scale factor band offsets',
        [512, 480, 384]))
    offsets.update(parse_single_offset_table(
        lines, 'Table B.7: Scale factor band offsets', 'Table B.8: Mapping from max_sfb_master',
        [256, 240, 192, 128, 120, 96]))
    # B.4-B.7 columns continue past the 48 kHz num_sfb for the 96/192 kHz transform
    # lengths that share the column - HSF reads exactly that continuation, so this
    # takes the same column three times, once per rate, instead of discarding it
    # past the 44.1/48 kHz part.
    if any(b <= a for col in offsets.values() for a, b in pairwise(col)):
        raise SystemExit('an sfb_offset column is not strictly increasing')

    def take_offsets(column_key, target_length, n, rate):
        col = offsets[column_key]
        if len(col) < n + 1:
            raise SystemExit(f'sfb_offset column {column_key}, {target_length}@{rate}: only '
                             f'{len(col)} entries, need {n + 1}')
        part = col[:n + 1]
        if part[0] != 0 or part[-1] != target_length:
            raise SystemExit(f'sfb_offset column {column_key}, {target_length}@{rate}: starts '
                             f'{part[0]}, ends {part[-1]} at sfb {n}')
        if any(v % 4 for v in part):
            raise SystemExit(f'sfb_offset column {column_key}, {target_length}@{rate}: value '
                             f'not a multiple of 4')
        return tuple(part)

    sfb_offset_48, sfb_offset_96, sfb_offset_192 = {}, {}, {}
    for tl in lengths:
        sfb_offset_48[tl] = take_offsets(tl, tl, num_sfb[tl], 48)
        report.append(f'sfb_offset[{tl:4d}]: {num_sfb[tl]} bands, 0..{tl} ok '
                      f'(text column has {len(offsets[tl])} entries)')
        tl_96 = tl * 2
        sfb_offset_96[tl_96] = take_offsets(tl, tl_96, num_sfb_96[tl_96], 96)
        tl_192 = tl * 4
        sfb_offset_192[tl_192] = take_offsets(tl, tl_192, num_sfb_192[tl_192], 192)

    master = {}
    specs = [
        ('Table B.8: Mapping from max_sfb_master from transform length 2 048', 'Table B.9:',
         2048, [1024, 512, 256, 128]),
        ('Table B.9: Mapping from max_sfb_master from transform length 1 024', 'Table B.10:',
         1024, [512, 256, 128]),
        ('Table B.10: Mapping from max_sfb_master from transform length 512', 'Table B.11:',
         512, [256, 128]),
        ('Table B.11: Mapping from max_sfb_master from transform length 256', 'Table B.12:',
         256, [128]),
        ('Table B.12: Mapping from max_sfb_master from transform length 1 920', 'Table B.13:',
         1920, [960, 480, 240, 120]),
        ('Table B.13: Mapping from max_sfb_master from transform length 960', 'Table B.14:',
         960, [480, 240, 120]),
        ('Table B.14: Mapping from max_sfb_master from transform length 480', 'Table B.15:',
         480, [240, 120]),
        ('Table B.15: Mapping from max_sfb_master from transform length 240', 'Table B.16:',
         240, [120]),
        ('Table B.16: Mapping from max_sfb_master from transform length 1 536', 'Table B.17:',
         1536, [768, 384, 192, 96]),
        ('Table B.17: Mapping from max_sfb_master from transform length 768', 'Table B.18:',
         768, [384, 192, 96]),
        ('Table B.18: Mapping from max_sfb_master from transform length 384', 'Table B.19:',
         384, [192, 96]),
        ('Table B.19: Mapping from max_sfb_master from transform length 192',
         'Annex C (normative)', 192, [96]),
    ]
    for title, nxt, src, targets in specs:
        cols = parse_master_table(lines, title, nxt, targets)
        n_rows = len(next(iter(cols.values())))
        side_bits = N_MSFB_BITS_48[src][1]
        if n_rows != (1 << side_bits):
            raise SystemExit(f'{title}: {n_rows} rows, n_side_bits for {src} is {side_bits}')
        for t, col in cols.items():
            if col[0] != 0 or any(b < a for a, b in pairwise(col)):
                raise SystemExit(f'{title}: column {t} not non-decreasing from 0: {col}')
            if max(col) > num_sfb[t]:
                raise SystemExit(f'{title}: column {t} exceeds num_sfb {num_sfb[t]}')
        master[src] = {t: tuple(col) for t, col in cols.items()}
        report.append(f'n_sfb_side from {src}: {n_rows} rows, targets {targets}')

    for tl, (msfb, _side, _msfbl) in N_MSFB_BITS_48.items():
        if num_sfb[tl] >= (1 << msfb):
            raise SystemExit(f'Table 106: num_sfb {num_sfb[tl]} for {tl} does not fit '
                             f'{msfb} bits')

    parts = ['"""AC-4 tables for the syntax transcription (ac4_syntax.py).\n\n'
             'Generated by tools/generators/gen_ac4_reference_tables.py - do not edit.\n'
             'Sources: ETSI TS 103 190-1 V1.4.1\n'
             'Annex A (Huffman codebooks: LEN/CW arrays from the ts_10319001v010401p0.zip\n'
             'table attachment, codebook parameters and Tables A.14/A.15 from the Annex A\n'
             'text), Annex B (Tables B.1-B.7 for 44.1/48, 96 and 192 kHz, B.8-B.19, parsed\n'
             'from the text and checked) and hand-transcribed clause 4/5 tables; ETSI TS\n'
             '103 190-2 V1.3.1 Annex A.1.2 (the A-JCC codebooks: LEN/CW arrays from the\n'
             'ts_10319002v010301p0.zip attachment, parameters from the text) and its Table\n'
             '83. Plain Python data.\n'
             '"""\n\n',
             '# name -> {"cb_off", "cb_mod", "cb_mod2", "cb_mod3", "len": [...], "cw": [...]}\n',
             'HUFFMAN_CODEBOOKS = {\n']
    all_params = {**CODEBOOK_PARAMS,
                  **{name: (n, off, None, None, None)
                     for name, (n, off) in AJCC_CODEBOOK_PARAMS.items()}}
    for name, (_n, off, mod, mod2, mod3) in all_params.items():
        lens, cws = arrays[name]['LEN'], arrays[name]['CW']
        parts.append(f'    {name!r}: {{\n'
                     f'        "cb_off": {off!r}, "cb_mod": {mod!r}, "cb_mod2": {mod2!r}, '
                     f'"cb_mod3": {mod3!r},\n'
                     f'        "len": [\n{fmt_list(lens, 20, 12)}\n        ],\n'
                     f'        "cw": [\n{fmt_list([hex(c) for c in cws], 9, 12)}\n        ],\n'
                     '    },\n')
    parts.append('}\n\n# Kraft sums (numerator, denominator); (1, 1) means the codebook is '
                 'complete.\n')
    parts.append(assign('HUFFMAN_KRAFT', {n: (k.numerator, k.denominator)
                                          for n, k in kraft.items()}))
    parts.append('\n# Table A.14 CB_DIM and Table A.15 UNSIGNED_CB, codebook number 1..11\n')
    parts.append(assign('CB_DIM', CB_DIM))
    parts.append(assign('UNSIGNED_CB', UNSIGNED_CB))
    parts.append('\n# Table B.1: transform length -> num_sfb (44.1/48 kHz)\n')
    parts.append(assign('NUM_SFB_48', dict(sorted(num_sfb.items(), reverse=True))))
    parts.append('\n# Tables B.4-B.7: transform length -> sfb_offset[0..num_sfb] (44.1/48 kHz)\n')
    parts.append('SFB_OFFSET_48 = {\n')
    for tl in lengths:
        parts.append(f'    {tl}: (\n{fmt_list(list(sfb_offset_48[tl]), 14, 8)}\n    ),\n')
    parts.append('}\n')
    parts.append('\n# Table B.2 / the 96 kHz columns of Tables B.4-B.7: the HSF extension\'s own\n'
                 '# transform length (double the owning channel\'s) -> num_sfb / sfb_offset.\n')
    parts.append(assign('NUM_SFB_96', dict(sorted(num_sfb_96.items(), reverse=True))))
    parts.append('SFB_OFFSET_96 = {\n')
    for tl in lengths_96:
        parts.append(f'    {tl}: (\n{fmt_list(list(sfb_offset_96[tl]), 14, 8)}\n    ),\n')
    parts.append('}\n')
    parts.append('\n# Table B.3 / the 192 kHz columns of Tables B.4-B.7: quadruple the owning '
                 'channel\'s.\n')
    parts.append(assign('NUM_SFB_192', dict(sorted(num_sfb_192.items(), reverse=True))))
    parts.append('SFB_OFFSET_192 = {\n')
    for tl in lengths_192:
        parts.append(f'    {tl}: (\n{fmt_list(list(sfb_offset_192[tl]), 14, 8)}\n    ),\n')
    parts.append('}\n\n# Tables B.8-B.19: largest transform length -> '
                 '{transform length: n_sfb_side[max_sfb_master]}\n')
    parts.append(assign('N_SFB_SIDE', master))
    small = [
        ('Table 83 (and 84): frame_rate_index -> frame_len_base', 'FRAME_LEN_BASE',
         FRAME_LEN_BASE),
        ('Table 100: frame_length (>= 1 536) -> transform length by transf_length[i]',
         'TRANSF_LENGTH_LONG_BASE', TRANSF_LENGTH_LONG_BASE),
        ('Table 103: frame_length (< 1 536) -> transform length by transf_length '
         '(None = not allowed)', 'TRANSF_LENGTH_SHORT_BASE', TRANSF_LENGTH_SHORT_BASE),
        ('Table 106: transform length -> (n_msfb_bits, n_side_bits, n_msfbl_bits)',
         'N_MSFB_BITS_48', N_MSFB_BITS_48),
        ('Table 109: (transf_length[0], transf_length[1]) -> n_grp_bits '
         '(frame_len_base >= 1 536)', 'N_GRP_BITS_LONG_BASE', N_GRP_BITS_LONG_BASE),
        ('Table 110: frame_len_base (< 1 536) -> n_grp_bits by transf_length',
         'N_GRP_BITS_SHORT_BASE', N_GRP_BITS_SHORT_BASE),
        ('Table 143: acpl_num_param_bands_id -> acpl_num_param_bands', 'ACPL_NUM_PARAM_BANDS',
         ACPL_NUM_PARAM_BANDS),
        ('Table 197: acpl_num_param_bands -> parameter band of QMF subband 0..63', 'SB_TO_PB',
         sb_to_pb()),
        ('Table 163: drc_gains_config -> nr_drc_bands', 'NR_DRC_BANDS', NR_DRC_BANDS),
        ('Table 169: frame length -> nr_drc_subframes', 'NR_DRC_SUBFRAMES', NR_DRC_SUBFRAMES),
        ('Table 171: de_channel_config -> de_nr_channels', 'DE_NR_CHANNELS', DE_NR_CHANNELS),
        ('Clause 5.7.6.3.1.1: A-SPX low-resolution template subband group table',
         'SBG_TEMPLATE_LOWRES', SBG_TEMPLATE_LOWRES),
        ('Clause 5.7.6.3.1.1: A-SPX high-resolution template subband group table',
         'SBG_TEMPLATE_HIGHRES', SBG_TEMPLATE_HIGHRES),
        ('Table 192: frame_length -> num_ts_in_ats', 'NUM_TS_IN_ATS', NUM_TS_IN_ATS),
        ('Table 194: num_aspx_timeslots -> {aspx_num_[env|noise]: tab_border}', 'TAB_BORDER',
         TAB_BORDER),
        ('Part 2 Table 83: ajcc_num_param_bands_id -> ajcc_num_bands_table', 'AJCC_NUM_BANDS',
         AJCC_NUM_BANDS),
    ]
    for comment, name, value in small:
        parts.append(f'\n# {comment}\n')
        parts.append(assign(name, value))
    OUT.write_text(''.join(parts), encoding='utf-8', newline='\n')

    print('\n'.join(report))
    incomplete = {n: k for n, k in kraft.items() if k != 1}
    print(f'\n{len(kraft)} codebooks, all prefix-free; incomplete (Kraft < 1): '
          + (', '.join(f'{n}={k} ({float(k):.6f})' for n, k in incomplete.items()) or 'none'))
    print(f'wrote {OUT}')


if __name__ == '__main__':
    main()
