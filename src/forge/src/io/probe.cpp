#include "ac3/io/probe.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <ios>
#include <istream>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "ac3/core/crc16.hpp"
#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/decoder/syntax_trace.hpp"
#include "ac3/emdf/emdf.hpp"
#include "ac3/io/elementary.hpp"
#include "ac3/meta/drc.hpp"
#include "ac3/oba/oamd.hpp"

namespace ac3::io {

namespace {

[[nodiscard]] bool sync_at(std::span<const std::byte> at) {
    return at.size() >= 2 && std::to_integer<std::uint8_t>(at[0]) == 0x0B &&
           std::to_integer<std::uint8_t>(at[1]) == 0x77;
}

// §5.4.1.2/§E2.3.4. AC-3 protects its first 5/8 twice over - crc1 covers the
// checkpoint, crc2 the whole frame - and E-AC-3 has no crc1 at all, so crc2 is
// its only error check. Either way the register reads zero over the region
// past the sync word, its own bytes included, which is why nothing here has to
// know where the checksum itself sits.
[[nodiscard]] bool crc_ok(std::span<const std::byte> frame, const FrameHeader& header) {
    if (frame.size() < 6) {
        return false;
    }
    if (crc16(frame.subspan(2)) != 0) {
        return false;
    }
    if (header.kind != StreamKind::kAc3) {
        return true;
    }
    const auto words58 = frame_size_58_words(static_cast<std::uint32_t>(frame.size() / 2));
    return 2 * words58 >= 2 && 2 * words58 <= frame.size() &&
           crc16(frame.subspan(2, 2 * words58 - 2)) == 0;
}

// Samples one syncframe of this shape carries. A block is 256 samples and
// AC-3 always codes six of them; Annex E's numblkscod is the only thing that
// makes this a question at all.
[[nodiscard]] int frame_samples(const FrameHeader& header) {
    const int blocks = header.kind == StreamKind::kAc3
                           ? kBlocksPerFrame
                           : eac3::blocks_per_syncframe(header.numblkscod);
    return blocks * kSamplesPerBlock;
}

}  // namespace

// ---------------------------------------------------------------------------
// Prober
// ---------------------------------------------------------------------------

struct Prober::Impl {
    ProbeOptions options;
    ProbeReport report;
    // One decoder of each generation, kept across the whole walk: both carry
    // per-substream overlap-add and object state, and re-creating them per
    // frame would throw away exactly the continuity a stream depends on.
    // Neither reconstructs - see DecoderConfig::skip_reconstruction.
    FrameDecoder ac3;
    Eac3Decoder eac3;
    FrameSyntax syntax;
    ProbeAccessUnit unit;
    std::uint64_t samples = 0;
    bool first_unit = true;
    // The stream-wide identity fields are taken from the FIRST syncframe of
    // the first access unit, which is its independent substream. A dedicated
    // flag, not "no syncframes recorded yet": the per-frame record is only
    // kept when detail was asked for, so without it every frame of the first
    // unit would have looked like the first one - and the last dependent's
    // acmod would have overwritten the bed's.
    bool first_frame = true;
    // Locations the program occupies once every dependent of the FIRST access
    // unit has been unioned in (§E3.8.2). Later units are assumed to match,
    // exactly as scan() assumes it.
    std::uint16_t locations = 0;

    explicit Impl(ProbeOptions opts) : options(std::move(opts)) {
        DecoderConfig config;
        config.skip_reconstruction = true;
        config.syntax = &syntax;
        ac3 = FrameDecoder{config};
        eac3 = Eac3Decoder{config};
    }

    // One substream identity's row, created on first sight.
    ProbeSubstream& slot_for(const FrameHeader& header) {
        for (auto& sub : report.substreams) {
            if (sub.strmtyp == header.strmtyp && sub.substreamid == header.substreamid) {
                return sub;
            }
        }
        report.substreams.push_back(ProbeSubstream{.strmtyp = header.strmtyp,
                                                   .substreamid = header.substreamid,
                                                   .bsid = header.bsid,
                                                   .bsmod = header.bsmod,
                                                   .acmod = header.acmod,
                                                   .lfe = header.lfe,
                                                   .numblkscod = header.numblkscod,
                                                   .chanmap = header.chanmap});
        return report.substreams.back();
    }

    void note_payload_id(int id) {
        auto& ids = report.emdf_payload_ids;
        const auto at = std::ranges::lower_bound(ids, id);
        if (at == ids.end() || *at != id) {
            ids.insert(at, id);
        }
        report.oamd = report.oamd || id == emdf::kPayloadIdOamd;
        report.joc = report.joc || id == emdf::kPayloadIdJoc;
    }

    void accumulate_syntax(const FrameSyntax& syn) {
        if (!syn.valid) {
            return;
        }
        auto& tools = report.tools;
        bool aht = false;
        for (int stream = 0; stream < kMaxSyntaxStreams; ++stream) {
            aht = aht || syn.aht_stream[static_cast<std::size_t>(stream)];
        }
        tools.aht_frames += aht ? 1 : 0;
        tools.transient_prenoise_frames += syn.transient_prenoise ? 1 : 0;
        for (int index = 0; index < syn.emdf_payload_count; ++index) {
            note_payload_id(syn.emdf_payload_ids[static_cast<std::size_t>(index)]);
        }
        for (int blk = 0; blk < syn.block_count; ++blk) {
            const auto& block = syn.blocks[static_cast<std::size_t>(blk)];
            if (!block.entered) {
                continue;
            }
            ++tools.blocks;
            tools.block_switch += block.block_switch != 0 ? 1 : 0;
            tools.dither += block.dither != 0 ? 1 : 0;
            tools.coupling += block.coupling ? 1 : 0;
            tools.enhanced_coupling += block.enhanced_coupling ? 1 : 0;
            tools.spectral_extension += block.spectral_extension ? 1 : 0;
            tools.rematrixing += block.rematrixing ? 1 : 0;
            tools.delta_bit_alloc += block.delta_bit_alloc ? 1 : 0;
            tools.skip_field += block.skip_field ? 1 : 0;
            // The coded streams only: an uncoded slot reports kReuse, which
            // would otherwise inflate the reuse count with channels that are
            // not there.
            const int coded = syn.fbw_channels + (syn.lfe ? 1 : 0);
            for (int stream = 0; stream < coded; ++stream) {
                const auto strategy =
                    block.exp_strategy[static_cast<std::size_t>(stream)];
                ++tools.exp_strategy[static_cast<std::size_t>(strategy)];
            }
            if (block.coupling) {
                ++tools.exp_strategy[static_cast<std::size_t>(
                    block.exp_strategy[kCouplingSyntaxStream])];
            }
        }
    }

    // §7.7.1.2's effective words, which the decoder has already resolved.
    // Unity is what a block with no word inherits, so a stream that never
    // transmits one leaves the range unseen rather than pinned at unity.
    void accumulate_dynrng(std::span<const std::uint8_t> words, int blocks, MinMax& into) {
        for (int blk = 0; blk < blocks && blk < static_cast<int>(words.size()); ++blk) {
            if (words[static_cast<std::size_t>(blk)] != meta::kDynrngUnity) {
                into.add(words[static_cast<std::size_t>(blk)]);
            }
        }
    }

    void accumulate_header(const FrameHeader& header) {
        ++report.syncframes;
        auto& sub = slot_for(header);
        ++sub.syncframes;
        if (header.chanmap && !sub.chanmap) {
            sub.chanmap = header.chanmap;
        }
        // A dependent's own dialnorm is part of the same program's metadata
        // and is reported with it; its compr bit means something else
        // entirely (§E3.8.5) and read_frame_header already declines to
        // report that as a word.
        report.dialnorm.add(header.dialnorm);
        if (header.dialnorm2) {
            report.dialnorm2.add(*header.dialnorm2);
        }
        if (header.compr) {
            report.compr.add(*header.compr);
        }
        if (header.compr2) {
            report.compr2.add(*header.compr2);
        }
        if (header.oba_complexity_index && !report.oba_complexity_index) {
            report.oba_complexity_index = header.oba_complexity_index;
        }
        if (first_unit && header.strmtyp == eac3::StreamType::kDependent && header.chanmap) {
            locations = static_cast<std::uint16_t>(locations | *header.chanmap);
        }
    }
};

Prober::Prober(ProbeOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}
Prober::~Prober() = default;
Prober::Prober(Prober&&) noexcept = default;
Prober& Prober::operator=(Prober&&) noexcept = default;

std::expected<void, ScanError> Prober::push(std::span<const std::byte> unit) {
    auto& impl = *impl_;
    if (unit.empty()) {
        return std::unexpected(ScanError::kEmpty);
    }
    auto& out = impl.unit;
    out.index = impl.report.access_units;
    out.byte_offset = impl.report.bytes;
    out.bytes = unit.size();
    out.start_seconds = 0.0;
    out.syncframes.clear();

    // The unit's own framing first, syncframe by syncframe. Nothing in this
    // loop consults the decoder: a stream whose audio is unreadable still has
    // a shape, and reporting that shape is most of what a probe is for.
    std::size_t offset = 0;
    int unit_samples = 0;
    while (offset < unit.size()) {
        if (!sync_at(unit.subspan(offset))) {
            return std::unexpected(ScanError::kLostSync);
        }
        const auto header = read_frame_header(unit.subspan(offset));
        if (!header) {
            return std::unexpected(header.error());
        }
        if (offset + header->bytes > unit.size()) {
            return std::unexpected(ScanError::kTruncated);
        }
        const auto frame = unit.subspan(offset, header->bytes);

        if (impl.first_frame) {
            impl.first_frame = false;
            impl.report.kind = header->kind;
            impl.report.bsid = header->bsid;
            impl.report.bsmod = header->bsmod;
            impl.report.sample_rate = header->sample_rate;
            impl.report.reduced_rate = header->reduced_rate;
            impl.report.acmod = header->acmod;
            impl.report.lfe = header->lfe;
            impl.report.numblkscod = header->numblkscod;
            impl.report.coded_channels = header->coded_channels();
            if (header->kind == StreamKind::kAc3) {
                impl.report.nominal_bitrate_kbps = header->bitrate_kbps;
            }
            impl.locations = eac3::chanmap::acmod_map(header->acmod, header->lfe);
        }
        impl.accumulate_header(*header);

        ProbeSyncframe reported{};
        reported.byte_offset = out.byte_offset + offset;
        reported.header = *header;
        reported.crc_valid = crc_ok(frame, *header);
        if (!reported.crc_valid) {
            ++impl.report.crc_failures;
        }
        if (impl.options.authenticity) {
            reported.authenticity_tag = impl.options.authenticity(frame);
            impl.report.authenticity_tagged_frames += reported.authenticity_tag ? 1 : 0;
        }

        // The parse tier. A refusal is recorded against the frame and the
        // walk continues: the decoders are strict by design (they refuse
        // syntax they decline to guess at, not just syntax that is wrong),
        // and a probe that stopped there would be useless on the streams it
        // most needs to describe.
        impl.syntax.reset();
        if (header->kind == StreamKind::kAc3) {
            const auto decoded = impl.ac3.decode_frame(frame);
            if (decoded) {
                impl.accumulate_dynrng(decoded->dynrng, kBlocksPerFrame, impl.report.dynrng);
                impl.accumulate_dynrng(decoded->dynrng2, kBlocksPerFrame, impl.report.dynrng2);
                std::ranges::copy(decoded->dynrng, reported.dynrng.begin());
            } else {
                reported.parse_error = decoded.error();
            }
        } else {
            const auto decoded = impl.eac3.decode_substream(frame);
            if (decoded && decoded->has_value()) {
                const auto& sub = **decoded;
                const int blocks = eac3::blocks_per_syncframe(sub.numblkscod);
                impl.accumulate_dynrng(sub.dynrng, blocks, impl.report.dynrng);
                impl.accumulate_dynrng(sub.dynrng2, blocks, impl.report.dynrng2);
                std::ranges::copy(sub.dynrng, reported.dynrng.begin());
                if (sub.object_metadata) {
                    ++impl.report.object_frames;
                    if (!impl.report.program) {
                        impl.report.program = sub.object_metadata->program;
                    }
                    reported.objects = sub.object_metadata;
                }
            } else if (!decoded) {
                reported.parse_error = decoded.error();
            }
            // decoded && !decoded->has_value() is the transient pre-noise
            // holdback, which skip_reconstruction bypasses outright - so it
            // cannot happen here, and a frame that somehow produced it is
            // simply one with no parse-tier answers rather than a failure.
        }
        if (reported.parse_error) {
            ++impl.report.parse_failures;
            if (!impl.report.first_parse_error) {
                impl.report.first_parse_error = reported.parse_error;
            }
        }
        impl.accumulate_syntax(impl.syntax);
        if (impl.options.detail) {
            reported.syntax = impl.syntax;
        }

        // An access unit's duration is its INDEPENDENT substream's: every
        // dependent codes the same samples of the same program, so adding
        // theirs would count the unit two or three times over.
        if (header->kind == StreamKind::kAc3 ||
            header->strmtyp != eac3::StreamType::kDependent) {
            unit_samples = frame_samples(*header);
        }
        if (impl.options.detail || impl.options.on_access_unit) {
            out.syncframes.push_back(std::move(reported));
        }
        offset += header->bytes;
    }

    out.start_seconds = static_cast<double>(impl.samples) /
                        static_cast<double>(sample_rate_hz(impl.report.sample_rate));
    if (impl.options.on_access_unit) {
        impl.options.on_access_unit(out);
    }

    ++impl.report.access_units;
    impl.report.bytes += unit.size();
    impl.samples += static_cast<std::uint64_t>(unit_samples);
    if (impl.first_unit) {
        impl.report.substreams_per_unit = impl.report.substreams.size();
        impl.report.min_access_unit_bytes = unit.size();
        impl.report.max_access_unit_bytes = unit.size();
        impl.first_unit = false;
    } else {
        impl.report.min_access_unit_bytes =
            std::min(impl.report.min_access_unit_bytes, unit.size());
        impl.report.max_access_unit_bytes =
            std::max(impl.report.max_access_unit_bytes, unit.size());
    }
    return {};
}

ProbeReport Prober::report() const {
    ProbeReport out = impl_->report;
    out.rendered_channels = eac3::chanmap::channel_count(impl_->locations);
    // Dual mono is the one layout there is no layout for: 1+1's two channels
    // are unrelated programmes rather than directions, so `layout` is left
    // empty and `rendered_channels` stands on its own - the same stance
    // DecodedAccessUnit takes.
    if (out.acmod != Acmod::kDualMono) {
        out.layout = eac3::chanmap::expand(impl_->locations);
    } else {
        out.rendered_channels = 2;
    }
    const double rate = static_cast<double>(sample_rate_hz(out.sample_rate));
    out.duration_seconds = static_cast<double>(impl_->samples) / rate;
    if (out.duration_seconds > 0.0) {
        out.bitrate_kbps = static_cast<double>(out.bytes) * 8.0 / out.duration_seconds / 1000.0;
    }
    out.variable_bitrate = out.min_access_unit_bytes != out.max_access_unit_bytes;
    return out;
}

std::expected<ProbeReport, ScanError> probe(std::span<const std::byte> stream,
                                            const ProbeOptions& options) {
    if (stream.size() < 6) {
        return std::unexpected(ScanError::kEmpty);
    }
    // The unit boundaries themselves come from the same walk scan() uses, so
    // a probe and a mux can never disagree about where a packet begins.
    const auto scanned = scan(stream);
    if (!scanned) {
        return std::unexpected(scanned.error());
    }
    Prober prober{options};
    for (const auto& unit : scanned->access_units) {
        if (const auto pushed = prober.push(unit); !pushed) {
            return std::unexpected(pushed.error());
        }
    }
    return prober.report();
}

// ---------------------------------------------------------------------------
// AccessUnitReader
// ---------------------------------------------------------------------------

struct AccessUnitReader::Impl {
    // 64 KiB covers a couple of dozen access units at any real rate, so the
    // read syscall count stays low without the window itself ever being
    // interesting next to the process.
    static constexpr std::size_t kChunk = 64 * 1024;
    // The longest syncframe either format can declare: E-AC-3's frmsiz is 11
    // bits of words, so (2047 + 1) * 2.
    static constexpr std::size_t kMaxFrameBytes = 4096;

    std::istream& in;
    std::vector<std::byte> window;
    std::vector<std::byte> unit;
    std::size_t pos = 0;
    std::uint64_t base = 0;  // stream offset of window[0]
    std::uint64_t unit_offset = 0;
    bool exhausted = false;

    explicit Impl(std::istream& stream) : in(stream) {}

    // Guarantees `want` bytes are readable at `pos`, or reports how many
    // actually are. Compacts first so the window never grows with the file.
    std::size_t ensure(std::size_t want) {
        if (window.size() - pos >= want) {
            return window.size() - pos;
        }
        if (pos > 0) {
            window.erase(window.begin(), window.begin() + static_cast<std::ptrdiff_t>(pos));
            base += pos;
            pos = 0;
        }
        while (window.size() < want && !exhausted) {
            const std::size_t previous = window.size();
            window.resize(previous + kChunk);
            in.read(reinterpret_cast<char*>(window.data() + previous),  // NOLINT
                    static_cast<std::streamsize>(kChunk));
            const auto got = static_cast<std::size_t>(in.gcount());
            window.resize(previous + got);
            if (got < kChunk) {
                exhausted = true;
            }
        }
        return window.size() - pos;
    }
};

AccessUnitReader::AccessUnitReader(std::istream& in) : impl_(std::make_unique<Impl>(in)) {}
AccessUnitReader::~AccessUnitReader() = default;

std::uint64_t AccessUnitReader::byte_offset() const { return impl_->unit_offset; }

std::expected<std::span<const std::byte>, ScanError> AccessUnitReader::next() {
    auto& impl = *impl_;
    impl.unit.clear();
    impl.unit_offset = impl.base + impl.pos;

    while (true) {
        // Enough for any header this could be; a short tail is either the end
        // of the stream or a truncated frame, and the header parse below says
        // which.
        const std::size_t available = impl.ensure(Impl::kMaxFrameBytes);
        if (available == 0) {
            break;
        }
        const auto at = std::span<const std::byte>{impl.window}.subspan(impl.pos, available);
        if (!sync_at(at)) {
            return std::unexpected(ScanError::kLostSync);
        }
        const auto header = read_frame_header(at);
        if (!header) {
            return std::unexpected(header.error());
        }
        if (header->bytes > available) {
            return std::unexpected(ScanError::kTruncated);
        }
        // An independent substream (or any AC-3 frame) begins a new access
        // unit; a dependent joins the one in progress. So a unit is closed by
        // discovering the NEXT one's first frame, which is why this peeks
        // before consuming.
        const bool starts_unit = header->kind == StreamKind::kAc3 ||
                                 header->strmtyp != eac3::StreamType::kDependent;
        if (starts_unit && !impl.unit.empty()) {
            break;
        }
        if (impl.unit.empty()) {
            impl.unit_offset = impl.base + impl.pos;
        }
        impl.unit.insert(impl.unit.end(), at.begin(),
                         at.begin() + static_cast<std::ptrdiff_t>(header->bytes));
        impl.pos += header->bytes;
    }
    return std::span<const std::byte>{impl.unit};
}

}  // namespace ac3::io
