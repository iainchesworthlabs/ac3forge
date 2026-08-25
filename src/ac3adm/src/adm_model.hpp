#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include "ac3adm/ac3adm.hpp"
#include "ac3adm/model.hpp"

// Forward-declared rather than #include <adm/document.hpp> here: libadm's
// own headers (and the Boost headers they pull in) are an implementation
// detail of this translation-unit pair (adm_model.hpp/.cpp) plus adm.cpp -
// nothing else in ac3adm, and nothing in its public ac3adm/ac3adm.hpp or
// ac3adm/model.hpp headers, needs to know libadm's types exist. See
// ac3adm/model.hpp's own header comment for why the two libraries'
// identically-named classes (adm::AudioObject vs. ac3adm::AudioObject, ...)
// must never appear unqualified in the same header.
namespace adm {
class Document;
class AudioTrackUid;
}  // namespace adm

namespace ac3adm::detail {

// Translates a fully-parsed libadm `adm::Document` (see `adm::parseXml()`)
// into this module's own ADM object graph (ac3adm::AdmModel), per
// Recommendation ITU-R BS.2076-2 (10/2019) Annex 1. libadm already enforces
// the schema's Required/Default/Optional rules while parsing, so this
// function trusts a successfully-returned Document to have every Required
// field present - it does not re-validate them.
[[nodiscard]] AdmModel build_adm_model(const std::shared_ptr<::adm::Document>& document);

// The write-side inverse: builds a libadm `adm::Document` from `model`, ready for
// `adm::reassignIds()` and `adm::writeXml()` (both called by adm.cpp's write_bw64, not here - this
// function only builds the graph). `track_uids_by_key` maps each `AdmModel::AudioTrackUid::uid`
// string to the `adm::AudioTrackUid` it became, keyed by that SAME correlation string (see
// ac3adm.hpp's write_bw64 doc comment on why these are correlation keys, not real ADM IDs) - so
// write_bw64 can resolve `AdmDocument::chna` entries (which name a track_uids[].uid) back to the
// libadm element they describe, after reassignIds() has given it its real, final AudioTrackUidId.
struct BuiltDocument {
    std::shared_ptr<::adm::Document> document;
    std::unordered_map<std::string, std::shared_ptr<::adm::AudioTrackUid>> track_uids_by_key;
};

[[nodiscard]] std::expected<BuiltDocument, AdmWriteError> build_libadm_document(const AdmModel& model);

}  // namespace ac3adm::detail
