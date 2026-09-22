#include "adm_model.hpp"

#include <chrono>
#include <expected>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>

#include <adm/adm.hpp>
#include <boost/variant.hpp>

// Clause references below are to Recommendation ITU-R BS.2076-2 (10/2019)
// Annex 1 unless stated otherwise. Every `adm::` symbol in this file is
// libadm's own (github.com/ebu/libadm); every `ac3adm::` symbol - including
// the unqualified ones, since this file lives inside namespace ac3adm::detail
// - is this module's own. See adm_model.hpp's header comment for why the two
// must never mix unqualified.

namespace ac3adm::detail {

namespace {

double to_seconds(const adm::Time& time) {
    return std::chrono::duration<double>(time.asNanoseconds()).count();
}

// libadm declares several coordinate/dimension NamedTypes as NamedType<float, ...> even though
// this module's own equivalents (ac3adm/model.hpp) are double - no precision reason, just how
// libadm declared them. Every plain float->double widening below goes through this helper to
// make the promotion explicit rather than implicit: -Wdouble-promotion is real on GCC/Clang
// (never caught by MSVC - see CONTRIBUTING.md's own multi-compiler verification note) and
// Clang's variant of it - unlike GCC's, which only fires when a float and a double literal have
// to unify inside a ternary - flags every single one of these plain assignments too, confirmed
// directly by building this exact file on both.
double to_double(float value) {
    return static_cast<double>(value);
}

// Table 7/10/20/53's five typeDefinition values, plus UNDEFINED and the
// 0x1000-0xFFFF "User Custom" range libadm folds into neither - see
// adm::TypeDescriptor's own doc comment ("valid values are in the range
// [0, 5]"): libadm does not model typeLabel values above 5 as a distinct
// concept, so any TypeDescriptor this project doesn't recognize (there are
// none beyond UNDEFINED/0..5) falls back to kUserCustom.
TypeDefinition to_type_definition(const adm::TypeDescriptor& type_descriptor) {
    if (type_descriptor == adm::TypeDefinition::DIRECT_SPEAKERS) return TypeDefinition::kDirectSpeakers;
    if (type_descriptor == adm::TypeDefinition::MATRIX) return TypeDefinition::kMatrix;
    if (type_descriptor == adm::TypeDefinition::OBJECTS) return TypeDefinition::kObjects;
    if (type_descriptor == adm::TypeDefinition::HOA) return TypeDefinition::kHoa;
    if (type_descriptor == adm::TypeDefinition::BINAURAL) return TypeDefinition::kBinaural;
    if (type_descriptor == adm::TypeDefinition::UNDEFINED) return TypeDefinition::kUnknown;
    return TypeDefinition::kUserCustom;
}

// One overload per ADM element type - each type's own `id_type` typedef
// names a distinct libadm class (AudioObjectId, AudioContentId, ...), so a
// single function template keyed on the element type covers every case via
// ordinary overload resolution rather than needing a trait per element.
std::string id_of(const std::shared_ptr<const adm::AudioProgramme>& e) {
    return adm::formatId(e->get<adm::AudioProgrammeId>());
}
std::string id_of(const std::shared_ptr<const adm::AudioContent>& e) {
    return adm::formatId(e->get<adm::AudioContentId>());
}
std::string id_of(const std::shared_ptr<const adm::AudioObject>& e) {
    return adm::formatId(e->get<adm::AudioObjectId>());
}
std::string id_of(const std::shared_ptr<const adm::AudioPackFormat>& e) {
    return adm::formatId(e->get<adm::AudioPackFormatId>());
}
std::string id_of(const std::shared_ptr<const adm::AudioChannelFormat>& e) {
    return adm::formatId(e->get<adm::AudioChannelFormatId>());
}
std::string id_of(const std::shared_ptr<const adm::AudioStreamFormat>& e) {
    return adm::formatId(e->get<adm::AudioStreamFormatId>());
}
std::string id_of(const std::shared_ptr<const adm::AudioTrackFormat>& e) {
    return adm::formatId(e->get<adm::AudioTrackFormatId>());
}
std::string id_of(const std::shared_ptr<const adm::AudioTrackUid>& e) {
    return adm::formatId(e->get<adm::AudioTrackUidId>());
}

template <typename Element>
std::optional<std::string> id_of_opt(const std::shared_ptr<const Element>& e) {
    if (!e) {
        return std::nullopt;
    }
    return id_of(e);
}

template <typename Range>
std::vector<std::string> ids_of(const Range& range) {
    std::vector<std::string> ids;
    for (const auto& element : range) {
        ids.push_back(id_of(element));
    }
    return ids;
}

// §5.4.3.1/§5.4.3.3, Tables 12/15/16: DirectSpeakers and Objects blocks both
// carry a position, but as different libadm types (SpeakerPosition vs.
// Position) with different sub-getter names - this pulls the common
// azimuth/elevation/distance / X/Y/Z shape out of whichever one a given
// block actually has.
template <typename Spherical, typename Cartesian, typename BlockFormat>
void set_position(const BlockFormat& block, AudioBlockFormat& out) {
    if (block.template has<Cartesian>()) {
        const auto cartesian = block.template get<Cartesian>();
        out.cartesian = true;
        out.position = CartesianPosition{
            .x = to_double(cartesian.template get<adm::X>().get()),
            .y = to_double(cartesian.template get<adm::Y>().get()),
            .z = cartesian.template has<adm::Z>() ? to_double(cartesian.template get<adm::Z>().get()) : 0.0,
        };
    } else if (block.template has<Spherical>()) {
        const auto spherical = block.template get<Spherical>();
        out.cartesian = false;
        out.position = PolarPosition{
            .azimuth_deg = to_double(spherical.template get<adm::Azimuth>().get()),
            .elevation_deg = to_double(spherical.template get<adm::Elevation>().get()),
            .distance = spherical.template has<adm::Distance>()
                            ? to_double(spherical.template get<adm::Distance>().get())
                            : 1.0,
        };
    }
}

// §5.4.3.3, Table 17: Objects' `position` (unlike DirectSpeakers') is
// exposed by libadm as a single `adm::Position` boost::variant, so this
// takes the more direct isCartesian()/boost::get() path instead of
// set_position()'s has<Cartesian>()/has<Spherical>() probing.
void set_objects_position(const adm::AudioBlockFormatObjects& block, AudioBlockFormat& out) {
    if (!block.has<adm::Position>()) {
        return;
    }
    const adm::Position position = block.get<adm::Position>();
    if (adm::isCartesian(position)) {
        const auto& cartesian = boost::get<adm::CartesianPosition>(position);
        out.cartesian = true;
        out.position = CartesianPosition{
            .x = to_double(cartesian.get<adm::X>().get()),
            .y = to_double(cartesian.get<adm::Y>().get()),
            .z = cartesian.has<adm::Z>() ? to_double(cartesian.get<adm::Z>().get()) : 0.0,
        };
    } else {
        const auto& spherical = boost::get<adm::SphericalPosition>(position);
        out.cartesian = false;
        out.position = PolarPosition{
            .azimuth_deg = to_double(spherical.get<adm::Azimuth>().get()),
            .elevation_deg = to_double(spherical.get<adm::Elevation>().get()),
            .distance = spherical.has<adm::Distance>() ? to_double(spherical.get<adm::Distance>().get()) : 1.0,
        };
    }
}

AudioBlockFormat convert_common(const std::string& id, const adm::Rtime& rtime,
                                 const boost::optional<adm::Duration>& duration, const adm::Gain& gain,
                                 const adm::Importance& importance) {
    AudioBlockFormat block;
    block.id = id;
    block.rtime_s = to_seconds(rtime.get());
    if (duration) {
        block.has_duration = true;
        block.duration_s = to_seconds(duration->get());
    }
    block.gain = gain.asLinear();
    block.has_importance = true;
    block.importance = importance.get();
    return block;
}

AudioBlockFormat convert(const adm::AudioBlockFormatDirectSpeakers& src) {
    auto block = convert_common(adm::formatId(src.get<adm::AudioBlockFormatId>()), src.get<adm::Rtime>(),
                                 src.has<adm::Duration>() ? boost::optional<adm::Duration>(src.get<adm::Duration>())
                                                           : boost::none,
                                 src.get<adm::Gain>(), src.get<adm::Importance>());
    set_position<adm::SphericalSpeakerPosition, adm::CartesianSpeakerPosition>(src, block);
    for (const auto& label : src.has<adm::SpeakerLabels>() ? src.get<adm::SpeakerLabels>() : adm::SpeakerLabels{}) {
        block.speaker_labels.push_back(label.get());
    }
    return block;
}

AudioBlockFormat convert(const adm::AudioBlockFormatObjects& src) {
    auto block = convert_common(adm::formatId(src.get<adm::AudioBlockFormatId>()), src.get<adm::Rtime>(),
                                 src.has<adm::Duration>() ? boost::optional<adm::Duration>(src.get<adm::Duration>())
                                                           : boost::none,
                                 src.get<adm::Gain>(), src.get<adm::Importance>());
    set_objects_position(src, block);
    block.width = to_double(src.get<adm::Width>().get());
    block.height = to_double(src.get<adm::Height>().get());
    block.depth = to_double(src.get<adm::Depth>().get());
    block.diffuse = to_double(src.get<adm::Diffuse>().get());
    if (src.has<adm::ChannelLock>()) {
        const auto channel_lock = src.get<adm::ChannelLock>();
        block.has_channel_lock = true;
        block.channel_lock = channel_lock.get<adm::ChannelLockFlag>().get();
        if (channel_lock.has<adm::MaxDistance>()) {
            block.has_channel_lock_max_distance = true;
            block.channel_lock_max_distance = to_double(channel_lock.get<adm::MaxDistance>().get());
        }
    }
    if (src.has<adm::JumpPosition>()) {
        const auto jump_position = src.get<adm::JumpPosition>();
        block.has_jump_position = true;
        block.jump_position = jump_position.get<adm::JumpPositionFlag>().get();
        if (jump_position.has<adm::InterpolationLength>()) {
            block.has_interpolation_length = true;
            block.interpolation_length_s =
                std::chrono::duration<double>(jump_position.get<adm::InterpolationLength>().get()).count();
        }
    }
    return block;
}

AudioBlockFormat convert(const adm::AudioBlockFormatHoa& src) {
    auto block = convert_common(adm::formatId(src.get<adm::AudioBlockFormatId>()), src.get<adm::Rtime>(),
                                 src.has<adm::Duration>() ? boost::optional<adm::Duration>(src.get<adm::Duration>())
                                                           : boost::none,
                                 src.get<adm::Gain>(), src.get<adm::Importance>());
    // §5.4.3.4: order/degree are Required for HOA blocks - libadm's parser
    // already enforces that, so has_hoa_order/has_hoa_degree are set
    // unconditionally rather than guarded by has<>() the way the genuinely
    // optional fields above are.
    block.has_hoa_order = true;
    block.hoa_order = src.get<adm::Order>().get();
    block.has_hoa_degree = true;
    block.hoa_degree = src.get<adm::Degree>().get();
    block.hoa_normalization = src.get<adm::Normalization>().get();
    return block;
}

// Matrix and Binaural blocks (§5.4.3.2, §5.4.3.5) contribute only the common
// id/rtime/duration/gain/importance fields set by convert_common() -
// Matrix's own coefficient-matrix content and Binaural's near-absence of
// content are both outside this phase's scope, per model.hpp's own header
// comment.
AudioBlockFormat convert(const adm::AudioBlockFormatMatrix& src) {
    return convert_common(adm::formatId(src.get<adm::AudioBlockFormatId>()), src.get<adm::Rtime>(),
                           src.has<adm::Duration>() ? boost::optional<adm::Duration>(src.get<adm::Duration>())
                                                     : boost::none,
                           src.get<adm::Gain>(), src.get<adm::Importance>());
}

AudioBlockFormat convert(const adm::AudioBlockFormatBinaural& src) {
    return convert_common(adm::formatId(src.get<adm::AudioBlockFormatId>()), src.get<adm::Rtime>(),
                           src.has<adm::Duration>() ? boost::optional<adm::Duration>(src.get<adm::Duration>())
                                                     : boost::none,
                           src.get<adm::Gain>(), src.get<adm::Importance>());
}

AudioChannelFormat convert(const std::shared_ptr<const adm::AudioChannelFormat>& src) {
    AudioChannelFormat channel_format;
    channel_format.id = id_of(src);
    channel_format.name = src->get<adm::AudioChannelFormatName>().get();
    const adm::TypeDescriptor type_descriptor = src->get<adm::TypeDescriptor>();
    channel_format.type = to_type_definition(type_descriptor);

    // §5.3.2: exactly one of these five ranges is non-empty, matching
    // channel_format.type - libadm stores each typeDefinition's blocks in
    // its own internal vector (see AudioChannelFormat::getElements<T>()).
    for (const auto& block : src->getElements<adm::AudioBlockFormatDirectSpeakers>()) {
        channel_format.block_formats.push_back(convert(block));
    }
    for (const auto& block : src->getElements<adm::AudioBlockFormatObjects>()) {
        channel_format.block_formats.push_back(convert(block));
    }
    for (const auto& block : src->getElements<adm::AudioBlockFormatHoa>()) {
        channel_format.block_formats.push_back(convert(block));
    }
    for (const auto& block : src->getElements<adm::AudioBlockFormatMatrix>()) {
        channel_format.block_formats.push_back(convert(block));
    }
    for (const auto& block : src->getElements<adm::AudioBlockFormatBinaural>()) {
        channel_format.block_formats.push_back(convert(block));
    }
    return channel_format;
}

AudioPackFormat convert(const std::shared_ptr<const adm::AudioPackFormat>& src) {
    AudioPackFormat pack_format;
    pack_format.id = id_of(src);
    pack_format.name = src->get<adm::AudioPackFormatName>().get();
    pack_format.type = to_type_definition(src->get<adm::TypeDescriptor>());
    pack_format.channel_format_refs = ids_of(src->getReferences<adm::AudioChannelFormat>());
    pack_format.pack_format_refs = ids_of(src->getReferences<adm::AudioPackFormat>());
    return pack_format;
}

AudioStreamFormat convert(const std::shared_ptr<const adm::AudioStreamFormat>& src) {
    AudioStreamFormat stream_format;
    stream_format.id = id_of(src);
    stream_format.name = src->get<adm::AudioStreamFormatName>().get();
    stream_format.channel_format_ref = id_of_opt(src->getReference<adm::AudioChannelFormat>());
    stream_format.pack_format_ref = id_of_opt(src->getReference<adm::AudioPackFormat>());
    // §5.2's audioTrackFormatIDRef is 0..* but AudioTrackFormat is held via
    // weak_ptr on this side of the (cyclic) reference - see
    // AudioStreamFormat::getAudioTrackFormatReferences()'s own doc comment.
    for (const auto& weak_track : src->getAudioTrackFormatReferences()) {
        if (const auto track = weak_track.lock()) {
            stream_format.track_format_refs.push_back(id_of(track));
        }
    }
    return stream_format;
}

AudioTrackFormat convert(const std::shared_ptr<const adm::AudioTrackFormat>& src) {
    AudioTrackFormat track_format;
    track_format.id = id_of(src);
    track_format.name = src->get<adm::AudioTrackFormatName>().get();
    track_format.stream_format_ref = id_of_opt(src->getReference<adm::AudioStreamFormat>());
    return track_format;
}

AudioTrackUid convert(const std::shared_ptr<const adm::AudioTrackUid>& src) {
    AudioTrackUid track_uid;
    track_uid.uid = id_of(src);
    if (src->has<adm::SampleRate>()) {
        track_uid.has_sample_rate = true;
        track_uid.sample_rate = src->get<adm::SampleRate>().get();
    }
    if (src->has<adm::BitDepth>()) {
        track_uid.has_bit_depth = true;
        track_uid.bit_depth = src->get<adm::BitDepth>().get();
    }
    track_uid.track_format_ref = id_of_opt(src->getReference<adm::AudioTrackFormat>());
    track_uid.channel_format_ref = id_of_opt(src->getReference<adm::AudioChannelFormat>());
    track_uid.pack_format_ref = id_of_opt(src->getReference<adm::AudioPackFormat>());
    return track_uid;
}

AudioObject convert(const std::shared_ptr<const adm::AudioObject>& src) {
    AudioObject object;
    object.id = id_of(src);
    object.name = src->get<adm::AudioObjectName>().get();
    object.start_s = to_seconds(src->get<adm::Start>().get());
    if (src->has<adm::Duration>()) {
        object.has_duration = true;
        object.duration_s = to_seconds(src->get<adm::Duration>().get());
    }
    object.pack_format_refs = ids_of(src->getReferences<adm::AudioPackFormat>());
    object.track_uid_refs = ids_of(src->getReferences<adm::AudioTrackUid>());
    object.object_refs = ids_of(src->getReferences<adm::AudioObject>());
    return object;
}

AudioContent convert(const std::shared_ptr<const adm::AudioContent>& src) {
    AudioContent content;
    content.id = id_of(src);
    content.name = src->get<adm::AudioContentName>().get();
    content.object_refs = ids_of(src->getReferences<adm::AudioObject>());
    return content;
}

AudioProgramme convert(const std::shared_ptr<const adm::AudioProgramme>& src) {
    AudioProgramme programme;
    programme.id = id_of(src);
    programme.name = src->get<adm::AudioProgrammeName>().get();
    programme.content_refs = ids_of(src->getReferences<adm::AudioContent>());
    return programme;
}

}  // namespace

namespace {

// The write-side counterpart of to_seconds() above: an ac3adm::AudioBlockFormat's *_s fields are
// plain seconds (model.hpp's own convention, chosen so nothing downstream of ac3adm has to know
// libadm's Time/FractionalTime split exists), so every rtime/duration/interpolationLength this
// writer emits goes through this one conversion rather than five ad-hoc ones.
adm::Time seconds_to_time(double seconds) {
    return adm::Time(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double>(seconds)));
}

adm::AudioBlockFormatObjects to_libadm_block(const AudioBlockFormat& block) {
    // The Dolby Atmos Master ADM Profile - and this writer's only caller, ac3::admbridge's
    // write-side (bridge.cpp) - always produces cartesian blocks; a caller handing this writer a
    // polar one is a bug in that caller, not a file this function was designed to accept (see
    // ac3adm.hpp's own AdmWriteError::kInvalidDocument doc comment).
    const auto& cartesian = std::get<CartesianPosition>(block.position);
    adm::AudioBlockFormatObjects out{
        adm::CartesianPosition(adm::X(static_cast<float>(cartesian.x)), adm::Y(static_cast<float>(cartesian.y)),
                               adm::Z(static_cast<float>(cartesian.z))),
        adm::Rtime(seconds_to_time(block.rtime_s)),
        adm::Gain::fromLinear(block.gain),
        adm::Width(static_cast<float>(block.width)),
        adm::Height(static_cast<float>(block.height)),
        adm::Depth(static_cast<float>(block.depth)),
    };
    if (block.has_duration) {
        out.set(adm::Duration(seconds_to_time(block.duration_s)));
    }
    if (block.has_channel_lock) {
        out.set(adm::ChannelLock(adm::ChannelLockFlag(block.channel_lock)));
    }
    if (block.has_jump_position) {
        adm::JumpPosition jump{adm::JumpPositionFlag(block.jump_position)};
        if (block.has_interpolation_length) {
            jump.set(adm::InterpolationLength(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double>(block.interpolation_length_s))));
        }
        out.set(jump);
    }
    return out;
}

adm::AudioBlockFormatDirectSpeakers to_libadm_direct_speakers_block(const AudioBlockFormat& block) {
    adm::AudioBlockFormatDirectSpeakers out{adm::Rtime(seconds_to_time(block.rtime_s)), adm::Gain::fromLinear(block.gain)};
    if (block.has_duration) {
        out.set(adm::Duration(seconds_to_time(block.duration_s)));
    }
    // set(), not the constructor's own named-arg list: SpeakerPosition's two alternatives
    // (Cartesian/Spherical) are read off the same `position`/`cartesian` fields
    // AudioBlockFormatObjects above reads, but AudioBlockFormatDirectSpeakers has no matching
    // constructor overload for either - see audio_block_format_direct_speakers.hpp's own
    // set(CartesianSpeakerPosition)/set(SphericalSpeakerPosition).
    const auto& cartesian = std::get<CartesianPosition>(block.position);
    out.set(adm::CartesianSpeakerPosition(adm::X(static_cast<float>(cartesian.x)), adm::Y(static_cast<float>(cartesian.y)),
                                          adm::Z(static_cast<float>(cartesian.z))));
    for (const auto& label : block.speaker_labels) {
        out.add(adm::SpeakerLabel(label));
    }
    return out;
}

// A small "resolve or fail" helper shared by every *_refs loop below: every reference in an
// AdmModel this writer accepts must resolve within the SAME model (see this file's own
// build_libadm_document doc comment - unlike the read side, there is no partial/best-effort
// tolerance here, since the caller building the model controls every string in it).
template <typename Value>
std::expected<std::reference_wrapper<const std::shared_ptr<Value>>, AdmWriteError> resolve(
    const std::unordered_map<std::string, std::shared_ptr<Value>>& by_id, const std::string& id) {
    const auto it = by_id.find(id);
    if (it == by_id.end()) {
        return std::unexpected(AdmWriteError::kInvalidDocument);
    }
    return std::cref(it->second);
}

}  // namespace

std::expected<BuiltDocument, AdmWriteError> build_libadm_document(const AdmModel& model) {
    auto document = ::adm::Document::create();

    std::unordered_map<std::string, std::shared_ptr<::adm::AudioChannelFormat>> channel_formats_by_id;
    for (const auto& channel_format : model.channel_formats) {
        ::adm::TypeDescriptor type;
        if (channel_format.type == TypeDefinition::kObjects) {
            type = ::adm::TypeDefinition::OBJECTS;
        } else if (channel_format.type == TypeDefinition::kDirectSpeakers) {
            type = ::adm::TypeDefinition::DIRECT_SPEAKERS;
        } else {
            // Matrix/HOA/Binaural/User Custom/Unknown - out of this writer's scope, same
            // boundary ac3::admbridge's own read-side classify_object() draws (bridge.cpp).
            return std::unexpected(AdmWriteError::kInvalidDocument);
        }
        auto libadm_channel = ::adm::AudioChannelFormat::create(::adm::AudioChannelFormatName(channel_format.name), type);
        for (const auto& block : channel_format.block_formats) {
            if (channel_format.type == TypeDefinition::kObjects) {
                libadm_channel->add(to_libadm_block(block));
            } else {
                libadm_channel->add(to_libadm_direct_speakers_block(block));
            }
        }
        document->add(libadm_channel);
        channel_formats_by_id.emplace(channel_format.id, std::move(libadm_channel));
    }

    std::unordered_map<std::string, std::shared_ptr<::adm::AudioPackFormat>> pack_formats_by_id;
    for (const auto& pack_format : model.pack_formats) {
        if (!pack_format.pack_format_refs.empty()) {
            // Nested audioPackFormat - out of scope, same as the channel-format loop above.
            return std::unexpected(AdmWriteError::kInvalidDocument);
        }
        ::adm::TypeDescriptor type;
        if (pack_format.type == TypeDefinition::kObjects) {
            type = ::adm::TypeDefinition::OBJECTS;
        } else if (pack_format.type == TypeDefinition::kDirectSpeakers) {
            type = ::adm::TypeDefinition::DIRECT_SPEAKERS;
        } else {
            return std::unexpected(AdmWriteError::kInvalidDocument);
        }
        auto libadm_pack = ::adm::AudioPackFormat::create(::adm::AudioPackFormatName(pack_format.name), type);
        for (const auto& ref : pack_format.channel_format_refs) {
            const auto resolved = resolve(channel_formats_by_id, ref);
            if (!resolved) {
                return std::unexpected(resolved.error());
            }
            libadm_pack->addReference(resolved->get());
        }
        document->add(libadm_pack);
        pack_formats_by_id.emplace(pack_format.id, std::move(libadm_pack));
    }

    // §5.1/§5.2 are skipped in favour of BS.2076-2's plain-PCM shortcut (model.hpp's own
    // AudioTrackUid comment: "the audioTrackUID has to refer to the corresponding
    // audioChannelFormat" when audioTrackFormat/audioStreamFormat are both omitted) - this writer
    // never produces coded/explicit-stream audio, so there is nothing for either element to
    // describe. model.stream_formats/model.track_formats are consequently always empty for a
    // document this writer builds; the loop bodies below exist only so a document built some
    // other way (a future second producer of ac3adm::AdmModel) still round-trips correctly.
    std::unordered_map<std::string, std::shared_ptr<::adm::AudioStreamFormat>> stream_formats_by_id;
    for (const auto& stream_format : model.stream_formats) {
        auto libadm_stream =
            ::adm::AudioStreamFormat::create(::adm::AudioStreamFormatName(stream_format.name), ::adm::FormatDefinition::PCM);
        if (stream_format.channel_format_ref) {
            const auto resolved = resolve(channel_formats_by_id, *stream_format.channel_format_ref);
            if (!resolved) {
                return std::unexpected(resolved.error());
            }
            libadm_stream->setReference(resolved->get());
        }
        document->add(libadm_stream);
        stream_formats_by_id.emplace(stream_format.id, std::move(libadm_stream));
    }

    std::unordered_map<std::string, std::shared_ptr<::adm::AudioTrackFormat>> track_formats_by_id;
    for (const auto& track_format : model.track_formats) {
        auto libadm_track =
            ::adm::AudioTrackFormat::create(::adm::AudioTrackFormatName(track_format.name), ::adm::FormatDefinition::PCM);
        if (track_format.stream_format_ref) {
            const auto resolved = resolve(stream_formats_by_id, *track_format.stream_format_ref);
            if (!resolved) {
                return std::unexpected(resolved.error());
            }
            libadm_track->setReference(resolved->get());
        }
        document->add(libadm_track);
        track_formats_by_id.emplace(track_format.id, std::move(libadm_track));
    }

    std::unordered_map<std::string, std::shared_ptr<::adm::AudioTrackUid>> track_uids_by_id;
    for (const auto& track_uid : model.track_uids) {
        auto libadm_track_uid = ::adm::AudioTrackUid::create();
        if (track_uid.has_sample_rate) {
            libadm_track_uid->set(::adm::SampleRate(track_uid.sample_rate));
        }
        if (track_uid.has_bit_depth) {
            libadm_track_uid->set(::adm::BitDepth(track_uid.bit_depth));
        }
        if (track_uid.track_format_ref) {
            const auto resolved = resolve(track_formats_by_id, *track_uid.track_format_ref);
            if (!resolved) {
                return std::unexpected(resolved.error());
            }
            libadm_track_uid->setReference(resolved->get());
        }
        if (track_uid.pack_format_ref) {
            const auto resolved = resolve(pack_formats_by_id, *track_uid.pack_format_ref);
            if (!resolved) {
                return std::unexpected(resolved.error());
            }
            libadm_track_uid->setReference(resolved->get());
        }
        if (track_uid.channel_format_ref) {
            const auto resolved = resolve(channel_formats_by_id, *track_uid.channel_format_ref);
            if (!resolved) {
                return std::unexpected(resolved.error());
            }
            libadm_track_uid->setReference(resolved->get());
        }
        document->add(libadm_track_uid);
        track_uids_by_id.emplace(track_uid.uid, libadm_track_uid);
    }

    std::unordered_map<std::string, std::shared_ptr<::adm::AudioObject>> objects_by_id;
    for (const auto& object : model.objects) {
        if (!object.object_refs.empty()) {
            // Nested audioObject references - out of scope, same as ac3::admbridge's own
            // read-side collect_leaf_objects() only ever WALKS these, never expects to write them.
            return std::unexpected(AdmWriteError::kInvalidDocument);
        }
        auto libadm_object = ::adm::AudioObject::create(::adm::AudioObjectName(object.name));
        if (object.start_s != 0.0) {
            libadm_object->set(::adm::Start(seconds_to_time(object.start_s)));
        }
        for (const auto& ref : object.pack_format_refs) {
            const auto resolved = resolve(pack_formats_by_id, ref);
            if (!resolved) {
                return std::unexpected(resolved.error());
            }
            libadm_object->addReference(resolved->get());
        }
        for (const auto& ref : object.track_uid_refs) {
            const auto resolved = resolve(track_uids_by_id, ref);
            if (!resolved) {
                return std::unexpected(resolved.error());
            }
            libadm_object->addReference(resolved->get());
        }
        document->add(libadm_object);
        objects_by_id.emplace(object.id, std::move(libadm_object));
    }

    std::unordered_map<std::string, std::shared_ptr<::adm::AudioContent>> contents_by_id;
    for (const auto& content : model.contents) {
        auto libadm_content = ::adm::AudioContent::create(::adm::AudioContentName(content.name));
        for (const auto& ref : content.object_refs) {
            const auto resolved = resolve(objects_by_id, ref);
            if (!resolved) {
                return std::unexpected(resolved.error());
            }
            libadm_content->addReference(resolved->get());
        }
        document->add(libadm_content);
        contents_by_id.emplace(content.id, std::move(libadm_content));
    }

    for (const auto& programme : model.programmes) {
        auto libadm_programme = ::adm::AudioProgramme::create(::adm::AudioProgrammeName(programme.name));
        for (const auto& ref : programme.content_refs) {
            const auto resolved = resolve(contents_by_id, ref);
            if (!resolved) {
                return std::unexpected(resolved.error());
            }
            libadm_programme->addReference(resolved->get());
        }
        document->add(libadm_programme);
    }

    return BuiltDocument{.document = std::move(document), .track_uids_by_key = std::move(track_uids_by_id)};
}

AdmModel build_adm_model(const std::shared_ptr<::adm::Document>& document) {
    AdmModel model;
    if (!document) {
        return model;
    }
    for (const auto& programme : document->getElements<adm::AudioProgramme>()) {
        model.programmes.push_back(convert(programme));
    }
    for (const auto& content : document->getElements<adm::AudioContent>()) {
        model.contents.push_back(convert(content));
    }
    for (const auto& object : document->getElements<adm::AudioObject>()) {
        model.objects.push_back(convert(object));
    }
    for (const auto& pack_format : document->getElements<adm::AudioPackFormat>()) {
        model.pack_formats.push_back(convert(pack_format));
    }
    for (const auto& channel_format : document->getElements<adm::AudioChannelFormat>()) {
        model.channel_formats.push_back(convert(channel_format));
    }
    for (const auto& stream_format : document->getElements<adm::AudioStreamFormat>()) {
        model.stream_formats.push_back(convert(stream_format));
    }
    for (const auto& track_format : document->getElements<adm::AudioTrackFormat>()) {
        model.track_formats.push_back(convert(track_format));
    }
    for (const auto& track_uid : document->getElements<adm::AudioTrackUid>()) {
        model.track_uids.push_back(convert(track_uid));
    }
    return model;
}

}  // namespace ac3adm::detail
