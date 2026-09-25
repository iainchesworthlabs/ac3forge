# AC-4 encoder: readings

The readings this encoder takes where ETSI TS 103 190-1 V1.4.1 (Part 1) and ETSI TS 103 190-2 V1.3.1
(Part 2) leave a writer's choice open. Where the decoder depends on the same reading,
`src/ac4dec/ERRATA.md` has the entry and this page points at it; the writer and both readers take it.

The evidence for a reading is one of:

- **Readers**: what the encoder writes is read back as written. The decoder's reader, the Python parser
  (`tools/references/ac4_syntax.py`) and the encoder's own trace agree record for record on every stream
  the tests, the fuzz target `fuzz_ac4_encode` and the encoder-space harness
  (`tools/ci/fuzz_ac4_encoder_space.py`) write, and FFmpeg's raw AC-4 and mov demuxers frame them.
- **Streams**: DEE's streams, or a reader outside this project, settle it.
- **Text**: the text alone.

Later phases add the readings their tools need.

## Shared with the decoder

The writer takes the decoder's reading of each of these:

- [audio_size covers the fill](../ac4dec/ERRATA.md#audio_size-covers-the-fill): a frame's bits beyond
  its audio are `fill_bits` inside `audio_size`, before `metadata()`.
- [byte_align is relative to the substream](../ac4dec/ERRATA.md#byte_align-is-relative-to-the-substream).
- [ext_code is at most 21 bits](../ac4dec/ERRATA.md#ext_code-is-at-most-21-bits): the quantiser clips a
  line at 8191, and a band whose peak would pass it takes a coarser step.
- [Scale factors outside 0 to 255](../ac4dec/ERRATA.md#scale-factors-outside-0-to-255): every scale
  factor the deltas reach stays in range.
- [Pseudocode 59's stray block](../ac4dec/ERRATA.md#pseudocode-59s-stray-block): the M/S matrix and the
  prediction, with `0.1f` a float, and an `alpha_q` sent against a pair of bands `sap_data()` sent no
  coefficient for counted from 0.
- [Full scale, and the overlap-add's factor of two](../ac4dec/ERRATA.md#full-scale-and-the-overlap-adds-factor-of-two),
  [KBD_RIGHT's argument](../ac4dec/ERRATA.md#kbd_rights-argument) and
  [The KBD kernel is summed to p = N](../ac4dec/ERRATA.md#the-kbd-kernel-is-summed-to-p-n): the forward
  transform is the transpose of the decoder's, through the same windows, with lines scaled by 2^16 so
  that a full-scale input decodes at full scale.
- [Partial coupling starts at acpl_param_band](../ac4dec/ERRATA.md#partial-coupling-starts-at-acpl_param_band):
  the A-CPL writer (`src/ac4enc/src/acpl/acpl_syntax.hpp`, phase D5's, for the constructed streams and
  for E4) sends each parameter set from `acpl_param_band`, its first value along frequency from the F0
  codebook, as Table 65 reads it.

## The QMF domain

The readings phase E2 takes for the ASPX codec mode: companding and A-SPX, written. The writer takes the
decoder's reading of each of these, and the tests and the encoder-space harness hold the three traces
equal on every ASPX stream they write:

- [Every codec mode passes through the QMF banks](../ac4dec/ERRATA.md#every-codec-mode-passes-through-the-qmf-banks):
  an ASPX stream lags its input by the SIMPLE mode's delay, 4,385 samples at `frame_rate_index` 13.
- [Companding measures against full scale 1.0](../ac4dec/ERRATA.md#companding-measures-against-full-scale-10),
  [Companding's slots are Q_low's](../ac4dec/ERRATA.md#compandings-slots-are-q_lows) and
  [The companding average](../ac4dec/ERRATA.md#the-companding-average): the compressor below inverts the
  expander those readings give; the writer sends `b_compand_on` per channel and never `sync_flag`.
- [The estimated envelope's time divisor](../ac4dec/ERRATA.md#the-estimated-envelopes-time-divisor): a
  signal envelope is the input's mean energy per QMF subsample over its groups and slots.
- [The first signal scale factor below zero](../ac4dec/ERRATA.md#the-first-signal-scale-factor-below-zero):
  an envelope coded along frequency whose first value is below the F0 codebooks' floor sends 0 there, and
  the second group's value takes it over.
- [The sinusoid's subband](../ac4dec/ERRATA.md#the-sinusoids-subband), [b_sine_at_end](../ac4dec/ERRATA.md#b_sine_at_end)
  and [Before the first interval](../ac4dec/ERRATA.md#before-the-first-interval): what the encoder keeps
  of the decoder's state to choose delta coding and sinusoids.
- [Pre-flattening's direction](../ac4dec/ERRATA.md#pre-flattenings-direction): the encoder runs the
  decoder's high frequency generator on its input's low band to choose inverse filtering, noise floors and
  sinusoids, and flattens the patch as the decoder does. Phase D4 changed the reading.

### Where the encoder's QMF slots fall

- **Where:** Part 1 Table 188's `d_pcm` and `d_ctrl`, 5.7.3's analysis, and 5.7.6.3.2's
  `ts_offset_hfgen`, read from the writer's side: which input samples a frame's A-SPX data describes.
- **Reading:** the decoder's QMF slot g covers the 64 samples of the encoder's delayed input from
  `64 g - d_pcm`, since the output of the inverse transform is held `d_pcm` samples before the
  analysis; frame f's control data arrives `d_ctrl` frames later, behind `ts_offset_hfgen` slots, so its
  interval's slot i is QMF slot `num_qmf_timeslots (f + d_ctrl) - ts_offset_hfgen + i`: at
  `frame_rate_index` 13, 352 samples, 1 frame and 6 slots, `32 (f + 1) - 6 + i`. The encoder analyses
  its input with the decoder's own bank on that axis and estimates each frame's envelopes, noise floors
  and inverse filtering over those slots, and dialogue enhancement's and DRC's values over the same
  block, where the decoder's output stages meet them.
- **Evidence:** Streams and Readers. The encoder's ASPX streams decode at the lag of DEE's, and above
  the crossover the decoded A-SPX tiles land as close to the source's energy as DEE's do
  (`tools/checks/score_ac4_encode.py --gold`).

### The compressor

- **Where:** Part 1 5.7.5 gives the expander only: each slot of the low band times `2^(1/alpha)
  L^((1 - alpha)/alpha)`, `alpha` 0.65, with L the slot's level.
- **Reading:** the compressor multiplies each slot of the input's low band, below `sbx`, by `0.5
  L^(alpha - 1)`, L measured as the expander measures it on the input's own analysis: expanded, the
  slot's level is L again. The compressed slots, with nothing above `sbx`, are synthesised back by the
  decoder's synthesis bank, whose output runs `d_pcm` + 577 samples (929 at `frame_rate_index` 13)
  behind the analysis's input; the spectral frontend codes that output as far on, so that the decoder's
  analysis of what it decodes sees the compressed slots on the same axis.
- **Evidence:** Observation. A 1 kHz tone whose level steps by 30 dB steps by 0.65 of that, 19.5 dB,
  compressed (`tests/ac4enc/test_ac4enc_aspx.cpp`), and the encoder's companded streams decode within
  0.4 dB of their source's level.

### A sinusoid's group carries its energy

- **Where:** Part 1 Pseudocodes 92 to 94, pp. 222 and 223: a group with `aspx_add_harmonic` set puts its
  sinusoid in its middle subband, at the level `scf_sig / (1 + scf_noise)` of the whole group, and scales
  the rest of the group to the noise.
- **Reading:** the encoder sends, for a group whose sinusoid an envelope carries, the energy per QMF
  subsample of the subband it stands for, not the group's mean, which would set the sinusoid a group's
  width low.
- **Evidence:** Text.

### Balance values are sent halved

- **Where:** Part 1 Pseudocodes 80 and 81 add each value of a balance channel twice (`delta` 2), and
  Pseudocode 84 reads the pair as a sum, `2^(qa/a + 1)` times 64, and a ratio, `2^(qb/a - PAN_OFFSET)`,
  with `PAN_OFFSET` 12. The balance F0 codebooks hold 0 to 24 at 1.5 dB and 0 to 12 at 3 dB.
- **Reading:** the sum's value is `a (log2(2^(qL/a) + 2^(qR/a)) - 1)`, and the value sent for the balance
  is half of `a (PAN_OFFSET + (qL - qR)/a)`, so that the F0 range is centred on equal channels; the noise
  floors likewise, without `a`. A balance channel's values are coded along time only from the last
  frame's balance values, which the doubling leaves even.
- **Evidence:** Readers. The encoder writes `aspx_balance` only when asked (`experimental=aspx-balance`),
  since no reader outside this project has read it from the encoder yet.

### A FIX end, a FIX start

- **Where:** Part 1 Pseudocode 76 starts a VARFIX or VARVAR interval where the last one stopped, which a
  FIXVAR or VARVAR interval's `aspx_var_bord_right` moves up to three A-SPX slots past its frame's end,
  and starts a FIXFIX or FIXVAR interval at the frame's start whatever the last did.
- **Reading:** an interval that ends with its frame is followed by one that starts with its frame, and
  one that runs on by one that starts where it stopped, so that the intervals tile the QMF slots.
- **Evidence:** Text.

### No time deltas into an I-frame

- **Where:** Part 1 4.3.10.3: `aspx_sig_delta_dir` and `aspx_noise_delta_dir` code an envelope along time
  from the one before, which for an interval's first envelope is the last frame's.
- **Reading:** an I-frame's first signal and noise envelopes are coded along frequency, so that a decoder
  can start there; later envelopes may be coded along time.
- **Evidence:** Text.

## Table of contents and framing

### sequence_counter

- **Where:** Part 1 4.3.3.2.2 counts `sequence_counter` from 1 to 1020 and round again; Part 1 Annex
  E.1 asks for 0 in the first frame of a file.
- **Reading:** 0 in the stream's first frame, then 1 to 1020 and round again from 1.
- **Evidence:** Readers.

### The CRC of a sync frame

- **Where:** Part 2 Annex G.4.2 gives the generator polynomial and initial state of `crc_word`, and G.3.1
  places it after the raw frame.
- **Reading:** the CRC-16 of polynomial 0x8005, from 0, over `frame_size` (two bytes, or five with the
  24-bit extension) and the raw frame.
- **Evidence:** Streams. The inspector's check of this coverage passes on every frame of DEE's 0xAC41
  streams, and the encoder-space harness computes it again from the text.

### Leftover bytes go in payload_base

- **Where:** Part 1 4.3.3.2.10 and 4.3.3.2.11: `payload_base` places the first substream that many
  bytes after the table of contents.
- **Reading:** at a constant rate a frame's size is fixed, and a table of contents whose size moves with
  the substream sizes it carries can leave one to seven bytes over; `payload_base_minus1` takes them, as
  zero bytes between the table of contents and the presentation substream.
- **Evidence:** Readers.

### An MP4 track's channel count

- **Where:** Part 2 E.4.5: `channelcount` of the `ac-4` sample entry "should be set to 2".
- **Reading:** 2, for mono as for stereo; the presentation says what the stream holds.
- **Evidence:** Text. FFmpeg's mov demuxer reads the track.

## The 5.X and 7.X elements

The readings phase E3 takes for 5.0 and 5.1, and for 7.0 and 7.1 as an experimental option. The writer
takes the decoder's reading of each of these, and the tests and the encoder-space harness hold the three
traces equal on every 5.X and 7.X stream they write:

- [The LFE's track is not numbered in Tables 180 and 182](../ac4dec/ERRATA.md#the-lfes-track-is-not-numbered-in-tables-180-and-182):
  the LFE's `mono_data(1)` first, the channel data's tracks counted after it, and in the 7.X element C's
  `mono_data(0)` after the additional pair where `coding_config` 0 and 2 send it.
- [The 7.X element's additional channels](../ac4dec/ERRATA.md#the-7x-elements-additional-channels): the
  encoder sends `b_use_sap_add_ch` 0, so its additional pair is its own two channels and the reading's
  matrix is not written.
- Table 213's name for 3/2/2's last pair (the misprints in `src/ac4dec/ERRATA.md`): Tfl and Tfr, as
  Tables 88 and 183 have them.

## A-CPL

The readings phase E4 takes for ASPX_ACPL_2 and ASPX_ACPL_3 in the 5.X element, and for ASPX_ACPL_1
there and A-CPL in the channel pair as experimental options. The writer takes the decoder's reading of
each of these:

- [When A-CPL's parameters apply](../ac4dec/ERRATA.md#when-a-cpls-parameters-apply): a frame's
  parameters are estimated from the QMF slots of the A-SPX interval they share a control frame with,
  over a window centred on the last of them, where smooth interpolation reaches the new values.
- [ASPX_ACPL_1: the framing of the residuals](../ac4dec/ERRATA.md#aspx_acpl_1-the-framing-of-the-residuals):
  the 5.X element's residuals share A and B's framing and layout group, and `max_sfb_master`, in
  n_side_bits of the largest transform length, stops them at `acpl_qmf_band`.
- [get_max_sfb() with b_dual_maxsfb](../ac4dec/ERRATA.md#get_max_sfb-with-b_dual_maxsfb): the channel
  pair's ASPX_ACPL_1 sends one `sf_info()` with `b_dual_maxsfb`, the side stopped at `acpl_qmf_band` by
  its own `max_sfb_side`, and `chparam_info()` at `sap_mode` 0.

### The downmixes A-CPL codes

- **Where:** Part 1 Pseudocodes 115 to 118, pp. 238 to 241, give the upmix; nothing gives the downmix a
  writer codes, or its level.
- **Reading:** the signals the upmix keeps whatever its parameters: the channel pair's x0 = (L + R) / 2,
  whose z0 + z1 is 2 x0; ASPX_ACPL_1's and 2's (L + Ls / sqrt 2) / 2 and its mirror, whose z0 + z1 /
  sqrt 2 is 2 x0 once Pseudocode 117 has scaled z1 by sqrt 2; and ASPX_ACPL_3's Lo and Ro, L + C /
  sqrt 2 + Ls / sqrt 2 and its mirror, over 1 + sqrt 2, which Pseudocode 118's input gain, 1 + 2
  sqrt 0.5, brings back to Lo and Ro. ASPX_ACPL_1's residuals are (L - Ls / sqrt 2) / 2 and its
  mirror, or the pair's (L - R) / 2: below `acpl_qmf_band` Pseudocode 116 takes x0 plus and minus the
  residual, which gives each pair back as it was.
- **Evidence:** Streams: DEE's ASPX_ACPL_2 and ASPX_ACPL_3 streams decode with these downmixes, recovered
  from the output, 21 to 25 dB over the noise below the crossover on music (phase D5).

### ASPX_ACPL_3's gammas

- **Where:** Part 1 Pseudocodes 118 and 119, pp. 241 and 242: six gammas mix Lo and Ro into the three
  decorrelators' inputs, the two modules' groups and C. The text says what they do, not how a writer
  chooses them, and more than one choice rebuilds the same channels.
- **Reading:** gamma5 and gamma6 are the least squares prediction of C / sqrt 2 from Lo and Ro, per
  parameter band, and gamma1 = 1 - gamma5, gamma2 = -gamma6, gamma3 = -gamma5 and gamma4 = 1 - gamma6,
  counted in the quantiser's steps (ten fine steps or five coarse ones make 1), so that each module's
  group is Lo or Ro less the predicted centre, L + Ls / sqrt 2 or its mirror where the prediction is
  exact. beta3 gives the centre the energy the prediction leaves out.
- **Evidence:** Streams: DEE's 5.1 streams at 96 kbps hold the four relations in 3,534 and 3,539 of the
  3,555 bands of music and film, and one relation is a step out in the rest.

## The MP4 sample entry's dac4

`ac4::build_dac4()` in `src/ac4` writes Annex E.6's `ac4_dsi_v1()` from a table of contents, for the
encoder's MP4 output and for `ac3cli mp4` alike. The DSI of a presentation of one channel-coded
substream in one substream group is derived whole (Annex E.10 and E.11); these are the readings it
takes. The evidence for each is DEE's MP4 muxer: for the committed DEE streams and for the encoder's,
the box it writes is the box `build_dac4()` writes, byte for byte (`tests/ac4/test_ac4.cpp`,
`tools/checks/check_ac4_encode_readers.py`), but for the 3/2/2 layout's top front pair (below).

### Pseudocode E.3 leaves channel groups out

- **Where:** Part 2 E.10.3, pp. 235 and 236, against Table A.27, p. 214.
- **Text:** Pseudocode E.3 sets no LFE group for any channel mode, and for `pres_ch_mode` 11 and up
  sets neither L/R nor Ls/Rs, and sets group 2 where its comment says C, which Table A.27 numbers 1.
  It never sets group 16, Lscr/Rscr: its condition is `if (0)`, "not present in any supported channel
  configuration", where Table A.27 gives the pair to the 9.X layouts, `pres_ch_mode` 13 and 14. For
  22.2 it sets group 7, Tsl/Tsr, only where `pres_top_channel_pairs` is 1, where Table A.27 has them in
  22.2 whatever the top pairs.
- **Reading:** the groups Table A.27 gives each mode: group 6 wherever the mode has an LFE (5.1, the
  7.1 modes, 7.1.4, 9.1.4 and 22.2), and for 11 and up L/R, Ls/Rs, C where `b_pres_centre_present`,
  Lb/Rb where `b_pres_4_back_channels_present` and the top pairs `pres_top_channel_pairs` names;
  Lscr/Rscr for 9.0.4 and 9.1.4, and Tsl/Tsr for 22.2. The same for `dsi_substream_channel_groups[]`.
- **Evidence:** Streams, for 5.1 and 5.1.4: DEE's muxer sets groups 0, 1, 2 and 6 for 5.1, and 0, 1, 2,
  4, 5 and 6 for its 5.1.4. Text for the 9.X layouts and 22.2, which no stream here carries.

### The 3/2/2 layout's top front pair

- **Where:** Part 2 Table A.27, p. 214, and Pseudocode E.3, pp. 235 and 236.
- **Text:** both give the 7.X element's 3/2/2 modes (`pres_ch_mode` 9 and 10) group 4, Tfl and Tfr.
- **Reading:** group 4, as printed, in `presentation_v1_channel_groups[]` and
  `dsi_substream_channel_groups[]`.
- **Evidence:** Text, which both places agree on, against two readers. Given the encoder's 3/2/2
  streams (an experimental layout), DEE's muxer writes groups 0, 1 and 2, and 6 for 7.1, and none for
  the pair, while it writes the 3/4/0 and 5/2/0 modes' pairs (groups 3 and 17) as printed.
  MediaInfo's trace names the channel mode 7.1 3/2/2.1, and its summary counts seven channels, the
  pair as Tfc. DEE's encoder writes no 7.X layout, so neither reader has a stream of its own maker's
  to read there. `tools/checks/check_ac4_encode_readers.py` reports the muxer's box as differing in
  group 4 alone.

### b_presentation_core_differs

- **Where:** Part 2 Table E.11, p. 233.
- **Text:** true "if the pres_ch_mode_core according to pseudocode 26 has a value of -1".
- **Reading:** true where `pres_ch_mode_core` is not -1, as `b_presentation_core_channel_coded`'s rule,
  false where it is -1, implies; for one channel-coded substream, the immersive modes 11 to 14 (Table
  71), with Table E.14's code for their core.
- **Evidence:** Streams. DEE's muxer writes 0 for 2.0 and 5.1, and 1 with the 5.1.2 core for its 5.1.4.

### The bit rate and the indicators

- **Where:** Part 2 Table E.7 and E.10.1.
- **Reading:** `bit_rate_mode` follows the table of contents' `wait_frames` as Table E.7 says (1 where it
  is 0, 2 where it is 1 to 6, 3 otherwise, `b_wait_frames` 0 included); `bit_rate` is written as 0,
  unknown, with `bit_rate_precision` 0xFFFFFFFF. `de_indicator` and `immersive_audio_indicator` are
  facts of the substreams, which a table of contents does not carry: the encoder gives them, 0 and 0,
  and a DSI built from a stream's table of contents alone leaves out the closing byte that holds them,
  as its `pres_bytes` allows.
- **Evidence:** Streams, for the encoder's output: DEE's muxer writes the same. For DEE's streams it
  writes mode 2 and sets `de_indicator`, which `ac3cli mp4` cannot see from the table of contents.

## The presentation substream

### dialnorm_bits

- **Where:** Part 1 4.3.12.2.1: the dialogue level in 0.25 dB steps from 0 to -31.75 dBFS.
- **Reading:** `dialnorm_bits` is the level's magnitude over 0.25, rounded; `ac3cli ac4-encode`'s
  `dialnorm=` gives it in whole decibels, as the AC-3 encoders take it.
- **Evidence:** Readers.

### A drc_frame() with no DRC

- **Where:** Part 2 6.2.2.3: `drc_metadata_size_value` counts the bits of `drc_frame()`.
- **Reading:** a frame that sends no DRC writes `drc_frame()` as `b_drc_present` 0 alone, and a size of
  one bit; `tools_metadata_size` of the audio substream's `metadata()` is likewise one bit,
  `b_de_data_present` 0.
- **Evidence:** Readers: both readers hold these sizes to the bits read.

### Dialogue enhancement and DRC in the frame's metadata

The writer takes the decoder's reading of each of these (phase E5):

- [de_data() predicts from the wrong channel](../ac4dec/ERRATA.md#de_data-predicts-from-the-wrong-channel):
  a channel after the first is sent along its own bands in an I-frame.
- [Dialogue enhancement and DRC configuration across I-frames](../ac4dec/ERRATA.md#dialogue-enhancement-and-drc-configuration-across-i-frames):
  `de_config()` and `drc_config()` go in I-frames, and a frame between them sends `b_de_config_flag` 0,
  and `b_drc_present` 0 unless a mode sends gains.
- [drc_repeat_id copies a whole mode](../ac4dec/ERRATA.md#drc_repeat_id-copies-a-whole-mode): a repeat of
  a mode that sends gains sends a gainset in `drc_data()` too, that mode's gains again.
- [drc_gains() is a brace short](../ac4dec/ERRATA.md#drc_gains-is-a-brace-short) and
  [DRC's units](../ac4dec/ERRATA.md#drcs-units): gains in whole dB2, frequency-differential along the
  first subframe's bands and time-differential along each band's subframes.
- [When dialogue enhancement's, DRC's and the downmix's values apply](../ac4dec/ERRATA.md#when-dialogue-enhancements-drcs-and-the-downmixs-values-apply):
  a frame's dialogue enhancement parameters and DRC gains are computed on the block its control data
  meets (above, "Where the encoder's QMF slots fall").

### drc_gainset_size counts drc_version

- **Where:** Part 1 4.3.13.5.1, p. 130 ("the size in bits of the following drc_gains element"), against
  Table 74, p. 67 (`bits_left = drc_gainset_size - 2 - used_bits`).
- **Reading:** the formula's: the size counts `drc_version`'s two bits and `drc_gains()`, which is what
  a reader skipping a gainset by its size needs. The decoder accepts either reading at `drc_version` 0
  ([drc_gainset_size does and does not count drc_version](../ac4dec/ERRATA.md#drc_gainset_size-does-and-does-not-count-drc_version)).
- **Evidence:** Readers. Transmitted gains are experimental (`experimental=drc-gains`): no stream DEE
  writes sends them.

## Presentations

The table of contents of several presentations and their mixing fields (`src/ac4enc/src/frame/toc_writer.cpp`
and `metadata.cpp`), which phase D7's test multiplexer writes and phase E6 extends to the encoder's own
presentations. The writer takes the decoder's reading of each of these:

- [presentation_config 1 and 4 read more specifiers than n_substream_groups](../ac4dec/ERRATA.md#presentation_config-1-and-4-read-more-specifiers-than-n_substream_groups)
  and [Substream group gains](../ac4dec/ERRATA.md#substream-group-gains): every specifier the
  configuration reads, and `sg_gain` for n_substream_groups groups as 6.2.1.3 assigns it: none for
  configuration 1, the main and associated groups' for configuration 4.
- [The dialogue's gain and pans](../ac4dec/ERRATA.md#the-dialogues-gain-and-pans) and
  [Panning](../ac4dec/ERRATA.md#panning): `dialog_max_gain` for a g_dialog_max of (1 + `dialog_max_gain`)
  x 3 dB, and pans in 1.5 degree steps clockwise from the front, 330 degrees L and 30 degrees R.
- [The hybrid dialogue enhancement's waveform](../ac4dec/ERRATA.md#the-hybrid-dialogue-enhancements-waveform):
  a hybrid method's `de_signal_contribution` sets the waveform's share, alpha_c = x / 31, of the gain.

## Rates

### What wait_frames counts

- **Where:** Part 1 4.3.3.2.4 and Table 81, p. 73: the frames a decoder "should wait" after receiving
  the frame before its output; 6.2.4, p. 263, and Part 2 Annex B, pp. 217 and 218, which estimates the
  rate from sizes over m frames and `m + wait_frames(0) - wait_frames(m)`, give it a buffer's meaning.
- **Reading:** the whole frame periods between the frame's arrival, whole, over a channel at the
  stream's rate and its output, for a decoder that starts at the frame: its slack, floored (in twos at
  indices 10 to 12, where Table 81 counts in twos). Such a decoder outputs the frame up to a frame (two)
  early, so each frame keeps at least a frame (two) of slack and at most what the buffer holds: 1 to 5
  frames, or 2 to 11. Over m frames the sizes then come to within a frame (two) of Annex B's N'.
- **Evidence:** Text. The tests check, frame by frame and from the stream alone, that a decoder starting
  at any frame is never fed a frame late and that its buffer never holds more than 6.2.4 sets.

### br_code carries the raw frames' rate

- **Where:** Part 2 6.3.2.1.2 and Annex B, steps 2 to 4 and 13.
- **Reading:** the sequence carries the rate of the raw frames, `raw_ac4_frame()`s, in kbps, which is
  the rate the encoder is given; Annex B adds a sync frame's overhead to it as B. The writer sends 0b11
  and six base-3 digits of the fraction of log2 of the rate, a precision of 3^-6 of an octave, then 0b11
  again.
- **Evidence:** Text.
