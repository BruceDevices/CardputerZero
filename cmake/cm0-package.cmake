#
# SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
#
# SPDX-License-Identifier: MIT
# Packaging and install layout for CardputerZero Debian builds.

include(GNUInstallDirs)

set(APP_DISPLAY_NAME "BruceCardputerZero" CACHE STRING "Human-readable application name used by launchers and package filename")
set(APP_DEBIAN_REVISION "bruce1" CACHE STRING "Debian package revision/vendor suffix")
set(APP_DEBIAN_ARCHITECTURE "arm64" CACHE STRING "Debian package architecture")
set(APP_MAINTAINER "BruceDevices <contact@bruce.computer>" CACHE STRING "Debian package maintainer")
# Package summary / .desktop comment. Must be non-empty: CMake >= 4.x CPackDeb
# errors on an empty summary (older CPack silently fell back to the project
# DESCRIPTION). Falls back to the project() description if unset.
if(NOT APP_PACKAGE_DESCRIPTION)
    if(PROJECT_DESCRIPTION)
        set(APP_PACKAGE_DESCRIPTION "${PROJECT_DESCRIPTION}")
    else()
        set(APP_PACKAGE_DESCRIPTION "Bruce security toolkit UI for CardputerZero")
    endif()
endif()
# M5Stack APPLaunch packaging rule: EVERY file must live under
# /usr/share/APPLaunch/ and the app must NOT run as root. APPLaunch launches the
# binary directly from the .desktop `Exec` (no systemd service), so we ship no
# service at all - that satisfies both submission checks. Layout mirrors the
# official examples: bin/<exec>, share/{fonts,images,audio}, applications/*.desktop.
set(APP_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/package")
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/templates/app.desktop.in"
    "${APP_GENERATED_DIR}/${PROJECT_NAME}.desktop"
    @ONLY
)

install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION bin
)

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/assets/audio/"
    DESTINATION "share/audio"
    PATTERN ".DS_Store" EXCLUDE
)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/"
    DESTINATION "share/fonts"
    PATTERN ".DS_Store" EXCLUDE
)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/assets/images/"
    DESTINATION "share/images"
    PATTERN ".DS_Store" EXCLUDE
)

install(FILES "${APP_GENERATED_DIR}/${PROJECT_NAME}.desktop"
    DESTINATION "applications"
)

install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
    DESTINATION "share/doc/${PROJECT_NAME}"
)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/LICENSE.txt"
    DESTINATION "share/doc/${PROJECT_NAME}"
    RENAME "third-party-assets-license.txt"
)

set(CPACK_GENERATOR "DEB")
# All install() DESTINATIONs above are relative to this prefix, so everything
# lands under /usr/share/APPLaunch/ as the app store requires.
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr/share/APPLaunch")
set(CPACK_OUTPUT_FILE_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/dist")
set(CPACK_PACKAGE_NAME "${APP_DISPLAY_NAME}")
set(CPACK_PACKAGE_VENDOR "BruceDevices")
set(CPACK_PACKAGE_CONTACT "${APP_MAINTAINER}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${APP_PACKAGE_DESCRIPTION}")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "${APP_DISPLAY_NAME}_${PROJECT_VERSION}_${APP_DEBIAN_REVISION}_${APP_DEBIAN_ARCHITECTURE}")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/LICENSE.txt")

string(TOLOWER "${APP_DISPLAY_NAME}" APP_DEBIAN_PACKAGE_NAME)
string(REGEX REPLACE "[^a-z0-9+.-]" "-" APP_DEBIAN_PACKAGE_NAME "${APP_DEBIAN_PACKAGE_NAME}")
set(CPACK_DEBIAN_PACKAGE_NAME "${APP_DEBIAN_PACKAGE_NAME}")
set(CPACK_DEBIAN_PACKAGE_VERSION "${PROJECT_VERSION}-${APP_DEBIAN_REVISION}")
set(CPACK_DEBIAN_PACKAGE_RELEASE "${APP_DEBIAN_REVISION}")
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "${APP_DEBIAN_ARCHITECTURE}")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${APP_MAINTAINER}")
set(CPACK_DEBIAN_PACKAGE_SECTION "APPLaunch")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
# Depends = only the shared libraries the executable actually links against. The
# app must not start without these.
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libc6, libstdc++6, libgcc-s1, libfreetype6, libpng16-16, libjpeg62-turbo, zlib1g, libfmt10")
# Recommends = the CLI tools the menu shells out to. The app runs fine without
# any of them (each menu entry just fails with a hint), so a missing tool must
# never block installation - which a hard Depends does under `dpkg -i` / offline
# apt. `apt install ./pkg.deb` still pulls these in automatically by default.
# Tools that back a visible menu feature a user expects to work out of the box
# (SDR listing/capture, PN532 NFC, wardrive GPS, QR). `apt install ./pkg.deb`
# pulls Recommends automatically, so these install with zero manual apt.
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "iproute2, iw, wireless-tools, rfkill, bluez, curl, netcat-openbsd, nmap, smbclient, i2c-tools, gpiod, flashrom, rtl-sdr, rtl-433, libnfc-bin, libnfc-examples, gpsd, gpsd-clients, qrencode")
# Suggests = heavier / offensive tooling the user opts into per feature (not
# auto-installed). Promote to Recommends above if you want it on by default.
set(CPACK_DEBIAN_PACKAGE_SUGGESTS "aircrack-ng, mdk4, hostapd, arp-scan, dnsmasq, v4l-utils, lirc, alsa-utils, dsniff, kismet")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION TRUE)

# Install-time passwordless-sudo setup. The postinst writes a scoped
# /etc/sudoers.d rule so the non-root GUI app can self-escalate its root tools
# (and apt installs, via --root-sh) with no password prompt - no manual step
# after `apt install ./pkg.deb`. The rule is generated by the maintainer
# script, not shipped as a data file, so the payload stays under
# /usr/share/APPLaunch/. Turn OFF (-DAPP_INSTALL_SUDOERS=OFF) for a strict
# APPLaunch store submission that forbids maintainer scripts; deploy.sh still
# installs the rule over SSH in that case.
option(APP_INSTALL_SUDOERS "Package a postinst that installs the passwordless sudoers rule" ON)
if(APP_INSTALL_SUDOERS)
    set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
        "${CMAKE_CURRENT_LIST_DIR}/templates/postinst;${CMAKE_CURRENT_LIST_DIR}/templates/postrm")
endif()

include(CPack)
