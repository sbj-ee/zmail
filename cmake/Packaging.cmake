# Install rules + CPack (.deb only; Linux amd64).
# Version comes solely from project(zmail VERSION ...) -> PROJECT_VERSION.
include(GNUInstallDirs)

install(TARGETS zmail RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${CMAKE_SOURCE_DIR}/packaging/zmail.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
# App icon: assets/icons/zmail-<N>.png -> hicolor/<N>x<N>/apps/zmail.png
# (Icon=zmail in the .desktop file). zmail-1024.png is the master only:
# hicolor has no 1024x1024 directory.
foreach(_sz 16 24 32 48 64 128 256 512)
  install(FILES ${CMAKE_SOURCE_DIR}/assets/icons/zmail-${_sz}.png
          DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${_sz}x${_sz}/apps
          RENAME zmail.png)
endforeach()
# Bundled default new-mail chime (also compiled into Qt resources).
install(FILES ${CMAKE_SOURCE_DIR}/assets/sounds/new-mail.wav
        DESTINATION ${CMAKE_INSTALL_DATADIR}/zmail/sounds)
install(FILES ${CMAKE_SOURCE_DIR}/LICENSE
        DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/zmail RENAME copyright)

set(CPACK_PACKAGE_NAME "zmail")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VENDOR "Stephen B. Johnson")
set(CPACK_PACKAGE_CONTACT "Stephen B. Johnson <49662809+sbj-ee@users.noreply.github.com>")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/sbj-ee/zmail")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Gmail desktop client with per-sender new-mail sounds (Qt 6)")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
set(CPACK_GENERATOR "DEB")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CPACK_PACKAGE_CONTACT}")
set(CPACK_DEBIAN_PACKAGE_SECTION "mail")
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "${CPACK_PACKAGE_HOMEPAGE_URL}")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
# shlibdeps can't see Qt plugins: the SQLite driver holds the mail cache.
# Refresh tokens need a Secret Service keyring (GNOME Keyring on Ubuntu).
# Colour emoji in subjects and mail: zmail asks for "Noto Color Emoji" by name.
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libqt6sql6-sqlite, libqt6multimedia6")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "gnome-keyring, hunspell-en-us, fonts-noto-color-emoji")
# zmail_<ver>_amd64.deb (the name a future updater can expect).
set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")
include(CPack)
