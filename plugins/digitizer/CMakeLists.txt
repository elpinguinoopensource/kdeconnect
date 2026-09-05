# SPDX-FileCopyrightText: 2025 Martin Sh <hemisputnik@proton.me>
# SPDX-License-Identifier: BSD-2-Clause

if(UNIX AND NOT APPLE)
    pkg_check_modules(Evdev IMPORTED_TARGET libevdev)

    if (Evdev_FOUND)
        set(HAS_DEPS 1)
    endif()
endif()

if(HAS_DEPS)
    kdeconnect_add_plugin(kdeconnect_digitizer SOURCES digitizerplugin.cpp abstractdigitizerimpl.cpp)

    target_link_libraries(kdeconnect_digitizer
        kdeconnectcore
        KF6::I18n
        KF6::WidgetsAddons
    )

    if(UNIX AND NOT APPLE)
        target_link_libraries(kdeconnect_digitizer PkgConfig::Evdev)

        target_sources(kdeconnect_digitizer PUBLIC linuxdigitizerimpl.cpp)
    endif()

    # udev is not multilib: it only ever reads rules from <prefix>/lib/udev/rules.d
    # (plus /run/udev/rules.d and /etc/udev/rules.d), never from lib64.
    # KDE_INSTALL_LIBDIR resolves to "lib64" on distributions such as openSUSE and
    # Fedora, which would install this rule where udev ignores it, silently leaving
    # the digitizer unable to open /dev/uinput.
    #
    # Prefer the directory udev itself reports (needs libudev's pkg-config file);
    # fall back to the conventional non-multilib path when it is unavailable.
    # The prefix is deliberately not "UDev", which ECM's FindUDev.cmake owns.
    pkg_check_modules(SystemdUDev udev)
    if(SystemdUDev_FOUND AND SystemdUDev_udevdir)
        set(UDEV_RULES_INSTALL_DIR "${SystemdUDev_udevdir}/rules.d")
    else()
        set(UDEV_RULES_INSTALL_DIR "${CMAKE_INSTALL_PREFIX}/lib/udev/rules.d")
    endif()

    install(FILES 40-kdeconnect-uinput.rules DESTINATION ${UDEV_RULES_INSTALL_DIR})

    message(STATUS "digitizer: Dependencies met, compiling.")
else()
    message(WARNING "digitizer: Dependencies missing, not compiling.")
endif()
