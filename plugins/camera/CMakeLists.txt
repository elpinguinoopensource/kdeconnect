qt_add_resources(kdeconnect_camera_SRCS assets.qrc)
# tests/CMakeLists.txt lives in a different directory scope and needs the
# generated resource sources to link idlewriter's qrc access into the test
# binary, so export the variable through the cache.
set(kdeconnect_camera_SRCS "${kdeconnect_camera_SRCS}" CACHE INTERNAL "Generated sources for the camera plugin resources")

kdeconnect_add_plugin(kdeconnect_camera SOURCES cameraplugin.cpp streamwriter.cpp idlewriter.cpp ${kdeconnect_camera_SRCS})
target_link_libraries(kdeconnect_camera kdeconnectcore Qt::DBus Qt::Core KF6::I18n)

install(FILES "kdeconnect_camera_config.qml" DESTINATION ${KDE_INSTALL_DATADIR}/kdeconnect)
