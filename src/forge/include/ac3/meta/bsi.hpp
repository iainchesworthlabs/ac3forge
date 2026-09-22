#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "ac3/core/tables.hpp"
#include "ac3/export.hpp"
#include "ac3/meta/mixing.hpp"

// The bit stream information a frame carries ABOUT itself, as opposed to the
// coding parameters that say how to decode it.
//
// None of it changes a single output sample. What it does decide is whether a
// receiver can tell a complete main programme from an audio-description track,
// whether a stereo pair is a Dolby Surround matrix that wants a Pro Logic
// decoder behind it, and what acoustic level the mix was actually judged at.
// AC-3 spreads the group across bsi (§5.4.2); E-AC-3 gathers the same fields
// into one optional infomdat element (Table E1.2, §E2.3.1.62 - which defers to
// §5.4.2 for every one of them bar sourcefscod).
//
// Annex D is the other half of this header. AC-3's two 14-bit timecod fields
// "have never been applied for their originally anticipated purpose" (§D1), so
// Annex D reuses them: a stream that sets bsid to 6 spends those same 28 bits
// on separate Lt/Rt and Lo/Ro downmix levels, a preferred-downmix indication,
// and the Surround EX / Dolby Headphone / A-D converter flags. The two layouts
// are the same size to the bit, which is why §D3.2 can promise a legacy
// decoder reads a bsid-6 stream as timecode it already ignores rather than
// losing sync.

namespace ac3::meta {

// §5.4.2.2, Table 5.7. Code 7 is two things at once: an associated voice-over
// service at acmod 1/0, and a main karaoke service at anything wider. There is
// no bit that distinguishes them - acmod does - so one enumerator names both
// and describe() reads acmod to say which.
enum class BitstreamMode : std::uint8_t {
    kCompleteMain = 0,
    kMusicAndEffects = 1,
    kVisuallyImpaired = 2,
    kHearingImpaired = 3,
    kDialogue = 4,
    kCommentary = 5,
    kEmergency = 6,
    kVoiceOverOrKaraoke = 7,
};

// §5.4.2.6, Table 5.11: whether a 2/0 pair is a Dolby Surround matrix. '11' is
// reserved and reads as "not indicated".
enum class SurroundMode : std::uint8_t {
    kNotIndicated = 0,
    kNotDolbySurround = 1,
    kDolbySurround = 2,
};

// Table D2.7 (AC-3 xbsi2) / Table E1.2's infomdat. '01' is a positive
// statement that the programme is NOT Surround EX, which is not the same
// claim as '00' "not indicated" - a receiver may auto-engage its EX decoder
// on '10' and must not on '01', while '00' leaves the choice to the listener.
enum class SurroundExMode : std::uint8_t {
    kNotIndicated = 0,
    kNotSurroundEx = 1,
    kSurroundEx = 2,  // Dolby Surround EX or Pro Logic IIx
    kProLogicIIz = 3,
};

// Table D2.8. '11' is reserved and reads as "not indicated".
enum class HeadphoneMode : std::uint8_t {
    kNotIndicated = 0,
    kNotDolbyHeadphone = 1,
    kDolbyHeadphone = 2,
};

// Table D2.9. §D2.3.1.10: "If the type of A/D converter used is not known,
// the 'Standard' setting should be chosen", so there is no "not indicated"
// here and the default is a real claim rather than a blank.
enum class AdConverterType : std::uint8_t {
    kStandard = 0,
    kHdcd = 1,
};

// §5.4.2.15, Table 5.12. '11' is reserved and reads as "not indicated".
enum class RoomType : std::uint8_t {
    kNotIndicated = 0,
    kLargeRoomXCurve = 1,
    kSmallRoomFlat = 2,
};

// §5.4.2.14/15: the acoustic level and monitoring environment the final mix
// was judged at. mixlevel is a 5-bit code for 80 + code dB SPL, so 0..31 spans
// 80..111 dB SPL.
//
// adconvtyp belongs to this group only in E-AC-3, whose infomdat carries it
// beside roomtyp (Table E1.2). AC-3's own audprodie stops after roomtyp and
// puts adconvtyp in Annex D's xbsi2 instead, so the value here is ignored when
// this struct is written into an AC-3 bsi - AlternateBsi::adconvtyp is the one
// that reaches the wire there.
struct AudioProduction {
    int mixlevel = 0;  // 0..31, i.e. 80..111 dB SPL
    RoomType roomtyp = RoomType::kNotIndicated;
    AdConverterType adconvtyp = AdConverterType::kStandard;
};

inline constexpr int kMixLevelBaseDbSpl = 80;

[[nodiscard]] constexpr int mix_level_db_spl(int mixlevel) {
    return kMixLevelBaseDbSpl + mixlevel;
}

// §5.4.2.26-28: a 28-bit time code split into a low-resolution half counting
// 8-second increments up to 24 hours and a high-resolution half counting
// 1/64ths of a 1/30 s frame up to 8 seconds. Either half may be sent alone
// (Table 5.13), which is why they are two structs rather than one - "the
// coarse half only" is a real, legal state and not a partially filled value.
//
// AC-3 bsid 8 only. Annex D spends the same bits on xbsi1/xbsi2, and E-AC-3
// has no time code field at all.
struct TimeCodeCoarse {
    int hours = 0;          // 0..23
    int minutes = 0;        // 0..59
    int eight_seconds = 0;  // 0..7, i.e. 0, 8, 16 ... 56 seconds
};

struct TimeCodeFine {
    int seconds = 0;        // 0..7, within the coarse half's 8-second bucket
    int frames = 0;         // 0..29, at 30 frames per second
    int sixty_fourths = 0;  // 0..63 of one frame
};

// The whole informational group, in one struct both codecs fill.
//
// Which fields actually reach the wire depends on acmod and on the codec, per
// §5.4.2 and Table E1.2 - a 3/2 stream sends no dsurmod, a stream that is not
// 2/2 or 3/2 sends no dsurexmod, and so on. The values here are what goes out
// when the corresponding field exists, so a caller may set the lot and let the
// layout decide, exactly as MixMetadata already works.
//
// Everything defaults to the value the encoder wrote before any of this was
// configurable, so a config that says nothing about it produces the same
// stream it always did.
struct BsiInfo {
    // §5.4.2.2. The one field here a receiver acts on rather than merely
    // displays: ATSC A/53 and DVB both key associated-service handling off it.
    BitstreamMode bsmod = BitstreamMode::kCompleteMain;
    // §5.4.2.6 / Table E1.2's infomdat. 2/0 only.
    SurroundMode dsurmod = SurroundMode::kNotIndicated;
    // Table E1.2's infomdat, 2/0 only. In AC-3 this same field lives in
    // Annex D's xbsi2 (AlternateBsi::dheadphonmod) instead, and a bsid-8
    // stream has nowhere to put it at all.
    HeadphoneMode dheadphonmod = HeadphoneMode::kNotIndicated;
    // Table E1.2's infomdat, acmod >= 0x6 (2/2 and 3/2) only. Same split as
    // dheadphonmod above: AC-3 carries it in xbsi2.
    SurroundExMode dsurexmod = SurroundExMode::kNotIndicated;
    // §5.4.2.11/12: langcod is "an 8 bit reserved value that shall be set to
    // 0xFF if present", the language table having been dropped in favour of
    // the signalling layer's own ISO 639-2 code. So there is nothing to
    // choose but whether the byte is there, and the writer always sends 0xFF.
    // AC-3 only - E-AC-3's infomdat has no language field.
    bool langcod = false;
    bool langcod2 = false;  // 1+1 only
    // §5.4.2.13-15. std::nullopt clears audprodie.
    std::optional<AudioProduction> audprod = std::nullopt;
    std::optional<AudioProduction> audprod2 = std::nullopt;  // 1+1 only
    // §5.4.2.24/25. origbs defaults true because an encoder producing a
    // stream IS the original of it.
    bool copyrightb = false;
    bool origbs = true;
    // §5.4.2.26-28, AC-3 bsid 8 only - see TimeCodeCoarse above.
    std::optional<TimeCodeCoarse> timecod1 = std::nullopt;
    std::optional<TimeCodeFine> timecod2 = std::nullopt;
    // §E2.3.1.63: the source material was sampled at twice the rate fscod
    // indicates. E-AC-3 only, and only when fscod is not 0x3 (a reduced-rate
    // fscod2 frame carries no such bit).
    bool sourcefscod = false;
};

// Annex D's xbsi2 group (§D2.3.1.7-12), the half of the alternate syntax that
// is not downmix levels. AC-3 bsid 6 only.
struct ExtendedBsi {
    SurroundExMode dsurexmod = SurroundExMode::kNotIndicated;
    HeadphoneMode dheadphonmod = HeadphoneMode::kNotIndicated;
    AdConverterType adconvtyp = AdConverterType::kStandard;
    // §D2.3.1.11: "reserved for future assignment. Encoders shall set these
    // bits to all 0's." Reported on decode so a third-party stream's value is
    // visible; the encoder always writes the zero the spec requires and never
    // reads this field.
    std::uint8_t xbsi2 = 0;
    // §D2.3.1.12: "reserved for use by the encoder, and is not used by the
    // decoder" - the one bit in this header an encoder may spend on itself.
    bool encinfo = false;
};

// Annex D's whole contribution to bsi: the two optional 14-bit groups that a
// bsid-6 stream writes where a bsid-8 one writes timecod1 and timecod2. Set on
// an AC-3 encoder config it selects bsid 6; reported by the decoder it means
// bsid 6 was read.
//
// `mix` is the xbsi1 group (§D2.3.1.1-6) - dmixmod plus separate Lt/Rt and
// Lo/Ro centre and surround levels, which is why it reuses MixMetadata rather
// than restating those five fields. Annex D has no LFE mix level and none of
// mixmdate's programme-mixing depth, so everything in MixMetadata past the
// five levels is unread here.
struct AlternateBsi {
    std::optional<MixMetadata> mix = std::nullopt;       // xbsi1e
    std::optional<ExtendedBsi> extended = std::nullopt;  // xbsi2e
};

// Every value fits the bits §5.4.2 / Table D2.1 gives its field. An encoder
// checks this before writing: a mixing level of 40 needs six bits where the
// syntax has five, so writing it would not merely record the wrong level, it
// would push every following field one bit along and the frame would decode as
// something else entirely.
[[nodiscard]] AC3FORGE_EXPORT bool valid_bsi_info(const BsiInfo& value);
[[nodiscard]] AC3FORGE_EXPORT bool valid_alternate_bsi(const AlternateBsi& value);

// Names for a front end to show, in the same order as each enum's values.
[[nodiscard]] AC3FORGE_EXPORT std::string_view describe(BitstreamMode value, Acmod acmod);
[[nodiscard]] AC3FORGE_EXPORT std::string_view describe(SurroundMode value);
[[nodiscard]] AC3FORGE_EXPORT std::string_view describe(SurroundExMode value);
[[nodiscard]] AC3FORGE_EXPORT std::string_view describe(HeadphoneMode value);
[[nodiscard]] AC3FORGE_EXPORT std::string_view describe(AdConverterType value);
[[nodiscard]] AC3FORGE_EXPORT std::string_view describe(RoomType value);

// The CLI token vocabularies, shared with the help text so the two cannot
// drift. Each parse function returns false on anything unrecognised and
// leaves `out` untouched.
inline constexpr std::string_view kBsmodNames =
    "cm | me | vi | hi | dialogue | commentary | emergency | voiceover (or 0..7)";
inline constexpr std::string_view kSurroundModeNames = "none | off | on";
inline constexpr std::string_view kSurroundExModeNames = "none | off | ex | pliiz";
inline constexpr std::string_view kHeadphoneModeNames = "none | off | on";
inline constexpr std::string_view kAdConverterNames = "standard | hdcd";
inline constexpr std::string_view kRoomTypeNames = "none | large | small";

[[nodiscard]] AC3FORGE_EXPORT bool parse_bsmod(std::string_view text, BitstreamMode& out);
[[nodiscard]] AC3FORGE_EXPORT bool parse_surround_mode(std::string_view text, SurroundMode& out);
[[nodiscard]] AC3FORGE_EXPORT bool parse_surround_ex_mode(std::string_view text,
                                                          SurroundExMode& out);
[[nodiscard]] AC3FORGE_EXPORT bool parse_headphone_mode(std::string_view text, HeadphoneMode& out);
[[nodiscard]] AC3FORGE_EXPORT bool parse_ad_converter(std::string_view text, AdConverterType& out);
[[nodiscard]] AC3FORGE_EXPORT bool parse_room_type(std::string_view text, RoomType& out);

// "HH:MM:SS", "HH:MM:SS:FF" or "HH:MM:SS:FF.N" (N being 1/64ths of a frame),
// split across the two halves §5.4.2.26 defines: the coarse half takes the
// hours, minutes and whole 8-second buckets, the fine half the remaining
// 0-7 seconds plus the frame and its fraction. The short form still fills
// both halves - a time code that stopped at 8-second resolution would be a
// strange thing to ask for by writing out the seconds.
inline constexpr std::string_view kTimeCodeSyntax = "HH:MM:SS[:FF[.N]] (FF 0..29, N 1/64 frame)";

[[nodiscard]] AC3FORGE_EXPORT bool parse_timecode(std::string_view text, TimeCodeCoarse& coarse,
                                                  TimeCodeFine& fine);

// The inverse, in the same "HH:MM:SS:FF.N" vocabulary parse_timecode takes.
[[nodiscard]] AC3FORGE_EXPORT std::string format_timecode(const TimeCodeCoarse& coarse,
                                                          const TimeCodeFine& fine);

}  // namespace ac3::meta
