"""Which presentation of an AC-4 table of contents a decoder selects (ETSI TS 103 190-2 V1.3.1
clause 4.8.2), transcribed from the text and src/ac4dec/ERRATA.md's readings ("Which presentations
can be selected", "The order of the preferences") separately from the decoder's
src/ac4dec/src/presentations.cpp, over the tables of
contents ac4_parse.py reads. tools/checks/test_ac4_presentation_selection.py holds it to the
table tests/golden/ac4dec/presentations/presentation-selection.tsv, which the decoder's test
builds and holds the decoder to.

A presentation can be selected when the decoder can decode it: every substream it names is a
channel-coded substream in this elementary stream, in one of the channel modes the decoder renders
(Part 1's, 0 to 10), at 48 or 44.1 kHz; its presentation_version is 0, 1 or 2 (Part 2 clause
6.3.2.3.1); it carries audio (presentation_config 0 to 5, or a single substream or group); its
md_compat is one the tables define (Part 1 Table 86: 0 to 4 and 7; Part 2 Table 55: 0 to 3 and 7)
and within the decoder's level; and the stream has not disabled it (b_enable_presentation).

Of those, the one a system asks for by presentation_id, else by position, else the one that best
meets its preferences in the order the clause lists them: the language of its main or dialogue
audio (the whole tag, then the primary subtag), then its associated audio (the service asked for,
or none), then the kind of audio (pre-virtualized for headphones or not); the first in the table
of contents among equals.
"""

# Part 2 Table 53: each ac4_sgi_specifier()'s substream type for presentation_configs 0 to 4.
ROLES_V1 = {
    0: ('music_effects', 'dialogue'),
    1: ('main', 'dialogue_enhancement'),
    2: ('main', 'associated'),
    3: ('music_effects', 'dialogue', 'associated'),
    4: ('main', 'dialogue_enhancement', 'associated'),
}

# Part 2 Table 54: presentation_config 5's substream types by content_classifier.
TABLE_54 = {0b000: 'main', 0b001: 'music_effects', 0b010: 'associated', 0b011: 'associated',
            0b100: 'dialogue', 0b101: 'associated', 0b110: 'main', 0b111: 'main'}

# Part 1 Table 85, by the role names ac4_parse.py gives presentation_version 0 substreams.
ROLES_V0 = {'M+E': 'music_effects', 'Dialog': 'dialogue', 'DE': 'dialogue_enhancement',
            'Associate': 'associated', 'Main': 'main', 'main': 'main'}

# Part 1 Table 92: an associated substream's refinement, decoder mix then premix.
TABLE_92 = {'audio-description': ('qad', 'qax'), 'audio-description-subtitles': ('qas', 'qtx'),
            'spoken-subtitles': ('qss', 'qsx'), 'emergency-information': ('qei', 'qex')}

# The channel modes the decoder renders: Part 1's (mono to 7.1).
DECODED_CH_MODES = range(0, 11)


def _language(content_type):
    if not content_type or content_type.get('language_tag') is None:
        return ''
    return content_type['language_tag'].decode('ascii', errors='replace')


def _classifier(content_type):
    return content_type['content_classifier'] if content_type else None


def _decodable(info):
    return (info.get('ch_mode') in DECODED_CH_MODES and info.get('sf_multiplier') is None
            and info.get('substream_index') is not None)


def members(toc, p):
    """(members, decodable): each substream of presentation `p` with its role, classifier,
    language, substream index, group (version 1), the position of the ac4_sgi_specifier() or
    substream info that names it, and ch_mode, in the order the presentation names them;
    whether every one can be decoded."""
    out = []
    if toc['substream_groups'] is None:  # presentation_version 0 (Part 1 Table 4)
        decodable = bool(p.get('substreams'))
        for position, (name, info) in enumerate(p.get('substreams', [])):
            decodable = (decodable and _decodable(info)
                         and info.get('hsf_ext_substream_index') is None)
            out.append({'role': ROLES_V0.get(name, 'main'),
                        'classifier': _classifier(info.get('content_type')),
                        'language': _language(info.get('content_type')),
                        'substream_index': info.get('substream_index'), 'group': None,
                        'position': position, 'ch_mode': info.get('ch_mode')})
        return out, decodable
    refs = p.get('group_refs', [])
    decodable = bool(refs) and p.get('frame_rate_fraction', 1) == 1
    config = p.get('presentation_config')
    for position, ref in enumerate(refs):
        if not isinstance(ref, int) or ref >= len(toc['substream_groups']):
            decodable = False
            continue
        if ref in refs[:position]:
            continue  # a group named twice counts once
        group = toc['substream_groups'][ref]
        if config is None:
            role = 'main'
        elif config in ROLES_V1:
            role = ROLES_V1[config][position] if position < len(ROLES_V1[config]) else 'main'
        elif config == 5:
            role = TABLE_54.get(_classifier(group['content_type']), 'main')
        else:
            role = 'main'
        decodable = (decodable and group['b_substreams_present'] and group['b_channel_coded']
                     and bool(group['substreams']))
        for sub in group['substreams']:
            if sub['kind'] != 'chan':
                decodable = False
                continue
            decodable = (decodable and _decodable(sub['info'])
                         and sub.get('hsf_ext_substream_index') is None)
            out.append({'role': role, 'classifier': _classifier(group['content_type']),
                        'language': _language(group['content_type']),
                        'substream_index': sub['info'].get('substream_index'), 'group': ref,
                        'position': position, 'ch_mode': sub['info'].get('ch_mode')})
    return out, decodable and bool(out)


def selectable(toc, p, level):
    """Whether a decoder of compatibility level `level` may select presentation `p`."""
    _, decodable = members(toc, p)
    if not decodable or p.get('enable_presentation') is False:
        return False
    if not 0 <= p.get('presentation_version', 0) <= 2:
        return False
    config = p.get('presentation_config')
    if config is not None and not 0 <= config <= 5:
        return False
    md_compat = p.get('md_compat')
    if md_compat is None:
        return False
    reserved = range(4, 7) if toc['substream_groups'] is not None else range(5, 7)
    return md_compat not in reserved and md_compat <= level


def _primary(tag):
    return tag.split('-', 1)[0].lower()


def _language_rank(found, wanted):
    if not wanted or not found:
        return 0
    if found.lower() == wanted.lower():
        return 2
    return 1 if _primary(found) == _primary(wanted) else 0


def _service(found):
    """(classifier, tag) of the associated service a presentation carries, or None: its
    associated substream's, or its main substream's where that is classified as associated audio
    (Table 54) or carries a Table 92 code, the service premixed."""
    for m in found:
        if m['role'] == 'associated':
            return m['classifier'], m['language']
    for m in found:
        if m['role'] in ('main', 'music_effects'):
            codes = {code for pair in TABLE_92.values() for code in pair}
            if TABLE_54.get(m['classifier']) == 'associated' or m['language'].lower() in codes:
                return m['classifier'], m['language']
            break
    return None


def _presentation_language(found):
    for m in found:
        if m['role'] == 'dialogue' and m['language']:
            return m['language']
    for m in found:
        if m['role'] in ('main', 'music_effects') and m['language']:
            return m['language']
    return ''


def select_presentation(toc, choice, level):
    """The index into toc['presentations'] a decoder of level `level` selects for `choice`, a
    dict with presentation_id, index, language, associated, associated_type ('any' or a key of
    TABLE_92) and headphones; None when it can select none."""
    best = None
    best_rank = None
    by_id = None
    by_index = None
    for i, p in enumerate(toc['presentations']):
        if not selectable(toc, p, level):
            continue
        if by_id is None and choice.get('presentation_id') is not None \
                and p.get('presentation_id') == choice['presentation_id']:
            by_id = i
        if choice.get('index') == i:
            by_index = i
        found, _ = members(toc, p)
        service = _service(found)
        wanted = choice.get('associated')
        if wanted is None:
            associated = 1 if service is None else 0
        else:
            kind = choice.get('associated_type', 'any')
            associated = 1 if (service is not None and service[0] == wanted
                               and (kind == 'any' or service[1].lower() in TABLE_92[kind])) else 0
        language = _language_rank(_presentation_language(found), choice.get('language') or '')
        virtualized = 1 if bool(p.get('b_pre_virtualized')) == bool(choice.get('headphones')) else 0
        rank = (language, associated, virtualized)
        if best_rank is None or rank > best_rank:
            best, best_rank = i, rank
    if by_id is not None:
        return by_id
    if by_index is not None:
        return by_index
    return best


class PresentationName:
    """An alternative presentation's name, gathered frame by frame from its presentation
    substream's presentation_name (Part 2 clauses 6.3.3.1.2 to 6.3.3.1.4), as
    src/ac4dec/ERRATA.md's "A presentation name in chunks" reads the clause: a field whose last
    byte is 0 holds the whole name; else one whose second-last byte is 0 is the last chunk, its
    last byte the number of chunks, and the name is that many chunks of consecutive frames ending
    with it; a chunk before the last is all name. A name ends at its first zero byte. A frame
    without a name, a count the chunks do not make, and a change of source drop the chunks
    gathered; the last whole name stays."""

    def __init__(self):
        self.name = ''
        self._chunks = []

    @staticmethod
    def _text(data):
        return bytes(data).split(b'\x00', 1)[0].decode('utf-8', errors='replace')

    def frame(self, data):
        """One frame of the presentation substream: its presentation_name bytes, or None when
        the frame sends no name."""
        if not data:
            self._chunks = []
            return
        if data[-1] == 0:
            self.name = self._text(data[:-1])
            self._chunks = []
        elif len(data) >= 2 and data[-2] == 0:
            chunks = [*self._chunks, bytes(data[:-2])]
            self._chunks = []
            if len(chunks) == data[-1]:
                self.name = self._text(b''.join(chunks))
        else:
            if len(self._chunks) == 255:  # no count reaches further
                self._chunks = []
            self._chunks.append(bytes(data))

    def forget(self):
        """A change of source."""
        self.name = ''
        self._chunks = []
