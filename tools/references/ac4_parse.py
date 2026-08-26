"""Independent AC-4 sync-frame / TOC / presentation / substream parser.

ETSI TS 103 190-1 V1.4.1 (2025-07), "Digital Audio Compression (AC-4)
Standard; Part 1: Channel based coding" and ETSI TS 103 190-2 V1.3.1
(2025-07), "... Part 2: Immersive and personalized audio". Section numbers
below cite whichever of the two parts actually defines the element; Part 2
clause 6 supersedes Part 1 clause 4 wherever the two disagree (bitstream
versioning is what selects between them - see parse_ac4_toc()).

Written to check the C++ ac4:: parser's field placement against a
known-good stream (a real Dolby Encoding Engine encode, not one this
project's own tooling produced), the same role tools/references/eac3_parse.py
plays for E-AC-3.

Scope: TOC, presentation, substream-group and channel-coded substream-info
framing only, plus the outer envelope (audio_size) of each substream's
ac4_substream(). audio_data and metadata() payloads are reported as byte
ranges, never decoded - this is a bitstream inspector, not a decoder. A-JOC,
direct-coded-object and OAMD substream groups (b_channel_coded == 0) are
recognised but not walked: TS 103 190-2 clause 6.3.2.8-6.3.2.12 defines their
info elements, and transcribing those (object position tables, bed/dynamic
object assignment, OAMD metadata) is out of scope for this pass. A stream
that carries one is reported as such and parsing of that presentation's
substream-group loop stops there, cleanly, rather than silently going out of
sync.

Usage:  python tools/references/ac4_parse.py <file.ac4> [frame_index]
"""

import sys
from pathlib import Path

BASE_SAMP_FREQ = {0: 44100, 1: 48000}  # Table 82

# Table 88 (TS 103 190-1 §4.3.3.7.1): channel_mode for presentation_version 0.
CHANNEL_MODE_V0 = {
    0b0: ('Mono', 0), 0b10: ('Stereo', 1), 0b1100: ('3.0', 2), 0b1101: ('5.0', 3),
    0b1110: ('5.1', 4), 0b1111000: ('7.0: 3/4/0', 5), 0b1111001: ('7.1: 3/4/0.1', 6),
    0b1111010: ('7.0: 5/2/0', 7), 0b1111011: ('7.1: 5/2/0.1', 8),
    0b1111100: ('7.0: 3/2/2', 9), 0b1111101: ('7.1: 3/2/2.1', 10),
}
# Table 56 (TS 103 190-2 §6.3.2.7.2): channel_mode for presentation_version 1,
# extending Table 88 with 8- and 9-bit codes up to 22.2.
CHANNEL_MODE_V1 = dict(CHANNEL_MODE_V0)
CHANNEL_MODE_V1.update({
    0b11111100: ('7.0.4', 11), 0b11111101: ('7.1.4', 12),
    0b111111100: ('9.0.4', 13), 0b111111101: ('9.1.4', 14),
    0b111111110: ('22.2', 15),
})
# Table 90 (TS 103 190-1 §4.3.3.7.5): bitrate_indicator -> kbit/s per channel.
BITRATE_KBPS = {
    0b000: 16, 0b010: 20, 0b100: 24, 0b110: 28, 0b00100: 32, 0b00101: 40,
    0b00110: 48, 0b00111: 56, 0b01100: 64, 0b01101: 80, 0b01110: 96, 0b01111: 112,
}


class Reader:
    def __init__(self, data):
        self.data = data
        self.pos = 0

    def bits(self, n):
        v = 0
        for _ in range(n):
            byte = self.data[self.pos >> 3]
            v = (v << 1) | ((byte >> (7 - (self.pos & 7))) & 1)
            self.pos += 1
        return v

    def byte_align(self):
        self.pos = (self.pos + 7) & ~7


def variable_bits(r, n_bits):
    """Table 3 (§4.2.2): a value sent as groups of n_bits, MSB group first,
    each followed by a continuation bit."""
    value = 0
    while True:
        value += r.bits(n_bits)
        if not r.bits(1):
            return value
        value <<= n_bits
        value += 1 << n_bits


# --- Annex G: AC-4 sync frame (transport layer, common to both parts) ------

def crc16(data):
    """Annex G.4.2: generator polynomial x^16+x^15+x^2+1, initial state
    0x0000, no reflection, no final XOR. A correctly-received frame's
    trailing crc_word drives this back to 0."""
    crc = 0x0000
    poly = 0x8005  # x^16 + x^15 + x^2 + 1, MSB-first form
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ poly) & 0xFFFF if (crc & 0x8000) else (crc << 1) & 0xFFFF
    return crc


def iter_sync_frames(data):
    """Annex G.3.1/G.3.2: walk ac4_syncframe() elements back to back.

    Yields (offset, sync_word, raw_ac4_frame_bytes, crc_ok_or_None) tuples.
    crc_ok is None when sync_word == 0xAC40 (no crc_word transmitted).
    """
    pos = 0
    while pos + 4 <= len(data):
        sync = (data[pos] << 8) | data[pos + 1]
        if sync not in (0xAC40, 0xAC41):
            raise ValueError(f'lost sync at byte {pos}: {sync:#06x}')
        frame_size = (data[pos + 2] << 8) | data[pos + 3]
        header = 4
        if frame_size == 0xFFFF:
            frame_size = (data[pos + 4] << 16) | (data[pos + 5] << 8) | data[pos + 6]
            header = 7
        frame_start = pos + header
        frame_end = frame_start + frame_size
        crc_ok = None
        total = frame_end
        if sync == 0xAC41:
            crc_ok = crc16(data[pos + 2:frame_end]) == \
                ((data[frame_end] << 8) | data[frame_end + 1])
            total = frame_end + 2
        yield pos, sync, data[frame_start:frame_end], crc_ok
        pos = total


# --- §4.2.3.5 emdf_info / §4.2.14.15 emdf_reserved --------------------------

def parse_emdf_reserved(r):
    """Table 80. Despite the clause title, the syntax table itself is headed
    emdf_protection() - the same element, called as emdf_reserved() from
    emdf_info(). Two independent 2-bit length codes (0/1/4/16 bytes each,
    added together) bound a trailing reserved run; unlike the classic Annex H
    EMDF container's own prim/sec protection fields (0/8/32/128 BITS each),
    this one counts BYTES and uses a different power-of-four table."""
    n_skip_bytes = 0
    primary = r.bits(2)
    secondary = r.bits(2)
    if primary > 0:
        n_skip_bytes += 1 << (2 * (primary - 1))
    if secondary > 0:
        n_skip_bytes += 1 << (2 * (secondary - 1))
    r.bits(8 * n_skip_bytes)


def parse_emdf_info(r):
    emdf_version = r.bits(2)
    if emdf_version == 3:
        emdf_version += variable_bits(r, 2)
    key_id = r.bits(3)
    if key_id == 7:
        key_id += variable_bits(r, 3)
    payloads_substream_index = None
    if r.bits(1):  # b_emdf_payloads_substream_info
        payloads_substream_index = parse_substream_index_ref(r)
    parse_emdf_reserved(r)
    return {'emdf_version': emdf_version, 'key_id': key_id,
            'payloads_substream_index': payloads_substream_index}


def parse_substream_index_ref(r):
    """The `substream_index; ...2; if (==3) += variable_bits(2)` shape
    repeated by every *_substream_info element (§4.3.3.7.9 and its Part 2
    counterparts) to name a row of substream_index_table()."""
    idx = r.bits(2)
    if idx == 3:
        idx += variable_bits(r, 2)
    return idx


# --- §4.2.3.7 content_type --------------------------------------------------

def parse_content_type(r):
    content_classifier = r.bits(3)
    language = None
    if r.bits(1):  # b_language_indicator
        if r.bits(1):  # b_serialized_language_tag
            r.bits(1)  # b_start_tag
            r.bits(16)  # language_tag_chunk
        else:
            n = r.bits(6)
            language = bytes(r.bits(8) for _ in range(n))
    return {'content_classifier': content_classifier, 'language_tag': language}


# --- §4.2.3.4 frame_rate_multiply_info / §6.2.1.4 frame_rate_fractions_info -

def parse_frame_rate_multiply_info(r, frame_rate_index):
    """Table 87 (§4.3.3.5.3): resolves frame_rate_factor (1, 2 or 4)."""
    if frame_rate_index in (2, 3, 4):
        if r.bits(1):  # b_multiplier
            return 4 if r.bits(1) else 2
        return 1
    if frame_rate_index in (0, 1, 7, 8, 9):
        return 2 if r.bits(1) else 1
    return 1


def parse_frame_rate_fractions_info(r, frame_rate_index, frame_rate_factor):
    if frame_rate_index in (5, 6, 7, 8, 9) and frame_rate_factor == 1:
        if r.bits(1):
            return 2
    elif frame_rate_index in (10, 11, 12):
        if r.bits(1):
            return 4 if r.bits(1) else 2
    return 1


# --- §4.2.3.9 ac4_hsf_ext_substream_info (Part 1 has no parameter; Part 2 --
# --- gates it on b_substreams_present, §6.2.1.14) ---------------------------

def parse_hsf_ext_substream_info(r, b_substreams_present=True):
    if b_substreams_present:
        return parse_substream_index_ref(r)
    return None


# --- §4.2.3.8 / §6.2.1.5 presentation_config_ext_info -----------------------

def parse_presentation_config_ext_info(r):
    n_skip_bytes = r.bits(5)
    if r.bits(1):  # b_more_skip_bytes
        n_skip_bytes += variable_bits(r, 2) << 5
    for _ in range(n_skip_bytes):
        r.bits(8)


# --- §4.2.3.6 ac4_substream_info (presentation_version 0 channel_mode) -----

def parse_substream_info_v0(r, fs_index, frame_rate_factor):
    channel_mode = r.bits(1)
    if channel_mode == 0:
        pass  # mono, 1 bit
    else:
        channel_mode = (channel_mode << 1) | r.bits(1)
        if channel_mode != 0b10:
            channel_mode = (channel_mode << 2) | r.bits(2)
            if channel_mode not in (0b1100, 0b1101, 0b1110):
                channel_mode = (channel_mode << 3) | r.bits(3)
                if channel_mode == 0b1111111:
                    channel_mode += variable_bits(r, 2)
    name, ch_mode = CHANNEL_MODE_V0.get(channel_mode, (f'reserved({channel_mode:#x})', None))
    sf_multiplier = None
    if fs_index == 1 and r.bits(1):  # b_sf_multiplier
        sf_multiplier = r.bits(1)
    bitrate_kbps = None
    if r.bits(1):  # b_bitrate_info
        bitrate_indicator = _read_bitrate_indicator(r)
        bitrate_kbps = BITRATE_KBPS.get(bitrate_indicator, 'unlimited')
    if channel_mode in (0b1111010, 0b1111011, 0b1111100, 0b1111101):
        r.bits(1)  # add_ch_base
    content_type = parse_content_type(r) if r.bits(1) else None  # b_content_type
    for _ in range(frame_rate_factor):
        r.bits(1)  # b_iframe
    substream_index = parse_substream_index_ref(r)
    return {'channel_mode': channel_mode, 'channel_mode_name': name, 'ch_mode': ch_mode,
            'sf_multiplier': sf_multiplier, 'bitrate_kbps': bitrate_kbps,
            'content_type': content_type, 'substream_index': substream_index}


def _read_bitrate_indicator(r):
    """Table 90's code is 3 bits unless the top two bits are both set, in
    which case it extends to 5 - the same variable-width shape channel_mode
    uses, just with its own prefix set."""
    v = r.bits(3)
    if v in (0b001, 0b011, 0b101, 0b111):
        v = (v << 2) | r.bits(2)
    return v


# --- §6.3.2.7 ac4_substream_info_chan (presentation_version 1 channel_mode) -

def parse_substream_info_chan(r, fs_index, frame_rate_factor, b_substreams_present):
    channel_mode = r.bits(1)
    if channel_mode == 0:
        pass
    else:
        channel_mode = (channel_mode << 1) | r.bits(1)
        if channel_mode != 0b10:
            channel_mode = (channel_mode << 2) | r.bits(2)
            if channel_mode not in (0b1100, 0b1101, 0b1110):
                channel_mode = (channel_mode << 3) | r.bits(3)
                # Table 56's 7-bit codes stop at 0b1111101 (7.1: 3/2/2.1);
                # the two remaining 7-bit values are BOTH incomplete
                # prefixes, but of different lengths - 0b1111110 needs only
                # one more bit (11111100/11111101, both terminal), while
                # 0b1111111 needs two more (11111110|0/1 and 11111111|0/1,
                # the latter of which - 0b111111111 - is what triggers the
                # variable_bits() extension). Reading a fixed-width chunk
                # here regardless of which 7-bit prefix was seen misreads
                # every 9.x/22.2 channel_mode and desyncs the frame.
                if channel_mode == 0b1111110:
                    channel_mode = (channel_mode << 1) | r.bits(1)
                elif channel_mode == 0b1111111:
                    channel_mode = (channel_mode << 1) | r.bits(1)
                    channel_mode = (channel_mode << 1) | r.bits(1)
                    if channel_mode == 0b111111111:
                        channel_mode += variable_bits(r, 2)
    name, ch_mode = CHANNEL_MODE_V1.get(channel_mode, (f'reserved({channel_mode:#x})', None))
    original = {}
    if channel_mode in (0b11111100, 0b11111101, 0b111111100, 0b111111101):
        original['b_4_back_channels_present'] = bool(r.bits(1))
        original['b_centre_present'] = bool(r.bits(1))
        original['top_channels_present'] = r.bits(2)
    sf_multiplier = None
    if fs_index == 1 and r.bits(1):
        sf_multiplier = r.bits(1)
    bitrate_kbps = None
    if r.bits(1):
        bitrate_indicator = _read_bitrate_indicator(r)
        bitrate_kbps = BITRATE_KBPS.get(bitrate_indicator, 'unlimited')
    if channel_mode in (0b1111010, 0b1111011, 0b1111100, 0b1111101):
        r.bits(1)  # add_ch_base
    for _ in range(frame_rate_factor):
        r.bits(1)  # b_audio_ndot
    substream_index = parse_substream_index_ref(r) if b_substreams_present else None
    return {'channel_mode': channel_mode, 'channel_mode_name': name, 'ch_mode': ch_mode,
            'original_content': original, 'sf_multiplier': sf_multiplier,
            'bitrate_kbps': bitrate_kbps, 'substream_index': substream_index}


# --- §4.2.3.2 ac4_presentation_info (presentation_version 0 path) ----------

_PRESENTATION_CONFIG_ROLES = {
    0: ('M+E', 'Dialog'), 1: ('Main', 'DE'), 2: ('Main', 'Associate'),
    3: ('M+E', 'Dialog', 'Associate'), 4: ('Main', 'DE', 'Associate'), 5: ('Main',),
}


def parse_presentation_version(r):
    """§4.2.3.3 / Table 6: a run of 1-bits terminated by a 0 bit."""
    version = 0
    while r.bits(1):
        version += 1
    return version


def parse_presentation_info(r, fs_index, frame_rate_index):
    b_single_substream = r.bits(1)
    presentation_config = None
    if not b_single_substream:
        presentation_config = r.bits(3)
        if presentation_config == 7:
            presentation_config += variable_bits(r, 2)
    presentation_version = parse_presentation_version(r)
    substreams = []
    emdf_substreams = []
    if not b_single_substream and presentation_config == 6:
        b_add_emdf_substreams = True
    else:
        md_compat = r.bits(3)
        presentation_id = None
        if r.bits(1):  # b_belongs_to_presentation_id
            presentation_id = variable_bits(r, 2)
        frame_rate_factor = parse_frame_rate_multiply_info(r, frame_rate_index)
        emdf = parse_emdf_info(r)
        if b_single_substream:
            substreams.append(('main', parse_substream_info_v0(r, fs_index, frame_rate_factor)))
        else:
            b_hsf_ext = r.bits(1)
            roles = _PRESENTATION_CONFIG_ROLES.get(presentation_config)
            if roles is None:
                parse_presentation_config_ext_info(r)
            else:
                for i, role in enumerate(roles):
                    substreams.append((role, parse_substream_info_v0(r, fs_index, frame_rate_factor)))
                    if i == 0 and b_hsf_ext:
                        parse_hsf_ext_substream_info(r)
        b_pre_virtualized = r.bits(1)
        b_add_emdf_substreams = r.bits(1)
        if b_add_emdf_substreams:
            n = r.bits(2)
            if n == 0:
                n = variable_bits(r, 2) + 4
            for _ in range(n):
                emdf_substreams.append(parse_emdf_info(r))
        return {'presentation_version': presentation_version, 'presentation_config': presentation_config,
                'md_compat': md_compat, 'presentation_id': presentation_id, 'emdf': emdf,
                'substreams': substreams, 'emdf_substreams': emdf_substreams,
                'b_pre_virtualized': bool(b_pre_virtualized)}
    n = r.bits(2)
    if n == 0:
        n = variable_bits(r, 2) + 4
    for _ in range(n):
        emdf_substreams.append(parse_emdf_info(r))
    return {'presentation_version': presentation_version, 'presentation_config': presentation_config,
            'substreams': [], 'emdf_substreams': emdf_substreams}


# --- §6.2.1.6 ac4_substream_group_info / §6.2.1.8 ac4_substream_info_chan --

class ObjectCodedGroup(Exception):
    """Raised when a substream group is not channel-coded. TS 103 190-2
    clause 6.3.2.8 (A-JOC), 6.3.2.10 (direct-coded objects) and 6.3.2.12
    (OAMD) define the info elements needed to stay synchronised past this
    point; this parser does not transcribe them (see module docstring)."""


def parse_substream_group_info(r, fs_index, frame_rate_factor):
    # frame_rate_factor is a frame-global quantity in the spec's own telling
    # (§6.3.2.1.3's b_iframe_global talks about "a series of 2 or 4
    # substreams" at the whole-FRAME level, not per presentation), even
    # though the only element that transmits it, frame_rate_multiply_info(),
    # is called once per presentation inside ac4_presentation_v1_info() -
    # ahead of, and structurally separate from, this function's own call
    # site in ac4_toc()'s substream-group loop. ac4_substream_info_chan()'s
    # b_audio_ndot loop (§6.2.1.8) bounds itself on a bare `frame_rate_factor`
    # with no parameter, i.e. ambient state rather than a per-group value, so
    # the caller resolves it once (from the first presentation) and threads
    # it through explicitly instead of re-deriving it per group.
    b_substreams_present = bool(r.bits(1))
    b_hsf_ext = bool(r.bits(1))
    b_single_substream = r.bits(1)
    if b_single_substream:
        n_lf_substreams = 1
    else:
        n = r.bits(2)
        n_lf_substreams = n + 2
        if n_lf_substreams == 5:
            n_lf_substreams += variable_bits(r, 2)
    b_channel_coded = r.bits(1)
    if not b_channel_coded:
        raise ObjectCodedGroup(
            'substream group is object/A-JOC/OAMD-coded (b_channel_coded=0); '
            'TS 103 190-2 clause 6.3.2.8-6.3.2.12 not implemented')
    substreams = []
    for _ in range(n_lf_substreams):
        # sus_ver only exists for bitstream_version == 1; the caller only
        # reaches this function for bitstream_version >= 2, where it is
        # implicitly 1 (extended ac4_substream() syntax) per §6.2.1.6.
        chan = parse_substream_info_chan(r, fs_index, frame_rate_factor, b_substreams_present)
        if b_hsf_ext:
            parse_hsf_ext_substream_info(r, b_substreams_present)
        substreams.append(chan)
    content_type = parse_content_type(r) if r.bits(1) else None  # b_content_type
    return {'b_substreams_present': b_substreams_present, 'substreams': substreams,
            'content_type': content_type}


# --- §6.2.1.3 ac4_presentation_v1_info / §6.2.1.7 ac4_sgi_specifier --------

_V1_CONFIG_GROUP_COUNTS = {0: 2, 1: 1, 2: 2, 3: 3, 4: 2}


def parse_sgi_specifier(r, bitstream_version, fs_index, frame_rate_factor):
    """Returns a group_index (int) for bitstream_version >= 2, or an inline
    ac4_substream_group_info() dict for bitstream_version == 1."""
    if bitstream_version == 1:
        return parse_substream_group_info(r, fs_index, frame_rate_factor)
    group_index = r.bits(3)
    if group_index == 7:
        group_index += variable_bits(r, 2)
    return group_index


def parse_presentation_v1_info(r, bitstream_version, fs_index, frame_rate_index):
    b_single_substream_group = r.bits(1)
    presentation_config = None
    if not b_single_substream_group:
        presentation_config = r.bits(3)
        if presentation_config == 7:
            presentation_config += variable_bits(r, 2)
    presentation_version = 0
    if bitstream_version != 1:
        presentation_version = parse_presentation_version(r)
    group_refs = []
    if not b_single_substream_group and presentation_config == 6:
        pass  # EMDF-only presentation; handled by the caller like v0's case
    else:
        md_compat = None
        if bitstream_version != 1:
            md_compat = r.bits(3)
        presentation_id = None
        if r.bits(1):  # b_presentation_id
            presentation_id = variable_bits(r, 2)
        frame_rate_factor = parse_frame_rate_multiply_info(r, frame_rate_index)
        parse_frame_rate_fractions_info(r, frame_rate_index, frame_rate_factor)
        emdf = parse_emdf_info(r)
        b_enable_presentation = None
        if r.bits(1):  # b_presentation_filter
            b_enable_presentation = bool(r.bits(1))
        if b_single_substream_group:
            group_refs.append(parse_sgi_specifier(r, bitstream_version, fs_index, frame_rate_factor))
        else:
            r.bits(1)  # b_multi_pid
            n_groups = _V1_CONFIG_GROUP_COUNTS.get(presentation_config)
            if n_groups is not None:
                for _ in range(n_groups):
                    group_refs.append(parse_sgi_specifier(r, bitstream_version, fs_index, frame_rate_factor))
            elif presentation_config == 5:
                n = r.bits(2) + 2
                if n == 5:
                    n += variable_bits(r, 2)
                for _ in range(n):
                    group_refs.append(parse_sgi_specifier(r, bitstream_version, fs_index, frame_rate_factor))
            else:
                parse_presentation_config_ext_info(r)
        r.bits(1)  # b_pre_virtualized
        b_add_emdf_substreams = r.bits(1)
        # ac4_presentation_substream_info() (§6.2.1.12)
        r.bits(1)  # b_alternative
        r.bits(1)  # b_pres_ndot
        parse_substream_index_ref(r)
        if b_add_emdf_substreams:
            n = r.bits(2)
            if n == 0:
                n = variable_bits(r, 2) + 4
            for _ in range(n):
                parse_emdf_info(r)
        return {'presentation_version': presentation_version,
                'presentation_config': presentation_config, 'group_refs': group_refs,
                'md_compat': md_compat, 'enable_presentation': b_enable_presentation,
                'frame_rate_factor': frame_rate_factor}
    return {'presentation_version': presentation_version,
            'presentation_config': presentation_config, 'group_refs': group_refs,
            'frame_rate_factor': 1}


# --- §4.2.3.11 substream_index_table ----------------------------------------

def parse_substream_index_table(r):
    n_substreams = r.bits(2)
    if n_substreams == 0:
        n_substreams = variable_bits(r, 2) + 4
    if n_substreams == 1:
        b_size_present = bool(r.bits(1))
    else:
        b_size_present = True
    sizes = []
    if b_size_present:
        for _ in range(n_substreams):
            # Table 14: b_more_bits precedes substream_size[s], not the
            # other way around.
            b_more_bits = r.bits(1)
            size = r.bits(10)
            if b_more_bits:
                size += variable_bits(r, 2) << 10
            sizes.append(size)
    return n_substreams, sizes


# --- §4.2.1 / §6.2.1.1 ac4_toc ---------------------------------------------

def parse_ac4_toc(r):
    bitstream_version = r.bits(2)
    if bitstream_version == 3:
        bitstream_version += variable_bits(r, 2)
    if bitstream_version > 2:
        raise ValueError(f'bitstream_version {bitstream_version} > 2 is not '
                          f'decodable per TS 103 190-2 §6.3.2.1.1')
    sequence_counter = r.bits(10)
    wait_frames = None
    if r.bits(1):  # b_wait_frames
        wait_frames = r.bits(3)
        if wait_frames > 0:
            r.bits(2)  # br_code (Part 2) / reserved (Part 1) - both 2 bits
    fs_index = r.bits(1)
    frame_rate_index = r.bits(4)
    b_iframe_global = bool(r.bits(1))
    b_single_presentation = r.bits(1)
    if b_single_presentation:
        n_presentations = 1
    elif r.bits(1):  # b_more_presentations
        n_presentations = variable_bits(r, 2) + 2
    else:
        n_presentations = 0
    # §4.3.3.2.10/.11 (Part 1) / §6.2.1.1 (Part 2, identical shape): where
    # substream 0's payload starts, relative to the end of the byte-aligned
    # ac4_toc(), in bytes. Defaults to 0 (immediately after the TOC) when
    # b_payload_base is unset.
    payload_base = 0
    if r.bits(1):  # b_payload_base
        payload_base = r.bits(5) + 1
        if payload_base == 0x20:
            payload_base += variable_bits(r, 3)
    toc = {'bitstream_version': bitstream_version, 'sequence_counter': sequence_counter,
           'wait_frames': wait_frames, 'sample_rate': BASE_SAMP_FREQ[fs_index],
           'frame_rate_index': frame_rate_index, 'b_iframe_global': b_iframe_global,
           'n_presentations': n_presentations, 'payload_base': payload_base}
    if bitstream_version <= 1:
        toc['presentations'] = [parse_presentation_info(r, fs_index, frame_rate_index)
                                 for _ in range(n_presentations)]
        toc['substream_groups'] = None
    else:
        program_id = None
        if r.bits(1):  # b_program_id
            short_id = r.bits(16)
            uuid = r.bits(16 * 8) if r.bits(1) else None  # b_program_uuid_present
            program_id = {'short_program_id': short_id, 'uuid': uuid}
        toc['program_id'] = program_id
        presentations = [parse_presentation_v1_info(r, bitstream_version, fs_index, frame_rate_index)
                          for _ in range(n_presentations)]
        toc['presentations'] = presentations
        # §6.3.2.1.8: total_n_substream_groups is derived, not transmitted -
        # 1 + the highest group_index any ac4_sgi_specifier() referenced.
        max_group_index = -1
        for p in presentations:
            for ref in p.get('group_refs', []):
                if isinstance(ref, int):
                    max_group_index = max(max_group_index, ref)
        total_groups = max_group_index + 1
        # See parse_substream_group_info()'s own comment: frame_rate_factor
        # is frame-global in practice, so the first presentation's resolved
        # value is what every group's ac4_substream_info_chan() call uses.
        group_frame_rate_factor = presentations[0]['frame_rate_factor'] if presentations else 1
        toc['substream_groups'] = [parse_substream_group_info(r, fs_index, group_frame_rate_factor)
                                    for _ in range(total_groups)]
    n_substreams, substream_sizes = parse_substream_index_table(r)
    toc['n_substreams'] = n_substreams
    toc['substream_sizes'] = substream_sizes
    r.byte_align()
    return toc


def channel_substream_indices(toc):
    """Table 15 (Part 1) / Table 50 (Part 2): substream_index_table() is one
    flat array, but each entry's *type* - and so which ac4_substream_data
    element actually sits there - is decided by which kind of *_info element
    referenced it. ac4_substream_info()/ac4_substream_info_chan() map to
    ac4_substream() (the audio_size-prefixed shape parse_substream_header()
    reads); ac4_presentation_substream_info() and emdf_info()'s payloads
    reference map to ac4_presentation_substream() and
    emdf_payloads_substream() instead, neither of which this parser
    transcribes - reading audio_size out of one of those would just be
    reinterpreting the wrong bytes as the wrong shape."""
    indices = set()
    if toc['substream_groups'] is not None:
        for group in toc['substream_groups']:
            for sub in group['substreams']:
                if sub['substream_index'] is not None:
                    indices.add(sub['substream_index'])
    else:
        for pres in toc['presentations']:
            for _, sub in pres.get('substreams', []):
                if sub['substream_index'] is not None:
                    indices.add(sub['substream_index'])
    return indices


# --- §4.2.4.2 / §6.2.2.2 ac4_substream: header only (audio_size) -----------

def parse_substream_header(r):
    audio_size = r.bits(15)
    if r.bits(1):  # b_more_bits
        audio_size += variable_bits(r, 7) << 15
    return audio_size


def parse_raw_frame(raw):
    """§4.2.1 raw_ac4_frame(): ac4_toc() then n_substreams substream
    payloads, located via payload_base and substream_index_table()'s sizes
    (§4.3.3.12.4's Pseudocode 1) rather than by parsing through audio_data."""
    r = Reader(raw)
    toc = parse_ac4_toc(r)
    toc_bytes = (r.pos + 7) // 8
    chan_indices = channel_substream_indices(toc)
    substreams = []
    offset = toc_bytes + toc['payload_base']
    for index, size in enumerate(toc['substream_sizes']):
        payload = raw[offset:offset + size]
        audio_size = None
        if index in chan_indices and len(payload) >= 3:
            audio_size = parse_substream_header(Reader(payload))
        substreams.append({'offset': offset, 'size': size, 'is_channel_audio': index in chan_indices,
                            'audio_size': audio_size})
        offset += size
    return toc, substreams


def main():
    path = Path(sys.argv[1])
    want = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    data = path.read_bytes()

    frames = list(iter_sync_frames(data))
    print(f'{path}: {len(frames)} sync frame(s), {len(data)} bytes')
    if want >= len(frames):
        raise SystemExit(f'only {len(frames)} sync frames present')

    offset, sync, raw, crc_ok = frames[want]
    print(f'frame {want}: offset {offset}, sync {sync:#06x}, {len(raw)} bytes'
          + (f', crc {"ok" if crc_ok else "FAILED"}' if crc_ok is not None else ''))

    try:
        toc, substreams = parse_raw_frame(raw)
    except (ObjectCodedGroup, ValueError) as exc:
        raise SystemExit(f'REFUSED: {exc}')

    print(f"  bitstream_version={toc['bitstream_version']} "
          f"sequence_counter={toc['sequence_counter']} "
          f"sample_rate={toc['sample_rate']} frame_rate_index={toc['frame_rate_index']} "
          f"n_presentations={toc['n_presentations']} n_substreams={toc['n_substreams']}")
    for i, pres in enumerate(toc['presentations']):
        print(f"  presentation {i}: {pres}")
    if toc['substream_groups'] is not None:
        for i, group in enumerate(toc['substream_groups']):
            print(f"  substream_group {i}: {group}")
    for i, sub in enumerate(substreams):
        kind = 'channel audio' if sub['is_channel_audio'] else 'other (presentation/EMDF-payloads)'
        print(f"  substream {i}: offset={sub['offset']} size={sub['size']} [{kind}]"
              + (f" audio_size={sub['audio_size']}" if sub['is_channel_audio'] else ''))

    ok = all(c is not False for _, _, _, c in frames)
    print('VERDICT:', 'all CRCs ok' if ok else 'CRC FAILURE present')


if __name__ == '__main__':
    main()
