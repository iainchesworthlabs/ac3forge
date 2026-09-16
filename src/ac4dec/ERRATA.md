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
- **Evidence:** Streams for factor 1 (every stream here); Text above it.

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
  configuration before the next I-frame fails as missing its I-frame.
- **Evidence:** Streams: DEE starts counting at 1019, so every stream here passes the wrap to 1 in its
  third frame. `tests/ac4dec/test_ac4dec_decoder.cpp` checks a jump and a 0.

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
- **Reading:** -1, the value a transmitted 0 gives, which points at no envelope: no transient. The phase
  that decodes the HF generator confirms or replaces this.

### Stray semicolon in the limiter's patch borders

- **Where:** Part 1 Pseudocode 72, p. 207: `for (sbg = 1; sbg < num_sbg_patches; sbg++);` before its
  block.
- **Reading:** the block is the loop's body, copying the interior patch borders, which matches
  `num_sbg_lim = num_sbg_sig_lowres + num_sbg_patches - 1`. Not syntax; recorded for the HF phase.

### freq_res_prev in Pseudocode 80

- **Where:** Part 1 Pseudocode 80, p. 214: `atsg_freqres[num_atsg_sig_prev - 1]`, with two unbalanced
  parentheses.
- **Reading:** the previous interval's resolution vector, which the paragraph after the pseudocode names
  `freq_res_prev`. Not syntax; recorded for the envelope phase.

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
  - `superset(0, 1)` is 1, as 6.3.3.1.27 says, although its own rule would give 3.0; 9.X.4 with 22.2 has
    no superset.
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
  when longer; `variable_bits()` records its value modulo 2^64.
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
