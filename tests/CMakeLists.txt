find_package(Qt6 ${QT_MIN_VERSION} REQUIRED COMPONENTS Test)

set(kdeconnect_libraries
    kdeconnectcore
    kdeconnectdbusinterfaces
    kdeconnectmodels
    kdeconnectsmshelper
    kdeconnectversion
    KF6::GuiAddons
    KF6::I18n
    KF6::KIOWidgets
    KF6::Notifications
    Qt::DBus
    Qt::Network
    KF6::People
    Qt::Qml
    Qt::Test
)

ecm_add_test(notificationstest.cpp LINK_LIBRARIES ${kdeconnect_libraries})
ecm_add_test(camerastreamtest.cpp LINK_LIBRARIES ${kdeconnect_libraries})
target_sources(camerastreamtest PRIVATE
    ${CMAKE_SOURCE_DIR}/plugins/camera/streamwriter.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/../plugins/camera/plugin_camera_debug.cpp
)
target_include_directories(camerastreamtest PRIVATE
    ${CMAKE_SOURCE_DIR}/plugins/camera
    ${CMAKE_CURRENT_BINARY_DIR}/../plugins/camera
)
ecm_add_test(idlewritertest.cpp LINK_LIBRARIES ${kdeconnect_libraries})
# kdeconnect_camera_SRCS holds the qrc_assets.cpp generated in the camera
# plugin's binary dir; the GENERATED flag is directory-scoped, so re-mark it
# here for CMake to accept it as a source of this target.
set_source_files_properties(${kdeconnect_camera_SRCS} PROPERTIES GENERATED TRUE)
target_sources(idlewritertest PRIVATE
    ${CMAKE_SOURCE_DIR}/plugins/camera/idlewriter.cpp
    ${kdeconnect_camera_SRCS}
    ${CMAKE_CURRENT_BINARY_DIR}/../plugins/camera/plugin_camera_debug.cpp
)
target_include_directories(idlewritertest PRIVATE
    ${CMAKE_SOURCE_DIR}/plugins/camera
    ${CMAKE_CURRENT_BINARY_DIR}/../plugins/camera
)
ecm_add_test(pluginloadtest.cpp LINK_LIBRARIES ${kdeconnect_libraries})
ecm_add_test(sendfiletest.cpp LINK_LIBRARIES ${kdeconnect_libraries})
target_sources(sendfiletest PRIVATE testdevice.cpp)
ecm_add_test(smshelpertest.cpp LINK_LIBRARIES ${kdeconnect_libraries})
if (WIN32 OR APPLE)
    ecm_add_test(mdnstest.cpp LINK_LIBRARIES ${kdeconnect_libraries})
endif()
