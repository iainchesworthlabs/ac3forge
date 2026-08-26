#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <fmt/base.h>
#include <fmt/format.h>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ac3/quality/distortion.hpp"
#include "ac3/analysis/levels.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/decoder/output.hpp"
#include "ac3/encoder/assignment.hpp"
#include "ac3/encoder/plan.hpp"
#include "ac3/io/wav.hpp"
#include "ac3/meta/loudness.hpp"
#include "ac3/oba/joc.hpp"
#include "ac3/signing/emdf_atmos_signer.hpp"
#include "ac3/signing/signing_key.hpp"
#include "matroska/matroska.hpp"
#include "mp4/dash.hpp"
#include "mp4/hls.hpp"
#include "mp4/mp4.hpp"
#include "recording_sink.hpp"

// The CLI-wide support layer: option/metadata parsing, path/stdio conventions, frame and WAV I/O,
// and level reporting shared by nearly every command in main.cpp's kCommands table. Split out of
// main.cpp (which used to define all of this in its own anonymous namespace) as the first step of
// the repo-structure review's H4 monolith split - see that review for why, and main.cpp's own
// command-table comment for the design these helpers serve.
//
// Everything here has external linkage (namespace ac3cli, not main.cpp's old anonymous namespace)
// because it is now called from a different translation unit. A few helpers that are genuinely
// private to one function's own implementation (parse_double, to_bytes, write_wav_f32_arg) stay
// out of this header entirely and live in an anonymous namespace inside support.cpp instead,
// preserving the original "internal unless something else needs it" default.
//
// Everything about layouts, coding tools and metadata itself lives in ac3::plan, so the GUI
// cannot mean something different by "514" or by "all" than this does. What is here is argument
// shape, validation and printing - Options carries plan::Metadata verbatim rather than a second,
// CLI-specific copy of the same fields.
namespace ac3cli {

std::uint32_t parse_u32_or(std::string_view text, std::uint32_t fallback);

// A positional argument in seconds, which several stream tools take and the
// command table's own u32()/i32() accessors cannot express - `cut` in
// particular needs sub-second precision to name an access unit at all
// (1536 samples at 48 kHz is 32 ms).
double parse_seconds_or(std::string_view text, double fallback);

// --- metadata options -------------------------------------------------------
// Bare words and key=value tokens, appended after the positional arguments in
// any order, the same way 'couple' already works. Everything defaults off, so
// a command line that says nothing about metadata produces exactly the stream
// it produced before this layer existed.

// --- verbosity -------------------------------------------------------------
// Set once by main(), from the `quiet`/`verbose` tokens, and read by every
// status printer below. A global rather than another field threaded through
// every run_* signature: the two tokens are properties of the invocation, not
// of any one command's arguments, and every command already takes an Options
// it would otherwise have to reach into at ~90 separate print sites.
void set_verbosity(bool quiet, bool verbose);

// True when `verbose` was given: the progress line runs whatever the run's
// length, and the routing/source decisions name themselves as they are made.
[[nodiscard]] bool verbose_mode();

// True when `quiet` was given. Only the progress/status printers need to ask;
// everything else goes through status_stream()/status_println(), which
// already account for it.
[[nodiscard]] bool quiet_mode();

// Everything a command accepts after its positional arguments, in any order.
// The metadata group is ac3::plan::Metadata verbatim; drc_scale is decode-
// side local, because nothing an encoder is configured with corresponds to
// it; sources/map_spec describe routing rather than metadata, but share this
// same trailing-options surface (parse_options) the way dialnorm2= already
// shares it despite being layout-1+1-specific - a command that has no use
// for a field simply never sets it.
struct Options {
    // Decoder side, for 'decode'.
    double drc_scale = 0.0;
    // 'decode'/'monitor' only: the §7.8 output stage (ac3/decoder/output.hpp).
    // Every field defaults off, so a plain invocation still writes the coded
    // channels untouched - see channels=/downmix=/drcmode= in
    // print_meta_usage. Set straight into DecoderConfig::output.
    ac3::OutputConfig output{};
    // Each src= occurrence, in order given - additional input sources beyond
    // the primary positional argument. encode/eac3-encode only; empty unless
    // multi-source input is in play.
    std::vector<std::string> sources;
    // Each offset= occurrence: (sourceIndex, seconds) - leading silence ahead
    // of that source's own audio, in the same 0-based numbering src=
    // establishes (0 = the primary positional argument, 1..N = each src= in
    // order). encode/eac3-encode only, including the classic single-file
    // path, where source 0 is the only source there is. A given sourceIndex
    // may appear more than once; the last occurrence wins (see
    // offset_samples_for).
    std::vector<std::pair<std::size_t, double>> offsets;
    // The raw map= text, if given - parsed into a plan::Assignment once the
    // sources are loaded and their channel counts are known, which
    // parse_options itself cannot do (it only sees command-line text, not
    // opened files).
    std::optional<std::string> map_spec;
    // signing-key=<path>, read by sign-objects/verify-objects below - kept
    // apart from those two so their own comments stay about what they DO
    // rather than where the key comes from.
    std::optional<std::string> signing_key;
    // 'probe' only: how much per-frame detail the report carries - unset for
    // the stream summary alone, "frames" for one entry per access unit,
    // "blocks" to also dump every block's coding tools and exponent
    // strategies. A string rather than an enum for the same reason
    // qc_preset below is one: parse_options only ever sees command-line text,
    // and the command that consumes it is the one that knows what the values
    // mean.
    std::optional<std::string> detail;
    // 'qc' only: which delivery gate(s) to check the measurement against -
    // one of ac3::meta::kQcPresetNames, or "all" to check every preset.
    // Unset (measure-only, no gate) is the default - a plain
    // 'ac3cli qc <file>' just reports the numbers, no pass/fail verdict.
    std::optional<std::string> qc_preset;
    ac3::plan::Metadata p{};
    // Atmos object signing (atmos/atmos-path/atmos-encode). Off unless the
    // operator both asks (sign-objects) and provides a key - either
    // signing-key=<path> here, or the AC3FORGE_SIGNING_KEY[_FILE] env vars
    // load_signing_key() falls back to. The key is never stored by this tool;
    // see docs/concepts/object-signing.md.
    bool sign_objects = false;
    // 'decode'/'monitor' only: check each frame's EMDF object container
    // against signing-key= (same option sign-objects uses - a decode never
    // signs, so there is no ambiguity in sharing it) instead of just playing
    // it. Off by default: a signed-but-unchecked stream decodes exactly like
    // an unsigned one unless the operator opts in here - see
    // docs/concepts/object-signing.md.
    bool verify_objects = false;
    // 'fmp4' only: also write the object-stripped 5.1 companion rendition
    // into the same #EXT-X-MEDIA group, which is what Apple's HLS Authoring
    // Specification asks for alongside a CHANNELS="<N>/JOC" Atmos rendition.
    // Off by default, matching every bare token here: a plain invocation
    // writes exactly the single-rendition directory it always has, and a
    // stream with no object layer has no companion to write anyway.
    bool hls_fallback_51 = false;
    // 'ts' only, both broadcast profiles: the identification values neither
    // registry's descriptor can read off the bitstream because they describe
    // how services in a multiplex RELATE, not what one elementary stream
    // contains. Unset omits the field rather than inventing a number - see
    // mpegts::ServiceInfo::mainid.
    std::optional<int> mainid = std::nullopt;
    std::optional<int> asvc = std::nullopt;
    // 'eac3-encode' only: run ac3::verify's E-AC-3 encoder/decoder mirror
    // self-check (ac3/verify/eac3_selfcheck.hpp) over every access unit this
    // command emits, and refuse the run on the first disagreement. Off by
    // default like every bare token here, and deliberately so: it decodes
    // every access unit a second time on top of encoding it, which roughly
    // doubles the work. What it buys is the one class of defect a round trip
    // cannot see - a misreading of Annex E that the encoder and the decoder
    // share - which for ecpl, tpn, fscod2 and 7.1.4 is otherwise unchecked
    // by anything at all (docs/verification.md).
    bool verify = false;
    // 'live' only: a second ("slave") capture device index, same numbering
    // ac3::audio::enumerate_devices()/'devices' uses and the capture_device
    // positional already reads. Unset means the classic single-device
    // session, unchanged from before this option existed.
    std::optional<int> capture2 = std::nullopt;
    // 'record'/'live' only: which container the take is written into - the
    // same five RecordingSink streams the GUI's own Container combo offers
    // (EncoderController::recording_sink_container). Defaults to the bare
    // elementary stream, so a plain invocation writes exactly the .ac3/.ec3
    // it always has. Every one of the five is written incrementally through
    // RecordingSink itself (roadmap IO9 - there is no accumulate-then-mux
    // path left on either command), kFmp4 included: RecordingSink's own
    // kFmp4 backend (Fmp4FolderWriter) now takes the rolling-window option
    // fmp4_window_segments below needs, so there is no separate writer left
    // to maintain here the way there briefly was.
    RecordingSink::Container container = RecordingSink::Container::kElementary;
    // container=fmp4 only: how many of the most recent media segments the
    // HLS playlist and DASH MPD list - a rolling live window
    // (mp4::FragmentOptions::playlist_window_segments). 0, the default,
    // lists every segment, which is what a session whose directory will be
    // served whole afterwards wants; a real origin deleting segments behind
    // itself sets its own depth here.
    std::uint32_t fmp4_window_segments = 0;
    // 'record'/'live' only: the encoded layout, and whether the codec is
    // derived from it or forced. Empty layout means stereo, which is what
    // both commands did before they could be told otherwise; codec unset
    // means "AC-3 unless the layout needs E-AC-3", plan::carries()'s own
    // answer. Wide layouts on record/live are roadmap IO9 - the GUI has
    // always done them.
    std::string take_layout;
    std::optional<ac3::plan::Codec> take_codec;
    // 'record'/'live' only: how long the capture device may deliver nothing
    // before the session stops as a failure rather than sitting there
    // reading "running" (ac3::audio::SilenceWatchdog, the same class and the
    // same 3 s default the GUI's live session uses). 0 disables it, for a
    // device that legitimately goes quiet for longer than that.
    std::chrono::milliseconds watchdog{3000};
    // 'live' only: the object-slot budget for mode=atmos, allocated once at
    // session start so a slot bound later cannot change the stream's object
    // count mid-session. Unset means one slot per captured channel, which is
    // what live has always done.
    std::optional<std::size_t> live_objects;
    // 'live' only: whether an AC-3-only passthrough endpoint gets the
    // parallel 5.1 AC-3 downmix leg (the default, matching the GUI's
    // wants_downmix_leg) or a plain refusal (downmix=off, what the CLI did
    // before roadmap IO9).
    bool downmix_leg = true;
    // 'play' only: whether a source format the chosen sink does not accept
    // gets an automatic fallback - an in-memory transcode to AC-3 when the
    // sink takes AC-3 but not E-AC-3, or a decoded PCM leg over MonitorSink
    // when it takes neither - or the plain refusal 'play' always gave before
    // roadmap UX9 (follow=off). Same on-by-default, off-to-restore-the-old-
    // behaviour shape as downmix_leg above.
    bool follow_sink = true;
    // Off by default, matching every bare token here - keep whatever frames
    // a failed encode already produced, written beside the intended output
    // as <name>.partial.<ext> instead of discarded outright. The same
    // "named and kept, never silently discarded" behaviour the GUI's own
    // keepPartialOutput preference gives EncoderController's file encodes
    // (see gui/encoder_controller.cpp's partial_output_path), offered here
    // per invocation rather than as a standing preference - see
    // write_partial_output.
    bool keep_partial = false;
    // The §7.9.4 fast forward MDCT, on by default like the library configs
    // it feeds; fast-mdct=off forces the direct §8.2.3.2 reference form
    // wherever this command encodes (encode/sine and the atmos/record/live
    // session builders, via plan::Tools::fast_mdct) AND wherever it decodes
    // JOC's own bed analysis under joc-domain=mdct (via
    // DecoderConfig::fast_mdct, PF8 - a decode's only forward transform,
    // reached from 'decode'/'monitor'/'live'; QMF-domain reconstruction,
    // the default, has no forward/direct choice to make). Same key=off
    // shape surmixlev=/lfemix= already use. E-AC-3's own tools= string
    // reaches the encode-side field with its own tokens ("nofastmdct" to
    // force direct, matching "noatten"; the old opt-in "fastmdct" parses as
    // a no-op) - AC-3 has no tools= string to extend, so this option is its
    // equivalent, the same relationship 'couple' has to cpl/cpl:N. The bare
    // word 'fast-mdct' (the opt-in spelling from when this defaulted off)
    // stays accepted and now names what already happens.
    bool fast_mdct = true;
    // The decode-side counterpart for INVERSE transforms: §7.9.4 step 3's
    // complex transform via the radix-2 FFT instead of the pseudocode's
    // direct sum (DecoderConfig::fast_imdct - see its own comment for the
    // accepted quality evidence). Covers PCM reconstruction, enhanced
    // coupling and JOC object synthesis; fast_mdct above is the one FORWARD
    // exception (JOC bed analysis). On by default like the library config
    // it feeds; fast-imdct=off - or mode=reference, which turns this AND
    // fast_mdct off together - forces the direct evaluation for runs where
    // agreement with the spec's stated arithmetic matters more than speed.
    // 'decode' reads it; the QC/levels/playback decoders stay on the
    // library default, where a ~1e-12 difference cannot move a reported
    // figure.
    bool fast_imdct = true;
    // The two verbosity tokens, recorded here as well as in the file-scope
    // flags set_verbosity settles (see above): a command that wants to reason
    // about them - run_live names each leg only when verbose - reads them off
    // the Options it already has rather than calling back into a global.
    bool quiet = false;
    bool verbose = false;
    // Which domain JOC estimates and applies its reconstruction matrix in
    // (AtmosConfig::joc_domain / DecoderConfig::joc_domain). QMF - §7.1's
    // 64-band complex filterbank, what §6.6.6 describes and what a licensed
    // decoder runs - is the default; joc-domain=mdct selects the cheaper
    // 256-bin MDCT approximation this project used before it had a
    // filterbank.
    //
    // Deliberately outside mode= in both directions. The two transform
    // switches mode= drives are the same answer computed two ways, agreeing
    // to ~1e-12, so naming the fast one costs nothing; these two domains
    // are different answers about 5 dB apart, and a speed preference should
    // not silently pick the worse one. mode=reference has nothing to add
    // either - the default is already §6.6.6's own domain - so mode= stays
    // exactly the two transform switches it has always been.
    ac3::joc::Domain joc_domain = ac3::joc::Domain::kQmf;
    // The per-frame search over §7.2.2's transmitted bit allocation
    // parameters, judged on the reconstruction error the decoder will
    // produce (EncoderConfig::search, ac3/quality/distortion.hpp).
    // search=distortion minimises that error; search=perceptual weights it
    // by a tonality/masking model first. Off by default, like the library
    // config it feeds - it costs encode time, and this project does not turn
    // a decision knob on without the numbers. AC-3 encodes only.
    ac3::quality::Criterion search = ac3::quality::Criterion::kNone;
    // §7.2.2.4 fast gain, Table 7.11 - the OTHER axis search= moves, offered
    // here as a pin for the runs that want one code held across a whole
    // encode rather than chosen per frame (plan::Tools::fgaincod, reaching
    // EncoderConfig::fgaincod and eac3::FrameConfig::fgaincod). -1 is
    // 'auto', which means different things to the two codecs and
    // deliberately so: AC-3 hangs fgaincod off an element it already sends
    // every block, so auto follows ac3::rate_adaptive_fgaincod()'s measured
    // curve for free; E-AC-3's baie does not carry fgaincod at all, so auto
    // leaves Table E1.4's implied 0x4 and writes no element. Pinning 0..7
    // makes E-AC-3 pay for the per-block fgaincode element in all six
    // blocks - which is exactly the trade this option exists to let a
    // measurement run put a number on. Not command-scoped, for the same
    // reason dither=/search= are not: every command that encodes at all can
    // answer it, in either codec.
    int fgaincod = -1;
    // 'probe' only: emit the JSON document (schema ac3forge.probe/1) instead
    // of the human-readable table. Off by default - a bare `ac3cli probe
    // <file>` is meant to be read by a person, and every other command here
    // prints for one too.
    bool json = false;
    // §7.3.4 dithflag (plan::Tools::dither), on by default like the library
    // configs it feeds; dither=off pins it at 0 unconditionally wherever this
    // command encodes, the same key=off shape fast-mdct=off already uses -
    // AC-3 has no tools= string, so this is that field's equivalent. E-AC-3's
    // own tools= string reaches the same field with "nodither". The only
    // reason to reach for this: a caller needs bit-for-bit agreement between
    // two decoders of the SAME encode more than it needs dither's real
    // perceptual benefit - real dither values are decoder-defined, so two
    // independent, spec-correct decoders diverge in the dithered bins by
    // design (see EncoderConfig::dither's own comment), which is exactly
    // what tools/checks/verify_gold_reference.sh needs this for.
    bool dither = true;
    // Whether channels= or downmix= actually named a target this run, so the
    // two can cooperate without either silently winning: downmix=ltrt on its
    // own means stereo, channels=2 on its own means Lo/Ro, and the pair in
    // either order means what both said.
    bool downmix_named = false;
    // 'decode'/'monitor' only: §7.10 error concealment. Off by default, so a
    // damaged frame is still reported rather than papered over.
    ac3::ConcealmentPolicy concealment = ac3::ConcealmentPolicy::kNone;
    // 'transcode' only: the OUTPUT codec, when out_path's own suffix cannot
    // say (stdout, or a file named something other than .ac3/.ec3). Unset
    // means "take it from the suffix", which is what every ordinary
    // invocation does.
    std::optional<ac3::plan::Codec> codec = std::nullopt;
    // Whether dialnorm=/dialnorm2= appeared on the command line at all, as
    // opposed to `p.dialnorm` merely holding its default of 31. Only
    // 'transcode' reads these, and only because its default is to PRESERVE
    // the source stream's own value: without this it could not tell an
    // explicit `dialnorm=31` from silence on the subject, and would quietly
    // preserve 27 for an operator who asked for 31.
    bool dialnorm_given = false;
    bool dialnorm2_given = false;
    // 'metadata' only: the §7.7.2 compr word (and Ch2's own) to STAMP onto
    // an existing stream, as the 8-bit wire value the requested dB gain
    // implies. Distinct from `p.heavy`, which asks an ENCODER to derive one
    // from the signal - there is no signal to derive from here, only bits to
    // overwrite, and only where the stream already carries a compr word.
    std::optional<std::uint8_t> compr_word = std::nullopt;
    std::optional<std::uint8_t> compr2_word = std::nullopt;
    // 'metadata' only: Table 5.5's service type and Table 5.11's Dolby
    // Surround mode. Neither has an encode-side equivalent in plan::Metadata
    // - this project's encoders write bsmod 0 and dsurmod 0 unconditionally -
    // so these exist for the rewrite path alone.
    std::optional<int> bsmod = std::nullopt;
    std::optional<int> dsurmod = std::nullopt;
    // 'decode'/'qc'/'levels': which programme of a multi-programme E-AC-3
    // stream to work on - the §E2.3.1.2 substreamid of its independent
    // substream. Unset takes the first programme the stream carries, which is
    // the only one there is for effectively all content; the commands say so
    // when a stream turns out to carry more than one. Never a fold of several
    // programmes: they are alternatives (a second language, an audio
    // description), not layers, so mixing them is never what a caller wants.
    std::optional<int> programme;
    // 'eac3-encode': a SECOND programme to author into the same stream as a
    // second independent substream (§E2.3.1.2). Unset - the default - writes
    // the single-programme stream this command always has.
    std::optional<std::string> programme2;
    // That programme's own layout token, bit rate and dialnorm. Empty/unset
    // follow the second source's own channel count, half the primary's rate
    // (an associated service is normally much narrower than the main mix) and
    // dialnorm 31. Its own, not the primary's: a commentary track is levelled
    // independently of the mix it is played against, which is the whole point
    // of carrying it as a separate programme.
    std::string programme2_layout;
    std::optional<std::uint32_t> programme2_bitrate;
    int programme2_dialnorm = 31;
    // 'qc' only: which soundfield to meter. false (layout=bed, the default)
    // measures the independent substream's own Table 5.8 bed through
    // BS.1770 Annex 1's basic algorithm - what this command has always
    // done. true (layout=rendered) measures the whole assembled program,
    // every dependent substream's height/wide/rear channels included,
    // through BS.1770-5 Annex 3's extended algorithm. See run_qc.
    bool qc_rendered_layout = false;
    // 'qc' only: objects=<layout> (roadmap IO12). Set when the stream's
    // dynamic objects should be re-rendered by their own OAMD position onto
    // the named advanced sound system layout and metered through BS.1770-5
    // Annex 4, instead of (or as well as - the two are independent switches)
    // the channel-based measurement layout= above selects. See run_qc.
    std::optional<ac3::plan::LayoutId> qc_objects_layout;
};

// Returns false and prints the offending token on anything unrecognised: a
// silently ignored metadata flag looks exactly like metadata that did not work.
//
// `command` decides what `layout=` means: `qc`'s own is a bed/rendered switch
// (see Options::qc_rendered_layout's comment), record/live's is a channel
// layout name or list (Options::take_layout's). The two commands settled on
// the same token independently - matching the GUI's own "layout" language in
// each context - so this is the one place that has to know which command is
// asking, everywhere else in this function stays command-agnostic.
bool parse_options(std::span<char*> tokens, Options& out, std::string_view command);

// Reads a loudness measurement someone else already pushed every sample
// into, reports it the same way every dialnorm=auto path does, and returns
// the dialnorm it implies. Factored out of measured_dialnorm/
// measured_dialnorm_channel below so a measurement built incrementally
// across many frames (the src=/map= routed-programme pre-pass) reports
// itself identically to one built from a single whole-buffer push - same
// text, same rounding, one place either could go wrong. `programme` is the
// println's leading label ("Ch1"/"Ch2"), empty for a whole-programme
// measurement that is not about one dual-mono channel; `field` is the
// bitstream field this measurement feeds ("dialnorm"/"dialnorm2"). `out`
// defaults to stdout for callers with no "-" output stream to protect (the
// standalone loudness command); every dialnorm=auto/dialnorm2=auto encode
// path passes status_stream(out) instead, the same convention
// print_channel_summary and print_routing use.
std::optional<int> finish_measurement(const ac3::meta::LoudnessMeter& meter,
                                      std::string_view programme, std::string_view field,
                                      FILE* out = stdout);

// BS.1770 integrated loudness of a whole WAV, and the dialnorm it implies.
// Never meaningful for a dual-mono (1+1) target - Ch1 and Ch2 are two
// unrelated programmes sharing one syncframe (§E1.3, no downmix between
// them), so a single BS.1770 pass across both channels would measure a
// blend of two different things rather than either programme's own level;
// callers route dual mono through measured_dialnorm_channel on each
// programme's own channel alone instead.
std::optional<int> measured_dialnorm(const ac3::io::WavData& wav, ac3::SampleRate rate,
                                     ac3::Acmod acmod, bool lfe, FILE* out = stdout);

// Same measurement, for one dual-mono programme's own channel alone - never a
// programme's worth of BS.1770 surround weighting, since a 1+1 channel is not
// part of a soundfield. `programme`/`field` are finish_measurement's own
// labels above - "Ch1"/"dialnorm" or "Ch2"/"dialnorm2", the two programmes
// sharing this one function since the measurement itself does not differ.
std::optional<int> measured_dialnorm_channel(std::span<const float> channel, ac3::SampleRate rate,
                                             std::string_view programme, std::string_view field,
                                             FILE* out = stdout);

// Dual mono's Ch1/Ch2 arrive as either one two-channel file or two mono ones;
// this settles which shape `wav` is in and merges a second file's channel in
// when there is one, so everything downstream sees a plain two-channel source
// the same way it always has - `plan::route`'s own 1+1 handling only ever
// looks at the channel count, never how many files it came from.
bool prepare_dual_mono_source(ac3::io::WavData& wav, std::string_view layout,
                              std::string_view in2_path);

// The conventional Unix "-" file argument: a lone dash means stdin for an
// input path or stdout for an output path, the same convention ffmpeg, sox
// and most other Unix tools use for pipe-based workflows (e.g.
// `ac3cli encode - - 448 couple < in.wav > out.ac3`). Checked by exact
// string match only - a path that merely starts with '-' is an ordinary
// (if oddly named) filename, not this convention.
bool is_stdio_path(std::string_view path);

// Where a command's human-readable status report goes, once out_path's own
// destination is settled: stdout as always, unless out_path IS "-" - the
// binary payload itself is going to stdout then, and a status line like
// "encoded N frames..." landing in the middle of that stream would corrupt
// whatever is reading it downstream. The same split ffmpeg and friends make
// between their progress/log output and the media they actually pipe.
// Under `quiet` this returns nullptr instead - "nowhere" - which every
// status printer here treats as "print nothing". nullptr rather than the
// platform's null device: a FILE* to NUL/dev/null would need a per-platform
// name in a tree that deliberately has no preprocessor conditionals, and a
// discarded write is cheaper than a real one to a real handle anyway.
//
// What quiet does NOT silence is a REPORTING command's report - 'levels',
// 'loudness', 'qc', 'devices' and 'outputs' print their answer with plain
// fmt::println, because that answer is the command's output rather than
// commentary on it. Silencing those would leave the command doing nothing
// observable at all.
FILE* status_stream(std::string_view out_path);

// The same, for a command with no "-"-capable output path to protect: stdout,
// or nowhere under quiet.
FILE* status_stream();

// The programme ids ac3::programme_ids() found, as "0, 1" - what every
// command that takes programme= prints when a stream turns out to carry more
// than one, and what it lists back when the id asked for is not among them.
std::string format_programme_ids(std::span<const int> ids);

// The programme a command should work on: `wanted` when the stream carries it,
// else the first one it does carry; a message on stderr and std::nullopt when
// `wanted` names a programme that is not there. `ids` is what
// ac3::programme_ids() returned and must not be empty. Shared by decode, qc
// and levels so all three answer a bad programme= the same way.
std::optional<int> choose_programme(std::span<const int> ids, std::optional<int> wanted);

// fmt::println with a "nowhere" destination: a no-op when `out` is nullptr
// (see status_stream above), an ordinary println otherwise. Every status line
// in this CLI goes through this, so `quiet` is honoured in one place rather
// than at each site.
template <typename... Args>
void status_println(FILE* out, fmt::format_string<Args...> format, Args&&... args) {
    if (out != nullptr) {
        fmt::println(out, format, std::forward<Args>(args)...);
    }
}

inline void status_println(FILE* out) {
    if (out != nullptr) {
        fmt::println(out, "");
    }
}

// A one-line "done / total" report on stderr for a run long enough to be
// worth watching, rewritten in place the way print_live_meter's own line is.
// stderr, never stdout: a '-' output owns stdout, and a progress line in the
// middle of a piped elementary stream would corrupt whatever is reading it.
//
// Off for a short run unless `verbose` asked for it, and off entirely under
// `quiet` - a two-second encode that prints a progress bar is noise, and the
// point of the token pair is that a script can choose. start() decides once;
// tick() and finish() do nothing at all when it decided no.
class Progress {
   public:
    // `verb` leads the line ("encoding", "decoding"); `total` is the unit
    // count when it is known up front (frames or access units), 0 when it is
    // not - the line then counts up without a percentage.
    void start(std::string_view verb, std::uint64_t total);
    void tick(std::uint64_t done);
    // Prints the finished line and ends it, so a captured log keeps the
    // final count instead of a half-overwritten one.
    void finish();

   private:
    bool active_ = false;
    std::string verb_;
    std::uint64_t total_ = 0;
    std::uint64_t done_ = 0;
    std::chrono::steady_clock::time_point last_{};
};

bool write_frames(std::string_view path, std::span<const std::vector<std::byte>> frames);

// Where a failed encode's frames land when keep-partial is given: ".partial"
// spliced in before the suffix, so "out.ec3" keeps its half-finished take as
// "out.partial.ec3" - the same naming EncoderController::partial_output_path
// gives the GUI's own keepPartialOutput preference (see gui/
// encoder_controller.cpp), so a file produced either way is named alike.
std::string partial_output_path(std::string_view path);

// One frame written `count` times, for the silence generators: they used to
// materialise `count` identical copies of a single ~2 KB frame first (~268
// MB for an hour of silence) purely to satisfy write_frames' list shape.
bool write_repeated_frame(std::string_view path, std::span<const std::byte> frame,
                          std::uint64_t count);

// Writes whatever frames a failed encode already produced to
// partial_output_path(out_path) when keep_partial asked for it and there is
// at least one - "named and kept, never silently discarded", the same rule
// the GUI's own keep-partial-output preference follows. A no-op (silently)
// when keep_partial is false or nothing was encoded yet; a write failure for
// the partial itself is reported but does not change the caller's own exit
// code, since the ORIGINAL error is still the one that matters.
void write_partial_output(std::string_view out_path, bool keep_partial,
                          std::span<const std::vector<std::byte>> frames);

// Streams encoded frames to their destination as they are produced, so an
// encode's output no longer accumulates (~3.4 MB per minute at 448 kbps -
// the last O(duration) term the encode commands carried once their input
// went streaming). A file destination is written incrementally; abort() -
// the encoder failed mid-stream - honours keep-partial exactly as
// write_partial_output does: the bytes already written are renamed to
// partial_output_path() and reported when asked for, deleted otherwise, so
// a failed run's observable outcome is unchanged. "-" accumulates and
// writes stdout once at close(): a pipe cannot take bytes back, and a
// failed run without keep-partial must leave stdout untouched. Tracks the
// per-frame size stats the E-AC-3 VBR report used to re-walk its frame
// list for.
//
// `defer` keeps the frames instead of streaming them - for the Atmos
// commands' sign-objects path, where apply_object_signing rewrites every
// frame AFTER the encode loop and the bytes therefore cannot leave until
// then. Deferred frames are reachable through deferred() for exactly that
// rewrite; close() then writes them all (write_frames) and abort() hands
// them to write_partial_output, so the defer path IS the pre-sink code
// shape, just held behind the same five-call interface the streaming path
// uses.
class EncodedStreamSink {
   public:
    [[nodiscard]] bool open(std::string_view path, bool keep_partial, bool defer = false);
    [[nodiscard]] bool push(std::span<const std::byte> frame);
    // Keeps the vector's own allocation when deferring (the callers all
    // have one to give up); the streaming path just forwards to the span
    // overload.
    [[nodiscard]] bool push(std::vector<std::byte>&& frame);
    // Success path; flushes the "-" buffer / writes the deferred frames.
    // False if the destination failed.
    [[nodiscard]] bool close();
    void abort();

    [[nodiscard]] std::vector<std::vector<std::byte>>& deferred() { return deferred_; }

    [[nodiscard]] std::size_t frames() const { return frames_; }
    [[nodiscard]] std::size_t min_bytes() const { return min_bytes_; }
    [[nodiscard]] std::size_t max_bytes() const { return max_bytes_; }
    [[nodiscard]] std::uint64_t total_bytes() const { return total_bytes_; }

   private:
    std::string path_;
    bool stdio_ = false;
    bool keep_partial_ = false;
    bool defer_ = false;
    bool open_ = false;
    std::ofstream file_;
    std::vector<std::byte> buffered_;                 // "-" only
    std::vector<std::vector<std::byte>> deferred_;    // defer only
    std::size_t frames_ = 0;
    std::size_t min_bytes_ = 0;
    std::size_t max_bytes_ = 0;
    std::uint64_t total_bytes_ = 0;
};

// Interleaves `channels` (one vector per decoded channel, AC-3/E-AC-3 coded
// order) into WAV/Windows speaker order for playback, reading order[i] as
// which channels[] entry belongs at interleaved position i - the same
// permutation ac3::io::write_wav_f32 and plan::wav_order/wav_channel_order
// already produce for exactly this AC-3-order-vs-WAV-order reconciliation
// (see ac3/io/wav.hpp).
std::vector<float> interleave_reordered(std::span<const std::vector<float>> channels,
                                        std::span<const std::size_t> order);

std::vector<std::byte> read_all(std::string_view path);

// The elementary stream at `in_path`: `in_path`'s own bytes verbatim if it is
// already one, or (roadmap IO2) the first AC-3/E-AC-3 track demuxed out of a
// recognised Matroska/MP4/MPEG-TS container, via apps/common/
// container_input.hpp's ac3::apps::elementary_stream_from_bytes - the same
// three readers `ac3cli demux` already streams through, run here in their
// batch/zero-copy form since every caller has the file resident anyway.
// `decode`, `qc`, `levels`, `play` and `monitor` all used to call
// read_all(path) directly and now call this instead, so all five accept a
// container in place of a raw .ac3/.ec3 with no other change to how they
// work.
//
// Prints its own error and returns empty on ANY failure - a missing file, an
// unreadable one, or a recognised container with no AC-3/E-AC-3 track - so a
// caller's own "cannot read" message is not also needed; every existing
// caller's `if (stream.empty()) { ...; return kExitInput; }` guard already
// does the right thing with an empty result regardless of which of those it
// was.
[[nodiscard]] std::vector<std::byte> read_elementary_stream(std::string_view in_path);

// Wraps ac3::io::read_wav to honor the "-" stdin convention (is_stdio_path
// above): "-" reads the WAV from stdin, binary mode set first, instead of
// opening a file with that literal name.
std::expected<ac3::io::WavData, ac3::io::WavError> read_wav_arg(std::string_view path);

// Streams planar float channels into a WAV as they decode, so the decoded
// programme never sits in memory whole (it used to: ~69 MB per minute of
// 5.1). Channels arrive per SLOT and may momentarily advance unevenly -
// E-AC-3's transient-pre-noise flush appends per mapped slot - so each slot
// keeps a small carry, and whole interleaved frames go to WavStreamWriter as
// soon as every slot has them. Two deliberate fallbacks: out_path "-"
// accumulates and writes in one shot at close (stdout cannot seek, and
// WavStreamWriter patches its header), and any residue left by slots of
// unequal final length is dropped with a warning - the whole-buffer write
// this replaces indexed every channel to the first one's length, so equal
// lengths are the only case that ever actually occurred.
class PlanarWavSink {
   public:
    // `order`: entry i names the source slot that belongs at WAV position i
    // (write_wav_f32's convention); empty means identity.
    [[nodiscard]] bool open(std::string_view path, std::uint32_t sample_rate, std::size_t slots,
                            std::span<const std::size_t> order);

    [[nodiscard]] bool is_open() const { return open_; }

    [[nodiscard]] bool append(std::size_t slot, std::span<const float> samples);

    // Finalize; reports whether the write side stayed healthy. Unequal
    // residue across slots (never produced by a healthy stream) is dropped.
    [[nodiscard]] std::expected<void, ac3::io::WavError> close();

    // The decode failed part-way: close and remove whatever was written, so
    // a failed run leaves no output file - exactly like the whole-buffer
    // write it replaces, which never ran at all on failure.
    void abort();

   private:
    [[nodiscard]] bool drain();

    std::string path_;
    bool stdio_ = false;
    bool open_ = false;
    std::uint32_t sample_rate_ = 0;
    ac3::io::WavStreamWriter writer_;
    std::vector<std::vector<float>> slots_;
    std::vector<std::size_t> consumed_;
    std::vector<std::size_t> order_;
    std::vector<float> scratch_;
};

// Streams raw little-endian PCM16 payload bytes into a WAV as they are
// produced - 'spdif''s IEC 61937 burst carrier, whose payload used to
// accumulate whole before one write_wav_pcm16_raw() call (~0.7 MB per
// second at an E-AC-3 4x carrier rate, the largest O(duration) term the
// CLI had left). The header is byte-identical to write_wav_pcm16_raw's;
// its two size fields are patched at close(), because an E-AC-3 burst
// payload's length is only known once the packer has seen the last unit.
class Pcm16RawWavSink {
   public:
    [[nodiscard]] bool open(std::string_view path, std::uint32_t sample_rate,
                            std::uint16_t channels);

    [[nodiscard]] bool is_open() const { return open_; }

    [[nodiscard]] bool push(std::span<const std::byte> bytes);

    // Finalize: patch the RIFF/data sizes to what was actually written.
    [[nodiscard]] bool close();

    // The wrap failed part-way: close and remove whatever was written, so a
    // failed run leaves no output file - exactly like the whole-buffer
    // write this replaces, which never ran at all on failure.
    void abort();

   private:
    std::string path_;
    bool open_ = false;
    std::fstream file_;
    std::uint64_t data_bytes_ = 0;
};

// ---------------------------------------------------------------------------
// Level reporting. Every number comes from ac3::analysis, so a level reads
// the same here as on the GUI's meters; only the drawing is local.
// ---------------------------------------------------------------------------

// A bar on the same -60..0 dBFS scale the GUI's meters use. ASCII rather than
// block glyphs: this has to stay legible in a bare console whatever code page
// it happens to be running.
std::string meter_bar(double db, int width);

// The exact figures for a finished run. Peak and RMS here are unweighted over
// the whole signal — ballistics exist to make a moving display readable, and
// would only blur a question that has a right answer.
// `out` defaults to stdout for every existing caller; the only ones that
// pass anything else are the "-" stdout-output commands (encode/eac3-encode/
// atmos-encode/decode), which redirect it to stderr so this human-readable
// report doesn't land in the middle of the binary stream those commands may
// be writing to the very same stdout - see status_stream()'s own comment.
void print_channel_summary(const ac3::analysis::LevelMeter& meter, FILE* out = stdout);

// One line, rewritten in place. A carriage return rather than ANSI cursor
// moves, so it behaves the same in a bare console as in a terminal that
// speaks escape sequences. Every field is fixed width, so the line never
// leaves fragments of a longer previous line behind.
void print_live_meter(const ac3::analysis::LevelMeter& meter, double seconds);

// Sets `plan`'s channels from `name` and writes a human-readable label for
// it into `label`, reporting a bad token against the set the codec can
// actually carry (so asking AC-3 for 7.1.4 says which of the two things is
// wrong) or false on anything neither a named layout nor a channel list
// accepts. Tried in that order: a name recognised by parse_layout wins, so a
// custom list can never shadow one of the seven presets.
bool resolve_layout(std::string_view name, ac3::plan::Codec codec, ac3::plan::Plan& plan,
                    std::string& label);

// What `record`/`live` resolved their layout=/codec=/bitrate into: one
// plan::Plan, the label to print for it, and the two facts every caller
// immediately needs from it (which codec, how many coded channels). Shared
// because the two commands must agree exactly - a take is a take whether or
// not it also monitors and passes through, and roadmap IO9's whole point is
// that neither is stereo-AC-3-only any more.
//
// codec= forces the codec; without it, the codec is derived - AC-3 unless the
// layout needs the dependent substreams only E-AC-3 has, the same
// plan::carries() answer plan::derive_codec would give for a file encode.
struct TakePlan {
    ac3::plan::Plan plan;
    std::string label;
    bool eac3 = false;
    // What the encoder is fed: bed plus every dependent substream's channels.
    int coded_channels = 0;
    // What a decoder renders from them - fewer than coded_channels wherever a
    // dependent REPLACES a bed channel (7.1 renders 8 speakers from 10 coded).
    // The container and the monitor both want this one: 'mkv'/'ts' scanning
    // the same finished stream count the channels it renders (ac3::io::scan),
    // so a streamed take must declare the same number the after-the-fact wrap
    // would, and MonitorSink is fed the decoder's own rendered channels.
    int rendered_channels = 0;
};

// nullopt with the reason already printed: a bad layout name, a layout the
// forced codec cannot carry, or a bitrate that codec has no frame size for.
std::optional<TakePlan> resolve_take_plan(const Options& meta, std::uint32_t bitrate,
                                          ac3::SampleRate rate);

// The RecordingSink::Config a resolved take implies, so 'record' and 'live'
// cannot describe the same take differently to the container.
RecordingSink::Config take_sink_config(const Options& meta, const TakePlan& take,
                                       std::uint32_t sample_rate_hz);

// One dynamic object's source taps: (flattened source channel, linear gain).
// The flattened space concatenates every source's channels in load order -
// source 0's first, then source 1's - which is the same numbering
// gather_frame() fills and the same one `live`'s two capture devices use.
//
// One tap is a plain `obj` row. Several are `objm`: a contiguous range of ONE
// source's channels folded to a single mono object, each tap already scaled by
// 1/n so several full-range channels summed together do not clip past what one
// alone would (ac3::plan::DestinationKind::kObjectMono's own contract). A slot
// with no taps is allocated but unbound, and carried silent - the state
// `live objects=<N>` leaves a slot in when nothing is mapped onto it.
struct ObjectSlot {
    std::vector<std::pair<std::size_t, double>> taps;
};

// The object slots a map= assignment describes, over `shapes`' flattened
// channel space: every `obj` row its own slot first, in (source, channel)
// order, then each maximal contiguous run of `objm` rows within one source
// folded to one. Empty when the assignment names no object destination at all
// - which is a real answer (a purely location-mapped assignment), not an
// error, so the caller decides what to do about it.
//
// Shared by `atmos-encode` and `live mode=atmos` so that the objects a given
// map= produces are the same objects either way - roadmap IO9's actual point:
// a GUI assignment reproduced headlessly has to reproduce.
[[nodiscard]] std::vector<ObjectSlot> object_slots_from_assignment(
    const ac3::plan::Assignment& assignment, std::span<const ac3::plan::SourceShape> shapes);

// What a "wrote N frames to <path>" line says about the container it went
// into - " (Matroska)", " (MPEG-TS)", " (IEC 61937 WAV carrier)", or nothing
// at all for the bare elementary stream, which is what the path's own suffix
// already says. One function so 'record' and 'live' word it identically.
std::string_view container_note(RecordingSink::Container container);

// A WAV's rate as an fscod (or, for E-AC-3, fscod2), or a diagnosis. Shared
// because every encode path asks the same question. Classic AC-3 has only
// A/52 Table 5.6's three rates; E-AC-3 additionally accepts the three Annex E
// fscod2 half rates (24/22.05/16 kHz), which have no AC-3 counterpart at all.
std::optional<ac3::SampleRate> wav_sample_rate(std::uint32_t hz, std::string_view codec, bool eac3);

// A source's channels routed onto a plan's coded channels, or a diagnosis.
std::optional<ac3::plan::Routing> routing_or_error(const ac3::plan::Plan& p, std::size_t channels);

// Checks EMDF object signatures on a stream about to be decoded/monitored,
// when the operator asked for it (verify-objects) and supplied a key. Reads
// the raw stream bytes the same way sign_atmos_stream does - independent of,
// and either before or alongside, whatever Eac3Decoder itself does with
// those same bytes; never routed through it, since that class's own stance
// is that the protection field is opaque per spec (see decoder.hpp). Returns
// the summary, or nullopt if verification was requested but the key could
// not be loaded, or if any signed frame's tag did not match (both cases
// already print their own message). Not requested -> an all-zero summary,
// nothing checked, stream untouched either way: this only reads bytes, it
// never signs. A signed stream is either fully verified or the command
// refuses - matching this project's own "graceful 5.1 fallback is
// either/or" stance - never a silent partial pass.
std::optional<ac3::signing::VerifySummary> apply_object_verification(
    std::span<const std::byte> stream, const Options& meta);

}  // namespace ac3cli
