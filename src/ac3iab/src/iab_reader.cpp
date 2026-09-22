#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ac3iab/ac3iab.hpp"
#include "ac3iab/model.hpp"
#include "bitreader.hpp"

// The IAB element grammar - §9 (Bitstream IAFrame Specification) and §10 (IAFrame Data
// Fields) of SMPTE ST 2098-2:2022. Each parse_* function below implements exactly one syntax
// table from §9, in the same field order, and is annotated with the table/clause it
// transcribes. See bitreader.hpp's own header comment for why every element's payload can be
// parsed from bit position 0 of its own freshly-scoped BitReader.

namespace ac3iab {

namespace {

using detail::BitReader;

// §10.1.1 Table 14 - ElementID values.
namespace element_id {
inline constexpr std::uint32_t kBedDefinition = 0x10;
inline constexpr std::uint32_t kBedRemap = 0x20;
inline constexpr std::uint32_t kObjectDefinition = 0x40;
inline constexpr std::uint32_t kObjectZoneDefinition19 = 0x80;
inline constexpr std::uint32_t kAuthoringToolInfo = 0x100;
inline constexpr std::uint32_t kUserData = 0x101;
inline constexpr std::uint32_t kAudioDataDlc = 0x200;
inline constexpr std::uint32_t kAudioDataPcm = 0x400;
}  // namespace element_id

// Forward declarations - BedDefinition/BedRemap and ObjectDefinition/ObjectZoneDefinition19
// are mutually recursive per §9 Table 4 (a BedDefinition's Children can themselves be
// BedDefinition/BedRemap elements; likewise ObjectDefinition).
[[nodiscard]] std::expected<BedDefinition, IabError> parse_bed_definition(
    std::span<const std::byte> payload, std::uint8_t frame_rate_code);
[[nodiscard]] std::expected<BedRemap, IabError> parse_bed_remap(
    std::span<const std::byte> payload, std::uint8_t frame_rate_code);
[[nodiscard]] std::expected<ObjectDefinition, IabError> parse_object_definition(
    std::span<const std::byte> payload, std::uint8_t frame_rate_code);
[[nodiscard]] std::expected<ObjectZoneDefinition19, IabError> parse_object_zone19(
    std::span<const std::byte> payload, std::uint8_t frame_rate_code);

// §5.5: resolves a raw 10-bit gain code G to its linear value - shared by every *Gain[n]
// field (ChannelGain/ObjectGain/RemapGain/ZoneGain/ZoneGain19).
[[nodiscard]] double resolve_gain_code(std::uint32_t g) {
    if (g == 0x3ff) {
        return 0.0;  // "samples shall be multiplied by zero"
    }
    return std::pow(2.0, -static_cast<double>(g) / 64.0);
}

// Reads a 2-bit gain prefix plus, when the prefix is > 1, its following 10-bit code - the
// shared shape behind ChannelGainPrefix/ObjectGainPrefix/RemapGainPrefix (Table 20: 0x0 ->
// 1.0 unity, 0x1 -> 0.0 mute). Table 20 marks 0x3 "Reserved", but every syntax table gates the
// 10-bit code on `> 1` (not `== 2`), so 0x3 structurally carries the same code as 0x2 in the
// bitstream; resolved the same way here rather than left as an undefined bit-position bug.
[[nodiscard]] std::expected<double, IabError> read_unity_gain(BitReader& br) {
    auto prefix = br.read_bits(2);
    if (!prefix) {
        return std::unexpected(prefix.error());
    }
    if (*prefix == 0) {
        return 1.0;
    }
    if (*prefix == 1) {
        return 0.0;
    }
    auto code = br.read_bits(10);
    if (!code) {
        return std::unexpected(code.error());
    }
    return resolve_gain_code(static_cast<std::uint32_t>(*code));
}

// Same prefix shape as read_unity_gain() above, for ZoneGainPrefix/ZoneGain (§10.5.13-14,
// Table 25) and ZoneGain19 (§10.6.2-3): 0x0 -> 0.0 mute, 0x1 -> 1.0 unity - the reverse of
// Table 20's 0/1 mapping. Unlike ChannelGain/ObjectGain/RemapGain, the explicit 10-bit code
// here is NOT run through §5.5's log2 formula (resolve_gain_code() above) - §10.5.14/§10.6.3
// each give their own, different, linear formula: "gain = ZoneGain/(2^10-1)".
[[nodiscard]] std::expected<double, IabError> read_zone_gain(BitReader& br) {
    auto prefix = br.read_bits(2);
    if (!prefix) {
        return std::unexpected(prefix.error());
    }
    if (*prefix == 0) {
        return 0.0;
    }
    if (*prefix == 1) {
        return 1.0;
    }
    auto code = br.read_bits(10);
    if (!code) {
        return std::unexpected(code.error());
    }
    return static_cast<double>(*code) / 1023.0;  // §10.5.14/§10.6.3: ZoneGain/(2^10 - 1)
}

// Reads a 2-bit decorrelation prefix plus, when > 1, its following 8-bit code - the shared
// shape behind ChannelDecorCoefPrefix (Table 21) and ObjectDecorCoefPrefix (Table 27), which
// resolve identically: 0x0 -> 0.0 none, 0x1 -> 1.0 maximum, else code/255.
[[nodiscard]] std::expected<double, IabError> read_decor(BitReader& br) {
    auto prefix = br.read_bits(2);
    if (!prefix) {
        return std::unexpected(prefix.error());
    }
    if (*prefix == 0) {
        return 0.0;
    }
    if (*prefix == 1) {
        return 1.0;
    }
    auto code = br.read_bits(8);
    if (!code) {
        return std::unexpected(code.error());
    }
    return static_cast<double>(*code) / 255.0;
}

// §5.4 DistanceXY - ObjectPosX/Y (§10.5.7).
[[nodiscard]] double distance_xy(std::uint64_t dn, unsigned n) {
    const double half = static_cast<double>(std::uint64_t{1} << (n - 1));
    return static_cast<double>(dn) / half - (half - 1.0) / half;
}

// §5.4 DistanceZ - ObjectPosZ (§10.5.7) and, regardless of axis, ObjectSnapTolerance
// (§10.5.10) and ObjectSpread/ObjectSpreadX/Y/Z (§10.5.16-17).
[[nodiscard]] double distance_z(std::uint64_t dn, unsigned n) {
    const double max = static_cast<double>((std::uint64_t{1} << n) - 1);
    return static_cast<double>(dn) / max;
}

// AudioDescriptionText (§10.3.13/§10.5) and AuthoringToolURI (§10.9.1): a NUL-terminated
// strict-ASCII string, one 8-bit byte per character.
[[nodiscard]] std::expected<std::string, IabError> read_cstring(BitReader& br) {
    std::string result;
    while (true) {
        auto byte = br.read_bits(8);
        if (!byte) {
            return std::unexpected(byte.error());
        }
        if (*byte == 0) {
            return result;
        }
        result.push_back(static_cast<char>(*byte));
    }
}

// §10.3.12-13 / the identical field group in §10.5, Table 22: shared by BedDefinition and
// ObjectDefinition. Bit 0 (not_indicated) means "bits 1-6 shall be ignored" per Table 22's
// own note - this still decodes them into the struct (see model.hpp's AudioDescription
// comment) rather than dropping them, leaving the ignore-when-bit-0-is-set interpretation to
// the caller.
[[nodiscard]] std::expected<AudioDescription, IabError> parse_audio_description(BitReader& br) {
    auto raw = br.read_bits(8);
    if (!raw) {
        return std::unexpected(raw.error());
    }
    const auto byte = static_cast<unsigned>(*raw);

    AudioDescription desc;
    desc.not_indicated = (byte & 0x01) != 0;
    desc.dialog = (byte & 0x02) != 0;
    desc.music = (byte & 0x04) != 0;
    desc.effects = (byte & 0x08) != 0;
    desc.foley = (byte & 0x10) != 0;
    desc.ambience = (byte & 0x20) != 0;

    if (byte & 0x80) {
        auto text = read_cstring(br);
        if (!text) {
            return std::unexpected(text.error());
        }
        desc.text = std::move(*text);
    }
    return desc;
}

// §9 Table 3 (IAElement) / §10.1: reads one element's ElementID/ElementSize header and slices
// exactly ElementSize bytes as its payload, without interpreting them - the generic mechanism
// behind both "skip an ElementID this reader does not recognize" (the IAElement switch's own
// `default` case) and "skip an ElementID this reader recognizes but a Child context does not
// allow" (Table 4's "Other Child elements shall be ignored").
struct ElementHeader {
    std::uint32_t id = 0;
    std::span<const std::byte> payload;
};

[[nodiscard]] std::expected<ElementHeader, IabError> read_element_header(BitReader& br) {
    auto id = br.read_plex(8);
    if (!id) {
        return std::unexpected(id.error());
    }
    auto size = br.read_plex(8);
    if (!size) {
        return std::unexpected(size.error());
    }
    auto payload = br.read_bytes(static_cast<std::size_t>(*size));
    if (!payload) {
        return std::unexpected(payload.error());
    }
    return ElementHeader{.id = static_cast<std::uint32_t>(*id), .payload = *payload};
}

// §9.1 Table 5 / Table 4: IAFrame's Children are BedDefinition, ObjectDefinition,
// AudioDataDLC, AudioDataPCM, AuthoringToolInfo and UserData.
[[nodiscard]] std::expected<void, IabError> parse_iaframe_children(
        BitReader& br, IaFrame& frame, unsigned count) {
    for (unsigned i = 0; i < count; ++i) {
        auto header = read_element_header(br);
        if (!header) {
            return std::unexpected(header.error());
        }

        switch (header->id) {
            case element_id::kBedDefinition: {
                auto bed = parse_bed_definition(header->payload, frame.frame_rate_code);
                if (!bed) {
                    return std::unexpected(bed.error());
                }
                frame.beds.push_back(std::move(*bed));
                break;
            }
            case element_id::kObjectDefinition: {
                auto object = parse_object_definition(header->payload, frame.frame_rate_code);
                if (!object) {
                    return std::unexpected(object.error());
                }
                frame.objects.push_back(std::move(*object));
                break;
            }
            case element_id::kAudioDataDlc: {
                BitReader dlc_br(header->payload);
                auto audio_data_id = dlc_br.read_plex(8);
                if (!audio_data_id) {
                    return std::unexpected(audio_data_id.error());
                }
                auto dlc_size = dlc_br.read_bits(16);  // §10.7.2
                if (!dlc_size) {
                    return std::unexpected(dlc_size.error());
                }
                auto coded = dlc_br.read_bytes(static_cast<std::size_t>(*dlc_size));
                if (!coded) {
                    return std::unexpected(coded.error());
                }
                AudioDataDlc dlc;
                dlc.audio_data_id = static_cast<std::uint32_t>(*audio_data_id);
                dlc.coded.assign(coded->begin(), coded->end());
                frame.audio_dlc.push_back(std::move(dlc));
                break;
            }
            case element_id::kAudioDataPcm: {
                auto count_per_frame = sample_count(frame.frame_rate_code, frame.sample_rate == 96000);
                if (!count_per_frame) {
                    return std::unexpected(IabError::kReservedFrameRate);
                }

                BitReader pcm_br(header->payload);
                auto audio_data_id = pcm_br.read_plex(8);
                if (!audio_data_id) {
                    return std::unexpected(audio_data_id.error());
                }

                AudioDataPcm pcm;
                pcm.audio_data_id = static_cast<std::uint32_t>(*audio_data_id);
                const std::size_t bytes_per_sample = frame.bit_depth / 8;
                pcm.samples.reserve(*count_per_frame);
                for (std::uint32_t n = 0; n < *count_per_frame; ++n) {
                    auto sample_bytes = pcm_br.read_bytes(bytes_per_sample);
                    if (!sample_bytes) {
                        return std::unexpected(sample_bytes.error());
                    }

                    // §10.8.1: little-endian byte order (each byte itself transmitted MSB-
                    // first) - the multi-byte assembly order here is the opposite of §5.1's
                    // usual default, which this field explicitly overrides.
                    std::int32_t raw = 0;
                    for (std::size_t byte_index = 0; byte_index < bytes_per_sample; ++byte_index) {
                        raw |= static_cast<std::int32_t>(std::to_integer<std::uint8_t>((*sample_bytes)[byte_index]))
                               << (8 * byte_index);
                    }
                    const std::int32_t sign_bit = std::int32_t{1} << (frame.bit_depth - 1);
                    if (raw & sign_bit) {
                        raw -= (sign_bit << 1);
                    }
                    pcm.samples.push_back(static_cast<float>(raw) / static_cast<float>(sign_bit));
                }
                pcm_br.align_to_byte();
                frame.audio_pcm.push_back(std::move(pcm));
                break;
            }
            case element_id::kAuthoringToolInfo: {
                BitReader info_br(header->payload);
                auto uri = read_cstring(info_br);
                if (!uri) {
                    return std::unexpected(uri.error());
                }
                frame.authoring_tool = AuthoringToolInfo{.uri = std::move(*uri)};
                break;
            }
            case element_id::kUserData: {
                BitReader user_br(header->payload);
                auto id_bytes = user_br.read_bytes(16);
                if (!id_bytes) {
                    return std::unexpected(id_bytes.error());
                }
                UserData user;
                std::copy(id_bytes->begin(), id_bytes->end(), user.user_id.begin());
                const std::size_t remaining_bytes = user_br.bits_remaining() / 8;
                auto data_bytes = user_br.read_bytes(remaining_bytes);
                if (!data_bytes) {
                    return std::unexpected(data_bytes.error());
                }
                user.data.assign(data_bytes->begin(), data_bytes->end());
                frame.user_data.push_back(std::move(user));
                break;
            }
            default:
                break;  // §9 Table 4: any other ElementID is ignored (already skipped above)
        }
    }
    return {};
}

// §9.2 Table 6 / Table 4: a BedDefinition's Children are BedDefinition and BedRemap.
[[nodiscard]] std::expected<void, IabError> parse_bed_children(
        BitReader& br, BedDefinition& bed, unsigned count, std::uint8_t frame_rate_code) {
    for (unsigned i = 0; i < count; ++i) {
        auto header = read_element_header(br);
        if (!header) {
            return std::unexpected(header.error());
        }

        switch (header->id) {
            case element_id::kBedDefinition: {
                auto child = parse_bed_definition(header->payload, frame_rate_code);
                if (!child) {
                    return std::unexpected(child.error());
                }
                bed.beds.push_back(std::move(*child));
                break;
            }
            case element_id::kBedRemap: {
                auto remap = parse_bed_remap(header->payload, frame_rate_code);
                if (!remap) {
                    return std::unexpected(remap.error());
                }
                bed.remaps.push_back(std::move(*remap));
                break;
            }
            default:
                break;  // §9 Table 4: only BedDefinition/BedRemap are allowed here
        }
    }
    return {};
}

// §9.4 Table 8 / Table 4: an ObjectDefinition's Children are ObjectDefinition and
// ObjectZoneDefinition19.
[[nodiscard]] std::expected<void, IabError> parse_object_children(
        BitReader& br, ObjectDefinition& object, unsigned count, std::uint8_t frame_rate_code) {
    for (unsigned i = 0; i < count; ++i) {
        auto header = read_element_header(br);
        if (!header) {
            return std::unexpected(header.error());
        }

        switch (header->id) {
            case element_id::kObjectDefinition: {
                auto child = parse_object_definition(header->payload, frame_rate_code);
                if (!child) {
                    return std::unexpected(child.error());
                }
                object.objects.push_back(std::move(*child));
                break;
            }
            case element_id::kObjectZoneDefinition19: {
                auto zone19 = parse_object_zone19(header->payload, frame_rate_code);
                if (!zone19) {
                    return std::unexpected(zone19.error());
                }
                object.zone19 = std::move(*zone19);
                break;
            }
            default:
                break;  // §9 Table 4: only ObjectDefinition/ObjectZoneDefinition19 are allowed here
        }
    }
    return {};
}

// §9.2 Table 6.
std::expected<BedDefinition, IabError> parse_bed_definition(
        std::span<const std::byte> payload, std::uint8_t frame_rate_code) {
    BitReader br(payload);
    BedDefinition bed;

    auto meta_id = br.read_plex(8);
    if (!meta_id) {
        return std::unexpected(meta_id.error());
    }
    bed.meta_id = static_cast<std::uint32_t>(*meta_id);

    auto conditional = br.read_bits(1);
    if (!conditional) {
        return std::unexpected(conditional.error());
    }
    bed.activation.conditional = (*conditional != 0);
    if (bed.activation.conditional) {
        auto use_case = br.read_bits(8);
        if (!use_case) {
            return std::unexpected(use_case.error());
        }
        bed.activation.use_case = static_cast<UseCaseCode>(*use_case);
    }

    auto channel_count = br.read_plex(4);
    if (!channel_count) {
        return std::unexpected(channel_count.error());
    }
    // No reserve(): ChannelCount is attacker-controlled and Plex(4) can escalate arbitrarily
    // high, so an untrusted value here must not drive a pre-allocation - each iteration below
    // performs real, bounds-checked bit reads and fails fast with kTruncated well before a
    // corrupt or hostile count could grow this vector unreasonably large.
    for (std::uint64_t n = 0; n < *channel_count; ++n) {
        BedChannel channel;

        auto channel_id = br.read_plex(4);
        if (!channel_id) {
            return std::unexpected(channel_id.error());
        }
        channel.channel_id = static_cast<std::uint32_t>(*channel_id);

        auto audio_data_id = br.read_plex(8);
        if (!audio_data_id) {
            return std::unexpected(audio_data_id.error());
        }
        channel.audio_data_id = static_cast<std::uint32_t>(*audio_data_id);

        auto gain = read_unity_gain(br);
        if (!gain) {
            return std::unexpected(gain.error());
        }
        channel.gain = *gain;

        auto decor_exists = br.read_bits(1);
        if (!decor_exists) {
            return std::unexpected(decor_exists.error());
        }
        if (*decor_exists != 0) {
            auto reserved = br.read_bits(4);  // Reserved, set to 0
            if (!reserved) {
                return std::unexpected(reserved.error());
            }
            auto decor = read_decor(br);
            if (!decor) {
                return std::unexpected(decor.error());
            }
            channel.decorrelation = *decor;
        }

        bed.channels.push_back(channel);
    }

    auto reserved10 = br.read_bits(10);  // Reserved, set to 0x180
    if (!reserved10) {
        return std::unexpected(reserved10.error());
    }

    br.align_to_byte();

    auto description = parse_audio_description(br);
    if (!description) {
        return std::unexpected(description.error());
    }
    bed.description = std::move(*description);

    auto sub_element_count = br.read_plex(8);
    if (!sub_element_count) {
        return std::unexpected(sub_element_count.error());
    }

    auto children = parse_bed_children(br, bed, static_cast<unsigned>(*sub_element_count), frame_rate_code);
    if (!children) {
        return std::unexpected(children.error());
    }

    return bed;
}

// §9.3 Table 7.
std::expected<BedRemap, IabError> parse_bed_remap(
        std::span<const std::byte> payload, std::uint8_t frame_rate_code) {
    auto sub_block_count = num_pan_sub_blocks(frame_rate_code);
    if (!sub_block_count) {
        return std::unexpected(IabError::kReservedFrameRate);
    }

    BitReader br(payload);
    BedRemap remap;

    auto meta_id = br.read_plex(8);
    if (!meta_id) {
        return std::unexpected(meta_id.error());
    }
    remap.meta_id = static_cast<std::uint32_t>(*meta_id);

    auto use_case = br.read_bits(8);
    if (!use_case) {
        return std::unexpected(use_case.error());
    }
    remap.use_case = static_cast<UseCaseCode>(*use_case);

    auto source_channels = br.read_plex(4);
    if (!source_channels) {
        return std::unexpected(source_channels.error());
    }
    remap.source_channels = static_cast<std::uint32_t>(*source_channels);

    auto destination_channels = br.read_plex(4);
    if (!destination_channels) {
        return std::unexpected(destination_channels.error());
    }
    remap.destination_channels = static_cast<std::uint32_t>(*destination_channels);

    remap.sub_blocks.reserve(*sub_block_count);
    for (unsigned sb = 0; sb < *sub_block_count; ++sb) {
        BedRemapSubBlock block;
        if (sb == 0) {
            block.has_remap_info = true;
        } else {
            auto exists = br.read_bits(1);
            if (!exists) {
                return std::unexpected(exists.error());
            }
            block.has_remap_info = (*exists != 0);
        }

        if (block.has_remap_info) {
            // No reserve(): DestinationChannels/SourceChannels are the same attacker-
            // controlled Plex(4) shape as BedDefinition's ChannelCount above.
            for (std::uint32_t o_chan = 0; o_chan < remap.destination_channels; ++o_chan) {
                auto dest_id = br.read_plex(4);
                if (!dest_id) {
                    return std::unexpected(dest_id.error());
                }
                block.destination_channel_ids.push_back(static_cast<std::uint32_t>(*dest_id));

                std::vector<double> row;
                for (std::uint32_t i_chan = 0; i_chan < remap.source_channels; ++i_chan) {
                    auto gain = read_unity_gain(br);
                    if (!gain) {
                        return std::unexpected(gain.error());
                    }
                    row.push_back(*gain);
                }
                block.gains.push_back(std::move(row));
            }
        }

        remap.sub_blocks.push_back(std::move(block));
    }

    br.align_to_byte();

    auto reserved = br.read_plex(8);  // Reserved, set to 0
    if (!reserved) {
        return std::unexpected(reserved.error());
    }

    return remap;
}

// §9.4 Table 8.
std::expected<ObjectDefinition, IabError> parse_object_definition(
        std::span<const std::byte> payload, std::uint8_t frame_rate_code) {
    auto sub_block_count = num_pan_sub_blocks(frame_rate_code);
    if (!sub_block_count) {
        return std::unexpected(IabError::kReservedFrameRate);
    }

    BitReader br(payload);
    ObjectDefinition object;

    auto meta_id = br.read_plex(8);
    if (!meta_id) {
        return std::unexpected(meta_id.error());
    }
    object.meta_id = static_cast<std::uint32_t>(*meta_id);

    auto audio_data_id = br.read_plex(8);
    if (!audio_data_id) {
        return std::unexpected(audio_data_id.error());
    }
    object.audio_data_id = static_cast<std::uint32_t>(*audio_data_id);

    auto conditional = br.read_bits(1);
    if (!conditional) {
        return std::unexpected(conditional.error());
    }
    object.activation.conditional = (*conditional != 0);
    if (object.activation.conditional) {
        auto reserved1 = br.read_bits(1);  // Reserved, set to 1
        if (!reserved1) {
            return std::unexpected(reserved1.error());
        }
        auto use_case = br.read_bits(8);
        if (!use_case) {
            return std::unexpected(use_case.error());
        }
        object.activation.use_case = static_cast<UseCaseCode>(*use_case);
    }

    auto reserved0 = br.read_bits(1);  // Reserved, set to 0
    if (!reserved0) {
        return std::unexpected(reserved0.error());
    }

    object.sub_blocks.reserve(*sub_block_count);
    for (unsigned sb = 0; sb < *sub_block_count; ++sb) {
        ObjectPanSubBlock block;
        if (sb == 0) {
            block.has_pan_info = true;
        } else {
            auto exists = br.read_bits(1);
            if (!exists) {
                return std::unexpected(exists.error());
            }
            block.has_pan_info = (*exists != 0);
        }

        if (block.has_pan_info) {
            auto gain = read_unity_gain(br);
            if (!gain) {
                return std::unexpected(gain.error());
            }
            block.gain = *gain;

            auto reserved3 = br.read_bits(3);  // Reserved, set to 0b001
            if (!reserved3) {
                return std::unexpected(reserved3.error());
            }

            auto pos_x = br.read_bits(16);
            if (!pos_x) {
                return std::unexpected(pos_x.error());
            }
            auto pos_y = br.read_bits(16);
            if (!pos_y) {
                return std::unexpected(pos_y.error());
            }
            auto pos_z = br.read_bits(16);
            if (!pos_z) {
                return std::unexpected(pos_z.error());
            }
            block.position.x = distance_xy(*pos_x, 16);
            block.position.y = distance_xy(*pos_y, 16);
            block.position.z = distance_z(*pos_z, 16);

            auto snap = br.read_bits(1);
            if (!snap) {
                return std::unexpected(snap.error());
            }
            block.snap = (*snap != 0);
            if (block.snap) {
                auto tol_exists = br.read_bits(1);
                if (!tol_exists) {
                    return std::unexpected(tol_exists.error());
                }
                if (*tol_exists != 0) {
                    auto tol = br.read_bits(12);
                    if (!tol) {
                        return std::unexpected(tol.error());
                    }
                    block.snap_tolerance = distance_z(*tol, 12);
                }
                auto res2 = br.read_bits(1);  // Res2, set to 0
                if (!res2) {
                    return std::unexpected(res2.error());
                }
            }

            auto zone_control = br.read_bits(1);
            if (!zone_control) {
                return std::unexpected(zone_control.error());
            }
            if (*zone_control != 0) {
                std::array<double, kZoneCount> gains{};
                for (auto& gain_value : gains) {
                    auto zg = read_zone_gain(br);
                    if (!zg) {
                        return std::unexpected(zg.error());
                    }
                    gain_value = *zg;
                }
                block.zone_gains = gains;
            }

            auto spread_mode = br.read_bits(2);
            if (!spread_mode) {
                return std::unexpected(spread_mode.error());
            }
            block.spread.mode = static_cast<ObjectSpreadMode>(*spread_mode);
            switch (block.spread.mode) {
                case ObjectSpreadMode::kLowRez: {
                    auto spread = br.read_bits(8);
                    if (!spread) {
                        return std::unexpected(spread.error());
                    }
                    block.spread.x = block.spread.y = block.spread.z = distance_z(*spread, 8);
                    break;
                }
                case ObjectSpreadMode::kNone:
                    break;
                case ObjectSpreadMode::kOneD: {
                    auto spread = br.read_bits(12);
                    if (!spread) {
                        return std::unexpected(spread.error());
                    }
                    block.spread.x = block.spread.y = block.spread.z = distance_z(*spread, 12);
                    break;
                }
                case ObjectSpreadMode::kThreeD: {
                    auto sx = br.read_bits(12);
                    if (!sx) {
                        return std::unexpected(sx.error());
                    }
                    auto sy = br.read_bits(12);
                    if (!sy) {
                        return std::unexpected(sy.error());
                    }
                    auto sz = br.read_bits(12);
                    if (!sz) {
                        return std::unexpected(sz.error());
                    }
                    block.spread.x = distance_z(*sx, 12);
                    block.spread.y = distance_z(*sy, 12);
                    block.spread.z = distance_z(*sz, 12);
                    break;
                }
            }

            auto reserved4 = br.read_bits(4);  // Reserved, set to 0
            if (!reserved4) {
                return std::unexpected(reserved4.error());
            }

            auto decor = read_decor(br);
            if (!decor) {
                return std::unexpected(decor.error());
            }
            block.decorrelation = *decor;
        }

        object.sub_blocks.push_back(block);
    }

    br.align_to_byte();

    auto description = parse_audio_description(br);
    if (!description) {
        return std::unexpected(description.error());
    }
    object.description = std::move(*description);

    auto sub_element_count = br.read_plex(8);
    if (!sub_element_count) {
        return std::unexpected(sub_element_count.error());
    }

    auto children =
        parse_object_children(br, object, static_cast<unsigned>(*sub_element_count), frame_rate_code);
    if (!children) {
        return std::unexpected(children.error());
    }

    return object;
}

// §9.5 Table 9.
std::expected<ObjectZoneDefinition19, IabError> parse_object_zone19(
        std::span<const std::byte> payload, std::uint8_t frame_rate_code) {
    auto sub_block_count = num_pan_sub_blocks(frame_rate_code);
    if (!sub_block_count) {
        return std::unexpected(IabError::kReservedFrameRate);
    }

    BitReader br(payload);
    ObjectZoneDefinition19 zone19;
    zone19.sub_blocks.reserve(*sub_block_count);

    for (unsigned sb = 0; sb < *sub_block_count; ++sb) {
        Zone19SubBlock block;
        if (sb == 0) {
            block.has_zone_info = true;
        } else {
            auto exists = br.read_bits(1);
            if (!exists) {
                return std::unexpected(exists.error());
            }
            block.has_zone_info = (*exists != 0);
        }

        if (block.has_zone_info) {
            for (auto& gain : block.zone_gains) {
                auto zg = read_zone_gain(br);
                if (!zg) {
                    return std::unexpected(zg.error());
                }
                gain = *zg;
            }
        }

        zone19.sub_blocks.push_back(block);
    }

    br.align_to_byte();
    return zone19;
}

}  // namespace

// §9.1 Table 5: the public entry point - parses one already-extracted IAFrame element's
// payload (see ac3iab.hpp's own comment on why this takes a bare payload rather than a full
// IAElement including its own header).
std::expected<IaFrame, IabError> parse_iaframe(std::span<const std::byte> payload) {
    BitReader br(payload);
    IaFrame frame;

    auto version = br.read_bits(8);
    if (!version) {
        return std::unexpected(version.error());
    }
    if (*version != 1) {
        return std::unexpected(IabError::kReservedVersion);  // §10.2.1: 0 and 2 are forbidden
    }
    frame.version = static_cast<std::uint8_t>(*version);

    auto sample_rate_code = br.read_bits(2);
    if (!sample_rate_code) {
        return std::unexpected(sample_rate_code.error());
    }
    switch (*sample_rate_code) {
        case 0x0:
            frame.sample_rate = 48000;
            break;
        case 0x1:
            frame.sample_rate = 96000;
            break;
        default:
            return std::unexpected(IabError::kReservedSampleRate);
    }

    auto bit_depth_code = br.read_bits(2);
    if (!bit_depth_code) {
        return std::unexpected(bit_depth_code.error());
    }
    switch (*bit_depth_code) {
        case 0x0:
            frame.bit_depth = 16;
            break;
        case 0x1:
            frame.bit_depth = 24;
            break;
        default:
            return std::unexpected(IabError::kReservedBitDepth);
    }

    auto frame_rate_code = br.read_bits(4);
    if (!frame_rate_code) {
        return std::unexpected(frame_rate_code.error());
    }
    if (!num_pan_sub_blocks(static_cast<std::uint8_t>(*frame_rate_code))) {
        return std::unexpected(IabError::kReservedFrameRate);
    }
    frame.frame_rate_code = static_cast<std::uint8_t>(*frame_rate_code);

    auto max_rendered = br.read_plex(8);
    if (!max_rendered) {
        return std::unexpected(max_rendered.error());
    }
    frame.max_rendered = static_cast<std::uint32_t>(*max_rendered);

    br.align_to_byte();

    auto sub_element_count = br.read_plex(8);
    if (!sub_element_count) {
        return std::unexpected(sub_element_count.error());
    }

    auto children = parse_iaframe_children(br, frame, static_cast<unsigned>(*sub_element_count));
    if (!children) {
        return std::unexpected(children.error());
    }

    return frame;
}

}  // namespace ac3iab
