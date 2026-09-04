kdeconnect_add_plugin(kdeconnect_camera SOURCES cameraplugin.cpp streamwriter.cpp)
target_link_libraries(kdeconnect_camera kdeconnectcore Qt::DBus Qt::Core KF6::I18n)

install(FILES "kdeconnect_camera_config.qml" DESTINATION ${KDE_INSTALL_DATADIR}/kdeconnect)
