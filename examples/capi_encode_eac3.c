/* Encode a 5.1.2 E-AC-3 access unit through the C API (roadmap item AP5),
 * the plain-C counterpart to encode_eac3.cpp. A 3/2+LFE bed carries the 5.1
 * base layer; one dependent substream widens it with a Vhl/Vhr height pair -
 * see docs/library/encoding-eac3.md's "Wide layouts" section for what a
 * dependent's chanmap means and docs/library/c-api.md for the C surface.
 *
 * Real audio from the first frame onward matters: an all-zero frame takes
 * the §7.2.2.1.1 all-zero bit-allocation path and exercises almost none of
 * the encoder - see CONTRIBUTING.md on why silence is a bad test signal.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "ac3forge_c/ac3forge.h"

#define kNumFrames 31 /* 48000 Hz / 1536 samples per frame, near enough one second */
#define kNumChannels 8 /* 3/2 (5) + LFE (1) + dependent 2/0 (2) */
/* Not M_PI: math.h only defines it as a non-standard extension (absent under
 * MSVC without _USE_MATH_DEFINES), and this example is built on every
 * platform the project targets. */
#define kPi 3.14159265358979323846

static const double kTonesHz[kNumChannels] = {1000.0, 1200.0, 800.0,  600.0,
                                              1400.0, 60.0,   2000.0, 1300.0};

static void fill_with_audio(float* channels[kNumChannels], int frame, double rate) {
    for (int ch = 0; ch < kNumChannels; ++ch) {
        for (int n = 0; n < AC3FORGE_SAMPLES_PER_FRAME; ++n) {
            const double t = (frame * AC3FORGE_SAMPLES_PER_FRAME + n) / rate;
            channels[ch][n] = (float)(0.3 * sin(2.0 * kPi * kTonesHz[ch] * t));
        }
    }
}

int main(void) {
    ac3forge_eac3_frame_config_t independent;
    ac3forge_eac3_frame_config_init(&independent);
    independent.bitrate_kbps = 448;
    independent.acmod = AC3FORGE_ACMOD_3_2; /* L, C, R, Ls, Rs */
    independent.lfe = 1;

    ac3forge_eac3_frame_config_t dependent;
    ac3forge_eac3_frame_config_init(&dependent);
    dependent.bitrate_kbps = 192;
    dependent.acmod = AC3FORGE_ACMOD_2_0;
    dependent.has_chanmap = 1;
    dependent.chanmap = AC3FORGE_CHANMAP_512_HEIGHT; /* Vhl, Vhr -> 5.1.2 */

    ac3forge_eac3_access_unit_encoder_t* encoder = NULL;
    ac3forge_status_t status =
        ac3forge_eac3_access_unit_encoder_create(&independent, &dependent, 1, &encoder);
    if (status != AC3FORGE_OK) {
        fprintf(stderr, "access-unit encoder create failed: %s\n", ac3forge_status_message(status));
        return 1;
    }

    ac3forge_decoder_config_t decoder_config;
    ac3forge_decoder_config_init(&decoder_config);
    ac3forge_eac3_decoder_t* decoder = NULL;
    status = ac3forge_eac3_decoder_create(&decoder_config, &decoder);
    if (status != AC3FORGE_OK) {
        fprintf(stderr, "decoder create failed: %s\n", ac3forge_status_message(status));
        ac3forge_eac3_access_unit_encoder_destroy(encoder);
        return 1;
    }

    float storage[kNumChannels][AC3FORGE_SAMPLES_PER_FRAME];
    float* channels[kNumChannels];
    for (int ch = 0; ch < kNumChannels; ++ch) {
        channels[ch] = storage[ch];
    }

    size_t total_bytes = 0;
    for (int frame = 0; frame < kNumFrames; ++frame) {
        fill_with_audio(channels, frame, 48000.0);

        ac3forge_eac3_access_unit_t* unit = NULL;
        status = ac3forge_eac3_access_unit_encoder_encode(
            encoder, (const float* const*)channels, kNumChannels, AC3FORGE_SAMPLES_PER_FRAME, NULL,
            0, &unit);
        if (status != AC3FORGE_OK) {
            fprintf(stderr, "encode failed: %s\n", ac3forge_status_message(status));
            ac3forge_eac3_decoder_destroy(decoder);
            ac3forge_eac3_access_unit_encoder_destroy(encoder);
            return 1;
        }
        total_bytes += ac3forge_eac3_access_unit_size(unit);

        ac3forge_decoded_access_unit_t* decoded = NULL;
        status = ac3forge_eac3_decoder_decode_access_unit(
            decoder, ac3forge_eac3_access_unit_data(unit), ac3forge_eac3_access_unit_size(unit),
            &decoded);
        ac3forge_eac3_access_unit_destroy(unit);
        if (status != AC3FORGE_OK) {
            fprintf(stderr, "decode failed: %s\n", ac3forge_status_message(status));
            ac3forge_eac3_decoder_destroy(decoder);
            ac3forge_eac3_access_unit_encoder_destroy(encoder);
            return 1;
        }

        if (frame == 0 && decoded != NULL) {
            printf("decoded acmod=%d substreams=%d channels=%zu dialnorm=%d\n",
                   (int)ac3forge_decoded_access_unit_acmod(decoded),
                   ac3forge_decoded_access_unit_substream_count(decoded),
                   ac3forge_decoded_access_unit_channel_count(decoded),
                   ac3forge_decoded_access_unit_dialnorm(decoded));
        }
        if (decoded != NULL) {
            ac3forge_decoded_access_unit_destroy(decoded);
        }
    }

    ac3forge_eac3_decoder_destroy(decoder);
    ac3forge_eac3_access_unit_encoder_destroy(encoder);

    printf("%zu bytes of E-AC-3 (5.1.2), decoded via ac3forge_c %s\n", total_bytes,
           ac3forge_version().full);
    return 0;
}
