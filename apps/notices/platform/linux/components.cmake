# The Linux packages' notices (../../notices.cmake, cmake/Notices.cmake):
# what the tarball, the .deb, the .rpm and the AppImage carry. One file for
# all four - .github/workflows/_build.yml builds the AppImage out of this
# same `runtime` component's install tree (its "Install into AppDir" step),
# so whatever is written here is what ends up inside the image as well. That
# is why the Qt section is qt-linux rather than a bundled/system pair: the
# .deb loads the distribution's Qt and the AppImage carries its own, and one
# section has to be true of both.
set(AC3FORGE_NOTICES_PLATFORM "Linux")
set(AC3FORGE_NOTICES_LOCATION "/usr/share/doc/ac3forge/NOTICES.txt (share/doc/ac3forge/ in the tarball, usr/share/doc/ac3forge/ inside the AppImage), beside LICENSE.txt")
set(AC3FORGE_NOTICE_FRAGMENTS header qt-linux fmt fonts trademarks)
