# AC-4 decoder: errata and readings

The places where ETSI TS 103 190-1 V1.4.1 (Part 1) and ETSI TS 103 190-2 V1.3.1 (Part 2) contradict
themselves, leave a case open or print a defect, and the reading this decoder takes for each. Page
numbers are the printed ones, which equal the PDF page numbers; tables whose extracted text was unclear
were read on a rendering of the page.

The syntax is transcribed twice: in C++ here, and in Python in `tools/references/ac4_syntax.py`,
written separately from the text. Where an entry says "both", the two transcriptions take the same
reading. The evidence for a reading is one of:

- **Streams**: encoded streams reach the syntax. Both transcriptions read every frame of the 107 local
  census streams (50,728 frames of DEE 6.5.4 output), of the committed streams, and of the public
  channel-based streams from DASH-IF, CTA WAVE and Chromium (6,670 frames from other Dolby encoders) to
  the end of every substream, apart from the refused 5.1.4 audio, with every size check holding, and
  their traces agree.
- **Text**: no stream here reaches the syntax. Both transcriptions take the reading, and their traces
  agree where the differential check (see the end of this page) reaches it, which shows they read it
  alike; whether the reading is the intended one rests on the text.
- **Observation**: the text says nothing; the encoded streams decide.

Later phases add the readings their processing needs.

## Table of contents and presentations

### presentation_config 1 and 4 read more specifiers than n_substream_groups

- **Where:** Part 2 6.2.1.3, p. 115.
- **Text:** "Main + DE" (1) reads two `ac4_sgi_specifier()` and sets `n_substream_groups = 1`; "Main +
  DE + Associated Audio" (4) reads three and sets 2.
- **Reading:** the specifiers are read as the syntax says. `n_substream_groups` keeps the assigned value
  where it is used as a count (the `sg_gain` loop of `ac4_presentation_substream()`), while
  `pres_ch_mode`, `pres_ch_mode_core`, `n_substreams_in_presentation` and the other presentation helpers
  are taken over every group the specifiers name. Pseudocode 25 loops to `n_substream_groups`, which
  would leave out the associated audio of configuration 4; clauses 6.3.3.1.29 to 6.3.3.1.31 define the
  helpers over "all substreams in the presentation".
- **Evidence:** Text. The inspector (`src/ac4`) and the Python parser had read one specifier too few;
  `tests/ac4/test_ac4_presentation_configs.cpp` builds both configurations.

### presentation_version 2 is read as immersive stereo

- **Where:** Part 2 6.3.2.3.1, p. 158: a decoder "shall decode a presentation if its presentation version
  is 1 or 2". Nothing else in V1.3.1 defines version 2.
- **Observed:** every DEE immersive-stereo encode here (`dee_ac4ims_encoder`: the census's 22 and the
  three committed, at 23.976, 24, 25 and 29.97 fps) is presentation_version 2 over one channel-coded
  substream whose `ac4_substream_info_chan()` carries `channel_mode` 0b1111000, which Table 56 (p. 160)
  gives to 7.0 (3/4/0). Read as 7.0, `metadata()` fails its `tools_metadata_size` check in I-frames, the
  presentation substream reads one bit past its end, and the audio fails in its section data. Read as
  stereo, every frame of every stream ends exactly.
- **Reading:** in a presentation_version 2 presentation, a channel-coded substream with that code is
  stereo wherever its channel mode is used: its channel element, its `metadata()` and the
  presentation's `pres_ch_mode`.
- **Evidence:** Observation.

### The frame rate factor of a substream group

- **Where:** Part 2 6.2.1.8, p. 118: `ac4_substream_info_chan()` loops `b_audio_ndot` over
  `frame_rate_factor`, which `frame_rate_multiply_info()` sets per presentation, while the substream
  groups are read after every presentation.
- **Reading:** every group takes the factor of the first presentation that transmits
  `frame_rate_multiply_info()`; an EMDF-only presentation (configuration 6) transmits none and is passed
  over. Both transcriptions read it so. The text leaves open which factor applies when presentations
  carry different ones, and no stream here does. With a factor above 1,
  `substream_index` names the first of that many consecutive substreams (Part 1 4.3.3.7.9, p. 79),
  and both transcriptions read each as an instance of its own, with that instance's `b_audio_ndot`.
- **The series, not the instance, is what carries state.** 4.3.3.5.3, p. 78, has the substreams of a
  series decoded consecutively, and 4.3.3.2.7, p. 74, fulfils `b_iframe_global` when the **first**
  `b_iframe` of a series of 2 or 4 is true, so a stream whose I-frames set only that first flag is legal.
  The configuration an I-frame of the series sends therefore serves the instances after it, and each
  instance predicts from the one before: one slot of carried state per series, the first instance's. A
  slot per instance leaves every instance after the first with a configuration no I-frame ever sent, and
  every frame of such a stream fails as missing its I-frame.
- **Each instance covers `frame_len_base / frame_rate_factor` samples.** Tables 83 and 87 leave no other
  reading: every (index, factor) pair Table 87 permits lands on another index's listed length - 2048 at
  25 fps doubled is 1024, the 50 fps entry; 1536 quadrupled is 384 - and the base length would put two
  or four frames' samples into one frame period. The length sets transform lengths and the widths taken
  from them (`max_sfb` among them), so an instance read at the base length is misread, not mis-scaled.
- **Evidence:** Streams for factor 1 (every stream here); Text above it. The differential check's
  synthetic frames carry factor 2 and 4, which is where the two transcriptions meet this path at all:
  with one side reading an instance at the base length, that check reports the `max_sfb` width
  differing.

### A frame rate the sample rate does not define

- **Where:** Part 1 Table 83, p. 76, gives `frame_len_base` for each `frame_rate_index` at 48 kHz;
  Table 84, p. 76, covers 44.1 kHz and defines index 13 alone, leaving every other index reserved there.
- **Reading:** such a frame has no frame length, so nothing in it that derives from one is read: every
  audio substream and the presentation substream are refused for a reserved `frame_rate_index`, rather
  than read with the 48 kHz length or read until a field that needs the length is reached.
  `ac4::samples_per_frame()` reads the pair the same way. Both transcriptions derive the length in one
  place, which is what keeps the audio and presentation substreams of a frame on the same value.
- **Evidence:** Text; every stream here is 48 kHz.

### The efficient high frame rate mode is refused

- **Where:** Part 2 5.1.3, p. 30, and Table 18: above 30 fps a presentation may transmit
  `frame_rate_fraction` 2 or 4, spreading one coded frame over that many `raw_ac4_frame()`s, each
  carrying fragments of the substreams; a decoder holds the partial frames and concatenates them.
- **Reading:** this phase reads no fragments, so a frame whose presentation carries a fraction above 1
  has every substream refused as unsupported, naming the mode. Reading a fragment as a whole substream
  reports a legal stream as a damaged one, which is what the decoder did before the fraction was carried
  out of the table of contents at all.
- **Evidence:** Text; no stream here uses the mode.

### A substream named by several elements

- **Where:** Part 1 Table 15, p. 33, and Part 2 Table 50, p. 123: the element that names a substream's
  index decides which syntax the substream holds. Nothing forbids two elements naming one index.
- **Reading:** a substream is read once per frame, as the first element names it, in this order: every
  presentation's EMDF payload substreams and presentation substream, then the substream groups in the
  order the presentations reference them. A stream that names one substream twice is malformed; the
  order only makes the two transcriptions read such a stream alike.
- **Evidence:** Text; no stream here names a substream twice.

### A substream group named twice by one presentation

- **Where:** Part 2 6.2.1.3, p. 115: a presentation reads its `ac4_sgi_specifier()` elements in turn, and
  nothing forbids two of them naming one `group_index`. Clauses 6.3.3.1.29 to 6.3.3.1.31, p. 166, define
  `pres_ch_mode`, `n_substreams_in_presentation` and the other helpers over the substreams in the
  presentation.
- **Reading:** a group named twice holds the same substreams both times, so it counts once, in the
  helpers and in the substream assignment alike. `n_substream_groups`, which 6.2.1.3 assigns and the
  `sg_gain` loop of `ac4_presentation_substream()` uses as a count, keeps the value the clause gives it.
- **Evidence:** Text; no stream here names a group twice. Counting per reference also made a frame that
  names one group thousands of times cost one walk of that group per reference, which is a way to make a
  40 KB frame take a billion iterations.

### A change of source

- **Where:** Part 1 4.3.3.2.2, p. 72: a frame continues the stream when its `sequence_counter` is the
  previous frame's plus 1, wraps from 1020 to 1, or follows a 0, which a splicing device writes into the
  first frame after a splice; anything else is a change of source, and a decoder bridges the gap "until
  the next independently decodable frame".
- **Reading:** at a change of source, everything carried between frames is forgotten: I-frame
  configuration, A-SPX offsets and borders, DRC and dialogue enhancement state. A frame that needs
  configuration before the next I-frame fails as missing its I-frame. Only a frame whose table of
  contents holds together counts as the predecessor of the next: a frame whose substream sizes run past
  it leaves the counter where it was, so the frame after it reads as a change of source. The text does
  not say whether a counter transmitted in an unreadable frame still counts, and forgetting what such a
  frame might have carried is the safer half of the choice.
- **Evidence:** Streams: DEE starts counting at 1019, so every stream here passes the wrap to 1 in its
  third frame. `tests/ac4dec/test_ac4dec_decoder.cpp` checks a jump and a 0. Text for the frame that
  does not parse.

### oamd_common_data() has two call sites; only one is read

- **Where:** Part 2 §6.2.8.1, embedded by `b_oamd_common_data_present` in both `ac4_substream_info_ajoc()`
  (§6.2.1.9, a TOC-level element) and `oamd_substream()` (§6.2.2.4, an A-JOC/object substream's own DATA
  content).
- **Reading:** only the first is read, at the TOC level (`ac4::`/`ac4_parse.py`), since a wrong reading
  there desyncs every substream after it - correctness here is what the differential check and
  `tests/ac4/test_ac4.cpp`'s synthetic vectors can hold to. The second, inside `oamd_substream()`, is not:
  no A-JOC/object substream's DATA is decoded at all yet, so an independent second transcription of the
  same element there would have nothing to cross-check it against.
- **Evidence:** Text; no stream here (nor Chromium's public A-JOC test file, not re-checked since) reaches
  the TOC-level occurrence with non-trivial content - see docs/verification.md's AC-4 section.

### bits_used from trim()/bed_render_info()/headphone() is measured, not returned

- **Where:** Part 2 §6.2.8.1: `bits_used = trim(); add_data_bits = add_data_bits - bits_used;` and the
  same for `bed_render_info()` and `headphone()`, three calls whose own syntax tables (§6.2.8.8, 6.2.8.9,
  6.2.8.9a) read fields in the ordinary way and state no return value.
- **Reading:** `bits_used` is the reader position immediately after the call minus the position
  immediately before it - what each function actually read, not a quantity it computes and returns.
- **Evidence:** Text.

### An add_data budget a nested element overruns fails the substream

- **Where:** Part 2 §6.2.8.1: `add_data_bits = add_data_bits - bits_used`, unguarded, for all three of
  `trim()`, `bed_render_info()` and `headphone()`; `add_data_bits` then sizes the final `add_data` read.
  The text does not say what a `bits_used` bigger than the remaining `add_data_bits` means.
- **Reading:** a failure: `trim()`/`bed_render_info()`/`headphone()` reading more than `add_data_bytes`
  budgeted them is possible only on a malformed stream (a real encoder sizes `add_data_bytes` to fit
  exactly what it wrote), and letting `add_data` or the fields after `oamd_common_data()` be read from a
  position the budget never actually reserved for them would misparse rather than fail.
- **Evidence:** Text; `tests/ac4/test_ac4.cpp` covers it with a `trim()` sized past an 8-bit budget.

## Substream framing

### ac4_substream() byte alignment after audio_size

- **Where:** Part 1 Table 16, p. 33, has a `byte_align` after the `audio_size` header; Part 2 6.2.2.2,
  p. 123, has none.
- **Reading:** either; the header is 16 bits plus 8 for each `variable_bits(7)` group, so the alignment
  reads nothing.

### audio_size covers the fill

- **Where:** Part 1 4.3.4.1, p. 82.
- **Reading:** `metadata()` starts at the first bit of `audio_data()` plus 8 × `audio_size`; what lies
  between the end of `audio_data()` and that point is `fill_bits` and `byte_align`, and `audio_data()`
  reaching past it is a failure.
- **Evidence:** Streams. In every audio substream read, `audio_data()` ended 0 to 7 bits short of that
  point.

### byte_align is relative to the substream

- **Where:** Part 1 4.3.1.3, p. 71: alignment is "relative to the start of the enclosing syntactic
  element".
- **Reading:** every `byte_align` aligns to the start of the substream, including the one that ends an
  `emdf_payloads_substream()` nested in `metadata()`, which starts at an arbitrary bit.
- **Evidence:** Streams. The immersive-stereo streams carry an EMDF payload in `metadata()` in every
  second frame, and every such substream ends exactly.

## ASF

### sect_sfb_offset at max_sfb

- **Where:** Part 1 Pseudocode 4, p. 90, defines `sect_sfb_offset[g][sfb]` for `sfb < max_sfb`, while
  `asf_spectral_data()` (Table 40, p. 46) reads it at `sect_end == max_sfb`.
- **Reading:** the same formula at `sfb == max_sfb`, which is also the next group's start.
- **Evidence:** Streams.

### sf_info_lfe() below 1536 samples

- **Where:** Part 1 Table 35, p. 42, sets `b_long_frame = 1` with the comment "transform length =
  frame_length" and never sets `transf_length`; Pseudocode 2, p. 87, returns `transf_length` when
  `frame_len_base` is below 1536.
- **Reading:** the LFE transform covers the frame: the index of the whole-frame transform (3 for 1024,
  960 and 768 samples; 2 for 512 and 384).
- **Evidence:** Text.

### n_sect_bits below 1536 samples

- **Where:** Part 1 Table 39 and Pseudocode 6, p. 91, choose 3 or 5 bits by comparing the
  `transf_length` index with 2.
- **Reading:** as written: index 2 gives 3 bits even where it is the whole-frame transform (512 and
  384 samples).
- **Evidence:** Text.

### get_max_sfb() with b_dual_maxsfb

- **Where:** Part 1 Pseudocode 5, p. 91, returns `max_sfb_side` only "when decoding the side channel".
- **Reading:** in stereo ASPX_ACPL_1 with `b_enable_mdct_stereo_proc`, the `chparam_info()` uses
  `max_sfb`, and the second `sf_data()` uses `max_sfb_side`.
- **Evidence:** Text.

### ext_code is at most 21 bits

- **Where:** Part 1 Pseudocode 20, p. 141, reads leading ones without a bound; Table 40, p. 46, gives
  `ext_code` "5…21" bits.
- **Reading:** the escape is `2 * N_ext + 5` bits, so N_ext is at most 8 and a magnitude at most 8191. A
  ninth leading one fails the substream, and nothing is recorded for the escape.
- **Evidence:** Text. `fuzz_ac4_decode` found the unbounded loop within seconds: a shift past 32 bits.

### Section data outside its range

- **Where:** Part 1 4.3.6.3.1, p. 91: `sect_cb` 12 to 15 "shall not be used"; Table 40's loop would pass
  over them.
- **Reading:** a `sect_cb` of 12 to 15, and a section that ends beyond `max_sfb`, fail the substream. The
  two transcriptions detect these at different elements, which changes only where a corrupt
  substream's trace stops.

### asf_section_data()'s max_sfb, with an active HSF extension

- **Where:** Part 1 Table 39, p. 45, sets `max_sfb = get_max_sfb(g)`. Its own section-splitting branch,
  two lines later (`if (sect_end[g][i] > num_sfb_48(transf_length_g)) { ... }`), can only trigger when
  the loop reads past `num_sfb_48`, which `get_max_sfb(g)` alone never permits: 4.3.6.2.2 gives
  `max_sfb[i]` a ceiling of `num_sfb` (`num_sfb_48`, at this call), so `get_max_sfb(g) <= num_sfb_48`
  always holds. Tables 42a to 42c (`asf_hsf_spectral_data()`, `asf_hsf_scalefac_data()`,
  `asf_hsf_snf_data()`, pp. 48 and 49), read from this channel's `ac4_hsf_ext_substream()`, depend on
  `num_sec_lsf[g] < num_sec[g]` and on `sfb_cb[g][sfb]`/`sect_sfb_offset[g][sfb]` being set for `sfb` up
  to `get_max_sfb_hsf(g)` (4.3.16.2, p. 138) - reachable only if `asf_section_data()` itself reads that
  far.
- **Reading:** `max_sfb` in Table 39's own pseudocode is `get_max_sfb_hsf(g)`, not `get_max_sfb(g)`,
  whenever this channel's HSF extension is active (its `ac4_hsf_ext_substream_info()` links a substream,
  and this channel's own `sf_multiplier` is set - Part 2 Table 89, p. 78). `asf_spectral_data()`,
  `asf_scalefac_data()` and `asf_snf_data()` (Tables 40 to 42) are unaffected: their own `get_max_sfb(g)`
  and `min(get_max_sfb(g), num_sfb_48(...))` calls keep the core-only reading, which is what makes a
  channel with no active extension unaffected byte for byte by touching the section loop at all.
- **Evidence:** Text; no stream here uses the mode.

### ac4_hsf_ext_substream()'s max_sfb_ext_hsf and num_channels

- **Where:** Part 1 Table 17, p. 34: `max_sfb_ext_hsf[0]` and, `if (b_different_framing)`, `[1]` are read
  once, before a `for (ch = 0; ch < num_channels; ch++) { sf_hsf_data(); }` loop. Neither
  `b_different_framing` nor `num_channels` is defined in this substream's own syntax; both are properties
  `sf_info()`/`asf_psy_info()` (4.2.7.1, 4.2.8.2) set once per track of the *owning* channel-coded
  substream's own element (`single_channel_element`, `channel_pair_element`, and so on, 4.2.6) - most of
  which read one shared `sf_info()` for every track (3_0, 5_X, 7_X), leaving only a `channel_pair_element`
  without `b_enable_mdct_stereo_proc` able to hold two, one per track.
- **Reading:** `num_channels` is the owning element's own track count, in the order its `sf_data()` calls
  are made (`mono_data()`, `stereo_data()`, `two_channel_data()`, and so on) - the same order and count
  `sf_hsf_data()`'s loop needs to match, LFE and ASPX_ACPL_1 residual tracks included. `b_different_framing`
  is the first of those tracks' own value: every track's `sf_info()` (hence its own `b_different_framing`)
  is read before the element's *first* `sf_data()` call, the point `asf_section_data()` first needs
  `max_sfb_ext_hsf` - the first track's is the only one available to size this header when it must be
  read. A later track whose own `b_different_framing` calls for `max_sfb_ext_hsf[1]` where the first
  track's did not read one takes it as 0: no additional bands for that track's own second half, rather
  than a failure.
- **Evidence:** Text; no stream here uses the mode.

## Channel elements

### Configuration belongs to the codec mode it was sent for

- **Where:** Part 1 Tables 20 to 33: each element reads `aspx_config()` and `acpl_config_1ch()` or
  `acpl_config_2ch()` in I-frames, for the codec mode that frame signals; nothing says what a later frame
  in another codec mode uses.
- **Reading:** an I-frame replaces all of an element's configuration, and a frame whose codec mode (or
  element) differs from the last I-frame's, and needs configuration, fails as missing its I-frame. The
  A-SPX offsets and borders carried per A-SPX element position are kept across I-frames of the same
  element and codec mode, and forgotten when either changes.
- **Evidence:** Streams for unchanging modes; Text for a change.

### ASPX_ACPL_1: the framing of the residuals

- **Where:** Part 1 Tables 25 (5_X, pp. 38 and 39) and 33 (7_X, pp. 41 and 42); 4.3.5.13, p. 84.
- **Text:** ASPX_ACPL_1 reads `max_sfb_master`, two `chparam_info()` and two `sf_data(ASF)`. The notes
  give `max_sfb_master`'s width, n_side_bits of "the largest signalled transform length from" the
  channel data above ("for coding_config == 0, this depends on which channel pair the additional
  channels are derived from"), and 4.3.5.13 maps it to each block's `max_sfb` through Tables B.8 to
  B.19. Nothing names the `sf_info()` whose windows and groups the four elements follow.
- **Reading:** residual *i*, and the `chparam_info()` before it, follow the framing of the track that
  the residual's A-CPL module pairs it with:
  - 5_X: tracks A and B of the channel data (Table 181, p. 180), which Pseudocode 117 (p. 240) pairs
    with the residuals and 5.3.4.3.2's matrix combines with them;
  - 7_X: Table 202 (p. 242) and Pseudocode 120: L and R (tracks A and B) for 5/2/0 and 3/2/2 with
    `add_ch_base` 0; Ls and Rs (tracks D and E) for 3/4/0, and for 5/2/0 and 3/2/2 with `add_ch_base` 1.
    Table 182 (p. 181) places A, B, D and E among the tracks by `coding_config` and `2ch_mode`.

  `max_sfb_master`'s width is n_side_bits of the largest transform length the two tracks' `sf_info()`
  signal. A window group of that length takes `max_sfb_master` as its `max_sfb`; a shorter one takes the
  `n_sfb_side` value for its length.
- **Why:** a residual is combined with its partner track band by band, which needs the two to share
  their framing, and the partner's framing is the only one the syntax has sent at that point.
- **Evidence:** Text. The two transcriptions had first taken different readings (the first channel
  data `sf_info()`, and the one holding the largest transform length); neither matched Tables 181 and
  202.

### b_use_sap_add_ch: the framing of its chparam_info()

- **Where:** Part 1 Table 33, p. 41: the two `chparam_info()` come before the `two_channel_data()` that
  carries the additional channels' own `sf_info()`, yet `chparam_info()` needs `num_window_groups` and
  `get_max_sfb()`.
- **Reading:** each follows the framing of the track Table 183 (p. 182) codes its additional channel
  against: F and G against D and E (Ls and Rs) for 3/4/0, and against A and B (L and R) for 5/2/0 and
  3/2/2. `add_ch_base` plays no part here; Table 183 does not use it.
- **Evidence:** Text. As above, the two transcriptions had first differed.

## A-SPX

### aspx_num_rel_right cites the wrong note

- **Where:** Part 1 Table 53, p. 55: in the VARVAR case `aspx_num_rel_right` cites "Note 2" (the
  float-division note); its width prints "2 (1)".
- **Reading:** Note 1: 1 bit when `num_aspx_timeslots` is 8 or fewer, else 2, as 4.3.10.4.7 (p. 101)
  says for every interval class.
- **Evidence:** Text (no stream here uses VARVAR).

### aspx_ec_data() takes the derived frequency resolution

- **Where:** Part 1 Tables 51 and 52, pp. 53 and 54, pass `aspx_freq_res[ch]`, which `aspx_framing()`
  (Table 53) sends only when `aspx_freq_res_mode` is 0, and for FIXFIX only for the first envelope.
- **Reading:** each envelope's resolution is `atsg_freqres` from Pseudocodes 76 and 77 (pp. 210 and 211):
  FIXFIX copies the first envelope's to every envelope; mode 1 gives low and 3 high; mode 2 compares each
  envelope's length with `num_aspx_timeslots/6.0 + 3.25` (in integers, `12*length > 2*num_aspx_timeslots
  + 39`). Mode 2 outside I-frames needs `previous_stop_pos` from the previous frame, kept per channel and
  per A-SPX element position.
- **Evidence:** Streams for FIXFIX, FIXVAR and VARFIX (DEE uses mode 2 throughout); Text for VARVAR.

### aspx_balance takes the first channel's framing

- **Where:** Part 1 Table 52, p. 54: with `aspx_balance` 1, `aspx_framing(1)` is not read, while
  `aspx_delta_dir(1)` and channel 1's `aspx_ec_data()` loop over its envelope counts.
- **Reading:** channel 1 takes channel 0's framing (interval class, envelope and noise counts, borders,
  `aspx_tsg_ptr`, resolutions and `aspx_qmode_env`), and its `previous_stop_pos` becomes channel 0's. It
  still reads its own `aspx_delta_dir(1)`, and its data with the balance codebooks. Clause 5.7.6.3.5 says
  the time envelopes "are identical for the channels".
- **Evidence:** Streams (balance is set in 5,943 census `aspx_data_2ch()` elements).

### A stray brace in aspx_hfgen_iwc_2ch()

- **Where:** Part 1 Table 56, p. 57: the `for` in the `aspx_fic_right` branch opens a brace that nothing
  closes, which, counted literally, puts the time-interleaved section inside `aspx_fic_present`.
- **Reading:** the brace is stray; `aspx_tic_present` is read whether or not `aspx_fic_present` is set,
  as the page's indentation and Table 55 show. The same table lacks a semicolon after
  `aspx_tna_mode[1][n] = aspx_tna_mode[0][n]`.
- **Evidence:** Streams.

### Counts computed exactly

- **Where:** Part 1 Pseudocode 70, p. 206 (`num_sbg_noise`), and Table 53's Note 2 (`ptr_bits`).
- **Reading:** both in integers: `num_sbg_noise` as the largest k with
  `2^(2k-1) * sbx^(2*aspx_noise_sbg) <= sbz^(2*aspx_noise_sbg)` (at least 1), and `ptr_bits` as the bit
  width of `aspx_num_env + 1`. A floating-point evaluation agrees on all 1,808 reachable
  `num_sbg_noise` settings, the closest to a rounding boundary being 0.003 away.

### Values the A-SPX syntax cannot follow

- **Where:** Part 1 Pseudocode 68, p. 204; Table 128 and 4.3.10.1.9, p. 101.
- **Reading:** failures: an `aspx_xover_subband_offset` at or beyond `num_sbg_master` (the master table
  would be indexed past its end, or leave no subband group), more than five noise subband groups
  (5.7.6.3.1.3), and more envelopes than Table 128 allows (four for FIXFIX, five otherwise).

### aspx_tsg_ptr for FIXFIX

- **Where:** Part 1 Table 53 sends no `aspx_tsg_ptr` for FIXFIX, while Pseudocodes 92 and 95 (5.7.6.4.2)
  compare envelopes with it for every interval class.
- **Reading:** -1, the value a transmitted 0 gives, which points at no envelope: no transient. Pseudocodes
  92, 95 and 99 then treat no envelope as a transient's, and add sinusoids from the first.

### Stray semicolon in the limiter's patch borders

- **Where:** Part 1 Pseudocode 72, p. 207: `for (sbg = 1; sbg < num_sbg_patches; sbg++);` before its
  block.
- **Reading:** the block is the loop's body, copying the interior patch borders, which matches
  `num_sbg_lim = num_sbg_sig_lowres + num_sbg_patches - 1`. Not syntax: the limiter
  (`src/ac4core/src/aspx/frequency_tables.cpp`) takes it.

### freq_res_prev in Pseudocode 80

- **Where:** Part 1 Pseudocode 80, p. 214: `atsg_freqres[num_atsg_sig_prev - 1]`, with two unbalanced
  parentheses.
- **Reading:** the previous interval's resolution vector, which the paragraph after the pseudocode names
  `freq_res_prev`: its last envelope's resolution, which maps the first envelope's time deltas between
  resolutions. Not syntax: the envelope decoding (`src/ac4dec/src/pcm/aspx.cpp`) takes it.

## A-CPL

### Partial coupling starts at acpl_param_band

- **Where:** Part 1 Pseudocode 121, p. 244, against `acpl_huff_data()` (Table 65, p. 61): the table sends
  bands from `start_band`; the pseudocode's frequency and time differencing run from band 0.
- **Reading:** the syntax as Table 65 writes it; the pseudocode's differencing starts at `start_band`
  when `acpl_param_band` is not 0 (PARTIAL mode). Not syntax; recorded for the A-CPL phase.

### Codebook offsets

- **Where:** Part 1 Tables 58 and 65.
- **Reading:** the trace records codebook indices before `cb_off`. Dequantisation subtracts `cb_off` for
  A-CPL's F0 codebooks and every DF and DT codebook, and not for A-SPX's F0 codebooks, whose tables print
  none.

## Metadata, DRC and dialogue enhancement

### drc_gainset_size does and does not count drc_version

- **Where:** Part 1 4.3.13.5.1, p. 130 ("the size in bits of the following drc_gains element"), against
  Table 74, p. 67 (`bits_left = drc_gainset_size - 2 - used_bits`).
- **Reading:** for `drc_version` 1 or more, the formula decides how many `drc2_bits` are read. For
  version 0 nothing is read by the size; both `used_bits + 2` and `used_bits` are accepted, and anything
  else fails as a misread.
- **Evidence:** Text (DEE sends DRC curves, never gains).

### b_associated and b_dialog are parameters at sus_ver 0

- **Where:** Part 2 6.2.7.4's note, p. 139; Part 1 4.3.12.4.1 and 4.3.12.4.2, p. 118; Table 158a,
  p. 119.
- **Reading:** at sus_ver 0 neither is read. `b_associated` is set for a substream whose
  `content_classifier` is 0b010, 0b011 or 0b101, or which is the associated audio substream of a
  presentation_config 2, 3 or 4 presentation; read literally, every substream of such a presentation,
  the main one included, would be "associated". `b_dialog` is set for the dialogue substream of
  presentation_config 0 or 3, or for `content_classifier` 0b100. At sus_ver 1, which bitstream_version 2
  implies, `b_dialog` is a field.
- **Evidence:** Streams for sus_ver 1; Text for sus_ver 0.

### drc_gains() is a brace short

- **Where:** Part 1 Table 75, p. 67: the band loop has no opening brace, yet a closing one follows the
  reference reset.
- **Reading:** the indentation's: the band loop holds the subframe loop and the reset. The number of
  `drc_gain_code` reads, channels × bands × subframes − 1, is the same under any placement.

### drc_repeat_id copies a whole mode

- **Where:** Part 1 Table 72, p. 65; 4.3.13.3.5, p. 124.
- **Reading:** a repeated mode takes the default profile flag, the compression curve flag,
  `drc_gains_config` and the curve of the mode it names, and keeps its own output levels. A repeat of a
  mode the same `drc_config()` has not yet configured is a failure. The syntax copies only the curve
  flag, but without `drc_gains_config` a repeated gains mode has no band count to read by.
- **Evidence:** Streams (a census stream with DEE's DRC options repeats earlier modes).

### nr_drc_channels for modes Table 168 leaves out

- **Where:** Part 1 Table 168, p. 131, lists mono, stereo, 5.1 and the 7.1 modes; Part 2 Table 69,
  p. 170, the immersive modes and 22.2.
- **Reading:** a mode without LFE takes its twin's count (3 for 5.0 and the 7.0 modes), since Table 69
  puts the LFE in a group "in case they are present". 3.0 has no twin, and a presentation with no
  channel mode has no count: channel-dependent gains there are refused as unsupported.
- **Evidence:** Text.

### oamd_dyndata_single() in metadata() of a channel-coded substream

- **Where:** Part 2 6.2.7.1, p. 135, reads it when `b_alternative` is set and `b_ajoc` is 0, which holds
  for a channel-coded substream of an alternative presentation; its `n_objs` and object types exist only
  for object substreams.
- **Reading:** a channel-coded substream never carries it. Table 7 places OAMD dynamic data only in
  object audio substreams that are not A-JOC coded.

### de_data() predicts from the wrong channel

- **Where:** Part 2 6.2.7.6, p. 140: `ref_val = de_par[0][band]` for channels after the first; Part 1
  Table 78, p. 69, writes `de_par[ch][band]`.
- **Reading:** Part 1's. The codewords read do not depend on it, only the parameter values, so the trace
  is unaffected. A DEE stream with `de_channel_config` 6 and `de_ms_proc_flag` 0 takes this path in every
  I-frame; the phase that applies dialogue enhancement checks the reading there.

### Dialogue enhancement and DRC configuration across I-frames

- **Where:** Part 2 4.5.2, pp. 35 and 36; Part 1 Tables 70 and 76; 4.3.14.5.3, p. 133.
- **Reading:** an I-frame replaces the stored `drc_config()` and `de_config()` when it carries them and
  clears them when `b_drc_present` or `b_de_data_present` is 0; a later frame that needs a configuration
  none holds fails as missing its I-frame. The simulcast `de_data()` keeps its own time-differential
  state. `de_par_prev` is zeroed for channels a frame does not code.

### The end of an EMDF payload list

- **Where:** Part 1 4.3.15.1.1, p. 134, against Table 18, p. 34.
- **Reading:** the syntax's: `while (emdf_payload_id != 0)` reads the 5-bit id as its test, and an id of
  0 ends the list with nothing after it but `byte_align`; the semantics' fields "set to 0" are not read.

### further_loudness_info() at sus_ver 0

- **Where:** Part 2 6.2.7.3, p. 138, against Part 1 Table 68, p. 63.
- **Reading:** Part 2's, which reads `b_rtllcomp` and `rtll_comp` inside the extension; an `e_bits_size`
  too small to hold them is a failure.
- **Evidence:** Text (bitstream_version 2 implies sus_ver 1).

### basic_metadata() across the page break

- **Where:** Part 2 6.2.7.2, pp. 135 and 136.
- **Reading:** the rendered page 136 closes the `sus_ver == 0` block after `preferred_dmx_method`, so the
  5.X and 7.X blocks, `phase90_info_mc`, `b_surround_attenuation_known` and `b_lfe_attenuation_known`
  are read at both substream versions, although the extracted text's indentation suggests otherwise.
  "channel_mode == 5_X" is ch_mode 3 or 4, "7_X" 5 to 10, "3/4/0" 5 or 6, and "3/2/2" 9 or 10.
- **Evidence:** Streams for stereo and 5.1.

### The presentation substream

- **Where:** Part 2 6.2.2.3 to 6.2.2.5 and 6.3.3.1, pp. 124 to 172; 6.2.9, p. 152.
- **Readings:**
  - `superset(0, 1)` is 1, as 6.3.3.1.27 says, although its own rule would give 3.0. Six unordered pairs
    have no mode holding both: 5/2/0 and 5/2/0.1 each with 9.0.4 and 9.1.4, and 9.0.4 and 9.1.4 each with
    22.2 - the first of each pair brings Lw/Rw, the second Lscr/Rscr, and 22.2 has Lw/Rw without Lscr/Rscr.
    6.3.3.1.27 gives no result for them, and the reading taken is that there is none: `pres_ch_mode` is
    -1, so the presentation substream reads the fields that answer to a presentation with no single
    channel mode (`b_oamd_common_timing`, `custom_dmx_data()`'s `bs_ch_config` branch, `b_obj_loud_corr`).
    Naming the larger of the two instead would claim a layout the presentation does not have, and would
    drop the LFE of 5/2/0.1 against 9.0.4.
  - Table 72's conditions overlap; 2 wins when both hold.
  - `n_substreams_in_presentation` counts one per `ac4_substream_info_chan/_ajoc/_obj()`, whatever the
    frame rate factor; HSF extension substreams are not counted.
  - The advanced dialogue enhancement defaults are never given: an I-frame without the configuration, or
    without `advanced_de_data()`, clears it.
  - `advanced_de_compr_thresh`, "an integer" from -32 to 31, is 6-bit two's complement, the one signed
    coding Part 2 states (6.3.9.8.3); the trace records the raw code.
  - The syntax reads `b_tdc_extension` and `reserved_bits` where the semantics describe
    `tdc_extension`; the records use the syntax's names.
  - `if (3 <= bs_ch_config <= 4)` is the range 3 to 4; read as C it holds for every value.
  - `channel_mode_contains_TflTfr()` (Pseudocode 36, p. 183) is true for ch_mode 9 and 10 only, so the
    immersive modes carry no `b_tfl_active`; implemented as written.

### Misprints with no effect

- Part 1 Table 80, p. 70, is titled `emdf_reserved()` over a syntax headed `emdf_protection()`; one
  element.
- Part 2 Table 48, p. 111, cites Part 1 clauses 4.2.4.2 and 4.2.4.3 for `ac4_hsf_ext_substream` and
  `emdf_payloads_substream`, which V1.4.1 numbers 4.2.4.3 and 4.2.4.4.
- Part 1 Table B.2, p. 283, lists a 96 kHz transform length of 920 where the other tables have 960.
- Part 1 Pseudocode 21, p. 142, lacks the brace that closes `if (first_scf_found == 1)` before its
  `else`.
- Part 1 Table 213, p. 267, names the last pair of 7.X 3/2/2 (Lth, Rth); Table 88, p. 77, and Table 183,
  p. 182, name it Tfl and Tfr, as the decoder does.
- Part 1 5.1.4.2, p. 143, has the noise fill replace silent bands "if noise fill data is present as
  indicated when b_snf_data_exists is false", and the next sentence makes the tool inactive when it is
  false. It runs when `b_snf_data_exists` is true, the only case in which `asf_snf_data()` (Table 42,
  p. 48) reads any noise fill data.

## Reconstruction

The readings the decoding of clause 5 takes. Phase D2 of `planning/ac4.md` decodes the audio spectral
frontend, stereo processing, the inverse transform and frame alignment for mono and stereo in the SIMPLE
codec mode; later phases add theirs. The Python reference transcribes the syntax only, so these are the
decoder's readings alone, and the evidence for each is DEE's streams scored against their sources and
against librempeg (`docs/verification.md`, "The decoder's output"), the text, or a test against the
clause's formula.

### Full scale, and the overlap-add's factor of two

- **Where:** Part 1 5.5.2.2, p. 186: Pseudocode 62 divides by N, and Pseudocode 64 adds the windowed
  blocks as they are. The informative example after Table 187, p. 190, adds each block's windowed
  samples to the overlap buffer "using a factor of 2". Neither part says what sample value is full scale.
- **Reading:** Pseudocodes 60 to 64 as printed, with no factor of two, and full scale at 2^15: the
  decoder divides its output by 32 768. The two are one constant in the output, so the measurement
  below fixes their product, and this pair is the one that needs no factor the pseudocode does not
  print.
- **Evidence:** Streams. DEE's 2.0 tone leg (`ac4-20-tones-192`, a -20 dBFS sine on each channel,
  loudness measured only) decodes at 0.005 dB below its source, and the 2.0 music leg at 0.02 dB below;
  with the example's factor both would be 6.02 dB above. librempeg decodes both at the same level,
  within 0.001 dB of this decoder. Through the literal transform and a forward MDCT without scaling, a
  windowed round trip has a gain of 1/2 (`tests/ac4core/test_ac4core_dsp.cpp`), which is what a factor of
  two in the example would restore.

### KBD_RIGHT's argument

- **Where:** Part 1 5.5.2.2 step 6, p. 188, windows the previous block's second half with
  KBD_RIGHT(NW, n - Nskip) for Nskip <= n < NW + Nskip, an argument from 0 to NW - 1. 5.5.3, p. 189,
  defines KBD_RIGHT(N, n) for N <= n < 2N only.
- **Reading:** KBD_RIGHT(NW, NW + n - Nskip): the right half of the window at the same position, which is
  the left half reversed.
- **Evidence:** Streams, and the text's own condition. DEE switches block lengths in 31 of the 120 frames
  of the 2.0 music leg, which decodes at the SNR librempeg reaches. With this reading the windows meet
  the Princen-Bradley condition and blocks reconstruct their input to 1e-12 across every transition
  Table 187 allows (`tests/ac4core/test_ac4core_dsp.cpp`); the argument as printed lies outside the
  function's domain.

### The KBD kernel is summed to p = N

- **Where:** Part 1 5.5.3, p. 189, defines the kernel W(N, n, alpha) "for 0 <= n < N", and the sums in
  both KBD_LEFT and KBD_RIGHT run to p = N.
- **Reading:** the kernel's formula at n = N as well, which makes it a Kaiser window of N + 1 points,
  symmetric about N/2, with W(N, N) = W(N, 0).
- **Evidence:** Text. With the term at p = N the halves meet the Princen-Bradley condition exactly; the
  windows equal numpy's Kaiser window of N + 1 points, cumulated, to 1e-12
  (`tests/ac4core/test_ac4core_dsp.cpp`).

### The overlap buffer before the first block

- **Where:** Part 1 5.5.2.1, p. 185, describes `overlap` and `Nprev` as state carried from the previous
  block, and says nothing of their value before the first one.
- **Reading:** silence, and a previous block of full length, so that the first block takes its
  unmodified left window. A change of source (Part 1 4.3.3.2.2) starts from the same state.
- **Evidence:** Text; the first frame's output differs from a mid-stream decode only in the half block
  the missing predecessor would have filled.

### Pseudocode 59's stray block

- **Where:** Part 1 5.3.2, p. 174. After the branches for `sap_mode` 0, 1 and 2, Pseudocode 59 prints an
  `if (sap_used[g][sfb]) { ... } else { ... }` pair that sets a, b, c and d again, followed by an
  `else { // sap_mode == 3` with no `if` of its own. Taken as printed, the pair would reset every M/S
  band to the identity, since `sap_used` is only set in the `sap_mode` 3 branch.
- **Reading:** the pair is a stray copy of the end of the `sap_mode` 3 branch and belongs to no branch.
  `sap_mode` 0, and 1 where `ms_used` is 0, give the identity; 2, and 1 where `ms_used` is 1, give M/S
  (a = b = c = 1, d = -1); 3 gives the prediction of its own branch. `0.1f` is taken as written, a
  float. An `alpha_q` a later delta refers to, in a band `sap_data()` sent no coefficient for, is 0.
- **Evidence:** Streams. DEE's 2.0 streams use all three modes (the tone leg `sap_mode` 3 in 119 of 120
  frames, the music leg mostly 2), and decode at 50 dB (tones) and 35 dB (music) SNR against their
  source, the SNR librempeg reaches, with the two decoders' outputs 78 to 93 dB apart.

### Scale factors outside 0 to 255

- **Where:** Part 1 5.1.3.2, p. 142: "Only scale factor values sfn in the range 0 to 255 are valid".
- **Reading:** a scale factor that the deltas take outside that range fails the substream as
  `kInvalidStream`.
- **Evidence:** Text; no stream here does it.

### x = x++ in Pseudocode 57

- **Where:** Part 1 5.2.8.3, pp. 171 and 172: Pseudocodes 56 and 57 update the generator's state with
  `psS->uiStateIdx = psS->uiStateIdx++;` and, on a wrap, `psS->uiCurrentIdx = psS->uiCurrentIdx++;`,
  which C leaves undefined and C++17 makes a no-op.
- **Reading:** an increment.
- **Evidence:** Text. Pseudocode 24 gives the state after 255 x (sequence_counter mod 256) steps in closed
  form; stepping the generator from Pseudocode 55's state reaches that closed form at all 65,286 offsets
  with the increment and at 2 with the no-op. `tests/ac4dec/test_ac4dec_pcm.cpp` steps it for every
  counter. No stream here sets `b_snf_data_exists`, so no stream exercises the generator: not DEE's, the
  census's or the third-party ones.

### When the noise fill's generator starts

- **Where:** Part 1 5.1.4.2, p. 145: the generator "is initialized at the beginning of the decoding of an
  Audio Spectral Front end (ASF) frame, using the sequence_counter value".
- **Reading:** once for each audio substream in each frame, before its first `sf_data()`, and drawn from
  in the order the substream's `sf_data()` elements occur. Started again for every track, it would give
  every track of a frame the same sequence, since `sequence_counter` is the frame's, and the noise of the
  two channels of a pair would be the same noise at two levels.
- **Evidence:** Text; no stream here sets `b_snf_data_exists`.

### The LFE's track is not numbered in Tables 180 and 182

- **Where:** Part 1 5.3.4.3.0 and 5.3.4.4.0, pp. 180 and 181: Tables 180 and 182 number the tracks "according
  to the bitstream order of the channel data elements", five or seven of them, and name no LFE, though the
  5_X and 7_X elements read the LFE's `mono_data(1)` before any channel data (Tables 25 and 33).
- **Reading:** the tables count from the first track after the LFE's. The LFE's `mono_data(1)` gives the
  LFE channel by 5.3.3.1 (O0 = I0), and the LFE goes through the QMF banks with the other channels,
  untouched there: 6.2.10 leaves it out of A-SPX and Table 212 out of companding.
- **Evidence:** Streams. DEE's 5.1 streams from 192 to 768 kbps decode with each channel's tone on its own
  channel, the LFE's included, and agree with librempeg's decode to 83 dB channel by channel.

### The 7.X element's additional channels

- **Where:** Part 1 5.3.4.4.1, p. 181, and Table 183, p. 182: with `b_use_sap_add_ch`, a 2 x 2 matrix
  makes two channels of an output of one channel data element (D or E, or A or B) and one of the
  additional `two_channel_data()` (F or G), "after the creation of the preliminary outputs". The two come
  from different elements, each with its own `sf_info()`, and nothing makes their time/frequency tiles
  the same.
- **Reading:** the matrix applies tile by tile under the framing its `chparam_info()` was read with, the
  first input's ("b_use_sap_add_ch: the framing of its chparam_info()" above), to the two inputs' lines in
  window order after ungrouping: in each window, the bands below that framing's `max_sfb` for the window's
  group, at the window's own band offsets. The inputs must be transformed alike, window for window, and a
  frame whose inputs are not is refused as invalid. Bands above `max_sfb` are left as they are.
- **Why:** a tile of one input has no counterpart in the other unless their windows match, and window
  order is where the two elements' lines meet: in bitstream order each keeps its own grouping and
  `max_sfb`.
- **Evidence:** Text, and the constructed 7.X streams of `tests/ac4dec/ac4dec_constructed.cpp`, whose
  tracks are the channels through the inverse of Table 183's matrix and which decode with each tone on its
  channel. The encoder writes the element as an experimental option, with `b_use_sap_add_ch` 0, so its
  streams do not reach the matrix.

## The QMF domain

The readings phase D3 of `planning/ac4.md` takes for the QMF banks, companding and A-SPX decoding (Part 1
clause 5.7). They are the decoder's alone, as under "Reconstruction". The evidence is DEE's 2.0 legs at 48
to 144 kbps and its native-rate immersive stereo (IMS) legs in G0's gold set, scored against their sources
(`tools/checks/score_ac4_decode.py`), the text, or a test in `tests/ac4core/test_ac4core_aspx.cpp` and
`tests/ac4dec/test_ac4dec_aspx.cpp`. Across those 24 legs DEE sets the limiter, interpolation and
pre-flattening in every `aspx_config()`, uses FIXFIX, FIXVAR and VARFIX intervals, and never sets
`aspx_balance`, either interleaved waveform coding, `sync_flag` or VARVAR; the tests carry those.

### Every codec mode passes through the QMF banks

- **Where:** Part 1 6.2.8, p. 266, says the QMF analysis "is needed for the tools which operate in the QMF
  domain", and 5.7.1, p. 193, that the synthesis works on QMF data delayed by six QMF slots. Figure 9,
  p. 259, draws one chain for every substream.
- **Reading:** SIMPLE substreams pass through the analysis and synthesis banks too, behind the same
  history of `ts_offset_hfgen` slots that A-SPX keeps (Table 192), so the decoder has one delay for every
  codec mode: `d_pcm`, the banks' 577 samples and 6 x 64 samples, 1,313 at `frame_rate_index` 13. The
  banks reconstruct to 75 to 88 dB on tones and 78 dB on noise, which now bounds a SIMPLE decode's SNR.
- **Evidence:** Observation. DEE's SIMPLE and ASPX 2.0 streams decode with the same lag, 4,385 samples,
  and its output manifests give both the same MP4 offset; librempeg's output lags DEE's source by one
  delay, 3,649 samples, on SIMPLE and ASPX streams alike. DEE's IMS encoder runs one frame shorter: its
  streams lag by 2,337.

### Companding measures against full scale 1.0

- **Where:** Part 1 5.7.5.2, p. 198: the gain `L(ts)^((1 - alpha)/alpha)` depends on the scale of the
  slot level `L`, which the text does not state; A-SPX's signal scale factors (5.7.6.3.5) are absolute
  energies in the same QMF matrices.
- **Reading:** the QMF domain runs at the scale the inverse transform produces, full scale 2^15 ("Full
  scale, and the overlap-add's factor of two"), and companding divides its levels by 2^15 before the
  exponent.
- **Evidence:** Observation. DEE's 48 kbps 2.0 legs set `b_compand_on` in 470 of their 474 channel frames.
  Measured at 2^15 they decode 48.5 dB loud, which is (2^15)^(0.35/0.65), 48.6 dB; against full scale 1.0
  they decode within 0.17 dB of the source. A-SPX's envelopes read only at 2^15: its smallest signal scale
  factor, 64, is the energy of one least significant bit of white noise there, and of noise at 0 dBFS at
  full scale 1.0.

### The companding average

- **Where:** Part 1 5.7.5.2, p. 198. The average gain's exponent prints as "1alpha / alpha". `L_avg`'s sum
  runs from `ts0` to `ts1`, where the tool's range is `[ts0, ts1 - 1]`. With `sync_flag`, `g_synch(ts)`
  averages the channels' gains per slot, but the two channels of an `aspx_data_2ch()` without
  `aspx_balance` frame their intervals separately.
- **Reading:** the exponent `(1 - alpha)/alpha`, as for the per-slot gain; the average over `[ts0, ts1)`,
  divided by `ts1 - ts0`; with `sync_flag`, each slot averages the gains of the channels whose interval
  holds it, and each channel is scaled over its own interval.
- **Evidence:** Text for the exponent and the sum. The gold legs set `b_compand_avg` in 721 channel frames
  and never `sync_flag`.

### Companding's slots are Q_low's

- **Where:** Part 1 5.7.5.1, p. 197: the matrices hold "exactly those QMF time slots that are part of the
  A-SPX interval". The interval's borders count from slot 0 of Q_low, the analysis delayed by
  `ts_offset_hfgen` (5.7.6.3.2, 5.7.6.3.3.1), while companding runs before A-SPX (Figure 6).
- **Reading:** companding scales the delayed matrix over the interval's slots on Q_low's axis. A slot that
  an interval running past its frame's end holds is companded once, with that interval, and waits for the
  next frame companded.
- **Evidence:** Text: only on that axis does every slot an interval can hold, up to `num_qmf_timeslots +
  ts_offset_hfgen`, exist when the interval is decoded. Observation, slightly: DEE's companded speech legs
  at 48 and 64 kbps score 0.14 and 0.18 dB more SNR below the crossover this way than companded six slots
  earlier, on the analysis's own axis.

### The estimated envelope's time divisor

- **Where:** Part 1 Pseudocode 90, p. 220, sums `|Q_high|^2` over the QMF slots from `tsa` to `tsz` and
  divides by `atsg_sig[atsg+1] - atsg_sig[atsg]`, the envelope's length in A-SPX slots, each
  `num_ts_in_ats` QMF slots long.
- **Reading:** the length in QMF slots, which makes `est_sig_sb` the mean energy per QMF subsample that
  clause 3.1 makes a signal scale factor: "average energy of the signal within the region in a QMF matrix".
- **Evidence:** Observation. Where the source has content above the crossover (DEE's music at 48 kbps and
  speech at 48, 64 and 128 kbps), the divisor as printed decoded the A-SPX tiles 3.5 to 4.8 dB below the
  source's on average when phase D3 measured them, and this one 1.3 to 2.2 dB below, of which the limiter
  accounted for up to 1 dB. With the pre-flattening phase D4 reads ("Pre-flattening's direction", below),
  this one decodes them 0.5 to 1.0 dB below.

### alpha0's parentheses

- **Where:** Part 1 Pseudocode 87, p. 218: `alpha0[sb] = - cov[sb][0][1] + alpha1[sb] *
  cplx_conj(cov[sb][1][2]);`, then divided by `cov[sb][1][1]`. The line computing `denom` drops `[sb]` from
  `cov[1][2]`.
- **Reading:** `alpha0 = -(cov01 + alpha1 conj(cov12)) / cov11`, the first normal equation of the covariance
  method the clause names, whose second gives `alpha1` as printed; and `cov[sb][1][2]`. The pair then
  whitens: a subband that follows a two-slot recursion returns its coefficients.
- **Evidence:** Text. The gold legs set `aspx_tna_mode` Light to Heavy in most noise groups, but decode to
  the same tile energies and log-spectral distance, within 0.1 dB, under either sign, so they do not decide
  it.

### Pre-flattening's direction

- **Where:** Part 1 5.7.6.4.1, pp. 217 to 220: pre-flattening derives "a gain value ... from a coarse
  approximation of the slope of the source range", and "the inverse of this gain value is applied during the
  patching process". Pseudocode 85 defines `gain_vec[sb] = pow(10, (mean_energy - slope[sb])/20)`, the gain
  that brings each subband of the fitted slope to the mean, and Pseudocode 89 multiplies the patch by
  `1/gain_vec[p]`. Together the two double the low band's slope in the patch, where the clause names the
  step pre-flattening and describes the fit as the slope to take out.
- **Reading:** the patch is multiplied by `gain_vec[p]`: the fitted slope is taken out of the low band as it
  is copied up, and each patched subband starts from the fit's mean level.
- **Why:** with `aspx_interpolation` set, as in every stream here, the envelope adjuster gains each subband
  to its envelope whatever the patch's shape; what the patch's slope changes is the limiter, which cuts a
  gain more than 3 dB over its limiter group's (Pseudocodes 96 to 101). A patch whose slope is doubled needs
  its largest gains at the top of each patch, where the limiter cuts them.
- **Evidence:** Streams, not all one way. Over G0's legs with content above the crossover, this reading
  brings the A-SPX tiles nearer the source's energy: 2.0 speech at 48 and 64 kbps from 2.4 and 2.8 dB to
  1.3 and 1.6 dB, with ViSQOL from 4.23 and 4.40 to 4.55 and 4.50; 2.0 music at 48 kbps from 2.7 to 1.5 dB;
  immersive stereo at 64 kbps from 3.1 to 1.7 dB; 5.1 film's centre from 5.2 to 1.9 dB at 192 kbps and from
  2.7 to 1.7 dB at 256 to 320. As printed, film's centre loses 4.6 to 10.9 dB in its first patch's top
  group, and at 256 kbps the limiter takes it all: the envelope adjuster's output before the limiter is
  within 0.3 dB of the envelope there, and 3.9 dB under it after. It takes them a little further on 2.0 music at 64 kbps and immersive stereo at 96,
  0.3 and 0.2 dB, and on 2.0 speech at 96 to 144 kbps, whose crossover is 13.5 kHz, from 2.4 to 3.1 dB,
  with ViSQOL 0.05 to 0.07 lower. librempeg's decodes of these legs sit level across the subband groups,
  1.2 to 2.2 dB under the source, in every one.

### The first signal scale factor below zero

- **Where:** Part 1 Pseudocode 82, p. 215: `qscf_sig_sbg[0][atsg] == 0 && scf_sig_sbg[1][atsg] < 0`. No
  dequantised scale factor, `64 * 2^(qscf/a)`, is negative, so as printed the rule never applies.
- **Reading:** `qscf_sig_sbg[1][atsg] < 0`: an envelope coded along frequency whose first value is 0 and
  second negative takes the second group's scale factor for the first. The F0 codebooks send no negative
  value, and this lets an envelope start below their floor.
- **Evidence:** Observation. DEE relies on it in 265 of the 484 signal envelopes of its 96 to 144 kbps
  music legs, whose source is near silence above the crossover. With this reading the first group's
  energy, frame by frame, errs against the source within 2.2 dB of the second group's error; as printed,
  it sits 6.9 dB above it.

### The sinusoid's subband

- **Where:** Part 1 Pseudocode 92, p. 222: `sb_mid = (int) 0.5*(sbz+sba);`, where C's cast binds to `0.5`
  and gives 0. The same lines reuse `sba` and `sbz` for the group's own borders.
- **Reading:** `(int)(0.5 * (sbz + sba))` over the group's borders relative to `sbx`: its middle subband,
  rounded down, as the paragraph before says ("the sinusoid is placed in the middle of the high-frequency
  resolution subband group").
- **Evidence:** Text; the speech legs add sinusoids in 35 frames each.

### b_sine_at_end

- **Where:** Part 1 Pseudocode 95, p. 224, sets `b_sine_at_end` from this interval's `aspx_tsg_ptr` and
  never reads it; its test, like Pseudocode 99's, reads `p_sine_at_end`, which Pseudocode 92 sets from the
  previous interval's.
- **Reading:** as printed: `p_sine_at_end`, which makes the first envelope a transient's when the previous
  interval's transient was at its end. `b_sine_at_end` is unused.
- **Evidence:** Text.

### aspx_limiter

- **Where:** Part 1 4.3.10.1.7, p. 98, turns the limiter off with `aspx_limiter` 0; clause 5.7.6.4.2.2 never
  tests it.
- **Reading:** with the limiter off, Pseudocodes 96 to 101 are skipped: the gains and levels go to the
  assembly unlimited and unboosted.
- **Evidence:** Text; every gold leg sets it.

### The limiter's last group

- **Where:** Part 1 Pseudocodes 72 to 74, p. 207, can remove `sbz` from `sbg_lim`, when it is no patch
  border and lies less than 0.245 octave above the border before it. Pseudocodes 96 and 100 then map the
  subbands above the table's last border to a group past its end.
- **Reading:** the last limiter group runs to `sbz`, in the sums of Pseudocodes 96 and 99 and in the gains.
- **Evidence:** Text. 168 of the 5,622 configurations and base rates a stream can select end the limiter
  table below `sbz`, and 232 end the patches below it (`tests/ac4core/test_ac4core_aspx.cpp`); DEE's do
  neither.

### The noise and tone generators' indices

- **Where:** Part 1 Pseudocodes 103 and 105, pp. 228 and 229: `noise_idx_prev[sb][ts]` and
  `sine_idx_prev[sb][ts]` are "the last noise_idx" and "the last sine_idx" "from the previous A-SPX
  interval", written as matrices; both add `ts - atsg_sig[0]`, a QMF slot less an A-SPX slot. Pseudocodes
  107 and 108 start their loops at `atsg_sig[0]`, without `num_ts_in_ats`.
- **Reading:** one running index each per channel, from the last one the previous interval used: the noise
  index counts on by `num_sb_aspx` a QMF slot and 1 a subband, the sine index by 1 a QMF slot, both from
  the interval's first QMF slot, `atsg_sig[0] * num_ts_in_ats`, where Pseudocodes 107 and 108 start too.
  `master_reset` restarts the noise index at 0, and the first frame starts the sine index at 1.
- **Evidence:** Text. Either way the noise and the tones take the same sequences; which entry a subband
  gets cannot be measured against a source.

### Interleaved waveform coding

- **Where:** Part 1 5.7.6.5.2, p. 231, counts `aspx_tic_used_in_slot` in A-SPX slots "starting at the A-SPX
  timeslot that coincides with QMF timeslot 0"; 5.7.6.5.3 gives the output for frequency and time
  interleaving only.
- **Reading:** slot n covers the frame's output slots `n * num_ts_in_ats` to `(n + 1) * num_ts_in_ats - 1`,
  counted on Q_low's axis. Elsewhere the output is Q_low below `sbx` and the assembled `Y` from `sbx` to
  `sbz`, the waveform-coded input kept there only in a high resolution group `aspx_fic_used_in_sfb` marks,
  where it is added; above `sbz`, nothing but a time-interleaved slot's input.
- **Evidence:** Text; no gold leg interleaves.

### Before the first interval

- **Where:** Part 1 5.7.2 holds control data back `d_ctrl` frames; Pseudocodes 75, 80, 81, 86, 88, 92 and
  106 read the previous interval's state, and give its first value only for some of it.
- **Reading:** until a frame's control data comes due, the QMF matrix passes through as in SIMPLE mode. The
  previous Q_low, `Y` and envelopes are silence, `aspx_tna_mode_prev` and the chirp factors 0 (as
  5.7.6.4.1.3 says), `aspx_tsg_ptr_prev` -1, and `master_reset` is set at the first configuration. The
  first envelope's time deltas start from 0 at the resolution it has.
- **Evidence:** Text.

### Scale factors far out of range

- **Where:** Part 1 5.7.6.3.5: dequantisation raises 2 to a sum of transmitted deltas, which a stream that is
  not audio can take anywhere.
- **Reading:** the exponent is clamped to +-96 before `2^x`, and the output to +-10^9 of full scale before
  it becomes `float`: no stream DEE writes comes near, and every value the decoder computes stays finite.
- **Evidence:** Text; `fuzz_ac4_decode`.

## Tables

The Huffman codebooks come from the table attachment of Part 1, `ts_103190_tables.c`, which Annex A
names as normative, with the parameters Annex A prints; every one of the 60 is a complete prefix code
(its Kraft sum is exactly 1). Annex B's scale factor band tables come from the text, each checked against
a rendering of its page. `tools/generators/gen_ac4_tables.py` and
`tools/generators/gen_ac4_reference_tables.py` generate the C++ and Python tables separately.

## The trace

Decisions about what a record holds, which both transcriptions share (the full contract is in
`docs/verification.md`):

- Elements read into temporaries record the bits sent: `tmp_num_env`, and `aspx_rel_bord_left`,
  `aspx_rel_bord_right` and `aspx_tsg_ptr` for Table 53's anonymous `tmp`.
- `aspx_int_class` is one record whose value is the code read (0, 2, 6 or 7).
- A field whose width the stream sets and whose bits the syntax does not interpret (`add_data`,
  `extensions_bits`, `drc2_bits`) is one record valued at its last 64 bits, split into 65535-bit records
  when longer; `variable_bits()` records its value modulo 2^64, and splits the same way when its groups
  run past 65535 bits, each record then valued at its own last 64 bits. A record's width is 16 bits, so
  an element wider than that has no single record to sit in.
- An element that runs past the end of its substream is not recorded.

## The differential check

No stream here reaches most of the syntax: noise fill, VARVAR framing, time-interleaved A-SPX, the mono,
3.0 and 7.X elements, ASPX_ACPL_1, transmitted DRC gains, dialogue enhancement methods 1 to 3 and
alternative presentations among it. To compare the two transcriptions there, both read streams made for
the purpose: DEE frames with one substream altered (a random tail from a random bit, a few flipped bits,
or a random codec mode), and tables of contents built for the channel modes no encoder here writes, over
random payloads. Wherever both read a substream to its end their traces must agree record for record,
and where either stops they must agree up to that point. The two transcriptions still stop at different
elements on some corrupt input, since each checks some values at a different point, which the check
reports separately.
