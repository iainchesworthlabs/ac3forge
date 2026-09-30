/* Encode and decode one second of AC-3 through the C API (roadmap item F1),
 * the plain-C counterpart to encode_ac3.cpp/decode_stream.cpp. Every example
 * in this directory backs the excerpts in the docs/library/ pages; they live
 * here so the build checks them - this one is compiled as C, not C++, to
 * prove iclforge_c/iclforge.h is genuinely usable from a C toolchain, not
 * merely valid C parsed by a C++ compiler.
 *
 * Real audio from the first frame onward matters: an all-zero frame takes
 * the §7.2.2.1.1 all-zero bit-allocation path and exercises almost none of
 * the encoder - see CONTRIBUTING.md on why silence is a bad test signal.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "iclforge_c/iclforge.h"

#define kNumFrames 31 /* 48000 Hz / 1536 samples per frame, near enough one second */
/* Not M_PI: math.h only defines it as a non-standard extension (absent under
 * MSVC without _USE_MATH_DEFINES), and this example is built on every
 * platform the project targets. */
#define kPi 3.14159265358979323846

static void fill_with_audio(float* left, float* right, int frame, double rate) {
    for (int n = 0; n < ICLFORGE_SAMPLES_PER_FRAME; ++n) {
        const double t = (frame * ICLFORGE_SAMPLES_PER_FRAME + n) / rate;
        left[n] = (float)(0.5 * sin(2.0 * kPi * 1000.0 * t));
        right[n] = (float)(0.5 * sin(2.0 * kPi * 800.0 * t));
    }
}

int main(void) {
    iclforge_encoder_config_t encoder_config;
    iclforge_encoder_config_init(&encoder_config);
    encoder_config.bitrate_kbps = 192;
    encoder_config.acmod = ICLFORGE_ACMOD_2_0; /* L, R */

    iclforge_encoder_t* encoder = NULL;
    iclforge_status_t status = iclforge_encoder_create(&encoder_config, &encoder);
    if (status != ICLFORGE_OK) {
        fprintf(stderr, "encoder create failed: %s\n", iclforge_status_message(status));
        return 1;
    }

    iclforge_decoder_config_t decoder_config;
    iclforge_decoder_config_init(&decoder_config);

    iclforge_decoder_t* decoder = NULL;
    status = iclforge_decoder_create(&decoder_config, &decoder);
    if (status != ICLFORGE_OK) {
        fprintf(stderr, "decoder create failed: %s\n", iclforge_status_message(status));
        iclforge_encoder_destroy(encoder);
        return 1;
    }

    float left[ICLFORGE_SAMPLES_PER_FRAME];
    float right[ICLFORGE_SAMPLES_PER_FRAME];
    const float* channels[2] = {left, right};

    size_t total_bytes = 0;
    for (int frame = 0; frame < kNumFrames; ++frame) {
        fill_with_audio(left, right, frame, 48000.0);

        iclforge_bytes_t* encoded = NULL;
        status = iclforge_encoder_encode_frame(encoder, channels, 2, ICLFORGE_SAMPLES_PER_FRAME,
                                                &encoded);
        if (status != ICLFORGE_OK) {
            fprintf(stderr, "encode failed: %s\n", iclforge_status_message(status));
            iclforge_decoder_destroy(decoder);
            iclforge_encoder_destroy(encoder);
            return 1;
        }
        total_bytes += iclforge_bytes_size(encoded);

        iclforge_decoded_frame_t* decoded = NULL;
        status = iclforge_decoder_decode_frame(decoder, iclforge_bytes_data(encoded),
                                                iclforge_bytes_size(encoded), &decoded);
        iclforge_bytes_destroy(encoded);
        if (status != ICLFORGE_OK) {
            fprintf(stderr, "decode failed: %s\n", iclforge_status_message(status));
            iclforge_decoder_destroy(decoder);
            iclforge_encoder_destroy(encoder);
            return 1;
        }

        if (frame == 0) {
            printf("decoded acmod=%d channels=%zu dialnorm=%d\n",
                   (int)iclforge_decoded_frame_acmod(decoded),
                   iclforge_decoded_frame_channel_count(decoded),
                   iclforge_decoded_frame_dialnorm(decoded));
        }
        iclforge_decoded_frame_destroy(decoded);
    }

    iclforge_decoder_destroy(decoder);
    iclforge_encoder_destroy(encoder);

    printf("%zu bytes of AC-3, decoded via iclforge_c %s\n", total_bytes,
           iclforge_version().full);
    return 0;
}
