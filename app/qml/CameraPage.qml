/*
 * SPDX-FileCopyrightText: 2026 Alfredo Medrano Sanchez <alfredomedranosanchez@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kdeconnect

Kirigami.ScrollablePage {
    id: root

    title: i18nd("kdeconnect-app", "Camera")
    property var device
    property var pluginInterface // CameraDeviceDbusInterface

    // Per-device defaults stored by kdeconnect_camera_config.qml
    KdeConnectPluginConfig {
        id: config
        deviceId: root.device ? root.device.id() : ""
        pluginName: "kdeconnect_camera"
    }

    readonly property int defaultFps: config.getInt("fps", 30)
    readonly property int defaultBitrate: config.getInt("bitrate", 4000)

    // Array of {cameraId, facing, hasFlash, sizes: [{width, height, fps}]}
    property var cameras: []
    // fps manually picked by the user; -1 means "follow the selected resolution"
    property int userFps: -1
    property string errorMessage: ""
    property string stoppedMessage: ""

    function facingLabel(facing) {
        switch (facing) {
        case "back":
            return i18ndc("kdeconnect-app", "Camera facing direction", "Back")
        case "front":
            return i18ndc("kdeconnect-app", "Camera facing direction", "Front")
        default:
            return i18ndc("kdeconnect-app", "Camera facing direction", "External")
        }
    }

    // Maps the stable protocol error codes sent by the Android CameraProtocol
    // (forwarded verbatim by the C++ plugin) to human-readable labels. Unknown
    // codes keep the raw value so diagnostics are not lost.
    function errorLabel(code) {
        switch (code) {
        case "in_use":
            return i18ndc("kdeconnect-app", "Camera error, the camera is busy on the device", "Camera in use by another app")
        case "denied":
            return i18ndc("kdeconnect-app", "Camera error, user denied camera permission on the device", "Camera permission denied")
        case "unsupported":
            return i18ndc("kdeconnect-app", "Camera error, camera or requested format unsupported", "Camera or format not supported")
        case "disconnected":
            return i18ndc("kdeconnect-app", "Camera error, device went away mid-stream", "Device disconnected")
        case "stopped":
            return i18ndc("kdeconnect-app", "Camera error, stream stopped on the device", "Stream stopped on the device")
        case "ended":
            return i18ndc("kdeconnect-app", "Camera stream ended normally on the device", "Stream ended")
        case "no_payload":
            return i18ndc("kdeconnect-app", "Camera error, malformed camera packet", "Invalid camera request")
        default:
            return i18nd("kdeconnect-app", "Camera error (%1)", code)
        }
    }

    function cameraLabel(index) {
        if (index < 0 || index >= root.cameras.length) {
            return ""
        }
        const camera = root.cameras[index]
        //: %1 is the camera facing (Back/Front/External), %2 the camera identifier
        return i18nd("kdeconnect-app", "%1 (%2)", facingLabel(camera.facing), camera.cameraId)
    }

    // Async DBus call: listCameras() returns a pending reply, resolved via DBusAsyncResponse.
    DBusAsyncResponse {
        id: cameraListResponse
        autoDelete: false
        onSuccess: result => root.cameras = result
        onError: error => console.warn("Camera: could not fetch camera list", error)
    }

    Connections {
        target: root.pluginInterface

        function onCameraListReceived(cameras) {
            root.cameras = cameras
        }

        function onErrorReceived(error) {
            root.errorMessage = errorLabel(error)
        }

        function onStreamStopped(reason) {
            root.stoppedMessage = errorLabel(reason)
        }
    }

    Component.onCompleted: {
        root.pluginInterface.refreshCameraList()
        cameraListResponse.setPendingCall(root.pluginInterface.listCameras())
    }

    actions: [
        Kirigami.Action {
            icon.name: "view-refresh"
            text: i18nd("kdeconnect-app", "Refresh")
            onTriggered: {
                root.pluginInterface.refreshCameraList()
                cameraListResponse.setPendingCall(root.pluginInterface.listCameras())
            }
        }
    ]

    Kirigami.FormLayout {
        id: layout

        readonly property int cameraIndex: cameraCombo.currentIndex
        readonly property var camera: cameraIndex >= 0 && cameraIndex < root.cameras.length
            ? root.cameras[cameraIndex] : null
        readonly property int sizeIndex: resolutions.currentIndex
        readonly property var size: sizeIndex >= 0 && sizeIndex < resolutions.model.length
            ? resolutions.model[sizeIndex] : null

        QQC2.ComboBox {
            id: cameraCombo
            Kirigami.FormData.label: i18nc("@label:listbox", "Camera:")
            Layout.fillWidth: true
            model: root.cameras.length
            enabled: count > 0
            displayText: root.cameraLabel(currentIndex)
            onCurrentIndexChanged: root.userFps = -1
        }

        QQC2.ComboBox {
            id: resolutions
            Kirigami.FormData.label: i18nc("@label:listbox", "Resolution:")
            Layout.fillWidth: true
            // Cap the model to avoid UI jank on cameras with 100+ sizes
            readonly property int maxEntries: 20
            model: {
                const sizes = layout.camera && layout.camera.sizes ? layout.camera.sizes : []
                const out = []
                for (let i = 0; i < Math.min(sizes.length, maxEntries); ++i) {
                    out.push(sizes[i])
                }
                return out
            }
            enabled: count > 0
            //: %1 image width, %2 image height, %3 maximum frame rate
            displayText: model.length > 0 && currentIndex >= 0
                ? i18nd("kdeconnect-app", "%1x%2 @ %3 fps", model[currentIndex].width, model[currentIndex].height, model[currentIndex].fps)
                : ""
        }

        QQC2.SpinBox {
            id: fps
            Kirigami.FormData.label: i18nc("@label:spinbox frame rate", "Frame rate:")
            Layout.fillWidth: true
            from: 1
            to: 240
            editable: true
            value: root.userFps >= 0 ? root.userFps
                : (layout.size && layout.size.fps > 0 ? layout.size.fps : root.defaultFps)
            onValueModified: root.userFps = value
            //: Suffix for the frame rate spin box (frames per second)
            suffix: " " + i18ndc("kdeconnect-app", "Abbreviation for frames per second", "fps")
        }

        QQC2.SpinBox {
            id: bitrate
            Kirigami.FormData.label: i18nc("@label:spinbox", "Bitrate:")
            Layout.fillWidth: true
            from: 250
            to: 50000
            stepSize: 250
            editable: true
            value: root.defaultBitrate
            //: Suffix for the bitrate spin box (kilobits per second)
            suffix: " " + i18ndc("kdeconnect-app", "Abbreviation for kilobits per second", "kbps")
        }

        QQC2.Button {
            id: startStop
            Kirigami.FormData.label: i18nc("@label", "Stream:")
            Layout.fillWidth: true
            enabled: layout.camera !== null && layout.size !== null
            flat: true
            icon.name: root.pluginInterface && root.pluginInterface.streaming ? "media-playback-stop" : "media-playback-start"
            text: root.pluginInterface && root.pluginInterface.streaming
                ? i18nd("kdeconnect-app", "Stop")
                : i18nd("kdeconnect-app", "Start")
            onClicked: {
                root.stoppedMessage = ""
                if (root.pluginInterface.streaming) {
                    root.pluginInterface.stopCamera()
                } else {
                    // The wire protocol expects bits per second; the SpinBox shows kilobits per second.
                    root.pluginInterface.startCamera(layout.camera.cameraId, layout.size.width, layout.size.height, fps.value, bitrate.value * 1000)
                }
            }
        }

        QQC2.Label {
            Kirigami.FormData.label: i18nc("@label", "Status:")
            text: {
                if (!(root.pluginInterface && root.pluginInterface.streaming)) {
                    return i18ndc("kdeconnect-app", "Camera stream state", "Idle")
                }
                var path = root.pluginInterface.devicePath
                if (path && path.length > 0) {
                    return i18nd("kdeconnect-app", "Streaming to %1 ...").arg(path)
                }
                return i18nd("kdeconnect-app", "Streaming ...")
            }
        }

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: root.errorMessage.length > 0
            type: Kirigami.MessageType.Error
            text: root.errorMessage
            actions: [
                Kirigami.Action {
                    icon.name: "dialog-cancel"
                    text: i18ndc("kdeconnect-app", "Dismiss an error message", "Dismiss")
                    onTriggered: root.errorMessage = ""
                }
            ]
        }

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: root.stoppedMessage.length > 0 && !(root.pluginInterface && root.pluginInterface.streaming)
            type: Kirigami.MessageType.Warning
            text: root.stoppedMessage
        }
    }
}
