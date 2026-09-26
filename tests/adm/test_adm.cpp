#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "platform/process.hpp"

#include "ac3adm/ac3adm.hpp"

// These tests build BW64/RF64 fixtures byte-by-byte, independently of
// src/ac3adm's own implementation (which is itself just a thin translation
// layer over the vendored libbw64/libadm - see src/ac3adm/CMakeLists.txt),
// rather than round-tripping data this same code produced - the same
// reasoning test_mpegts.cpp and test_matroska.cpp document for their own
// independent readers/writers. ac3adm::ac3adm began as a reader only (phase 1
// of roadmap item B1); roadmap item IM2 later gave it write_bw64(), and the
// write tests at the end of this file keep the same rule from the other side:
// they check the bytes write_bw64() put on disk directly, not only what the
// same two libraries read back from them.
//
// Every test below goes through the public ac3adm API only (parse_bw64(), and
// write_bw64() for the write tests) -
// libadm and libbw64 already have their own upstream test suites for their
// own internals (chunk-walking, XML/schema validation), so what is worth
// re-testing here is this module's own boundary: does a real BW64 file
// carrying a given ADM shape come out as the right ac3adm::AdmModel/
// AdmDocument, and does a malformed one come back as the right AdmError.
//
// The embedded ADM XML for the "Car" fixture is adapted from Recommendation
// ITU-R BS.2076-2 (10/2019) Annex 2 §2's own "Object-based example" (the
// "Car" object), trimmed to one audioBlockFormat instead of three for
// brevity - it is the standard's own worked example, not invented data.

namespace {

using Bytes = std::string;

void put_u16le(Bytes& out, std::uint16_t value) {
    out.push_back(static_cast<char>(value & 0xFFu));
    out.push_back(static_cast<char>((value >> 8) & 0xFFu));
}

void put_u32le(Bytes& out, std::uint32_t value) {
    put_u16le(out, static_cast<std::uint16_t>(value & 0xFFFFu));
    put_u16le(out, static_cast<std::uint16_t>((value >> 16) & 0xFFFFu));
}

void put_u64le(Bytes& out, std::uint64_t value) {
    put_u32le(out, static_cast<std::uint32_t>(value & 0xFFFF'FFFFu));
    put_u32le(out, static_cast<std::uint32_t>((value >> 32) & 0xFFFF'FFFFu));
}

void put_fourcc(Bytes& out, std::string_view cc) {
    REQUIRE(cc.size() == 4);
    out += cc;
}

// A fixed-width ADM ID field (BS.2088-1 §8.2's UID[12]/trackRef[14]/
// packRef[11]), written verbatim at exactly `width` bytes.
void put_fixed(Bytes& out, std::string_view value, std::size_t width) {
    REQUIRE(value.size() == width);
    out += value;
}

// Appends one chunk: 4-byte ID, 4-byte declared size, content, and a pad
// byte if the content length is odd (BS.2088-1 §4's own note on chunk
// alignment). `declared_size` lets a test force 0xFFFFFFFF to exercise the
// <ds64>-resolution path even for a chunk far smaller than 4 GB - legal for
// a real writer to do too, and the only practical way to test that path
// without an actual multi-gigabyte fixture.
void append_chunk(Bytes& out, std::string_view id, const Bytes& content,
                   std::optional<std::uint32_t> declared_size = std::nullopt) {
    put_fourcc(out, id);
    put_u32le(out, declared_size.value_or(static_cast<std::uint32_t>(content.size())));
    out += content;
    if (content.size() % 2 != 0) {
        out.push_back('\0');
    }
}

Bytes build_fmt_chunk(std::uint16_t channels, std::uint32_t sample_rate, std::uint16_t bits_per_sample) {
    Bytes fmt;
    put_u16le(fmt, 1);  // WAVE_FORMAT_PCM, BS.2088-1 §2.6.2
    put_u16le(fmt, channels);
    put_u32le(fmt, sample_rate);
    const auto block_align = static_cast<std::uint16_t>(channels * (bits_per_sample / 8));
    put_u32le(fmt, sample_rate * block_align);
    put_u16le(fmt, block_align);
    put_u16le(fmt, bits_per_sample);
    return fmt;
}

// A WAVE_FORMAT_IEEE_FLOAT (formatTag 3) <fmt > chunk - used by the float32/float64 tests below.
// Not accepted by anything this project's own encoder/decoder writes or reads elsewhere; exists
// purely to exercise the one shape ac3adm's own read/write paths never produce.
Bytes build_float_fmt_chunk(std::uint16_t channels, std::uint32_t sample_rate, std::uint16_t bits_per_sample) {
    Bytes fmt;
    put_u16le(fmt, 3);  // WAVE_FORMAT_IEEE_FLOAT
    put_u16le(fmt, channels);
    put_u32le(fmt, sample_rate);
    const auto block_align = static_cast<std::uint16_t>(channels * (bits_per_sample / 8));
    put_u32le(fmt, sample_rate * block_align);
    put_u16le(fmt, block_align);
    put_u16le(fmt, bits_per_sample);
    return fmt;
}

// One track, one UID - BS.2088-1 §8.3.1's "simple stereo" shape reduced to
// mono, matching this fixture's one-channel <fmt>.
Bytes build_chna_chunk() {
    Bytes chna;
    put_u16le(chna, 1);  // numTracks
    put_u16le(chna, 1);  // numUIDs
    put_u16le(chna, 1);  // trackIndex (1-based)
    put_fixed(chna, "ATU_00000001", 12);
    put_fixed(chna, "AT_00031001_01", 14);
    put_fixed(chna, "AP_00031001", 11);
    chna.push_back('\0');  // pad byte, §8.2
    return chna;
}

Bytes build_pcm16_data(int frames) {
    Bytes data;
    for (int frame = 0; frame < frames; ++frame) {
        const auto sample = static_cast<std::int16_t>((frame * 4000) - 12000);
        put_u16le(data, static_cast<std::uint16_t>(sample));
    }
    return data;
}

// Raw IEEE-754 samples for build_float_fmt_chunk's WAVE_FORMAT_IEEE_FLOAT fixtures - written
// bit-for-bit, not scaled, matching libbw64's own decodeFloatSamples (utils.hpp).
Bytes build_float32_data(std::initializer_list<float> samples) {
    Bytes data;
    for (const float sample : samples) {
        put_u32le(data, std::bit_cast<std::uint32_t>(sample));
    }
    return data;
}

// The 64-bit-per-sample counterpart - libbw64's decodeFloatSamples switches on bitsPerSample
// (only 32 and 64 decode as float at all; 64 reads a double bit-for-bit and narrows to float),
// so this exercises a distinct branch from build_float32_data's, not just a wider one.
Bytes build_float64_data(std::initializer_list<double> samples) {
    Bytes data;
    for (const double sample : samples) {
        put_u64le(data, std::bit_cast<std::uint64_t>(sample));
    }
    return data;
}

std::string_view kCarAdmXml = R"(<?xml version="1.0" encoding="UTF-8"?>
<audioFormatExtended version="ITU-R_BS.2076-2">
  <audioProgramme audioProgrammeID="APR_1001" audioProgrammeName="CarsSounds">
    <audioContentIDRef>ACO_1001</audioContentIDRef>
  </audioProgramme>
  <audioContent audioContentID="ACO_1001" audioContentName="Cars">
    <audioObjectIDRef>AO_1001</audioObjectIDRef>
  </audioContent>
  <audioObject audioObjectID="AO_1001" audioObjectName="Car" start="00:00:00.00000">
    <audioPackFormatIDRef>AP_00031001</audioPackFormatIDRef>
    <audioTrackUIDRef>ATU_00000001</audioTrackUIDRef>
  </audioObject>
  <audioPackFormat audioPackFormatID="AP_00031001" audioPackFormatName="Car" typeLabel="0003" typeDefinition="Objects">
    <audioChannelFormatIDRef>AC_00031001</audioChannelFormatIDRef>
  </audioPackFormat>
  <audioChannelFormat audioChannelFormatID="AC_00031001" audioChannelFormatName="Car1" typeLabel="0003" typeDefinition="Objects">
    <audioBlockFormat audioBlockFormatID="AB_00031001_00000001">
      <position coordinate="azimuth">-22.5</position>
      <position coordinate="elevation">5.0</position>
      <position coordinate="distance">1.0</position>
      <width>12.5</width>
    </audioBlockFormat>
  </audioChannelFormat>
  <audioStreamFormat audioStreamFormatID="AS_00031001" audioStreamFormatName="PCM_Car1" formatLabel="0001" formatDefinition="PCM">
    <audioChannelFormatIDRef>AC_00031001</audioChannelFormatIDRef>
    <audioTrackFormatIDRef>AT_00031001_01</audioTrackFormatIDRef>
  </audioStreamFormat>
  <audioTrackFormat audioTrackFormatID="AT_00031001_01" audioTrackFormatName="PCM_Car1" formatLabel="0001" formatDefinition="PCM">
    <audioStreamFormatIDRef>AS_00031001</audioStreamFormatIDRef>
  </audioTrackFormat>
  <audioTrackUID UID="ATU_00000001" sampleRate="48000" bitDepth="16">
    <audioTrackFormatIDRef>AT_00031001_01</audioTrackFormatIDRef>
    <audioPackFormatIDRef>AP_00031001</audioPackFormatIDRef>
  </audioTrackUID>
</audioFormatExtended>
)";

// A minimal DirectSpeakers channel (BS.2076-2 §5.4.3.1, Table 12) - one
// block naming a single loudspeaker position by label and polar coordinate.
//
// IDs use a "9001" suffix rather than a low, standard-looking one: libadm's parseXml() always
// merges the file's own content into a document pre-populated with BS.2076-2 Annex A's "common
// definitions" (see find_by_id's own comment below), and it turns out the standard set already
// defines an "AC_00010001"/"AB_00010001_00000001" of its own for the very first DirectSpeakers
// layout - reusing that ID here collided ("Duplicate Id AC_00010001 found") until this fixture
// picked one clear of the common set's low ID space instead, confirmed empirically against
// libadm's own vendored resources/common_definitions.xml.
std::string_view kDirectSpeakersAdmXml = R"(<?xml version="1.0" encoding="UTF-8"?>
<audioFormatExtended version="ITU-R_BS.2076-2">
  <audioChannelFormat audioChannelFormatID="AC_00019001" audioChannelFormatName="FrontLeft" typeLabel="0001" typeDefinition="DirectSpeakers">
    <audioBlockFormat audioBlockFormatID="AB_00019001_00000001">
      <speakerLabel>M+030</speakerLabel>
      <position coordinate="azimuth">30.0</position>
      <position coordinate="elevation">0.0</position>
      <position coordinate="distance">1.0</position>
    </audioBlockFormat>
  </audioChannelFormat>
</audioFormatExtended>
)";

// A minimal first-order HOA channel (BS.2076-2 §5.4.3.4, Table 18) - degree/
// order/normalization identify one ACN/N3D component. Same "9001" suffix
// reasoning as kDirectSpeakersAdmXml above - "AC_00040102" is itself one of
// the common set's own HOA definitions.
std::string_view kHoaAdmXml = R"(<?xml version="1.0" encoding="UTF-8"?>
<audioFormatExtended version="ITU-R_BS.2076-2">
  <audioChannelFormat audioChannelFormatID="AC_00049001" audioChannelFormatName="N3D_ACN_1" typeLabel="0004" typeDefinition="HOA">
    <audioBlockFormat audioBlockFormatID="AB_00049001_00000001">
      <degree>1</degree>
      <order>-1</order>
      <normalization>N3D</normalization>
    </audioBlockFormat>
  </audioChannelFormat>
</audioFormatExtended>
)";

// A Cartesian Objects channel (BS.2076-2 Table 16/17) with a jumpPosition
// (§10.3) - exercises the cartesian branch of ac3adm::Position and the
// channelLock/jumpPosition fields adm_model.cpp's convert() populates.
std::string_view kCartesianObjectAdmXml = R"(<?xml version="1.0" encoding="UTF-8"?>
<audioFormatExtended version="ITU-R_BS.2076-2">
  <audioChannelFormat audioChannelFormatID="AC_00031002" audioChannelFormatName="Car2" typeLabel="0003" typeDefinition="Objects">
    <audioBlockFormat audioBlockFormatID="AB_00031002_00000001">
      <cartesian>1</cartesian>
      <position coordinate="X">-0.2</position>
      <position coordinate="Y">0.5</position>
      <position coordinate="Z">0.0</position>
      <jumpPosition interpolationLength="0.05">1</jumpPosition>
    </audioBlockFormat>
  </audioChannelFormat>
</audioFormatExtended>
)";

// Wraps fmt/chna/axml/data into a classic 32-bit "RIFF....WAVE" file -
// BS.2088-1's own compatibility case (§2.5) for a file that stays under the
// 4 GB threshold, which is what most real ADM BWF masters actually are.
//
// An empty `chna` or `axml` means "this chunk is genuinely absent", not "present with zero
// bytes of content" - the two are different things a reader must tell apart (BS.2088-1 §9 rule
// 2: <axml> is optional; a 0-byte <axml> is instead just a chunk holding an empty, non-
// well-formed "document"). libbw64's own parseChnaChunk() also hard-rejects a <chna> chunk
// under 4 bytes ("illegal chna chunk size"), so a real 0-byte <chna> is not even a legal fixture
// to write in the first place - skipping the append when empty covers both chunks correctly
// with one rule, confirmed necessary by actually hitting both failure modes.
Bytes build_riff(const Bytes& fmt, const Bytes& chna, const Bytes& axml, const Bytes& data) {
    Bytes body;
    append_chunk(body, "fmt ", fmt);
    if (!chna.empty()) {
        append_chunk(body, "chna", chna);
    }
    if (!axml.empty()) {
        append_chunk(body, "axml", axml);
    }
    append_chunk(body, "data", data);

    Bytes file;
    put_fourcc(file, "RIFF");
    put_u32le(file, static_cast<std::uint32_t>(4 + body.size()));  // "WAVE" + body, classic RIFF ckSize
    put_fourcc(file, "WAVE");
    file += body;
    return file;
}

// Wraps the same four chunks into an RF64 file with a real <ds64> chunk
// (BS.2088-1 §§2.4, 4), deliberately declaring <data>'s own 32-bit ckSize
// as 0xFFFFFFFF so its real size has to come from ds64's dataSizeLow/High -
// the specific mechanism this container format exists for.
Bytes build_rf64(const Bytes& fmt, const Bytes& chna, const Bytes& axml, const Bytes& data) {
    // See build_riff()'s own comment just above: an empty chna/axml means "absent", so it
    // contributes nothing to either the total size below or the chunk list itself.
    const auto chunk_total = [](const Bytes& content) {
        return content.empty() ? std::size_t{0} : 8 + content.size() + (content.size() % 2);
    };
    const std::size_t ds64_content_size = 28;  // bw64Size(8) + dataSize(8) + dummy(8) + tableLength(4), no table[]
    const std::size_t ds64_total = 8 + ds64_content_size + (ds64_content_size % 2);
    const std::uint64_t bw64_size = 4 /* "WAVE" */ + ds64_total + chunk_total(fmt) + chunk_total(chna) +
                                     chunk_total(axml) + chunk_total(data);

    Bytes ds64_content;
    put_u64le(ds64_content, bw64_size);
    put_u64le(ds64_content, data.size());  // dataSizeLow/High
    put_u64le(ds64_content, 0);            // dummyLow/High, §4.2
    put_u32le(ds64_content, 0);            // tableLength - no other chunk needs 64-bit resolution here

    Bytes body;
    append_chunk(body, "ds64", ds64_content);
    append_chunk(body, "fmt ", fmt);
    if (!chna.empty()) {
        append_chunk(body, "chna", chna);
    }
    if (!axml.empty()) {
        append_chunk(body, "axml", axml);
    }
    append_chunk(body, "data", data, 0xFFFFFFFFu);  // forces ds64 resolution

    Bytes file;
    put_fourcc(file, "RF64");
    put_u32le(file, 0xFFFFFFFFu);  // top-level size also unresolved
    put_fourcc(file, "WAVE");
    file += body;
    return file;
}

Bytes minimal_fixture_bytes(bool as_rf64) {
    const auto fmt = build_fmt_chunk(1, 48000, 16);
    const auto chna = build_chna_chunk();
    const Bytes axml(kCarAdmXml);
    const auto data = build_pcm16_data(4);
    return as_rf64 ? build_rf64(fmt, chna, axml, data) : build_riff(fmt, chna, axml, data);
}

// A one-channel BW64 file wrapping the given axml content, with no <chna>
// entry (not needed by the typeDefinition-specific tests below - they only
// look at doc->model, not doc->chna).
Bytes wrap_axml_only(std::string_view axml_xml) {
    const auto fmt = build_fmt_chunk(1, 48000, 16);
    const auto data = build_pcm16_data(2);
    return build_riff(fmt, Bytes{}, Bytes(axml_xml), data);
}

// libadm's own parseXml() always merges the file's own content into a document already
// pre-populated with BS.2076-2 Annex A's "common definitions" (43 pack formats, 300 each of
// channel/stream/track formats, one per standard loudspeaker layout - confirmed by grepping
// libadm's own vendored resources/common_definitions.xml). Real ADM files rely on being able to
// reference those IDs (e.g. a stereo bed's pack format "AP_00010002") without re-declaring them
// locally, so ac3adm::ac3adm deliberately keeps them in the resulting AdmModel rather than
// filtering them back out - phase 2 needs exactly this, a pack/channel/stream/track format
// reference that resolves regardless of whether the file re-declared it or relied on the common
// set. That means pack_formats/channel_formats/stream_formats/track_formats are never just "what
// this one fixture defined" the way programmes/contents/objects/track_uids still are (common_
// definitions.xml defines none of those four) - this helper finds one specific element by ID out
// of a collection that may also hold the common set, rather than assuming it is the only/first
// entry.
//
// `id` is std::string_view, not const std::string& - every call site here passes a string
// literal, which would otherwise bind to a temporary std::string constructed just for the call.
// GCC 15's -Wdangling-reference (real, -Werror on the Linux/GCC preset - not something MSVC
// catches, confirmed by hitting this only on the Linux leg) flags the caller's own `const auto&`
// binding to this function's returned container element as a possible dangling reference in
// that situation, even though the returned reference is into `elements` and never aliases `id`
// at all - string_view sidesteps the whole question since a literal converts to it without
// constructing anything owning.
template <typename T>
const T& find_by_id(const std::vector<T>& elements, std::string_view id) {
    auto it = std::find_if(elements.begin(), elements.end(), [&](const T& e) { return e.id == id; });
    REQUIRE(it != elements.end());
    return *it;
}

std::uint16_t get_u16le(std::string_view bytes, std::size_t at) {
    return static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[at]) |
                                      (static_cast<unsigned char>(bytes[at + 1]) << 8));
}

std::uint32_t get_u32le(std::string_view bytes, std::size_t at) {
    return static_cast<std::uint32_t>(get_u16le(bytes, at)) |
           (static_cast<std::uint32_t>(get_u16le(bytes, at + 2)) << 16);
}

// The content of the first top-level chunk named `id` in a RIFF file - append_chunk()'s layout
// above, read back: id, 32-bit little-endian size, content, a pad byte after odd-sized content.
// The 32-bit sizes are all a file this small needs: libbw64's writer keeps it plain RIFF and only
// turns a file into BW64 with a <ds64> chunk past 4 GB.
std::optional<std::string_view> find_chunk(std::string_view file, std::string_view id) {
    std::size_t at = 12;  // "RIFF", the RIFF size, "WAVE"
    while (at + 8 <= file.size()) {
        const std::size_t size = get_u32le(file, at + 4);
        if (file.substr(at, 4) == id) {
            return file.substr(at + 8, size);
        }
        at += 8 + size + (size % 2);
    }
    return std::nullopt;
}

// Every <audioTrackUID ...> start tag in `xml`, as written. The space after the element name keeps
// an audioObject's <audioTrackUIDRef> children out of the match.
std::vector<std::string_view> track_uid_start_tags(std::string_view xml) {
    constexpr std::string_view kOpen = "<audioTrackUID ";
    std::vector<std::string_view> tags;
    for (auto at = xml.find(kOpen); at != std::string_view::npos; at = xml.find(kOpen, at + kOpen.size())) {
        const auto end = xml.find('>', at);
        REQUIRE(end != std::string_view::npos);
        tags.push_back(xml.substr(at, end - at + 1));
    }
    return tags;
}

// A document write_bw64() accepts: one audioObject per entry of `declared_bit_depths`, each on its
// own track, with that entry as its AudioTrackUid's bit depth (nullopt: has_bit_depth false). Each
// object's audioPackFormat and audioChannelFormat are `type` - kObjects, or kDirectSpeakers, the
// writer's other supported typeDefinition - and each channel carries one cartesian block. Built
// with the full audioStreamFormat -> audioTrackFormat chain ac3::admbridge::write() uses rather
// than BS.2076-2's plain-PCM shortcut: libadm's reassignIds() gives any audioChannelFormat no
// audioStreamFormat references the id zero (bridge.cpp's own comment on it), and several channels
// collapsed onto one id read back as a duplicate-ID failure, not as anything a test using this
// means to check.
ac3adm::AdmDocument objects_document(
    const std::vector<std::optional<std::uint32_t>>& declared_bit_depths,
    ac3adm::TypeDefinition type = ac3adm::TypeDefinition::kObjects) {
    ac3adm::AdmDocument document;
    document.audio.sample_rate = 48000;
    auto& model = document.model;

    ac3adm::AudioContent content;
    content.id = "content";
    content.name = "Programme";

    for (std::size_t i = 0; i < declared_bit_depths.size(); ++i) {
        const std::string key = std::to_string(i);

        ac3adm::AudioBlockFormat block;
        block.cartesian = true;
        block.position = ac3adm::CartesianPosition{.x = (0.5 * static_cast<double>(i)) - 0.5, .y = 1.0, .z = 0.0};

        ac3adm::AudioChannelFormat channel_format;
        channel_format.id = "chan" + key;
        channel_format.name = "Object " + key;
        channel_format.type = type;
        channel_format.block_formats.push_back(block);

        ac3adm::AudioPackFormat pack_format;
        pack_format.id = "pack" + key;
        pack_format.name = channel_format.name;
        pack_format.type = type;
        pack_format.channel_format_refs = {channel_format.id};

        ac3adm::AudioStreamFormat stream_format;
        stream_format.id = "stream" + key;
        stream_format.name = channel_format.name;
        stream_format.channel_format_ref = channel_format.id;

        ac3adm::AudioTrackFormat track_format;
        track_format.id = "track" + key;
        track_format.name = channel_format.name;
        track_format.stream_format_ref = stream_format.id;

        ac3adm::AudioTrackUid track_uid;
        track_uid.uid = "atu" + key;
        track_uid.has_sample_rate = true;
        track_uid.sample_rate = 48000;
        track_uid.has_bit_depth = declared_bit_depths[i].has_value();
        track_uid.bit_depth = declared_bit_depths[i].value_or(0U);
        track_uid.track_format_ref = track_format.id;
        track_uid.pack_format_ref = pack_format.id;

        ac3adm::AudioObject object;
        object.id = "obj" + key;
        object.name = channel_format.name;
        object.pack_format_refs = {pack_format.id};
        object.track_uid_refs = {track_uid.uid};
        content.object_refs.push_back(object.id);

        ac3adm::ChnaEntry chna_entry;
        chna_entry.track_index = static_cast<std::uint16_t>(i + 1);
        chna_entry.uid = track_uid.uid;
        document.chna.push_back(chna_entry);

        document.audio.channels.push_back({0.25F, -0.5F, 0.125F, 0.1F * static_cast<float>(i + 1)});

        model.channel_formats.push_back(std::move(channel_format));
        model.pack_formats.push_back(std::move(pack_format));
        model.stream_formats.push_back(std::move(stream_format));
        model.track_formats.push_back(std::move(track_format));
        model.track_uids.push_back(std::move(track_uid));
        model.objects.push_back(std::move(object));
    }
    model.contents.push_back(std::move(content));

    ac3adm::AudioProgramme programme;
    programme.id = "programme";
    programme.name = "Programme";
    programme.content_refs = {"content"};
    model.programmes.push_back(std::move(programme));
    return document;
}

// Where a write test puts its files: a directory of its own under AC3FORGE_TEST_SCRATCH_DIR, with
// this process's id folded in (tests/platform/process.hpp says why).
std::filesystem::path write_scratch_dir(std::string_view name) {
    auto dir = std::filesystem::path{AC3FORGE_TEST_SCRATCH_DIR} /
               (std::string(name) + "_" + ac3::test::platform::process_id());
    std::filesystem::create_directories(dir);
    return dir;
}

}  // namespace

TEST_CASE("parses a minimal RIFF/WAVE ADM file", "[adm]") {
    std::istringstream stream(minimal_fixture_bytes(false));
    auto doc = ac3adm::parse_bw64(stream);
    REQUIRE(doc.has_value());

    SECTION("PCM audio") {
        CHECK(doc->audio.sample_rate == 48000);
        CHECK(doc->audio.bits_per_sample == 16);
        REQUIRE(doc->audio.channels.size() == 1);
        REQUIRE(doc->audio.frame_count() == 4);
        // (frame * 4000 - 12000) / 32768, frame 0..3 - decoded independently
        // of ac3adm's own PCM-decoding arithmetic to actually check it.
        CHECK(doc->audio.channels[0][0] == Catch::Approx(-12000.0f / 32768.0f));
        CHECK(doc->audio.channels[0][3] == Catch::Approx(0.0f / 32768.0f));
    }

    SECTION("chna join table") {
        REQUIRE(doc->chna.size() == 1);
        CHECK(doc->chna[0].track_index == 1);
        CHECK(doc->chna[0].uid == "ATU_00000001");
        CHECK(doc->chna[0].track_ref == "AT_00031001_01");
        CHECK(doc->chna[0].pack_ref == "AP_00031001");
    }

    SECTION("ADM object graph") {
        REQUIRE(doc->model.programmes.size() == 1);
        CHECK(doc->model.programmes[0].id == "APR_1001");
        CHECK(doc->model.programmes[0].content_refs == std::vector<std::string>{"ACO_1001"});

        REQUIRE(doc->model.contents.size() == 1);
        CHECK(doc->model.contents[0].object_refs == std::vector<std::string>{"AO_1001"});

        REQUIRE(doc->model.objects.size() == 1);
        CHECK(doc->model.objects[0].id == "AO_1001");
        CHECK(doc->model.objects[0].pack_format_refs == std::vector<std::string>{"AP_00031001"});
        CHECK(doc->model.objects[0].track_uid_refs == std::vector<std::string>{"ATU_00000001"});

        const auto& pack_format = find_by_id(doc->model.pack_formats, "AP_00031001");
        CHECK(pack_format.type == ac3adm::TypeDefinition::kObjects);
        CHECK(pack_format.channel_format_refs == std::vector<std::string>{"AC_00031001"});

        const auto& channel = find_by_id(doc->model.channel_formats, "AC_00031001");
        CHECK(channel.type == ac3adm::TypeDefinition::kObjects);
        REQUIRE(channel.block_formats.size() == 1);
        const auto& block = channel.block_formats[0];
        CHECK(block.id == "AB_00031001_00000001");
        CHECK_FALSE(block.cartesian);
        REQUIRE(std::holds_alternative<ac3adm::PolarPosition>(block.position));
        const auto& polar = std::get<ac3adm::PolarPosition>(block.position);
        CHECK(polar.azimuth_deg == Catch::Approx(-22.5));
        CHECK(polar.elevation_deg == Catch::Approx(5.0));
        CHECK(polar.distance == Catch::Approx(1.0));
        CHECK(block.width == Catch::Approx(12.5));

        REQUIRE(doc->model.track_uids.size() == 1);
        CHECK(doc->model.track_uids[0].uid == "ATU_00000001");
        CHECK(doc->model.track_uids[0].has_sample_rate);
        CHECK(doc->model.track_uids[0].sample_rate == 48000);
        CHECK(doc->model.track_uids[0].has_bit_depth);
        CHECK(doc->model.track_uids[0].bit_depth == 16);
    }
}

TEST_CASE("parses the same content via RF64 with a ds64-resolved data chunk", "[adm]") {
    std::istringstream stream(minimal_fixture_bytes(true));
    auto doc = ac3adm::parse_bw64(stream);
    REQUIRE(doc.has_value());
    CHECK(doc->audio.sample_rate == 48000);
    REQUIRE(doc->audio.frame_count() == 4);
    REQUIRE(doc->model.objects.size() == 1);
    CHECK(doc->model.objects[0].id == "AO_1001");
}

// BS.2076-2 §5.9 makes both of audioTrackUID's sampleRate and bitDepth attributes optional, and
// masters without bitDepth are real: every one write_bw64() wrote before it began setting it (see
// the write tests at the end of this file) carries sampleRate alone. The reader reports whichever
// attributes are present and leaves has_* false for the rest rather than refusing the file.
TEST_CASE("an audioTrackUID without bitDepth still parses", "[adm]") {
    const auto removed = GENERATE(std::string_view{R"( bitDepth="16")"},
                                  std::string_view{R"( sampleRate="48000" bitDepth="16")"});
    CAPTURE(removed);
    std::string xml(kCarAdmXml);
    const auto at = xml.find(removed);
    REQUIRE(at != std::string::npos);
    xml.erase(at, removed.size());

    std::istringstream stream(
        build_riff(build_fmt_chunk(1, 48000, 16), build_chna_chunk(), Bytes(xml), build_pcm16_data(4)));
    const auto doc = ac3adm::parse_bw64(stream);
    REQUIRE(doc.has_value());
    REQUIRE(doc->model.track_uids.size() == 1);
    const auto& track_uid = doc->model.track_uids[0];
    CHECK(track_uid.uid == "ATU_00000001");
    CHECK_FALSE(track_uid.has_bit_depth);
    CHECK(track_uid.has_sample_rate == (removed.find("sampleRate") == std::string_view::npos));
    REQUIRE(track_uid.track_format_ref.has_value());
    CHECK(*track_uid.track_format_ref == "AT_00031001_01");
}

TEST_CASE("rejects a file that is not RIFF/RF64/BW64", "[adm]") {
    std::istringstream stream(std::string("NOPE") + std::string(8, '\0'));
    auto doc = ac3adm::parse_bw64(stream);
    REQUIRE_FALSE(doc.has_value());
    // libbw64 reports "not a recognized container" the same way it reports
    // "could not open" - a single std::runtime_error family with no
    // distinguishing exception type (see src/ac3adm/src/adm.cpp's own
    // comment on parse_bw64_path) - so this, too, surfaces as kCannotOpen
    // rather than the more specific kNotRiff.
    CHECK(doc.error() == ac3adm::AdmError::kCannotOpen);
}

TEST_CASE("parses a float32 (IEEE-float) fmt chunk through the ordinary libbw64 path", "[adm]") {
    // Confirmed by reading parser.hpp directly, not assumed: the pinned libbw64 accepts a bare
    // WAVE_FORMAT_IEEE_FLOAT formatTag the same way it accepts WAVE_FORMAT_PCM, and
    // Bw64Reader::read() decodes it via its own decodeFloatSamples - no ac3adm-side routing
    // around libbw64 for this case any more (see model.hpp's PcmAudio comment for what this
    // module used to do here and why it no longer needs to).
    const auto fmt = build_float_fmt_chunk(1, 48000, 32);
    const auto chna = build_chna_chunk();
    const Bytes axml(kCarAdmXml);
    const auto data = build_float32_data({-0.5F, 0.0F});
    std::istringstream stream(build_riff(fmt, chna, axml, data));
    auto doc = ac3adm::parse_bw64(stream);
    REQUIRE(doc.has_value());

    CHECK(doc->audio.sample_rate == 48000);
    CHECK(doc->audio.bits_per_sample == 32);
    REQUIRE(doc->audio.channels.size() == 1);
    REQUIRE(doc->audio.frame_count() == 2);
    // Read back bit-for-bit, not rescaled - checked against the exact bytes written, not just
    // that SOME value came back.
    CHECK(doc->audio.channels[0][0] == Catch::Approx(-0.5F));
    CHECK(doc->audio.channels[0][1] == Catch::Approx(0.0F));

    // Same <chna>/<axml> fixtures the integer-PCM tests use, so "the ADM metadata is unaffected
    // by which sample format the file carries" is checked, not just asserted.
    REQUIRE(doc->chna.size() == 1);
    CHECK(doc->chna[0].uid == "ATU_00000001");
    REQUIRE(doc->model.programmes.size() == 1);
    CHECK(doc->model.programmes[0].id == "APR_1001");
}

// The 64-bit-per-sample counterpart - a distinct branch in libbw64's own decodeFloatSamples, not
// exercised by the 32-bit case above, and not something the retired float_pcm_bw64.hpp ever
// covered with its own test either (it supported both widths, per model.hpp's own doc comment,
// but only the 32-bit one was ever actually run here). Needs patch_libbw64.cmake's second patch
// to reach that decode at all: FormatInfoChunk's constructor (chunks.hpp) otherwise refuses any
// 64-bit <fmt >, PCM or float, before decodeFloatSamples is ever called - found by this test
// itself failing (kCannotOpen) the first time it ran, against the pinned fork unpatched for this.
TEST_CASE("parses a float64 (double-precision) fmt chunk", "[adm]") {
    const auto fmt = build_float_fmt_chunk(1, 48000, 64);
    const auto data = build_float64_data({-0.25, 0.75});
    std::istringstream stream(build_riff(fmt, Bytes{}, Bytes{}, data));
    auto doc = ac3adm::parse_bw64(stream);
    REQUIRE(doc.has_value());

    CHECK(doc->audio.bits_per_sample == 64);
    REQUIRE(doc->audio.channels.size() == 1);
    REQUIRE(doc->audio.frame_count() == 2);
    CHECK(doc->audio.channels[0][0] == Catch::Approx(-0.25));
    CHECK(doc->audio.channels[0][1] == Catch::Approx(0.75));
}

TEST_CASE("malformed XML in axml surfaces as kMalformedXml", "[adm]") {
    const auto fmt = build_fmt_chunk(1, 48000, 16);
    const auto chna = build_chna_chunk();
    // A genuinely unterminated tag (no closing '>', truncated mid-attribute) - not just a
    // mismatched close tag. libadm's own XML layer (rapidxml, vendored as a private
    // dependency) turned out to be lenient about mismatched close tags - a
    // "<a><b></a>" style fixture parses "successfully" (in whatever shape rapidxml
    // produces for it) and only fails later, as an ADM-structure complaint
    // (AdmError::kMalformedAdm, see the next test and src/ac3adm/src/adm.cpp's own
    // comment on why). This fixture instead breaks XML tokenizing itself, which is
    // needed to actually reach kMalformedXml - confirmed empirically, not assumed.
    const Bytes axml = "<audioFormatExtended><audioObject audioObjectID=\"AO_1";
    const auto data = build_pcm16_data(2);
    std::istringstream stream(build_riff(fmt, chna, axml, data));
    auto doc = ac3adm::parse_bw64(stream);
    REQUIRE_FALSE(doc.has_value());
    CHECK(doc.error() == ac3adm::AdmError::kMalformedXml);
}

TEST_CASE("a missing required ADM attribute surfaces as kMalformedXml", "[adm]") {
    const auto fmt = build_fmt_chunk(1, 48000, 16);
    const auto chna = build_chna_chunk();
    // audioObjectID (Required=Yes, BS.2076-2 Table 24) is missing.
    const Bytes axml =
        R"(<audioFormatExtended version="ITU-R_BS.2076-2"><audioObject audioObjectName="Nameless"/></audioFormatExtended>)";
    const auto data = build_pcm16_data(2);
    std::istringstream stream(build_riff(fmt, chna, axml, data));
    auto doc = ac3adm::parse_bw64(stream);
    REQUIRE_FALSE(doc.has_value());
    // Not kMalformedAdm, despite "missing a required ADM attribute" sounding like the more
    // ADM-shaped complaint of the two: libadm's own mandatory-attribute check
    // (xml_parser_helper.hpp's parseAttribute()) throws a plain, untyped std::runtime_error
    // rather than one of its own adm::error:: types - confirmed by catching and printing the
    // real exception during development, not assumed from the enum's own naming. See
    // ac3adm::AdmError's own doc comment (ac3adm.hpp) and src/ac3adm/src/adm.cpp's
    // read_adm_model() for the full explanation.
    CHECK(doc.error() == ac3adm::AdmError::kMalformedXml);
}

TEST_CASE("a duplicate ADM element ID surfaces as kMalformedAdm", "[adm]") {
    const auto fmt = build_fmt_chunk(1, 48000, 16);
    const auto chna = build_chna_chunk();
    // Two audioProgramme elements sharing one ID - libadm's own id_assignment/duplicate-id
    // check (xml_parser.cpp) throws adm::error::XmlParsingDuplicateId, a real
    // adm::error::AdmException subclass, unlike the missing-attribute case just above - this is
    // what AdmError::kMalformedAdm is actually reachable for.
    const Bytes axml =
        R"(<audioFormatExtended version="ITU-R_BS.2076-2">
             <audioProgramme audioProgrammeID="APR_1001" audioProgrammeName="A"/>
             <audioProgramme audioProgrammeID="APR_1001" audioProgrammeName="B"/>
           </audioFormatExtended>)";
    const auto data = build_pcm16_data(2);
    std::istringstream stream(build_riff(fmt, chna, axml, data));
    auto doc = ac3adm::parse_bw64(stream);
    REQUIRE_FALSE(doc.has_value());
    CHECK(doc.error() == ac3adm::AdmError::kMalformedAdm);
}

// Test name deliberately ASCII-only (no "section-sign clause number" the way this file's other
// comments cite one) - ctest's own Windows test invocation mangles a non-ASCII TEST_CASE name
// when passing it as a Catch2 --name filter argument, turning it into a byte no test actually
// matches ("No test cases matched") and failing the ctest entry outright even though the test
// itself is fine - confirmed by hitting exactly that under `ctest -j`, not assumed.
TEST_CASE("chna rows with trackIndex 0 (unused placeholders, BS.2088-1 clause 8.2) pass through unfiltered",
          "[adm]") {
    // libbw64's own ChnaChunk::numTracks() recomputes the field from the *distinct* trackIndex
    // values actually present (chunks.hpp) rather than trusting the declared one, and
    // bw64::readFile rejects a mismatch - so numTracks here is 2 (the real index 1 plus the
    // placeholder 0), not 1, even though only one row is a real track.
    Bytes chna;
    put_u16le(chna, 2);  // numTracks (see above - counts the placeholder's 0 as distinct too)
    put_u16le(chna, 2);  // numUIDs (one real, one placeholder)
    put_u16le(chna, 1);
    put_fixed(chna, "ATU_00000001", 12);
    put_fixed(chna, "AT_00031001_01", 14);
    put_fixed(chna, "AP_00031001", 11);
    chna.push_back('\0');
    put_u16le(chna, 0);  // §8.2: trackIndex 0 = unused row
    put_fixed(chna, std::string(12, '\0'), 12);
    put_fixed(chna, std::string(14, '\0'), 14);
    put_fixed(chna, std::string(11, '\0'), 11);
    chna.push_back('\0');

    const auto fmt = build_fmt_chunk(1, 48000, 16);
    const Bytes axml(kCarAdmXml);
    const auto data = build_pcm16_data(2);
    std::istringstream stream(build_riff(fmt, chna, axml, data));
    auto doc = ac3adm::parse_bw64(stream);
    REQUIRE(doc.has_value());
    // ac3adm::ac3adm surfaces every row as-is (ChnaEntry's own comment in model.hpp: "callers
    // that want 'the entries for track N' filter by track_index themselves") - it does not drop
    // placeholder rows itself, so both come through here.
    REQUIRE(doc->chna.size() == 2);
    CHECK(doc->chna[0].track_index == 1);
    CHECK(doc->chna[0].uid == "ATU_00000001");
    CHECK(doc->chna[1].track_index == 0);
    // The placeholder row above is NUL-padded (BS.2088-1 §8.2: "null strings... N null
    // characters (ASCII value zero)"), not space-padded, matching what a real unused chna slot
    // actually looks like on disk - libbw64's own AudioId::uid()/trackRef()/packRef() return the
    // raw file bytes verbatim on the read path (no read-side padding normalization; the
    // space-memset in AudioId's constructor is a write-side default that a full-width read
    // value, like this one, always overwrites completely). These three checks are what actually
    // exercise trim_padding()'s NUL-trimming - without it, each field below comes back as its
    // full fixed width (12/14/11 characters) full of embedded '\0' bytes instead of empty,
    // silently contradicting ChnaEntry's own doc comment in model.hpp ("may be empty, §8.2").
    CHECK(doc->chna[1].uid.empty());
    CHECK(doc->chna[1].track_ref.empty());
    CHECK(doc->chna[1].pack_ref.empty());
}

TEST_CASE("a chna row's NUL-padded packRef trims to empty even when the rest of the row is real",
          "[adm]") {
    // BS.2088-1 §8.3.2's own worked example: packRef is legitimately NUL-padded/absent on a
    // populated, non-placeholder row too - not just on a wholly-unused trackIndex-0 slot - e.g.
    // when a track's audioStreamFormat references a pack directly rather than the chna row
    // itself naming one. This is a real trackIndex (not 0), a real uid and trackRef, but an
    // all-NUL packRef.
    Bytes chna;
    put_u16le(chna, 1);  // numTracks
    put_u16le(chna, 1);  // numUIDs
    put_u16le(chna, 1);
    put_fixed(chna, "ATU_00000001", 12);
    put_fixed(chna, "AT_00031001_01", 14);
    put_fixed(chna, std::string(11, '\0'), 11);  // packRef: not required here, NUL-padded
    chna.push_back('\0');

    const auto fmt = build_fmt_chunk(1, 48000, 16);
    const Bytes axml(kCarAdmXml);
    const auto data = build_pcm16_data(2);
    std::istringstream stream(build_riff(fmt, chna, axml, data));
    auto doc = ac3adm::parse_bw64(stream);
    REQUIRE(doc.has_value());
    REQUIRE(doc->chna.size() == 1);
    CHECK(doc->chna[0].track_index == 1);
    CHECK(doc->chna[0].uid == "ATU_00000001");
    CHECK(doc->chna[0].track_ref == "AT_00031001_01");
    CHECK(doc->chna[0].pack_ref.empty());
}

TEST_CASE("a file with no axml chunk still parses, with an empty ADM model", "[adm]") {
    const auto fmt = build_fmt_chunk(1, 48000, 16);
    const auto chna = build_chna_chunk();
    const auto data = build_pcm16_data(2);
    std::istringstream stream(build_riff(fmt, chna, Bytes{}, data));
    auto doc = ac3adm::parse_bw64(stream);
    REQUIRE(doc.has_value());
    CHECK(doc->model.programmes.empty());
    CHECK(doc->model.objects.empty());
    REQUIRE(doc->audio.frame_count() == 2);
}

// Found by fuzz/fuzz_adm_parse (signing-verify fuzz walk) in its first minute: bw64's
// numberOfFrames() is the <data> chunk's DECLARED size over the block
// alignment, so a sixty-byte file claiming four gigabytes of PCM made
// read_pcm allocate four gigabytes. The reproducer is committed as
// fuzz/regressions/fuzz_adm_parse/oversized-data-chunk-oom; this is the same
// shape as a unit test, and it fails (out of memory, or a bad_alloc) against
// the pre-fix read_pcm.
//
// 0x7FFFFF00 rather than the fuzzer's own 0xF7FFFF07: the exact value does not
// matter as long as it is far past the file, and a value with the top bit
// clear keeps this a plain oversized 32-bit ckSize rather than something a
// reader might read as an RF64 escape.
TEST_CASE("an oversized declared data chunk is bounded by the real file size", "[adm]") {
    const auto fmt = build_fmt_chunk(1, 48000, 16);
    const auto real_data = build_pcm16_data(4);

    Bytes body;
    append_chunk(body, "fmt ", fmt);
    append_chunk(body, "data", real_data, 0x7FFFFF00u);
    Bytes file;
    put_fourcc(file, "RIFF");
    put_u32le(file, static_cast<std::uint32_t>(4 + body.size()));
    put_fourcc(file, "WAVE");
    file += body;

    std::istringstream stream(file);
    auto doc = ac3adm::parse_bw64(stream);
    // Whether this parses or is refused is not the point - it must not try to
    // allocate the two gigabytes the <data> header asks for. What it does do
    // is read no more frames than the file could possibly hold.
    if (doc.has_value()) {
        CHECK(doc->audio.frame_count() <= file.size());
    }
}

// The second and third findings from fuzz/fuzz_adm_parse, both the same
// shape and both different from the <data> case above: libbw64 materialises
// every chunk it reads EXCEPT <data> into a std::vector sized straight from
// the chunk header, during readFile() itself, so a 99-byte file whose <axml>
// claims four gigabytes asked for four gigabytes before any ac3forge code
// ran. adm.cpp's chunk_sizes_fit() now refuses any chunk but <data> whose
// declared size runs past the end of the file; see its own comment for what
// that covers and what it deliberately does not.
//
// 0xFFFFFFFF is one of the two values the fuzzer actually reached, and is
// also RF64's <ds64> escape - so it is checked here explicitly alongside an
// ordinary oversized value, to pin down that the escape is only honoured on
// <data> (the "parses the same content via RF64" case above is the other
// half of that pair).
TEST_CASE("a non-data chunk declaring more than the file holds is refused", "[adm]") {
    const auto declared = GENERATE(std::uint32_t{0x7FFFFF00}, std::uint32_t{0xFFFFFFFF});
    CAPTURE(declared);

    Bytes body;
    append_chunk(body, "fmt ", build_fmt_chunk(1, 48000, 16));
    append_chunk(body, "axml", Bytes(kCarAdmXml), declared);
    append_chunk(body, "data", build_pcm16_data(4));
    Bytes file;
    put_fourcc(file, "RIFF");
    put_u32le(file, static_cast<std::uint32_t>(4 + body.size()));
    put_fourcc(file, "WAVE");
    file += body;

    std::istringstream stream(file);
    const auto doc = ac3adm::parse_bw64(stream);
    REQUIRE_FALSE(doc.has_value());
    CHECK(doc.error() == ac3adm::AdmError::kNotRiff);
}

// The other half of that check: a recording cut off part-way through <data>
// is an ordinary file, not an attack, and must still read. Two independent
// layers have to agree on that for this to hold: chunk_sizes_fit() exempts
// <data> from its own pre-scan (immediately above), AND libbw64's own
// internal chunk-table walk has to as well - which the pinned commit refuses
// outright unless patched (src/ac3adm/patch_libbw64.cmake's whole reason for
// existing; see its own comment for why upstream doesn't do this itself).
TEST_CASE("a file truncated inside its data chunk still parses", "[adm]") {
    Bytes file = minimal_fixture_bytes(false);
    file.resize(file.size() - 3);  // lose the tail of <data>, keep every header
    std::istringstream stream(file);
    const auto doc = ac3adm::parse_bw64(stream);
    REQUIRE(doc.has_value());
    // Not just "didn't error" - the frames actually present are the ones
    // that survive the truncation, confirming libbw64's own DataChunk size
    // was clamped (patch_libbw64.cmake) rather than the read merely not
    // crashing on a declared size it didn't honour.
    CHECK(doc->audio.frame_count() < 4);
}

// The exemption above is deliberately narrow: only <data> may run past the end of the file once
// libbw64 resolves its size. A plain oversized 32-bit header on any OTHER chunk is already
// covered by "a non-data chunk declaring more than the file holds is refused" above (caught by
// chunk_sizes_fit() before libbw64 is ever reached, so it says nothing about libbw64's own
// behaviour) - what that test can't reach is a <ds64> table entry giving some other chunk an
// oversized 64-bit size while its own 32-bit header stays honest, since chunk_sizes_fit()
// deliberately does not read the table's entries (see its own comment). That shape passes our
// pre-check untouched and reaches libbw64's real internal chunk walk, which is what this pins:
// the patched carve-out still throws for anything but <data>. This is the same shape
// fuzz_adm_parse's mutation reached (an uncommitted probe during development, not one of the
// regressions above) once instrumenting ac3adm_objects exposed the library's own chunk-table
// resolution rather than only chunk_sizes_fit()'s narrower one.
TEST_CASE("a ds64 table entry oversizing a non-data chunk is still refused", "[adm]") {
    const auto fmt = build_fmt_chunk(1, 48000, 16);
    const auto data = build_pcm16_data(4);
    const Bytes axml(kCarAdmXml);

    Bytes ds64_content;
    put_u64le(ds64_content, 0);            // bw64Size - unused by this check
    put_u64le(ds64_content, data.size());  // dataSize - honest
    put_u64le(ds64_content, 0);            // dummy, §4.2
    put_u32le(ds64_content, 1);            // tableLength: one entry
    put_fourcc(ds64_content, "axml");
    put_u64le(ds64_content, std::uint64_t{1} << 40);  // 1 TiB - <axml>'s real size is a few hundred bytes

    Bytes body;
    append_chunk(body, "ds64", ds64_content);
    append_chunk(body, "fmt ", fmt);
    append_chunk(body, "axml", axml);  // real, honest 32-bit header - only the ds64 table lies
    append_chunk(body, "data", data);

    Bytes file;
    put_fourcc(file, "RF64");
    put_u32le(file, 0xFFFFFFFFu);
    put_fourcc(file, "WAVE");
    file += body;

    std::istringstream stream(file);
    const auto doc = ac3adm::parse_bw64(stream);
    REQUIRE_FALSE(doc.has_value());
}

// libbw64 materialises any chunk id it has no class of its own for into an
// UnknownChunk, whose 0.10.0 constructor resizes a std::vector<char> to the
// declared size and then hands stream.read() `&data_[0]` - undefined behaviour
// when that size is zero, and the report that kept `ac3adm_objects` out of
// fuzz/CMakeLists.txt's instrumented set until src/ac3adm/patch_libbw64.cmake
// existed. A zero-length chunk is ordinary content: BS.2088-1 §4 puts no floor
// under a chunk's size, and an empty JUNK is a normal thing for a producer to
// leave behind. The same shape is committed as
// fuzz/regressions/fuzz_adm_parse/zero-length-unknown-chunk.
//
// An unpatched libbw64 fails this under UBSan ("reference binding to null
// pointer of type 'char'") and on any standard library with its bounds checks
// turned on (_GLIBCXX_ASSERTIONS, MSVC's debug iterators); an optimised
// libstdc++ build without them passes a null pointer and a zero length to
// istream::read and survives, which is why it went unseen for so long.
TEST_CASE("a zero-length chunk of an id libbw64 does not know still parses", "[adm]") {
    Bytes body;
    append_chunk(body, "fmt ", build_fmt_chunk(1, 48000, 16));
    append_chunk(body, "JUNK", Bytes{});
    append_chunk(body, "data", build_pcm16_data(4));
    Bytes file;
    put_fourcc(file, "RIFF");
    put_u32le(file, static_cast<std::uint32_t>(4 + body.size()));
    put_fourcc(file, "WAVE");
    file += body;

    std::istringstream stream(file);
    const auto doc = ac3adm::parse_bw64(stream);
    REQUIRE(doc.has_value());
    CHECK(doc->audio.frame_count() == 4);
}

// The same defect one layer down, and the reason the patch covers
// Bw64Reader::read() as well as the chunk constructor: an empty <data> chunk
// asks read_pcm for zero frames, and 0.10.0 takes `&rawDataBuffer_[0]` of the
// buffer it just resized to zero. A file carrying ADM metadata and no audio
// yet is a real shape - BS.2088-1 §9 makes <axml> optional, not <data>'s
// content - so this reads as an empty PcmAudio rather than an error.
TEST_CASE("a zero-length data chunk parses with no frames", "[adm]") {
    Bytes body;
    append_chunk(body, "fmt ", build_fmt_chunk(1, 48000, 16));
    append_chunk(body, "data", Bytes{});
    Bytes file;
    put_fourcc(file, "RIFF");
    put_u32le(file, static_cast<std::uint32_t>(4 + body.size()));
    put_fourcc(file, "WAVE");
    file += body;

    std::istringstream stream(file);
    const auto doc = ac3adm::parse_bw64(stream);
    REQUIRE(doc.has_value());
    CHECK(doc->audio.frame_count() == 0);
}

// Found while auditing libbw64 for the pattern above, and confirmed by running
// both shapes through fuzz_adm_parse against an instrumented ac3adm, against
// the pinned 0.10.0. nBlockAlign is a 16-bit field, and libbw64's own
// blockAlignment() returned a bare uint16_t, so a <fmt > whose channel count
// times its sample width ran past 65,535 wrapped BOTH the reader's and the
// file's own declared value to the same wrong number - which is why the
// "should be" check inside libbw64's own fmt parsing compared the two and
// passed. 32,768 channels at 16 bits wrapped to 0, and numberOfFrames()
// divided by it (a SIGFPE, on an uninstrumented build too). 32,769 wrapped to
// 2, which sized the read buffer at two bytes a frame while the decode loop
// read 65,538 of them - a heap overread the length of a whole frame, which an
// uninstrumented ac3adm ran as a clean execution and returned as audio.
// adm.cpp's own read_pcm() guard (still there, see its own comment) refused
// both by comparing libbw64's value against one this module computes itself
// in a width that cannot wrap.
//
// The pinned fork closes the same case one layer further in: its
// blockAlignment() is utils::safeCast<uint16_t>, which THROWS on overflow
// rather than wrapping - and that call happens inside parseFormatInfoChunk's
// own sanity check, before a Bw64Reader is ever constructed, so the whole
// open now fails with kCannotOpen and adm.cpp's guard never gets a chance to
// run. Confirmed by re-running this exact fixture after the re-pin: it used
// to read successfully with `channels` empty; now `parse_bw64` itself fails.
TEST_CASE("a fmt whose block alignment overflows 16 bits is refused outright", "[adm]") {
    const auto channels = GENERATE(std::uint16_t{32768}, std::uint16_t{32769});
    CAPTURE(channels);

    Bytes body;
    append_chunk(body, "fmt ", build_fmt_chunk(channels, 48000, 16));
    // 65,538 bytes - large enough that, on the OLD libbw64 this fixture was
    // written against, the wrapped-to-2 case would have computed a non-zero
    // frame count and reached the (now-refused) read.
    append_chunk(body, "data", build_pcm16_data(32769));
    Bytes file;
    put_fourcc(file, "RIFF");
    put_u32le(file, static_cast<std::uint32_t>(4 + body.size()));
    put_fourcc(file, "WAVE");
    file += body;

    std::istringstream stream(file);
    const auto doc = ac3adm::parse_bw64(stream);
    REQUIRE_FALSE(doc.has_value());
    CHECK(doc.error() == ac3adm::AdmError::kCannotOpen);
}

// Found by fuzz_adm_parse 265 seconds into its first full-budget instrumented run, and at the
// time, in this project's own code rather than the vendored reader's: the module used to detect
// a float master by walking the chunk table itself, ahead of libbw64, in its own
// find_chunk() (retired since - see model.hpp's PcmAudio comment for why libbw64 now reads float
// directly and that walk no longer exists). find_chunk() stepped over each chunk with
// `at += 8 + declared + (declared & 1)`, computed in the uint32_t that `declared` is - a chunk
// declaring 0xFFFFFFF7 carried that sum to exactly 2^32, which wraps to zero, so the walk sat on
// the same chunk and never ended. Every file reached that walk: the float-detection pass ran
// ahead of libbw64 on all of them, and chunk_sizes_fit() ahead of THAT allows an oversized
// <data> on purpose, for the truncated-recording case just above - this fixture carries no
// <fmt > chunk, which is what made the old find_chunk() walk past <data> instead of stopping at
// the chunk it was looking for. Kept as a regression against the code that replaced it: this
// exact fixture (and the one the fuzzer produced, past a mutated "fmp ",
// fuzz/regressions/fuzz_adm_parse/chunk-size-wraps-the-walk) now reaches libbw64's own reader
// instead, missing its mandatory <fmt > chunk, and should fail cleanly rather than hang either
// way.
TEST_CASE("a chunk size that once wrapped the retired float-detection walk still parses cleanly",
          "[adm]") {
    Bytes body;
    append_chunk(body, "data", Bytes{}, 0xFFFFFFF7u);
    Bytes file;
    put_fourcc(file, "RIFF");
    put_u32le(file, static_cast<std::uint32_t>(4 + body.size()));
    put_fourcc(file, "WAVE");
    file += body;

    std::istringstream stream(file);
    const auto doc = ac3adm::parse_bw64(stream);
    CHECK_FALSE(doc.has_value());
}

TEST_CASE("parses a DirectSpeakers channel's speakerLabel and polar position", "[adm][model]") {
    std::istringstream stream(wrap_axml_only(kDirectSpeakersAdmXml));
    auto doc = ac3adm::parse_bw64(stream);
    REQUIRE(doc.has_value());
    const auto& channel = find_by_id(doc->model.channel_formats, "AC_00019001");
    CHECK(channel.type == ac3adm::TypeDefinition::kDirectSpeakers);
    REQUIRE(channel.block_formats.size() == 1);
    const auto& block = channel.block_formats[0];
    REQUIRE(block.speaker_labels.size() == 1);
    CHECK(block.speaker_labels[0] == "M+030");
    REQUIRE(std::holds_alternative<ac3adm::PolarPosition>(block.position));
    CHECK(std::get<ac3adm::PolarPosition>(block.position).azimuth_deg == Catch::Approx(30.0));
}

TEST_CASE("parses HOA order/degree/normalization", "[adm][model]") {
    std::istringstream stream(wrap_axml_only(kHoaAdmXml));
    auto doc = ac3adm::parse_bw64(stream);
    REQUIRE(doc.has_value());
    const auto& channel = find_by_id(doc->model.channel_formats, "AC_00049001");
    CHECK(channel.type == ac3adm::TypeDefinition::kHoa);
    const auto& block = channel.block_formats.at(0);
    REQUIRE(block.has_hoa_order);
    CHECK(block.hoa_order == -1);
    REQUIRE(block.has_hoa_degree);
    CHECK(block.hoa_degree == 1);
    CHECK(block.hoa_normalization == "N3D");
}

TEST_CASE("parses Cartesian object positions and jumpPosition", "[adm][model]") {
    std::istringstream stream(wrap_axml_only(kCartesianObjectAdmXml));
    auto doc = ac3adm::parse_bw64(stream);
    REQUIRE(doc.has_value());
    const auto& channel = find_by_id(doc->model.channel_formats, "AC_00031002");
    const auto& block = channel.block_formats.at(0);
    CHECK(block.cartesian);
    REQUIRE(std::holds_alternative<ac3adm::CartesianPosition>(block.position));
    const auto& cart = std::get<ac3adm::CartesianPosition>(block.position);
    CHECK(cart.x == Catch::Approx(-0.2));
    CHECK(cart.y == Catch::Approx(0.5));
    REQUIRE(block.has_jump_position);
    CHECK(block.jump_position);
    REQUIRE(block.has_interpolation_length);
    CHECK(block.interpolation_length_s == Catch::Approx(0.05));
}

TEST_CASE("ADM sample-based time format matches the equivalent decimal form", "[adm][model]") {
    // BS.2076-2 §5.11's own example: "01:34:16.12000S48000 is the same as 01:34:16.25000".
    const auto fmt = build_fmt_chunk(1, 48000, 16);
    const auto data = build_pcm16_data(2);

    const Bytes axml_samples(
        R"(<audioFormatExtended version="ITU-R_BS.2076-2">
             <audioObject audioObjectID="AO_1001" audioObjectName="X" start="01:34:16.12000S48000"/>
           </audioFormatExtended>)");
    const Bytes axml_decimal(
        R"(<audioFormatExtended version="ITU-R_BS.2076-2">
             <audioObject audioObjectID="AO_1001" audioObjectName="X" start="01:34:16.25000"/>
           </audioFormatExtended>)");

    std::istringstream stream_samples(build_riff(fmt, Bytes{}, axml_samples, data));
    std::istringstream stream_decimal(build_riff(fmt, Bytes{}, axml_decimal, data));
    auto doc_samples = ac3adm::parse_bw64(stream_samples);
    auto doc_decimal = ac3adm::parse_bw64(stream_decimal);
    REQUIRE(doc_samples.has_value());
    REQUIRE(doc_decimal.has_value());
    REQUIRE(doc_samples->model.objects.size() == 1);
    REQUIRE(doc_decimal->model.objects.size() == 1);
    CHECK(doc_samples->model.objects[0].start_s == Catch::Approx(doc_decimal->model.objects[0].start_s));
}

TEST_CASE("describe() returns a non-empty string for every AdmError", "[adm]") {
    using ac3adm::AdmError;
    for (const auto error : {AdmError::kCannotOpen, AdmError::kNotRiff, AdmError::kMalformedXml,
                              AdmError::kMalformedAdm, AdmError::kOther}) {
        CHECK_FALSE(ac3adm::describe(error).empty());
    }
}

// The Dolby Atmos Master ADM Profile expects every audioTrackUID to state the bit depth its track
// is stored at, and Dolby Encoding Engine 6.5.4 refuses a master whose audioTrackUIDs leave it out
// ("Mismatched track bit depth between ADM and WAV") - which every master write_bw64() wrote did,
// with sampleRate alone, until this test was added. One track per way a caller's model can carry
// the field - absent, stale (describing some other file), already right - and each has to come
// out as the <fmt > chunk's own width. Three tracks rather than two: a two-channel round trip has
// already hidden one libadm id problem in this writer (see objects_document()).
TEST_CASE("write_bw64 gives every audioTrackUID the bit depth of the fmt chunk", "[adm][write]") {
    const auto document = objects_document({std::nullopt, 16U, 24U});

    const auto dir = std::filesystem::path{AC3FORGE_TEST_SCRATCH_DIR} /
                     ("adm_write_" + ac3::test::platform::process_id());
    std::filesystem::create_directories(dir);
    const auto path = (dir / "track_uid_bit_depth.wav").string();
    const auto written = ac3adm::write_bw64(path, document);
    REQUIRE(written.has_value());

    // The raw bytes first: the same two libraries write and read this file, so a parse_bw64()
    // round trip on its own could agree with a mistake both of them make.
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in);
    const std::string file{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};

    const auto fmt = find_chunk(file, "fmt ");
    REQUIRE(fmt.has_value());
    REQUIRE(fmt->size() >= 16);
    // build_fmt_chunk()'s layout: formatTag, channels, sampleRate, bytes per second, block
    // alignment, then bits per sample at byte 14.
    const auto bits_per_sample = get_u16le(*fmt, 14);
    CHECK(bits_per_sample == ac3adm::kWriteBitDepth);

    const auto axml = find_chunk(file, "axml");
    REQUIRE(axml.has_value());
    const auto tags = track_uid_start_tags(*axml);
    REQUIRE(tags.size() == 3);
    const std::string expected = "bitDepth=\"" + std::to_string(bits_per_sample) + "\"";
    for (const auto tag : tags) {
        CAPTURE(tag);
        CHECK(tag.find(expected) != std::string_view::npos);
    }

    // Then through the reader: every audioTrackUID reports it, equal to the width the audio
    // itself was read at, and sampleRate still comes from the model.
    const auto parsed = ac3adm::parse_bw64(path);
    REQUIRE(parsed.has_value());
    CHECK(parsed->audio.bits_per_sample == bits_per_sample);
    REQUIRE(parsed->model.track_uids.size() == 3);
    for (const auto& track_uid : parsed->model.track_uids) {
        CAPTURE(track_uid.uid);
        CHECK(track_uid.has_bit_depth);
        CHECK(track_uid.bit_depth == parsed->audio.bits_per_sample);
        CHECK(track_uid.has_sample_rate);
        CHECK(track_uid.sample_rate == 48000);
    }
}

// AdmWriteError::kInvalidDocument's doc comment (ac3adm.hpp) names a block whose position is polar,
// but the translator behind write_bw64() read every block's position with an unchecked
// std::get<CartesianPosition>, so a polar block threw std::bad_variant_access out of a function
// that returns std::expected - for both typeDefinitions the writer supports. A default-constructed
// AudioBlockFormat is such a block: its position starts as PolarPosition{}. The channel is given a
// second block and that is the one changed, so a check of each channel's first block alone would
// not pass.
TEST_CASE("write_bw64 reports a polar block as kInvalidDocument without throwing", "[adm][write]") {
    const auto type =
        GENERATE(ac3adm::TypeDefinition::kObjects, ac3adm::TypeDefinition::kDirectSpeakers);
    INFO("typeDefinition " << (type == ac3adm::TypeDefinition::kObjects ? "Objects"
                                                                        : "DirectSpeakers"));
    const auto dir = write_scratch_dir("adm_write_polar");
    auto document = objects_document({std::nullopt}, type);
    auto& blocks = document.model.channel_formats.front().block_formats;
    auto second = blocks.front();
    second.rtime_s = 0.5;
    blocks.push_back(second);

    // Unchanged, the document writes, so what is refused below is refused for the block alone.
    REQUIRE(ac3adm::write_bw64((dir / "cartesian.wav").string(), document).has_value());

    auto& block = blocks.back();
    SECTION("a polar position") {
        block.cartesian = false;
        block.position =
            ac3adm::PolarPosition{.azimuth_deg = 30.0, .elevation_deg = 0.0, .distance = 1.0};
    }
    SECTION("a default-constructed block") {
        block = ac3adm::AudioBlockFormat{};
    }

    const auto path = dir / "polar.wav";
    std::filesystem::remove(path);
    const auto written = ac3adm::write_bw64(path.string(), document);
    REQUIRE_FALSE(written.has_value());
    CHECK(written.error() == ac3adm::AdmWriteError::kInvalidDocument);
    CHECK_FALSE(std::filesystem::exists(path));
}

// What write_bw64() does with the libadm document once it is built - reassignIds(), formatting
// the <chna> rows' IDs, writeXml() - ran outside any try block, so an exception from any of it
// left write_bw64() the same way. adm::formatId() throws for a document the translator accepts:
// it writes an audioTrackFormat's counter as two hex digits, and reassignIds() numbers the
// audioTrackFormats on one audioStreamFormat from 01, so the 256th is 0x100, which does not fit.
// With the <chna> row's audioTrackUID naming that track, the throw comes while the rows are
// resolved; naming the first track, it comes from writeXml() instead.
TEST_CASE("write_bw64 reports a libadm failure after the build as kOther without throwing",
          "[adm][write]") {
    const auto uid_names_last_track = GENERATE(false, true);
    CAPTURE(uid_names_last_track);
    auto document = objects_document({std::nullopt});
    auto& model = document.model;
    for (int i = 1; i < 256; ++i) {
        ac3adm::AudioTrackFormat track_format;
        track_format.id = "extra_track" + std::to_string(i);
        track_format.name = "Track";
        track_format.stream_format_ref = model.stream_formats.front().id;
        model.track_formats.push_back(std::move(track_format));
    }
    REQUIRE(model.track_formats.size() == 256);
    if (uid_names_last_track) {
        model.track_uids.front().track_format_ref = model.track_formats.back().id;
    }

    const auto path = write_scratch_dir("adm_write_id_overflow") / "overflow.wav";
    std::filesystem::remove(path);
    const auto written = ac3adm::write_bw64(path.string(), document);
    REQUIRE_FALSE(written.has_value());
    CHECK(written.error() == ac3adm::AdmWriteError::kOther);
    CHECK_FALSE(std::filesystem::exists(path));
}

// An audioTrackUID refers to an audioTrackFormat or, for plain PCM, straight to an
// audioChannelFormat, never both, and libadm's AudioTrackUid::setReference() throws
// adm::error::AudioTrackUidMutuallyExclusiveReferences when given the second. That left
// write_bw64() the same way a polar block's std::bad_variant_access did.
TEST_CASE("write_bw64 reports an audioTrackUID naming a track and a channel format as "
          "kInvalidDocument",
          "[adm][write]") {
    auto document = objects_document({std::nullopt});
    auto& track_uid = document.model.track_uids.front();
    REQUIRE(track_uid.track_format_ref.has_value());
    track_uid.channel_format_ref = document.model.channel_formats.front().id;

    const auto path = write_scratch_dir("adm_write_track_uid_refs") / "both_refs.wav";
    std::filesystem::remove(path);
    const auto written = ac3adm::write_bw64(path.string(), document);
    REQUIRE_FALSE(written.has_value());
    CHECK(written.error() == ac3adm::AdmWriteError::kInvalidDocument);
    CHECK_FALSE(std::filesystem::exists(path));
}
