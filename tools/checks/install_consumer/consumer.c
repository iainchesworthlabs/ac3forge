/* A consumer of an INSTALLED iclforge package's C API, built by
 * tools/checks/check_install_consumer.sh against `cmake --install`'s output rather than the
 * build tree. The in-tree C examples and tests/capi compile against build-tree include paths,
 * which hold files an install has to be told to copy (the generated version.h among them), so
 * only this translation unit includes iclforge_c/iclforge.h the way a downstream project does.
 *
 * Beyond compiling, it encodes one frame and checks the AC-3 syncword, so a package whose
 * library loads but whose codec was left off the link line fails here too. The frame is a
 * sawtooth rather than silence, which takes the all-zero bit-allocation path and says almost
 * nothing about the link (see examples/capi_encode_decode.c), and uses no libm, which a C link
 * does not add.
 */

#include <stdio.h>

#include <iclforge_c/iclforge.h>

/* version.h's own promise, the one tests/capi checks from C++ (this is the C11 spelling). */
_Static_assert(ICLFORGE_C_VERSION == ICLFORGE_C_VERSION_MAJOR * 1000000 +
                                         ICLFORGE_C_VERSION_MINOR * 1000 +
                                         ICLFORGE_C_VERSION_PATCH,
               "ICLFORGE_C_VERSION disagrees with its own MAJOR/MINOR/PATCH");

int main(void) {
    /* Built and linked from the same package, so the compile-time macros and the runtime
     * report must agree. */
    const iclforge_version_t linked = iclforge_version();
    if (linked.major != ICLFORGE_C_VERSION_MAJOR || linked.minor != ICLFORGE_C_VERSION_MINOR ||
        linked.patch != ICLFORGE_C_VERSION_PATCH) {
        fprintf(stderr, "compiled against %d.%d.%d but linked %d.%d.%d (%s)\n",
                ICLFORGE_C_VERSION_MAJOR, ICLFORGE_C_VERSION_MINOR, ICLFORGE_C_VERSION_PATCH,
                linked.major, linked.minor, linked.patch, linked.full);
        return 1;
    }

    iclforge_encoder_config_t config;
    iclforge_encoder_config_init(&config);
    config.bitrate_kbps = 192;
    config.acmod = ICLFORGE_ACMOD_2_0; /* L, R */

    iclforge_encoder_t* encoder = NULL;
    iclforge_status_t status = iclforge_encoder_create(&config, &encoder);
    if (status != ICLFORGE_OK) {
        fprintf(stderr, "encoder create failed: %s\n", iclforge_status_message(status));
        return 1;
    }

    float left[ICLFORGE_SAMPLES_PER_FRAME];
    float right[ICLFORGE_SAMPLES_PER_FRAME];
    for (int n = 0; n < ICLFORGE_SAMPLES_PER_FRAME; ++n) {
        left[n] = (float)(n % 96) / 96.0f - 0.5f;
        right[n] = (float)(n % 64) / 64.0f - 0.5f;
    }
    const float* channels[2] = {left, right};

    iclforge_bytes_t* encoded = NULL;
    status = iclforge_encoder_encode_frame(encoder, channels, 2, ICLFORGE_SAMPLES_PER_FRAME,
                                           &encoded);
    if (status != ICLFORGE_OK) {
        fprintf(stderr, "encode failed: %s\n", iclforge_status_message(status));
        iclforge_encoder_destroy(encoder);
        return 1;
    }

    const uint8_t* bytes = iclforge_bytes_data(encoded);
    const size_t size = iclforge_bytes_size(encoded);
    const int syncword = size >= 2 && bytes[0] == 0x0B && bytes[1] == 0x77;
    if (syncword) {
        printf("iclforge_c %s: encoded one frame, %zu bytes\n", linked.full, size);
    } else {
        fprintf(stderr, "encoded frame (%zu bytes) does not start with the AC-3 syncword\n", size);
    }

    iclforge_bytes_destroy(encoded);
    iclforge_encoder_destroy(encoder);
    return syncword ? 0 : 1;
}
