# Overlay port: microsoft/vcpkg's cpp-httplib port as of commit d725c087, unchanged but for
# this comment. It pins 0.56.0 ahead of vcpkg.json's builtin-baseline, which still has
# 0.52.0: src/sendspin's WebSocket transport needs 0.56.0's ws::WebSocket::set_read_timeout(),
# which hands a blocked read back as ReadResult::Timeout without closing the connection and is
# safe to call while another thread reads. Delete this directory once the baseline reaches
# 0.56.0. CMakePresets.json's core preset puts cmake/vcpkg/ports on VCPKG_OVERLAY_PORTS.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO yhirose/cpp-httplib
    REF "v${VERSION}"
    SHA512 3db42b77a96ae2f0e41c54c3897142f6c18fa237d85e6d173ea2e1ab238e77f6ac77134dcb80f6870697ef40bb68027b131b9d45823a41cedb86c677a01482b4
    HEAD_REF master
    PATCHES
        fix-find-brotli.patch
)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        brotli  HTTPLIB_REQUIRE_BROTLI
        openssl HTTPLIB_REQUIRE_OPENSSL
        zlib    HTTPLIB_REQUIRE_ZLIB
        zstd    HTTPLIB_REQUIRE_ZSTD
)

set(VCPKG_BUILD_TYPE release) # header-only port

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
    ${FEATURE_OPTIONS}
    -DHTTPLIB_USE_OPENSSL_IF_AVAILABLE=OFF
    -DHTTPLIB_USE_ZLIB_IF_AVAILABLE=OFF
    -DHTTPLIB_USE_BROTLI_IF_AVAILABLE=OFF
    -DHTTPLIB_USE_ZSTD_IF_AVAILABLE=OFF
    -DHTTPLIB_COMPILE=OFF
    -DHTTPLIB_TEST=OFF
    -DHTTPLIB_BUILD_MODULES=OFF
    -DHTTPLIB_REQUIRE_WOLFSSL=OFF
    -DHTTPLIB_USE_WOLFSSL_IF_AVAILABLE=OFF
    -DHTTPLIB_REQUIRE_MBEDTLS=OFF
    -DHTTPLIB_USE_MBEDTLS_IF_AVAILABLE=OFF
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME httplib CONFIG_PATH lib/cmake/httplib)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/lib")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
