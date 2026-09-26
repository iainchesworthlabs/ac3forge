// The encoder's companding and A-SPX writer (src/ac4enc/src/aspx/) read back
// by the decoder's reader: aspx_config(), every interval class, one and two
// channels, balance, sinusoids, both kinds of interleaved waveform coding and
// companding's three forms, each element's trace the writer's record for
// record. The encoder's own A-SPX configurations, and what its QMF front end
// makes of a signal, are held here too.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <numbers>
#include <optional>
#include <string_view>
#include <utility>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "ac4/syntax.hpp"
#include "aspx/aspx_encoder.hpp"
#include "aspx/aspx_syntax.hpp"
#include "bit_reader.hpp"
#include "bit_writer.hpp"
#include "syntax/aspx.hpp"
#include "syntax/channel_elements.hpp"
#include "syntax/context.hpp"
#include "tables/huffman_codes.hpp"

namespace {

using ac4::SyntaxRecord;
using ac4::detail::AspxChannelFields;
using ac4::detail::AspxConfigFields;
using ac4::detail::AspxCounts;
using ac4::detail::AspxEnvelopeFields;
using ac4::detail::AspxIntervalClass;
using ac4::detail::BitReader;
using ac4::detail::BitWriter;

// A trace kept here. ac4::SyntaxSink refers to its callable without owning
// it, so the callable is a member, alive as long as the recording.
struct Recording {
    std::vector<SyntaxRecord> records;
    std::function<void(const SyntaxRecord&)> push = [this](const SyntaxRecord& r) { records.push_back(r); };
    Recording() = default;
    Recording(const Recording&) = delete;
    Recording& operator=(const Recording&) = delete;
    [[nodiscard]] ac4::SyntaxSink sink() { return ac4::SyntaxSink(push); }
};

void require_same(std::span<const SyntaxRecord> written, std::span<const SyntaxRecord> read) {
    REQUIRE(written.size() == read.size());
    for (std::size_t i = 0; i < written.size(); ++i) {
        CAPTURE(i, written[i].name, read[i].name);
        CHECK(written[i].name == read[i].name);
        CHECK(written[i].bit_offset == read[i].bit_offset);
        CHECK(written[i].bits == read[i].bits);
        CHECK(written[i].value == read[i].value);
    }
}

// A small deterministic generator, for envelope values and flags.
struct Lcg {
    std::uint32_t state = 12345;
    int below(int n) {
        state = state * 1664525U + 1013904223U;
        return static_cast<int>((state >> 8) % static_cast<std::uint32_t>(n));
    }
};

// Values of `count` groups that the envelope's codebooks hold: the first along
// frequency from F0, the rest a delta each way.
AspxEnvelopeFields envelope(Lcg& rng, std::size_t count, int delta_dir, bool signal, int quant_mode,
                            bool balance) {
    AspxEnvelopeFields e;
    e.delta_dir = delta_dir;
    for (int attempt = 0; attempt < 100; ++attempt) {
        e.values.assign(count, 0);
        for (std::size_t i = 0; i < count; ++i) {
            e.values[i] = i == 0 && delta_dir == 0 ? rng.below(12) : rng.below(9) - 4;
        }
        if (ac4::detail::aspx_envelope_codable(signal, quant_mode, balance, e)) {
            return e;
        }
    }
    FAIL("no codable envelope");
    return e;
}

AspxCounts counts_for(const AspxConfigFields& config, int xover) {
    ac4::detail::aspx::SubbandGroups groups;
    const ac4::detail::aspx::FrequencyConfig frequency{.master_freq_scale = config.master_freq_scale,
                                                       .start_freq = config.start_freq,
                                                       .stop_freq = config.stop_freq,
                                                       .noise_sbg = config.noise_sbg,
                                                       .xover_subband_offset = xover};
    REQUIRE(ac4::detail::aspx::derive_subband_groups(frequency, groups) ==
            ac4::detail::aspx::GroupsError::kNone);
    return AspxCounts{.num_sbg_sig_highres = groups.num_sbg_sig_highres,
                      .num_sbg_sig_lowres = groups.num_sbg_sig_lowres,
                      .num_sbg_noise = groups.num_sbg_noise,
                      .num_aspx_timeslots = ac4::detail::kAspxTimeslots};
}

// One channel's fields for a framing, with values its codebooks hold. The
// envelopes' frequency resolution is the one the syntax derives: sent under
// aspx_freq_res_mode 0, low under 1, high under 3, and under 2 the encoder's
// reading of Pseudocode 77, which the parser is held to below.
AspxChannelFields channel(Lcg& rng, const AspxConfigFields& config, const AspxCounts& counts,
                          AspxChannelFields base, bool balance) {
    AspxChannelFields c = std::move(base);
    const int num_env = c.framing.num_env();
    const bool one_fixfix = c.framing.int_class == AspxIntervalClass::kFixFix && num_env == 1;
    if (c.qmode_env < 0) {
        c.qmode_env = one_fixfix ? 0 : config.quant_mode_env;
    }
    // In an I-frame a variable start is var_bord_left.
    const std::vector<int> borders = ac4::detail::interval_borders(c.framing, c.framing.var_bord_left);
    c.envelope_freq_res.clear();
    for (int env = 0; env < num_env; ++env) {
        int high = 1;
        switch (config.freq_res_mode) {
            case 0:
                high = c.framing.freq_res.at(c.framing.int_class == AspxIntervalClass::kFixFix
                                                 ? 0
                                                 : static_cast<std::size_t>(env));
                break;
            case 1:
                high = 0;
                break;
            case 2:
                high = ac4::detail::envelope_high_res(borders, env, c.framing.tsg_ptr) ? 1 : 0;
                break;
            default:
                high = 1;
                break;
        }
        c.envelope_freq_res.push_back(high);
    }
    if (c.tna_mode.empty()) {
        for (int g = 0; g < counts.num_sbg_noise; ++g) {
            c.tna_mode.push_back(rng.below(4));
        }
    }
    c.sig.clear();
    for (int env = 0; env < num_env; ++env) {
        const auto groups = static_cast<std::size_t>(c.envelope_freq_res[static_cast<std::size_t>(env)] != 0
                                                         ? counts.num_sbg_sig_highres
                                                         : counts.num_sbg_sig_lowres);
        // Along time only where the envelope before has this one's resolution.
        const bool same = env > 0 && c.envelope_freq_res[static_cast<std::size_t>(env)] ==
                                         c.envelope_freq_res[static_cast<std::size_t>(env - 1)];
        c.sig.push_back(envelope(rng, groups, same ? rng.below(2) : 0, true, c.qmode_env, balance));
    }
    c.noise.clear();
    for (int env = 0; env < c.framing.num_noise(); ++env) {
        c.noise.push_back(envelope(rng, static_cast<std::size_t>(counts.num_sbg_noise), env > 0 ? rng.below(2) : 0,
                                   false, 0, balance));
    }
    return c;
}

AspxChannelFields framing(AspxIntervalClass int_class) {
    AspxChannelFields c;
    c.qmode_env = -1;
    c.framing.int_class = int_class;
    return c;
}

// A config and element read back: aspx_config() and aspx_data_1ch() or
// aspx_data_2ch() written in turn, then read by parse_aspx_config() and the
// data parser, which must use every bit and record what was written.
void round_trip(const AspxConfigFields& config, bool iframe, int xover, bool balance,
                const std::vector<AspxChannelFields>& channels) {
    Recording written;
    BitWriter w(0, written.sink());
    const AspxCounts counts = counts_for(config, xover);
    ac4::detail::write_aspx_config(w, config);
    if (channels.size() == 1) {
        ac4::detail::write_aspx_data_1ch(w, iframe, xover, config, counts, channels[0]);
    } else {
        ac4::detail::write_aspx_data_2ch(w, iframe, xover, config, counts, balance, {channels[0], channels[1]});
    }
    Recording read;
    BitReader r(w.bytes(), 0, read.sink());
    ac4::detail::AspxConfig parsed{};
    REQUIRE(ac4::detail::parse_aspx_config(r, parsed));
    CHECK(parsed.quant_mode_env == config.quant_mode_env);
    CHECK(parsed.start_freq == config.start_freq);
    CHECK(parsed.stop_freq == config.stop_freq);
    CHECK(parsed.master_freq_scale == config.master_freq_scale);
    CHECK(parsed.noise_sbg == config.noise_sbg);
    CHECK(parsed.freq_res_mode == config.freq_res_mode);
    ac4::detail::SubstreamContext ctx;
    ctx.b_iframe = iframe;
    ctx.ch_mode = channels.size() == 1 ? ac4::detail::ch_mode::kMono : ac4::detail::ch_mode::kStereo;
    ac4::detail::AspxElementState state;
    state.have_xover_subband_offset = !iframe;
    state.xover_subband_offset = static_cast<std::uint8_t>(xover);
    if (channels.size() == 1) {
        ac4::detail::AspxData1ch data;
        const auto ok = ac4::detail::parse_aspx_data_1ch(r, ctx, parsed, state, data);
        INFO((ok ? std::string_view{} : ok.error().reason));
        REQUIRE(ok);
        CHECK(data.channel.framing.num_env == channels[0].framing.num_env());
    } else {
        ac4::detail::AspxData2ch data;
        const auto ok = ac4::detail::parse_aspx_data_2ch(r, ctx, parsed, state, data);
        INFO((ok ? std::string_view{} : ok.error().reason));
        REQUIRE(ok);
        CHECK(data.balance == balance);
    }
    CHECK_FALSE(r.overflow());
    CHECK(r.position() == w.bit_position());
    require_same(written.records, read.records);
}

AspxConfigFields dee_config(int start, int stop, int scale) {
    AspxConfigFields c;
    c.start_freq = start;
    c.stop_freq = stop;
    c.master_freq_scale = scale;
    return c;
}

}  // namespace

TEST_CASE("A-SPX FIXFIX intervals of one, two and four envelopes read back as written", "[ac4enc][aspx]") {
    Lcg rng;
    for (const AspxConfigFields& base : {dee_config(5, 0, 0), dee_config(4, 1, 1), dee_config(6, 1, 1)}) {
        for (const int freq_res_mode : {0, 1, 2, 3}) {
            for (const int quant_mode : {0, 1}) {
                for (const int tmp_num_env : {0, 1, 2}) {
                    for (const bool iframe : {true, false}) {
                        CAPTURE(base.start_freq, freq_res_mode, quant_mode, tmp_num_env, iframe);
                        AspxConfigFields config = base;
                        config.freq_res_mode = freq_res_mode;
                        config.quant_mode_env = quant_mode;
                        config.num_env_bits_fixfix = tmp_num_env > 1 ? 1 : rng.below(2);
                        const AspxCounts counts = counts_for(config, 0);
                        AspxChannelFields f = framing(AspxIntervalClass::kFixFix);
                        f.framing.tmp_num_env = tmp_num_env;
                        f.framing.freq_res = {rng.below(2)};
                        round_trip(config, iframe, 0, false, {channel(rng, config, counts, f, false)});
                    }
                }
            }
        }
    }
}

TEST_CASE("A-SPX variable borders, sinusoids and interleaving read back as written", "[ac4enc][aspx]") {
    Lcg rng;
    for (const int freq_res_mode : {0, 1, 3}) {
        for (const bool iframe : {true, false}) {
            CAPTURE(freq_res_mode, iframe);
            AspxConfigFields config = dee_config(4, 1, 1);
            config.freq_res_mode = freq_res_mode;
            const int xover = iframe ? 1 : 0;
            const AspxCounts counts = counts_for(config, xover);

            AspxChannelFields fixvar = framing(AspxIntervalClass::kFixVar);
            fixvar.framing.var_bord_right = 1;
            fixvar.framing.rel_bord_right = {2, 6};
            fixvar.framing.tsg_ptr = 2;
            fixvar.framing.freq_res = {1, 0, 1};
            AspxChannelFields varfix = framing(AspxIntervalClass::kVarFix);
            varfix.framing.var_bord_left = 1;
            varfix.framing.rel_bord_left = {4};
            varfix.framing.tsg_ptr = -1;
            varfix.framing.freq_res = {0, 1};
            AspxChannelFields varvar = framing(AspxIntervalClass::kVarVar);
            varvar.framing.var_bord_left = iframe ? 2 : 0;
            varvar.framing.rel_bord_left = {2};
            varvar.framing.var_bord_right = 3;
            varvar.framing.rel_bord_right = {4, 2};
            varvar.framing.tsg_ptr = 0;
            varvar.framing.freq_res = {1, 1, 0, 1};
            // Sinusoids in some groups, frequency interleaving in others and
            // time interleaving in some slots.
            for (AspxChannelFields* f : {&fixvar, &varvar}) {
                for (int g = 0; g < counts.num_sbg_sig_highres; ++g) {
                    f->add_harmonic.push_back(rng.below(3) == 0);
                    f->fic_used_in_sfb.push_back(rng.below(2) == 0);
                }
                for (int ts = 0; ts < counts.num_aspx_timeslots; ++ts) {
                    f->tic_used_in_slot.push_back(rng.below(4) == 0);
                }
            }
            for (const AspxChannelFields& f : {fixvar, varfix, varvar}) {
                round_trip(config, iframe, xover, false, {channel(rng, config, counts, f, false)});
            }
            // Pairs: each pair of framings unbalanced, and balanced ones, whose
            // second channel repeats the first's framing and inverse
            // filtering and sends balance values.
            const std::array<AspxChannelFields, 3> all{fixvar, varfix, varvar};
            for (const AspxChannelFields& left : all) {
                for (const AspxChannelFields& right : all) {
                    round_trip(config, iframe, xover, false,
                               {channel(rng, config, counts, left, false), channel(rng, config, counts, right, false)});
                }
                const AspxChannelFields first = channel(rng, config, counts, left, false);
                AspxChannelFields second = left;
                second.tna_mode = first.tna_mode;
                second.add_harmonic.clear();
                second.tic_used_in_slot = first.tic_used_in_slot;  // aspx_tic_copy
                round_trip(config, iframe, xover, true, {first, channel(rng, config, counts, second, true)});
            }
        }
    }
}

TEST_CASE("the encoder's interval borders, resolutions and noise borders are the parser's", "[ac4enc][aspx]") {
    // Every framing the encoder chooses from, and more: written, parsed, and
    // held to the parser's Pseudocode 76 and 77 and Table 193.
    Lcg rng;
    AspxConfigFields config = dee_config(4, 1, 1);
    config.freq_res_mode = 2;
    const AspxCounts counts = counts_for(config, 0);
    std::vector<AspxChannelFields> framings;
    for (const int tmp : {0, 1}) {
        AspxChannelFields f = framing(AspxIntervalClass::kFixFix);
        f.framing.tmp_num_env = tmp;
        framings.push_back(f);
    }
    for (const int end : {0, 1, 2, 3}) {
        for (const std::vector<int>& right : {std::vector<int>{}, {4}, {2, 6}, {6, 6, 4}, {8, 8}}) {
            for (int ptr = -1; ptr <= static_cast<int>(right.size()); ++ptr) {
                AspxChannelFields f = framing(AspxIntervalClass::kFixVar);
                f.framing.var_bord_right = end;
                f.framing.rel_bord_right = right;
                f.framing.tsg_ptr = ptr;
                framings.push_back(f);
            }
        }
    }
    for (const int start : {0, 1, 2, 3}) {
        for (const std::vector<int>& left : {std::vector<int>{}, {4}, {2, 4}, {6, 2, 4}}) {
            for (int ptr = -1; ptr <= static_cast<int>(left.size()); ++ptr) {
                AspxChannelFields f = framing(AspxIntervalClass::kVarFix);
                f.framing.var_bord_left = start;
                f.framing.rel_bord_left = left;
                f.framing.tsg_ptr = ptr;
                framings.push_back(f);
                AspxChannelFields v = framing(AspxIntervalClass::kVarVar);
                v.framing.var_bord_left = start;
                v.framing.rel_bord_left = left;
                v.framing.var_bord_right = (start + 1) % 4;
                v.framing.rel_bord_right = left.size() < 2 ? std::vector<int>{2} : std::vector<int>{};
                v.framing.tsg_ptr = ptr;
                framings.push_back(v);
            }
        }
    }
    for (const AspxChannelFields& base : framings) {
        const AspxChannelFields f = channel(rng, config, counts, base, false);
        const int start = f.framing.int_class == AspxIntervalClass::kFixFix ||
                                  f.framing.int_class == AspxIntervalClass::kFixVar
                              ? 0
                              : f.framing.var_bord_left;
        const std::vector<int> borders = ac4::detail::interval_borders(f.framing, start);
        CAPTURE(static_cast<int>(f.framing.int_class), start, borders);
        BitWriter w;
        ac4::detail::write_aspx_data_1ch(w, true, 0, config, counts, f);
        BitReader r(w.bytes(), 0, {});
        ac4::detail::AspxConfig parsed_config{};
        parsed_config.valid = true;
        parsed_config.quant_mode_env = static_cast<std::uint8_t>(config.quant_mode_env);
        parsed_config.start_freq = static_cast<std::uint8_t>(config.start_freq);
        parsed_config.stop_freq = static_cast<std::uint8_t>(config.stop_freq);
        parsed_config.master_freq_scale = static_cast<std::uint8_t>(config.master_freq_scale);
        parsed_config.noise_sbg = static_cast<std::uint8_t>(config.noise_sbg);
        parsed_config.freq_res_mode = static_cast<std::uint8_t>(config.freq_res_mode);
        ac4::detail::SubstreamContext ctx;
        ctx.b_iframe = true;
        ctx.ch_mode = ac4::detail::ch_mode::kMono;
        ac4::detail::AspxElementState state;
        ac4::detail::AspxData1ch data;
        REQUIRE(ac4::detail::parse_aspx_data_1ch(r, ctx, parsed_config, state, data));
        const ac4::detail::AspxFraming& parsed = data.channel.framing;
        REQUIRE(static_cast<int>(parsed.num_env) + 1 == static_cast<int>(borders.size()));
        for (std::size_t i = 0; i < borders.size(); ++i) {
            CHECK(parsed.atsg_sig[i] == borders[i]);
        }
        for (int env = 0; env < parsed.num_env; ++env) {
            CHECK((parsed.atsg_freqres[static_cast<std::size_t>(env)] != 0) ==
                  ac4::detail::envelope_high_res(borders, env, f.framing.tsg_ptr));
        }
        const std::vector<int> noise = ac4::detail::noise_borders(f.framing, borders);
        REQUIRE(static_cast<int>(parsed.num_noise) + 1 == static_cast<int>(noise.size()));
        for (std::size_t i = 0; i < noise.size(); ++i) {
            CHECK(parsed.atsg_noise[i] == noise[i]);
        }
    }
}

TEST_CASE("the A-SPX data a frame falls back to cost more where its interval starts a slot in",
          "[ac4enc][aspx]") {
    // AspxChannelEncoder::fallback() takes one envelope from where the last
    // interval stopped: FIXFIX from the frame's start, VARFIX from a slot
    // in, as after a FIXVAR interval run on to an attack's parity, which in
    // an I-frame sends the border too. create() sizes the frame that holds
    // nothing more with both: at the lowest rates the second costs 2 bits a
    // channel more, and in stereo at 8 kbps that is past its 42-byte frame.
    const std::optional<ac4::detail::AspxSetup> setup = ac4::detail::aspx_setup_for(8.0, 48000);
    REQUIRE(setup.has_value());
    const ac4::detail::AspxChannelEncoder encoder(*setup);
    const AspxChannelFields fixed = encoder.fallback(true, true);
    const AspxChannelFields varied = encoder.fallback(true, true, 1);
    CHECK(fixed.framing.int_class == AspxIntervalClass::kFixFix);
    CHECK(varied.framing.int_class == AspxIntervalClass::kVarFix);
    CHECK(varied.framing.var_bord_left == 1);
    const auto bits = [&](const AspxChannelFields& f, int channels) {
        BitWriter w;
        if (channels == 1) {
            ac4::detail::write_aspx_data_1ch(w, true, 0, setup->config, setup->counts, f);
        } else {
            ac4::detail::write_aspx_data_2ch(w, true, 0, setup->config, setup->counts, false,
                                             {f, f});
        }
        return w.bit_position();
    };
    CHECK(bits(varied, 1) > bits(fixed, 1));
    CHECK(bits(varied, 2) > bits(fixed, 2));
}
TEST_CASE("companding_control() in its three forms reads back through the channel element", "[ac4enc][aspx]") {
    const std::optional<ac4::detail::AspxSetup> setup = ac4::detail::aspx_setup_for(32.0, 48000);
    REQUIRE(setup.has_value());
    Lcg rng;
    for (const int channels : {1, 2}) {
        for (const bool sync : {false, true}) {
            for (const int on : {0, 1, 2, 3}) {
                for (const bool average : {false, true}) {
                    if ((channels == 1 && (sync || on > 1)) || (sync && on > 1)) {
                        continue;
                    }
                    CAPTURE(channels, sync, on, average);
                    ac4::detail::AspxElement element;
                    element.companding.num_chan = channels;
                    element.companding.sync_flag = sync;
                    element.companding.compand_on = {(on & 1) != 0, (on & 2) != 0};
                    element.companding.compand_avg = average;
                    for (int c = 0; c < channels; ++c) {
                        AspxChannelFields f = framing(AspxIntervalClass::kFixFix);
                        element.channels.push_back(channel(rng, setup->config, setup->counts, f, false));
                    }
                    // A channel element with no bands around the A-SPX parts.
                    Recording written;
                    BitWriter w(0, written.sink());
                    w.write(channels == 2 ? 2U : 1U, 1, channels == 2 ? "stereo_codec_mode" : "mono_codec_mode");
                    ac4::detail::write_aspx_head(w, true, *setup, element);
                    if (channels == 2) {
                        w.write(1, 0, "b_enable_mdct_stereo_proc");
                    }
                    for (int track = 0; track < channels; ++track) {
                        const char* name = channels == 1 ? "spec_frontend" : (track == 0 ? "spec_frontend_l" : "spec_frontend_r");
                        w.write(1, 0, name);
                        w.write(1, 1, "b_long_frame");
                        w.write(6, 0, "max_sfb");
                    }
                    for (int track = 0; track < channels; ++track) {
                        w.write(8, 0, "reference_scale_factor");
                        w.write(1, 0, "b_snf_data_exists");
                    }
                    ac4::detail::write_aspx_tail(w, true, *setup, element);

                    Recording read;
                    BitReader r(w.bytes(), 0, read.sink());
                    ac4::detail::SubstreamContext ctx;
                    ctx.b_iframe = true;
                    ctx.ch_mode = channels == 2 ? ac4::detail::ch_mode::kStereo : ac4::detail::ch_mode::kMono;
                    ac4::detail::ChannelElementState state;
                    ac4::detail::ChannelElement parsed;
                    const auto ok = ac4::detail::parse_audio_data_chan(r, ctx, state, parsed);
                    INFO((ok ? std::string_view{} : ok.error().reason));
                    REQUIRE(ok);
                    REQUIRE(parsed.companding.has_value());
                    CHECK(parsed.companding->sync_flag == sync);
                    CHECK(parsed.companding->b_compand_on[0] == ((on & 1) != 0));
                    CHECK(r.position() == w.bit_position());
                    require_same(written.records, read.records);
                }
            }
        }
    }
}

TEST_CASE("the encoder's A-SPX configurations are DEE's", "[ac4enc][aspx]") {
    // Crossover subband, companding, by kbps a channel.
    struct Row {
        double kbps;
        int sbx;
        bool companding;
    };
    for (const Row row : {Row{24.0, 20, true}, Row{32.0, 28, true}, Row{48.0, 36, true}, Row{64.0, 36, false},
                          Row{72.0, 36, false}}) {
        CAPTURE(row.kbps);
        for (const int rate : {48000, 44100}) {
            const auto setup = ac4::detail::aspx_setup_for(row.kbps, rate);
            REQUIRE(setup.has_value());
            CHECK(setup->groups.sbx == row.sbx);
            CHECK(setup->companding == row.companding);
            CHECK(setup->base_48k == (rate == 48000));
            CHECK(setup->patches.num_sbg_patches > 0);
        }
    }
    CHECK_FALSE(ac4::detail::aspx_setup_for(48.0, 32000).has_value());

    // The 5.X and 7.X elements, as DEE's 5.1 streams have them: subband 32
    // from 192 kbps (38.4 a channel), and with aspx_xover_subband_offset 1
    // subband 34 from 256; below 192 the tables above; never companding.
    struct Multichannel {
        double kbps;
        int sbx;
        int xover;
    };
    for (const Multichannel row : {Multichannel{24.0, 20, 0}, Multichannel{36.0, 28, 0}, Multichannel{38.4, 32, 0},
                                   Multichannel{51.2, 34, 1}, Multichannel{76.0, 34, 1}}) {
        CAPTURE(row.kbps);
        for (const int rate : {48000, 44100}) {
            const auto setup = ac4::detail::aspx_setup_for(row.kbps, rate, true);
            REQUIRE(setup.has_value());
            CHECK(setup->groups.sbx == row.sbx);
            CHECK(setup->xover_subband_offset == row.xover);
            CHECK_FALSE(setup->companding);
        }
    }
}

TEST_CASE("the QMF front end's compressed low band is the signal again after the decoder's expansion",
          "[ac4enc][aspx]") {
    // A tone below the crossover whose level steps by 30 dB: compressed, the
    // steps shrink to alpha of their size in dB, and expanded as the decoder
    // expands they come back.
    const auto setup = ac4::detail::aspx_setup_for(24.0, 48000);
    REQUIRE(setup.has_value());
    ac4::detail::AspxChannelEncoder channel(*setup);
    const std::size_t slots = 400;
    std::vector<double> signal(slots * 64);
    for (std::size_t n = 0; n < signal.size(); ++n) {
        const double level = (n / 4096) % 2 == 0 ? 0.3 : 0.3 * std::pow(10.0, -30.0 / 20.0);
        signal[n] = level * std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(n) / 48000.0);
    }
    std::array<double, 64> chunk{};
    for (std::size_t g = 0; g < slots; ++g) {
        for (std::size_t i = 0; i < 64; ++i) {
            const long long s = static_cast<long long>(64 * g + i) - ac4::detail::kAnalysisLead;
            chunk[i] = s >= 0 && static_cast<std::size_t>(s) < signal.size() ? signal[static_cast<std::size_t>(s)] : 0.0;
        }
        channel.push_slot(chunk);
    }
    // The compressed signal's RMS over each 4 096-sample stretch, away from
    // the steps: its loud and quiet stretches differ by alpha times 30 dB.
    const auto rms = [&](std::size_t from, std::size_t to) {
        double sum = 0.0;
        for (std::size_t n = from; n < to; ++n) {
            const double c = channel.companded(static_cast<long long>(n));
            sum += c * c;
        }
        return std::sqrt(sum / static_cast<double>(to - from));
    };
    const double loud = rms(4096 * 2 + 1024, 4096 * 3 - 1024);
    const double quiet = rms(4096 * 3 + 1024, 4096 * 4 - 1024);
    CHECK(std::abs(20.0 * std::log10(loud / quiet) - 0.65 * 30.0) < 0.5);
}
