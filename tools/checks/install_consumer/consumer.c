/* A consumer of an INSTALLED ac3forge package's C API, built by
 * tools/checks/check_install_consumer.sh against `cmake --install`'s output rather than the
 * build tree. The in-tree C examples and tests/capi compile against build-tree include paths,
 * which hold files an install has to be told to copy (the generated version.h among them), so
 * only this translation unit includes ac3forge_c/ac3forge.h the way a downstream project does.
 *
 * Beyond compiling, it encodes one frame and checks the AC-3 syncword, so a package whose
 * library loads but whose codec was left off the link line fails here too. The frame is a
 * sawtooth rather than silence, which takes the all-zero bit-allocation path and says almost
 * nothing about the link (see examples/capi_encode_decode.c), and uses no libm, which a C link
 * does not add.
 */

#include <stdio.h>

#include <ac3forge_c/ac3forge.h>

/* version.h's own promise, the one tests/capi checks from C++ (this is the C11 spelling). */
_Static_assert(AC3FORGE_C_VERSION == AC3FORGE_C_VERSION_MAJOR * 1000000 +
                                         AC3FORGE_C_VERSION_MINOR * 1000 +
                                         AC3FORGE_C_VERSION_PATCH,
               "AC3FORGE_C_VERSION disagrees with its own MAJOR/MINOR/PATCH");

int main(void) {
    /* Built and linked from the same package, so the compile-time macros and the runtime
     * report must agree. */
    const ac3forge_version_t linked = ac3forge_version();
    if (linked.major != AC3FORGE_C_VERSION_MAJOR || linked.minor != AC3FORGE_C_VERSION_MINOR ||
        linked.patch != AC3FORGE_C_VERSION_PATCH) {
        fprintf(stderr, "compiled against %d.%d.%d but linked %d.%d.%d (%s)\n",
                AC3FORGE_C_VERSION_MAJOR, AC3FORGE_C_VERSION_MINOR, AC3FORGE_C_VERSION_PATCH,
                linked.major, linked.minor, linked.patch, linked.full);
        return 1;
    }

    ac3forge_encoder_config_t config;
    ac3forge_encoder_config_init(&config);
    config.bitrate_kbps = 192;
    config.acmod = AC3FORGE_ACMOD_2_0; /* L, R */

    ac3forge_encoder_t* encoder = NULL;
    ac3forge_status_t status = ac3forge_encoder_create(&config, &encoder);
    if (status != AC3FORGE_OK) {
        fprintf(stderr, "encoder create failed: %s\n", ac3forge_status_message(status));
        return 1;
    }

    float left[AC3FORGE_SAMPLES_PER_FRAME];
    float right[AC3FORGE_SAMPLES_PER_FRAME];
    for (int n = 0; n < AC3FORGE_SAMPLES_PER_FRAME; ++n) {
        left[n] = (float)(n % 96) / 96.0f - 0.5f;
        right[n] = (float)(n % 64) / 64.0f - 0.5f;
    }
    const float* channels[2] = {left, right};

    ac3forge_bytes_t* encoded = NULL;
    status = ac3forge_encoder_encode_frame(encoder, channels, 2, AC3FORGE_SAMPLES_PER_FRAME,
                                           &encoded);
    if (status != AC3FORGE_OK) {
        fprintf(stderr, "encode failed: %s\n", ac3forge_status_message(status));
        ac3forge_encoder_destroy(encoder);
        return 1;
    }

    const uint8_t* bytes = ac3forge_bytes_data(encoded);
    const size_t size = ac3forge_bytes_size(encoded);
    const int syncword = size >= 2 && bytes[0] == 0x0B && bytes[1] == 0x77;
    if (syncword) {
        printf("ac3forge_c %s: encoded one frame, %zu bytes\n", linked.full, size);
    } else {
        fprintf(stderr, "encoded frame (%zu bytes) does not start with the AC-3 syncword\n", size);
    }

    ac3forge_bytes_destroy(encoded);
    ac3forge_encoder_destroy(encoder);
    return syncword ? 0 : 1;
}
