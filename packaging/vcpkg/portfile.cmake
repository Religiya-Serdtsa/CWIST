# vcpkg port for CWIST. Maintained here and mirrored into a vcpkg registry
# overlay (ports/cwist/) or submitted upstream to microsoft/vcpkg.
#
# Before publishing:
#   1. make dist
#   2. upload dist/cwist-<version>.tar.gz to the GitHub release assets
#   3. update REF/SHA512 below (`vcpkg hash cwist-<version>.tar.gz`)

vcpkg_download_distfile(ARCHIVE
    URLS "https://github.com/Religiya-Serdtsa/CWIST/releases/download/v${VERSION}/cwist-${VERSION}.tar.gz"
    FILENAME "cwist-${VERSION}.tar.gz"
    SHA512 00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000 # fill on release
)

vcpkg_extract_source_archive(
    SOURCE_PATH
    ARCHIVE "${ARCHIVE}"
)

# CWIST uses a plain GNU Makefile with vendored dependencies; drive it
# directly rather than re-creating the build under CMake.
vcpkg_configure_make(
    SOURCE_PATH "${SOURCE_PATH}"
    SKIP_CONFIGURE
)

vcpkg_install_make(
    INSTALL_TARGET install
    OPTIONS "PREFIX=${CURRENT_PACKAGES_DIR}"
)

vcpkg_fixup_pkgconfig()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug")

file(INSTALL "${SOURCE_PATH}/LICENSE"
     DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}"
     RENAME copyright)
