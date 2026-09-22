#include "ac3/latency.hpp"
#include "ac3/meta/drc.hpp"
#include "ac3/version.hpp"
#include "ac3forge_c/ac3forge.h"

#include "internal.hpp"

extern "C" {

const char* ac3forge_status_message(ac3forge_status_t status) {
    switch (status) {
        case AC3FORGE_OK: return "ok";
        case AC3FORGE_ERROR_INVALID_ARGUMENT: return "invalid argument";
        case AC3FORGE_ERROR_OUT_OF_MEMORY: return "out of memory";
        case AC3FORGE_ERROR_INTERNAL: return "internal error";
        case AC3FORGE_ERROR_ENCODE_INVALID_BITRATE: return "invalid bitrate";
        case AC3FORGE_ERROR_ENCODE_INVALID_DIALNORM: return "invalid dialnorm";
        case AC3FORGE_ERROR_ENCODE_INVALID_SUBSTREAM: return "invalid substream";
        case AC3FORGE_ERROR_ENCODE_INVALID_CHANNEL_MAP: return "invalid channel map";
        case AC3FORGE_ERROR_ENCODE_TOO_MANY_CHANNELS: return "too many channels";
        case AC3FORGE_ERROR_ENCODE_INVALID_MIX_LEVEL: return "invalid mix level";
        case AC3FORGE_ERROR_ENCODE_INVALID_BSI: return "invalid bit stream information";
        case AC3FORGE_ERROR_ENCODE_INVALID_OBJECT_AUDIO: return "invalid object audio";
        case AC3FORGE_ERROR_DECODE_TRUNCATED: return "truncated frame";
        case AC3FORGE_ERROR_DECODE_BAD_SYNC_WORD: return "bad sync word";
        case AC3FORGE_ERROR_DECODE_BAD_CRC: return "bad CRC";
        case AC3FORGE_ERROR_DECODE_RESERVED_VALUE: return "reserved value";
        case AC3FORGE_ERROR_DECODE_UNSUPPORTED: return "legal but unsupported syntax";
        case AC3FORGE_ERROR_DECODE_INVALID_STREAM: return "invalid stream";
        case AC3FORGE_ERROR_SCAN_EMPTY: return "empty stream";
        case AC3FORGE_ERROR_SCAN_LOST_SYNC: return "lost sync";
        case AC3FORGE_ERROR_SCAN_UNSUPPORTED_BSID: return "unsupported bsid";
        case AC3FORGE_ERROR_SCAN_RESERVED_VALUE: return "reserved value";
        case AC3FORGE_ERROR_SCAN_TRUNCATED: return "truncated stream";
        case AC3FORGE_ERROR_SCAN_UNSUPPORTED_STRUCTURE: return "unsupported stream structure";
    }
    return "unknown status";
}

int ac3forge_latency_total_samples(const ac3forge_latency_t* latency) {
    if (latency == nullptr) {
        return 0;
    }
    // Summed here rather than by calling through ac3::LatencyBudget: the
    // caller may have filled this struct in by hand (it is plain data, and
    // nothing stops an integrator writing their own terms into it to price a
    // configuration they have not built an encoder for).
    return latency->frame_samples + latency->transform_samples + latency->lookahead_samples +
           latency->holdback_samples;
}

double ac3forge_latency_ms(int samples, ac3forge_sample_rate_t sample_rate) {
    return ac3::latency_ms(samples, ac3forge_c::to_cpp(sample_rate));
}

ac3forge_version_t ac3forge_version(void) {
    return ac3forge_version_t{.major = ac3::version_major,
                               .minor = ac3::version_minor,
                               .patch = ac3::version_patch,
                               .full = ac3::version_full.data()};
}

void ac3forge_heavy_config_init(ac3forge_heavy_config_t* config) {
    if (config == nullptr) {
        return;
    }
    const ac3::meta::HeavyConfig defaults{};
    *config = ac3forge_heavy_config_t{.dialogue_target_dbfs = defaults.dialogue_target_dbfs,
                                       .peak_ceiling_dbfs = defaults.peak_ceiling_dbfs,
                                       .release_db_per_second = defaults.release_db_per_second};
}

}  // extern "C"
