#include "ac3/encoder/silent_frame.hpp"

#include <string_view>

namespace ac3 {

std::string_view describe(FrameError error) {
    switch (error) {
        case FrameError::kInvalidBitrate: return "bitrate does not map to a legal frmsizecod row";
        case FrameError::kInvalidDialnorm: return "dialnorm out of range 1..31";
        case FrameError::kInvalidSubstream:
            return "substream strmtyp/substreamid out of range, too many dependents, or a "
                   "dependent disagrees with its parent on sample rate";
        case FrameError::kInvalidChannelMap:
            return "chanmap does not add up to the channels acmod/lfeon code";
        case FrameError::kTooManyChannels:
            return "bed and dependents together exceed sixteen distinct locations";
        case FrameError::kInvalidMixLevel: return "mixing-metadata field reserved or out of range";
        case FrameError::kInvalidObjectAudio:
            return "aux user data too long, or object count outside what addbsi allows";
        case FrameError::kInvalidBsi:
            return "a bsi field would not fit, or the config asked for two things the syntax "
                   "cannot carry at once";
    }
    return "unknown frame error";
}

}  // namespace ac3
